#include "WaveformAnalyzer.h"
#include <QDebug>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <complex>

WaveformAnalyzer::WaveformAnalyzer(TrackData* trackData, juce::AudioFormatManager* formatManager, int pointsPerSecond)
    : juce::Thread("WaveformAnalyzerThread"), m_trackData(trackData), m_formatManager(formatManager), m_pointsPerSecond(pointsPerSecond)
{
}

WaveformAnalyzer::~WaveformAnalyzer()
{
    stopAnalysis();
}

void WaveformAnalyzer::startAnalysis(const QString& filePath)
{
    stopAnalysis();
    m_filePath = filePath;
    startThread();
}

void WaveformAnalyzer::stopAnalysis()
{
    signalThreadShouldExit();
    stopThread(2000);
}

// Linkwitz-Riley 4th-order crossover (two cascaded Butterworth 2nd-order filters).
// The three output bands sum back to the original signal without phase cancellation.
struct LR4Crossover {
    // Butterworth 2nd order, kaskadiert = LR4
    juce::dsp::IIR::Filter<float> lp1, lp2, hp1, hp2;

    void prepareLowPass(double sr, float freq) {
        auto c = juce::dsp::IIR::Coefficients<float>::makeLowPass(sr, freq, 0.7071f);
        lp1.coefficients = c;
        lp2.coefficients = c;
    }
    void prepareHighPass(double sr, float freq) {
        auto c = juce::dsp::IIR::Coefficients<float>::makeHighPass(sr, freq, 0.7071f);
        hp1.coefficients = c;
        hp2.coefficients = c;
    }
    void reset() { lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); }

    float processLP(float s) { return lp2.processSample(lp1.processSample(s)); }
    float processHP(float s) { return hp2.processSample(hp1.processSample(s)); }
};

// Envelope follower with separate attack and release time constants.
// Attack and release are expressed in milliseconds.
struct EnvelopeFollower {
    float state = 0.0f;
    float attackCoef  = 0.0f;
    float releaseCoef = 0.0f;

    // attackMs / releaseMs in milliseconds
    void prepare(double sampleRate, float attackMs, float releaseMs) {
        attackCoef  = std::exp(-1.0f / (static_cast<float>(sampleRate) * attackMs  * 0.001f));
        releaseCoef = std::exp(-1.0f / (static_cast<float>(sampleRate) * releaseMs * 0.001f));
    }

    float process(float rectified) {
        float coef = rectified > state ? attackCoef : releaseCoef;
        state = rectified + coef * (state - rectified);
        return state;
    }

    void reset() { state = 0.0f; }
};

void WaveformAnalyzer::run()
{
    juce::File file(m_filePath.toStdString());
    if (!file.existsAsFile()) return;

    std::unique_ptr<juce::AudioFormatReader> reader(m_formatManager->createReaderFor(file));
    if (!reader) return;

    m_trackData->clear();

    const juce::int64 totalSamples = reader->lengthInSamples;
    const double      sampleRate   = reader->sampleRate;
    const double      duration     = totalSamples / sampleRate;
    const int         numPoints    = static_cast<int>(duration * m_pointsPerSecond);

    if (numPoints <= 0) return;

    m_trackData->setTotalExpected(numPoints);
    m_trackData->reserve(numPoints);

    // Samples per waveform bin (one x-pixel in the overview).
    const int samplesPerBin = static_cast<int>(totalSamples / numPoints);
    if (samplesPerBin < 1) return;

    // Stage 1: Linkwitz-Riley crossover (LR4, 24 dB/oct).
    // Crossover frequencies: 200 Hz (bass/mid), 4000 Hz (mid/high).
    const float freqLowMid  = 200.0f;
    const float freqMidHigh = 4000.0f;

    LR4Crossover xoverLow;   // LP at 200 Hz -> LOW band
    LR4Crossover xoverMid;   // HP at 200 Hz then LP at 4 kHz -> MID band
    LR4Crossover xoverHigh;  // HP at 4 kHz -> HIGH band

    xoverLow.prepareLowPass (sampleRate, freqLowMid);
    xoverMid.prepareHighPass(sampleRate, freqLowMid);
    xoverMid.prepareLowPass (sampleRate, freqMidHigh);  // lp1/lp2 for 4 kHz
    xoverHigh.prepareHighPass(sampleRate, freqMidHigh);
    xoverLow.reset(); xoverMid.reset(); xoverHigh.reset();

    // Stage 2: per-band envelope followers.
    // Time constants tuned for typical DJ waveform aesthetics:
    //   LOW:  attack 2 ms, release 80 ms  — sharp kick onset, visible bass sustain
    //   MID:  attack 5 ms, release 120 ms — chord stabs and synth bodies
    //   HIGH: attack 1 ms, release 40 ms  — hi-hats stay sharp, decay quickly
    EnvelopeFollower envLow, envMid, envHigh, envFull;
    envLow.prepare (sampleRate,  2.0f,  80.0f);
    envMid.prepare (sampleRate,  5.0f, 120.0f);
    envHigh.prepare(sampleRate,  1.0f,  40.0f);
    envFull.prepare(sampleRate,  3.0f, 100.0f);

    // Stage 3: sample-accurate per-bin analysis.
    // For each waveform bin (one x-pixel in the overview) we compute:
    //   true peak  — absolute maximum of raw samples in the bin
    //   envelope   — smoothed value of the envelope follower at bin end
    //   rms        — root mean square of envelope values (for smooth rendering)
    // The envelope follower runs sample-accurately across the entire track so
    // it maintains its state correctly between bins.
    float globalMaxPeak = 0.001f;

    const int batchSize = 100;
    QVector<TrackData::WaveformBin> batch;
    batch.reserve(batchSize);

    juce::AudioBuffer<float> readBuf(static_cast<int>(reader->numChannels), samplesPerBin);

    for (int bin = 0; bin < numPoints; ++bin)
    {
        if (threadShouldExit()) break;

        reader->read(&readBuf, 0, samplesPerBin,
                     static_cast<juce::int64>(bin) * samplesPerBin, true, false);

        // Mono-Mix: sum all channels and divide.
        const int numCh = static_cast<int>(reader->numChannels);

        float truePeakFull = 0.0f;
        float truePeakLow  = 0.0f;
        float truePeakMid  = 0.0f;
        float truePeakHigh = 0.0f;

        double sqSumFull = 0.0, sqSumLow = 0.0, sqSumMid = 0.0, sqSumHigh = 0.0;

        float envValFull = 0.0f, envValLow = 0.0f, envValMid = 0.0f, envValHigh = 0.0f;

        for (int s = 0; s < samplesPerBin; ++s)
        {
            // Mono-Mix
            float mono = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                mono += readBuf.getReadPointer(ch)[s];
            mono /= static_cast<float>(numCh);

            // Stage 1: crossover
            float sigLow   = xoverLow.processLP(mono);
            float sigMidHP = xoverMid.processHP(mono);    // HP at 200 Hz
            float sigMid   = xoverMid.processLP(sigMidHP); // then LP at 4 kHz
            float sigHigh  = xoverHigh.processHP(mono);

            // Stage 2: rectify + envelope follower
            float rectFull = std::abs(mono);
            float rectLow  = std::abs(sigLow);
            float rectMid  = std::abs(sigMid);
            float rectHigh = std::abs(sigHigh);

            envValFull = envFull.process(rectFull);
            envValLow  = envLow.process(rectLow);
            envValMid  = envMid.process(rectMid);
            envValHigh = envHigh.process(rectHigh);

            // Stage 3a: true peak (raw signal, no envelope)
            if (rectFull > truePeakFull) truePeakFull = rectFull;
            if (rectLow  > truePeakLow)  truePeakLow  = rectLow;
            if (rectMid  > truePeakMid)  truePeakMid  = rectMid;
            if (rectHigh > truePeakHigh) truePeakHigh = rectHigh;

            // Stage 3b: RMS over envelope values
            sqSumFull += envValFull * envValFull;
            sqSumLow  += envValLow  * envValLow;
            sqSumMid  += envValMid  * envValMid;
            sqSumHigh += envValHigh * envValHigh;
        }

        // Build the bin from accumulated values.
        const float inv = 1.0f / static_cast<float>(samplesPerBin);
        TrackData::WaveformBin wbin;
        wbin.fullPeak = truePeakFull;
        wbin.fullRms  = std::sqrt(static_cast<float>(sqSumFull) * inv);
        wbin.lowPeak  = truePeakLow;
        wbin.lowRms   = std::sqrt(static_cast<float>(sqSumLow)  * inv);
        wbin.midPeak  = truePeakMid;
        wbin.midRms   = std::sqrt(static_cast<float>(sqSumMid)  * inv);
        wbin.highPeak = truePeakHigh;
        wbin.highRms  = std::sqrt(static_cast<float>(sqSumHigh) * inv);

        if (truePeakFull > globalMaxPeak) {
            globalMaxPeak = truePeakFull;
            m_trackData->setGlobalMaxPeak(globalMaxPeak);
        }

        batch.append(wbin);
        if (batch.size() >= batchSize) {
            m_trackData->appendData(batch);
            batch.clear();
            wait(1);
        }
    }

    if (!batch.isEmpty())
        m_trackData->appendData(batch);

    // Stage 4: short-time FFT to build an accumulated chromagram over the full track.
    // A 4096-point Hann-windowed FFT (hop 1024) maps each FFT bin to one of 12
    // pitch classes. The result is passed to Krumhansl-Schmuckler key detection.
    const int   fftOrder = 12;                   // 2^12 = 4096 points
    const int   fftSize  = 1 << fftOrder;
    juce::dsp::FFT fft(fftOrder);

    // Pre-compute Hann window
    std::vector<float> hannWindow(fftSize);
    for (int i = 0; i < fftSize; ++i)
        hannWindow[i] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / (fftSize - 1)));

    std::vector<float> chroma(12, 0.0f);
    std::vector<float> fftBuf(fftSize * 2, 0.0f); // real+imag interleaved for JUCE FFT

    // ODF (onset detection function) via low-band energy flux, one entry per waveform bin.
    std::vector<float> odf;
    odf.reserve(numPoints);
    float prevLowEnv = 0.0f;

    // Second reader for FFT frames, independent of the waveform reader above.
    std::unique_ptr<juce::AudioFormatReader> fftReader(m_formatManager->createReaderFor(file));
    if (!fftReader) { m_trackData->appendData(batch); return; }

    // FFT hop: one new frame every 1024 samples (~74% overlap with a 4096-point window).
    const int fftHop = 1024;
    const int numFftFrames = static_cast<int>((totalSamples - fftSize) / fftHop);
    juce::AudioBuffer<float> fftReadBuf(1, fftSize);

    // Maps an FFT bin index to a pitch class 0-11 (C=0 ... B=11).
    // Uses MIDI note formula: midi = 12 * log2(freq / A4) + 69, then mod 12.
    auto binToChroma = [&](int k) -> int {
        if (k <= 0) return -1;
        double freq = static_cast<double>(k) * sampleRate / fftSize;
        if (freq < 27.5 || freq > 4200.0) return -1;
        double midi = 12.0 * std::log2(freq / 440.0) + 69.0;
        int pc = static_cast<int>(std::round(midi)) % 12;
        if (pc < 0) pc += 12;
        return pc;
    };

    for (int f = 0; f < numFftFrames; ++f) {
        if (threadShouldExit()) break;

        fftReader->read(&fftReadBuf, 0, fftSize,
                        static_cast<juce::int64>(f) * fftHop, true, false);

        const float* src = fftReadBuf.getReadPointer(0);

        // Apply Hann window and copy into real part of fftBuf
        for (int i = 0; i < fftSize; ++i) {
            fftBuf[i * 2]     = src[i] * hannWindow[i]; // real
            fftBuf[i * 2 + 1] = 0.0f;                   // imag
        }

        fft.performFrequencyOnlyForwardTransform(fftBuf.data());

        // Accumulate magnitudes into chroma bins
        for (int k = 1; k < fftSize / 2; ++k) {
            int pc = binToChroma(k);
            if (pc >= 0)
                chroma[pc] += fftBuf[k]; // after ForwardTransform, fftBuf holds magnitudes
        }
    }

    if (threadShouldExit()) return;

    // Build the ODF from already-computed lowRms values: positive half-wave rectified flux.
    {
        QVector<TrackData::WaveformBin> waveData = m_trackData->getWaveformData();
        odf.resize(waveData.size(), 0.0f);
        float prev = 0.0f;
        for (int i = 0; i < waveData.size(); ++i) {
            float cur = waveData[i].lowRms;
            odf[i] = std::max(0.0f, cur - prev);
            prev   = cur;
        }
    }

    if (threadShouldExit()) return;

    // BPM via autocorrelation of the ODF.
    // Autocorrelation averages over all beats simultaneously, making it robust
    // against occasional weak or missing beats that would trip up median-IBI methods.
    // Steps:
    //   1. R[lag] = sum(odf[t] * odf[t+lag]) for lag in [lagMin, lagMax]
    //   2. Find the lag with the strongest peak -> beat period
    //   3. Sub-sample refinement via parabolic interpolation
    const int odfSize = static_cast<int>(odf.size());

    // Lag bounds corresponding to 200 BPM (short interval) and 60 BPM (long interval).
    const float odfRate   = static_cast<float>(m_pointsPerSecond);
    const int   lagMin    = static_cast<int>(odfRate * 60.0f / 200.0f);
    const int   lagMax    = static_cast<int>(odfRate * 60.0f / 60.0f);
    const int   acfLength = lagMax + 1;

    std::vector<double> acf(acfLength, 0.0);

    // Compute normalised autocorrelation (O(N * lagMax) ~ 1M ops for a typical track).
    for (int lag = lagMin; lag <= lagMax; ++lag) {
        double sum = 0.0;
        int    cnt = odfSize - lag;
        if (cnt <= 0) continue;
        for (int t = 0; t < cnt; ++t)
            sum += odf[t] * odf[t + lag];
        acf[lag] = sum / cnt; // normalised by length
    }

    // Find the strongest peak in acf[lagMin..lagMax].
    int    bestLag  = lagMin;
    double bestAcf  = -1.0;
    for (int lag = lagMin; lag <= lagMax; ++lag) {
        if (acf[lag] > bestAcf) { bestAcf = acf[lag]; bestLag = lag; }
    }

    if (bestAcf <= 0.0 || threadShouldExit()) return;

    // Parabolic interpolation for sub-bin precision (only when peak is not at the boundary).
    double refinedLag = static_cast<double>(bestLag);
    if (bestLag > lagMin && bestLag < lagMax) {
        double y0 = acf[bestLag - 1];
        double y1 = acf[bestLag];
        double y2 = acf[bestLag + 1];
        double denom = 2.0 * (2.0 * y1 - y0 - y2);
        if (std::abs(denom) > 1e-10)
            refinedLag += (y0 - y2) / denom;
    }

    double estimatedBpm = odfRate * 60.0 / refinedLag;

    // Oktavkorrektur: manchmal findet die ACF die halbe/doppelte Periode
    while (estimatedBpm < 80.0)  estimatedBpm *= 2.0;
    while (estimatedBpm > 175.0) estimatedBpm /= 2.0;

    // First beat offset: find the earliest ODF peak that fits the detected grid.
    // Beats are filtered by a ±12% tolerance window around the beat period.
    const double beatPeriodBins = odfRate * 60.0 / estimatedBpm;
    const double tolerance      = beatPeriodBins * 0.12;

    // Only consider local maxima above 30% of the global ODF maximum.
    float odfMax = *std::max_element(odf.begin(), odf.end());
    float odfThresh = odfMax * 0.30f;

    // Collect candidate beat positions as local maxima above the threshold.
    std::vector<int> candidates;
    for (int i = 1; i < odfSize - 1; ++i) {
        if (odf[i] > odfThresh && odf[i] >= odf[i-1] && odf[i] >= odf[i+1])
            candidates.push_back(i);
    }

    // Pick the earliest candidate whose phase is consistent with the most other candidates
    // (majority vote over a set of up to 20 early hypothesis reference points).
    qint64 firstBeatSample = 0;
    if (!candidates.empty()) {
        int refBin = candidates[0];

        int bestScore = 0;
        int bestRef   = refBin;
        int testCount = std::min((int)candidates.size(), 20);
        for (int ci = 0; ci < testCount; ++ci) {
            int ref = candidates[ci];
            int score = 0;
            for (int cj = 0; cj < (int)candidates.size(); ++cj) {
                double dist = candidates[cj] - ref;
                double mod  = std::fmod(std::abs(dist), beatPeriodBins);
                if (mod > beatPeriodBins / 2.0) mod = beatPeriodBins - mod;
                if (mod < tolerance) ++score;
            }
            if (score > bestScore) { bestScore = score; bestRef = ref; }
        }

        // Convert ODF bin to audio sample: bin i -> sample i * samplesPerBin
        firstBeatSample = static_cast<qint64>(bestRef) * samplesPerBin;
    }

    m_trackData->setBpmData(estimatedBpm, firstBeatSample, sampleRate);

    if (threadShouldExit()) return;

    // Key detection via Krumhansl-Schmuckler algorithm.
    // The accumulated chromagram is correlated against all 24 rotated KS profiles
    // (12 major + 12 minor). The root with the highest Pearson correlation wins.
    //
    // KS profiles (Krumhansl & Schmuckler 1990, normalised):
    //   Major: C=6.35 C#=2.23 D=3.48 D#=2.33 E=4.38 F=4.09
    //          F#=2.52 G=5.19 G#=2.39 A=3.66 A#=2.29 B=2.88
    //   Minor: C=6.33 C#=2.68 D=3.52 D#=5.38 E=2.60 F=3.53
    //          F#=2.54 G=4.75 G#=3.98 A=2.69 A#=3.34 B=3.17
    static const double ksMajor[12] = {
        6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
        2.52, 5.19, 2.39, 3.66, 2.29, 2.88
    };
    static const double ksMinor[12] = {
        6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
        2.54, 4.75, 3.98, 2.69, 3.34, 3.17
    };

    // Camelot Wheel notation: index 0=C, 1=C#, ..., 11=B (major=B-side, minor=A-side)
    static const char* camelotMajor[12] = {
        "8B","3B","10B","5B","12B","7B","2B","9B","4B","11B","6B","1B"
    };
    static const char* camelotMinor[12] = {
        "5A","12A","7A","2A","9A","4A","11A","6A","1A","8A","3A","10A"
    };

    // Normalise chroma vector.
    double chromaSum = 0.0;
    for (int i = 0; i < 12; ++i) chromaSum += chroma[i];
    if (chromaSum < 1e-10) { return; } // no chroma data -> no key
    for (int i = 0; i < 12; ++i) chroma[i] /= chromaSum;

    // Pearson correlation against all 24 rotated profiles.
    // Rotation by r semitones = transposition to another key.
    auto pearson = [](const double* profile, const std::vector<float>& c, int root) -> double {
        double pMean = 0.0, cMean = 0.0;
        for (int i = 0; i < 12; ++i) {
            pMean += profile[(i + root) % 12];
            cMean += c[i];
        }
        pMean /= 12.0; cMean /= 12.0;

        double num = 0.0, dp = 0.0, dc = 0.0;
        for (int i = 0; i < 12; ++i) {
            double pv = profile[(i + root) % 12] - pMean;
            double cv = c[i] - cMean;
            num += pv * cv;
            dp  += pv * pv;
            dc  += cv * cv;
        }
        double denom = std::sqrt(dp * dc);
        return denom > 1e-10 ? num / denom : 0.0;
    };

    int    bestRoot  = 0;
    bool   bestMajor = true;
    double bestCorr  = -2.0;

    for (int root = 0; root < 12; ++root) {
        double corrMaj = pearson(ksMajor, chroma, root);
        double corrMin = pearson(ksMinor, chroma, root);
        if (corrMaj > bestCorr) { bestCorr = corrMaj; bestRoot = root; bestMajor = true;  }
        if (corrMin > bestCorr) { bestCorr = corrMin; bestRoot = root; bestMajor = false; }
    }

    QString detectedKey = bestMajor
        ? QString(camelotMajor[bestRoot])
        : QString(camelotMinor[bestRoot]);

    m_trackData->setKeyData(detectedKey);
}
