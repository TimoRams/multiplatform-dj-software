#pragma once

#include <QRegularExpression>
#include <QString>

// Recognising the built-in controller and the mapping that ships with it.
// Lives next to the rest of the FLX10 code because these names describe one
// specific piece of hardware; the generic MIDI layer only asks the questions.
namespace flx10 {

inline const QString kControllerName = QStringLiteral("DDJ-FLX10");
inline const QString kMappingFile = QStringLiteral("DDJ-FLX10.brockdj.xml");
inline const QString kMappingLabel = QStringLiteral("Built-in: DDJ-FLX10");
inline const QString kMappingResource =
    QStringLiteral(":/controllers/mappings/midi/DDJ-FLX10.brockdj.xml");

inline bool isBuiltInMapping(const QString& mappingName)
{
    return mappingName == kMappingLabel || mappingName == kMappingFile;
}

// Device names arrive punctuated differently depending on the driver, so
// compare on letters and digits alone.
inline QString matchKey(QString value)
{
    static const QRegularExpression separators(QStringLiteral("[^a-z0-9]+"));
    value = value.toLower();
    value.replace(separators, QString());
    return value;
}

inline bool looksLikeControllerName(const QString& value)
{
    const QString key = matchKey(value);
    return key.contains(QStringLiteral("flx10")) || key.contains(QStringLiteral("ddjflx10"));
}

// Only the first two decks have jog displays and dedicated hardware channels on
// this unit; decks 3 and 4 are software-only. That is a property of this
// controller, not of decks in general, so the limit lives here. Deck numbers on
// the wire are 1-based — convert to domain::DeckId at the boundary.
inline constexpr int kNativeDeckCount = 2;
inline constexpr int kFirstNativeDeckNumber = 1;
inline constexpr int kLastNativeDeckNumber = kNativeDeckCount;

[[nodiscard]] constexpr bool isNativeDeckNumber(int deckNumber) noexcept
{
    return deckNumber >= kFirstNativeDeckNumber && deckNumber <= kLastNativeDeckNumber;
}

} // namespace flx10
