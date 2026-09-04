#include "NonBlockingChildProcess.h"
#include <map>
#include <string>
#include <vector>

#if JUCE_WINDOWS

namespace
{
    // Standard Windows argv quoting (see "Parsing C++ Command-Line Arguments"
    // on MSDN): wraps in quotes and escapes only what needs escaping so paths
    // with spaces (this project lives under a path with spaces itself) survive
    // CreateProcessW's command-line parsing intact.
    juce::String quoteArg (const juce::String& arg)
    {
        if (arg.isNotEmpty() && ! arg.containsAnyOf (" \t\n\v\""))
            return arg;

        juce::String result = "\"";
        int backslashes = 0;

        for (auto c : arg)
        {
            if (c == '\\')
            {
                ++backslashes;
            }
            else if (c == '"')
            {
                result += juce::String::repeatedString ("\\", backslashes * 2 + 1) + "\"";
                backslashes = 0;
                continue;
            }
            else
            {
                if (backslashes > 0)
                    result += juce::String::repeatedString ("\\", backslashes);
                backslashes = 0;
                result += juce::String::charToString (c);
                continue;
            }
        }

        if (backslashes > 0)
            result += juce::String::repeatedString ("\\", backslashes * 2);

        result += "\"";
        return result;
    }

    // Builds a CreateProcessW-compatible environment block: this process's
    // own environment with extraEnvironment's entries merged on top (PATH
    // prepended rather than replaced - see the .h comment on start()).
    // Returns an empty vector if extraEnvironment is empty (caller then
    // passes nullptr to CreateProcessW to inherit unmodified, as before).
    std::vector<wchar_t> buildEnvironmentBlock (const juce::StringPairArray& extraEnvironment)
    {
        if (extraEnvironment.size() == 0)
            return {};

        LPWCH inherited = GetEnvironmentStringsW();
        if (inherited == nullptr)
            return {};

        // Parse the inherited block (sequence of null-terminated "KEY=VALUE"
        // strings, double-null-terminated) into a case-insensitive map so
        // extraEnvironment's keys can find and override/extend them
        // regardless of the casing Windows happened to store them in.
        std::map<juce::String, juce::String> vars; // key stored uppercase for lookup; original casing not needed - CreateProcessW/env lookups are case-insensitive on Windows
        for (const wchar_t* p = inherited; *p != L'\0'; )
        {
            juce::String entry (p);
            auto eq = entry.indexOfChar ('=');
            // Windows environment blocks can contain entries starting with
            // '=' (drive-letter current-directory tracking, e.g. "=C:=C:\foo")
            // - skip those rather than mis-splitting them.
            if (eq > 0)
                vars[entry.substring (0, eq).toUpperCase()] = entry.substring (eq + 1);
            p += entry.length() + 1;
        }
        FreeEnvironmentStringsW (inherited);

        for (auto& key : extraEnvironment.getAllKeys())
        {
            auto upperKey = key.toUpperCase();
            auto value = extraEnvironment[key];
            if (upperKey == "PATH" && vars.count ("PATH") != 0)
                vars["PATH"] = value + ";" + vars["PATH"];
            else
                vars[upperKey] = value;
        }

        // Built directly as a wchar_t buffer rather than via juce::String
        // concatenation - juce::String isn't meant to carry embedded null
        // characters, which this block requires between entries.
        std::vector<wchar_t> block;
        for (auto& [key, value] : vars)
        {
            juce::String entry = key + "=" + value;
            auto entryW = entry.toWideCharPointer();
            block.insert (block.end(), entryW, entryW + entry.length());
            block.push_back (L'\0');
        }
        block.push_back (L'\0'); // block-terminating extra null

        return block;
    }
}

NonBlockingChildProcess::~NonBlockingChildProcess()
{
    closeAll();
}

void NonBlockingChildProcess::closeAll()
{
    if (hProcess != nullptr)        { CloseHandle (hProcess); hProcess = nullptr; }
    if (hThread != nullptr)         { CloseHandle (hThread); hThread = nullptr; }
    if (hChildStdOutWrite != nullptr) { CloseHandle (hChildStdOutWrite); hChildStdOutWrite = nullptr; }
    if (hReadPipe != nullptr)       { CloseHandle (hReadPipe); hReadPipe = nullptr; }
}

bool NonBlockingChildProcess::start (const juce::File& exe, const juce::StringArray& args,
                                      const juce::StringPairArray& extraEnvironment)
{
    jassert (hProcess == nullptr); // one shot - make a fresh NonBlockingChildProcess per run

    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof (SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    if (! CreatePipe (&hReadPipe, &hChildStdOutWrite, &sa, 0))
    {
        closeAll();
        return false;
    }

    // Only the write end (inherited by the child) should be inheritable -
    // our own read end must not leak into the child process.
    if (! SetHandleInformation (hReadPipe, HANDLE_FLAG_INHERIT, 0))
    {
        closeAll();
        return false;
    }

    HANDLE hNul = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, 0, nullptr);

    juce::String cmdLine = quoteArg (exe.getFullPathName());
    for (auto& a : args)
        cmdLine += " " + quoteArg (a);

    auto cmdLineW = cmdLine.toWideCharPointer();
    std::vector<wchar_t> cmdLineBuffer (cmdLineW, cmdLineW + wcslen (cmdLineW) + 1);

    STARTUPINFOW si {};
    si.cb = sizeof (STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNul;
    si.hStdOutput = hChildStdOutWrite;
    si.hStdError = hChildStdOutWrite; // merged, same as juce::ChildProcess::wantStdOut|wantStdErr

    auto envBlock = buildEnvironmentBlock (extraEnvironment);
    DWORD creationFlags = CREATE_NO_WINDOW;
    LPVOID envPtr = nullptr;
    if (! envBlock.empty())
    {
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
        envPtr = envBlock.data();
    }

    PROCESS_INFORMATION pi {};
    BOOL ok = CreateProcessW (nullptr, cmdLineBuffer.data(), nullptr, nullptr, TRUE,
                                creationFlags, envPtr, nullptr, &si, &pi);

    if (hNul != nullptr && hNul != INVALID_HANDLE_VALUE)
        CloseHandle (hNul);

    // The child now owns its copy of the write end (and NUL read end) - our
    // copy must be closed so ReadFile on hReadPipe sees EOF once the child
    // exits, rather than blocking forever waiting for a write end that's
    // still open in this (the parent) process.
    CloseHandle (hChildStdOutWrite);
    hChildStdOutWrite = nullptr;

    if (! ok)
    {
        closeAll();
        return false;
    }

    hProcess = pi.hProcess;
    hThread = pi.hThread; // not needed once the process is running
    CloseHandle (hThread);
    hThread = nullptr;

    return true;
}

bool NonBlockingChildProcess::isRunning() const noexcept
{
    if (hProcess == nullptr)
        return false;

    DWORD exitCode = 0;
    return GetExitCodeProcess (hProcess, &exitCode) && exitCode == STILL_ACTIVE;
}

int NonBlockingChildProcess::readAvailable (void* dest, int maxBytes) const noexcept
{
    if (hReadPipe == nullptr || maxBytes <= 0)
        return 0;

    DWORD available = 0;
    if (! PeekNamedPipe (hReadPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
        return 0;

    DWORD toRead = juce::jmin ((DWORD) maxBytes, available);
    DWORD numRead = 0;
    if (! ReadFile (hReadPipe, dest, toRead, &numRead, nullptr))
        return 0;

    return (int) numRead;
}

void NonBlockingChildProcess::kill()
{
    if (hProcess != nullptr && isRunning())
        TerminateProcess (hProcess, 1);
}

bool NonBlockingChildProcess::waitForProcessToFinish (int timeoutMs) const
{
    if (hProcess == nullptr)
        return true;

    return WaitForSingleObject (hProcess, (DWORD) juce::jmax (0, timeoutMs)) == WAIT_OBJECT_0;
}

uint32_t NonBlockingChildProcess::getExitCode() const noexcept
{
    if (hProcess == nullptr)
        return 0;

    DWORD exitCode = 0;
    GetExitCodeProcess (hProcess, &exitCode);
    return (uint32_t) exitCode;
}

#else // POSIX (macOS / Linux)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
    // Builds a posix_spawn-compatible environment: this process's own
    // environment (via the extern `environ`) with extraEnvironment's entries
    // merged on top (PATH prepended rather than replaced - see the .h
    // comment on start(), same contract as the Windows side). Returns an
    // owning vector of "KEY=VALUE" strings plus a null-terminated char*
    // array of pointers into them - the caller must keep both alive for the
    // duration of the posix_spawn() call.
    struct EnvBlock
    {
        std::vector<std::string> entries;
        std::vector<char*> ptrs; // entries.data() pointers + trailing nullptr
    };

    EnvBlock buildEnvironmentBlock (const juce::StringPairArray& extraEnvironment)
    {
        EnvBlock block;

        std::map<juce::String, juce::String> vars;
        for (char** p = environ; *p != nullptr; ++p)
        {
            juce::String entry (*p);
            auto eq = entry.indexOfChar ('=');
            if (eq > 0)
                vars[entry.substring (0, eq)] = entry.substring (eq + 1);
        }

        for (auto& key : extraEnvironment.getAllKeys())
        {
            auto value = extraEnvironment[key];
            if (key == "PATH" && vars.count ("PATH") != 0)
                vars["PATH"] = value + ":" + vars["PATH"];
            else
                vars[key] = value;
        }

        block.entries.reserve (vars.size());
        for (auto& [key, value] : vars)
            block.entries.push_back ((key + "=" + value).toStdString());

        block.ptrs.reserve (block.entries.size() + 1);
        for (auto& entry : block.entries)
            block.ptrs.push_back (entry.data());
        block.ptrs.push_back (nullptr);

        return block;
    }
}

NonBlockingChildProcess::~NonBlockingChildProcess()
{
    closeAll();
}

void NonBlockingChildProcess::closeAll()
{
    if (readFd != -1) { ::close (readFd); readFd = -1; }
}

bool NonBlockingChildProcess::start (const juce::File& exe, const juce::StringArray& args,
                                      const juce::StringPairArray& extraEnvironment)
{
    jassert (childPid == -1); // one shot - make a fresh NonBlockingChildProcess per run

    int pipeFds[2] { -1, -1 };
    if (::pipe (pipeFds) != 0)
        return false;

    int readEnd = pipeFds[0];
    int writeEnd = pipeFds[1];

    // Our own read end must not leak into the child (which gets its stdout
    // /stderr dup'd from writeEnd instead) or into any further children this
    // process spawns later.
    ::fcntl (readEnd, F_SETFD, FD_CLOEXEC);
    // Non-blocking so readAvailable() never blocks waiting for output, same
    // contract as the Windows PeekNamedPipe-based version.
    ::fcntl (readEnd, F_SETFL, O_NONBLOCK);

    // Own the argv/exe strings for the lifetime of this call - posix_spawn
    // takes char* (not const char*), and the pointers below must stay valid
    // until it returns.
    std::vector<std::string> argStorage;
    argStorage.push_back (exe.getFullPathName().toStdString());
    for (auto& a : args)
        argStorage.push_back (a.toStdString());

    std::vector<char*> argv;
    argv.reserve (argStorage.size() + 1);
    for (auto& a : argStorage)
        argv.push_back (a.data());
    argv.push_back (nullptr);

    auto envBlock = buildEnvironmentBlock (extraEnvironment);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init (&actions);
    posix_spawn_file_actions_addopen (&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2 (&actions, writeEnd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2 (&actions, writeEnd, STDERR_FILENO); // merged, same as the Windows side
    posix_spawn_file_actions_addclose (&actions, writeEnd);
    posix_spawn_file_actions_addclose (&actions, readEnd);

    pid_t pid = -1;
    int spawnResult = ::posix_spawn (&pid, argStorage[0].c_str(), &actions, nullptr,
                                      argv.data(), envBlock.ptrs.data());

    posix_spawn_file_actions_destroy (&actions);

    // The child now owns its dup'd copies of writeEnd - our copy must be
    // closed so a read() on readEnd sees EOF once the child exits, rather
    // than blocking forever waiting for a write end that's still open here.
    ::close (writeEnd);

    if (spawnResult != 0)
    {
        ::close (readEnd);
        return false;
    }

    readFd = readEnd;
    childPid = pid;
    reaped = false;
    cachedExitCode = 0;

    return true;
}

bool NonBlockingChildProcess::isRunning() const noexcept
{
    if (childPid == -1 || reaped)
        return false;

    int status = 0;
    pid_t result = ::waitpid (childPid, &status, WNOHANG);

    if (result == 0)
        return true; // still running

    if (result == childPid)
    {
        reaped = true;
        cachedExitCode = (uint32_t) (WIFEXITED (status) ? WEXITSTATUS (status) : 1);
    }
    // result == -1 (e.g. ECHILD because something else already reaped it)
    // falls through as "not running" too - nothing more we can learn here.

    return false;
}

int NonBlockingChildProcess::readAvailable (void* dest, int maxBytes) const noexcept
{
    if (readFd == -1 || maxBytes <= 0)
        return 0;

    ssize_t numRead = ::read (readFd, dest, (size_t) maxBytes);

    if (numRead < 0) // EAGAIN/EWOULDBLOCK (nothing buffered right now) or a transient EINTR
        return 0;

    return (int) numRead;
}

void NonBlockingChildProcess::kill()
{
    if (childPid != -1 && isRunning())
        ::kill (childPid, SIGKILL);
}

bool NonBlockingChildProcess::waitForProcessToFinish (int timeoutMs) const
{
    if (childPid == -1 || reaped)
        return true;

    auto deadline = juce::Time::getMillisecondCounter() + (uint32_t) juce::jmax (0, timeoutMs);

    for (;;)
    {
        if (! isRunning()) // also reaps + caches the exit code once it exits
            return true;

        if (juce::Time::getMillisecondCounter() >= deadline)
            return false;

        juce::Thread::sleep (2);
    }
}

uint32_t NonBlockingChildProcess::getExitCode() const noexcept
{
    return cachedExitCode;
}

#endif
