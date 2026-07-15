#pragma once

#include <QObject>
#include <algorithm>
#include <array>
#include <cmath>

class SettingsManager;

class UiScaleController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double scale READ scale WRITE setScale NOTIFY scaleChanged)
    Q_PROPERTY(int percent READ percent NOTIFY scaleChanged)

public:
    static constexpr std::array<double, 7> kScaleSteps {0.80, 0.90, 1.00, 1.10, 1.20, 1.30, 1.40};

    explicit UiScaleController(SettingsManager* settings = nullptr, QObject* parent = nullptr);

    [[nodiscard]] double scale() const noexcept { return m_scale; }
    [[nodiscard]] int percent() const noexcept;
    [[nodiscard]] static double validatedScale(double value) noexcept
    {
        if (!std::isfinite(value))
            return 1.0;
        const auto closest = std::ranges::min_element(kScaleSteps, [value](double a, double b) {
            return std::abs(a - value) < std::abs(b - value);
        });
        return closest != kScaleSteps.end() ? *closest : 1.0;
    }
    [[nodiscard]] static double increasedScale(double value) noexcept
    {
        value = validatedScale(value);
        const auto next = std::ranges::find_if(kScaleSteps, [value](double step) { return step > value + 0.001; });
        return next == kScaleSteps.end() ? kScaleSteps.back() : *next;
    }
    [[nodiscard]] static double decreasedScale(double value) noexcept
    {
        value = validatedScale(value);
        for (auto it = kScaleSteps.rbegin(); it != kScaleSteps.rend(); ++it)
            if (*it < value - 0.001)
                return *it;
        return kScaleSteps.front();
    }

    Q_INVOKABLE void setScale(double value);
    Q_INVOKABLE void increase();
    Q_INVOKABLE void decrease();
    Q_INVOKABLE void reset();

signals:
    void scaleChanged();

private:
    void persist();

    SettingsManager* m_settings = nullptr;
    double m_scale = 1.0;
};
