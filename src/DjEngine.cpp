#include "DjEngine.h"
#include "BrickwallLimiter.h"
#include "CoverArtExtractor.h"
#include "CoverArtProvider.h"
#include "FxProcessor.h"
#include "LibraryDatabase.h"
#include "TrackIdGenerator.h"
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QDateTime>
#include <QRegularExpression>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <rubberband/RubberBandStretcher.h>
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

}

class DjEngine::TimeStretchAudioSource : public juce::AudioSource {
public:
    TimeStretchAudioSource(juce::AudioSource* inSource) : source(inSource) {}

    void setTempoRatio(double ratio) {
        if (std::abs(tempoRatio - ratio) < 0.001) return;
        tempoRatio = ratio;
        if (stretcher) {
            stretcher->setTimeRatio(1.0 / tempoRatio);
        }
    }

    void prepareToPlay(int samplesPerBlockExpected, double sr) override {
        sampleRate = sr;
        if (source) source->prepareToPlay(samplesPerBlockExpected, sr);
        stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
            sr, 2, 
            RubberBand::RubberBandStretcher::OptionProcessRealTime |
            RubberBand::RubberBandStretcher::OptionPitchHighQuality);
        
        scratchBuffer.setSize(2, 8192);
        outputBuffer.setSize(2, 65536);
        fifo = std::make_unique<juce::AbstractFifo>(65536);
        wasBypassed = true;
        justUnbypassed = false;
    }

    void releaseResources() override {
        if (source) source->releaseResources();
        stretcher.reset();
        fifo.reset();
    }

    void setKeylock(bool enabled) {
        keylockEnabled = enabled;
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        bool bypass = !keylockEnabled && (std::abs(tempoRatio - 1.0) < 0.001);
        
        if (bypass) {
            if (source) source->getNextAudioBlock(info);
            else info.clearActiveBufferRegion();
            
            if (!wasBypassed && info.numSamples > 0) {
                info.buffer->applyGainRamp(info.startSample, std::min(info.numSamples, 512), 0.0f, 1.0f);
            }
            
            wasBypassed = true;
            justUnbypassed = false;
            return;
        }

        if (wasBypassed) {
            if (stretcher) stretcher->reset();
            if (fifo) fifo->reset();
            wasBypassed = false;
            justUnbypassed = true;
        } else {
            justUnbypassed = false;
        }

        if (!source || !stretcher || !fifo) {
            info.clearActiveBufferRegion();
            return;
        }

        int framesNeeded = info.numSamples;
        int destStart = info.startSample;
        int maxPullLoops = 20;

        while (fifo->getNumReady() < framesNeeded && maxPullLoops > 0) {
            int pullSize = stretcher->getSamplesRequired();
            if (pullSize == 0) pullSize = 1024;
            
            if (scratchBuffer.getNumSamples() < pullSize) {
                scratchBuffer.setSize(2, pullSize, true);
            }
            
            juce::AudioSourceChannelInfo pullInfo;
            pullInfo.buffer = &scratchBuffer;
            pullInfo.startSample = 0;
            pullInfo.numSamples = pullSize;
            scratchBuffer.clear(0, pullSize);
            
            source->getNextAudioBlock(pullInfo);
            
            const float* inputs[2] = { scratchBuffer.getReadPointer(0), scratchBuffer.getReadPointer(1) };
            if (scratchBuffer.getNumChannels() == 1) inputs[1] = inputs[0];

            stretcher->process(inputs, pullSize, false);
            
            int avail = stretcher->available();
            if (avail > 0) {
                if (outputBuffer.getNumSamples() < avail) {
                    outputBuffer.setSize(2, std::max((int)outputBuffer.getNumSamples(), avail * 2), true);
                }
                
                int start1, size1, start2, size2;
                fifo->prepareToWrite(avail, start1, size1, start2, size2);
                
                if (size1 > 0) {
                    float* outputs1[2] = { outputBuffer.getWritePointer(0, start1), outputBuffer.getWritePointer(1, start1) };
                    stretcher->retrieve(outputs1, size1);
                }
                if (size2 > 0) {
                    float* outputs2[2] = { outputBuffer.getWritePointer(0, start2), outputBuffer.getWritePointer(1, start2) };
                    stretcher->retrieve(outputs2, size2);
                }
                fifo->finishedWrite(size1 + size2);
            }
            maxPullLoops--;
        }

        int ready = fifo->getNumReady();
        if (ready >= framesNeeded) {
            int start1, size1, start2, size2;
            fifo->prepareToRead(framesNeeded, start1, size1, start2, size2);
            if (size1 > 0) {
                info.buffer->copyFrom(0, destStart, outputBuffer, 0, start1, size1);
                info.buffer->copyFrom(1, destStart, outputBuffer, 1, start1, size1);
            }
            if (size2 > 0) {
                info.buffer->copyFrom(0, destStart + size1, outputBuffer, 0, start2, size2);
                info.buffer->copyFrom(1, destStart + size1, outputBuffer, 1, start2, size2);
            }
            fifo->finishedRead(size1 + size2);
            
            if (justUnbypassed && framesNeeded > 0) {
                info.buffer->applyGainRamp(info.startSample, std::min(framesNeeded, 512), 0.0f, 1.0f);
            }
        } else {
            info.clearActiveBufferRegion();
        }
    }

    // Return total latency introduced by this component in samples
    int getLatencySamples() const {
        int delay = 0;
        if (stretcher) delay += stretcher->getStartDelay();
        if (fifo) delay += fifo->getNumReady();
        return delay;
    }

private:
    juce::AudioSource* source = nullptr;
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    juce::AudioBuffer<float> scratchBuffer;
    juce::AudioBuffer<float> outputBuffer;
    std::unique_ptr<juce::AbstractFifo> fifo;
    double sampleRate = 44100.0;
    double tempoRatio = 1.0;
    bool wasBypassed = true;
    bool keylockEnabled = false;
    bool justUnbypassed = false;
};

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
        m_limiter.prepare(sampleRate, samplesPerBlockExpected, 2);
        
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

        // ── Master Volume + Anti-Clip Limiter ───────────────────────────
        {
            auto* buf = bufferToFill.buffer;
            const int s = bufferToFill.startSample;
            const int n = bufferToFill.numSamples;

            // Apply master volume
            float masterGain = s_masterVolume.load(std::memory_order_relaxed);
            if (std::abs(masterGain - 1.0f) > 0.001f) {
                for (int ch = 0; ch < buf->getNumChannels(); ++ch)
                    buf->applyGain(ch, s, n, masterGain);
            }

            // Brickwall Limiter: always processes (consistent latency),
            // seamless crossfade handled internally via setEnabled()
            {
                m_limiter.setEnabled(s_antiClipEnabled.load(std::memory_order_relaxed));

                float* channelPtrs[16];
                const int numCh = std::min(buf->getNumChannels(), 16);
                for (int ch = 0; ch < numCh; ++ch)
                    channelPtrs[ch] = buf->getWritePointer(ch);

                float gr = m_limiter.processBlock(channelPtrs, numCh, s, n);
                s_gainReduction.store(m_limiter.isEnabled() ? gr : 1.0f,
                                      std::memory_order_relaxed);
            }
        }

        // ── VU peak level measurement (post-processing) ───────────────────
        {
            auto* buf = bufferToFill.buffer;
            const int s = bufferToFill.startSample;
            const int n = bufferToFill.numSamples;
            float peakL = 0.0f, peakR = 0.0f;
            if (buf->getNumChannels() > 0)
                peakL = buf->getMagnitude(0, s, n);
            if (buf->getNumChannels() > 1)
                peakR = buf->getMagnitude(1, s, n);
            m_peakL.store(peakL, std::memory_order_relaxed);
            m_peakR.store(peakR, std::memory_order_relaxed);
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
    BrickwallLimiter m_limiter;

public:
    // VU meter peak levels — written on audio thread, read from UI thread
    std::atomic<float> m_peakL { 0.0f };
    std::atomic<float> m_peakR { 0.0f };

    // Global shared state: master volume + anti-clip (shared across all decks)
    static std::atomic<float> s_masterVolume;
    static std::atomic<bool>  s_antiClipEnabled;
    static std::atomic<float> s_gainReduction;
};

// Static shared state: master volume + anti-clip
std::atomic<float> DjEngine::MixerDspSource::s_masterVolume{1.0f};
std::atomic<bool>  DjEngine::MixerDspSource::s_antiClipEnabled{false};
std::atomic<float> DjEngine::MixerDspSource::s_gainReduction{1.0f};

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
    
    timeStretchSource = std::make_unique<TimeStretchAudioSource>(resamplingSource.get());
    
    // Create the mixer DSP source to apply EQ, Filter, and Gain based on Pioneer DJM A9.
    mixerSource = std::make_unique<MixerDspSource>(timeStretchSource.get(), &transportSource);
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
    // When stopped/paused: return the frozen position (set by togglePlay).
    if (!m_snapValid || !transportSource.isPlaying())
        return getPosition();

    // Forward-interpolate from the last snapshot using elapsed wall-clock time.
    // This keeps the waveform smooth between onTimer() ticks (every 16 ms).
    // Multiply by tempo ratio because the audio playhead moves faster/slower
    // than wall clock (transport reads at tempoRatio × realtime).
    double currentTempoRatio = getTempoRatio();
    double elapsed = (static_cast<double>(m_snapClock.nsecsElapsed()) * 1e-9) * currentTempoRatio;

    // When reverse is on, interpolate backwards instead of forwards
    double interpolated = m_isReverse
        ? m_snapPosition - elapsed
        : m_snapPosition + elapsed;

    // No latency compensation: the 46ms hardware buffer latency is imperceptible
    // visually, and subtracting it caused a visible jump on play/pause transitions
    // (playing = compensated, paused = uncompensated → discontinuity).

    double len = transportSource.getLengthInSeconds();
    interpolated = std::clamp(interpolated, 0.0, len > 0.0 ? len : interpolated);

    return static_cast<float>(interpolated);
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

void DjEngine::setLibraryDatabase(LibraryDatabase* db)
{
    m_libraryDb = db;
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

        // ── Add track to library database ────────────────────────────────
        if (m_libraryDb) {
            int durSec = static_cast<int>(durationSec);
            m_currentTrackId = TrackIdGenerator::generate(
                m_trackArtist, m_trackTitle, durSec, rawPath);
            m_libraryDb->addTrack(m_currentTrackId,
                                 m_trackTitle, m_trackArtist, durSec, rawPath);
        }

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

        // ── Connect analysis results to library DB ───────────────────────
        if (m_libraryDb && !m_currentTrackId.isEmpty()) {
            auto trackId = m_currentTrackId;
            auto* db = m_libraryDb;

            // Disconnect any previous connections (from a prior loadTrack call).
            disconnect(m_trackData, &TrackData::bpmAnalyzed, this, nullptr);
            disconnect(m_trackData, &TrackData::keyAnalyzed, this, nullptr);

            connect(m_trackData, &TrackData::bpmAnalyzed, this, [db, trackId, this]() {
                double bpm = m_trackData->getBpm();
                QString key = m_trackData->getDetectedKey();
                if (bpm > 0.0)
                    db->updateAnalysisData(trackId, static_cast<float>(bpm), key);
            });
            connect(m_trackData, &TrackData::keyAnalyzed, this, [db, trackId, this]() {
                double bpm = m_trackData->getBpm();
                QString key = m_trackData->getDetectedKey();
                if (!key.isEmpty())
                    db->updateAnalysisData(trackId, static_cast<float>(bpm), key);
            });
        }

        emit trackLoaded();
        emit progressChanged();
    }
}

void DjEngine::togglePlay()
{
    if (transportSource.isPlaying()) {
        // Compute the interpolated visual position BEFORE stopping so we can
        // freeze the transport at exactly the position the waveform was showing.
        // This eliminates the visible jump when pressing pause.
        double frozenPos;
        if (m_snapValid) {
            double tempoR = getTempoRatio();
            double elapsed = (static_cast<double>(m_snapClock.nsecsElapsed()) * 1e-9) * tempoR;
            frozenPos = m_isReverse
                ? m_snapPosition - elapsed
                : m_snapPosition + elapsed;
            double len = transportSource.getLengthInSeconds();
            frozenPos = std::clamp(frozenPos, 0.0, len > 0.0 ? len : frozenPos);
        } else {
            frozenPos = transportSource.getCurrentPosition();
        }

        transportSource.stop();
        transportSource.setPosition(frozenPos);
        m_snapValid = false;
        m_atomicPlayheadPos.store(frozenPos, std::memory_order_relaxed);
    } else {
        // Snapshot current position immediately so the interpolation starts
        // from exactly where the waveform is showing (no 16ms wait for onTimer).
        m_snapPosition = transportSource.getCurrentPosition();
        m_snapClock.restart();
        m_snapValid = true;
        m_atomicPlayheadPos.store(m_snapPosition, std::memory_order_relaxed);

        transportSource.start();
    }
        
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
    // Always emit VU level updates (30 Hz timer = nice for meters)
    emit vuLevelChanged();
    emit gainReductionChanged();
}

float DjEngine::vuLevelL() const
{
    return mixerSource ? mixerSource->m_peakL.load(std::memory_order_relaxed) : 0.0f;
}

float DjEngine::vuLevelR() const
{
    return mixerSource ? mixerSource->m_peakR.load(std::memory_order_relaxed) : 0.0f;
}

float DjEngine::gainReduction() const
{
    // s_gainReduction is a static atomic shared across all instances
    return MixerDspSource::s_gainReduction.load(std::memory_order_relaxed);
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

void DjEngine::setMasterVolume(float v) {
    MixerDspSource::s_masterVolume.store(std::clamp(v, 0.0f, 1.5f),
                                         std::memory_order_relaxed);
}

void DjEngine::setAntiClip(bool enabled) {
    MixerDspSource::s_antiClipEnabled.store(enabled, std::memory_order_relaxed);
}

// ──────────────────────────────────────────────────────────────────────────────

void DjEngine::updateSpeedAndPitch()
{
    double speedMultiplier = 1.0 + (m_tempoPercent / 100.0);
    
    if (m_keylock) {
        if (resamplingSource) resamplingSource->setResamplingRatio(1.0);
        if (timeStretchSource) {
            timeStretchSource->setTempoRatio(speedMultiplier);
            timeStretchSource->setKeylock(true);
        }
    } else {
        if (resamplingSource) resamplingSource->setResamplingRatio(speedMultiplier);
        if (timeStretchSource) {
            timeStretchSource->setTempoRatio(1.0); // bypass
            timeStretchSource->setKeylock(false);
        }
    }
}

void DjEngine::setKeylock(bool on)
{
    if (m_keylock == on) return;
    m_keylock = on;
    updateSpeedAndPitch();
    emit keylockChanged();
}

void DjEngine::setTempoPercent(double percent)
{
    // Clamp to ±100% range (WIDE mode)
    percent = std::clamp(percent, -100.0, 100.0);
    
    if (m_tempoPercent == percent) return;
    
    m_tempoPercent = percent;
    updateSpeedAndPitch();
    
    qDebug() << "[DjEngine] Tempo set to" << percent << "%" << "(keylock:" << m_keylock << ")";
    
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
