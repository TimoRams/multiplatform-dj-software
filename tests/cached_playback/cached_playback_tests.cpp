#include "audio/cache/CachedPlaybackAudioSource.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace {
bool require(bool v, const char* m) { if (!v) std::cerr << "FAIL: " << m << '\n'; return v; }
bool writeWave(const QString& path, double sr, int channels, int samples)
{
    juce::WavAudioFormat f;
    auto fs = std::make_unique<juce::FileOutputStream>(juce::File(path.toStdString()));
    if (!fs->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> stream = std::move(fs);
    auto w = f.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(sr)
        .withNumChannels(channels).withBitsPerSample(16));
    if (!w) return false;
    juce::AudioBuffer<float> b(channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < samples; ++i)
            b.setSample(ch, i, static_cast<float>(0.2 * std::sin(2 * juce::MathConstants<double>::pi * (220 + ch * 100) * i / sr)));
    return w->writeFromAudioSampleBuffer(b, 0, samples);
}
bool waitPages(AudioPageCache& c, const AudioCacheHandle& h)
{
    c.requestRange(h, 0, h.pageCount() - 1, AudioCachePriority::PlaybackReadAhead);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        bool ready = true;
        for (int p = 0; p < h.pageCount(); ++p) ready &= static_cast<bool>(c.tryGetPage(h, p));
        if (ready) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
bool finite(const juce::AudioBuffer<float>& b) {
    for (int c=0;c<b.getNumChannels();++c) for(int i=0;i<b.getNumSamples();++i)
        if(!std::isfinite(b.getSample(c,i))) return false; return true;
}
}

static_assert(noexcept(std::declval<CachedPlaybackAudioSource&>().getNextAudioBlock(
    std::declval<const juce::AudioSourceChannelInfo&>())));

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); bool ok = true; QTemporaryDir dir;
    const QString stereo = dir.filePath("stereo.wav"), mono = dir.filePath("mono.wav");
    const QString highRate = dir.filePath("96k.wav"), ultraRate = dir.filePath("192k.wav");
    ok &= require(writeWave(stereo, 48000, 2, 50000), "stereo fixture");
    ok &= require(writeWave(mono, 44100, 1, 17000), "mono fixture");
    ok &= require(writeWave(highRate, 96000, 2, 17000), "96 kHz fixture");
    ok &= require(writeWave(ultraRate, 192000, 2, 17000), "192 kHz fixture");
    AudioPageCache cache(16 * 1024 * 1024);
    auto h = cache.openTrack({stereo});
    ok &= require(waitPages(cache, h), "pages ready");
    CachedPlaybackAudioSource source(cache, h);
    source.prepareToPlay(8192, 48000);
    for (int size : {64,128,256,512,1024,2048,4096,8192}) {
        juce::AudioBuffer<float> out(2, size); source.getNextAudioBlock({&out,0,size});
        ok &= require(finite(out), "forward blocks finite");
    }
    source.setNextReadPosition(16380);
    juce::AudioBuffer<float> cross(2, 8192); source.getNextAudioBlock({&cross,0,8192});
    ok &= require(source.getNextReadPosition() == 16380 + 8192, "cross-page position advances");
    source.setReverse(true); source.setCommandedReadPosition(20000); source.getNextAudioBlock({&cross,0,8192});
    ok &= require(source.getNextReadPosition() == 20000 - 8192, "reverse position retreats");
    source.setLoopRangeSamples(16000, 17000, 48000); source.setReverse(false);
    source.setNextReadPosition(16950); source.getNextAudioBlock({&cross,0,8192});
    ok &= require(source.getNextReadPosition() >= 16000 && source.getNextReadPosition() < 17000,
                  "forward loop wraps");
    source.setReverse(true); source.setCommandedReadPosition(16010); source.getNextAudioBlock({&cross,0,8192});
    ok &= require(source.getNextReadPosition() >= 16000 && source.getNextReadPosition() < 17000,
                  "reverse loop wraps");

    auto hm = cache.openTrack({mono}); ok &= require(waitPages(cache, hm), "mono ready");
    CachedPlaybackAudioSource monoSource(cache, hm); juce::AudioBuffer<float> mo(2,512);
    monoSource.getNextAudioBlock({&mo,0,512}); ok &= require(finite(mo), "mono to stereo finite");
    for (const auto& path : {highRate, ultraRate}) {
        auto rateHandle = cache.openTrack({path});
        ok &= require(waitPages(cache, rateHandle), "high-rate pages ready");
        CachedPlaybackAudioSource rateSource(cache, rateHandle);
        rateSource.getNextAudioBlock({&mo,0,512});
        ok &= require(finite(mo), "high-rate playback finite");
        cache.releaseTrack(rateHandle);
    }

    AudioPageCache missCache(16 * 1024 * 1024); auto missH = missCache.openTrack({stereo});
    CachedPlaybackAudioSource miss(missCache, missH); cross.clear(); miss.getNextAudioBlock({&cross,0,8192});
    ok &= require(finite(cross) && miss.cacheStats().starvationBlocks > 0, "miss fades without I/O");
    ok &= require(waitPages(missCache, missH), "miss requests decode asynchronously");
    miss.setNextReadPosition(0); miss.getNextAudioBlock({&cross,0,8192});
    ok &= require(miss.cacheStats().recoveryEvents > 0, "recovery fades in");

    auto shared = cache.openTrack({stereo});
    ok &= require(shared.id() == h.id(), "two decks share cache entry");
    cache.releaseTrack(shared);
    ok &= require(source.cacheStats().diskReadsFromAudioThread == 0, "no RT disk reads");
    ok &= require(source.cacheStats().decoderCallsFromAudioThread == 0, "no RT decoder calls");

    source.setReverse(false); source.setNextReadPosition(-100);
    ok &= require(source.getNextReadPosition() == 0, "pre-roll clamps to zero");
    source.setLooping(false); source.setNextReadPosition(h.lengthInSamples()-100);
    source.getNextAudioBlock({&cross,0,8192});
    ok &= require(source.getNextReadPosition() == h.lengthInSamples(), "track end clamps");

    if (qEnvironmentVariableIsSet("BROCKDJ_PLAYBACK_BENCHMARK")) {
        source.setNextReadPosition(0); const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<1000;++i){ source.setNextReadPosition((i*512)%40000); source.getNextAudioBlock({&mo,0,512}); }
        std::cout << "cached playback 512-block us=" << std::chrono::duration<double,std::micro>(
            std::chrono::steady_clock::now()-start).count()/1000 << '\n';
    }
    return ok ? 0 : 1;
}
