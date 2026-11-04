#include "audio/SetRecorder.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QtEndian>
#include <chrono>
#include <thread>
#include <iostream>
#include <cmath>

bool check(bool condition, const char* label) {
    if (!condition) std::cerr << "FAIL: " << label << '\n';
    return condition;
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    SetRecorder recorder;
    bool ok = check(!recorder.startRecording(directory.path()), "an unprepared audio engine cannot record");
    recorder.configure(48000);
    juce::AudioBuffer<float> tone(2, 512);
    for (int i = 0; i < 512; ++i) { tone.setSample(0, i, 0.25f); tone.setSample(1, i, -0.5f); }
    ok &= check(recorder.startRecording(directory.path()), "a prepared recorder starts on writable storage");
    for (int i = 0; i < 8; ++i) recorder.capture(tone, 512);
    for (int i = 0; i < 200 && recorder.elapsedSeconds() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    recorder.markTrack();
    recorder.capture(tone, 512);
    recorder.stopRecording();
    ok &= check(recorder.error().isEmpty() && !recorder.recording(), "stop finalizes without a disk error");
    ok &= check(std::abs(recorder.elapsedSeconds() - 4608.0 / 48000) < 1e-6, "all accepted frames are drained on stop");
    const auto files = QDir(directory.path()).entryList({"*.wav"}, QDir::Files);
    ok &= check(files.size() == 2, "track mark creates a new WAV part");
    qint64 total = 0;
    for (const auto& file : files) {
        QFile wav(QDir(directory.path()).filePath(file));
        ok &= check(wav.open(QIODevice::ReadOnly), "recording can be opened");
        const auto bytes = wav.readAll();
        ok &= check(bytes.left(4) == "RIFF" && bytes.mid(8, 4) == "WAVE"
            && qFromLittleEndian<quint16>(bytes.constData() + 34) == 24
            && qFromLittleEndian<quint32>(bytes.constData() + 24) == 48000
            && qFromLittleEndian<quint32>(bytes.constData() + 40) == bytes.size() - 44,
            "each part has a valid 24-bit 48 kHz WAV header matching its data size");
        total += (bytes.size() - 44) / 6;
        if (bytes.size() >= 50) {
            const auto sample = [&](int offset) {
                const auto* p = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
                int v = p[0] | p[1] << 8 | p[2] << 16;
                if (v & 0x800000) v -= 0x1000000;
                return v / 8388607.0;
            };
            ok &= check(std::abs(sample(44) - 0.25) < 1e-5 && std::abs(sample(47) + 0.5) < 1e-5,
                        "PCM samples retain stereo identity and amplitude");
        }
    }
    ok &= check(total == 4608, "splitting loses or duplicates no frames");
    ok &= check(recorder.startRecording(directory.path()), "recorder can restart after stop");
    recorder.configure(44100);
    recorder.capture(tone, 512);
    recorder.stopRecording();
    ok &= check(recorder.error().contains("sample rate"), "device rate change stops rather than corrupting a WAV");
    recorder.configure(48000);
    recorder.setDirectory(directory.filePath("SharedRecordingFolder"));
    recorder.toggleRecording();
    ok &= check(recorder.recording() && recorder.filePath().contains("SharedRecordingFolder"), "toggle uses shared recording directory");
    recorder.capture(tone, 512);
    const auto startStop = std::chrono::steady_clock::now();
    recorder.requestStopRecording();
    ok &= check(std::chrono::steady_clock::now() - startStop < std::chrono::milliseconds(30), "UI stop requests never join the disk writer");
    for (int i = 0; i < 200 && recorder.recording(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ok &= check(!recorder.recording() && recorder.error().isEmpty(), "asynchronous stop drains and completes");
    recorder.stopRecording();
    ok &= check(recorder.startRecording(directory.path()), "recorder recovers from a device change");
    recorder.capture(tone, (1 << 19) + 1);
    recorder.stopRecording();
    ok &= check(recorder.error().contains("keep up"), "bounded ring overflow stops recording without blocking playback");
    QFile blocker(directory.filePath("file")); blocker.open(QIODevice::WriteOnly); blocker.close();
    ok &= check(!recorder.startRecording(blocker.fileName() + "/child"), "invalid storage rejects start");
    return ok ? 0 : 1;
}
