#include "DjEngine.h"
#include "fx/BrickwallLimiter.h"
#include "library/CoverArtExtractor.h"
#include "library/CoverArtProvider.h"
#include "fx/FxProcessor.h"
#include "library/LibraryDatabase.h"
#include "library/TrackIdGenerator.h"
#include "WaveformCache.h"
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
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <rubberband/RubberBandStretcher.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <thread>
#include <future>
#include <chrono>
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

#if JUCE_JACK && (JUCE_LINUX || JUCE_BSD)
bool probeJackServer(QString& message)
{
    jack_status_t status = JackFailure;
    jack_client_t* client = jack_client_open("RamsbrockDJProbe", JackNoStartServer, &status);
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

    int bestSize = availableSizes[0];
    int bestDistance = std::abs(bestSize - requestedSize);

    for (const int candidate : availableSizes) {
        const int distance = std::abs(candidate - requestedSize);
        if (distance < bestDistance || (distance == bestDistance && candidate < bestSize)) {
            bestSize = candidate;
            bestDistance = distance;
        }
    }

    return bestSize;
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

// Set to true if any initialiseWithTimeout call was abandoned due to timeout.
// Subsequent init attempts are skipped to avoid multiple multi-second stalls.
static std::atomic<bool> s_audioInitTimedOut{false};

// Runs initialiseWithDefaultDevices on a background thread with a timeout so that
// a frozen macOS CoreAudio daemon (coreaudiod) cannot hang the entire application.
// If the deadline is exceeded the thread is detached; the app starts without audio.
static juce::String initialiseWithTimeout(juce::AudioDeviceManager& manager,
                                          int numInputChannels,
                                          int numOutputChannels,
                                          std::chrono::seconds timeout = std::chrono::seconds(6))
{
    if (s_audioInitTimedOut.load())
        return {};  // skip — a previous attempt already timed out

    std::promise<juce::String> prom;
    auto fut = prom.get_future();

    std::thread t([&manager, numInputChannels, numOutputChannels, p = std::move(prom)]() mutable {
        p.set_value(manager.initialiseWithDefaultDevices(numInputChannels, numOutputChannels));
    });

    if (fut.wait_for(timeout) == std::future_status::timeout) {
        s_audioInitTimedOut = true;
        qCritical() << "[DjEngine] Audio device initialization timed out after"
                    << timeout.count() << "s — starting without audio."
                    << "To fix: restart the app, or run: sudo killall coreaudiod";
        t.detach();
        return {};
    }

    t.join();
    return fut.get();
}

void ensureSharedAudioDeviceManagerReady(juce::AudioDeviceManager& manager)
{
    if (manager.getCurrentAudioDevice() != nullptr)
        return;

    const juce::String err = initialiseWithTimeout(manager, 0, 2);
    if (err.isNotEmpty())
        qWarning() << "JUCE AudioDeviceManager err:" << QString::fromStdString(err.toStdString());
}

struct OutputLatencySnapshot {
    int outputRawSamples = 0;
    int callbackBufferSamples = 0;
    int effectiveOutputSamples = 0;
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
    const juce::String initErr = initialiseWithTimeout(probe, 0, 2, std::chrono::seconds(4));
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
    int effectiveSamples = outputRawSamples > 0 ? outputRawSamples : callbackBufferSamples;
    if (effectiveSamples <= 0)
        effectiveSamples = 512;
    return {
        .outputRawSamples = outputRawSamples,
        .callbackBufferSamples = callbackBufferSamples,
        .effectiveOutputSamples = effectiveSamples,
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

class ReverseStreamAudioSource : public juce::PositionableAudioSource {
public:
    ReverseStreamAudioSource(juce::PositionableAudioSource* source) : m_source(source) {}

    void setLoopRangeSamples(juce::int64 loopInSample, juce::int64 loopOutSample, double sampleRate) {
        const juce::SpinLock::ScopedLockType lock(m_stateLock);
        if (!m_source)
            return;

        m_sampleRate = sampleRate > 1.0 ? sampleRate : 44100.0;
        m_windowSamples = std::clamp(static_cast<int>(std::lround(m_sampleRate * 0.002)), 16, 256);

        const juce::int64 total = std::max<juce::int64>(0, m_source->getTotalLength());
        if (total <= 2) {
            m_loopEnabled = false;
            return;
        }

        juce::int64 in = std::clamp(loopInSample, static_cast<juce::int64>(0), total - 2);
        juce::int64 out = std::clamp(loopOutSample, static_cast<juce::int64>(1), total - 1);

        const juce::int64 minLen = std::max<juce::int64>(8, m_windowSamples * 2);
        if (out <= in + minLen)
            out = std::min(total - 1, in + minLen);
        if (out <= in + 2) {
            m_loopEnabled = false;
            return;
        }

        const int searchRadius = std::clamp(static_cast<int>(std::lround(m_sampleRate * 0.0015)), 8, 512);
        const juce::int64 snappedIn = findNearestZeroCrossingUnsafe(in, searchRadius, total);
        const juce::int64 snappedOut = findNearestZeroCrossingUnsafe(out, searchRadius, total);

        in = std::clamp(snappedIn, static_cast<juce::int64>(0), total - 2);
        out = std::clamp(snappedOut, in + 1, total - 1);
        if (out <= in + minLen)
            out = std::min(total - 1, in + minLen);
        if (out <= in + 2) {
            m_loopEnabled = false;
            return;
        }

        m_loopInSample = in;
        m_loopOutSample = out;
        m_loopEnabled = true;
        m_pendingFadeInSamples = 0;

        if (m_logicalPos < m_loopInSample || m_logicalPos >= m_loopOutSample)
            m_logicalPos = m_loopInSample;
    }

    void clearLoopRangeSamples() {
        const juce::SpinLock::ScopedLockType lock(m_stateLock);
        m_loopEnabled = false;
        m_pendingFadeInSamples = 0;
    }

    void setNextReadPosition(juce::int64 newPosition) override {
        const juce::SpinLock::ScopedLockType lock(m_stateLock);
        m_source->setNextReadPosition(newPosition);
        m_logicalPos = newPosition;
    }

    juce::int64 getNextReadPosition() const override {
        return m_logicalPos;
    }

    juce::int64 getTotalLength() const override {
        return m_source->getTotalLength();
    }

    bool isLooping() const override { return m_source->isLooping(); }
    void setLooping(bool shouldLoop) override { m_source->setLooping(shouldLoop); }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
        const juce::SpinLock::ScopedLockType lock(m_stateLock);
        m_source->prepareToPlay(samplesPerBlockExpected, sampleRate);
        m_sampleRate = sampleRate > 1.0 ? sampleRate : 44100.0;
        m_windowSamples = std::clamp(static_cast<int>(std::lround(m_sampleRate * 0.002)), 16, 256);
        m_pendingFadeInSamples = 0;
        m_pendingDirectionFadeInSamples = 0;
    }

    void releaseResources() override {
        const juce::SpinLock::ScopedLockType lock(m_stateLock);
        m_source->releaseResources();
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override {
        const juce::SpinLock::ScopedLockType lock(m_stateLock);

        if (m_loopEnabled && !m_reverse.load(std::memory_order_relaxed)) {
            getLoopedForwardAudioBlock(bufferToFill);
            applyDirectionFadeInToRange(bufferToFill.buffer,
                                        bufferToFill.startSample,
                                        bufferToFill.numSamples,
                                        bufferToFill.buffer ? bufferToFill.buffer->getNumChannels() : 0);
            return;
        }

        if (!m_reverse.load(std::memory_order_relaxed)) {
            // Forward playback
            m_source->setNextReadPosition(m_logicalPos);
            m_source->getNextAudioBlock(bufferToFill);
            m_logicalPos = m_source->getNextReadPosition();
            applyDirectionFadeInToRange(bufferToFill.buffer,
                                        bufferToFill.startSample,
                                        bufferToFill.numSamples,
                                        bufferToFill.buffer ? bufferToFill.buffer->getNumChannels() : 0);
            return;
        }

        // Reverse playback
        const int numSamples = bufferToFill.numSamples;
        juce::int64 currentPos = m_logicalPos;

        // Start reading from currentPos - numSamples, because reading goes forwards
        juce::int64 readStart = currentPos - numSamples;
        int samplesToRead = numSamples;
        int zerosToPad = 0;

        if (readStart < 0) {
            samplesToRead = static_cast<int>(currentPos);
            zerosToPad = numSamples - samplesToRead;
            readStart = 0;
        }

        if (samplesToRead > 0) {
            m_source->setNextReadPosition(readStart);
            
            juce::AudioSourceChannelInfo readInfo(bufferToFill);
            readInfo.startSample = bufferToFill.startSample;
            readInfo.numSamples = samplesToRead;
            m_source->getNextAudioBlock(readInfo);

            for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                float* ptr = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                std::reverse(ptr, ptr + samplesToRead);
            }

            // If we run into the start of the file, fade out smoothly before
            // zero-padding the remainder to avoid clicks.
            if (zerosToPad > 0) {
                applyFadeOutToTail(bufferToFill.buffer,
                                   bufferToFill.startSample,
                                   samplesToRead,
                                   bufferToFill.buffer->getNumChannels());
            }
        }

        if (zerosToPad > 0) {
            for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                juce::FloatVectorOperations::clear(
                    bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample + samplesToRead),
                    zerosToPad);
            }
        }

        m_logicalPos = currentPos - samplesToRead;
        if (m_logicalPos < 0) m_logicalPos = 0;

        applyDirectionFadeInToRange(bufferToFill.buffer,
                                    bufferToFill.startSample,
                                    bufferToFill.numSamples,
                                    bufferToFill.buffer ? bufferToFill.buffer->getNumChannels() : 0);
    }

    void setReverse(bool rev) {
        const juce::SpinLock::ScopedLockType lock(m_stateLock);
        const bool prev = m_reverse.load(std::memory_order_relaxed);
        if (prev == rev)
            return;

        m_reverse.store(rev, std::memory_order_relaxed);
        m_pendingDirectionFadeInSamples = std::max(16, m_windowSamples);
    }

private:
    void applyFadeInToRange(juce::AudioBuffer<float>* buffer,
                            int startSample,
                            int count,
                            int numChannels) {
        if (!buffer || count <= 0 || m_windowSamples <= 0 || m_pendingFadeInSamples <= 0)
            return;

        const int applyCount = std::min(count, m_pendingFadeInSamples);
        const int startPhase = m_windowSamples - m_pendingFadeInSamples;
        for (int i = 0; i < applyCount; ++i) {
            const float g = std::clamp(static_cast<float>(startPhase + i + 1)
                                     / static_cast<float>(m_windowSamples),
                                       0.0f, 1.0f);
            for (int ch = 0; ch < numChannels; ++ch) {
                float* w = buffer->getWritePointer(ch, startSample + i);
                *w *= g;
            }
        }
        m_pendingFadeInSamples -= applyCount;
    }

    void applyFadeOutToTail(juce::AudioBuffer<float>* buffer,
                            int chunkStart,
                            int chunkLen,
                            int numChannels) {
        if (!buffer || chunkLen <= 0 || m_windowSamples <= 0)
            return;

        const int fadeLen = std::min(m_windowSamples, chunkLen);
        const int fadeStart = chunkStart + chunkLen - fadeLen;
        for (int i = 0; i < fadeLen; ++i) {
            const float g = static_cast<float>(fadeLen - i)
                          / static_cast<float>(fadeLen + 1);
            for (int ch = 0; ch < numChannels; ++ch) {
                float* w = buffer->getWritePointer(ch, fadeStart + i);
                *w *= g;
            }
        }
    }

    void applyDirectionFadeInToRange(juce::AudioBuffer<float>* buffer,
                                     int startSample,
                                     int count,
                                     int numChannels) {
        if (!buffer || count <= 0 || m_windowSamples <= 0 || m_pendingDirectionFadeInSamples <= 0)
            return;

        const int applyCount = std::min(count, m_pendingDirectionFadeInSamples);
        const int startPhase = m_windowSamples - m_pendingDirectionFadeInSamples;
        for (int i = 0; i < applyCount; ++i) {
            const float g = std::clamp(static_cast<float>(startPhase + i + 1)
                                     / static_cast<float>(m_windowSamples),
                                       0.0f, 1.0f);
            for (int ch = 0; ch < numChannels; ++ch) {
                float* w = buffer->getWritePointer(ch, startSample + i);
                *w *= g;
            }
        }
        m_pendingDirectionFadeInSamples -= applyCount;
    }

    void getLoopedForwardAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) {
        auto* out = bufferToFill.buffer;
        if (!out || bufferToFill.numSamples <= 0 || !m_source) {
            bufferToFill.clearActiveBufferRegion();
            return;
        }

        const int numChannels = out->getNumChannels();
        int remaining = bufferToFill.numSamples;
        int destOffset = 0;

        while (remaining > 0) {
            if (m_logicalPos < m_loopInSample || m_logicalPos >= m_loopOutSample)
                m_logicalPos = m_loopInSample;

            const juce::int64 samplesToBoundary = m_loopOutSample - m_logicalPos;
            if (samplesToBoundary <= 0) {
                m_logicalPos = m_loopInSample;
                continue;
            }

            const int chunk = std::min<int>(remaining, static_cast<int>(samplesToBoundary));

            juce::AudioSourceChannelInfo readInfo(bufferToFill);
            readInfo.startSample = bufferToFill.startSample + destOffset;
            readInfo.numSamples = chunk;

            m_source->setNextReadPosition(m_logicalPos);
            m_source->getNextAudioBlock(readInfo);

            applyFadeInToRange(out, readInfo.startSample, chunk, numChannels);

            const bool hitsBoundary = (chunk == samplesToBoundary);
            if (hitsBoundary) {
                applyFadeOutToTail(out, readInfo.startSample, chunk, numChannels);
                m_logicalPos = m_loopInSample;
                m_pendingFadeInSamples = m_windowSamples;
            } else {
                m_logicalPos += chunk;
            }

            destOffset += chunk;
            remaining -= chunk;
        }
    }

    juce::int64 findNearestZeroCrossingUnsafe(juce::int64 approx,
                                              int radius,
                                              juce::int64 totalLength) {
        if (!m_source || totalLength <= 2)
            return approx;

        const juce::int64 start = std::clamp(approx - static_cast<juce::int64>(radius),
                                             static_cast<juce::int64>(0),
                                             totalLength - 2);
        const juce::int64 end = std::clamp(approx + static_cast<juce::int64>(radius),
                                           static_cast<juce::int64>(1),
                                           totalLength - 1);
        const int count = static_cast<int>(std::max<juce::int64>(0, end - start + 1));
        if (count < 2)
            return std::clamp(approx, static_cast<juce::int64>(0), totalLength - 1);

        juce::AudioBuffer<float> probe(2, count);
        juce::AudioSourceChannelInfo info;
        info.buffer = &probe;
        info.startSample = 0;
        info.numSamples = count;
        probe.clear();

        const juce::int64 oldPos = m_source->getNextReadPosition();
        m_source->setNextReadPosition(start);
        m_source->getNextAudioBlock(info);
        m_source->setNextReadPosition(oldPos);

        const int ch1 = probe.getNumChannels() > 1 ? 1 : 0;
        const float* p0 = probe.getReadPointer(0);
        const float* p1 = probe.getReadPointer(ch1);

        juce::int64 bestSample = std::clamp(approx, start, end);
        juce::int64 bestDist = std::numeric_limits<juce::int64>::max();

        for (int i = 1; i < count; ++i) {
            const float prev = 0.5f * (p0[i - 1] + p1[i - 1]);
            const float curr = 0.5f * (p0[i] + p1[i]);
            const bool crosses = (prev <= 0.0f && curr >= 0.0f)
                              || (prev >= 0.0f && curr <= 0.0f);
            if (!crosses)
                continue;

            const juce::int64 sampleIdx = start + i;
            const juce::int64 dist = std::llabs(sampleIdx - approx);
            if (dist < bestDist) {
                bestDist = dist;
                bestSample = sampleIdx;
                if (dist == 0)
                    break;
            }
        }

        if (bestDist == std::numeric_limits<juce::int64>::max()) {
            float bestAbs = std::numeric_limits<float>::max();
            for (int i = 0; i < count; ++i) {
                const float s = 0.5f * (p0[i] + p1[i]);
                const float a = std::abs(s);
                if (a < bestAbs) {
                    bestAbs = a;
                    bestSample = start + i;
                }
            }
        }

        return std::clamp(bestSample, static_cast<juce::int64>(0), totalLength - 1);
    }

    juce::PositionableAudioSource* m_source;
    std::atomic<bool> m_reverse{false};
    juce::int64 m_logicalPos{0};
    juce::SpinLock m_stateLock;
    bool m_loopEnabled{false};
    juce::int64 m_loopInSample{0};
    juce::int64 m_loopOutSample{0};
    double m_sampleRate{44100.0};
    int m_windowSamples{64};
    int m_pendingFadeInSamples{0};
    int m_pendingDirectionFadeInSamples{0};
};

class DjEngine::TimeStretchAudioSource : public juce::AudioSource {
public:
    static constexpr int kMinPullSize = 64;
    static constexpr int kMaxPullSize = 512;
    static constexpr int kMaxPrefillSamples = 2048;
    static constexpr int kDefaultPrefillCapSamples = 256;
    static constexpr int kPrefillMaxBlocks = 2;
    static constexpr int kPullLoopLimit = 24;
    static constexpr double kPrefillDeadbandTempoDelta = 0.01;
    static constexpr double kPrefillDynamicFactor = 0.005;
    static constexpr double kPrefillExtremeThreshold = 0.30;
    static constexpr double kPrefillExtremeFactor = 0.010;

    TimeStretchAudioSource(juce::AudioSource* inSource) : source(inSource) {}

    void setTempoRatio(double ratio) {
        if (std::abs(tempoRatio - ratio) < 0.001) return;
        tempoRatio = std::clamp(ratio, 0.01, 8.0);
        updateStretcherRatios();
    }

    void setPitchLockEnabled(bool enabled) {
        if (m_pitchLockEnabled.load(std::memory_order_relaxed) == enabled)
            return;

        m_pitchLockEnabled.store(enabled, std::memory_order_relaxed);
        updateStretcherRatios();
    }

    void prepareToPlay(int samplesPerBlockExpected, double sr) override {
        sampleRate = sr;
        if (source) source->prepareToPlay(samplesPerBlockExpected, sr);
        stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
            sr, 2, 
            RubberBand::RubberBandStretcher::OptionProcessRealTime |
            RubberBand::RubberBandStretcher::OptionWindowShort |
            RubberBand::RubberBandStretcher::OptionPitchHighSpeed);

        m_maxProcessSize = std::clamp(samplesPerBlockExpected > 0 ? samplesPerBlockExpected * 2 : 512,
                                      kMinPullSize,
                                      4096);
        stretcher->setMaxProcessSize(static_cast<size_t>(m_maxProcessSize));

        const int baseBlockSize = samplesPerBlockExpected > 0
            ? samplesPerBlockExpected
            : kDefaultPrefillCapSamples;
        m_prefillHardCapSamples = std::clamp(baseBlockSize * kPrefillMaxBlocks,
                                             kMinPullSize,
                                             kMaxPrefillSamples);

        updateStretcherRatios();

        m_startPadRemaining.store(static_cast<int>(stretcher->getPreferredStartPad()), std::memory_order_relaxed);
        m_startDelayTrimRemaining.store(static_cast<int>(stretcher->getStartDelay()), std::memory_order_relaxed);
        
        scratchBuffer.setSize(2, 8192);
        outputBuffer.setSize(2, 65536);
        trimBuffer.setSize(2, 1024);
        fifo = std::make_unique<juce::AbstractFifo>(65536);
    }

    void releaseResources() override {
        if (source) source->releaseResources();
        stretcher.reset();
        fifo.reset();
        m_prefillTargetSamples = 0;
        m_reportedLatencySamples.store(0, std::memory_order_relaxed);
        m_startPadRemaining.store(0, std::memory_order_relaxed);
        m_startDelayTrimRemaining.store(0, std::memory_order_relaxed);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        if (!source || !stretcher || !fifo) {
            info.clearActiveBufferRegion();
            return;
        }

        const int numCh = std::min(info.buffer->getNumChannels(), 2);
        const int framesNeeded = info.numSamples;
        const int destStart = info.startSample;
        int maxPullLoops = kPullLoopLimit;

        const int desiredPrefill = computeDesiredPrefillSamples();
        updatePrefillTarget(desiredPrefill);

        while (fifo->getNumReady() < framesNeeded + m_prefillTargetSamples && maxPullLoops > 0) {
            const int shortfall = std::max(0, (framesNeeded + m_prefillTargetSamples) - fifo->getNumReady());
            int pullSize = stretcher->getSamplesRequired();
            if (pullSize <= 0)
                pullSize = std::max(kMinPullSize, shortfall);
            pullSize = std::clamp(pullSize, kMinPullSize, std::min(kMaxPullSize, m_maxProcessSize));

            if (scratchBuffer.getNumSamples() < pullSize)
                scratchBuffer.setSize(2, pullSize, true);

            juce::AudioSourceChannelInfo pullInfo;
            pullInfo.buffer = &scratchBuffer;
            pullInfo.startSample = 0;
            pullInfo.numSamples = pullSize;
            scratchBuffer.clear(0, pullSize);

            const int startPadRemaining = m_startPadRemaining.load(std::memory_order_relaxed);
            if (startPadRemaining > 0) {
                const int padNow = std::min(startPadRemaining, pullSize);
                m_startPadRemaining.store(startPadRemaining - padNow, std::memory_order_relaxed);

                if (padNow < pullSize) {
                    juce::AudioSourceChannelInfo tailInfo;
                    tailInfo.buffer = &scratchBuffer;
                    tailInfo.startSample = padNow;
                    tailInfo.numSamples = pullSize - padNow;
                    source->getNextAudioBlock(tailInfo);
                }
            } else {
                source->getNextAudioBlock(pullInfo);
            }

            const float* inputs[2] = { scratchBuffer.getReadPointer(0), scratchBuffer.getReadPointer(1) };
            if (scratchBuffer.getNumChannels() == 1)
                inputs[1] = inputs[0];

            stretcher->process(inputs, pullSize, false);

            int avail = stretcher->available();
            int toTrim = m_startDelayTrimRemaining.load(std::memory_order_relaxed);
            if (avail > 0 && toTrim > 0) {
                const int trimNow = std::min(avail, toTrim);
                trimStretcherOutput(trimNow);
                m_startDelayTrimRemaining.store(toTrim - trimNow, std::memory_order_relaxed);
                avail = stretcher->available();
            }

            if (avail > 0) {
                if (outputBuffer.getNumSamples() < avail)
                    outputBuffer.setSize(2, std::max((int) outputBuffer.getNumSamples(), avail * 2), true);

                int start1, size1, start2, size2;
                fifo->prepareToWrite(avail, start1, size1, start2, size2);

                if (size1 > 0) {
                    float* outputs1[2] = { outputBuffer.getWritePointer(0, start1), outputBuffer.getWritePointer(1, start1) };
                    stretcher->retrieve(outputs1, size1);
                }
                if (size2 > 0) {
                    float* outputs2[2] = { outputBuffer.getWritePointer(0, start2), outputBuffer.getWritePointer(1, start2) };
                    stretcher->retrieve(outputs2, size2);
                }

                fifo->finishedWrite(size1 + size2);
            }

            --maxPullLoops;
        }

        updateReportedLatency(std::max(m_prefillTargetSamples, fifo->getNumReady()));

        const int ready = fifo->getNumReady();
        const int toRead = std::min(ready, framesNeeded);
        if (toRead <= 0) {
            info.clearActiveBufferRegion();
            return;
        }

        int start1, size1, start2, size2;
        fifo->prepareToRead(toRead, start1, size1, start2, size2);

        if (size1 > 0) {
            for (int ch = 0; ch < numCh; ++ch)
                info.buffer->copyFrom(ch, destStart, outputBuffer, ch, start1, size1);
        }
        if (size2 > 0) {
            for (int ch = 0; ch < numCh; ++ch)
                info.buffer->copyFrom(ch, destStart + size1, outputBuffer, ch, start2, size2);
        }

        fifo->finishedRead(size1 + size2);

        if (toRead < framesNeeded) {
            const int remainderStart = destStart + toRead;
            const int remainderLen = framesNeeded - toRead;
            for (int ch = 0; ch < numCh; ++ch)
                info.buffer->clear(ch, remainderStart, remainderLen);
        }

    }

    // Return total latency introduced by this component in samples
    int getLatencySamples() const {
        int delay = m_reportedLatencySamples.load(std::memory_order_relaxed);
        delay += std::max(0, m_startDelayTrimRemaining.load(std::memory_order_relaxed));
        return delay;
    }

private:
    void updateReportedLatency(int targetSamples) {
        targetSamples = std::max(0, targetSamples);
        const int current = m_reportedLatencySamples.load(std::memory_order_relaxed);
        if (current == targetSamples)
            return;

        const int step = current < targetSamples
            ? std::max(8, (targetSamples - current) / 4)
            : std::max(16, (current - targetSamples) / 6);

        const int next = current < targetSamples
            ? std::min(targetSamples, current + step)
            : std::max(targetSamples, current - step);

        m_reportedLatencySamples.store(next, std::memory_order_relaxed);
    }

    int computeDesiredPrefillSamples() const {
        if (!m_pitchLockEnabled.load(std::memory_order_relaxed))
            return 0;

        const double tempoDelta = std::abs(tempoRatio - 1.0);
        if (tempoDelta < kPrefillDeadbandTempoDelta)
            return 0;

        const int dynamic = static_cast<int>(std::lround(tempoDelta * sampleRate * kPrefillDynamicFactor));
        const int extremeBoost = tempoDelta > kPrefillExtremeThreshold
            ? static_cast<int>(std::lround((tempoDelta - kPrefillExtremeThreshold) * sampleRate * kPrefillExtremeFactor))
            : 0;
        const int hardCap = std::max(kMinPullSize, m_prefillHardCapSamples);
        return std::clamp(dynamic + extremeBoost, 0, hardCap);
    }

    void updatePrefillTarget(int desiredPrefill) {
        if (m_prefillTargetSamples == desiredPrefill)
            return;

        if (desiredPrefill > m_prefillTargetSamples) {
            const int step = std::max(16, (desiredPrefill - m_prefillTargetSamples) / 6);
            m_prefillTargetSamples = std::min(desiredPrefill, m_prefillTargetSamples + step);
        } else {
            const int step = std::max(32, (m_prefillTargetSamples - desiredPrefill) / 3);
            m_prefillTargetSamples = std::max(desiredPrefill, m_prefillTargetSamples - step);
        }
    }

    void trimStretcherOutput(int samplesToTrim) {
        if (!stretcher || samplesToTrim <= 0)
            return;

        while (samplesToTrim > 0) {
            const int avail = stretcher->available();
            if (avail <= 0)
                break;

            const int chunk = std::min(samplesToTrim, avail);
            if (trimBuffer.getNumSamples() < chunk)
                trimBuffer.setSize(2, chunk, false, false, true);

            float* trimOut[2] = {
                trimBuffer.getWritePointer(0),
                trimBuffer.getWritePointer(1)
            };
            stretcher->retrieve(trimOut, chunk);
            samplesToTrim -= chunk;
        }
    }

    void updateStretcherRatios() {
        if (!stretcher)
            return;

        const double safeTempoRatio = std::clamp(std::abs(tempoRatio), 0.01, 8.0);
        const bool pitchLock = m_pitchLockEnabled.load(std::memory_order_relaxed);

        // Tempo is controlled by ResamplingAudioSource. Rubber Band stays at
        // 1:1 time and optionally compensates pitch when keylock is enabled.
        stretcher->setTimeRatio(1.0);
        stretcher->setPitchScale(pitchLock ? (1.0 / safeTempoRatio) : 1.0);
    }

    juce::AudioSource* source = nullptr;
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    juce::AudioBuffer<float> scratchBuffer;
    juce::AudioBuffer<float> outputBuffer;
    juce::AudioBuffer<float> trimBuffer;
    std::unique_ptr<juce::AbstractFifo> fifo;
    double sampleRate = 44100.0;
    double tempoRatio = 1.0;
    std::atomic<bool> m_pitchLockEnabled { false };
    std::atomic<int> m_reportedLatencySamples { 0 };
    std::atomic<int> m_startPadRemaining { 0 };
    std::atomic<int> m_startDelayTrimRemaining { 0 };
    int m_maxProcessSize = 512;
    int m_prefillTargetSamples = 0;
    int m_prefillHardCapSamples = kMaxPrefillSamples;
};

class DjEngine::MixerDspSource : public juce::AudioSource {
public:
    MixerDspSource(juce::AudioSource* inSource, DjEngine* owner)
        : source(inSource), m_owner(owner) {}
    // ── FxProcessor slot (called from Qt main thread) ──────────────────────
    void setFxEffectType(EffectType type) { m_fx.setEffectType(type); }
    void setFxAmount(float amount)        { m_fx.setAmount(amount); }
    void setFxSCKnob(float knob)          { m_fx.setSCKnobValue(knob); }
    void setFxSCParam(float param)        { m_fx.setSCParamValue(param); }

    // ── PAD FX slot (independent of the FX bar chain) ──────────────────────
    void setPadFxEffectType(EffectType type) { m_padFx.setEffectType(type); }
    void setPadFxAmount(float amount)        { m_padFx.setAmount(amount); }
    void clearPadFx() {
        m_padFx.setEffectType(EffectType::None);
        m_padFx.setAmount(0.0f);
    }
    void setVinylBrakeActive(bool active) { m_vinylBrakeWanted.store(active,  std::memory_order_relaxed); }
    void setEchoOutActive   (bool active) { m_echoOutWanted.store(active,     std::memory_order_relaxed); }
    void setBackspinActive  (bool active) { m_backspinWanted.store(active,    std::memory_order_relaxed); }
    void setRollOutActive   (bool active) { m_rollOutWanted.store(active,     std::memory_order_relaxed); }
    void setScratchTimbre(float amount)   { scratchTimbre.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed); }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
        if (source) source->prepareToPlay(samplesPerBlockExpected, sampleRate);

        m_routeScratch.setSize(2, std::max(64, samplesPerBlockExpected), false, true, true);

        m_fx.prepare(sampleRate, samplesPerBlockExpected, 2);
        m_padFx.prepare(sampleRate, samplesPerBlockExpected, 2);
        m_limiter.prepare(sampleRate, samplesPerBlockExpected, 2);
        
        juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(samplesPerBlockExpected), 2 };
        lowEq.prepare(spec);
        midEq.prepare(spec);
        highEq.prepare(spec);
        colorFilter.prepare(spec);

        m_sampleRate = sampleRate;
        m_scratchWarmLpState[0] = 0.0f;
        m_scratchWarmLpState[1] = 0.0f;

        // 1.15 s ramp from full speed to a complete stop
        m_vinylBrakeRampDown  = 1.0f / (1.15f * static_cast<float>(sampleRate));
        m_vinylBrakeFactor    = 1.0f;
        m_vinylBrakeWritePos  = 0;
        m_vinylBrakeReadPos   = 0.0f;
        m_vinylBrakeNeedSync  = true;
        m_backspinNeedSync    = true;
        m_vinylBrakeBufL.fill(0.0f);
        m_vinylBrakeBufR.fill(0.0f);

        // Echo Out
        m_echoOutDelaySamples = std::min(kEchoOutBuf - 1,
                                         static_cast<int>(sampleRate * 0.375));
        m_echoOutLpCoef = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi
                                           * 4000.0f / static_cast<float>(sampleRate));
        m_echoOutBufL.fill(0.0f);
        m_echoOutBufR.fill(0.0f);
        m_echoOutWritePos  = 0;
        m_echoOutLpStateL  = 0.0f;
        m_echoOutLpStateR  = 0.0f;
        m_echoOutAudioActive = false;

        // Backspin: 2.5× initial speed, decelerates to 0 in ~2.0 s
        m_backspinSpeedRampDown = 2.5f / (2.0f * static_cast<float>(sampleRate));
        m_backspinSpeed         = 0.0f;

        // Roll Out
        m_rollOutRampDown  = 1.0f / (1.5f * static_cast<float>(sampleRate));
        m_rollOutNeedSync  = true;
        m_rollOutGain      = 1.0f;

        updateFilters();
    }

    void releaseResources() override {
        if (source) source->releaseResources();
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override {
        if (source) {
            if (bufferToFill.numSamples > 0) {
                source->getNextAudioBlock(bufferToFill);
            } else {
                // Keep this path side-effect free; some JACK states can report
                // zero-sized callbacks transiently. We only log in that case.
            }
        }

        if (m_sampleRate <= 0.0 || bufferToFill.buffer->getNumChannels() == 0 || bufferToFill.numSamples == 0) return;

        // Dynamic scratch timbre: at very slow platter speed, soften highs and
        // add a tiny saturation curve so drags sound less metallic/robotic.
        const float timbre = scratchTimbre.load(std::memory_order_relaxed);
        if (timbre > 0.0001f) {
            const int numChannels = std::min(bufferToFill.buffer->getNumChannels(), 2);
            const float cutoffHz = 1400.0f + (1.0f - timbre) * 9000.0f;
            const float pole = std::exp(-2.0f * juce::MathConstants<float>::pi * cutoffHz
                                        / static_cast<float>(m_sampleRate));
            const float alpha = 1.0f - std::clamp(pole, 0.0f, 0.9999f);
            const float drive = 1.0f + timbre * 1.2f;
            const float norm = std::max(0.001f, std::tanh(drive));

            for (int ch = 0; ch < numChannels; ++ch) {
                float state = m_scratchWarmLpState[ch];
                float* w = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                for (int i = 0; i < bufferToFill.numSamples; ++i) {
                    state += alpha * (w[i] - state);
                    w[i] = std::tanh(state * drive) / norm;
                }
                m_scratchWarmLpState[ch] = state;
            }
        }

        juce::dsp::AudioBlock<float> block(*bufferToFill.buffer);
        auto slicedBlock = block.getSubBlock(bufferToFill.startSample, bufferToFill.numSamples);
        juce::dsp::ProcessContextReplacing<float> context(slicedBlock);

        // Apply trim pre-EQ/pre-fader (QML trim range: 0..2).
        float gain = trimVal.load(std::memory_order_relaxed);
        if (std::abs(gain - 1.0f) > 0.001f) {
            for (size_t ch = 0; ch < slicedBlock.getNumChannels(); ++ch) {
                juce::FloatVectorOperations::multiply(slicedBlock.getChannelPointer(ch), gain, static_cast<int>(slicedBlock.getNumSamples()));
            }
        }

        lowEq.process(context);
        midEq.process(context);
        highEq.process(context);
        colorFilter.process(context);

        // ── FX slot (Reverb / Bitcrusher / PitchShifter) ──────────────────────
        m_fx.process(*bufferToFill.buffer,
                     bufferToFill.startSample,
                     bufferToFill.numSamples);

        // ── PAD FX slot (Performance Pads — independent of FX bar) ────────────
        m_padFx.process(*bufferToFill.buffer,
                        bufferToFill.startSample,
                        bufferToFill.numSamples);

        // ── Circular buffer: always records; vinyl brake + backspin read from it ──
        {
            const bool wantBrake    = m_vinylBrakeWanted.load(std::memory_order_relaxed);
            const bool wantBackspin = m_backspinWanted.load(std::memory_order_relaxed);

            const int numCh = std::min(bufferToFill.buffer->getNumChannels(), 2);
            const int bStart = bufferToFill.startSample;
            const int bN     = bufferToFill.numSamples;
            float* dataL = numCh > 0 ? bufferToFill.buffer->getWritePointer(0, bStart) : nullptr;
            float* dataR = numCh > 1 ? bufferToFill.buffer->getWritePointer(1, bStart) : nullptr;

            // Sync read positions on first sample of activation
            if (wantBrake && m_vinylBrakeNeedSync) {
                m_vinylBrakeReadPos  = static_cast<float>((m_vinylBrakeWritePos - 1 + kVinylBrakeBuf) & kVinylBrakeMask);
                m_vinylBrakeFactor   = 1.0f;
                m_vinylBrakeNeedSync = false;
            }
            if (!wantBrake)
                m_vinylBrakeNeedSync = true;

            if (wantBackspin && m_backspinNeedSync) {
                m_backspinReadPos  = static_cast<float>((m_vinylBrakeWritePos - 1 + kVinylBrakeBuf) & kVinylBrakeMask);
                m_backspinSpeed    = 2.5f;  // start at 2.5× backward speed → decelerates to 0
                m_backspinNeedSync = false;
            }
            if (!wantBackspin) {
                m_backspinNeedSync = true;
                m_backspinSpeed    = 0.0f;
            }

            for (int i = 0; i < bN; ++i) {
                // Always record post-FX audio so backspin always has fresh material
                const int wp = m_vinylBrakeWritePos & kVinylBrakeMask;
                if (dataL) m_vinylBrakeBufL[wp] = dataL[i];
                if (dataR) m_vinylBrakeBufR[wp] = dataR[i];
                ++m_vinylBrakeWritePos;

                if (wantBrake || m_vinylBrakeFactor < 1.0f) {
                    // Ramp playback rate toward zero (pitch drops to silence)
                    if (wantBrake) {
                        m_vinylBrakeFactor = std::max(0.0f, m_vinylBrakeFactor - m_vinylBrakeRampDown);
                    } else {
                        // Released: snap back to live audio immediately
                        m_vinylBrakeFactor  = 1.0f;
                        m_vinylBrakeReadPos = static_cast<float>((m_vinylBrakeWritePos - 1) & kVinylBrakeMask);
                    }
                    const int rp = static_cast<int>(m_vinylBrakeReadPos) & kVinylBrakeMask;
                    if (m_vinylBrakeFactor > 0.0f) {
                        if (dataL) dataL[i] = m_vinylBrakeBufL[rp];
                        if (dataR) dataR[i] = m_vinylBrakeBufR[rp];
                    } else {
                        if (dataL) dataL[i] = 0.0f;
                        if (dataR) dataR[i] = 0.0f;
                    }
                    m_vinylBrakeReadPos += m_vinylBrakeFactor;
                    if (m_vinylBrakeReadPos >= static_cast<float>(kVinylBrakeBuf))
                        m_vinylBrakeReadPos -= static_cast<float>(kVinylBrakeBuf);
                }
                else if (wantBackspin) {
                    if (m_backspinSpeed > 0.0f) {
                        // Linear-interpolated backward read: high pitch → low pitch → silence
                        const int   rp0  = static_cast<int>(m_backspinReadPos) & kVinylBrakeMask;
                        const int   rp1  = (rp0 + 1) & kVinylBrakeMask;
                        const float frac = m_backspinReadPos - std::floor(m_backspinReadPos);
                        if (dataL) dataL[i] = m_vinylBrakeBufL[rp0] + frac * (m_vinylBrakeBufL[rp1] - m_vinylBrakeBufL[rp0]);
                        if (dataR) dataR[i] = m_vinylBrakeBufR[rp0] + frac * (m_vinylBrakeBufR[rp1] - m_vinylBrakeBufR[rp0]);
                        m_backspinReadPos -= m_backspinSpeed;
                        if (m_backspinReadPos < 0.0f)
                            m_backspinReadPos += static_cast<float>(kVinylBrakeBuf);
                        m_backspinSpeed = std::max(0.0f, m_backspinSpeed - m_backspinSpeedRampDown);
                    } else {
                        if (dataL) dataL[i] = 0.0f;
                        if (dataR) dataR[i] = 0.0f;
                    }
                }
            }
        }

        // ── Echo Out (stop effect): records live audio when idle; when active, cuts
        //    live audio and plays the echo tail from the pre-recorded buffer → silence
        {
            const bool wantEchoOut = m_echoOutWanted.load(std::memory_order_relaxed);

            if (!wantEchoOut && m_echoOutAudioActive) {
                // Toggle turned off: reset for next use
                m_echoOutAudioActive = false;
                m_echoOutLpStateL    = 0.0f;
                m_echoOutLpStateR    = 0.0f;
            } else if (wantEchoOut && !m_echoOutAudioActive) {
                m_echoOutAudioActive = true;
            }

            const int numCh = std::min(bufferToFill.buffer->getNumChannels(), 2);
            const int bStart = bufferToFill.startSample;
            const int bN     = bufferToFill.numSamples;

            if (!m_echoOutAudioActive) {
                // Idle: continuously record live audio into the delay buffer (no feedback)
                const float* srcL = numCh > 0 ? bufferToFill.buffer->getReadPointer(0, bStart) : nullptr;
                const float* srcR = numCh > 1 ? bufferToFill.buffer->getReadPointer(1, bStart) : nullptr;
                for (int i = 0; i < bN; ++i) {
                    m_echoOutBufL[m_echoOutWritePos & kEchoOutMask] = srcL ? srcL[i] : 0.f;
                    m_echoOutBufR[m_echoOutWritePos & kEchoOutMask] = srcR ? srcR[i] : 0.f;
                    ++m_echoOutWritePos;
                }
            } else {
                // Active: cut live audio, play echo tail from pre-recorded buffer → silence
                float* dataL = numCh > 0 ? bufferToFill.buffer->getWritePointer(0, bStart) : nullptr;
                float* dataR = numCh > 1 ? bufferToFill.buffer->getWritePointer(1, bStart) : nullptr;
                for (int i = 0; i < bN; ++i) {
                    const int rp = (m_echoOutWritePos - m_echoOutDelaySamples + kEchoOutBuf) & kEchoOutMask;
                    const float delL = m_echoOutBufL[rp];
                    const float delR = m_echoOutBufR[rp];
                    m_echoOutLpStateL += m_echoOutLpCoef * (delL - m_echoOutLpStateL);
                    m_echoOutLpStateR += m_echoOutLpCoef * (delR - m_echoOutLpStateR);
                    // Feed silence + feedback so the echo repeats and decays
                    m_echoOutBufL[m_echoOutWritePos & kEchoOutMask] = m_echoOutLpStateL * kEchoOutFeedback;
                    m_echoOutBufR[m_echoOutWritePos & kEchoOutMask] = m_echoOutLpStateR * kEchoOutFeedback;
                    ++m_echoOutWritePos;
                    // Output is ONLY the echo tail (live audio is cut → stop effect)
                    if (dataL) dataL[i] = m_echoOutLpStateL;
                    if (dataR) dataR[i] = m_echoOutLpStateR;
                }
            }
        }

        // ── Roll Out (stop effect): loops a captured 250 ms segment and fades to silence
        {
            const bool wantRollOut = m_rollOutWanted.load(std::memory_order_relaxed);

            if (wantRollOut && m_rollOutNeedSync) {
                m_rollOutLoopLen   = std::min(static_cast<int>(m_sampleRate * 0.25),
                                              kVinylBrakeBuf / 4);
                m_rollOutLoopStart = (m_vinylBrakeWritePos - m_rollOutLoopLen + kVinylBrakeBuf * 2)
                                      & kVinylBrakeMask;
                m_rollOutOffset    = 0.0f;
                m_rollOutGain      = 1.0f;
                m_rollOutNeedSync  = false;
            }
            if (!wantRollOut)
                m_rollOutNeedSync = true;

            if (wantRollOut && !m_rollOutNeedSync) {
                const int numCh = std::min(bufferToFill.buffer->getNumChannels(), 2);
                const int bStart = bufferToFill.startSample;
                const int bN     = bufferToFill.numSamples;
                float* dataL = numCh > 0 ? bufferToFill.buffer->getWritePointer(0, bStart) : nullptr;
                float* dataR = numCh > 1 ? bufferToFill.buffer->getWritePointer(1, bStart) : nullptr;

                for (int i = 0; i < bN; ++i) {
                    const int rp = (m_rollOutLoopStart + static_cast<int>(m_rollOutOffset))
                                    & kVinylBrakeMask;
                    if (m_rollOutGain > 0.0f) {
                        if (dataL) dataL[i] = m_vinylBrakeBufL[rp] * m_rollOutGain;
                        if (dataR) dataR[i] = m_vinylBrakeBufR[rp] * m_rollOutGain;
                        m_rollOutGain = std::max(0.0f, m_rollOutGain - m_rollOutRampDown);
                    } else {
                        if (dataL) dataL[i] = 0.0f;
                        if (dataR) dataR[i] = 0.0f;
                    }
                    m_rollOutOffset += 1.0f;
                    if (static_cast<int>(m_rollOutOffset) >= m_rollOutLoopLen)
                        m_rollOutOffset -= static_cast<float>(m_rollOutLoopLen);
                }
            }
        }

        // Channel pre-fader meter: post Trim/EQ/Filter/FX, pre channel fader/crossfader.
        {
            auto* buf = bufferToFill.buffer;
            const int s = bufferToFill.startSample;
            const int n = bufferToFill.numSamples;

            float peakPreL = 0.0f;
            float peakPreR = 0.0f;
            if (buf->getNumChannels() > 0)
                peakPreL = buf->getMagnitude(0, s, n);
            if (buf->getNumChannels() > 1)
                peakPreR = buf->getMagnitude(1, s, n);

            m_preFaderPeakL.store(peakPreL, std::memory_order_relaxed);
            m_preFaderPeakR.store(peakPreR, std::memory_order_relaxed);
        }

        // Apply channel volume (includes crossfader contribution from UI).
        const float faderGain = faderVal.load(std::memory_order_relaxed);
        if (std::abs(faderGain - 1.0f) > 0.001f) {
            for (size_t ch = 0; ch < slicedBlock.getNumChannels(); ++ch) {
                juce::FloatVectorOperations::multiply(slicedBlock.getChannelPointer(ch), faderGain, static_cast<int>(slicedBlock.getNumSamples()));
            }
        }

        // ── Master Volume + Anti-Clip Limiter ───────────────────────────
        {
            auto* buf = bufferToFill.buffer;
            const int s = bufferToFill.startSample;
            const int n = bufferToFill.numSamples;

            // Apply master volume
            float masterGain = s_masterVolume.load(std::memory_order_relaxed);
            if (std::abs(masterGain - 1.0f) > 0.001f) {
                for (int ch = 0; ch < buf->getNumChannels(); ++ch)
                    buf->applyGain(ch, s, n, masterGain);
            }

            // ── VU peak level measurement (BEFORE limiter, real incoming signal) ───
            // Measure true peaks before anti-clip processing so VU shows real levels.
            {
                float peakL = 0.0f, peakR = 0.0f;
                if (buf->getNumChannels() > 0)
                    peakL = buf->getMagnitude(0, s, n);
                if (buf->getNumChannels() > 1)
                    peakR = buf->getMagnitude(1, s, n);
                m_peakL.store(peakL, std::memory_order_relaxed);
                m_peakR.store(peakR, std::memory_order_relaxed);
                // Strict digital clip indicator: only flag when real incoming signal
                // overshoots full scale with a small margin.
                constexpr float kClipThreshold = 1.001f;
                m_clipDetected.store((peakL > kClipThreshold) || (peakR > kClipThreshold),
                                     std::memory_order_relaxed);
            }

            // Brickwall Limiter: always processes (consistent latency),
            // seamless crossfade handled internally via setEnabled()
            {
                m_limiter.setEnabled(s_antiClipEnabled.load(std::memory_order_relaxed));

                float* channelPtrs[16];
                const int numCh = std::min(buf->getNumChannels(), 16);
                for (int ch = 0; ch < numCh; ++ch)
                    channelPtrs[ch] = buf->getWritePointer(ch);

                float gr = m_limiter.processBlock(channelPtrs, numCh, s, n);
                s_gainReduction.store(m_limiter.isEnabled() ? gr : 1.0f,
                                      std::memory_order_relaxed);
            }

            applyOutputRouting(*buf, s, n);
        }
    }

    void setTrim(float val) { trimVal = val; }
    void setFader(float val) { faderVal = val; }

    void setEq(float l, float m, float h) {
        // -1 to +1 -> approx -32 dB to +6 dB (Pioneer DJM A9 is -infinity to +6)
        lowVol = l;
        midVol = m;
        highVol = h;
        // avoid continuous recomputation; just flag it or recompute directly
        updateFilters();
    }

    void setFilterVal(float f) {
        filterVal = f;
        updateFilters();
    }

private:
    void routeStereoToPair(juce::AudioBuffer<float>& buffer,
                           const float* srcL,
                           const float* srcR,
                           int start,
                           int n,
                           int firstChannel,
                           bool add)
    {
        // Null pointer checks
        if (!srcL || !srcR || n <= 0)
            return;
        
        if (firstChannel < 1)
            return;

        const int leftChannel = firstChannel - 1;
        const int rightChannel = leftChannel + 1;
        
        // Bounds check: ensure both channels exist in buffer
        if (leftChannel < 0 || rightChannel >= buffer.getNumChannels())
            return;
        
        // Additional safety: verify buffer is valid and has enough samples
        if (buffer.getNumSamples() < (start + n))
            return;

        float* dstL = buffer.getWritePointer(leftChannel, start);
        float* dstR = buffer.getWritePointer(rightChannel, start);
        
        if (!dstL || !dstR)
            return;

        for (int i = 0; i < n; ++i) {
            if (add) {
                dstL[i] += srcL[i];
                dstR[i] += srcR[i];
            } else {
                dstL[i] = srcL[i];
                dstR[i] = srcR[i];
            }
        }
    }

    void applyOutputRouting(juce::AudioBuffer<float>& buffer, int start, int n)
    {
        if (buffer.getNumChannels() < 2)
            return;
        if (m_routeScratch.getNumSamples() < n)
            return;

        m_routeScratch.copyFrom(0, 0, buffer, 0, start, n);
        m_routeScratch.copyFrom(1, 0, buffer, std::min(1, buffer.getNumChannels() - 1), start, n);
        const float* srcL = m_routeScratch.getReadPointer(0);
        const float* srcR = m_routeScratch.getReadPointer(1);

        const auto routing = unpackRouting(s_outputRoutingPacked.load(std::memory_order_relaxed));
        const bool cueActive = m_owner != nullptr && m_owner->cueEnabled();

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, start, n);

        bool hasOutput = false;
        if (routing.masterFirstChannel >= 1) {
            routeStereoToPair(buffer, srcL, srcR, start, n, routing.masterFirstChannel, false);
            hasOutput = true;
        }

        if (routing.boothFirstChannel >= 1) {
            const bool add = hasOutput;
            routeStereoToPair(buffer, srcL, srcR, start, n, routing.boothFirstChannel, add);
            hasOutput = true;
        }

        if (cueActive && routing.headphonesFirstChannel >= 1) {
            const bool add = hasOutput;
            routeStereoToPair(buffer, srcL, srcR, start, n, routing.headphonesFirstChannel, add);
        }
    }

    float getDecibelsFromKnob(float kb) const {
        if (kb < 0.0f) {
            return kb * 32.0f; // -1 -> -32 dB (approx -inf / kill)
        } else {
            return kb * 6.0f;  // +1 -> +6 dB
        }
    }

    void updateFilters() {
        if (m_sampleRate <= 0) return;
        
        // Update EQs using standard DJ shelving/peak frequencies
        *lowEq.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(m_sampleRate, 250.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(lowVol)));
        *midEq.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(m_sampleRate, 1000.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(midVol)));
        *highEq.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(m_sampleRate, 2500.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(highVol)));

        // Color Filter (LPF/HPF combo)
        if (std::abs(filterVal) < 0.05f) {
            // Flat response / completely bypassed
            // Since we can't 'bypass' perfectly, we just set a flat peak filter
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(m_sampleRate, 1000.0f, 0.707f, 1.0f);
        } else if (filterVal < 0.0f) {
            // Low Pass: sweep down from 20000 to ~80 Hz
            // t goes from 1.0 down to 0
            float t = 1.0f + filterVal;
            float freq = 80.0f * std::pow(20000.0f / 80.0f, t);
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(m_sampleRate, std::max(20.0f, freq), 1.2f);
        } else {
            // High Pass: sweep up from 20 to ~10000 Hz
            float freq = 20.0f * std::pow(10000.0f / 20.0f, filterVal);
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(m_sampleRate, std::max(20.0f, freq), 1.2f);
        }
    }

    juce::AudioSource* source = nullptr;
    DjEngine* m_owner = nullptr;
    double m_sampleRate = 0;

    std::atomic<float> trimVal{1.0f};
    std::atomic<float> faderVal{1.0f};
    
    // UI vals from -1 to 1
    std::atomic<float> lowVol{0.0f};
    std::atomic<float> midVol{0.0f};
    std::atomic<float> highVol{0.0f};
    std::atomic<float> filterVal{0.0f};

    using FilterType = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    FilterType lowEq;
    FilterType midEq;
    FilterType highEq;
    FilterType colorFilter;

    FxProcessor m_fx;
    FxProcessor m_padFx;
    BrickwallLimiter m_limiter;

    // Vinyl brake + Backspin — shared circular buffer, transport is unaffected
    static constexpr int kVinylBrakeBuf  = 1 << 18;
    static constexpr int kVinylBrakeMask = kVinylBrakeBuf - 1;
    std::atomic<bool> m_vinylBrakeWanted { false };
    std::atomic<bool> m_backspinWanted   { false };
    float m_vinylBrakeFactor   = 1.0f;
    float m_vinylBrakeRampDown = 0.0f;
    int   m_vinylBrakeWritePos = 0;
    float m_vinylBrakeReadPos  = 0.0f;
    float m_backspinReadPos    = 0.0f;
    bool  m_vinylBrakeNeedSync = true;  // audio-thread only
    bool  m_backspinNeedSync   = true;  // audio-thread only
    std::array<float, kVinylBrakeBuf> m_vinylBrakeBufL {};
    std::array<float, kVinylBrakeBuf> m_vinylBrakeBufR {};

    // Echo Out — records live audio when idle; when active cuts live and plays echo tail
    static constexpr int   kEchoOutBuf      = 65536;
    static constexpr int   kEchoOutMask     = kEchoOutBuf - 1;
    static constexpr float kEchoOutFeedback = 0.68f;
    std::atomic<bool>  m_echoOutWanted      { false };
    bool           m_echoOutAudioActive     = false;  // audio thread only
    int            m_echoOutWritePos        = 0;
    int            m_echoOutDelaySamples    = 0;
    float          m_echoOutLpCoef          = 0.0f;
    float          m_echoOutLpStateL        = 0.0f;
    float          m_echoOutLpStateR        = 0.0f;
    std::array<float, kEchoOutBuf> m_echoOutBufL {};
    std::array<float, kEchoOutBuf> m_echoOutBufR {};

    // Backspin speed ramp (audio thread only): starts at 2.0× backward, decelerates to 0
    float m_backspinSpeed        = 0.0f;
    float m_backspinSpeedRampDown = 0.0f;  // computed in prepareToPlay

    // Roll Out — loop + fade to silence in MixerDspSource (uses vinyl buffer for capture)
    std::atomic<bool> m_rollOutWanted    { false };
    bool  m_rollOutNeedSync  = true;
    int   m_rollOutLoopStart = 0;
    float m_rollOutOffset    = 0.0f;
    float m_rollOutGain      = 1.0f;
    float m_rollOutRampDown  = 0.0f;
    int   m_rollOutLoopLen   = 0;
    juce::AudioBuffer<float> m_routeScratch;
    std::atomic<float> scratchTimbre { 0.0f };
    float m_scratchWarmLpState[2] { 0.0f, 0.0f };

public:
    // VU meter peak levels — written on audio thread, read from UI thread
    std::atomic<float> m_peakL { 0.0f };
    std::atomic<float> m_peakR { 0.0f };
    std::atomic<float> m_preFaderPeakL { 0.0f };
    std::atomic<float> m_preFaderPeakR { 0.0f };
    std::atomic<bool>  m_clipDetected { false };

    // Global shared state: master volume + anti-clip (shared across all decks)
    static std::atomic<float> s_masterVolume;
    static std::atomic<bool>  s_antiClipEnabled;
    static std::atomic<float> s_gainReduction;
};

// Static shared state: master volume + anti-clip
std::atomic<float> DjEngine::MixerDspSource::s_masterVolume{1.0f};
std::atomic<bool>  DjEngine::MixerDspSource::s_antiClipEnabled{false};
std::atomic<float> DjEngine::MixerDspSource::s_gainReduction{1.0f};

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
    ensureSharedAudioDeviceManagerReady(deviceManager);

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
    connect(m_trackData, &TrackData::keyAnalyzed, this, [this]() {
        QString analysedKey = m_trackData->getDetectedKey();
        if (!analysedKey.isEmpty()) {
            m_trackKey = analysedKey;
            emit trackMetadataChanged();
        }

        persistCurrentAnalysisToLibrary();
    });

    // When BPM analysis finishes, re-emit tempoChanged so that currentBpm
    // and tempoRatio Q_PROPERTYs update in QML.
    connect(m_trackData, &TrackData::bpmAnalyzed, this, [this]() {
        emit tempoChanged();
        persistCurrentAnalysisToLibrary();
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
        persistCurrentAnalysisToLibrary();
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
            asVariant.push_back(m);
        }

        m_currentSegments = asVariant;
        emit segmentsChanged();

        if (m_libraryDb && !m_currentTrackId.isEmpty() && !segments.empty())
            m_libraryDb->updateTrackSegments(m_currentTrackId, segments);
    });

    juce::MessageManager::getInstance();
    formatManager.registerBasicFormats();

    // Prefer lower output buffer sizes for better scratch responsiveness.
    // Keep this conservative and only apply when the driver explicitly supports it.
    if (auto* device = deviceManager.getCurrentAudioDevice()) {
        const int currentSize = device->getCurrentBufferSizeSamples();
        const int targetSize = choosePreferredBufferSize(device, 128);

        if (targetSize < currentSize) {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager.getAudioDeviceSetup(setup);
            setup.bufferSize = targetSize;
            const juce::String setupErr = deviceManager.setAudioDeviceSetup(setup, true);
            if (setupErr.isEmpty()) {
                qDebug() << "[DjEngine] Reduced audio buffer size:" << currentSize << "->" << targetSize;
            } else {
                qWarning() << "[DjEngine] Could not reduce audio buffer size:" << QString::fromStdString(setupErr.toStdString());
            }
        }
    }

    deviceManager.addAudioCallback(&sourcePlayer);
    
    // Create the resampling source that wraps the transport source.
    // This allows us to control tempo/speed without changing the pitch.
    resamplingSource = std::make_unique<juce::ResamplingAudioSource>(
        &transportSource, 
        false,  // deleteSourceWhenDeleted = false (we own transportSource separately)
        2       // channels = 2 (stereo)
    );
    
    timeStretchSource = std::make_unique<TimeStretchAudioSource>(resamplingSource.get());
    
    // Create the mixer DSP source to apply EQ, Filter, and Gain based on Pioneer DJM A9.
    mixerSource = std::make_unique<MixerDspSource>(timeStretchSource.get(), this);
    mixerSource->setTrim(static_cast<float>(m_trim));
    mixerSource->setFader(static_cast<float>(m_volume));

    sourcePlayer.setSource(mixerSource.get());

    refreshHardwareLatency();
    clearOutputChannelCountCache();
    m_snapClock.start();

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
    sourcePlayer.setSource(nullptr);
    deviceManager.removeAudioCallback(&sourcePlayer);
    transportSource.setSource(nullptr);
}

void DjEngine::shutdownSharedAudioDeviceManager()
{
    auto& manager = sharedAudioDeviceManager();
    manager.closeAudioDevice();
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
    static int s_lastEffectiveSamples = -1;
    static int s_lastOutputRawSamples = -1;
    static int s_lastBufferSamples = -1;
    static int s_lastSampleRateRounded = -1;
    static bool s_loggedNoDevice = false;

    if (auto* device = deviceManager.getCurrentAudioDevice()) {
        const auto latency = readOutputLatencySnapshot(device);
        if (latency.sampleRate > 0.0)
            m_latencySeconds = static_cast<float>(static_cast<double>(latency.effectiveOutputSamples) / latency.sampleRate);

        const bool changed = (latency.effectiveOutputSamples != s_lastEffectiveSamples)
                          || (latency.outputRawSamples != s_lastOutputRawSamples)
                          || (latency.callbackBufferSamples != s_lastBufferSamples)
                          || (latency.roundedSampleRate() != s_lastSampleRateRounded)
                          || s_loggedNoDevice;

        if (changed) {
            qInfo() << "[DjEngine] Output latency:" << latency.effectiveOutputSamples
                    << "smp" << "(" << m_latencySeconds << "s)"
                    << "raw:" << latency.outputRawSamples
                    << "buf:" << latency.callbackBufferSamples
                    << "sr:" << latency.roundedSampleRate();
            s_lastEffectiveSamples = latency.effectiveOutputSamples;
            s_lastOutputRawSamples = latency.outputRawSamples;
            s_lastBufferSamples = latency.callbackBufferSamples;
            s_lastSampleRateRounded = latency.roundedSampleRate();
            s_loggedNoDevice = false;
        }
    } else {
        if (!s_loggedNoDevice) {
            qInfo() << "[DjEngine] No audio device yet; keeping last known latency";
            s_loggedNoDevice = true;
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
    bufferSize = std::clamp(bufferSize, 64, 4096);

    setLastAudioDeviceError(QString());

    const auto previousRouting = unpackRouting(s_outputRoutingPacked.load(std::memory_order_relaxed));

#if JUCE_LINUX || JUCE_BSD
    const QString requestedType = !deviceType.isEmpty() ? deviceType : getCurrentAudioDeviceType();
    const bool jackBackendRequested = requestedType.toLower().contains(QStringLiteral("jack"));
    if (requestedType.toLower().contains(QStringLiteral("alsa"))
        && sampleRate >= 96000
        && bufferSize < 128) {
        bufferSize = 128;
    }
#else
    const bool jackBackendRequested = deviceType.toLower().contains(QStringLiteral("jack"));
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
    s_outputRoutingPacked.store(packRouting({
        .masterFirstChannel = masterFirstChannel,
        .headphonesFirstChannel = headphonesFirstChannel,
        .boothFirstChannel = boothFirstChannel
    }), std::memory_order_relaxed);

    auto& manager = deviceManager;
    const juce::String previousType = manager.getCurrentAudioDeviceType();
    juce::AudioDeviceManager::AudioDeviceSetup previousSetup;
    manager.getAudioDeviceSetup(previousSetup);
    if (manager.getCurrentAudioDevice() == nullptr) {
        // The user explicitly requested a settings change — clear the timeout flag so
        // we retry even if startup init timed out (coreaudiod may have recovered).
        s_audioInitTimedOut = false;
        qWarning() << "[DjEngine] No current audio device before setup; trying default initialisation";
        const juce::String initErr = initialiseWithTimeout(manager, 0, 2);
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
                       << "- using default";
            sanitizedOutput.clear();
        }
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    manager.getAudioDeviceSetup(setup);
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
        } else {
            qWarning() << "[DjEngine] No current audio device to get name from";
            return false;
        }
    } else {
        setup.outputDeviceName = toJuceString(sanitizedOutput);
    }

    int maxOutputChannels = 2;  // Safe default
    const auto maxRoutedChannel = [](int firstChannel) -> int {
        return firstChannel >= 1 ? firstChannel + 1 : 0;
    };
    int maxRequestedChannel = std::max({
        maxRoutedChannel(masterFirstChannel),
        maxRoutedChannel(headphonesFirstChannel),
        maxRoutedChannel(boothFirstChannel),
        2
    });
    
    // JACK setup should avoid extra device probing because it can block.
    if (!jackBackendRequested) {
        if (auto* device = manager.getCurrentAudioDevice()) {
        const int namesCount = device->getOutputChannelNames().size();
        const int activeSetBits = device->getActiveOutputChannels().countNumberOfSetBits();
        if (namesCount > 0)
            maxOutputChannels = namesCount;
        else if (activeSetBits > 0)
            maxOutputChannels = activeSetBits;
        else
            maxOutputChannels = 2;
            
            // Clamp to reasonable max
            maxOutputChannels = std::clamp(maxOutputChannels, 2, kMaxSupportedOutputChannel);
            
            setup.bufferSize = choosePreferredBufferSize(device, setup.bufferSize);
        }
    } else {
        maxOutputChannels = std::clamp(maxRequestedChannel, 2, kMaxSupportedOutputChannel);
    }
    
    // Validate and clamp requested channels to actual device channels
    auto validateChannelForDevice = [maxOutputChannels](int& firstChannel) {
        if (firstChannel < 1)
            return;
        // Ensure both left and right channels fit within device
        // firstChannel is 1-based, so rightChannel = firstChannel + 1
        if (firstChannel < 1 || (firstChannel + 1) > maxOutputChannels) {
            qWarning() << "[DjEngine] Channel" << firstChannel << "out of range [1-" 
                       << maxOutputChannels - 1 << "], disabling this route";
            firstChannel = -1;  // Mark as disabled
        }
    };
    validateChannelForDevice(masterFirstChannel);
    validateChannelForDevice(headphonesFirstChannel);
    validateChannelForDevice(boothFirstChannel);
    
    // Update routing after validation
    s_outputRoutingPacked.store(packRouting({
        .masterFirstChannel = masterFirstChannel,
        .headphonesFirstChannel = headphonesFirstChannel,
        .boothFirstChannel = boothFirstChannel
    }), std::memory_order_relaxed);
    
    juce::BigInteger selectedOutputChannels;
    const auto setPairBits = [&selectedOutputChannels](int firstChannel) {
        if (firstChannel < 1)
            return;
        selectedOutputChannels.setBit(firstChannel - 1);
        selectedOutputChannels.setBit(firstChannel);
    };
    setPairBits(masterFirstChannel);
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
            setup.bufferSize = std::clamp(bufferSize, 128, 512);
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

        // During active scratch/release, onTimer() owns the resampling ratio.
        if (deck->m_isScrubbing || deck->m_scratchReleaseActive)
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
        snapshot.outputEffectiveSamples = latency.effectiveOutputSamples;
        if (latency.sampleRate > 0.0)
            snapshot.sampleRate = latency.sampleRate;
    } else if (m_lastLatencySnapshot.sampleRate > 0.0) {
        snapshot.outputRawSamples = m_lastLatencySnapshot.outputRawSamples;
        snapshot.bufferSamples = m_lastLatencySnapshot.bufferSamples;
        snapshot.outputEffectiveSamples = m_lastLatencySnapshot.outputEffectiveSamples;
    }

    if (timeStretchSource)
        snapshot.rubberbandSamples = std::max(0, timeStretchSource->getLatencySamples());

    // BrickwallLimiter default lookahead is 1.5 ms (set in BrickwallLimiter.h).
    snapshot.limiterSamples = std::max(0, static_cast<int>(std::lround(snapshot.sampleRate * 0.0015)));
    m_lastLatencySnapshot = snapshot;
    return snapshot;
}

double DjEngine::totalLatencyMs() const
{
    const auto snapshot = buildLatencySnapshot();
    if (snapshot.sampleRate <= 0.0)
        return 0.0;

    const int totalSamples = snapshot.outputEffectiveSamples
                           + snapshot.rubberbandSamples
                           + snapshot.limiterSamples;
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

    QVariantMap effectiveOutputRow;
    effectiveOutputRow.insert("name", QStringLiteral("Output to Speakers"));
    effectiveOutputRow.insert("samples", snapshot.outputEffectiveSamples);
    effectiveOutputRow.insert("ms", toMs(snapshot.outputEffectiveSamples));
    effectiveOutputRow.insert("countInTotal", true);
    rows.push_back(effectiveOutputRow);

    QVariantMap rawOutputRow;
    rawOutputRow.insert("name", QStringLiteral("JUCE Output (Raw)"));
    rawOutputRow.insert("samples", snapshot.outputRawSamples);
    rawOutputRow.insert("ms", toMs(snapshot.outputRawSamples));
    rawOutputRow.insert("countInTotal", false);
    rows.push_back(rawOutputRow);

    QVariantMap bufferRow;
    bufferRow.insert("name", QStringLiteral("Callback Buffer (Info)"));
    bufferRow.insert("samples", snapshot.bufferSamples);
    bufferRow.insert("ms", toMs(snapshot.bufferSamples));
    bufferRow.insert("countInTotal", false);
    rows.push_back(bufferRow);

    QVariantMap rubberbandRow;
    rubberbandRow.insert("name", QStringLiteral("RubberBand / Keylock"));
    rubberbandRow.insert("samples", snapshot.rubberbandSamples);
    rubberbandRow.insert("ms", toMs(snapshot.rubberbandSamples));
    rubberbandRow.insert("countInTotal", true);
    rows.push_back(rubberbandRow);

    QVariantMap limiterRow;
    limiterRow.insert("name", QStringLiteral("DSP Limiter Lookahead"));
    limiterRow.insert("samples", snapshot.limiterSamples);
    limiterRow.insert("ms", toMs(snapshot.limiterSamples));
    limiterRow.insert("countInTotal", true);
    rows.push_back(limiterRow);

    return rows;
}

double DjEngine::getPosition() const
{
    if (m_isScrubbing)
        return m_scrubHoldPosition;
    return transportSource.getCurrentPosition();
}

double DjEngine::getVisualPosition() const
{
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

    // No latency compensation: the 46ms hardware buffer latency is imperceptible
    // visually, and subtracting it caused a visible jump on play/pause transitions
    // (playing = compensated, paused = uncompensated → discontinuity).

    double len = transportSource.getLengthInSeconds();
    interpolated = std::clamp(interpolated, 0.0, len > 0.0 ? len : interpolated);

    return interpolated;
}

double DjEngine::getVisualPositionQml() const
{
    return getVisualPosition();
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

void DjEngine::setLibraryDatabase(LibraryDatabase* db)
{
    m_libraryDb = db;
    loadHotCuesForCurrentTrack();
    loadMainCueForCurrentTrack();
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
        beatGrid);
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
    m_mainCueSec = -1.0;
    m_mainCuePreviewActive = false;
    emit segmentsChanged();
    emit hotCuesChanged();
}

void DjEngine::populateMetadataFromReader(const juce::AudioFormatReader& reader,
                                          const QString& rawPath,
                                          const juce::File& file)
{
    const auto metaMap = buildMetadataLookup(reader.metadataValues);

    m_trackTitle = metaValue(metaMap, {"title", "id3title", "tit2", "tt2", "name", "tracktitle", "song"});
    m_trackArtist = metaValue(metaMap, {"artist", "id3artist", "tpe1", "albumartist", "tpe2", "band", "performer", "leadartist"});
    m_trackAlbum = metaValue(metaMap, {"album", "id3album", "talb", "record", "albumtitle"});
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
            const QString tlTitle  = cleanup(QString::fromStdString(tag->title().to8Bit(true)));
            const QString tlArtist = cleanup(QString::fromStdString(tag->artist().to8Bit(true)));
            if (m_trackTitle.isEmpty()  && !tlTitle.isEmpty())  m_trackTitle  = tlTitle;
            if (m_trackArtist.isEmpty() && !tlArtist.isEmpty()) m_trackArtist = tlArtist;
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
    m_currentTrackId = existingId.isEmpty()
        ? TrackIdGenerator::generate(m_trackArtist, m_trackTitle, durSec, rawPath)
        : existingId;
    m_libraryDb->addTrack(m_currentTrackId,
                          m_trackTitle, m_trackArtist, durSec, rawPath, bitrateKbps);

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
                                    cachedAnalysis.beatGrid);
        }

        const QString cachedKey = cachedAnalysis.key.trimmed();
        if (!cachedKey.isEmpty()) {
            m_trackKey = cachedKey;
            m_trackData->setKeyData(cachedKey);
        }
    } else {
        // Strictly hide segment UI state until fresh analysis writes data.
        m_currentSegments = QVariantList();
        emit segmentsChanged();
    }

    loadHotCuesForCurrentTrack();
    loadMainCueForCurrentTrack();
    return hasDbAnalysis;
}

void DjEngine::attachReaderToTransport(juce::AudioFormatReader* reader)
{
    transportSource.stop();
    transportSource.setSource(nullptr);

    readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    reverseWrapSource = std::make_unique<ReverseStreamAudioSource>(readerSource.get());
    reverseWrapSource->setReverse(m_isReverse);
    transportSource.setSource(reverseWrapSource.get(), 0, nullptr, reader->sampleRate);
    m_loadedTrackSampleRate = reader->sampleRate;
    transportSource.setPosition(0.0);
    ensureTransportRunningForPlayIntent();
}

void DjEngine::loadTrack(const QString& rawPath)
{
    juce::File file(rawPath.toStdString());

    if (!file.existsAsFile()) {
        qWarning() << "File does not exist:" << rawPath;
        return;
    }

    const quint64 gen = ++m_loadGen;

    // Immediately clear previous track state so the UI shows a clean slate.
    resetTrackLoadState();
    m_trackTitle.clear(); m_trackArtist.clear(); m_trackAlbum.clear();
    m_trackKey.clear();   m_trackDuration.clear(); m_trackDurationSec = 0.0;
    m_hasCoverArt = false; m_coverArtUrl.clear();
    if (m_coverProvider)
        m_coverProvider->clearCover(m_deckId);
    emit trackMetadataChanged();

    // All heavy file I/O (format detection, cover art, waveform cache) runs off the
    // main thread so the UI stays responsive during loading.
    const int pps = static_cast<int>(WAVEFORM_POINTS_PER_SECOND);
    std::thread([this, rawPath, file, gen, pps]() {
        auto* reader = formatManager.createReaderFor(file);
        if (!reader) {
            qWarning() << "[DjEngine] loadTrack: unsupported or unreadable format:" << rawPath;
            return;
        }

        QImage coverImage;
        const QByteArray coverData = CoverArtExtractor::extractCoverArt(rawPath).first;
        if (!coverData.isEmpty())
            coverImage.loadFromData(coverData);

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

        // Pre-downsample the full RGB data to at most kOverviewBins bins so the
        // overview waveform's paint() stays O(kOverviewBins) instead of O(total_frames).
        // Takes the max of each band per bin so transient peaks are preserved.
        static constexpr int kOverviewBins = 4096;
        QVector<TrackData::RgbWaveformFrame> overview;
        if (wfLoaded && !cache.rgb.isEmpty()) {
            const int total  = cache.rgb.size();
            const int factor = std::max(1, (total + kOverviewBins - 1) / kOverviewBins);
            overview.reserve((total + factor - 1) / factor);
            for (int i = 0; i < total; i += factor) {
                const int end = std::min(i + factor, total);
                float rms = 0.0f, low = 0.0f, mid = 0.0f, high = 0.0f;
                for (int j = i; j < end; ++j) {
                    const auto& f = cache.rgb[j];
                    rms  = std::max(rms,  f.rms);
                    low  = std::max(low,  f.low);
                    mid  = std::max(mid,  f.mid);
                    high = std::max(high, f.high);
                }
                TrackData::RgbWaveformFrame bin;
                bin.rms = rms; bin.low = low; bin.mid = mid; bin.high = high;
                overview.push_back(bin);
            }
        }

        QMetaObject::invokeMethod(this,
            [this, gen, reader, file, rawPath,
             coverImage = std::move(coverImage),
             cache     = std::move(cache),
             overview  = std::move(overview),
             wfLoaded]() mutable
            {
                if (m_loadGen != gen) {
                    delete reader;
                    return;
                }

                populateMetadataFromReader(*reader, rawPath, file);
                const double durationSec =
                    static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
                updateTrackDuration(durationSec);
                m_hasTrack = true;
                clearLoop();

                const bool hasDbAnalysis = hydrateLibraryStateForTrack(rawPath, durationSec);

                if (!coverImage.isNull() && m_coverProvider) {
                    m_coverProvider->setCoverImage(m_deckId, coverImage);
                    m_coverArtUrl = QString("image://coverart/%1?t=%2")
                                        .arg(m_deckId)
                                        .arg(QDateTime::currentMSecsSinceEpoch());
                    m_hasCoverArt = true;
                }

                if (wfLoaded) {
                    const int expected =
                        cache.totalExpected > 0 ? cache.totalExpected : cache.waveform.size();
                    m_trackData->setTotalExpected(expected);
                    m_trackData->replaceAllData(
                        std::move(cache.waveform), std::max(0.001f, cache.globalMaxPeak));
                    m_trackData->setRgbWaveformData(std::move(cache.rgb));
                    m_trackData->setOverviewRgbData(std::move(overview));
                }

                emit trackMetadataChanged();
                attachReaderToTransport(reader);
                emit trackLoaded();
                emit progressChanged();

                // Skip heavy analysis only when BOTH waveform cache and analysis metadata
                // (BPM/key/grid) are already available.
                if (!(wfLoaded && hasDbAnalysis))
                    m_analyzer->startAnalysis(rawPath, transportSource.getCurrentPosition());
            },
            Qt::QueuedConnection);
    }).detach();
}

void DjEngine::togglePlay()
{
    if (m_playRequested) {
        m_playRequested = false;
        if (transportSource.isPlaying()) {
            // Compute the interpolated visual position BEFORE stopping so we can
            // freeze the transport at exactly the position the waveform was showing.
            // This eliminates the visible jump when pressing pause.
            freezeTransportAt(getVisualPosition());
        }
    } else {
        m_playRequested = true;
        ensureTransportRunningForPlayIntent();
    }
        
    emit playingChanged();
}

void DjEngine::play()
{
    if (!m_playRequested)
        m_playRequested = true;

    ensureTransportRunningForPlayIntent();
    emit playingChanged();
}

void DjEngine::pause()
{
    if (!m_playRequested && !transportSource.isPlaying())
        return; // Already paused

    m_playRequested = false;

    if (transportSource.isPlaying()) {
        // Compute the interpolated visual position BEFORE stopping so we can
        // freeze the transport at exactly the position the waveform was showing.
        freezeTransportAt(getVisualPosition());
    }
    emit playingChanged();
}

void DjEngine::ensureTransportRunningForPlayIntent()
{
    if (!m_playRequested)
        return;

    if (m_isScrubbing || m_scratchReleaseActive || !m_hasTrack) {
        return;
    }

    if (deviceManager.getCurrentAudioDevice() == nullptr) {
        qWarning() << "[DjEngine] Play requested without active audio device; trying to recover";
        const juce::String initErr = initialiseWithTimeout(deviceManager, 0, 2, std::chrono::seconds(4));
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

void DjEngine::armSnapFromTransportPosition()
{
    setSnapAnchor(transportSource.getCurrentPosition(), true);
}

void DjEngine::freezeTransportAt(double positionSec)
{
    transportSource.stop();
    transportSource.setPosition(positionSec);
    m_snapValid = false;
    m_atomicPlayheadPos.store(positionSec, std::memory_order_relaxed);
}

void DjEngine::setPosition(float progress)
{
    double len = transportSource.getLengthInSeconds();
    if (len > 0.0) {
        const double newPos = progress * len;
        transportSource.setPosition(newPos);
        ensureTransportRunningForPlayIntent();
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(newPos);
    }
    emit progressChanged();
}

void DjEngine::advanceAbsoluteScrubFollower(double dtSec)
{
    if (!m_scratchAbsolutePositionControl)
        return;

    const double len = transportSource.getLengthInSeconds();
    if (len <= 0.0)
        return;

    const double targetPos = std::clamp(m_scratchAbsoluteTargetPosition, 0.0, len);
    const double prevPos = m_scrubHoldPosition;
    const double errorSec = targetPos - m_scrubHoldPosition;

    // Mass-spring-damper follower: slightly lagging, velocity-aware
    // and naturally braking before target to mimic platter inertia.
    const double accel = (errorSec * m_scratchAbsoluteFollowStiffness)
                       - (m_scratchAbsoluteFollowVelocity * m_scratchAbsoluteFollowDamping);
    m_scratchAbsoluteFollowVelocity += accel * dtSec;
    m_scratchAbsoluteFollowVelocity = std::clamp(
        m_scratchAbsoluteFollowVelocity,
        -m_scratchAbsoluteMaxFollowRate,
        m_scratchAbsoluteMaxFollowRate);

    double stepSec = m_scratchAbsoluteFollowVelocity * dtSec;
    if (std::abs(stepSec) > std::abs(errorSec))
        stepSec = errorSec;

    m_scrubHoldPosition = std::clamp(m_scrubHoldPosition + stepSec, 0.0, len);

    const double residualError = targetPos - m_scrubHoldPosition;
    if (std::abs(residualError) <= m_scratchAbsoluteSnapDistanceSec
        && std::abs(m_scratchAbsoluteFollowVelocity) <= m_scratchAbsoluteSnapVelocitySecPerSec) {
        m_scrubHoldPosition = targetPos;
        m_scratchAbsoluteFollowVelocity = 0.0;
    }

    const double movedSec = m_scrubHoldPosition - prevPos;
    m_scratchAccumulatedMoveSec += std::abs(movedSec);

    const double motionRate = (dtSec > 1e-6)
        ? std::clamp(movedSec / dtSec, -m_scratchMaxRate, m_scratchMaxRate)
        : 0.0;
    m_scratchTargetRate = motionRate;

    if (std::abs(targetPos - m_scrubHoldPosition) > m_scratchAbsoluteSnapDistanceSec
        || std::abs(motionRate) > 0.001) {
        m_lastScrubInputClock.restart();
    }
}

void DjEngine::updateScrubPlayheadAnchor()
{
    if (m_isScrubbing && m_scratchAbsolutePositionControl) {
        // Absolute drag mode: keep transport pinned to the grabbed target.
        // This avoids tiny left/right visual wobble between sparse input events.
        transportSource.setPosition(m_scrubHoldPosition);
        m_atomicPlayheadPos.store(m_scrubHoldPosition,
                                  std::memory_order_relaxed);
        m_snapPosition = m_scrubHoldPosition;
        return;
    }

    m_scrubHoldPosition = transportSource.getCurrentPosition();
    m_atomicPlayheadPos.store(m_scrubHoldPosition,
                              std::memory_order_relaxed);
    m_snapPosition = m_scrubHoldPosition;
}

void DjEngine::onTimer()
{
    if (m_isScrubbing || m_scratchReleaseActive) {
        double idleSec = 0.0;
        bool hasIdleSample = false;
        const double dtSec = m_scrubPhysicsClock.isValid()
            ? std::clamp(static_cast<double>(m_scrubPhysicsClock.nsecsElapsed()) * 1e-9,
                         0.001, 0.050)
            : 0.016;
        m_scrubPhysicsClock.restart();

        double targetRate = 0.0;
        double tau = m_scratchRateReleaseTauSec;
        if (m_isScrubbing) {
            advanceAbsoluteScrubFollower(dtSec);

            idleSec = m_lastScrubInputClock.isValid()
                ? static_cast<double>(m_lastScrubInputClock.nsecsElapsed()) * 1e-9
                : 1.0;
            hasIdleSample = true;

            targetRate = m_scratchAbsolutePositionControl
                ? m_scratchTargetRate
                : ((idleSec > m_scratchIdleTimeoutSec) ? 0.0 : m_scratchTargetRate);

            tau = (std::abs(targetRate) > std::abs(m_scratchSmoothedRate))
                ? m_scratchRateAttackTauSec
                : m_scratchRateReleaseTauSec;

            // Very slow hand motion needs stronger smoothing to avoid
            // "steppy"/digital sounding micro modulation.
            if (std::abs(targetRate) < 0.35)
                tau = std::max(tau, 0.080);
        } else {
            targetRate = m_scratchReleaseTargetRate;
            tau = (std::abs(targetRate) < 0.001)
                ? m_scratchReleaseToStopTauSec
                : m_scratchReleaseToPlayTauSec;
        }

        const double alpha = 1.0 - std::exp(-dtSec / std::max(0.001, tau));
        m_scratchSmoothedRate += (targetRate - m_scratchSmoothedRate) * alpha;

        if (std::abs(m_scratchSmoothedRate) >= m_scratchDirectionFlipThresholdRate)
            m_scratchDirectionSign = (m_scratchSmoothedRate < 0.0) ? -1.0 : 1.0;

        const double absRate = std::abs(m_scratchSmoothedRate);
        if (reverseWrapSource) {
            auto* reverseSource = static_cast<ReverseStreamAudioSource*>(reverseWrapSource.get());
            reverseSource->setReverse(m_scratchDirectionSign < 0.0);
        }

        if (mixerSource) {
            // Stronger coloration at very low platter speeds, fading out naturally
            // into clean playback at faster scratches.
            const double slowZone = std::clamp(1.0 - (absRate / 1.4), 0.0, 1.0);
            const float timbre = static_cast<float>(std::pow(slowZone, 1.25) * 0.85);
            mixerSource->setScratchTimbre(timbre);
        }

        if (resamplingSource) {
            // Lower floor while scrubbing preserves low-speed nuance and avoids
            // hard quantization to a fixed tiny speed.
            const double minRatio = m_isScrubbing ? 0.002 : 0.004;
            const double ratio = std::clamp(absRate, minRatio, m_scratchMaxRate);
            resamplingSource->setResamplingRatio(ratio);
        }

        // Playback hysteresis: avoid rapid start/stop toggling around zero.
        const double targetDistance = m_scratchAbsolutePositionControl
            ? std::abs(m_scratchAbsoluteTargetPosition - m_scrubHoldPosition)
            : 0.0;
        const bool absoluteMotionPending = m_isScrubbing
            && m_scratchAbsolutePositionControl
            && targetDistance > (m_scratchAbsoluteSnapDistanceSec * 1.5);
        const bool allowStopForIdleGrab = !m_isScrubbing
            || ((hasIdleSample && idleSec > 0.040) && !absoluteMotionPending);

        if (absRate < m_scratchControlStopThresholdRate && allowStopForIdleGrab) {
            if (transportSource.isPlaying())
                transportSource.stop();
        } else if ((absRate > m_scratchControlResumeThresholdRate || absoluteMotionPending)
                   && !transportSource.isPlaying()) {
            transportSource.start();
        }

        updateScrubPlayheadAnchor();
        m_snapTempoRatio = getTempoRatio();

        if (!m_isScrubbing && m_scratchReleaseActive
            && std::abs(m_scratchSmoothedRate - m_scratchReleaseTargetRate) <= m_scratchReleaseSettleThreshold) {
            m_scratchReleaseActive = false;
            m_scratchTargetRate = 0.0;
            m_scratchSmoothedRate = 0.0;
            m_scratchInputFilteredRate = 0.0;

            if (mixerSource)
                mixerSource->setScratchTimbre(0.0f);

            restorePostScrubPlaybackState();
            emit playingChanged();
        }

        emit progressChanged();
        emit vuLevelChanged();
        emit gainReductionChanged();
        return;
    }

    if (mixerSource)
        mixerSource->setScratchTimbre(0.0f);

    if (transportSource.isPlaying()) {
        // Store a fresh snapshot from the transport each control tick.
        // Interpolation happens only between these anchors.
        const double dtSec = m_snapValid
            ? std::clamp(static_cast<double>(m_snapClock.nsecsElapsed()) * 1e-9, 0.001, 0.050)
            : 0.004;

        const double measuredPos = transportSource.getCurrentPosition();
        m_snapPosition = measuredPos;
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
            }
        }

        m_snapClock.restart();
        m_snapValid = true;
        // Also update the atomic so QML FrameAnimation can poll it lock-free.
        m_atomicPlayheadPos.store(m_snapPosition, std::memory_order_relaxed);
        emit progressChanged();
    } else {
        m_snapValid = false;
        ensureTransportRunningForPlayIntent();
        // Keep atomic in sync with the stopped position.
        m_atomicPlayheadPos.store(transportSource.getCurrentPosition(),
                                   std::memory_order_relaxed);
    }
    // Phase-lock correction for sync followers (not during scratch).
    if (m_syncEnabled && !m_isSyncMaster && !m_isScrubbing && !m_scratchReleaseActive)
        updatePhaseCorrection();

    // Always emit VU level updates (30 Hz timer = nice for meters)
    emit vuLevelChanged();
    emit gainReductionChanged();
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
    return mixerSource ? mixerSource->m_clipDetected.load(std::memory_order_relaxed) : false;
}

float DjEngine::gainReduction() const
{
    // s_gainReduction is a static atomic shared across all instances
    return MixerDspSource::s_gainReduction.load(std::memory_order_relaxed);
}

QVariantList DjEngine::hotCues() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_hotCueSlots.size()));

    for (int i = 0; i < static_cast<int>(m_hotCueSlots.size()); ++i) {
        const auto& slot = m_hotCueSlots[static_cast<size_t>(i)];
        QVariantMap m;
        m.insert("index", i);
        m.insert("set", slot.set);
        m.insert("positionSec", slot.positionSec);
        m.insert("label", slot.label);
        m.insert("color", slot.color);
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
    for (int i = 0; i < static_cast<int>(m_hotCueSlots.size()); ++i) {
        auto& slot = m_hotCueSlots[static_cast<size_t>(i)];
        slot.set = false;
        slot.positionSec = 0.0;
        slot.label.clear();
        slot.color = defaultHotCueColor(i);
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

        auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
        slot.set = true;
        slot.positionSec = std::max(0.0, m.value("positionSec").toDouble());
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

    const auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    if (slot.set) {
        const QString label = slot.label.isEmpty()
            ? QStringLiteral("HOT CUE %1").arg(index + 1)
            : slot.label;
        m_libraryDb->upsertCuePoint(m_currentTrackId, index, slot.positionSec, label, slot.color);
    } else {
        m_libraryDb->deleteCuePoint(m_currentTrackId, index);
    }
}

void DjEngine::storeHotCue(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    slot.set = true;
    slot.positionSec = std::clamp(static_cast<double>(getVisualPosition()), 0.0, trackLen);
    if (slot.color.isEmpty())
        slot.color = defaultHotCueColor(index);
    if (slot.label.isEmpty())
        slot.label = QStringLiteral("HOT CUE %1").arg(index + 1);

    persistHotCueSlot(index);
    emit hotCuesChanged();
}

void DjEngine::triggerHotCue(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    const auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    if (!slot.set) {
        storeHotCue(index);
        return;
    }

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double pos = std::clamp(slot.positionSec, 0.0, trackLen);
    transportSource.setPosition(pos);
    ensureTransportRunningForPlayIntent();
    m_snapPosition = pos;
    m_snapTempoRatio = getTempoRatio();
    m_snapClock.restart();
    m_snapValid = true;
    m_atomicPlayheadPos.store(pos, std::memory_order_relaxed);
    if (m_analyzer && m_analyzer->isThreadRunning())
        m_analyzer->setSeekHint(pos);
    emit progressChanged();
}

void DjEngine::clearHotCue(int index)
{
    if (!isValidHotCueIndex(index))
        return;

    auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
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

    auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    slot.color = color;

    if (slot.set)
        persistHotCueSlot(index);

    emit hotCuesChanged();
}

void DjEngine::loadMainCueForCurrentTrack()
{
    m_mainCueSec = -1.0;

    if (!m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const double storedCue = m_libraryDb->mainCuePointForTrack(m_currentTrackId);
    m_mainCueSec = storedCue >= 0.0 ? storedCue : -1.0;
    emit mainCueChanged();
}

void DjEngine::persistMainCuePoint()
{
    if (!m_libraryDb || m_currentTrackId.isEmpty())
        return;
    m_libraryDb->upsertMainCuePoint(m_currentTrackId, m_mainCueSec);
}

void DjEngine::setLastAudioDeviceError(const QString& error)
{
    if (m_lastAudioDeviceError == error)
        return;
    m_lastAudioDeviceError = error;
    emit audioDeviceErrorChanged();
}

void DjEngine::cueButtonPress()
{
    if (!m_hasTrack)
        return;

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const bool wasPlaying = m_playRequested;

    if (wasPlaying) {
        // While playing, CUE should jump to the stored cue and continue playback.
        if (m_mainCueSec < 0.0) {
            m_mainCueSec = std::clamp(static_cast<double>(getVisualPosition()), 0.0, trackLen);
            persistMainCuePoint();
            emit mainCueChanged();
        }

        const double cuePos = std::clamp(m_mainCueSec, 0.0, trackLen);
        transportSource.setPosition(cuePos);
        setSnapAnchor(cuePos, true);
        if (m_analyzer && m_analyzer->isThreadRunning())
            m_analyzer->setSeekHint(cuePos);
        emit progressChanged();
        return;
    }

    // While paused, pressing CUE sets the cue point at current position and previews while held.
    const double cuePos = std::clamp(static_cast<double>(getVisualPosition()), 0.0, trackLen);
    m_mainCueSec = cuePos;
    persistMainCuePoint();
    emit mainCueChanged();

    transportSource.setPosition(cuePos);
    setSnapAnchor(cuePos, true);
    if (m_analyzer && m_analyzer->isThreadRunning())
        m_analyzer->setSeekHint(cuePos);

    m_mainCuePreviewActive = true;
    transportSource.start();
    emit playingChanged();
    emit progressChanged();
}

void DjEngine::cueButtonRelease()
{
    if (!m_mainCuePreviewActive)
        return;

    m_mainCuePreviewActive = false;

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double cuePos = std::clamp(m_mainCueSec >= 0.0 ? m_mainCueSec : 0.0, 0.0, trackLen);
    if (transportSource.isPlaying())
        transportSource.stop();
    transportSource.setPosition(cuePos);

    m_snapPosition = cuePos;
    m_snapClock.restart();
    m_snapValid = false;
    m_atomicPlayheadPos.store(cuePos, std::memory_order_relaxed);

    emit playingChanged();
    emit progressChanged();
}

void DjEngine::applyScratchNeutralRouting()
{
    if (timeStretchSource)
        timeStretchSource->setTempoRatio(1.0);
    if (resamplingSource)
        resamplingSource->setResamplingRatio(1.0);
    if (reverseWrapSource)
        static_cast<ReverseStreamAudioSource*>(reverseWrapSource.get())->setReverse(false);
}

void DjEngine::restorePostScrubPlaybackState()
{
    if (reverseWrapSource)
        static_cast<ReverseStreamAudioSource*>(reverseWrapSource.get())->setReverse(m_scrubSavedReverseState);
    if (resamplingSource)
        resamplingSource->setResamplingRatio(1.0);
    if (timeStretchSource)
        timeStretchSource->setTempoRatio(1.0);

    updateSpeedAndPitch();

    if (m_playRequested)
        transportSource.start();
    else
        transportSource.stop();

    m_scrubWasPlaying = false;
    m_snapTempoRatio = getTempoRatio();
    m_snapClock.restart();
    m_snapValid = true;
}

// ─── Scrub API ────────────────────────────────────────────────────────────────

void DjEngine::pauseForScrub()
{
    if (m_isScrubbing)
        return;

    m_phaseNudge  = 0.0;
    m_resyncBoost = false;

    bool wasPlayingBeforeGrab = m_playRequested || transportSource.isPlaying();
    if (m_scratchReleaseActive) {
        // If we re-grab while release glide is still active, preserve the
        // original pre-scratch intent instead of the temporary glide transport state.
        wasPlayingBeforeGrab = m_scrubWasPlaying;
    }

    m_scratchReleaseActive = false;
    m_scratchReleaseTargetRate = 0.0;
    m_scrubWasPlaying = wasPlayingBeforeGrab;
    m_isScrubbing = true;
    m_snapValid = false;
    m_scratchAbsolutePositionControl = false;

    m_scrubHoldPosition = transportSource.getCurrentPosition();
    m_scratchAbsoluteTargetPosition = m_scrubHoldPosition;
    m_scratchAbsoluteFollowVelocity = 0.0;
    m_scratchBaseRate = std::max(0.01, std::abs(getTempoRatio()));
    m_scratchAccumulatedMoveSec = 0.0;
    m_scratchTargetRate = 0.0;
    m_scratchSmoothedRate = 0.0;
    m_scratchInputFilteredRate = 0.0;
    m_scrubPhysicsClock.restart();
    m_lastScrubInputClock.restart();
    emit scrubbingChanged();

    // Scratch should run as pure speed=pitch behavior through a neutral
    // Rubber Band stage so routing and latency stay continuous.
    m_scrubSavedReverseState = m_isReverse;
    m_scratchDirectionSign = m_scrubSavedReverseState ? -1.0 : 1.0;
    applyScratchNeutralRouting();

    // Immediate touch brake: no scratch noise on plain click/hold.
    transportSource.stop();
}

void DjEngine::scrubBy(double pixelDelta)
{
    // Convert pixel delta → time delta.
    // The waveform maps (pixelsPerPoint × waveformPointsPerSecond) pixels to 1 second.
    // Dragging right  → positive pixelDelta → waveform moves right
    //                 → playhead moves BACKWARD (we "pull" the record left).
    // Dragging left   → negative pixelDelta → playhead moves FORWARD.
    // ∴ timeDelta = −pixelDelta / pixelsPerSecond
    if (m_pixelsPerSecond <= 0.0) return;

    // Match the rendered waveform scale used by ScrollingWaveformItem:
    // effectivePpp = waveformZoom / tempoRatio
    // => effective pixels/sec = m_pixelsPerSecond / tempoRatio.
    // This keeps scratch feel consistent when tempo fader changes.
    const double tempoRatio = std::max(0.01, std::abs(getTempoRatio()));
    const double effectivePixelsPerSecond = std::max(1.0, m_pixelsPerSecond / tempoRatio);

    double timeDelta = -pixelDelta / effectivePixelsPerSecond;
    scratchBySeconds(timeDelta);
}

void DjEngine::scratchBySeconds(double deltaSeconds)
{
    if (deltaSeconds == 0.0)
        return;

    const double requestedDelta = std::clamp(deltaSeconds,
                                             -m_scratchEventSpikeClampSec,
                                             m_scratchEventSpikeClampSec);
    const bool fineMove = std::abs(requestedDelta) <= m_scratchFineMoveThresholdSec;
    const double directStep = fineMove
        ? requestedDelta
        : std::clamp(requestedDelta,
                     -m_scratchDirectStepLimitSec,
                     m_scratchDirectStepLimitSec);

    const double len = transportSource.getLengthInSeconds();
    if (len <= 0.0)
        return;

    if (m_scratchReleaseActive)
        m_scratchReleaseActive = false;

    m_scratchAbsolutePositionControl = false;
    m_scratchAbsoluteTargetPosition = m_scrubHoldPosition;
    m_scratchAbsoluteFollowVelocity = 0.0;

    const double dtSecRaw = m_lastScrubInputClock.isValid()
        ? std::max(0.001, static_cast<double>(m_lastScrubInputClock.nsecsElapsed()) * 1e-9)
        : 0.008;
    const double rawRate = requestedDelta / dtSecRaw;
    const double baseRate = std::max(0.01, m_scratchBaseRate);
    const double absRaw = std::abs(rawRate);
    double shapedAbsRate = 0.0;

    if (absRaw <= baseRate) {
        // High precision near zero movement; cubic rise avoids twitching.
        const double t = std::clamp(absRaw / baseRate, 0.0, 1.0);
        shapedAbsRate = baseRate * (0.15 * t + 0.85 * t * t * t);
    } else {
        // Above base speed, accelerate more aggressively for fast swipes.
        shapedAbsRate = baseRate + (absRaw - baseRate) * 1.35;
    }

    if (fineMove)
        shapedAbsRate *= 0.72;

    const double instantaneousRate = std::copysign(
        std::clamp(shapedAbsRate, 0.0, m_scratchMaxRate),
        rawRate);
    pushScratchVelocityTick(instantaneousRate);

    if (m_isScrubbing && !transportSource.isPlaying() && std::abs(instantaneousRate) > 0.001)
        transportSource.start();

    const double currentPos = m_scrubHoldPosition;
    const double nextPos = std::clamp(currentPos + directStep, 0.0, len);
    transportSource.setPosition(nextPos);

    m_scratchAccumulatedMoveSec += std::abs(directStep);
    m_scrubHoldPosition = nextPos;
    m_lastScrubInputClock.restart();

    // Keep the atomic + snap in sync for all synced UIs.
    m_atomicPlayheadPos.store(m_scrubHoldPosition, std::memory_order_relaxed);
    m_snapPosition = m_scrubHoldPosition;
    m_scrubPhysicsClock.restart();
    emit progressChanged();
}

void DjEngine::setScrubPosition(double positionSeconds)
{
    if (!m_isScrubbing)
        return;

    const double len = transportSource.getLengthInSeconds();
    if (len <= 0.0)
        return;

    double nextPos = std::clamp(positionSeconds, 0.0, len);
    if (m_loopActive && m_loopOutSec > m_loopInSec) {
        const double loopStart = std::clamp(m_loopInSec, 0.0, len);
        const double loopEnd = std::clamp(m_loopOutSec, loopStart, len);
        const double loopLength = loopEnd - loopStart;

        if (loopLength > 0.0) {
            if (nextPos < loopStart) {
                const double wraps = std::ceil((loopStart - nextPos) / loopLength);
                nextPos += wraps * loopLength;
            } else if (nextPos > loopEnd) {
                const double wraps = std::ceil((nextPos - loopEnd) / loopLength);
                nextPos -= wraps * loopLength;
            }

            while (nextPos < loopStart)
                nextPos += loopLength;
            while (nextPos > loopEnd)
                nextPos -= loopLength;
        }
    }

    const double prevTargetPos = m_scratchAbsolutePositionControl
        ? m_scratchAbsoluteTargetPosition
        : m_scrubHoldPosition;
    const double deltaSec = nextPos - prevTargetPos;
    if (std::abs(deltaSec) <= 2e-5)
        return;

    // A large position jump while a loop is active means the QML wrapped the
    // scrub target across a loop boundary (Serato-style). Snap to the new
    // position immediately instead of deriving a bogus reverse velocity from
    // the jump delta — the next input event will supply the correct velocity.
    if (m_loopActive && m_loopOutSec > m_loopInSec) {
        const double halfLoop = (m_loopOutSec - m_loopInSec) * 0.5;
        if (std::abs(deltaSec) > halfLoop) {
            m_scratchReleaseActive = false;
            m_scratchAbsolutePositionControl = true;
            m_scratchAbsoluteTargetPosition = nextPos;
            m_scrubHoldPosition = nextPos;
            m_scratchAbsoluteFollowVelocity = 0.0;
            transportSource.setPosition(nextPos);
            m_atomicPlayheadPos.store(nextPos, std::memory_order_relaxed);
            m_lastScrubInputClock.restart();
            return;
        }
    }

    // Absolute grab mode with preserved scratch feel:
    // derive a velocity target from absolute pointer movement so scratching
    // remains audible and release inertia still works.
    m_scratchReleaseActive = false;
    m_scratchAbsolutePositionControl = true;
    m_scratchAbsoluteTargetPosition = nextPos;

    const double dtSecRaw = m_lastScrubInputClock.isValid()
        ? std::max(0.001, static_cast<double>(m_lastScrubInputClock.nsecsElapsed()) * 1e-9)
        : 0.008;
    const double rawRate = deltaSec / dtSecRaw;
    const double baseRate = std::max(0.01, m_scratchBaseRate);
    const double absRaw = std::abs(rawRate);

    double shapedAbsRate = 0.0;
    if (absRaw <= baseRate) {
        // Preserve fine control near zero without killing tactile response.
        const double t = std::clamp(absRaw / baseRate, 0.0, 1.0);
        shapedAbsRate = baseRate * (0.10 * t + 0.90 * t * t * t);
    } else {
        shapedAbsRate = baseRate + (absRaw - baseRate) * 1.25;
    }

    const double instantaneousRate = std::copysign(
        std::clamp(shapedAbsRate, 0.0, m_scratchMaxRate),
        rawRate);
    pushScratchVelocityTick(instantaneousRate);

    if (m_isScrubbing && !transportSource.isPlaying() && std::abs(instantaneousRate) > 0.001)
        transportSource.start();

    // Hand movement itself contributes to release intent, while actual travel
    // is accumulated in onTimer() via the spring follower.
    m_scratchAccumulatedMoveSec += std::abs(deltaSec) * 0.45;
}

void DjEngine::pushScratchVelocityTick(double velocityRate)
{
    const double rawTarget = std::clamp(velocityRate, -m_scratchMaxRate, m_scratchMaxRate);
    const double dtSec = m_lastScrubInputClock.isValid()
        ? std::clamp(static_cast<double>(m_lastScrubInputClock.nsecsElapsed()) * 1e-9,
                     0.001, 0.050)
        : 0.008;

    m_scratchInputFilteredRate += (rawTarget - m_scratchInputFilteredRate)
        * std::clamp(m_scratchInputRateFilterAlpha, 0.0, 1.0);

    const double maxStep = std::max(0.001, m_scratchInputRateSlewPerSec) * dtSec;
    const double desired = std::clamp(m_scratchInputFilteredRate, -m_scratchMaxRate, m_scratchMaxRate);
    const double delta = std::clamp(desired - m_scratchTargetRate, -maxStep, maxStep);
    m_scratchTargetRate = std::clamp(m_scratchTargetRate + delta,
                                     -m_scratchMaxRate,
                                     m_scratchMaxRate);
    m_lastScrubInputClock.restart();
}

void DjEngine::resumeAfterScrub()
{
    if (!m_isScrubbing)
        return;

    m_isScrubbing = false;
    m_scratchAbsolutePositionControl = false;
    m_scratchAbsoluteFollowVelocity = 0.0;
    m_scratchInputFilteredRate = m_scratchSmoothedRate;

    // Touch/hold without meaningful movement: resume immediately with no glide.
    if (m_scratchAccumulatedMoveSec < m_scratchInertiaMoveThresholdSec) {
        m_scratchReleaseActive = false;
        m_scratchReleaseTargetRate = 0.0;
        m_scratchTargetRate = 0.0;
        m_scratchSmoothedRate = 0.0;

        restorePostScrubPlaybackState();
        emit scrubbingChanged();
        emit playingChanged();
        return;
    }

    // Keep current fling velocity and glide back to normal transport speed.
    // If the deck was paused before scratch, glide to a full stop instead.
    constexpr double kMinReleaseKickRate = 0.06;
    if (std::abs(m_scratchSmoothedRate) < kMinReleaseKickRate) {
        const double seedDir = (std::abs(m_scratchTargetRate) > 0.001)
            ? std::copysign(1.0, m_scratchTargetRate)
            : m_scratchDirectionSign;
        m_scratchSmoothedRate = seedDir * kMinReleaseKickRate;
    }
    m_scratchSmoothedRate = std::clamp(m_scratchSmoothedRate, -m_scratchMaxRate, m_scratchMaxRate);
    const double releaseBaseRate = std::max(0.01, std::abs(getTempoRatio()));
    const double releaseDirection = m_scrubSavedReverseState ? -1.0 : 1.0;
    m_scratchReleaseTargetRate = m_scrubWasPlaying ? (releaseDirection * releaseBaseRate) : 0.0;
    m_scratchReleaseActive = true;
    m_lastScrubInputClock.restart();
    m_scrubPhysicsClock.restart();

    emit scrubbingChanged();
}

void DjEngine::setDownbeatAtCurrentPosition()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed()) return;

    double anchorSec     = static_cast<double>(getVisualPosition());
    double trackLengthSec = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLengthSec <= 0.0) return;

    // Delegate rebuild + emit beatgridChanged() to TrackData.
    m_trackData->shiftBeatgridToDownbeat(anchorSec, trackLengthSec);

    // Persist immediately so manual beatgrid edits survive app restarts.
    persistCurrentAnalysisToLibrary();
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
    emit tempoChanged();   // update BPM display in UI
}

// ──────────────────────────────────────────────────────────────────────────────

void DjEngine::setMasterVolume(float v) {
    MixerDspSource::s_masterVolume.store(std::clamp(v, 0.0f, 1.5f),
                                         std::memory_order_relaxed);
}

void DjEngine::setAntiClip(bool enabled) {
    MixerDspSource::s_antiClipEnabled.store(enabled, std::memory_order_relaxed);
}

// ──────────────────────────────────────────────────────────────────────────────

double DjEngine::getBeatPhase() const
{
    if (!m_trackData) return 0.0;
    const double bpm = m_trackData->getBpm();
    if (bpm <= 0.0) return 0.0;

    const double pos    = getPosition();
    const double nomLen = 60.0 / bpm;   // nominal beat length from BPM
    const auto&  grid   = m_trackData->getBeatGrid();

    if (grid.size() >= 2) {
        // Find the last beat marker at or before the current position.
        const auto it = std::upper_bound(grid.begin(), grid.end(), pos,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });

        const auto prev = (it != grid.begin()) ? std::prev(it) : grid.begin();
        const double prevSec = prev->positionSec;

        // Beat length: use distance to next grid marker when available.
        double beatLen = nomLen;
        if (std::next(prev) != grid.end()) {
            const double candidate = std::next(prev)->positionSec - prevSec;
            if (candidate > 0.01)
                beatLen = candidate;
        }

        return std::clamp((pos - prevSec) / beatLen, 0.0, 0.9999);
    }

    // No usable beat grid — phase from BPM alone.
    return std::fmod(pos / nomLen, 1.0);
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

    // Compute actual beat length at current position.
    double beatLen = 60.0 / bpm;
    const auto& grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        const double pos = getPosition();
        const auto it = std::upper_bound(grid.begin(), grid.end(), pos,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        if (it != grid.begin()) {
            const auto prev = std::prev(it);
            if (std::next(prev) != grid.end()) {
                const double candidate = std::next(prev)->positionSec - prev->positionSec;
                if (candidate > 0.01)
                    beatLen = candidate;
            }
        }
    }

    const double seekOffset = diff * beatLen;
    const double newPos = std::max(0.0, getPosition() + seekOffset);
    transportSource.setPosition(newPos);
    m_phaseNudge = 0.0;
    armSnapFromTransportPosition();
}

void DjEngine::updateSpeedAndPitch()
{
    double speedMultiplier = 1.0 + ((m_tempoPercent + m_phaseNudge) / 100.0);

    // Keep one tempo authority (resampler) so fader response is identical
    // with and without keylock.
    if (resamplingSource)
        resamplingSource->setResamplingRatio(speedMultiplier);

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

    if (m_isScrubbing || m_scratchReleaseActive) {
        m_scratchBaseRate = std::max(0.01, std::abs(getTempoRatio()));
        if (m_scratchReleaseActive && m_scrubWasPlaying) {
            const double releaseDirection = m_scrubSavedReverseState ? -1.0 : 1.0;
            m_scratchReleaseTargetRate = releaseDirection * m_scratchBaseRate;
        }
    }

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

    // Actual beat length at current position.
    double beatLen = 60.0 / bpm;
    const auto& grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        const double pos = getPosition();
        const auto it = std::upper_bound(grid.begin(), grid.end(), pos,
            [](double v, const TrackData::BeatMarker& m) { return v < m.positionSec; });
        if (it != grid.begin()) {
            const auto prev = std::prev(it);
            if (std::next(prev) != grid.end()) {
                const double candidate = std::next(prev)->positionSec - prev->positionSec;
                if (candidate > 0.01)
                    beatLen = candidate;
            }
        }
    }

    // Seek 85% of the phase error immediately (barely audible for errors up to half a beat),
    // then let the boosted P-controller handle the remaining 15% in ~2 seconds.
    const double seekOffset = diff * beatLen * 0.85;
    const double newPos = std::max(0.0, getPosition() + seekOffset);
    transportSource.setPosition(newPos);
    m_phaseNudge  = 0.0;
    m_resyncBoost = true;
    armSnapFromTransportPosition();
    updatePhaseCorrection();
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

    const auto grid = m_trackData->getBeatGrid();
    if (!grid.empty()) {
        double best = grid.front().positionSec;
        double bestDist = std::abs(best - sec);
        for (const auto& m : grid) {
            const double d = std::abs(m.positionSec - sec);
            if (d < bestDist) {
                best = m.positionSec;
                bestDist = d;
            }
        }
        return best;
    }

    const double bpm = m_trackData->getBpm();
    const double sr = m_trackData->getSampleRate();
    if (bpm <= 0.0 || sr <= 0.0)
        return sec;

    const double beatDur = 60.0 / bpm;
    const double firstBeat = static_cast<double>(m_trackData->getFirstBeatSample()) / sr;
    const double beatIndex = std::round((sec - firstBeat) / beatDur);
    return firstBeat + beatIndex * beatDur;
}

double DjEngine::beatDurationAround(double sec) const
{
    if (!m_trackData)
        return 0.5;

    const auto grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        int nearest = 0;
        double bestDist = std::abs(grid[0].positionSec - sec);
        for (int i = 1; i < static_cast<int>(grid.size()); ++i) {
            const double d = std::abs(grid[static_cast<size_t>(i)].positionSec - sec);
            if (d < bestDist) {
                bestDist = d;
                nearest = i;
            }
        }

        if (nearest + 1 < static_cast<int>(grid.size())) {
            const double d = grid[static_cast<size_t>(nearest + 1)].positionSec
                           - grid[static_cast<size_t>(nearest)].positionSec;
            if (d > 1e-3)
                return d;
        }
        if (nearest > 0) {
            const double d = grid[static_cast<size_t>(nearest)].positionSec
                           - grid[static_cast<size_t>(nearest - 1)].positionSec;
            if (d > 1e-3)
                return d;
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

    double start = std::clamp(startSec, 0.0, trackLen);
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

    m_loopInSec = std::clamp(pos, 0.0, trackLen);
    m_loopInSet = true;

    if (m_loopActive && m_loopOutSec <= m_loopInSec) {
        m_loopOutSec = std::min(trackLen, m_loopInSec + beatDurationAround(m_loopInSec));
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
    outPos = std::clamp(outPos, 0.0, trackLen);

    // If no IN point is set yet, create a sensible default one-beat loop ending at OUT.
    if (!m_loopInSet) {
        const double beatDurAtOut = beatDurationAround(outPos);
        if (beatDurAtOut <= 1e-4)
            return;
        m_loopInSec = std::clamp(outPos - beatDurAtOut, 0.0, outPos);
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
    if (m_loopActive && std::abs(m_loopLengthBeats - 4.0) < 0.1) {
        clearLoop();
        return;
    }
    startLoopAt(static_cast<double>(getVisualPosition()), 4.0);
}

void DjEngine::toggleLoopThreeQuarter()
{
    // 3/4 loop = three quarters of ONE beat.
    if (m_loopActive && std::abs(m_loopLengthBeats - 0.75) < 0.06) {
        clearLoop();
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

void DjEngine::applyLoopRangeToAudioSource()
{
    if (!reverseWrapSource || !m_loopActive || m_loopOutSec <= m_loopInSec)
        return;

    if (m_isReverse) {
        clearLoopRangeOnAudioSource();
        return;
    }

    auto* reverseSource = static_cast<ReverseStreamAudioSource*>(reverseWrapSource.get());
    if (!reverseSource)
        return;

    const double sr = (m_loadedTrackSampleRate > 1.0)
        ? m_loadedTrackSampleRate
        : (m_trackData ? m_trackData->getSampleRate() : 44100.0);

    const juce::int64 loopInSample = static_cast<juce::int64>(std::llround(m_loopInSec * sr));
    const juce::int64 loopOutSample = static_cast<juce::int64>(std::llround(m_loopOutSec * sr));

    reverseSource->setLoopRangeSamples(loopInSample, loopOutSample, sr);
}

void DjEngine::clearLoopRangeOnAudioSource()
{
    if (!reverseWrapSource)
        return;
    auto* reverseSource = static_cast<ReverseStreamAudioSource*>(reverseWrapSource.get());
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
        static_cast<ReverseStreamAudioSource*>(reverseWrapSource.get())->setReverse(on);
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
