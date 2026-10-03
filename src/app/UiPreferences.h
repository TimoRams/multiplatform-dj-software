#pragma once

#include "waveform/WaveformAggregator.h"

#include <QObject>
#include <QString>
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

class WaveformZoomController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    // Relative to the default detail view; the raw value is pixels per line.
    Q_PROPERTY(QString zoomLabel READ zoomLabel NOTIFY zoomChanged)
    // Geometric detents use a logarithmic fraction across the zoom range.
    Q_PROPERTY(double zoomFraction READ zoomFraction NOTIFY zoomChanged)

public:
    static constexpr double kMinimum = 0.0056;
    static constexpr double kMaximum = 10.0;
    static constexpr double kDefault = 0.22;
    static constexpr double kFactor = 1.15;

    explicit WaveformZoomController(SettingsManager* settings = nullptr, QObject* parent = nullptr);

    [[nodiscard]] double zoom() const noexcept { return m_zoom; }
    [[nodiscard]] QString zoomLabel() const { return zoomLabelFor(m_zoom); }
    [[nodiscard]] double zoomFraction() const noexcept { return zoomFractionFor(m_zoom); }

    [[nodiscard]] static QString zoomLabelFor(double value)
    {
        const double relative = validatedZoom(value) / kDefault;
        // Two decimals keep the small zoom-out detents distinguishable.
        return QStringLiteral("%1x").arg(relative,
                                         0,
                                         'f',
                                         relative < 1.0 ? 2 : 1);
    }
    [[nodiscard]] static double zoomFractionFor(double value) noexcept
    {
        const double span = std::log(kMaximum / kMinimum);
        if (!(span > 0.0))
            return 0.0;
        return std::clamp(std::log(validatedZoom(value) / kMinimum) / span, 0.0, 1.0);
    }

    [[nodiscard]] static double validatedZoom(double value) noexcept
    {
        if (!std::isfinite(value))
            return kDefault;
        return std::clamp(value, kMinimum, kMaximum);
    }
    [[nodiscard]] static double increasedZoom(double value) noexcept
    {
        return validatedZoom(validatedZoom(value) * kFactor);
    }
    [[nodiscard]] static double decreasedZoom(double value) noexcept
    {
        return validatedZoom(validatedZoom(value) / kFactor);
    }
    [[nodiscard]] static std::uint8_t lodLevelForPhysicalPixels(
        double physicalPixelsPerCanonicalLine) noexcept
    {
        return waveform::WaveformLodPyramid::selectLevel(
            physicalPixelsPerCanonicalLine);
    }

    Q_INVOKABLE void setZoom(double value);
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void reset();

signals:
    void zoomChanged();

private:
    void persist();

    SettingsManager* m_settings = nullptr;
    double m_zoom = kDefault;
};
