#include "NonBlockingChildProcess.h"
#include <map>
#include <vector>

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
