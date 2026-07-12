#include "DeckCueLoopController.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr double kMinLoopSeconds = 0.001;
constexpr double kMinLoopBeats = 1.0 / 64.0;
constexpr double kMaxLoopBeats = 4096.0;
}

DeckCueLoopController::DeckCueLoopController()
{
    clearHotCues();
    clearSavedLoops();
}

void DeckCueLoopController::beginTrack(std::uint64_t generation)
{
    m_trackGeneration = generation;
    m_mainCue = {};
    m_activeLoop = {};
    m_pendingCueJump = {};
    clearHotCues();
    clearSavedLoops();
}

bool DeckCueLoopController::validIndex(int index) const noexcept
{
    return index >= 0 && index < kSlotCount;
}

QString DeckCueLoopController::defaultHotCueColor(int index)
{
    static constexpr const char* colors[] = {
        "#e04040", "#e08030", "#e0c030", "#40c040",
        "#3080e0", "#8040e0", "#e040a0", "#40c0c0"
    };
    return QString::fromLatin1(colors[static_cast<unsigned>(index) % kSlotCount]);
}

QString DeckCueLoopController::defaultSavedLoopColor(int index)
{
    static constexpr const char* colors[] = {
        "#30b050", "#3080e0", "#e08030", "#8040e0",
        "#e04040", "#40c0c0", "#e0c030", "#e040a0"
    };
    return QString::fromLatin1(colors[static_cast<unsigned>(index) % kSlotCount]);
}

void DeckCueLoopController::clearHotCues()
{
    for (int i = 0; i < kSlotCount; ++i)
        m_hotCues[static_cast<size_t>(i)] = HotCue{.color = defaultHotCueColor(i)};
}

void DeckCueLoopController::clearSavedLoops()
{
    for (int i = 0; i < kSlotCount; ++i)
        m_savedLoops[static_cast<size_t>(i)] = SavedLoop{.color = defaultSavedLoopColor(i)};
}

bool DeckCueLoopController::setHotCue(int index, double positionSec, QString label, QString color)
{
    if (!validIndex(index) || !std::isfinite(positionSec))
        return false;
    HotCue next{true, positionSec, std::move(label), std::move(color)};
    if (next.label.isEmpty()) next.label = QStringLiteral("HOT CUE %1").arg(index + 1);
    if (next.color.isEmpty()) next.color = defaultHotCueColor(index);
    auto& slot = m_hotCues[static_cast<size_t>(index)];
    if (slot == next) return false;
    slot = std::move(next);
    return true;
}

bool DeckCueLoopController::clearHotCue(int index)
{
    if (!validIndex(index)) return false;
    auto& slot = m_hotCues[static_cast<size_t>(index)];
    const HotCue next{.color = slot.color.isEmpty() ? defaultHotCueColor(index) : slot.color};
    if (slot == next) return false;
    slot = next;
    return true;
}

bool DeckCueLoopController::setHotCueColor(int index, QString color)
{
    if (!validIndex(index)) return false;
    if (color.trimmed().isEmpty()) color = defaultHotCueColor(index);
    auto& slot = m_hotCues[static_cast<size_t>(index)];
    if (slot.color == color) return false;
    slot.color = std::move(color);
    return true;
}

bool DeckCueLoopController::setSavedLoop(int index, double inSec, double outSec,
                                         double lengthBeats, QString label, QString color)
{
    if (!validIndex(index) || !std::isfinite(inSec) || !std::isfinite(outSec)
        || outSec <= inSec + kMinLoopSeconds) return false;
    SavedLoop next{true, inSec, outSec,
                   std::clamp(std::isfinite(lengthBeats) ? lengthBeats : 4.0,
                              kMinLoopBeats, kMaxLoopBeats),
                   std::move(label), std::move(color)};
    if (next.label.isEmpty()) next.label = QStringLiteral("LOOP %1").arg(index + 1);
    if (next.color.isEmpty()) next.color = defaultSavedLoopColor(index);
    auto& slot = m_savedLoops[static_cast<size_t>(index)];
    if (slot == next) return false;
    slot = std::move(next);
    return true;
}

bool DeckCueLoopController::clearSavedLoop(int index)
{
    if (!validIndex(index)) return false;
    auto& slot = m_savedLoops[static_cast<size_t>(index)];
    const SavedLoop next{.color = defaultSavedLoopColor(index)};
    if (slot == next) return false;
    slot = next;
    return true;
}

bool DeckCueLoopController::setLoop(double inSec, double outSec, double lengthBeats, bool active)
{
    if (!std::isfinite(inSec) || !std::isfinite(outSec) || outSec <= inSec + kMinLoopSeconds)
        return false;
    m_activeLoop = {active, true, inSec, outSec,
                    std::clamp(std::isfinite(lengthBeats) ? lengthBeats : kMinLoopBeats,
                               kMinLoopBeats, kMaxLoopBeats)};
    return true;
}

bool DeckCueLoopController::clearLoop() noexcept
{
    if (!m_activeLoop.active && !m_activeLoop.inSet && m_activeLoop.lengthBeats == 0.0) return false;
    m_activeLoop = {};
    return true;
}

bool DeckCueLoopController::deactivateLoop() noexcept
{
    if (!m_activeLoop.active) return false;
    m_activeLoop.active = false;
    return true;
}

bool DeckCueLoopController::reactivateLoop() noexcept
{
    if (m_activeLoop.active || !m_activeLoop.inSet
        || m_activeLoop.outSec <= m_activeLoop.inSec + kMinLoopSeconds) return false;
    m_activeLoop.active = true;
    return true;
}

void DeckCueLoopController::scheduleCueJump(double targetSec, double fireSec,
                                            double currentPosition) noexcept
{
    m_pendingCueJump = {true, fireSec, targetSec, currentPosition, m_trackGeneration};
}

void DeckCueLoopController::cancelCueJump() noexcept { m_pendingCueJump.active = false; }

std::optional<double> DeckCueLoopController::serviceCueJump(double currentPosition) noexcept
{
    if (!m_pendingCueJump.active || m_pendingCueJump.trackGeneration != m_trackGeneration)
        return std::nullopt;
    const bool reached = currentPosition + 1e-4 >= m_pendingCueJump.fireSec;
    const bool wrapped = currentPosition < m_pendingCueJump.lastPositionSec - 0.02;
    m_pendingCueJump.lastPositionSec = currentPosition;
    if (!reached && !wrapped) return std::nullopt;
    m_pendingCueJump.active = false;
    return m_pendingCueJump.targetSec;
}

double DeckCueLoopController::beatDurationAround(double sec, const BeatGridSnapshot& grid) noexcept
{
    if (grid.beats.size() >= 2) {
        const auto it = std::upper_bound(grid.beats.begin(), grid.beats.end(), sec);
        const auto prev = it != grid.beats.begin() ? std::prev(it) : grid.beats.begin();
        if (std::next(prev) != grid.beats.end()) {
            const double d = *std::next(prev) - *prev;
            if (d > 1e-3) return d;
        }
        if (prev != grid.beats.begin()) {
            const double d = *prev - *std::prev(prev);
            if (d > 1e-3) return d;
        }
    }
    return grid.bpm > 0.0 ? 60.0 / grid.bpm : 0.5;
}

double DeckCueLoopController::quantizedBeatAt(double sec, const BeatGridSnapshot& grid) noexcept
{
    if (!grid.beats.empty()) {
        const auto it = std::upper_bound(grid.beats.begin(), grid.beats.end(), sec);
        if (it == grid.beats.begin()) return grid.beats.front();
        if (it == grid.beats.end()) return grid.beats.back();
        const auto prev = std::prev(it);
        return sec - *prev <= *it - sec ? *prev : *it;
    }
    if (grid.bpm <= 0.0) return sec;
    const double duration = 60.0 / grid.bpm;
    return grid.firstBeatSec + std::round((sec - grid.firstBeatSec) / duration) * duration;
}

double DeckCueLoopController::nextBeatBoundaryAfter(double sec, const BeatGridSnapshot& grid) noexcept
{
    constexpr double epsilon = 1e-4;
    if (!grid.beats.empty()) {
        const auto it = std::upper_bound(grid.beats.begin(), grid.beats.end(), sec + epsilon);
        if (it != grid.beats.end()) return *it;
        const double duration = beatDurationAround(sec, grid);
        if (duration > 1e-4)
            return grid.beats.back() + (std::floor((sec - grid.beats.back()) / duration) + 1.0) * duration;
        return sec;
    }
    if (grid.bpm <= 0.0) return sec;
    const double duration = 60.0 / grid.bpm;
    return grid.firstBeatSec + (std::floor((sec - grid.firstBeatSec) / duration + epsilon) + 1.0) * duration;
}
