#include "logger.h"

#include <ShlObj.h>

#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>

// Logger. The log lives in %LOCALAPPDATA%\MELE2VR, matching ME1 VR's convention: AppData\Local is
// hidden by default in Explorer, writable without admin, and outside the game tree, so a Steam
// verify cannot wipe it and it never clutters the game folder. Falls back to the dll's own
// folder if LOCALAPPDATA is somehow unset.

namespace
{
std::mutex g_mutex;

std::wstring OwnModuleDir()
{
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&OwnModuleDir), &self) != 0 &&
        GetModuleFileNameW(self, path, MAX_PATH) != 0)
    {
        std::wstring full(path);
        const size_t slash = full.find_last_of(L'\\');
        if (slash != std::wstring::npos) return full.substr(0, slash);
    }
    return L".";
}

std::wstring BaseDir()
{
    wchar_t local[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
    {
        std::wstring folder = std::wstring(local, len) + L"\\MELE2VR";
        CreateDirectoryW(folder.c_str(), nullptr);   // harmless if it already exists
        const DWORD attr = GetFileAttributesW(folder.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) return folder;
    }
    return OwnModuleDir();
}

void EnsureDir(const std::wstring& dir) noexcept
{
    CreateDirectoryW(dir.c_str(), nullptr);
}
}

namespace ME2VR::Log
{
std::wstring LogPath()
{
    return BaseDir() + L"\\MELE2VR_Log.txt";
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::atomic_bool g_diagnostics{false};

// Tags that exist to investigate the renderer, not to tell anyone what happened. They are per-frame or
// per-draw and would bury the useful lines (one session was 24k ME2DISC entries). Suppressed unless
// diagnostics are on. Anything not listed here still gets through, so a new message is visible by
// default rather than silently lost.
const char* const kDiagnosticTags[] = {
    "ME2DISC", "VEHDIAG", "VEHAIM", "OBJPANEL", "OBJCIRC", "DISPQ", "SFRDIAG", "UIGATE", "AERPACE2",
    "EYETAG2", "UIRATIO2", "UIRATIO", "GAMEUIQ", "FOVEXIT", "UIGHOST", "PANELCENSUS", "HUDDISC",
    "FILLSRC", "POSETAG", "INVMAT", "SFRCONV", "MOVEFIX", "FPSTORM", "DECOUPLE", "ME2WPN", "CINEXCL",
    "XRAPI", "DIBR", "SFR", "ME2FP", "ME2PW", "HUDGRP", "OBJTEXT", "CINEUI", "VRCINE",
};

bool IsDiagnosticLine(const std::string& line) noexcept
{
    // A line can carry more than one tag ("[ME2XR] [EYETAG2] ..."), so check them all.
    for (const char* tag : kDiagnosticTags)
    {
        std::string needle = "[";
        needle += tag;
        needle += "]";
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

void Line(const std::string& line) noexcept
{
    if (!g_diagnostics.load(std::memory_order_relaxed) && IsDiagnosticLine(line)) return;
    try
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::wstring dir = BaseDir();
        EnsureDir(dir);
        FILE* file = nullptr;
        if (_wfopen_s(&file, LogPath().c_str(), L"ab") == 0 && file != nullptr)
        {
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            fprintf_s(file,
                      "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\r\n",
                      st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                      line.c_str());
            fclose(file);
        }
        OutputDebugStringA((line + "\r\n").c_str());
    }
    catch (...)
    {
    }
}

void SetDiagnostics(bool on) noexcept { g_diagnostics.store(on, std::memory_order_relaxed); }
bool DiagnosticsOn() noexcept { return g_diagnostics.load(std::memory_order_relaxed); }

void WindowsError(const char* context, DWORD error) noexcept
{
    char buffer[256] = {};
    sprintf_s(buffer, "%s failed; GetLastError=%lu", context != nullptr ? context : "<unknown>", error);
    Line(buffer);
}
}
