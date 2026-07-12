#include "PosixSignalHandler.h"

#include <QSocketNotifier>
#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

struct PosixSignalHandler::Private
{
    int readFd = -1;
    int writeFd = -1;
    std::unique_ptr<QSocketNotifier> notifier;
    QString error;
    bool shutdownRequested = false;
#if defined(Q_OS_UNIX)
    struct sigaction previousInt {};
    struct sigaction previousTerm {};
    bool intInstalled = false;
    bool termInstalled = false;
#endif
};

#if defined(Q_OS_UNIX)
namespace {

volatile sig_atomic_t g_signalWriteFd = -1;

extern "C" void handlePosixTerminationSignal(int signalNumber) noexcept
{
    const int savedErrno = errno;
    const int fd = static_cast<int>(g_signalWriteFd);
    if (fd >= 0) {
        const unsigned char value = static_cast<unsigned char>(signalNumber);
        const ssize_t ignored = ::write(fd, &value, sizeof(value));
        static_cast<void>(ignored);
    }
    errno = savedErrno;
}

bool setDescriptorFlags(int fd)
{
    const int statusFlags = ::fcntl(fd, F_GETFL, 0);
    if (statusFlags < 0 || ::fcntl(fd, F_SETFL, statusFlags | O_NONBLOCK) < 0)
        return false;

    const int descriptorFlags = ::fcntl(fd, F_GETFD, 0);
    return descriptorFlags >= 0
        && ::fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0;
}

QString systemError(const char* operation)
{
    return QStringLiteral("%1 failed: %2")
        .arg(QString::fromLatin1(operation), QString::fromLocal8Bit(std::strerror(errno)));
}

} // namespace
#endif

PosixSignalHandler::PosixSignalHandler(QObject* parent)
    : QObject(parent), d(std::make_unique<Private>())
{
}

PosixSignalHandler::~PosixSignalHandler()
{
    reset();
}

bool PosixSignalHandler::initialize()
{
#if !defined(Q_OS_UNIX)
    d->error = QStringLiteral("POSIX signal handling is unavailable on this platform");
    return false;
#else
    if (isInitialized())
        return true;
    if (g_signalWriteFd >= 0) {
        d->error = QStringLiteral("another POSIX signal handler is already active");
        return false;
    }

    int pipeFds[2] {-1, -1};
    if (::pipe(pipeFds) != 0) {
        d->error = systemError("pipe");
        return false;
    }
    d->readFd = pipeFds[0];
    d->writeFd = pipeFds[1];
    if (!setDescriptorFlags(d->readFd) || !setDescriptorFlags(d->writeFd)) {
        d->error = systemError("fcntl");
        reset();
        return false;
    }

    struct sigaction action {};
    action.sa_handler = handlePosixTerminationSignal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;

    g_signalWriteFd = static_cast<sig_atomic_t>(d->writeFd);
    if (::sigaction(SIGINT, &action, &d->previousInt) != 0) {
        d->error = systemError("sigaction(SIGINT)");
        reset();
        return false;
    }
    d->intInstalled = true;
    if (::sigaction(SIGTERM, &action, &d->previousTerm) != 0) {
        d->error = systemError("sigaction(SIGTERM)");
        reset();
        return false;
    }
    d->termInstalled = true;

    d->notifier = std::make_unique<QSocketNotifier>(d->readFd, QSocketNotifier::Read, this);
    connect(d->notifier.get(), &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { drainPipe(); });
    d->error.clear();
    return true;
#endif
}

bool PosixSignalHandler::isInitialized() const noexcept
{
    return d->readFd >= 0 && d->writeFd >= 0 && d->notifier != nullptr;
}

QString PosixSignalHandler::errorString() const
{
    return d->error;
}

int PosixSignalHandler::nativeReadDescriptor() const noexcept
{
    return d->readFd;
}

int PosixSignalHandler::nativeWriteDescriptor() const noexcept
{
    return d->writeFd;
}

void PosixSignalHandler::drainPipe()
{
#if defined(Q_OS_UNIX)
    d->notifier->setEnabled(false);
    unsigned char buffer[64];
    for (;;) {
        const ssize_t count = ::read(d->readFd, buffer, sizeof(buffer));
        if (count > 0)
            continue;
        if (count < 0 && errno == EINTR)
            continue;
        break; // EOF, EAGAIN or an unrecoverable descriptor error
    }

    if (!d->shutdownRequested) {
        d->shutdownRequested = true;
        emit shutdownRequested();
    }
    // Once shutdown has been requested, later signals may enqueue bytes but can
    // never start a second Qt shutdown path. A full non-blocking pipe is benign.
#endif
}

void PosixSignalHandler::reset() noexcept
{
#if defined(Q_OS_UNIX)
    sigset_t blockedSignals {};
    sigset_t previousMask {};
    ::sigemptyset(&blockedSignals);
    ::sigaddset(&blockedSignals, SIGINT);
    ::sigaddset(&blockedSignals, SIGTERM);
    const bool maskChanged = ::sigprocmask(SIG_BLOCK, &blockedSignals, &previousMask) == 0;

    if (d->termInstalled)
        ::sigaction(SIGTERM, &d->previousTerm, nullptr);
    if (d->intInstalled)
        ::sigaction(SIGINT, &d->previousInt, nullptr);
    d->termInstalled = false;
    d->intInstalled = false;
    g_signalWriteFd = -1;

    if (d->notifier)
        d->notifier->setEnabled(false);
    d->notifier.reset();
    if (d->readFd >= 0)
        ::close(d->readFd);
    if (d->writeFd >= 0)
        ::close(d->writeFd);
    d->readFd = -1;
    d->writeFd = -1;
    d->shutdownRequested = false;

    if (maskChanged)
        ::sigprocmask(SIG_SETMASK, &previousMask, nullptr);
#endif
}
