#include "platform/PosixSignalHandler.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    int closedReadFd = -1;
    int closedWriteFd = -1;

    {
        PosixSignalHandler handler;
        ok &= require(handler.initialize(), "self-pipe initialization must succeed");
        ok &= require(handler.initialize(), "repeated initialization must be idempotent");
        ok &= require(handler.isInitialized(), "component must report initialized state");

        closedReadFd = handler.nativeReadDescriptor();
        closedWriteFd = handler.nativeWriteDescriptor();
        const int readStatus = ::fcntl(closedReadFd, F_GETFL, 0);
        const int writeStatus = ::fcntl(closedWriteFd, F_GETFL, 0);
        const int readDescriptorFlags = ::fcntl(closedReadFd, F_GETFD, 0);
        const int writeDescriptorFlags = ::fcntl(closedWriteFd, F_GETFD, 0);
        ok &= require((readStatus & O_NONBLOCK) != 0 && (writeStatus & O_NONBLOCK) != 0,
                      "both pipe descriptors must be non-blocking");
        ok &= require((readDescriptorFlags & FD_CLOEXEC) != 0
                          && (writeDescriptorFlags & FD_CLOEXEC) != 0,
                      "both pipe descriptors must be close-on-exec");

        int shutdownCount = 0;
        QEventLoop loop;
        QObject::connect(&handler, &PosixSignalHandler::shutdownRequested, &loop, [&]() {
            ++shutdownCount;
            QTimer::singleShot(20, &loop, &QEventLoop::quit);
        });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);

        ::kill(::getpid(), SIGINT);
        ::kill(::getpid(), SIGINT);
        ::kill(::getpid(), SIGTERM);
        loop.exec();
        QCoreApplication::processEvents();

        ok &= require(shutdownCount == 1, "multiple signals must request shutdown exactly once");
        unsigned char byte = 0;
        errno = 0;
        const ssize_t remaining = ::read(closedReadFd, &byte, sizeof(byte));
        ok &= require(remaining < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
                      "notifier must drain the pipe completely");
    }

    errno = 0;
    ok &= require(::fcntl(closedReadFd, F_GETFD, 0) < 0 && errno == EBADF,
                  "read descriptor must close during destruction");
    errno = 0;
    ok &= require(::fcntl(closedWriteFd, F_GETFD, 0) < 0 && errno == EBADF,
                  "write descriptor must close during destruction");

    // Restoration and global-state cleanup must allow a fresh component.
    {
        PosixSignalHandler replacement;
        ok &= require(replacement.initialize(), "component must reinitialize after destruction");
    }

    QCoreApplication::processEvents();
    return ok ? 0 : 1;
}
