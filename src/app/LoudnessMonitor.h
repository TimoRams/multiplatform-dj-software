#pragma once

#include "audio/LoudnessMeter.h"

#include <QObject>
#include <QTimer>

#include <functional>

class SettingsManager;

// Master loudness (EBU R128) for QML. Drains the audio thread's 100 ms
// K-weighted blocks ten times a second. Values are LUFS; -70 means silence.
class LoudnessMonitor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(double momentary READ momentary NOTIFY changed)
    Q_PROPERTY(double shortTerm READ shortTerm NOTIFY changed)
    Q_PROPERTY(double integrated READ integrated NOTIFY changed)
    Q_PROPERTY(double maxShortTerm READ maxShortTerm NOTIFY changed)
    Q_PROPERTY(bool hasSignal READ hasSignal NOTIFY changed)
    Q_PROPERTY(double targetLufs READ targetLufs WRITE setTargetLufs NOTIFY targetLufsChanged)
    Q_PROPERTY(double floorLufs READ floorLufs CONSTANT)
public:
    using BlockSource = std::function<int(const std::function<void(float)>&)>;

    explicit LoudnessMonitor(QObject* parent = nullptr);

    // Tests inject their own block source; the default drains AudioEngine.
    void setBlockSource(BlockSource source) { m_source = std::move(source); }
    void attachSettingsManager(SettingsManager* settings);
    void poll();

    [[nodiscard]] double momentary() const { return m_momentary; }
    [[nodiscard]] double shortTerm() const { return m_shortTerm; }
    [[nodiscard]] double integrated() const { return m_integrated; }
    [[nodiscard]] double maxShortTerm() const { return m_maxShortTerm; }
    [[nodiscard]] bool hasSignal() const { return m_momentary > brockdj::audio::kLufsFloor; }
    [[nodiscard]] double targetLufs() const { return m_targetLufs; }
    void setTargetLufs(double value);
    [[nodiscard]] static double floorLufs() { return brockdj::audio::kLufsFloor; }

    // Restarts the integrated measurement (e.g. at the start of a set).
    Q_INVOKABLE void reset();

signals:
    void changed();
    void targetLufsChanged();

private:
    void publish();

    brockdj::audio::LoudnessAccumulator m_accumulator;
    BlockSource m_source;
    SettingsManager* m_settings = nullptr;
    QTimer m_timer;
    double m_momentary = brockdj::audio::kLufsFloor;
    double m_shortTerm = brockdj::audio::kLufsFloor;
    double m_integrated = brockdj::audio::kLufsFloor;
    double m_maxShortTerm = brockdj::audio::kLufsFloor;
    double m_targetLufs = -9.0;
};
