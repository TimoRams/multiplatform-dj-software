#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace brockdj::stems {

// One HTDemucs inference window: planar stereo in, four stems out.
class StemInference
{
public:
    static constexpr std::int64_t kWindow = 343980; // 7.8 s at 44.1 kHz
    static constexpr double kSampleRate = 44100.0;
    // Model output order.
    enum ModelStem : int { Drums = 0, Bass = 1, Other = 2, Vocals = 3 };
    static constexpr int kModelStems = 4;

    virtual ~StemInference() = default;
    // output layout: [modelStem][channel][kWindow]
    virtual bool separate(const float* left, const float* right, float* output, std::string& error) = 0;
    // Thread-safe: makes a running separate() return early (false).
    virtual void abort() noexcept {}
    virtual std::string backend() const { return "CPU"; }
    virtual std::string acceleratorWarning() const { return {}; }
};

using StemInferenceFactory = std::function<std::unique_ptr<StemInference>(std::string& error)>;

struct StemInferenceOptions {
    bool preferGpu = false;
    std::string openVinoLibraryPath;
};

// Whether this build links ONNX Runtime.
[[nodiscard]] bool onnxStemInferenceAvailable() noexcept;
// Creates the ONNX Runtime HTDemucs backend, or nullptr with `error` set.
[[nodiscard]] std::unique_ptr<StemInference> createOnnxStemInference(const std::string& modelPath,
                                                                     int threads,
                                                                     std::string& error,
                                                                     const StemInferenceOptions& options = {});

} // namespace brockdj::stems
