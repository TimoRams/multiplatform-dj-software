#pragma once

#include <cstdint>

namespace engine::sync {

enum class SyncError : std::uint8_t {
    None,
    NoTrack,
    InvalidBpm,
    StaleMasterGeneration,
    StaleTrackGeneration
};

struct DeckSyncInputSnapshot {
    bool hasTrack = false;
    bool playing = false;
    bool scratching = false;
    bool scratchRelease = false;
    bool reverse = false;
    bool slipEnabled = false;
    bool loopActive = false;
    bool keylockEnabled = false;
    bool beatgridValid = false;
    bool downbeatValid = false;

    double trackBpm = 0.0;
    double effectiveBpm = 0.0;
    double playbackRate = 1.0;
    double audiblePositionSeconds = 0.0;
    double beatPosition = 0.0;
    double barPosition = 0.0;
    double beatPhase = 0.0;
    double beatLengthSeconds = 0.0;
    double keylockLatencySeconds = 0.0;
    double beatConfidence = 0.0;
    double downbeatConfidence = 0.0;

    std::uint64_t trackIdentity = 0;
    std::uint64_t trackGeneration = 0;
    std::uint64_t transportGeneration = 0;
};

struct DeckSyncCommand {
    bool syncEnabled = false;
    bool isMaster = false;
    bool phaseArrangeRequested = false;
    bool resyncRequested = false;
    bool tightDoubleSync = false;

    double targetBpm = 0.0;
    double masterBeatPhase = 0.0;
    double masterBarPosition = 0.0;
    double masterPositionSeconds = 0.0;
    double masterKeylockLatencySeconds = 0.0;
    bool masterPlaying = false;
    bool sameTrack = false;

    std::uint64_t masterGeneration = 0;
    std::uint64_t targetTrackGeneration = 0;
};

struct DeckSyncSnapshot {
    int deckIndex = -1;
    bool syncEnabled = false;
    bool isMaster = false;
    bool tempoSyncAvailable = false;
    bool phaseSyncAvailable = false;
    double targetBpm = 0.0;
    double phaseErrorBeats = 0.0;
    double phaseNudgePercent = 0.0;
    SyncError error = SyncError::None;
    std::uint64_t masterGeneration = 0;
    std::uint64_t trackGeneration = 0;
    std::uint64_t stateGeneration = 0;
};

struct DeckSyncActions {
    bool tempoChanged = false;
    double targetTempoPercent = 0.0;
    bool phaseNudgeChanged = false;
    double phaseNudgePercent = 0.0;
    bool seekRequested = false;
    double seekOffsetSeconds = 0.0;
    std::uint64_t targetTrackGeneration = 0;
    std::uint64_t masterGeneration = 0;
};

struct LinkSyncSnapshot {
    bool enabled = false;
    int numPeers = 0;
    double bpm = 120.0;
    double beat = 0.0;
    double phase = 0.0;
    std::uint64_t generation = 0;
};

struct SyncCoordinatorSnapshot {
    int masterDeckIndex = -1;
    double masterBpm = 0.0;
    double masterBeatPhase = 0.0;
    double masterBarPosition = 0.0;
    bool linkActive = false;
    std::uint64_t masterGeneration = 0;
    std::uint64_t stateGeneration = 0;
};

} // namespace engine::sync
