#include "DjEngineCommonIncludes.h"
#include "audio/device/AudioDeviceService.h"

QStringList DjEngine::getAvailableAudioDeviceTypes() const
{
    return m_audioDeviceService.availableDeviceTypes();
}

QStringList DjEngine::getAvailableAudioOutputDevices(const QString& type) const
{
    return m_audioDeviceService.availableOutputDevices(type);
}

QStringList DjEngine::getAvailableOutputChannelPairs(const QString& type,
                                                     const QString& output) const
{
    return m_audioDeviceService.availableOutputChannelPairs(type, output);
}

QString DjEngine::getCurrentAudioDeviceType() const { return m_audioDeviceService.currentDeviceType(); }
QString DjEngine::getCurrentAudioOutputDevice() const { return m_audioDeviceService.currentOutputDevice(); }
int DjEngine::getCurrentAudioSampleRate() const { return m_audioDeviceService.currentSampleRate(); }
int DjEngine::getCurrentAudioBufferSize() const { return m_audioDeviceService.currentBufferSize(); }
bool DjEngine::isJackServerRunning() const { return m_audioDeviceService.isJackServerRunning(); }
QString DjEngine::jackServerStatus() const { return m_audioDeviceService.jackServerStatus(); }
QString DjEngine::lastAudioDeviceError() const { return m_audioDeviceService.lastError(); }
QString DjEngine::audioDeviceFallbackMessage() const { return m_audioDeviceService.fallbackMessage(); }
