#pragma once

#include <QString>
#include <algorithm>
#include <cmath>

namespace analysis {
enum class AnalysisProfile { Electronic, Legacy };

struct AnalysisOptions {
    AnalysisProfile profile = AnalysisProfile::Electronic;
    double minBpm = 48.0;
    double maxBpm = 255.0;

    [[nodiscard]] AnalysisOptions normalized() const noexcept {
        auto value = *this;
        value.minBpm = std::clamp(std::isfinite(minBpm) ? minBpm : 48.0, 30.0, 299.0);
        value.maxBpm = std::clamp(std::isfinite(maxBpm) ? maxBpm : 255.0, value.minBpm + 1.0, 300.0);
        return value;
    }
    [[nodiscard]] QString fingerprint() const {
        const auto value = normalized();
        return QStringLiteral("%1-v1:%2:%3")
            .arg(profile == AnalysisProfile::Electronic ? QStringLiteral("electronic") : QStringLiteral("legacy"))
            .arg(value.minBpm, 0, 'g', 17).arg(value.maxBpm, 0, 'g', 17);
    }
};
}
