#include "SeparationEngine.h"
#include <regex>

// CUDA BACKEND
// ------------
// demucs-cuda.exe is built from the same demucs-rs-fork source as
// demucs.exe/demucs-gpu.exe, just with burn's native CUDA backend (feature
// "cuda", type B = burn::backend::Cuda, still f32 throughout - no precision
// change) instead of the generic wgpu/Vulkan compute-shader path. Measured
// ~4x faster than the wgpu GPU build on this machine (RTX 2050): 66.7s vs
// 155.2s for the same 240s clip, steady-state.
//
// We first tried fp16 on the wgpu backend for a similar speedup (smaller
// code change - just the backend's float element type). That produced 100%
// NaN output: htdemucs's 5 transformer layers (self+cross-attention,
// LayerNorm/GroupNorm) are numerically unstable in naive full-network fp16 -
// a single overflow anywhere gets broadcast through every subsequent
// normalization layer. Real mixed precision (matmuls in fp16, softmax/norms
// forced back to f32) would fix this but means surgery inside demucs-core's
// model code, not a backend swap. CUDA was the safer lever: still f32
// throughout, just native kernels instead of generic compute shaders.
//
// NOT YET BUNDLED FOR END USERS: demucs-cuda.exe resolves its CUDA install
// via NVRTC, which needs cuda_runtime.h (~30MB, from the Toolkit's include/
// dir) and nvrtc64_*.dll (~90MB) + nvrtc-builtins64_*.dll (~4MB) + a working
// cudart64_*.dll, none of which are on a typical end user's machine unless
// they've installed the CUDA Toolkit themselves (as this dev machine now
// has). resolveCudaEnvironment() below probes for a system install and the
// engine silently falls back to the wgpu GPU build if none is found - so
// this is safe to ship as-is (matches today's GPU-then-CPU fallback
// behavior for anyone without CUDA installed), but doesn't get anyone the
// CUDA speedup unless they have the Toolkit already. Bundling the ~125MB of
// headers + DLLs above (mirroring how htdemucs.safetensors is bundled rather
// than downloaded - see MODEL WEIGHTS below) would make the CUDA path work
// out of the box for NVIDIA users; deferred as a separate decision given the
// installer-size increase and NVIDIA redistributable licensing to check.
namespace
{
    // Base htdemucs (not htdemucs_ft): traded the ft ensemble's better separation
    // quality/less bleed for 4x the speed - see SeparationEngine.h's MODEL note.
    // htdemucs_ft.safetensors is left in Source/Separation/bin (unused by default)
    // in case we switch back.
    constexpr const char* modelFileName = "htdemucs.safetensors";
    constexpr const char* ftModelFileName = "htdemucs_ft.safetensors";
   #if JUCE_WINDOWS
    constexpr const char* cpuExeName = "demucs.exe";
    constexpr const char* gpuExeName = "demucs-gpu.exe";
    constexpr const char* cudaExeName = "demucs-cuda.exe";
   #else
    // macOS/Linux builds ship unextensioned companion binaries built from
    // demucs-rs-fork's demucs-cli (Metal-accelerated by default on macOS -
    // see that repo's README). No CUDA build exists off Windows.
    constexpr const char* cpuExeName = "demucs";
    constexpr const char* gpuExeName = "demucs-gpu";
    constexpr const char* cudaExeName = "demucs-cuda";
   #endif

    juce::File getCacheDir()
    {
       #if JUCE_WINDOWS
        auto base = juce::File (juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {}));
        if (! base.isDirectory())
            base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        return base.getChildFile ("demucs-rs");
       #else
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                 .getChildFile (".cache").getChildFile ("demucs-rs");
       #endif
    }

    // Finds the last "PROGRESS:N/M" line our own fork of demucs-cli/demucs-cli-gpu
    // prints (see progress.rs in the demucs-rs source this project vendors
    // pre-built binaries of) - a plain println! alongside indicatif's own
    // (terminal-only, and in practice never actually emitted by these binaries -
    // see SeparationEngine.h's PROGRESS comment) bar, specifically so a piped
    // reader like this one gets a real signal regardless of terminal attachment.
    bool findLastProgressFraction (const juce::String& text, double& outFraction)
    {
        std::string s = text.toStdString();
        static const std::regex re (R"(PROGRESS:(\d+)\s*/\s*(\d+))");
        auto begin = std::sregex_iterator (s.begin(), s.end(), re);
        auto end   = std::sregex_iterator();
        if (begin == end)
            return false;

        std::smatch last;
        for (auto it = begin; it != end; ++it)
            last = *it;

        auto pos = std::stod (last[1].str());
        auto len = std::stod (last[2].str());
        if (len <= 0.0)
            return false;

        outFraction = juce::jlimit (0.0, 1.0, pos / len);
        return true;
    }
}

//==============================================================================
SeparationEngine::SeparationEngine() : juce::Thread ("DR-VoxSplit Separation")
{
    formatManager.registerBasicFormats();
}

SeparationEngine::~SeparationEngine()
{
    cancel();
    stopThread (5000);
}

void SeparationEngine::prepare()
{
    engineReady = locateBinaries() && primeModelCache();
    resolveCudaEnvironment();
}

bool SeparationEngine::locateBinaries()
{
    pluginDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    demucsExe      = pluginDir.getChildFile (cpuExeName);
    gpuExe         = pluginDir.getChildFile (gpuExeName);
    cudaExe        = pluginDir.getChildFile (cudaExeName);
    bundledModel   = pluginDir.getChildFile (modelFileName);
    bundledFtModel = pluginDir.getChildFile (ftModelFileName);

    // htdemucs_ft (High Quality) is optional - its absence just means that
    // option isn't offered, not that the engine is unavailable.
    if (! bundledFtModel.existsAsFile())
    {
        DBG ("SeparationEngine: htdemucs_ft.safetensors not found next to plugin binary - High Quality option disabled ("
             << pluginDir.getFullPathName() << ")");
        bundledFtModel = juce::File();
    }

    // demucs-gpu.exe and demucs-cuda.exe are both optional - their absence
    // just means we never attempt that tier, not that the engine is
    // unavailable.
    if (! gpuExe.existsAsFile())
    {
        DBG ("SeparationEngine: demucs-gpu.exe not found next to plugin binary - GPU acceleration disabled ("
             << pluginDir.getFullPathName() << ")");
        gpuExe = juce::File();
    }

    if (! cudaExe.existsAsFile())
    {
        DBG ("SeparationEngine: demucs-cuda.exe not found next to plugin binary - CUDA acceleration disabled ("
             << pluginDir.getFullPathName() << ")");
        cudaExe = juce::File();
    }

    if (! demucsExe.existsAsFile())
    {
        DBG ("SeparationEngine: demucs.exe not found next to plugin binary (" << pluginDir.getFullPathName() << ")");
        return false;
    }

    if (! bundledModel.existsAsFile())
    {
        DBG ("SeparationEngine: bundled model not found next to plugin binary (" << pluginDir.getFullPathName() << ")");
        return false;
    }

    return true;
}

bool SeparationEngine::primeModelCache()
{
    auto cacheDir = getCacheDir();

    if (! cacheDir.isDirectory() && ! cacheDir.createDirectory())
    {
        DBG ("SeparationEngine: failed to create model cache dir " << cacheDir.getFullPathName());
        return false;
    }

    auto cachedModel = cacheDir.getChildFile (modelFileName);

    // Only copy if missing or a different size - avoids re-copying an 84MB
    // file on every plugin instantiation.
    if (! (cachedModel.existsAsFile() && cachedModel.getSize() == bundledModel.getSize()))
    {
        if (! bundledModel.copyFileTo (cachedModel))
        {
            DBG ("SeparationEngine: failed to prime model cache at " << cachedModel.getFullPathName());
            return false;
        }
    }

    // htdemucs_ft (High Quality) - optional, primed the same way so switching
    // Quality doesn't need a fresh copy/download mid-session.
    if (bundledFtModel.existsAsFile())
    {
        auto cachedFtModel = cacheDir.getChildFile (ftModelFileName);
        if (! (cachedFtModel.existsAsFile() && cachedFtModel.getSize() == bundledFtModel.getSize()))
            bundledFtModel.copyFileTo (cachedFtModel); // non-fatal if this fails - High Quality just won't be selectable-successfully; Fast still works
    }

    return true;
}

bool SeparationEngine::resolveCudaEnvironment()
{
    cudaEnvironmentResolved = false;
    cudaEnvironment.clear();

    if (cudaExe == juce::File())
        return false; // demucs-cuda.exe itself not bundled with this build

    auto cudaRoot = juce::File ("C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA");
    if (! cudaRoot.isDirectory())
        return false;

    // Pick the highest-numbered vX.Y install that actually has what we need
    // - a machine can accumulate multiple CUDA Toolkit versions over time,
    // and an older one left behind (partial uninstall, etc.) shouldn't win
    // over a newer working one.
    juce::File bestVersionDir;
    double bestVersion = -1.0;

    for (auto& child : juce::RangedDirectoryIterator (cudaRoot, false, "v*", juce::File::findDirectories))
    {
        auto dir = child.getFile();
        auto versionText = dir.getFileName().substring (1); // strip leading 'v'
        auto version = versionText.getDoubleValue();        // "13.2" -> 13.2; malformed names sort last via -1 default below

        if (! dir.getChildFile ("include").getChildFile ("cuda_runtime.h").existsAsFile())
            continue;

        auto binDir = dir.getChildFile ("bin").getChildFile ("x64");
        bool hasNvrtc = false;
        for (auto& dll : juce::RangedDirectoryIterator (binDir, false, "nvrtc64_*.dll", juce::File::findFiles))
        {
            juce::ignoreUnused (dll);
            hasNvrtc = true;
            break;
        }
        if (! hasNvrtc)
            continue;

        if (version > bestVersion)
        {
            bestVersion = version;
            bestVersionDir = dir;
        }
    }

    if (bestVersionDir == juce::File())
        return false;

    cudaEnvironment.set ("CUDA_PATH", bestVersionDir.getFullPathName());
    cudaEnvironment.set ("PATH", bestVersionDir.getChildFile ("bin").getChildFile ("x64").getFullPathName());
    cudaEnvironmentResolved = true;
    return true;
}

double SeparationEngine::effectiveWarnDurationSeconds() const noexcept
{
    if (backendPreference.load() == BackendPreference::forceCpu)
        return cpuWarnDurationSeconds;
    if (lastGoodBackend == Backend::cpu)
        return cpuWarnDurationSeconds;
    if (lastGoodBackend == Backend::gpu)
        return gpuWarnDurationSeconds;
    if (lastGoodBackend == Backend::cuda)
        return cudaWarnDurationSeconds;
    if (isCudaBuildPresent())
        return cudaWarnDurationSeconds;
    return isGpuBuildPresent() ? gpuWarnDurationSeconds : cpuWarnDurationSeconds;
}

double SeparationEngine::effectiveMaxDurationSeconds() const noexcept
{
    if (backendPreference.load() == BackendPreference::forceCpu)
        return cpuMaxDurationSeconds;
    if (lastGoodBackend == Backend::cpu)
        return cpuMaxDurationSeconds;
    if (lastGoodBackend == Backend::gpu)
        return gpuMaxDurationSeconds;
    if (lastGoodBackend == Backend::cuda)
        return cudaMaxDurationSeconds;
    if (isCudaBuildPresent())
        return cudaMaxDurationSeconds;
    return isGpuBuildPresent() ? gpuMaxDurationSeconds : cpuMaxDurationSeconds;
}

bool SeparationEngine::isSupportedExtension (const juce::String& extensionWithDot)
{
    static const juce::StringArray supportedExtensions { ".wav", ".wave", ".mp3", ".flac", ".aiff", ".aif", ".m4a", ".aac" };
    return supportedExtensions.contains (extensionWithDot.toLowerCase());
}

juce::String SeparationEngine::validateInputFile (const juce::File& file, double& outDurationSeconds)
{
    outDurationSeconds = 0.0;

    if (! isSupportedExtension (file.getFileExtension()))
        return "Unsupported file type \"" + file.getFileExtension() + "\". Supported formats: WAV, MP3, FLAC, AIFF, M4A.";

    if (! file.existsAsFile())
        return "File not found.";

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
        return "Couldn't read this file - it may be corrupt, DRM-protected, or an unsupported variant of its format.";

    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return "This file appears to be empty.";

    outDurationSeconds = (double) reader->lengthInSamples / reader->sampleRate;

    auto maxSeconds = effectiveMaxDurationSeconds();
    if (outDurationSeconds > maxSeconds)
        return "This file is " + juce::String (outDurationSeconds / 60.0, 1) + " minutes long, which exceeds the "
             + juce::String ((int) (maxSeconds / 60.0)) + "-minute limit for this plugin"
             + (isGpuBuildPresent() ? juce::String (".") : juce::String (" (running on CPU - no compatible GPU acceleration was found)."));

    return {};
}

void SeparationEngine::addListener (Listener* l)
{
    const juce::ScopedLock sl (listenerLock);
    listeners.addIfNotAlreadyThere (l);
}

void SeparationEngine::removeListener (Listener* l)
{
    const juce::ScopedLock sl (listenerLock);
    listeners.removeAllInstancesOf (l);
}

bool SeparationEngine::startSeparation (const juce::File& inputFile)
{
    if (isThreadRunning())
        return false;

    pendingInputFile = inputFile;
    startThread();
    return true;
}

void SeparationEngine::cancel()
{
    {
        const juce::ScopedLock sl (processLock);
        if (childProcess != nullptr)
            childProcess->kill();
    }
    signalThreadShouldExit();
}

void SeparationEngine::notifyProgress (float progress01, const juce::String& stage)
{
    juce::MessageManager::callAsync ([this, progress01, stage]
    {
        const juce::ScopedLock sl (listenerLock);
        for (auto* l : listeners)
            l->separationProgress (progress01, stage);
    });
}

void SeparationEngine::notifyComplete (const Result& result)
{
    juce::MessageManager::callAsync ([this, result]
    {
        const juce::ScopedLock sl (listenerLock);
        for (auto* l : listeners)
            l->separationComplete (result);
    });
}

void SeparationEngine::notifyFailed (const juce::String& error)
{
    juce::MessageManager::callAsync ([this, error]
    {
        const juce::ScopedLock sl (listenerLock);
        for (auto* l : listeners)
            l->separationFailed (error);
    });
}

bool SeparationEngine::runOnce (const juce::File& exe, const juce::File& inputFile, double durationSeconds,
                                 double secondsPerSecondOfAudioEstimate, juce::String& outTailOutput,
                                 const juce::StringPairArray& extraEnvironment)
{
    // Fresh output dir per attempt: if a GPU attempt partially writes a stem
    // before failing, we don't want the CPU retry to see stale leftovers and
    // think it succeeded.
    runOutputDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("DR-VoxSplit")
                     .getChildFile (juce::Uuid().toString());
    runOutputDir.createDirectory();

    juce::StringArray args;
    args.add (inputFile.getFullPathName());
    args.add ("--model");
    args.add (quality.load() == Quality::highQuality && bundledFtModel.existsAsFile() ? "htdemucs_ft" : "htdemucs");
    args.add ("--output");
    args.add (runOutputDir.getFullPathName());

    {
        const juce::ScopedLock sl (processLock);
        childProcess = std::make_unique<NonBlockingChildProcess>();
    }

    if (! childProcess->start (exe, args, extraEnvironment))
    {
        outTailOutput = "Failed to launch " + exe.getFileName();
        const juce::ScopedLock sl (processLock);
        childProcess.reset();
        return false;
    }

    juce::String tailOutput;
    bool haveRealProgress = false;
    auto startTimeMs = juce::Time::getMillisecondCounterHiRes();
    auto estimatedTotalMs = juce::jmax (1000.0, durationSeconds * secondsPerSecondOfAudioEstimate * 1000.0);

    // NonBlockingChildProcess::readAvailable() is non-blocking (0 means "nothing to
    // read right now", not "process gone") - so unlike the old
    // juce::ChildProcess-based loop, we drive our own short polling interval
    // here rather than being at the mercy of however long the child stays
    // silent. That keeps progress ticks and cancellation checks responsive
    // even through long output-free stretches like model loading.
    char buffer[4096];
    for (;;)
    {
        if (threadShouldExit())
        {
            const juce::ScopedLock sl (processLock);
            if (childProcess != nullptr)
                childProcess->kill();
            outTailOutput = "Cancelled.";
            childProcess.reset();
            return false;
        }

        auto bytesRead = childProcess->readAvailable (buffer, sizeof (buffer));

        if (bytesRead > 0)
        {
            tailOutput += juce::String::fromUTF8 (buffer, bytesRead);
            if (tailOutput.length() > 4000)
                tailOutput = tailOutput.substring (tailOutput.length() - 4000);

            double fraction = 0.0;
            if (findLastProgressFraction (tailOutput, fraction))
            {
                haveRealProgress = true;
                notifyProgress ((float) fraction, "Separating...");
            }

            continue; // more may be buffered - keep draining before sleeping
        }

        if (! childProcess->isRunning())
            break; // confirmed no more output was left buffered (last read above returned 0)

        if (! haveRealProgress)
        {
            auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - startTimeMs;
            notifyProgress ((float) juce::jlimit (0.0, 0.95, elapsedMs / estimatedTotalMs), "Separating...");
        }

        juce::Thread::sleep (15);
    }

    childProcess->waitForProcessToFinish (2000);
    auto exitCode = childProcess->getExitCode();

    {
        const juce::ScopedLock sl (processLock);
        childProcess.reset();
    }

    outTailOutput = tailOutput;

    if (exitCode != 0)
        return false;

    return runOutputDir.getChildFile ("vocals.wav").existsAsFile();
}

void SeparationEngine::run()
{
    if (! engineReady)
    {
        notifyFailed ("Separation engine isn't available (demucs.exe or the bundled model is missing from the plugin install).");
        return;
    }

    auto inputFile = pendingInputFile;

    double durationSeconds = 0.0;
    auto validationError = validateInputFile (inputFile, durationSeconds);
    if (validationError.isNotEmpty())
    {
        notifyFailed (validationError);
        return;
    }

    juce::String tailOutput;
    Backend usedBackend = Backend::unknown;
    bool succeeded = false;

    auto preference = backendPreference.load();
    const bool allowGpuTiers = (preference != BackendPreference::forceCpu);
    const bool allowCpuTier  = (preference != BackendPreference::forceGpu);
    // Forcing GPU explicitly overrides the "already proved CPU-only this
    // session" heuristic below - the user asking again is reason enough to
    // retry, e.g. after fixing a driver.
    const bool cpuOnlyProven = (preference == BackendPreference::automatic) && (lastGoodBackend == Backend::cpu);

    // Try CUDA first unless we already proved it broken this session (still
    // present on disk, but not worth re-attempting every run once we know
    // we'll end up on gpu or cpu anyway).
    if (allowGpuTiers && isCudaBuildPresent() && lastGoodBackend != Backend::gpu && ! cpuOnlyProven)
    {
        notifyProgress (-1.0f, "Loading model (CUDA)...");
        succeeded = runOnce (cudaExe, inputFile, durationSeconds, cudaEstimatedSecondsPerSecondOfAudio, tailOutput, cudaEnvironment);
        if (succeeded)
        {
            usedBackend = Backend::cuda;
        }
        else if (threadShouldExit())
        {
            notifyFailed ("Cancelled.");
            return;
        }
        else
        {
            notifyProgress (-1.0f, "CUDA acceleration unavailable - falling back...");
        }
    }

    // Then GPU (wgpu/Vulkan) unless we already proved it broken this session.
    if (allowGpuTiers && ! succeeded && gpuExe != juce::File() && ! cpuOnlyProven)
    {
        notifyProgress (-1.0f, "Loading model (GPU)...");
        succeeded = runOnce (gpuExe, inputFile, durationSeconds, gpuEstimatedSecondsPerSecondOfAudio, tailOutput);
        if (succeeded)
        {
            usedBackend = Backend::gpu;
        }
        else if (threadShouldExit())
        {
            notifyFailed ("Cancelled.");
            return;
        }
        else
        {
            notifyProgress (-1.0f, allowCpuTier
                ? "GPU acceleration unavailable - falling back to CPU (this will take longer)..."
                : "GPU acceleration unavailable.");
        }
    }

    if (! succeeded && allowCpuTier)
    {
        notifyProgress (-1.0f, "Loading model (CPU)...");
        succeeded = runOnce (demucsExe, inputFile, durationSeconds, cpuEstimatedSecondsPerSecondOfAudio, tailOutput);
        if (succeeded)
            usedBackend = Backend::cpu;
    }

    if (! succeeded && ! allowCpuTier)
    {
        notifyFailed ("GPU acceleration isn't available on this machine (no working CUDA/GPU build found or the driver rejected it) "
                      "- switch Processing Device to Auto or CPU to run this separation.");
        return;
    }

    if (threadShouldExit())
    {
        notifyFailed ("Cancelled.");
        return;
    }

    lastGoodBackend = usedBackend != Backend::unknown ? usedBackend : lastGoodBackend;

    if (! succeeded)
    {
        auto trimmed = tailOutput.trim();
        auto lastLine = trimmed.isEmpty() ? juce::String ("Unknown error.")
                                           : trimmed.substring (juce::jmax (0, trimmed.length() - 300));
        notifyFailed ("Separation failed: " + lastLine);
        return;
    }

    auto vocalsFile = runOutputDir.getChildFile ("vocals.wav");
    std::unique_ptr<juce::AudioFormatReader> vocalsReader (formatManager.createReaderFor (vocalsFile));
    if (vocalsReader == nullptr)
    {
        notifyFailed ("Separation finished but the vocals output couldn't be read back.");
        return;
    }

    const int numSamples = (int) vocalsReader->lengthInSamples;
    const int numChannels = (int) vocalsReader->numChannels;
    const double sampleRate = vocalsReader->sampleRate;

    juce::AudioBuffer<float> instrumentalBuffer (numChannels, numSamples);
    instrumentalBuffer.clear();

    for (auto* stemName : { "drums.wav", "bass.wav", "other.wav" })
    {
        auto stemFile = runOutputDir.getChildFile (stemName);
        if (! stemFile.existsAsFile())
            continue;

        std::unique_ptr<juce::AudioFormatReader> stemReader (formatManager.createReaderFor (stemFile));
        if (stemReader == nullptr)
            continue;

        juce::AudioBuffer<float> stemBuffer ((int) stemReader->numChannels, (int) stemReader->lengthInSamples);
        stemReader->read (&stemBuffer, 0, (int) stemReader->lengthInSamples, 0, true, true);

        const int channelsToAdd = juce::jmin (numChannels, stemBuffer.getNumChannels());
        const int samplesToAdd  = juce::jmin (numSamples, stemBuffer.getNumSamples());
        for (int ch = 0; ch < channelsToAdd; ++ch)
            instrumentalBuffer.addFrom (ch, 0, stemBuffer, ch, 0, samplesToAdd);
    }

    auto instrumentalFile = runOutputDir.getChildFile ("instrumental.wav");
    {
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::FileOutputStream> outStream (instrumentalFile.createOutputStream());
        if (outStream != nullptr)
        {
            // 32 (not 24): demucs.exe's own stems are 32-bit IEEE float (confirmed by
            // inspecting its output directly) - matching that here means our derived
            // instrumental mix carries the same bit depth as vocals.wav rather than
            // truncating to 24-bit int, the only such precision loss found in the pipeline.
            std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (
                outStream.get(), sampleRate, (unsigned int) numChannels, 32, {}, 0));
            if (writer != nullptr)
            {
                outStream.release(); // writer now owns the stream
                writer->writeFromAudioSampleBuffer (instrumentalBuffer, 0, numSamples);
            }
        }
    }

    Result result;
    result.vocalsFile = vocalsFile;
    result.instrumentalFile = instrumentalFile;
    result.sampleRate = sampleRate;
    result.durationSeconds = (double) numSamples / sampleRate;
    result.backendUsed = usedBackend;

    notifyProgress (1.0f, "Done");
    notifyComplete (result);
}
