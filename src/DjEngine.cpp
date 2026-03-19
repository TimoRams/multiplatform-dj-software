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

QString defaultHotCueColor(int index)
{
    static const std::array<const char*, 16> colors = {
        "#e04040", "#e08030", "#e0d030", "#30b050",
        "#30a0d0", "#6060e0", "#c040c0", "#e06080",
        "#ff4d4d", "#ff9f43", "#f6e05e", "#48bb78",
        "#38b2ac", "#4299e1", "#9f7aea", "#ed64a6"
    };
    if (index < 0 || index >= static_cast<int>(colors.size()))
        return QStringLiteral("#e04040");
    return QString::fromLatin1(colors[static_cast<size_t>(index)]);
}

}

class ReverseStreamAudioSource : public juce::PositionableAudioSource {
public:
    ReverseStreamAudioSource(juce::PositionableAudioSource* source) : m_source(source) {}

    void setNextReadPosition(juce::int64 newPosition) override {
        m_source->setNextReadPosition(newPosition);
        m_logicalPos = newPosition;
    }

    juce::int64 getNextReadPosition() const override {
        return m_logicalPos;
    }

    juce::int64 getTotalLength() const override {
        return m_source->getTotalLength();
    }

    bool isLooping() const override { return m_source->isLooping(); }
    void setLooping(bool shouldLoop) override { m_source->setLooping(shouldLoop); }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
        m_source->prepareToPlay(samplesPerBlockExpected, sampleRate);
    }

    void releaseResources() override {
        m_source->releaseResources();
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override {
        if (!m_reverse.load(std::memory_order_relaxed)) {
            // Forward playback
            m_source->setNextReadPosition(m_logicalPos);
            m_source->getNextAudioBlock(bufferToFill);
            m_logicalPos = m_source->getNextReadPosition();
            return;
        }

        // Reverse playback
        const int numSamples = bufferToFill.numSamples;
        juce::int64 currentPos = m_logicalPos;

        // Start reading from currentPos - numSamples, because reading goes forwards
        juce::int64 readStart = currentPos - numSamples;
        int samplesToRead = numSamples;
        int zerosToPad = 0;

        if (readStart < 0) {
            samplesToRead = static_cast<int>(currentPos);
            zerosToPad = numSamples - samplesToRead;
            readStart = 0;
        }

        if (samplesToRead > 0) {
            m_source->setNextReadPosition(readStart);
            
            juce::AudioSourceChannelInfo readInfo(bufferToFill);
            readInfo.startSample = bufferToFill.startSample;
            readInfo.numSamples = samplesToRead;
            m_source->getNextAudioBlock(readInfo);

            for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                float* ptr = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                std::reverse(ptr, ptr + samplesToRead);
            }
        }

        if (zerosToPad > 0) {
            for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch) {
                juce::FloatVectorOperations::clear(
                    bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample + samplesToRead),
                    zerosToPad);
            }
        }

        m_logicalPos = currentPos - samplesToRead;
        if (m_logicalPos < 0) m_logicalPos = 0;
    }

    void setReverse(bool rev) {
        m_reverse.store(rev, std::memory_order_relaxed);
    }

private:
    juce::PositionableAudioSource* m_source;
    std::atomic<bool> m_reverse{false};
    juce::int64 m_logicalPos{0};
};

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
        tailBuffer.setSize(2, samplesPerBlockExpected + kCrossfadeLen);
        tailSamples = 0;
        crossfadeRemaining = 0;
        crossfadeTailPos = 0;
        prevBypass = true;
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
        const int numCh = std::min(info.buffer->getNumChannels(), 2);

        // ── Detect state transition ───────────────────────────────────────
        bool transitioned = (bypass != prevBypass);
        prevBypass = bypass;

        if (transitioned && tailSamples > 0) {
            // Start crossfade from saved tail into new signal
            crossfadeRemaining = std::min(tailSamples, kCrossfadeLen);
            crossfadeTailPos = 0;
        }

        // ── Produce output via current path ───────────────────────────────
        if (bypass) {
            if (source) source->getNextAudioBlock(info);
            else info.clearActiveBufferRegion();

            if (transitioned) {
                // Just switched to bypass — stretcher was active until now
                wasBypassed = true;
                justUnbypassed = false;
            }

        } else {
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
            } else {
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
                } else {
                    // FIFO underrun (stretcher warming up) — silence is ok,
                    // the crossfade from the tail buffer masks it.
                    info.clearActiveBufferRegion();
                }
            }
        }

        // ── Apply crossfade from saved tail into new output ───────────────
        if (crossfadeRemaining > 0) {
            int fadeLen = std::min(crossfadeRemaining, info.numSamples);
            int fadeTotal = std::min(tailSamples, kCrossfadeLen);

            for (int i = 0; i < fadeLen; ++i) {
                float t = static_cast<float>(crossfadeTailPos + i + 1) / static_cast<float>(fadeTotal);
                t = std::clamp(t, 0.0f, 1.0f);

                for (int ch = 0; ch < numCh; ++ch) {
                    float newSamp = info.buffer->getSample(ch, info.startSample + i);
                    float oldSamp = (crossfadeTailPos + i < tailSamples)
                        ? tailBuffer.getSample(ch, crossfadeTailPos + i)
                        : 0.0f;
                    // Equal-power-ish crossfade: old fades out, new fades in
                    info.buffer->setSample(ch, info.startSample + i,
                        oldSamp * (1.0f - t) + newSamp * t);
                }
            }
            crossfadeTailPos += fadeLen;
            crossfadeRemaining -= fadeLen;
        }

        // ── Save current output as tail for potential next transition ─────
        int samplesToSave = std::min(info.numSamples, tailBuffer.getNumSamples());
        for (int ch = 0; ch < numCh; ++ch) {
            tailBuffer.copyFrom(ch, 0, *info.buffer, ch, info.startSample, samplesToSave);
        }
        tailSamples = samplesToSave;
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

    // Seamless crossfade state
    juce::AudioBuffer<float> tailBuffer;       // last block's output
    int tailSamples = 0;                       // valid samples in tailBuffer
    int crossfadeRemaining = 0;                // samples left in crossfade
    int crossfadeTailPos = 0;                  // read position in tail
    bool prevBypass = true;                    // previous bypass state
    static constexpr int kCrossfadeLen = 1024; // ~23 ms @ 44.1 kHz
};

class DjEngine::MixerDspSource : public juce::AudioSource {
public:
    MixerDspSource(juce::AudioSource* inSource, juce::AudioTransportSource* transport)
        : source(inSource), m_transport(transport) {}

    // ── FxProcessor slot (called from Qt main thread) ──────────────────────
    void setFxEffectType(EffectType type) { m_fx.setEffectType(type); }
    void setFxAmount(float amount)        { m_fx.setAmount(amount); }
    void setFxSCKnob(float knob)          { m_fx.setSCKnobValue(knob); }

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
            // Strict digital clip indicator: only flag when we overshoot full scale
            // with a small margin to avoid false positives from limiter ceiling.
            constexpr float kClipThreshold = 1.001f;
            m_clipDetected.store((peakL > kClipThreshold) || (peakR > kClipThreshold),
                                std::memory_order_relaxed);
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
    BrickwallLimiter m_limiter;

public:
    // VU meter peak levels — written on audio thread, read from UI thread
    std::atomic<float> m_peakL { 0.0f };
    std::atomic<float> m_peakR { 0.0f };
    std::atomic<bool>  m_clipDetected { false };

    // Global shared state: master volume + anti-clip (shared across all decks)
    static std::atomic<float> s_masterVolume;
    static std::atomic<bool>  s_antiClipEnabled;
    static std::atomic<float> s_gainReduction;
};

// Static shared state: master volume + anti-clip
std::atomic<float> DjEngine::MixerDspSource::s_masterVolume{1.0f};
std::atomic<bool>  DjEngine::MixerDspSource::s_antiClipEnabled{false};
std::atomic<float> DjEngine::MixerDspSource::s_gainReduction{1.0f};

std::mutex DjEngine::s_syncMutex;
std::vector<DjEngine*> DjEngine::s_syncDecks;
DjEngine* DjEngine::s_syncMasterDeck = nullptr;

void DjEngine::updateSyncMasterLocked()
{
    DjEngine* newMaster = nullptr;
    for (auto* d : s_syncDecks) {
        if (d && d->m_syncEnabled) {
            newMaster = d;
            break;
        }
    }

    s_syncMasterDeck = newMaster;
    for (auto* d : s_syncDecks) {
        if (!d)
            continue;
        const bool wasMaster = d->m_isSyncMaster;
        d->m_isSyncMaster = (d == s_syncMasterDeck) && d->m_syncEnabled;
        if (wasMaster != d->m_isSyncMaster)
            emit d->syncMasterChanged();
    }
}

void DjEngine::propagateMasterTempoLocked(DjEngine* master)
{
    if (!master || !master->m_trackData)
        return;

    double masterBpm = 0.0;
    std::vector<DjEngine*> followers;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        masterBpm = master->getCurrentBpm();
        if (masterBpm <= 0.0)
            return;
        followers.reserve(s_syncDecks.size());
        for (auto* d : s_syncDecks) {
            if (!d || d == master || !d->m_syncEnabled || d->m_isSyncMaster)
                continue;
            followers.push_back(d);
        }
    }

    for (auto* d : followers) {
        if (!d->m_trackData)
            continue;
        const double baseBpm = d->m_trackData->getBpm();
        if (baseBpm <= 0.0)
            continue;
        const double pct = ((masterBpm / baseBpm) - 1.0) * 100.0;
        d->setTempoPercent(pct);
    }
}

DjEngine::DjEngine(QObject* parent) : QObject(parent)
{
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        s_syncDecks.push_back(this);
    }

    m_trackData = new TrackData(this);
    m_analyzer  = new WaveformAnalyzer(m_trackData, &formatManager, 150);
    clearHotCueState();

    // When the analyzer detects a key, override the (often absent) ID3 key field.
    connect(m_trackData, &TrackData::keyAnalyzed, this, [this]() {
        QString analysedKey = m_trackData->getDetectedKey();
        if (!analysedKey.isEmpty()) {
            m_trackKey = analysedKey;
            emit trackMetadataChanged();
        }

        persistCurrentAnalysisToLibrary();
    });

    // When BPM analysis finishes, re-emit tempoChanged so that currentBpm
    // and tempoRatio Q_PROPERTYs update in QML.
    connect(m_trackData, &TrackData::bpmAnalyzed, this, [this]() {
        emit tempoChanged();
        persistCurrentAnalysisToLibrary();
        bool propagateFromSelf = false;
        DjEngine* masterToFollow = nullptr;
        if (m_syncEnabled) {
            {
                std::lock_guard<std::mutex> g(s_syncMutex);
                updateSyncMasterLocked();
                propagateFromSelf = m_isSyncMaster;
                masterToFollow = s_syncMasterDeck;
            }
            if (propagateFromSelf)
                propagateMasterTempoLocked(this);
            else if (masterToFollow)
                propagateMasterTempoLocked(masterToFollow);
        }
    });

    connect(m_trackData, &TrackData::beatgridChanged, this, [this]() {
        persistCurrentAnalysisToLibrary();
    });

    connect(m_trackData, &TrackData::segmentsAnalyzed, this, [this]() {
        const auto segments = m_trackData->getSegments();

        QVariantList asVariant;
        asVariant.reserve(static_cast<int>(segments.size()));
        for (const auto& s : segments) {
            QVariantMap m;
            m.insert("label", s.label);
            m.insert("startTime", s.startTime);
            m.insert("endTime", s.endTime);
            m.insert("colorHex", s.colorHex);
            asVariant.push_back(m);
        }

        m_currentSegments = asVariant;
        emit segmentsChanged();

        if (m_libraryDb && !m_currentTrackId.isEmpty() && !segments.empty())
            m_libraryDb->updateTrackSegments(m_currentTrackId, segments);
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
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        s_syncDecks.erase(std::remove(s_syncDecks.begin(), s_syncDecks.end(), this), s_syncDecks.end());
        if (s_syncMasterDeck == this)
            s_syncMasterDeck = nullptr;
        updateSyncMasterLocked();
    }

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
    if (m_isScrubbing)
        return static_cast<float>(m_scrubHoldPosition);
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

double DjEngine::getVisualPositionQml() const
{
    return static_cast<double>(getVisualPosition());
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
    loadHotCuesForCurrentTrack();
}

void DjEngine::persistCurrentAnalysisToLibrary()
{
    if (!m_libraryDb || m_currentTrackId.isEmpty() || !m_trackData)
        return;

    const double bpm = m_trackData->getBpm();
    const QString key = m_trackData->getDetectedKey().trimmed();
    const auto beatGrid = m_trackData->getBeatGrid();

    if (bpm <= 0.0 && key.isEmpty() && beatGrid.empty())
        return;

    m_libraryDb->updateAnalysisData(
        m_currentTrackId,
        static_cast<float>(bpm),
        key,
        m_trackData->getFirstBeatSample(),
        m_trackData->getSampleRate(),
        beatGrid);
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
        m_trackData->clear();
        m_currentSegments.clear();
        clearHotCueState();
        emit segmentsChanged();
        emit hotCuesChanged();

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
        m_trackDurationSec = durationSec;
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
        clearLoop();

        // ── Add track to library database ────────────────────────────────
        if (m_libraryDb) {
            int durSec = static_cast<int>(durationSec);
            int bitrateKbps = 0;
            if (durationSec > 0.0) {
                const auto bytes = static_cast<double>(file.getSize());
                bitrateKbps = static_cast<int>(std::lround((bytes * 8.0) / durationSec / 1000.0));
            }
            m_currentTrackId = TrackIdGenerator::generate(
                m_trackArtist, m_trackTitle, durSec, rawPath);
            m_libraryDb->addTrack(m_currentTrackId,
                                 m_trackTitle, m_trackArtist, durSec, rawPath, bitrateKbps);

            LibraryDatabase::AnalysisSnapshot cachedAnalysis;
            if (m_libraryDb->tryGetAnalysisData(m_currentTrackId, &cachedAnalysis)
                && cachedAnalysis.isAnalyzed) {
                m_currentSegments = m_libraryDb->trackSegmentsForTrack(m_currentTrackId);
                emit segmentsChanged();

                if (cachedAnalysis.bpm > 0.0) {
                    m_trackData->setBpmData(cachedAnalysis.bpm,
                                            cachedAnalysis.firstBeatSample,
                                            cachedAnalysis.sampleRate,
                                            cachedAnalysis.beatGrid);
                }

                const QString cachedKey = cachedAnalysis.key.trimmed();
                if (!cachedKey.isEmpty()) {
                    m_trackKey = cachedKey;
                    m_trackData->setKeyData(cachedKey);
                }
            } else {
                // Strictly hide segment UI state until fresh analysis writes data.
                m_currentSegments = QVariantList();
                emit segmentsChanged();
            }

            loadHotCuesForCurrentTrack();
        }

        qDebug() << "[DjEngine] title=" << m_trackTitle
                 << " artist=" << m_trackArtist
                 << " key=" << m_trackKey;

        emit trackMetadataChanged();

        transportSource.stop();
        transportSource.setSource(nullptr);

        readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        reverseWrapSource = std::make_unique<ReverseStreamAudioSource>(readerSource.get());
        reverseWrapSource->setReverse(m_isReverse);
        transportSource.setSource(reverseWrapSource.get(), 0, nullptr, reader->sampleRate);
        transportSource.setPosition(0.0);

        m_analyzer->startAnalysis(rawPath);

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
    if (m_isScrubbing) {
        // Click-and-hold should brake quietly; audible scratch is only while
        // there is active motion input.
        const qint64 idleMs = m_lastScrubInputClock.isValid()
            ? m_lastScrubInputClock.elapsed()
            : 1000;

        if (idleMs > 25) {
            if (transportSource.isPlaying())
                transportSource.stop();
            transportSource.setPosition(m_scrubHoldPosition);
            m_atomicPlayheadPos.store(m_scrubHoldPosition,
                                      std::memory_order_relaxed);
        } else {
            if (!transportSource.isPlaying())
                transportSource.start();
            // Keep UI perfectly pinned to the commanded scrub position.
            // The transport may move slightly between callbacks when running,
            // which caused visible back/forth jitter in the scrolling waveform.
            transportSource.setPosition(m_scrubHoldPosition);
            m_atomicPlayheadPos.store(m_scrubHoldPosition,
                                      std::memory_order_relaxed);
        }

        emit progressChanged();
        emit vuLevelChanged();
        emit gainReductionChanged();
        return;
    }

    if (transportSource.isPlaying()) {
        // Store a position snapshot with a matching wall-clock timestamp.
        // getVisualPosition() forward-interpolates from here for each render frame.
        m_snapPosition = transportSource.getCurrentPosition();

        if (m_loopActive && m_loopOutSec > m_loopInSec) {
            if (!m_isReverse && m_snapPosition >= m_loopOutSec) {
                transportSource.setPosition(m_loopInSec);
                m_snapPosition = m_loopInSec;
            } else if (m_isReverse && m_snapPosition <= m_loopInSec) {
                transportSource.setPosition(m_loopOutSec);
                m_snapPosition = m_loopOutSec;
            }
        }

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

bool DjEngine::clipDetected() const
{
    return mixerSource ? mixerSource->m_clipDetected.load(std::memory_order_relaxed) : false;
}

float DjEngine::gainReduction() const
{
    // s_gainReduction is a static atomic shared across all instances
    return MixerDspSource::s_gainReduction.load(std::memory_order_relaxed);
}

QVariantList DjEngine::hotCues() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_hotCueSlots.size()));

    for (int i = 0; i < static_cast<int>(m_hotCueSlots.size()); ++i) {
        const auto& slot = m_hotCueSlots[static_cast<size_t>(i)];
        QVariantMap m;
        m.insert("index", i);
        m.insert("set", slot.set);
        m.insert("positionSec", slot.positionSec);
        m.insert("label", slot.label);
        m.insert("color", slot.color);
        out.push_back(m);
    }

    return out;
}

bool DjEngine::isValidHotCueIndex(int index) const
{
    return index >= 0 && index < static_cast<int>(m_hotCueSlots.size());
}

void DjEngine::clearHotCueState()
{
    for (int i = 0; i < static_cast<int>(m_hotCueSlots.size()); ++i) {
        auto& slot = m_hotCueSlots[static_cast<size_t>(i)];
        slot.set = false;
        slot.positionSec = 0.0;
        slot.label.clear();
        slot.color = defaultHotCueColor(i);
    }
}

void DjEngine::loadHotCuesForCurrentTrack()
{
    clearHotCueState();

    if (!m_libraryDb || m_currentTrackId.isEmpty()) {
        emit hotCuesChanged();
        return;
    }

    const QVariantList stored = m_libraryDb->cuePointsForTrack(m_currentTrackId);
    for (const QVariant& v : stored) {
        const QVariantMap m = v.toMap();
        const int index = m.value("index").toInt();
        if (!isValidHotCueIndex(index))
            continue;

        auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
        slot.set = true;
        slot.positionSec = std::max(0.0, m.value("positionSec").toDouble());
        slot.label = m.value("label").toString();
        const QString color = m.value("color").toString().trimmed();
        slot.color = color.isEmpty() ? defaultHotCueColor(index) : color;
    }

    emit hotCuesChanged();
}

void DjEngine::persistHotCueSlot(int index)
{
    if (!isValidHotCueIndex(index) || !m_libraryDb || m_currentTrackId.isEmpty())
        return;

    const auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    if (slot.set) {
        const QString label = slot.label.isEmpty()
            ? QStringLiteral("HOT CUE %1").arg(index + 1)
            : slot.label;
        m_libraryDb->upsertCuePoint(m_currentTrackId, index, slot.positionSec, label, slot.color);
    } else {
        m_libraryDb->deleteCuePoint(m_currentTrackId, index);
    }
}

void DjEngine::storeHotCue(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    slot.set = true;
    slot.positionSec = std::clamp(static_cast<double>(getVisualPosition()), 0.0, trackLen);
    if (slot.color.isEmpty())
        slot.color = defaultHotCueColor(index);
    if (slot.label.isEmpty())
        slot.label = QStringLiteral("HOT CUE %1").arg(index + 1);

    persistHotCueSlot(index);
    emit hotCuesChanged();
}

void DjEngine::triggerHotCue(int index)
{
    if (!isValidHotCueIndex(index) || !m_hasTrack)
        return;

    const auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    if (!slot.set) {
        storeHotCue(index);
        return;
    }

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    const double pos = std::clamp(slot.positionSec, 0.0, trackLen);
    transportSource.setPosition(pos);
    m_snapPosition = pos;
    m_snapClock.restart();
    m_snapValid = true;
    m_atomicPlayheadPos.store(pos, std::memory_order_relaxed);
    emit progressChanged();
}

void DjEngine::clearHotCue(int index)
{
    if (!isValidHotCueIndex(index))
        return;

    auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    slot.set = false;
    slot.positionSec = 0.0;
    slot.label.clear();
    if (slot.color.isEmpty())
        slot.color = defaultHotCueColor(index);

    persistHotCueSlot(index);
    emit hotCuesChanged();
}

void DjEngine::setHotCueColor(int index, const QString& colorHex)
{
    if (!isValidHotCueIndex(index))
        return;

    QString color = colorHex.trimmed();
    if (color.isEmpty())
        color = defaultHotCueColor(index);

    auto& slot = m_hotCueSlots[static_cast<size_t>(index)];
    slot.color = color;

    if (slot.set)
        persistHotCueSlot(index);

    emit hotCuesChanged();
}

// ─── Scrub API ────────────────────────────────────────────────────────────────

void DjEngine::pauseForScrub()
{
    if (m_isScrubbing)
        return;

    m_scrubWasPlaying = transportSource.isPlaying();
    m_isScrubbing = true;
    m_snapValid = false;

    m_scrubHoldPosition = transportSource.getCurrentPosition();
    m_lastScrubInputClock.restart();

    // Immediate touch brake: no scratch noise on plain click/hold.
    transportSource.stop();
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
    scratchBySeconds(timeDelta);
}

void DjEngine::scratchBySeconds(double deltaSeconds)
{
    if (deltaSeconds == 0.0)
        return;

    // Precision-first response for mouse waveform scrubbing.
    // A tiny clamp guards against accidental event spikes without causing jumps.
    deltaSeconds = std::clamp(deltaSeconds, -0.05, 0.05);

    double len = transportSource.getLengthInSeconds();
    if (len <= 0.0)
        return;

    const double basePos = m_scrubHoldPosition;
    double newPos = std::clamp(basePos + deltaSeconds, 0.0, len);

    if (m_isScrubbing && !transportSource.isPlaying())
        transportSource.start();

    transportSource.setPosition(newPos);
    m_scrubHoldPosition = newPos;
    m_lastScrubInputClock.restart();

    // Keep the atomic + snap in sync for all synced UIs.
    m_atomicPlayheadPos.store(newPos, std::memory_order_relaxed);
    m_snapPosition = newPos;
    emit progressChanged();
}

void DjEngine::resumeAfterScrub()
{
    if (!m_isScrubbing)
        return;

    m_isScrubbing = false;
    if (m_scrubWasPlaying)
        transportSource.start();
    else
        transportSource.stop();

    m_scrubWasPlaying = false;
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

    // Persist immediately so manual beatgrid edits survive app restarts.
    persistCurrentAnalysisToLibrary();
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
    persistCurrentAnalysisToLibrary();
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
    persistCurrentAnalysisToLibrary();
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
    bool useKeylockPath = m_keylock; // Reverse is now continuous, Keylock works with it!
    
    if (useKeylockPath) {
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

    if (m_syncEnabled) {
        bool amMaster = false;
        {
            std::lock_guard<std::mutex> g(s_syncMutex);
            updateSyncMasterLocked();
            amMaster = m_isSyncMaster;
        }
        if (amMaster)
            propagateMasterTempoLocked(this);
    }
}

void DjEngine::setManualBpm(double bpm)
{
    if (!m_trackData)
        return;

    const double clamped = std::clamp(bpm, 20.0, 300.0);
    if (clamped <= 0.0)
        return;

    const double trackLen = static_cast<double>(transportSource.getLengthInSeconds());
    const double currentSec = static_cast<double>(getVisualPosition());
    const double anchor = nearestDownbeatAnchor(m_trackData->getBeatGrid(), currentSec);

    m_trackData->setBpm(clamped);
    if (trackLen > 0.0)
        m_trackData->shiftBeatgridToDownbeat(anchor, trackLen);

    persistCurrentAnalysisToLibrary();
    emit tempoChanged();

    if (m_syncEnabled) {
        bool amMaster = false;
        {
            std::lock_guard<std::mutex> g(s_syncMutex);
            updateSyncMasterLocked();
            amMaster = m_isSyncMaster;
        }
        if (amMaster)
            propagateMasterTempoLocked(this);
    }
}

void DjEngine::setSyncEnabled(bool enabled)
{
    if (m_syncEnabled == enabled)
        return;

    m_syncEnabled = enabled;
    emit syncChanged();

    bool amMaster = false;
    DjEngine* masterDeck = nullptr;
    {
        std::lock_guard<std::mutex> g(s_syncMutex);
        updateSyncMasterLocked();
        amMaster = m_isSyncMaster;
        masterDeck = s_syncMasterDeck;
    }

    if (m_syncEnabled) {
        if (amMaster) {
            propagateMasterTempoLocked(this);
        } else if (masterDeck) {
            const double masterBpm = masterDeck->getCurrentBpm();
            if (m_trackData) {
                const double baseBpm = m_trackData->getBpm();
                if (masterBpm > 0.0 && baseBpm > 0.0) {
                    const double pct = ((masterBpm / baseBpm) - 1.0) * 100.0;
                    setTempoPercent(pct);
                }
            }
            return;
        }
    }
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

void DjEngine::setQuantizeEnabled(bool enabled)
{
    if (m_quantizeEnabled == enabled)
        return;
    m_quantizeEnabled = enabled;
    emit quantizeEnabledChanged();
}

double DjEngine::quantizedBeatAt(double sec) const
{
    if (!m_trackData)
        return sec;

    const auto grid = m_trackData->getBeatGrid();
    if (!grid.empty()) {
        double best = grid.front().positionSec;
        double bestDist = std::abs(best - sec);
        for (const auto& m : grid) {
            const double d = std::abs(m.positionSec - sec);
            if (d < bestDist) {
                best = m.positionSec;
                bestDist = d;
            }
        }
        return best;
    }

    const double bpm = m_trackData->getBpm();
    const double sr = m_trackData->getSampleRate();
    if (bpm <= 0.0 || sr <= 0.0)
        return sec;

    const double beatDur = 60.0 / bpm;
    const double firstBeat = static_cast<double>(m_trackData->getFirstBeatSample()) / sr;
    const double beatIndex = std::round((sec - firstBeat) / beatDur);
    return firstBeat + beatIndex * beatDur;
}

double DjEngine::beatDurationAround(double sec) const
{
    if (!m_trackData)
        return 0.5;

    const auto grid = m_trackData->getBeatGrid();
    if (grid.size() >= 2) {
        int nearest = 0;
        double bestDist = std::abs(grid[0].positionSec - sec);
        for (int i = 1; i < static_cast<int>(grid.size()); ++i) {
            const double d = std::abs(grid[static_cast<size_t>(i)].positionSec - sec);
            if (d < bestDist) {
                bestDist = d;
                nearest = i;
            }
        }

        if (nearest + 1 < static_cast<int>(grid.size())) {
            const double d = grid[static_cast<size_t>(nearest + 1)].positionSec
                           - grid[static_cast<size_t>(nearest)].positionSec;
            if (d > 1e-3)
                return d;
        }
        if (nearest > 0) {
            const double d = grid[static_cast<size_t>(nearest)].positionSec
                           - grid[static_cast<size_t>(nearest - 1)].positionSec;
            if (d > 1e-3)
                return d;
        }
    }

    const double bpm = m_trackData->getBpm();
    return bpm > 0.0 ? (60.0 / bpm) : 0.5;
}

void DjEngine::startLoopAt(double startSec, double lengthBeats)
{
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    double start = std::clamp(startSec, 0.0, trackLen);
    if (m_quantizeEnabled)
        start = quantizedBeatAt(start);

    const double beatDur = beatDurationAround(start);
    if (beatDur <= 1e-4)
        return;

    constexpr double kMinLoopBeats = 0.125; // 1/8 beat
    constexpr double kMaxLoopBeats = 4096.0;
    double beats = std::clamp(lengthBeats, kMinLoopBeats, kMaxLoopBeats);
    double end = start + beats * beatDur;
    if (end > trackLen)
        end = trackLen;
    if (end <= start + 0.001)
        return;

    m_loopInSec = start;
    m_loopOutSec = end;
    m_loopLengthBeats = (end - start) / beatDur;
    m_loopActive = true;
    m_loopInSet = true;
    emit loopChanged();
}

void DjEngine::setLoopIn()
{
    double pos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        pos = quantizedBeatAt(pos);

    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    m_loopInSec = std::clamp(pos, 0.0, trackLen);
    m_loopInSet = true;

    if (m_loopActive && m_loopOutSec <= m_loopInSec) {
        m_loopOutSec = std::min(trackLen, m_loopInSec + beatDurationAround(m_loopInSec));
    }
    emit loopChanged();
}

void DjEngine::setLoopOut()
{
    const double trackLen = transportSource.getLengthInSeconds();
    if (trackLen <= 0.0)
        return;

    if (!m_loopInSet)
        setLoopIn();

    double pos = static_cast<double>(getVisualPosition());
    if (m_quantizeEnabled)
        pos = quantizedBeatAt(pos);

    double out = std::clamp(pos, 0.0, trackLen);
    const double beatDur = beatDurationAround(m_loopInSec);
    if (out <= m_loopInSec + 0.001)
        out = std::min(trackLen, m_loopInSec + beatDur);
    if (out <= m_loopInSec + 0.001)
        return;

    m_loopOutSec = out;
    m_loopLengthBeats = (m_loopOutSec - m_loopInSec) / std::max(beatDur, 1e-4);
    m_loopActive = true;
    emit loopChanged();
}

void DjEngine::toggleLoop4Beats()
{
    if (m_loopActive && std::abs(m_loopLengthBeats - 4.0) < 0.1) {
        clearLoop();
        return;
    }
    startLoopAt(static_cast<double>(getVisualPosition()), 4.0);
}

void DjEngine::toggleLoopThreeQuarter()
{
    // 3/4 loop = three quarters of ONE beat.
    if (m_loopActive && std::abs(m_loopLengthBeats - 0.75) < 0.06) {
        clearLoop();
        return;
    }
    startLoopAt(static_cast<double>(getVisualPosition()), 0.75);
}

void DjEngine::halveLoopLength()
{
    if (!m_loopActive) {
        startLoopAt(static_cast<double>(getVisualPosition()), 2.0);
        return;
    }
    startLoopAt(m_loopInSec, m_loopLengthBeats / 2.0);
}

void DjEngine::doubleLoopLength()
{
    if (!m_loopActive) {
        startLoopAt(static_cast<double>(getVisualPosition()), 8.0);
        return;
    }
    startLoopAt(m_loopInSec, m_loopLengthBeats * 2.0);
}

void DjEngine::clearLoop()
{
    if (!m_loopActive && !m_loopInSet && m_loopLengthBeats == 0.0)
        return;
    m_loopActive = false;
    m_loopInSet = false;
    m_loopLengthBeats = 0.0;
    m_loopInSec = 0.0;
    m_loopOutSec = 0.0;
    emit loopChanged();
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
    if (reverseWrapSource) {
        static_cast<ReverseStreamAudioSource*>(reverseWrapSource.get())->setReverse(on);
    }
    updateSpeedAndPitch();
    emit reverseChanged();
}
