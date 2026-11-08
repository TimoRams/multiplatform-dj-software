#include "app/AutomixController.h"

#include "app/MixerControl.h"
#include "app/SettingsManager.h"
#include "deck/DjEngine.h"
#include "domain/TrackData.h"
#include "library/LibraryDatabase.h"

#include <QFileInfo>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

using namespace brockdj::automix;

namespace {
constexpr auto kBarsKey = "automixTransitionBars";
constexpr double kTriggerGraceSec = 0.25;
constexpr double kLowKill = -1.0;
}

AutomixController::AutomixController(QObject* parent)
    : QObject(parent)
{
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, &AutomixController::tick);
}

AutomixController::~AutomixController()
{
    if (m_eqTouched)
        restoreEq();
}

void AutomixController::attach(DjEngine* deckA, DjEngine* deckB, MixerControl* mixer,
                               LibraryDatabase* library, SettingsManager* settings)
{
    m_decks[0] = deckA;
    m_decks[1] = deckB;
    m_mixer = mixer;
    m_library = library;
    m_settingsManager = settings;

    for (int index = 0; index < 2; ++index) {
        DjEngine* const engine = m_decks[index];
        if (!engine)
            continue;
        connect(engine, &DjEngine::trackLoaded, this, [this, index]() { onDeckLoaded(index); });
        connect(engine, &DjEngine::trackLoadErrorChanged, this, [this, index]() { onDeckLoadError(index); });
    }
    if (library) {
        connect(library, &LibraryDatabase::queueChanged, this, [this]() {
            m_queueExhausted = false;
            if (!m_enabled)
                return;
            QMetaObject::invokeMethod(this, [this]() {
                if (m_enabled && m_state == State::Waiting && m_loadingDeck < 0)
                    startFromIdle(false);
                else
                    tick();
            }, Qt::QueuedConnection);
        });
    }
    if (settings) {
        bool ok = false;
        const int stored = settings->getUiState(QString::fromLatin1(kBarsKey)).toInt(&ok);
        if (ok)
            setTransitionBars(stored);
    }
}

DjEngine* AutomixController::deck(int index) const
{
    return index == 0 || index == 1 ? m_decks[index].data() : nullptr;
}

QString AutomixController::channelId(int index) const
{
    return index == 0 ? QStringLiteral("deckA") : QStringLiteral("deckB");
}

void AutomixController::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!enabled) {
        m_timer.stop();
        if (m_eqTouched)
            restoreEq();
        m_loadingDeck = -1;
        m_mixNowRequested = false;
        m_next = NextState::None;
        setProgress(-1.0, 0.0);
        setState(State::Off);
        emit enabledChanged();
        return;
    }

    emit enabledChanged();
    takeOverCrossfader();
    DjEngine* const a = deck(0);
    DjEngine* const b = deck(1);
    const bool aPlaying = a && a->isPlaying();
    const bool bPlaying = b && b->isPlaying();
    if (aPlaying || bPlaying) {
        if (aPlaying && bPlaying)
            m_main = m_mixer && m_mixer->crossfaderState().value(QStringLiteral("position")).toDouble() > 0.0 ? 1 : 0;
        else
            m_main = aPlaying ? 0 : 1;
        // A track the DJ already cued on the other deck is the obvious next one.
        DjEngine* const other = otherDeck();
        if (other && other->hasTrack() && !other->isPlaying()) {
            m_next = NextState::Ready;
            m_nextTitle = other->trackTitle();
            m_nextArtist = other->trackArtist();
        } else {
            m_next = NextState::None;
        }
        if (m_mixer)
            m_mixer->setCrossfaderPosition(static_cast<float>(sideOf(m_main)));
        setState(State::Playing);
    } else {
        startFromIdle(true);
    }
    m_timer.start();
    tick();
}

void AutomixController::setTransitionBars(int bars)
{
    bars = std::clamp(bars, 4, 64);
    if (bars == m_settings.transitionBars)
        return;
    m_settings.transitionBars = bars;
    if (m_settingsManager)
        m_settingsManager->setUiState(QString::fromLatin1(kBarsKey), QString::number(bars));
    emit transitionBarsChanged();
}

QString AutomixController::state() const
{
    switch (m_state) {
    case State::Off: return QStringLiteral("off");
    case State::Waiting: return QStringLiteral("waiting");
    case State::Starting: return QStringLiteral("starting");
    case State::Playing: return QStringLiteral("playing");
    case State::Mixing: return QStringLiteral("mixing");
    }
    return {};
}

QString AutomixController::statusText() const
{
    switch (m_state) {
    case State::Off: return tr("Automix aus");
    case State::Waiting: return tr("Warteschlange leer – Tracks hinzufügen");
    case State::Starting: return tr("Lade ersten Track …");
    case State::Mixing: return tr("Übergang läuft");
    case State::Playing:
        if (m_next == NextState::Loading)
            return tr("Lade nächsten Track …");
        if (m_next == NextState::Ready)
            return tr("Nächster Track bereit");
        return tr("Kein weiterer Track in der Warteschlange");
    }
    return {};
}

QString AutomixController::currentDeck() const
{
    if (m_state == State::Off || m_state == State::Waiting)
        return {};
    return m_main == 0 ? QStringLiteral("A") : QStringLiteral("B");
}

TrackTimeline AutomixController::timelineOf(const DjEngine& engine) const
{
    TrackTimeline timeline;
    timeline.durationSec = engine.getDuration();
    if (const TrackData* const data = engine.getTrackData()) {
        timeline.bpm = data->getBpm();
        if (const auto grid = data->getBeatGridSnapshot()) {
            timeline.downbeats.reserve(grid->size() / 4 + 1);
            for (const auto& marker : *grid) {
                if (marker.isDownbeat)
                    timeline.downbeats.push_back(marker.positionSec);
            }
        }
    }
    const QVariantList segments = engine.currentSegments();
    timeline.phrases.reserve(static_cast<std::size_t>(segments.size()));
    for (const QVariant& value : segments) {
        const QVariantMap segment = value.toMap();
        PhraseSpan span;
        span.startSec = segment.value(QStringLiteral("startTime")).toDouble();
        span.endSec = segment.value(QStringLiteral("endTime")).toDouble();
        span.kind = phraseKindFromLabel(segment.value(QStringLiteral("label")).toString().toLatin1().constData());
        timeline.phrases.push_back(span);
    }
    return timeline;
}

bool AutomixController::loadNextFromQueue(int deckIndex)
{
    DjEngine* const target = deck(deckIndex);
    if (!target || !m_library)
        return false;
    const QVariantList queue = m_library->getQueueTracks();
    for (const QVariant& value : queue) {
        const QVariantMap track = value.toMap();
        const QString trackId = track.value(QStringLiteral("trackId")).toString();
        const QString path = track.value(QStringLiteral("filePath")).toString();
        m_library->dequeueTrack(trackId);
        if (path.isEmpty() || !QFileInfo::exists(path))
            continue;
        m_loadingDeck = deckIndex;
        m_loadingPath = path;
        m_nextTitle = track.value(QStringLiteral("title")).toString();
        m_nextArtist = track.value(QStringLiteral("artist")).toString();
        target->loadTrack(path);
        emit statusChanged();
        return true;
    }
    return false;
}

void AutomixController::startFromIdle(bool allowResume)
{
    m_main = 0;
    DjEngine* const a = deck(0);
    if (!a)
        return;
    if (loadNextFromQueue(0)) {
        setState(State::Starting);
        return;
    }
    // Nothing queued: play what is already on deck A, if anything is left of it.
    if (allowResume && a->hasTrack() && a->getPosition() < a->getDuration() - 10.0) {
        playStartedDeck();
        return;
    }
    m_queueExhausted = true;
    m_nextTitle.clear();
    m_nextArtist.clear();
    setState(State::Waiting);
}

void AutomixController::playStartedDeck()
{
    DjEngine* const engine = mainDeck();
    if (!engine)
        return;
    const double duration = engine->getDuration();
    if (m_state == State::Starting && duration > 0.0)
        engine->setPosition(static_cast<float>(introStartSec(timelineOf(*engine)) / duration));
    if (m_mixer)
        m_mixer->setCrossfaderPosition(static_cast<float>(sideOf(m_main)));
    engine->play();
    m_next = NextState::None;
    m_nextTitle.clear();
    m_nextArtist.clear();
    setState(State::Playing);
}

void AutomixController::onDeckLoaded(int deckIndex)
{
    if (!m_enabled)
        return;
    if (deckIndex != m_loadingDeck) {
        // The DJ loaded something on the waiting deck: respect that choice.
        if (m_state == State::Playing && deckIndex == 1 - m_main && m_loadingDeck < 0) {
            DjEngine* const engine = deck(deckIndex);
            m_next = NextState::Ready;
            m_nextTitle = engine ? engine->trackTitle() : QString();
            m_nextArtist = engine ? engine->trackArtist() : QString();
            emit statusChanged();
        }
        return;
    }
    m_loadingDeck = -1;
    m_loadingPath.clear();
    if (m_state == State::Starting && deckIndex == m_main) {
        playStartedDeck();
        return;
    }
    m_next = NextState::Ready;
    emit statusChanged();
}

void AutomixController::onDeckLoadError(int deckIndex)
{
    DjEngine* const engine = deck(deckIndex);
    if (!m_enabled || deckIndex != m_loadingDeck || !engine || engine->trackLoadError().isEmpty())
        return;
    m_loadingDeck = -1;
    m_loadingPath.clear();
    if (m_state == State::Starting) {
        startFromIdle(false);
        return;
    }
    m_next = NextState::None;
    emit statusChanged();
}

void AutomixController::tick()
{
    if (!m_enabled)
        return;
    if (m_state == State::Mixing) {
        updateTransition();
        return;
    }
    if (m_state == State::Starting)
        return;

    DjEngine* main = mainDeck();
    if (!main || !main->isPlaying()) {
        DjEngine* const other = otherDeck();
        if (other && other->isPlaying()) {
            m_main = 1 - m_main;
            m_next = NextState::None;
            main = other;
        } else {
            setProgress(-1.0, 0.0);
            // Paused by the DJ in the middle of a track: wait until it plays again.
            if (main && main->hasTrack() && m_state == State::Playing
                && main->getPosition() < main->getDuration() - 1.0)
                return;
            if (other && m_next == NextState::Ready && m_state == State::Playing) {
                // The track ran out before a transition could start: hard cut
                // to the prepared one rather than leaving silence.
                m_main = 1 - m_main;
                m_state = State::Starting;
                playStartedDeck();
            } else if (m_loadingDeck < 0 && m_state != State::Waiting) {
                startFromIdle(false);
            }
            return;
        }
    }
    setState(State::Playing);

    DjEngine* const other = otherDeck();
    if (!other)
        return;
    if (m_next == NextState::None && m_loadingDeck < 0 && !m_queueExhausted && !other->isPlaying()) {
        if (loadNextFromQueue(1 - m_main)) {
            m_next = NextState::Loading;
        } else {
            m_queueExhausted = true;
            m_nextTitle.clear();
            m_nextArtist.clear();
        }
        emit statusChanged();
    }
    if (m_next != NextState::Ready || other->isPlaying()) {
        setProgress(-1.0, 0.0);
        setPlanText({});
        return;
    }

    const double position = main->getPosition();
    const TransitionPlan plan = planTransition(timelineOf(*main), timelineOf(*other), m_settings,
                                               position - kTriggerGraceSec, m_mixNowRequested);
    if (!plan.valid) {
        setProgress(-1.0, 0.0);
        return;
    }
    QString anchor;
    switch (plan.anchor) {
    case TransitionAnchor::Outro: anchor = tr("Outro"); break;
    case TransitionAnchor::Phrase: anchor = tr("Phrase"); break;
    case TransitionAnchor::Bar: anchor = tr("Takt"); break;
    case TransitionAnchor::Immediate: anchor = tr("Sofort"); break;
    case TransitionAnchor::Time: anchor = tr("Ohne Beatgrid"); break;
    }
    setPlanText(plan.beatAligned && plan.bars > 0
        ? tr("%1 · %2 Takte").arg(anchor).arg(plan.bars)
        : tr("%1 · %2 s").arg(anchor).arg(std::lround(plan.outLengthSec)));

    if (position >= plan.outStartSec) {
        beginTransition(plan, position);
        return;
    }
    const double trackBpm = main->getTrackData() ? main->getTrackData()->getBpm() : 0.0;
    const double rate = trackBpm > 0.0 && main->getCurrentBpm() > 0.0
        ? main->getCurrentBpm() / trackBpm : 1.0;
    setProgress((plan.outStartSec - position) / std::max(0.05, rate), 0.0);
}

void AutomixController::takeOverCrossfader()
{
    if (!m_mixer)
        return;
    m_mixer->setCrossfaderAssignment(channelId(0), QStringLiteral("A"));
    m_mixer->setCrossfaderAssignment(channelId(1), QStringLiteral("B"));
}

void AutomixController::beginTransition(const TransitionPlan& plan, double outPosition)
{
    DjEngine* const main = mainDeck();
    DjEngine* const incoming = otherDeck();
    if (!main || !incoming)
        return;
    m_plan = plan;
    m_mixNowRequested = false;
    const int in = 1 - m_main;

    m_userLow[m_main] = main->eqLow();
    m_userLow[in] = incoming->eqLow();
    m_eqTouched = true;
    takeOverCrossfader();
    if (m_mixer) {
        m_mixer->setCrossfaderPosition(static_cast<float>(sideOf(m_main)));
        m_mixer->setEqLow(channelId(in), kLowKill);
    }

    if (plan.beatAligned) {
        // The outgoing deck must lead: it is the one the audience hears.
        incoming->setSyncEnabled(false);
        main->setSyncEnabled(true);
    }
    const double duration = incoming->getDuration();
    if (duration > 0.0) {
        const double target = incomingPositionFor(plan, outPosition);
        incoming->setPosition(static_cast<float>(std::clamp(target, 0.0, duration) / duration));
    }
    if (plan.beatAligned)
        incoming->setSyncEnabled(true);
    incoming->play();
    if (plan.beatAligned)
        incoming->reSync();

    setState(State::Mixing);
    setProgress(0.0, 0.0);
}

void AutomixController::updateTransition()
{
    DjEngine* const main = mainDeck();
    DjEngine* const incoming = otherDeck();
    if (!incoming || !incoming->isPlaying()) {
        // The DJ stopped the incoming track: hand control back.
        abortTransition();
        return;
    }
    const double progress = main && m_plan.outLengthSec > 0.0
        ? (main->getPosition() - m_plan.outStartSec) / m_plan.outLengthSec
        : 1.0;
    if (!main || !main->isPlaying() || progress >= 1.0) {
        finishTransition();
        return;
    }
    const TransitionLevels levels = transitionLevelsAt(m_plan, progress);
    const int in = 1 - m_main;
    if (m_mixer) {
        const double from = sideOf(m_main);
        m_mixer->setCrossfaderPosition(static_cast<float>(from + (sideOf(in) - from) * levels.incoming));
        m_mixer->setEqLow(channelId(m_main),
                          m_userLow[m_main] + (kLowKill - m_userLow[m_main]) * levels.outLowKill);
        m_mixer->setEqLow(channelId(in),
                          m_userLow[in] + (kLowKill - m_userLow[in]) * levels.inLowKill);
    }
    setProgress(0.0, std::clamp(progress, 0.0, 1.0));
}

void AutomixController::restoreEq()
{
    m_eqTouched = false;
    if (!m_mixer)
        return;
    for (int index = 0; index < 2; ++index)
        m_mixer->setEqLow(channelId(index), m_userLow[index]);
}

void AutomixController::finishTransition()
{
    DjEngine* const main = mainDeck();
    const int in = 1 - m_main;
    if (m_mixer)
        m_mixer->setCrossfaderPosition(static_cast<float>(sideOf(in)));
    if (main) {
        if (main->isPlaying())
            main->pause();
        if (m_plan.beatAligned)
            main->setSyncEnabled(false);   // the new track takes over as sync leader
    }
    restoreEq();
    m_main = in;
    m_next = NextState::None;
    m_nextTitle.clear();
    m_nextArtist.clear();
    setPlanText({});
    setProgress(-1.0, 0.0);
    setState(State::Playing);
}

void AutomixController::abortTransition()
{
    restoreEq();
    if (m_mixer)
        m_mixer->setCrossfaderPosition(static_cast<float>(sideOf(m_main)));
    setEnabled(false);
}

void AutomixController::mixNow()
{
    if (!m_enabled || m_state != State::Playing || m_next != NextState::Ready)
        return;
    m_mixNowRequested = true;
    tick();
}

void AutomixController::skipNext()
{
    if (!m_enabled || m_state == State::Mixing || m_state == State::Starting)
        return;
    m_loadingDeck = -1;
    m_loadingPath.clear();
    m_next = NextState::None;
    m_mixNowRequested = false;
    m_queueExhausted = false;
    m_nextTitle.clear();
    m_nextArtist.clear();
    emit statusChanged();
    tick();
}

void AutomixController::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit statusChanged();
}

void AutomixController::setProgress(double secondsToMix, double mixProgress)
{
    if (std::abs(secondsToMix - m_secondsToMix) < 0.05 && std::abs(mixProgress - m_mixProgress) < 0.002)
        return;
    m_secondsToMix = secondsToMix;
    m_mixProgress = mixProgress;
    emit progressChanged();
}

void AutomixController::setPlanText(const QString& text)
{
    if (m_planText == text)
        return;
    m_planText = text;
    emit statusChanged();
}
