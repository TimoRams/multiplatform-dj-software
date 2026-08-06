#pragma once

#include "audio/AudioRouting.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

struct AudioParameters {
    float masterGain = 1.0f;
    bool limiterEnabled = false;
    int masterFxType = 0;
    float masterFxAmount = 0.0f;
    float masterFxExternalDelaySeconds = -1.0f;
    float masterFxPrimaryParameter = 0.5f;

    float crossfaderPosition = 0.0f;
    CrossfaderCurve crossfaderCurve = CrossfaderCurve::ConstantPower;
    std::array<CrossfaderAssignment, 4> crossfaderAssignments {
        CrossfaderAssignment::A,
        CrossfaderAssignment::B,
        CrossfaderAssignment::A,
        CrossfaderAssignment::B
    };

    float headphoneMix = 0.5f;
    float headphoneGain = 1.0f;
    bool masterCueEnabled = false;
    std::array<bool, 4> pflEnabled { false, false, false, false };

    int masterFirstChannel = 1;
    int headphonesFirstChannel = -1;
    int boothFirstChannel = -1;
};

// A wait-free audio-thread snapshot. The writer only touches its private back
// slot, then exchanges it with the mailbox. The reader similarly exchanges its
// private front slot, so neither side can access the same slot concurrently.
template <typename Parameters>
class RealtimeSnapshotStore final {
public:
    RealtimeSnapshotStore() = default;

    template <typename Update>
    void update(Update&& update)
    {
        std::lock_guard lock(m_writerMutex);
        std::forward<Update>(update)(m_writerState);
        m_slots[static_cast<std::size_t>(m_back)] = m_writerState;
        const auto previous = m_mailbox.exchange(
            static_cast<unsigned>(m_back) | kDirtyBit, std::memory_order_acq_rel);
        m_back = static_cast<int>(previous & kIndexMask);
    }

    [[nodiscard]] Parameters snapshot() noexcept
    {
        const auto pending = m_mailbox.load(std::memory_order_acquire);
        if ((pending & kDirtyBit) != 0u) {
            const auto previous = m_mailbox.exchange(
                static_cast<unsigned>(m_front), std::memory_order_acq_rel);
            m_front = static_cast<int>(previous & kIndexMask);
        }
        return m_slots[static_cast<std::size_t>(m_front)];
    }

    [[nodiscard]] Parameters controlSnapshot() const
    {
        std::lock_guard lock(m_writerMutex);
        return m_writerState;
    }

private:
    mutable std::mutex m_writerMutex;
    Parameters m_writerState {};
    std::array<Parameters, 3> m_slots {};
    static constexpr unsigned kDirtyBit = 1u << 31;
    static constexpr unsigned kIndexMask = ~kDirtyBit;
    std::atomic<unsigned> m_mailbox { 1 };
    int m_front = 0;
    int m_back = 2;
};

using AudioParameterStore = RealtimeSnapshotStore<AudioParameters>;

enum class AudioCommandType : std::uint8_t {
    Play,
    Pause,
    Seek,
    SetLoop,
    ClearLoop,
    BeginScratch,
    EndScratch,
    ResetDeck
};

struct AudioCommand {
    AudioCommandType type = AudioCommandType::Pause;
    double valueA = 0.0;
    double valueB = 0.0;
    std::uint64_t generation = 0;
};

// SPSC control-to-audio queue. Overflow deliberately drops the newest command:
// the producer never mutates the consumer cursor and the audio thread never
// waits. The sticky overflow counter makes the loss observable.
template <std::size_t Capacity = 64>
class AudioCommandQueue final {
public:
    static_assert(Capacity >= 2);

    [[nodiscard]] bool push(const AudioCommand& command) noexcept
    {
        const auto write = m_write.load(std::memory_order_relaxed);
        const auto next = increment(write);
        if (next == m_read.load(std::memory_order_acquire)) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_commands[write] = command;
        m_write.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(AudioCommand& command) noexcept
    {
        const auto read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire))
            return false;
        command = m_commands[read];
        m_read.store(increment(read), std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t droppedCommands() const noexcept
    {
        return m_dropped.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t increment(std::size_t index) noexcept
    {
        return (index + 1) % Capacity;
    }

    std::array<AudioCommand, Capacity> m_commands {};
    std::atomic<std::size_t> m_read { 0 };
    std::atomic<std::size_t> m_write { 0 };
    std::atomic<std::uint64_t> m_dropped { 0 };
};
