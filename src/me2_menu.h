#pragma once

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// In-headset Insert menu (ImGui -> texture -> head-locked quad), ported from ME1's M8 menu.
// Settings persist to MELE2VR.ini next to MassEffect2.exe. Options are filled in over time.
namespace ME2VR::Menu
{
void Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) noexcept;
void OnPresent(IDXGISwapChain* swapChain) noexcept;   // poll Insert toggle
ID3D11Texture2D* RenderFrame() noexcept;              // returns the menu texture while open, else null
void RenderToBackbuffer(IDXGISwapChain* swapChain) noexcept;  // flat (VR-off) path: draw straight to backbuffer
bool IsOpen() noexcept;
int GetRecenterKey() noexcept;   // current rebindable recenter hotkey (VK code)
int GetFpToggleKey() noexcept;   // current rebindable first-person toggle hotkey (VK code)
bool IsRebindingFpToggle() noexcept;   // true while capturing a new key for the FP toggle
int LeftStickMagnitude() noexcept;         // [FPSTORM] live left-stick deflection 0..32767 (real pad)
bool GetMoveFollowsHead() noexcept;        // [MOVEFIX] "run where you look"
void SetMoveFollowsHead(bool on) noexcept;
}
