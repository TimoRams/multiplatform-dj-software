#pragma once

#include <QString>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

class DeckCueLoopController
{
public:
    static constexpr int kSlotCount = 8;
    static constexpr double kUnsetMainCue = -11.0;

    struct MainCueState {
        double positionSec = kUnsetMainCue;
        bool previewActive = false;
        bool buttonDown = false;
        bool holdPreviewPending = false;
        std::uint64_t pressSerial = 0;
    };

    struct HotCue {
        bool set = false;
        double positionSec = 0.0;
        QString label;
        QString color;
        friend bool operator==(const HotCue&, const HotCue&) = default;
    };

    struct ActiveLoopState {
        bool active = false;
        bool inSet = false;
        double inSec = 0.0;
        double outSec = 0.0;
        double lengthBeats = 0.0;
    };

    struct SavedLoop {
        bool set = false;
        double inSec = 0.0;
        double outSec = 0.0;
        double lengthBeats = 0.0;
        QString label;
        QString color;
        friend bool operator==(const SavedLoop&, const SavedLoop&) = default;
    };

    struct PendingCueJump {
        bool active = false;
        double fireSec = 0.0;
        double targetSec = 0.0;
        double lastPositionSec = 0.0;
        std::uint64_t trackGeneration = 0;
    };

    struct BeatGridSnapshot {
        std::vector<double> beats;
        double bpm = 0.0;
        double firstBeatSec = 0.0;
    };

    DeckCueLoopController();

    void beginTrack(std::uint64_t generation);
    [[nodiscard]] std::uint64_t trackGeneration() const noexcept { return m_trackGeneration; }

    [[nodiscard]] MainCueState& mainCue() noexcept { return m_mainCue; }
    [[nodiscard]] const MainCueState& mainCue() const noexcept { return m_mainCue; }
    [[nodiscard]] ActiveLoopState& activeLoop() noexcept { return m_activeLoop; }
    [[nodiscard]] const ActiveLoopState& activeLoop() const noexcept { return m_activeLoop; }
    [[nodiscard]] PendingCueJump& pendingCueJump() noexcept { return m_pendingCueJump; }
    [[nodiscard]] const PendingCueJump& pendingCueJump() const noexcept { return m_pendingCueJump; }
    [[nodiscard]] std::array<HotCue, kSlotCount>& hotCues() noexcept { return m_hotCues; }
    [[nodiscard]] const std::array<HotCue, kSlotCount>& hotCues() const noexcept { return m_hotCues; }
    [[nodiscard]] std::array<SavedLoop, kSlotCount>& savedLoops() noexcept { return m_savedLoops; }
    [[nodiscard]] const std::array<SavedLoop, kSlotCount>& savedLoops() const noexcept { return m_savedLoops; }

    [[nodiscard]] bool validIndex(int index) const noexcept;
    void clearHotCues();
    void clearSavedLoops();
    bool setHotCue(int index, double positionSec, QString label = {}, QString color = {});
    bool clearHotCue(int index);
    bool setHotCueColor(int index, QString color);
    bool setSavedLoop(int index, double inSec, double outSec, double lengthBeats,
                      QString label = {}, QString color = {});
    bool clearSavedLoop(int index);

    bool setLoop(double inSec, double outSec, double lengthBeats, bool active = true);
    bool clearLoop() noexcept;
    bool deactivateLoop() noexcept;
    bool reactivateLoop() noexcept;

    void scheduleCueJump(double targetSec, double fireSec, double currentPosition) noexcept;
    void cancelCueJump() noexcept;
    [[nodiscard]] std::optional<double> serviceCueJump(double currentPosition) noexcept;

    [[nodiscard]] static double quantizedBeatAt(double sec, const BeatGridSnapshot& grid) noexcept;
    [[nodiscard]] static double nextBeatBoundaryAfter(double sec, const BeatGridSnapshot& grid) noexcept;
    [[nodiscard]] static double beatDurationAround(double sec, const BeatGridSnapshot& grid) noexcept;
    [[nodiscard]] static double beatJumpTarget(double positionSec, double beats,
                                               const BeatGridSnapshot& grid) noexcept;
    [[nodiscard]] static QString defaultHotCueColor(int index);
    [[nodiscard]] static QString defaultSavedLoopColor(int index);

private:
    std::uint64_t m_trackGeneration = 0;
    MainCueState m_mainCue;
    std::array<HotCue, kSlotCount> m_hotCues;
    ActiveLoopState m_activeLoop;
    std::array<SavedLoop, kSlotCount> m_savedLoops;
    PendingCueJump m_pendingCueJump;
};
