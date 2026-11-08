#include "SamplerController.h"
#include "audio/AudioEngine.h"
#include "deck/DjEngine.h"
#include "deck/sync/DeckSync.h"
#include "link/LinkManager.h"
#include "app/MidiClockController.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <QtConcurrent/QtConcurrent>
#include <QDir>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
// Bounded overview work also for full-length source tracks: read small windows
// throughout the file instead of allocating/decoding the whole track for UI.
QVariantList sourceOverview(juce::AudioFormatReader& reader)
{
    QVariantList result;
    juce::AudioBuffer<float> window(2,512);
    for (int bin = 0; bin < 192; ++bin) {
        const auto position = static_cast<juce::int64>(reader.lengthInSamples * (bin / 192.0));
        const int count = static_cast<int>(std::min<juce::int64>(512, reader.lengthInSamples - position));
        window.clear();
        float peak = 0;
        if (count > 0 && reader.read(&window,0,count,position,true,true))
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < count; ++i)
                if (const float v = window.getSample(ch,i); std::isfinite(v)) peak = std::max(peak,std::abs(v));
        result.append(peak);
    }
    return result;
}
}

QString SamplerController::localPath(const QString& path) {
    const QUrl url(path);
    return url.isLocalFile() ? url.toLocalFile() : path;
}
SamplerController::SamplerController(SamplerAudioEngine& audio, ControlClock& clock,
                                     const QString& directory, QObject* parent)
    : QObject(parent), m_audio(audio), m_directory(directory) {
    m_clock.timestamp = juce::Time::getMillisecondCounterHiRes() * 0.001;
    m_audio.publishClock(m_clock);
    ControlClock::Callbacks callbacks;
    callbacks.syncApply = [this](const ControlTickContext&) { tick(); };
    callbacks.statistics = [this](const ControlTickContext&) { refreshPlayback(); };
    m_lastStates.fill(-1);
    m_gainSaveTimer.setSingleShot(true); m_gainSaveTimer.setInterval(250);
    connect(&m_gainSaveTimer, &QTimer::timeout, this, [this] { save(); });
    m_registration = clock.registerCallbacks(std::move(callbacks));
    restore();
    m_audio.setSequencerBank(m_bank);
    m_audio.setSequencerSwing(static_cast<float>(m_sequencerSwing));
    m_audio.setSequencerFill(m_fillEvery, m_fillDensity);
    for (int bank = 0; bank < 4; ++bank) publishSequencerBank(bank);
}
SamplerController::~SamplerController() { shutdown(); }
void SamplerController::shutdown() {
    if (m_stopped) return;
    cancelEditor(m_editorSession);
    if (m_gainSaveTimer.isActive()) { m_gainSaveTimer.stop(); save(); }
    m_stopped = true; m_registration.reset(); m_audio.setSequencerEnabled(false); m_audio.stopAll(); m_audio.stopPreview();
    for (auto* job : m_jobs) {
        disconnect(job, nullptr, this, nullptr);
        job->waitForFinished();
        const auto result = job->result();
        if (!result->asset.isEmpty()) QFile::remove(result->asset);
        delete job;
    }
    m_jobs.clear();
    for (auto* job : m_probes) { disconnect(job, nullptr, this, nullptr); job->waitForFinished(); delete job; }
    m_probes.clear();
}
void SamplerController::attachDecks(const std::array<DjEngine*, 4>& decks,
                                   engine::sync::SyncCoordinator* sync, LinkManager* link) {
    m_decks = decks; m_sync = sync; m_link = link;
}
void SamplerController::fail(const QString& message) { m_error = message; emit changed(); }
QVariantList SamplerController::slotList() const {
    QVariantList result;
    for (int pad = 0; pad < 8; ++pad) {
        const int index = m_bank * 8 + pad; const auto& s = m_slots[index];
        QString state = "EMPTY";
        if (s.loading) state = "LOADING";
        else if (!s.error.isEmpty()) state = "ERROR";
        else switch (m_audio.state(index)) {
            case SamplerAudioEngine::Ready: state = "READY"; break;
            case SamplerAudioEngine::Pending: state = "QUEUED"; break;
            case SamplerAudioEngine::Playing: state = "PLAYING"; break;
            case SamplerAudioEngine::Busy: state = "BUSY"; break;
            default: break;
        }
        result.append(QVariantMap{{"name",s.name},{"color",s.color},{"state",state},{"error",s.error},
            {"transportState", QStringList {"EMPTY", "READY", "QUEUED", "PLAYING", "BUSY"}.value(m_audio.state(index))},
            {"loaded",!s.asset.isEmpty() && !s.loading && s.error.isEmpty()}, {"duration",s.duration},
            {"bpm",s.bpm},{"gain",s.gain},{"loop",s.loop},{"sync",s.sync},
            {"start",s.start},{"end",s.end},{"waveform",s.waveform}});
    }
    return result;
}
QVariantList SamplerController::sequencerPattern() const {
    QVariantList result;
    for (int track = 0; track < 8; ++track) {
        int mask = 0;
        for (int step = 0; step < 16; ++step)
            if (m_patterns[m_bank * 8 + track][step]) mask |= 1 << step;
        result.append(mask);
    }
    return result;
}
QVariantList SamplerController::sequencerSteps() const {
    QVariantList result;
    for (int track = 0; track < 8; ++track) {
        QVariantList steps;
        for (auto velocity : m_patterns[m_bank * 8 + track]) steps.append(velocity);
        result.append(QVariant(steps));
    }
    return result;
}
QVariantList SamplerController::sequencerTracks() const {
    QVariantList result;
    for (int track = 0; track < 8; ++track)
        result.append(QVariantMap{{"muted", bool(m_patternMuted[m_bank] & (1 << track))},
                                 {"solo", bool(m_patternSolo[m_bank] & (1 << track))}});
    return result;
}
void SamplerController::publishSequencerBank(int bank) {
    m_audio.setSequencerLength(bank, m_patternLengths[bank]);
    m_audio.setSequencerTracks(bank, m_patternMuted[bank], m_patternSolo[bank]);
    for (int track = 0; track < 8; ++track)
        for (int step = 0; step < SamplerAudioEngine::kSequenceSteps; ++step)
            m_audio.setSequencerVelocity(bank, track, step, m_patterns[bank * 8 + track][step]);
}
void SamplerController::setBank(int bank) {
    bank = std::clamp(bank, 0, 3); if (bank == m_bank) return;
    const int previous = m_bank;
    m_bank = bank;
    if (!save()) { m_bank = previous; emit changed(); emit playbackChanged(); return; }
    m_audio.setSequencerBank(bank); emit changed(); emit playbackChanged();
}
void SamplerController::setSelectedSlot(int index) { if (index >= 0 && index < 8) { m_selectedSlot = index; emit changed(); } }
void SamplerController::setGain(double gain) {
    if (!std::isfinite(gain)) return;
    gain = std::clamp(gain, 0.0, 1.5);
    if (gain == m_gain) return;
    m_gain = gain;
    if (!m_applyingMidiGain) m_midiPickup.arm();
    m_audio.setGain(static_cast<float>(m_gain)); m_gainSaveTimer.start(); emit changed();
}
void SamplerController::setCue(bool enabled) {
    const int headphones = AudioEngine::headphonesFirstChannel();
    const int master = AudioEngine::masterFirstChannel(), booth = AudioEngine::boothFirstChannel();
    if (enabled && (headphones <= 0 || (master > 0 && std::abs(headphones-master) < 2)
        || (booth > 0 && std::abs(headphones-booth) < 2))) {
        m_cue = false; m_audio.setCue(false);
        fail("Sampler CUE requires separate headphone outputs. Configure them in Audio settings."); return;
    }
    m_cue = enabled; m_audio.setCue(enabled); emit changed();
}
void SamplerController::setQuantize(int beats) {
    if (beats != 0 && beats != 1 && beats != 4) return;
    m_quantize = beats; save(); emit changed();
}
void SamplerController::setSequencerEnabled(bool enabled) {
    if (m_sequencerEnabled == enabled) return;
    m_sequencerEnabled = enabled;
    if (!enabled && m_roll) { m_roll = 0; m_audio.setSequencerRoll(0); }
    m_audio.setSequencerEnabled(enabled);
    emit changed();
}
void SamplerController::setSequencerSwing(double swing) {
    if (!std::isfinite(swing)) return;
    swing = std::clamp(swing, 0.0, 0.5);
    if (m_sequencerSwing == swing) return;
    const double previous = m_sequencerSwing;
    m_sequencerSwing = swing;
    m_audio.setSequencerSwing(static_cast<float>(swing));
    if (!save()) {
        m_sequencerSwing = previous;
        m_audio.setSequencerSwing(static_cast<float>(previous));
        emit changed();
        return;
    }
    emit changed();
}
void SamplerController::setSequencerStep(int pad, int step, bool enabled) {
    setSequencerVelocity(pad, step, enabled ? 127 : 0);
}
void SamplerController::setSequencerVelocity(int pad, int step, int velocity) {
    if (pad < 0 || pad >= 8 || step < 0 || step >= SamplerAudioEngine::kSequenceSteps
        || velocity < 0 || velocity > 127) { fail("Invalid sequencer step or velocity"); return; }
    auto& stored = m_patterns[m_bank * 8 + pad][step];
    const auto previous = stored;
    stored = static_cast<std::uint8_t>(velocity);
    if (stored == previous) return;
    if (!save()) { stored = previous; emit changed(); return; }
    m_audio.setSequencerVelocity(m_bank, pad, step, velocity);
    emit changed();
}
void SamplerController::setSequencerLength(int steps) {
    if (steps < 16 || steps > 64 || steps % 16 != 0) { fail("Pattern length must be 16, 32, 48 or 64 steps"); return; }
    const int previous = m_patternLengths[m_bank];
    m_patternLengths[m_bank] = steps;
    if (!save()) { m_patternLengths[m_bank] = previous; emit changed(); return; }
    m_audio.setSequencerLength(m_bank, steps);
    emit changed();
}
void SamplerController::setSequencerFillEvery(int bars) {
    if (bars != 0 && bars != 1 && bars != 2 && bars != 4 && bars != 8) { fail("Fill interval must be off, 1, 2, 4 or 8 bars"); return; }
    if (m_fillEvery == bars) return;
    const int previous = m_fillEvery;
    m_fillEvery = bars;
    if (!save()) { m_fillEvery = previous; emit changed(); return; }
    m_audio.setSequencerFill(m_fillEvery, m_fillDensity);
    emit changed();
}
void SamplerController::setSequencerFillDensity(int density) {
    density = std::clamp(density, 0, 100);
    if (m_fillDensity == density) return;
    const int previous = m_fillDensity;
    m_fillDensity = density;
    if (!save()) { m_fillDensity = previous; emit changed(); return; }
    m_audio.setSequencerFill(m_fillEvery, m_fillDensity);
    emit changed();
}
void SamplerController::setSequencerRoll(int steps) {
    if (steps != 0 && steps != 1 && steps != 2 && steps != 4 && steps != 8) return;
    if (m_roll == steps) return;
    m_roll = steps;
    m_audio.setSequencerRoll(steps);
    emit changed();
}
void SamplerController::setSequencerTrackFlag(int pad, bool enabled, bool solo) {
    if (pad < 0 || pad >= 8) { fail("Invalid sequencer track"); return; }
    auto& mask = solo ? m_patternSolo[m_bank] : m_patternMuted[m_bank];
    const auto previous = mask;
    mask = static_cast<std::uint8_t>(enabled ? mask | (1 << pad) : mask & ~(1 << pad));
    if (!save()) { mask = previous; emit changed(); return; }
    m_audio.setSequencerTracks(m_bank, m_patternMuted[m_bank], m_patternSolo[m_bank]);
    emit changed();
}
void SamplerController::setSequencerMuted(int pad, bool muted) { setSequencerTrackFlag(pad, muted, false); }
void SamplerController::setSequencerSolo(int pad, bool solo) { setSequencerTrackFlag(pad, solo, true); }
void SamplerController::setInternalBpm(double bpm) {
    if (!std::isfinite(bpm) || bpm < 20 || bpm > 400) { fail("Internal tempo must be between 20 and 400 BPM"); return; }
    const double previous = m_internalBpm;
    m_internalBpm = bpm;
    if (!save()) { m_internalBpm = previous; emit changed(); return; }
    tick();
    emit changed();
}
void SamplerController::copySequencerPattern(int destination) {
    if (destination < 0 || destination >= 4 || destination == m_bank) { fail("Choose a different destination pattern"); return; }
    const auto previous = m_patterns;
    const int length = m_patternLengths[destination];
    const auto muted = m_patternMuted[destination], solo = m_patternSolo[destination];
    for (int track = 0; track < 8; ++track)
        m_patterns[destination * 8 + track] = m_patterns[m_bank * 8 + track];
    m_patternLengths[destination] = sequencerLength();
    m_patternMuted[destination] = m_patternMuted[m_bank];
    m_patternSolo[destination] = m_patternSolo[m_bank];
    if (!save()) {
        m_patterns = previous; m_patternLengths[destination] = length;
        m_patternMuted[destination] = muted; m_patternSolo[destination] = solo;
        emit changed();
        return;
    }
    publishSequencerBank(destination);
    emit changed();
}
void SamplerController::clearSequencerPattern() {
    const auto previous = m_patterns;
    for (int track = 0; track < 8; ++track) m_patterns[m_bank * 8 + track].fill(0);
    if (!save()) { m_patterns = previous; emit changed(); return; }
    publishSequencerBank(m_bank);
    emit changed();
}
void SamplerController::trigger(int pad) {
    if (pad < 0 || pad >= 8) return;
    const int index = m_bank * 8 + pad; const auto& s = m_slots[index];
    if (s.loading || s.asset.isEmpty() || !s.error.isEmpty()) return;
    if (!m_audio.trigger(index, s.loop && s.sync ? m_quantize : 0)) fail("Sampler command queue full; try again");
}
void SamplerController::stop(int pad) { if (pad >= 0 && pad < 8) m_audio.stop(m_bank * 8 + pad); }
void SamplerController::stopAll() { setSequencerEnabled(false); m_audio.stopAll(); }
void SamplerController::clear(int pad) {
    if (pad >= 0 && pad < 8) (void) clearSlot(m_bank * 8 + pad);
}
bool SamplerController::clearSlot(int index) {
    const Slot previousSlot = m_slots[index];
    const auto generation = previousSlot.generation + 1;
    const QString previous = previousSlot.asset;
    m_slots[index] = {}; m_slots[index].generation = generation;
    if (!save()) { m_slots[index] = previousSlot; m_slots[index].generation = generation; m_slots[index].loading = false; emit changed(); return false; }
    m_audio.install(index, {});
    if (!previous.isEmpty()) QFile::remove(QDir(m_directory).filePath(previous));
    emit changed();
    return true;
}
void SamplerController::probeFile(const QString& path) {
    const auto generation = ++m_probeGeneration;
    m_fileInfo = {{"loading", true}}; emit fileInfoChanged();
    auto* job = new QFutureWatcher<QVariantMap>(this); m_probes.push_back(job);
    connect(job, &QFutureWatcherBase::finished, this, [this, job, generation] {
        if (generation == m_probeGeneration) { m_fileInfo = job->result(); emit fileInfoChanged(); editorProbeFinished(generation); }
        std::erase(m_probes, job); job->deleteLater();
    });
    job->setFuture(QtConcurrent::run([path = localPath(path)]() -> QVariantMap {
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(path.toStdString())));
        if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || !std::isfinite(reader->sampleRate) || reader->sampleRate < 8000 || reader->sampleRate > 384000) return {{"error", "Select a supported mono/stereo audio file"}};
        return {{"duration", reader->lengthInSamples / reader->sampleRate}, {"name", QFileInfo(path).completeBaseName()}, {"waveform",sourceOverview(*reader)}, {"sampleRate", reader->sampleRate}};
    }));
}
void SamplerController::loadFile(int pad, const QString& path, double start, double end, double bpm, bool loop, bool sync,
                                 const QString& name, const QString& color, double gain) {
    if (pad < 0 || pad >= 8 || m_stopped) return;
    Slot desired;
    desired.name = QFileInfo(localPath(path)).completeBaseName();
    if (!name.isEmpty()) desired.name = name.left(128);
    if (QColor(color).isValid()) desired.color = color;
    desired.gain = std::isfinite(gain) ? std::clamp(gain, 0.0, 1.5) : 1;
    desired.loop = loop; desired.sync = loop && sync && bpm >= 20 && bpm <= 400;
    desired.bpm = std::isfinite(bpm) && bpm >= 20 && bpm <= 400 ? bpm : 0;
    load(m_bank * 8 + pad, localPath(path), start, end, true, desired);
}
void SamplerController::load(int index, const QString& path, double start, double end, bool imported, Slot desired, std::uint64_t editorSession) {
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || (end >= 0 && end <= start)) { fail("Choose a valid sample range"); return; }
    auto& old = m_slots[index]; old.loading = true; old.error.clear();
    const auto generation = ++old.generation;
    const QString directory = m_directory;
    auto* job = new QFutureWatcher<std::shared_ptr<Loaded>>(this);
    m_jobs.push_back(job);
    connect(job, &QFutureWatcherBase::finished, this, [this, job, index, generation, desired, imported, start, end, editorSession]() mutable {
        auto result = job->result();
        std::erase(m_jobs, job); job->deleteLater();
        auto& old = m_slots[index];
        if (old.generation != generation || m_stopped || (editorSession && m_editorSession != editorSession)) {
            if (imported && !result->asset.isEmpty()) QFile::remove(result->asset);
            finishEditorSave(editorSession, false, "This sample has changed. Close and reopen the editor.");
            return;
        }
        old.loading = false;
        if (!result->error.isEmpty() || !result->sample) {
            m_error = result->error;
            if (m_audio.state(index) == SamplerAudioEngine::Empty) old.error = result->error;
            finishEditorSave(editorSession, false, m_error);
            emit changed(); return;
        }
        const auto oldBytes = old.pcmBytes;
        const auto newBytes = static_cast<std::uint64_t>(result->sample->pcm.getNumSamples()) * 8;
        if (m_audio.residentBytes() - std::min(oldBytes, m_audio.residentBytes()) + newBytes > 256ull * 1024 * 1024) {
            if (imported) QFile::remove(result->asset);
            fail("Sampler RAM limit reached (256 MiB). Clear another slot first.");
            finishEditorSave(editorSession, false, m_error); return;
        }
        const QString previous = old.asset;
        desired.generation = generation; desired.duration = imported ? result->duration : result->assetDuration;
        desired.pcmBytes = newBytes;
        desired.sampleRate = result->sample->sampleRate;
        desired.waveform = !imported && !desired.waveform.isEmpty() ? desired.waveform : result->waveform;
        desired.asset = imported ? QFileInfo(result->asset).fileName() : old.asset;
        desired.start = imported ? 0 : start;
        desired.end = imported ? result->duration : (end < 0 ? result->duration : end);
        result->sample->loop = desired.loop; result->sample->sync = desired.sync;
        result->sample->gain = static_cast<float>(desired.gain); result->sample->bpm = desired.bpm;
        const Slot previousSlot = old; old = desired;
        if (!save()) { old = previousSlot; old.loading = false; if (imported) QFile::remove(result->asset); finishEditorSave(editorSession, false, m_error); return; }
        m_audio.install(index, std::move(result->sample)); m_error.clear();
        if (imported && !previous.isEmpty() && previous != old.asset) QFile::remove(QDir(m_directory).filePath(previous));
        emit changed();
        finishEditorSave(editorSession, true, {});
    });
    job->setFuture(QtConcurrent::run([path, directory, start, end, imported, budget = m_budget] {
        auto r = std::make_shared<Loaded>();
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(path.toStdString())));
        if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || !std::isfinite(reader->sampleRate) || reader->sampleRate < 8000 || reader->sampleRate > 384000) { r->error = "Cannot read this mono/stereo audio file"; return r; }
        const double duration = reader->lengthInSamples / reader->sampleRate;
        const double finish = end < 0 ? duration : std::min(duration, end);
        if (finish <= start || finish - start > 30.0001) { r->error = "Choose a sample range of at most 30 seconds"; return r; }
        const auto first = static_cast<juce::int64>(std::llround(start * reader->sampleRate));
        const int frames = static_cast<int>(std::llround((finish - start) * reader->sampleRate));
        if (frames < 2) { r->error = "Sample range is too short"; return r; }
        const auto bytes = static_cast<std::uint64_t>(frames) * 8;
        auto reserved = budget->load();
        do {
            if (reserved + bytes > 256ull * 1024 * 1024) { r->error = "Sampler RAM limit reached (256 MiB). Clear another slot first."; return r; }
        } while (!budget->compare_exchange_weak(reserved, reserved + bytes));
        r->sample = std::make_unique<SamplerSample>(); auto& sample = *r->sample;
        sample.budget = budget; sample.budgetBytes = bytes;
        sample.sampleRate = reader->sampleRate; sample.pcm.setSize(2, frames);
        if (!reader->read(&sample.pcm, 0, frames, first, true, true)) { r->error = "Sample decoding failed"; r->sample.reset(); return r; }
        if (reader->numChannels == 1) sample.pcm.copyFrom(1, 0, sample.pcm, 0, 0, frames);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < frames; ++i)
            if (!std::isfinite(sample.pcm.getSample(ch, i))) sample.pcm.setSample(ch, i, 0);
        r->duration = frames / sample.sampleRate; r->assetDuration = duration;
        for (int bin = 0; bin < 192; ++bin) {
            float peak = 0;
            for (int i = bin * frames / 192; i < (bin + 1) * frames / 192; ++i)
                peak = std::max(peak, std::max(std::abs(sample.pcm.getSample(0, i)), std::abs(sample.pcm.getSample(1, i))));
            r->waveform.append(peak);
        }
        if (!imported && (start > 0 || finish < duration)) r->waveform = sourceOverview(*reader);
        if (imported) {
            if (!QDir().mkpath(directory)) { r->error = "Cannot create sampler storage"; return r; }
            const QString name = QUuid::createUuid().toString(QUuid::Id128) + ".wav";
            const QString final = QDir(directory).filePath(name), temp = final + ".tmp";
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> stream(juce::File(temp.toStdString()).createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer(stream ? wav.createWriterFor(stream.get(), sample.sampleRate, 2, 24, {}, 0) : nullptr);
            if (!writer) { r->error = "Cannot write sampler asset"; QFile::remove(temp); return r; }
            stream.release();
            const bool written = writer->writeFromAudioSampleBuffer(sample.pcm, 0, frames);
            writer.reset();
            if (!written || !QFile::rename(temp, final)) { r->error = "Sampler asset write failed"; QFile::remove(temp); return r; }
            r->asset = final;
        }
        return r;
    }));
    emit changed();
}
void SamplerController::edit(int pad, const QString& name, const QString& color, double gain, bool loop,
                            bool sync, double bpm, double start, double end) {
    if (pad < 0 || pad >= 8) return;
    const int index = m_bank * 8 + pad;
    if (m_slots[index].asset.isEmpty() || m_slots[index].loading) return;
    if (!std::isfinite(gain) || !std::isfinite(bpm) || (sync && (bpm < 20 || bpm > 400))) { fail("Sync requires source BPM between 20 and 400"); return; }
    Slot desired = m_slots[index];
    desired.name = name.left(128); desired.color = QColor(color).isValid() ? color : "#e0a344";
    desired.gain = std::clamp(gain, 0.0, 1.5); desired.loop = loop; desired.sync = loop && sync; desired.bpm = bpm;
    load(index, QDir(m_directory).filePath(desired.asset), start, end, false, desired);
}
void SamplerController::captureDeck(int pad, int deckIndex, int beats) {
    if (pad < 0 || pad >= 8 || deckIndex < 0 || deckIndex >= 4) return;
    auto* deck = m_decks[deckIndex];
    if (!deck || !deck->hasTrack()) { fail("Load a track on the selected deck first"); return; }
    const double bpm = deck->getCurrentBpm() / std::max(0.01, deck->getTempoRatio());
    double start = deck->getPosition(), end = 0;
    if (deck->loopActive()) { start = deck->loopInPosition(); end = deck->loopOutPosition(); }
    else {
        if (bpm <= 0 || !std::isfinite(bpm)) { fail("Correct the beatgrid before capturing beats"); return; }
        start = std::max(0.0, start - deck->getBeatPhase() * 60 / bpm);
        if (beats != 1 && beats != 2 && beats != 4 && beats != 8 && beats != 16 && beats != 32) beats = 4;
        end = start + beats * 60 / bpm;
    }
    Slot desired; desired.name = QString("Deck %1 — %2 beats").arg(QChar('A' + deckIndex)).arg(beats);
    desired.loop = true; desired.sync = bpm > 0; desired.bpm = bpm;
    load(m_bank * 8 + pad, deck->trackFilePath(), start, end, true, desired);
}
bool SamplerController::save() {
    QJsonArray entries;
    for (const auto& s : m_slots) entries.append(QJsonObject{{"asset",s.asset},{"name",s.name},{"color",s.color},
        {"gain",s.gain},{"bpm",s.bpm},{"loop",s.loop},{"sync",s.sync},{"start",s.start},{"end",s.end}});
    if (!QDir().mkpath(m_directory)) { fail("Cannot create sampler storage"); return false; }
    QSaveFile file(QDir(m_directory).filePath("banks.json"));
    QJsonArray patterns;
    for (const auto& track : m_patterns) {
        QJsonArray steps;
        for (auto velocity : track) steps.append(velocity);
        patterns.append(steps);
    }
    QJsonArray lengths, muted, solo;
    for (int bank = 0; bank < 4; ++bank) {
        lengths.append(m_patternLengths[bank]); muted.append(m_patternMuted[bank]); solo.append(m_patternSolo[bank]);
    }
    const auto data = QJsonDocument(QJsonObject{{"version",2},{"bank",m_bank},{"gain",m_gain},
        {"quantize",m_quantize},{"swing",m_sequencerSwing},{"patterns",patterns},{"slots",entries},
        {"lengths",lengths},{"muted",muted},{"solo",solo},{"internalBpm",m_internalBpm},
        {"fillEvery",m_fillEvery},{"fillDensity",m_fillDensity}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) { fail("Cannot save sampler banks"); return false; }
    return true;
}
void SamplerController::restore() {
    QFile file(QDir(m_directory).filePath("banks.json"));
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) { fail("Cannot read sampler banks"); return; }
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    const int version = root["version"].toInt();
    if (version != 1 && version != 2) { fail("Unsupported sampler bank format"); return; }
    if (version == 2) {
        const auto patterns = root["patterns"].toArray();
        const auto lengths = root["lengths"].toArray(), muted = root["muted"].toArray(), solo = root["solo"].toArray();
        bool valid = patterns.size() == 32 && lengths.size() == 4 && muted.size() == 4 && solo.size() == 4;
        for (const auto& row : patterns) {
            const auto steps = row.toArray();
            valid &= steps.size() == 64;
            for (const auto& value : steps)
                valid &= value.isDouble() && value.toInt(-1) >= 0 && value.toInt(-1) <= 127
                    && value.toDouble() == value.toInt(-1);
        }
        if (valid) {
            for (int bank = 0; bank < 4; ++bank) {
                const int length = lengths[bank].toInt(-1);
                valid &= lengths[bank].isDouble() && lengths[bank].toDouble() == length
                    && length >= 16 && length <= 64 && length % 16 == 0;
                for (const auto value : {muted[bank], solo[bank]})
                    valid &= value.isDouble() && value.toInt(-1) >= 0 && value.toInt(-1) <= 255
                        && value.toDouble() == value.toInt(-1);
            }
        }
        const double tempo = root["internalBpm"].toDouble(-1);
        valid &= std::isfinite(tempo) && tempo >= 20 && tempo <= 400;
        if (!valid) { fail("Invalid sequencer data in sampler banks"); return; }
    }
    m_bank = std::clamp(root["bank"].toInt(), 0, 3);
    m_gain = std::clamp(root["gain"].toDouble(0.72), 0.0, 1.5); m_audio.setGain(static_cast<float>(m_gain));
    m_quantize = root["quantize"].toInt(1); if (m_quantize != 0 && m_quantize != 1 && m_quantize != 4) m_quantize = 1;
    m_sequencerSwing = std::clamp(root["swing"].toDouble(0), 0.0, 0.5);
    const auto patterns = root["patterns"].toArray();
    for (int i = 0; i < std::min(static_cast<int>(m_patterns.size()), static_cast<int>(patterns.size())); ++i) {
        if (version == 1) {
            const int mask = std::clamp(patterns[i].toInt(), 0, 65535);
            for (int step = 0; step < 16; ++step) m_patterns[i][step] = mask & (1 << step) ? 127 : 0;
        } else {
            const auto steps = patterns[i].toArray();
            for (int step = 0; step < std::min(64, static_cast<int>(steps.size())); ++step)
                m_patterns[i][step] = static_cast<std::uint8_t>(std::clamp(steps[step].toInt(), 0, 127));
        }
    }
    if (version == 2) {
        const auto lengths = root["lengths"].toArray(), muted = root["muted"].toArray(), solo = root["solo"].toArray();
        for (int bank = 0; bank < 4; ++bank) {
            const int length = lengths.at(bank).toInt(16);
            m_patternLengths[bank] = length >= 16 && length <= 64 && length % 16 == 0 ? length : 16;
            m_patternMuted[bank] = static_cast<std::uint8_t>(std::clamp(muted.at(bank).toInt(), 0, 255));
            m_patternSolo[bank] = static_cast<std::uint8_t>(std::clamp(solo.at(bank).toInt(), 0, 255));
        }
        m_internalBpm = std::clamp(root["internalBpm"].toDouble(120), 20.0, 400.0);
        const int fillEvery = root["fillEvery"].toInt(0);
        m_fillEvery = fillEvery == 1 || fillEvery == 2 || fillEvery == 4 || fillEvery == 8 ? fillEvery : 0;
        m_fillDensity = std::clamp(root["fillDensity"].toInt(50), 0, 100);
    }
    const auto entries = root["slots"].toArray();
    for (int i = 0; i < std::min(32, static_cast<int>(entries.size())); ++i) {
        const auto object = entries[i].toObject(); Slot s;
        s.asset = object["asset"].toString();
        if (s.asset.isEmpty() || QFileInfo(s.asset).fileName() != s.asset) continue;
        s.name = object["name"].toString().left(128); s.color = object["color"].toString("#e0a344");
        s.bpm = object["bpm"].toDouble(); s.gain = std::clamp(object["gain"].toDouble(1),0.0,1.5);
        s.loop = object["loop"].toBool(); s.sync = s.loop && object["sync"].toBool() && s.bpm >= 20 && s.bpm <= 400;
        s.start = object["start"].toDouble(); s.end = object["end"].toDouble(-1);
        m_slots[i] = s;
        load(i, QDir(m_directory).filePath(s.asset), s.start, s.end, false, s);
    }
}
void SamplerController::tick() {
    if (m_stopped) return;
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    double bpm = m_internalBpm, beat = m_clock.beat + (now - m_clock.timestamp) * m_clock.bpm / 60;
    QString source = "INTERNAL";
    int index = -1;
    const auto sync = m_sync ? m_sync->snapshot() : engine::sync::SyncCoordinatorSnapshot{};
    const auto external = m_midiClock ? m_midiClock->externalClock(now) : brockdj::midiclock::ClockSnapshot{};
    bool externalRestarted = false;
    if (sync.linkActive && m_link && m_link->enabled()) {
        bpm = m_link->bpm(); beat = m_link->currentBeat(); source = "LINK";
    } else if (external.valid) {
        bpm = external.bpm; beat = external.beat + (now - external.timestamp) * external.bpm / 60; source = "MIDI";
        externalRestarted = external.generation != m_externalClockGeneration;
        m_externalClockGeneration = external.generation;
    } else {
        const auto usable = [this](int i) { return i >= 0 && i < 4 && m_decks[i] && m_decks[i]->isPlaying() && !m_decks[i]->isReverse() && m_decks[i]->getCurrentBpm() > 0; };
        if (usable(sync.masterDeckIndex)) index = sync.masterDeckIndex;
        else for (int i = 0; i < 4; ++i) if (usable(i)) { index = i; break; }
        if (index >= 0) { bpm = m_decks[index]->getCurrentBpm(); beat = m_decks[index]->getBeatPosition(); source = QString("DECK %1").arg(QChar('A'+index)); }
    }
    if (!std::isfinite(bpm) || bpm < 20 || bpm > 400 || !std::isfinite(beat)) {
        if (m_midiClock) m_midiClock->publishMasterClock({});
        return;
    }
    if (source != m_clockSource || externalRestarted) { ++m_clock.generation; m_clockSource = source; }
    m_clock.bpm = bpm; m_clock.beat = beat; m_clock.timestamp = now;
    m_audio.publishClock(m_clock);
    if (m_midiClock) {
        // Never echo an incoming MIDI clock back out (feedback through MIDI thru).
        bool anyDeckPlaying = false;
        for (auto* deck : m_decks) anyDeckPlaying |= deck && deck->isPlaying();
        m_midiClock->publishMasterClock({source != "MIDI", anyDeckPlaying || m_sequencerEnabled,
                                         bpm, beat, now, m_clock.generation});
    }
}
