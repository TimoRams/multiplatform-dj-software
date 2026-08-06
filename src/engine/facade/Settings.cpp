#include "FacadeIncludes.h"
#include "audio/device/AudioDeviceService.h"

bool DjEngine::applyAudioDeviceSettings(int sampleRate, int bufferSize)
{
    return m_audioDeviceService.applySettings(sampleRate, bufferSize);
}

bool DjEngine::applyAudioDeviceSettings(const QString& type, const QString& output,
                                        int sampleRate, int bufferSize,
                                        int masterFirstChannel, int headphonesFirstChannel,
                                        int boothFirstChannel)
{
    return m_audioDeviceService.applySettings(type, output, sampleRate, bufferSize,
                                              masterFirstChannel, headphonesFirstChannel,
                                              boothFirstChannel);
}

void DjEngine::setOutputFirstChannel(int firstChannel)
{
    m_audioDeviceService.setOutputFirstChannel(firstChannel);
}

void DjEngine::setMasterVolume(float value) { AudioEngine::setMasterVolume(value); }
void DjEngine::setAntiClip(bool enabled) { AudioEngine::setAntiClipEnabled(enabled); }

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
