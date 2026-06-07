#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <QHash>
#include <QString>
#include <optional>

namespace metadata {

constexpr double kParamEpsilon = 1e-6;

[[nodiscard]] bool nearlyEqual(double a, double b);
[[nodiscard]] QString fromJuce(const juce::String& s);
[[nodiscard]] QString cleanup(QString text);
[[nodiscard]] QString normaliseKey(const QString& key);
[[nodiscard]] QHash<QString, QString> buildMetadataLookup(const juce::StringPairArray& metadata);
[[nodiscard]] QString metaValue(const QHash<QString, QString>& map,
                                std::initializer_list<const char*> candidates);

struct Id3v1Tag {
    QString title;
    QString artist;
    QString album;
    QString year;
};

[[nodiscard]] std::optional<Id3v1Tag> readId3v1(const QString& path);
void filenameHeuristic(const QString& baseName, QString& title, QString& artist);
[[nodiscard]] double parseBpmString(const QString& raw);

} // namespace metadata
