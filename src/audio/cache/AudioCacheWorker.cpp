#include "AudioCacheWorker.h"
#include "AudioPageCache.h"

AudioCacheWorker::AudioCacheWorker(AudioPageCache& owner)
    : m_owner(owner), m_thread([this] { m_owner.workerRun(m_shutdown); })
{
}

AudioCacheWorker::~AudioCacheWorker() { shutdownAndJoin(); }
void AudioCacheWorker::notify() noexcept { m_owner.notifyWorker(); }

void AudioCacheWorker::shutdownAndJoin() noexcept
{
    if (m_shutdown.exchange(true, std::memory_order_acq_rel)) return;
    m_owner.notifyWorker();
    if (m_thread.joinable()) m_thread.join();
}
