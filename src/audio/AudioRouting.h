#pragma once

// AudioRouting.h - pure topology declaration. No DSP logic, no state.
// Must match docs/architecture/audio-routing-target.md.

enum class LogicalBus {
    Master,
    Headphones,
    Booth,
    DeckA,
    DeckB,
    DeckC,
    DeckD
};

enum class CrossfaderAssignment {
    A,
    Thru,
    B
};

enum class ChannelFaderCurve {
    Smooth,
    Linear,
    Fast
};

enum class CrossfaderCurve {
    ConstantPower,
    Smooth,
    Scratch
};

namespace AudioRoutingConstants {

constexpr int kScratchCrossfadeMinSamples = 32;
constexpr int kScratchCrossfadeMaxSamples = 128;
constexpr int kCacheMissResumeCrossfadeMinSamples = 32;
constexpr int kCacheMissResumeCrossfadeMaxSamples = 128;

} // namespace AudioRoutingConstants

// Canonical signal flow:
//
// DeckAudioPipeline:
//   Cache -> PlaybackReader -> Transport -> RenderModeRouter
//   -> Trim -> ChannelEQ -> ColorFX -> DeckFX(PreFaderInsert)
//   -> ChannelMeter -> PFLTap -> ChannelFader -> CrossfaderGain -> DeckProgramOutput
//
// MasterMixer:
//   Sum(DeckProgramOutputs) + Sum(DeckFX PostFaderTail wet returns)
//   -> MasterFX -> MasterGain -> Limiter -> MasterMeter -> MasterTap
//
// HeadphoneBus:
//   Sum(PFLTaps) -> CueSum; (CueSum, Master) -> ConstantPowerBlend
//   -> HeadphoneGain -> HeadphoneLimiter -> HeadphoneOutput
