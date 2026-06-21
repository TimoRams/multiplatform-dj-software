#pragma once

namespace controllers {

constexpr int kDeckCount = 4;
constexpr int kFlx10NativeDeckCount = 2;
constexpr int kFirstDeckIndex = 1;
constexpr int kLastDeckIndex = kDeckCount;

constexpr int kFlx10FirstDeckIndex = 1;
constexpr int kFlx10LastDeckIndex = kFlx10NativeDeckCount;

constexpr bool isValidDeckIndex(int deck) noexcept
{
    return deck >= kFirstDeckIndex && deck <= kLastDeckIndex;
}

constexpr bool isFlx10NativeDeck(int deck) noexcept
{
    return deck >= kFlx10FirstDeckIndex && deck <= kFlx10LastDeckIndex;
}

} // namespace controllers
