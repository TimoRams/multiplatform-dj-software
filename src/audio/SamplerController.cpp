#include "SamplerController.h"
#include "audio/AudioEngine.h"
#include "deck/DjEngine.h"
#include "deck/sync/DeckSync.h"
#include "link/LinkManager.h"
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
    callbacks.statistics = [this](const ControlTickContext&) { emit changed(); };
    m_registration = clock.registerCallbacks(std::move(callbacks));
    restore();
}
SamplerController::~SamplerController() { shutdown(); }
void SamplerController::shutdown() {
    if (m_stopped) return;
    m_stopped = true; m_registration.reset(); m_audio.stopAll();
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
            {"loaded",!s.asset.isEmpty() && !s.loading && s.error.isEmpty()}, {"duration",s.duration},
            {"bpm",s.bpm},{"gain",s.gain},{"loop",s.loop},{"sync",s.sync},
            {"start",s.start},{"end",s.end},{"waveform",s.waveform}});
    }
    return result;
}
void SamplerController::setBank(int bank) {
    bank = std::clamp(bank, 0, 3); if (bank == m_bank) return;
    m_bank = bank; save(); emit changed();
}
void SamplerController::setSelectedSlot(int index) { if (index >= 0 && index < 8) { m_selectedSlot = index; emit changed(); } }
void SamplerController::setGain(double gain) {
    if (!std::isfinite(gain)) return;
    m_gain = std::clamp(gain, 0.0, 1.5); m_audio.setGain(static_cast<float>(m_gain)); save(); emit changed();
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
void SamplerController::trigger(int pad) {
    if (pad < 0 || pad >= 8) return;
    const int index = m_bank * 8 + pad; const auto& s = m_slots[index];
    if (s.loading || s.asset.isEmpty() || !s.error.isEmpty()) return;
    if (!m_audio.trigger(index, s.loop && s.sync ? m_quantize : 0)) fail("Sampler command queue full; try again");
}
void SamplerController::stop(int pad) { if (pad >= 0 && pad < 8) m_audio.stop(m_bank * 8 + pad); }
void SamplerController::stopAll() { m_audio.stopAll(); }
void SamplerController::clear(int pad) {
    if (pad < 0 || pad >= 8) return;
    const int index = m_bank * 8 + pad;
    const Slot previousSlot = m_slots[index];
    const auto generation = previousSlot.generation + 1;
    const QString previous = previousSlot.asset;
    m_slots[index] = {}; m_slots[index].generation = generation;
    if (!save()) { m_slots[index] = previousSlot; m_slots[index].generation = generation; m_slots[index].loading = false; emit changed(); return; }
    m_audio.install(index, {});
    if (!previous.isEmpty()) QFile::remove(QDir(m_directory).filePath(previous));
    emit changed();
}
void SamplerController::probeFile(const QString& path) {
    const auto generation = ++m_probeGeneration;
    m_fileInfo = {{"loading", true}}; emit fileInfoChanged();
    auto* job = new QFutureWatcher<QVariantMap>(this); m_probes.push_back(job);
    connect(job, &QFutureWatcherBase::finished, this, [this, job, generation] {
        if (generation == m_probeGeneration) { m_fileInfo = job->result(); emit fileInfoChanged(); }
        std::erase(m_probes, job); job->deleteLater();
    });
    job->setFuture(QtConcurrent::run([path = localPath(path)]() -> QVariantMap {
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(path.toStdString())));
        if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || !std::isfinite(reader->sampleRate) || reader->sampleRate <= 0) return {{"error", "Select a supported mono/stereo audio file"}};
        return {{"duration", reader->lengthInSamples / reader->sampleRate}, {"name", QFileInfo(path).completeBaseName()}, {"waveform",sourceOverview(*reader)}};
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
void SamplerController::load(int index, const QString& path, double start, double end, bool imported, Slot desired) {
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || (end >= 0 && end <= start)) { fail("Choose a valid sample range"); return; }
    auto& old = m_slots[index]; old.loading = true; old.error.clear();
    const auto generation = ++old.generation;
    const QString directory = m_directory;
    auto* job = new QFutureWatcher<std::shared_ptr<Loaded>>(this);
    m_jobs.push_back(job);
    connect(job, &QFutureWatcherBase::finished, this, [this, job, index, generation, desired, imported, start, end]() mutable {
        auto result = job->result();
        std::erase(m_jobs, job); job->deleteLater();
        auto& old = m_slots[index];
        if (old.generation != generation || m_stopped) {
            if (imported && !result->asset.isEmpty()) QFile::remove(result->asset);
            return;
        }
        old.loading = false;
        if (!result->error.isEmpty() || !result->sample) {
            m_error = result->error;
            if (m_audio.state(index) == SamplerAudioEngine::Empty) old.error = result->error;
            emit changed(); return;
        }
        const auto oldBytes = old.pcmBytes;
        const auto newBytes = static_cast<std::uint64_t>(result->sample->pcm.getNumSamples()) * 8;
        if (m_audio.residentBytes() - std::min(oldBytes, m_audio.residentBytes()) + newBytes > 256ull * 1024 * 1024) {
            if (imported) QFile::remove(result->asset);
            fail("Sampler RAM limit reached (256 MiB). Clear another slot first."); return;
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
        if (!save()) { old = previousSlot; old.loading = false; if (imported) QFile::remove(result->asset); return; }
        m_audio.install(index, std::move(result->sample)); m_error.clear();
        if (imported && !previous.isEmpty() && previous != old.asset) QFile::remove(QDir(m_directory).filePath(previous));
        emit changed();
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
    const auto data = QJsonDocument(QJsonObject{{"version",1},{"bank",m_bank},{"gain",m_gain},{"quantize",m_quantize},{"slots",entries}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) { fail("Cannot save sampler banks"); return false; }
    return true;
}
void SamplerController::restore() {
    QFile file(QDir(m_directory).filePath("banks.json"));
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) { fail("Cannot read sampler banks"); return; }
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    if (root["version"].toInt() != 1) { fail("Unsupported sampler bank format"); return; }
    m_bank = std::clamp(root["bank"].toInt(), 0, 3);
    m_gain = std::clamp(root["gain"].toDouble(0.72), 0.0, 1.5); m_audio.setGain(static_cast<float>(m_gain));
    m_quantize = root["quantize"].toInt(1); if (m_quantize != 0 && m_quantize != 1 && m_quantize != 4) m_quantize = 1;
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
    double bpm = m_clock.bpm, beat = m_clock.beat + (now - m_clock.timestamp) * bpm / 60;
    QString source = "INTERNAL";
    int index = -1;
    const auto sync = m_sync ? m_sync->snapshot() : engine::sync::SyncCoordinatorSnapshot{};
    if (sync.linkActive && m_link && m_link->enabled()) {
        bpm = m_link->bpm(); beat = m_link->currentBeat(); source = "LINK";
    } else {
        const auto usable = [this](int i) { return i >= 0 && i < 4 && m_decks[i] && m_decks[i]->isPlaying() && !m_decks[i]->isReverse() && m_decks[i]->getCurrentBpm() > 0; };
        if (usable(sync.masterDeckIndex)) index = sync.masterDeckIndex;
        else for (int i = 0; i < 4; ++i) if (usable(i)) { index = i; break; }
        if (index >= 0) { bpm = m_decks[index]->getCurrentBpm(); beat = m_decks[index]->getBeatPosition(); source = QString("DECK %1").arg(QChar('A'+index)); }
    }
    if (!std::isfinite(bpm) || bpm < 20 || bpm > 400 || !std::isfinite(beat)) return;
    if (source != m_clockSource) { ++m_clock.generation; m_clockSource = source; }
    m_clock.bpm = bpm; m_clock.beat = beat; m_clock.timestamp = now;
    m_audio.publishClock(m_clock);
}
