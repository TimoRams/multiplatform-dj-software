#pragma once

#include <QLatin1String>
#include <QString>
#include <QStringView>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace domain {

// QML/settings use "deckA"; hardware uses 1-based numbers. Convert at the edge.
enum class DeckId : std::uint8_t { A = 0, B = 1, C = 2, D = 3 };

inline constexpr int kDeckCount = 4;

inline constexpr std::array<DeckId, kDeckCount> kAllDecks {
    DeckId::A, DeckId::B, DeckId::C, DeckId::D
};

[[nodiscard]] constexpr std::size_t toIndex(DeckId deck) noexcept
{
    return static_cast<std::size_t>(deck);
}

[[nodiscard]] constexpr std::optional<DeckId> deckFromIndex(int index) noexcept
{
    if (index < 0 || index >= kDeckCount)
        return std::nullopt;
    return static_cast<DeckId>(static_cast<std::uint8_t>(index));
}

[[nodiscard]] constexpr int toHardwareNumber(DeckId deck) noexcept
{
    return static_cast<int>(toIndex(deck)) + 1;
}

[[nodiscard]] constexpr std::optional<DeckId> deckFromHardwareNumber(int number) noexcept
{
    return deckFromIndex(number - 1);
}

[[nodiscard]] inline const QString& toChannelId(DeckId deck)
{
    static const std::array<QString, kDeckCount> ids {
        QStringLiteral("deckA"), QStringLiteral("deckB"),
        QStringLiteral("deckC"), QStringLiteral("deckD")
    };
    return ids[toIndex(deck)];
}

[[nodiscard]] inline std::optional<DeckId> deckFromChannelId(QStringView channelId)
{
    if (channelId.size() != 5 || !channelId.startsWith(QLatin1String("deck")))
        return std::nullopt;
    const char16_t letter = channelId.at(4).unicode();
    if (letter < u'A' || letter > u'D')
        return std::nullopt;
    return static_cast<DeckId>(static_cast<std::uint8_t>(letter - u'A'));
}

template <typename T>
[[nodiscard]] constexpr const T& selectDeck(DeckId deck,
                                            const T& a, const T& b,
                                            const T& c, const T& d) noexcept
{
    switch (deck) {
    case DeckId::B: return b;
    case DeckId::C: return c;
    case DeckId::D: return d;
    case DeckId::A: break;
    }
    return a;
}

} // namespace domain

namespace TransportLimits {

// Transport preroll length in seconds. Independent of beatgrid placement.
constexpr double kPreRollSeconds = 32.0;

} // namespace TransportLimits

struct TrackSegment {
    QString label;
    float startTime = 0.0f;
    float endTime = 0.0f;
    QString colorHex;
    float confidence = 0.0f;
};
