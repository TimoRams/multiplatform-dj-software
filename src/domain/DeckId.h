#pragma once

#include <QLatin1String>
#include <QString>
#include <QStringView>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace domain {

// The one way to name a deck inside the application. QML, settings and MIDI
// parameter ids all speak "deckA"; some hardware counts decks from 1. Both are
// wire formats — convert at the edge that produces or consumes them and pass
// DeckId everywhere in between, so a typo cannot silently address nothing.
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

// Hardware and protocol numbering is 1-based.
[[nodiscard]] constexpr int toHardwareNumber(DeckId deck) noexcept
{
    return static_cast<int>(toIndex(deck)) + 1;
}

[[nodiscard]] constexpr std::optional<DeckId> deckFromHardwareNumber(int number) noexcept
{
    return deckFromIndex(number - 1);
}

// The "deckA".."deckD" spelling used by QML, the parameter store and settings.
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

// Picks one of four per-deck values without spelling out the branch chain at
// every call site. Deliberately a template so this header stays free of any
// dependency on what the caller happens to be holding — raw pointers, smart
// pointers and guarded handles all work.
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
