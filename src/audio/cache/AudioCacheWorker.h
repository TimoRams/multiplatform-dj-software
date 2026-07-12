#pragma once

#include <atomic>
#include <thread>

class AudioPageCache;

class AudioCacheWorker final
{
public:
    explicit AudioCacheWorker(AudioPageCache& owner);
    ~AudioCacheWorker();
    void notify() noexcept;
    void shutdownAndJoin() noexcept;

private:
    AudioPageCache& m_owner;
    std::atomic<bool> m_shutdown{false};
    std::thread m_thread;
};
