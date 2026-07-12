#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QSocketNotifier;

class PosixSignalHandler final : public QObject
{
    Q_OBJECT

public:
    explicit PosixSignalHandler(QObject* parent = nullptr);
    ~PosixSignalHandler() override;

    PosixSignalHandler(const PosixSignalHandler&) = delete;
    PosixSignalHandler& operator=(const PosixSignalHandler&) = delete;

    bool initialize();
    [[nodiscard]] bool isInitialized() const noexcept;
    [[nodiscard]] QString errorString() const;

    // Exposed for the focused descriptor-lifetime test and diagnostics only.
    [[nodiscard]] int nativeReadDescriptor() const noexcept;
    [[nodiscard]] int nativeWriteDescriptor() const noexcept;

signals:
    void shutdownRequested();

private:
    void drainPipe();
    void reset() noexcept;

    struct Private;
    std::unique_ptr<Private> d;
};
