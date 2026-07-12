#include "DjEngineCommonIncludes.h"
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

void DjEngine::setMasterVolume(float value) { DjMasterBus::setMasterVolume(value); }
void DjEngine::setAntiClip(bool enabled) { DjMasterBus::setAntiClipEnabled(enabled); }
