#include "DjEngine.h"
#include "CoverArtExtractor.h"
#include "CoverArtProvider.h"
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QDateTime>
#include <QRegularExpression>
#include <juce_core/juce_core.h>
#include <cstring>

// Metadata utilities: normalise and query JUCE StringPairArray across all tag formats
// (ID3v2, Vorbis comments, MP4 atoms, etc.).
namespace {

QString fromJuce(const juce::String& s) {
    return QString::fromUtf8(s.toRawUTF8());
}

QString cleanup(QString text) {
    if (text.isEmpty()) return text;
    text.replace(QRegularExpression(QStringLiteral("[\\x00\\r\\n\\t]+")), QStringLiteral(" "));
    return text.simplified().trimmed();
}

QString normaliseKey(const QString& key) {
    QString result;
    result.reserve(key.size());
    for (const QChar ch : key.trimmed().toLower())
        if (ch.isLetterOrNumber()) result.append(ch);
    return result;
}

QHash<QString,QString> buildMetadataLookup(const juce::StringPairArray& metadata) {
    QHash<QString,QString> map;
    auto keys   = metadata.getAllKeys();
    auto values = metadata.getAllValues();
    for (int i = 0; i < metadata.size(); ++i) {
        QString val = cleanup(fromJuce(values[i]));
        if (val.isEmpty()) continue;
        QString nk = normaliseKey(fromJuce(keys[i]));
        if (!nk.isEmpty() && !map.contains(nk))
            map.insert(nk, val);
        // Also index each colon-separated segment so "ID3:Title" matches as "title".
        QString raw = cleanup(fromJuce(keys[i]));
        if (raw.contains(QLatin1Char(':'))) {
            for (const auto& part : raw.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
                QString alt = normaliseKey(part);
                if (!alt.isEmpty() && !map.contains(alt))
                    map.insert(alt, val);
            }
        }
    }
    return map;
}

QString metaValue(const QHash<QString,QString>& map, std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) {
        auto it = map.constFind(normaliseKey(QString::fromUtf8(c)));
        if (it != map.cend()) return it.value();
    }
    return {};
}

// ID3v1 fallback: reads the 128-byte trailer appended to the end of the file.
struct Id3v1Tag { QString title, artist, album, year; };

std::optional<Id3v1Tag> readId3v1(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 128) return std::nullopt;
    f.seek(f.size() - 128);
    QByteArray data = f.read(128);
    if (data.size() != 128 || std::memcmp(data.constData(), "TAG", 3) != 0) return std::nullopt;
    Id3v1Tag t;
    t.title  = cleanup(QString::fromLatin1(data.mid(3, 30)));
    t.artist = cleanup(QString::fromLatin1(data.mid(33, 30)));
    t.album  = cleanup(QString::fromLatin1(data.mid(63, 30)));
    t.year   = cleanup(QString::fromLatin1(data.mid(93, 4)));
    return t;
}

// Heuristic: split "Artist - Title" filenames when tags are absent.
void filenameHeuristic(const QString& baseName, QString& title, QString& artist) {
    if (title.isEmpty()) title = baseName;
    if (artist.isEmpty()) {
        static const QRegularExpression pat(QStringLiteral("^\\s*(.+?)\\s*[-–]\\s*(.+)\\s*$"));
        auto m = pat.match(baseName);
        if (m.hasMatch()) {
            QString a = cleanup(m.captured(1));
            QString t = cleanup(m.captured(2));
            if (!a.isEmpty()) artist = a;
            if (!t.isEmpty()) title  = t;
        }
    }
}

double parseBpmString(const QString& raw) {
    if (raw.isEmpty()) return 0.0;
    QString c = raw.trimmed().replace(QLatin1Char(','), QLatin1Char('.'));
    static const QRegularExpression numPat(QStringLiteral("([0-9]+(?:\\.[0-9]+)?)"));
    auto m = numPat.match(c);
    if (m.hasMatch()) { bool ok; double v = m.captured(1).toDouble(&ok); if (ok) return v; }
    return 0.0;
}

} // anon namespace

DjEngine::DjEngine(QObject* parent) : QObject(parent)
{
    m_trackData = new TrackData(this);
    m_analyzer  = new WaveformAnalyzer(m_trackData, &formatManager, 150);

    // When the analyzer detects a key, override the (often absent) ID3 key field.
    connect(m_trackData, &TrackData::keyAnalyzed, this, [this]() {
        QString analysedKey = m_trackData->getDetectedKey();
        if (!analysedKey.isEmpty()) {
            m_trackKey = analysedKey;
            emit trackMetadataChanged();
        }
    });

    juce::MessageManager::getInstance();
    formatManager.registerBasicFormats();

    juce::String err = deviceManager.initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty()) {
        qWarning() << "JUCE AudioDeviceManager err:" << QString::fromStdString(err.toStdString());
    }

    deviceManager.addAudioCallback(&sourcePlayer);
    sourcePlayer.setSource(&transportSource);

    // Compute total hardware latency (output latency + current buffer size).
    // getVisualPosition() subtracts this from the transport read pointer so the
    // waveform display tracks the audio that is actually reaching the speakers.
    if (auto* device = deviceManager.getCurrentAudioDevice()) {
        int latencySamples = device->getOutputLatencyInSamples()
                           + device->getCurrentBufferSizeSamples();
        double sr = device->getCurrentSampleRate();
        if (sr > 0.0)
            m_latencySeconds = static_cast<float>(latencySamples / sr);
        qDebug() << "[DjEngine] Hardware latency:" << latencySamples
                 << "samples =" << m_latencySeconds << "s"
                 << "(out:" << device->getOutputLatencyInSamples()
                 << "buf:" << device->getCurrentBufferSizeSamples() << ")";
    } else {
        m_latencySeconds = 0.011f;
        qDebug() << "[DjEngine] No audio device yet, using fallback latency 11ms";
    }
    m_snapClock.start();

    connect(&timer, &QTimer::timeout, this, &DjEngine::onTimer);
    timer.start(16);
}

DjEngine::~DjEngine()
{
    if (m_analyzer) {
        m_analyzer->stopAnalysis();
        delete m_analyzer;
    }
    sourcePlayer.setSource(nullptr);
    deviceManager.removeAudioCallback(&sourcePlayer);
    transportSource.setSource(nullptr);
}

float DjEngine::getProgress() const
{
    if (transportSource.getTotalLength() > 0.0)
        return static_cast<float>(transportSource.getCurrentPosition() / transportSource.getLengthInSeconds());
    return 0.0f;
}

float DjEngine::getDuration() const
{
    return static_cast<float>(transportSource.getLengthInSeconds());
}

float DjEngine::getPosition() const
{
    return static_cast<float>(transportSource.getCurrentPosition());
}

float DjEngine::getVisualPosition() const
{
    if (!m_snapValid || !transportSource.isPlaying())
        return getPosition();

    // Forward-interpolate from the last snapshot using elapsed wall-clock time.
    // This keeps the waveform smooth between onTimer() ticks (every 16 ms).
    double elapsed = static_cast<double>(m_snapClock.nsecsElapsed()) * 1e-9;
    double interpolated = m_snapPosition + elapsed;

    // Subtract hardware latency so the display shows what is currently audible,
    // not the audio-thread read pointer which is latencySeconds ahead.
    double compensated = interpolated - static_cast<double>(m_latencySeconds);
    compensated = std::max(0.0, compensated);

    double len = transportSource.getLengthInSeconds();
    if (len > 0.0)
        compensated = std::min(compensated, len);

    return static_cast<float>(compensated);
}

void DjEngine::setCoverArtProvider(CoverArtProvider* provider, const QString& deckId)
{
    m_coverProvider = provider;
    m_deckId = deckId;
}

bool DjEngine::isPlaying() const
{
    return transportSource.isPlaying();
}

TrackData* DjEngine::getTrackData() const
{
    return m_trackData;
}

void DjEngine::loadTrack(const QString& rawPath)
{
    juce::File file(rawPath.toStdString());

    if (!file.existsAsFile()) {
        qWarning() << "File does not exist:" << rawPath;
        return;
    }

    auto* reader = formatManager.createReaderFor(file);
    if (reader != nullptr)
    {
        const auto metaMap = buildMetadataLookup(reader->metadataValues);

        qDebug() << "=== METADATA for" << rawPath << "(" << metaMap.size() << "keys) ===";
        for (auto it = metaMap.cbegin(); it != metaMap.cend(); ++it)
            qDebug() << "  " << it.key() << "=" << it.value();

        m_trackTitle  = metaValue(metaMap, {"title", "id3title", "tit2", "tt2", "name", "tracktitle", "song"});
        m_trackArtist = metaValue(metaMap, {"artist", "id3artist", "tpe1", "albumartist", "tpe2", "band", "performer", "leadartist"});
        m_trackAlbum  = metaValue(metaMap, {"album", "id3album", "talb", "record", "albumtitle"});
        m_trackKey    = metaValue(metaMap, {"key", "tkey", "initialkey", "musickey", "keysig"});

        // Tag BPM is used immediately; the background analyzer will overwrite it later.
        QString tagBpm = metaValue(metaMap, {"bpm", "tbpm", "tmpo", "tempo", "beatsperminute"});
        double bpmVal  = parseBpmString(tagBpm);
        if (bpmVal > 0.0)
            m_trackData->setBpmData(bpmVal, 0, reader->sampleRate);

        auto v1 = readId3v1(rawPath);
        if (v1) {
            if (m_trackTitle.isEmpty()  && !v1->title.isEmpty())  m_trackTitle  = v1->title;
            if (m_trackArtist.isEmpty() && !v1->artist.isEmpty()) m_trackArtist = v1->artist;
            if (m_trackAlbum.isEmpty()  && !v1->album.isEmpty())  m_trackAlbum  = v1->album;
        }

        QString baseName = cleanup(QString::fromStdString(
            file.getFileNameWithoutExtension().toStdString()));
        filenameHeuristic(baseName, m_trackTitle, m_trackArtist);

        double durationSec = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        int mins = static_cast<int>(durationSec) / 60;
        int secs = static_cast<int>(durationSec) % 60;
        m_trackDuration = QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));

        m_hasCoverArt = false;
        m_coverArtUrl.clear();
        if (m_coverProvider) {
            auto [coverData, coverFmt] = CoverArtExtractor::extractCoverArt(rawPath);
            if (!coverData.isEmpty()) {
                m_coverProvider->setCover(m_deckId, coverData);
                // Append a timestamp query parameter to bust QML's image:// URL cache.
                m_coverArtUrl = QString("image://coverart/%1?t=%2")
                                    .arg(m_deckId)
                                    .arg(QDateTime::currentMSecsSinceEpoch());
                m_hasCoverArt = true;
                qDebug() << "[DjEngine] Cover art loaded:" << coverFmt
                         << coverData.size() << "bytes";
            } else {
                m_coverProvider->clearCover(m_deckId);
                qDebug() << "[DjEngine] No cover art found";
            }
        }

        m_hasTrack = true;

        qDebug() << "[DjEngine] title=" << m_trackTitle
                 << " artist=" << m_trackArtist
                 << " key=" << m_trackKey;

        emit trackMetadataChanged();

        transportSource.stop();
        transportSource.setSource(nullptr);

        readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        transportSource.setSource(readerSource.get(), 0, nullptr, reader->sampleRate);
        transportSource.setPosition(0.0);

        m_analyzer->startAnalysis(rawPath);

        emit trackLoaded();
        emit progressChanged();
    }
}

void DjEngine::togglePlay()
{
    if (transportSource.isPlaying())
        transportSource.stop();
    else
        transportSource.start();
        
    emit playingChanged();
}

void DjEngine::setPosition(float progress)
{
    double len = transportSource.getLengthInSeconds();
    if (len > 0.0)
        transportSource.setPosition(progress * len);
    emit progressChanged();
}

void DjEngine::onTimer()
{
    if (transportSource.isPlaying()) {
        // Store a position snapshot with a matching wall-clock timestamp.
        // getVisualPosition() forward-interpolates from here for each render frame.
        m_snapPosition = transportSource.getCurrentPosition();
        m_snapClock.restart();
        m_snapValid = true;
        emit progressChanged();
    } else {
        m_snapValid = false;
    }
}
