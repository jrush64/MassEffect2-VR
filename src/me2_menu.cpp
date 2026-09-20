#include "me2_menu.h"

#include "calcview_hook.h"
#include "convo_fp.h"
#include "d3d_capture.h"
#include "engine_probe.h"
#include "logger.h"
#include "pchud.h"
#include "me2_xr.h"

#include <cstdint>

#include <Windows.h>
#include <Xinput.h>
#include <d3d11.h>
#include <dxgi.h>

#include "MinHook.h"   // [MOVEFIX] XInput hooks (stick capture + run-where-you-look)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

namespace ME2VR::Menu
{
namespace
{
// Per-group HUD baked defaults (2026-07-26, from a tuned + saved ini). Shared by LoadValues (the
// ini-default fallback) and the HUD tab's Reset buttons, so "reset" restores the shipped baseline
// rather than a hardcoded (0,0,1,1) that would drift from whatever the real default becomes. Only
// G0/G5/G6 differ from neutral -- Weapon+ammo nudged right, Health+squad enlarged and raised,
// Objective circle mirrored left and enlarged.
struct HudGroupDefault { float offX, offY, scaleX, scaleY; };
constexpr HudGroupDefault kHudGroupDefaults[] = {
    { 232.0f,   0.0f, 1.00f, 1.00f },   // G0 Weapon + ammo
    {   0.0f,   0.0f, 1.00f, 1.00f },   // G1 Target info
    {   0.0f,   0.0f, 1.00f, 1.00f },   // G2 Powers
    {   0.0f,   0.0f, 1.00f, 1.00f },   // G3 Notifications
    {   0.0f,   0.0f, 1.00f, 1.00f },   // G4 Centre status
    {   0.0f, -41.0f, 1.75f, 1.75f },   // G5 Health + squad          (offY baked 2026-07-27)
    { -232.0f,  0.0f, 1.30f, 1.30f },   // G6 Objective circle
    { -232.0f,  0.0f, 1.00f, 1.00f },   // G7 Mission/objective panel (offX baked 2026-07-27, onto G6's X)
};
constexpr int kHudGroupDefaultCount = static_cast<int>(sizeof(kHudGroupDefaults) / sizeof(kHudGroupDefaults[0]));
HudGroupDefault HudGroupDefaultFor(int g) noexcept
{
    return (g >= 0 && g < kHudGroupDefaultCount) ? kHudGroupDefaults[g] : HudGroupDefault{ 0.0f, 0.0f, 1.0f, 1.0f };
}

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11Texture2D* g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
int g_w = 900;
int g_h = 720;
bool g_ready = false;
bool g_open = false;
bool g_insertWasDown = false;
HWND g_hwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_mouseAnchorValid = false;
POINT g_mouseAnchorScreen = {};
float g_virtualMouseX = 450.0f;
float g_virtualMouseY = 360.0f;
bool g_savedFlash = false;
int g_savedFrames = 0;
bool g_valuesLoaded = false;
int g_recenterKey = 'R';        // rebindable recenter hotkey (VK code); default R
int g_fpToggleKey = 'K';        // rebindable first-person toggle hotkey (VK code); default K, ME1 parity
bool g_rebindingFpToggle = false;
bool g_rebindingRecenter = false;   // menu is waiting to capture a new key

// ME1-parity settings that live here rather than in a subsystem.
// [PROFILES] ME1 parity. A profile is a complete snapshot of the settings file, so it captures every
// key automatically and can never drift out of sync with the option list the way a hand-maintained
// per-field copy would. Slot 0 is the live file; slots 1-3 are sidecars next to it.
constexpr int kProfileCount = 4;
int  g_activeProfile = 0;
int  g_profileKeys[kProfileCount] = { '1', '2', '3', '4' };
bool g_profileHotkeys = true;
int  g_rebindingProfile = -1;
const char* ProfileName(int i) noexcept
{
    switch (i) { case 0: return "Default"; case 1: return "Custom 1"; case 2: return "Custom 2"; default: return "Custom 3"; }
}

bool g_disableDof = false;            // [DOFWASH] MUST default OFF -- see the menu note below.
bool g_recenterKeyEnabled = true;
int  g_menuKey = VK_INSERT;           // rebindable, unlike ME2's old hardcoded Insert
bool g_rebindingMenuKey = false;

// VK code -> short display name for the rebind UI.
const char* KeyName(int vk) noexcept
{
    static char buf[16];
    if (vk >= 'A' && vk <= 'Z') { buf[0] = static_cast<char>(vk); buf[1] = '\0'; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = static_cast<char>(vk); buf[1] = '\0'; return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { sprintf_s(buf, "F%d", vk - VK_F1 + 1); return buf; }
    switch (vk)
    {
        case VK_SPACE: return "Space";  case VK_TAB: return "Tab";   case VK_RETURN: return "Enter";
        case VK_LSHIFT: case VK_SHIFT: return "Shift";  case VK_LCONTROL: case VK_CONTROL: return "Ctrl";
        case VK_OEM_3: return "`";  case VK_HOME: return "Home";  case VK_END: return "End";
        case VK_BACK: return "Backspace";
    }
    sprintf_s(buf, "0x%02X", vk);
    return buf;
}

// Engine-level game-input freeze while the menu is open (so mouse/look don't drive the camera).
// P1 ULocalPlayer -> Actor (PlayerController) @0x68 -> bIgnoreMoveInput@0x7A5 / bIgnoreLookInput@0x7A6.
constexpr std::uintptr_t kUPlayerActor = 0x68;
constexpr std::uintptr_t kPcIgnoreMove = 0x7A5;
constexpr std::uintptr_t kPcIgnoreLook = 0x7A6;
bool g_frozen = false;
unsigned char g_savedMove = 0, g_savedLook = 0;

void EnforceFreeze(bool wantFrozen) noexcept
{
    __try
    {
        const std::uintptr_t lp = ME2VR::EngineProbe::GetPrimaryLocalPlayer();
        if (lp == 0) return;
        const std::uintptr_t pc = *reinterpret_cast<std::uintptr_t*>(lp + kUPlayerActor);
        if (pc < 0x10000) return;
        auto* im = reinterpret_cast<unsigned char*>(pc + kPcIgnoreMove);
        auto* il = reinterpret_cast<unsigned char*>(pc + kPcIgnoreLook);
        if (wantFrozen)
        {
            if (!g_frozen) { g_savedMove = *im; g_savedLook = *il; g_frozen = true; }
            *im = 1; *il = 1;   // re-assert every frame (the game can reset these)
        }
        else if (g_frozen)
        {
            *im = g_savedMove; *il = g_savedLook; g_frozen = false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ===================== [MOVEFIX] XInput hooks (ported from ME1 vr_menu, 2026-07-20) =====================
// ME2 had NO XInput hooks (audit 2026-07-16). Two jobs: (1) capture the live left-stick magnitude for
// the FP-storm latch; (2) rotate the LEFT (movement) stick by the applied render-side head-look yaw so
// pushing forward runs where the head is LOOKING ("run where you look"). HeadLookYawUU is the EXACT
// applied offset and is 0 whenever look is off (menus, cine, weapon-out head-aim) -> exact no-op there.
// Also zeroes the pad while the Insert menu is open (pad-level belt to EnforceFreeze's engine braces).
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_origXInput = nullptr;
XInputGetState_t g_origXInputEx = nullptr;   // XInputGetStateEx (ordinal 100) trampoline
std::atomic<int> g_leftStickMag{0};
std::atomic_bool g_moveFollowsHead{true};    // "Run where you look" (ini MoveFollowsHead)
// [DECOUPLE] ME1 parity: zero a RIGHT-stick axis so only the head can steer it. Pitch is the useful
// one in VR (looking up/down with the stick fights the headset), yaw is for players who want to turn
// purely by looking. Applied after the menu/move handling so it can never affect movement.
std::atomic_bool g_decoupledPitch{false};
std::atomic_bool g_decoupledYaw{false};
// [R3RECENTER] ME1 parity: double-tap a pad button to recenter. Players use a gamepad in the
// headset, where reaching for a keyboard key is exactly the thing you can't do.
//
// Originally right-stick-click, changed 2026-07-26: R3 already opens ME2's own radial/squad menu, so
// every recenter attempt fought that menu instead of recentering. Moved to BACK (View button) --
// unbound during ME2 gameplay, so there is no native action to collide with. Kept as a DOUBLE-tap
// rather than single-press: this hook reads the pad state BEFORE the game or UE3 ever sees it, so a
// single-press trigger on a button used for anything else (B/melee was considered and rejected for
// exactly this) would fire on every rapid press during normal play, not just a deliberate ones.
constexpr DWORD kR3DoubleClickMs = 400;

// [DECOUPLEMENU 2026-08-22] Decoupled pitch/yaw zero a look axis before the game ever reads it,
// which is right for gameplay and wrong for menus: while a full-screen GUI or the galaxy map is up,
// that axis is how you move the cursor, so entries simply cannot be reached. Reported against ME1:
// "some commands like universe map are out of reach unless DP is off". The option is about gameplay
// look, so it stands down whenever a menu owns the screen and comes back by itself afterwards.
// Cached briefly because the input hooks are polled far faster than this state can change.
bool MenuOwnsLook() noexcept
{
    static unsigned long long s_lastMs = 0;
    static bool s_cached = false;
    const unsigned long long now = GetTickCount64();
    if (now - s_lastMs >= 50)
    {
        s_lastMs = now;
        s_cached = (ME2VR::D3DCapture::GetMenuMode());
    }
    return s_cached;
}

inline void ApplyStickDecouple(XINPUT_GAMEPAD* pad) noexcept
{
    if (MenuOwnsLook()) return;   // [DECOUPLEMENU] menus need the stick axis back
    if (g_decoupledYaw.load(std::memory_order_relaxed))   pad->sThumbRX = 0;
    if (g_decoupledPitch.load(std::memory_order_relaxed)) pad->sThumbRY = 0;
}

// Back-button double tap -> recenter. Edge-detected on the real pad state, before the menu
// zeroing, and ignored while the menu is open.
inline void CheckR3Recenter(const XINPUT_STATE* state, DWORD result, DWORD idx) noexcept
{
    if (result != ERROR_SUCCESS || idx != 0 || state == nullptr || g_open) return;
    static bool s_r3Prev = false;
    static DWORD s_lastClickMs = 0;
    const bool r3 = (state->Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
    if (r3 && !s_r3Prev)
    {
        const DWORD now = GetTickCount();
        if (s_lastClickMs != 0 && (now - s_lastClickMs) <= kR3DoubleClickMs)
        {
            ME2VR::Me2Xr::Recenter();
            s_lastClickMs = 0;            // consume, so a triple-click isn't two recenters
        }
        else s_lastClickMs = now;
    }
    s_r3Prev = r3;
}

inline void CaptureLeftStick(const XINPUT_STATE* state, DWORD result, DWORD idx) noexcept
{
    if (result == ERROR_SUCCESS && idx == 0 && state != nullptr)
    {
        const int lx = state->Gamepad.sThumbLX;
        const int ly = state->Gamepad.sThumbLY;
        g_leftStickMag.store((std::max)(std::abs(lx), std::abs(ly)), std::memory_order_relaxed);
    }
}

inline void RotateMoveStickByHeadLook(XINPUT_GAMEPAD* pad) noexcept
{
    const int32_t yawUU = ME2VR::CalcViewHook::HeadLookYawUU();
    if (yawUU == 0) return;
    if (pad->sThumbLX == 0 && pad->sThumbLY == 0) return;
    // Positive yawUU = view turned LEFT (same ApplyHeadRotation Ry convention as ME1) -> rotate the
    // stick vector left by the same angle. Preserves magnitude (dead zones unchanged); clamps to int16.
    const float a = static_cast<float>(yawUU) * (6.2831853f / 65536.0f);
    const float c = std::cos(a), s = std::sin(a);
    const float lx = static_cast<float>(pad->sThumbLX);
    const float ly = static_cast<float>(pad->sThumbLY);
    float rx = lx * c - ly * s;   // x=right, y=forward; +a rotates forward toward left (-x)
    float ry = lx * s + ly * c;
    if (rx > 32767.0f) rx = 32767.0f; else if (rx < -32768.0f) rx = -32768.0f;
    if (ry > 32767.0f) ry = 32767.0f; else if (ry < -32768.0f) ry = -32768.0f;
    pad->sThumbLX = static_cast<SHORT>(rx);
    pad->sThumbLY = static_cast<SHORT>(ry);
}

DWORD WINAPI HookedXInputGetState(DWORD idx, XINPUT_STATE* state) noexcept
{
    const DWORD r = g_origXInput ? g_origXInput(idx, state) : ERROR_DEVICE_NOT_CONNECTED;
    CaptureLeftStick(state, r, idx);
    CheckR3Recenter(state, r, idx);
    if (state != nullptr)
    {
        if (g_open) ZeroMemory(&state->Gamepad, sizeof(XINPUT_GAMEPAD));
        else
        {
            if (g_moveFollowsHead.load(std::memory_order_relaxed)) RotateMoveStickByHeadLook(&state->Gamepad);
            ApplyStickDecouple(&state->Gamepad);
        }
    }
    return r;
}

DWORD WINAPI HookedXInputGetStateEx(DWORD idx, XINPUT_STATE* state) noexcept
{
    const DWORD r = g_origXInputEx ? g_origXInputEx(idx, state)
                                   : (g_origXInput ? g_origXInput(idx, state) : ERROR_DEVICE_NOT_CONNECTED);
    CaptureLeftStick(state, r, idx);
    CheckR3Recenter(state, r, idx);
    if (state != nullptr)
    {
        if (g_open) ZeroMemory(&state->Gamepad, sizeof(XINPUT_GAMEPAD));
        else
        {
            if (g_moveFollowsHead.load(std::memory_order_relaxed)) RotateMoveStickByHeadLook(&state->Gamepad);
            ApplyStickDecouple(&state->Gamepad);
        }
    }
    return r;
}

// ===================== [DECOUPLE] mouse half =====================
// The stick decouple above only touches XInput, so decoupled pitch/yaw did nothing for mouse players.
// UE3 reads mouse-look through RAW INPUT (that's why the menu's WndProc swallows WM_INPUT), so the
// place to cut an axis is the raw-input read itself: let the call through, then zero the axis the head
// owns before the engine ever sees the packet. Absolute cursor movement is untouched, so menus and the
// mouse pointer behave normally - only the relative look delta is affected.
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
GetRawInputData_t g_origGetRawInputData = nullptr;
GetRawInputBuffer_t g_origGetRawInputBuffer = nullptr;

inline void DecoupleRawMouse(RAWINPUT* ri) noexcept
{
    if (ri == nullptr || ri->header.dwType != RIM_TYPEMOUSE) return;
    if ((ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) return;   // absolute device: leave alone
    if (MenuOwnsLook()) return;   // [DECOUPLEMENU] menus need the mouse axis back
    if (g_decoupledYaw.load(std::memory_order_relaxed))   ri->data.mouse.lLastX = 0;
    if (g_decoupledPitch.load(std::memory_order_relaxed)) ri->data.mouse.lLastY = 0;
}

UINT WINAPI HookedGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) noexcept
{
    const UINT r = g_origGetRawInputData ? g_origGetRawInputData(h, cmd, data, size, hdr) : static_cast<UINT>(-1);
    if (!g_open && cmd == RID_INPUT && data != nullptr && r != static_cast<UINT>(-1))
        DecoupleRawMouse(reinterpret_cast<RAWINPUT*>(data));
    return r;
}

// Batched variant: the buffer is a packed array walked with NEXTRAWINPUTBLOCK, not a flat stride.
UINT WINAPI HookedGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdr) noexcept
{
    const UINT r = g_origGetRawInputBuffer ? g_origGetRawInputBuffer(data, size, hdr) : static_cast<UINT>(-1);
    if (!g_open && data != nullptr && r != static_cast<UINT>(-1) && r > 0)
    {
        // Advance by hand: NEXTRAWINPUTBLOCK's alignment macro needs a type this TU doesn't pull in.
        // Each block is dwSize bytes, 8-byte aligned on x64.
        RAWINPUT* cur = data;
        for (UINT i = 0; i < r && cur != nullptr; ++i)
        {
            DecoupleRawMouse(cur);
            const ULONG_PTR next = (reinterpret_cast<ULONG_PTR>(cur) + cur->header.dwSize + 7u) & ~static_cast<ULONG_PTR>(7u);
            cur = reinterpret_cast<RAWINPUT*>(next);
        }
    }
    return r;
}

void InstallMouseHooks() noexcept
{
    static bool s_done = false;
    if (s_done) return;
    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) return;
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32 == nullptr) return;
    if (FARPROC p = GetProcAddress(u32, "GetRawInputData"))
    {
        void* orig = nullptr;
        if (MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedGetRawInputData), &orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
        {
            g_origGetRawInputData = reinterpret_cast<GetRawInputData_t>(orig);
            ME2VR::Log::Line("[DECOUPLE] GetRawInputData hook ready (mouse decouple live)");
        }
    }
    if (FARPROC p = GetProcAddress(u32, "GetRawInputBuffer"))
    {
        void* orig = nullptr;
        if (MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedGetRawInputBuffer), &orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
        {
            g_origGetRawInputBuffer = reinterpret_cast<GetRawInputBuffer_t>(orig);
            ME2VR::Log::Line("[DECOUPLE] GetRawInputBuffer hook ready");
        }
    }
    s_done = (g_origGetRawInputData != nullptr || g_origGetRawInputBuffer != nullptr);
}

void InstallPadHooks() noexcept
{
    static bool s_done = false;
    static unsigned s_calls = 0;
    if (s_done) return;
    if ((s_calls++ % 300) != 0) return;   // retry ~every 5s until the game has loaded an XInput dll
    const MH_STATUS mhInit = MH_Initialize();   // tolerate already-initialized (d3d_capture inits it too)
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) return;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll" };
    for (const char* d : dlls)
    {
        HMODULE m = GetModuleHandleA(d);
        if (m == nullptr) continue;   // only hook XInput dlls the GAME already loaded
        FARPROC p = GetProcAddress(m, "XInputGetState");
        if (p != nullptr)
        {
            void* orig = nullptr;
            if (MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedXInputGetState), &orig) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
            {
                if (g_origXInput == nullptr) g_origXInput = reinterpret_cast<XInputGetState_t>(orig);
                ME2VR::Log::Line(std::string("[MOVEFIX] XInputGetState hook ready: ") + d);
            }
        }
        // XInputGetStateEx - ordinal 100, no named export (xinput1_3/1_4; absent from 9_1_0). UE3 can
        // poll the pad through THIS entry, bypassing the named hook (ME1's menu-leak lesson).
        FARPROC pex = GetProcAddress(m, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(100)));
        if (pex != nullptr && pex != p)
        {
            void* origEx = nullptr;
            if (MH_CreateHook(reinterpret_cast<void*>(pex), reinterpret_cast<void*>(&HookedXInputGetStateEx), &origEx) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(pex)) == MH_OK)
            {
                if (g_origXInputEx == nullptr) g_origXInputEx = reinterpret_cast<XInputGetState_t>(origEx);
                ME2VR::Log::Line(std::string("[MOVEFIX] XInputGetStateEx (ord 100) hook ready: ") + d);
            }
        }
    }
    if (g_origXInput != nullptr || g_origXInputEx != nullptr) s_done = true;   // latch only on success
}
// ========================================================================================================

template <typename T> void SafeRelease(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
float ClampF(float v, float lo, float hi, float fb) noexcept { return std::isfinite(v) ? (std::max)(lo, (std::min)(hi, v)) : fb; }
int   ClampI(int v, int lo, int hi, int fb) noexcept { (void)fb; return (std::max)(lo, (std::min)(hi, v)); }

std::wstring IniPath()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p + L"MELE2VR.ini";
}

// First-person per-state config <-> ini. Keys live under [FirstPerson] as S<idx>_<field>.
float IniGetF(const std::wstring& path, const wchar_t* key, float def) noexcept
{
    wchar_t d[32] = {}, buf[64] = {};
    swprintf_s(d, L"%.3f", def);
    GetPrivateProfileStringW(L"FirstPerson", key, d, buf, 64, path.c_str());
    return static_cast<float>(_wtof(buf));
}

void LoadFpStates(const std::wstring& path) noexcept
{
    for (int i = 0; i < ME2VR::EngineProbe::FpStateCount(); ++i)
    {
        ME2VR::EngineProbe::FpStateCfg* s = ME2VR::EngineProbe::GetFpStateCfg(i);
        if (s == nullptr) continue;
        const ME2VR::EngineProbe::FpStateCfg d = ME2VR::EngineProbe::FpStateDefault(i);
        wchar_t k[32] = {};
        swprintf_s(k, L"S%d_on", i);       s->on       = GetPrivateProfileIntW(L"FirstPerson", k, d.on ? 1 : 0, path.c_str()) != 0;
        swprintf_s(k, L"S%d_hideHead", i); s->hideHead = GetPrivateProfileIntW(L"FirstPerson", k, d.hideHead ? 1 : 0, path.c_str()) != 0;
        swprintf_s(k, L"S%d_hideBody", i); s->hideBody = GetPrivateProfileIntW(L"FirstPerson", k, d.hideBody ? 1 : 0, path.c_str()) != 0;
        swprintf_s(k, L"S%d_hideWeapon", i); s->hideWeapon = GetPrivateProfileIntW(L"FirstPerson", k, d.hideWeapon ? 1 : 0, path.c_str()) != 0;
        swprintf_s(k, L"S%d_x", i);        s->x = ClampF(IniGetF(path, k, d.x), -200.0f, 400.0f, d.x);
        swprintf_s(k, L"S%d_y", i);        s->y = ClampF(IniGetF(path, k, d.y), -100.0f, 100.0f, d.y);
        swprintf_s(k, L"S%d_z", i);        s->z = ClampF(IniGetF(path, k, d.z), -100.0f, 200.0f, d.z);
    }
    // Master first-person toggle persists too (defaults ON). It used to reset to OFF on every relaunch, which
    // silently disabled ALL first-person - making the per-state sliders/booleans look completely dead.
    ME2VR::EngineProbe::SetFirstPerson(GetPrivateProfileIntW(L"FirstPerson", L"Enabled", 0, path.c_str()) != 0);   // baked 2026-07-26 from a tuned ini
}

void SaveFpStates(const std::wstring& path) noexcept
{
    for (int i = 0; i < ME2VR::EngineProbe::FpStateCount(); ++i)
    {
        const ME2VR::EngineProbe::FpStateCfg* s = ME2VR::EngineProbe::GetFpStateCfg(i);
        if (s == nullptr) continue;
        wchar_t k[32] = {}, v[32] = {};
        swprintf_s(k, L"S%d_on", i);       WritePrivateProfileStringW(L"FirstPerson", k, s->on ? L"1" : L"0", path.c_str());
        swprintf_s(k, L"S%d_hideHead", i); WritePrivateProfileStringW(L"FirstPerson", k, s->hideHead ? L"1" : L"0", path.c_str());
        swprintf_s(k, L"S%d_hideBody", i); WritePrivateProfileStringW(L"FirstPerson", k, s->hideBody ? L"1" : L"0", path.c_str());
        swprintf_s(k, L"S%d_hideWeapon", i); WritePrivateProfileStringW(L"FirstPerson", k, s->hideWeapon ? L"1" : L"0", path.c_str());
        swprintf_s(k, L"S%d_x", i); swprintf_s(v, L"%.1f", s->x); WritePrivateProfileStringW(L"FirstPerson", k, v, path.c_str());
        swprintf_s(k, L"S%d_y", i); swprintf_s(v, L"%.1f", s->y); WritePrivateProfileStringW(L"FirstPerson", k, v, path.c_str());
        swprintf_s(k, L"S%d_z", i); swprintf_s(v, L"%.1f", s->z); WritePrivateProfileStringW(L"FirstPerson", k, v, path.c_str());
    }
    WritePrivateProfileStringW(L"FirstPerson", L"Enabled", ME2VR::EngineProbe::GetFirstPerson() ? L"1" : L"0", path.c_str());
}

void LoadValues() noexcept
{
    const std::wstring path = IniPath();
    wchar_t buf[64] = {};
    // Baked 2026-07-26 from a tuned + saved ini (both the ini-default STRING and the ClampF fallback
    // updated together, so a corrupted/unparseable value falls back to the SAME baseline, not the old one).
    // Stereo defaults baked 2026-07-31 from a tuned config.
    // Lower separation with a nearer convergence plane and eyes swapped - note SwapEyes=1 is now the
    // default, which independently matches a separate report that swapping eyes was needed to get
    // correct depth. Two independent results arriving at the same swap say the old default was inverted.
    GetPrivateProfileStringW(L"VR", L"HalfEyeUU", L"3.55", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetHalfEyeUU(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 16.0f, 3.55f));
    GetPrivateProfileStringW(L"VR", L"Convergence", L"0.025", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetSfrConvergence(ClampF(static_cast<float>(_wtof(buf)), -0.2f, 0.2f, 0.025f));
    ME2VR::CalcViewHook::SetSwapEyes(GetPrivateProfileIntW(L"VR", L"SwapEyes", 1, path.c_str()) != 0);
    // VR mode picker (0=Mono, 1=Stereo, 2=AER) + AER knobs.
    {   // default SFR; MIGRATE old SBS Stereo (1) -> SFR (4): old stereo is locked away (picker has no 1).
        int m = ClampI(GetPrivateProfileIntW(L"VR", L"Mode", 4, path.c_str()), 0, 4, 4);
        if (m == 1) m = 4;
        ME2VR::CalcViewHook::SetVrMode(m);
    }
    GetPrivateProfileStringW(L"VR", L"AerHalfEyeUU", L"2.5", buf, 64, path.c_str());   // baked 2026-07-26
    ME2VR::CalcViewHook::SetAerHalfEyeUU(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 10.0f, 2.5f));
    ME2VR::CalcViewHook::SetAerSwapEyes(GetPrivateProfileIntW(L"VR", L"AerSwapEyes", 0, path.c_str()) != 0);   // [AERSHAKE bake] 0: FIFO stamp killed the pipeline swap this compensated
    ME2VR::CalcViewHook::SetAerFramePacing(GetPrivateProfileIntW(L"VR", L"AerFramePacing", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetAerFramePacingHz(ClampI(GetPrivateProfileIntW(L"VR", L"AerFramePacingHz", 0, path.c_str()), 0, 240, 0));
    ME2VR::CalcViewHook::SetStereoFramePacing(GetPrivateProfileIntW(L"VR", L"StereoFramePacing", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetFullRefreshPacing(GetPrivateProfileIntW(L"VR", L"FullRefreshPacing", 1, path.c_str()) != 0);
    ME2VR::D3DCapture::SetUiDupEnabled(GetPrivateProfileIntW(L"VR", L"UiDupEnabled", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetVrEnabled(GetPrivateProfileIntW(L"VR", L"Enabled", 1, path.c_str()) != 0);   // VR on by default
    ME2VR::D3DCapture::SetMonoMenus(GetPrivateProfileIntW(L"VR", L"MonoMenus", 1, path.c_str()) != 0);   // ME1-style: menus/map mono by default
    ME2VR::D3DCapture::SetMirrorPresentEvery(GetPrivateProfileIntW(L"VR", L"MirrorPresentEvery", 0, path.c_str()));   // [MIRRORREC] 0 = default 8
    ME2VR::D3DCapture::SetGm7NeedsPause(GetPrivateProfileIntW(L"VR", L"Gm7NeedsPause", 0, path.c_str()) != 0);   // [GM7PAUSE] experimental, default OFF (non-pausing menus broke mono menus)
    ME2VR::D3DCapture::SetPass0Anchor(GetPrivateProfileIntW(L"VR", L"Pass0Anchor", 1, path.c_str()) != 0);       // [PASS0ANCHOR] correct left-eye snapshot timing
    // [CLEANUP] the UI toggle for this is gone (see Tracking tab); force it ON regardless of what an
    // older ini says, so a stale MoveFollowsHead=0 from before this cleanup can't leave someone stuck
    // with the "look left, run right" bug this exists to fix, with no way back into the UI to undo it.
    SetMoveFollowsHead(true);                                                                            // [MOVEFIX] run where you look
    // ---- ME1 parity block (added 2026-07-21) ----
    ME2VR::Me2Xr::SetQuestFovMatch(GetPrivateProfileIntW(L"VR", L"QuestFovMatch", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetLeanInvertFwd(GetPrivateProfileIntW(L"VR", L"LeanInvertFwd", 0, path.c_str()) != 0);
    g_decoupledPitch.store(GetPrivateProfileIntW(L"VR", L"DecoupledPitch", 0, path.c_str()) != 0, std::memory_order_relaxed);
    g_decoupledYaw.store(GetPrivateProfileIntW(L"VR", L"DecoupledYaw", 0, path.c_str()) != 0, std::memory_order_relaxed);
    g_disableDof = GetPrivateProfileIntW(L"VR", L"DisableDof", 0, path.c_str()) != 0;
    ME2VR::Log::SetDiagnostics(GetPrivateProfileIntW(L"VR", L"Diagnostics", 0, path.c_str()) != 0);   // applied at next launch
    g_recenterKeyEnabled = GetPrivateProfileIntW(L"VR", L"RecenterKeyEnabled", 1, path.c_str()) != 0;
    g_menuKey = ClampI(GetPrivateProfileIntW(L"VR", L"MenuKey", VK_INSERT, path.c_str()), 1, 254, VK_INSERT);
    ME2VR::Me2Xr::SetVrFovFill(GetPrivateProfileIntW(L"VR", L"VrFovFill", 1, path.c_str()) != 0);
    GetPrivateProfileStringW(L"VR", L"VrFillH", L"1.0", buf, 64, path.c_str());
    ME2VR::Me2Xr::SetVrFillH(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"VrFillV", L"1.0", buf, 64, path.c_str());
    ME2VR::Me2Xr::SetVrFillV(static_cast<float>(_wtof(buf)));
    g_profileHotkeys = GetPrivateProfileIntW(L"VR", L"ProfileHotkeys", 1, path.c_str()) != 0;
    ME2VR::PcHud::SetEnabled(GetPrivateProfileIntW(L"HUD", L"PerElement", 1, path.c_str()) != 0);
    for (int g = 0; g < ME2VR::PcHud::GroupCount(); ++g)
    {
        ME2VR::PcHud::GroupCfg* c = ME2VR::PcHud::GetGroup(g);
        if (c == nullptr) continue;
        const HudGroupDefault gd = HudGroupDefaultFor(g);
        wchar_t k[32] = {}, def[32] = {};
        // NOTE: `def` (the ini-default STRING) and `buf` (the OUTPUT buffer the result lands in) must
        // be two separate buffers -- passing the same one for both means GetPrivateProfileStringW
        // reads its default argument out of the very buffer it is simultaneously overwriting.
        swprintf_s(k, L"G%d_offX", g);  swprintf_s(def, L"%.1f", gd.offX);  GetPrivateProfileStringW(L"HUD", k, def, buf, 64, path.c_str()); c->offX = ClampF(static_cast<float>(_wtof(buf)), -2000.0f, 2000.0f, gd.offX);
        swprintf_s(k, L"G%d_offY", g);  swprintf_s(def, L"%.1f", gd.offY);  GetPrivateProfileStringW(L"HUD", k, def, buf, 64, path.c_str()); c->offY = ClampF(static_cast<float>(_wtof(buf)), -2000.0f, 2000.0f, gd.offY);
        swprintf_s(k, L"G%d_scaleX", g); swprintf_s(def, L"%.2f", gd.scaleX); GetPrivateProfileStringW(L"HUD", k, def, buf, 64, path.c_str()); c->scaleX = ClampF(static_cast<float>(_wtof(buf)), 0.1f, 5.0f, gd.scaleX);
        swprintf_s(k, L"G%d_scaleY", g); swprintf_s(def, L"%.2f", gd.scaleY); GetPrivateProfileStringW(L"HUD", k, def, buf, 64, path.c_str()); c->scaleY = ClampF(static_cast<float>(_wtof(buf)), 0.1f, 5.0f, gd.scaleY);
    }
    for (int i = 0; i < kProfileCount; ++i)
    {
        wchar_t k[24] = {};
        swprintf_s(k, L"ProfileKey%d", i);
        g_profileKeys[i] = ClampI(GetPrivateProfileIntW(L"VR", k, '1' + i, path.c_str()), 1, 254, '1' + i);
    }
    ME2VR::EngineProbe::SetFpHeadHideDelay(GetPrivateProfileIntW(L"FirstPerson", L"HeadHideDelay", 20, path.c_str()));
    // [CONVOFP] fresh keys; default OFF so it ships inert until enabled.
    ME2VR::ConvoFp::SetEnabled(GetPrivateProfileIntW(L"FirstPerson", L"ConvoFirstPerson", 0, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetConvoFpInvertFacing(
        GetPrivateProfileIntW(L"FirstPerson", L"ConvoFpInvertFacing", 0, path.c_str()) != 0);
    GetPrivateProfileStringW(L"FirstPerson", L"ConvoFpTurnRate", L"90", buf, 64, path.c_str());
    ME2VR::ConvoFp::SetTurnRate(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"FirstPerson", L"ConvoFpAnimFollow", L"0.65", buf, 64, path.c_str());
    ME2VR::ConvoFp::SetAnimFollow(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"FirstPerson", L"ConvoFpEyeUpUU", L"0", buf, 64, path.c_str());
    ME2VR::ConvoFp::SetEyeUpUU(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"FirstPerson", L"ConvoFpZoom", L"1.0", buf, 64, path.c_str());
    ME2VR::ConvoFp::SetZoom(static_cast<float>(_wtof(buf)));
    ME2VR::ConvoFp::SetHideHead(GetPrivateProfileIntW(L"FirstPerson", L"ConvoFpHideHead", 1, path.c_str()) != 0);
    ME2VR::ConvoFp::SetKillDof(GetPrivateProfileIntW(L"FirstPerson", L"ConvoFpDisableDof", 1, path.c_str()) != 0);
    GetPrivateProfileStringW(L"VR", L"MenuPanelDist", L"1.5", buf, 64, path.c_str());
    ME2VR::Me2Xr::SetMenuPanelDist(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"MenuPanelSize", L"0.90", buf, 64, path.c_str());   // baked 2026-07-26
    ME2VR::Me2Xr::SetMenuPanelSize(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"MenuPanelOffX", L"0.0", buf, 64, path.c_str());
    ME2VR::Me2Xr::SetMenuPanelOffX(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"MenuPanelOffY", L"0.0", buf, 64, path.c_str());
    ME2VR::Me2Xr::SetMenuPanelOffY(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"PoseTagDelay", L"2.0", buf, 64, path.c_str()); ME2VR::CalcViewHook::SetPoseTagDelayFrames(static_cast<float>(_wtof(buf)));
    ME2VR::EngineProbe::SetVehicleHeadMode(GetPrivateProfileIntW(L"VR", L"VehicleHeadMode", 3, path.c_str()));   // 0 off/1 free/2 cam+view/3 cam fixed; baked 2026-07-26
    // VR conversations/cutscenes are DEFAULT ON as of 2026-07-31 - no longer experimental. They were
    // the headline of testing ("cutscenes now working in full VR are amazing"), and the
    // stereo-loss bugs that made them unreliable are fixed ([PASS1DOUBLE]/[GAMEPLAYVR]/[ENGCINE]).
    ME2VR::CalcViewHook::SetCineVrConvo(GetPrivateProfileIntW(L"VR", L"CineVrConvo", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetCineVrCutscene(GetPrivateProfileIntW(L"VR", L"CineVrCutscene", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetCineVrHeadTracking(GetPrivateProfileIntW(L"VR", L"CineVrHeadTracking", 1, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetEngCineAlive(GetPrivateProfileIntW(L"VR", L"EngCineAlive", 1, path.c_str()) != 0);        // [ENGCINE] gm8+alive = cutscene (default on)
    ME2VR::CalcViewHook::SetCineEnterFrames(GetPrivateProfileIntW(L"VR", L"CineEnterFrames", 8, path.c_str()));       // [CINEDEBOUNCE] 0 = old instant entry
    GetPrivateProfileStringW(L"VR", L"CineScreenZoom", L"1.0", buf, 64, path.c_str());
    ME2VR::Me2Xr::SetCineScreenZoom(static_cast<float>(_wtof(buf)));
    // [CINEXCL] first-person conversations and VR conversations/cutscenes are mutually exclusive
    // (both put conversations in VR). A both-on ini (hand-edited, or written before this rule) is
    // resolved in favour of first-person conversations -- the more specific choice -- and logged.
    if (ME2VR::ConvoFp::GetEnabled() &&
        (ME2VR::CalcViewHook::GetCineVrConvo() || ME2VR::CalcViewHook::GetCineVrCutscene()))
    {
        ME2VR::CalcViewHook::SetCineVrConvo(false);
        ME2VR::CalcViewHook::SetCineVrCutscene(false);
        ME2VR::Log::Line("[CINEXCL] ini had BOTH first-person conversations and VR cine on -> kept first-person, VR cine off");
    }
    GetPrivateProfileStringW(L"VR", L"MenuScreenDist", L"1.6", buf, 64, path.c_str()); ME2VR::Me2Xr::SetMenuScreenDist(static_cast<float>(_wtof(buf)));
    GetPrivateProfileStringW(L"VR", L"MenuScreenSize", L"2.2", buf, 64, path.c_str()); ME2VR::Me2Xr::SetMenuScreenSize(static_cast<float>(_wtof(buf)));
    // [SFR-UI] HUD master transform (tuned in the HUD tab). ONE-TIME MIGRATION at the [UIRATIO]
    // switch: pre-ratio inis carry a manual un-stretch (HudScaleX ~0.55) that would now double-shrink
    // on top of the automatic tan-ratio - reset the trims to neutral once, marked by UiRatioMigrated.
    if (GetPrivateProfileIntW(L"VR", L"UiRatioMigrated", 0, path.c_str()) == 0)
    {
        ME2VR::CalcViewHook::SetSfrUiScaleX(1.0f); ME2VR::CalcViewHook::SetSfrUiScaleY(1.0f);
        ME2VR::CalcViewHook::SetSfrUiOffX(0.0f);   ME2VR::CalcViewHook::SetSfrUiOffY(0.0f);
        WritePrivateProfileStringW(L"VR", L"UiRatioMigrated", L"1", path.c_str());
    }
    else
    {
        GetPrivateProfileStringW(L"VR", L"HudScaleX", L"1.0", buf, 64, path.c_str()); ME2VR::CalcViewHook::SetSfrUiScaleX(static_cast<float>(_wtof(buf)));
        GetPrivateProfileStringW(L"VR", L"HudScaleY", L"1.0", buf, 64, path.c_str()); ME2VR::CalcViewHook::SetSfrUiScaleY(static_cast<float>(_wtof(buf)));
        GetPrivateProfileStringW(L"VR", L"HudOffX",   L"0.0", buf, 64, path.c_str()); ME2VR::CalcViewHook::SetSfrUiOffX(static_cast<float>(_wtof(buf)));
        GetPrivateProfileStringW(L"VR", L"HudOffY",   L"0.0", buf, 64, path.c_str()); ME2VR::CalcViewHook::SetSfrUiOffY(static_cast<float>(_wtof(buf)));
    }
    g_recenterKey = GetPrivateProfileIntW(L"VR", L"RecenterKey", 'R', path.c_str());
    g_fpToggleKey = GetPrivateProfileIntW(L"VR", L"FpToggleKey", 'K', path.c_str());
    GetPrivateProfileStringW(L"VR", L"PosScale", L"2.0", buf, 64, path.c_str());   // baked 2026-07-26
    ME2VR::CalcViewHook::SetHeadPosScale(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 12.0f, 2.0f));
    ME2VR::CalcViewHook::SetHeadPosEnabled(GetPrivateProfileIntW(L"VR", L"PosEnabled", 1, path.c_str()) != 0);
    ME2VR::EngineProbe::SetHeadAimEnabled(GetPrivateProfileIntW(L"VR", L"HeadAim", 1, path.c_str()) != 0);
    ME2VR::EngineProbe::SetHeadAimInvertYaw(GetPrivateProfileIntW(L"VR", L"HeadAimInvYaw", 0, path.c_str()) != 0);
    ME2VR::EngineProbe::SetHeadAimInvertPitch(GetPrivateProfileIntW(L"VR", L"HeadAimInvPitch", 0, path.c_str()) != 0);
    // Head-look tuning (ME1 parity - smoothing is the shake fix).
    ME2VR::CalcViewHook::SetHeadLookUserEnabled(GetPrivateProfileIntW(L"VR", L"HeadLook", 1, path.c_str()) != 0);
    GetPrivateProfileStringW(L"VR", L"HeadLookSmoothing", L"0.4", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetHeadLookSmoothing(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 0.95f, 0.4f));
    GetPrivateProfileStringW(L"VR", L"LookSensitivity", L"1.0", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetLookSensitivity(ClampF(static_cast<float>(_wtof(buf)), 0.1f, 3.0f, 1.0f));
    ME2VR::CalcViewHook::SetInvertLookYaw(GetPrivateProfileIntW(L"VR", L"InvertLookYaw", 0, path.c_str()) != 0);
    ME2VR::CalcViewHook::SetInvertLookPitch(GetPrivateProfileIntW(L"VR", L"InvertLookPitch", 0, path.c_str()) != 0);
    // DIBR (depth warp) keys.
    GetPrivateProfileStringW(L"VR", L"DepthWarpGain", L"3.5", buf, 64, path.c_str());   // baked 2026-07-26
    ME2VR::CalcViewHook::SetDepthWarpGain(ClampF(static_cast<float>(_wtof(buf)), 0.0f, 10.0f, 3.5f));
    GetPrivateProfileStringW(L"VR", L"DepthWarpConv", L"0.985", buf, 64, path.c_str());
    ME2VR::CalcViewHook::SetDepthWarpConv(ClampF(static_cast<float>(_wtof(buf)), 0.90f, 1.005f, 0.985f));
    ME2VR::CalcViewHook::SetDepthWarpFlip(GetPrivateProfileIntW(L"VR", L"DepthWarpFlip", 0, path.c_str()) != 0);   // baked 2026-07-26
    ME2VR::CalcViewHook::SetDibrAutoConverge(GetPrivateProfileIntW(L"VR", L"DibrAutoConverge", 1, path.c_str()) != 0);
    // [INVMAT] stale-inverse fix (LE1 dark-panel root cause) - default ON.
    // [CLEANUP] no UI toggle any more (see Tracking/View); force ON regardless of a stale ini value.
    ME2VR::CalcViewHook::SetInvMatrixFix(true);
    LoadFpStates(path);
    g_valuesLoaded = true;
}

void SaveValues() noexcept
{
    const std::wstring path = IniPath();
    wchar_t buf[64] = {};
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetHalfEyeUU());
    WritePrivateProfileStringW(L"VR", L"HalfEyeUU", buf, path.c_str());
    swprintf_s(buf, L"%.4f", ME2VR::CalcViewHook::GetSfrConvergence());
    WritePrivateProfileStringW(L"VR", L"Convergence", buf, path.c_str());
    // ---- ME1 parity block ----
    WritePrivateProfileStringW(L"VR", L"QuestFovMatch", ME2VR::Me2Xr::GetQuestFovMatch() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"LeanInvertFwd", ME2VR::CalcViewHook::GetLeanInvertFwd() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"DecoupledPitch", g_decoupledPitch.load(std::memory_order_relaxed) ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"DecoupledYaw", g_decoupledYaw.load(std::memory_order_relaxed) ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"DisableDof", g_disableDof ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"Diagnostics", ME2VR::Log::DiagnosticsOn() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"RecenterKeyEnabled", g_recenterKeyEnabled ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", g_menuKey);       WritePrivateProfileStringW(L"VR", L"MenuKey", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"VrFovFill", ME2VR::Me2Xr::GetVrFovFill() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::Me2Xr::GetVrFillH()); WritePrivateProfileStringW(L"VR", L"VrFillH", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::Me2Xr::GetVrFillV()); WritePrivateProfileStringW(L"VR", L"VrFillV", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"ProfileHotkeys", g_profileHotkeys ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"HUD", L"PerElement", ME2VR::PcHud::GetEnabled() ? L"1" : L"0", path.c_str());
    for (int g = 0; g < ME2VR::PcHud::GroupCount(); ++g)
    {
        const ME2VR::PcHud::GroupCfg* c = ME2VR::PcHud::GetGroup(g);
        if (c == nullptr) continue;
        wchar_t k[32] = {};
        swprintf_s(k, L"G%d_offX", g);  swprintf_s(buf, L"%.1f", c->offX);  WritePrivateProfileStringW(L"HUD", k, buf, path.c_str());
        swprintf_s(k, L"G%d_offY", g);  swprintf_s(buf, L"%.1f", c->offY);  WritePrivateProfileStringW(L"HUD", k, buf, path.c_str());
        swprintf_s(k, L"G%d_scaleX", g); swprintf_s(buf, L"%.3f", c->scaleX); WritePrivateProfileStringW(L"HUD", k, buf, path.c_str());
        swprintf_s(k, L"G%d_scaleY", g); swprintf_s(buf, L"%.3f", c->scaleY); WritePrivateProfileStringW(L"HUD", k, buf, path.c_str());
    }
    for (int i = 0; i < kProfileCount; ++i)
    {
        wchar_t k[24] = {};
        swprintf_s(k, L"ProfileKey%d", i);
        swprintf_s(buf, L"%d", g_profileKeys[i]);
        WritePrivateProfileStringW(L"VR", k, buf, path.c_str());
    }
    swprintf_s(buf, L"%d", ME2VR::EngineProbe::GetFpHeadHideDelay());
    WritePrivateProfileStringW(L"FirstPerson", L"HeadHideDelay", buf, path.c_str());
    // [CONVOFP]
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFirstPerson",
                               ME2VR::ConvoFp::GetEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpInvertFacing",
                               ME2VR::CalcViewHook::GetConvoFpInvertFacing() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::ConvoFp::GetTurnRate());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpTurnRate", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::ConvoFp::GetAnimFollow());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpAnimFollow", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::ConvoFp::GetEyeUpUU());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpEyeUpUU", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::ConvoFp::GetZoom());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpZoom", buf, path.c_str());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpHideHead",
                               ME2VR::ConvoFp::GetHideHead() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"FirstPerson", L"ConvoFpDisableDof",
                               ME2VR::ConvoFp::GetKillDof() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuPanelDist()); WritePrivateProfileStringW(L"VR", L"MenuPanelDist", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuPanelSize()); WritePrivateProfileStringW(L"VR", L"MenuPanelSize", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuPanelOffX()); WritePrivateProfileStringW(L"VR", L"MenuPanelOffX", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetMenuPanelOffY()); WritePrivateProfileStringW(L"VR", L"MenuPanelOffY", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"SwapEyes", ME2VR::CalcViewHook::GetSwapEyes() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", ME2VR::CalcViewHook::GetVrMode());
    WritePrivateProfileStringW(L"VR", L"Mode", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetAerHalfEyeUU());
    WritePrivateProfileStringW(L"VR", L"AerHalfEyeUU", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"AerSwapEyes", ME2VR::CalcViewHook::GetAerSwapEyes() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"AerFramePacing", ME2VR::CalcViewHook::GetAerFramePacing() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", ME2VR::CalcViewHook::GetAerFramePacingHz());
    WritePrivateProfileStringW(L"VR", L"AerFramePacingHz", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"StereoFramePacing", ME2VR::CalcViewHook::GetStereoFramePacing() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"FullRefreshPacing", ME2VR::CalcViewHook::GetFullRefreshPacing() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"UiDupEnabled", ME2VR::D3DCapture::GetUiDupEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"Enabled", ME2VR::CalcViewHook::GetVrEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"MonoMenus", ME2VR::D3DCapture::GetMonoMenus() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", ME2VR::D3DCapture::GetMirrorPresentEvery());   // [MIRRORREC]
    WritePrivateProfileStringW(L"VR", L"MirrorPresentEvery", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"Gm7NeedsPause", ME2VR::D3DCapture::GetGm7NeedsPause() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"Pass0Anchor", ME2VR::D3DCapture::GetPass0Anchor() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"MoveFollowsHead", GetMoveFollowsHead() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetPoseTagDelayFrames()); WritePrivateProfileStringW(L"VR", L"PoseTagDelay", buf, path.c_str());
    swprintf_s(buf, L"%d", ME2VR::EngineProbe::GetVehicleHeadMode()); WritePrivateProfileStringW(L"VR", L"VehicleHeadMode", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"CineVrConvo", ME2VR::CalcViewHook::GetCineVrConvo() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"CineVrCutscene", ME2VR::CalcViewHook::GetCineVrCutscene() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"CineVrHeadTracking", ME2VR::CalcViewHook::GetCineVrHeadTracking() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"EngCineAlive", ME2VR::CalcViewHook::GetEngCineAlive() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%d", ME2VR::CalcViewHook::GetCineEnterFrames()); WritePrivateProfileStringW(L"VR", L"CineEnterFrames", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::Me2Xr::GetCineScreenZoom()); WritePrivateProfileStringW(L"VR", L"CineScreenZoom", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::Me2Xr::GetMenuScreenDist()); WritePrivateProfileStringW(L"VR", L"MenuScreenDist", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::Me2Xr::GetMenuScreenSize()); WritePrivateProfileStringW(L"VR", L"MenuScreenSize", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetSfrUiScaleX()); WritePrivateProfileStringW(L"VR", L"HudScaleX", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetSfrUiScaleY()); WritePrivateProfileStringW(L"VR", L"HudScaleY", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetSfrUiOffX());   WritePrivateProfileStringW(L"VR", L"HudOffX", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetSfrUiOffY());   WritePrivateProfileStringW(L"VR", L"HudOffY", buf, path.c_str());
    swprintf_s(buf, L"%d", g_recenterKey);
    WritePrivateProfileStringW(L"VR", L"RecenterKey", buf, path.c_str());
    swprintf_s(buf, L"%d", g_fpToggleKey);
    WritePrivateProfileStringW(L"VR", L"FpToggleKey", buf, path.c_str());
    swprintf_s(buf, L"%.2f", ME2VR::CalcViewHook::GetHeadPosScale());
    WritePrivateProfileStringW(L"VR", L"PosScale", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"PosEnabled", ME2VR::CalcViewHook::GetHeadPosEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadAim", ME2VR::EngineProbe::GetHeadAimEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadAimInvYaw", ME2VR::EngineProbe::GetHeadAimInvertYaw() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadAimInvPitch", ME2VR::EngineProbe::GetHeadAimInvertPitch() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"HeadLook", ME2VR::CalcViewHook::GetHeadLookUserEnabled() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetHeadLookSmoothing());
    WritePrivateProfileStringW(L"VR", L"HeadLookSmoothing", buf, path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetLookSensitivity());
    WritePrivateProfileStringW(L"VR", L"LookSensitivity", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"InvertLookYaw", ME2VR::CalcViewHook::GetInvertLookYaw() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"InvertLookPitch", ME2VR::CalcViewHook::GetInvertLookPitch() ? L"1" : L"0", path.c_str());
    swprintf_s(buf, L"%.3f", ME2VR::CalcViewHook::GetDepthWarpGain());
    WritePrivateProfileStringW(L"VR", L"DepthWarpGain", buf, path.c_str());
    swprintf_s(buf, L"%.4f", ME2VR::CalcViewHook::GetDepthWarpConv());
    WritePrivateProfileStringW(L"VR", L"DepthWarpConv", buf, path.c_str());
    WritePrivateProfileStringW(L"VR", L"DepthWarpFlip", ME2VR::CalcViewHook::GetDepthWarpFlip() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"DibrAutoConverge", ME2VR::CalcViewHook::GetDibrAutoConverge() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"VR", L"InvMatrixFix", ME2VR::CalcViewHook::GetInvMatrixFix() ? L"1" : L"0", path.c_str());
    SaveFpStates(path);
    g_savedFlash = true;
    g_savedFrames = 120;
    ME2VR::Log::Line("[ME2MENU] settings saved to MELE2VR.ini");
}

// ---- [PROFILES] slot files live next to the live ini: MELE2VR.p1.ini ... p3.ini ----
std::wstring ProfilePath(int slot) noexcept
{
    std::wstring p = IniPath();
    if (slot <= 0) return p;                       // slot 0 IS the live file
    const size_t dot = p.rfind(L".ini");
    if (dot == std::wstring::npos) return p;
    wchar_t suffix[16] = {};
    swprintf_s(suffix, L".p%d.ini", slot);
    return p.substr(0, dot) + suffix;
}

// Write the CURRENT settings into a slot. Slot 0 is just a normal save.
void SaveProfile(int slot) noexcept
{
    SaveValues();                                   // flush live state to the live ini first
    if (slot > 0) CopyFileW(IniPath().c_str(), ProfilePath(slot).c_str(), FALSE);
    g_activeProfile = slot;
    ME2VR::Log::Line("[PROFILES] saved -> " + std::string(ProfileName(slot)));
}

// Switch to a slot: copy that snapshot over the live ini and re-read everything. A slot that has
// never been saved has no file, so the mod keeps the current settings rather than wiping them.
void LoadProfile(int slot) noexcept
{
    if (slot < 0 || slot >= kProfileCount) return;
    if (slot > 0)
    {
        const std::wstring src = ProfilePath(slot);
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            g_activeProfile = slot;                 // empty slot: adopt it, keep current values
            ME2VR::Log::Line("[PROFILES] " + std::string(ProfileName(slot)) + " is empty - keeping current settings");
            return;
        }
        CopyFileW(src.c_str(), IniPath().c_str(), FALSE);
    }
    else if (g_activeProfile > 0)
    {
        // Returning to Default: its snapshot is p0 if the mod ever made one, else the live file already is it.
        const std::wstring src = ProfilePath(0);
        (void)src;
    }
    g_activeProfile = slot;
    LoadValues();
    ME2VR::Log::Line("[PROFILES] loaded " + std::string(ProfileName(slot)));
}

void SeedVirtualMouse() noexcept
{
    g_mouseAnchorValid = false;
    POINT pt = {};
    if (!GetCursorPos(&pt)) return;
    HWND hwnd = g_hwnd ? g_hwnd : GetForegroundWindow();
    RECT rc = {}; POINT client = pt;
    if (hwnd && ScreenToClient(hwnd, &client) && GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0)
    {
        g_virtualMouseX = ClampF(static_cast<float>(client.x) / rc.right * g_w, 0.0f, static_cast<float>(g_w - 1), g_w * 0.5f);
        g_virtualMouseY = ClampF(static_cast<float>(client.y) / rc.bottom * g_h, 0.0f, static_cast<float>(g_h - 1), g_h * 0.5f);
    }
    else { g_virtualMouseX = g_w * 0.5f; g_virtualMouseY = g_h * 0.5f; }
    g_mouseAnchorScreen = pt;
    g_mouseAnchorValid = true;
    ClipCursor(nullptr);
    ShowCursor(TRUE);
}

LRESULT CALLBACK MenuWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (g_open)
    {
        switch (msg)
        {
        case WM_INPUT: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
            if (static_cast<int>(w) != g_menuKey) return 0;
            break;
        default: break;
        }
    }
    return CallWindowProcW(g_originalWndProc, h, msg, w, l);
}

void SetGameWindow(HWND hwnd) noexcept
{
    if (hwnd == nullptr || hwnd == g_hwnd) return;
    if (g_hwnd && g_originalWndProc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    g_hwnd = hwnd;
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWndProc)));
}

void PollInsert() noexcept
{
    const bool down = (GetAsyncKeyState(g_menuKey) & 0x8000) != 0;
    if (down && !g_insertWasDown)
    {
        g_open = !g_open;
        if (g_open) { if (!g_valuesLoaded) LoadValues(); SeedVirtualMouse(); }
        else
        {
            g_mouseAnchorValid = false;
            // [MENUSAVE] Settings only persisted via the explicit Save buttons, so a checkbox toggled
            // in-headset silently reverted on the next boot - a test would then run with the opposite
            // setting and read as a render failure ("cutscenes are flat") when it was a deliberate
            // choice nobody made. Same trap ME3 hit on 2026-07-30. Closing the menu now flushes.
            SaveValues();
        }
        // Auto-recenter on every Insert press: opening/closing the menu is the natural "reset the view" moment,
        // and it re-syncs the headset frame to the game camera if they've drifted apart.
        ME2VR::Me2Xr::Recenter();
        ME2VR::Log::Line(std::string("[ME2MENU] Insert menu ") + (g_open ? "opened" : "closed") + " (+recenter)");
    }
    g_insertWasDown = down;

    // [PROFILES] switch hotkeys (ME1 parity, default 1-4). Deliberately NOT gated on the menu being
    // closed - ME1 allows switching while it's open, and mid-fight profile swaps are the whole point.
    // Suppressed only while a rebind is capturing, so binding a key can't also trigger a switch.
    if (g_profileHotkeys && g_rebindingProfile < 0 && !g_rebindingMenuKey && !g_rebindingRecenter)
    {
        static bool s_profDown[kProfileCount] = {};
        for (int i = 0; i < kProfileCount; ++i)
        {
            const bool d = (GetAsyncKeyState(g_profileKeys[i]) & 0x8000) != 0;
            if (d && !s_profDown[i] && g_activeProfile != i) LoadProfile(i);
            s_profDown[i] = d;
        }
    }
}

void FeedMouse() noexcept
{
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = true;
    ClipCursor(nullptr);
    POINT pt = {};
    if (!g_mouseAnchorValid) SeedVirtualMouse();
    if (GetCursorPos(&pt) && g_mouseAnchorValid)
    {
        g_virtualMouseX = ClampF(g_virtualMouseX + (pt.x - g_mouseAnchorScreen.x), 0.0f, static_cast<float>(g_w - 1), g_w * 0.5f);
        g_virtualMouseY = ClampF(g_virtualMouseY + (pt.y - g_mouseAnchorScreen.y), 0.0f, static_cast<float>(g_h - 1), g_h * 0.5f);
        SetCursorPos(g_mouseAnchorScreen.x, g_mouseAnchorScreen.y);   // pin the real cursor
    }
    io.AddMousePosEvent(g_virtualMouseX, g_virtualMouseY);
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
}

void BuildUI() noexcept
{
    // Draggable + resizable window (title bar = drag handle); initial size/pos only on first use.
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(g_w * 0.5f - 270.0f, g_h * 0.5f - 215.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("MASS EFFECT 2 - VR", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    // Keep the window fully inside the menu's own backing texture (g_w x g_h). ImGui itself is happy
    // to let a window be dragged anywhere, including past the edge of io.DisplaySize -- but that
    // "anywhere" is still just the same fixed-size offscreen texture this whole panel is rendered
    // into, so any part dragged past its bounds is not merely off THIS window, it is off the texture
    // entirely and never makes it into the headset. That is the "drag it down and it cuts off"
    // report. Clamp every frame so dragging simply stops at the edge instead of vanishing past it.
    {
        const ImVec2 wpos = ImGui::GetWindowPos();
        const ImVec2 wsize = ImGui::GetWindowSize();
        const float maxX = (std::max)(0.0f, static_cast<float>(g_w) - wsize.x);
        const float maxY = (std::max)(0.0f, static_cast<float>(g_h) - wsize.y);
        const float cx = ClampF(wpos.x, 0.0f, maxX, 0.0f);
        const float cy = ClampF(wpos.y, 0.0f, maxY, 0.0f);
        if (cx != wpos.x || cy != wpos.y) ImGui::SetWindowPos(ImVec2(cx, cy));
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs"))
    {
        if (ImGui::BeginTabItem("Tracking"))
        {
            // Reorganized 2026-07-26 to match ME1's shipped tab: named, collapsible sections instead
            // of one long column, and gating limited to real overrides/hazards -- not "advanced-sounding"
            // controls. ME1's own rule (verbatim from its source): un-gate anything "genuinely per-user",
            // gate only what's a dev/diagnostic knob or can misbehave if fiddled with blind.
            if (ImGui::CollapsingHeader("Recenter", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Button("Recenter view")) ME2VR::Me2Xr::Recenter();
                ImGui::SameLine();
                ImGui::Text("Hotkey: %s", KeyName(g_recenterKey));
                ImGui::SameLine();
                // Button label carries the prompt (matches Menu-key/Profile-hotkey rebind elsewhere in
                // this file) instead of a separate text line.
                if (ImGui::SmallButton(g_rebindingRecenter ? "press a key..." : "Rebind"))
                    g_rebindingRecenter = true;
                if (g_rebindingRecenter)
                {
                    for (int vk = 0x08; vk <= 0xFE; ++vk)
                    {
                        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_INSERT) continue;
                        if (GetAsyncKeyState(vk) & 0x8000)
                        {
                            if (vk != VK_ESCAPE) { g_recenterKey = vk; SaveValues(); }
                            g_rebindingRecenter = false;
                            break;
                        }
                    }
                }
                ImGui::TextDisabled("Gamepad: double-tap Back to recenter.");
            }

            // Head-look (view rotation) - ME1 parity. Smoothing is THE fix for the shaky look-left/right.
            // [CLEANUP] "Panel fix: refresh inverse matrices" REMOVED from the UI (was also duplicated on
            // the View tab) -- matching ME1's own call on this exact control verbatim: "a confirmed
            // always-on bug fix with no reason for anyone to ever touch it. No toggle." Forced on at load
            // (LoadValues), not read from a possibly-stale ini value, so an old install can't get stuck
            // with the dark fog-band bug this exists to prevent.
            if (ImGui::CollapsingHeader("Head Look (explore)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool hle = ME2VR::CalcViewHook::GetHeadLookUserEnabled();
                if (ImGui::Checkbox("Head tracking enabled (view look-around)", &hle)) ME2VR::CalcViewHook::SetHeadLookUserEnabled(hle);
                float ptd = ME2VR::CalcViewHook::GetPoseTagDelayFrames();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Tracking smoothness (pose-tag delay)", &ptd, 0.0f, 3.0f, "%.2f frames"))
                    ME2VR::CalcViewHook::SetPoseTagDelayFrames(ptd);
                ImGui::TextDisabled("Raise if the world drags with your head; lower if it leads ahead.");
                float hls = ME2VR::CalcViewHook::GetHeadLookSmoothing();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Head-look smoothing", &hls, 0.0f, 0.95f, "%.2f"))
                    ME2VR::CalcViewHook::SetHeadLookSmoothing(ClampF(hls, 0.0f, 0.95f, 0.4f));
                float ls = ME2VR::CalcViewHook::GetLookSensitivity();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Look sensitivity", &ls, 0.25f, 2.0f, "%.2f"))
                    ME2VR::CalcViewHook::SetLookSensitivity(ClampF(ls, 0.1f, 3.0f, 1.0f));
                bool ily = ME2VR::CalcViewHook::GetInvertLookYaw();
                if (ImGui::Checkbox("Invert look yaw", &ily)) ME2VR::CalcViewHook::SetInvertLookYaw(ily);
                ImGui::SameLine();
                bool ilp = ME2VR::CalcViewHook::GetInvertLookPitch();
                if (ImGui::Checkbox("Invert look pitch", &ilp)) ME2VR::CalcViewHook::SetInvertLookPitch(ilp);
            }

            // [CLEANUP] Positional head tracking (6DOF) / Positional scale REMOVED from this tab -- it was
            // a straight duplicate of the "Lean (6DOF)" section on the View tab (same getter/setter, so
            // moving one silently moved the other). ME1 only ever had lean on its View tab; kept there.

            if (ImGui::CollapsingHeader("Head Aim", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool ha = ME2VR::EngineProbe::GetHeadAimEnabled();
                if (ImGui::Checkbox("Head drives aim (combat)", &ha)) ME2VR::EngineProbe::SetHeadAimEnabled(ha);
                bool iy = ME2VR::EngineProbe::GetHeadAimInvertYaw();
                if (ImGui::Checkbox("Invert aim yaw", &iy)) ME2VR::EngineProbe::SetHeadAimInvertYaw(iy);
                ImGui::SameLine();
                bool ip = ME2VR::EngineProbe::GetHeadAimInvertPitch();
                if (ImGui::Checkbox("Invert aim pitch", &ip)) ME2VR::EngineProbe::SetHeadAimInvertPitch(ip);

                bool dp = g_decoupledPitch.load(std::memory_order_relaxed);
                if (ImGui::Checkbox("Decoupled pitch (look up/down with head only)", &dp))
                    g_decoupledPitch.store(dp, std::memory_order_relaxed);
                bool dy = g_decoupledYaw.load(std::memory_order_relaxed);
                if (ImGui::Checkbox("Decoupled yaw (turn left/right with head only)", &dy))
                    g_decoupledYaw.store(dy, std::memory_order_relaxed);

                // [CLEANUP] "Run where you look" checkbox REMOVED -- ME1's own call on this exact
                // mechanism (its comment, verbatim): "it shouldn't [be a toggle] anyway... this is the
                // confirmed FP-storm fix (the 'look left, move right' bug), not an experiment; a user
                // unchecking it would silently reintroduce the bug with no visible cause." Forced on at
                // load, same as ME1's config default.

                ImGui::TextUnformatted("Vehicle head (Hammerhead)");
                int vmode = ME2VR::EngineProbe::GetVehicleHeadMode();
                static const char* const kVehLabels[] = {
                    "Off (stick only)", "Free look (head turns view)",
                    "Cannon aim + view (view follows)", "Cannon aim, fixed view (decoupled)" };
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::Combo("##vehmode", &vmode, kVehLabels, 4)) ME2VR::EngineProbe::SetVehicleHeadMode(vmode);
            }

            if (ImGui::CollapsingHeader("Menus & Map", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool monoMenus = ME2VR::D3DCapture::GetMonoMenus();
                if (ImGui::Checkbox("Mono menus & map (like ME1)", &monoMenus)) ME2VR::D3DCapture::SetMonoMenus(monoMenus);

                // [MIRRORREC 2026-08-22] Desktop-mirror rate, for screen recording. In VR the flat window is
                // only a mirror - the headset image is already submitted before the mirror present - so it is
                // presented 1 frame in N. That throttle is not cosmetic: presenting every frame creates DWM
                // back-pressure that stalls the render thread, which is what capped ME2 at 60fps (87 -> 119.7
                // when throttled) and cost ME1 5.8ms -> 14.2ms per frame on identical work. It changes NOTHING
                // about resolution, image quality or the headset view - only the monitor picture and the frame
                // rate - so it is opt-in and the label says what it costs.
                {
                    const int cur = ME2VR::D3DCapture::GetMirrorPresentEvery();
                    int sel = (cur <= 1) ? 2 : ((cur <= 2) ? 1 : 0);
                    static const char* const kMirrorItems[] = { "Normal (best frame rate)",
                                                                "Smooth - every 2nd frame",
                                                                "Every frame (smoothest, costs the most)" };
                    ImGui::SetNextItemWidth(360.0f);
                    if (ImGui::Combo("Desktop mirror (screen recording)", &sel, kMirrorItems, 3))
                    {
                        const int v = (sel == 2) ? 1 : ((sel == 1) ? 2 : 8);
                        ME2VR::D3DCapture::SetMirrorPresentEvery(v); SaveValues();
                    }
                    ImGui::TextDisabled("Only the picture on your monitor. Does not change resolution, quality or");
                    ImGui::TextDisabled("the headset image. Raise it to record, put it back to Normal to play.");
                }

                // [GM7PAUSE] The engine reports GUI mode 7 for blocking menus AND for live gameplay with
                // the HUD up, so the old always-flat rule dropped combat to a flat panel. Only menus pause.
                // [GM7PAUSE] Hidden 2026-07-31, default OFF. It made mode 7 go flat only while the engine
                // was PAUSED, to stop the world flattening when a HUD element was up. But some full-screen
                // menus never set Pauser, so they stayed in stereo = visibly broken mono menus.
                // The combat flattening it targeted was mostly the capture and cine-latch bugs, fixed
                // properly since, so the trade was all cost and no benefit. Still reachable for testing
                // via VR/Gm7NeedsPause in the ini; no UI, because nobody should find this by accident.

                // [PASS0ANCHOR] Hidden 2026-07-31. This is the "VR dies after a cutscene" fix (left-eye
                // snapshot timing) and there is no legitimate reason to turn it OFF - off is the bug.
                // It only ever existed as an A/B switch while the fix was being proven. Still reachable
                // via VR/Pass0Anchor in the ini for debugging. Same for [ENGCINE] below.
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: menu mode 7 only goes flat while the game is actually paused,\n"
                                      "so objectives/tutorials/aiming no longer flatten combat.\n"
                                      "OFF: old behaviour (any mode 7 = flat panel).\n"
                                      "Live now: mode %d, %s, %s",
                                      ME2VR::D3DCapture::GetAutoGameMode(),
                                      ME2VR::D3DCapture::GetAutoPaused() ? "paused" : "live",
                                      ME2VR::D3DCapture::GetMenuMode() ? "FLAT panel" : "VR");

                // "Flat panel" (not "Menu screen") to avoid the name clash with Comfort's "Menu
                // distance/size", which sizes THIS Insert panel, not the game's own menus.
                float md = ME2VR::Me2Xr::GetMenuScreenDist();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Flat panel distance", &md, 0.4f, 4.0f, "%.2f m")) ME2VR::Me2Xr::SetMenuScreenDist(md);
                float msz = ME2VR::Me2Xr::GetMenuScreenSize();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Flat panel size", &msz, 0.8f, 4.0f, "%.2f m")) ME2VR::Me2Xr::SetMenuScreenSize(msz);

                {
                    bool flat = ME2VR::D3DCapture::GetMenuModeManual();
                    if (ImGui::Checkbox("Flat screen mode (manual override)", &flat)) ME2VR::D3DCapture::SetMenuMode(flat);
                }
                ImGui::TextDisabled("F2 always toggles flat/VR manually.");
            }
            ImGui::EndTabItem();
        }

        // ==== TAB: First Person (per-state eye offsets, ME1-style) ====
        if (ImGui::BeginTabItem("First Person"))
        {
            bool fpOn = ME2VR::EngineProbe::GetFirstPerson();
            if (ImGui::Checkbox("First person enabled", &fpOn)) ME2VR::EngineProbe::SetFirstPerson(fpOn);
            ImGui::SameLine();
            ImGui::Text("Hotkey: %s", KeyName(g_fpToggleKey));
            ImGui::SameLine();
            if (ImGui::SmallButton(g_rebindingFpToggle ? "press a key...##fp" : "Rebind##fp"))
                g_rebindingFpToggle = true;
            if (g_rebindingFpToggle)
            {
                for (int vk = 0x08; vk <= 0xFE; ++vk)
                {
                    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_INSERT) continue;
                    if (GetAsyncKeyState(vk) & 0x8000)
                    {
                        if (vk != VK_ESCAPE) { g_fpToggleKey = vk; SaveValues(); }
                        g_rebindingFpToggle = false;
                        break;
                    }
                }
            }
            {
                int hhd = ME2VR::EngineProbe::GetFpHeadHideDelay();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderInt("Head hide delay (frames)", &hhd, 0, 60))
                    ME2VR::EngineProbe::SetFpHeadHideDelay(hhd);
            }
            ImGui::TextDisabled("Each camera state below has its own eye offset (Forward/Up/Right).");
            ImGui::TextDisabled("Cover, sniper, the Mako, and cutscenes stay third person, same as ME1.");
            ImGui::Separator();

            auto fpRow = [&](int id)
            {
                ME2VR::EngineProbe::FpStateCfg* s = ME2VR::EngineProbe::GetFpStateCfg(id);
                if (s == nullptr) return;
                ImGui::PushID(id);
                if (ImGui::CollapsingHeader(ME2VR::EngineProbe::FpStateLabel(id)))
                {
                    if (ImGui::SmallButton("Reset to default")) *s = ME2VR::EngineProbe::FpStateDefault(id);
                    ImGui::Checkbox("First person", &s->on);
                    ImGui::SameLine(); ImGui::Checkbox("Hide head", &s->hideHead);
                    ImGui::SameLine(); ImGui::Checkbox("Hide body", &s->hideBody);
                    ImGui::SameLine(); ImGui::Checkbox("Hide weapon", &s->hideWeapon);
                    ImGui::SetNextItemWidth(360.0f); ImGui::SliderFloat("Forward", &s->x, -100.0f, 200.0f, "%.0f");
                    ImGui::SetNextItemWidth(360.0f); ImGui::SliderFloat("Up",      &s->z, -100.0f, 200.0f, "%.0f");
                    ImGui::SetNextItemWidth(360.0f); ImGui::SliderFloat("Right",   &s->y, -100.0f, 100.0f, "%.0f");
                }
                ImGui::PopID();
            };
            // Scrollable list so Save/Restore stay pinned below regardless of window height.
            const float footer = ImGui::GetFrameHeightWithSpacing() + 12.0f;
            ImGui::BeginChild("fpstates", ImVec2(0.0f, -footer), true);
            for (int i = 0; i < ME2VR::EngineProbe::FpStateCount(); ++i) fpRow(i);
            ImGui::EndChild();

            ImGui::Separator();
            if (ImGui::Button("Restore ALL to defaults"))
                for (int i = 0; i < ME2VR::EngineProbe::FpStateCount(); ++i)
                    if (auto* s = ME2VR::EngineProbe::GetFpStateCfg(i)) *s = ME2VR::EngineProbe::FpStateDefault(i);
            ImGui::SameLine();
            if (ImGui::Button("Save")) SaveValues();
            ImGui::SameLine(); ImGui::TextDisabled("(writes MELE2VR.ini)");
            ImGui::EndTabItem();
        }

        // ==== TAB: Comfort (ME1 parity) - where THIS panel sits in the headset ====
        // ==== TAB: View (ME1 parity) - how much of the headset the render fills, and 6DOF ====
        if (ImGui::BeginTabItem("View"))
        {
            ImGui::TextUnformatted("VR FOV fill");
            bool ff = ME2VR::Me2Xr::GetVrFovFill();
            if (ImGui::Checkbox("Fill the headset FOV", &ff)) ME2VR::Me2Xr::SetVrFovFill(ff);
            if (ff)
            {
                float fh = ME2VR::Me2Xr::GetVrFillH();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Fill H", &fh, 0.5f, 1.25f, "%.2f")) ME2VR::Me2Xr::SetVrFillH(fh);
                float fv = ME2VR::Me2Xr::GetVrFillV();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Fill V", &fv, 0.5f, 1.25f, "%.2f")) ME2VR::Me2Xr::SetVrFillV(fv);
                if (ImGui::SmallButton("Reset fill")) { ME2VR::Me2Xr::SetVrFillH(1.0f); ME2VR::Me2Xr::SetVrFillV(1.0f); }
            }
            ImGui::Separator();

            ImGui::TextUnformatted("Lean (6DOF)");
            bool pe = ME2VR::CalcViewHook::GetHeadPosEnabled();
            if (ImGui::Checkbox("Positional head tracking (lean)", &pe)) ME2VR::CalcViewHook::SetHeadPosEnabled(pe);
            float ps = ME2VR::CalcViewHook::GetHeadPosScale();
            ImGui::SetNextItemWidth(360.0f);
            if (ImGui::SliderFloat("Lean gain", &ps, 0.0f, 6.0f, "%.2f"))
                ME2VR::CalcViewHook::SetHeadPosScale(ClampF(ps, 0.0f, 12.0f, 1.0f));
            bool linv = ME2VR::CalcViewHook::GetLeanInvertFwd();
            if (ImGui::Checkbox("Invert lean forward/back", &linv)) ME2VR::CalcViewHook::SetLeanInvertFwd(linv);
            ImGui::Separator();

            // [CLEANUP] "Panel fix: refresh inverse matrices" toggle removed here too (was duplicated
            // with Tracking, same setter). Forced on at load; see the note on the Tracking tab.

            {
                ImGui::Separator();
                ImGui::TextDisabled("[advanced] OpenXR runtime: %s",
                                    ME2VR::Me2Xr::IsOculusRuntime() ? "Meta (Quest Link)" : "other");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("VR"))
        {
            // Reorganized 2026-07-26 to match ME1: named collapsible sections, only the SELECTED
            // render mode's panel shown (this was already correctly if/else-if gated by mode -- an
            // earlier flat listing of every control mis-read that as all four modes stacked visible;
            // they never were), and gating limited to real hazards the way ME1's own comments define
            // them: resolution above a few thousand square risks a GPU driver reset, live pacing-Hz
            // changes can hang the game, DIBR's fine convergence knobs are dev-tuning. Everything else
            // -- separation, swap-eyes, the on/off pacing bools, depth strength -- stays visible, same
            // as ME1 keeps those exact controls visible ("a user lever in every mode").
            // Read OUTSIDE the collapsing header below -- the per-mode panels further down need this
            // value whether or not that section is expanded, so it can't be scoped to its block.
            int mode = ME2VR::CalcViewHook::GetVrMode();
            if (ImGui::CollapsingHeader("VR mode", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool vr = ME2VR::CalcViewHook::GetVrEnabled();
                if (ImGui::Checkbox("Enable VR (OpenXR headset output)", &vr)) ME2VR::CalcViewHook::SetVrEnabled(vr);
                ImGui::Separator();

                // VR mode picker: Mono (flat panel) / Stereo (same-frame SBS) / AER (alternate-eye).
                // Old SBS "Stereo" (mode 1) phased out of the picker 2026-07-20 (ME1-style lockaway): SFR is
                // THE stereo mode (per-eye primary renders = correct bloom/UI in both eyes). Code kept,
                // unreachable from the UI; LoadValues migrates saved Mode 1 -> 4.
                ImGui::TextUnformatted("Render mode");
                if (ImGui::RadioButton("Mono (flat panel)", mode == 0)) { ME2VR::CalcViewHook::SetVrMode(0); mode = 0; }
                ImGui::SameLine();
                if (ImGui::RadioButton("Stereo", mode == 4)) { ME2VR::CalcViewHook::SetVrMode(4); mode = 4; }
                ImGui::SameLine();
                if (ImGui::RadioButton("AER (alternate-eye)", mode == 2)) { ME2VR::CalcViewHook::SetVrMode(2); mode = 2; }
                ImGui::SameLine();
                if (ImGui::RadioButton("DIBR (depth warp)", mode == 3)) { ME2VR::CalcViewHook::SetVrMode(3); mode = 3; }

                // ---- [VRRES] Render resolution (restart to apply). Gated: ME1's own comment on this
                // exact control says it plainly -- "past ~4096 RENDERS TWICE = GPU-driver-reset gamble."
                // Writes MELE2VR.ini [VR] RenderW/H; the DLL writes it into GamerSettings.ini next launch.
                {
                    ImGui::Separator();
                    const std::wstring ini = IniPath();
                    int curW = GetPrivateProfileIntW(L"VR", L"RenderW", 0, ini.c_str());
                    int curH = GetPrivateProfileIntW(L"VR", L"RenderH", 0, ini.c_str());
                    ImGui::TextUnformatted("Render resolution (restart to apply)");

                    // [VRRES2] ONE list, like ME1. Every preset is 16:9 on purpose: LE2's engine rewrites
                    // ResY back to a 16:9 height at startup no matter what the mod puts in GamerSettings.ini
                    // (measured -- a 4096x4096 override came back as 4096x2304 every launch), so a square
                    // option was never real. Stereo splits this backbuffer side-by-side, so the PER-EYE
                    // size is half the width: 6144x3456 -> 3072x3456 per eye, which is the first preset
                    // at/above this headset's own 3072x3264 recommendation. Below that the mod is submitting
                    // less than native and the compositor upscales -- that was the ME1-looks-sharper gap.
                    static const char* const kResLabels[] = {
                        "3K  -  3072 x 1728",
                        "4K  -  4096 x 2304",
                        "5K  -  5120 x 2880",
                        "6K  -  6144 x 3456",
                        "8K  -  8192 x 4608",
                        "10K - 10240 x 5760",
                    };
                    static const int kResW[] = { 3072, 4096, 5120, 6144,  8192, 10240 };
                    static const int kResH[] = { 1728, 2304, 2880, 3456,  4608,  5760 };
                    constexpr int kResCount = static_cast<int>(sizeof(kResW) / sizeof(kResW[0]));

                    int idx = 3;   // 6K if the ini holds something the mod doesn't recognise
                    for (int k = 0; k < kResCount; ++k) if (kResW[k] == curW && kResH[k] == curH) { idx = k; break; }
                    ImGui::SetNextItemWidth(420.0f);
                    if (ImGui::Combo("##vrres", &idx, kResLabels, kResCount))
                    {
                        wchar_t v[16];
                        swprintf_s(v, L"%d", kResW[idx]); WritePrivateProfileStringW(L"VR", L"RenderW", v, ini.c_str());
                        swprintf_s(v, L"%d", kResH[idx]); WritePrivateProfileStringW(L"VR", L"RenderH", v, ini.c_str());
                    }
                }
            }

            // Per-mode panel moved above Conversations & Cutscenes 2026-07-26: it makes no sense for
            // the controls belonging to the render mode you just picked to sit BELOW an unrelated
            // section. ##hdr on each header: its visible text is IDENTICAL to the mode radio button
            // above (e.g. "Stereo" / "AER (alternate-eye)"), and ImGui hashes widget IDs from the
            // label text alone -- same text, same ID, regardless of widget type. Two different
            // widgets sharing one ID inside the same window is exactly ImGui's "duplicate ID" assert.
            // DIBR never collided because its header text ("depth reprojection") already differs from
            // its radio ("depth warp"); Stereo and AER did not have that accidental difference.
            if (mode == 4 && ImGui::CollapsingHeader("Stereo##hdr", ImGuiTreeNodeFlags_DefaultOpen))   // SFR: same-frame per-eye render, the ME1 stereo fix
            {
                float he = ME2VR::CalcViewHook::GetHalfEyeUU();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Eye separation (IPD)##sfr", &he, 0.0f, 10.0f, "%.2f uu"))
                    ME2VR::CalcViewHook::SetHalfEyeUU(ClampF(he, 0.0f, 16.0f, 6.0f));

                float cv = ME2VR::CalcViewHook::GetSfrConvergence();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Convergence##sfr", &cv, -0.12f, 0.12f, "%.4f"))
                    ME2VR::CalcViewHook::SetSfrConvergence(ClampF(cv, -0.2f, 0.2f, 0.04f));

                bool sw = ME2VR::CalcViewHook::GetSwapEyes();
                if (ImGui::Checkbox("Swap eyes (invert depth)##sfr", &sw)) ME2VR::CalcViewHook::SetSwapEyes(sw);

                bool sp = ME2VR::CalcViewHook::GetStereoFramePacing();
                if (ImGui::Checkbox("Display-locked pacing (smoother)##sfr", &sp)) ME2VR::CalcViewHook::SetStereoFramePacing(sp);

                bool fr = ME2VR::CalcViewHook::GetFullRefreshPacing();
                if (ImGui::Checkbox("Full refresh rate##sfr", &fr))
                { ME2VR::CalcViewHook::SetFullRefreshPacing(fr); SaveValues(); }
                ImGui::TextDisabled("On: up to the headset refresh, framerate varies. Off: half refresh, rock steady.");
            }
            // [CLEANUP] The old SBS "Stereo 1" panel (mode == 1) is DELETED here, not gated -- the radio
            // above never sets mode to 1 (it was phased out 2026-07-20), so this whole block could never
            // be seen or reached from the UI. Matches ME1's own precedent on this exact legacy mode,
            // verbatim from its source: "make sure the old stereo never shows its ugly head again." Its
            // one still-live setting (Duplicate UI per-eye) moved to the Graphics section above, where it
            // is actually reachable.
            else if (mode == 2 && ImGui::CollapsingHeader("AER (alternate-eye)##hdr", ImGuiTreeNodeFlags_DefaultOpen))   // ---- AER controls ----
            {
                float ahe = ME2VR::CalcViewHook::GetAerHalfEyeUU();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("AER eye separation", &ahe, 0.0f, 10.0f, "%.2f uu"))
                    ME2VR::CalcViewHook::SetAerHalfEyeUU(ClampF(ahe, 0.0f, 10.0f, 1.6f));

                bool asw = ME2VR::CalcViewHook::GetAerSwapEyes();
                if (ImGui::Checkbox("Swap eyes (invert depth)##aer", &asw)) ME2VR::CalcViewHook::SetAerSwapEyes(asw);

                bool ap = ME2VR::CalcViewHook::GetAerFramePacing();
                if (ImGui::Checkbox("Display-locked pacing (the flicker fix)", &ap)) ME2VR::CalcViewHook::SetAerFramePacing(ap);
                bool afr = ME2VR::CalcViewHook::GetFullRefreshPacing();
                if (ImGui::Checkbox("Run at full frame rate##aer", &afr))
                {
                    ME2VR::CalcViewHook::SetFullRefreshPacing(afr);
                    SaveValues();
                }

                // Gated: hazard per ME1's own comment -- "touching Pacing Hz live can hang the game."
                {
                    int hz = ME2VR::CalcViewHook::GetAerFramePacingHz();
                    ImGui::SetNextItemWidth(420.0f);
                    if (ImGui::SliderInt("Pacing Hz (0 = auto-learn)", &hz, 0, 144))
                        ME2VR::CalcViewHook::SetAerFramePacingHz(ClampI(hz, 0, 240, 0));
                }
            }
            else if (mode == 3 && ImGui::CollapsingHeader("DIBR (depth reprojection)", ImGuiTreeNodeFlags_DefaultOpen))   // ---- DIBR controls ----
            {
                float gain = ME2VR::CalcViewHook::GetDepthWarpGain();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Depth strength", &gain, 0.0f, 5.0f, "%.2f"))
                    ME2VR::CalcViewHook::SetDepthWarpGain(ClampF(gain, 0.0f, 10.0f, 2.0f));

                // Gated: fine convergence/flip tuning, matching ME1's own treatment of this exact trio.
                {
                    bool autoConv = ME2VR::CalcViewHook::GetDibrAutoConverge();
                    if (ImGui::Checkbox("Auto-convergence (track subject)", &autoConv))
                        ME2VR::CalcViewHook::SetDibrAutoConverge(autoConv);
                    if (!autoConv)
                    {
                        float conv = ME2VR::CalcViewHook::GetDepthWarpConv();
                        ImGui::SetNextItemWidth(420.0f);
                        if (ImGui::SliderFloat("Convergence plane", &conv, 0.90f, 1.00f, "%.4f"))
                            ME2VR::CalcViewHook::SetDepthWarpConv(ClampF(conv, 0.90f, 1.005f, 0.985f));
                    }
                    bool flip = ME2VR::CalcViewHook::GetDepthWarpFlip();
                    if (ImGui::Checkbox("Flip depth", &flip)) ME2VR::CalcViewHook::SetDepthWarpFlip(flip);
                }
            }
            // Mono: nothing to show -- no per-mode panel.

            // Runtime + graphics options that apply to EVERY render mode -- matches ME1's own "Graphics"
            // section, which puts DoF here rather than repeating it per-mode. Sits BELOW the per-mode
            // panel (moved 2026-07-26): the controls for the mode you just picked should follow the mode
            // picker directly, with the mode-agnostic graphics options after them.
            if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool qf = ME2VR::Me2Xr::GetQuestFovMatch();
                if (ImGui::Checkbox("Quest Link image fix", &qf)) ME2VR::Me2Xr::SetQuestFovMatch(qf);

                // [DOFWASH] Hidden 2026-07-27. This wrote DepthOfField=False into GamerSettings
                // [SystemSettings], which in UE3 selects an uber-post-process permutation that skips
                // TONEMAPPING -- the frame ships ungraded: flat, raised blacks, no contrast, on every
                // runtime. Bisected against a live repro: that key alone, nothing else set, reproduces
                // it; remove it and the colour is correct. Cost most of a day.
                // The ini key still works for anyone who wants it, but it defaults OFF and is no longer
                // one click away. Conversation DoF is UNAFFECTED -- that runs through
                // ABioPlayerController.DisableDOF() per scene (First Person tab) and never touches this.
                // If gameplay DoF is wanted again, do it at the post-process node, NOT this engine key.

                // [CLEANUP] relocated from the dead "Stereo 1" legacy panel below (radio never sets
                // mode 1) -- still gates real behaviour, just had no way to reach it. A ghost-image fix
                // nobody needs until something looks wrong.
                {
                    bool ud = ME2VR::D3DCapture::GetUiDupEnabled();
                    if (ImGui::Checkbox("Duplicate UI per-eye (HUD/text fix)", &ud)) ME2VR::D3DCapture::SetUiDupEnabled(ud);
                }
            }

            // ---- [VRCINE] Conversations & Cutscenes (experimental) ----
            if (ImGui::CollapsingHeader("Conversations & Cutscenes (experimental)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool cvc = ME2VR::CalcViewHook::GetCineVrConvo();
                if (ImGui::Checkbox("VR conversations", &cvc))
                {
                    ME2VR::CalcViewHook::SetCineVrConvo(cvc);
                    // [CINEXCL] mutually exclusive with first-person conversations (see the FP tab).
                    if (cvc && ME2VR::ConvoFp::GetEnabled())
                    {
                        ME2VR::ConvoFp::SetEnabled(false);
                        ME2VR::Log::Line("[CINEXCL] VR conversations ON -> first-person conversations OFF");
                    }
                }
                bool cvs = ME2VR::CalcViewHook::GetCineVrCutscene();
                if (ImGui::Checkbox("VR cutscenes", &cvs))
                {
                    ME2VR::CalcViewHook::SetCineVrCutscene(cvs);
                    if (cvs && ME2VR::ConvoFp::GetEnabled())
                    {
                        ME2VR::ConvoFp::SetEnabled(false);
                        ME2VR::Log::Line("[CINEXCL] VR cutscenes ON -> first-person conversations OFF");
                    }
                }
                bool cht = ME2VR::CalcViewHook::GetCineVrHeadTracking();
                if (ImGui::Checkbox("Head tracking during cinematics", &cht)) ME2VR::CalcViewHook::SetCineVrHeadTracking(cht);
                float cz = ME2VR::Me2Xr::GetCineScreenZoom();
                if (ImGui::SliderFloat("Cutscene zoom", &cz, 0.5f, 3.0f, "%.2f")) ME2VR::Me2Xr::SetCineScreenZoom(cz);

                // [ENGCINE] gm 8 covers prerendered movies AND in-engine staged cutscenes (Archangel
                // mid-mission scenes) - the engine reports them identically, so without the scene-alive
                // check the in-engine ones drop to a head-locked flat panel and stereo silently dies.
                // [ENGCINE] Hidden 2026-07-31 for the same reason: it tells in-engine cutscenes apart
                // from prerendered video so the former keep their depth. Off is simply wrong.
                // Escape hatch: VR/EngCineAlive in the ini.

                // ==== [CONVOFP] first-person conversations -- same family as the two toggles above,
                // so it lives here rather than on the First Person tab. Same ME1 pattern: a toggle with
                // its own zoom slider directly underneath. Everything past the zoom is fine-tuning
                // (facing rate/sign, animation follow, eye trim, head hide, per-scene DoF) and a live
                // diagnostic readout -- gated, not because it's dangerous, but because it's noise for
                // a first-time user; the two lines that matter (on/off, zoom) stay in front of everyone.
                ImGui::Separator();
                bool cfp = ME2VR::ConvoFp::GetEnabled();
                if (ImGui::Checkbox("First person conversations", &cfp))
                {
                    ME2VR::ConvoFp::SetEnabled(cfp);
                    // [CINEXCL] mutually exclusive with VR conversations/cutscenes -- the rule is "one
                    // disables the other and vice versa, they cannot clash."
                    if (cfp && (ME2VR::CalcViewHook::GetCineVrConvo() || ME2VR::CalcViewHook::GetCineVrCutscene()))
                    {
                        ME2VR::CalcViewHook::SetCineVrConvo(false);
                        ME2VR::CalcViewHook::SetCineVrCutscene(false);
                        ME2VR::Log::Line("[CINEXCL] first-person conversations ON -> VR conversations/cutscenes OFF");
                    }
                }
                float fz = ME2VR::ConvoFp::GetZoom();
                if (ImGui::SliderFloat("First person conversation zoom", &fz, 1.0f, 2.5f, "%.2fx"))
                    ME2VR::ConvoFp::SetZoom(fz);

                if (cfp)
                {
                    ImGui::Separator();
                    float turn = ME2VR::ConvoFp::GetTurnRate();
                    ImGui::SetNextItemWidth(360.0f);
                    if (ImGui::SliderFloat("Turn to speaker (deg/sec)", &turn, 0.0f, 360.0f, "%.0f"))
                        ME2VR::ConvoFp::SetTurnRate(turn);
                    ImGui::TextDisabled("0 = never turns, you look for them yourself.");

                    float follow = ME2VR::ConvoFp::GetAnimFollow();
                    ImGui::SetNextItemWidth(360.0f);
                    if (ImGui::SliderFloat("Animation follow", &follow, 0.0f, 1.0f, "%.2f"))
                        ME2VR::ConvoFp::SetAnimFollow(follow);
                    ImGui::TextDisabled("0 = rock steady. 1 = rides Shepard's animated head (more head bob).");

                    float up = ME2VR::ConvoFp::GetEyeUpUU();
                    ImGui::SetNextItemWidth(360.0f);
                    if (ImGui::SliderFloat("Eye height trim (uu)", &up, -40.0f, 40.0f, "%+.0f"))
                        ME2VR::ConvoFp::SetEyeUpUU(up);

                    bool hh = ME2VR::ConvoFp::GetHideHead();
                    if (ImGui::Checkbox("Hide Shepard's head", &hh)) ME2VR::ConvoFp::SetHideHead(hh);

                    bool kd = ME2VR::ConvoFp::GetKillDof();
                    if (ImGui::Checkbox("Disable depth of field in conversations", &kd)) ME2VR::ConvoFp::SetKillDof(kd);

                    bool inv = ME2VR::CalcViewHook::GetConvoFpInvertFacing();
                    if (ImGui::Checkbox("Invert facing", &inv)) ME2VR::CalcViewHook::SetConvoFpInvertFacing(inv);

                    if (ME2VR::ConvoFp::IsArmed())
                    {
                        ME2VR::ConvoFp::Diag d{};
                        ME2VR::ConvoFp::GetDiag(&d);
                        static const char* kSrc[5] = { "none", "eye height", "head bone", "actor location", "holding" };
                        ImGui::Separator();
                        ImGui::Text("ACTIVE  source=%s  speakers=%d  yaw=%.0f",
                                    kSrc[(d.source >= 0 && d.source <= 4) ? d.source : 0], d.speakerCount, d.baseYawDeg);
                    }
                }
                ImGui::Separator();
            }

            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Comfort"))
        {
            // Wrapped in a named section 2026-07-26 to match ME1's "Menu comfort" header, instead of a
            // flat column.
            if (ImGui::CollapsingHeader("Menu comfort", ImGuiTreeNodeFlags_DefaultOpen))
            {
                float md = ME2VR::Me2Xr::GetMenuPanelDist();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Menu distance (m)", &md, 0.8f, 4.0f, "%.2f")) ME2VR::Me2Xr::SetMenuPanelDist(md);
                float ms = ME2VR::Me2Xr::GetMenuPanelSize();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Menu size (m wide)", &ms, 0.5f, 3.5f, "%.2f")) ME2VR::Me2Xr::SetMenuPanelSize(ms);
                float mx = ME2VR::Me2Xr::GetMenuPanelOffX();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Menu left/right (m)", &mx, -1.5f, 1.5f, "%.2f")) ME2VR::Me2Xr::SetMenuPanelOffX(mx);
                float my = ME2VR::Me2Xr::GetMenuPanelOffY();
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::SliderFloat("Menu up/down (m)", &my, -1.0f, 1.0f, "%.2f")) ME2VR::Me2Xr::SetMenuPanelOffY(my);
                if (ImGui::Button("Center menu"))
                {
                    ME2VR::Me2Xr::SetMenuPanelOffX(0.0f);
                    ME2VR::Me2Xr::SetMenuPanelOffY(0.0f);
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset panel"))
                {
                    ME2VR::Me2Xr::SetMenuPanelDist(1.5f); ME2VR::Me2Xr::SetMenuPanelSize(0.90f);   // matches the baked default
                    ME2VR::Me2Xr::SetMenuPanelOffX(0.0f); ME2VR::Me2Xr::SetMenuPanelOffY(0.0f);
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Menu open key");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", KeyName(g_menuKey));
                ImGui::SameLine();
                if (ImGui::SmallButton(g_rebindingMenuKey ? "press a key..." : "Rebind##menukey"))
                    g_rebindingMenuKey = !g_rebindingMenuKey;
                if (g_rebindingMenuKey)
                {
                    for (int vk = 8; vk < 255; ++vk)
                    {
                        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                        if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
                        if (vk == VK_ESCAPE) { g_rebindingMenuKey = false; break; }
                        g_menuKey = vk; g_rebindingMenuKey = false; SaveValues(); break;
                    }
                }
            }
            ImGui::EndTabItem();
        }
        // ==== TAB: HUD ====
        // [HUDOPEN 2026-08-21] Every player has a different headset, IPD and comfort zone, so
        // nudging HUD pieces into view is ordinary setup, not an expert feature.
        if (ImGui::BeginTabItem("HUD"))
        {
            // Split into two named sections 2026-07-26, matching ME1's "PCHUD movie" header pattern
            // instead of one long unsectioned column.
            if (ImGui::CollapsingHeader("HUD auto-match (SFR mode)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextDisabled("Trims on top of the auto un-stretch. Keep near 1.0 / 0.");
                // [HUDNOTE] The element transforms are applied from the game's own HUD event,
                // which does not tick while this menu owns input, so edits land once it is closed.
                ImGui::TextDisabled("Close this menu to see HUD size changes.");
                ImGui::Separator();

                float sx = ME2VR::CalcViewHook::GetSfrUiScaleX();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Scale X (width)", &sx, 0.2f, 1.5f, "%.3f")) ME2VR::CalcViewHook::SetSfrUiScaleX(sx);

                float sy = ME2VR::CalcViewHook::GetSfrUiScaleY();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Scale Y (height)", &sy, 0.2f, 1.5f, "%.3f")) ME2VR::CalcViewHook::SetSfrUiScaleY(sy);

                float ox = ME2VR::CalcViewHook::GetSfrUiOffX();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Offset X", &ox, -0.5f, 0.5f, "%.3f")) ME2VR::CalcViewHook::SetSfrUiOffX(ox);

                float oy = ME2VR::CalcViewHook::GetSfrUiOffY();
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::SliderFloat("Offset Y", &oy, -0.5f, 0.5f, "%.3f")) ME2VR::CalcViewHook::SetSfrUiOffY(oy);
                if (ImGui::Button("Reset HUD"))
                {
                    ME2VR::CalcViewHook::SetSfrUiScaleX(1.0f); ME2VR::CalcViewHook::SetSfrUiScaleY(1.0f);
                    ME2VR::CalcViewHook::SetSfrUiOffX(0.0f);   ME2VR::CalcViewHook::SetSfrUiOffY(0.0f);
                }
                ImGui::SameLine();
                if (ImGui::Button("Save##hud")) SaveValues();
            }

            // ---- Per-element control. Paths are read from the game's own HUD handler, so these move
            // the real Flash objects rather than the whole overlay. ----
            if (ImGui::CollapsingHeader("Individual HUD pieces", ImGuiTreeNodeFlags_DefaultOpen))
            {
            bool pcOn = ME2VR::PcHud::GetEnabled();
            if (ImGui::Checkbox("Enable per-element HUD control", &pcOn)) ME2VR::PcHud::SetEnabled(pcOn);
            if (pcOn)
            {
                for (int g = 0; g < ME2VR::PcHud::GroupCount(); ++g)
                {
                    ME2VR::PcHud::GroupCfg* c = ME2VR::PcHud::GetGroup(g);
                    if (c == nullptr) continue;
                    ImGui::PushID(3000 + g);
                    if (ImGui::CollapsingHeader(ME2VR::PcHud::GroupLabel(g)))
                    {
                        // Resets to THIS group's baked default, not a blanket (0,0,1,1) -- G0/G5/G6
                        // ship with a real default position/size, and "Reset" undoing to a different
                        // state than what a fresh install actually shows would be its own bug.
                        // [HUDCONSIST 2026-08-22] Sliders, then Link, then Reset - same order in all
                        // three games so the piece controls read identically everywhere.
                        ImGui::SetNextItemWidth(360.0f);
                        ImGui::SliderFloat("Left / right", &c->offX, -600.0f, 600.0f, "%.0f");
                        ImGui::SetNextItemWidth(360.0f);
                        ImGui::SliderFloat("Up / down", &c->offY, -500.0f, 500.0f, "%.0f");
                        ImGui::SetNextItemWidth(360.0f);
                        ImGui::SliderFloat("Size X (width)", &c->scaleX, 0.3f, 3.0f, "%.2f");
                        ImGui::SetNextItemWidth(360.0f);
                        ImGui::SliderFloat("Size Y (height)", &c->scaleY, 0.3f, 3.0f, "%.2f");
                        if (ImGui::SmallButton("Link Y to X")) c->scaleY = c->scaleX;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset"))
                        {
                            const HudGroupDefault gd = HudGroupDefaultFor(g);
                            c->offX = gd.offX; c->offY = gd.offY; c->scaleX = gd.scaleX; c->scaleY = gd.scaleY;
                        }
                    }
                    ImGui::PopID();
                }
                if (ImGui::Button("Reset ALL pieces"))
                    for (int g = 0; g < ME2VR::PcHud::GroupCount(); ++g)
                        if (auto* c = ME2VR::PcHud::GetGroup(g))
                        {
                            const HudGroupDefault gd = HudGroupDefaultFor(g);
                            c->offX = gd.offX; c->offY = gd.offY; c->scaleX = gd.scaleX; c->scaleY = gd.scaleY;
                        }
                ImGui::SameLine();
                if (ImGui::Button("Save##pchud")) SaveValues();
            }
            }   // Individual HUD pieces
            ImGui::EndTabItem();
        }
        // ==== TAB: Profiles (ME1 parity) - 4 complete setting snapshots + switch hotkeys ====
        if (ImGui::BeginTabItem("Profiles"))
        {
            // Split into two named sections 2026-07-26, matching ME1's separate "Profiles" / "Profile
            // hotkeys" headers -- selecting/saving a slot and binding its key were interleaved per-row
            // before; ME1 keeps them as two distinct jobs.
            if (ImGui::CollapsingHeader("Profiles", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextDisabled("Each slot is a full snapshot of your settings.");
                ImGui::Separator();
                ImGui::Text("Active: %s", ProfileName(g_activeProfile));
                for (int i = 0; i < kProfileCount; ++i)
                {
                    ImGui::PushID(1000 + i);
                    if (ImGui::RadioButton(ProfileName(i), g_activeProfile == i)) LoadProfile(i);
                    ImGui::SameLine(220.0f);
                    if (ImGui::SmallButton("Save to this slot")) SaveProfile(i);
                    ImGui::PopID();
                }
            }

            if (ImGui::CollapsingHeader("Profile hotkeys", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Checkbox("Enable profile hotkeys", &g_profileHotkeys)) SaveValues();
                ImGui::Separator();
                for (int i = 0; i < kProfileCount; ++i)
                {
                    ImGui::PushID(2000 + i);
                    ImGui::Text("%s", ProfileName(i));
                    ImGui::SameLine(160.0f);
                    ImGui::TextDisabled("%s", KeyName(g_profileKeys[i]));
                    ImGui::SameLine();
                    if (ImGui::SmallButton(g_rebindingProfile == i ? "press..." : "Rebind"))
                        g_rebindingProfile = (g_rebindingProfile == i) ? -1 : i;
                    ImGui::PopID();
                }
                if (g_rebindingProfile >= 0 && g_rebindingProfile < kProfileCount)
                {
                    for (int vk = 8; vk < 255; ++vk)
                    {
                        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == g_menuKey) continue;
                        if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
                        if (vk == VK_ESCAPE) { g_rebindingProfile = -1; break; }
                        g_profileKeys[g_rebindingProfile] = vk; g_rebindingProfile = -1; SaveValues(); break;
                    }
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Save")) SaveValues();
    if (g_savedFlash && g_savedFrames > 0) { --g_savedFrames; ImGui::SameLine(); ImGui::TextDisabled("saved"); }
    ImGui::SameLine(); ImGui::TextDisabled("%s: close   (settings -> MELE2VR.ini)", KeyName(g_menuKey));
    ImGui::End();
}
}  // namespace

void Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) noexcept
{
    if (g_ready || device == nullptr || context == nullptr || width <= 0 || height <= 0) return;
    g_device = device; g_context = context; g_device->AddRef(); g_context->AddRef();
    g_w = width; g_h = height;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_w; td.Height = g_h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_tex)) || !g_tex) { SafeRelease(g_context); SafeRelease(g_device); return; }
    if (FAILED(g_device->CreateRenderTargetView(g_tex, nullptr, &g_rtv)) || !g_rtv) { SafeRelease(g_tex); SafeRelease(g_context); SafeRelease(g_device); return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.3f);
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 0.85f;
    if (!ImGui_ImplDX11_Init(g_device, g_context)) { ImGui::DestroyContext(); SafeRelease(g_rtv); SafeRelease(g_tex); SafeRelease(g_context); SafeRelease(g_device); return; }

    LoadValues();
    g_ready = true;
    ME2VR::Log::Line("[ME2MENU] Insert menu ready (Insert to open).");
}

void OnPresent(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain != nullptr && g_hwnd == nullptr)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(swapChain->GetDesc(&desc))) SetGameWindow(desc.OutputWindow);
    }
    PollInsert();
    EnforceFreeze(g_open);   // freeze game look/move input while the menu is open
    InstallPadHooks();       // [MOVEFIX] one-shot; no-op until the game has loaded an XInput dll
    InstallMouseHooks();     // [DECOUPLE] raw-input mouse half (user32 is always loaded)
}

ID3D11Texture2D* RenderFrame() noexcept
{
    if (!g_ready || !g_open || g_context == nullptr || g_rtv == nullptr) return nullptr;
    FeedMouse();
    ImGui_ImplDX11_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    BuildUI();
    ImGui::Render();
    const float clear[4] = {0, 0, 0, 0};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    return g_tex;
}

// Flat mode (VR off): the VR submit path that normally composites the menu never runs, so draw ImGui
// straight onto the swapchain backbuffer with the REAL mouse (no virtual-mouse quad). Mutually exclusive
// with RenderFrame() - only one fires per frame, gated by the VR switch at the call site.
void RenderToBackbuffer(IDXGISwapChain* swapChain) noexcept
{
    if (!g_ready || !g_open || g_context == nullptr || g_device == nullptr || swapChain == nullptr) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || bb == nullptr) return;
    D3D11_TEXTURE2D_DESC bd = {};
    bb->GetDesc(&bd);
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(g_device->CreateRenderTargetView(bb, nullptr, &rtv)) && rtv != nullptr)
    {
        ImGui_ImplDX11_NewFrame();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(bd.Width), static_cast<float>(bd.Height));
        if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
        io.MouseDrawCursor = true;
        ClipCursor(nullptr);
        POINT pt = {};
        if (GetCursorPos(&pt) && g_hwnd != nullptr && ScreenToClient(g_hwnd, &pt))
        {
            RECT rc = {};
            GetClientRect(g_hwnd, &rc);
            const float sx = (rc.right  > 0) ? static_cast<float>(bd.Width)  / rc.right  : 1.0f;
            const float sy = (rc.bottom > 0) ? static_cast<float>(bd.Height) / rc.bottom : 1.0f;
            io.AddMousePosEvent(static_cast<float>(pt.x) * sx, static_cast<float>(pt.y) * sy);
        }
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        ImGui::NewFrame();
        BuildUI();
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        rtv->Release();
    }
    bb->Release();
}

bool IsOpen() noexcept { return g_open; }
int LeftStickMagnitude() noexcept { return g_leftStickMag.load(std::memory_order_relaxed); }
bool GetMoveFollowsHead() noexcept { return g_moveFollowsHead.load(std::memory_order_relaxed); }
void SetMoveFollowsHead(bool on) noexcept { g_moveFollowsHead.store(on, std::memory_order_relaxed); }
int GetRecenterKey() noexcept { return g_recenterKey; }
int GetFpToggleKey() noexcept { return g_fpToggleKey; }
bool IsRebindingFpToggle() noexcept { return g_rebindingFpToggle; }
}  // namespace ME2VR::Menu
