#include "SamplerController.h"
#include "AudioEngine.h"
#include "deck/DjEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <QtConcurrent/QtConcurrent>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>

namespace {
QVariantMap playbackMap(const SamplerAudioEngine::PlaybackSnapshot& value) {
    return {{"position", value.positionSeconds}, {"duration", value.durationSeconds},
        {"progress", value.progress}, {"remainingBeats", value.remainingBeats},
        {"generation", QVariant::fromValue<qulonglong>(value.generation)}};
}
}
QVariantList SamplerController::playback() const {
    QVariantList result;
    for (int pad = 0; pad < 8; ++pad) result.append(playbackMap(m_playbackCache[m_bank * 8 + pad]));
    return result;
}
QVariantList SamplerController::bankActivity() const {
    QVariantList result;
    for (int bank = 0; bank < 4; ++bank) {
        int playing = 0, queued = 0;
        for (int pad = 0; pad < 8; ++pad) {
            const auto state = m_playbackCache[bank * 8 + pad].state;
            playing += state == SamplerAudioEngine::Playing;
            queued += state == SamplerAudioEngine::Pending;
        }
        result.append(QVariantMap {{"playing", playing}, {"queued", queued}});
    }
    return result;
}
void SamplerController::refreshPlayback() {
    bool statesChanged = false;
    for (int i = 0; i < 32; ++i) {
        const auto snapshot = m_audio.playback(i);
        if (snapshot.valid) m_playbackCache[i] = snapshot;
        const int state = m_audio.state(i);
        if (state != m_lastStates[i]) { m_lastStates[i] = state; statesChanged = true; }
    }
    if (m_lastUiBpm != m_clock.bpm || m_lastUiClock != m_clockSource) {
        m_lastUiBpm = m_clock.bpm; m_lastUiClock = m_clockSource; statesChanged = true;
    }
    if (!previewAvailable() && m_audio.previewPlaying()) stopEditorPreview();
    m_audio.collectStoppedPreview();
    if (statesChanged) emit changed();
    emit playbackChanged();
    emit previewChanged();
}
void SamplerController::setMidiGainNormalized(double value) {
    if (!std::isfinite(value)) return;
    const bool wasPending = midiPickupPending();
    m_midiGainSeen = true;
    if (m_midiPickup.accept(value, m_gain / 1.5)) {
        m_applyingMidiGain = true; setGain(value * 1.5); m_applyingMidiGain = false;
    }
    if (wasPending != midiPickupPending()) emit changed();
}
void SamplerController::resetMidiPickup() { m_midiGainSeen = false; m_midiPickup.disconnect(); emit changed(); }
bool SamplerController::previewAvailable() const {
    const int headphones = AudioEngine::headphonesFirstChannel();
    const int master = AudioEngine::masterFirstChannel(), booth = AudioEngine::boothFirstChannel();
    return m_audio.isPrepared() && headphones > 0 && (master <= 0 || std::abs(headphones - master) >= 2)
        && (booth <= 0 || std::abs(headphones - booth) >= 2);
}
QVariantMap SamplerController::previewPlayback() const { return playbackMap(m_audio.previewPlayback()); }
void SamplerController::setEditorError(const QString& error) { m_editorError = error; emit editorChanged(); }
bool SamplerController::validEditor(std::uint64_t session) const {
    return !m_stopped && session && session == m_editorSession && m_editorSlot >= 0
        && m_slots[m_editorSlot].generation == m_editorGeneration;
}
QVariantMap SamplerController::beginEditor(int pad) {
    cancelEditor(m_editorSession);
    if (m_stopped || pad < 0 || pad >= 8) return {};
    const int index = m_bank * 8 + pad;
    if (m_slots[index].loading) { setEditorError("This sample is still loading"); return {}; }
    const auto& slot = m_slots[index];
    m_editorSession = ++m_nextEditorSession; m_editorSlot = index; m_editorGeneration = slot.generation;
    m_editorSource = slot.asset.isEmpty() ? QString {} : QDir(m_directory).filePath(slot.asset);
    m_editorSourceDuration = slot.duration; m_editorSourceRate = slot.sampleRate;
    m_editorError.clear(); m_editorCapture.clear();
    setSelectedSlot(pad);
    emit editorChanged();
    return {{"session", QVariant::fromValue<qulonglong>(m_editorSession)}, {"slot", index},
        {"source", m_editorSource}, {"duration", slot.duration}, {"sampleRate", slot.sampleRate},
        {"name", slot.name}, {"color", slot.color}, {"gain", slot.gain}, {"loop", slot.loop},
        {"sync", slot.sync}, {"bpm", slot.bpm}, {"start", slot.start},
        {"end", slot.end > slot.start ? slot.end : slot.duration}, {"waveform", slot.waveform},
        {"loaded", !slot.asset.isEmpty()}};
}
void SamplerController::stopEditorPreview() {
    ++m_previewGeneration;
    if (m_previewCancel) m_previewCancel->store(true);
    m_previewLoading = false; m_audio.stopPreview(); emit previewChanged();
}
void SamplerController::cancelEditor(qulonglong session) {
    if (!session || session != m_editorSession) return;
    stopEditorPreview();
    if (m_waveCancel) m_waveCancel->store(true);
    ++m_waveGeneration;
    if (m_editorProbeGeneration == m_probeGeneration) ++m_probeGeneration;
    if (m_editorSaving && m_editorSlot >= 0 && m_slots[m_editorSlot].generation == m_editorGeneration) {
        ++m_slots[m_editorSlot].generation; m_slots[m_editorSlot].loading = false;
        emit changed();
    }
    m_editorSession = 0; m_editorSlot = -1; m_editorSaving = m_editorLoading = false;
    m_editorSource.clear(); m_editorError.clear(); emit editorChanged();
}
void SamplerController::setEditorFile(qulonglong session, const QString& path) {
    if (!validEditor(session) || m_editorSaving) return;
    stopEditorPreview();
    if (m_waveCancel) m_waveCancel->store(true);
    ++m_waveGeneration;
    m_editorSource = localPath(path); m_editorSourceDuration = 0;
    m_editorLoading = true; m_editorError.clear(); m_editorCapture.clear(); emit editorChanged();
    probeFile(m_editorSource); m_editorProbeGeneration = m_probeGeneration;
}
void SamplerController::editorProbeFinished(std::uint64_t probeGeneration) {
    if (!m_editorSession || !m_editorLoading || probeGeneration != m_editorProbeGeneration) return;
    m_editorLoading = false;
    if (!validEditor(m_editorSession)) { setEditorError("This sample has changed. Close and reopen the editor."); return; }
    m_editorError = m_fileInfo.value("error").toString();
    if (m_editorError.isEmpty()) {
        m_editorSourceDuration = m_fileInfo.value("duration").toDouble();
        m_editorSourceRate = m_fileInfo.value("sampleRate", 44100).toDouble();
        auto info = m_fileInfo; info.insert("source", m_editorSource);
        if (!m_editorCapture.isEmpty()) {
            for (auto it = m_editorCapture.cbegin(); it != m_editorCapture.cend(); ++it) info.insert(it.key(), it.value());
            m_editorCapture.clear();
        }
        emit editorSourceReady(m_editorSession, info);
    }
    emit editorChanged();
}
bool SamplerController::validateEditor(std::uint64_t session, const QVariantMap& draft) {
    if (!validEditor(session)) { setEditorError("This sample has changed. Close and reopen the editor."); return false; }
    if (editorBusy()) { setEditorError("Wait for the current sample operation to finish"); return false; }
    const double start = draft.value("start").toDouble(), end = draft.value("end").toDouble();
    const double gain = draft.value("gain", 1).toDouble(), bpm = draft.value("bpm").toDouble();
    if (m_editorSource.isEmpty() || !std::isfinite(start) || !std::isfinite(end)
        || start < 0 || end > m_editorSourceDuration + 0.000001 || end <= start
        || std::llround(end * m_editorSourceRate) - std::llround(start * m_editorSourceRate) < 2
        || end - start > 30.0001) { setEditorError("Choose a valid range of 2 frames to 30 seconds"); return false; }
    if (!std::isfinite(gain) || gain < 0 || gain > 1.5 || !std::isfinite(bpm)
        || (draft.value("loop").toBool() && draft.value("sync").toBool() && (bpm < 20 || bpm > 400))) {
        setEditorError("Sync requires source BPM between 20 and 400; gain must be 0–150%"); return false;
    }
    m_editorError.clear(); emit editorChanged(); return true;
}
void SamplerController::applyEditor(qulonglong session, const QVariantMap& draft) {
    if (!validateEditor(session, draft)) return;
    stopEditorPreview();
    auto desired = m_slots[m_editorSlot];
    desired.name = draft.value("name").toString().left(128);
    desired.color = QColor(draft.value("color").toString()).isValid() ? draft.value("color").toString() : "#e0a344";
    desired.gain = draft.value("gain", 1).toDouble(); desired.bpm = draft.value("bpm").toDouble();
    desired.loop = draft.value("loop").toBool(); desired.sync = desired.loop && draft.value("sync").toBool();
    const bool imported = desired.asset.isEmpty()
        || QFileInfo(m_editorSource).absoluteFilePath() != QFileInfo(QDir(m_directory).filePath(desired.asset)).absoluteFilePath();
    m_editorSaving = true; emit editorChanged();
    load(m_editorSlot, m_editorSource, draft.value("start").toDouble(), draft.value("end").toDouble(), imported, desired, session);
    m_editorGeneration = m_slots[m_editorSlot].generation;
}
void SamplerController::finishEditorSave(std::uint64_t session, bool success, const QString& error) {
    if (!session || session != m_editorSession) return;
    m_editorSaving = false;
    if (success) cancelEditor(session);
    else setEditorError(error);
    emit editorChanged(); emit editorFinished(session, success, error);
}
void SamplerController::clearEditor(qulonglong session) {
    if (!validEditor(session) || editorBusy()) return;
    stopEditorPreview();
    if (m_waveCancel) m_waveCancel->store(true);
    const bool success = clearSlot(m_editorSlot);
    m_editorGeneration = m_slots[m_editorSlot].generation;
    finishEditorSave(session, success, success ? QString {} : m_error);
}
void SamplerController::captureEditor(qulonglong session, int deckIndex, int beats) {
    if (!validEditor(session) || m_editorSaving || deckIndex < 0 || deckIndex >= 4) return;
    const auto* deck = m_decks[deckIndex];
    if (!deck || !deck->hasTrack()) { setEditorError("Load a track on the selected deck first"); return; }
    const double bpm = deck->getCurrentBpm() / std::max(0.01, deck->getTempoRatio());
    if (!std::isfinite(bpm) || bpm < 20 || bpm > 400) { setEditorError("Correct the source BPM before capturing beats"); return; }
    if (beats != 1 && beats != 2 && beats != 4 && beats != 8 && beats != 16 && beats != 32) beats = 4;
    const double start = deck->loopActive() ? deck->loopInPosition()
        : std::max(0.0, deck->getPosition() - deck->getBeatPhase() * 60 / bpm);
    const double end = deck->loopActive() ? deck->loopOutPosition() : start + beats * 60 / bpm;
    if (end <= start || end - start > 30.0001) { setEditorError("Choose a capture range of at most 30 seconds"); return; }
    setEditorFile(session, deck->trackFilePath());
    m_editorCapture = {{"start", start}, {"end", end}, {"bpm", bpm}, {"loop", true}, {"sync", true},
        {"name", QString("Deck %1 capture").arg(QChar('A' + deckIndex))}};
}
void SamplerController::previewEditor(qulonglong session, const QVariantMap& draft) {
    if (!validateEditor(session, draft)) return;
    if (!previewAvailable()) { setEditorError("Start audio and assign separate headphone outputs before previewing"); return; }
    stopEditorPreview(); m_audio.collectStoppedPreview();
    const auto generation = ++m_previewGeneration;
    const auto cancelled = m_previewCancel = std::make_shared<std::atomic<bool>>(false);
    m_previewLoading = true; emit previewChanged();
    const QString path = m_editorSource;
    const double start = draft.value("start").toDouble(), end = draft.value("end").toDouble();
    const bool loop = draft.value("loop").toBool(); const float gain = static_cast<float>(draft.value("gain", 1).toDouble());
    auto* job = new QFutureWatcher<std::shared_ptr<Loaded>>(this); m_jobs.push_back(job);
    connect(job, &QFutureWatcherBase::finished, this, [this, job, session, generation] {
        auto result = job->result(); std::erase(m_jobs, job); job->deleteLater();
        if (session != m_editorSession || generation != m_previewGeneration) return;
        m_previewLoading = false;
        if (!validEditor(session)) { setEditorError("This sample has changed. Close and reopen the editor."); emit previewChanged(); return; }
        if (!result->error.isEmpty()) setEditorError(result->error);
        else if (previewAvailable()) m_audio.installPreview(std::move(result->sample));
        emit previewChanged();
    });
    job->setFuture(QtConcurrent::run([path, start, end, loop, gain, cancelled, budget = m_budget] {
        auto result = std::make_shared<Loaded>();
        if (cancelled->load()) return result;
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(path.toStdString())));
        if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || !std::isfinite(reader->sampleRate) || reader->sampleRate < 8000 || reader->sampleRate > 384000) {
            result->error = "Cannot read this mono/stereo audio file"; return result;
        }
        const auto first = static_cast<juce::int64>(std::llround(start * reader->sampleRate));
        const int frames = static_cast<int>(std::llround(end * reader->sampleRate) - first);
        if (frames < 2 || end - start > 30.0001 || first + frames > reader->lengthInSamples) { result->error = "Preview range is no longer available"; return result; }
        if (cancelled->load()) return result;
        const auto bytes = static_cast<std::uint64_t>(frames) * 8;
        auto reserved = budget->load();
        do {
            if (reserved + bytes > 256ull * 1024 * 1024) { result->error = "Sampler RAM limit reached (256 MiB)"; return result; }
        } while (!budget->compare_exchange_weak(reserved, reserved + bytes));
        result->sample = std::make_unique<SamplerSample>(); auto& sample = *result->sample;
        sample.budget = budget; sample.budgetBytes = bytes; sample.sampleRate = reader->sampleRate;
        sample.loop = loop; sample.gain = gain; sample.pcm.setSize(2, frames);
        for (int offset = 0; offset < frames; offset += 4096) {
            if (cancelled->load()) { result->sample.reset(); return result; }
            const int count = std::min(4096, frames - offset);
            if (!reader->read(&sample.pcm, offset, count, first + offset, true, true)) {
                result->error = "Preview decoding failed"; result->sample.reset(); return result;
            }
        }
        if (reader->numChannels == 1) sample.pcm.copyFrom(1, 0, sample.pcm, 0, 0, frames);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < frames; ++i)
            if (!std::isfinite(sample.pcm.getSample(ch, i))) sample.pcm.setSample(ch, i, 0);
        return result;
    }));
}
void SamplerController::requestEditorWaveform(qulonglong session, double start, double end) {
    if (!validEditor(session) || m_editorLoading || !std::isfinite(start) || !std::isfinite(end)
        || start < 0 || end <= start || end - start > 30.0001 || end > m_editorSourceDuration + 0.000001) return;
    if (m_waveCancel) m_waveCancel->store(true);
    const auto cancelled = m_waveCancel = std::make_shared<std::atomic<bool>>(false);
    const auto generation = ++m_waveGeneration;
    const auto path = m_editorSource;
    auto* job = new QFutureWatcher<QVariantMap>(this); m_probes.push_back(job);
    connect(job, &QFutureWatcherBase::finished, this, [this, job, session, generation, start, end] {
        auto result = job->result(); std::erase(m_probes, job); job->deleteLater();
        if (!validEditor(session) || generation != m_waveGeneration) return;
        if (result.contains("error")) setEditorError(result.value("error").toString());
        else emit editorWaveformReady(session, start, end, result.value("peaks").toList());
    });
    job->setFuture(QtConcurrent::run([path, start, end, cancelled]() -> QVariantMap {
        if (cancelled->load()) return {};
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(path.toStdString())));
        if (!reader || !std::isfinite(reader->sampleRate) || reader->sampleRate < 8000 || reader->sampleRate > 384000 || reader->numChannels < 1 || reader->numChannels > 2)
            return {{"error", "Cannot read waveform source"}};
        const auto first = static_cast<juce::int64>(std::llround(start * reader->sampleRate));
        const auto frames = static_cast<juce::int64>(std::llround(end * reader->sampleRate)) - first;
        if (frames < 2 || first + frames > reader->lengthInSamples) return {{"error", "Waveform range is no longer available"}};
        std::array<float, 1024> peaks {};
        juce::AudioBuffer<float> block(2, 4096);
        for (juce::int64 offset = 0; offset < frames; offset += 4096) {
            if (cancelled->load()) return {};
            const int count = static_cast<int>(std::min<juce::int64>(4096, frames - offset));
            if (!reader->read(&block, 0, count, first + offset, true, true)) return {{"error", "Waveform decoding failed"}};
            for (int i = 0; i < count; ++i) {
                const auto bin = static_cast<std::size_t>(std::min<juce::int64>(1023, (offset + i) * 1024 / frames));
                for (int ch = 0; ch < static_cast<int>(reader->numChannels); ++ch) {
                    const float value = std::abs(block.getSample(ch, i));
                    if (std::isfinite(value)) peaks[bin] = std::max(peaks[bin], value);
                }
            }
        }
        QVariantList values; for (float peak : peaks) values.append(peak);
        return {{"peaks", values}};
    }));
}
