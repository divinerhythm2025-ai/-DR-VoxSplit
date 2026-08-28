/*
  ==============================================================================
    SeparationEngine.h

    Drives demucs-rs's own CLI binaries (vendored as demucs-gpu.exe / demucs.exe
    - see Source/Separation/bin/) to do 2-stem (vocals / instrumental) source
    separation on a background thread.

    ARCHITECTURE
    ------------
    demucs-rs (github.com/nikhilunni/demucs-rs) has no C API / cdylib surface
    to link against the way DeepFilterNet3's libDF did (see DR Prism's
    DeepFilterEngine for that pattern) - its own "VST3/CLAP" plugin is a
    self-contained Rust binary built with nih-plug, not a library meant to be
    embedded in a foreign C++ host. Its demucs-cli crate, however, already
    does everything we need (format decoding via symphonia, resampling,
    STFT chunking/overlap-add, model inference) and writes plain WAV files to
    disk - so this engine drives it as a subprocess via juce::ChildProcess
    rather than linking Rust code in-process.

    demucs-cli only ships a 4-stem model (drums/bass/other/vocals) - there is
    no "instrumental" stem. We ask it for all four, keep vocals.wav as-is, and
    sum drums+bass+other into our own instrumental buffer.

    GPU ACCELERATION
    -----------------
    The ML backend demucs-core is built on (burn, github.com/tracel-ai/burn)
    has no DirectML backend - that isn't an oversight in our build, it simply
    doesn't exist as an option (confirmed against burn's own backend list).
    What it does have is a wgpu-based backend that can target Vulkan, which
    - like DirectML - runs across NVIDIA/AMD/Intel GPUs on Windows rather than
    being locked to one vendor the way CUDA is, so it serves the same "broad
    hardware compatibility" goal. We build two separate demucs-cli binaries
    from the same source (see the demucs-cli-gpu crate added alongside
    demucs-cli, differing only in Cargo.toml feature flags): demucs-gpu.exe
    (burn's wgpu/Vulkan backend) and demucs.exe (burn's ndarray/CPU backend,
    the one already proven working). There is no single-binary runtime switch
    in demucs-cli's own code (the backend type is chosen at compile time via
    a Cargo feature) - runOnce()/run() implement the GPU-then-CPU-fallback
    logic on our side instead, one process attempt at a time.

    Measured on this machine (one process each, same 12.0s clip):
      - CPU (demucs.exe, ndarray):        3m21s  (~16.8x realtime)
      - GPU (demucs-gpu.exe, wgpu/Vulkan): ~23s  (~1.9x realtime)
    ~8.5x faster. The GPU figure above was an early, since-superseded estimate -
    see gpuEstimatedSecondsPerSecondOfAudio below (near the bottom of this
    class) for a more accurate, directly-remeasured number (~1.2x). CPU is
    unchanged. See gpuEstimatedSecondsPerSecondOfAudio /
    cpuEstimatedSecondsPerSecondOfAudio below and warn/max*Seconds.

    FALLBACK
    --------
    If demucs-gpu.exe is present, a run tries it first. If that process exits
    non-zero or produces no output (no compatible GPU, driver too old, no
    Vulkan ICD installed, etc.) we transparently retry the same input on
    demucs.exe (CPU) and tell the listener via a progress message, rather than
    failing outright. Which backend last succeeded is cached for the engine's
    lifetime (lastKnownGoodBackend) so we don't re-attempt a GPU path that's
    already been proven broken on every subsequent run - but every fresh
    engine instance (i.e. every fresh plugin instantiation) re-probes GPU
    availability by trying it again once.

    MODEL WEIGHTS
    --------------
    demucs-cli has no flag to point at a local model file - it always checks
    its own cache dir (%LOCALAPPDATA%/demucs-rs/<file>.safetensors on Windows)
    and downloads from Hugging Face on a miss. We bundle htdemucs.safetensors
    (~84MB) and copy it into that cache directory before ever invoking either
    CLI, so it always hits its own cache and never touches the network -
    matching the DeepFilterNet3 precedent of bundling model weights rather
    than relying on runtime auto-download. Both demucs-gpu.exe and demucs.exe
    share the same cache dir/file, so priming it once covers both.

    Base htdemucs (not htdemucs_ft) is the default as of this note: htdemucs_ft
    is an ensemble of 4 models individually fine-tuned per stem (drums/bass/
    other/vocals) rather than one shared model, giving noticeably better
    separation quality/less bleed, but at 4x the inference cost (one forward
    pass per stem model vs. one shared pass) and ~4x the bundled weight size
    (336MB vs 84MB). We tried htdemucs_ft and reverted to base htdemucs: the
    4x processing time wasn't worth the quality gain given current GPU speed.
    htdemucs_ft.safetensors is still bundled in Source/Separation/bin (just
    unused by default) in case that trade-off gets revisited - swapping
    modelFileName below and the "--model" arg in runOnce() back to
    "htdemucs_ft" is the whole change.

    KNOWN TRADE-OFF: reverting to base htdemucs reintroduces whatever bleed/
    mono-adjacent artifacts htdemucs_ft was specifically fixing. If that
    resurfaces during testing, it's this deliberate model choice, not a new
    bug in the separation pipeline.

    PROGRESS
    --------
    demucs-cli is capable of drawing an indicatif progress bar ("...
    {pos}/{len} ...") to stderr, but only when stderr is a real terminal -
    and even attached to a real terminal (confirmed with a Windows ConPTY/
    pseudo console, tested directly against demucs-gpu.exe on 6s and 60s
    clips, ~300s of real GPU inference), the vendored binaries never actually
    drew it - only the fixed eprintln! status lines ("Reading...", "Loading
    model...", "Separating...", "Done!"). So piping vs. a real terminal was
    never the fixable part of this.

    What actually gets us real progress: this project vendors its own fork
    of demucs-cli/demucs-cli-gpu (see progress.rs in the demucs-rs source),
    which prints a plain "PROGRESS:N/M" line via println! on every forward-
    pass step (see CliListener::on_event in that file) - independent of
    indicatif and independent of terminal attachment, so it survives a
    completely ordinary pipe. Confirmed directly: piped (no ConPTY, the exact
    way runOnce() below reads it), demucs-gpu.exe now prints PROGRESS:1/72
    through PROGRESS:72/72 in order. findLastProgressFraction() parses the
    last such line out of the captured output; once one has been seen
    (haveRealProgress), that becomes the reported progress instead of the
    time-based estimate.

    runOnce() reads the child through NonBlockingChildProcess (see
    NonBlockingChildProcess.h) rather than juce::ChildProcess, which fixes a
    separate, genuine responsiveness bug: juce::ChildProcess::
    readProcessOutput() on Windows blocks internally (polling with 1ms
    sleeps) until the child produces output or exits, so a caller's loop
    gets no chance to update progress or notice a cancellation request for
    however long the child stays silent - which, before the first PROGRESS
    line (model load / first-run GPU shader compilation), can be tens of
    seconds, and reads as a hang even though the child process isn't stuck.
    NonBlockingChildProcess::readAvailable() returns 0 immediately when
    there's nothing to read, so runOnce()'s loop stays in control of its own
    ~15ms polling interval throughout - ticking the time-based fallback and
    staying responsive to cancel() even during that silent stretch.
  ==============================================================================
*/

#pragma once

#include "NonBlockingChildProcess.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>

class SeparationEngine : private juce::Thread
{
public:
    SeparationEngine();
    ~SeparationEngine() override;

    /** Locates demucs.exe + the bundled model next to the plugin binary, and
        primes the CLI's own model cache directory from it. Safe to call
        repeatedly; cheap after the first call. */
    void prepare();

    /** False if demucs.exe (the required CPU fallback) / the bundled model
        couldn't be found or the cache couldn't be primed - startSeparation()
        will then fail immediately via Listener::separationFailed(). */
    bool isAvailable() const noexcept { return engineReady; }

    enum class Backend { unknown, cuda, gpu, cpu };

    /** Fast = base htdemucs (the original default - see MODEL WEIGHTS above).
        High quality = htdemucs_ft, the 4-model fine-tuned ensemble: noticeably
        less bleed, ~4x the inference cost. Both .safetensors files are bundled
        (see Source/Separation/bin/) and both get primed into demucs-cli's cache
        dir so switching is instant, no re-download/copy per switch. */
    enum class Quality { fast, highQuality };
    void setQuality (Quality q) noexcept { quality.store (q); }
    Quality getQuality() const noexcept { return quality.load(); }

    /** htdemucs_ft.safetensors ships alongside htdemucs.safetensors (see
        Source/Separation/bin/) so this should normally be true; exposed so the
        UI can grey out "High Quality" rather than assume. */
    bool isHighQualityModelAvailable() const noexcept { return bundledFtModel.existsAsFile(); }

    /** automatic = existing GPU(CUDA/wgpu)-then-CPU-fallback behaviour.
        forceCpu/forceGpu let the user override that instead of only ever
        getting the automatic choice. forceGpu tries CUDA then wgpu/Vulkan
        (whichever builds are present) but does NOT fall back to CPU - if
        neither GPU path is available/works, the run fails with a clear
        message rather than silently running on CPU anyway, since "force"
        should mean force. */
    enum class BackendPreference { automatic, forceCpu, forceGpu };
    void setBackendPreference (BackendPreference p) noexcept { backendPreference.store (p); }
    BackendPreference getBackendPreference() const noexcept { return backendPreference.load(); }

    /** demucs-gpu.exe found next to the plugin binary. Doesn't mean the GPU
        path will actually succeed at runtime (that depends on driver/Vulkan
        support) - just whether we'll attempt it. See lastKnownGoodBackend()
        for whether it has actually been proven to work this session. */
    bool isGpuBuildPresent() const noexcept { return gpuExe.existsAsFile(); }

    /** demucs-cuda.exe found next to the plugin binary AND a usable CUDA
        Toolkit install located on this machine (see
        resolveCudaEnvironment()). Like isGpuBuildPresent(), this only means
        we'll attempt the CUDA path, not that it's already been proven to
        work this session - NVIDIA-only, and currently requires the CUDA
        Toolkit's NVRTC + headers to already be installed system-wide (not
        yet bundled with the plugin - see the MODEL WEIGHTS-style note this
        would need before shipping to end users, in SeparationEngine.cpp). */
    bool isCudaBuildPresent() const noexcept { return cudaExe.existsAsFile() && cudaEnvironmentResolved; }

    /** Backend the most recent successful run actually used, or
        Backend::unknown before any run has completed. Used to pick which
        duration thresholds / time estimate apply. */
    Backend lastKnownGoodBackend() const noexcept { return lastGoodBackend; }

    //==============================================================================
    struct Result
    {
        juce::File vocalsFile;
        juce::File instrumentalFile;
        double sampleRate = 0.0;
        double durationSeconds = 0.0;
        Backend backendUsed = Backend::unknown;
    };

    struct Listener
    {
        virtual ~Listener() = default;

        /** progress01 in [0,1], or negative to indicate "indeterminate" (e.g.
            while the model is still loading and no chunk progress exists yet). */
        virtual void separationProgress (float progress01, const juce::String& stageMessage) = 0;
        virtual void separationComplete (const Result& result) = 0;
        virtual void separationFailed (const juce::String& errorMessage) = 0;
    };

    void addListener (Listener* l);
    void removeListener (Listener* l);

    //==============================================================================
    /** Duration thresholds used by the caller (editor) to decide whether to warn
        or refuse before calling startSeparation(). Not enforced here so the UI
        can show a confirmable warning rather than a hard failure.

        Three separate pairs because the three backends have such different
        throughput. Numbers below are for base htdemucs, our default model
        again (see the MODEL WEIGHTS note above) - measured on this machine:
        CPU (ndarray) ~16.8x realtime, GPU (wgpu/Vulkan) ~1.9x realtime on an
        early short clip, refined against a real 60s clip after reverting
        from htdemucs_ft (see gpuEstimatedSecondsPerSecondOfAudio above for
        which number is actually in use). While htdemucs_ft was the default
        these were divided by 4 to keep worst-case wall-clock time roughly
        constant across the 4x-slower model; now that we're back on base
        htdemucs they're restored to their original (pre-4x) values.
        cudaWarnDurationSeconds/cudaMaxDurationSeconds were added later
        (native CUDA backend via cudarc/cubecl-cuda, still f32 - see the CUDA
        BACKEND note in SeparationEngine.cpp), scaled up from the GPU pair by
        roughly the measured throughput ratio (~0.28s/s CUDA vs ~1.17s/s GPU
        steady-state on a real 240s clip on this machine, ~4x). Which pair
        applies depends on which backend is expected to succeed - see
        effectiveWarnDurationSeconds()/effectiveMaxDurationSeconds(), which
        pick based on isCudaBuildPresent(), isGpuBuildPresent() and
        lastKnownGoodBackend() (falls back to progressively more conservative
        numbers whenever a faster backend's success isn't yet established,
        e.g. before the first run of a session). */
    static constexpr double cudaWarnDurationSeconds = 60.0 * 60.0;
    static constexpr double cudaMaxDurationSeconds  = 180.0 * 60.0;
    static constexpr double gpuWarnDurationSeconds = 15.0 * 60.0;
    static constexpr double gpuMaxDurationSeconds  = 45.0 * 60.0;
    static constexpr double cpuWarnDurationSeconds = 2.0 * 60.0;
    static constexpr double cpuMaxDurationSeconds  = 10.0 * 60.0;

    /** Best current estimate of which threshold pair applies, given what we
        know so far this session (which builds are present/usable? has a
        backend already succeeded or already failed once?). */
    double effectiveWarnDurationSeconds() const noexcept;
    double effectiveMaxDurationSeconds() const noexcept;

    /** Quick pre-flight check the editor can use right after a file is chosen/
        dropped, before committing to a background run. Returns an empty string
        if the file looks separable, or a human-readable reason it isn't
        (unreadable / unsupported format, zero-length, exceeds
        effectiveMaxDurationSeconds()). */
    juce::String validateInputFile (const juce::File& file, double& outDurationSeconds);

    /** Extension-only check (no file I/O) - suitable for drag-hover feedback,
        where we can't afford to open the file yet. */
    static bool isSupportedExtension (const juce::String& extensionWithDot);

    /** Starts separation on the background thread. Only one run at a time;
        returns false (and does nothing) if a run is already in progress. */
    bool startSeparation (const juce::File& inputFile);

    bool isSeparating() const noexcept { return isThreadRunning(); }

    /** Requests cancellation of an in-progress run; the child process is killed
        and Listener::separationFailed() is called with a "cancelled" message. */
    void cancel();

private:
    void run() override;
    bool locateBinaries();
    bool primeModelCache();

    /** Probes the standard Windows CUDA Toolkit install location
        (C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v*) for the
        highest-numbered version directory that actually contains what
        demucs-cuda.exe needs at runtime (include\cuda_runtime.h for NVRTC,
        bin\x64\nvrtc64_*.dll for the JIT compiler itself), and if found,
        fills cudaEnvironment with CUDA_PATH and PATH entries to pass to
        runOnce() when launching demucs-cuda.exe. Sets cudaEnvironmentResolved
        and returns whether a usable install was found; safe to call
        repeatedly, cheap after the first call. Doesn't touch or require an
        ambient CUDA_PATH env var - cubecl-cuda's own Windows fallback (no
        env var set) is a hardcoded path with no version subfolder and would
        never actually resolve, so we always resolve and pass this
        explicitly. */
    bool resolveCudaEnvironment();

    void notifyProgress (float progress01, const juce::String& stage);
    void notifyComplete (const Result& result);
    void notifyFailed (const juce::String& error);

    /** Runs one demucs-cli binary end to end (spawn, drain output, wait for
        exit) against runOutputDir, reporting progress against the given
        per-backend time estimate. Returns true if it exited 0 and produced
        vocals.wav; outTailOutput carries the last bit of captured output for
        error reporting either way. Doesn't touch lastGoodBackend or read the
        resulting stems - run() does that once it knows which attempt won.
        extraEnvironment is passed straight through to
        NonBlockingChildProcess::start() - empty for the gpu/cpu backends,
        filled in from cudaEnvironment for the cuda backend. */
    bool runOnce (const juce::File& exe, const juce::File& inputFile, double durationSeconds,
                  double secondsPerSecondOfAudioEstimate, juce::String& outTailOutput,
                  const juce::StringPairArray& extraEnvironment = {});

    juce::File pluginDir;
    juce::File cudaExe;
    juce::File gpuExe;
    juce::File demucsExe; // CPU (ndarray) binary - the required fallback
    juce::File bundledModel;   // htdemucs.safetensors (fast, required)
    juce::File bundledFtModel; // htdemucs_ft.safetensors (high quality, optional)
    juce::File runOutputDir;

    std::atomic<Quality> quality { Quality::fast };
    std::atomic<BackendPreference> backendPreference { BackendPreference::automatic };

    bool cudaEnvironmentResolved = false;
    juce::StringPairArray cudaEnvironment; // CUDA_PATH, PATH - see resolveCudaEnvironment()

    Backend lastGoodBackend = Backend::unknown;

    // Reverted from htdemucs_ft (4x inference cost) back to base htdemucs - see
    // the MODEL WEIGHTS note above. cpuEstimatedSecondsPerSecondOfAudio reverts
    // to its original pre-ft measurement unchanged. gpuEstimatedSecondsPerSecond-
    // OfAudio does not: the original "2.0, measured ~1.9x" figure predates real
    // per-chunk progress reporting and turned out to be pessimistic. Directly
    // remeasured post-revert on this machine with the actual base model (not
    // extrapolated): a 6s clip took 16.8s total and a 60s clip took 80.1s total;
    // the delta (63.3s more processing for 54s more audio) isolates steady-state
    // throughput from the ~10s one-time GPU shader-compile/model-load overhead,
    // giving ~1.17s/s. That agrees closely with an independent estimate from
    // this same session's htdemucs_ft measurements (~4.5-5x measured for ft,
    // divided by 4 for one model pass instead of four: ~1.13s/s) - two
    // independent methods converging on ~1.1-1.2 is trusted over the older,
    // untested 2.0 figure. This only matters as the fallback shown before the
    // first real PROGRESS line arrives (see the PROGRESS note above) and for
    // the duration thresholds just above - not for progress during the run
    // itself, which is real now regardless of this estimate's accuracy.
    static constexpr double gpuEstimatedSecondsPerSecondOfAudio = 1.2;  // measured ~1.13-1.17x, see above
    static constexpr double cpuEstimatedSecondsPerSecondOfAudio = 17.0; // reverted, measured ~16.8x realtime

    // Native CUDA backend (cudarc/cubecl-cuda, still f32 - see the CUDA
    // BACKEND note in SeparationEngine.cpp). Measured directly on this
    // machine (RTX 2050) on a real 240s clip, steady-state (autotune cache
    // already warm): 66.7s total, ~0.28s/s.
    static constexpr double cudaEstimatedSecondsPerSecondOfAudio = 0.28;

    juce::AudioFormatManager formatManager;

    juce::CriticalSection listenerLock;
    juce::Array<Listener*> listeners;

    std::unique_ptr<NonBlockingChildProcess> childProcess;
    juce::CriticalSection processLock; // guards childProcess for cancel() from another thread

    juce::File pendingInputFile;
    bool engineReady = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeparationEngine)
};
