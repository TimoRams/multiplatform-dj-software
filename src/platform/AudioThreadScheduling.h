#pragma once

#include <atomic>
#include <cstdint>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace platform {

enum class AudioThreadSchedulingState : std::uint8_t {
    Unsupported,
    WaitingForCallback,
    Active,
    AlreadyRealtime,
    PermissionDenied,
    Failed
};

struct AudioThreadSchedulingStatus {
    AudioThreadSchedulingState state = AudioThreadSchedulingState::Unsupported;
    int nativeError = 0;
    int scheduler = 0;
    int priority = 0;
    std::uint64_t callbackThreadGeneration = 0;
};

class AudioThreadScheduling final {
public:
    static constexpr int kDefaultRealtimePriority = 20;
    static constexpr int kMaximumRealtimePriority = 40;

    static constexpr int normalizeRealtimePriority(int priority) noexcept
    {
        if (priority <= 0)
            return kDefaultRealtimePriority;
        return priority > kMaximumRealtimePriority ? kMaximumRealtimePriority : priority;
    }

    // Audio callback only: captures a native thread handle without a syscall,
    // lock, allocation, or priority change.
    void observeCallbackThread() noexcept;

    // Control thread only: requests the policy after the device has started.
    void requestRealtimePriority(int requestedPriority) noexcept;
    void reset() noexcept;

    [[nodiscard]] AudioThreadSchedulingStatus status() const noexcept;

private:
    void setStatus(AudioThreadSchedulingState state,
                   int nativeError,
                   int scheduler,
                   int priority) noexcept;

#if defined(__linux__)
    std::atomic<pthread_t> m_callbackThread {pthread_t{}};
#endif
    std::atomic<std::uint64_t> m_callbackThreadGeneration {0};
    std::atomic<std::uint64_t> m_attemptedGeneration {0};
    std::atomic<AudioThreadSchedulingState> m_state {
#if defined(__linux__)
        AudioThreadSchedulingState::WaitingForCallback
#else
        AudioThreadSchedulingState::Unsupported
#endif
    };
    std::atomic<int> m_nativeError {0};
    std::atomic<int> m_scheduler {0};
    std::atomic<int> m_priority {0};
};

[[nodiscard]] const char* audioThreadSchedulingStateName(
    AudioThreadSchedulingState state) noexcept;

} // namespace platform
