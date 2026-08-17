#include "platform/AudioThreadScheduling.h"

#include <cassert>
#include <cstring>
#include <iostream>

int main()
{
    using platform::AudioThreadScheduling;
    using platform::AudioThreadSchedulingState;

    assert(AudioThreadScheduling::normalizeRealtimePriority(-1)
           == AudioThreadScheduling::kDefaultRealtimePriority);
    assert(AudioThreadScheduling::normalizeRealtimePriority(0)
           == AudioThreadScheduling::kDefaultRealtimePriority);
    assert(AudioThreadScheduling::normalizeRealtimePriority(1) == 1);
    assert(AudioThreadScheduling::normalizeRealtimePriority(20) == 20);
    assert(AudioThreadScheduling::normalizeRealtimePriority(100)
           == AudioThreadScheduling::kMaximumRealtimePriority);

    AudioThreadScheduling scheduling;
    scheduling.requestRealtimePriority(20);
    const auto beforeCallback = scheduling.status();
#if defined(__linux__)
    assert(beforeCallback.state == AudioThreadSchedulingState::WaitingForCallback);
    assert(beforeCallback.callbackThreadGeneration == 0);

    scheduling.observeCallbackThread();
    const auto afterCallback = scheduling.status();
    assert(afterCallback.state == AudioThreadSchedulingState::WaitingForCallback);
    assert(afterCallback.callbackThreadGeneration == 1);

    scheduling.reset();
    const auto afterReset = scheduling.status();
    assert(afterReset.state == AudioThreadSchedulingState::WaitingForCallback);
    assert(afterReset.callbackThreadGeneration == 0);
#else
    assert(beforeCallback.state == AudioThreadSchedulingState::Unsupported);
#endif

    assert(std::strcmp(platform::audioThreadSchedulingStateName(
                            AudioThreadSchedulingState::Active),
                       "sched-fifo-active")
           == 0);
    assert(std::strcmp(platform::audioThreadSchedulingStateName(
                            AudioThreadSchedulingState::PermissionDenied),
                       "permission-denied")
           == 0);

    std::cout << "Audio thread scheduling tests passed\n";
    return 0;
}
