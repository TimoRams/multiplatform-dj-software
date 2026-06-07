#include "DjEngine.h"
#include "audio/ReverseStreamAudioSource.h"
#include "DjMasterBus.h"
#include "library/CoverArtExtractor.h"
#include "library/CoverArtProvider.h"
#include "library/LibraryCoverService.h"
#include "fx/FxProcessor.h"
#include "library/LibraryDatabase.h"
#include "library/TrackIdGenerator.h"
#include "WaveformCache.h"
#include "WaveformAnalyzer.h"
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
#include <QVariantMap>
#include <QImage>
#include <QBuffer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <rubberband/RubberBandStretcher.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
#include <jack/jack.h>
#endif

// Metadata utilities: normalise and query JUCE StringPairArray across all tag formats
// (ID3v2, Vorbis comments, MP4 atoms, etc.).
namespace {

constexpr double kVolumeMin = 0.0;
constexpr double kVolumeMax = 1.0;
constexpr double kTrimMin = 0.0;
constexpr double kTrimMax = 2.0;
constexpr double kEqMin = -1.0;
constexpr double kEqMax = 1.0;
constexpr double kFilterMin = -1.0;
constexpr double kFilterMax = 1.0;
constexpr double kParamEpsilon = 1e-6;

double playHistoryThresholdSeconds(double durationSec)
{
    if (durationSec <= 0.0)
        return 12.0;

    if (durationSec <= 45.0)
        return std::clamp(durationSec * 0.35, 5.0, 12.0);

    return std::clamp(durationSec * 0.12, 10.0, 20.0);
}

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
bool probeJackServer(QString& message)
{
    jack_status_t status = JackFailure;
    jack_client_t* client = jack_client_open("BrockDJProbe", JackNoStartServer, &status);
    if (client == nullptr) {
        if (status & JackVersionError)
            message = QStringLiteral("JACK protocol version mismatch.");
        else if (status & JackServerError)
            message = QStringLiteral("JACK server error. Is PipeWire-JACK running?");
        else if (status & JackServerFailed)
            message = QStringLiteral("JACK server not running. Start PipeWire or jackd.");
        else
            message = QStringLiteral("JACK server not available. Start PipeWire-JACK.");
        return false;
    }

    jack_client_close(client);
    message = QStringLiteral("JACK server running.");
    return true;
}

int readJackBufferSize(jack_client_t* client)
{
    return client != nullptr ? static_cast<int>(jack_get_buffer_size(client)) : 0;
}

bool waitForJackBufferSize(jack_client_t* client, int requestedFrames, int& effectiveFrames)
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        effectiveFrames = readJackBufferSize(client);
        if (effectiveFrames == requestedFrames)
            return true;
        QThread::msleep(25);
    }

    effectiveFrames = readJackBufferSize(client);
    return effectiveFrames == requestedFrames;
}

bool forcePipeWireQuantum(int requestedFrames, QString& message)
{
    if (requestedFrames <= 0)
        return false;

    if (QStandardPaths::findExecutable(QStringLiteral("pw-metadata")).isEmpty()) {
        message = QStringLiteral("PipeWire metadata tool not found; cannot force JACK quantum.");
        return false;
    }

    QProcess process;
    process.start(QStringLiteral("pw-metadata"),
                  {QStringLiteral("-n"),
                   QStringLiteral("settings"),
                   QStringLiteral("0"),
                   QStringLiteral("clock.force-quantum"),
                   QString::number(requestedFrames)});
    if (!process.waitForFinished(700)) {
        process.kill();
        process.waitForFinished(100);
        message = QStringLiteral("Timed out while asking PipeWire for %1 frames/period.")
            .arg(requestedFrames);
        return false;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
        message = stderrText.isEmpty()
            ? QStringLiteral("PipeWire rejected %1 frames/period.").arg(requestedFrames)
            : stderrText;
        return false;
    }

    message = QStringLiteral("PipeWire quantum forced to %1 frames/period.").arg(requestedFrames);
    return true;
}

bool requestJackBufferSize(int requestedFrames, int& effectiveFrames, int& effectiveSampleRate, QString& message)
{
    effectiveFrames = requestedFrames;
    effectiveSampleRate = 0;

    if (requestedFrames <= 0) {
        message.clear();
        return true;
    }

    jack_status_t status = JackFailure;
    jack_client_t* client = jack_client_open("BrockDJBufferSetup", JackNoStartServer, &status);
    if (client == nullptr) {
        if (status & JackVersionError)
            message = QStringLiteral("JACK protocol version mismatch.");
        else if (status & JackServerError)
            message = QStringLiteral("JACK server error. Is PipeWire-JACK running?");
        else if (status & JackServerFailed)
            message = QStringLiteral("JACK server not running. Start PipeWire or jackd.");
        else
            message = QStringLiteral("JACK server not available. Start PipeWire-JACK.");
        return false;
    }

    effectiveSampleRate = static_cast<int>(jack_get_sample_rate(client));
    const jack_nframes_t current = jack_get_buffer_size(client);
    const int currentFrames = static_cast<int>(current);

    const int requestedForServer = requestedFrames;
    const jack_nframes_t requested = static_cast<jack_nframes_t>(requestedForServer);
    if (current == requested) {
        jack_client_close(client);
        message = QStringLiteral("JACK already uses %1 frames/period.").arg(effectiveFrames);
        return true;
    }

    const int result = jack_set_buffer_size(client, requested);

    if (result != 0) {
        jack_client_close(client);
        effectiveFrames = currentFrames > 0 ? currentFrames : requestedFrames;
        message = QStringLiteral("JACK rejected %1 frames/period. Change it in your JACK or PipeWire settings.")
            .arg(requestedForServer);
        return false;
    }

    waitForJackBufferSize(client, requestedForServer, effectiveFrames);
    effectiveSampleRate = static_cast<int>(jack_get_sample_rate(client));

    if (effectiveFrames != requestedForServer) {
        QString pipeWireMsg;
        if (forcePipeWireQuantum(requestedForServer, pipeWireMsg)) {
            waitForJackBufferSize(client, requestedForServer, effectiveFrames);
            if (effectiveFrames == requestedForServer) {
                effectiveSampleRate = static_cast<int>(jack_get_sample_rate(client));
                jack_client_close(client);
                message = pipeWireMsg;
                return true;
            }
        }
    }

    jack_client_close(client);

    if (effectiveFrames != requestedForServer) {
        message = QStringLiteral("Requested %1 JACK frames/period, but JACK reports %2.")
            .arg(requestedForServer)
            .arg(effectiveFrames);
    } else {
        message = QStringLiteral("JACK uses %1 frames/period.").arg(effectiveFrames);
    }
    return true;
}
#endif

bool nearlyEqual(double a, double b) {
    return std::abs(a - b) <= kParamEpsilon;
}

QString fromJuce(const juce::String& s) {
    return QString::fromUtf8(s.toRawUTF8());
}

QString cleanup(QString text) {
    if (text.isEmpty()) return text;
    text.replace(QRegularExpression(QStringLiteral("[\\x00\\r\\n\\t]+")), QStringLiteral(" "));
    return text.simplified().trimmed();
}

QString normaliseKey(const QString& key) {
    QString result;
    result.reserve(key.size());
    for (const QChar ch : key.trimmed().toLower())
        if (ch.isLetterOrNumber()) result.append(ch);
    return result;
}

QHash<QString,QString> buildMetadataLookup(const juce::StringPairArray& metadata) {
    QHash<QString,QString> map;
    auto keys   = metadata.getAllKeys();
    auto values = metadata.getAllValues();
    for (int i = 0; i < metadata.size(); ++i) {
        QString val = cleanup(fromJuce(values[i]));
        if (val.isEmpty()) continue;
        QString nk = normaliseKey(fromJuce(keys[i]));
        if (!nk.isEmpty() && !map.contains(nk))
            map.insert(nk, val);
        // Also index each colon-separated segment so "ID3:Title" matches as "title".
        QString raw = cleanup(fromJuce(keys[i]));
        if (raw.contains(QLatin1Char(':'))) {
            for (const auto& part : raw.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
                QString alt = normaliseKey(part);
                if (!alt.isEmpty() && !map.contains(alt))
                    map.insert(alt, val);
            }
        }
    }
    return map;
}

QString metaValue(const QHash<QString,QString>& map, std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) {
        auto it = map.constFind(normaliseKey(QString::fromUtf8(c)));
        if (it != map.cend()) return it.value();
    }
    return {};
}

// ID3v1 fallback: reads the 128-byte trailer appended to the end of the file.
struct Id3v1Tag { QString title, artist, album, year; };

std::optional<Id3v1Tag> readId3v1(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 128) return std::nullopt;
    f.seek(f.size() - 128);
    QByteArray data = f.read(128);
    if (data.size() != 128 || std::memcmp(data.constData(), "TAG", 3) != 0) return std::nullopt;
    Id3v1Tag t;
    t.title  = cleanup(QString::fromLatin1(data.mid(3, 30)));
    t.artist = cleanup(QString::fromLatin1(data.mid(33, 30)));
    t.album  = cleanup(QString::fromLatin1(data.mid(63, 30)));
    t.year   = cleanup(QString::fromLatin1(data.mid(93, 4)));
    return t;
}

// Heuristic: split "Artist - Title" filenames when tags are absent.
void filenameHeuristic(const QString& baseName, QString& title, QString& artist) {
    if (title.isEmpty()) title = baseName;
    if (artist.isEmpty()) {
        static const QRegularExpression pat(QStringLiteral("^\\s*(.+?)\\s*[-–]\\s*(.+)\\s*$"));
        auto m = pat.match(baseName);
        if (m.hasMatch()) {
            QString a = cleanup(m.captured(1));
            QString t = cleanup(m.captured(2));
            if (!a.isEmpty()) artist = a;
            if (!t.isEmpty()) title  = t;
        }
    }
}

double parseBpmString(const QString& raw) {
    if (raw.isEmpty()) return 0.0;
    QString c = raw.trimmed().replace(QLatin1Char(','), QLatin1Char('.'));
    static const QRegularExpression numPat(QStringLiteral("([0-9]+(?:\\.[0-9]+)?)"));
    auto m = numPat.match(c);
    if (m.hasMatch()) { bool ok; double v = m.captured(1).toDouble(&ok); if (ok) return v; }
    return 0.0;
}

QString defaultHotCueColor(int index)
{
    static const std::array<const char*, 16> colors = {
        "#e04040", "#e08030", "#e0d030", "#30b050",
        "#30a0d0", "#6060e0", "#c040c0", "#e06080",
        "#ff4d4d", "#ff9f43", "#f6e05e", "#48bb78",
        "#38b2ac", "#4299e1", "#9f7aea", "#ed64a6"
    };
    if (index < 0 || index >= static_cast<int>(colors.size()))
        return QStringLiteral("#e04040");
    return QString::fromLatin1(colors[static_cast<size_t>(index)]);
}

QString defaultSavedLoopColor(int index)
{
    static const std::array<const char*, 8> colors = {
        "#30b050", "#38b2ac", "#4299e1", "#48bb78",
        "#2f855a", "#319795", "#3182ce", "#276749"
    };
    if (index < 0 || index >= static_cast<int>(colors.size()))
        return QStringLiteral("#30b050");
    return QString::fromLatin1(colors[static_cast<size_t>(index)]);
}

juce::String toJuceString(const QString& text)
{
    return juce::String::fromUTF8(text.toUtf8().constData());
}

int choosePreferredBufferSize(juce::AudioIODevice* device, int requestedSize)
{
    if (device == nullptr)
        return requestedSize;

    const auto availableSizes = device->getAvailableBufferSizes();
    if (availableSizes.isEmpty())
        return requestedSize;

    if (availableSizes.contains(requestedSize))
        return requestedSize;

    int smallestAtLeastRequested = 0;
    int largestAvailable = availableSizes[0];
    for (const int candidate : availableSizes) {
        largestAvailable = std::max(largestAvailable, candidate);
        if (candidate >= requestedSize
            && (smallestAtLeastRequested == 0 || candidate < smallestAtLeastRequested))
            smallestAtLeastRequested = candidate;
    }

    if (smallestAtLeastRequested > 0)
        return smallestAtLeastRequested;

    return largestAvailable;
}

int minimumStableBufferSizeForBackend(const QString& deviceType)
{
#if JUCE_LINUX || JUCE_BSD
    const QString lower = deviceType.toLower();
    if (lower.contains(QStringLiteral("jack")))
        return 64;
    if (!lower.contains(QStringLiteral("jack")))
        return 512;
#endif
    return 128;
}

int clampToStableBufferSize(const QString& deviceType, int requestedSize)
{
    return std::clamp(requestedSize, minimumStableBufferSizeForBackend(deviceType), 4096);
}

juce::AudioIODeviceType* findDeviceType(juce::AudioDeviceManager& deviceManager, const QString& typeName)
{
    if (typeName.isEmpty())
        return deviceManager.getCurrentDeviceTypeObject();

    for (auto* type : deviceManager.getAvailableDeviceTypes()) {
        if (type != nullptr && QString::fromUtf8(type->getTypeName().toRawUTF8()) == typeName)
            return type;
    }

    return nullptr;
}

juce::AudioDeviceManager& sharedAudioDeviceManager()
{
    static juce::AudioDeviceManager manager;
    return manager;
}


struct OutputLatencySnapshot {
    int outputRawSamples = 0;
    int callbackBufferSamples = 0;
    int backendOutputSamples = 0;
    double sampleRate = 0.0;

    [[nodiscard]] int roundedSampleRate() const noexcept
    {
        return sampleRate > 0.0 ? static_cast<int>(std::lround(sampleRate)) : 0;
    }
};

struct OutputRoutingConfig {
    int masterFirstChannel = 1;
    int headphonesFirstChannel = -1;
    int boothFirstChannel = -1;
};

constexpr uint64_t kRoutingFieldMask = 0x1fu;
constexpr int kMaxSupportedOutputChannel = 30;

int clampFirstChannelForPack(int firstChannel)
{
    if (firstChannel < 1)
        return -1;
    return std::clamp(firstChannel, 1, kMaxSupportedOutputChannel);
}

uint64_t packRouting(const OutputRoutingConfig& cfg)
{
    const auto encode = [](int firstChannel) -> uint64_t {
        const int clamped = clampFirstChannelForPack(firstChannel);
        return static_cast<uint64_t>(clamped < 1 ? 0 : (clamped + 1));
    };

    uint64_t packed = 0;
    packed |= encode(cfg.masterFirstChannel);
    packed |= encode(cfg.headphonesFirstChannel) << 5;
    packed |= encode(cfg.boothFirstChannel) << 10;
    return packed;
}

OutputRoutingConfig unpackRouting(uint64_t packed)
{
    const auto decode = [](uint64_t value) -> int {
        const int decoded = static_cast<int>(value & kRoutingFieldMask) - 1;
        return decoded < 1 ? -1 : decoded;
    };

    return {
        .masterFirstChannel = decode(packed),
        .headphonesFirstChannel = decode(packed >> 5),
        .boothFirstChannel = decode(packed >> 10)
    };
}

std::atomic<uint64_t> s_outputRoutingPacked { packRouting(OutputRoutingConfig{}) };
std::mutex s_outputChannelCountCacheMutex;
QHash<QString, int> s_outputChannelCountCache;

void clearOutputChannelCountCache()
{
    std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
    s_outputChannelCountCache.clear();
}

int readDeviceOutputChannelCount(const QString& deviceType, const QString& outputDevice)
{
    const QString key = deviceType.trimmed() + QStringLiteral("\n") + outputDevice.trimmed();
    {
        std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
        const auto it = s_outputChannelCountCache.constFind(key);
        if (it != s_outputChannelCountCache.cend())
            return it.value();
    }

#if JUCE_LINUX || JUCE_BSD
    const QString loweredType = deviceType.trimmed().toLower();
    if (loweredType == QStringLiteral("jack") || loweredType.contains(QStringLiteral("jack"))) {
        // Probing JACK from a temporary AudioDeviceManager can block while JACK
        // negotiates graph state. For settings UI pair previews, return a safe
        // stereo fallback and avoid opening a second backend instance.
        std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
        s_outputChannelCountCache.insert(key, 2);
        return 2;
    }
#endif

    int channelCount = 2;

    juce::AudioDeviceManager probe;
    const juce::String initErr = probe.initialiseWithDefaultDevices(0, 2);
    if (initErr.isNotEmpty() || probe.getCurrentAudioDevice() == nullptr) {
        std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
        s_outputChannelCountCache.insert(key, channelCount);
        return channelCount;
    }

    juce::AudioIODeviceType* type = nullptr;
    if (!deviceType.isEmpty()) {
        type = findDeviceType(probe, deviceType);
        if (type != nullptr)
            probe.setCurrentAudioDeviceType(type->getTypeName(), true);
    } else {
        type = probe.getCurrentDeviceTypeObject();
    }

    if (type == nullptr)
        type = probe.getCurrentDeviceTypeObject();

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    probe.getAudioDeviceSetup(setup);
    setup.useDefaultInputChannels = true;
    setup.inputDeviceName.clear();
    setup.useDefaultOutputChannels = true;
    QString sanitizedOutput = outputDevice.trimmed();
    if (sanitizedOutput.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0)
        sanitizedOutput.clear();
    if (!sanitizedOutput.isEmpty() && type != nullptr) {
        type->scanForDevices();
        const auto names = type->getDeviceNames(false);
        bool found = false;
        for (const auto& name : names) {
            if (QString::fromUtf8(name.toRawUTF8()).trimmed() == sanitizedOutput) {
                found = true;
                break;
            }
        }
        if (!found)
            sanitizedOutput.clear();
    }

    if (!sanitizedOutput.isEmpty())
        setup.outputDeviceName = toJuceString(sanitizedOutput);

    juce::String error = probe.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty() && setup.outputDeviceName.isNotEmpty()) {
        setup.outputDeviceName.clear();
        error = probe.setAudioDeviceSetup(setup, true);
    }

    if (auto* device = probe.getCurrentAudioDevice()) {
        const int namesCount = device->getOutputChannelNames().size();
        const auto activeChannels = device->getActiveOutputChannels();
        const int activeSetBits = activeChannels.countNumberOfSetBits();

        if (namesCount > 0 && activeSetBits > 0)
            channelCount = std::max(namesCount, activeSetBits);
        else if (namesCount > 0)
            channelCount = namesCount;
        else if (activeSetBits > 0)
            channelCount = activeSetBits;
    }

    channelCount = std::clamp(channelCount, 2, kMaxSupportedOutputChannel);
    {
        std::lock_guard<std::mutex> lock(s_outputChannelCountCacheMutex);
        s_outputChannelCountCache.insert(key, channelCount);
    }
    return channelCount;
}

int readCurrentDeviceOutputChannelCount(const juce::AudioDeviceManager& manager,
                                        const QString& deviceType,
                                        const QString& outputDevice)
{
    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
        return -1;

    const QString currentType = QString::fromUtf8(manager.getCurrentAudioDeviceType().toRawUTF8());
    if (!deviceType.isEmpty() && currentType != deviceType)
        return -1;

    const QString currentOutput = QString::fromUtf8(device->getName().toRawUTF8());
    if (!outputDevice.isEmpty() && currentOutput != outputDevice)
        return -1;

    const int namesCount = device->getOutputChannelNames().size();
    const int activeSetBits = device->getActiveOutputChannels().countNumberOfSetBits();
    int channelCount = 2;
    if (namesCount > 0 && activeSetBits > 0)
        channelCount = std::max(namesCount, activeSetBits);
    else if (namesCount > 0)
        channelCount = namesCount;
    else if (activeSetBits > 0)
        channelCount = activeSetBits;

    return std::clamp(channelCount, 2, kMaxSupportedOutputChannel);
}

QStringList buildChannelPairList(int channelCount)
{
    QStringList pairs;
    pairs.push_back(QStringLiteral("None"));

    channelCount = std::clamp(channelCount, 2, kMaxSupportedOutputChannel);
    for (int first = 1; first + 1 <= channelCount; first += 2)
        pairs.push_back(QStringLiteral("%1-%2").arg(first).arg(first + 1));

    return pairs;
}

OutputLatencySnapshot readOutputLatencySnapshot(juce::AudioIODevice* device)
{
    if (!device)
        return {};

    const int outputRawSamples = std::max(0, device->getOutputLatencyInSamples());
    const int callbackBufferSamples = std::max(0, device->getCurrentBufferSizeSamples());
    int backendSamples;
    if (outputRawSamples <= 0) {
        backendSamples = 0;
    } else if (callbackBufferSamples > 0
               && outputRawSamples % callbackBufferSamples == 0
               && outputRawSamples >= 2 * callbackBufferSamples) {
        // ALSA over-reports: getOutputLatencyInSamples() returns numPeriods*periodSize.
        // The current period is already counted as the callback buffer, so subtract one.
        backendSamples = outputRawSamples - callbackBufferSamples;
    } else {
        backendSamples = outputRawSamples;
    }
    return {
        .outputRawSamples = outputRawSamples,
        .callbackBufferSamples = callbackBufferSamples,
        .backendOutputSamples = backendSamples,
        .sampleRate = device->getCurrentSampleRate()
    };
}

QString describeDeviceState(juce::AudioDeviceManager& manager)
{
    auto* device = manager.getCurrentAudioDevice();
    const QString typeName = QString::fromUtf8(manager.getCurrentAudioDeviceType().toRawUTF8());
    if (device == nullptr)
        return QStringLiteral("type=%1, device=<none>").arg(typeName);

    const QString deviceName = QString::fromUtf8(device->getName().toRawUTF8());
    const int outLatency = std::max(0, device->getOutputLatencyInSamples());
    const int buffer = std::max(0, device->getCurrentBufferSizeSamples());
    const double sampleRate = device->getCurrentSampleRate();
    return QStringLiteral("type=%1, device=%2, sr=%3, buf=%4, outLat=%5")
        .arg(typeName,
             deviceName,
             QString::number(sampleRate, 'f', 1),
             QString::number(buffer),
             QString::number(outLatency));
}

}


std::mutex DjEngine::s_syncMutex;
std::vector<DjEngine*> DjEngine::s_syncDecks;
DjEngine* DjEngine::s_syncMasterDeck = nullptr;

void DjEngine::updateSyncMasterLocked()
{
    DjEngine* newMaster = nullptr;
    for (auto* d : s_syncDecks) {
        if (d && d->m_syncEnabled) {
            newMaster = d;
            break;
        }
    }

    s_syncMasterDeck = newMaster;
    for (auto* d : s_syncDecks) {
        if (!d)
            continue;
        const bool wasMaster = d->m_isSyncMaster;
        d->m_isSyncMaster = (d == s_syncMasterDeck) && d->m_syncEnabled;
        if (wasMaster != d->m_isSyncMaster)
            emit d->syncMasterChanged();
    }
}

void DjEngine::propagateMasterTempoLocked(DjEngine* master)
{
    if (!master || !master->m_trackData)
        return;

    double masterBpm = 0.0;
    std::vector<DjEngine*> followers;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        masterBpm = master->getCurrentBpm();
        if (masterBpm <= 0.0)
            return;
        followers.reserve(s_syncDecks.size());
        for (auto* d : s_syncDecks) {
            if (!d || d == master || !d->m_syncEnabled || d->m_isSyncMaster)
                continue;
            followers.push_back(d);
        }
    }

    for (auto* d : followers) {
        if (!d->m_trackData)
            continue;
        const double baseBpm = d->m_trackData->getBpm();
        if (baseBpm <= 0.0)
            continue;
        const double pct = ((masterBpm / baseBpm) - 1.0) * 100.0;
        d->applyTempoPercent(pct);
    }
}

DjEngine::DjEngine(QObject* parent)
    : QObject(parent)
    , deviceManager(sharedAudioDeviceManager())
{
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        s_syncDecks.push_back(this);
    }

    m_trackData = new TrackData(this);
    m_analyzer  = new WaveformAnalyzer(
        m_trackData,
        &formatManager,
        static_cast<int>(WAVEFORM_POINTS_PER_SECOND));
    clearHotCueState();

    // When the analyzer detects a key, override the (often absent) ID3 key field.
    m_analysisPersistTimer = new QTimer(this);
    m_analysisPersistTimer->setSingleShot(true);
    m_analysisPersistTimer->setInterval(400);
    connect(m_analysisPersistTimer, &QTimer::timeout, this, [this]() {
        persistCurrentAnalysisToLibrary();
    });

    connect(m_trackData, &TrackData::keyAnalyzed, this, [this]() {
        QString analysedKey = m_trackData->getDetectedKey();
        if (!analysedKey.isEmpty()) {
            m_trackKey = analysedKey;
            emit trackMetadataChanged();
        }

        m_analysisPersistTimer->start();
    });

    // When BPM analysis finishes, re-emit tempoChanged so that currentBpm
    // and tempoRatio Q_PROPERTYs update in QML.
    connect(m_trackData, &TrackData::bpmAnalyzed, this, [this]() {
        emit tempoChanged();
        m_analysisPersistTimer->start();
        bool propagateFromSelf = false;
        DjEngine* masterToFollow = nullptr;
        if (m_syncEnabled) {
            {
                std::lock_guard<std::mutex> g(s_syncMutex);
                updateSyncMasterLocked();
                propagateFromSelf = m_isSyncMaster;
                masterToFollow = s_syncMasterDeck;
            }
            if (propagateFromSelf)
                propagateMasterTempoLocked(this);
            else if (masterToFollow)
                propagateMasterTempoLocked(masterToFollow);
        }
    });

    connect(m_trackData, &TrackData::beatgridChanged, this, [this]() {
        m_analysisPersistTimer->start();
    });

    connect(m_trackData, &TrackData::segmentsAnalyzed, this, [this]() {
        const auto segments = m_trackData->getSegments();

        QVariantList asVariant;
        asVariant.reserve(static_cast<int>(segments.size()));
        for (const auto& s : segments) {
            QVariantMap m;
            m.insert("label", s.label);
            m.insert("startTime", s.startTime);
            m.insert("endTime", s.endTime);
            m.insert("colorHex", s.colorHex);
            m.insert("confidence", s.confidence);
            asVariant.push_back(m);
        }

        m_currentSegments = asVariant;
        emit segmentsChanged();

        if (m_libraryDb && !m_currentTrackId.isEmpty())
            m_libraryDb->updateTrackSegments(m_currentTrackId, segments);
    });

    juce::MessageManager::getInstance();
    formatManager.registerBasicFormats();
    readAheadThread.startThread();

    // Keep Linux/ALSA stable by default. Tiny startup buffers are great for
    // latency, but they are too fragile while analysis and DB writes are active.
    if (auto* device = deviceManager.getCurrentAudioDevice()) {
        const int currentSize = device->getCurrentBufferSizeSamples();
        const int stableTarget = minimumStableBufferSizeForBackend(getCurrentAudioDeviceType());
        const int targetSize = choosePreferredBufferSize(device, stableTarget);

        if (targetSize > 0 && targetSize != currentSize && currentSize < stableTarget) {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager.getAudioDeviceSetup(setup);
            setup.bufferSize = targetSize;
            const juce::String setupErr = deviceManager.setAudioDeviceSetup(setup, true);
            if (setupErr.isEmpty()) {
                qDebug() << "[DjEngine] Raised audio buffer size for stability:" << currentSize << "->" << targetSize;
            } else {
                qWarning() << "[DjEngine] Could not adjust audio buffer size:" << QString::fromStdString(setupErr.toStdString());
            }
        }
    }

    // Audio callback is registered by DjMasterBus, not per-deck.

    scratchBridge = std::make_unique<engine::audio::ScratchDeckBridge>(&transportSource, false);
    timeStretchSource = std::make_unique<TimeStretchAudioSource>(scratchBridge.get());
    
    // Create the mixer DSP source to apply EQ, Filter, and Gain based on Pioneer DJM A9.
    mixerSource = std::make_unique<MixerDspSource>(timeStretchSource.get());
    mixerSource->setTrim(static_cast<float>(m_trim));
    mixerSource->setFader(static_cast<float>(m_volume));

    // DjMasterBus calls prepareToPlay on this source via addDeck().

    refreshHardwareLatency();
    clearOutputChannelCountCache();
    m_snapClock.start();
    m_playHistoryClock.start();

    connect(&timer, &QTimer::timeout, this, &DjEngine::onTimer);
    timer.setTimerType(Qt::PreciseTimer);
    // Faster control snapshots reduce audible speed stepping while scratching.
    timer.start(4);
}

DjEngine::~DjEngine()
{
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        s_syncDecks.erase(std::remove(s_syncDecks.begin(), s_syncDecks.end(), this), s_syncDecks.end());
        if (s_syncMasterDeck == this)
            s_syncMasterDeck = nullptr;
        updateSyncMasterLocked();
    }

    if (m_analyzer) {
        m_analyzer->stopAnalysis();
        delete m_analyzer;
    }
    transportSource.setSource(nullptr);
    reverseWrapSource.reset();
    bufferedReaderSource.reset();
    directReaderSource.reset();
    readerSource.reset();
    readAheadThread.stopThread(1000);
}

void DjEngine::shutdownSharedAudioDeviceManager()
{
    auto& manager = sharedAudioDeviceManager();
    manager.closeAudioDevice();
}

void DjEngine::prepareForShutdown()
{
    QObject::disconnect(&timer, nullptr, this, nullptr);
    timer.stop();

    if (m_analysisPersistTimer) {
        QObject::disconnect(m_analysisPersistTimer, nullptr, this, nullptr);
        m_analysisPersistTimer->stop();
    }

    if (m_analyzer) {
        m_analyzer->setCompletionCallback({});
        m_analyzer->stopAnalysis();
    }

    transportSource.stop();
}

juce::AudioDeviceManager& DjEngine::getSharedAudioDeviceManager()
{
    return sharedAudioDeviceManager();
}

float DjEngine::getProgress() const
{
    if (transportSource.getTotalLength() > 0.0)
        return static_cast<float>(transportSource.getCurrentPosition() / transportSource.getLengthInSeconds());
    return 0.0f;
}

float DjEngine::getDuration() const
{
    return static_cast<float>(transportSource.getLengthInSeconds());
}

void DjEngine::refreshHardwareLatency()
{
    if (auto* device = deviceManager.getCurrentAudioDevice()) {
        const auto latency = readOutputLatencySnapshot(device);
        if (latency.sampleRate > 0.0) {
            m_latencySeconds.store(
                static_cast<float>(static_cast<double>(latency.backendOutputSamples) / latency.sampleRate),
                std::memory_order_relaxed);
            const int visualCompSamples = latency.callbackBufferSamples + latency.backendOutputSamples;
            m_visualLatencyCompensationSeconds.store(
                static_cast<float>(std::clamp(static_cast<double>(visualCompSamples) / latency.sampleRate, 0.0, 0.250)),
                std::memory_order_relaxed);
        }

        const bool changed = (latency.backendOutputSamples != m_lastLoggedEffectiveSamples)
                          || (latency.outputRawSamples != m_lastLoggedOutputRawSamples)
                          || (latency.callbackBufferSamples != m_lastLoggedBufferSamples)
                          || (latency.roundedSampleRate() != m_lastLoggedSampleRateRounded)
                          || m_latencyLoggedNoDevice;

        if (changed) {
            qInfo() << "[DjEngine] Backend output latency:" << latency.backendOutputSamples
                    << "smp" << "(" << m_latencySeconds.load(std::memory_order_relaxed) << "s)"
                    << "raw:" << latency.outputRawSamples
                    << "buf:" << latency.callbackBufferSamples
                    << "sr:" << latency.roundedSampleRate();
            m_lastLoggedEffectiveSamples  = latency.backendOutputSamples;
            m_lastLoggedOutputRawSamples  = latency.outputRawSamples;
            m_lastLoggedBufferSamples     = latency.callbackBufferSamples;
            m_lastLoggedSampleRateRounded = latency.roundedSampleRate();
            m_latencyLoggedNoDevice = false;
        }
    } else {
        m_visualLatencyCompensationSeconds.store(0.0f, std::memory_order_relaxed);
        if (!m_latencyLoggedNoDevice) {
            qInfo() << "[DjEngine] No audio device yet; keeping last known latency";
            m_latencyLoggedNoDevice = true;
        }
    }
}

bool DjEngine::applyAudioDeviceSettings(int sampleRate, int bufferSize)
{
    const auto routing = unpackRouting(s_outputRoutingPacked.load(std::memory_order_relaxed));
    return applyAudioDeviceSettings(getCurrentAudioDeviceType(),
                                    getCurrentAudioOutputDevice(),
                                    sampleRate,
                                    bufferSize,
                                    routing.masterFirstChannel,
                                    routing.headphonesFirstChannel,
                                    routing.boothFirstChannel);
}

QStringList DjEngine::getAvailableAudioDeviceTypes() const
{
    QStringList types;

    auto& manager = const_cast<juce::AudioDeviceManager&>(deviceManager);
    const QString currentType = getCurrentAudioDeviceType();
    QString jackType;
    QString pipewireType;
    QString pulseType;

    for (auto* type : manager.getAvailableDeviceTypes()) {
        if (type == nullptr || type->getTypeName().isEmpty())
            continue;
        const QString name = QString::fromUtf8(type->getTypeName().toRawUTF8());
        types.push_back(name);
#if JUCE_LINUX || JUCE_BSD
        const QString lower = name.toLower();
        if (jackType.isEmpty() && lower == QStringLiteral("jack"))
            jackType = name;
        if (pipewireType.isEmpty() && lower.contains(QStringLiteral("pipewire")))
            pipewireType = name;
        if (pulseType.isEmpty() && (lower.contains(QStringLiteral("pulse")) || lower.contains(QStringLiteral("pulseaudio"))))
            pulseType = name;
#endif
    }

    const QString preferredType = !jackType.isEmpty() ? jackType
                                : !pipewireType.isEmpty() ? pipewireType
                                : !pulseType.isEmpty() ? pulseType
                                : currentType;

    const int preferredIndex = types.indexOf(preferredType);
    if (preferredIndex > 0)
        types.move(preferredIndex, 0);
    else {
        const int currentIndex = types.indexOf(currentType);
        if (currentIndex > 0)
            types.move(currentIndex, 0);
    }

    return types;
}

QStringList DjEngine::getAvailableAudioOutputDevices(const QString& deviceType) const
{
    QStringList devices;
    QStringList allDevices;

    auto& manager = const_cast<juce::AudioDeviceManager&>(deviceManager);
    auto* type = findDeviceType(manager, deviceType);
    if (type == nullptr)
        return devices;

    type->scanForDevices();
    const QString selectedType = !deviceType.isEmpty()
        ? deviceType
        : QString::fromUtf8(type->getTypeName().toRawUTF8());
    const QString currentOutput = getCurrentAudioOutputDevice();
    const QString selectedTypeLower = selectedType.toLower();
    QSet<QString> seen;

    const auto names = type->getDeviceNames(false);
    for (const auto& name : names) {
        const QString qName = QString::fromUtf8(name.toRawUTF8()).trimmed();
        if (qName.isEmpty() || seen.contains(qName))
            continue;

        seen.insert(qName);
        allDevices.push_back(qName);

        bool keep = true;
#if JUCE_LINUX || JUCE_BSD
        const QString lowerName = qName.toLower();
        
        // Exclude virtual/internal/problematic devices on Linux
        keep = !(lowerName.startsWith(QStringLiteral("hw:"))
                 || lowerName.startsWith(QStringLiteral("plughw:"))
                 || lowerName.startsWith(QStringLiteral("sysdefault:"))
                 || lowerName.startsWith(QStringLiteral("front:"))
                 || lowerName.startsWith(QStringLiteral("surround"))
                 || lowerName.startsWith(QStringLiteral("iec958:"))
                 || lowerName.startsWith(QStringLiteral("dmix:"))
                 || lowerName.startsWith(QStringLiteral("dsnoop:"))
                 || lowerName.startsWith(QStringLiteral("usbstream:"))
                 || lowerName.startsWith(QStringLiteral("jackinput"))
                 || lowerName.contains(QStringLiteral("internal"))
                 || lowerName.contains(QStringLiteral("loopback"))
                 || lowerName.startsWith(QStringLiteral("lavaplayer"))
                 || lowerName.startsWith(QStringLiteral("Combined"))
                 || lowerName == QStringLiteral("null"));

        if (!keep) {
            if (lowerName == QStringLiteral("default")
                || lowerName.contains(QStringLiteral("pipewire"))
                || lowerName.contains(QStringLiteral("pulse"))) {
                keep = true;
            }
        }
        
        // On PipeWire/ALSA, prefer human-readable names and exclude raw configs
        if (keep && (selectedTypeLower.contains(QStringLiteral("alsa")) || selectedTypeLower.contains(QStringLiteral("pipewire")) || selectedTypeLower.contains(QStringLiteral("pulse")))) {
            // Look for actual physical devices or named profiles
            keep = !lowerName.contains(QStringLiteral("@"))
                && !lowerName.startsWith(QStringLiteral("builtin_"))
                && !lowerName.contains(QStringLiteral(":CARD="))
                && !lowerName.contains(QStringLiteral(":DEV="));
        }
#endif

        if (keep)
            devices.push_back(qName);
    }

    if (devices.isEmpty())
        devices = allDevices;

    if (!currentOutput.isEmpty() && !devices.contains(currentOutput) && allDevices.contains(currentOutput))
        devices.push_front(currentOutput);

    const int currentOutputIndex = devices.indexOf(currentOutput);
    if (currentOutputIndex > 0)
        devices.move(currentOutputIndex, 0);

    return devices;
}

QStringList DjEngine::getAvailableOutputChannelPairs(const QString& deviceType,
                                                     const QString& outputDevice) const
{
    QString selectedType = deviceType;
    QString selectedOutput = outputDevice;

    if (selectedType.isEmpty())
        selectedType = getCurrentAudioDeviceType();
    if (selectedOutput.isEmpty())
        selectedOutput = getCurrentAudioOutputDevice();

    const QString loweredType = selectedType.trimmed().toLower();
    if (loweredType == QStringLiteral("jack") || loweredType.contains(QStringLiteral("jack")))
        return buildChannelPairList(kMaxSupportedOutputChannel);

    int channelCount = readCurrentDeviceOutputChannelCount(deviceManager, selectedType, selectedOutput);
    if (channelCount < 2)
        channelCount = readDeviceOutputChannelCount(selectedType, selectedOutput);

    return buildChannelPairList(channelCount);
}

QString DjEngine::getCurrentAudioDeviceType() const
{
    return QString::fromUtf8(deviceManager.getCurrentAudioDeviceType().toRawUTF8());
}

QString DjEngine::getCurrentAudioOutputDevice() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return QString::fromUtf8(device->getName().toRawUTF8());
    return QString();
}

int DjEngine::getCurrentAudioSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return static_cast<int>(std::lround(device->getCurrentSampleRate()));
    return 0;
}

int DjEngine::getCurrentAudioBufferSize() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();
    return 0;
}

bool DjEngine::isJackServerRunning() const
{
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
    QString msg;
    return probeJackServer(msg);
#else
    return false;
#endif
}

QString DjEngine::jackServerStatus() const
{
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
    QString msg;
    probeJackServer(msg);
    return msg;
#else
    return QStringLiteral("JACK backend not built in this binary.");
#endif
}

bool DjEngine::applyAudioDeviceSettings(const QString& deviceType,
                                        const QString& outputDevice,
                                        int sampleRate,
                                        int bufferSize,
                                        int masterFirstChannel,
                                        int headphonesFirstChannel,
                                        int boothFirstChannel)
{
    sampleRate = std::clamp(sampleRate, 44100, 96000);

    setLastAudioDeviceError(QString());
    setAudioDeviceFallbackMessage(QString());

    const auto previousRouting = unpackRouting(s_outputRoutingPacked.load(std::memory_order_relaxed));

    const QString requestedType = !deviceType.isEmpty() ? deviceType : getCurrentAudioDeviceType();
#if JUCE_LINUX || JUCE_BSD
    const bool jackBackendRequested = requestedType.toLower().contains(QStringLiteral("jack"));
    bufferSize = std::clamp(bufferSize, 64, 4096);
#else
    const bool jackBackendRequested = requestedType.toLower().contains(QStringLiteral("jack"));
    bufferSize = std::clamp(bufferSize, 64, 4096);
#endif

    if (jackBackendRequested) {
#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
        QString jackMsg;
        if (!probeJackServer(jackMsg)) {
            setLastAudioDeviceError(jackMsg);
            return false;
        }
#else
        setLastAudioDeviceError(QStringLiteral("JACK backend not built in this binary."));
        return false;
#endif
    }

    masterFirstChannel = clampFirstChannelForPack(masterFirstChannel);
    headphonesFirstChannel = clampFirstChannelForPack(headphonesFirstChannel);
    boothFirstChannel = clampFirstChannelForPack(boothFirstChannel);
    const int previousMasterFirstChannel = m_masterFirstChannelAtomic.load(std::memory_order_relaxed);
    m_masterFirstChannelAtomic.store(masterFirstChannel, std::memory_order_relaxed);
    s_outputRoutingPacked.store(packRouting({
        .masterFirstChannel = masterFirstChannel,
        .headphonesFirstChannel = headphonesFirstChannel,
        .boothFirstChannel = boothFirstChannel
    }), std::memory_order_relaxed);
    DjMasterBus::setOutputRouting(masterFirstChannel, boothFirstChannel, headphonesFirstChannel);

    auto& manager = deviceManager;
    const juce::String previousType = manager.getCurrentAudioDeviceType();
    juce::AudioDeviceManager::AudioDeviceSetup previousSetup;
    manager.getAudioDeviceSetup(previousSetup);
    if (manager.getCurrentAudioDevice() == nullptr) {
        qWarning() << "[DjEngine] No current audio device before setup; trying default initialisation";
        const juce::String initErr = manager.initialiseWithDefaultDevices(0, 2);
        if (initErr.isNotEmpty())
            qWarning() << "[DjEngine] initialiseWithDefaultDevices failed:" << QString::fromStdString(initErr.toStdString());
    }
    
    auto* type = findDeviceType(manager, deviceType);
    if (!deviceType.isEmpty() && type == nullptr) {
        manager.setCurrentAudioDeviceType(toJuceString(deviceType), true);
        type = findDeviceType(manager, deviceType);
    }
    if (!deviceType.isEmpty() && type == nullptr) {
        const QString msg = jackBackendRequested
            ? QStringLiteral("JACK backend not available. Start JACK or install JACK support.")
            : QStringLiteral("Audio backend not available: %1").arg(deviceType);
        qWarning() << "[DjEngine]" << msg;
        setLastAudioDeviceError(msg);
        return false;
    }
    if (!deviceType.isEmpty() && QString::fromUtf8(manager.getCurrentAudioDeviceType().toRawUTF8()) != deviceType) {
        manager.setCurrentAudioDeviceType(toJuceString(deviceType), true);
        type = findDeviceType(manager, deviceType);
    }

    QString sanitizedOutput = outputDevice.trimmed();
    if (sanitizedOutput.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0)
        sanitizedOutput.clear();

    if (!jackBackendRequested && !sanitizedOutput.isEmpty() && type != nullptr) {
        type->scanForDevices();
        const auto names = type->getDeviceNames(false);
        bool found = false;
        for (const auto& name : names) {
            if (QString::fromUtf8(name.toRawUTF8()).trimmed() == sanitizedOutput) {
                found = true;
                break;
            }
        }
        if (!found) {
            qWarning() << "[DjEngine] Requested output device not found:" << sanitizedOutput
                       << "- falling back to system default";
            setAudioDeviceFallbackMessage(
                QStringLiteral("Saved audio device \"%1\" is no longer available. "
                               "Falling back to system default — open Settings → Audio Setup to reconfigure.")
                .arg(sanitizedOutput));
            sanitizedOutput.clear();
        }
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    manager.getAudioDeviceSetup(setup);
    if (jackBackendRequested) {
        if (auto* device = manager.getCurrentAudioDevice()) {
            const double currentJackRate = device->getCurrentSampleRate();
            if (currentJackRate > 0.0)
                sampleRate = static_cast<int>(std::lround(currentJackRate));
        }

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
        QString jackBufferMsg;
        int effectiveJackBuffer = bufferSize;
        int effectiveJackSampleRate = 0;
        if (!requestJackBufferSize(bufferSize, effectiveJackBuffer, effectiveJackSampleRate, jackBufferMsg)) {
            qWarning() << "[DjEngine]" << jackBufferMsg;
            setAudioDeviceFallbackMessage(jackBufferMsg);
        } else if (effectiveJackBuffer != bufferSize) {
            qWarning() << "[DjEngine]" << jackBufferMsg;
            setAudioDeviceFallbackMessage(jackBufferMsg);
        }
        bufferSize = effectiveJackBuffer;
        if (effectiveJackSampleRate > 0)
            sampleRate = effectiveJackSampleRate;
#endif
    }
    setup.sampleRate = static_cast<double>(sampleRate);
    setup.bufferSize = bufferSize;
    setup.useDefaultInputChannels = true;
    setup.inputDeviceName.clear();
    setup.useDefaultOutputChannels = true;
    
    if (jackBackendRequested) {
        if (sanitizedOutput.isEmpty()) {
            if (type != nullptr) {
                type->scanForDevices();
                const auto names = type->getDeviceNames(false);
                if (names.size() > 0)
                    setup.outputDeviceName = names[0];
            }

            if (setup.outputDeviceName.isEmpty()) {
                const QString msg = QStringLiteral("No JACK output ports available. Start PipeWire-JACK or jackd.");
                qWarning() << "[DjEngine]" << msg;
                setLastAudioDeviceError(msg);
                return false;
            }
        } else {
            setup.outputDeviceName = toJuceString(sanitizedOutput);
        }
    } else if (sanitizedOutput.isEmpty()) {
        if (auto* device = manager.getCurrentAudioDevice()) {
            setup.outputDeviceName = device->getName();
        } else if (type != nullptr) {
            // No device open yet (first startup or empty saved config).
            // Scan and pick the first available device so audio starts automatically.
            type->scanForDevices();
            const auto names = type->getDeviceNames(false);
            if (!names.isEmpty())
                setup.outputDeviceName = names[0];
            qDebug() << "[DjEngine] No current device; using first available:"
                     << QString::fromUtf8(setup.outputDeviceName.toRawUTF8());
        }
        // setup.outputDeviceName may still be empty — JUCE will use its own default.
    } else {
        setup.outputDeviceName = toJuceString(sanitizedOutput);
    }

    int maxOutputChannels = 2;  // Safe default
    const auto maxRoutedChannel = [](int firstChannel) -> int {
        return firstChannel >= 1 ? firstChannel + 1 : 0;
    };
    int maxRequestedChannel = std::max({
        maxRoutedChannel(masterFirstChannel),
        maxRoutedChannel(masterFirstChannel + 2),  // second deck auto-assigned to next pair
        maxRoutedChannel(headphonesFirstChannel),
        maxRoutedChannel(boothFirstChannel),
        2
    });
    
    // JACK setup should avoid extra device probing because it can block.
    if (!jackBackendRequested) {
        if (auto* device = manager.getCurrentAudioDevice()) {
            const int namesCount = device->getOutputChannelNames().size();
            const int activeSetBits = device->getActiveOutputChannels().countNumberOfSetBits();
            int deviceChannels = 2;
            if (namesCount > 0)
                deviceChannels = namesCount;
            else if (activeSetBits > 0)
                deviceChannels = activeSetBits;
            deviceChannels = std::clamp(deviceChannels, 2, kMaxSupportedOutputChannel);

            // Use the larger of what the device currently has and what is being
            // requested.  If we only took the current active count (often 2 for a
            // default stereo open) we would wrongly reject e.g. ch 3-4 on a
            // 4-channel interface before JUCE even tries to open them.
            maxOutputChannels = std::max(maxRequestedChannel, deviceChannels);
            maxOutputChannels = std::clamp(maxOutputChannels, 2, kMaxSupportedOutputChannel);

            setup.bufferSize = choosePreferredBufferSize(device, setup.bufferSize);
        } else {
            // No device open yet — trust the requested channels; JUCE will error
            // if the hardware doesn't support them and the fallback path kicks in.
            maxOutputChannels = std::clamp(maxRequestedChannel, 2, kMaxSupportedOutputChannel);
        }
    } else {
        maxOutputChannels = std::clamp(maxRequestedChannel, 2, kMaxSupportedOutputChannel);
    }

    juce::BigInteger selectedOutputChannels;
    const auto setPairBits = [&selectedOutputChannels](int firstChannel) {
        if (firstChannel < 1)
            return;
        selectedOutputChannels.setBit(firstChannel - 1);
        selectedOutputChannels.setBit(firstChannel);
    };
    setPairBits(masterFirstChannel);
    setPairBits(masterFirstChannel + 2);  // second deck output pair
    setPairBits(headphonesFirstChannel);
    setPairBits(boothFirstChannel);
    if (jackBackendRequested) {
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        for (int ch = 0; ch < maxOutputChannels; ++ch)
            setup.outputChannels.setBit(ch);
    } else if (selectedOutputChannels.countNumberOfSetBits() > 0) {
        setup.useDefaultOutputChannels = false;
        setup.outputChannels = selectedOutputChannels;
    }

    auto* currentDevice = manager.getCurrentAudioDevice();
    const bool needsReopen = currentDevice != nullptr
        && ((std::abs(currentDevice->getCurrentSampleRate() - static_cast<double>(sampleRate)) > 0.5)
            || (currentDevice->getCurrentBufferSizeSamples() != bufferSize)
            || jackBackendRequested);

    if (needsReopen)
        manager.closeAudioDevice();

    // Try to apply the audio device setup with fallback strategy
    juce::String error = manager.setAudioDeviceSetup(setup, true);
    
    if (error.isNotEmpty()) {
        qWarning() << "[DjEngine] Initial device setup failed:" << QString::fromStdString(error.toStdString());
        
        // Fallback 1: Try without custom channel routing
        if (setup.useDefaultOutputChannels == false) {
            qWarning() << "[DjEngine] Retrying without custom channel routing";
            setup.useDefaultOutputChannels = true;
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        // Fallback 2: Try with default output device
        if (error.isNotEmpty() && setup.outputDeviceName.isNotEmpty()) {
            qWarning() << "[DjEngine] Retrying with default output device";
            setup.outputDeviceName.clear();
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        // Fallback 3: Try minimum viable setup
        if (error.isNotEmpty()) {
            qWarning() << "[DjEngine] Attempting minimum viable setup";
            manager.getAudioDeviceSetup(setup);
            setup.sampleRate = static_cast<double>(sampleRate);
            setup.bufferSize = clampToStableBufferSize(requestedType, bufferSize);
            setup.useDefaultOutputChannels = true;
            setup.outputDeviceName.clear();
            error = manager.setAudioDeviceSetup(setup, true);
        }
        
        if (error.isNotEmpty()) {
            qWarning() << "[DjEngine] Failed to apply audio device settings after all fallbacks:" << QString::fromStdString(error.toStdString());
        }
    }

    auto* activeDevice = manager.getCurrentAudioDevice();
    const bool deviceReady = activeDevice != nullptr && activeDevice->isOpen();
    if (error.isNotEmpty() || !deviceReady) {
        if (!deviceReady)
            qWarning() << "[DjEngine] Audio device not available after apply; restoring previous device";

        if (!previousType.isEmpty() && previousType != manager.getCurrentAudioDeviceType())
            manager.setCurrentAudioDeviceType(previousType, true);

        const juce::String restoreErr = manager.setAudioDeviceSetup(previousSetup, true);
        if (restoreErr.isNotEmpty()) {
            qWarning() << "[DjEngine] Failed to restore previous audio device:" << QString::fromStdString(restoreErr.toStdString());
        }

        s_outputRoutingPacked.store(packRouting(previousRouting), std::memory_order_relaxed);
        m_masterFirstChannelAtomic.store(previousMasterFirstChannel, std::memory_order_relaxed);

        QString errorText = QString::fromStdString(error.toStdString());
        if (errorText.isEmpty())
            errorText = QStringLiteral("Audio device setup failed.");
        if (jackBackendRequested && !errorText.contains(QStringLiteral("JACK"), Qt::CaseInsensitive))
            errorText = QStringLiteral("JACK device failed to open. Is the JACK server running?");
        setLastAudioDeviceError(errorText);
        return false;
    }

    setLastAudioDeviceError(QString());

    refreshHardwareLatency();

    if (jackBackendRequested) {
        if (auto* activeJackDevice = manager.getCurrentAudioDevice()) {
            const int actualJackBuffer = activeJackDevice->getCurrentBufferSizeSamples();
            if (actualJackBuffer > 0 && actualJackBuffer != bufferSize) {
                setAudioDeviceFallbackMessage(QStringLiteral(
                    "Requested %1 JACK frames/period, but the active JACK device opened at %2. "
                    "Check PipeWire/JACK server quantum settings.")
                    .arg(bufferSize)
                    .arg(actualJackBuffer));
                bufferSize = actualJackBuffer;
            }
        }
    }

    // AudioDeviceManager is shared across all deck instances. Reconfiguring the
    // device can cause internal sources to re-prepare and some playback ratios
    // to fall back to defaults. Reapply tempo/keylock state for every deck so
    // no deck is left at stale speed after another deck applies settings.
    std::vector<DjEngine*> decks;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        decks = s_syncDecks;
    }

    for (auto* deck : decks) {
        if (!deck)
            continue;

        deck->refreshHardwareLatency();

        // During active scratch/release, onTimer() owns the scratch routing.
        if (deck->m_scratch.scrubbing() || deck->m_scratch.releaseGlide())
            continue;

        deck->updateSpeedAndPitch();
    }

    return true;
}

DjEngine::LatencySnapshot DjEngine::buildLatencySnapshot() const
{
    LatencySnapshot snapshot;
    if (m_lastLatencySnapshot.sampleRate > 0.0)
        snapshot.sampleRate = m_lastLatencySnapshot.sampleRate;

    if (auto* device = deviceManager.getCurrentAudioDevice()) {
        const auto latency = readOutputLatencySnapshot(device);
        snapshot.outputRawSamples = latency.outputRawSamples;
        snapshot.bufferSamples = latency.callbackBufferSamples;
        snapshot.backendOutputSamples = latency.backendOutputSamples;
        if (latency.sampleRate > 0.0)
            snapshot.sampleRate = latency.sampleRate;
    } else if (m_lastLatencySnapshot.sampleRate > 0.0) {
        snapshot.outputRawSamples = m_lastLatencySnapshot.outputRawSamples;
        snapshot.bufferSamples = m_lastLatencySnapshot.bufferSamples;
        snapshot.backendOutputSamples = m_lastLatencySnapshot.backendOutputSamples;
    }

    if (timeStretchSource)
        snapshot.keylockSamples = std::max(0, timeStretchSource->getLatencySamples());

    snapshot.limiterSamples = std::max(0, DjMasterBus::limiterLatencySamples());
    snapshot.resamplerSamples = 0;
    snapshot.mixerFxSamples = 0;
    m_lastLatencySnapshot = snapshot;
    return snapshot;
}

double DjEngine::totalLatencyMs() const
{
    const auto snapshot = buildLatencySnapshot();
    if (snapshot.sampleRate <= 0.0)
        return 0.0;

    const int totalSamples = snapshot.bufferSamples
                           + snapshot.backendOutputSamples
                           + snapshot.keylockSamples
                           + snapshot.resamplerSamples
                           + snapshot.limiterSamples
                           + snapshot.mixerFxSamples;
    return (static_cast<double>(totalSamples) / snapshot.sampleRate) * 1000.0;
}

QVariantList DjEngine::latencyBreakdown() const
{
    const auto snapshot = buildLatencySnapshot();
    if (snapshot.sampleRate <= 0.0)
        return {};

    const auto toMs = [sampleRate = snapshot.sampleRate](int samples) -> double {
        return (static_cast<double>(samples) / sampleRate) * 1000.0;
    };

    QVariantList rows;
    const int audioDeviceSamples = snapshot.bufferSamples + snapshot.backendOutputSamples;
    const int dspSamples = snapshot.keylockSamples
                         + snapshot.resamplerSamples
                         + snapshot.limiterSamples
                         + snapshot.mixerFxSamples;

    QVariantMap audioDeviceRow;
    audioDeviceRow.insert("name", QStringLiteral("Audio Device Total"));
    audioDeviceRow.insert("samples", audioDeviceSamples);
    audioDeviceRow.insert("ms", toMs(audioDeviceSamples));
    audioDeviceRow.insert("countInTotal", false);
    rows.push_back(audioDeviceRow);

    QVariantMap bufferRow;
    bufferRow.insert("name", QStringLiteral("Device Buffer / Period"));
    bufferRow.insert("samples", snapshot.bufferSamples);
    bufferRow.insert("ms", toMs(snapshot.bufferSamples));
    bufferRow.insert("countInTotal", true);
    rows.push_back(bufferRow);

    QVariantMap driverRow;
    driverRow.insert("name", QStringLiteral("Backend / Hardware"));
    driverRow.insert("samples", snapshot.backendOutputSamples);
    driverRow.insert("ms", toMs(snapshot.backendOutputSamples));
    driverRow.insert("countInTotal", true);
    rows.push_back(driverRow);

    QVariantMap dspRow;
    dspRow.insert("name", QStringLiteral("DSP Latency"));
    dspRow.insert("samples", dspSamples);
    dspRow.insert("ms", toMs(dspSamples));
    dspRow.insert("countInTotal", false);
    rows.push_back(dspRow);

    QVariantMap rubberbandRow;
    rubberbandRow.insert("name", QStringLiteral("Keylock / Timestretch"));
    rubberbandRow.insert("samples", snapshot.keylockSamples);
    rubberbandRow.insert("ms", toMs(snapshot.keylockSamples));
    rubberbandRow.insert("countInTotal", true);
    rows.push_back(rubberbandRow);

    QVariantMap resamplerRow;
    resamplerRow.insert("name", QStringLiteral("Resampler"));
    resamplerRow.insert("samples", snapshot.resamplerSamples);
    resamplerRow.insert("ms", toMs(snapshot.resamplerSamples));
    resamplerRow.insert("countInTotal", true);
    rows.push_back(resamplerRow);

    QVariantMap limiterRow;
    limiterRow.insert("name", QStringLiteral("Limiter Lookahead"));
    limiterRow.insert("samples", snapshot.limiterSamples);
    limiterRow.insert("ms", toMs(snapshot.limiterSamples));
    limiterRow.insert("countInTotal", true);
    rows.push_back(limiterRow);

    QVariantMap fxRow;
    fxRow.insert("name", QStringLiteral("Mixer / FX Chain"));
    fxRow.insert("samples", snapshot.mixerFxSamples);
    fxRow.insert("ms", toMs(snapshot.mixerFxSamples));
    fxRow.insert("countInTotal", true);
    rows.push_back(fxRow);

    QVariantMap totalRow;
    totalRow.insert("name", QStringLiteral("Total Estimated Latency"));
    totalRow.insert("samples", audioDeviceSamples + dspSamples);
    totalRow.insert("ms", toMs(audioDeviceSamples + dspSamples));
    totalRow.insert("countInTotal", false);
    rows.push_back(totalRow);

    return rows;
}

QVariantMap DjEngine::audioPerformanceStats() const
{
    QVariantMap stats;
    const auto snapshot = buildLatencySnapshot();
    const double callbackBudgetUsec = snapshot.sampleRate > 0.0
        ? (static_cast<double>(snapshot.bufferSamples) / snapshot.sampleRate) * 1000000.0
        : 0.0;

    stats.insert(QStringLiteral("callbackAverageUsec"), DjMasterBus::callbackAverageUsec());
    stats.insert(QStringLiteral("callbackWorstUsec"), DjMasterBus::callbackWorstUsec());
    stats.insert(QStringLiteral("callbackBudgetUsec"), callbackBudgetUsec);
    stats.insert(QStringLiteral("callbackCount"),
                 QVariant::fromValue<qulonglong>(DjMasterBus::callbackCount()));
    stats.insert(QStringLiteral("callbackOverruns"),
                 QVariant::fromValue<qulonglong>(DjMasterBus::callbackOverrunCount()));
    stats.insert(QStringLiteral("sampleRate"), snapshot.sampleRate);
    stats.insert(QStringLiteral("bufferSamples"), snapshot.bufferSamples);

    QVariantList fxProfiles;
    for (int i = 1; i <= static_cast<int>(EffectType::RollOut); ++i) {
        const auto type = static_cast<EffectType>(i);
        const auto profile = FxProcessor::getCpuProfile(type);
        if (profile.count == 0)
            continue;

        QVariantMap row;
        row.insert(QStringLiteral("name"), QString::fromLatin1(FxProcessor::effectTypeName(type)));
        row.insert(QStringLiteral("averageUsec"),
                   static_cast<double>(profile.totalUsec) / static_cast<double>(profile.count));
        row.insert(QStringLiteral("worstUsec"), static_cast<double>(profile.worstUsec));
        row.insert(QStringLiteral("count"), QVariant::fromValue<qulonglong>(profile.count));
        fxProfiles.push_back(row);
    }
    stats.insert(QStringLiteral("fxProfiles"), fxProfiles);
    return stats;
}

double DjEngine::getPosition() const
{
    if (m_scratch.scrubbing() || m_scratch.releaseGlide() || m_preRollCountdownActive)
        return m_scrubHoldPosition;
    // In pre-roll, transport is clamped at 0 while the visual position is
    // negative. m_scrubHoldPosition is the authoritative visual position here —
    // it is kept in sync by freezeTransportAt(), cue operations, and hot cues.
    if (m_scrubHoldPosition < 0.0)
        return m_scrubHoldPosition;
    return transportSource.getCurrentPosition();
}

double DjEngine::getVisualPosition() const
{
    // During scratch the waveform follows the actual platter position, not the
    // raw mouse/turntable target. The target can lead slightly; the follower
    // below provides the velocity/inertia that makes scratching sound natural.
    if (m_scratch.scrubbing() || m_scratch.releaseGlide())
        return m_scrubHoldPosition;

    // Pre-roll countdown: interpolate using the pre-roll wall clock so the waveform
    // scrolls smoothly at sub-frame granularity, just like the normal snap-clock path.
    // Clamp at 0 to avoid briefly overshooting track start before tickTransportStopped()
    // clears the flag — the timer may lag up to one tick (~16 ms) behind the clock.
    if (m_preRollCountdownActive) {
        const double elapsed = static_cast<double>(m_preRollClock.nsecsElapsed()) * 1e-9;
        return std::min(m_preRollVisualStartPos + elapsed * getTempoRatio(), 0.0);
    }

    // When stopped/paused: return the frozen position (set by togglePlay).
    if (!m_snapValid || !transportSource.isPlaying())
        return getPosition();

    // Forward-interpolate from the last snapshot using elapsed wall-clock time.
    // This keeps the waveform smooth between onTimer() ticks.
    // Use the tempo ratio captured at snapshot time to avoid micro speed
    // discontinuities within a timer interval.
    const double elapsed = (static_cast<double>(m_snapClock.nsecsElapsed()) * 1e-9)
        * std::max(0.0001, m_snapTempoRatio);

    // When reverse is on, interpolate backwards instead of forwards
    double interpolated = m_isReverse
        ? m_snapPosition - elapsed
        : m_snapPosition + elapsed;

    // Render the scrolling waveform at the speaker-time playhead. Without this,
    // larger hardware buffers make the UI visibly lead/lag the audible beat.
    const double visualLatency = static_cast<double>(
        m_visualLatencyCompensationSeconds.load(std::memory_order_relaxed));
    double latencyBlend = 1.0;
    if (m_visualSeekSettleClock.isValid()) {
        constexpr double kVisualSeekSettleSeconds = 0.090;
        const double settleElapsed = static_cast<double>(m_visualSeekSettleClock.nsecsElapsed()) * 1e-9;
        if (settleElapsed < kVisualSeekSettleSeconds) {
            const double t = std::clamp(settleElapsed / kVisualSeekSettleSeconds, 0.0, 1.0);
            latencyBlend = t * t * (3.0 - 2.0 * t);
        }
    }
    interpolated += m_isReverse ? visualLatency * latencyBlend : -visualLatency * latencyBlend;

    double len = transportSource.getLengthInSeconds();
    interpolated = std::clamp(interpolated, -PRE_ROLL_SECONDS, len > 0.0 ? len : interpolated);

    return interpolated;
}

double DjEngine::getVisualPositionQml() const
{
    return getVisualPosition();
}

double DjEngine::loopPreviewOutPosition() const
{
    if (m_loopActive && m_loopOutSec > m_loopInSec)
        return m_loopOutSec;

    if (!m_loopInSet)
        return m_loopOutSec;

    const double trackLen = m_trackDurationSec;
    if (trackLen <= 0.0)
        return m_loopInSec;

    double outPos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        outPos = quantizedBeatAt(outPos);

    outPos = std::clamp(outPos, -PRE_ROLL_SECONDS, trackLen);

    constexpr double minLenSec = 0.001;
    if (outPos <= m_loopInSec + minLenSec) {
        const double beatDur = beatDurationAround(m_loopInSec);
        if (beatDur > 1e-4)
            outPos = std::min(trackLen, m_loopInSec + beatDur);
    }

    return outPos;
}

double DjEngine::getPlayheadPositionAtomic() const
{
    // Lock-free load — always returns the last value written by onTimer().
    // QML FrameAnimation calls this every VSync frame; must never block.
    return m_atomicPlayheadPos.load(std::memory_order_relaxed);
}

void DjEngine::setCoverArtProvider(CoverArtProvider* provider, const QString& deckId)
{
    m_coverProvider = provider;
    m_deckId = deckId;
}

void DjEngine::setLibraryCoverService(LibraryCoverService* service)
{
    m_libraryCoverService = service;
}

QImage DjEngine::currentCoverImage() const
{
    if (!m_coverProvider || m_deckId.isEmpty())
        return {};

    return m_coverProvider->coverImage(m_deckId);
}

void DjEngine::setLibraryDatabase(LibraryDatabase* db)
{
    m_libraryDb = db;
    loadHotCuesForCurrentTrack();
    loadSavedLoopsForCurrentTrack();
    loadMainCueForCurrentTrack();
    emit beatgridLockedChanged();
}

void DjEngine::persistCurrentAnalysisToLibrary()
{
    if (!m_libraryDb || m_currentTrackId.isEmpty() || !m_trackData)
        return;

    const double bpm = m_trackData->getBpm();
    const QString key = m_trackData->getDetectedKey().trimmed();
    const auto beatGrid = m_trackData->getBeatGrid();

    if (bpm <= 0.0 && key.isEmpty() && beatGrid.empty())
        return;

    m_libraryDb->updateAnalysisData(
        m_currentTrackId,
        static_cast<float>(bpm),
        key,
        m_trackData->getFirstBeatSample(),
        m_trackData->getSampleRate(),
        beatGrid,
        m_trackData->getConfidenceInfo(),
        m_trackData->getBeatGridInfo());
}

bool DjEngine::isPlaying() const
{
    return m_playRequested;
}

TrackData* DjEngine::getTrackData() const
{
    return m_trackData;
}

void DjEngine::resetTrackLoadState()
{
    m_trackData->clear();
    m_currentSegments.clear();
    clearHotCueState();
    clearSavedLoopState();
    // Sentinel clearly outside the renderable pre-roll range so no cue is drawn
    // while no track is loaded.  The load path sets a real value after analysing
    // the first audible frame, so this value is never visible during playback.
    m_mainCueSec = -(PRE_ROLL_SECONDS + 1.0);
    resetMainCueButtonState();
    emit segmentsChanged();
    emit hotCuesChanged();
    emit savedLoopsChanged();
    emit beatgridLockedChanged();
}

void DjEngine::populateMetadataFromReader(const juce::AudioFormatReader& reader,
                                          const QString& rawPath,
                                          const juce::File& file)
{
    const auto metaMap = buildMetadataLookup(reader.metadataValues);

    m_trackTitle = metaValue(metaMap, {"title", "id3title", "tit2", "tt2", "name", "tracktitle", "song"});
    m_trackArtist = metaValue(metaMap, {"artist", "id3artist", "tpe1", "albumartist", "tpe2", "band", "performer", "leadartist"});
    m_trackAlbum = metaValue(metaMap, {"album", "id3album", "talb", "record", "albumtitle"});
    m_trackGenre = metaValue(metaMap, {"genre", "tcon", "contenttype"});
    m_trackComment = metaValue(metaMap, {"comment", "comm", "description"});
    m_trackKey = metaValue(metaMap, {"key", "tkey", "initialkey", "musickey", "keysig"});

    // Tag BPM is used immediately; the background analyzer will overwrite it later.
    const QString tagBpm = metaValue(metaMap, {"bpm", "tbpm", "tmpo", "tempo", "beatsperminute"});
    const double bpmVal = parseBpmString(tagBpm);
    if (bpmVal > 0.0)
        m_trackData->setBpmData(bpmVal, 0, reader.sampleRate);

    // TagLib fills gaps left by JUCE readers — JUCE's FLAC reader skips Vorbis comments entirely.
    {
        TagLib::FileRef tlFile(rawPath.toLocal8Bit().constData());
        if (!tlFile.isNull() && tlFile.tag()) {
            const TagLib::Tag* tag = tlFile.tag();
            const QString tlTitle   = cleanup(QString::fromStdString(tag->title().to8Bit(true)));
            const QString tlArtist  = cleanup(QString::fromStdString(tag->artist().to8Bit(true)));
            const QString tlGenre   = cleanup(QString::fromStdString(tag->genre().to8Bit(true)));
            const QString tlComment = cleanup(QString::fromStdString(tag->comment().to8Bit(true)));
            if (m_trackTitle.isEmpty()   && !tlTitle.isEmpty())   m_trackTitle   = tlTitle;
            if (m_trackArtist.isEmpty()  && !tlArtist.isEmpty())  m_trackArtist  = tlArtist;
            if (m_trackGenre.isEmpty()   && !tlGenre.isEmpty())   m_trackGenre   = tlGenre;
            if (m_trackComment.isEmpty() && !tlComment.isEmpty()) m_trackComment = tlComment;
        }
    }

    const auto v1 = readId3v1(rawPath);
    if (v1) {
        if (m_trackTitle.isEmpty() && !v1->title.isEmpty())
            m_trackTitle = v1->title;
        if (m_trackArtist.isEmpty() && !v1->artist.isEmpty())
            m_trackArtist = v1->artist;
        if (m_trackAlbum.isEmpty() && !v1->album.isEmpty())
            m_trackAlbum = v1->album;
    }

    const QString baseName = cleanup(QString::fromStdString(file.getFileNameWithoutExtension().toStdString()));
    filenameHeuristic(baseName, m_trackTitle, m_trackArtist);
}

void DjEngine::updateTrackDuration(double durationSec)
{
    m_trackDurationSec = durationSec;
    const int mins = static_cast<int>(durationSec) / 60;
    const int secs = static_cast<int>(durationSec) % 60;
    m_trackDuration = QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
}

bool DjEngine::hydrateLibraryStateForTrack(const QString& rawPath, double durationSec)
{
    if (!m_libraryDb)
        return false;

    const int durSec = static_cast<int>(durationSec);
    int bitrateKbps = 0;
    const juce::File file(rawPath.toStdString());
    if (durationSec > 0.0) {
        const auto bytes = static_cast<double>(file.getSize());
        bitrateKbps = static_cast<int>(std::lround((bytes * 8.0) / durationSec / 1000.0));
    }

    // Prefer the existing DB id for this file path so that analysis data and cue points
    // are preserved even when metadata (and thus a freshly-generated hash) has changed.
    const QString existingId = m_libraryDb->trackIdForFilePath(rawPath);
    m_currentTrackId    = existingId.isEmpty()
        ? TrackIdGenerator::generate(m_trackArtist, m_trackTitle, durSec, rawPath)
        : existingId;
    m_playLogged       = false;
    m_playedAccumSec   = 0.0;
    m_playHistoryClock.restart();
    m_libraryDb->addTrack(m_currentTrackId,
                          m_trackTitle, m_trackArtist, durSec, rawPath, bitrateKbps,
                          m_trackGenre, m_trackAlbum, m_trackComment);

    bool hasDbAnalysis = false;
    LibraryDatabase::AnalysisSnapshot cachedAnalysis;
    if (m_libraryDb->tryGetAnalysisData(m_currentTrackId, &cachedAnalysis)
        && cachedAnalysis.isAnalyzed) {
        hasDbAnalysis = true;
        m_currentSegments = m_libraryDb->trackSegmentsForTrack(m_currentTrackId);
        emit segmentsChanged();

        if (cachedAnalysis.bpm > 0.0) {
            m_trackData->setBpmData(cachedAnalysis.bpm,
                                    cachedAnalysis.firstBeatSample,
                                    cachedAnalysis.sampleRate,
                                    cachedAnalysis.beatGrid,
                                    cachedAnalysis.confidence,
                                    cachedAnalysis.beatGridInfo);
        }

        const QString cachedKey = cachedAnalysis.key.trimmed();
        if (!cachedKey.isEmpty()) {
            m_trackKey = cachedKey;
            m_trackData->setKeyData(cachedKey);
        }

        std::vector<TrackSegment> cachedSegments;
        cachedSegments.reserve(static_cast<size_t>(m_currentSegments.size()));
        for (const QVariant& value : m_currentSegments) {
            const QVariantMap map = value.toMap();
            TrackSegment segment;
            segment.label = map.value(QStringLiteral("label")).toString();
            segment.startTime = static_cast<float>(map.value(QStringLiteral("startTime")).toDouble());
            segment.endTime = static_cast<float>(map.value(QStringLiteral("endTime")).toDouble());
            segment.colorHex = map.value(QStringLiteral("colorHex")).toString();
            segment.confidence = static_cast<float>(map.value(QStringLiteral("confidence")).toDouble());
            if (segment.endTime > segment.startTime + 0.01f)
                cachedSegments.push_back(segment);
        }
        if (!cachedSegments.empty())
            m_trackData->setSegmentsData(std::move(cachedSegments));
    } else {
        // Strictly hide segment UI state until fresh analysis writes data.
        m_currentSegments = QVariantList();
        emit segmentsChanged();
    }

    loadHotCuesForCurrentTrack();
    loadSavedLoopsForCurrentTrack();
    loadMainCueForCurrentTrack();
    emit beatgridLockedChanged();
    return hasDbAnalysis;
}

void DjEngine::attachReaderToTransport(juce::AudioFormatReader* bufferedReader,
                                       juce::AudioFormatReader* directReader)
{
    static constexpr int kReaderReadAheadSamples = 1 << 18;
    static constexpr int kReaderReadAheadChannels = 2;

    const double scratchResumePos = m_scrubHoldPosition >= 0.0 ? m_scrubHoldPosition : 0.0;
    transportSource.stop();
    if (scratchBridge)
        scratchBridge->beginTransportSwap();
    transportSource.setSource(nullptr);

    reverseWrapSource.reset();
    bufferedReaderSource.reset();
    directReaderSource.reset();
    readerSource.reset();

    readerSource = std::make_unique<juce::AudioFormatReaderSource>(bufferedReader, true);
    directReaderSource = std::make_unique<juce::AudioFormatReaderSource>(directReader, true);
    bufferedReaderSource = std::make_unique<juce::BufferingAudioSource>(
        readerSource.get(),
        readAheadThread,
        false,
        kReaderReadAheadSamples,
        kReaderReadAheadChannels,
        true);
    reverseWrapSource = std::make_unique<ReverseStreamAudioSource>(
        bufferedReaderSource.get(),
        directReaderSource.get());
    reverseWrapSource->setReverse(m_isReverse);
    transportSource.setSource(reverseWrapSource.get(), 0, nullptr, bufferedReader->sampleRate);
    m_loadedTrackSampleRate = bufferedReader->sampleRate;
    transportSource.setPosition(0.0);
    syncScratchBridgeToTransport();
    terminateScratchSession(scratchResumePos);
    if (scratchBridge)
        scratchBridge->endTransportSwap();
    ensureTransportRunningForPlayIntent();
}

void DjEngine::ejectTrack()
{
    ++m_loadGen;
    if (m_analyzer)
        m_analyzer->stopAnalysis();

    m_playRequested = false;
    transportSource.stop();
    transportSource.setSource(nullptr);
    reverseWrapSource.reset();
    bufferedReaderSource.reset();
    directReaderSource.reset();
    readerSource.reset();
    terminateScratchSession(0.0);

    resetTrackLoadState();
    m_trackTitle.clear();   m_trackArtist.clear();  m_trackAlbum.clear();
    m_trackGenre.clear();   m_trackComment.clear();
    m_trackKey.clear();     m_trackDuration.clear(); m_trackDurationSec = 0.0;
    m_hasCoverArt = false; m_coverArtUrl.clear();
    if (m_coverProvider)
        m_coverProvider->clearCover(m_deckId);
    m_hasTrack = false;
    emit trackMetadataChanged();
    emit progressChanged();
}

void DjEngine::loadTrack(const QString& rawPath)
{
    juce::File file(rawPath.toStdString());

    if (!file.existsAsFile()) {
        qWarning() << "File does not exist:" << rawPath;
        return;
    }

    const quint64 gen = ++m_loadGen;

    // Stop any in-flight analysis before clearing TrackData — otherwise the
    // analyzer thread keeps writing into cleared/replaced buffers (UI freeze).
    if (m_analyzer)
        m_analyzer->stopAnalysis();

    // Immediately clear previous track state so the UI shows a clean slate.
    resetTrackLoadState();
    m_trackTitle.clear();   m_trackArtist.clear();  m_trackAlbum.clear();
    m_trackGenre.clear();   m_trackComment.clear();
    m_trackKey.clear();     m_trackDuration.clear(); m_trackDurationSec = 0.0;
    m_hasCoverArt = false; m_coverArtUrl.clear();
    if (m_coverProvider)
        m_coverProvider->clearCover(m_deckId);
    emit trackMetadataChanged();

    // All heavy file I/O (format detection, cover art, waveform cache) runs off the
    // main thread so the UI stays responsive during loading.
    const int pps = static_cast<int>(WAVEFORM_POINTS_PER_SECOND);
    auto analysisState = std::make_shared<std::atomic<bool>>(false);
    std::thread([this, rawPath, file, gen, pps, analysisState]() {
        std::lock_guard<std::mutex> loadGuard(m_loadMutex);
        if (m_loadGen != gen)
            return;

        auto* reader = formatManager.createReaderFor(file);
        if (!reader) {
            qWarning() << "[DjEngine] loadTrack: unsupported or unreadable format:" << rawPath;
            return;
        }
        auto* directReader = formatManager.createReaderFor(file);
        if (!directReader) {
            qWarning() << "[DjEngine] loadTrack: could not create direct reader:" << rawPath;
            delete reader;
            return;
        }

        // Waveform cache + instant overview must finish BEFORE the transport reader
        // is handed to the main thread — sharing one reader across threads corrupts
        // JUCE's internal read state and has caused segfaults on load.
        WaveformCache::Payload cache;
        bool wfLoaded = WaveformCache::loadForFile(rawPath, pps, &cache)
                        && !cache.waveform.isEmpty()
                        && !cache.rgb.isEmpty();
        if (wfLoaded) {
            const int expected = cache.totalExpected > 0 ? cache.totalExpected : cache.waveform.size();
            wfLoaded = expected > 0
                       && cache.waveform.size() >= static_cast<int>(expected * 0.98)
                       && cache.rgb.size()      >= static_cast<int>(expected * 0.98);
        }

        QVector<TrackData::RgbWaveformFrame> instantOvr;
        int instantExpected = 0;
        if (!wfLoaded) {
            if (auto* ovrReader = formatManager.createReaderFor(file)) {
                instantOvr = WaveformAnalyzer::buildInstantOverview(ovrReader);
                const double durationSec =
                    static_cast<double>(ovrReader->lengthInSamples) / ovrReader->sampleRate;
                instantExpected = static_cast<int>(durationSec * pps);
                delete ovrReader;
            }
        }

        QMetaObject::invokeMethod(this,
            [this, gen, reader, directReader, file, rawPath, analysisState]() mutable
            {
                if (m_loadGen != gen) {
                    delete reader;
                    delete directReader;
                    return;
                }

                m_hasTrack = true;
                attachReaderToTransport(reader, directReader);

                populateMetadataFromReader(*reader, rawPath, file);
                const double durationSec =
                    static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
                updateTrackDuration(durationSec);
                clearLoop();

                const bool hasDbAnalysis = hydrateLibraryStateForTrack(rawPath, durationSec);
                analysisState->store(hasDbAnalysis, std::memory_order_relaxed);

                emit trackMetadataChanged();
                emit trackLoaded();
                emit progressChanged();
            },
            Qt::QueuedConnection);

        QMetaObject::invokeMethod(this,
            [this, gen, rawPath,
             cache           = std::move(cache),
             instantOvr        = std::move(instantOvr),
             instantExpected,
             analysisState,
             wfLoaded]() mutable
            {
                if (m_loadGen != gen)
                    return;

                // Defer bulk waveform hand-off so library/deck UI can process input first.
                QTimer::singleShot(0, this, [this, gen, rawPath,
                                             cache           = std::move(cache),
                                             instantOvr        = std::move(instantOvr),
                                             instantExpected,
                                             analysisState,
                                             wfLoaded]() mutable
                {
                    if (m_loadGen != gen)
                        return;

                    if (wfLoaded) {
                        const int expected =
                            cache.totalExpected > 0 ? cache.totalExpected : cache.waveform.size();
                        m_trackData->setTotalExpected(expected);
                        m_trackData->replaceAllData(
                            std::move(cache.waveform), std::max(0.001f, cache.globalMaxPeak));
                        m_trackData->setRgbWaveformData(std::move(cache.rgb));
                        if (!cache.peakMip.isEmpty())
                            m_trackData->setPeakMipData(std::move(cache.peakMip));
                    } else if (!instantOvr.isEmpty()) {
                        m_trackData->setTotalExpected(std::max(1, instantExpected));
                        m_trackData->setOverviewRgbData(std::move(instantOvr));
                    }

                    const bool hasDbAnalysis = analysisState->load(std::memory_order_relaxed);
                    if (!(wfLoaded && hasDbAnalysis))
                        m_analyzer->startAnalysis(rawPath, transportSource.getCurrentPosition());
                });
            },
            Qt::QueuedConnection);

        // Remaining heavy I/O tasks are intentionally decoupled from waveform
        // startup. They can finish later without holding back progressive paint.
        QImage coverImage;
        const QByteArray coverData = CoverArtExtractor::extractCoverArt(rawPath).first;
        if (!coverData.isEmpty())
            coverImage.loadFromData(coverData);

        double autoCueSec = -1.0;
        {
            auto* cueReader = formatManager.createReaderFor(file);
            if (cueReader) {
                const double sr = cueReader->sampleRate;
                if (sr > 0.0) {
                    static constexpr double kMaxScanSec      = 10.0;
                    static constexpr float  kSilenceThreshold = 0.001f; // ~-60 dBFS
                    static constexpr int    kBlockSize        = 1024;

                    const juce::int64 maxScan = static_cast<juce::int64>(sr * kMaxScanSec);
                    const int numCh = static_cast<int>(std::max<unsigned int>(cueReader->numChannels, 1u));
                    juce::AudioBuffer<float> buf(numCh, kBlockSize);

                    juce::int64 firstAudibleSample = -1;
                    for (juce::int64 pos = 0; pos < maxScan && firstAudibleSample < 0; pos += kBlockSize) {
                        const int toRead = static_cast<int>(
                            std::min<juce::int64>(kBlockSize, maxScan - pos));
                        buf.clear();
                        cueReader->read(&buf, 0, toRead, pos, true, true);
                        for (int i = 0; i < toRead && firstAudibleSample < 0; ++i) {
                            for (int ch = 0; ch < numCh; ++ch) {
                                if (std::abs(buf.getSample(ch, i)) >= kSilenceThreshold) {
                                    firstAudibleSample = pos + i;
                                    break;
                                }
                            }
                        }
                    }

                    if (firstAudibleSample > 0)
                        autoCueSec = static_cast<double>(firstAudibleSample) / sr;
                }
                delete cueReader;
            }
        }

        QMetaObject::invokeMethod(this,
            [this, gen,
             coverImage = std::move(coverImage),
             autoCueSec]() mutable
            {
                if (m_loadGen != gen)
                    return;

                if (!coverImage.isNull() && m_coverProvider) {
                    m_coverProvider->setCoverImage(m_deckId, coverImage);
                    m_coverArtUrl = QString("image://coverart/%1?t=%2")
                                        .arg(m_deckId)
                                        .arg(QDateTime::currentMSecsSinceEpoch());
                    m_hasCoverArt = true;

                    if (m_libraryCoverService && !m_currentTrackId.isEmpty()) {
                        QByteArray coverBytes;
                        QBuffer coverBuffer(&coverBytes);
                        coverBuffer.open(QIODevice::WriteOnly);
                        if (coverImage.save(&coverBuffer, "JPG"))
                            m_libraryCoverService->publishCover(m_currentTrackId, coverBytes);
                    }

                    emit trackMetadataChanged();
                }

                if (autoCueSec > 0.0
                    && m_mainCueSec < 0.0
                    && !m_playRequested
                    && !transportSource.isPlaying()) {
                    m_mainCueSec = autoCueSec;
                    emit mainCueChanged();
                    transportSource.setPosition(autoCueSec);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void DjEngine::togglePlay()
{
    if (m_playRequested) {
        m_playRequested = false;
        if (mixerSource)
            mixerSource->armClickFreeTransition();
        resetMainCueButtonState();
        m_preRollCountdownActive = false;
        // Always freeze: calling stop() on an already-stopped transport is safe.
        // Skipping this when isPlaying()==false left the transport in a live state
        // whenever there was a brief race between the audio thread and this call.
        freezeTransportAt(getVisualPosition());
    } else {
        m_playRequested = true;
        if (m_scrubHoldPosition < 0.0) {
            m_preRollCountdownActive = true;
            m_preRollVisualStartPos = m_scrubHoldPosition;
            m_preRollClock.restart();
        } else {
            ensureTransportRunningForPlayIntent();
        }
    }

    emit playingChanged();
}

void DjEngine::play()
{
    if (!m_playRequested)
        m_playRequested = true;

    if (m_scrubHoldPosition < 0.0 && !m_preRollCountdownActive) {
        m_preRollCountdownActive = true;
        m_preRollVisualStartPos = m_scrubHoldPosition;
        m_preRollClock.restart();
    } else {
        ensureTransportRunningForPlayIntent();
    }
    emit playingChanged();
}

void DjEngine::pause()
{
    if (!m_playRequested && !transportSource.isPlaying() && !m_preRollCountdownActive)
        return; // Already paused

    m_playRequested = false;
    if (mixerSource)
        mixerSource->armClickFreeTransition();
    resetMainCueButtonState();
    m_preRollCountdownActive = false;
    freezeTransportAt(getVisualPosition());
    emit playingChanged();
}

void DjEngine::ensureTransportRunningForPlayIntent()
{
    if (!m_playRequested)
        return;

    // Pre-roll countdown manages its own transport start — don't interfere.
    if (m_preRollCountdownActive)
        return;

    if (m_scratch.scrubbing() || m_scratch.releaseGlide() || !m_hasTrack) {
        return;
    }

    if (deviceManager.getCurrentAudioDevice() == nullptr) {
        qWarning() << "[DjEngine] Play requested without active audio device; trying to recover";
        const juce::String initErr = deviceManager.initialiseWithDefaultDevices(0, 2);
        if (initErr.isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr) {
            qWarning() << "[DjEngine] Could not recover audio device on play";
            return;
        }
        refreshHardwareLatency();
    }

    if (transportSource.isPlaying()) {
        return;
    }

    const double len = transportSource.getLengthInSeconds();
    if (len <= 0.0) {
        qWarning() << "[DjEngine] cannot start transport: invalid length" << len;
        return;
    }

    // Keep transport stopped at true EOF. As soon as the playhead is moved
    // back from the end, this function resumes playback automatically.
    const double pos = transportSource.getCurrentPosition();
    if (pos >= len - 0.0001) {
        return;
    }

    armSnapFromTransportPosition();
    transportSource.start();
}

void DjEngine::setSnapAnchor(double positionSec, bool valid)
{
    m_snapPosition = positionSec;
    m_snapTempoRatio = getTempoRatio();
    m_snapClock.restart();
    m_snapValid = valid;
    m_atomicPlayheadPos.store(positionSec, std::memory_order_relaxed);
}

void DjEngine::armVisualSeekSettle()
{
    m_visualSeekSettleClock.restart();
}

void DjEngine::armSnapFromTransportPosition()
{
    setSnapAnchor(transportSource.getCurrentPosition(), true);
}

void DjEngine::freezeTransportAt(double positionSec)
{
    transportSource.stop();
    transportSource.setPosition(std::max(0.0, positionSec));
    m_snapValid = false;
    // Keep m_scrubHoldPosition in sync so getPosition() returns the right value
    // when stopped in pre-roll (where transport is clamped at 0).
    m_scrubHoldPosition = positionSec;
    m_atomicPlayheadPos.store(positionSec, std::memory_order_relaxed);
}

engine::scratch::ScratchLoopCtx DjEngine::scratchLoopCtx() const noexcept
{
    engine::scratch::ScratchLoopCtx ctx;
    ctx.active = m_loopActive && (m_loopOutSec > m_loopInSec);
    ctx.inSec = m_loopInSec;
    ctx.outSec = m_loopOutSec;
    return ctx;
}

void DjEngine::syncScratchBridgeToTransport()
{
    if (!scratchBridge)
        return;

    const double len = std::max(0.0, transportSource.getLengthInSeconds());
    scratchBridge->configureTrack(m_loadedTrackSampleRate, len);
    // Scratch pulls must seek the file directly — the buffered transport reader
    // causes dropouts/aliasing when the scratch resampler jumps read positions.
    scratchBridge->setScratchInputSource(directReaderSource
                                            ? static_cast<juce::AudioSource*>(directReaderSource.get())
                                            : reverseWrapSource.get());
    scratchBridge->setReverse(m_isReverse);
    scratchBridge->setLoopRangeSeconds(m_loopInSec, m_loopOutSec, m_loopActive, m_loadedTrackSampleRate);
    scratchBridge->setDeckTempoRatio(getTempoRatio());
    scratchBridge->setKeylockPassthrough(m_keylock);

    const double pos = std::max(0.0, transportSource.getCurrentPosition());
    scratchBridge->syncReadPositionSeconds(pos, m_loadedTrackSampleRate);
}

void DjEngine::terminateScratchSession(double positionSec)
{
    m_scratch.clear();
    m_scratchSnapReadPending = false;

    if (scratchBridge)
        scratchBridge->exitScratchMode(std::max(0.0, positionSec), m_loadedTrackSampleRate);
}

void DjEngine::setPosition(float progress)
{
    double len = transportSource.getLengthInSeconds();
    if (len > 0.0) {
        const double newPos = std::clamp(static_cast<double>(progress) * len,
                                         -PRE_ROLL_SECONDS,
                                         len);
        const double previousPos = getVisualPosition();
        if (m_playLogged && (newPos < previousPos - 15.0 || newPos <= len * 0.10)) {
            m_playLogged = false;
            m_playedAccumSec = 0.0;
            m_playHistoryClock.restart();
        }
        m_preRollCountdownActive = false;
        if (newPos < 0.0) {
            transportSource.stop();
            transportSource.setPosition(0.0);
            m_scrubHoldPosition = newPos;
            m_atomicPlayheadPos.store(newPos, std::memory_order_relaxed);
            m_snapValid = false;
            if (m_playRequested) {
                m_preRollCountdownActive = true;
                m_preRollVisualStartPos = newPos;
                m_preRollClock.restart();
            }
        } else {
            transportSource.setPosition(newPos);
            m_scrubHoldPosition = newPos;
            ensureTransportRunningForPlayIntent();
            armSnapFromTransportPosition();
            armVisualSeekSettle();
        }
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(std::max(0.0, newPos));
    }
    emit progressChanged();
}

void DjEngine::updateScrubPlayheadAnchor()
{
    // Pre-roll: no audio data exists before t=0.  Stop the transport to prevent
    // frame-0 audio leaking through the resampler while the platter is in silence.
    if (m_scrubHoldPosition < 0.0 && transportSource.isPlaying())
        transportSource.stop();

    if ((m_scratch.scrubbing() || m_scratch.releaseGlide()) && m_scrubHoldPosition >= 0.0) {
        // Release glide: hold position is authoritative (vinyl deltas + coast).
        // Reading transport back while resampling lags behind causes a brief
        // slow-down / catch-up wobble when the wheel is still spinning out.
        transportSource.setPosition(std::max(0.0, m_scrubHoldPosition));
        m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_relaxed);
        m_snapPosition = m_scrubHoldPosition;
        return;
    }

    // Only sync from transport when not in pre-roll: transport is clamped at 0
    // while visual position is negative, so syncing would erase the pre-roll offset.
    if (m_scrubHoldPosition >= 0.0)
        m_scrubHoldPosition = transportSource.getCurrentPosition();
    m_atomicPlayheadPos.store(m_scrubHoldPosition,
                              std::memory_order_relaxed);
    m_snapPosition = m_scrubHoldPosition;
}

void DjEngine::onTimer()
{
    if (m_scratch.scrubbing() || m_scratch.releaseGlide()) {
        tickScratchPhysics();
        return;
    }

    if (mixerSource)
        mixerSource->setScratchTimbre(0.0f);

    decayJogNudge();

    // Safety: transport must not run when there's no play intent and no cue preview.
    // Catches any leftover transport state that togglePlay/pause missed.
    if (transportSource.isPlaying() && !m_playRequested && !m_mainCuePreviewActive) {
        freezeTransportAt(transportSource.getCurrentPosition());
        emit vuLevelChanged();
        emit gainReductionChanged();
        return;
    }

    // Safety: transport must not run during pre-roll countdown (visual position is
    // negative; transport is clamped at 0 and driven by wall-clock).
    if (transportSource.isPlaying() && m_preRollCountdownActive)
        transportSource.stop();

    if (transportSource.isPlaying()) {
        if (!tickTransportPlaying())
            return;
        // Accumulate real audible playback time for play-count logging. Use wall
        // time instead of assuming the control timer fires exactly every 4 ms.
        if (!m_playLogged && !m_currentTrackId.isEmpty() && m_playRequested) {
            const double elapsedSec = m_playHistoryClock.isValid()
                ? static_cast<double>(m_playHistoryClock.restart()) / 1000.0
                : 0.0;
            m_playedAccumSec += std::clamp(elapsedSec, 0.0, 0.25);

            const double playheadSec = std::max(0.0, static_cast<double>(getVisualPosition()));
            const double nearEndSec = m_trackDurationSec > 0.0 ? m_trackDurationSec * 0.80 : 1e9;
            const double thresholdSec = playHistoryThresholdSeconds(m_trackDurationSec);
            const bool enoughPlayback = m_playedAccumSec >= thresholdSec;
            const bool mixedNearEnd = playheadSec >= nearEndSec
                && m_playedAccumSec >= std::min(6.0, thresholdSec * 0.6);

            if (enoughPlayback || mixedNearEnd) {
                m_playLogged = true;
                if (m_libraryDb)
                    m_libraryDb->logPlay(m_currentTrackId);
            }
        }
    } else {
        m_playHistoryClock.restart();
        tickTransportStopped();
    }

    if (m_syncEnabled && !m_isSyncMaster && !m_scratch.scrubbing() && !m_scratch.releaseGlide())
        updatePhaseCorrection();

    updateFxBeatSyncPosition();
    emit vuLevelChanged();
    emit gainReductionChanged();
}

void DjEngine::tickScratchPhysics()
{
    if (!scratchBridge)
        return;

    auto& physicsClock = m_scratch.physicsClock();
    const double dtSec = physicsClock.isValid()
        ? std::clamp(static_cast<double>(physicsClock.nsecsElapsed()) * 1e-9, 0.001, 0.050)
        : 0.016;
    physicsClock.restart();

    const double scratchRate = m_scratch.tick(scratchBridge.get(), dtSec);
    const double absRate = std::abs(scratchRate);

    if (mixerSource) {
        const double timbreSignal = std::clamp(std::sqrt(std::max(absRate, 0.08)), 0.18, 1.0);
        mixerSource->setScratchTimbre(static_cast<float>(timbreSignal));
    }

    if (m_scratch.scrubbing() || m_scratch.releaseGlide()) {
        m_scrubHoldPosition = m_scratch.scrubbing()
            ? m_scratch.lastRawSec()
            : scratchBridge->targetPositionSeconds(m_loadedTrackSampleRate);
        updateScrubPlayheadAnchor();
    }

    if (!m_scratch.scrubbing() && m_scratch.releaseGlide() && !scratchBridge->isInertiaActive()) {
        m_scratch.setReleaseGlide(false);
        restorePostScrubPlaybackState();
        if (mixerSource)
            mixerSource->setScratchTimbre(0.0f);
        emit scrubbingChanged();
    }

    m_snapTempoRatio = getTempoRatio();
    emitPlaybackStateChanged();
}

void DjEngine::decayJogNudge()
{
    if (m_jogNudgePercent == 0.0)
        return;

    // Fade jog outer-rim nudge back to 0% after ~150ms of no new jog events.
    const double idleSec = m_lastJogNudgeClock.isValid()
        ? static_cast<double>(m_lastJogNudgeClock.nsecsElapsed()) * 1e-9
        : 1.0;
    if (idleSec > 0.080) {
        constexpr double kNudgeDecayTau = 0.080;
        const double alpha = 1.0 - std::exp(-idleSec / kNudgeDecayTau);
        m_jogNudgePercent -= m_jogNudgePercent * alpha;
        if (std::abs(m_jogNudgePercent) < 0.05)
            m_jogNudgePercent = 0.0;
        updateSpeedAndPitch();
    }
}

// Returns true if onTimer should continue to the phase-correction/VU path,
// false if it should return immediately (pre-roll loop wrap triggered).
bool DjEngine::tickTransportPlaying()
{
    // Store a fresh snapshot from the transport each control tick.
    // Interpolation happens only between these anchors.
    const double dtSec = m_snapValid
        ? std::clamp(static_cast<double>(m_snapClock.nsecsElapsed()) * 1e-9, 0.001, 0.050)
        : 0.004;

    const double measuredPos = transportSource.getCurrentPosition();
    m_snapPosition   = measuredPos;
    m_snapTempoRatio = getTempoRatio();

    // Slip shadow: advance continuously while loop/reverse diverts the transport.
    if (isSlipDiverted()) {
        const double dur = std::max(0.001, static_cast<double>(getDuration()));
        m_slipPosition = std::min(m_slipPosition + dtSec * getTempoRatio(), dur);
    } else {
        m_slipPosition = measuredPos;
    }

    if (m_loopActive && m_loopOutSec > m_loopInSec) {
        if (m_isReverse && m_snapPosition <= m_loopInSec) {
            transportSource.setPosition(m_loopOutSec);
            m_snapPosition = m_loopOutSec;
        } else if (!m_isReverse && m_loopInSec < 0.0
                   && m_snapPosition >= m_loopOutSec) {
            // Loop with pre-roll IN point: the audio source cannot enforce this
            // (no negative samples).  Software-wrap: stop transport and start a
            // pre-roll countdown from the true (negative) loop-in position.
            transportSource.stop();
            m_snapValid = false;
            m_scrubHoldPosition = m_loopInSec;
            m_preRollCountdownActive = true;
            m_preRollVisualStartPos  = m_loopInSec;
            m_preRollClock.restart();
            m_atomicPlayheadPos.store(m_loopInSec, std::memory_order_relaxed);
            emitPlaybackStateChanged();
            return false;
        }
    }

    m_snapClock.restart();
    m_snapValid = true;
    m_atomicPlayheadPos.store(m_snapPosition, std::memory_order_relaxed);
    emit progressChanged();
    return true;
}

void DjEngine::tickTransportStopped()
{
    m_snapValid = false;
    if (m_preRollCountdownActive) {
        // Advance visual position from negative toward 0 at playback rate.
        const double elapsed   = static_cast<double>(m_preRollClock.nsecsElapsed()) * 1e-9;
        const double visualPos = m_preRollVisualStartPos + elapsed * getTempoRatio();
        m_scrubHoldPosition = visualPos;
        m_atomicPlayheadPos.store(visualPos, std::memory_order_relaxed);

        // Loop entirely in pre-roll: both endpoints negative, wrap back to loop IN.
        if (m_loopActive && m_loopOutSec <= 0.0 && m_loopOutSec > m_loopInSec
                && visualPos >= m_loopOutSec) {
            m_preRollVisualStartPos = m_loopInSec;
            m_scrubHoldPosition     = m_loopInSec;
            m_preRollClock.restart();
            m_atomicPlayheadPos.store(m_loopInSec, std::memory_order_relaxed);
            emit progressChanged();
            return;
        }

        if (visualPos >= 0.0) {
            m_preRollCountdownActive = false;
            m_scrubHoldPosition = 0.0;
            transportSource.setPosition(0.0);
            armSnapFromTransportPosition();
            transportSource.start();
            // For loops crossing 0: re-apply audio-source loop now that transport
            // is in-track.  applyLoopRangeToAudioSource() was a no-op while
            // loopIn was negative; with loopIn < 0 the audio loop [0, loopOut]
            // is software-enforced via the transport-playing branch above.
        }
        emit progressChanged();
    } else {
        ensureTransportRunningForPlayIntent();
        // In pre-roll the transport is clamped at 0, so syncing from transport
        // erases the negative visual position. Preserve m_scrubHoldPosition when negative.
        const double transportPos = transportSource.getCurrentPosition();
        const double atomicPos    = (m_scrubHoldPosition < 0.0)
            ? m_scrubHoldPosition
            : transportPos;
        m_atomicPlayheadPos.store(atomicPos, std::memory_order_relaxed);
    }
}

float DjEngine::vuLevelL() const
{
    return mixerSource ? mixerSource->m_peakL.load(std::memory_order_relaxed) : 0.0f;
}

float DjEngine::vuLevelR() const
{
    return mixerSource ? mixerSource->m_peakR.load(std::memory_order_relaxed) : 0.0f;
}

float DjEngine::preFaderVuLevelL() const
{
    return mixerSource ? mixerSource->m_preFaderPeakL.load(std::memory_order_relaxed) : 0.0f;
}

float DjEngine::preFaderVuLevelR() const
{
    return mixerSource ? mixerSource->m_preFaderPeakR.load(std::memory_order_relaxed) : 0.0f;
}

bool DjEngine::clipDetected() const
{
    // Master clip detection now comes from DjMasterBus (summed signal).
    return DjMasterBus::masterClipDetected_s();
}

float DjEngine::gainReduction() const
{
    return DjMasterBus::gainReduction();
}

juce::AudioSource* DjEngine::getAudioSource() const
{
    return mixerSource.get();
}

const juce::AudioBuffer<float>& DjEngine::getPflBuffer() const
{
    return mixerSource->getPflBuffer();
}

QVariantList DjEngine::hotCues() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_hotCueSlots.size()));

    for (size_t i = 0; i < m_hotCueSlots.size(); ++i) {
        const auto& slot = m_hotCueSlots[i];
        QVariantMap m;
        m.insert("index",       static_cast<int>(i));
        m.insert("set",         slot.set);
        m.insert("positionSec", slot.positionSec);
        m.insert("label",       slot.label);
        m.insert("color",       slot.color);
        out.push_back(m);
    }

    return out;
}

bool DjEngine::isValidHotCueIndex(int index) const
{
    return index >= 0 && index < static_cast<int>(m_hotCueSlots.size());
}

void DjEngine::clearHotCueState()
{
    for (size_t i = 0; i < m_hotCueSlots.size(); ++i) {
        auto& slot = m_hotCueSlots[i];
        slot.set = false;
        slot.positionSec = 0.0;
        slot.label.clear();
        slot.color = defaultHotCueColor(static_cast<int>(i));
    }
}

void DjEngine::loadHotCuesForCurrentTrack()
{
    clearHotCueState();

    if (!m_libraryDb || m_currentTrackId.isEmpty()) {
        emit hotCuesChanged();
        return;
    }

    const QVariantList stored = m_libraryDb->cuePointsForTrack(m_currentTrackId);
    for (const QVariant& v : stored) {
        const QVariantMap m = v.toMap();
        const int index = m.value("index").toInt();
        if (!isValidHotCueIndex(index))
            continue;

        auto& slot = slotAt(index);
        slot.set = true;
        slot.positionSec = std::max(-PRE_ROLL_SECONDS, m.value("positionSec").toDouble());
        slot.label = m.value("label").toString();
        const QString color = m.value("color").toString().trimmed();
        slot.color = color.isEmpty() ? defaultHotCueColor(index) : color;
    }

    emit hotCuesChanged();
}

void DjEngine::persistHotCueSlot(int index)
{
    if (!isValidHotCueIndex(index) || !m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const auto& slot = slotAt(index);
    if (slot.set) {
        const QString label = slot.label.isEmpty()
            ? QStringLiteral("HOT CUE %1").arg(index + 1)
            : slot.label;
        m_libraryDb->upsertCuePoint(m_currentTrackId, index, slot.positionSec, label, slot.color);
    } else {
        m_libraryDb->deleteCuePoint(m_currentTrackId, index);
    }
}

bool DjEngine::isHotCuePad(int index) const
{
    return isValidHotCueIndex(index) && slotAt(index).set;
}

bool DjEngine::isLoopCuePad(int index) const
{
    return isValidSavedLoopIndex(index) && savedLoopAt(index).set;
}

bool DjEngine::hasStorableLoopRegion() const
{
    return m_loopInSet && m_loopOutSec > m_loopInSec + 0.001;
}

void DjEngine::storeHotCue(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    if (isLoopCuePad(index))
        clearSavedLoop(index);

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    auto& slot = slotAt(index);
    slot.set = true;
    slot.positionSec = std::clamp(static_cast<double>(getVisualPosition()), -PRE_ROLL_SECONDS, trackLen);
    if (slot.color.isEmpty())
        slot.color = defaultHotCueColor(index);
    if (slot.label.isEmpty())
        slot.label = QStringLiteral("HOT CUE %1").arg(index + 1);

    persistHotCueSlot(index);
    emit hotCuesChanged();
}

void DjEngine::storeCuePad(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    if (hasStorableLoopRegion()) {
        if (isHotCuePad(index))
            clearHotCue(index);
        storeSavedLoop(index);
        return;
    }

    if (isLoopCuePad(index))
        clearSavedLoop(index);
    storeHotCue(index);
}

void DjEngine::triggerHotCueJump(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack || !isHotCuePad(index))
        return;

    const auto& slot = slotAt(index);
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double pos = std::clamp(slot.positionSec, -PRE_ROLL_SECONDS, trackLen);
    transportSource.setPosition(std::max(0.0, pos));
    m_scrubHoldPosition = pos;
    if (m_playRequested && pos < 0.0) {
        m_preRollCountdownActive = true;
        m_preRollVisualStartPos = pos;
        m_preRollClock.restart();
    } else {
        ensureTransportRunningForPlayIntent();
    }
    setSnapAnchor(pos, true);
    armVisualSeekSettle();
    if (m_analyzer && m_analyzer->isThreadRunning())
        m_analyzer->setSeekHint(pos);
    emit progressChanged();
}

void DjEngine::triggerCuePad(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    if (isLoopCuePad(index)) {
        triggerSavedLoop(index);
        return;
    }

    if (isHotCuePad(index)) {
        triggerHotCueJump(index);
        return;
    }

    storeCuePad(index);
}

void DjEngine::triggerHotCue(int index)
{
    triggerCuePad(index);
}

void DjEngine::clearCuePad(int index)
{
    if (isLoopCuePad(index))
        clearSavedLoop(index);
    if (isHotCuePad(index))
        clearHotCue(index);
}

void DjEngine::clearHotCue(int index)
{
    if (!isValidHotCueIndex(index))
        return;

    auto& slot = slotAt(index);
    slot.set = false;
    slot.positionSec = 0.0;
    slot.label.clear();
    if (slot.color.isEmpty())
        slot.color = defaultHotCueColor(index);

    persistHotCueSlot(index);
    emit hotCuesChanged();
}

void DjEngine::setHotCueColor(int index, const QString& colorHex)
{
    if (!isValidHotCueIndex(index))
        return;

    QString color = colorHex.trimmed();
    if (color.isEmpty())
        color = defaultHotCueColor(index);

    auto& slot = slotAt(index);
    slot.color = color;

    if (slot.set)
        persistHotCueSlot(index);

    emit hotCuesChanged();
}

QVariantList DjEngine::savedLoops() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_savedLoopSlots.size()));

    for (size_t i = 0; i < m_savedLoopSlots.size(); ++i) {
        const auto& slot = m_savedLoopSlots[i];
        QVariantMap entry;
        entry.insert("index",       static_cast<int>(i));
        entry.insert("set",         slot.set);
        entry.insert("inSec",       slot.inSec);
        entry.insert("outSec",      slot.outSec);
        entry.insert("lengthBeats", slot.lengthBeats);
        entry.insert("label",       slot.label);
        entry.insert("color",       slot.color);
        out.push_back(entry);
    }

    return out;
}

bool DjEngine::isValidSavedLoopIndex(int index) const
{
    return index >= 0 && index < static_cast<int>(m_savedLoopSlots.size());
}

void DjEngine::clearSavedLoopState()
{
    for (size_t i = 0; i < m_savedLoopSlots.size(); ++i) {
        auto& slot = m_savedLoopSlots[i];
        slot.set = false;
        slot.inSec = 0.0;
        slot.outSec = 0.0;
        slot.lengthBeats = 0.0;
        slot.label.clear();
        slot.color = defaultSavedLoopColor(static_cast<int>(i));
    }
}

void DjEngine::loadSavedLoopsForCurrentTrack()
{
    clearSavedLoopState();

    if (!m_libraryDb || m_currentTrackId.isEmpty()) {
        emit savedLoopsChanged();
        return;
    }

    const QVariantList stored = m_libraryDb->savedLoopsForTrack(m_currentTrackId);
    for (const QVariant& v : stored) {
        const QVariantMap m = v.toMap();
        const int index = m.value("index").toInt();
        if (!isValidSavedLoopIndex(index))
            continue;

        auto& slot = savedLoopAt(index);
        slot.set = true;
        slot.inSec = m.value("inSec").toDouble();
        slot.outSec = m.value("outSec").toDouble();
        slot.label = m.value("label").toString();
        const QString color = m.value("color").toString().trimmed();
        slot.color = color.isEmpty() ? defaultSavedLoopColor(index) : color;

        const double beatDur = beatDurationAround(slot.inSec);
        if (beatDur > 1e-4)
            slot.lengthBeats = (slot.outSec - slot.inSec) / beatDur;
    }

    emit savedLoopsChanged();
}

void DjEngine::persistSavedLoopSlot(int index)
{
    if (!isValidSavedLoopIndex(index) || !m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const auto& slot = savedLoopAt(index);
    if (slot.set) {
        const QString label = slot.label.isEmpty()
            ? QStringLiteral("LOOP %1").arg(index + 1)
            : slot.label;
        m_libraryDb->upsertSavedLoop(m_currentTrackId,
                                     index,
                                     slot.inSec,
                                     slot.outSec,
                                     label,
                                     slot.color);
    } else {
        m_libraryDb->deleteSavedLoop(m_currentTrackId, index);
    }
}

void DjEngine::activateLoopRange(double inSec, double outSec, bool jumpToIn)
{
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    double in = std::clamp(inSec, -PRE_ROLL_SECONDS, trackLen);
    double out = std::clamp(outSec, -PRE_ROLL_SECONDS, trackLen);
    if (out <= in + 0.001)
        return;

    m_loopInSec = in;
    m_loopOutSec = out;
    m_loopInSet = true;
    m_loopActive = true;

    const double beatDur = beatDurationAround(in);
    if (beatDur > 1e-4) {
        constexpr double kMinLoopBeats = 1.0 / 64.0;
        constexpr double kMaxLoopBeats = 4096.0;
        const double beats = (out - in) / beatDur;
        m_loopLengthBeats = std::clamp(beats, kMinLoopBeats, kMaxLoopBeats);
    }

    applyLoopRangeToAudioSource();

    if (jumpToIn) {
        const double pos = std::max(0.0, in);
        transportSource.setPosition(pos);
        m_scrubHoldPosition = pos;
        setSnapAnchor(pos, true);
        armVisualSeekSettle();
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(pos);
        emit progressChanged();
    }

    emit loopChanged();
}

void DjEngine::storeSavedLoop(int index)
{
    if (!isValidSavedLoopIndex(index) || !m_hasTrack)
        return;

    if (isHotCuePad(index))
        clearHotCue(index);

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    double inSec = 0.0;
    double outSec = 0.0;

    if (m_loopInSet && m_loopOutSec > m_loopInSec + 0.001) {
        inSec = m_loopInSec;
        outSec = m_loopOutSec;
    } else if (m_loopActive && m_loopOutSec > m_loopInSec + 0.001) {
        inSec = m_loopInSec;
        outSec = m_loopOutSec;
    } else {
        const double pos = static_cast<double>(getVisualPosition());
        const double beatDur = beatDurationAround(pos);
        if (beatDur <= 1e-4)
            return;
        inSec = std::clamp(pos, -PRE_ROLL_SECONDS, trackLen);
        outSec = std::min(trackLen, inSec + 4.0 * beatDur);
    }

    if (outSec <= inSec + 0.001)
        return;

    auto& slot = savedLoopAt(index);
    slot.set = true;
    slot.inSec = inSec;
    slot.outSec = outSec;
    const double beatDur = beatDurationAround(inSec);
    slot.lengthBeats = beatDur > 1e-4 ? (outSec - inSec) / beatDur : 4.0;
    if (slot.color.isEmpty())
        slot.color = defaultSavedLoopColor(index);
    if (slot.label.isEmpty())
        slot.label = QStringLiteral("LOOP %1").arg(index + 1);

    persistSavedLoopSlot(index);
    emit savedLoopsChanged();
}

void DjEngine::triggerSavedLoop(int index)
{
    if (!isValidSavedLoopIndex(index) || !m_hasTrack)
        return;

    const auto& slot = savedLoopAt(index);
    if (!slot.set) {
        storeSavedLoop(index);
        return;
    }

    activateLoopRange(slot.inSec, slot.outSec, true);
    ensureTransportRunningForPlayIntent();
}

void DjEngine::clearSavedLoop(int index)
{
    if (!isValidSavedLoopIndex(index))
        return;

    auto& slot = savedLoopAt(index);
    slot.set = false;
    slot.inSec = 0.0;
    slot.outSec = 0.0;
    slot.lengthBeats = 0.0;
    slot.label.clear();
    slot.color = defaultSavedLoopColor(index);

    persistSavedLoopSlot(index);
    emit savedLoopsChanged();
}

bool DjEngine::beatgridLocked() const
{
    return m_trackData && m_trackData->beatgridLockedByUser();
}

void DjEngine::setBeatgridLocked(bool locked)
{
    if (!m_trackData)
        return;

    if (m_trackData->beatgridLockedByUser() == locked)
        return;

    m_trackData->setBeatgridLocked(locked);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
}

void DjEngine::loadMainCueForCurrentTrack()
{
    m_mainCueSec = -(PRE_ROLL_SECONDS + 1.0);

    if (!m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const double storedCue = m_libraryDb->mainCuePointForTrack(m_currentTrackId);
    m_mainCueSec = storedCue >= 0.0 ? storedCue : -(PRE_ROLL_SECONDS + 1.0);
    emit mainCueChanged();
}

void DjEngine::persistMainCuePoint()
{
    if (!m_libraryDb || m_currentTrackId.isEmpty())
        return;
    m_libraryDb->upsertMainCuePoint(m_currentTrackId, m_mainCueSec);
}

void DjEngine::resetMainCueButtonState()
{
    ++m_mainCuePressSerial;
    m_mainCueButtonDown = false;
    m_mainCueHoldPreviewPending = false;
    m_mainCuePreviewActive = false;
}

void DjEngine::startMainCueHoldPreview(quint64 pressSerial)
{
    if (pressSerial != m_mainCuePressSerial
        || !m_mainCueButtonDown
        || !m_mainCueHoldPreviewPending
        || m_mainCuePreviewActive
        || m_playRequested
        || !m_hasTrack) {
        return;
    }

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double cuePos = std::clamp(m_mainCueSec >= -PRE_ROLL_SECONDS ? m_mainCueSec : 0.0,
                                     -PRE_ROLL_SECONDS,
                                     trackLen);

    m_mainCueHoldPreviewPending = false;
    m_mainCuePreviewActive = true;
    transportSource.setPosition(std::max(0.0, cuePos));
    m_scrubHoldPosition = cuePos;
    if (cuePos < 0.0) {
        m_preRollCountdownActive = true;
        m_preRollVisualStartPos = cuePos;
        m_preRollClock.restart();
        m_snapValid = false;
        m_atomicPlayheadPos.store(cuePos, std::memory_order_relaxed);
    } else {
        setSnapAnchor(cuePos, true);
        armVisualSeekSettle();
        transportSource.start();
    }
    emit playingChanged();
    emit progressChanged();
}

void DjEngine::setLastAudioDeviceError(const QString& error)
{
    if (m_lastAudioDeviceError == error)
        return;
    m_lastAudioDeviceError = error;
    emit audioDeviceErrorChanged();
}

void DjEngine::setAudioDeviceFallbackMessage(const QString& message)
{
    if (m_audioDeviceFallbackMessage == message)
        return;
    m_audioDeviceFallbackMessage = message;
    emit audioDeviceFallbackChanged();
}

void DjEngine::cueButtonPress()
{
    if (!m_hasTrack)
        return;

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    if (m_mainCueButtonDown)
        return;

    m_mainCueButtonDown = true;
    m_mainCueHoldPreviewPending = false;
    const quint64 pressSerial = ++m_mainCuePressSerial;

    const bool wasPlaying = m_playRequested;

    if (wasPlaying) {
        // While playing, CUE jumps to the stored cue and continues playback.
        if (m_mainCueSec < -PRE_ROLL_SECONDS) {
            m_mainCueSec = std::clamp(static_cast<double>(getVisualPosition()), -PRE_ROLL_SECONDS, trackLen);
            persistMainCuePoint();
            emit mainCueChanged();
        }

        const double cuePos = std::clamp(m_mainCueSec, -PRE_ROLL_SECONDS, trackLen);
        transportSource.setPosition(std::max(0.0, cuePos));
        m_scrubHoldPosition = cuePos;
        setSnapAnchor(cuePos, true);
        armVisualSeekSettle();
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(cuePos);
        emit progressChanged();
        return;
    }

    // While paused, pressing CUE sets the cue point at current position and previews while held.
    const double cuePos = std::clamp(static_cast<double>(getVisualPosition()), -PRE_ROLL_SECONDS, trackLen);
    m_mainCueSec = cuePos;
    persistMainCuePoint();
    emit mainCueChanged();

    transportSource.setPosition(std::max(0.0, cuePos));
    m_scrubHoldPosition = cuePos;
    setSnapAnchor(cuePos, true);
    armVisualSeekSettle();
    if (m_analyzer && m_analyzer->isThreadRunning())
        m_analyzer->setSeekHint(cuePos);

    // Start cue preview immediately so MIDI/controller cue has the same
    // down-event immediacy as a physical transport button.
    m_mainCueHoldPreviewPending = true;
    startMainCueHoldPreview(pressSerial);
    emit progressChanged();
}

void DjEngine::cueButtonRelease()
{
    if (!m_mainCueButtonDown && !m_mainCuePreviewActive)
        return;

    m_mainCueButtonDown = false;
    m_mainCueHoldPreviewPending = false;
    ++m_mainCuePressSerial;

    if (!m_mainCuePreviewActive)
        return;

    m_mainCuePreviewActive = false;

    // CUE+Play trick (Serato/Rekordbox behavior): if PLAY was pressed while CUE was
    // held, continue playing normally instead of snapping back to the cue point.
    if (m_playRequested) {
        emit playingChanged();
        return;
    }

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double cuePos = std::clamp(m_mainCueSec >= -PRE_ROLL_SECONDS ? m_mainCueSec : 0.0, -PRE_ROLL_SECONDS, trackLen);
    m_preRollCountdownActive = false;
    if (mixerSource)
        mixerSource->armClickFreeTransition();
    if (transportSource.isPlaying())
        transportSource.stop();
    transportSource.setPosition(std::max(0.0, cuePos));
    m_scrubHoldPosition = cuePos;  // sync visual hold so pre-roll position survives timer ticks

    m_snapPosition = cuePos;
    m_snapClock.restart();
    m_snapValid = false;
    m_atomicPlayheadPos.store(cuePos, std::memory_order_relaxed);
    armVisualSeekSettle();

    emit playingChanged();
    emit progressChanged();
}

void DjEngine::applyScratchNeutralRouting()
{
    if (timeStretchSource)
        timeStretchSource->enterScratchBypass();
    if (scratchBridge)
        scratchBridge->setKeylockPassthrough(false);
    if (reverseWrapSource)
        reverseWrapSource->setReverse(false);
}

void DjEngine::restorePostScrubPlaybackState()
{
    const double resumeSec = std::max(0.0, m_scrubHoldPosition);
    transportSource.setPosition(resumeSec);
    m_scrubHoldPosition = resumeSec;
    m_atomicPlayheadPos.store(resumeSec, std::memory_order_relaxed);
    m_snapPosition = resumeSec;

    if (mixerSource)
        mixerSource->armClickFreeTransition();

    if (reverseWrapSource)
        reverseWrapSource->setReverse(m_isReverse);

    if (m_scratch.wasPlaying() && !m_playRequested)
        m_playRequested = true;

    if (!m_playRequested)
        transportSource.stop();

    if (timeStretchSource)
        timeStretchSource->endScratchBypass();

    // Resume at the live deck tempo — never hard-reset to 1.0× first, which
    // causes a brief slow-down when the tempo fader is above/below center.
    updateSpeedAndPitch();

    // Re-apply loop range to the audio source — scratch neutral routing may have
    // changed the reverse state, which gates loop enforcement in applyLoopRangeToAudioSource.
    if (m_loopActive)
        applyLoopRangeToAudioSource();

    if (m_playRequested) {
        if (m_scrubHoldPosition < 0.0) {
            // Scratch glide may have started the transport from position 0; stop it
            // before the pre-roll countdown takes over so it does not race with the
            // countdown logic and cause a snap to 0 via getVisualPosition().
            transportSource.stop();
            m_preRollCountdownActive = true;
            m_preRollVisualStartPos = m_scrubHoldPosition;
            m_preRollClock.restart();
        } else {
            transportSource.start();
        }
    }

    m_scratch.setWasPlaying(false);
    m_snapTempoRatio = getTempoRatio();
    m_snapClock.restart();
    m_snapValid = !m_preRollCountdownActive;
}

// ─── Scrub API ────────────────────────────────────────────────────────────────

void DjEngine::pauseForScrub(double anchorPositionSec)
{
    if (m_scratch.scrubbing())
        return;

    if (mixerSource)
        mixerSource->armClickFreeTransition();

    m_phaseNudge      = 0.0;
    m_jogNudgePercent = 0.0;
    m_resyncBoost = false;

    const bool wasPlayingBeforeGrab = m_playRequested || transportSource.isPlaying();
    m_scratch.setReleaseGlide(false);
    m_scratch.setWasPlaying(wasPlayingBeforeGrab);
    m_scratch.setScrubbing(true);
    m_snapValid = false;
    m_scratchSnapReadPending = wasPlayingBeforeGrab;

    // During pre-roll countdown, m_scrubHoldPosition is negative; don't clobber it
    // with transport position (which is always 0 before beat 1).
    m_preRollCountdownActive = false;

    // Stop transport before anchoring — while playing, audio truth is transport
    // position, not latency-compensated visual position from QML.
    if (transportSource.isPlaying())
        transportSource.stop();

    if (wasPlayingBeforeGrab) {
        m_scrubHoldPosition = transportSource.getCurrentPosition();
    } else if (anchorPositionSec >= 0.0) {
        m_scrubHoldPosition = anchorPositionSec;
    } else if (m_scrubHoldPosition >= 0.0) {
        // keep frozen hold
    } else {
        m_scrubHoldPosition = transportSource.getCurrentPosition();
    }

    const double len = transportSource.getLengthInSeconds();
    const auto loopCtx = scratchLoopCtx();
    m_scrubHoldPosition = m_scratch.armGrab(m_scrubHoldPosition, len, loopCtx);

    if (scratchBridge) {
        scratchBridge->beginScratch(m_scrubHoldPosition, m_loadedTrackSampleRate, std::max(0.0, len));
        scratchBridge->setReverse(m_isReverse);
        scratchBridge->setKeylockPassthrough(false);
        if (wasPlayingBeforeGrab)
            scratchBridge->syncReadToTarget(m_loadedTrackSampleRate);
    }

    emit scrubbingChanged();

    applyScratchNeutralRouting();
    clearLoopRangeOnAudioSource();
}

void DjEngine::scratchBySeconds(double deltaSeconds, bool vinylOneToOnePosition)
{
    juce::ignoreUnused(vinylOneToOnePosition);
    if (deltaSeconds == 0.0)
        return;

    if (!m_scratch.scrubbing() || !scratchBridge)
        return;

    if (m_scratchSnapReadPending) {
        m_scratchSnapReadPending = false;
        scratchBridge->syncReadToTarget(m_loadedTrackSampleRate);
    }

    if (!m_scratch.submitRelative(scratchBridge.get(), deltaSeconds, m_loadedTrackSampleRate))
        return;

    scratchBridge->tickControlThread(0.004);
    m_scrubHoldPosition = m_scratch.lastRawSec();
    updateScrubPlayheadAnchor();
    emit progressChanged();
}

void DjEngine::setScrubPosition(double positionSeconds)
{
    if (!m_scratch.scrubbing() || !scratchBridge)
        return;

    const double len = transportSource.getLengthInSeconds();
    if (len <= 0.0)
        return;

    if (m_scratchSnapReadPending) {
        m_scratchSnapReadPending = false;
        scratchBridge->syncReadToTarget(m_loadedTrackSampleRate);
    }

    if (!m_scratch.submitAbsolute(scratchBridge.get(),
                                  positionSeconds,
                                  m_loadedTrackSampleRate,
                                  len,
                                  SCRATCH_PRE_ROLL_SECONDS,
                                  scratchLoopCtx())) {
        return;
    }

    scratchBridge->tickControlThread(0.004);
    m_scrubHoldPosition = m_scratch.lastRawSec();
    updateScrubPlayheadAnchor();
    emit progressChanged();
}

double DjEngine::platterAngleDegrees() const
{
    if (!scratchBridge)
        return 0.0;
    return scratchBridge->platter().displayAngleDegrees();
}

void DjEngine::resumeAfterScrub()
{
    if (!m_scratch.scrubbing() || !scratchBridge)
        return;

    const double resumePos = std::max(0.0, m_scrubHoldPosition);
    m_scratch.setScrubbing(false);
    m_scratch.setReleaseGlide(false);
    scratchBridge->exitScratchMode(resumePos, m_loadedTrackSampleRate);
    restorePostScrubPlaybackState();

    emit scrubbingChanged();
}

void DjEngine::applyScratchReleaseJog(double deltaSeconds)
{
    if (!m_scratch.releaseGlide() || deltaSeconds == 0.0 || !scratchBridge)
        return;

    scratchBridge->addTargetDeltaSeconds(deltaSeconds, m_loadedTrackSampleRate);
    m_scrubHoldPosition = scratchBridge->targetPositionSeconds(m_loadedTrackSampleRate);
    updateScrubPlayheadAnchor();
    m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_relaxed);
    m_snapPosition = m_scrubHoldPosition;
    emit progressChanged();
}

void DjEngine::finishScrubWithoutInertia()
{
    if (!m_scratch.scrubbing() && !m_scratch.releaseGlide())
        return;

    terminateScratchSession(m_scrubHoldPosition);

    restorePostScrubPlaybackState();
    emit scrubbingChanged();
    emit playingChanged();
    emit progressChanged();
}

void DjEngine::applyJogNudge(double signedTicks)
{
    if (m_scratch.scrubbing() || m_scratch.releaseGlide())
        return;

    // FLX10 rim ticks are relative jog deltas, not coarse tempo-percent steps.
    // Keep pitch bend gentle; fast rim turns still reach the clamp naturally.
    constexpr double kPercentPerTick = 0.75;
    constexpr double kMaxNudgePercent = 6.0;
    m_jogNudgePercent = std::clamp(signedTicks * kPercentPerTick, -kMaxNudgePercent, kMaxNudgePercent);
    m_lastJogNudgeClock.restart();
    updateSpeedAndPitch();
}

void DjEngine::setDownbeatAtPosition(double anchorSec)
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed())
        return;

    const double trackLengthSec = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLengthSec <= 0.0)
        return;

    m_trackData->shiftBeatgridToDownbeat(anchorSec, trackLengthSec);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
}

void DjEngine::setDownbeatAtCurrentPosition()
{
    setDownbeatAtPosition(static_cast<double>(getVisualPosition()));
}

void DjEngine::nudgeBeatgridMs(double milliseconds)
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed())
        return;

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0 || std::abs(milliseconds) < 1e-6)
        return;

    m_trackData->nudgeBeatgrid(milliseconds / 1000.0, trackLen);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
}

void DjEngine::nudgeBeatgridBeats(double beats)
{
    if (!m_trackData || std::abs(beats) < 1e-6)
        return;

    const double pos = static_cast<double>(getVisualPosition());
    const double beatDur = beatDurationAround(pos);
    if (beatDur <= 1e-4)
        return;

    nudgeBeatgridMs(beats * beatDur * 1000.0);
}

// Helper: find the positionSec of the downbeat (isDownbeat == true) nearest
// to currentSec in the existing beat grid.  Falls back to currentSec itself
// if the grid is empty or has no downbeats.
static double nearestDownbeatAnchor(const std::vector<TrackData::BeatMarker>& grid,
                                    double currentSec)
{
    double best     = currentSec;
    double bestDist = std::numeric_limits<double>::max();
    for (const auto& m : grid) {
        if (!m.isDownbeat) continue;
        double d = std::abs(m.positionSec - currentSec);
        if (d < bestDist) { bestDist = d; best = m.positionSec; }
    }
    return best;
}

void DjEngine::doubleBpm()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed()) return;
    double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLen <= 0.0) return;

    double currentSec = static_cast<double>(getVisualPosition());
    double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    double newBpm = m_trackData->getBpm() * 2.0;
    m_trackData->setBpm(newBpm);
    m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();   // update BPM display in UI
}

void DjEngine::halveBpm()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed()) return;
    double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLen <= 0.0) return;

    double currentSec = static_cast<double>(getVisualPosition());
    double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    double newBpm = m_trackData->getBpm() / 2.0;
    m_trackData->setBpm(newBpm);
    m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);
    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();   // update BPM display in UI
}

// ──────────────────────────────────────────────────────────────────────────────

void DjEngine::setOutputFirstChannel(int firstChannel)
{
    const int clamped = std::max(1, firstChannel);
    m_masterFirstChannelAtomic.store(clamped, std::memory_order_relaxed);
}

void DjEngine::setMasterVolume(float v) {
    DjMasterBus::setMasterVolume(v);
}

void DjEngine::setAntiClip(bool enabled) {
    DjMasterBus::setAntiClipEnabled(enabled);
}

// ──────────────────────────────────────────────────────────────────────────────

// Returns the beat interval [prevSec, prevSec+lengthSec) that contains positionSec.
// Requires m_trackData non-null and BPM > 0.
DjEngine::BeatInterval DjEngine::beatIntervalAt(double positionSec) const
{
    const double bpm    = m_trackData->getBpm();
    const double nomLen = 60.0 / bpm;
    const auto&  grid   = m_trackData->getBeatGrid();

    if (grid.size() >= 2) {
        const auto it   = std::upper_bound(grid.begin(), grid.end(), positionSec,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        const auto prev = (it != grid.begin()) ? std::prev(it) : grid.begin();
        const double prevSec = prev->positionSec;
        double beatLen = nomLen;
        if (std::next(prev) != grid.end()) {
            const double candidate = std::next(prev)->positionSec - prevSec;
            if (candidate > 0.01)
                beatLen = candidate;
        }
        return {prevSec, beatLen};
    }

    const double sr        = m_trackData->getSampleRate();
    const double firstBeat = sr > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sr : 0.0;
    const double idx = std::floor((positionSec - firstBeat) / nomLen);
    return {firstBeat + idx * nomLen, nomLen};
}

double DjEngine::getBeatPhase() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0) return 0.0;

    const double pos = getPosition();
    if (m_trackData->getBeatGrid().size() >= 2) {
        const auto [prevSec, beatLen] = beatIntervalAt(pos);
        return std::clamp((pos - prevSec) / beatLen, 0.0, 0.9999);
    }

    // No usable beat grid — phase from BPM alone.
    return std::fmod(pos / (60.0 / bpm), 1.0);
}

double DjEngine::getBeatPosition() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0) return 0.0;

    const double pos  = getPosition();
    const auto&  grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        const auto it = std::upper_bound(grid.begin(), grid.end(), pos,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        const auto prev      = (it != grid.begin()) ? std::prev(it) : grid.begin();
        const int  beatIndex = static_cast<int>(std::distance(grid.begin(), prev));
        // Use the same interval length as beatIntervalAt (> 0.01 threshold),
        // except fall back to max(0.001, …) at the very first marker so
        // pre-roll positions before the first beat return sensible fractions.
        double beatLen = 60.0 / bpm;
        if (std::next(prev) != grid.end()) {
            const double candidate = std::next(prev)->positionSec - prev->positionSec;
            if (candidate > 0.001)
                beatLen = candidate;
        }
        return static_cast<double>(beatIndex) + ((pos - prev->positionSec) / beatLen);
    }

    const double sr        = m_trackData->getSampleRate();
    const double firstBeat = sr > 0.0
        ? static_cast<double>(m_trackData->getFirstBeatSample()) / sr : 0.0;
    return (pos - firstBeat) / (60.0 / bpm);
}

void DjEngine::updateFxBeatSyncPosition()
{
    if (!mixerSource || !m_trackData)
        return;

    const double pos = getPosition();
    const double beatDur = beatDurationAround(pos);
    if (beatDur <= 0.001)
        return;

    mixerSource->setBeatSyncPosition(getBeatPosition(), beatDur);
}

void DjEngine::updatePhaseCorrection()
{
    // Only sync followers with a valid, playing track should phase-correct.
    if (!m_syncEnabled || m_isSyncMaster || !isPlaying() || !m_trackData) {
        if (m_phaseNudge != 0.0) {
            m_phaseNudge = 0.0;
            updateSpeedAndPitch();
        }
        return;
    }

    DjEngine* master = nullptr;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        master = s_syncMasterDeck;
    }

    if (!master || !master->isPlaying() || !master->m_trackData
            || master->m_trackData->getBpm() <= 0.0) {
        if (m_phaseNudge != 0.0) {
            m_phaseNudge = 0.0;
            updateSpeedAndPitch();
        }
        return;
    }

    const double masterPhase = master->getBeatPhase();
    const double myPhase     = getBeatPhase();

    // Signed phase error wrapped to [-0.5, +0.5] of a beat.
    double diff = masterPhase - myPhase;
    if (diff >  0.5) diff -= 1.0;
    if (diff < -0.5) diff += 1.0;

    constexpr double kTolerance = 0.02;
    // Normal: ±4% nudge. Boosted (reSync()): ±15% — after the 85% seek the remaining
    // 15% of error converges in ~2 s (τ = 50 * beatLen / kMaxNudge ≈ 1.6 s at 128 BPM).
    const double kMaxNudge = m_resyncBoost ? 15.0 : 4.0;
    const double kGain     = kMaxNudge / 0.5;

    const double newNudge = (std::abs(diff) < kTolerance)
        ? 0.0
        : std::clamp(diff * kGain, -kMaxNudge, kMaxNudge);

    // Clear boost once we're within tolerance.
    if (m_resyncBoost && std::abs(diff) < kTolerance)
        m_resyncBoost = false;

    if (newNudge != m_phaseNudge) {
        m_phaseNudge = newNudge;
        updateSpeedAndPitch();
    }
}

void DjEngine::snapPhaseToMaster(DjEngine* master)
{
    if (!master || !master->m_trackData || !m_trackData)
        return;

    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0)
        return;

    const double masterPhase = master->getBeatPhase();
    const double myPhase     = getBeatPhase();

    // Signed phase error wrapped to [-0.5, +0.5].
    double diff = masterPhase - myPhase;
    if (diff >  0.5) diff -= 1.0;
    if (diff < -0.5) diff += 1.0;

    // Nothing to do if already aligned.
    if (std::abs(diff) < 0.005)
        return;

    const double beatLen    = beatIntervalAt(getPosition()).lengthSec;
    const double seekOffset = diff * beatLen;
    const double len        = transportSource.getLengthInSeconds();
    const double newPos     = std::clamp(getPosition() + seekOffset, -PRE_ROLL_SECONDS,
                                         len > 0.0 ? len : PRE_ROLL_SECONDS);
    if (newPos < 0.0) {
        if (m_preRollCountdownActive) {
            m_preRollVisualStartPos = newPos;
            m_preRollClock.restart();
        }
        m_scrubHoldPosition = newPos;
        m_atomicPlayheadPos.store(newPos, std::memory_order_relaxed);
    } else {
        transportSource.setPosition(newPos);
        armSnapFromTransportPosition();
    }
    m_phaseNudge = 0.0;
}

void DjEngine::updateSpeedAndPitch()
{
    double speedMultiplier = 1.0 + ((m_tempoPercent + m_phaseNudge + m_jogNudgePercent) / 100.0);
    speedMultiplier = std::clamp(speedMultiplier, 0.01, 8.0);

    if (scratchBridge) {
        scratchBridge->setDeckTempoRatio(speedMultiplier);
        scratchBridge->setKeylockPassthrough(m_keylock);
    }

    if (timeStretchSource) {
        timeStretchSource->setTempoRatio(speedMultiplier);
        timeStretchSource->setPitchLockEnabled(m_keylock);
    }
}

void DjEngine::setKeylock(bool on)
{
    if (m_keylock == on) return;
    m_keylock = on;
    updateSpeedAndPitch();
    emit keylockChanged();
}

void DjEngine::applyTempoPercent(double percent)
{
    percent = std::clamp(percent, -100.0, 100.0);
    if (m_tempoPercent == percent) return;
    m_tempoPercent = percent;

    if (scratchBridge && (m_scratch.scrubbing() || m_scratch.releaseGlide()))
        scratchBridge->setDeckTempoRatio(getTempoRatio());

    updateSpeedAndPitch();
    emit tempoChanged();

    if (m_syncEnabled) {
        bool amMaster = false;
        {
            std::lock_guard<std::mutex> g(s_syncMutex);
            updateSyncMasterLocked();
            amMaster = m_isSyncMaster;
        }
        if (amMaster)
            propagateMasterTempoLocked(this);
    }
}

void DjEngine::setTempoPercent(double percent)
{
    // Sync followers ignore tempo-fader moves — only the master drives tempo.
    if (m_syncEnabled && !m_isSyncMaster)
        return;

    applyTempoPercent(percent);
}

void DjEngine::setTempoRangePercent(double percent)
{
    const double clamped = std::clamp(percent, 6.0, 100.0);
    if (std::abs(m_tempoRangePercent - clamped) < 0.001)
        return;

    m_tempoRangePercent = clamped;
    applyTempoPercent(std::clamp(m_tempoPercent, -m_tempoRangePercent, m_tempoRangePercent));
    emit tempoRangeChanged();
}

void DjEngine::setManualBpm(double bpm)
{
    if (!m_trackData)
        return;

    const double clamped = std::clamp(bpm, 20.0, 300.0);
    if (clamped <= 0.0)
        return;

    const double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    const double currentSec = static_cast<double>(getVisualPosition());
    const double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    m_trackData->setBpm(clamped);
    if (trackLen > 0.0)
        m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);

    persistCurrentAnalysisToLibrary();
    emit beatgridLockedChanged();
    emit tempoChanged();

    if (m_syncEnabled) {
        bool amMaster = false;
        {
            std::lock_guard<std::mutex> g(s_syncMutex);
            updateSyncMasterLocked();
            amMaster = m_isSyncMaster;
        }
        if (amMaster)
            propagateMasterTempoLocked(this);
    }
}

void DjEngine::setSyncEnabled(bool enabled)
{
    if (m_syncEnabled == enabled)
        return;

    m_syncEnabled = enabled;

    if (!enabled) {
        m_resyncBoost = false;
        if (m_phaseNudge != 0.0) {
            m_phaseNudge = 0.0;
            updateSpeedAndPitch();
        }
    }

    emit syncChanged();

    bool amMaster = false;
    DjEngine* masterDeck = nullptr;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        updateSyncMasterLocked();
        amMaster = m_isSyncMaster;
        masterDeck = s_syncMasterDeck;
    }

    if (m_syncEnabled) {
        if (amMaster) {
            propagateMasterTempoLocked(this);
        } else if (masterDeck) {
            const double masterBpm = masterDeck->getCurrentBpm();
            if (m_trackData) {
                const double baseBpm = m_trackData->getBpm();
                if (masterBpm > 0.0 && baseBpm > 0.0) {
                    const double pct = ((masterBpm / baseBpm) - 1.0) * 100.0;
                    applyTempoPercent(pct);
                }
            }
            snapPhaseToMaster(masterDeck);
            return;
        }
    }
}

void DjEngine::reSync()
{
    if (!m_syncEnabled || m_isSyncMaster || !m_trackData)
        return;

    DjEngine* master = nullptr;
    { std::lock_guard<std::mutex> g(s_syncMutex); master = s_syncMasterDeck; }
    if (!master || !master->m_trackData)
        return;

    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0)
        return;

    const double masterPhase = master->getBeatPhase();
    const double myPhase     = getBeatPhase();
    double diff = masterPhase - myPhase;
    if (diff >  0.5) diff -= 1.0;
    if (diff < -0.5) diff += 1.0;

    if (std::abs(diff) < 0.005)
        return;

    // Seek 85% of the phase error immediately (barely audible for errors up to half a beat),
    // then let the boosted P-controller handle the remaining 15% in ~2 seconds.
    const double beatLen    = beatIntervalAt(getPosition()).lengthSec;
    const double seekOffset = diff * beatLen * 0.85;
    const double len        = transportSource.getLengthInSeconds();
    const double newPos     = std::clamp(getPosition() + seekOffset, -PRE_ROLL_SECONDS,
                                         len > 0.0 ? len : PRE_ROLL_SECONDS);
    if (newPos < 0.0) {
        if (m_preRollCountdownActive) {
            m_preRollVisualStartPos = newPos;
            m_preRollClock.restart();
        }
        m_scrubHoldPosition = newPos;
        m_atomicPlayheadPos.store(newPos, std::memory_order_relaxed);
    } else {
        transportSource.setPosition(newPos);
        armSnapFromTransportPosition();
    }
    m_phaseNudge  = 0.0;
    m_resyncBoost = true;
    updatePhaseCorrection();
}

double DjEngine::getPreRollSeconds() const
{
    return PRE_ROLL_SECONDS;
}

void DjEngine::updateGain()
{
    if (mixerSource) {
        mixerSource->setFader(static_cast<float>(m_volume));
        mixerSource->setTrim(static_cast<float>(m_trim));
    }
}

void DjEngine::applyMixerEq()
{
    if (!mixerSource)
        return;
    mixerSource->setEq(static_cast<float>(m_eqLow),
                       static_cast<float>(m_eqMid),
                       static_cast<float>(m_eqHigh));
}

void DjEngine::applyMixerFilter()
{
    if (!mixerSource)
        return;
    mixerSource->setFilterVal(static_cast<float>(m_filter));
}

void DjEngine::setVolume(double value)
{
    const double clamped = std::clamp(value, kVolumeMin, kVolumeMax);
    if (nearlyEqual(m_volume, clamped))
        return;

    m_volume = clamped;
    if (mixerSource)
        mixerSource->setFader(static_cast<float>(m_volume));
    emit volumeChanged();
}

void DjEngine::setTrim(double value)
{
    const double clamped = std::clamp(value, kTrimMin, kTrimMax);
    if (nearlyEqual(m_trim, clamped))
        return;

    m_trim = clamped;
    if (mixerSource)
        mixerSource->setTrim(static_cast<float>(m_trim));
    emit trimChanged();
}

void DjEngine::setEqHigh(double value)
{
    const double clamped = std::clamp(value, kEqMin, kEqMax);
    if (nearlyEqual(m_eqHigh, clamped))
        return;

    m_eqHigh = clamped;
    applyMixerEq();
    emit eqHighChanged();
}

void DjEngine::setEqMid(double value)
{
    const double clamped = std::clamp(value, kEqMin, kEqMax);
    if (nearlyEqual(m_eqMid, clamped))
        return;

    m_eqMid = clamped;
    applyMixerEq();
    emit eqMidChanged();
}

void DjEngine::setEqLow(double value)
{
    const double clamped = std::clamp(value, kEqMin, kEqMax);
    if (nearlyEqual(m_eqLow, clamped))
        return;

    m_eqLow = clamped;
    applyMixerEq();
    emit eqLowChanged();
}

void DjEngine::setFilter(double value)
{
    const double clamped = std::clamp(value, kFilterMin, kFilterMax);
    if (nearlyEqual(m_filter, clamped))
        return;

    m_filter = clamped;
    applyMixerFilter();
    emit filterChanged();
}

void DjEngine::setCueEnabled(bool value)
{
    const bool prev = m_cueEnabled.exchange(value, std::memory_order_relaxed);
    if (prev != value)
        emit cueEnabledChanged();
}

bool DjEngine::masterCueEnabled() const
{
    return DjMasterBus::masterCueEnabled();
}

double DjEngine::headphoneMix() const
{
    return static_cast<double>(DjMasterBus::headphoneMix());
}

void DjEngine::setMasterCueEnabled(bool value)
{
    const bool prev = DjMasterBus::masterCueEnabled();
    DjMasterBus::setMasterCueEnabled(value);
    if (prev != value)
        emit masterCueEnabledChanged();
}

void DjEngine::setHeadphoneMix(double value)
{
    const float clamped = static_cast<float>(std::clamp(value, 0.0, 1.0));
    const float prev = static_cast<float>(DjMasterBus::headphoneMix());
    DjMasterBus::setHeadphoneMix(clamped);
    if (std::abs(prev - clamped) > 0.0001f)
        emit headphoneMixChanged();
}

void DjEngine::setQuantizeEnabled(bool enabled)
{
    if (m_quantizeEnabled == enabled)
        return;
    m_quantizeEnabled = enabled;
    emit quantizeEnabledChanged();
}

double DjEngine::quantizedBeatAt(double sec) const
{
    if (!m_trackData)
        return sec;

    const auto& grid = m_trackData->getBeatGrid();
    if (!grid.empty()) {
        // Binary search: first marker strictly after sec.
        const auto it = std::upper_bound(grid.begin(), grid.end(), sec,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        if (it == grid.begin())
            return grid.front().positionSec;
        if (it == grid.end())
            return grid.back().positionSec;
        const auto prev  = std::prev(it);
        const double dPrev = sec - prev->positionSec;
        const double dNext = it->positionSec - sec;
        return (dPrev <= dNext) ? prev->positionSec : it->positionSec;
    }

    const double bpm = m_trackData->getBpm();
    const double sr  = m_trackData->getSampleRate();
    if (bpm <= 0.0 || sr <= 0.0)
        return sec;

    const double beatDur   = 60.0 / bpm;
    const double firstBeat = static_cast<double>(m_trackData->getFirstBeatSample()) / sr;
    const double beatIndex = std::round((sec - firstBeat) / beatDur);
    return firstBeat + beatIndex * beatDur;
}

double DjEngine::beatDurationAround(double sec) const
{
    if (!m_trackData)
        return 0.5;

    const auto& grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        // Binary search for the containing interval [prev, next).
        const auto it   = std::upper_bound(grid.begin(), grid.end(), sec,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        const auto prev = (it != grid.begin()) ? std::prev(it) : grid.begin();
        if (std::next(prev) != grid.end()) {
            const double d = std::next(prev)->positionSec - prev->positionSec;
            if (d > 1e-3) return d;
        }
        if (prev != grid.begin()) {
            const double d = prev->positionSec - std::prev(prev)->positionSec;
            if (d > 1e-3) return d;
        }
    }

    const double bpm = m_trackData->getBpm();
    return bpm > 0.0 ? (60.0 / bpm) : 0.5;
}

void DjEngine::startLoopAt(double startSec, double lengthBeats)
{
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    double start = std::clamp(startSec, -PRE_ROLL_SECONDS, trackLen);
    if (m_quantizeEnabled)
        start = quantizedBeatAt(start);

    const double beatDur = beatDurationAround(start);
    if (beatDur <= 1e-4)
        return;

    constexpr double kMinLoopBeats = 1.0 / 64.0;
    constexpr double kMaxLoopBeats = 4096.0;
    double beats = std::clamp(lengthBeats, kMinLoopBeats, kMaxLoopBeats);
    double end = start + beats * beatDur;
    if (end > trackLen)
        end = trackLen;
    if (end <= start + 0.001)
        return;

    m_loopInSec = start;
    m_loopOutSec = end;
    m_loopLengthBeats = (end - start) / beatDur;
    m_loopActive = true;
    m_loopInSet = true;
    applyLoopRangeToAudioSource();
    emit loopChanged();
}

void DjEngine::setLoopIn()
{
    double pos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        pos = quantizedBeatAt(pos);

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    m_loopInSec = std::clamp(pos, -PRE_ROLL_SECONDS, trackLen);
    m_loopInSet = true;

    if (m_loopActive) {
        if (m_loopOutSec <= m_loopInSec)
            m_loopOutSec = std::min(trackLen, m_loopInSec + beatDurationAround(m_loopInSec));
    } else {
        m_loopOutSec = m_loopInSec;
        m_loopLengthBeats = 0.0;
    }
    if (m_loopActive)
        applyLoopRangeToAudioSource();
    emit loopChanged();
}

void DjEngine::setLoopOut()
{
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    double outPos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        outPos = quantizedBeatAt(outPos);
    outPos = std::clamp(outPos, -PRE_ROLL_SECONDS, trackLen);

    // If no IN point is set yet, create a sensible default one-beat loop ending at OUT.
    if (!m_loopInSet) {
        const double beatDurAtOut = beatDurationAround(std::max(0.0, outPos));
        if (beatDurAtOut <= 1e-4)
            return;
        m_loopInSec = std::clamp(outPos - beatDurAtOut, -PRE_ROLL_SECONDS, outPos);
        m_loopInSet = true;
    }

    const double minLenSec = 0.001;
    if (outPos <= m_loopInSec + minLenSec) {
        outPos = std::min(trackLen, m_loopInSec + beatDurationAround(m_loopInSec));
    }
    if (outPos <= m_loopInSec + minLenSec)
        return;

    m_loopOutSec = outPos;
    m_loopActive = true;

    const double beatDurAtIn = beatDurationAround(m_loopInSec);
    if (beatDurAtIn > 1e-4) {
        constexpr double kMinLoopBeats = 1.0 / 64.0;
        constexpr double kMaxLoopBeats = 4096.0;
        const double beats = (m_loopOutSec - m_loopInSec) / beatDurAtIn;
        m_loopLengthBeats = std::clamp(beats, kMinLoopBeats, kMaxLoopBeats);
    }

    applyLoopRangeToAudioSource();
    emit loopChanged();
}

void DjEngine::toggleLoop4Beats()
{
    if (m_loopActive) {
        deactivateLoop();
        return;
    }
    setLoop4Beats();
}

void DjEngine::setLoop4Beats()
{
    startLoopAt(static_cast<double>(getVisualPosition()), 4.0);
}

void DjEngine::toggleLoopThreeQuarter()
{
    // 3/4 loop = three quarters of ONE beat.
    if (m_loopActive && std::abs(m_loopLengthBeats - 0.75) < 0.06) {
        deactivateLoop();
        return;
    }
    startLoopAt(static_cast<double>(getVisualPosition()), 0.75);
}

void DjEngine::halveLoopLength()
{
    if (!m_loopActive) {
        startLoopAt(static_cast<double>(getVisualPosition()), 2.0);
        return;
    }
    startLoopAt(m_loopInSec, m_loopLengthBeats / 2.0);
}

void DjEngine::doubleLoopLength()
{
    if (!m_loopActive) {
        startLoopAt(static_cast<double>(getVisualPosition()), 8.0);
        return;
    }
    startLoopAt(m_loopInSec, m_loopLengthBeats * 2.0);
}

void DjEngine::clearLoop()
{
    if (!m_loopActive && !m_loopInSet && m_loopLengthBeats == 0.0)
        return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_loopActive = false;
    m_loopInSet = false;
    m_loopLengthBeats = 0.0;
    m_loopInSec = 0.0;
    m_loopOutSec = 0.0;
    clearLoopRangeOnAudioSource();
    if (wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit loopChanged();
}

void DjEngine::deactivateLoop()
{
    if (!m_loopActive)
        return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_loopActive = false;
    clearLoopRangeOnAudioSource();
    if (wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit loopChanged();
}

void DjEngine::reactivateLoop()
{
    if (m_loopActive || m_loopInSec >= m_loopOutSec)
        return;
    m_loopActive = true;
    applyLoopRangeToAudioSource();
    emit loopChanged();
}

void DjEngine::beatJump(double beats)
{
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double current = getVisualPosition();
    const double beatDur = beatDurationAround(std::max(0.0, current));
    if (beatDur <= 1e-4)
        return;

    const double next = std::clamp(current + beats * beatDur, -PRE_ROLL_SECONDS, trackLen);
    setPosition(static_cast<float>(next / trackLen));
}

void DjEngine::applyLoopRangeToAudioSource()
{
    if (!reverseWrapSource || !m_loopActive || m_loopOutSec <= m_loopInSec)
        return;

    if (m_isReverse) {
        clearLoopRangeOnAudioSource();
        return;
    }

    // Loops involving pre-roll are enforced in software by onTimer() because the
    // audio source has no concept of negative sample positions (silence doesn't
    // exist in the buffer).  Clear any audio-source loop for these cases.
    if (m_loopInSec < 0.0 || m_loopOutSec <= 0.0) {
        clearLoopRangeOnAudioSource();
        return;
    }

    auto* reverseSource = reverseWrapSource.get();
    if (!reverseSource)
        return;

    const double sr = (m_loadedTrackSampleRate > 1.0)
        ? m_loadedTrackSampleRate
        : (m_trackData ? m_trackData->getSampleRate() : 44100.0);

    const juce::int64 loopInSample  = static_cast<juce::int64>(std::llround(m_loopInSec  * sr));
    const juce::int64 loopOutSample = static_cast<juce::int64>(std::llround(m_loopOutSec * sr));

    reverseSource->setLoopRangeSamples(loopInSample, loopOutSample, sr);
}

void DjEngine::clearLoopRangeOnAudioSource()
{
    if (!reverseWrapSource)
        return;
    auto* reverseSource = reverseWrapSource.get();
    if (reverseSource)
        reverseSource->clearLoopRangeSamples();
}

void DjEngine::setFxEffectType(EffectType type)
{
    if (mixerSource) mixerSource->setFxEffectType(type);
}

void DjEngine::setFxWetDry(float amount)
{
    if (mixerSource) mixerSource->setFxAmount(amount);
}

void DjEngine::setFxExternalDelayTime(float seconds)
{
    if (mixerSource) mixerSource->setFxExternalDelayTime(seconds);
}

void DjEngine::setFxPrimaryParam(float v)
{
    if (mixerSource) mixerSource->setFxPrimaryParam(v);
}

void DjEngine::setFxSlotEffectType(int slot, EffectType type)
{
    if (mixerSource) mixerSource->setFxSlotEffectType(slot, type);
}

void DjEngine::setFxSlotWetDry(int slot, float amount)
{
    if (mixerSource) mixerSource->setFxSlotAmount(slot, amount);
}

void DjEngine::setFxSlotExternalDelayTime(int slot, float seconds)
{
    if (mixerSource) mixerSource->setFxSlotExternalDelayTime(slot, seconds);
}

void DjEngine::setFxSlotPrimaryParam(int slot, float v)
{
    if (mixerSource) mixerSource->setFxSlotPrimaryParam(slot, v);
}

void DjEngine::setPadFx(const QString& effectName, float wet)
{
    static const QHash<QString, EffectType> kMap = {
        {"Echo",       EffectType::Echo},
        {"Reverb",     EffectType::Reverb},
        {"Roll",       EffectType::Roll},
        {"SlipRoll",   EffectType::SlipRoll},
        {"Flanger",    EffectType::Flanger},
        {"Phaser",     EffectType::Phaser},
        {"Bitcrusher", EffectType::Bitcrusher},
        {"Trans",      EffectType::Trans},
        {"Stretch",    EffectType::Stretch},
        {"Filter",     EffectType::SoundColorFilter},
        {"RollOut",    EffectType::RollOut},
    };
    const EffectType type = kMap.value(effectName, EffectType::None);
    if (mixerSource) {
        mixerSource->setPadFxEffectType(type);
        mixerSource->setPadFxAmount(wet);
    }
}

void DjEngine::clearPadFx()
{
    if (mixerSource)
        mixerSource->clearPadFx();
}

void DjEngine::startVinylBrake()
{
    if (m_vinylBrakeActive) return;
    m_vinylBrakeActive = true;
    if (mixerSource) mixerSource->setVinylBrakeActive(true);
    emit vinylBrakeChanged();
}

void DjEngine::stopVinylBrake()
{
    if (!m_vinylBrakeActive) return;
    m_vinylBrakeActive = false;
    if (mixerSource) mixerSource->setVinylBrakeActive(false);
    emit vinylBrakeChanged();
}

void DjEngine::startEchoOut()
{
    if (m_echoOutActive) return;
    m_echoOutActive = true;
    if (mixerSource) mixerSource->setEchoOutActive(true);
    emit echoOutChanged();
}

void DjEngine::stopEchoOut()
{
    if (!m_echoOutActive) return;
    m_echoOutActive = false;
    if (mixerSource) mixerSource->setEchoOutActive(false);
    emit echoOutChanged();
}

void DjEngine::startBackspin()
{
    if (m_backspinActive) return;
    m_backspinActive = true;
    if (mixerSource) mixerSource->setBackspinActive(true);
    emit backspinChanged();
}

void DjEngine::stopBackspin()
{
    if (!m_backspinActive) return;
    m_backspinActive = false;
    if (mixerSource) mixerSource->setBackspinActive(false);
    emit backspinChanged();
}

void DjEngine::startRollOut()
{
    if (m_rollOutActive) return;
    m_rollOutActive = true;
    if (mixerSource) mixerSource->setRollOutActive(true);
    emit rollOutChanged();
}

void DjEngine::stopRollOut()
{
    if (!m_rollOutActive) return;
    m_rollOutActive = false;
    if (mixerSource) mixerSource->setRollOutActive(false);
    emit rollOutChanged();
}

void DjEngine::setFxSCKnob(float knob)
{
    if (mixerSource) mixerSource->setFxSCKnob(knob);
}

void DjEngine::setFxSCParam(float param)
{
    if (mixerSource) mixerSource->setFxSCParam(param);
}

void DjEngine::setReverse(bool on)
{
    if (m_isReverse == on) return;
    const bool wasSlipDiverted = isSlipDiverted();
    m_isReverse = on;
    if (reverseWrapSource) {
        reverseWrapSource->setReverse(on);
        if (m_loopActive)
            applyLoopRangeToAudioSource();
    }
    updateSpeedAndPitch();
    if (!on && wasSlipDiverted && !isSlipDiverted())
        returnToSlipPosition();
    emit reverseChanged();
}

void DjEngine::setSlip(bool on)
{
    if (m_slipActive == on) return;
    m_slipActive = on;
    if (on)
        m_slipPosition = transportSource.getCurrentPosition();
    emit slipChanged();
}

void DjEngine::returnToSlipPosition()
{
    const double dur = std::max(0.001, static_cast<double>(getDuration()));
    const double pos = std::clamp(m_slipPosition, 0.0, dur);
    transportSource.setPosition(pos);
    m_snapPosition = pos;
    m_snapClock.restart();
    m_snapValid = true;
    m_atomicPlayheadPos.store(pos, std::memory_order_relaxed);
}
