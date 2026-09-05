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

struct TagLibTags {
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString comment;
    QString year;
    QString trackNumber;
    double bpm = 0.0;
};

// JUCE's own decoders don't agree on tag parsing across platforms: on macOS,
// CoreAudioFormat is registered ahead of the MP3/format-specific readers and
// silently returns no ID3 metadata at all for many files, which previously
// left title/artist to a naive "filename split" heuristic and produced
// swapped/garbled results. TagLib parses ID3v1/v2, Vorbis comments and MP4
// atoms identically on every platform, so it is used as the authoritative
// source of tag data regardless of which JUCE reader decoded the audio.
[[nodiscard]] std::optional<TagLibTags> readTagLibTags(const QString& path);

} // namespace metadata
