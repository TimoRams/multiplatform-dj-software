#include "AudioThreadScheduling.h"

#include <algorithm>

#if defined(__linux__)
#include <cerrno>
#include <sched.h>
#endif

namespace platform {

void AudioThreadScheduling::observeCallbackThread() noexcept
{
#if defined(__linux__)
    const pthread_t currentThread = pthread_self();
    if (m_callbackThread.load(std::memory_order_relaxed) == currentThread)
        return;

    m_callbackThread.store(currentThread, std::memory_order_release);
    m_callbackThreadGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_attemptedGeneration.store(0, std::memory_order_release);
    setStatus(AudioThreadSchedulingState::WaitingForCallback, 0, SCHED_OTHER, 0);
#endif
}

void AudioThreadScheduling::requestRealtimePriority(int requestedPriority) noexcept
{
#if !defined(__linux__)
    static_cast<void>(requestedPriority);
    setStatus(AudioThreadSchedulingState::Unsupported, 0, 0, 0);
#else
    const auto generation = m_callbackThreadGeneration.load(std::memory_order_acquire);
    if (generation == 0) {
        setStatus(AudioThreadSchedulingState::WaitingForCallback, 0, SCHED_OTHER, 0);
        return;
    }

    if (m_attemptedGeneration.exchange(generation, std::memory_order_acq_rel) == generation)
        return;

    const pthread_t callbackThread = m_callbackThread.load(std::memory_order_acquire);
    int currentScheduler = SCHED_OTHER;
    sched_param currentParameters {};
    const int currentResult = pthread_getschedparam(
        callbackThread, &currentScheduler, &currentParameters);
    if (currentResult != 0) {
        setStatus(AudioThreadSchedulingState::Failed, currentResult, SCHED_OTHER, 0);
        return;
    }

    if (currentScheduler == SCHED_FIFO || currentScheduler == SCHED_RR) {
        setStatus(AudioThreadSchedulingState::AlreadyRealtime, 0, currentScheduler,
                  currentParameters.sched_priority);
        return;
    }

    const int minimumPriority = sched_get_priority_min(SCHED_FIFO);
    const int maximumPriority = sched_get_priority_max(SCHED_FIFO);
    if (minimumPriority < 0 || maximumPriority < minimumPriority) {
        setStatus(AudioThreadSchedulingState::Failed, errno, currentScheduler,
                  currentParameters.sched_priority);
        return;
    }

    const int priority = std::clamp(normalizeRealtimePriority(requestedPriority),
                                    minimumPriority,
                                    std::min(kMaximumRealtimePriority, maximumPriority));
    sched_param parameters {};
    parameters.sched_priority = priority;
    const int result = pthread_setschedparam(callbackThread, SCHED_FIFO, &parameters);
    if (result == 0) {
        setStatus(AudioThreadSchedulingState::Active, 0, SCHED_FIFO, priority);
    } else if (result == EPERM || result == EACCES) {
        setStatus(AudioThreadSchedulingState::PermissionDenied, result,
                  currentScheduler, currentParameters.sched_priority);
    } else {
        setStatus(AudioThreadSchedulingState::Failed, result,
                  currentScheduler, currentParameters.sched_priority);
    }
#endif
}

void AudioThreadScheduling::reset() noexcept
{
#if defined(__linux__)
    m_callbackThread.store(pthread_t{}, std::memory_order_release);
    setStatus(AudioThreadSchedulingState::WaitingForCallback, 0, SCHED_OTHER, 0);
#else
    setStatus(AudioThreadSchedulingState::Unsupported, 0, 0, 0);
#endif
    m_callbackThreadGeneration.store(0, std::memory_order_release);
    m_attemptedGeneration.store(0, std::memory_order_release);
}

AudioThreadSchedulingStatus AudioThreadScheduling::status() const noexcept
{
    return {
        .state = m_state.load(std::memory_order_acquire),
        .nativeError = m_nativeError.load(std::memory_order_relaxed),
        .scheduler = m_scheduler.load(std::memory_order_relaxed),
        .priority = m_priority.load(std::memory_order_relaxed),
        .callbackThreadGeneration = m_callbackThreadGeneration.load(std::memory_order_acquire)
    };
}

void AudioThreadScheduling::setStatus(AudioThreadSchedulingState state,
                                      int nativeError,
                                      int scheduler,
                                      int priority) noexcept
{
    m_nativeError.store(nativeError, std::memory_order_relaxed);
    m_scheduler.store(scheduler, std::memory_order_relaxed);
    m_priority.store(priority, std::memory_order_relaxed);
    m_state.store(state, std::memory_order_release);
}

const char* audioThreadSchedulingStateName(AudioThreadSchedulingState state) noexcept
{
    switch (state) {
    case AudioThreadSchedulingState::Unsupported:
        return "unsupported";
    case AudioThreadSchedulingState::WaitingForCallback:
        return "waiting-for-callback";
    case AudioThreadSchedulingState::Active:
        return "sched-fifo-active";
    case AudioThreadSchedulingState::AlreadyRealtime:
        return "already-realtime";
    case AudioThreadSchedulingState::PermissionDenied:
        return "permission-denied";
    case AudioThreadSchedulingState::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace platform
