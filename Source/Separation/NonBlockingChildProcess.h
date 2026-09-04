/*
  ==============================================================================
    NonBlockingChildProcess.h

    Launches a Windows console binary with its stdout/stderr redirected to a
    pipe this class polls without blocking. Deliberately not a
    juce::ChildProcess replacement in general - just enough surface for
    SeparationEngine::runOnce()'s use case (spawn, poll output, kill, wait,
    exit code).

    Exists because juce::ChildProcess::readProcessOutput() on Windows
    (juce_Threads_windows.cpp) loops internally - polling with 1ms sleeps -
    until the child produces output or exits. A caller stuck inside that call
    gets no chance to update progress or notice a cancellation request for
    however long the child stays silent, which for demucs-cli during model
    load / first-run GPU shader compilation can be tens of seconds - it reads
    as a hang even though the child process isn't actually stuck.
    readAvailable() below returns immediately with 0 when there's nothing to
    read, so the caller always stays in control of its own polling and
    cancellation cadence.

    (An earlier version of this class used a ConPTY/pseudo console instead of
    a plain pipe, on the theory that demucs-cli's indicatif progress bar only
    renders when attached to a real terminal. That's true of indicatif, but
    turned out not to matter: this project's own fork of demucs-cli now
    prints its own "PROGRESS:N/M" line via a plain println! specifically so a
    piped reader gets real progress regardless of terminal attachment - see
    progress.rs in the vendored demucs-rs source and SeparationEngine.cpp's
    findLastProgressFraction(). That made the pseudo console machinery dead
    weight, so it was removed in favour of this simpler pipe-based version.)
  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <sys/types.h> // pid_t
#endif

class NonBlockingChildProcess
{
public:
    NonBlockingChildProcess() = default;
    ~NonBlockingChildProcess();

    /** Spawns exe with args, stdout+stderr merged and redirected to a pipe
        this object owns. args should not include the exe path itself.
        Returns false (and leaves the object in a clean, re-startable state)
        if any step of pipe/process creation fails.

        extraEnvironment, if non-empty, is merged on top of this process's own
        environment block for the child only (this process's environment is
        untouched) - keys not present in extraEnvironment are inherited as-is;
        keys that are present overwrite the inherited value, except "PATH"
        which is prepended (extraEnvironment's PATH value + ";" + the
        inherited PATH) rather than replaced, so the child can still find
        normal system binaries. Needed for demucs-cuda.exe, which resolves
        its CUDA install via the CUDA_PATH env var and needs that install's
        bin\x64 directory on PATH to find nvrtc*.dll - see
        SeparationEngine::resolveCudaEnvironment(). */
    bool start (const juce::File& exe, const juce::StringArray& args,
                const juce::StringPairArray& extraEnvironment = {});

    /** True if the process is still alive. */
    bool isRunning() const noexcept;

    /** Non-blocking: copies up to maxBytes of whatever output is currently
        buffered into dest and returns how many bytes were copied, or 0
        immediately if none is available right now (does NOT wait for the
        child to produce output) - unlike juce::ChildProcess on Windows. */
    int readAvailable (void* dest, int maxBytes) const noexcept;

    /** Forcibly terminates the process, if still running. */
    void kill();

    /** Blocks (bounded by timeoutMs) until the process has exited. Returns
        false if it's still running when the timeout elapses. */
    bool waitForProcessToFinish (int timeoutMs) const;

    /** Only meaningful after the process has exited. */
    uint32_t getExitCode() const noexcept;

private:
    void closeAll();

   #if JUCE_WINDOWS
    HANDLE hChildStdOutWrite = nullptr; // child's end - closed once the child owns it
    HANDLE hReadPipe = nullptr;         // parent reads the child's merged stdout/stderr here
    HANDLE hProcess = nullptr;
    HANDLE hThread = nullptr;
   #else
    int readFd = -1;             // parent reads the child's merged stdout/stderr here, O_NONBLOCK
    pid_t childPid = -1;
    mutable bool reaped = false; // whether childPid has already been consumed by a waitpid() call
    mutable uint32_t cachedExitCode = 0;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NonBlockingChildProcess)
};
