#include "stems/StemInference.h"
#include "stems/Utf8Path.h"

#if BROCKDJ_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
#endif

namespace brockdj::stems {

#if BROCKDJ_HAS_ONNXRUNTIME
namespace {

// Lower background priority to reduce contention with the audio callback and UI.
void lowerCurrentThreadPriority() noexcept
{
#if defined(__linux__)
    (void) setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#elif defined(_WIN32)
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    (void) pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
}

OrtCustomThreadHandle createLowPriorityThread(void*, OrtThreadWorkerFn fn, void* param)
{
    try {
        auto* thread = new std::thread([fn, param] {
            lowerCurrentThreadPriority();
            fn(param);
        });
        return reinterpret_cast<OrtCustomThreadHandle>(thread);
    } catch (const std::bad_alloc&) {
        std::fputs("[stems] Cannot allocate an inference thread\n", stderr);
    } catch (const std::system_error& error) {
        std::fprintf(stderr, "[stems] Cannot start an inference thread: %s\n", error.what());
    }
    return nullptr;
}

void joinLowPriorityThread(OrtCustomThreadHandle handle)
{
    auto* thread = reinterpret_cast<std::thread*>(const_cast<OrtCustomHandleType*>(handle));
    if (thread->joinable())
        thread->join();
    delete thread;
}

class OnnxStemInference final : public StemInference
{
public:
    OnnxStemInference(const std::string& modelPath, int threads, const StemInferenceOptions& config)
        : m_env(ORT_LOGGING_LEVEL_ERROR, "BrockDJ-stems")
        , m_modelPath(modelPath)
        , m_threads(threads)
    {
        auto options = cpuOptions();
        if (config.preferGpu) {
            try {
                configureGpu(options, config);
            } catch (const Ort::Exception& error) {
                warn(std::string("GPU initialization failed: ") + error.what());
                m_backend = "CPU";
                options = cpuOptions();
            }
        }
        try {
            m_session = createSession(options);
        } catch (const Ort::Exception& error) {
            if (m_backend == "CPU")
                throw;
            fallBackToCpu(std::string("GPU model initialization failed: ") + error.what());
        }
    }

    std::string backend() const override { return m_backend; }
    std::string acceleratorWarning() const override { return m_warning; }

private:
    Ort::SessionOptions cpuOptions() const
    {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(m_threads);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.DisableCpuMemArena();
        options.DisableMemPattern();
        // The model has very large foldable constants. Bound the optimizer's
        // temporary allocations; older runtimes can only disable folding.
#if ORT_API_VERSION >= 27
        options.AddConfigEntry("optimization.constant_folding_max_output_size_in_bytes", "1048576");
#else
        options.AddConfigEntry("optimization.disable_specified_optimizers", "ConstantFolding");
#endif
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
        options.SetCustomCreateThreadFn(createLowPriorityThread);
        options.SetCustomJoinThreadFn(joinLowPriorityThread);
        return options;
    }

    std::unique_ptr<Ort::Session> createSession(const Ort::SessionOptions& options)
    {
        const auto nativePath = pathFromUtf8(m_modelPath);
        return std::make_unique<Ort::Session>(m_env, nativePath.c_str(), options);
    }

    void warn(std::string message)
    {
        m_warning = std::move(message);
        std::fprintf(stderr, "[stems] %s; using CPU\n", m_warning.c_str());
    }

    void configureGpu(Ort::SessionOptions& options, const StemInferenceOptions& config)
    {
#if defined(__APPLE__)
        (void) config;
        const auto providers = Ort::GetAvailableProviders();
        if (std::find(providers.begin(), providers.end(), "CoreMLExecutionProvider") == providers.end()) {
            warn("CoreML is unavailable in this ONNX Runtime");
            return;
        }
        options.AppendExecutionProvider("CoreML", {
            {"ModelFormat", "MLProgram"}, {"MLComputeUnits", "CPUAndGPU"},
            {"RequireStaticInputShapes", "1"}
        });
        m_backend = "CoreML CPU/GPU";
#elif ORT_API_VERSION >= 23 && (defined(__linux__) || defined(_WIN32))
        if (config.openVinoLibraryPath.empty()) {
            warn("OpenVINO GPU plugin is not installed (see stems setup instructions)");
            return;
        }
        const auto nativePath = pathFromUtf8(config.openVinoLibraryPath);
        m_env.RegisterExecutionProviderLibrary("OpenVINOExecutionProvider", nativePath.c_str());
        for (const auto& device : m_env.GetEpDevices()) {
            if (std::string_view(device.EpName()) != "OpenVINOExecutionProvider"
                || device.Device().Type() != OrtHardwareDeviceType_GPU)
                continue;
            options.AppendExecutionProvider_V2(m_env, { device },
                std::unordered_map<std::string, std::string> {
                    {"load_config", R"({"GPU":{"NUM_STREAMS":"1"}})"}
                });
            m_backend = "OpenVINO Intel GPU";
            return;
        }
        warn("No Intel GPU compute device found; install the Intel OpenCL compute driver");
#else
        (void) options;
        (void) config;
        warn("GPU stems are unavailable on this platform or ONNX Runtime version");
#endif
    }

    void fallBackToCpu(std::string message)
    {
        warn(std::move(message));
        m_backend = "CPU";
        m_session.reset();
        m_session = createSession(cpuOptions());
    }

public:
    bool separate(const float* left, const float* right, float* output, std::string& error) override
    {
        bool cancelled = false;
        try {
            std::copy_n(left, kWindow, m_input.begin());
            std::copy_n(right, kWindow, m_input.begin() + kWindow);
            const std::array<std::int64_t, 3> inputShape { 1, 2, kWindow };
            const std::array<std::int64_t, 4> outputShape { 1, kModelStems, 2, kWindow };
            auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value input = Ort::Value::CreateTensor<float>(
                memory, m_input.data(), m_input.size(), inputShape.data(), inputShape.size());
            Ort::Value out = Ort::Value::CreateTensor<float>(
                memory, output, static_cast<std::size_t>(kModelStems * 2 * kWindow),
                outputShape.data(), outputShape.size());
            const char* inputNames[] { "mix" };
            const char* outputNames[] { "stems" };
            Ort::RunOptions runOptions;
            {
                std::lock_guard lock(m_runMutex);
                if (m_abortRequested) {
                    m_abortRequested = false;
                    error = "aborted";
                    return false;
                }
                m_activeRun = &runOptions;
            }
            struct ClearActive {
                OnnxStemInference& self;
                bool& cancelled;
                ~ClearActive()
                {
                    std::lock_guard lock(self.m_runMutex);
                    cancelled = self.m_abortRequested;
                    self.m_activeRun = nullptr;
                    self.m_abortRequested = false;
                }
            } clearActive { *this, cancelled };
            m_session->Run(runOptions, inputNames, &input, 1, outputNames, &out, 1);
            if (!std::all_of(output, output + kModelStems * 2 * kWindow,
                             [](float value) { return std::isfinite(value); }))
                throw Ort::Exception("Stem model returned non-finite audio samples", ORT_FAIL);
            return true;
        } catch (const Ort::Exception& exception) {
            if (!cancelled && m_backend != "CPU") {
                try {
                    fallBackToCpu(std::string("GPU inference failed: ") + exception.what());
                    return separate(left, right, output, error);
                } catch (const Ort::Exception& cpuError) {
                    error = cpuError.what();
                    return false;
                }
            }
            error = exception.what();
            return false;
        }
    }

    void abort() noexcept override
    {
        std::lock_guard lock(m_runMutex);
        m_abortRequested = true;
        if (m_activeRun) {
            try {
                m_activeRun->SetTerminate();
            } catch (const Ort::Exception& error) {
                std::fprintf(stderr, "[stems] Cannot abort inference: %s\n", error.what());
            }
        }
    }

private:
    Ort::Env m_env;
    std::string m_modelPath;
    int m_threads;
    std::string m_backend = "CPU";
    std::string m_warning;
    std::unique_ptr<Ort::Session> m_session;
    std::vector<float> m_input = std::vector<float>(static_cast<std::size_t>(2 * kWindow));
    std::mutex m_runMutex;
    Ort::RunOptions* m_activeRun = nullptr;
    bool m_abortRequested = false;
};

} // namespace
#endif

bool onnxStemInferenceAvailable() noexcept
{
    return BROCKDJ_HAS_ONNXRUNTIME != 0;
}

std::unique_ptr<StemInference> createOnnxStemInference(const std::string& modelPath, int threads,
                                                       std::string& error,
                                                       const StemInferenceOptions& options)
{
#if BROCKDJ_HAS_ONNXRUNTIME
    try {
        lowerCurrentThreadPriority();
        return std::make_unique<OnnxStemInference>(modelPath, std::max(1, threads), options);
    } catch (const std::exception& exception) {
        error = exception.what();
        return nullptr;
    }
#else
    (void) modelPath;
    (void) threads;
    (void) options;
    error = "This build has no ONNX Runtime (run scripts/fetch-onnxruntime.sh and reconfigure)";
    return nullptr;
#endif
}

} // namespace brockdj::stems
