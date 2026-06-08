#include "MidiControllerManager.h"

#include "DjEngine.h"
#include "ParameterStore.h"
#include "SettingsManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QtGlobal>
#include <QUrl>
#include <QVariantMap>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <exception>

namespace {
const juce::String kAllMidiInputsIdentifier("__all_midi_inputs__");
const QString kBuiltInFlx10ControllerName = QStringLiteral("DDJ-FLX10");
const QString kBuiltInFlx10MappingFile = QStringLiteral("DDJ-FLX10.brockdj.xml");
const QString kBuiltInFlx10MappingLabel = QStringLiteral("Built-in: DDJ-FLX10");
const QString kBuiltInFlx10MappingResource = QStringLiteral(":/controllers/mappings/midi/DDJ-FLX10.brockdj.xml");
// FLX10 hardware jog wheels report ~1500 relative ticks per vinyl revolution
// (Mixxx scratchEnable uses the same constant). One revolution ≈ 1.8 s @ 33⅓ RPM.
constexpr double kFlx10ScratchIntervalsPerRevolution = 1500.0;
constexpr double kVinylRpm = 33.0 + 1.0 / 3.0;

double flx10ScratchDeltaSec(double ticks)
{
    return ticks * (60.0 / kVinylRpm) / kFlx10ScratchIntervalsPerRevolution;
}

bool isBuiltInFlx10Mapping(const QString& mappingName)
{
    return mappingName == kBuiltInFlx10MappingLabel
        || mappingName == kBuiltInFlx10MappingFile;
}

bool openDirectoryInFileManager(const QString& path)
{
    if (path.isEmpty())
        return false;

    QDir dir(path);
    if (!dir.exists() && !dir.mkpath("."))
        return false;

    const QString nativePath = QDir::toNativeSeparators(dir.absolutePath());

#if defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("open"), { nativePath }))
        return true;
#elif defined(Q_OS_WIN)
    if (QProcess::startDetached(QStringLiteral("explorer.exe"), { nativePath }))
        return true;
#elif defined(Q_OS_UNIX)
    if (QProcess::startDetached(QStringLiteral("xdg-open"), { nativePath }))
        return true;
#endif

    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

bool isHotCueParam(const QString& paramId)
{
    return paramId.startsWith(QStringLiteral("deckA_hotcue"))
        || paramId.startsWith(QStringLiteral("deckB_hotcue"));
}

bool isPerformancePadParam(const QString& paramId)
{
    return paramId.startsWith(QStringLiteral("deckA_pad"))
        || paramId.startsWith(QStringLiteral("deckB_pad"));
}

bool parsePerformancePadParam(const QString& paramId, QChar& deck, int& index, bool& clear)
{
    QString suffix;
    if (paramId.startsWith(QStringLiteral("deckA_pad"))) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(QStringLiteral("deckA_pad").size());
    } else if (paramId.startsWith(QStringLiteral("deckB_pad"))) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(QStringLiteral("deckB_pad").size());
    } else {
        return false;
    }

    clear = suffix.endsWith(QStringLiteral("_clear"));
    if (clear)
        suffix.chop(QStringLiteral("_clear").size());

    bool ok = false;
    index = suffix.toInt(&ok) - 1;
    return ok && index >= 0 && index < 8;
}

bool parseDirectPadParam(const QString& paramId, const QString& stem, QChar& deck, int& index)
{
    QString suffix;
    const QString deckAStem = QStringLiteral("deckA_") + stem;
    const QString deckBStem = QStringLiteral("deckB_") + stem;
    if (paramId.startsWith(deckAStem)) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(deckAStem.size());
    } else if (paramId.startsWith(deckBStem)) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(deckBStem.size());
    } else {
        return false;
    }

    bool ok = false;
    index = suffix.toInt(&ok) - 1;
    return ok && index >= 0 && index < 8;
}

bool parsePadModeParam(const QString& paramId, QChar& deck, MidiPadMode& mode)
{
    QString suffix;
    if (paramId.startsWith(QStringLiteral("deckA_pad_mode_"))) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(QStringLiteral("deckA_pad_mode_").size());
    } else if (paramId.startsWith(QStringLiteral("deckB_pad_mode_"))) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(QStringLiteral("deckB_pad_mode_").size());
    } else {
        return false;
    }

    if (suffix == QStringLiteral("hotcue")) {
        mode = MidiPadMode::HotCue;
        return true;
    }
    if (suffix == QStringLiteral("padfx")) {
        mode = MidiPadMode::PadFx;
        return true;
    }
    if (suffix == QStringLiteral("beatjump")) {
        mode = MidiPadMode::BeatJump;
        return true;
    }
    return false;
}

bool parseDeckButtonParam(const QString& paramId, const QString& suffix, QChar& deck)
{
    if (paramId == QStringLiteral("deckA_") + suffix) {
        deck = QLatin1Char('A');
        return true;
    }
    if (paramId == QStringLiteral("deckB_") + suffix) {
        deck = QLatin1Char('B');
        return true;
    }
    return false;
}

bool parseDeckFxSlotParam(const QString& paramId, QChar& deck, int& slot)
{
    QString suffix;
    if (paramId.startsWith(QStringLiteral("deckA_fx_slot"))) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(QStringLiteral("deckA_fx_slot").size());
    } else if (paramId.startsWith(QStringLiteral("deckB_fx_slot"))) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(QStringLiteral("deckB_fx_slot").size());
    } else {
        return false;
    }

    bool ok = false;
    slot = suffix.toInt(&ok);
    return ok && slot >= 1 && slot <= 3;
}

bool parseBeatFxSelectParam(const QString& paramId, int& position)
{
    if (!paramId.startsWith(QStringLiteral("beat_fx_select_")))
        return false;

    bool ok = false;
    position = paramId.mid(QStringLiteral("beat_fx_select_").size()).toInt(&ok);
    return ok && position >= 1 && position <= 14;
}

EffectType beatFxTypeForPosition(int position)
{
    static constexpr EffectType kTypes[14] = {
        EffectType::Echo,
        EffectType::LowCutEcho,
        EffectType::MtDelay,
        EffectType::Spiral,
        EffectType::Reverb,
        EffectType::Trans,
        EffectType::Flanger,
        EffectType::Phaser,
        EffectType::Bitcrusher,
        EffectType::PitchShifter,
        EffectType::Stretch,
        EffectType::EnigmaJet,
        EffectType::Roll,
        EffectType::SlipRoll
    };

    return kTypes[static_cast<size_t>(std::clamp(position, 1, 14) - 1)];
}

bool parseHotCueParam(const QString& paramId, QChar& deck, int& index, bool& clear)
{
    QString suffix;
    if (paramId.startsWith(QStringLiteral("deckA_hotcue"))) {
        deck = QLatin1Char('A');
        suffix = paramId.mid(QStringLiteral("deckA_hotcue").size());
    } else if (paramId.startsWith(QStringLiteral("deckB_hotcue"))) {
        deck = QLatin1Char('B');
        suffix = paramId.mid(QStringLiteral("deckB_hotcue").size());
    } else {
        return false;
    }

    clear = suffix.endsWith(QStringLiteral("_clear"));
    if (clear)
        suffix.chop(QStringLiteral("_clear").size());

    bool ok = false;
    index = suffix.toInt(&ok) - 1;
    return ok && index >= 0 && index < 8;
}

QString toQString(const juce::String& value)
{
    return QString::fromStdString(value.toStdString());
}

QString hexByte(int value)
{
    return QStringLiteral("%1")
        .arg(value & 0xff, 2, 16, QLatin1Char('0'))
        .toUpper();
}

int clampMidi7bit(int value)
{
    return std::max(0, std::min(127, value));
}

bool containsIdentifier(const std::vector<juce::String>& ids, const juce::String& needle)
{
    return std::ranges::find(ids, needle) != ids.end();
}

MidiInteractionType defaultInteractionTypeForParam(const QString& paramId)
{
    if (paramId == QStringLiteral("deckA_cue")
        || paramId == QStringLiteral("deckB_cue")
        || paramId == QStringLiteral("deckA_jog_touch")
        || paramId == QStringLiteral("deckB_jog_touch")
        || paramId == QStringLiteral("deckA_shift")
        || paramId == QStringLiteral("deckB_shift")
        || (isPerformancePadParam(paramId)
            && !paramId.contains(QStringLiteral("_pad_mode_"))
            && !paramId.endsWith(QStringLiteral("_clear")))) {
        return MidiInteractionType::Momentary;
    }

    if (paramId == QStringLiteral("deckA_play")
        || paramId == QStringLiteral("deckB_play")
        || isHotCueParam(paramId)
        || (isPerformancePadParam(paramId) && paramId.endsWith(QStringLiteral("_clear")))
        || paramId.startsWith(QStringLiteral("deckA_pad_mode_"))
        || paramId.startsWith(QStringLiteral("deckB_pad_mode_"))
        || paramId.startsWith(QStringLiteral("deckA_loop_"))
        || paramId.startsWith(QStringLiteral("deckB_loop_"))
        || paramId.startsWith(QStringLiteral("deckA_beatjump_"))
        || paramId.startsWith(QStringLiteral("deckB_beatjump_"))
        || paramId.startsWith(QStringLiteral("deckA_beatjump_pad"))
        || paramId.startsWith(QStringLiteral("deckB_beatjump_pad"))
        || paramId.startsWith(QStringLiteral("deckA_padfx_pad"))
        || paramId.startsWith(QStringLiteral("deckB_padfx_pad"))
        || paramId.startsWith(QStringLiteral("deckA_fx_slot"))
        || paramId.startsWith(QStringLiteral("deckB_fx_slot"))
        || paramId == QStringLiteral("deckA_beat_sync")
        || paramId == QStringLiteral("deckB_beat_sync")
        || paramId == QStringLiteral("deckA_beatsync")
        || paramId == QStringLiteral("deckB_beatsync")
        || paramId == QStringLiteral("deckA_key_sync")
        || paramId == QStringLiteral("deckB_key_sync")
        || paramId == QStringLiteral("deckA_keylock")
        || paramId == QStringLiteral("deckB_keylock")
        || paramId == QStringLiteral("deckA_slip")
        || paramId == QStringLiteral("deckB_slip")
        || paramId == QStringLiteral("deckA_tempo_reset")
        || paramId == QStringLiteral("deckB_tempo_reset")
        || paramId == QStringLiteral("deckA_rate_reset")
        || paramId == QStringLiteral("deckB_rate_reset")
        || paramId.startsWith(QStringLiteral("beat_fx_"))
        || paramId == QStringLiteral("deckA_headphone_cue")
        || paramId == QStringLiteral("deckB_headphone_cue")
        || paramId == QStringLiteral("master_cue")
        || paramId.startsWith(QStringLiteral("library_load_"))
        || paramId == QStringLiteral("library_back")
        || paramId == QStringLiteral("library_expand")
        || paramId == QStringLiteral("library_collapse")
        || paramId == QStringLiteral("library_playlist_next")
        || paramId == QStringLiteral("library_playlist_prev")) {
        return MidiInteractionType::Toggle;
    }

    if (paramId.endsWith(QStringLiteral("_jog_move"))
        || paramId.endsWith(QStringLiteral("_jog_nudge"))
        || paramId.endsWith(QStringLiteral("_jog_scratch"))
        || paramId == QStringLiteral("library_browse")) {
        return MidiInteractionType::EncoderRelative;
    }

    if (paramId == QStringLiteral("deckA_vol")
        || paramId == QStringLiteral("deckB_vol")
        || paramId == QStringLiteral("crossfader")
        || paramId == QStringLiteral("headphone_mix")
        || paramId == QStringLiteral("deckA_tempo")
        || paramId == QStringLiteral("deckB_tempo")) {
        return MidiInteractionType::Fader;
    }

    return MidiInteractionType::EncoderAbsolute;
}

QString interactionTypeToString(MidiInteractionType type)
{
    switch (type) {
    case MidiInteractionType::Momentary:
        return QStringLiteral("momentary");
    case MidiInteractionType::Toggle:
        return QStringLiteral("toggle");
    case MidiInteractionType::EncoderRelative:
        return QStringLiteral("encoder-relative");
    case MidiInteractionType::EncoderAbsolute:
        return QStringLiteral("encoder-absolute");
    case MidiInteractionType::Fader:
        return QStringLiteral("fader");
    }
    return QStringLiteral("encoder-absolute");
}

MidiInteractionType interactionTypeFromString(const QString& rawValue,
                                              const QString& paramId)
{
    const QString value = rawValue.trimmed().toLower();
    if (value == QStringLiteral("momentary"))
        return MidiInteractionType::Momentary;
    if (value == QStringLiteral("toggle"))
        return MidiInteractionType::Toggle;
    if (value == QStringLiteral("encoder-relative"))
        return MidiInteractionType::EncoderRelative;
    if (value == QStringLiteral("encoder-absolute"))
        return MidiInteractionType::EncoderAbsolute;
    if (value == QStringLiteral("fader"))
        return MidiInteractionType::Fader;
    return defaultInteractionTypeForParam(paramId);
}

bool isButtonInteraction(MidiInteractionType type)
{
    return type == MidiInteractionType::Momentary
        || type == MidiInteractionType::Toggle;
}

bool shouldAlwaysDispatch(MidiInteractionType type)
{
    return type == MidiInteractionType::Momentary
        || type == MidiInteractionType::Toggle
        || type == MidiInteractionType::EncoderRelative;
}

bool isRelativeInteraction(MidiInteractionType type)
{
    return type == MidiInteractionType::EncoderRelative;
}

bool isFlx10JogRelativeParam(const QString& paramId)
{
    return paramId.endsWith(QStringLiteral("_jog_move"))
        || paramId.endsWith(QStringLiteral("_jog_nudge"))
        || paramId.endsWith(QStringLiteral("_jog_scratch"));
}

float decodeRelativeCcValue(int rawValue, const QString& paramId)
{
    const int raw = clampMidi7bit(rawValue);
    if (isFlx10JogRelativeParam(paramId)) {
        // FLX10 jog CCs are relative around 0x40 for the full 7-bit range:
        // 0x40 = neutral, 0x41..0x7F = +1..+63, 0x3F..0x01 = -1..-63.
        // Treating 0x7F as two's-complement -1 makes fast spins reverse direction.
        return static_cast<float>(raw - 0x40);
    }

    // Support the two common relative CC encodings for generic encoders:
    // - binary offset: 64 = neutral, 65..96 = +1..+32, 63..32 = -1..-32
    // - two's complement: 1..63 = +1..+63, 65..127 = -63..-1
    if (raw >= 32 && raw <= 96)
        return static_cast<float>(raw - 64);
    return static_cast<float>(raw < 64 ? raw : raw - 128);
}

float decodeWrappedAbsoluteDelta(int previousRaw, int currentRaw)
{
    int delta = clampMidi7bit(currentRaw) - clampMidi7bit(previousRaw);
    if (delta > 64)
        delta -= 128;
    else if (delta < -64)
        delta += 128;
    return static_cast<float>(delta);
}

MidiMappingEntry makeMappingEntry(const QString& paramId)
{
    return { paramId, defaultInteractionTypeForParam(paramId) };
}

MidiMappingEntry makeMappingEntry(const QString& paramId,
                                  MidiInteractionType type)
{
    return { paramId, type };
}

QString midiControlLabel(int msgId)
{
    if (msgId >= 10000) {
        const int remainder = msgId - 10000;
        const int channel = remainder / 2000;
        const int sub = remainder % 2000;
        if (sub == 1500)
            return QStringLiteral("Ch%1 Pitch").arg(channel + 1);
        if (sub >= 1000)
            return QStringLiteral("Ch%1 CC %2").arg(channel + 1).arg(sub - 1000);
        return QStringLiteral("Ch%1 Note %2").arg(channel + 1).arg(sub);
    }

    if (msgId == 1500)
        return QStringLiteral("Pitch");
    if (msgId >= 1000)
        return QStringLiteral("CC %1").arg(msgId - 1000);
    return QStringLiteral("Note %1").arg(msgId);
}

int indexOfIdentifier(const std::vector<juce::String>& ids, const juce::String& needle)
{
    const auto it = std::ranges::find(ids, needle);
    if (it == ids.end())
        return -1;
    return static_cast<int>(std::distance(ids.begin(), it));
}

QString midiMatchKey(QString value)
{
    value = value.toLower();
    value.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QString());
    return value;
}

bool looksLikeFlx10Name(const QString& value)
{
    const QString key = midiMatchKey(value);
    return key.contains(QStringLiteral("flx10"))
        || key.contains(QStringLiteral("ddjflx10"));
}

template <typename DeviceList>
void appendMidiDeviceNames(const DeviceList& devices,
                           std::vector<juce::String>& outIdentifiers,
                           QStringList& outNames)
{
    outIdentifiers.reserve(outIdentifiers.size() + devices.size());
    for (const auto& dev : devices) {
        outIdentifiers.push_back(dev.identifier);
        outNames.push_back(toQString(dev.name));
    }
}
}

MidiControllerManager::MidiControllerManager(ParameterStore* store, QObject* parent)
    : QObject(parent),
      m_parameterStore(store),
      m_midiFeedback(this)
{
    if (m_parameterStore) {
        connect(m_parameterStore, &ParameterStore::parameterChanged,
                this, &MidiControllerManager::onParameterChanged);
    }

    m_midiDeviceListConnection = juce::MidiDeviceListConnection::make([this]
    {
        QMetaObject::invokeMethod(this, [this]()
        {
            if (m_shutdownComplete.load(std::memory_order_acquire))
                return;
            refreshMidiAndMappings();
        }, Qt::QueuedConnection);
    });

    m_midiFeedback.setMidiSender([this](uint8_t status, uint8_t data1, uint8_t data2, const QString& type)
    {
        return sendMidiShort(status, data1, data2, type);
    });

    for (QTimer* timer : {&m_jogAReleaseTimer, &m_jogBReleaseTimer}) {
        timer->setSingleShot(true);
        timer->setTimerType(Qt::PreciseTimer);
        timer->setInterval(120);
    }
    connect(&m_jogAReleaseTimer, &QTimer::timeout, this, [this] {
        m_jogAReleasedRecently = false;
        if (!m_jogATouched && m_deckA && m_deckA->isScrubbing())
            m_deckA->resumeAfterScrub();
    });
    connect(&m_jogBReleaseTimer, &QTimer::timeout, this, [this] {
        m_jogBReleasedRecently = false;
        if (!m_jogBTouched && m_deckB && m_deckB->isScrubbing())
            m_deckB->resumeAfterScrub();
    });

    m_selectedController = SettingsManager::getInstance().getSelectedController();
    m_selectedMappingFile = SettingsManager::getInstance().getSelectedMappingFile();

    refreshMidiAndMappings();
    restoreSavedDeviceSelections();

    if (!m_selectedMappingFile.isEmpty())
        loadBrockDjXmlMapping(m_selectedMappingFile);
    else
        loadNativeMappingIfExists();

    autoOpenFlx10MidiOutputIfNeeded();

    m_startupRefreshTimer.setSingleShot(true);
    connect(&m_startupRefreshTimer, &QTimer::timeout, this, [this]()
    {
        if (m_shutdownComplete.load(std::memory_order_acquire))
            return;
        refreshMidiAndMappings();
        autoOpenFlx10MidiOutputIfNeeded();
        // Avoid reopening stale saved identifiers repeatedly on startup.
        // Users can still select a device explicitly in settings.
    });
    m_startupRefreshTimer.start(750);
}

void MidiControllerManager::shutdown()
{
    if (m_shutdownComplete.exchange(true, std::memory_order_acq_rel))
        return;

    // Drop JUCE device-list callbacks before touching Qt timers — queued refresh
    // lambdas must not run while I/O is being torn down.
    m_midiDeviceListConnection = juce::MidiDeviceListConnection{};

    // Stop LED feedback timers/sender first — avoid QTimer::stop() during aboutToQuit.
    m_midiFeedback.prepareForShutdown();

    // Disconnect timers instead of stop() — QTimer::stop() has crashed during
    // aboutToQuit when a timeout handler is still unwinding on macOS.
    QObject::disconnect(&m_startupRefreshTimer, nullptr, this, nullptr);
    QObject::disconnect(&m_jogAReleaseTimer, nullptr, this, nullptr);
    QObject::disconnect(&m_jogBReleaseTimer, nullptr, this, nullptr);

    if (m_parameterStore)
        QObject::disconnect(m_parameterStore, nullptr, this, nullptr);

    if (m_deckA)
        QObject::disconnect(m_deckA, nullptr, this, nullptr);
    if (m_deckB && m_deckB != m_deckA)
        QObject::disconnect(m_deckB, nullptr, this, nullptr);
    m_deckA = nullptr;
    m_deckB = nullptr;

    // Feedback already torn down in prepareForShutdown(); skip stopFlx10OutputSession
    // here — it would touch Qt state while the scene graph may still be winding down.

    for (auto& input : m_midiInputs) {
        if (input)
            input->stop();
    }
    m_midiInputs.clear();

#if defined(Q_OS_LINUX)
    if (m_alsaMidiOutput)
        m_alsaMidiOutput.reset();
#endif

    if (m_midiOutput)
        m_midiOutput.reset();

    stopAlsaInputMonitor();
}

MidiControllerManager::~MidiControllerManager()
{
    if (!m_shutdownComplete.load(std::memory_order_acquire))
        shutdown();
}

QStringList MidiControllerManager::getAvailableMidiInputDevices()
{
    return m_availableInputDeviceNames;
}

QStringList MidiControllerManager::getAvailableMidiOutputDevices()
{
    return m_availableOutputDeviceNames;
}

QStringList MidiControllerManager::getAvailableMidiDevices()
{
    return m_availableControllerDeviceNames;
}

void MidiControllerManager::refreshMidiDeviceCache()
{
    m_availableInputDeviceIdentifiers.clear();
    m_availableInputDeviceNames.clear();

    m_availableInputDeviceIdentifiers.push_back(kAllMidiInputsIdentifier);
    m_availableInputDeviceNames.push_back(QStringLiteral("All MIDI Inputs"));

    appendMidiDeviceNames(juce::MidiInput::getAvailableDevices(),
                          m_availableInputDeviceIdentifiers,
                          m_availableInputDeviceNames);

    m_availableOutputDeviceIdentifiers.clear();
    m_availableOutputDeviceNames.clear();

    appendMidiDeviceNames(juce::MidiOutput::getAvailableDevices(),
                          m_availableOutputDeviceIdentifiers,
                          m_availableOutputDeviceNames);

#if defined(Q_OS_LINUX)
    // JUCE does not reliably expose every ALSA sequencer port for larger DJ
    // controllers. Keep the direct ALSA ports visible too, so devices like the
    // DDJ-FLX10 can be selected by the port that actually emits button events.
    populateFromAlsaFallback();
#endif

    rebuildControllerDeviceCache();

    logAvailableMidiPorts();
}

void MidiControllerManager::rebuildControllerDeviceCache()
{
    m_availableControllerDeviceIdentifiers.clear();
    m_availableControllerInputIndexes.clear();
    m_availableControllerDeviceNames.clear();

    bool flx10Added = false;
    for (int i = 0; i < static_cast<int>(m_availableInputDeviceIdentifiers.size()); ++i) {
        const juce::String& identifier = m_availableInputDeviceIdentifiers[static_cast<size_t>(i)];
        if (identifier == kAllMidiInputsIdentifier)
            continue;

        const QString name = m_availableInputDeviceNames.value(i);
        if (looksLikeFlx10Name(name)) {
            if (flx10Added)
                continue;

            int preferredInput = i;
            for (int candidate = 0; candidate < static_cast<int>(m_availableInputDeviceIdentifiers.size()); ++candidate) {
                if (isPseudoAlsaIdentifier(m_availableInputDeviceIdentifiers[static_cast<size_t>(candidate)])
                        && looksLikeFlx10Name(m_availableInputDeviceNames.value(candidate))) {
                    preferredInput = candidate;
                    break;
                }
            }

            m_availableControllerDeviceIdentifiers.push_back(m_availableInputDeviceIdentifiers[static_cast<size_t>(preferredInput)]);
            m_availableControllerInputIndexes.push_back(preferredInput);
            m_availableControllerDeviceNames.push_back(QStringLiteral("DDJ-FLX10 (auto MIDI I/O)"));
            flx10Added = true;
            continue;
        }

        m_availableControllerDeviceIdentifiers.push_back(identifier);
        m_availableControllerInputIndexes.push_back(i);
        m_availableControllerDeviceNames.push_back(name);
    }
}

void MidiControllerManager::populateFromAlsaFallback()
{
#if defined(Q_OS_LINUX)
    snd_seq_t* seq = nullptr;
    const int openResult = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (openResult < 0) {
        qWarning() << "[MIDI] ALSA fallback failed:" << QString::fromUtf8(snd_strerror(openResult));
        return;
    }

    snd_seq_client_info_t* clientInfo = nullptr;
    snd_seq_port_info_t* portInfo = nullptr;
    snd_seq_client_info_alloca(&clientInfo);
    snd_seq_port_info_alloca(&portInfo);

    snd_seq_client_info_set_client(clientInfo, -1);
    while (snd_seq_query_next_client(seq, clientInfo) >= 0) {
        const int client = snd_seq_client_info_get_client(clientInfo);
        const QString clientName = QString::fromUtf8(snd_seq_client_info_get_name(clientInfo)).trimmed();
        const QString lowerClient = clientName.toLower();

        if (lowerClient == "system"
            || lowerClient == "midi through"
            || lowerClient.startsWith("pipewire")
            || lowerClient.contains("brockdj")
            || lowerClient.contains("aseqdump"))
            continue;

        snd_seq_port_info_set_client(portInfo, client);
        snd_seq_port_info_set_port(portInfo, -1);
        while (snd_seq_query_next_port(seq, portInfo) >= 0) {
            const int port = snd_seq_port_info_get_port(portInfo);
            const QString portName = QString::fromUtf8(snd_seq_port_info_get_name(portInfo)).trimmed();
            const unsigned int caps = snd_seq_port_info_get_capability(portInfo);
            const QString label = QString("%1:%2 - %3 (%4)")
                                      .arg(client)
                                      .arg(port)
                                      .arg(portName.isEmpty() ? clientName : portName)
                                      .arg(clientName);

            const bool readable = (caps & SND_SEQ_PORT_CAP_READ) != 0
                && (caps & SND_SEQ_PORT_CAP_SUBS_READ) != 0;
            const bool writable = (caps & SND_SEQ_PORT_CAP_WRITE) != 0
                && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE) != 0;

            if (readable) {
                const juce::String inputIdentifier("alsa:" + juce::String(client) + ":" + juce::String(port));
                m_availableInputDeviceIdentifiers.push_back(inputIdentifier);
                m_availableInputDeviceNames.push_back(label);
            }

            if (writable) {
                const juce::String outputIdentifier("alsa-out:" + juce::String(client) + ":" + juce::String(port));
                m_availableOutputDeviceIdentifiers.push_back(outputIdentifier);
                m_availableOutputDeviceNames.push_back(label + QStringLiteral(" (ALSA)"));
            }
        }
    }

    snd_seq_close(seq);
#endif
}

bool MidiControllerManager::isPseudoAlsaIdentifier(const juce::String& identifier) const
{
#if defined(Q_OS_LINUX)
    return identifier.startsWith("alsa:");
#else
    Q_UNUSED(identifier);
    return false;
#endif
}

bool MidiControllerManager::isPseudoAlsaOutputIdentifier(const juce::String& identifier) const
{
#if defined(Q_OS_LINUX)
    return identifier.startsWith("alsa-out:");
#else
    Q_UNUSED(identifier);
    return false;
#endif
}

void MidiControllerManager::startAlsaInputMonitor(const juce::String& pseudoIdentifier)
{
#if defined(Q_OS_LINUX)
    if (!isPseudoAlsaIdentifier(pseudoIdentifier))
        return;

    const QString id = toQString(pseudoIdentifier);
    const QStringList parts = id.split(':');
    if (parts.size() != 3) {
        qWarning() << "[MIDI] Invalid ALSA pseudo identifier:" << id;
        return;
    }

    const QString port = parts.at(1) + ":" + parts.at(2);
    auto monitor = std::make_unique<QProcess>(this);
    QProcess* process = monitor.get();
    m_alsaMonitorBuffers[process].clear();

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]()
    {
        if (!process)
            return;

        QString& buffer = m_alsaMonitorBuffers[process];
        buffer.append(QString::fromUtf8(process->readAllStandardOutput()));

        int newline = buffer.indexOf('\n');
        while (newline >= 0) {
            const QString line = buffer.left(newline).trimmed();
            buffer.remove(0, newline + 1);

            if (!line.isEmpty()) {
                const QRegularExpression numRegex(R"((\d+))");
                const auto matches = numRegex.globalMatch(line);
                QList<int> numbers;
                auto it = matches;
                while (it.hasNext())
                    numbers.push_back(it.next().captured(1).toInt());

                // aseqdump line format: "... Ch, controller/note N, value/velocity V"
                // Numbers from end: [..., channel, controllerOrNote, value]
                auto decodeTriple = [&numbers](int& ch, int& first, int& second) -> bool
                {
                    if (numbers.size() < 3)
                        return false;
                    ch     = numbers.at(numbers.size() - 3);
                    first  = numbers.at(numbers.size() - 2);
                    second = numbers.at(numbers.size() - 1);
                    return true;
                };

                // aseqdump formats are not consistent across ALSA/PipeWire paths:
                // some report MIDI channels as 0-based, others as 1-based. Prefer
                // an exact mapped ID, then the 1-based-adjusted candidate where safe.
                auto resolveMsgId = [this](int channelAware, int oneBasedAdjusted, int legacy) -> int
                {
                    if (m_midiToParam.count(channelAware))
                        return channelAware;
                    if (oneBasedAdjusted >= 0 && m_midiToParam.count(oneBasedAdjusted))
                        return oneBasedAdjusted;
                    if (m_midiToParam.count(legacy))
                        return legacy;
                    return channelAware; // learning: store channel-aware
                };

                int ch = 0, a = 0, b = 0;

                if (line.contains("Control change", Qt::CaseInsensitive) && decodeTriple(ch, a, b)) {
                    const int cc              = clampMidi7bit(a);
                    const int chClamped       = std::max(0, std::min(15, ch));
                    const int channelAwareMsgId = 10000 + chClamped * 2000 + 1000 + cc;
                    const int oneBasedMsgId     = ch > 0 ? 10000 + std::min(15, ch - 1) * 2000 + 1000 + cc : -1;
                    const int legacyMsgId       = cc + 1000;
                    const int msgId             = resolveMsgId(channelAwareMsgId, oneBasedMsgId, legacyMsgId);
                    const auto it               = m_midiToParam.find(msgId);
                    float value;
                    if (it != m_midiToParam.end() && isRelativeInteraction(it->second.interactionType)) {
                        value = decodeRelativeCcValue(b, it->second.paramId);
                    } else {
                        value = clampMidi7bit(b) / 127.0f;
                    }
                    processDecodedMidiEvent(msgId, value, false);
                } else if (line.contains("Note on", Qt::CaseInsensitive) ||
                           line.contains("Note off", Qt::CaseInsensitive)) {
                    const bool isOff = line.contains("Note off", Qt::CaseInsensitive);

                    // --- Format A: "Note on 0, note 11, velocity 127"  ← common aseqdump ---
                    // Some ALSA drivers omit the velocity on NoteOff. Extract channel/note
                    // from the named fields first; the source port prefix also contains
                    // numbers, so a generic "last three numbers" fallback can swap note/vel.
                    static const QRegularExpression noteTextRx(
                        R"((?:Note\s+on|Note\s+off)\s+(\d+)\s*,\s*note\s+(\d+)(?:\s*,\s*velocity\s+(\d+))?)",
                        QRegularExpression::CaseInsensitiveOption);

                    // --- Format B: "Chan N ... (statusByte note vel)"  ← newer aseqdump ---
                    // Extract channel from "Chan N" (1-based) and note/vel from the 3-number
                    // parenthetical. The first number in the paren is the MIDI status byte:
                    // either treat "Chan N" for channel OR decode status & 0x0F.
                    static const QRegularExpression noteParenRx(
                        R"((?:Chan|Channel)\s+(\d+).*?\((\d+)\s+(\d+)\s+(\d+)\))",
                        QRegularExpression::CaseInsensitiveOption);

                    // --- Format C: "Chan N, Note on/off NOTE vel VELOCITY"  ← older aseqdump ---
                    static const QRegularExpression noteVerboseRx(
                        R"((?:Chan|Channel)\s+(\d+).*?Note\s+(?:on|off)[^\d]*(\d+)(?:.*?(?:velocity|vel)[^\d]*(\d+))?)",
                        QRegularExpression::CaseInsensitiveOption);

                    int ch0  = -1;
                    int note = -1;
                    int vel  = 0;

                    const auto nmText = noteTextRx.match(line);
                    if (nmText.hasMatch()) {
                        ch0  = std::max(0, std::min(15, nmText.captured(1).toInt()));
                        note = clampMidi7bit(nmText.captured(2).toInt());
                        vel  = nmText.captured(3).isEmpty() ? 0 : clampMidi7bit(nmText.captured(3).toInt());
                    } else {
                        const auto nmA = noteParenRx.match(line);
                        if (nmA.hasMatch()) {
                            ch0  = std::max(0, std::min(15, nmA.captured(1).toInt() - 1));
                            note = clampMidi7bit(nmA.captured(3).toInt());
                            vel  = clampMidi7bit(nmA.captured(4).toInt());
                        } else {
                            // Format C: get note from note-name number or last available digit,
                            // velocity from "velocity N" or "vel N" suffix.
                            // Extract channel from Chan/Channel N
                            static const QRegularExpression chanRx(
                                R"((?:Chan|Channel)\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
                            const auto chanM = chanRx.match(line);
                            if (chanM.hasMatch())
                                ch0 = std::max(0, std::min(15, chanM.captured(1).toInt() - 1));

                            const auto nmB = noteVerboseRx.match(line);
                            if (nmB.hasMatch()) {
                                ch0 = std::max(0, std::min(15, nmB.captured(1).toInt() - 1));
                                note = clampMidi7bit(nmB.captured(2).toInt());
                                vel = nmB.captured(3).isEmpty() ? 0 : clampMidi7bit(nmB.captured(3).toInt());
                            } else if (decodeTriple(ch, a, b)) {
                                // Last resort only. It can be wrong for some aseqdump NoteOff
                                // lines because the source port prefix contributes numbers.
                                ch0  = std::max(0, std::min(15, ch));
                                note = clampMidi7bit(a);
                                vel  = clampMidi7bit(b);
                            }
                        }
                    }

                    if (ch0 >= 0 && note >= 0) {
                        const int channelAwareMsgId = 10000 + ch0 * 2000 + note;
                        // Note channels are controller-action dense on the FLX10. A one-based
                        // fallback can turn unmapped pad notes into Ch7 library-load buttons,
                        // so notes must be exact or legacy only.
                        const int msgId             = resolveMsgId(channelAwareMsgId, -1, note);
                        const bool zeroVelocity     = isOff || (vel == 0);
                        qDebug() << "[MIDI ALSA]" << (isOff ? "NoteOff" : "NoteOn")
                                 << "ch0:" << ch0 << "note:" << note << "vel:" << vel
                                 << "msgId:" << msgId
                                 << "raw:" << line;
                        processDecodedMidiEvent(msgId, zeroVelocity ? 0.0f : vel / 127.0f, zeroVelocity);
                    }
                } else if ((line.contains("Pitchbend", Qt::CaseInsensitive) ||
                            line.contains("Pitch bend", Qt::CaseInsensitive)) &&
                           decodeTriple(ch, a, b)) {
                    // aseqdump: "Pitchbend  <ch>, value <signed>" — b is raw +8192 offset
                    // decodeTriple gives last 3 numbers; for negative aseqdump values the
                    // sign is stripped by the digit regex, so use the channel-only 2-number
                    // form and parse signed value directly.
                    static const QRegularExpression pbRx(R"((?:pitchbend|pitch\s+bend)\s+(\d+),\s*value\s+(-?\d+))",
                                                         QRegularExpression::CaseInsensitiveOption);
                    const auto pbMatch = pbRx.match(line);
                    if (pbMatch.hasMatch()) {
                        const int pbCh  = pbMatch.captured(1).toInt();
                        const int pbRaw = pbMatch.captured(2).toInt(); // -8192..+8191
                        const int channelAwareMsgId = 10000 + std::max(0, std::min(15, pbCh)) * 2000 + 1500;
                        const int msgId = resolveMsgId(channelAwareMsgId, -1, 1500);
                        const float value = static_cast<float>(pbRaw + 8192) / 16383.0f;
                        processDecodedMidiEvent(msgId, value, false);
                    }
                }
            }

            newline = buffer.indexOf('\n');
        }
    });

    connect(process, &QProcess::errorOccurred, this, [port](QProcess::ProcessError error)
    {
        qWarning() << "[MIDI] aseqdump error on" << port << "error:" << static_cast<int>(error);
    });

    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, port](int exitCode, QProcess::ExitStatus status)
    {
        qWarning() << "[MIDI] aseqdump finished on" << port
                   << "exitCode:" << exitCode
                   << "status:" << static_cast<int>(status);
        m_alsaMonitorBuffers.erase(process);
    });

    connect(process, &QProcess::readyReadStandardError, this, [process]()
    {
        if (!process)
            return;
        const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (!err.isEmpty())
            qWarning() << "[MIDI] aseqdump stderr:" << err;
    });

    process->start("aseqdump", {"-p", port});
    if (!process->waitForStarted(1200)) {
        qWarning() << "[MIDI] Could not start aseqdump for" << port;
        m_alsaMonitorBuffers.erase(process);
        return;
    }

    qInfo() << "[MIDI] Started ALSA input monitor on" << port;
    m_alsaInputMonitors.push_back(std::move(monitor));

#else
    Q_UNUSED(pseudoIdentifier);
#endif
}

void MidiControllerManager::stopAlsaInputMonitor()
{
#if defined(Q_OS_LINUX)
    if (m_alsaInputMonitors.empty())
        return;

    for (auto& monitor : m_alsaInputMonitors) {
        if (!monitor)
            continue;
        if (monitor->state() != QProcess::NotRunning) {
            monitor->terminate();
            if (!monitor->waitForFinished(400))
                monitor->kill();
        }
    }

    m_alsaInputMonitors.clear();
    m_alsaMonitorBuffers.clear();
#endif
}

void MidiControllerManager::openMidiInputByIdentifier(const juce::String& identifier)
{
    stopAlsaInputMonitor();

    for (auto& input : m_midiInputs) {
        if (input)
            input->stop();
    }
    m_midiInputs.clear();

    if (identifier.isEmpty())
        return;

    if (identifier == kAllMidiInputsIdentifier) {
        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& dev : devices) {
            auto input = juce::MidiInput::openDevice(dev.identifier, this);
            if (!input) {
                qWarning() << "[MIDI] Failed to open input:" << toQString(dev.identifier);
                continue;
            }

            input->start();
            qDebug() << "[MIDI] Opened input:" << toQString(dev.name)
                     << "id:" << toQString(dev.identifier);
            m_midiInputs.push_back(std::move(input));
        }

        if (m_midiInputs.empty())
            qWarning() << "[MIDI] All MIDI Inputs selected, but no JUCE inputs could be opened";
        return;
    }

    if (isPseudoAlsaIdentifier(identifier)) {
        const int selectedIndex = indexOfIdentifier(m_availableInputDeviceIdentifiers, identifier);
        const QString selectedName = (selectedIndex >= 0 && selectedIndex < m_availableInputDeviceNames.size())
            ? m_availableInputDeviceNames.at(selectedIndex)
            : QString();
        const bool flx10Context = looksLikeFlx10Name(selectedName)
            || normalizeControllerKeyFromXmlBase(getSelectedController()) == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
            || isBuiltInFlx10Mapping(getSelectedMapping());

        if (flx10Context) {
            int started = 0;
            for (int i = 0; i < static_cast<int>(m_availableInputDeviceIdentifiers.size()); ++i) {
                if (!isPseudoAlsaIdentifier(m_availableInputDeviceIdentifiers[static_cast<size_t>(i)]))
                    continue;
                if (!looksLikeFlx10Name(m_availableInputDeviceNames.at(i)))
                    continue;

                startAlsaInputMonitor(m_availableInputDeviceIdentifiers[static_cast<size_t>(i)]);
                ++started;
            }

            if (started > 0) {
                qInfo() << "[MIDI] FLX10 ALSA input monitors active:" << started;
                return;
            }
        }

        startAlsaInputMonitor(identifier);
        return;
    }

    auto input = juce::MidiInput::openDevice(identifier, this);
    if (!input) {
        qWarning() << "[MIDI] Failed to open input:" << toQString(identifier);
        return;
    }

    input->start();
    m_midiInputs.push_back(std::move(input));
}

void MidiControllerManager::openMidiOutputByIdentifier(const juce::String& identifier)
{
    stopFlx10OutputSession();

    if (m_midiOutput)
        m_midiOutput.reset();
#if defined(Q_OS_LINUX)
    if (m_alsaMidiOutput)
        m_alsaMidiOutput.reset();
#endif

    m_selectedMidiOutputIdentifier = {};
    m_selectedMidiOutputName.clear();
    m_selectedMidiOutputIndex = -1;
    m_flx10RawLedTestRun = false;
    m_lastMidiShortValues.clear();

    if (identifier.isEmpty() || isPseudoAlsaIdentifier(identifier))
        return;

    const int outputIndex = indexOfIdentifier(m_availableOutputDeviceIdentifiers, identifier);
    const QString outputName = (outputIndex >= 0 && outputIndex < m_availableOutputDeviceNames.size())
        ? m_availableOutputDeviceNames.at(outputIndex)
        : toQString(identifier);

#if defined(Q_OS_LINUX)
    if (isPseudoAlsaOutputIdentifier(identifier)) {
        auto output = std::make_unique<AlsaMidiOutput>();
        QString errorMessage;
        if (!output->open(toQString(identifier), &errorMessage)) {
            qWarning() << "[MIDI OUT] Failed to open ALSA output:" << outputName
                       << "id:" << toQString(identifier)
                       << "index:" << outputIndex
                       << "error:" << errorMessage;
            return;
        }

        m_selectedMidiOutputIdentifier = identifier;
        m_selectedMidiOutputName = outputName;
        m_selectedMidiOutputIndex = outputIndex;
        m_alsaMidiOutput = std::move(output);
        qInfo().noquote() << QString("FLX10 MIDI OUT port: \"%1\", index=%2")
                                 .arg(m_selectedMidiOutputName)
                                 .arg(m_selectedMidiOutputIndex);
        startFlx10OutputSession();
        refreshAllDeckLeds();
        return;
    }
#endif

    auto output = juce::MidiOutput::openDevice(identifier);
    if (!output) {
        qWarning() << "[MIDI OUT] Failed to open output:" << outputName
                   << "id:" << toQString(identifier)
                   << "index:" << outputIndex;
        return;
    }

    m_selectedMidiOutputIdentifier = identifier;
    m_selectedMidiOutputName = outputName;
    m_selectedMidiOutputIndex = outputIndex;
    m_midiOutput = std::move(output);
    qInfo().noquote() << QString("FLX10 MIDI OUT port: \"%1\", index=%2")
                             .arg(m_selectedMidiOutputName)
                             .arg(m_selectedMidiOutputIndex);
    startFlx10OutputSession();
    refreshAllDeckLeds();
}

int MidiControllerManager::findMatchingMidiOutputIndexForInput(int inputIndex) const
{
    if (m_availableOutputDeviceIdentifiers.empty())
        return -1;

    const bool flx10Context = normalizeControllerKeyFromXmlBase(getSelectedController())
            == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
        || isBuiltInFlx10Mapping(getSelectedMapping());
    const QString inputName = (inputIndex >= 0 && inputIndex < m_availableInputDeviceNames.size())
        ? m_availableInputDeviceNames.at(inputIndex)
        : QString();
    const QString inputKey = midiMatchKey(inputName);

    if (flx10Context || looksLikeFlx10Name(inputName)) {
        for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
            if (looksLikeFlx10Name(m_availableOutputDeviceNames.at(i))
                    && isPseudoAlsaOutputIdentifier(m_availableOutputDeviceIdentifiers[static_cast<size_t>(i)]))
                return i;
        }

        for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
            if (looksLikeFlx10Name(m_availableOutputDeviceNames.at(i)))
                return i;
        }
    }

    if (!inputKey.isEmpty()) {
        for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
            const QString outputKey = midiMatchKey(m_availableOutputDeviceNames.at(i));
            if (!outputKey.isEmpty()
                    && (outputKey.contains(inputKey) || inputKey.contains(outputKey)))
                return i;
        }
    }

    const int ordinalOutputIndex = (inputIndex > 0) ? inputIndex - 1 : -1;
    if (ordinalOutputIndex >= 0
            && ordinalOutputIndex < static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
        return ordinalOutputIndex;

    return -1;
}

int MidiControllerManager::findMidiOutputIndexByName(const QString& nameOrIdentifier) const
{
    const QString needle = nameOrIdentifier.trimmed();
    if (needle.isEmpty())
        return -1;

    for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
        if (m_availableOutputDeviceNames.at(i).compare(needle, Qt::CaseInsensitive) == 0)
            return i;
    }

    for (int i = 0; i < static_cast<int>(m_availableOutputDeviceIdentifiers.size()); ++i) {
        if (toQString(m_availableOutputDeviceIdentifiers[static_cast<size_t>(i)]).compare(needle, Qt::CaseInsensitive) == 0)
            return i;
    }

    const QString needleKey = midiMatchKey(needle);
    if (needleKey.isEmpty())
        return -1;

    for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
        const QString outputKey = midiMatchKey(m_availableOutputDeviceNames.at(i));
        if (outputKey.contains(needleKey) || needleKey.contains(outputKey))
            return i;
    }

    return -1;
}

void MidiControllerManager::logAvailableMidiPorts() const
{
    qInfo() << "Available MIDI INPUT ports:";
    for (int i = 0; i < m_availableInputDeviceNames.size(); ++i) {
        const QString id = (i >= 0 && i < static_cast<int>(m_availableInputDeviceIdentifiers.size()))
            ? toQString(m_availableInputDeviceIdentifiers[static_cast<size_t>(i)])
            : QString();
        qInfo().noquote() << QString("[%1] %2  id=\"%3\"")
                                 .arg(i)
                                 .arg(m_availableInputDeviceNames.at(i))
                                 .arg(id);
    }

    qInfo() << "Available MIDI OUTPUT ports:";
    for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
        const QString id = (i >= 0 && i < static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
            ? toQString(m_availableOutputDeviceIdentifiers[static_cast<size_t>(i)])
            : QString();
        qInfo().noquote() << QString("[%1] %2  id=\"%3\"")
                                 .arg(i)
                                 .arg(m_availableOutputDeviceNames.at(i))
                                 .arg(id);
    }
}

bool MidiControllerManager::shouldUseFlx10Feedback() const
{
    if (normalizeControllerKeyFromXmlBase(getSelectedController())
            == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
        || isBuiltInFlx10Mapping(getSelectedMapping())) {
        return true;
    }

    const QString selectedOutput = SettingsManager::getInstance().getMidiOutputIdentifier();
    const juce::String selectedId = juce::String::fromUTF8(selectedOutput.toUtf8().constData());
    const int index = indexOfIdentifier(m_availableOutputDeviceIdentifiers, selectedId);
    if (index >= 0 && index < m_availableOutputDeviceNames.size())
        return looksLikeFlx10Name(m_availableOutputDeviceNames.at(index));

    return looksLikeFlx10Name(selectedOutput);
}

void MidiControllerManager::startFlx10OutputSession()
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen) {
        m_midiFeedback.setEnabled(false);
        return;
    }

    if (!shouldUseFlx10Feedback()) {
        if (m_midiFeedback.isEnabled())
            m_midiFeedback.clearAll();
        m_midiFeedback.setEnabled(false);
        return;
    }

    m_midiFeedback.setEnabled(true);
    if (!m_flx10RawLedTestRun && qEnvironmentVariableIntValue("BROCKDJ_FLX10_LED_TEST") != 0) {
        m_flx10RawLedTestRun = true;
        m_midiFeedback.testRawLedOutput();
    }
}

void MidiControllerManager::stopFlx10OutputSession()
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (outputOpen && m_midiFeedback.isEnabled())
        m_midiFeedback.clearAll();
    m_midiFeedback.setEnabled(false);
}

void MidiControllerManager::restoreSavedDeviceSelections()
{
    const bool flx10Context = normalizeControllerKeyFromXmlBase(getSelectedController())
            == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
        || isBuiltInFlx10Mapping(getSelectedMapping());
    const auto inputId = SettingsManager::getInstance().getMidiInputIdentifier();
    juce::String savedInput = inputId.isEmpty()
        ? kAllMidiInputsIdentifier
        : juce::String::fromUTF8(inputId.toUtf8().constData());

    if (flx10Context && (inputId.isEmpty() || savedInput == kAllMidiInputsIdentifier)) {
        for (int i = 0; i < static_cast<int>(m_availableInputDeviceIdentifiers.size()); ++i) {
            const auto& identifier = m_availableInputDeviceIdentifiers[static_cast<size_t>(i)];
            if (isPseudoAlsaIdentifier(identifier) && looksLikeFlx10Name(m_availableInputDeviceNames.at(i))) {
                savedInput = identifier;
                SettingsManager::getInstance().setMidiInputIdentifier(toQString(identifier));
                qInfo() << "[MIDI] Auto-selected FLX10 ALSA input"
                        << m_availableInputDeviceNames.at(i)
                        << "index:" << i;
                break;
            }
        }
    }

    if (containsIdentifier(m_availableInputDeviceIdentifiers, savedInput))
        openMidiInputByIdentifier(savedInput);
    else
        openMidiInputByIdentifier(kAllMidiInputsIdentifier);

    const auto outputId = SettingsManager::getInstance().getMidiOutputIdentifier();
    const QByteArray envOutputName = qgetenv("BROCKDJ_MIDI_OUT");
    if (!envOutputName.isEmpty()) {
        const QString requestedOutput = QString::fromUtf8(envOutputName).trimmed();
        const int outputIndex = findMidiOutputIndexByName(requestedOutput);
        if (outputIndex >= 0 && outputIndex < static_cast<int>(m_availableOutputDeviceIdentifiers.size())) {
            const juce::String identifier = m_availableOutputDeviceIdentifiers[static_cast<size_t>(outputIndex)];
            SettingsManager::getInstance().setMidiOutputIdentifier(toQString(identifier));
            qInfo() << "[MIDI OUT] BROCKDJ_MIDI_OUT selected output" << requestedOutput
                    << "->" << m_availableOutputDeviceNames.at(outputIndex)
                    << "index:" << outputIndex;
            openMidiOutputByIdentifier(identifier);
            return;
        }

        qWarning() << "[MIDI OUT] BROCKDJ_MIDI_OUT did not match any output port:" << requestedOutput;
    }

    if (!outputId.isEmpty()) {
        const juce::String savedOutput = juce::String::fromUTF8(outputId.toUtf8().constData());
        if (containsIdentifier(m_availableOutputDeviceIdentifiers, savedOutput))
            openMidiOutputByIdentifier(savedOutput);
    }

    autoOpenFlx10MidiOutputIfNeeded();
}

bool MidiControllerManager::autoOpenFlx10MidiOutputIfNeeded()
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (outputOpen || m_availableOutputDeviceIdentifiers.empty())
        return false;

    const bool flx10Context = normalizeControllerKeyFromXmlBase(getSelectedController())
            == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName)
        || isBuiltInFlx10Mapping(getSelectedMapping());

    int outputIndex = -1;
    if (flx10Context) {
        for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
            if (looksLikeFlx10Name(m_availableOutputDeviceNames.at(i))
                    && isPseudoAlsaOutputIdentifier(m_availableOutputDeviceIdentifiers[static_cast<size_t>(i)])) {
                outputIndex = i;
                break;
            }
        }

        if (outputIndex < 0) {
            for (int i = 0; i < m_availableOutputDeviceNames.size(); ++i) {
                if (looksLikeFlx10Name(m_availableOutputDeviceNames.at(i))) {
                    outputIndex = i;
                    break;
                }
            }
        }
    }

    if (outputIndex < 0)
        return false;

    const juce::String identifier = m_availableOutputDeviceIdentifiers[static_cast<size_t>(outputIndex)];
    SettingsManager::getInstance().setMidiOutputIdentifier(toQString(identifier));
    qInfo() << "[MIDI OUT] Auto-selected FLX10 output"
            << m_availableOutputDeviceNames.at(outputIndex)
            << "index:" << outputIndex;
    openMidiOutputByIdentifier(identifier);
    return true;
}

void MidiControllerManager::selectMidiInputDevice(int index)
{
    if (index < 0 || index >= static_cast<int>(m_availableInputDeviceIdentifiers.size()))
        return;

    const auto identifier = m_availableInputDeviceIdentifiers[static_cast<size_t>(index)];
    openMidiInputByIdentifier(identifier);
    SettingsManager::getInstance().setMidiInputIdentifier(toQString(identifier));
}

void MidiControllerManager::selectMidiOutputDevice(int index)
{
    if (index < 0 || index >= static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
        return;

    const auto identifier = m_availableOutputDeviceIdentifiers[static_cast<size_t>(index)];
    SettingsManager::getInstance().setMidiOutputIdentifier(toQString(identifier));
    openMidiOutputByIdentifier(identifier);
}

void MidiControllerManager::selectMidiDevice(int index)
{
    if (index < 0 || index >= static_cast<int>(m_availableControllerInputIndexes.size()))
        return;

    const int inputIndex = m_availableControllerInputIndexes[static_cast<size_t>(index)];
    selectMidiInputDevice(inputIndex);

    const int outputIndex = findMatchingMidiOutputIndexForInput(inputIndex);
    if (outputIndex >= 0 && outputIndex < static_cast<int>(m_availableOutputDeviceIdentifiers.size()))
        selectMidiOutputDevice(outputIndex);

    emit midiDevicesUpdated();
}

int MidiControllerManager::getSelectedMidiInputIndex() const
{
    const QString selected = SettingsManager::getInstance().getMidiInputIdentifier();
    if (selected.isEmpty())
        return 0;

    const juce::String selectedId = juce::String::fromUTF8(selected.toUtf8().constData());
    return indexOfIdentifier(m_availableInputDeviceIdentifiers, selectedId);
}

int MidiControllerManager::getSelectedMidiOutputIndex() const
{
    const QString selected = SettingsManager::getInstance().getMidiOutputIdentifier();
    if (selected.isEmpty())
        return -1;

    const juce::String selectedId = juce::String::fromUTF8(selected.toUtf8().constData());
    return indexOfIdentifier(m_availableOutputDeviceIdentifiers, selectedId);
}

int MidiControllerManager::getSelectedMidiDeviceIndex() const
{
    const int selectedInputIndex = getSelectedMidiInputIndex();
    if (selectedInputIndex < 0)
        return -1;

    for (int i = 0; i < static_cast<int>(m_availableControllerInputIndexes.size()); ++i) {
        if (m_availableControllerInputIndexes[static_cast<size_t>(i)] == selectedInputIndex)
            return i;
    }

    const QString selectedInputName = m_availableInputDeviceNames.value(selectedInputIndex);
    if (looksLikeFlx10Name(selectedInputName)) {
        for (int i = 0; i < m_availableControllerDeviceNames.size(); ++i) {
            if (looksLikeFlx10Name(m_availableControllerDeviceNames.at(i)))
                return i;
        }
    }

    return -1;
}

QString MidiControllerManager::getMappingsDirectoryPath() const
{
    return SettingsManager::getInstance().getMappingsDirectoryPath();
}

QStringList MidiControllerManager::getAvailableMappingFiles()
{
    QStringList mappings { kBuiltInFlx10MappingLabel };

    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return mappings;

    mappings.append(dir.entryList({"*.xml", "*.XML"}, QDir::Files, QDir::Name));
    mappings.removeDuplicates();
    return mappings;
}

QString MidiControllerManager::getSettingsDirectoryPath() const
{
    return SettingsManager::getInstance().getConfigDirectoryPath();
}

bool MidiControllerManager::openSettingsDirectory() const
{
    return openDirectoryInFileManager(getSettingsDirectoryPath());
}

bool MidiControllerManager::openMappingsDirectory() const
{
    return openDirectoryInFileManager(getMappingsDirectoryPath());
}

QString MidiControllerManager::normalizeControllerKeyFromXmlBase(const QString& baseName) const
{
    QString key = baseName.trimmed();
    if (key.endsWith(".midi", Qt::CaseInsensitive))
        key.chop(5);
    if (key.endsWith(".brockdj", Qt::CaseInsensitive))
        key.chop(8);
    return key.toLower();
}

QString MidiControllerManager::normalizeControllerKeyFromJsBase(const QString& baseName) const
{
    QString key = baseName.trimmed();
    if (key.endsWith(".controller", Qt::CaseInsensitive))
        key.chop(11);
    return key.toLower();
}

QStringList MidiControllerManager::getAvailableControllers()
{
    QMap<QString, QString> dedup;
    dedup.insert(normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName), kBuiltInFlx10ControllerName);

    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return dedup.values();

    const auto xmlFiles = dir.entryList({"*.xml", "*.XML"}, QDir::Files, QDir::Name);
    for (const auto& file : xmlFiles) {
        QString base = QFileInfo(file).completeBaseName();
        const QString normalized = normalizeControllerKeyFromXmlBase(base);
        if (normalized.isEmpty())
            continue;

        if (!dedup.contains(normalized))
            dedup.insert(normalized, base);
    }

    const auto jsFiles = dir.entryList({"*.js", "*.JS"}, QDir::Files, QDir::Name);
    for (const auto& file : jsFiles) {
        QString base = QFileInfo(file).completeBaseName();
        const QString normalized = normalizeControllerKeyFromJsBase(base);
        if (normalized.isEmpty())
            continue;

        if (!dedup.contains(normalized))
            dedup.insert(normalized, base);
    }

    QStringList result = dedup.values();
    result.sort(Qt::CaseInsensitive);
    return result;
}

void MidiControllerManager::selectController(const QString& controllerName)
{
    m_selectedController = controllerName;
    SettingsManager::getInstance().setSelectedController(controllerName);
    emit mappingListUpdated();
}

QString MidiControllerManager::getSelectedController() const
{
    if (!m_selectedController.isEmpty())
        return m_selectedController;
    return SettingsManager::getInstance().getSelectedController();
}

QStringList MidiControllerManager::getAvailableXmlMappingFilesForController(const QString& controllerName) const
{
    const QString normalizedTarget = normalizeControllerKeyFromXmlBase(controllerName);
    if (normalizedTarget.isEmpty())
        return {};

    QStringList filtered;
    if (normalizedTarget == normalizeControllerKeyFromXmlBase(kBuiltInFlx10ControllerName))
        filtered.push_back(kBuiltInFlx10MappingLabel);

    QDir dir(getMappingsDirectoryPath());
    if (!dir.exists())
        return filtered;

    QStringList files = dir.entryList({"*.xml", "*.XML"}, QDir::Files, QDir::Name);

    for (const auto& file : files) {
        const QString base = QFileInfo(file).completeBaseName();
        if (normalizeControllerKeyFromXmlBase(base) == normalizedTarget)
            filtered.push_back(file);
    }

    filtered.sort(Qt::CaseInsensitive);
    return filtered;
}

QStringList MidiControllerManager::getAvailableMappingsForSelectedController()
{
    const QString controller = getSelectedController();
    if (controller.isEmpty())
        return getAvailableMappingFiles();
    return getAvailableXmlMappingFilesForController(controller);
}

void MidiControllerManager::selectMapping(const QString& mappingFileName)
{
    m_selectedMappingFile = mappingFileName;
    SettingsManager::getInstance().setSelectedMappingFile(mappingFileName);

    m_midiToParam.clear();
    m_paramToMidi.clear();
    m_momentaryHeldByMsgId.clear();
    m_scratchAbsoluteLastByMsgId.clear();

    if (!mappingFileName.isEmpty()) {
        if (!loadBrockDjXmlMapping(mappingFileName))
            qWarning() << "[MIDI] Failed to load mapping:" << mappingFileName;
    }

    emit mappingUpdated();
    emit mappingListUpdated();
    startFlx10OutputSession();
    refreshAllDeckLeds();
}

QString MidiControllerManager::getSelectedMapping() const
{
    if (!m_selectedMappingFile.isEmpty())
        return m_selectedMappingFile;
    return SettingsManager::getInstance().getSelectedMappingFile();
}

void MidiControllerManager::refreshMidiAndMappings()
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    refreshMidiDeviceCache();

    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    emit midiDevicesUpdated();
    emit controllerListUpdated();
    emit mappingListUpdated();
}

int MidiControllerManager::parseMappingNumber(const QString& rawValue) const
{
    QString value = rawValue.trimmed();
    bool ok = false;

    if (value.startsWith("0x", Qt::CaseInsensitive)) {
        const int parsed = value.mid(2).toInt(&ok, 16);
        return ok ? parsed : -1;
    }

    const int parsed = value.toInt(&ok, 10);
    return ok ? parsed : -1;
}

int MidiControllerManager::midiMessageIdFromStatusAndControl(int statusNo, int controlNo) const
{
    if (statusNo < 0 || controlNo < 0)
        return -1;

    const int statusHi = statusNo & 0xF0;
    const int midiCh = statusNo & 0x0F;
    int subId = clampMidi7bit(controlNo);

    if (statusHi == 0xB0)
        subId = 1000 + clampMidi7bit(controlNo);
    else if (statusHi == 0xE0)
        subId = 1500;
    else if (statusHi != 0x80 && statusHi != 0x90)
        return -1;

    return 10000 + midiCh * 2000 + subId;
}

QString MidiControllerManager::nativeMappingFilePath() const
{
    return SettingsManager::getInstance().getConfigDirectoryPath() + "/midi_mapping_native.xml";
}

QString MidiControllerManager::getMappingLabel(const QString& paramId) const
{
    const auto it = m_paramToMidi.find(paramId);
    if (it == m_paramToMidi.end())
        return {};
    const int msgId = it->second;
    // Channel-aware format: 10000 + channel*2000 + (isCc ? 1000+cc : note)
    if (msgId >= 10000) {
        const int remainder = msgId - 10000;
        const int channel   = remainder / 2000;
        const int sub       = remainder % 2000;
        if (sub == 1500)
            return QStringLiteral("Ch%1 Pitch").arg(channel + 1);
        if (sub >= 1000)
            return QStringLiteral("Ch%1 CC %2").arg(channel + 1).arg(sub - 1000);
        return QStringLiteral("Ch%1 Note %2").arg(channel + 1).arg(sub);
    }
    if (msgId == 1500)
        return QStringLiteral("Pitch");
    if (msgId >= 1000)
        return QStringLiteral("CC %1").arg(msgId - 1000);
    return QStringLiteral("Note %1").arg(msgId);
}

void MidiControllerManager::clearLearnedMapping(const QString& paramId)
{
    const auto paramIt = m_paramToMidi.find(paramId);
    if (paramIt == m_paramToMidi.end())
        return;

    const int msgId = paramIt->second;
    m_paramToMidi.erase(paramIt);
    m_midiToParam.erase(msgId);
    m_momentaryHeldByMsgId.erase(msgId);
    m_scratchAbsoluteLastByMsgId.erase(msgId);

    saveNativeMapping();
    emit mappingUpdated();
}

bool MidiControllerManager::isMappingInverted(const QString& paramId) const
{
    const auto it = m_paramInverted.find(paramId);
    return it != m_paramInverted.end() && it->second;
}

void MidiControllerManager::setMappingInverted(const QString& paramId, bool inverted)
{
    if (inverted)
        m_paramInverted[paramId] = true;
    else
        m_paramInverted.erase(paramId);
    saveNativeMapping();
    emit mappingInversionUpdated();
}

void MidiControllerManager::saveNativeMapping()
{
    const QString path = nativeMappingFilePath();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[MIDI] Could not write native mapping to" << path;
        return;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("BrockDJ_Mapping");
    xml.writeAttribute("version", "1");

    for (const auto& [msgId, entry] : m_midiToParam) {
        xml.writeStartElement("Entry");
        xml.writeAttribute("paramId", entry.paramId);
        xml.writeAttribute("msgId", QString::number(msgId));
        xml.writeAttribute("interactionType", interactionTypeToString(entry.interactionType));
        const auto invIt = m_paramInverted.find(entry.paramId);
        if (invIt != m_paramInverted.end() && invIt->second)
            xml.writeAttribute("inverted", "1");
        xml.writeEndElement();
    }

    xml.writeEndElement();
    xml.writeEndDocument();

    qDebug() << "[MIDI] Native mapping saved:" << path
             << "entries:" << static_cast<int>(m_midiToParam.size());
}

void MidiControllerManager::sendFlx10HotcuePaletteTest()
{
    bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        outputOpen = autoOpenFlx10MidiOutputIfNeeded();

    if (!outputOpen)
        return;

    m_midiFeedback.sendPaletteTest();
}

void MidiControllerManager::testFlx10LedOutput()
{
    bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        outputOpen = autoOpenFlx10MidiOutputIfNeeded();

    if (!outputOpen) {
        qWarning() << "[MIDI OUT] FLX10 raw LED test requested but no MIDI output is open";
        return;
    }

    m_midiFeedback.testRawLedOutput();
}

void MidiControllerManager::loadNativeMappingIfExists()
{
    const QString path = nativeMappingFilePath();
    QFile file(path);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[MIDI] Could not read native mapping from" << path;
        return;
    }

    QXmlStreamReader xml(&file);
    int count = 0;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name().toString() != "Entry")
            continue;

        const QString paramId = xml.attributes().value("paramId").toString();
        bool ok = false;
        const int msgId = xml.attributes().value("msgId").toString().toInt(&ok);
        const MidiInteractionType interactionType =
            interactionTypeFromString(xml.attributes().value("interactionType").toString(), paramId);
        const bool inverted = xml.attributes().value("inverted").toString() == QStringLiteral("1");
        if (ok && !paramId.isEmpty()) {
            m_midiToParam[msgId] = makeMappingEntry(paramId, interactionType);
            if (interactionType == MidiInteractionType::Momentary)
                m_momentaryHeldByMsgId[msgId] = false;
            else
                m_momentaryHeldByMsgId.erase(msgId);
            m_paramToMidi[paramId] = msgId;
            if (inverted)
                m_paramInverted[paramId] = true;
            ++count;
        }
    }

    qDebug() << "[MIDI] Native mapping loaded:" << path << "entries:" << count;

    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (outputOpen) {
        startFlx10OutputSession();
        if (shouldUseFlx10Feedback())
            refreshAllDeckLeds();
    }
}

bool MidiControllerManager::loadBrockDjXmlMapping(const QString& mappingFileName)
{
    const bool builtInMapping = isBuiltInFlx10Mapping(mappingFileName);
    const QString filePath = builtInMapping
        ? kBuiltInFlx10MappingResource
        : QDir(getMappingsDirectoryPath()).filePath(mappingFileName);
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[MIDI] Could not open mapping file:" << filePath;
        return false;
    }

    QXmlStreamReader xml(&file);

    std::map<int, MidiMappingEntry> nextMidiToParam;
    std::map<QString, int> nextParamToMidi;
    std::map<QString, bool> nextParamInverted = m_paramInverted;
    MidiFeedbackMapping feedbackMapping;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;

        const QString elementName = xml.name().toString();
        const auto attrs = xml.attributes();

        if (builtInMapping)
            applyMidiFeedbackMappingElement(elementName, attrs, feedbackMapping);

        if (elementName.compare(QStringLiteral("Entry"), Qt::CaseInsensitive) != 0)
            continue;

        const QString paramId = attrs.value("paramId").toString().trimmed();
        if (paramId.isEmpty())
            continue;

        bool ok = false;
        int msgId = attrs.value("msgId").toString().toInt(&ok);
        if (!ok) {
            const QString controlRaw = attrs.hasAttribute(QStringLiteral("control"))
                ? attrs.value("control").toString()
                : attrs.value("midino").toString();
            const int statusNo = parseMappingNumber(attrs.value("status").toString());
            const int controlNo = parseMappingNumber(controlRaw);
            msgId = midiMessageIdFromStatusAndControl(statusNo, controlNo);
        }
        if (msgId < 0)
            continue;

        const QString typeRaw = attrs.hasAttribute(QStringLiteral("type"))
            ? attrs.value("type").toString()
            : attrs.value("interactionType").toString();
        const MidiInteractionType interactionType = interactionTypeFromString(typeRaw, paramId);
        const QString invertedRaw = attrs.value("inverted").toString().trimmed().toLower();
        const bool inverted = invertedRaw == QStringLiteral("1")
            || invertedRaw == QStringLiteral("true")
            || invertedRaw == QStringLiteral("yes");

        nextMidiToParam[msgId] = makeMappingEntry(paramId, interactionType);
        nextParamToMidi[paramId] = msgId;
        if (inverted)
            nextParamInverted[paramId] = true;
    }

    if (xml.hasError()) {
        qWarning() << "[MIDI] XML parse error in" << mappingFileName << ":" << xml.errorString();
        return false;
    }

    m_midiToParam = std::move(nextMidiToParam);
    m_paramToMidi = std::move(nextParamToMidi);
    m_paramInverted = std::move(nextParamInverted);
    m_momentaryHeldByMsgId.clear();
    m_scratchAbsoluteLastByMsgId.clear();
    for (const auto& [msgId, entry] : m_midiToParam) {
        if (entry.interactionType == MidiInteractionType::Momentary)
            m_momentaryHeldByMsgId[msgId] = false;
    }

    qDebug() << "[MIDI] BrockDJ mapping loaded:" << mappingFileName
             << (builtInMapping ? "(built-in)" : "(user)")
             << "entries:" << static_cast<int>(m_midiToParam.size());
    if (builtInMapping) {
        m_midiFeedback.setMapping(feedbackMapping);
        m_deckAPadMode = MidiPadMode::HotCue;
        m_deckBPadMode = MidiPadMode::HotCue;
    }

    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (outputOpen) {
        startFlx10OutputSession();
        if (shouldUseFlx10Feedback())
            refreshAllDeckLeds();
    }
    return true;
}

void MidiControllerManager::applyMidiFeedbackMappingElement(const QString& elementName,
                                                             const QXmlStreamAttributes& attrs,
                                                             MidiFeedbackMapping& mapping) const
{
    auto parseByteAttr = [this, &attrs](const QString& name, int fallback) -> uint8_t
    {
        if (!attrs.hasAttribute(name))
            return static_cast<uint8_t>(std::clamp(fallback, 0, 255));
        const int parsed = parseMappingNumber(attrs.value(name).toString());
        return static_cast<uint8_t>(std::clamp(parsed >= 0 ? parsed : fallback, 0, 255));
    };

    if (elementName.compare(QStringLiteral("DeckLed"), Qt::CaseInsensitive) == 0) {
        const QString name = attrs.value(QStringLiteral("name")).toString().trimmed().toLower();
        const uint8_t control = parseByteAttr(QStringLiteral("control"), 0);
        if (name == QStringLiteral("play"))
            mapping.playNote = control;
        else if (name == QStringLiteral("cue"))
            mapping.cueNote = control;
        else if (name == QStringLiteral("loop_in"))
            mapping.loopInNote = control;
        else if (name == QStringLiteral("loop_out"))
            mapping.loopOutNote = control;
        else if (name == QStringLiteral("loop_4beat"))
            mapping.loop4BeatNote = control;
        else if (name == QStringLiteral("loop_reloop"))
            mapping.loopReloopNote = control;
        else if (name == QStringLiteral("tempo_reset"))
            mapping.tempoResetNote = control;
        else if (name == QStringLiteral("beat_sync"))
            mapping.beatSyncNote = control;
        else if (name == QStringLiteral("key_sync"))
            mapping.keySyncNote = control;
        return;
    }

    if (elementName.compare(QStringLiteral("DeckStatus"), Qt::CaseInsensitive) == 0) {
        const int deck = std::clamp(attrs.value(QStringLiteral("deck")).toInt(), 1, 4);
        mapping.deckNoteStatus[static_cast<size_t>(deck - 1)] =
            parseByteAttr(QStringLiteral("status"), mapping.deckNoteStatus[static_cast<size_t>(deck - 1)]);
        return;
    }

    if (elementName.compare(QStringLiteral("HotcuePads"), Qt::CaseInsensitive) == 0) {
        const int deck = std::clamp(attrs.value(QStringLiteral("deck")).toInt(), 1, 4);
        mapping.hotcueStatus[static_cast<size_t>(deck - 1)] =
            parseByteAttr(QStringLiteral("status"), mapping.hotcueStatus[static_cast<size_t>(deck - 1)]);
        mapping.hotcueShiftStatus[static_cast<size_t>(deck - 1)] =
            parseByteAttr(QStringLiteral("shiftStatus"), mapping.hotcueShiftStatus[static_cast<size_t>(deck - 1)]);
        return;
    }

    if (elementName.compare(QStringLiteral("VuMeter"), Qt::CaseInsensitive) == 0) {
        const int deck = std::clamp(attrs.value(QStringLiteral("deck")).toInt(), 1, 4);
        mapping.vuStatus[static_cast<size_t>(deck - 1)] =
            parseByteAttr(QStringLiteral("status"), mapping.vuStatus[static_cast<size_t>(deck - 1)]);
        mapping.vuControl = parseByteAttr(QStringLiteral("control"), mapping.vuControl);
        return;
    }

    if (elementName.compare(QStringLiteral("Color"), Qt::CaseInsensitive) == 0) {
        const QString name = attrs.value(QStringLiteral("name")).toString().trimmed().toLower();
        const uint8_t value = parseByteAttr(QStringLiteral("value"), 0);
        if (name == QStringLiteral("blue"))
            mapping.padBlue = value;
        else if (name == QStringLiteral("cyan"))
            mapping.padCyan = value;
        else if (name == QStringLiteral("green"))
            mapping.padGreen = value;
        else if (name == QStringLiteral("yellow"))
            mapping.padYellow = value;
        else if (name == QStringLiteral("orange"))
            mapping.padOrange = value;
        else if (name == QStringLiteral("red"))
            mapping.padRed = value;
        else if (name == QStringLiteral("pink"))
            mapping.padPink = value;
        else if (name == QStringLiteral("magenta"))
            mapping.padMagenta = value;
        else if (name == QStringLiteral("purple"))
            mapping.padPurple = value;
        else if (name == QStringLiteral("white"))
            mapping.padWhite = value;
    }
}

void MidiControllerManager::startMidiLearn(const QString& parameterId)
{
    bool hasLinuxAlsaMonitor = false;
#if defined(Q_OS_LINUX)
    hasLinuxAlsaMonitor = !m_alsaInputMonitors.empty();
#endif

    if (!hasLinuxAlsaMonitor)
        openMidiInputByIdentifier(kAllMidiInputsIdentifier);

    m_learnParameterId = parameterId;
    m_isLearning = true;
    // Clear live monitor so the UI immediately shows the next incoming event
    m_lastMidiEvent.clear();
    emit lastMidiEventChanged();
    qDebug() << "[MIDI] Learn started for" << parameterId;
    emit learnStarted(parameterId);
}

void MidiControllerManager::processDecodedMidiEvent(int msgId, float value, bool isNoteOff)
{
    // Live MIDI monitor — update for every event (both JUCE and ALSA paths)
    {
        const int sub  = (msgId >= 10000) ? (msgId - 10000) % 2000 : -1;
        const int chNo = (msgId >= 10000) ? (msgId - 10000) / 2000 : 0;
        QString evtLabel;
        if (isNoteOff)        evtLabel = QStringLiteral("Ch%1 NoteOff %2").arg(chNo+1).arg(sub);
        else if (sub == 1500) evtLabel = QStringLiteral("Ch%1 PitchBend").arg(chNo+1);
        else if (sub >= 1000) evtLabel = QStringLiteral("Ch%1 CC %2 = %3").arg(chNo+1).arg(sub-1000).arg(static_cast<int>(value*127));
        else if (sub >= 0)    evtLabel = QStringLiteral("Ch%1 Note %2  vel %3").arg(chNo+1).arg(sub).arg(static_cast<int>(value*127));

        qDebug() << "[MIDI IN]" << evtLabel << (m_isLearning ? "(LEARNING)" : "");
        if (!evtLabel.isEmpty() && evtLabel != m_lastMidiEvent) {
            m_lastMidiEvent = evtLabel;
            emit lastMidiEventChanged();
        }
    }

    if (m_isLearning && !isNoteOff) {
        learnMapping(msgId);
        return; // always skip dispatch while in learn mode
    }

    if (!m_parameterStore)
        return;

    auto dispatchTouchedAbsoluteJogFallback = [this, msgId, value]() -> bool
    {
        if (msgId < 10000)
            return false;

        const int sub = (msgId - 10000) % 2000;
        if (sub < 1000 || sub >= 1500)
            return false;

        // Some controllers send rim/nudge on CC N and touched platter motion on
        // CC N+1. Native learn captures the rim first, so keep CC N as nudge
        // and route the adjacent touched CC as scratch-only absolute motion.
        const int pairedJogMsgId = msgId - 1;
        const auto pairedIt = m_midiToParam.find(pairedJogMsgId);
        if (pairedIt == m_midiToParam.end()
            || pairedIt->second.interactionType != MidiInteractionType::EncoderRelative) {
            return false;
        }

        auto midiMomentaryHeld = [this](const QString& paramId) -> bool
        {
            const auto paramIt = m_paramToMidi.find(paramId);
            if (paramIt == m_paramToMidi.end())
                return false;
            const auto heldIt = m_momentaryHeldByMsgId.find(paramIt->second);
            return heldIt != m_momentaryHeldByMsgId.end() && heldIt->second;
        };

        const QString& pairedParamId = pairedIt->second.paramId;
        const bool pairedDeckA = pairedParamId == QStringLiteral("deckA_jog_move")
            || pairedParamId == QStringLiteral("deckA_jog_nudge");
        const bool pairedDeckB = pairedParamId == QStringLiteral("deckB_jog_move")
            || pairedParamId == QStringLiteral("deckB_jog_nudge");
        const bool deckATouched = pairedDeckA
            && (m_jogATouched || midiMomentaryHeld(QStringLiteral("deckA_jog_touch")));
        const bool deckBTouched = pairedDeckB
            && (m_jogBTouched || midiMomentaryHeld(QStringLiteral("deckB_jog_touch")));
        if (!deckATouched && !deckBTouched)
            return false;

        const QString scratchParamId = deckATouched
            ? QStringLiteral("deckA_jog_scratch")
            : QStringLiteral("deckB_jog_scratch");

        const int raw = clampMidi7bit(static_cast<int>(std::round(value * 127.0f)));
        const auto previousIt = m_scratchAbsoluteLastByMsgId.find(msgId);

        if (previousIt == m_scratchAbsoluteLastByMsgId.end()) {
            m_scratchAbsoluteLastByMsgId[msgId] = raw;
            qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                     << "value:" << raw
                     << "mappedAction:" << scratchParamId
                     << "interactionType:touched-absolute-jog"
                     << "dispatch=baseline";
            return true;
        }

        const int previousRaw = previousIt->second;
        m_scratchAbsoluteLastByMsgId[msgId] = raw;

        float delta = decodeWrappedAbsoluteDelta(previousRaw, raw);
        const auto invIt = m_paramInverted.find(pairedParamId);
        if (invIt != m_paramInverted.end() && invIt->second)
            delta = -delta;

        qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                 << "value:" << raw
                 << "previous:" << previousRaw
                 << "deltaTicks:" << delta
                 << "mappedAction:" << scratchParamId
                 << "interactionType:touched-absolute-jog"
                 << "dispatch:jogMoveFallback";

        if (delta == 0.0f)
            return true;

        dispatchMidiParameterToStore(scratchParamId, delta);
        return true;
    };

    // 14-bit CC handling: accumulate MSB (CC 0-31) and combine with LSB (CC 32-63).
    // The DDJ-FLX10 (and most modern controllers) send every analog control as a
    // MSB+LSB pair for 14-bit resolution instead of 7-bit.
    {
        const int sub = (msgId >= 10000) ? (msgId - 10000) % 2000 : -1;

        if (!isNoteOff && sub >= 1000 && sub < 1032) {
            // MSB CC (CC 0-31): accumulate the 7-bit value for later LSB pairing
            const auto msbIt = m_midiToParam.find(msgId);
            if (msbIt != m_midiToParam.end())
                m_msbAccumulator[msbIt->second.paramId] = static_cast<int>(value * 127.0f);
            // fall through to standard 7-bit dispatch below so the control moves
            // immediately, even before the LSB arrives
        } else if (!isNoteOff && sub >= 1032 && sub < 1064) {
            // LSB CC (CC 32-63): combine with stored MSB for 14-bit precision
            const auto currentIt = m_midiToParam.find(msgId);
            const bool currentIsDiscrete = currentIt != m_midiToParam.end()
                && shouldAlwaysDispatch(currentIt->second.interactionType);
            const int msbMsgId = msgId - 32; // paired MSB is always 32 less
            const auto msbIt = m_midiToParam.find(msbMsgId);
            if (!currentIsDiscrete
                && msbIt != m_midiToParam.end()
                && !shouldAlwaysDispatch(msbIt->second.interactionType)) {
                const QString& pId = msbIt->second.paramId;
                const int lsb = static_cast<int>(value * 127.0f);
                const int msb = m_msbAccumulator.count(pId) ? m_msbAccumulator.at(pId) : 64;
                const float combined = static_cast<float>((msb << 7) | lsb) / 16383.0f;
                dispatchParameterToStore(pId, combined);
                return; // handled via 14-bit pairing; don't double-dispatch
            }
            // No paired MSB found → fall through to standard dispatch
        }
    }

    const auto it = m_midiToParam.find(msgId);
    if (it == m_midiToParam.end()) {
        if (dispatchTouchedAbsoluteJogFallback())
            return;

        qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                 << "value:" << static_cast<int>(std::round(value * 127.0f))
                 << "mapping:not-found";
        return;
    }

    const QString paramId = it->second.paramId;
    const MidiInteractionType interactionType = it->second.interactionType;
    float dispatchValue = value;
    {
        const auto invIt = m_paramInverted.find(paramId);
        if (invIt != m_paramInverted.end() && invIt->second) {
            if (isRelativeInteraction(interactionType))
                dispatchValue = -dispatchValue;
            else if (!isNoteOff && !isButtonInteraction(interactionType))
                dispatchValue = 1.0f - dispatchValue;
        }
    }
    const bool pressed = dispatchValue > 0.0f;
    if (isButtonInteraction(interactionType)) {
        const int rawMidiValue = static_cast<int>(std::round(value * 127.0f));

        if (interactionType == MidiInteractionType::Toggle && !pressed) {
            qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                     << "value:" << rawMidiValue
                     << "interpreted:released"
                     << "mappedAction:" << paramId
                     << "interactionType:" << interactionTypeToString(interactionType)
                     << "previous:n/a"
                     << "current:n/a"
                     << "dispatch:ignored-toggle-release";
            return;
        }

        if (interactionType == MidiInteractionType::Momentary) {
            const bool previousHeld = m_momentaryHeldByMsgId.count(msgId)
                ? m_momentaryHeldByMsgId.at(msgId)
                : false;
            const bool currentHeld = pressed;
            m_momentaryHeldByMsgId[msgId] = currentHeld;

            const bool changed = (previousHeld != currentHeld);
            const bool forceRelease = !currentHeld;
            const char* dispatchName = currentHeld
                ? (changed ? "press" : "ignored-repeat-press")
                : (changed ? "release" : "force-release");

            qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                     << "value:" << rawMidiValue
                     << "interpreted:" << (currentHeld ? "pressed" : "released")
                     << "mappedAction:" << paramId
                     << "interactionType:" << interactionTypeToString(interactionType)
                     << "previous:" << previousHeld
                     << "current:" << currentHeld
                     << "dispatch:" << dispatchName;

            if (!changed && !forceRelease)
                return;
        } else {
            qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                     << "value:" << rawMidiValue
                     << "interpreted:" << (pressed ? "pressed" : "released")
                     << "mappedAction:" << paramId
                     << "interactionType:" << interactionTypeToString(interactionType)
                     << "previous:n/a"
                     << "current:n/a"
                     << "dispatch:press";
        }

        dispatchValue = pressed ? 1.0f : 0.0f;
    }

    // Relative encoders and buttons can produce repeated identical values that
    // are semantically distinct events, so always emit them. Analog controls
    // use deduplication to avoid MIDI feedback echo loops.
    if (shouldAlwaysDispatch(interactionType)) {
        dispatchMidiParameterToStore(paramId, dispatchValue);
    } else {
        dispatchParameterToStore(paramId, dispatchValue);
    }
}

void MidiControllerManager::dispatchParameterToStore(const QString& paramId, float value)
{
    if (!m_parameterStore)
        return;

    if (QThread::currentThread() == m_parameterStore->thread()) {
        m_parameterStore->setParameter(paramId, value);
        return;
    }

    QMetaObject::invokeMethod(m_parameterStore, "setParameter", Qt::QueuedConnection,
                              Q_ARG(QString, paramId),
                              Q_ARG(float, value));
}

void MidiControllerManager::dispatchMidiParameterToStore(const QString& paramId, float value)
{
    if (!m_parameterStore)
        return;

    if (QThread::currentThread() == m_parameterStore->thread()) {
        m_parameterStore->setMidiParameter(paramId, value);
        return;
    }

    QMetaObject::invokeMethod(m_parameterStore, "setMidiParameter", Qt::QueuedConnection,
                              Q_ARG(QString, paramId),
                              Q_ARG(float, value));
}

void MidiControllerManager::learnMapping(int msgId)
{
    if (m_learnParameterId.isEmpty())
        return;

    // Keep the maps one-to-one. Re-learning a control must not leave an old
    // MIDI event still driving the same parameter, and stealing a MIDI event
    // must clear the previous parameter's reverse lookup.
    const auto oldParamForMidi = m_midiToParam.find(msgId);
    if (oldParamForMidi != m_midiToParam.end()) {
        m_paramToMidi.erase(oldParamForMidi->second.paramId);
        m_momentaryHeldByMsgId.erase(msgId);
    }

    const auto oldMidiForParam = m_paramToMidi.find(m_learnParameterId);
    if (oldMidiForParam != m_paramToMidi.end()) {
        m_midiToParam.erase(oldMidiForParam->second);
        m_momentaryHeldByMsgId.erase(oldMidiForParam->second);
    }

    const QString learnedParamId = m_learnParameterId;
    const MidiMappingEntry learnedEntry = makeMappingEntry(learnedParamId);
    m_midiToParam[msgId] = learnedEntry;
    if (learnedEntry.interactionType == MidiInteractionType::Momentary)
        m_momentaryHeldByMsgId[msgId] = false;
    m_paramToMidi[learnedParamId] = msgId;
    m_learnParameterId.clear();
    m_isLearning = false;

    qDebug() << "[MIDI] Learned" << msgId << "->" << learnedParamId;
    saveNativeMapping();
    emit mappingUpdated();
}

void MidiControllerManager::handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message)
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    // Diagnostic: log every incoming MIDI message so we can see if JUCE is even
    // receiving Note On events.  The raw status byte tells us the truth.
    {
        const auto* d = message.getRawData();
        const int   sz = message.getRawDataSize();
        const int   status = sz > 0 ? static_cast<unsigned char>(d[0]) : 0;
        const int   d1     = sz > 1 ? static_cast<unsigned char>(d[1]) : -1;
        const int   d2     = sz > 2 ? static_cast<unsigned char>(d[2]) : -1;
        qDebug() << "[MIDI JUCE]" << "status:" << status
                 << "d1:" << d1 << "d2:" << d2
                 << "isNoteOn:" << message.isNoteOn()
                 << "isNoteOff:" << message.isNoteOff()
                 << "isCC:" << message.isController();
    }

    // Runs on the JUCE MIDI thread — decode raw bytes (cheap, no allocation),
    // then dispatch to the Qt main thread so processDecodedMidiEvent can safely
    // read/write m_isLearning and the maps without a data race.
    const int ch = message.getChannel() - 1; // 0-based

    // rawEnc encoding by type:
    //   CC         → raw controller value 0-127 (int stored as float)
    //   Note On/Off→ velocity 0.0-1.0 float
    //   Pitch Bend → raw 14-bit value 0-16383
    int   msgId   = -1;
    float rawEnc  = 0.0f;
    bool  noteOff = false;

    if (message.isController()) {
        msgId  = 10000 + ch * 2000 + 1000 + message.getControllerNumber();
        rawEnc = static_cast<float>(message.getControllerValue()); // 0-127
    } else if (message.isNoteOn()) {
        msgId  = 10000 + ch * 2000 + message.getNoteNumber();
        rawEnc = message.getFloatVelocity();
    } else if (message.isNoteOff()) {
        msgId  = 10000 + ch * 2000 + message.getNoteNumber();
        noteOff = true;
    } else if (message.isPitchWheel()) {
        // sub-ID 1500 is reserved for pitch bend (per channel, no note/cc number)
        msgId  = 10000 + ch * 2000 + 1500;
        rawEnc = static_cast<float>(message.getPitchWheelValue()); // 0-16383
    } else {
        return;
    }

    // Marshal to Qt main thread. Qt cancels the call automatically if `this`
    // is destroyed before the event loop processes it.
    QMetaObject::invokeMethod(this, [this, msgId, rawEnc, noteOff]() mutable
    {
        int   resolvedId    = msgId;
        float resolvedValue = rawEnc;

        if (msgId >= 10000) {
            const int sub = (msgId - 10000) % 2000;

            if (sub >= 1000 && sub < 1500) {
                // Regular CC: check legacy (channel-stripped) mapping too
                const int cc       = sub - 1000;
                const int legacyId = cc + 1000;
                if (!m_midiToParam.count(msgId) && m_midiToParam.count(legacyId))
                    resolvedId = legacyId;

                const auto it = m_midiToParam.find(resolvedId);
                if (it != m_midiToParam.end() && isRelativeInteraction(it->second.interactionType)) {
                    const int raw = static_cast<int>(rawEnc);
                    resolvedValue = decodeRelativeCcValue(raw, it->second.paramId);
                } else {
                    resolvedValue = clampMidi7bit(static_cast<int>(rawEnc)) / 127.0f;
                }
            } else if (sub == 1500) {
                const int legacyId = 1500;
                if (!m_midiToParam.count(msgId) && m_midiToParam.count(legacyId))
                    resolvedId = legacyId;

                // Pitch bend: normalise 0-16383 → 0.0-1.0
                resolvedValue = rawEnc / 16383.0f;
            }
            // sub < 1000 → Note On/Off: rawEnc is already a normalised float velocity
        }

        processDecodedMidiEvent(resolvedId, resolvedValue, noteOff);
    }, Qt::QueuedConnection);
}

void MidiControllerManager::onParameterChanged(const QString& id, float value)
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        return;

    // Jog and button actions are input-only here. Echoing them as
    // LED feedback can come back through ALSA/PipeWire as a fresh input event
    // and toggle Play/Cue twice.
    const MidiInteractionType interactionType = defaultInteractionTypeForParam(id);
    if (isRelativeInteraction(interactionType) || isButtonInteraction(interactionType))
        return;

    const auto it = m_paramToMidi.find(id);
    if (it == m_paramToMidi.end())
        return;

    const int msgId = it->second;
    juce::MidiMessage msg;

    // Decode channel and sub-id from channel-aware format (≥10000) or legacy format
    int channel = 1; // 1-based for JUCE
    int subId   = msgId;
    if (msgId >= 10000) {
        const int remainder = msgId - 10000;
        channel = (remainder / 2000) + 1; // convert 0-based channel to 1-based
        subId   = remainder % 2000;
    }

    if (subId == 1500) {
        const int pitch = std::max(0, std::min(16383, static_cast<int>(value * 16383.0f)));
        msg = juce::MidiMessage::pitchWheel(channel, pitch);
    } else if (subId >= 1000 && subId < 1500) {
        msg = juce::MidiMessage::controllerEvent(channel, subId - 1000,
                                                  clampMidi7bit(static_cast<int>(value * 127.0f)));
    } else {
        if (value > 0.0f)
            msg = juce::MidiMessage::noteOn(channel, clampMidi7bit(subId), value);
        else
            msg = juce::MidiMessage::noteOff(channel, clampMidi7bit(subId), 0.0f);
    }

    sendMidiMessageWithDebug(msg, QStringLiteral("mapped-feedback"));
}

int MidiControllerManager::hotCueStatusForDeck(int deck) const
{
    switch (std::clamp(deck, 1, 4)) {
    case 1: return 0x97;
    case 2: return 0x99;
    case 3: return 0x9B;
    case 4: return 0x9D;
    default: return 0x97;
    }
}

int MidiControllerManager::hotCueStatusForDeck(QChar deck) const
{
    return hotCueStatusForDeck(deck == QLatin1Char('A') ? 1 : 2);
}

bool MidiControllerManager::sendMidiShort(int statusNo, int controlNo, int value, const QString& messageType)
{
    const int status = statusNo & 0xff;
    const int control = clampMidi7bit(controlNo);
    const int dataValue = clampMidi7bit(value);
    const bool cacheable = messageType != QStringLiteral("raw-test")
        && messageType != QStringLiteral("pad-palette-test");
    const int cacheKey = (status << 8) | control;
    if (cacheable) {
        const auto it = m_lastMidiShortValues.find(cacheKey);
        if (it != m_lastMidiShortValues.end() && it->second == dataValue)
            return true;
    }

    const unsigned char bytes[3] = {
        static_cast<unsigned char>(status),
        static_cast<unsigned char>(control),
        static_cast<unsigned char>(dataValue)
    };
    const juce::MidiMessage msg(bytes, 3);
    const bool success = sendMidiMessageWithDebug(msg, messageType);
    if (success && cacheable)
        m_lastMidiShortValues[cacheKey] = dataValue;
    return success;
}

bool MidiControllerManager::sendMidiMessageWithDebug(const juce::MidiMessage& message, const QString& messageType)
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        return false;

    const auto* raw = message.getRawData();
    const int size = message.getRawDataSize();
    const int status = size > 0 ? static_cast<unsigned char>(raw[0]) : 0;
    const int data1 = size > 1 ? static_cast<unsigned char>(raw[1]) : 0;
    const int data2 = size > 2 ? static_cast<unsigned char>(raw[2]) : 0;
    const int channel = (status & 0x0f) + 1;

    QString type = messageType;
    if (type.isEmpty()) {
        const int high = status & 0xf0;
        if (high == 0x80 || high == 0x90)
            type = QStringLiteral("note");
        else if (high == 0xB0)
            type = QStringLiteral("cc");
        else if (high == 0xE0)
            type = QStringLiteral("pitch");
        else if (status == 0xF0)
            type = QStringLiteral("sysex");
        else
            type = QStringLiteral("raw");
    }

    const QString rawBytes = size == 3
        ? QStringLiteral("%1 %2 %3").arg(hexByte(status), hexByte(data1), hexByte(data2))
        : QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(raw), size).toHex(' ')).toUpper();
    const bool verboseMidiOut = qEnvironmentVariableIntValue("BROCKDJ_MIDI_OUT_LOG") > 0;
    const bool noisySuccess = type == QStringLiteral("vu-meter") || type == QStringLiteral("pad-led");
    const bool logSuccess = verboseMidiOut || !noisySuccess;

    try {
#if defined(Q_OS_LINUX)
        if (m_alsaMidiOutput && m_alsaMidiOutput->isOpen()) {
            if (size != 3) {
                qWarning().noquote()
                    << QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=error error=\"ALSA short output only supports 3-byte MIDI messages\"")
                           .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                           .arg(m_selectedMidiOutputIndex)
                           .arg(type)
                           .arg(channel)
                           .arg(rawBytes);
                return false;
            }

            QString errorMessage;
            const bool success = m_alsaMidiOutput->sendShort(
                static_cast<uint8_t>(status),
                static_cast<uint8_t>(data1),
                static_cast<uint8_t>(data2),
                &errorMessage);

            const QString logLine = QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=%6%7")
                .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                .arg(m_selectedMidiOutputIndex)
                .arg(type)
                .arg(channel)
                .arg(rawBytes)
                .arg(success ? QStringLiteral("success") : QStringLiteral("error"))
                .arg(success ? QString() : QStringLiteral(" error=\"%1\"").arg(errorMessage));
            if (success) {
                if (logSuccess)
                    qInfo().noquote() << logLine;
            } else {
                qWarning().noquote() << logLine;
            }
            return success;
        }
#endif

        m_midiOutput->sendMessageNow(message);
        if (logSuccess) {
            qInfo().noquote()
                << QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=success")
                       .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                       .arg(m_selectedMidiOutputIndex)
                       .arg(type)
                       .arg(channel)
                       .arg(rawBytes);
        }
        return true;
    } catch (const std::exception& e) {
        qWarning().noquote()
            << QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=error error=\"%6\"")
                   .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                   .arg(m_selectedMidiOutputIndex)
                   .arg(type)
                   .arg(channel)
                   .arg(rawBytes)
                   .arg(QString::fromUtf8(e.what()));
    } catch (...) {
        qWarning().noquote()
            << QString("[MIDI OUT] port=\"%1\" index=%2 type=%3 channel=%4 SEND RAW: %5 result=error error=\"unknown\"")
                   .arg(m_selectedMidiOutputName.isEmpty() ? QStringLiteral("<unknown>") : m_selectedMidiOutputName)
                   .arg(m_selectedMidiOutputIndex)
                   .arg(type)
                   .arg(channel)
                   .arg(rawBytes);
    }

    return false;
}

void MidiControllerManager::sendMidiNoteLed(int statusNo, int noteNo, int value)
{
    sendMidiShort(statusNo, noteNo, value, QStringLiteral("note-led"));
}

void MidiControllerManager::sendMappedNoteLed(const QString& paramId, bool on, int onValue)
{
    const bool outputOpen =
        (m_midiOutput != nullptr)
#if defined(Q_OS_LINUX)
        || (m_alsaMidiOutput && m_alsaMidiOutput->isOpen())
#endif
        ;

    if (!outputOpen)
        return;

    const auto it = m_paramToMidi.find(paramId);
    if (it == m_paramToMidi.end())
        return;

    int channel = 1;
    int subId = it->second;
    if (subId >= 10000) {
        const int remainder = subId - 10000;
        channel = (remainder / 2000) + 1;
        subId = remainder % 2000;
    }

    if (subId >= 1000)
        return;

    sendMidiShort(0x90 + (channel - 1), subId, on ? onValue : 0);
}

int MidiControllerManager::hotCueLedValueForColor(const QString& color) const
{
    struct NamedPaletteValue { const char* name; int value; double hue; };
    static constexpr NamedPaletteValue kFlx10PadPalette[] = {
        { "red", 0x29, 0.0 },
        { "orange", 0x25, 30.0 },
        { "yellow", 0x1D, 55.0 },
        { "green", 0x15, 120.0 },
        { "cyan", 0x11, 180.0 },
        { "blue", 0x01, 235.0 },
        { "purple", 0x3D, 280.0 },
        { "pink", 0x35, 325.0 },
    };

    QString hex = color.trimmed();
    if (hex.startsWith(QLatin1Char('#')))
        hex.remove(0, 1);

    if (hex.size() == 3) {
        QString expanded;
        expanded.reserve(6);
        for (const QChar c : hex) {
            expanded.append(c);
            expanded.append(c);
        }
        hex = expanded;
    }

    bool ok = false;
    const int rgb = hex.toInt(&ok, 16);
    if (!ok || hex.size() != 6)
        return kFlx10PadPalette[0].value;

    const double r = static_cast<double>((rgb >> 16) & 0xff) / 255.0;
    const double g = static_cast<double>((rgb >> 8) & 0xff) / 255.0;
    const double b = static_cast<double>(rgb & 0xff) / 255.0;
    const double maxC = std::max({ r, g, b });
    const double minC = std::min({ r, g, b });
    const double delta = maxC - minC;
    if (maxC <= 0.0 || delta <= 0.0001)
        return 0x7F;

    const double saturation = delta / maxC;
    if (saturation < 0.18)
        return 0x7F;

    double hue = 0.0;
    if (maxC == r)
        hue = 60.0 * std::fmod(((g - b) / delta), 6.0);
    else if (maxC == g)
        hue = 60.0 * (((b - r) / delta) + 2.0);
    else
        hue = 60.0 * (((r - g) / delta) + 4.0);
    if (hue < 0.0)
        hue += 360.0;

    int bestValue = kFlx10PadPalette[0].value;
    double bestDistance = 361.0;
    for (const auto& entry : kFlx10PadPalette) {
        double distance = std::abs(hue - entry.hue);
        distance = std::min(distance, 360.0 - distance);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestValue = entry.value;
        }
    }
    return bestValue;
}

void MidiControllerManager::refreshTransportAndLoopLeds(QChar deck, DjEngine* engine)
{
    if (!engine)
        return;

    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshDeckLeds(deck == QLatin1Char('A') ? 1 : 2);
        return;
    }

    const QString prefix = deck == QLatin1Char('A')
        ? QStringLiteral("deckA_")
        : QStringLiteral("deckB_");

    sendMappedNoteLed(prefix + QStringLiteral("play"), engine->isPlaying());
    sendMappedNoteLed(prefix + QStringLiteral("cue"), !engine->isPlaying());
    sendMappedNoteLed(prefix + QStringLiteral("headphone_cue"), engine->cueEnabled());
    const bool loopOutSet = engine->loopOutPosition() > engine->loopInPosition() + 0.001;
    const bool isFourBeatLoop = engine->loopActive() && std::abs(engine->loopLengthBeats() - 4.0) < 0.1;
    sendMappedNoteLed(prefix + QStringLiteral("loop_in"), engine->loopInSet());
    sendMappedNoteLed(prefix + QStringLiteral("loop_out"), loopOutSet);
    sendMappedNoteLed(prefix + QStringLiteral("loop_4beat"), isFourBeatLoop);
    sendMappedNoteLed(prefix + QStringLiteral("loop_reloop"), engine->loopActive());
    sendMappedNoteLed(prefix + QStringLiteral("beat_sync"), engine->syncEnabled());
    sendMappedNoteLed(prefix + QStringLiteral("key_sync"), engine->keylock());
    sendMappedNoteLed(prefix + QStringLiteral("keylock"), engine->keylock());
    sendMappedNoteLed(prefix + QStringLiteral("quantize"), engine->quantizeEnabled());
    sendMappedNoteLed(prefix + QStringLiteral("slip"), engine->slipActive());
}

void MidiControllerManager::refreshPadModeLeds(QChar deck)
{
    const QString prefix = deck == QLatin1Char('A')
        ? QStringLiteral("deckA_pad_mode_")
        : QStringLiteral("deckB_pad_mode_");
    const MidiPadMode mode = padModeForDeck(deck);
    sendMappedNoteLed(prefix + QStringLiteral("hotcue"), mode == MidiPadMode::HotCue);
    sendMappedNoteLed(prefix + QStringLiteral("padfx"), mode == MidiPadMode::PadFx);
    sendMappedNoteLed(prefix + QStringLiteral("beatjump"), mode == MidiPadMode::BeatJump);
}

void MidiControllerManager::refreshHotCueLeds(QChar deck, DjEngine* engine)
{
    if (!engine)
        return;

    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshHotcuePads(deck == QLatin1Char('A') ? 1 : 2);
        return;
    }

    const QVariantList hotCues = engine->hotCues();
    const QVariantList savedLoops = engine->savedLoops();
    const int status = hotCueStatusForDeck(deck);
    for (int i = 0; i < 8; ++i) {
        bool isSet = false;
        QString color;
        if (i < savedLoops.size()) {
            const QVariantMap loopCue = savedLoops.at(i).toMap();
            if (loopCue.value(QStringLiteral("set")).toBool()) {
                isSet = true;
                color = loopCue.value(QStringLiteral("color")).toString();
            }
        }
        if (!isSet && i < hotCues.size()) {
            const QVariantMap cue = hotCues.at(i).toMap();
            isSet = cue.value(QStringLiteral("set")).toBool();
            color = cue.value(QStringLiteral("color")).toString();
        }
        sendMidiNoteLed(status, i, isSet ? hotCueLedValueForColor(color) : 0);
    }
}

void MidiControllerManager::refreshPerformancePadLeds(QChar deck, DjEngine* engine)
{
    const MidiPadMode mode = padModeForDeck(deck);
    if (mode == MidiPadMode::HotCue) {
        refreshHotCueLeds(deck, engine);
        return;
    }

    const int status = hotCueStatusForDeck(deck);
    static constexpr int kPadFxColors[8] = { 0x11, 0x11, 0x15, 0x15, 0x25, 0x25, 0x29, 0x29 };
    static constexpr int kBeatJumpColors[8] = { 0x01, 0x01, 0x11, 0x11, 0x15, 0x15, 0x1D, 0x1D };
    const int* colors = mode == MidiPadMode::PadFx ? kPadFxColors : kBeatJumpColors;

    for (int i = 0; i < 8; ++i)
        sendMidiNoteLed(status, i, colors[i]);
}

void MidiControllerManager::refreshDeckLeds(QChar deck, DjEngine* engine)
{
    if (shouldUseFlx10Feedback()) {
        m_midiFeedback.refreshDeckLeds(deck == QLatin1Char('A') ? 1 : 2);
        refreshPadModeLeds(deck);
        refreshPerformancePadLeds(deck, engine);
        return;
    }

    refreshTransportAndLoopLeds(deck, engine);
    refreshPadModeLeds(deck);
    refreshPerformancePadLeds(deck, engine);
}

void MidiControllerManager::refreshAllDeckLeds()
{
    refreshDeckLeds(QLatin1Char('A'), m_deckA);
    refreshDeckLeds(QLatin1Char('B'), m_deckB);
}

MidiPadMode MidiControllerManager::padModeForDeck(QChar deck) const
{
    return deck == QLatin1Char('A') ? m_deckAPadMode : m_deckBPadMode;
}

void MidiControllerManager::setPadModeForDeck(QChar deck, MidiPadMode mode)
{
    if (deck == QLatin1Char('A'))
        m_deckAPadMode = mode;
    else
        m_deckBPadMode = mode;
    refreshPadModeLeds(deck);
    refreshPerformancePadLeds(deck, deck == QLatin1Char('A') ? m_deckA : m_deckB);
}

void MidiControllerManager::stopPadFxToggle(DjEngine* engine, int padIndex)
{
    if (!engine)
        return;

    switch (padIndex) {
    case 4: engine->stopEchoOut(); break;
    case 5: engine->stopBackspin(); break;
    case 6: engine->stopVinylBrake(); break;
    case 7: engine->stopRollOut(); break;
    default: break;
    }
}

void MidiControllerManager::clearPadFxState(QChar deck, DjEngine* engine)
{
    if (!engine)
        return;

    int& momentary = deck == QLatin1Char('A') ? m_deckAPadFxMomentary : m_deckBPadFxMomentary;
    int& toggle = deck == QLatin1Char('A') ? m_deckAPadFxToggle : m_deckBPadFxToggle;

    if (momentary >= 0) {
        engine->clearPadFx();
        momentary = -1;
    }

    if (toggle >= 0) {
        stopPadFxToggle(engine, toggle);
        toggle = -1;
    }
}

void MidiControllerManager::handlePerformancePad(QChar deck,
                                                 DjEngine* engine,
                                                 int padIndex,
                                                 bool pressed,
                                                 bool clearRequest)
{
    if (!engine || padIndex < 0 || padIndex >= 8)
        return;

    const MidiPadMode mode = padModeForDeck(deck);

    if (mode == MidiPadMode::HotCue) {
        if (!pressed)
            return;
        if (clearRequest)
            engine->clearCuePad(padIndex);
        else
            engine->triggerCuePad(padIndex);
        refreshHotCueLeds(deck, engine);
        return;
    }

    if (clearRequest)
        return;

    if (mode == MidiPadMode::BeatJump) {
        if (!pressed)
            return;
        static constexpr double kBeatJumpPads[8] = { -16.0, -8.0, -4.0, -2.0, 2.0, 4.0, 8.0, 16.0 };
        engine->beatJump(kBeatJumpPads[padIndex]);
        return;
    }

    int& momentary = deck == QLatin1Char('A') ? m_deckAPadFxMomentary : m_deckBPadFxMomentary;
    int& toggle = deck == QLatin1Char('A') ? m_deckAPadFxToggle : m_deckBPadFxToggle;

    if (padIndex < 4) {
        static const QString kMomentaryFx[4] = {
            QStringLiteral("Echo"),
            QStringLiteral("Flanger"),
            QStringLiteral("Reverb"),
            QStringLiteral("Roll")
        };

        if (pressed) {
            momentary = padIndex;
            engine->setPadFx(kMomentaryFx[padIndex], 1.0f);
        } else if (momentary == padIndex) {
            engine->clearPadFx();
            momentary = -1;
        }
        return;
    }

    if (!pressed)
        return;

    if (toggle == padIndex) {
        stopPadFxToggle(engine, padIndex);
        toggle = -1;
        return;
    }

    if (toggle >= 0)
        stopPadFxToggle(engine, toggle);

    switch (padIndex) {
    case 4: engine->startEchoOut(); break;
    case 5: engine->startBackspin(); break;
    case 6: engine->startVinylBrake(); break;
    case 7: engine->startRollOut(); break;
    default: break;
    }
    toggle = padIndex;
}

void MidiControllerManager::connectDecks(DjEngine* deckA, DjEngine* deckB)
{
    if (m_deckA)
        QObject::disconnect(m_deckA, nullptr, this, nullptr);
    if (m_deckB && m_deckB != m_deckA)
        QObject::disconnect(m_deckB, nullptr, this, nullptr);

    m_deckA = deckA;
    m_deckB = deckB;
    m_cueAHeld = false;
    m_cueBHeld = false;
    m_jogATouched = false;
    m_jogBTouched = false;
    m_jogAReleaseTimer.stop();
    m_jogBReleaseTimer.stop();
    m_jogAReleasedRecently = false;
    m_jogBReleasedRecently = false;
    m_deckAShiftHeld = false;
    m_deckBShiftHeld = false;
    m_deckAPadMode = MidiPadMode::HotCue;
    m_deckBPadMode = MidiPadMode::HotCue;
    m_deckAPadFxMomentary = -1;
    m_deckBPadFxMomentary = -1;
    m_deckAPadFxToggle = -1;
    m_deckBPadFxToggle = -1;
    m_deckAFxSlotsEnabled = { false, false, false };
    m_deckBFxSlotsEnabled = { false, false, false };
    m_beatFxActive = false;
    m_beatFxPosition = 1;
    m_beatFxTargetDeck = QLatin1Char('A');
    m_midiFeedback.setDecks(m_deckA, m_deckB);

    auto wireDeckLeds = [this](QChar deck, DjEngine* engine)
    {
        if (!engine)
            return;

        QObject::connect(engine, &DjEngine::trackLoaded,
                         this, [this] { refreshAllDeckLeds(); });
        QObject::connect(engine, &DjEngine::trackMetadataChanged,
                         this, [this] { refreshAllDeckLeds(); });
        QObject::connect(engine, &DjEngine::playingChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::loopChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::cueEnabledChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::tempoChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::syncChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::syncMasterChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::keylockChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::quantizeEnabledChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::slipChanged,
                         this, [this, deck, engine] { refreshTransportAndLoopLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::hotCuesChanged,
                         this, [this, deck, engine] { refreshHotCueLeds(deck, engine); });
        QObject::connect(engine, &DjEngine::savedLoopsChanged,
                         this, [this, deck, engine] { refreshHotCueLeds(deck, engine); });
    };
    wireDeckLeds(QLatin1Char('A'), m_deckA);
    wireDeckLeds(QLatin1Char('B'), m_deckB);
    refreshAllDeckLeds();

    if (!m_parameterStore)
        return;

    if (m_deckActionsConnection)
        QObject::disconnect(m_deckActionsConnection);

    // Route ParameterStore events to deck actions.
    // Volume and crossfader are handled in QML via parameterStore directly.
    // Button convention: 127/1.0 = press/on, 0 = release/off.
    m_deckActionsConnection = QObject::connect(m_parameterStore, &ParameterStore::parameterChanged,
        this, [this](const QString& id, float value)
    {
        DjEngine* const a = m_deckA;
        DjEngine* const b = m_deckB;

        auto engineForDeck = [a, b](QChar deck) -> DjEngine*
        {
            return deck == QLatin1Char('A') ? a : b;
        };

        auto toggleFxSlot = [this, &engineForDeck](QChar deck, int slot)
        {
            DjEngine* const engine = engineForDeck(deck);
            if (!engine || slot < 1 || slot > 3)
                return;

            std::array<bool, 3>& fxSlotStates = deck == QLatin1Char('A')
                ? m_deckAFxSlotsEnabled
                : m_deckBFxSlotsEnabled;
            const int index = slot - 1;
            fxSlotStates[static_cast<size_t>(index)] = !fxSlotStates[static_cast<size_t>(index)];

            static constexpr EffectType kSlotTypes[3] = {
                EffectType::Echo,
                EffectType::Flanger,
                EffectType::Reverb
            };
            const bool enabled = fxSlotStates[static_cast<size_t>(index)];
            engine->setFxSlotEffectType(slot, enabled ? kSlotTypes[index] : EffectType::None);
            engine->setFxSlotWetDry(slot, enabled ? 1.0f : 0.0f);

            qDebug() << "[MIDI ACTION] action=FxSlot"
                     << "deck:" << deck
                     << "slot:" << slot
                     << "enabled:" << enabled
                     << "dispatch=toggleFxSlot";
        };

        auto handleBeatJumpParam = [&engineForDeck](const QString& paramId) -> bool
        {
            QChar deck;
            double beats = 0.0;
            if (parseDeckButtonParam(paramId, QStringLiteral("beatjump_backward"), deck)
                || parseDeckButtonParam(paramId, QStringLiteral("beatjump_4_backward"), deck)) {
                beats = -4.0;
            } else if (parseDeckButtonParam(paramId, QStringLiteral("beatjump_forward"), deck)
                       || parseDeckButtonParam(paramId, QStringLiteral("beatjump_4_forward"), deck)) {
                beats = 4.0;
            } else if (parseDeckButtonParam(paramId, QStringLiteral("beatjump_16_backward"), deck)) {
                beats = -16.0;
            } else if (parseDeckButtonParam(paramId, QStringLiteral("beatjump_16_forward"), deck)) {
                beats = 16.0;
            } else {
                return false;
            }

            if (DjEngine* const engine = engineForDeck(deck))
                engine->beatJump(beats);
            qDebug() << "[MIDI ACTION] action=BeatJump"
                     << "deck:" << deck
                     << "beats:" << beats
                     << "dispatch=beatJump";
            return true;
        };

        auto applyBeatFx = [this, a, b]()
        {
            const EffectType type = beatFxTypeForPosition(m_beatFxPosition);
            const float wet = m_beatFxActive ? 1.0f : 0.0f;

            if (a) {
                a->setFxSlotEffectType(1, type);
                a->setFxSlotWetDry(1, m_beatFxTargetDeck == QLatin1Char('A') ? wet : 0.0f);
            }
            if (b) {
                b->setFxSlotEffectType(1, type);
                b->setFxSlotWetDry(1, m_beatFxTargetDeck == QLatin1Char('B') ? wet : 0.0f);
            }

            qDebug() << "[MIDI ACTION] action=BeatFx"
                     << "position:" << m_beatFxPosition
                     << "targetDeck:" << m_beatFxTargetDeck
                     << "active:" << m_beatFxActive
                     << "dispatch=applyBeatFxSlot1";
        };

        if (id == "deckA_play") {
            if (value >= 0.5f && a)
                a->togglePlay();
        }
        else if (id == "deckB_play") {
            if (value >= 0.5f && b)
                b->togglePlay();
        }
        else if (id == "deckA_cue") {
            const bool currentHeld = value >= 0.5f;
            const bool previousHeld = m_cueAHeld;
            if (currentHeld) {
                if (!previousHeld && a) {
                    m_cueAHeld = true;
                    qDebug() << "[MIDI ACTION] action=TransportCue deck=A"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=cueButtonPress";
                    a->cueButtonPress();
                } else {
                    qDebug() << "[MIDI ACTION] action=TransportCue deck=A"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=ignored-repeat-press";
                }
            } else {
                m_cueAHeld = false;
                qDebug() << "[MIDI ACTION] action=TransportCue deck=A"
                         << "previous:" << previousHeld
                         << "current:" << currentHeld
                         << "dispatch=cueButtonRelease";
                if (a)
                    a->cueButtonRelease();
            }
        }
        else if (id == "deckB_cue") {
            const bool currentHeld = value >= 0.5f;
            const bool previousHeld = m_cueBHeld;
            if (currentHeld) {
                if (!previousHeld && b) {
                    m_cueBHeld = true;
                    qDebug() << "[MIDI ACTION] action=TransportCue deck=B"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=cueButtonPress";
                    b->cueButtonPress();
                } else {
                    qDebug() << "[MIDI ACTION] action=TransportCue deck=B"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=ignored-repeat-press";
                }
            } else {
                m_cueBHeld = false;
                qDebug() << "[MIDI ACTION] action=TransportCue deck=B"
                         << "previous:" << previousHeld
                         << "current:" << currentHeld
                         << "dispatch=cueButtonRelease";
                if (b)
                    b->cueButtonRelease();
            }
        }
        else if (isHotCueParam(id)) {
            if (value < 0.5f)
                return;

            QChar deck;
            int hotCueIndex = -1;
            bool clear = false;
            if (!parseHotCueParam(id, deck, hotCueIndex, clear))
                return;

            DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
            if (!deckEngine)
                return;

            if (clear)
                deckEngine->clearCuePad(hotCueIndex);
            else
                deckEngine->triggerCuePad(hotCueIndex);
        }
        else {
            QChar deck;
            MidiPadMode mode = MidiPadMode::HotCue;
            int padIndex = -1;
            bool clearPad = false;

            if (parsePadModeParam(id, deck, mode)) {
                if (value < 0.5f)
                    return;

                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                if (padModeForDeck(deck) == MidiPadMode::PadFx && mode != MidiPadMode::PadFx)
                    clearPadFxState(deck, deckEngine);
                setPadModeForDeck(deck, mode);
                qDebug() << "[MIDI ACTION] action=PadMode"
                         << "deck:" << deck
                         << "mode:" << id
                         << "dispatch=setPadMode";
                return;
            }

            if (parsePerformancePadParam(id, deck, padIndex, clearPad)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                handlePerformancePad(deck, deckEngine, padIndex, value >= 0.5f, clearPad);
                return;
            }

            if (parseDirectPadParam(id, QStringLiteral("padfx_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                handlePerformancePad(deck, deckEngine, padIndex, value >= 0.5f, false);
                return;
            }

            if (parseDirectPadParam(id, QStringLiteral("beatjump_pad"), deck, padIndex)) {
                DjEngine* const deckEngine = deck == QLatin1Char('A') ? a : b;
                handlePerformancePad(deck, deckEngine, padIndex, value >= 0.5f, false);
                return;
            }

            int fxSlot = -1;
            if (parseDeckFxSlotParam(id, deck, fxSlot)) {
                if (value >= 0.5f)
                    toggleFxSlot(deck, fxSlot);
                return;
            }
        }
        QChar directDeck;
        if (value >= 0.5f && handleBeatJumpParam(id))
            return;
        int beatFxPosition = -1;
        if (parseBeatFxSelectParam(id, beatFxPosition)) {
            if (value >= 0.5f) {
                m_beatFxPosition = beatFxPosition;
                applyBeatFx();
            }
            return;
        }
        if (id == QStringLiteral("beat_fx_channel_deck_a")) {
            if (value >= 0.5f) {
                m_beatFxTargetDeck = QLatin1Char('A');
                applyBeatFx();
            }
            return;
        }
        if (id == QStringLiteral("beat_fx_channel_deck_b")) {
            if (value >= 0.5f) {
                m_beatFxTargetDeck = QLatin1Char('B');
                applyBeatFx();
            }
            return;
        }
        if (id == QStringLiteral("beat_fx_on")) {
            if (value >= 0.5f) {
                m_beatFxActive = !m_beatFxActive;
                applyBeatFx();
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("beat_sync"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setSyncEnabled(!engine->syncEnabled());
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("beatsync"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck)) {
                    if (engine->syncEnabled())
                        engine->reSync();
                    else
                        engine->setSyncEnabled(true);
                }
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("key_sync"), directDeck)
            || parseDeckButtonParam(id, QStringLiteral("keylock"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setKeylock(!engine->keylock());
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("slip"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setSlip(!engine->slipActive());
            }
            return;
        }
        if (parseDeckButtonParam(id, QStringLiteral("tempo_reset"), directDeck)
            || parseDeckButtonParam(id, QStringLiteral("rate_reset"), directDeck)) {
            if (value >= 0.5f) {
                if (DjEngine* const engine = engineForDeck(directDeck))
                    engine->setTempoPercent(0.0);
            }
            return;
        }
        if (id == "deckA_shift") {
            m_deckAShiftHeld = value >= 0.5f;
        }
        else if (id == "deckB_shift") {
            m_deckBShiftHeld = value >= 0.5f;
        }
        else if (id == "deckA_loop_in") {
            if (value >= 0.5f && a) {
                if (a->loopActive()) a->halveLoopLength();
                else a->setLoopIn();
            }
        }
        else if (id == "deckB_loop_in") {
            if (value >= 0.5f && b) {
                if (b->loopActive()) b->halveLoopLength();
                else b->setLoopIn();
            }
        }
        else if (id == "deckA_loop_out") {
            if (value >= 0.5f && a) {
                if (a->loopActive()) a->doubleLoopLength();
                else a->setLoopOut();
            }
        }
        else if (id == "deckB_loop_out") {
            if (value >= 0.5f && b) {
                if (b->loopActive()) b->doubleLoopLength();
                else b->setLoopOut();
            }
        }
        else if (id == "deckA_loop_in_adjust") {
            if (value >= 0.5f && a)
                a->halveLoopLength();
        }
        else if (id == "deckB_loop_in_adjust") {
            if (value >= 0.5f && b)
                b->halveLoopLength();
        }
        else if (id == "deckA_loop_out_adjust") {
            if (value >= 0.5f && a)
                a->doubleLoopLength();
        }
        else if (id == "deckB_loop_out_adjust") {
            if (value >= 0.5f && b)
                b->doubleLoopLength();
        }
        else if (id == "deckA_loop_reloop") {
            if (value >= 0.5f && a) {
                if (a->loopActive())
                    a->deactivateLoop();
                else if (m_deckAShiftHeld)
                    a->reactivateLoop();
            }
        }
        else if (id == "deckB_loop_reloop") {
            if (value >= 0.5f && b) {
                if (b->loopActive())
                    b->deactivateLoop();
                else if (m_deckBShiftHeld)
                    b->reactivateLoop();
            }
        }
        else if (id == "deckA_loop_4beat") {
            if (value >= 0.5f && a) {
                if (m_deckAShiftHeld) {
                    if (a->loopActive())
                        a->deactivateLoop();
                    else
                        a->reactivateLoop();
                } else {
                    a->setLoop4Beats();
                }
            }
        }
        else if (id == "deckB_loop_4beat") {
            if (value >= 0.5f && b) {
                if (m_deckBShiftHeld) {
                    if (b->loopActive())
                        b->deactivateLoop();
                    else
                        b->reactivateLoop();
                } else {
                    b->setLoop4Beats();
                }
            }
        }
        else if (id == "deckA_headphone_cue") {
            if (value >= 0.5f && a) a->setCueEnabled(!a->cueEnabled());
        }
        else if (id == "deckB_headphone_cue") {
            if (value >= 0.5f && b) b->setCueEnabled(!b->cueEnabled());
        }
        else if (id == "master_cue") {
            if (value >= 0.5f && a) a->setMasterCueEnabled(!a->masterCueEnabled());
        }
        else if (id == "headphone_mix") {
            if (a) a->setHeadphoneMix(static_cast<double>(value));
        }
        // Trim: MIDI 0-1 → engine 0-2 (center = 1.0 = unity)
        else if (id == "deckA_gain")   { if (a) a->setTrim(static_cast<double>(value) * 2.0); }
        else if (id == "deckB_gain")   { if (b) b->setTrim(static_cast<double>(value) * 2.0); }
        // EQ/filter: MIDI 0-1 → engine -1 to +1 (center = 0.0)
        else if (id == "deckA_eqHigh") { if (a) a->setEqHigh(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckB_eqHigh") { if (b) b->setEqHigh(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckA_eqMid")  { if (a) a->setEqMid(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckB_eqMid")  { if (b) b->setEqMid(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckA_eqLow")  { if (a) a->setEqLow(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckB_eqLow")  { if (b) b->setEqLow(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckA_filter") { if (a) a->setFilter(static_cast<double>(value) * 2.0 - 1.0); }
        else if (id == "deckB_filter") { if (b) b->setFilter(static_cast<double>(value) * 2.0 - 1.0); }
        // Tempo fader: MIDI 0-1 -> current deck tempo range.
        else if (id == "deckA_tempo")  {
            if (a) {
                const double range = std::max(1.0, a->tempoRangePercent());
                a->setTempoPercent(static_cast<double>(value) * 2.0 * range - range);
            }
        }
        else if (id == "deckB_tempo")  {
            if (b) {
                const double range = std::max(1.0, b->tempoRangePercent());
                b->setTempoPercent(static_cast<double>(value) * 2.0 * range - range);
            }
        }
        // Jog touch: 127 = finger down (enter scratch), 0 = lift (resume)
        else if (id == "deckA_jog_touch") {
            const bool currentHeld = value >= 0.5f;
            const bool previousHeld = m_jogATouched;
            if (currentHeld) {
                if (!previousHeld && a) {
                    m_jogATouched = true;
                    m_jogAReleasedRecently = false;
                    m_jogAReleaseTimer.stop();
                    m_scratchAbsoluteLastByMsgId.clear();
                    qDebug() << "[MIDI ACTION] action=JogTouch deck=A"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=pauseForScrub";
                    a->pauseForScrub();
                } else {
                    qDebug() << "[MIDI ACTION] action=JogTouch deck=A"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=ignored-repeat-press";
                }
            } else {
                m_jogATouched = false;
                m_scratchAbsoluteLastByMsgId.clear();
                qDebug() << "[MIDI ACTION] action=JogTouch deck=A"
                         << "previous:" << previousHeld
                         << "current:" << currentHeld
                         << "dispatch=resume-scrub-open-real-tick-window";
                if (a && a->isScrubbing())
                    a->resumeAfterScrub();
                m_jogAReleasedRecently = true;
                m_jogAReleaseTimer.start();
            }
        }
        else if (id == "deckB_jog_touch") {
            const bool currentHeld = value >= 0.5f;
            const bool previousHeld = m_jogBTouched;
            if (currentHeld) {
                if (!previousHeld && b) {
                    m_jogBTouched = true;
                    m_jogBReleasedRecently = false;
                    m_jogBReleaseTimer.stop();
                    m_scratchAbsoluteLastByMsgId.clear();
                    qDebug() << "[MIDI ACTION] action=JogTouch deck=B"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=pauseForScrub";
                    b->pauseForScrub();
                } else {
                    qDebug() << "[MIDI ACTION] action=JogTouch deck=B"
                             << "previous:" << previousHeld
                             << "current:" << currentHeld
                             << "dispatch=ignored-repeat-press";
                }
            } else {
                m_jogBTouched = false;
                m_scratchAbsoluteLastByMsgId.clear();
                qDebug() << "[MIDI ACTION] action=JogTouch deck=B"
                         << "previous:" << previousHeld
                         << "current:" << currentHeld
                         << "dispatch=resume-scrub-open-real-tick-window";
                if (b && b->isScrubbing())
                    b->resumeAfterScrub();
                m_jogBReleasedRecently = true;
                m_jogBReleaseTimer.start();
            }
        }
        // Jog wheels mirror the Mixxx FLX10 mapping: CC 0x22 is the touched
        // top-platter stream; CC 0x21 is rim / post-release free spin. While the
        // finger is down only CC 0x22 is used for scratch — routing both would
        // double the travel and spin the UI handle too fast.
        else if (id == "deckA_jog_nudge") {
            if (a) {
                if (m_jogATouched)
                    return;
                if (a->isScratchReleaseActive()) {
                    if (std::abs(value) > 0.0f) {
                        m_jogAReleaseTimer.start();
                        a->applyScratchReleaseJog(flx10ScratchDeltaSec(static_cast<double>(value)));
                    }
                } else if (m_jogAReleasedRecently || a->isScrubbing()) {
                    if (!a->isScrubbing() && std::abs(value) <= 0.0f)
                        return;
                    if (!a->isScrubbing())
                        a->pauseForScrub();
                    if (std::abs(value) > 0.0f)
                        m_jogAReleaseTimer.start();
                    a->scratchBySeconds(flx10ScratchDeltaSec(static_cast<double>(value)), true);
                } else {
                    a->applyJogNudge(static_cast<double>(value));
                }
            }
        }
        else if (id == "deckB_jog_nudge") {
            if (b) {
                if (m_jogBTouched)
                    return;
                if (b->isScratchReleaseActive()) {
                    if (std::abs(value) > 0.0f) {
                        m_jogBReleaseTimer.start();
                        b->applyScratchReleaseJog(flx10ScratchDeltaSec(static_cast<double>(value)));
                    }
                } else if (m_jogBReleasedRecently || b->isScrubbing()) {
                    if (!b->isScrubbing() && std::abs(value) <= 0.0f)
                        return;
                    if (!b->isScrubbing())
                        b->pauseForScrub();
                    if (std::abs(value) > 0.0f)
                        m_jogBReleaseTimer.start();
                    b->scratchBySeconds(flx10ScratchDeltaSec(static_cast<double>(value)), true);
                } else {
                    b->applyJogNudge(static_cast<double>(value));
                }
            }
        }
        else if (id == "deckA_jog_scratch") {
            if (a && std::abs(value) > 0.0f) {
                // CC 0x22 is the vinyl-on platter stream; engage scratch even if the
                // touch Note arrived a tick later than the first relative CC.
                if (!m_jogATouched) {
                    m_jogATouched = true;
                    m_jogAReleasedRecently = false;
                    m_jogAReleaseTimer.stop();
                    m_scratchAbsoluteLastByMsgId.clear();
                }
                if (!a->isScrubbing())
                    a->pauseForScrub();
                a->scratchBySeconds(flx10ScratchDeltaSec(static_cast<double>(value)), true);
            }
        }
        else if (id == "deckB_jog_scratch") {
            if (b && std::abs(value) > 0.0f) {
                if (!m_jogBTouched) {
                    m_jogBTouched = true;
                    m_jogBReleasedRecently = false;
                    m_jogBReleaseTimer.stop();
                    m_scratchAbsoluteLastByMsgId.clear();
                }
                if (!b->isScrubbing())
                    b->pauseForScrub();
                b->scratchBySeconds(flx10ScratchDeltaSec(static_cast<double>(value)), true);
            }
        }
        else if (id == "deckA_jog_move") {
            const double delta = flx10ScratchDeltaSec(static_cast<double>(value));
            if (a) {
                if (m_jogATouched || a->isScrubbing()) {
                    if (!a->isScrubbing())
                        a->pauseForScrub();
                    qDebug() << "[MIDI ACTION] action=JogMove deck=A"
                             << "ticks:" << value
                             << "deltaSec:" << delta
                             << "touched:" << m_jogATouched
                             << "scrubbing:" << a->isScrubbing()
                             << "dispatch=scratchBySeconds";
                    a->scratchBySeconds(delta, true);
                } else {
                    qDebug() << "[MIDI ACTION] action=JogMove deck=A"
                             << "ticks:" << value
                             << "touched:" << m_jogATouched
                             << "scrubbing:" << a->isScrubbing()
                             << "dispatch=applyJogNudge";
                    a->applyJogNudge(static_cast<double>(value));
                }
            }
        }
        else if (id == "deckB_jog_move") {
            const double delta = flx10ScratchDeltaSec(static_cast<double>(value));
            if (b) {
                if (m_jogBTouched || b->isScrubbing()) {
                    if (!b->isScrubbing())
                        b->pauseForScrub();
                    qDebug() << "[MIDI ACTION] action=JogMove deck=B"
                             << "ticks:" << value
                             << "deltaSec:" << delta
                             << "touched:" << m_jogBTouched
                             << "scrubbing:" << b->isScrubbing()
                             << "dispatch=scratchBySeconds";
                    b->scratchBySeconds(delta, true);
                } else {
                    qDebug() << "[MIDI ACTION] action=JogMove deck=B"
                             << "ticks:" << value
                             << "touched:" << m_jogBTouched
                             << "scrubbing:" << b->isScrubbing()
                             << "dispatch=applyJogNudge";
                    b->applyJogNudge(static_cast<double>(value));
                }
            }
        }
    });
}
