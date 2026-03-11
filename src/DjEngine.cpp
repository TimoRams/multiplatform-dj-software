#include "DjEngine.h"
#include "CoverArtExtractor.h"
#include "CoverArtProvider.h"
#include "FxProcessor.h"
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QDateTime>
#include <QRegularExpression>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <cstring>
#include <algorithm>
#include <cmath>

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

class DjEngine::MixerDspSource : public juce::AudioSource {
public:
    MixerDspSource(juce::AudioSource* inSource, juce::AudioTransportSource* transport)
        : source(inSource), m_transport(transport) {}

    // ── FxProcessor slot (called from Qt main thread) ──────────────────────
    void setFxEffectType(EffectType type) { m_fx.setEffectType(type); }
    void setFxAmount(float amount)        { m_fx.setAmount(amount); }
    void setFxSCKnob(float knob)          { m_fx.setSCKnobValue(knob); }

    void setReverse(bool rev) { m_reverse.store(rev, std::memory_order_relaxed); }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
        if (source) source->prepareToPlay(samplesPerBlockExpected, sampleRate);

        m_fx.prepare(sampleRate, samplesPerBlockExpected, 2);
        
        juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(samplesPerBlockExpected), 2 };
        lowEq.prepare(spec);
        midEq.prepare(spec);
        highEq.prepare(spec);
        colorFilter.prepare(spec);
        
        m_sampleRate = sampleRate;
        updateFilters();
    }

    void releaseResources() override {
        if (source) source->releaseResources();
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override {
        if (source) source->getNextAudioBlock(bufferToFill);

        if (m_sampleRate <= 0.0 || bufferToFill.buffer->getNumChannels() == 0 || bufferToFill.numSamples == 0) return;

        juce::dsp::AudioBlock<float> block(*bufferToFill.buffer);
        auto slicedBlock = block.getSubBlock(bufferToFill.startSample, bufferToFill.numSamples);
        juce::dsp::ProcessContextReplacing<float> context(slicedBlock);

        // Map inputs directly. Note: QML trim goes 0..2.
        // QML volume goes 0..1. Multiply both.
        float gain = static_cast<float>(trimVal * faderVal);
        if (std::abs(gain - 1.0f) > 0.001f) {
            for (size_t ch = 0; ch < slicedBlock.getNumChannels(); ++ch) {
                juce::FloatVectorOperations::multiply(slicedBlock.getChannelPointer(ch), gain, static_cast<int>(slicedBlock.getNumSamples()));
            }
        }

        lowEq.process(context);
        midEq.process(context);
        highEq.process(context);
        colorFilter.process(context);

        // ── FX slot (Reverb / Bitcrusher / PitchShifter) ──────────────────────
        m_fx.process(*bufferToFill.buffer,
                     bufferToFill.startSample,
                     bufferToFill.numSamples);

        // ── Reverse: flip samples in block + scrub transport backwards ─────────
        if (m_reverse.load(std::memory_order_relaxed) && m_transport && m_sampleRate > 0)
        {
            const int s = bufferToFill.startSample;
            const int numSamp = bufferToFill.numSamples;

            // 1. Reverse the samples in the block so audio plays backwards
            for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch)
            {
                float* ptr = bufferToFill.buffer->getWritePointer(ch) + s;
                std::reverse(ptr, ptr + numSamp);
            }

            // 2. Seek transport backwards by 2× block length:
            //    - 1× to undo the forward read that just happened
            //    - 1× to actually move backwards
            double currentPos = m_transport->getCurrentPosition();
            double blockDur   = static_cast<double>(numSamp) / m_sampleRate;
            double newPos     = currentPos - 2.0 * blockDur;
            if (newPos < 0.0) newPos = 0.0;
            m_transport->setPosition(newPos);
        }
    }

    void setTrim(float val) { trimVal = val; }
    void setFader(float val) { faderVal = val; }

    void setEq(float l, float m, float h) {
        // -1 to +1 -> approx -32 dB to +6 dB (Pioneer DJM A9 is -infinity to +6)
        lowVol = l;
        midVol = m;
        highVol = h;
        // avoid continuous recomputation; just flag it or recompute directly
        updateFilters();
    }

    void setFilterVal(float f) {
        filterVal = f;
        updateFilters();
    }

private:
    float getDecibelsFromKnob(float kb) const {
        if (kb < 0.0f) {
            return kb * 32.0f; // -1 -> -32 dB (approx -inf / kill)
        } else {
            return kb * 6.0f;  // +1 -> +6 dB
        }
    }

    void updateFilters() {
        if (m_sampleRate <= 0) return;
        
        // Update EQs using standard DJ shelving/peak frequencies
        *lowEq.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(m_sampleRate, 250.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(lowVol)));
        *midEq.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(m_sampleRate, 1000.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(midVol)));
        *highEq.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(m_sampleRate, 2500.0f, 0.707f, juce::Decibels::decibelsToGain(getDecibelsFromKnob(highVol)));

        // Color Filter (LPF/HPF combo)
        if (std::abs(filterVal) < 0.05f) {
            // Flat response / completely bypassed
            // Since we can't 'bypass' perfectly, we just set a flat peak filter
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(m_sampleRate, 1000.0f, 0.707f, 1.0f);
        } else if (filterVal < 0.0f) {
            // Low Pass: sweep down from 20000 to ~80 Hz
            // t goes from 1.0 down to 0
            float t = 1.0f + filterVal;
            float freq = 80.0f * std::pow(20000.0f / 80.0f, t);
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(m_sampleRate, std::max(20.0f, freq), 1.2f);
        } else {
            // High Pass: sweep up from 20 to ~10000 Hz
            float freq = 20.0f * std::pow(10000.0f / 20.0f, filterVal);
            *colorFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(m_sampleRate, std::max(20.0f, freq), 1.2f);
        }
    }

    juce::AudioSource* source = nullptr;
    juce::AudioTransportSource* m_transport = nullptr;
    double m_sampleRate = 0;

    std::atomic<float> trimVal{1.0f};
    std::atomic<float> faderVal{1.0f};
    
    // UI vals from -1 to 1
    std::atomic<float> lowVol{0.0f};
    std::atomic<float> midVol{0.0f};
    std::atomic<float> highVol{0.0f};
    std::atomic<float> filterVal{0.0f};

    using FilterType = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    FilterType lowEq;
    FilterType midEq;
    FilterType highEq;
    FilterType colorFilter;

    FxProcessor m_fx;
    std::atomic<bool> m_reverse { false };
};

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

    // When BPM analysis finishes, re-emit tempoChanged so that currentBpm
    // and tempoRatio Q_PROPERTYs update in QML.
    connect(m_trackData, &TrackData::bpmAnalyzed, this, [this]() {
        emit tempoChanged();
    });

    juce::MessageManager::getInstance();
    formatManager.registerBasicFormats();

    juce::String err = deviceManager.initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty()) {
        qWarning() << "JUCE AudioDeviceManager err:" << QString::fromStdString(err.toStdString());
    }

    deviceManager.addAudioCallback(&sourcePlayer);
    
    // Create the resampling source that wraps the transport source.
    // This allows us to control tempo/speed without changing the pitch.
    resamplingSource = std::make_unique<juce::ResamplingAudioSource>(
        &transportSource, 
        false,  // deleteSourceWhenDeleted = false (we own transportSource separately)
        2       // channels = 2 (stereo)
    );
    
    // Create the mixer DSP source to apply EQ, Filter, and Gain based on Pioneer DJM A9.
    mixerSource = std::make_unique<MixerDspSource>(resamplingSource.get(), &transportSource);
    mixerSource->setTrim(static_cast<float>(m_trim));
    mixerSource->setFader(static_cast<float>(m_volume));

    sourcePlayer.setSource(mixerSource.get());

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

    // When reverse is on, interpolate backwards instead of forwards
    double interpolated = m_isReverse
        ? m_snapPosition - elapsed
        : m_snapPosition + elapsed;

    // Subtract hardware latency so the display shows what is currently audible,
    // not the audio-thread read pointer which is latencySeconds ahead.
    double compensated = m_isReverse
        ? interpolated + static_cast<double>(m_latencySeconds)   // reverse: add latency
        : interpolated - static_cast<double>(m_latencySeconds);  // forward: subtract latency

    double len = transportSource.getLengthInSeconds();
    compensated = std::clamp(compensated, 0.0, len > 0.0 ? len : compensated);

    return static_cast<float>(compensated);
}

double DjEngine::getPlayheadPositionAtomic() const
{
    // Lock-free load — always returns the last value written by onTimer().
    // QML FrameAnimation calls this every VSync frame; must never block.
    return m_atomicPlayheadPos.load(std::memory_order_relaxed);
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
        // Also update the atomic so QML FrameAnimation can poll it lock-free.
        m_atomicPlayheadPos.store(m_snapPosition, std::memory_order_relaxed);
        emit progressChanged();
    } else {
        m_snapValid = false;
        // Keep atomic in sync with the stopped position.
        m_atomicPlayheadPos.store(transportSource.getCurrentPosition(),
                                   std::memory_order_relaxed);
    }
}

// ─── Scrub API ────────────────────────────────────────────────────────────────

void DjEngine::pauseForScrub()
{
    // Stop audio output immediately.  We deliberately do NOT emit playingChanged()
    // so the QML FrameAnimation keeps ticking and the waveform stays live.
    transportSource.stop();
    m_snapValid    = false;
    m_isScrubbing  = true;
}

void DjEngine::scrubBy(double pixelDelta)
{
    // Convert pixel delta → time delta.
    // The waveform maps (pixelsPerPoint × 150 pts/s) pixels to 1 second.
    // Dragging right  → positive pixelDelta → waveform moves right
    //                 → playhead moves BACKWARD (we "pull" the record left).
    // Dragging left   → negative pixelDelta → playhead moves FORWARD.
    // ∴ timeDelta = −pixelDelta / pixelsPerSecond
    if (m_pixelsPerSecond <= 0.0) return;

    double timeDelta = -pixelDelta / m_pixelsPerSecond;

    double len     = transportSource.getLengthInSeconds();
    double current = transportSource.getCurrentPosition();
    double newPos  = std::clamp(current + timeDelta, 0.0, len > 0.0 ? len : 0.0);

    transportSource.setPosition(newPos);

    // Keep the atomic + snap in sync so getVisualPosition() stays correct
    // while transport is stopped during scrub.
    m_atomicPlayheadPos.store(newPos, std::memory_order_relaxed);
    m_snapPosition = newPos;
}

void DjEngine::resumeAfterScrub()
{
    m_isScrubbing = false;
    transportSource.start();
    m_snapClock.restart();
    m_snapValid = true;
    emit playingChanged();
}

void DjEngine::setDownbeatAtCurrentPosition()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed()) return;

    double anchorSec     = static_cast<double>(getVisualPosition());
    double trackLengthSec = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLengthSec <= 0.0) return;

    // Delegate rebuild + emit beatgridChanged() to TrackData.
    m_trackData->shiftBeatgridToDownbeat(anchorSec, trackLengthSec);
}

// Helper: find the positionSec of the downbeat (isDownbeat == true) nearest
// to currentSec in the existing beat grid.  Falls back to currentSec itself
// if the grid is empty or has no downbeats.
static double nearestDownbeatAnchor(const std::vector<TrackData::BeatMarker>& grid,
                                    double currentSec)
{
    double best     = currentSec;
    double bestDist = std::numeric_limits<double>::max();
    for (const auto& m : grid) {
        if (!m.isDownbeat) continue;
        double d = std::abs(m.positionSec - currentSec);
        if (d < bestDist) { bestDist = d; best = m.positionSec; }
    }
    return best;
}

void DjEngine::doubleBpm()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed()) return;
    double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLen <= 0.0) return;

    double currentSec = static_cast<double>(getVisualPosition());
    double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    double newBpm = m_trackData->getBpm() * 2.0;
    m_trackData->setBpm(newBpm);
    m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);
    emit tempoChanged();   // update BPM display in UI
}

void DjEngine::halveBpm()
{
    if (!m_trackData || !m_trackData->isBpmAnalyzed()) return;
    double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    if (trackLen <= 0.0) return;

    double currentSec = static_cast<double>(getVisualPosition());
    double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    double newBpm = m_trackData->getBpm() / 2.0;
    m_trackData->setBpm(newBpm);
    m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);
    emit tempoChanged();   // update BPM display in UI
}

// ──────────────────────────────────────────────────────────────────────────────

void DjEngine::setTempoPercent(double percent)
{
    // Clamp to ±100% range (WIDE mode)
    percent = std::clamp(percent, -100.0, 100.0);
    
    if (m_tempoPercent == percent) return;
    
    m_tempoPercent = percent;
    
    // Calculate speed multiplier: 1.0 = 100% = normal speed
    // +8% = 1.08, -8% = 0.92
    double speedMultiplier = 1.0 + (percent / 100.0);
    
    // Apply to the resampling source for pitch-preserving tempo change
    if (resamplingSource) {
        resamplingSource->setResamplingRatio(speedMultiplier);
    }
    
    qDebug() << "[DjEngine] Tempo set to" << percent << "%" 
             << "(speed:" << speedMultiplier << "x)";
    
    emit tempoChanged();
}

void DjEngine::updateGain()
{
    if (mixerSource) {
        mixerSource->setFader(static_cast<float>(m_volume));
        mixerSource->setTrim(static_cast<float>(m_trim));
    }
}

void DjEngine::setVolume(double value)
{
    if (m_volume != value) {
        m_volume = value;
        updateGain();
        emit volumeChanged();
    }
}

void DjEngine::setTrim(double value)
{
    if (m_trim != value) {
        m_trim = value;
        updateGain();
        emit trimChanged();
    }
}

void DjEngine::setEqHigh(double value)
{
    if (m_eqHigh != value) {
        m_eqHigh = value;
        if (mixerSource) mixerSource->setEq(static_cast<float>(m_eqLow), static_cast<float>(m_eqMid), static_cast<float>(m_eqHigh));
        emit eqHighChanged();
    }
}

void DjEngine::setEqMid(double value)
{
    if (m_eqMid != value) {
        m_eqMid = value;
        if (mixerSource) mixerSource->setEq(static_cast<float>(m_eqLow), static_cast<float>(m_eqMid), static_cast<float>(m_eqHigh));
        emit eqMidChanged();
    }
}

void DjEngine::setEqLow(double value)
{
    if (m_eqLow != value) {
        m_eqLow = value;
        if (mixerSource) mixerSource->setEq(static_cast<float>(m_eqLow), static_cast<float>(m_eqMid), static_cast<float>(m_eqHigh));
        emit eqLowChanged();
    }
}

void DjEngine::setFilter(double value)
{
    if (m_filter != value) {
        m_filter = value;
        if (mixerSource) mixerSource->setFilterVal(static_cast<float>(m_filter));
        emit filterChanged();
    }
}

void DjEngine::setCueEnabled(bool value)
{
    if (m_cueEnabled != value) {
        m_cueEnabled = value;
        // In a real app, route this to a separate headphone/monitor audio output
        emit cueEnabledChanged();
    }
}

void DjEngine::setFxEffectType(EffectType type)
{
    if (mixerSource) mixerSource->setFxEffectType(type);
}

void DjEngine::setFxWetDry(float amount)
{
    if (mixerSource) mixerSource->setFxAmount(amount);
}

void DjEngine::setFxSCKnob(float knob)
{
    if (mixerSource) mixerSource->setFxSCKnob(knob);
}

void DjEngine::setReverse(bool on)
{
    if (m_isReverse == on) return;
    m_isReverse = on;
    if (mixerSource) mixerSource->setReverse(on);
    emit reverseChanged();
}
