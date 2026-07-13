#include "DjEngineCommonIncludes.h"
#include "audio/device/AudioDeviceService.h"


void DjEngine::refreshHardwareLatency()
{
    if (auto* device = m_audioDeviceService.manager().getCurrentAudioDevice()) {
        const auto latency = readOutputLatencySnapshot(device);
        if (latency.sampleRate > 0.0) {
            m_latencySeconds.store(
                static_cast<float>(static_cast<double>(latency.backendOutputSamples) / latency.sampleRate),
                std::memory_order_relaxed);
            const int visualCompSamples = latency.callbackBufferSamples + latency.backendOutputSamples;
            m_visualLatencyCompensationSeconds.store(
                static_cast<float>(std::clamp(static_cast<double>(visualCompSamples) / latency.sampleRate, 0.0, 0.250)),
                std::memory_order_relaxed);
        }

        const bool changed = (latency.backendOutputSamples != m_lastLoggedEffectiveSamples)
                          || (latency.outputRawSamples != m_lastLoggedOutputRawSamples)
                          || (latency.callbackBufferSamples != m_lastLoggedBufferSamples)
                          || (latency.roundedSampleRate() != m_lastLoggedSampleRateRounded)
                          || m_latencyLoggedNoDevice;

        if (changed) {
            qInfo() << "[DjEngine] Backend output latency:" << latency.backendOutputSamples
                    << "smp" << "(" << m_latencySeconds.load(std::memory_order_relaxed) << "s)"
                    << "raw:" << latency.outputRawSamples
                    << "buf:" << latency.callbackBufferSamples
                    << "sr:" << latency.roundedSampleRate();
            m_lastLoggedEffectiveSamples  = latency.backendOutputSamples;
            m_lastLoggedOutputRawSamples  = latency.outputRawSamples;
            m_lastLoggedBufferSamples     = latency.callbackBufferSamples;
            m_lastLoggedSampleRateRounded = latency.roundedSampleRate();
            m_latencyLoggedNoDevice = false;
        }
    } else {
        m_visualLatencyCompensationSeconds.store(0.0f, std::memory_order_relaxed);
        if (!m_latencyLoggedNoDevice) {
            qInfo() << "[DjEngine] No audio device yet; keeping last known latency";
            m_latencyLoggedNoDevice = true;
        }
    }
}


DjEngine::LatencySnapshot DjEngine::buildLatencySnapshot() const
{
    LatencySnapshot snapshot;
    if (m_lastLatencySnapshot.sampleRate > 0.0)
        snapshot.sampleRate = m_lastLatencySnapshot.sampleRate;

    if (auto* device = m_audioDeviceService.manager().getCurrentAudioDevice()) {
        const auto latency = readOutputLatencySnapshot(device);
        snapshot.outputRawSamples = latency.outputRawSamples;
        snapshot.bufferSamples = latency.callbackBufferSamples;
        snapshot.backendOutputSamples = latency.backendOutputSamples;
        if (latency.sampleRate > 0.0)
            snapshot.sampleRate = latency.sampleRate;
    } else if (m_lastLatencySnapshot.sampleRate > 0.0) {
        snapshot.outputRawSamples = m_lastLatencySnapshot.outputRawSamples;
        snapshot.bufferSamples = m_lastLatencySnapshot.bufferSamples;
        snapshot.backendOutputSamples = m_lastLatencySnapshot.backendOutputSamples;
    }

    if (m_audioGraph->timeStretchPtr())
        snapshot.keylockSamples = std::max(0, m_audioGraph->timeStretch().getLatencySamples());

    snapshot.limiterSamples = std::max(0, DjMasterBus::limiterLatencySamples());
    snapshot.resamplerSamples = 0;
    snapshot.mixerFxSamples = 0;
    m_lastLatencySnapshot = snapshot;
    return snapshot;
}


double DjEngine::totalLatencyMs() const
{
    const auto snapshot = buildLatencySnapshot();
    if (snapshot.sampleRate <= 0.0)
        return 0.0;

    const int totalSamples = snapshot.bufferSamples
                           + snapshot.backendOutputSamples
                           + snapshot.keylockSamples
                           + snapshot.resamplerSamples
                           + snapshot.limiterSamples
                           + snapshot.mixerFxSamples;
    return (static_cast<double>(totalSamples) / snapshot.sampleRate) * 1000.0;
}


QVariantList DjEngine::latencyBreakdown() const
{
    const auto snapshot = buildLatencySnapshot();
    if (snapshot.sampleRate <= 0.0)
        return {};

    const auto toMs = [sampleRate = snapshot.sampleRate](int samples) -> double {
        return (static_cast<double>(samples) / sampleRate) * 1000.0;
    };

    QVariantList rows;
    const int audioDeviceSamples = snapshot.bufferSamples + snapshot.backendOutputSamples;
    const int dspSamples = snapshot.keylockSamples
                         + snapshot.resamplerSamples
                         + snapshot.limiterSamples
                         + snapshot.mixerFxSamples;

    QVariantMap audioDeviceRow;
    audioDeviceRow.insert("name", QStringLiteral("Audio Device Total"));
    audioDeviceRow.insert("samples", audioDeviceSamples);
    audioDeviceRow.insert("ms", toMs(audioDeviceSamples));
    audioDeviceRow.insert("countInTotal", false);
    rows.push_back(audioDeviceRow);

    QVariantMap bufferRow;
    bufferRow.insert("name", QStringLiteral("Device Buffer / Period"));
    bufferRow.insert("samples", snapshot.bufferSamples);
    bufferRow.insert("ms", toMs(snapshot.bufferSamples));
    bufferRow.insert("countInTotal", true);
    rows.push_back(bufferRow);

    QVariantMap driverRow;
    driverRow.insert("name", QStringLiteral("Backend / Hardware"));
    driverRow.insert("samples", snapshot.backendOutputSamples);
    driverRow.insert("ms", toMs(snapshot.backendOutputSamples));
    driverRow.insert("countInTotal", true);
    rows.push_back(driverRow);

    QVariantMap dspRow;
    dspRow.insert("name", QStringLiteral("DSP Latency"));
    dspRow.insert("samples", dspSamples);
    dspRow.insert("ms", toMs(dspSamples));
    dspRow.insert("countInTotal", false);
    rows.push_back(dspRow);

    QVariantMap rubberbandRow;
    rubberbandRow.insert("name", QStringLiteral("Keylock / Timestretch"));
    rubberbandRow.insert("samples", snapshot.keylockSamples);
    rubberbandRow.insert("ms", toMs(snapshot.keylockSamples));
    rubberbandRow.insert("countInTotal", true);
    rows.push_back(rubberbandRow);

    QVariantMap resamplerRow;
    resamplerRow.insert("name", QStringLiteral("Resampler"));
    resamplerRow.insert("samples", snapshot.resamplerSamples);
    resamplerRow.insert("ms", toMs(snapshot.resamplerSamples));
    resamplerRow.insert("countInTotal", true);
    rows.push_back(resamplerRow);

    QVariantMap limiterRow;
    limiterRow.insert("name", QStringLiteral("Limiter Lookahead"));
    limiterRow.insert("samples", snapshot.limiterSamples);
    limiterRow.insert("ms", toMs(snapshot.limiterSamples));
    limiterRow.insert("countInTotal", true);
    rows.push_back(limiterRow);

    QVariantMap fxRow;
    fxRow.insert("name", QStringLiteral("Mixer / FX Chain"));
    fxRow.insert("samples", snapshot.mixerFxSamples);
    fxRow.insert("ms", toMs(snapshot.mixerFxSamples));
    fxRow.insert("countInTotal", true);
    rows.push_back(fxRow);

    QVariantMap totalRow;
    totalRow.insert("name", QStringLiteral("Total Estimated Latency"));
    totalRow.insert("samples", audioDeviceSamples + dspSamples);
    totalRow.insert("ms", toMs(audioDeviceSamples + dspSamples));
    totalRow.insert("countInTotal", false);
    rows.push_back(totalRow);

    return rows;
}


QVariantMap DjEngine::audioPerformanceStats() const
{
    QVariantMap stats;
    const auto snapshot = buildLatencySnapshot();
    const double callbackBudgetUsec = snapshot.sampleRate > 0.0
        ? (static_cast<double>(snapshot.bufferSamples) / snapshot.sampleRate) * 1000000.0
        : 0.0;

    stats.insert(QStringLiteral("callbackAverageUsec"), DjMasterBus::callbackAverageUsec());
    stats.insert(QStringLiteral("callbackWorstUsec"), DjMasterBus::callbackWorstUsec());
    stats.insert(QStringLiteral("callbackBudgetUsec"), callbackBudgetUsec);
    stats.insert(QStringLiteral("callbackCount"),
                 QVariant::fromValue<qulonglong>(DjMasterBus::callbackCount()));
    stats.insert(QStringLiteral("callbackOverruns"),
                 QVariant::fromValue<qulonglong>(DjMasterBus::callbackOverrunCount()));
    stats.insert(QStringLiteral("sampleRate"), snapshot.sampleRate);
    stats.insert(QStringLiteral("bufferSamples"), snapshot.bufferSamples);

    QVariantList fxProfiles;
    for (int i = 1; i <= static_cast<int>(EffectType::RollOut); ++i) {
        const auto type = static_cast<EffectType>(i);
        const auto profile = FxProcessor::getCpuProfile(type);
        if (profile.count == 0)
            continue;

        QVariantMap row;
        row.insert(QStringLiteral("name"), QString::fromLatin1(FxProcessor::effectTypeName(type)));
        row.insert(QStringLiteral("averageUsec"),
                   static_cast<double>(profile.totalUsec) / static_cast<double>(profile.count));
        row.insert(QStringLiteral("worstUsec"), static_cast<double>(profile.worstUsec));
        row.insert(QStringLiteral("count"), QVariant::fromValue<qulonglong>(profile.count));
        fxProfiles.push_back(row);
    }
    stats.insert(QStringLiteral("fxProfiles"), fxProfiles);
    return stats;
}
