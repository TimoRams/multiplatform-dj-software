#pragma once

#include "controllers/midi/MidiClockScheduler.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace juce { class MidiOutput; class MidiInput; }
class SettingsManager;

// MIDI beat clock out (24 PPQN + START/STOP) for drum machines, grooveboxes
// and synths, and MIDI clock in as an external tempo source for the sampler
// sequencer. Output runs on its own thread; the GUI thread only publishes the
// master clock snapshot.
class MidiClockController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList outputDevices READ outputDevices NOTIFY devicesChanged)
    Q_PROPERTY(QStringList inputDevices READ inputDevices NOTIFY devicesChanged)
    Q_PROPERTY(bool outputEnabled READ outputEnabled WRITE setOutputEnabled NOTIFY changed)
    Q_PROPERTY(QString outputDevice READ outputDevice WRITE setOutputDevice NOTIFY changed)
    Q_PROPERTY(bool sendTransport READ sendTransport WRITE setSendTransport NOTIFY changed)
    Q_PROPERTY(bool inputEnabled READ inputEnabled WRITE setInputEnabled NOTIFY changed)
    Q_PROPERTY(QString inputDevice READ inputDevice WRITE setInputDevice NOTIFY changed)
    Q_PROPERTY(QString outputStatus READ outputStatus NOTIFY statusChanged)
    Q_PROPERTY(QString inputStatus READ inputStatus NOTIFY statusChanged)
    Q_PROPERTY(bool inputActive READ inputActive NOTIFY statusChanged)
    Q_PROPERTY(double inputBpm READ inputBpm NOTIFY statusChanged)

public:
    explicit MidiClockController(SettingsManager* settings, QObject* parent = nullptr);
    ~MidiClockController() override;

    QStringList outputDevices() const { return m_outputNames; }
    QStringList inputDevices() const { return m_inputNames; }
    bool outputEnabled() const { return m_outputEnabled; }
    QString outputDevice() const { return m_outputDevice; }
    bool sendTransport() const { return m_sendTransport; }
    bool inputEnabled() const { return m_inputEnabled; }
    QString inputDevice() const { return m_inputDevice; }
    QString outputStatus() const { return m_outputStatus; }
    QString inputStatus() const { return m_inputStatus; }
    bool inputActive() const { return m_inputActive; }
    double inputBpm() const { return m_inputBpm; }

    void setOutputEnabled(bool enabled);
    void setOutputDevice(const QString& name);
    void setSendTransport(bool enabled);
    void setInputEnabled(bool enabled);
    void setInputDevice(const QString& name);

    Q_INVOKABLE void refreshDevices();

    // GUI thread: master clock from the sampler/sequencer clock arbiter.
    void publishMasterClock(const brockdj::midiclock::ClockSnapshot& snapshot);
    // GUI thread: external clock if MIDI clock in is enabled and locked.
    [[nodiscard]] brockdj::midiclock::ClockSnapshot externalClock(double now) const;

    void shutdown();

signals:
    void devicesChanged();
    void changed();
    void statusChanged();

private:
    class InputCallback;

    void reopenOutput();
    void reopenInput();
    void stopOutputThread();
    void outputLoop();
    void sendStatus(std::uint8_t status);
    void setOutputStatus(const QString& status);
    void setInputStatus(const QString& status);
    void refreshInputState();
    void persist();

    SettingsManager* m_settings = nullptr;
    QStringList m_outputNames, m_outputIds, m_inputNames, m_inputIds;
    bool m_outputEnabled = false;
    bool m_sendTransport = true;
    bool m_inputEnabled = false;
    QString m_outputDevice, m_inputDevice;
    QString m_outputStatus = QStringLiteral("Aus"), m_inputStatus = QStringLiteral("Aus");
    bool m_inputActive = false;
    double m_inputBpm = 0.0;
    bool m_shutdown = false;

    std::unique_ptr<juce::MidiOutput> m_output;
    std::thread m_outputThread;
    std::mutex m_outputMutex;
    std::condition_variable m_outputWake;
    bool m_outputStopping = false;
    brockdj::midiclock::ClockSnapshot m_masterClock;
    std::atomic<bool> m_sendTransportFlag {true};

    std::unique_ptr<InputCallback> m_inputCallback;
    std::unique_ptr<juce::MidiInput> m_input;
    mutable std::mutex m_inputMutex;
    brockdj::midiclock::MidiClockFollower m_follower;
    QTimer m_inputPoll;
};
