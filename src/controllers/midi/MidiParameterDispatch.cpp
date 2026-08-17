#include "MidiControllerManager.h"
#include "MidiParameterDispatch.h"

using namespace midi;

#include "ParameterStore.h"

#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace {

// Human-readable label for the raw message id this file encodes controls into.
// Ids below 10000 are channel-agnostic; above that the channel is folded in.
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


bool isChannelFaderParameter(const QString& paramId)
{
    return paramId == QStringLiteral("deckA_vol")
        || paramId == QStringLiteral("deckB_vol")
        || paramId == QStringLiteral("deckC_vol")
        || paramId == QStringLiteral("deckD_vol");
}

} // namespace

void MidiControllerManager::processDecodedMidiEvent(int msgId, float value, bool isNoteOff,
                                                    double eventTimestampSeconds)
{
    // Our own lamp writes must never be mistaken for a press. Checked on the
    // local clock because the input paths timestamp events on different ones,
    // and the write was stamped here too.
    if (!isNoteOff && ((msgId >= 10000 && (msgId - 10000) % 2000 < 1000) || msgId < 1000)) {
        const int rawValue = midi::clampMidi7bit(static_cast<int>(std::lround(value * 127.0f)));
        if (m_outputEchoGuard.isEcho(msgId, rawValue,
                                     juce::Time::getMillisecondCounterHiRes() * 0.001)) {
            if (m_midiTraceEnabled)
                qDebug() << "[MIDI IN] dropped own LED echo for" << midiControlLabel(msgId);
            return;
        }
    }

    if (!isNoteOff && msgId >= 10000) {
        const int remainder = msgId - 10000;
        const int channel = remainder / 2000;
        const int sub = remainder % 2000;
        if (channel >= 0 && channel < 2 && (sub == 1000 || sub == 1032)
            && !m_tempoRawInputSeen[static_cast<size_t>(channel)]) {
            m_tempoRawInputSeen[static_cast<size_t>(channel)] = true;
        }
    }

    // Keep the UI monitor useful without allocating QStrings and emitting a Qt
    // signal for every high-resolution jog tick.
    const double monitorNowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (m_isLearning || monitorNowSeconds >= m_nextMidiMonitorUpdateSeconds) {
        m_nextMidiMonitorUpdateSeconds = monitorNowSeconds + 1.0 / 30.0;
        const int sub  = (msgId >= 10000) ? (msgId - 10000) % 2000 : -1;
        const int chNo = (msgId >= 10000) ? (msgId - 10000) / 2000 : 0;
        QString evtLabel;
        if (isNoteOff)        evtLabel = QStringLiteral("Ch%1 NoteOff %2").arg(chNo+1).arg(sub);
        else if (sub == 1500) evtLabel = QStringLiteral("Ch%1 PitchBend").arg(chNo+1);
        else if (sub >= 1000) {
            const auto mappingIt = m_midiToParam.find(msgId);
            const bool relative = mappingIt != m_midiToParam.end()
                && midi::isRelativeInteraction(mappingIt->second.interactionType);
            if (relative) {
                const int ticks = static_cast<int>(std::lround(value));
                const QString tickText = ticks > 0
                    ? QStringLiteral("+%1").arg(ticks)
                    : QString::number(ticks);
                evtLabel = QStringLiteral("Ch%1 CC %2 ticks %3")
                    .arg(chNo + 1).arg(sub - 1000).arg(tickText);
            } else {
                evtLabel = QStringLiteral("Ch%1 CC %2 = %3")
                    .arg(chNo + 1).arg(sub - 1000)
                    .arg(static_cast<int>(std::lround(value * 127.0f)));
            }
        }
        else if (sub >= 0)    evtLabel = QStringLiteral("Ch%1 Note %2  vel %3").arg(chNo+1).arg(sub).arg(static_cast<int>(value*127));

        if (m_midiTraceEnabled)
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

    auto dispatchTouchedAbsoluteJogFallback = [this, msgId, value, eventTimestampSeconds]() -> bool
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

        const int raw = midi::clampMidi7bit(static_cast<int>(std::round(value * 127.0f)));
        const auto previousIt = m_scratchAbsoluteLastByMsgId.find(msgId);

        if (previousIt == m_scratchAbsoluteLastByMsgId.end()) {
            m_scratchAbsoluteLastByMsgId[msgId] = raw;
            if (m_midiTraceEnabled) qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                     << "value:" << raw
                     << "mappedAction:" << scratchParamId
                     << "interactionType:touched-absolute-jog"
                     << "dispatch=baseline";
            return true;
        }

        const int previousRaw = previousIt->second;
        m_scratchAbsoluteLastByMsgId[msgId] = raw;

        float delta = midi::decodeWrappedAbsoluteDelta(previousRaw, raw);
        const auto invIt = m_paramInverted.find(pairedParamId);
        if (invIt != m_paramInverted.end() && invIt->second)
            delta = -delta;

        if (m_midiTraceEnabled) qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                 << "value:" << raw
                 << "previous:" << previousRaw
                 << "deltaTicks:" << delta
                 << "mappedAction:" << scratchParamId
                 << "interactionType:touched-absolute-jog"
                 << "dispatch:jogMoveFallback";

        if (delta == 0.0f)
            return true;

        dispatchFlx10JogAction(scratchParamId, delta, eventTimestampSeconds);
        return true;
    };

    // 14-bit CC handling. The FLX10 sends tempo LSB first (CC 32), then MSB
    // (CC 0); other controllers may use the opposite order.
    {
        const int sub = (msgId >= 10000) ? (msgId - 10000) % 2000 : -1;
        auto dispatchPair = [this](const QString& paramId, int value14)
        {
            m_pending14BitMsbFallbacks.erase(paramId);
            if (isChannelFaderParameter(paramId))
                m_channelFaderMsbGates[paramId].confirmPair();
            float combined = static_cast<float>(value14) / 16383.0f;
            const auto invIt = m_paramInverted.find(paramId);
            if (invIt != m_paramInverted.end() && invIt->second)
                combined = 1.0f - combined;
            dispatchToStore(paramId, combined, ParameterStoreDispatch::Standard);
        };

        if (!isNoteOff && sub >= 1000 && sub < 1032) {
            const auto msbIt = m_midiToParam.find(msgId);
            if (msbIt != m_midiToParam.end()) {
                const QString& paramId = msbIt->second.paramId;
                auto& accumulator = m_14BitAccumulators[paramId];
                const int rawMsb = midi::clampMidi7bit(
                    static_cast<int>(std::lround(value * 127.0f)));
                accumulator.pushMsb(rawMsb);
                if (const auto value14 = accumulator.takeValue()) {
                    dispatchPair(paramId, *value14);
                    return;
                }

                const auto lsbIt = m_midiToParam.find(msgId + 32);
                if (lsbIt != m_midiToParam.end()
                    && lsbIt->second.paramId == paramId
                    && !midi::shouldAlwaysDispatch(msbIt->second.interactionType)) {
                    // A lone startup MSB from an FLX10 mixer port is not a
                    // trustworthy channel-fader snapshot. A complete pair is
                    // still accepted above; coarse fallback begins as soon as
                    // a changed MSB proves that the physical fader moved.
                    if (isChannelFaderParameter(paramId)
                        && !m_channelFaderMsbGates[paramId].shouldPublish(rawMsb)) {
                        return;
                    }

                    float fallback = static_cast<float>(rawMsb) / 127.0f;
                    const auto invIt = m_paramInverted.find(paramId);
                    if (invIt != m_paramInverted.end() && invIt->second)
                        fallback = 1.0f - fallback;
                    m_pending14BitMsbFallbacks[paramId] = fallback;
                    if (!m_14BitFallbackTimer.isActive())
                        m_14BitFallbackTimer.start();
                    return;
                }
            }
            // Controls without an explicitly mapped LSB retain immediate 7-bit dispatch.
        } else if (!isNoteOff && sub >= 1032 && sub < 1064) {
            const auto currentIt = m_midiToParam.find(msgId);
            const bool currentIsDiscrete = currentIt != m_midiToParam.end()
                && midi::shouldAlwaysDispatch(currentIt->second.interactionType);
            const int msbMsgId = msgId - 32; // paired MSB is always 32 less
            const auto msbIt = m_midiToParam.find(msbMsgId);
            if (!currentIsDiscrete
                && msbIt != m_midiToParam.end()
                && !midi::shouldAlwaysDispatch(msbIt->second.interactionType)) {
                const QString& paramId = msbIt->second.paramId;
                auto& accumulator = m_14BitAccumulators[paramId];
                accumulator.pushLsb(static_cast<int>(std::lround(value * 127.0f)));
                if (const auto value14 = accumulator.takeValue())
                    dispatchPair(paramId, *value14);
                return; // wait for MSB or publish the now-complete pair
            }
            // No paired MSB found → fall through to standard dispatch
        }
    }

    const auto it = m_midiToParam.find(msgId);
    if (it == m_midiToParam.end()) {
        if (dispatchTouchedAbsoluteJogFallback())
            return;

        if (m_midiTraceEnabled) qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
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
            if (midi::isRelativeInteraction(interactionType))
                dispatchValue = -dispatchValue;
            else if (!isNoteOff && !midi::isButtonInteraction(interactionType))
                dispatchValue = 1.0f - dispatchValue;
        }
    }
    const bool pressed = dispatchValue > 0.0f;
    if (midi::isButtonInteraction(interactionType)) {
        const int rawMidiValue = static_cast<int>(std::round(value * 127.0f));

        if (interactionType == MidiInteractionType::Toggle && !pressed) {
            if (m_midiTraceEnabled) qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                     << "value:" << rawMidiValue
                     << "interpreted:released"
                     << "mappedAction:" << paramId
                     << "interactionType:" << midi::interactionTypeToString(interactionType)
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

            if (m_midiTraceEnabled) qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                     << "value:" << rawMidiValue
                     << "interpreted:" << (currentHeld ? "pressed" : "released")
                     << "mappedAction:" << paramId
                     << "interactionType:" << midi::interactionTypeToString(interactionType)
                     << "previous:" << previousHeld
                     << "current:" << currentHeld
                     << "dispatch:" << dispatchName;

            if (!changed && !forceRelease)
                return;
        } else {
            if (m_midiTraceEnabled) qDebug() << "[MIDI MAP]" << midiControlLabel(msgId)
                     << "value:" << rawMidiValue
                     << "interpreted:" << (pressed ? "pressed" : "released")
                     << "mappedAction:" << paramId
                     << "interactionType:" << midi::interactionTypeToString(interactionType)
                     << "previous:n/a"
                     << "current:n/a"
                     << "dispatch:press";
        }

        dispatchValue = pressed ? 1.0f : 0.0f;
    }

    // Relative encoders and buttons can produce repeated identical values that
    // are semantically distinct events, so always emit them. Analog controls
    // use deduplication to avoid MIDI feedback echo loops.
    if (midi::isFlx10JogInputParam(paramId)
        && dispatchFlx10JogAction(paramId, dispatchValue, eventTimestampSeconds)) {
        return;
    }

    if (midi::shouldAlwaysDispatch(interactionType)) {
        dispatchToStore(paramId, dispatchValue, ParameterStoreDispatch::Midi);
    } else {
        dispatchToStore(paramId, dispatchValue, ParameterStoreDispatch::Standard);
    }
}

void MidiControllerManager::dispatchToStore(const QString& paramId, float value, ParameterStoreDispatch method)
{
    if (!m_parameterStore)
        return;

    const bool onStoreThread = QThread::currentThread() == m_parameterStore->thread();

    if (method == ParameterStoreDispatch::Midi) {
        if (onStoreThread) {
            m_parameterStore->setMidiParameter(paramId, value);
            return;
        }
        QMetaObject::invokeMethod(m_parameterStore, "setMidiParameter", Qt::QueuedConnection,
                                  Q_ARG(QString, paramId),
                                  Q_ARG(float, value));
        return;
    }

    if (onStoreThread) {
        m_parameterStore->setParameter(paramId, value);
        return;
    }

    QMetaObject::invokeMethod(m_parameterStore, "setParameter", Qt::QueuedConnection,
                              Q_ARG(QString, paramId),
                              Q_ARG(float, value));
}

void MidiControllerManager::enqueueRawMidiEvent(int msgId,
                                                float rawEncodedValue,
                                                bool noteOff,
                                                double eventTimestampSeconds)
{
    bool scheduleDrain = false;
    {
        std::lock_guard lock(m_pendingMidiMutex);
        if (m_pendingMidiCount == kPendingMidiCapacity) {
            m_pendingMidiHead = (m_pendingMidiHead + 1) % kPendingMidiCapacity;
            --m_pendingMidiCount;
            m_droppedMidiEvents.fetch_add(1, std::memory_order_relaxed);
        }

        const std::size_t tail =
            (m_pendingMidiHead + m_pendingMidiCount) % kPendingMidiCapacity;
        m_pendingMidiEvents[tail] = {
            msgId, rawEncodedValue, eventTimestampSeconds, noteOff
        };
        ++m_pendingMidiCount;
        if (!m_midiDrainScheduled) {
            m_midiDrainScheduled = true;
            scheduleDrain = true;
        }
    }

    if (scheduleDrain) {
        QMetaObject::invokeMethod(this, [this] { drainRawMidiEvents(); },
                                  Qt::QueuedConnection);
    }
}

void MidiControllerManager::drainRawMidiEvents()
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    std::array<PendingMidiEvent, kMidiDrainBatchSize> batch {};
    std::size_t batchSize = 0;
    bool scheduleNextBatch = false;
    {
        std::lock_guard lock(m_pendingMidiMutex);
        batchSize = std::min(m_pendingMidiCount, kMidiDrainBatchSize);
        for (std::size_t i = 0; i < batchSize; ++i) {
            batch[i] = m_pendingMidiEvents[m_pendingMidiHead];
            m_pendingMidiHead = (m_pendingMidiHead + 1) % kPendingMidiCapacity;
        }
        m_pendingMidiCount -= batchSize;
        scheduleNextBatch = m_pendingMidiCount != 0;
        if (!scheduleNextBatch)
            m_midiDrainScheduled = false;
    }

    for (std::size_t i = 0; i < batchSize; ++i) {
        const auto& event = batch[i];
        processRawMidiEvent(event.msgId,
                            event.rawEncodedValue,
                            event.noteOff,
                            event.timestampSeconds);
    }

    if (scheduleNextBatch && !m_shutdownComplete.load(std::memory_order_acquire)) {
        QMetaObject::invokeMethod(this, [this] { drainRawMidiEvents(); },
                                  Qt::QueuedConnection);
    }
}

void MidiControllerManager::processRawMidiEvent(int msgId,
                                                float rawEncodedValue,
                                                bool noteOff,
                                                double eventTimestampSeconds)
{
    int resolvedId = msgId;
    float resolvedValue = rawEncodedValue;

    if (msgId >= 10000) {
        const int sub = (msgId - 10000) % 2000;

        if (sub >= 1000 && sub < 1500) {
            const int cc = sub - 1000;
            const int legacyId = cc + 1000;
            if (!m_midiToParam.count(msgId) && m_midiToParam.count(legacyId))
                resolvedId = legacyId;

            const auto it = m_midiToParam.find(resolvedId);
            if (it != m_midiToParam.end()
                && midi::isRelativeInteraction(it->second.interactionType)) {
                resolvedValue = midi::decodeRelativeCcValue(
                    static_cast<int>(rawEncodedValue), it->second.paramId);
            } else {
                resolvedValue = midi::clampMidi7bit(
                    static_cast<int>(rawEncodedValue)) / 127.0f;
            }
        } else if (sub == 1500) {
            constexpr int legacyId = 1500;
            if (!m_midiToParam.count(msgId) && m_midiToParam.count(legacyId))
                resolvedId = legacyId;
            resolvedValue = rawEncodedValue / 16383.0f;
        }
    }

    processDecodedMidiEvent(resolvedId, resolvedValue, noteOff,
                            eventTimestampSeconds);
}

void MidiControllerManager::handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message)
{
    if (m_shutdownComplete.load(std::memory_order_acquire))
        return;

    // Diagnostic: log every incoming MIDI message so we can see if JUCE is even
    // receiving Note On events.  The raw status byte tells us the truth.
    if (m_midiTraceEnabled) {
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
    const double juceTimestampSeconds = message.getTimeStamp();
    const double eventTimestampSeconds = std::isfinite(juceTimestampSeconds)
            && juceTimestampSeconds > 0.0
        ? juceTimestampSeconds
        : juce::Time::getMillisecondCounterHiRes() * 0.001;

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

    enqueueRawMidiEvent(msgId, rawEnc, noteOff, eventTimestampSeconds);
}
