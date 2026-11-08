#pragma once

#include "deck/automix/AutomixPlanner.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class DjEngine;
class LibraryDatabase;
class MixerControl;
class SettingsManager;

// Plays the library queue (TrackQueue) on decks A/B with phrase-aware,
// beat-matched transitions. Runs on the UI thread from a 50 ms timer and only
// drives public deck and mixer controls, exactly as a DJ would.
class AutomixController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(int transitionBars READ transitionBars WRITE setTransitionBars NOTIFY transitionBarsChanged)
    Q_PROPERTY(QString state READ state NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString currentDeck READ currentDeck NOTIFY statusChanged)
    Q_PROPERTY(QString nextTitle READ nextTitle NOTIFY statusChanged)
    Q_PROPERTY(QString nextArtist READ nextArtist NOTIFY statusChanged)
    Q_PROPERTY(QString planText READ planText NOTIFY statusChanged)
    Q_PROPERTY(double secondsToMix READ secondsToMix NOTIFY progressChanged)
    Q_PROPERTY(double mixProgress READ mixProgress NOTIFY progressChanged)

public:
    explicit AutomixController(QObject* parent = nullptr);
    ~AutomixController() override;

    void attach(DjEngine* deckA, DjEngine* deckB, MixerControl* mixer,
                LibraryDatabase* library, SettingsManager* settings);

    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    void setEnabled(bool enabled);
    [[nodiscard]] int transitionBars() const noexcept { return m_settings.transitionBars; }
    void setTransitionBars(int bars);
    [[nodiscard]] QString state() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString currentDeck() const;
    [[nodiscard]] QString nextTitle() const { return m_nextTitle; }
    [[nodiscard]] QString nextArtist() const { return m_nextArtist; }
    [[nodiscard]] QString planText() const { return m_planText; }
    [[nodiscard]] double secondsToMix() const noexcept { return m_secondsToMix; }
    [[nodiscard]] double mixProgress() const noexcept { return m_mixProgress; }

    // Starts the transition on the next bar line.
    Q_INVOKABLE void mixNow();
    // Drops the prepared track and prepares the following queue entry.
    Q_INVOKABLE void skipNext();
    Q_INVOKABLE void tick();

signals:
    void enabledChanged();
    void transitionBarsChanged();
    void statusChanged();
    void progressChanged();

private:
    enum class State { Off, Waiting, Starting, Playing, Mixing };
    enum class NextState { None, Loading, Ready };

    [[nodiscard]] DjEngine* deck(int index) const;
    [[nodiscard]] DjEngine* mainDeck() const { return deck(m_main); }
    [[nodiscard]] DjEngine* otherDeck() const { return deck(1 - m_main); }
    [[nodiscard]] QString channelId(int index) const;
    [[nodiscard]] double sideOf(int index) const { return index == 0 ? -1.0 : 1.0; }
    [[nodiscard]] brockdj::automix::TrackTimeline timelineOf(const DjEngine& engine) const;

    bool loadNextFromQueue(int deckIndex);
    void startFromIdle(bool allowResume);
    void playStartedDeck();
    void beginTransition(const brockdj::automix::TransitionPlan& plan, double outPosition);
    void updateTransition();
    void finishTransition();
    void abortTransition();
    void restoreEq();
    void takeOverCrossfader();
    void onDeckLoaded(int deckIndex);
    void onDeckLoadError(int deckIndex);
    void setState(State state);
    void setProgress(double secondsToMix, double mixProgress);
    void setPlanText(const QString& text);

    QPointer<DjEngine> m_decks[2];
    QPointer<MixerControl> m_mixer;
    QPointer<LibraryDatabase> m_library;
    SettingsManager* m_settingsManager = nullptr;
    QTimer m_timer;

    brockdj::automix::AutomixSettings m_settings;
    brockdj::automix::TransitionPlan m_plan;
    bool m_enabled = false;
    bool m_mixNowRequested = false;
    bool m_queueExhausted = false;   // cleared when the queue changes
    State m_state = State::Off;
    NextState m_next = NextState::None;
    int m_main = 0;
    int m_loadingDeck = -1;
    QString m_loadingPath;
    QString m_nextTitle;
    QString m_nextArtist;
    QString m_planText;
    double m_secondsToMix = -1.0;
    double m_mixProgress = 0.0;
    double m_userLow[2] {0.0, 0.0};
    bool m_eqTouched = false;
};
