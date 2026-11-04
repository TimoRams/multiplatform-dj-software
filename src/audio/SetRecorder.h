#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <thread>
#include <vector>

// One audio producer, one disk consumer. The audio path only copies into a
// preallocated bounded ring. QFile, WAV conversion and rotation live on worker.
class SetRecorder final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    Q_PROPERTY(bool stopping READ stopping NOTIFY changed)
    Q_PROPERTY(QString filePath READ filePath NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(double elapsedSeconds READ elapsedSeconds NOTIFY changed)
    Q_PROPERTY(double freeSpaceMb READ freeSpaceMb NOTIFY changed)
    Q_PROPERTY(double peak READ peak NOTIFY changed)
public:
    explicit SetRecorder(QObject* parent = nullptr);
    ~SetRecorder() override;
    bool recording() const { return m_running.load(std::memory_order_acquire); }
    bool stopping() const { return recording() && !m_accepting.load(std::memory_order_acquire); }
    QString filePath() const { return m_path; }
    QString error() const;
    double elapsedSeconds() const;
    double freeSpaceMb() const;
    double peak() const { return m_peak.load(std::memory_order_relaxed); }
    void configure(double sampleRate) noexcept;
    void capture(const juce::AudioBuffer<float>& master, int samples) noexcept;
    Q_INVOKABLE bool startRecording(const QString& directory = {});
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void requestStopRecording();
    Q_INVOKABLE void toggleRecording();
    void setDirectory(const QString& directory) { m_directory = directory; }
    Q_INVOKABLE void markTrack();
signals:
    void changed();
private:
    void writeLoop(QString basePath, double rate);
    static constexpr std::uint64_t kCapacity = 1u << 19;
    std::vector<float> m_ring;
    std::atomic<std::uint64_t> m_written {0}, m_read {0}, m_frames {0};
    std::atomic<unsigned> m_readers {0};
    std::atomic<bool> m_accepting {false}, m_running {false};
    std::atomic<std::uint64_t> m_markAt {UINT64_MAX};
    std::atomic<int> m_failure {0};
    std::atomic<double> m_rate {0}, m_peak {0};
    double m_recordingRate = 0;
    QString m_path, m_startError, m_directory;
    QTimer m_timer;
    std::thread m_writer;
};
