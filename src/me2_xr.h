#pragma once

namespace ME2VR::Me2Xr
{
// Milestone A1: bring up OpenXR on the GAME's D3D11 device and create a session (log-only, no
// submit). Answers the one real Milestone-A risk: does ME2's own device bind to xrCreateSession?
// Call once per Present; makes a single attempt once the game device is available, then latches.
void Tick() noexcept;

// Re-origin the head-look reference to the current head pose (call from a future recenter hotkey).
void Recenter() noexcept;

// Menu/flat-screen quad placement (mono menus, loading, movies): distance (m) + width (m). Tunable
// in the Insert menu, persisted by me2_menu.
float GetMenuScreenDist() noexcept;   void SetMenuScreenDist(float m) noexcept;
float GetMenuScreenSize() noexcept;   void SetMenuScreenSize(float m) noexcept;
// [CINEFLAT] ME1-style zoom for the world-locked cutscene/conversation screen (menus unaffected).
float GetCineScreenZoom() noexcept;   void SetCineScreenZoom(float z) noexcept;
// [COMFORT] placement of the Insert menu's OWN panel (not the game-menu screen above).
float GetMenuPanelDist() noexcept;    void SetMenuPanelDist(float m) noexcept;
float GetMenuPanelSize() noexcept;    void SetMenuPanelSize(float m) noexcept;
float GetMenuPanelOffX() noexcept;    void SetMenuPanelOffX(float m) noexcept;
float GetMenuPanelOffY() noexcept;    void SetMenuPanelOffY(float m) noexcept;
// [LINKFOV] Quest Link / Air Link image fix - crops each submitted eye to the runtime's own frustum.
// [VRFILL] widen the rendered view to fill the headset; per-axis fraction (1.0 = full headset FOV).
bool  GetVrFovFill() noexcept;        void SetVrFovFill(bool on) noexcept;
float GetVrFillH() noexcept;          void SetVrFillH(float v) noexcept;
float GetVrFillV() noexcept;          void SetVrFillV(float v) noexcept;
bool GetQuestFovMatch() noexcept;     void SetQuestFovMatch(bool on) noexcept;
bool IsOculusRuntime() noexcept;      // true on Meta's PC runtime (so the menu can say it's relevant)
}
