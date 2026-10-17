#pragma once

#include <QByteArray>
#include <QString>

#include <memory>

// Only the screen's shared IOHID service is opened; never the USB device or
// audio/MIDI interfaces. All writes run on the controller's existing writer.
class Flx10MacHidTransport final
{
public:
    Flx10MacHidTransport();
    ~Flx10MacHidTransport();
    bool open(QString& error);
    void close() noexcept;
    void beginWrites();
    void endWrites() noexcept;
    int write(const QByteArray& packet, int& transferred);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
