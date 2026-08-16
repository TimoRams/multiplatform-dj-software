#pragma once

// Lightweight FX vocabulary shared by the control, deck and audio layers.
// Keep this header free of processor and JUCE dependencies: callers that only
// route an effect command should not need the complete DSP implementation.

enum class EffectType : int {
    None         = 0,
    Reverb       = 1,
    Bitcrusher   = 2,
    PitchShifter = 3,
    Echo         = 4,
    LowCutEcho   = 5,
    MtDelay      = 6,
    Spiral       = 7,
    Flanger      = 8,
    Phaser       = 9,
    Trans        = 10,
    EnigmaJet    = 11,
    Stretch      = 12,
    SlipRoll     = 13,
    Roll         = 14,
    MobiusSaw    = 15,
    MobiusTri    = 16,
    SoundColorFilter  = 17,
    SoundColorDubEcho = 18,
    SoundColorCrush   = 19,
    SoundColorSpace   = 20,
    SoundColorPitch   = 21,
    SoundColorNoise   = 22,
    SoundColorSweep   = 23,
    RollOut           = 24
};

enum class FxPlacement {
    PreFaderInsert,
    PostFaderTail,
    Master
};
