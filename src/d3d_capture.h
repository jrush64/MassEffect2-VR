#pragma once

#include <Windows.h>

struct ID3D11Device;
struct IDXGISwapChain;
struct ID3D11Texture2D;

namespace ME2VR::D3DCapture
{
void InstallBreakpointProbes() noexcept;
void TryInstallFactoryHooks(void* factory, const IID& riid) noexcept;

// Render-above-display-resolution spoof.
// ArmResolutionSpoof() MUST run synchronously in DllMain, before any thread; InstallDisplayQueryHooks()
// runs from the worker thread, before the game's D3D init.
void ArmResolutionSpoof() noexcept;
void InstallDisplayQueryHooks() noexcept;

// Retained references to the game's D3D11 device + swapchain (captured at first present).
// Null until the swapchain is created. Used by the OpenXR bridge (me2_xr). Not AddRef'd for caller.
ID3D11Device* GetGameDevice() noexcept;
IDXGISwapChain* GetGameSwapChain() noexcept;
unsigned GetBackbufferWidth() noexcept;
unsigned GetBackbufferHeight() noexcept;

// [SFR] same-frame stereo: the frame is rendered twice per present (calcview DrawDetour). Pass 0
// (left, -halfEye) is captured MID-present (pass 1 overwrites the backbuffer) by a render-thread
// in-command-stream marker: the draw hooks flag pass 0's backbuffer composite, the clear hook grabs
// the backbuffer at the next scene clear (all render-thread, no game-thread D3D = no race/crash).
// GetSfrPass0Texture returns that captured left-eye frame at submit (null before the first capture).
ID3D11Texture2D* GetSfrPass0Texture() noexcept;

// True when a full-screen menu/UI is up (detected by UI-draw volume). In this mode the frame should
// be presented MONO (no stereo split, UI shown full-width to both eyes) - 2D menus aren't stereo.
bool GetMenuMode() noexcept;           // composite: manual toggle OR auto (engine menu/map/movie mode)
void SetMenuMode(bool on) noexcept;    // manual flat toggle
bool GetMenuModeManual() noexcept;     // the manual flag alone (for the checkbox UI)
void SetAutoGameMode(int mode) noexcept;   // [AUTOMENU] engine EGameModes byte, published per frame
int  GetAutoGameMode() noexcept;
int  GetMirrorPresentEvery() noexcept;    void SetMirrorPresentEvery(int n) noexcept;   // [MIRRORREC]
bool GetMonoMenus() noexcept;          // ME1-style bool: mono menus/map (default) vs VR menus
void SetMonoMenus(bool on) noexcept;
// [PASS0ANCHOR] Take the pass-0 (left eye) snapshot exactly once per present, at the first scene clear
// after pass 0 composites to the backbuffer, instead of "last scene clear wins". The old rule assumed a
// stable clear count; when it changes (post-cutscene areas) the last clear lands after pass 1 and both
// eyes get the same image. ON = anchored (correct), OFF = old behaviour, for A/B.
bool GetPass0Anchor() noexcept;
void SetPass0Anchor(bool on) noexcept;
// [STALEEYE] Monotonic pass-0 capture counter. If it has not advanced since the last submit, the pass-0
// texture is a LEFTOVER from an earlier frame and must not be shown as the left eye - a frozen eye next
// to a live one reads as head tracking dying. Submit falls back to the live backbuffer instead.
unsigned long long GetSfrPass0CaptureSeq() noexcept;
// [GM7PAUSE] Engine mode 7 (GUI) covers BOTH blocking menus and live gameplay with the HUD up (objective
// display, tutorial popups, aiming) - only the former pauses the engine. Publishing the pause flag lets
// GetMenuMode() keep combat in VR instead of flattening it. Toggle exists to A/B against the old behaviour.
void SetAutoPaused(bool on) noexcept;      // AWorldInfo.Pauser != null, published per frame from me2_xr
bool GetAutoPaused() noexcept;
bool GetGm7NeedsPause() noexcept;          // true = mode 7 flat only while paused (fix); false = always flat (old)
void SetGm7NeedsPause(bool on) noexcept;

// Per-eye UI duplication: redraws full-viewport alpha-blended (Scaleform) draws into each SBS half so HUD
// text/panels aren't cross-eyed. The detector is a heuristic and can false-positive on full-screen effect
// passes -> those get duped at half scale = a translucent ghost. Toggle to isolate/disable that.
bool GetUiDupEnabled() noexcept;
void SetUiDupEnabled(bool on) noexcept;

// The UI overlay texture (HUD + menus, transparent bg) to composite as a flat layer over the
// always-stereo world. GetUiOverlayActive() is true on frames that drew UI.
ID3D11Texture2D* GetUiOverlayTexture() noexcept;
bool GetUiOverlayActive() noexcept;

// The captured GFx UI texture (complete UI), to be submitted as a quad layer. Null until seen.
ID3D11Texture2D* GetUiTexture() noexcept;
unsigned GetUiTexWidth() noexcept;
unsigned GetUiTexHeight() noexcept;

// --- DIBR (depth-image-based stereo): the 4th VR mode. Left = real frame, right = depth-warped synth. ---
void SetDepthMapEnabled(bool enabled) noexcept;   // master: run the depth capture + warp (true only in mode 3)
bool GetDepthMapEnabled() noexcept;
bool IsDibrStereoReady() noexcept;                // depthEnabled && depthReady && SRV valid
void SetDibrWarp(float gain, float convergence, bool flip) noexcept;   // push live warp tunables
// Milliseconds since Bink last decoded a frame (ULLONG_MAX until the first movie frame).
unsigned long long LastBinkFrameAgeMs() noexcept;
void GetDepthProbe(float* center, float* tl, float* br, float* tr) noexcept;   // depth taps (auto-converge feeds off center)
ID3D11Texture2D* GetDibrRightEye(ID3D11Texture2D* backBuffer) noexcept;        // synth eye; null -> submit backbuffer
}
