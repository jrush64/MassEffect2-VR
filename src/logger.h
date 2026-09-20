#pragma once

#include <Windows.h>

#include <string>

namespace ME2VR::Log
{
void Line(const std::string& line) noexcept;
// Developer diagnostics. OFF by default: the shipped log stays short and readable, so a user can send
// it and it says what the mod did rather than dumping per-frame measurements. Turn on with
// Diagnostics=1 under [VR] in MELE2VR.ini when actually investigating something.
void SetDiagnostics(bool on) noexcept;
bool DiagnosticsOn() noexcept;
void WindowsError(const char* context, DWORD error) noexcept;
std::string WideToUtf8(const std::wstring& text);
std::wstring LogPath();
}
