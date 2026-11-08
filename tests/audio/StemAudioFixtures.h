#pragma once

#include "audio/cache/AudioPageCache.h"
#include "stems/StemTrackData.h"

#include <memory>

inline std::shared_ptr<brockdj::stems::StemTrackData> makeHalfVocalStemData(
    AudioPageCache& cache, const AudioCacheHandle& handle, bool complete = true)
{
    using namespace brockdj::stems;
    auto data = std::make_shared<StemTrackData>(
        handle.id(), handle.sampleRate(), handle.lengthInSamples());
    for (std::int64_t index = 0; index < handle.pageCount(); ++index) {
        const auto page = cache.tryGetPage(handle, index);
        if (!page)
            return {};
        for (int channel = 0; channel < kStemChannels; ++channel) {
            const auto sourceChannel = std::min(channel, static_cast<int>(page->channelCount) - 1);
            const float* samples = page->channelData(static_cast<unsigned>(sourceChannel));
            for (std::int64_t i = 0; i < page->validSampleCount; ++i)
                data->write(StoredStem::Vocals, channel, page->firstSample + i, 0.5f * samples[i]);
        }
    }
    if (complete)
        data->markAllFinal();
    return data;
}
