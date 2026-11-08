#include "deck/DjEngine.h"

#include "app/SettingsManager.h"
#include "audio/DeckAudioPipeline.h"
#include "deck/DeckTransport.h"
#include "stems/StemMixState.h"
#include "stems/StemSeparationService.h"
#include "stems/StemsController.h"
#include "stems/StemWaveformOverlay.h"

#include <cmath>

using brockdj::stems::Stem;
using brockdj::stems::StemJobState;
using brockdj::stems::StemJobStatus;

void DjEngine::attachStemsController(StemsController* controller)
{
    if (m_stemsController == controller)
        return;
    if (m_stemsController)
        disconnect(m_stemsController, nullptr, this, nullptr);
    disconnect(m_stemsSettingsConnection);
    m_stemsController = controller;
    if (!controller)
        return;

    const auto requestAutomatically = [this] {
        if (m_hasTrack && m_stemsController && m_stemsController->available()
            && SettingsManager::getInstance().stemsEnabled())
            separateStems();
    };
    connect(controller, &StemsController::modelChanged, this, requestAutomatically);
    m_stemsSettingsConnection = connect(&SettingsManager::getInstance(),
                                        &SettingsManager::stemsEnabledChanged,
                                        this, requestAutomatically);
    requestAutomatically();
}

QVariantList DjEngine::stemGains() const
{
    QVariantList gains;
    const auto state = m_audioPipeline ? m_audioPipeline->stemMixState() : nullptr;
    for (int i = 0; i < brockdj::stems::kStemCount; ++i)
        gains.append(state ? static_cast<double>(state->gain(static_cast<Stem>(i))) : 1.0);
    return gains;
}

void DjEngine::setStemGain(int stem, double gain)
{
    if (stem < 0 || stem >= brockdj::stems::kStemCount || !m_audioPipeline || !std::isfinite(gain))
        return;
    const auto state = m_audioPipeline->stemMixState();
    const auto clamped = static_cast<float>(std::clamp(gain, 0.0, 1.0));
    if (!state || state->gain(static_cast<Stem>(stem)) == clamped)
        return;
    if (clamped != 1.0f && !ensureStemsRequested())
        return;
    state->setGain(static_cast<Stem>(stem), clamped);
    emit stemGainsChanged();
}

void DjEngine::toggleStemMute(int stem)
{
    if (stem < 0 || stem >= brockdj::stems::kStemCount || !m_audioPipeline)
        return;
    const auto state = m_audioPipeline->stemMixState();
    if (!state)
        return;
    setStemGain(stem, state->gain(static_cast<Stem>(stem)) > 0.5f ? 0.0 : 1.0);
}

void DjEngine::soloStem(int stem)
{
    if (stem < 0 || stem >= brockdj::stems::kStemCount || !m_audioPipeline)
        return;
    const auto state = m_audioPipeline->stemMixState();
    if (!state)
        return;
    if (!ensureStemsRequested())
        return;
    bool alreadySolo = true;
    for (int i = 0; i < brockdj::stems::kStemCount; ++i) {
        const float expected = i == stem ? 1.0f : 0.0f;
        alreadySolo = alreadySolo && state->gain(static_cast<Stem>(i)) == expected;
    }
    if (alreadySolo) {
        resetStemGains();
        return;
    }
    for (int i = 0; i < brockdj::stems::kStemCount; ++i)
        state->setGain(static_cast<Stem>(i), i == stem ? 1.0f : 0.0f);
    emit stemGainsChanged();
}

void DjEngine::resetStemGains()
{
    const auto state = m_audioPipeline ? m_audioPipeline->stemMixState() : nullptr;
    if (!state)
        return;
    state->resetGains();
    emit stemGainsChanged();
}

std::shared_ptr<const brockdj::stems::StemWaveformOverlay> DjEngine::stemWaveformOverlay() const
{
    const auto state = m_audioPipeline ? m_audioPipeline->stemMixState() : nullptr;
    if (!state || !m_hasTrack || !m_stemHandle.isValid())
        return {};
    const auto gains = state->targetGains();
    auto data = state->current();
    if (!data || data->trackId() != m_stemHandle.id())
        return {};
    auto overlay = std::make_shared<brockdj::stems::StemWaveformOverlay>();
    overlay->revision = brockdj::stems::stemWaveformOverlayRevision(data->trackId(), gains, data->envelopeEpoch());
    overlay->gains = gains;
    overlay->data = std::move(data);
    return overlay;
}

bool DjEngine::ensureStemsRequested()
{
    return separateStems();
}

void DjEngine::setStemsError(const QString& error)
{
    if (m_stemsError == error)
        return;
    m_stemsError = error;
    emit stemsStateChanged();
}

bool DjEngine::separateStems()
{
    if (!m_hasTrack || !m_stemHandle.isValid() || m_trackFilePath.isEmpty() || !m_audioPipeline) {
        setStemsError(tr("Load a track before using stems."));
        return false;
    }
    if (!m_stemsController || !m_stemsController->supported()) {
        setStemsError(tr("Stems are not supported by this build."));
        return false;
    }
    if (!m_stemsController->modelInstalled()) {
        setStemsError(tr("Download the stem model before using stems."));
        return false;
    }
    auto* service = m_stemsController->service();
    if (!service) {
        setStemsError(tr("The stem separation service is unavailable."));
        return false;
    }
    if (m_stemStatus) {
        const auto state = m_stemStatus->state.load();
        if (state == StemJobState::Queued || state == StemJobState::Separating || state == StemJobState::Ready)
            return true;
    }
    setStemsError({});
    m_stemStatus = std::make_shared<StemJobStatus>();
    m_stemStatus->playheadSample.store(
        static_cast<std::int64_t>(m_transport->positionSeconds() * m_stemHandle.sampleRate()));
    m_stemStatus->playing.store(isPlaying());
    service->submit(m_deckIndex,
                    { m_trackFilePath.toStdString(), m_stemHandle.id(), m_stemHandle.sampleRate(),
                      m_stemHandle.lengthInSamples() },
                    m_stemStatus, m_audioPipeline->stemMixState());
    pollStems();
    return true;
}

void DjEngine::cancelStems()
{
    if (m_stemsController && m_stemsController->service())
        m_stemsController->service()->cancel(m_deckIndex);
    m_stemStatus.reset();
    m_stemHandle = {};
    if (m_audioPipeline) {
        if (const auto state = m_audioPipeline->stemMixState()) {
            state->publish(nullptr);
            state->resetGains();
        }
    }
    emit stemGainsChanged();
    setStemsError({});
    pollStems();
}

void DjEngine::pollStems()
{
    int state = 0;
    double progress = 0.0;
    bool cached = false;
    bool playheadReady = false;
    QString error = m_stemsError;
    if (m_stemStatus) {
        if (m_hasTrack && m_stemHandle.isValid() && m_stemHandle.lengthInSamples() > 0) {
            const auto playhead = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(m_transport->positionSeconds() * m_stemHandle.sampleRate()),
                0, m_stemHandle.lengthInSamples() - 1);
            m_stemStatus->playheadSample.store(playhead, std::memory_order_relaxed);
            m_stemStatus->playing.store(isPlaying(), std::memory_order_relaxed);
            const auto mix = m_audioPipeline ? m_audioPipeline->stemMixState() : nullptr;
            brockdj::stems::StemGainSmoother smoother;
            brockdj::stems::StemReadScope scope(mix.get(), smoother, m_stemHandle.id());
            playheadReady = scope.data() && scope.data()->isReady(playhead);
        }
        state = static_cast<int>(m_stemStatus->state.load());
        progress = m_stemStatus->progress.load(std::memory_order_relaxed);
        cached = m_stemStatus->fromCache.load();
        error = state == static_cast<int>(StemJobState::Failed)
            ? QString::fromStdString(m_stemStatus->error()) : QString();
    }
    // 1 % steps keep QML bindings quiet.
    progress = std::floor(progress * 100.0) / 100.0;
    if (state == m_stemsState && progress == m_stemsProgress && cached == m_stemsCached
        && playheadReady == m_stemsPlayheadReady && error == m_stemsError)
        return;
    m_stemsState = state;
    m_stemsProgress = progress;
    m_stemsCached = cached;
    m_stemsPlayheadReady = playheadReady;
    m_stemsError = error;
    emit stemsStateChanged();
}
