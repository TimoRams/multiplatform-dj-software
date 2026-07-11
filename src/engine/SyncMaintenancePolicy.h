#pragma once

namespace engine {

[[nodiscard]] constexpr bool shouldRunFollowerSyncMaintenance(bool syncEnabled,
                                                              bool isSyncMaster,
                                                              bool scrubbing,
                                                              bool releaseGlide) noexcept
{
    return syncEnabled && !isSyncMaster && !scrubbing && !releaseGlide;
}

} // namespace engine
