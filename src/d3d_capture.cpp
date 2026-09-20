#include "d3d_capture.h"

#include "allocviewstate_finder.h"
#include "calcview_hook.h"
#include "convo_fp.h"
#include "engine_probe.h"
#include "pchud.h"
#include "logger.h"
#include "me2_menu.h"
#include "me2_xr.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>

#pragma intrinsic(_ReturnAddress)

namespace
{
constexpr UINT kPresentSlot = 8;
constexpr UINT kCreateSwapChainSlot = 10;
constexpr UINT kCreateSwapChainForHwndSlot = 15;
constexpr UINT kCreateSwapChainForCoreWindowSlot = 16;
constexpr UINT kCreateSwapChainForCompositionSlot = 24;
constexpr UINT kPSSetShaderResourcesSlot = 8;
constexpr UINT kDrawIndexedSlot = 12;
constexpr UINT kDrawSlot = 13;
constexpr UINT kDrawIndexedInstancedSlot = 20;
constexpr UINT kDrawInstancedSlot = 21;
constexpr UINT kOMSetRenderTargetsSlot = 33;
constexpr UINT kDrawAutoSlot = 38;
constexpr UINT kDrawIndexedInstancedIndirectSlot = 39;
constexpr UINT kDrawInstancedIndirectSlot = 40;
constexpr UINT kDispatchSlot = 41;
constexpr UINT kDispatchIndirectSlot = 42;
constexpr UINT kRSSetViewportsSlot = 44;
constexpr UINT kRSSetScissorRectsSlot = 45;
constexpr UINT kCopySubresourceRegionSlot = 46;
constexpr UINT kCopyResourceSlot = 47;
constexpr UINT kResolveSubresourceSlot = 57;
constexpr UINT kExecuteCommandListSlot = 58;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PSSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndexedInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using DispatchIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorRectsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using CopySubresourceRegionFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using ResolveSubresourceFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);

std::mutex g_hookMutex;
PresentFn g_originalPresent = nullptr;
void** g_presentSlot = nullptr;
CreateSwapChainFn g_originalCreateSwapChain = nullptr;
void** g_createSwapChainSlot = nullptr;
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
void** g_createSwapChainForHwndSlot = nullptr;
CreateSwapChainForCoreWindowFn g_originalCreateSwapChainForCoreWindow = nullptr;
void** g_createSwapChainForCoreWindowSlot = nullptr;
CreateSwapChainForCompositionFn g_originalCreateSwapChainForComposition = nullptr;
void** g_createSwapChainForCompositionSlot = nullptr;
PSSetShaderResourcesFn g_originalPSSetShaderResources = nullptr;
void** g_psSetShaderResourcesSlot = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
void** g_drawIndexedSlot = nullptr;
DrawFn g_originalDraw = nullptr;
void** g_drawSlot = nullptr;
DrawIndexedInstancedFn g_originalDrawIndexedInstanced = nullptr;
void** g_drawIndexedInstancedSlot = nullptr;
DrawInstancedFn g_originalDrawInstanced = nullptr;
void** g_drawInstancedSlot = nullptr;
OMSetRenderTargetsFn g_originalOMSetRenderTargets = nullptr;
void** g_omSetRenderTargetsSlot = nullptr;
DrawAutoFn g_originalDrawAuto = nullptr;
void** g_drawAutoSlot = nullptr;
DrawIndexedInstancedIndirectFn g_originalDrawIndexedInstancedIndirect = nullptr;
void** g_drawIndexedInstancedIndirectSlot = nullptr;
DrawInstancedIndirectFn g_originalDrawInstancedIndirect = nullptr;
void** g_drawInstancedIndirectSlot = nullptr;
DispatchFn g_originalDispatch = nullptr;
void** g_dispatchSlot = nullptr;
DispatchIndirectFn g_originalDispatchIndirect = nullptr;
void** g_dispatchIndirectSlot = nullptr;
RSSetViewportsFn g_originalRSSetViewports = nullptr;
void** g_rsSetViewportsSlot = nullptr;
RSSetScissorRectsFn g_originalRSSetScissorRects = nullptr;
void** g_rsSetScissorRectsSlot = nullptr;
CopySubresourceRegionFn g_originalCopySubresourceRegion = nullptr;
void** g_copySubresourceRegionSlot = nullptr;
CopyResourceFn g_originalCopyResource = nullptr;
void** g_copyResourceSlot = nullptr;
ResolveSubresourceFn g_originalResolveSubresource = nullptr;
void** g_resolveSubresourceSlot = nullptr;
ExecuteCommandListFn g_originalExecuteCommandList = nullptr;
void** g_executeCommandListSlot = nullptr;

std::atomic_bool g_captured{false};
std::atomic<unsigned long long> g_presentCount{0};
std::atomic<unsigned long long> g_psSetShaderResourcesCount{0};
std::atomic<unsigned long long> g_psSetShaderResourcesLogCount{0};
std::atomic<unsigned long long> g_drawCompositeLogCount{0};
std::atomic<unsigned long long> g_stereoBoundDrawLogCount{0};
std::atomic<unsigned long long> g_stateTraceLogCount{0};
std::atomic<unsigned long long> g_executeCommandListCount{0};
std::atomic<unsigned long long> g_executeCommandListLogCount{0};
std::atomic<unsigned long long> g_copyPathLogCount{0};
std::atomic<unsigned long long> g_omSetRenderTargetsCount{0};
std::atomic<unsigned long long> g_omSetRenderTargetsLogCount{0};
std::atomic<unsigned long long> g_viewportSetCount{0};
std::atomic<unsigned long long> g_viewportLogCount{0};
std::atomic<unsigned long long> g_scissorSetCount{0};
std::atomic<unsigned long long> g_scissorLogCount{0};
std::atomic<unsigned long long> g_stackLogCount{0};
std::atomic<unsigned long long> g_uiSeamStackCount{0};
// MinHook on the real draw FUNCTIONS (catches cached-pointer callers that bypass the vtable).
DrawIndexedFn g_mhRealDrawIndexed = nullptr;
DrawFn g_mhRealDraw = nullptr;
DrawIndexedInstancedFn g_mhRealDrawIndexedInstanced = nullptr;
std::atomic_bool g_mhDrawInstalled{false};
std::atomic_bool g_uiDupEnabled{true};      // master enable for UI-overlay redirect
// UI overlay: redirect full-screen UI draws to a private RT, presented as ONE flat layer over the
// (always-stereo) world. No per-eye split of the UI, no world mode switch -> no snap, menus readable.
ID3D11Texture2D* g_uiOverlayTex = nullptr;
ID3D11RenderTargetView* g_uiOverlayRTV = nullptr;
ID3D11ShaderResourceView* g_uiOverlaySRV = nullptr;
std::atomic<unsigned long long> g_overlayClearedFrame{0xFFFFFFFFFFFFFFFFull};
std::atomic_bool g_overlayHasContent{false};
std::atomic_bool g_menuMode{false};         // MANUAL flat-mode toggle (checkbox/hotkey) -> present mono, don't split/dup
// [AUTOMENU] ME1 parity: the engine's EGameModes byte, published per frame from me2_xr. Modes 7 (GUI
// menu: pause/inventory/squad/journal/map), 8 (movie), 9 (galaxy map), 10 (orbital/planet scan) force
// mono automatically when g_monoMenus is on (the ME1 "mono or VR menus" bool). -1 = unreadable = VR.
std::atomic<int>  g_autoGameMode{-1};
std::atomic_bool  g_monoMenus{true};        // ME1-style bool: true = menus/map flat mono (default), false = VR menus
// [GM7PAUSE] Mode 7 (GUI) is NOT only full-screen menus - ME2 also reports it during live gameplay while
// HUD elements are up (objective display, tutorial popups, aiming), which dropped all of COMBAT to a mono
// flat panel (2026-07-30). Real blocking menus PAUSE (AWorldInfo.Pauser != null); the gameplay
// HUD never does, so pause is the discriminator - the same trick ME3 uses for its gm-7 conversations.
// Published per frame from me2_xr next to the mode byte so GetMenuMode() stays a pure atomic read (it is
// called from every hot call site: split gate, SFR replay gate, UI dup/mirror, XR submit).
std::atomic_bool  g_autoPaused{false};
// DEFAULT OFF as of 2026-07-31: some full-screen gm-7 menus never set Pauser (stores were the flagged
// suspect from day one), so requiring pause left them rendering in stereo = broken mono menus.
// The combat-flattening this targeted turned out to be mostly the capture/cine-latch bugs, fixed
// properly since ([PASS1DOUBLE]/[CINELATCH]/[GAMEPLAYVR]); what remains is brief gm-7 flat blips in
// gameplay, a far smaller cost than menus rendering split. Kept as an opt-in experiment.
std::atomic_bool  g_gm7NeedsPause{false};   // true = mode 7 flat only while paused (experimental); false = any gm-7 flat (default)
float g_menuEma = 0.0f;                      // smoothed UI-draw rate (menu detector, noise-robust)
std::atomic_bool g_traceActive{false};        // ordered single-frame op trace
std::atomic<int> g_traceSeq{0};
std::atomic_bool g_uiPerEye{false};  // wrong layer (R8 mask, not UI) -> disabled while diagnosing
std::atomic<unsigned long long> g_uiPerEyeCount{0};
ID3D11Texture2D* g_uiSrcTexture = nullptr;   // the GFx UI render target (blit source); submitted as a quad
UINT g_uiTexW = 0, g_uiTexH = 0;
DXGI_FORMAT g_uiTexFmt = DXGI_FORMAT_UNKNOWN;
std::atomic_bool g_uiTexLogged{false};
std::uintptr_t g_exeBaseForUi = 0;
constexpr std::uintptr_t kUiCompositeCallerRva = 0x4850A2;   // distinctive caller of the UI blit
D3D11_VIEWPORT g_lastLoggedViewport = {};
bool g_haveLastLoggedViewport = false;

struct TrackedResource
{
    std::uintptr_t ptr;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
};

thread_local D3D11_VIEWPORT g_currentViewport = {};
thread_local bool g_haveCurrentViewport = false;
std::atomic<unsigned> g_pendingStereoCompositeDraws{0};
std::atomic<unsigned> g_traceAfterFullSizeStereoBind{0};
UINT g_pendingStereoSlot = 0;
TrackedResource g_pendingStereoResource = {};
TrackedResource g_boundPsTrackedResources[32] = {};
std::uintptr_t g_backbufferResourcePtr = 0;
UINT g_backbufferWidth = 0;
UINT g_backbufferHeight = 0;
ID3D11Device* g_gameDevice = nullptr;       // retained (AddRef'd) for the OpenXR bridge
IDXGISwapChain* g_gameSwapChain = nullptr;  // retained (AddRef'd)
void** g_immediateVtable = nullptr;         // the immediate context's vtable (for comparison)
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
CreateDeferredContextFn g_originalCreateDeferredContext = nullptr;
void** g_createDeferredContextSlot = nullptr;
std::atomic_bool g_deferredLogged{false};
std::uintptr_t g_lastLoggedRtvResourcePtr = 0;
UINT g_lastLoggedRtvWidth = 0;
UINT g_lastLoggedRtvHeight = 0;
DXGI_FORMAT g_lastLoggedRtvFormat = DXGI_FORMAT_UNKNOWN;
UINT g_lastLoggedRtvCount = 0;
std::uintptr_t g_currentRtvResourcePtr = 0;
UINT g_currentRtvWidth = 0;
UINT g_currentRtvHeight = 0;
DXGI_FORMAT g_currentRtvFormat = DXGI_FORMAT_UNKNOWN;
TrackedResource g_trackedStereoResources[16] = {};
UINT g_trackedStereoResourceCount = 0;
D3D11_RECT g_lastLoggedScissor = {};
bool g_haveLastLoggedScissor = false;
void* g_vectoredHandler = nullptr;
std::atomic_bool g_breakpointsInstalled{false};
std::atomic<unsigned long long> g_breakpointTotalHits{0};
thread_local std::uintptr_t g_rearmBreakpointAddress = 0;

struct BreakpointProbe
{
    std::uintptr_t rva;
    const char* name;
    BYTE originalByte;
    BYTE* address;
    std::atomic<unsigned long long> hits;
    bool armed;
};

BreakpointProbe g_breakpointProbes[] = {
    {0x34C616, "vp-main-A-ret0", 0, nullptr, 0, false},
    {0x34A28E, "vp-main-A-ret1", 0, nullptr, 0, false},
    {0x34C269, "vp-main-A-ret2", 0, nullptr, 0, false},
    {0x1F862E, "vp-main-B-ret0", 0, nullptr, 0, false},
    {0x3AAFC5, "vp-main-B-ret1", 0, nullptr, 0, false},
    {0x34B296, "vp-main-B-ret2", 0, nullptr, 0, false},
    {0x34C2C2, "vp-main-B-ret3", 0, nullptr, 0, false},
    {0x3546AE, "vp-small-quarter-ret", 0, nullptr, 0, false},
};

// ======================= DIBR (depth-image-based stereo) - globals =======================
// Ported from ME1 d3d_capture.cpp (shipped single-tap guard-gather warp). Pure D3D11:
// capture scene depth at ClearDSV, warp the finished color frame into a synthesized right eye. No engine
// offsets. The whole pipeline is gated on g_depthMapEnabled, set true ONLY in VR mode 3 (DIBR).
constexpr UINT kClearDepthStencilViewSlot = 53;   // ID3D11DeviceContext::ClearDepthStencilView (TODO-2: verify)
using ClearDSVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
ClearDSVFn g_mhRealClearDSV = nullptr;   // MinHook trampoline to the real ClearDepthStencilView
bool g_mhClearDSVInstalled = false;
ID3D11DeviceContext* g_gameContext = nullptr;      // immediate context for the warp pass (grabbed lazily)

std::atomic<bool> g_depthMapEnabled{false};        // master: capture + warp run only when true (mode 3)
void* g_boundDepthRes = nullptr;                   // resource behind the bound DSV (identity only, never deref'd)
struct DepthDrawEntry { void* res; uint32_t draws; };
DepthDrawEntry g_depthDraws[16] = {};
constexpr uint32_t kSceneDepthDrawThreshold = 30;  // >= this many draws since last clear = a real geometry pass

ID3D11Texture2D* g_depthCopy = nullptr;            // capture scratch (may be a flat pooled pass; content-gated below)
ID3D11Texture2D* g_depthPublished = nullptr;       // last VERIFIED-real depth - the SRV views THIS, never the raw grab
ID3D11ShaderResourceView* g_depthSrv = nullptr;
UINT g_depthCopyW = 0, g_depthCopyH = 0;
std::atomic<bool> g_depthReady{false};
std::atomic<int> g_depthCapLogs{0};
std::atomic<int> g_depthMissLogs{0};
std::atomic<UINT> g_sceneRenderW{0};               // LIVE render res (== backbuffer); the scene-size gate matches THIS
std::atomic<UINT> g_sceneRenderH{0};
ID3D11Texture2D* g_depthStaging[2] = {nullptr, nullptr};   // non-blocking double-buffered probe readback
bool g_depthStagingInFlight[2] = {false, false};
int  g_depthStagingWrite = 0;
ID3D11Texture2D* g_depthGpuSnap[2] = {nullptr, nullptr};   // GPU snapshot ring (promote source, GPU->GPU ~free)
std::atomic<int> g_depthProbeLogs{0};
DXGI_FORMAT g_depthCopyFmt = DXGI_FORMAT_UNKNOWN;
int g_depthDecodeKind = 0;                          // 0=24-bit uint, 1=float32(4B), 2=float32(8B texel)
std::atomic<uint32_t> g_depthCopyCount{0};
std::atomic<float> g_probeCenter{0.0f}, g_probeTL{0.0f}, g_probeBR{0.0f}, g_probeTR{0.0f}, g_probeLC{0.0f}, g_probeRC{0.0f};

// warp shader + pipeline state
ID3D11VertexShader* g_depthVizVs = nullptr;
ID3D11PixelShader* g_depthVizPs = nullptr;
ID3D11PixelShader* g_depthMapPs = nullptr;
ID3D11SamplerState* g_depthVizSampler = nullptr;
ID3D11RasterizerState* g_depthVizRaster = nullptr;
ID3D11DepthStencilState* g_depthVizDepthState = nullptr;
ID3D11BlendState* g_depthVizBlend = nullptr;
ID3D11Buffer* g_dibrParamsCb = nullptr;
ID3D11Buffer* g_depthMapCb = nullptr;
bool g_depthVizReady = false;
bool g_depthVizTried = false;
// warp I/O
ID3D11Texture2D* g_dibrColorCopy = nullptr;
ID3D11ShaderResourceView* g_dibrColorSrv = nullptr;
UINT g_dibrColorW = 0, g_dibrColorH = 0;
ID3D11Texture2D* g_dibrWarpedTex = nullptr;
ID3D11RenderTargetView* g_dibrWarpedRtv = nullptr;
ID3D11ShaderResourceView* g_dibrWarpedSrv = nullptr;
UINT g_dibrWarpedW = 0, g_dibrWarpedH = 0;
// live warp tunables (overwritten each frame by SetDibrWarp; defaults match the guide)
std::atomic<float> g_dibrGain{2.00f};
std::atomic<float> g_dibrConvergence{0.985f};
std::atomic<float> g_dibrSign{1.0f};
std::atomic<float> g_dibrNearCut{0.965f};
std::atomic<float> g_dibrNearScale{85.0f};
std::atomic<float> g_dibrEdgeScale{120.0f};
std::atomic<float> g_dibrCrossScale{140.0f};
std::atomic<float> g_dibrLeakScale{400.0f};
std::atomic<float> g_dibrSilhouetteScale{180.0f};
std::atomic<float> g_dibrSourceScale{1.0f};

// Find (or claim a free slot for) the per-depth-RESOURCE draw counter. Render-thread only, so no lock.
uint32_t* DepthDrawCounter(void* res) noexcept
{
    if (res == nullptr) return nullptr;
    for (auto& e : g_depthDraws) if (e.res == res) return &e.draws;
    for (auto& e : g_depthDraws) if (e.res == nullptr) { e.res = res; e.draws = 0; return &e.draws; }
    return nullptr;
}
// ===================== end DIBR globals =====================

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) noexcept;
HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* factory,
                                              IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** swapChain) noexcept;
HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IDXGIFactory2* factory,
                                                     IUnknown* device,
                                                     HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                     IDXGIOutput* output,
                                                     IDXGISwapChain1** swapChain) noexcept;
HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindowHook(IDXGIFactory2* factory,
                                                           IUnknown* device,
                                                           IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept;
HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionHook(IDXGIFactory2* factory,
                                                           IUnknown* device,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept;
void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* context,
                                                UINT startSlot,
                                                UINT numViews,
                                                ID3D11ShaderResourceView* const* shaderResourceViews) noexcept;
void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* context,
                                       UINT indexCount,
                                       UINT startIndexLocation,
                                       INT baseVertexLocation) noexcept;
void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* context,
                                UINT vertexCount,
                                UINT startVertexLocation) noexcept;
void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* context,
                                                UINT indexCountPerInstance,
                                                UINT instanceCount,
                                                UINT startIndexLocation,
                                                INT baseVertexLocation,
                                                UINT startInstanceLocation) noexcept;
void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* context,
                                         UINT vertexCountPerInstance,
                                         UINT instanceCount,
                                         UINT startVertexLocation,
                                         UINT startInstanceLocation) noexcept;
void STDMETHODCALLTYPE DrawAutoHook(ID3D11DeviceContext* context) noexcept;
void STDMETHODCALLTYPE DrawIndexedInstancedIndirectHook(ID3D11DeviceContext* context,
                                                        ID3D11Buffer* bufferForArgs,
                                                        UINT alignedByteOffsetForArgs) noexcept;
void STDMETHODCALLTYPE DrawInstancedIndirectHook(ID3D11DeviceContext* context,
                                                 ID3D11Buffer* bufferForArgs,
                                                 UINT alignedByteOffsetForArgs) noexcept;
void STDMETHODCALLTYPE DispatchHook(ID3D11DeviceContext* context,
                                    UINT threadGroupCountX,
                                    UINT threadGroupCountY,
                                    UINT threadGroupCountZ) noexcept;
void STDMETHODCALLTYPE DispatchIndirectHook(ID3D11DeviceContext* context,
                                            ID3D11Buffer* bufferForArgs,
                                            UINT alignedByteOffsetForArgs) noexcept;
void STDMETHODCALLTYPE ExecuteCommandListHook(ID3D11DeviceContext* context,
                                              ID3D11CommandList* commandList,
                                              BOOL restoreContextState) noexcept;
void STDMETHODCALLTYPE CopySubresourceRegionHook(ID3D11DeviceContext* context,
                                                 ID3D11Resource* dstResource,
                                                 UINT dstSubresource,
                                                 UINT dstX,
                                                 UINT dstY,
                                                 UINT dstZ,
                                                 ID3D11Resource* srcResource,
                                                 UINT srcSubresource,
                                                 const D3D11_BOX* srcBox) noexcept;
void STDMETHODCALLTYPE CopyResourceHook(ID3D11DeviceContext* context,
                                        ID3D11Resource* dstResource,
                                        ID3D11Resource* srcResource) noexcept;
void STDMETHODCALLTYPE ResolveSubresourceHook(ID3D11DeviceContext* context,
                                              ID3D11Resource* dstResource,
                                              UINT dstSubresource,
                                              ID3D11Resource* srcResource,
                                              UINT srcSubresource,
                                              DXGI_FORMAT format) noexcept;
void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* context,
                                              UINT numViews,
                                              ID3D11RenderTargetView* const* renderTargetViews,
                                              ID3D11DepthStencilView* depthStencilView) noexcept;
void STDMETHODCALLTYPE RSSetViewportsHook(ID3D11DeviceContext* context,
                                          UINT numViewports,
                                          const D3D11_VIEWPORT* viewports) noexcept;
void STDMETHODCALLTYPE RSSetScissorRectsHook(ID3D11DeviceContext* context,
                                             UINT numRects,
                                             const D3D11_RECT* rects) noexcept;
const char* FormatName(DXGI_FORMAT format) noexcept;

bool WriteByte(BYTE* address, BYTE value) noexcept
{
    if (address == nullptr) return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(address, sizeof(BYTE), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return false;
    }

    *address = value;

    DWORD ignored = 0;
    VirtualProtect(address, sizeof(BYTE), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(BYTE));
    return true;
}

BreakpointProbe* FindBreakpointProbe(std::uintptr_t address) noexcept
{
    for (BreakpointProbe& probe : g_breakpointProbes)
    {
        if (probe.address != nullptr && reinterpret_cast<std::uintptr_t>(probe.address) == address)
        {
            return &probe;
        }
    }
    return nullptr;
}

LONG WINAPI BreakpointVectoredHandler(EXCEPTION_POINTERS* info) noexcept
{
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT)
    {
        auto* probe = FindBreakpointProbe(reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress));
        if (probe == nullptr) return EXCEPTION_CONTINUE_SEARCH;

        WriteByte(probe->address, probe->originalByte);
        g_rearmBreakpointAddress = reinterpret_cast<std::uintptr_t>(probe->address);

#if defined(_M_X64)
        info->ContextRecord->Rip = g_rearmBreakpointAddress;
#else
        info->ContextRecord->Eip = static_cast<DWORD>(g_rearmBreakpointAddress);
#endif
        info->ContextRecord->EFlags |= 0x100;

        const auto hit = probe->hits.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto total = g_breakpointTotalHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total <= 160 || (hit % 300) == 0)
        {
            char buffer[256] = {};
            sprintf_s(buffer,
                      "[ME2DISC] BREAKPOINT %s rva=0x%llX hit=%llu total=%llu thread=%lu",
                      probe->name,
                      static_cast<unsigned long long>(probe->rva),
                      static_cast<unsigned long long>(hit),
                      static_cast<unsigned long long>(total),
                      GetCurrentThreadId());
            ME2VR::Log::Line(buffer);
        }

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_SINGLE_STEP && g_rearmBreakpointAddress != 0)
    {
        auto* probe = FindBreakpointProbe(g_rearmBreakpointAddress);
        if (probe != nullptr)
        {
            WriteByte(probe->address, 0xCC);
        }
        g_rearmBreakpointAddress = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

template <typename T>
void SafeRelease(T*& ptr) noexcept
{
    if (ptr != nullptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

std::string HexPointer(const void* ptr)
{
    char buffer[32] = {};
    sprintf_s(buffer, "0x%p", ptr);
    return buffer;
}

std::string HexHRESULT(HRESULT hr)
{
    char buffer[32] = {};
    sprintf_s(buffer, "0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

std::string CallerTag(void* address)
{
    if (address == nullptr) return "unknown";

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase == nullptr)
    {
        return HexPointer(address);
    }

    const auto module = static_cast<HMODULE>(mbi.AllocationBase);
    char path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    const char* name = path;
    if (len > 0)
    {
        const char* slash = std::strrchr(path, '\\');
        const char* fwd = std::strrchr(path, '/');
        const char* sep = slash > fwd ? slash : fwd;
        if (sep != nullptr && sep[1] != '\0') name = sep + 1;
    }

    const auto rva = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
    char buffer[256] = {};
    sprintf_s(buffer, "%s+0x%llX", name, static_cast<unsigned long long>(rva));
    return buffer;
}

std::string StackTag()
{
    void* frames[16] = {};
    const USHORT count = CaptureStackBackTrace(0, static_cast<DWORD>((std::min)(static_cast<size_t>(_countof(frames)), size_t{12})), frames, nullptr);
    std::string result;
    for (USHORT i = 0; i < count; ++i)
    {
        if (!result.empty()) result += " <- ";
        result += CallerTag(frames[i]);
    }
    return result;
}

// Ordered single-frame trace: current bound RT (size/fmt/backbuffer) + the PS input texture slot 0.
void FirstFire(const char* method, int id) noexcept
{
    static std::atomic<unsigned> mask{0};
    const unsigned bit = 1u << (id & 31);
    const unsigned prev = mask.fetch_or(bit, std::memory_order_relaxed);
    if ((prev & bit) == 0)
        ME2VR::Log::Line(std::string("[ME2DISC] *** DRAW HOOK FIRST FIRE: ") + method + " ***");
}

void TraceOp(const char* op, ID3D11DeviceContext* ctx, UINT count) noexcept
{
    return; // expensive ordered draw trace is diagnostic-only
    if (!g_traceActive.load(std::memory_order_acquire)) return;
    const int seq = g_traceSeq.fetch_add(1, std::memory_order_relaxed);
    if (seq > 5000) return;
    unsigned inW = 0, inH = 0, inF = 0;
    if (ctx != nullptr && count != 0xFFFFFFFFu)   // draws: peek PS SRV slot 0
    {
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->PSGetShaderResources(0, 1, &srv);
        if (srv) { ID3D11Resource* r = nullptr; srv->GetResource(&r);
            if (r) { ID3D11Texture2D* t = nullptr;
                if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
                    D3D11_TEXTURE2D_DESC d = {}; t->GetDesc(&d); inW = d.Width; inH = d.Height; inF = d.Format; t->Release(); }
                r->Release(); }
            srv->Release(); }
    }
    const bool isBB = (g_currentRtvResourcePtr == g_backbufferResourcePtr && g_backbufferResourcePtr != 0);
    char buf[448] = {};
    sprintf_s(buf, "[ME2DISC] TRACE %03d %-10s rt=%ux%u fmt=%u bb=%d cnt=%u psIn=%ux%u/f%u vp=%.0f,%.0f %.0fx%.0f",
              seq, op, g_currentRtvWidth, g_currentRtvHeight, static_cast<unsigned>(g_currentRtvFormat),
              isBB ? 1 : 0, count, inW, inH, inF,
              g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.Height : -1.0f);
    ME2VR::Log::Line(buf);
}

void TraceCopy(const char* op, const TrackedResource& dst, const TrackedResource& src) noexcept
{
    if (!g_traceActive.load(std::memory_order_acquire)) return;
    const int seq = g_traceSeq.fetch_add(1, std::memory_order_relaxed);
    if (seq > 5000) return;
    const bool dstBB = (dst.ptr == g_backbufferResourcePtr && g_backbufferResourcePtr != 0);
    char b[256] = {};
    sprintf_s(b, "[ME2DISC] TRACE %03d %-12s dst=%ux%u/f%u bb=%d <- src=%ux%u/f%u",
              seq, op, dst.width, dst.height, static_cast<unsigned>(dst.format), dstBB ? 1 : 0,
              src.width, src.height, static_cast<unsigned>(src.format));
    ME2VR::Log::Line(b);
}

// UI-seam discovery: when a draw targets the backbuffer (where the UI/GFx pass lands), capture the
// call stack - the engine's UI render function sits above the D3D wrapper. Capped; log-only.
void LogUiSeamStack(const char* kind, UINT count) noexcept
{
    return;   // disabled: discovery done; removing per-draw overhead/flicker
    // The UI/GFx renders to an LDR target (R8G8B8A8 / B8G8R8A8, optionally sRGB); the world renders
    // to HDR intermediates. Capture LDR-target draws of ALL draw types -> the engine UI render fn
    // sits in their call stacks. Log the RT size+fmt so the mod can tell its own RT from the backbuffer.
    const UINT f = static_cast<UINT>(g_currentRtvFormat);
    const bool ldr = (f == 28 || f == 29 || f == 87 || f == 91);
    if (!ldr || g_currentRtvWidth == 0) return;
    const auto n = g_uiSeamStackCount.fetch_add(1, std::memory_order_relaxed);
    if (n >= 200) return;
    char hdr[176] = {};
    sprintf_s(hdr, "[ME2DISC] UISEAM #%llu %s cnt=%u rt=%ux%u fmt=%u -> ",
              static_cast<unsigned long long>(n), kind, count, g_currentRtvWidth, g_currentRtvHeight, f);
    ME2VR::Log::Line(std::string(hdr) + StackTag());
}

// The whole UI is rendered by GFx to its own texture, then blitted to the backbuffer full-width by a
// single full-screen quad (<=6 verts). That blit samples by screen position, so splitting its viewport
// only samples half. Instead: capture the blit's SOURCE texture (the complete UI) and SKIP the blit;
// me2_xr then submits that texture as a zero-disparity quad layer over the stereo world.
// Gated by the blit's distinctive caller (+0x4850A2) so the world's own passes are untouched.
bool CaptureUiCompositeAndSkip(ID3D11DeviceContext* context, UINT count) noexcept
{
    if (!g_uiPerEye.load(std::memory_order_acquire) || context == nullptr) return false;
    if (count > 6) return false;   // full-screen quad/tri only (cheap pre-filter)

    // Identify the UI blit purely by its caller signature (+0x4850A2) -- no dependence on per-frame
    // render-target/viewport state tracking (which was unreliable under the flip-model swapchain).
    if (g_exeBaseForUi == 0) g_exeBaseForUi = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* frames[24] = {};
    const USHORT nf = CaptureStackBackTrace(0, 24, frames, nullptr);
    bool isUi = false;
    for (USHORT i = 0; i < nf; ++i)
        if (reinterpret_cast<std::uintptr_t>(frames[i]) == g_exeBaseForUi + kUiCompositeCallerRva) { isUi = true; break; }
    if (!isUi) return false;

    // Scan the bound PS textures and pick the UI COLOR target (R8G8B8A8 family) - slot 0 is often a
    // single-channel mask (R8), not the color. Only treat this as the UI blit if a color texture exists.
    ID3D11ShaderResourceView* srvs[8] = {};
    context->PSGetShaderResources(0, 8, srvs);
    ID3D11Texture2D* colorTex = nullptr;
    D3D11_TEXTURE2D_DESC colorDesc = {};
    for (UINT s = 0; s < 8; ++s)
    {
        if (srvs[s] == nullptr) continue;
        ID3D11Resource* res = nullptr;
        srvs[s]->GetResource(&res);
        if (res != nullptr)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
            {
                D3D11_TEXTURE2D_DESC d = {};
                tex->GetDesc(&d);
                const UINT f = static_cast<UINT>(d.Format);
                const bool isColor = (f == 28 || f == 29 || f == 87 || f == 91);
                if (!g_uiTexLogged.load(std::memory_order_acquire))
                    ME2VR::Log::Line("[ME2DISC] UI blit slot " + std::to_string(s) + " = " +
                                     std::to_string(d.Width) + "x" + std::to_string(d.Height) + " fmt=" + std::to_string(f) +
                                     (isColor ? " [COLOR]" : ""));
                if (isColor && colorTex == nullptr && d.Width >= 256 && d.Height >= 256)
                {
                    colorTex = tex; colorDesc = d;   // keep this ref
                }
                else { tex->Release(); }
            }
            res->Release();
        }
        srvs[s]->Release();
    }
    g_uiTexLogged.store(true, std::memory_order_release);

    if (colorTex == nullptr) return false;   // not the UI color composite -> leave it alone

    if (g_uiSrcTexture != colorTex)
    {
        if (g_uiSrcTexture) g_uiSrcTexture->Release();
        g_uiSrcTexture = colorTex;
    }
    else { colorTex->Release(); }
    g_uiTexW = colorDesc.Width; g_uiTexH = colorDesc.Height; g_uiTexFmt = colorDesc.Format;
    return true;   // skip the full-width blit; the UI goes out as a quad layer instead
}

bool IsInterestingViewport(UINT numViewports, const D3D11_VIEWPORT* viewports) noexcept
{
    if (numViewports == 0 || viewports == nullptr) return false;
    const D3D11_VIEWPORT& vp = viewports[0];
    return (std::fabs(vp.Width - 1863.0f) <= 1.0f && std::fabs(vp.Height - 1048.0f) <= 1.0f) ||
           (std::fabs(vp.Width - 933.0f) <= 1.0f && std::fabs(vp.Height - 526.0f) <= 1.0f);
}

bool IsInterestingScissor(UINT numRects, const D3D11_RECT* rects) noexcept
{
    if (numRects == 0 || rects == nullptr) return false;
    const D3D11_RECT& rect = rects[0];
    const long width = rect.right - rect.left;
    const long height = rect.bottom - rect.top;
    if (rect.left == 0 && rect.top == 0 && width >= 3600 && height >= 2000) return false;
    return (width >= 1300 && height >= 1800) || (width >= 3600 && height <= 1100);
}

void TrackStereoResource(std::uintptr_t resourcePtr, const D3D11_TEXTURE2D_DESC& desc) noexcept
{
    if (resourcePtr == 0) return;
    const bool interesting =
        (desc.Width == 1863 && desc.Height == 1048) ||
        (desc.Width == 933 && desc.Height == 526) ||
        (desc.Width == 931 && desc.Height == 524);
    if (!interesting) return;

    for (UINT i = 0; i < g_trackedStereoResourceCount; ++i)
    {
        if (g_trackedStereoResources[i].ptr == resourcePtr) return;
    }

    if (g_trackedStereoResourceCount >= _countof(g_trackedStereoResources)) return;
    TrackedResource& tracked = g_trackedStereoResources[g_trackedStereoResourceCount++];
    tracked.ptr = resourcePtr;
    tracked.width = desc.Width;
    tracked.height = desc.Height;
    tracked.format = desc.Format;

    char buffer[256] = {};
    sprintf_s(buffer,
              "[ME2DISC] TRACK stereo texture tex=%p %ux%u fmt=%u %s",
              reinterpret_cast<void*>(resourcePtr),
              desc.Width,
              desc.Height,
              static_cast<unsigned int>(desc.Format),
              FormatName(desc.Format));
    ME2VR::Log::Line(buffer);
}

const TrackedResource* FindTrackedStereoResource(std::uintptr_t resourcePtr) noexcept
{
    if (resourcePtr == 0) return nullptr;
    for (UINT i = 0; i < g_trackedStereoResourceCount; ++i)
    {
        if (g_trackedStereoResources[i].ptr == resourcePtr) return &g_trackedStereoResources[i];
    }
    return nullptr;
}

const TrackedResource* FindBoundPsStereoResource(UINT* outSlot) noexcept
{
    for (UINT i = 0; i < _countof(g_boundPsTrackedResources); ++i)
    {
        if (g_boundPsTrackedResources[i].ptr != 0)
        {
            if (outSlot != nullptr) *outSlot = i;
            return &g_boundPsTrackedResources[i];
        }
    }
    return nullptr;
}

void FormatBoundPsSummary(char* buffer, size_t bufferSize) noexcept
{
    if (buffer == nullptr || bufferSize == 0) return;
    buffer[0] = '\0';

    size_t used = 0;
    for (UINT i = 0; i < _countof(g_boundPsTrackedResources); ++i)
    {
        const TrackedResource& bound = g_boundPsTrackedResources[i];
        if (bound.ptr == 0) continue;

        const int written = sprintf_s(buffer + used,
                                      bufferSize - used,
                                      "%ss%u=%p/%ux%u/f%u",
                                      used == 0 ? "" : ",",
                                      i,
                                      reinterpret_cast<void*>(bound.ptr),
                                      bound.width,
                                      bound.height,
                                      static_cast<unsigned int>(bound.format));
        if (written <= 0) break;
        used += static_cast<size_t>(written);
        if (used + 1 >= bufferSize) break;
    }

    if (used == 0)
    {
        sprintf_s(buffer, bufferSize, "none");
    }
}

bool ConsumeStateTraceBudget() noexcept
{
    unsigned expected = g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire);
    while (expected != 0 &&
           !g_traceAfterFullSizeStereoBind.compare_exchange_weak(expected,
                                                                 expected - 1,
                                                                 std::memory_order_acq_rel,
                                                                 std::memory_order_acquire))
    {
    }
    return expected != 0;
}

void LogStateTrace(const char* kind, void* caller, const char* detail) noexcept
{
    if (!ConsumeStateTraceBudget()) return;

    const auto slot = g_stateTraceLogCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= 512) return;

    const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
    const bool currentIsFullSizeTarget =
        g_backbufferWidth != 0 &&
        g_backbufferHeight != 0 &&
        g_currentRtvWidth == g_backbufferWidth &&
        g_currentRtvHeight == g_backbufferHeight;

    char boundSummary[320] = {};
    FormatBoundPsSummary(boundSummary, sizeof(boundSummary));

    char buffer[1024] = {};
    sprintf_s(buffer,
              "[ME2DISC] TRACE %s caller=%s detail={%s} currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f} boundPS={%s}",
              kind,
              CallerTag(caller).c_str(),
              detail != nullptr ? detail : "",
              reinterpret_cast<void*>(g_currentRtvResourcePtr),
              g_currentRtvWidth,
              g_currentRtvHeight,
              static_cast<unsigned int>(g_currentRtvFormat),
              currentIsBackbuffer ? 1 : 0,
              currentIsFullSizeTarget ? 1 : 0,
              g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
              g_haveCurrentViewport ? g_currentViewport.Height : -1.0f,
              boundSummary);
    ME2VR::Log::Line(buffer);
}

bool TryDescribeTextureResource(ID3D11Resource* resource, TrackedResource* out) noexcept
{
    if (resource == nullptr || out == nullptr) return false;
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    if (FAILED(hr) || texture == nullptr) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    SafeRelease(texture);

    out->ptr = reinterpret_cast<std::uintptr_t>(resource);
    out->width = desc.Width;
    out->height = desc.Height;
    out->format = desc.Format;
    return true;
}

bool IsFullSizeTracked(const TrackedResource& resource) noexcept
{
    return g_backbufferWidth != 0 &&
           g_backbufferHeight != 0 &&
           resource.width == g_backbufferWidth &&
           resource.height == g_backbufferHeight;
}

bool IsResourceCopyInteresting(const TrackedResource& dst, const TrackedResource& src) noexcept
{
    return g_pendingStereoCompositeDraws.load(std::memory_order_acquire) != 0 ||
           FindTrackedStereoResource(dst.ptr) != nullptr ||
           FindTrackedStereoResource(src.ptr) != nullptr ||
           dst.ptr == g_backbufferResourcePtr ||
           src.ptr == g_backbufferResourcePtr ||
           IsFullSizeTracked(dst) ||
           IsFullSizeTracked(src);
}

const char* FormatName(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    default: return "DXGI_FORMAT_OTHER";
    }
}

bool PatchPointerSlot(void** slot, void* hook, void** original, const char* name) noexcept
{
    if (slot == nullptr || hook == nullptr || original == nullptr || *slot == nullptr) return false;
    if (*slot == hook) return true;

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        ME2VR::Log::WindowsError((std::string("[ME2DISC] VirtualProtect failed for ") + name).c_str(), GetLastError());
        return false;
    }

    *original = *slot;
    *slot = hook;

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    ME2VR::Log::Line(std::string("[ME2DISC] hooked ") + name);
    return true;
}

bool EnsureOverlayRT() noexcept
{
    if (g_uiOverlayTex != nullptr) return true;
    if (g_gameDevice == nullptr || g_backbufferWidth == 0 || g_backbufferHeight == 0) return false;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_backbufferWidth; td.Height = g_backbufferHeight; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_uiOverlayTex)) || g_uiOverlayTex == nullptr) return false;
    g_gameDevice->CreateRenderTargetView(g_uiOverlayTex, nullptr, &g_uiOverlayRTV);
    g_gameDevice->CreateShaderResourceView(g_uiOverlayTex, nullptr, &g_uiOverlaySRV);
    ME2VR::Log::Line("[ME2DISC] UI overlay RT created " + std::to_string(g_backbufferWidth) + "x" + std::to_string(g_backbufferHeight));
    return g_uiOverlayRTV != nullptr;
}

// --- UI-caller histogram (instrument): which exe call-site issues each UI-signature draw? ---------
struct UiCaller { std::uintptr_t rva; unsigned count; };
UiCaller g_uiCallers[48] = {};
std::atomic_flag g_uiCallerLock = ATOMIC_FLAG_INIT;
std::uintptr_t g_exeRangeBase = 0, g_exeRangeSize = 0;

void EnsureExeRange() noexcept
{
    if (g_exeRangeBase != 0) return;
    HMODULE m = GetModuleHandleW(nullptr);
    if (m == nullptr) return;
    g_exeRangeBase = reinterpret_cast<std::uintptr_t>(m);
    __try
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exeRangeBase);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_exeRangeBase + dos->e_lfanew);
        g_exeRangeSize = nt->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_exeRangeSize = 0x4000000; }
}

void RecordUiCaller() noexcept
{
    EnsureExeRange();
    if (g_exeRangeBase == 0 || g_exeRangeSize == 0) return;
    void* fr[12] = {};
    const USHORT n = CaptureStackBackTrace(1, 12, fr, nullptr);   // skip RecordUiCaller itself
    std::uintptr_t rva = 0;
    for (USHORT i = 0; i < n; ++i)
    {
        const auto addr = reinterpret_cast<std::uintptr_t>(fr[i]);
        if (addr >= g_exeRangeBase && addr < g_exeRangeBase + g_exeRangeSize) { rva = addr - g_exeRangeBase; break; }
    }
    if (rva == 0) return;
    while (g_uiCallerLock.test_and_set(std::memory_order_acquire)) {}
    int freeSlot = -1;
    for (int i = 0; i < 48; ++i)
    {
        if (g_uiCallers[i].count != 0 && g_uiCallers[i].rva == rva) { g_uiCallers[i].count++; g_uiCallerLock.clear(std::memory_order_release); return; }
        if (freeSlot < 0 && g_uiCallers[i].count == 0) freeSlot = i;
    }
    if (freeSlot >= 0) { g_uiCallers[freeSlot].rva = rva; g_uiCallers[freeSlot].count = 1; }
    g_uiCallerLock.clear(std::memory_order_release);
}

void DumpUiCallers(const char* tag) noexcept
{
    UiCaller snap[48];
    while (g_uiCallerLock.test_and_set(std::memory_order_acquire)) {}
    for (int i = 0; i < 48; ++i) { snap[i] = g_uiCallers[i]; g_uiCallers[i].rva = 0; g_uiCallers[i].count = 0; }
    g_uiCallerLock.clear(std::memory_order_release);
    unsigned total = 0; int live = 0;
    for (int i = 0; i < 48; ++i) { total += snap[i].count; if (snap[i].count) ++live; }
    if (total == 0) return;
    // log top 8 by count
    char line[300] = {};
    int len = sprintf_s(line, "[ME2DISC] UICALLERS %s total=%u sites=%d:", tag, total, live);
    for (int top = 0; top < 8; ++top)
    {
        int best = -1; unsigned bestc = 0;
        for (int i = 0; i < 48; ++i) if (snap[i].count > bestc) { bestc = snap[i].count; best = i; }
        if (best < 0) break;
        len += sprintf_s(line + len, sizeof(line) - len, " +0x%llX=%u",
                         static_cast<unsigned long long>(snap[best].rva), snap[best].count);
        snap[best].count = 0;
    }
    ME2VR::Log::Line(line);
}

// [UIGATE] telemetry counters - which IsUiDrawNow exit fires, at what rate. Pure logging; printed
// 1/sec from the Present hook next to [SFRDIAG], reset each print. Exists because on the Meta runtime
// the whole UI treatment provably never engages while every gate term reads as it should pass.
std::atomic<uint64_t> g_ugCalls{0}, g_ugPreFail{0}, g_ugVpFail{0}, g_ugBlendFail{0}, g_ugExcl{0}, g_ugUiTrue{0};
std::atomic<uint32_t> g_ugLastFmt{0}, g_ugLastRtvW{0};

// A UI draw = full-viewport + LDR backbuffer target + alpha-blended (vs the opaque world composite).
// Scaleform draws menus/HUD this way. Fills vpOut with the (full) viewport when true.
bool IsUiDrawNow(ID3D11DeviceContext* ctx, D3D11_VIEWPORT& vpOut) noexcept
{
    vpOut = {};
    g_ugCalls.fetch_add(1, std::memory_order_relaxed);
    g_ugLastFmt.store(static_cast<uint32_t>(g_currentRtvFormat), std::memory_order_relaxed);
    g_ugLastRtvW.store(g_currentRtvWidth, std::memory_order_relaxed);
    if (!(g_backbufferWidth > 0 && g_currentRtvFormat == DXGI_FORMAT_R8G8B8A8_UNORM &&
          g_currentRtvWidth == g_backbufferWidth))
        { g_ugPreFail.fetch_add(1, std::memory_order_relaxed); return false; }
    UINT num = 1;
    ctx->RSGetViewports(&num, &vpOut);
    if (!(num == 1 && vpOut.Width > static_cast<float>(g_backbufferWidth) * 0.75f))
        { g_ugVpFail.fetch_add(1, std::memory_order_relaxed); return false; }
    ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT sampleMask = 0;
    ctx->OMGetBlendState(&bs, bf, &sampleMask);
    bool blended = false;
    D3D11_RENDER_TARGET_BLEND_DESC rt0 = {};
    if (bs != nullptr) { D3D11_BLEND_DESC bd = {}; bs->GetDesc(&bd); rt0 = bd.RenderTarget[0]; blended = rt0.BlendEnable != FALSE; bs->Release(); }
    if (!blended) { g_ugBlendFail.fetch_add(1, std::memory_order_relaxed); return false; }

    // [UIGHOST] A blended full-screen draw that READS a scene-sized texture can be either of two very
    // different things, and round 1 (excluding ALL of them) proved BOTH exist:
    //   - a POST-PROCESS effect (the glow/sheen pass sampling scene color) -> shrinking it into the
    //     [UIRATIO] sub-viewport painted the ~1/3-scale "glass after-image" of the scene mid-screen;
    //   - the Scaleform HUD LAYER COMPOSITE (the finished UI texture blended over the frame) ->
    //     excluding it dropped the whole HUD out of the UI treatment: vertically-stretched UI and
    //     every HUD correction gone (the round-2 report).
    // Size cannot split them (both sample backbuffer-sized textures, slot 0). The BLEND EQUATION can:
    // compositing a UI layer over the frame is alpha-compositing (DestBlend = INV_SRC_ALPHA, straight
    // or premultiplied), while glow/sheen effects are additive-family (DestBlend = ONE etc.). So a
    // scene-reading draw keeps UI treatment ONLY when it alpha-composites.
    ID3D11ShaderResourceView* srvs[4] = {};
    ctx->PSGetShaderResources(0, 4, srvs);
    bool sceneRead = false;
    UINT sceneFmt = 0; int sceneSlot = -1;
    for (int i = 0; i < 4; ++i)
    {
        if (srvs[i] == nullptr) continue;
        if (!sceneRead)
        {
            ID3D11Resource* res = nullptr;
            srvs[i]->GetResource(&res);
            if (res != nullptr)
            {
                ID3D11Texture2D* tex = nullptr;
                res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
                if (tex != nullptr)
                {
                    D3D11_TEXTURE2D_DESC td = {};
                    tex->GetDesc(&td);
                    if (td.Width == g_backbufferWidth && td.Height == g_backbufferHeight)
                    { sceneRead = true; sceneFmt = td.Format; sceneSlot = i; }
                    tex->Release();
                }
                res->Release();
            }
        }
        srvs[i]->Release();
    }
    bool isUi = true;
    if (sceneRead && rt0.DestBlend != D3D11_BLEND_INV_SRC_ALPHA) isUi = false;

    // [UIGHOST] census: one line per distinct (src,dest,fmt,slot,verdict) combo of scene-reading
    // blended draws, so the NEXT discrimination question is answered from the log, not by guessing.
    if (sceneRead)
    {
        static std::atomic<uint64_t> s_seen[8] = {};
        const uint64_t key = (uint64_t)rt0.SrcBlend | ((uint64_t)rt0.DestBlend << 8) |
                             ((uint64_t)sceneFmt << 16) | ((uint64_t)(sceneSlot & 0xF) << 32) |
                             ((uint64_t)(isUi ? 1 : 0) << 36) | (1ull << 63);
        bool logged = false;
        for (int i = 0; i < 8 && !logged; ++i)
        {
            uint64_t expected = s_seen[i].load(std::memory_order_relaxed);
            if (expected == key) logged = true;
            else if (expected == 0 && s_seen[i].compare_exchange_strong(expected, key))
            {
                char b[192] = {};
                sprintf_s(b, "[UIGHOST] scene-reading blended draw: src=%u dest=%u fmt=%u slot=%d -> %s",
                          (unsigned)rt0.SrcBlend, (unsigned)rt0.DestBlend, sceneFmt, sceneSlot,
                          isUi ? "UI (alpha-composite)" : "EXCLUDED (effect)");
                ME2VR::Log::Line(b);
                logged = true;
            }
        }
    }
    (isUi ? g_ugUiTrue : g_ugExcl).fetch_add(1, std::memory_order_relaxed);
    return isUi;
}


// Per-eye duplicate of a UI draw, preserving aspect (halve W and H, center vertically) so it fuses at
// screen depth without the 2:1 vertical stretch. Caller passes a thunk that re-issues the real draw.
template <typename DrawThunk>
void DupUiDraw(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, DrawThunk&& draw) noexcept
{
    D3D11_VIEWPORT h = vp;
    h.Width = vp.Width * 0.5f;
    h.Height = vp.Height * 0.5f;
    h.TopLeftY = vp.TopLeftY + vp.Height * 0.25f;
    h.TopLeftX = vp.TopLeftX;                                  // left eye
    g_originalRSSetViewports(ctx, 1, &h);
    draw();
    h.TopLeftX = vp.TopLeftX + vp.Width * 0.5f;                // right eye
    g_originalRSSetViewports(ctx, 1, &h);
    draw();
    g_originalRSSetViewports(ctx, 1, &vp);                     // restore full
}

// [SFR] draw the UI ONCE into a CENTERED, scaled-down sub-viewport, so flat-screen-sized HUD/popups
// don't span the whole headset FOV ("large / in your face"). scale<1 shrinks toward center; 1.0 = full.
template <typename DrawThunk>
void DrawUiScaled(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, float scale, DrawThunk&& draw) noexcept
{
    D3D11_VIEWPORT s = vp;
    s.Width  = vp.Width  * scale;
    s.Height = vp.Height * scale;
    s.TopLeftX = vp.TopLeftX + (vp.Width  - s.Width)  * 0.5f;   // centered
    s.TopLeftY = vp.TopLeftY + (vp.Height - s.Height) * 0.5f;
    g_originalRSSetViewports(ctx, 1, &s);
    draw();
    g_originalRSSetViewports(ctx, 1, &vp);
}

// ===================== [SFR-UI] left-eye UI mirror (same-frame stereo) =====================
// SFR renders the frame twice: pass 0 (LEFT, -halfEye) is snapshotted to g_sfrPass0Tex mid-present;
// pass 1 (RIGHT, +halfEye) is the backbuffer at Present. Scaleform draws the HUD ONCE at end-of-frame,
// into the backbuffer -> the RIGHT eye only. Mirror each UI draw into the held pass-0 texture so the
// LEFT eye gets the same HUD, fused at screen depth. Ported from ME1's SfrMirrorUiDrawIntoPass0.
// (These SFR pass-0 globals live here, above the Mh* draw hooks that need them; SfrEnsurePass0Tex /
//  SfrMaybeCapturePass0AtClear below reference them.)
ID3D11Texture2D* g_sfrPass0Tex = nullptr;
D3D11_TEXTURE2D_DESC g_sfrPass0Desc = {};
std::atomic<uint64_t> g_sfrPass0Caps{0};
uint64_t g_sfrClearsThisPresent = 0;   // [SFRDIAG] scene-sized clears captured this present (render thread)
// [PASS0ANCHOR] ROOT CAUSE of "gameplay loses VR after a cutscene" (measured 2026-07-31, Archangel):
// the pass-0 snapshot used to fire at EVERY scene-sized depth clear, overwriting, and relied on the
// LAST clear before present still holding pass 0's composite. That only holds if the clear count is
// stable. It is 4/present in open Omega gameplay - and after the Archangel cutscene the engine turns
// on extra passes and it becomes a fluctuating 6/8/10/12/14. Then the final clear often lands AFTER
// pass 1 composited, so the "left eye" texture was pass 1's image: both eyes got the +halfEye render,
// which is exactly why raising separation slid the world sideways instead of adding depth. Every
// counter looked healthy (replays=600, replayAvg=0.3ms) because the replay was never the problem.
//
// Anchor instead of guessing: pass 0 composites to the BACKBUFFER, so the first scene clear after the
// backbuffer has been bound this present is the start of pass 1 - at that instant the backbuffer holds
// pass 0's finished image. Capture exactly once there, whatever the clear count is. Both signals are
// render-thread-side (OMSetRenderTargets + ClearDepthStencilView), so there is no cross-thread arm
// flag and none of the race that caused the old left-eye flicker.
std::uintptr_t g_bbResourcePtr = 0;        // cached backbuffer resource identity (refreshed per present)
bool g_sfrBbBoundThisPresent = false;      // backbuffer bound as RTV => pass 0 has composited
bool g_sfrCapturedThisPresent = false;     // pass-0 snapshot already taken this present
std::atomic_bool g_pass0Anchor{true};      // toggle for A/B against the old last-clear behaviour
std::atomic<uint64_t> g_sfrAnchorMisses{0}; // presents where the anchor never fired (diagnostic)
// [CLEARMAP] Frame-structure instrumentation. Two blind anchor designs failed; this makes the third
// one data-driven. Per present, record every scene-sized depth clear as <depthResource>@<bbDraws so
// far>, so one Archangel run shows exactly which resource the real passes clear, what the extra
// post-cutscene clears hit, and where each sits relative to the pass-0 backbuffer composite. Logged
// on clear-count CHANGE (the 4 -> 6/8/14 transitions) plus a periodic heartbeat. Render-thread only.
char     g_clearMap[224] = {};
int      g_clearMapLen = 0;
uint64_t g_sfrBbDrawsThisPresent = 0;      // draws targeting the backbuffer so far this present
uint64_t g_sfrLastClearCount = 0;          // previous present's scene-clear count (change detector)
uint64_t g_sceneClearsSeen = 0;            // ALL scene-sized clears this present (captures are now 0-1)
// [CINECOPY] MEASUREMENT ONLY (2026-07-31). [CLEARGROUP] finds the pass boundary by counting DRAWS to
// the backbuffer. ME3 proved cutscenes composite the scene to the backbuffer with a COPY rather than a
// draw ([CINECAP] in the ME3 tree); if ME2 does the same, the draw counter stays frozen through a
// cutscene, every clear looks like half of a pair, and the snapshot lands arbitrarily = "VR cutscenes
// go flat at times". Counting copies separately proves or kills that WITHOUT changing any behaviour:
// if bbCopies climbs while bbDraws stays flat during a cine, the anchor must use draws+copies.
uint64_t g_sfrBbCopiesThisPresent = 0;
ID3D11RenderTargetView* g_sfrPass0Rtv = nullptr;   // [SFR-UI] lazy RTV onto pass 0 for the left-eye mirror
// [CLEARGROUP] ROOT CAUSE of the Bekenstein right-eye flicker (measured 2026-08-01, [CLEARMAP]):
// the party opens every render pass with FOUR scene clears at one draw count, not the two the
// [PASS1DOUBLE] anchor assumed, so "second double = pass 1 start" fired inside pass 0's own opening
// quad - at bbDraws=0 - and the "pass 0" snapshot was the PREVIOUS frame's final image. The party
// also injects a whole extra scene pass on alternating frames (2-pass/3-pass alternation in the
// map). Measured pass order: pass 0 FIRST, the extra pass in the MIDDLE (inside the engine's own
// Draw, after the main scene), pass 1 - the replay - always LAST (the live-backbuffer eye never
// flickered). Group rule: consecutive scene clears at one bbDraws count are ONE group, confirmed
// as a pass opening by its SECOND clear (mid-pass singles - the old combat false-trigger - never
// confirm). The snapshot is taken at EXACTLY the second confirmed opening, where the backbuffer is
// guaranteed to hold pass 0's finished composite on both 2-pass and 3-pass frames.
uint64_t g_sfrClearRunBbDraws = ~0ull;   // bbDraws count of the current same-count clear run
unsigned g_sfrClearRunLen = 0;           // consecutive clears in that run
unsigned g_sfrPassOpens = 0;             // confirmed pass-opening groups this present

constexpr int kSfrModeValue = 4;     // VrMode::Sfr
inline bool SfrModeActive() noexcept { return ME2VR::CalcViewHook::GetVrMode() == kSfrModeValue; }

// [EXITKILL] hard-terminate at ExitProcess entry - see the install site for the full story.
using ExitProcessFn = void(WINAPI*)(UINT);
ExitProcessFn g_origExitProcess = nullptr;
bool g_exitKillInstalled = false;

// Ground-truth prerecorded-video activity. ME2's engine mode and loading-manager state are
// both ambiguous: fullscreen Bink movies can report Cinematic while a live 3D scene keeps
// rendering behind them. Stamp the decoder itself, as the proven ME1 implementation does.
std::atomic<unsigned long long> g_lastBinkFrameMs{0};
bool g_binkHooksTried = false;
using BinkGenericFn = std::uintptr_t(*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
BinkGenericFn g_origBinkDoFrame = nullptr;
BinkGenericFn g_origBinkDoFrameAsync = nullptr;
BinkGenericFn g_origBinkDoFrameAsyncWait = nullptr;

std::uintptr_t HookBinkDoFrame(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    g_lastBinkFrameMs.store(GetTickCount64(), std::memory_order_relaxed);
    return g_origBinkDoFrame ? g_origBinkDoFrame(a, b, c, d) : 0;
}

std::uintptr_t HookBinkDoFrameAsync(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    g_lastBinkFrameMs.store(GetTickCount64(), std::memory_order_relaxed);
    return g_origBinkDoFrameAsync ? g_origBinkDoFrameAsync(a, b, c, d) : 0;
}

std::uintptr_t HookBinkDoFrameAsyncWait(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    g_lastBinkFrameMs.store(GetTickCount64(), std::memory_order_relaxed);
    return g_origBinkDoFrameAsyncWait ? g_origBinkDoFrameAsyncWait(a, b, c, d) : 0;
}

void TryInstallBinkHooks() noexcept
{
    if (g_binkHooksTried) return;
    HMODULE bink = GetModuleHandleW(L"bink2w64.dll");
    if (bink == nullptr) return;
    g_binkHooksTried = true;

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;

    auto install = [bink](const char* exportName, void* hookFn, void** original) noexcept {
        void* target = reinterpret_cast<void*>(GetProcAddress(bink, exportName));
        if (target == nullptr) return false;
        const MH_STATUS created = MH_CreateHook(target, hookFn, original);
        if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) return false;
        const MH_STATUS enabled = MH_EnableHook(target);
        return enabled == MH_OK || enabled == MH_ERROR_ENABLED;
    };

    const bool syncOk = install("BinkDoFrame", reinterpret_cast<void*>(&HookBinkDoFrame),
                                reinterpret_cast<void**>(&g_origBinkDoFrame));
    const bool asyncOk = install("BinkDoFrameAsync", reinterpret_cast<void*>(&HookBinkDoFrameAsync),
                                 reinterpret_cast<void**>(&g_origBinkDoFrameAsync));
    const bool waitOk = install("BinkDoFrameAsyncWait", reinterpret_cast<void*>(&HookBinkDoFrameAsyncWait),
                                reinterpret_cast<void**>(&g_origBinkDoFrameAsyncWait));
    ME2VR::Log::Line(std::string("[BINK] decoder hooks sync=") + (syncOk ? "1" : "0") +
                     " async=" + (asyncOk ? "1" : "0") + " wait=" + (waitOk ? "1" : "0"));
}
bool g_mhOMSetRTInstalled = false;   // [RTVDURABLE] inline hook replaces the vtable patch
void WINAPI ExitProcessHook(UINT uExitCode) noexcept
{
    TerminateProcess(GetCurrentProcess(), uExitCode);   // never returns; skips the hanging detach chain
}

ID3D11RenderTargetView* SfrPass0Rtv() noexcept
{
    if (g_sfrPass0Tex == nullptr || g_gameDevice == nullptr) return nullptr;
    if (g_sfrPass0Rtv == nullptr)
        g_gameDevice->CreateRenderTargetView(g_sfrPass0Tex, nullptr, &g_sfrPass0Rtv);
    return g_sfrPass0Rtv;
}

// Armed once pass-0 has been snapshotted this present (so the mirror composites onto the left-eye
// world image, not a stale/empty texture). UI draws come at end-of-frame, after both passes clear.
inline bool SfrUiMirrorArmed() noexcept
{
    return SfrModeActive() && g_sfrPass0Tex != nullptr && g_sfrClearsThisPresent >= 1;
}

// Only STRAIGHT/PREMULTIPLIED alpha (dst=INV_SRC_ALPHA) is mirrored: fullscreen FX quads (darken=
// multiply, additive) share the "full-viewport backbuffer" signature but must NOT land in the left
// eye with UI state (ME1's "colored overlay on one eye" family). The backbuffer/right eye keeps them
// (the game's own untouched draw); only the left-eye mirror is gated.
bool SfrUiMirrorClassify(ID3D11DeviceContext* ctx) noexcept
{
    ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT sm = 0;
    ctx->OMGetBlendState(&bs, bf, &sm);
    bool ok = false;
    if (bs != nullptr)
    {
        D3D11_BLEND_DESC bd = {}; bs->GetDesc(&bd);
        ok = bd.RenderTarget[0].BlendEnable != FALSE &&
             bd.RenderTarget[0].DestBlend == D3D11_BLEND_INV_SRC_ALPHA;
        bs->Release();
    }
    return ok;
}

// Re-issue a UI draw into the held pass-0 (left-eye) texture: same viewport/blend/shader state, only
// the color RTV changes. Keep the game's OWN depth-stencil bound (Scaleform clips HUD with stencil
// masks; a null DSV draws masked elements whole = the ME1 red-radar bug). Render thread only.
template <typename DrawThunk>
void SfrMirrorUiDrawIntoPass0(ID3D11DeviceContext* ctx, DrawThunk&& draw) noexcept
{
    if (!SfrUiMirrorArmed()) return;
    ID3D11RenderTargetView* rtv = SfrPass0Rtv();
    if (rtv == nullptr) return;
    if (!SfrUiMirrorClassify(ctx)) return;
    ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* savedDsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, &savedDsv);
    ctx->OMSetRenderTargets(1, &rtv, savedDsv);
    draw();
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, savedDsv);
    for (auto*& r : savedRtvs) if (r != nullptr) { r->Release(); r = nullptr; }
    if (savedDsv != nullptr) savedDsv->Release();
}

// Draw a UI element into BOTH eyes: the game's backbuffer (right) and the held pass-0 (left), through
// the HUD master transform (independent X/Y scale + center-relative offset, tuned in the HUD tab).
// Same viewport/state for both eyes so the HUD fuses at screen depth. Lower Scale X to un-stretch the
// HUD (SFR renders 16:9 into a ~square eye FOV). Mirror is a no-op until pass-0 is captured / non-alpha.
// [UIRATIO] auto projection-match: the game computes HUD screen positions (the CROSSHAIR glued to the
// aim point!) with ITS OWN FOV; the mod renders the world with a WIDER (fill) FOV. Scaling the UI by
// tan(gameFov)/tan(renderFov) per axis about EXACT center puts every game-projected UI element on its
// world point - bullets meet the crosshair at any zoom, and the 16:9-into-square vertical "stretch"
// dies for free (same mismatch). User Scale/Offset are trims ON TOP. Shared by SFR (both eyes) AND
// AER/DIBR (single full-frame copied to both eyes - same stretch, same fix). Returns true + fills `s`
// when a custom viewport is needed; false = draw at the full viewport.
bool ComputeUiRatioViewport(const D3D11_VIEWPORT& vp, D3D11_VIEWPORT& s) noexcept
{
    if (g_originalRSSetViewports == nullptr) return false;
    float autoX = 1.0f, autoY = 1.0f;
    const float gh = ME2VR::CalcViewHook::GetGameRawFovH();
    const float gv = ME2VR::CalcViewHook::GetGameRawFovV();
    const float rh = ME2VR::CalcViewHook::GetRenderHalfFovH();
    const float rv = ME2VR::CalcViewHook::GetRenderHalfFovV();
    bool sanX = false, sanY = false;
    if (gh > 0.01f && gv > 0.01f && rh > 0.01f && rv > 0.01f)
    {
        autoX = tanf(gh) / tanf(rh);
        autoY = tanf(gv) / tanf(rv);
        if (!(autoX > 0.25f && autoX < 1.5f)) { autoX = 1.0f; sanX = true; }   // sanity (also catches NaN)
        if (!(autoY > 0.25f && autoY < 1.5f)) { autoY = 1.0f; sanY = true; }
    }
    // [UIRATIO2] telemetry: log the LIVE inputs/outputs whenever they move (>0.01), max ~1 line/2s.
    // Pure logging - behavior identical on every runtime. Exists because the Link UI-stretch hunt
    // produced three self-consistent models that all predicted "correct"; the log names the liar.
    {
        static float lx = -1.0f, ly = -1.0f, lrh = -1.0f, lrv = -1.0f;
        static ULONGLONG lastMs = 0;
        const ULONGLONG now = GetTickCount64();
        const bool moved = fabsf(autoX - lx) > 0.01f || fabsf(autoY - ly) > 0.01f ||
                           fabsf(rh - lrh) > 0.01f || fabsf(rv - lrv) > 0.01f;
        if (moved && now - lastMs > 2000)
        {
            lx = autoX; ly = autoY; lrh = rh; lrv = rv; lastMs = now;
            char b[224];
            std::snprintf(b, sizeof(b),
                          "[UIRATIO2] gh=%.4f gv=%.4f rh=%.4f rv=%.4f -> autoX=%.3f autoY=%.3f%s%s",
                          gh, gv, rh, rv, autoX, autoY,
                          sanX ? " SANITY-RESET-X" : "", sanY ? " SANITY-RESET-Y" : "");
            ME2VR::Log::Line(b);
        }
    }
    // [CONVOFP] In a first-person conversation this ratio is poison. The mod's render FOV is pinned to the
    // headset, but gh/gv are the DIRECTOR'S per-shot FOV -- so the ratio, and with it the whole UI
    // viewport, was being rescaled at EVERY camera cut. That is the subtitles and the conversation
    // wheel resizing constantly. The ratio exists to glue game-projected UI (the crosshair) onto its
    // world point; in a conversation there is no such element, and the mod is not showing the director's
    // shot anyway, so tracking that FOV buys nothing. Hold the last gameplay ratio instead: stable
    // across cuts, and the wheel ends up exactly the size it is in normal play.
    //
    // SAMPLING WINDOW MATTERS. The first version latched "the last non-armed frame", which is wrong:
    // the game narrows its FOV into the cine shot BEFORE the mod arms, and on those frames ApplyFov takes
    // its narrow early-return so render == game and the ratio computes to exactly 1.0. Freezing 1.0
    // means no un-stretch at all, and the UI takes the full square-render stretch -- subtitles broken
    // in a new way. So only sample on genuinely WIDE gameplay (>= the same 0.55 rest threshold
    // ApplyFov uses), which rejects cine frames, the narrowing transition, and ADS alike.
    static float s_gameplayAutoX = 1.0f, s_gameplayAutoY = 1.0f;
    static bool  s_haveGameplayRatio = false;
    static bool  s_wasArmed = false;
    // [CINEUI] the freeze covers ANY cine presentation, not just an armed first-person conversation:
    // in-VR conversations/cutscenes (VrCineActive) render at the headset window while gh tracks the
    // DIRECTOR'S per-shot FOV, so the live ratio rescaled the whole UI viewport on every camera cut --
    // the "subtitles stretch and squash with the shot" report. Same cure as CONVOFP: hold the last wide-
    // gameplay sample, so subtitles/wheel keep exactly their normal-play size through every cut.
    const bool armed = ME2VR::ConvoFp::IsArmed() || ME2VR::CalcViewHook::GetVrCineActive();
    if (!armed && gh >= 0.55f && autoX > 0.25f && autoY > 0.25f)
    {
        s_gameplayAutoX = autoX; s_gameplayAutoY = autoY; s_haveGameplayRatio = true;
    }
    // A save can enter a cinematic before this process has ever observed wide gameplay.  In that
    // case, freezing the live director-shot ratio is catastrophic (the captured failure was X=.257).
    // Reconstruct the missing gameplay sample from ME2's normal 90-degree horizontal FOV and the
    // actual backbuffer aspect.  This is the same projection ratio a real gameplay frame supplies;
    // it is runtime/headset independent and avoids hard-coding the observed .961/.480 result.
    bool usingSyntheticGameplayRatio = false;
    if (armed && !s_haveGameplayRatio && rh > 0.01f && rv > 0.01f)
    {
        constexpr float kGameplayHalfFovH = 0.7853981633974483f;
        const float aspect = (g_backbufferWidth != 0 && g_backbufferHeight != 0)
            ? static_cast<float>(g_backbufferWidth) / static_cast<float>(g_backbufferHeight)
            : (16.0f / 9.0f);
        const float gameplayHalfFovV = atanf(tanf(kGameplayHalfFovH) / aspect);
        const float syntheticX = tanf(kGameplayHalfFovH) / tanf(rh);
        const float syntheticY = tanf(gameplayHalfFovV) / tanf(rv);
        if (syntheticX > 0.25f && syntheticX < 1.5f &&
            syntheticY > 0.25f && syntheticY < 1.5f)
        {
            autoX = syntheticX;
            autoY = syntheticY;
            usingSyntheticGameplayRatio = true;
        }
    }
    else if (armed)
    {
        autoX = s_gameplayAutoX;
        autoY = s_gameplayAutoY;
    }
    if (armed != s_wasArmed)
    {
        char b[160];
        std::snprintf(b, sizeof(b), "[CONVOFP] UI ratio %s: x=%.3f y=%.3f (gameplay sample %s)",
                      armed ? "FROZEN for conversation" : "released to live",
                      autoX, autoY, s_haveGameplayRatio ? "valid" :
                      (usingSyntheticGameplayRatio ? "synthetic baseline" : "MISSING - using live"));
        ME2VR::Log::Line(b);
        s_wasArmed = armed;
    }
    const float sx = autoX * ME2VR::CalcViewHook::GetSfrUiScaleX();
    const float sy = autoY * ME2VR::CalcViewHook::GetSfrUiScaleY();
    const float ox = ME2VR::CalcViewHook::GetSfrUiOffX();
    const float oy = ME2VR::CalcViewHook::GetSfrUiOffY();
    if (!(sx < 0.999f || sx > 1.001f || sy < 0.999f || sy > 1.001f || ox != 0.0f || oy != 0.0f)) return false;
    s = vp;
    s.Width  = vp.Width  * sx; s.Height = vp.Height * sy;
    s.TopLeftX = vp.TopLeftX + (vp.Width  - s.Width)  * 0.5f + ox * vp.Width;
    s.TopLeftY = vp.TopLeftY + (vp.Height - s.Height) * 0.5f + oy * vp.Height;
    return true;
}

template <typename DrawThunk>
void SfrDrawUiBothEyes(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, DrawThunk&& draw) noexcept
{
    D3D11_VIEWPORT s = {};
    const bool custom = ComputeUiRatioViewport(vp, s);
    if (custom) g_originalRSSetViewports(ctx, 1, &s);
    draw();                                 // -> backbuffer (right eye)
    // Both UI copies land at identical pixels DELIBERATELY: the pair then rides the whole-frame
    // convergence shift, which is the depth the player's eyes are already converged on for the
    // world - so the crosshair fuses against the geometry under it. A "text at screen depth" shift
    // was tried here (2026-08-01) and reverted the same day: it fixed subtitle fusion but broke
    // every element that overlays the 3D world (crosshair doubled against its aim point). UI depth
    // must never be decoupled from world convergence for world-anchored elements.
    SfrMirrorUiDrawIntoPass0(ctx, draw);    // -> pass 0 (left eye); no-op until armed / non-alpha
    if (custom) g_originalRSSetViewports(ctx, 1, &vp);
}

// AER/DIBR: ONE full-frame render is copied to both eyes, so the UI draws ONCE - no per-eye mirror.
// Just apply the [UIRATIO] scale so the HUD isn't stretched vertically like SFR/stereo were.
template <typename DrawThunk>
void DrawUiRatioOnce(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, DrawThunk&& draw) noexcept
{
    D3D11_VIEWPORT s = {};
    const bool custom = ComputeUiRatioViewport(vp, s);
    if (custom) g_originalRSSetViewports(ctx, 1, &s);
    draw();
    if (custom) g_originalRSSetViewports(ctx, 1, &vp);
}
// ==========================================================================================

void STDMETHODCALLTYPE MhDrawIndexed(ID3D11DeviceContext* ctx, UINT a, UINT b, INT c) noexcept
{
    // DIBR draw-gate: attribute this draw to the bound depth resource (gameplay geometry flows through this
    // MinHook'd path, so the counter MUST be bumped here or the depth capture never triggers).
    if (g_depthMapEnabled.load(std::memory_order_relaxed) && g_boundDepthRes != nullptr)
    { uint32_t* dc = DepthDrawCounter(g_boundDepthRes); if (dc != nullptr) (*dc)++; }
    if (g_mhRealDrawIndexed == nullptr) return;
    if (g_bbResourcePtr != 0 && g_currentRtvResourcePtr == g_bbResourcePtr) ++g_sfrBbDrawsThisPresent;   // [CLEARMAP]

    D3D11_VIEWPORT vp = {};
    const bool isUiDraw = IsUiDrawNow(ctx, vp);

    if (isUiDraw && g_uiDupEnabled.load(std::memory_order_acquire) && g_originalRSSetViewports != nullptr && ME2VR::CalcViewHook::GetVrEnabled() &&
        !(ME2VR::D3DCapture::GetMenuMode() || ME2VR::CalcViewHook::GetCinematic()))   // mono frame -> draw UI once
    {
        const int uiMode = ME2VR::CalcViewHook::GetVrMode();
        if (uiMode == 1) { DupUiDraw(ctx, vp, [&] { g_mhRealDrawIndexed(ctx, a, b, c); }); return; }   // SBS: per-eye halves
        if (uiMode == 4) { SfrDrawUiBothEyes(ctx, vp, [&] { g_mhRealDrawIndexed(ctx, a, b, c); }); return; }   // SFR: mirror into BOTH eyes
        if (uiMode == 2 || uiMode == 3) { DrawUiRatioOnce(ctx, vp, [&] { g_mhRealDrawIndexed(ctx, a, b, c); }); return; }   // AER/DIBR: un-stretch
    }
    g_mhRealDrawIndexed(ctx, a, b, c);
}
// Scaleform batches TEXT GLYPHS through DrawIndexedInstanced. Without dup, glyphs render once full-width
// and land cross-eyed (= scrambled text) while panels (DrawIndexed) look right. Dup it the same way.
void STDMETHODCALLTYPE MhDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT ipc, UINT ic, UINT sil, INT bvl, UINT sii) noexcept
{
    if (g_mhRealDrawIndexedInstanced == nullptr) return;
    if (g_bbResourcePtr != 0 && g_currentRtvResourcePtr == g_bbResourcePtr) ++g_sfrBbDrawsThisPresent;   // [CLEARMAP]
    D3D11_VIEWPORT vp = {};
    if (IsUiDrawNow(ctx, vp) && g_uiDupEnabled.load(std::memory_order_acquire) && g_originalRSSetViewports != nullptr && ME2VR::CalcViewHook::GetVrEnabled() &&
        !(ME2VR::D3DCapture::GetMenuMode() || ME2VR::CalcViewHook::GetCinematic()))   // mono frame -> draw UI once
    {
        const int uiMode = ME2VR::CalcViewHook::GetVrMode();
        if (uiMode == 1) { DupUiDraw(ctx, vp, [&] { g_mhRealDrawIndexedInstanced(ctx, ipc, ic, sil, bvl, sii); }); return; }   // SBS
        if (uiMode == 4) { SfrDrawUiBothEyes(ctx, vp, [&] { g_mhRealDrawIndexedInstanced(ctx, ipc, ic, sil, bvl, sii); }); return; }   // SFR: both eyes
        if (uiMode == 2 || uiMode == 3) { DrawUiRatioOnce(ctx, vp, [&] { g_mhRealDrawIndexedInstanced(ctx, ipc, ic, sil, bvl, sii); }); return; }   // AER/DIBR
    }
    g_mhRealDrawIndexedInstanced(ctx, ipc, ic, sil, bvl, sii);
}
void STDMETHODCALLTYPE MhDraw(ID3D11DeviceContext* ctx, UINT a, UINT b) noexcept
{
    if (g_mhRealDraw == nullptr) return;
    if (g_bbResourcePtr != 0 && g_currentRtvResourcePtr == g_bbResourcePtr) ++g_sfrBbDrawsThisPresent;   // [CLEARMAP]
    // Menu/codex TEXT is drawn via non-indexed Draw; panels use DrawIndexed. Both need the per-eye dup or
    // text renders once full-width -> cross-eyed -> scrambled.
    D3D11_VIEWPORT vp = {};
    const bool isUiDraw = IsUiDrawNow(ctx, vp);
    if (isUiDraw && g_uiDupEnabled.load(std::memory_order_acquire) && g_originalRSSetViewports != nullptr && ME2VR::CalcViewHook::GetVrEnabled() &&
        !(ME2VR::D3DCapture::GetMenuMode() || ME2VR::CalcViewHook::GetCinematic()))   // mono frame -> draw UI once
    {
        const int uiMode = ME2VR::CalcViewHook::GetVrMode();
        if (uiMode == 1) { DupUiDraw(ctx, vp, [&] { g_mhRealDraw(ctx, a, b); }); return; }   // SBS: per-eye halves
        if (uiMode == 4) { SfrDrawUiBothEyes(ctx, vp, [&] { g_mhRealDraw(ctx, a, b); }); return; }   // SFR: mirror into BOTH eyes
        if (uiMode == 2 || uiMode == 3) { DrawUiRatioOnce(ctx, vp, [&] { g_mhRealDraw(ctx, a, b); }); return; }   // AER/DIBR: un-stretch
    }
    g_mhRealDraw(ctx, a, b);
}

// ============================ DIBR (depth-image-based stereo) - functions ============================
bool EnsureGameContext() noexcept
{
    if (g_gameContext != nullptr) return true;
    if (g_gameDevice == nullptr) return false;
    g_gameDevice->GetImmediateContext(&g_gameContext);   // AddRef'd; held for process lifetime
    return g_gameContext != nullptr;
}

void FillDibrParamBuffer(float* p, float eyeScale) noexcept
{
    if (p == nullptr) return;
    p[0]  = g_dibrGain.load(std::memory_order_relaxed);
    p[1]  = g_dibrConvergence.load(std::memory_order_relaxed);
    p[2]  = g_dibrSign.load(std::memory_order_relaxed);
    p[3]  = g_dibrNearCut.load(std::memory_order_relaxed);
    p[4]  = g_dibrNearScale.load(std::memory_order_relaxed);
    p[5]  = g_dibrEdgeScale.load(std::memory_order_relaxed);
    p[6]  = g_dibrCrossScale.load(std::memory_order_relaxed);
    p[7]  = g_dibrLeakScale.load(std::memory_order_relaxed);
    p[8]  = 0.0f;   // (was labMode; layout kept)
    p[9]  = g_dibrSilhouetteScale.load(std::memory_order_relaxed);
    p[10] = g_dibrSourceScale.load(std::memory_order_relaxed);
    p[11] = eyeScale;
}

// SRV-able copy of the finished color frame (the warp samples this while the mod renders the synth eye).
bool EnsureColorCopy(const D3D11_TEXTURE2D_DESC& bbDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_dibrColorCopy != nullptr && g_dibrColorW == bbDesc.Width && g_dibrColorH == bbDesc.Height) return true;
    SafeRelease(g_dibrColorSrv); SafeRelease(g_dibrColorCopy);
    D3D11_TEXTURE2D_DESC cd = bbDesc;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.Usage = D3D11_USAGE_DEFAULT;
    cd.CPUAccessFlags = 0; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_dibrColorCopy);
    if (FAILED(hr) || g_dibrColorCopy == nullptr) { ME2VR::Log::Line("[DIBR] color copy create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateShaderResourceView(g_dibrColorCopy, nullptr, &g_dibrColorSrv);
    if (FAILED(hr) || g_dibrColorSrv == nullptr) { ME2VR::Log::Line("[DIBR] color srv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_dibrColorCopy); return false; }
    g_dibrColorW = bbDesc.Width; g_dibrColorH = bbDesc.Height;
    ME2VR::Log::Line("[DIBR] color copy created " + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    return true;
}

// Synthesized-right-eye target (backbuffer format/size, so it CopyResource's straight into the OpenXR eye).
bool EnsureWarpedTex(const D3D11_TEXTURE2D_DESC& bbDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_dibrWarpedTex != nullptr && g_dibrWarpedW == bbDesc.Width && g_dibrWarpedH == bbDesc.Height) return true;
    SafeRelease(g_dibrWarpedSrv); SafeRelease(g_dibrWarpedRtv); SafeRelease(g_dibrWarpedTex);
    D3D11_TEXTURE2D_DESC cd = bbDesc;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_dibrWarpedTex);
    if (FAILED(hr) || g_dibrWarpedTex == nullptr) { ME2VR::Log::Line("[DIBR] warped tex create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateRenderTargetView(g_dibrWarpedTex, nullptr, &g_dibrWarpedRtv);
    if (FAILED(hr) || g_dibrWarpedRtv == nullptr) { ME2VR::Log::Line("[DIBR] warped rtv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_dibrWarpedTex); return false; }
    g_gameDevice->CreateShaderResourceView(g_dibrWarpedTex, nullptr, &g_dibrWarpedSrv);
    g_dibrWarpedW = bbDesc.Width; g_dibrWarpedH = bbDesc.Height;
    ME2VR::Log::Line("[DIBR] warped eye tex created " + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    return true;
}

bool CompileDepthShader(const char* src, const char* entry, const char* profile, ID3DBlob** blob) noexcept
{
    if (blob == nullptr) return false;
    *blob = nullptr;
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (comp == nullptr) comp = LoadLibraryW(L"d3dcompiler_43.dll");
    if (comp == nullptr) { ME2VR::Log::Line("[DIBR] no d3dcompiler dll"); return false; }
    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                          LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(comp, "D3DCompile"));
    if (compile == nullptr) { FreeLibrary(comp); ME2VR::Log::Line("[DIBR] no D3DCompile export"); return false; }
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, profile, 0, 0, blob, &err);
    if (FAILED(hr))
    {
        if (err) { ME2VR::Log::Line(std::string("[DIBR] compile err: ") + reinterpret_cast<const char*>(err->GetBufferPointer())); err->Release(); }
        FreeLibrary(comp);
        return false;
    }
    if (err) err->Release();
    FreeLibrary(comp);
    return true;
}

bool EnsureDepthVizShaders() noexcept
{
    if (g_depthVizReady) return true;
    if (g_depthVizTried) return false;
    g_depthVizTried = true;
    if (g_gameDevice == nullptr) return false;
    // Guard-gather DIBR warp (verbatim from ME1 - game-agnostic; reversed-Z assumed in the disparity sign).
    const char* src =
        "Texture2D colorTex : register(t0);\n"
        "Texture2D depthTex : register(t1);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer DibrParams : register(b0) { float gain; float convergence; float sgn; float nearCut; float nearScale; float edgeScale; float crossScale; float leakScale; float labMode; float silhouetteScale; float sourceScale; float eyeScale; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID){ float2 p=float2((id==2)?3.0:-1.0,(id==1)?3.0:-1.0);\n"
        "  VSOut o; o.pos=float4(p,0.0,1.0); o.uv=float2((p.x+1.0)*0.5, 1.0-((p.y+1.0)*0.5)); return o; }\n"
        "float2 SourceUv(float2 uv){ return saturate((uv - 0.5) / max(sourceScale, 0.01) + 0.5); }\n"
        // EDGE-SHIMMER FIX (2026-07-12): silhouette pixels are where GPU depth is noisiest frame-to-frame,
        // and a raw single-tap depth drives the per-pixel disparity AND every guard comparison - so a 1-frame
        // depth wobble at an edge pixel visibly crawls the edge even with a static camera. Pseudo-median of 5
        // taps (center + 4 diagonals at ~1.25 texels): a single noisy tap is rejected outright instead of
        // averaged in, true edges stay sharp (median preserves steps), and both the disparity and the guards
        // see the same stabilized value.
        "float Med3(float a, float b, float c){ return max(min(a,b), min(max(a,b), c)); }\n"
        "float DepthMed5(float2 uv){\n"
        "  uint tw, th; depthTex.GetDimensions(tw, th);\n"
        "  float2 t = 1.25 / float2(max(tw,1u), max(th,1u));\n"
        "  float a = depthTex.Sample(s0, uv).r;\n"
        "  float b = depthTex.Sample(s0, saturate(uv + float2( t.x,  t.y))).r;\n"
        "  float c = depthTex.Sample(s0, saturate(uv + float2(-t.x,  t.y))).r;\n"
        "  float e = depthTex.Sample(s0, saturate(uv + float2( t.x, -t.y))).r;\n"
        "  float f = depthTex.Sample(s0, saturate(uv + float2(-t.x, -t.y))).r;\n"
        "  return Med3(Med3(a, b, c), e, f);\n"
        "}\n"
        "float DepthEdge(float2 uv, float d){\n"
        "  float2 t=float2(1.0/1024.0,1.0/1024.0);\n"
        "  float dl=depthTex.Sample(s0, float2(saturate(uv.x-t.x),uv.y)).r;\n"
        "  float dr=depthTex.Sample(s0, float2(saturate(uv.x+t.x),uv.y)).r;\n"
        "  float du=depthTex.Sample(s0, float2(uv.x,saturate(uv.y-t.y))).r;\n"
        "  float dd=depthTex.Sample(s0, float2(uv.x,saturate(uv.y+t.y))).r;\n"
        "  return saturate(max(max(abs(d-dl),abs(d-dr)),max(abs(d-du),abs(d-dd))) * edgeScale);\n"
        "}\n"
        "float SilhouetteCross(float2 uv, float2 warpedUv, float d){\n"
        "  float occ=0.0;\n"
        "  float2 u1=lerp(uv, warpedUv, 0.25);\n"
        "  float2 u2=lerp(uv, warpedUv, 0.50);\n"
        "  float2 u3=lerp(uv, warpedUv, 0.75);\n"
        "  float2 u4=warpedUv;\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u1).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u2).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u3).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u4).r - 0.0010) * silhouetteScale));\n"
        "  return occ;\n"
        "}\n"
        "float4 PSMain(VSOut i):SV_Target{\n"
        "  float2 baseUv = SourceUv(i.uv);\n"
        "  float d = DepthMed5(baseUv);\n"
        "  float disparity = sgn * gain * eyeScale * (convergence - d);\n"
        "  float edge = DepthEdge(baseUv, d);\n"
        "  float2 probeUv = SourceUv(float2(saturate(i.uv.x + disparity), i.uv.y));\n"
        "  float probeD = DepthMed5(probeUv);\n"
        "  float crs = saturate(abs(probeD - d) * crossScale);\n"
        "  float nearFg = saturate((nearCut - d) * nearScale);\n"
        "  float fgLeak = saturate((probeD - d - 0.0005) * leakScale);\n"
        "  float silhouette = SilhouetteCross(baseUv, probeUv, d);\n"
        "  float guard = 1.0 - saturate(max(max(max(edge, crs), fgLeak), silhouette));\n"
        "  guard *= (1.0 - nearFg);\n"
        "  guard = guard * guard;\n"
        "  float2 warpedUv = SourceUv(float2(saturate(i.uv.x + disparity * guard), i.uv.y));\n"
        "  return colorTex.Sample(s0, warpedUv);\n"
        "}\n";
    const char* mapSrc =
        "Texture2D depthTex : register(t1);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer DepthMapParams : register(b0) { float nearD; float farD; float flipD; float gammaD; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 PSMap(VSOut i):SV_Target{\n"
        "  float d = depthTex.Sample(s0, i.uv).r;\n"
        "  float g = saturate((d - farD) / max(nearD - farD, 1e-5));\n"
        "  if (flipD > 0.5) g = 1.0 - g;\n"
        "  g = pow(saturate(g), max(gammaD, 0.05));\n"
        "  return float4(g, g, g, 1.0);\n"
        "}\n";
    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* mapPs = nullptr;
    if (!CompileDepthShader(src, "VSMain", "vs_4_0", &vs) ||
        !CompileDepthShader(src, "PSMain", "ps_4_0", &ps) ||
        !CompileDepthShader(mapSrc, "PSMap", "ps_4_0", &mapPs))
    { if (vs) vs->Release(); if (ps) ps->Release(); if (mapPs) mapPs->Release(); return false; }
    HRESULT hr = g_gameDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_depthVizVs);
    if (SUCCEEDED(hr)) hr = g_gameDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_depthVizPs);
    if (SUCCEEDED(hr)) hr = g_gameDevice->CreatePixelShader(mapPs->GetBufferPointer(), mapPs->GetBufferSize(), nullptr, &g_depthMapPs);
    vs->Release(); ps->Release(); mapPs->Release();
    if (FAILED(hr) || g_depthVizVs == nullptr || g_depthVizPs == nullptr) { ME2VR::Log::Line("[DIBR] shader create failed"); return false; }
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_gameDevice->CreateSamplerState(&sd, &g_depthVizSampler);
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    g_gameDevice->CreateRasterizerState(&rd, &g_depthVizRaster);
    D3D11_DEPTH_STENCIL_DESC dd = {}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
    g_gameDevice->CreateDepthStencilState(&dd, &g_depthVizDepthState);
    D3D11_BLEND_DESC bd = {}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_gameDevice->CreateBlendState(&bd, &g_depthVizBlend);
    D3D11_BUFFER_DESC cbd = {}; cbd.ByteWidth = 48; cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_gameDevice->CreateBuffer(&cbd, nullptr, &g_dibrParamsCb);
    D3D11_BUFFER_DESC mcbd = {}; mcbd.ByteWidth = 16; mcbd.Usage = D3D11_USAGE_DYNAMIC;
    mcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; mcbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_gameDevice->CreateBuffer(&mcbd, nullptr, &g_depthMapCb);
    if (g_depthVizSampler == nullptr || g_depthVizRaster == nullptr || g_depthVizDepthState == nullptr) { ME2VR::Log::Line("[DIBR] state create failed"); return false; }
    g_depthVizReady = true;
    ME2VR::Log::Line("[DIBR] warp shaders ready");
    return true;
}

// Copy + SRV formats FOLLOW the source. A hardcoded R24G8 copy of a D32 source is an illegal CopyResource
// that D3D silently drops -> uninitialized flat depth. (TODO-1: add a case if LE2's format isn't here.)
bool EnsureDepthCopy(const D3D11_TEXTURE2D_DESC& srcDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_depthCopy != nullptr && g_depthCopyW == srcDesc.Width && g_depthCopyH == srcDesc.Height &&
        g_depthCopyFmt == srcDesc.Format) return true;
    DXGI_FORMAT copyFmt, srvFmt; int decode;
    switch (srcDesc.Format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        copyFmt = DXGI_FORMAT_R24G8_TYPELESS; srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; decode = 0; break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        copyFmt = DXGI_FORMAT_R32_TYPELESS; srvFmt = DXGI_FORMAT_R32_FLOAT; decode = 1; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        copyFmt = DXGI_FORMAT_R32G8X24_TYPELESS; srvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; decode = 2; break;
    default:
        { static int s_fmtLogs = 0; if (s_fmtLogs++ < 3) ME2VR::Log::Line("[DIBR] unsupported depth format " + std::to_string(static_cast<int>(srcDesc.Format))); return false; }
    }
    SafeRelease(g_depthSrv); SafeRelease(g_depthCopy); SafeRelease(g_depthPublished);
    g_depthReady.store(false, std::memory_order_release);
    D3D11_TEXTURE2D_DESC cd = srcDesc;
    cd.Format = copyFmt; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.Usage = D3D11_USAGE_DEFAULT;
    cd.CPUAccessFlags = 0; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthCopy);
    if (FAILED(hr) || g_depthCopy == nullptr) { ME2VR::Log::Line("[DIBR] copy tex create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthPublished);
    if (FAILED(hr) || g_depthPublished == nullptr) { ME2VR::Log::Line("[DIBR] published tex create failed hr=" + HexHRESULT(hr)); SafeRelease(g_depthCopy); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srvFmt; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    hr = g_gameDevice->CreateShaderResourceView(g_depthPublished, &sd, &g_depthSrv);
    if (FAILED(hr) || g_depthSrv == nullptr) { ME2VR::Log::Line("[DIBR] srv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_depthCopy); SafeRelease(g_depthPublished); return false; }
    SafeRelease(g_depthStaging[0]); SafeRelease(g_depthStaging[1]);
    SafeRelease(g_depthGpuSnap[0]); SafeRelease(g_depthGpuSnap[1]);
    g_depthStagingInFlight[0] = g_depthStagingInFlight[1] = false; g_depthStagingWrite = 0;
    D3D11_TEXTURE2D_DESC stg = cd; stg.BindFlags = 0; stg.Usage = D3D11_USAGE_STAGING; stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_gameDevice->CreateTexture2D(&stg, nullptr, &g_depthStaging[0]);
    g_gameDevice->CreateTexture2D(&stg, nullptr, &g_depthStaging[1]);
    g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthGpuSnap[0]);
    g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthGpuSnap[1]);
    g_depthCopyW = srcDesc.Width; g_depthCopyH = srcDesc.Height;
    g_depthCopyFmt = srcDesc.Format; g_depthDecodeKind = decode;
    char fbuf[144];
    std::snprintf(fbuf, sizeof(fbuf), "[DIBR] sampleable copy created %ux%u srcFmt=%d copyFmt=%d decode=%d",
                  srcDesc.Width, srcDesc.Height, static_cast<int>(srcDesc.Format), static_cast<int>(copyFmt), decode);
    ME2VR::Log::Line(fbuf);
    return true;
}

// Snapshot 'tex' into scratch, probe every 4th capture (non-blocking staging ring), and PROMOTE a
// spread-verified capture to g_depthPublished (hold-last-good otherwise). Render-thread only.
void RunDepthCapturePipeline(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) noexcept
{
    if (ctx == nullptr || tex == nullptr || !EnsureDepthCopy(d)) return;
    ctx->CopyResource(g_depthCopy, tex);
    g_depthCopyCount.fetch_add(1, std::memory_order_relaxed);
    const uint32_t pc = g_depthProbeLogs.fetch_add(1, std::memory_order_relaxed);
    // Every 2nd capture (was 4th): the auto-convergence input was updating at quarter rate and adding lag on
    // top of its own smoothing. The staging ring is 2-deep and non-blocking, so 1/2 cadence is still safe.
    if ((pc % 2u) != 0u || g_depthStaging[0] == nullptr || g_depthStaging[1] == nullptr) return;
    const int w = g_depthStagingWrite; const int r = 1 - w;
    ctx->CopyResource(g_depthStaging[w], g_depthCopy);
    if (g_depthGpuSnap[w] != nullptr) ctx->CopyResource(g_depthGpuSnap[w], g_depthCopy);
    g_depthStagingInFlight[w] = true; g_depthStagingWrite = r;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (!g_depthStagingInFlight[r] ||
        FAILED(ctx->Map(g_depthStaging[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;
    g_depthStagingInFlight[r] = false;
    const int decodeKind = g_depthDecodeKind;
    auto sample = [&](UINT x, UINT y) -> float {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        if (decodeKind == 1) return *reinterpret_cast<const float*>(row + static_cast<size_t>(x) * 4u);
        if (decodeKind == 2) return *reinterpret_cast<const float*>(row + static_cast<size_t>(x) * 8u);
        const uint32_t v = *reinterpret_cast<const uint32_t*>(row + static_cast<size_t>(x) * 4u);
        return static_cast<float>(v & 0x00FFFFFFu) / 16777215.0f;
    };
    // CONVERGENCE STABILITY (2026-07-12: auto-convergence "keeps moving around"): the convergence
    // input was a SINGLE center pixel - any depth noise or a thin prop crossing one pixel yanked the whole
    // stereo convergence plane. Now: median of a 3x3 grid spread over the central ~20% of the frame. The
    // median rejects outlier pixels entirely (a wall edge or particle through one tap changes nothing) while
    // still tracking what the player is actually looking at. Raw center kept for the log comparison.
    const float centerRaw = sample(d.Width / 2, d.Height / 2);
    float med[9];
    {
        const UINT cx = d.Width / 2, cy = d.Height / 2;
        const UINT ox = d.Width / 10, oy = d.Height / 10;
        int n9 = 0;
        for (int gy = -1; gy <= 1; ++gy)
            for (int gx = -1; gx <= 1; ++gx)
                med[n9++] = sample(cx + gx * ox, cy + gy * oy);
        // insertion sort 9 floats; median = med[4]
        for (int i = 1; i < 9; ++i)
        {
            const float v = med[i];
            int j = i - 1;
            while (j >= 0 && med[j] > v) { med[j + 1] = med[j]; --j; }
            med[j + 1] = v;
        }
    }
    const float center = med[4];
    const float tl = sample(24, 24);
    const float br = sample(d.Width - 24, d.Height - 24);
    const float tr = sample(d.Width - 24, 24);
    const float lc = sample(d.Width / 4, d.Height / 2);
    const float rc = sample((d.Width * 3) / 4, d.Height / 2);
    g_probeCenter.store(center, std::memory_order_relaxed);
    g_probeTL.store(tl, std::memory_order_relaxed); g_probeBR.store(br, std::memory_order_relaxed);
    g_probeTR.store(tr, std::memory_order_relaxed); g_probeLC.store(lc, std::memory_order_relaxed);
    g_probeRC.store(rc, std::memory_order_relaxed);
    float mn = center, mx = center;
    const float taps[5] = { tl, br, tr, lc, rc };
    for (float t : taps) { if (t < mn) mn = t; if (t > mx) mx = t; }
    const bool good = (mx - mn) > 0.005f;
    if (pc < 45u || (pc % 240u) == 0u)
    {
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "[DIBR_PROBE] depth centerMed=%.5f centerRaw=%.5f TL=%.5f BR=%.5f TR=%.5f L=%.5f R=%.5f spread=%.5f %s",
                      center, centerRaw, tl, br, tr, lc, rc, mx - mn, good ? "PROMOTE" : "hold");
        ME2VR::Log::Line(buf);
    }
    ctx->Unmap(g_depthStaging[r], 0);
    if (good && g_depthPublished != nullptr && g_depthGpuSnap[r] != nullptr)
    {
        ctx->CopyResource(g_depthPublished, g_depthGpuSnap[r]);
        g_depthReady.store(true, std::memory_order_release);
    }
}

// Capture the scene depth into the sampleable copy BEFORE the game wipes it. Only active in DIBR mode
// (g_depthMapEnabled); DIBR renders mono so the clear-time capture is intact (no stereo trample).
// ================= [SFR] pass-0 (left eye) mid-present capture =================
// The SFR double render overwrites the backbuffer with pass 1 (right, +halfEye) by present, so pass 0
// (left, -halfEye) is grabbed MID-present. ME2 records its scene rendering on a DEFERRED context (the
// immediate-context DRAW hooks fire ~never in SFR, proven by [SFRDIAG] drawCalls=1), and the game
// thread (DrawDetour) is decoupled from that stream - so a cross-thread arm flag raced (left-eye
// flicker) and a game-thread D3D copy crashed. The one thing that IS reliably in the deferred stream in
// order: the scene depth CLEARS (the clear hook fires for every one). So the fix needs NO flag at all:
// capture the backbuffer at EVERY scene-sized depth clear, overwriting. Each pass starts with a scene
// clear and composites at its END, so at a pass's clear the backbuffer still holds the PREVIOUS pass's
// final image. Pass 1 (the replay) is the LAST pass, so its clear is the last capture before present =
// pass 0's finished image. Deterministic, fully in submission order, no race.
// [SFR-UI] g_sfrPass0Tex / g_sfrPass0Desc / g_sfrPass0Caps / g_sfrClearsThisPresent / g_sfrPass0Rtv
// and SfrModeActive() are defined above (moved up so the Mh* UI draw hooks can reference them).

bool SfrEnsurePass0Tex(const D3D11_TEXTURE2D_DESC& bb) noexcept
{
    if (g_sfrPass0Tex != nullptr &&
        g_sfrPass0Desc.Width == bb.Width && g_sfrPass0Desc.Height == bb.Height &&
        g_sfrPass0Desc.Format == bb.Format)
        return true;
    if (g_sfrPass0Tex != nullptr) { g_sfrPass0Tex->Release(); g_sfrPass0Tex = nullptr; }
    if (g_sfrPass0Rtv != nullptr) { g_sfrPass0Rtv->Release(); g_sfrPass0Rtv = nullptr; }   // [SFR-UI] stale after recreate
    D3D11_TEXTURE2D_DESC d = bb;
    // [SFR-UI] BIND_RENDER_TARGET so UI draws can be mirrored into the left-eye pass; still a valid
    // CopyResource dest (the snapshot) and source (submit copies it to the left eye swapchain).
    d.BindFlags = D3D11_BIND_RENDER_TARGET; d.CPUAccessFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
    if (g_gameDevice == nullptr || FAILED(g_gameDevice->CreateTexture2D(&d, nullptr, &g_sfrPass0Tex)))
    { g_sfrPass0Tex = nullptr; return false; }
    g_sfrPass0Desc = d;
    return true;
}

void SfrMaybeCapturePass0AtClear(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv) noexcept
{
    if (ctx == nullptr || dsv == nullptr || g_gameSwapChain == nullptr) return;
    // scene-sized gate: only the replay's real full-scene depth clear (not a small pre-pass/reflection).
    ID3D11Resource* dres = nullptr;
    dsv->GetResource(&dres);
    if (dres == nullptr) return;
    bool sceneSized = false;
    ID3D11Texture2D* dtex = nullptr;
    if (SUCCEEDED(dres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dtex))) && dtex != nullptr)
    {
        D3D11_TEXTURE2D_DESC dd = {}; dtex->GetDesc(&dd);
        const UINT rw = g_sceneRenderW.load(std::memory_order_acquire);
        const UINT rh = g_sceneRenderH.load(std::memory_order_acquire);
        sceneSized = (rw != 0) && (dd.Width == rw) && (dd.Height == rh) && (dd.SampleDesc.Count == 1);
        dtex->Release();
    }
    dres->Release();
    if (!sceneSized) return;

    ++g_sceneClearsSeen;
    // [CLEARMAP] record this scene clear: which depth resource (identity only, low 20 bits is plenty
    // to tell targets apart) and how many backbuffer draws had happened when it fired.
    if (ME2VR::Log::DiagnosticsOn() && g_clearMapLen < (int)sizeof(g_clearMap) - 20)
    {
        const int w = sprintf_s(g_clearMap + g_clearMapLen, sizeof(g_clearMap) - g_clearMapLen,
                                " %05llx@%llu",
                                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(dres) & 0xFFFFFull),
                                static_cast<unsigned long long>(g_sfrBbDrawsThisPresent));
        if (w > 0) g_clearMapLen += w;
    }

    // [CLEARGROUP] one snapshot per confirmed pass boundary, last one wins. Measured 2026-07-31
    // ([CLEARMAP], 146 samples, healthy + broken) and 2026-08-01 (Bekenstein): real passes open with
    // scene-sized clears at ONE bbDraws count - two in most areas, FOUR at the party - while the
    // extra clears in combat areas are SINGLES scattered mid-pass ("@22 @27 @31"). The old rule
    // ("second DOUBLE = pass 1 start") counted clear PAIRS, so a four-clear opening consumed both
    // pairs at bbDraws=0 and the snapshot held the PREVIOUS frame's final image: the right-eye
    // flicker. Group rule: consecutive clears at one bbDraws count are ONE group, confirmed as a
    // pass opening by its SECOND clear (singles never confirm - the combat false-trigger stays
    // excluded). The FIRST confirmed opening of a present is pass' own start (backbuffer still
    // holds last frame); every LATER confirmed opening means the backbuffer holds the previous
    // pass's finished composite - snapshot there, overwriting. The last confirmed opening is
    // pass 1's start, so the final snapshot is pass 0's composite even when a mission stacks extra
    // scene passes ahead of pass 0 (the party's alternating third pass).
    if (g_pass0Anchor.load(std::memory_order_relaxed))
    {
        if (g_sfrBbDrawsThisPresent == g_sfrClearRunBbDraws) ++g_sfrClearRunLen;
        else { g_sfrClearRunBbDraws = g_sfrBbDrawsThisPresent; g_sfrClearRunLen = 1; }
        if (g_sfrClearRunLen != 2) return;   // only a group's 2nd clear confirms it; singles never do
        // Capture at EXACTLY the second confirmed opening. Measured pass order (Bekenstein,
        // 2026-08-01, two failed anchors' worth of [CLEARMAP]): pass 0 renders FIRST, the party's
        // extra scene pass renders in the MIDDLE (inside the engine's own Draw, after the main
        // scene), and pass 1 - the replay - is always LAST (the live-backbuffer eye never
        // flickered). So the second opening is the only boundary where the backbuffer is
        // guaranteed to hold pass 0's finished composite: at the third it already holds the
        // extra pass. A last-opening-wins overwrite was tried and captured the extra pass's
        // image on alternating frames = the flicker survived.
        if (++g_sfrPassOpens != 2) return;
    }

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || bb == nullptr)
        return;
    D3D11_TEXTURE2D_DESC bd = {}; bb->GetDesc(&bd);
    if (SfrEnsurePass0Tex(bd))
    {
        ctx->CopyResource(g_sfrPass0Tex, bb);   // snapshot the backbuffer at THIS pass boundary
        ++g_sfrClearsThisPresent;
        g_sfrPass0Caps.fetch_add(1, std::memory_order_relaxed);
        g_sfrCapturedThisPresent = true;
        // [CLEARMAP] mark the capture point in the map so the log shows WHERE the snapshot landed
        if (g_clearMapLen > 0 && g_clearMapLen < (int)sizeof(g_clearMap) - 2)
        { g_clearMap[g_clearMapLen++] = '*'; g_clearMap[g_clearMapLen] = '\0'; }
    }
    bb->Release();
}

void STDMETHODCALLTYPE MhClearDepthStencilView(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
                                              UINT flags, FLOAT depthVal, UINT8 stencil) noexcept
{
    // [SFR] pass-0 capture: snapshot the backbuffer at EVERY scene-sized depth clear. Each pass clears
    // depth at its start and composites at its end, so at a clear the backbuffer holds the PREVIOUS
    // pass's final image; pass 1 (the replay) is last, so its clear = the last snapshot before present =
    // pass 0's finished left-eye image. No flag/marker = no cross-context race.
    if (SfrModeActive())
        SfrMaybeCapturePass0AtClear(ctx, dsv);
    // Bring-up instrumentation: confirm the hook actually fires + whether capture is enabled.
    {
        static std::atomic<int> s_dsvFires{0};
        const int f = s_dsvFires.fetch_add(1, std::memory_order_relaxed);
        if (f < 3 || (g_depthMapEnabled.load(std::memory_order_relaxed) && (f % 600) == 0))
            ME2VR::Log::Line("[DIBR] ClearDSV fired #" + std::to_string(f) + " enabled=" +
                             std::to_string(g_depthMapEnabled.load(std::memory_order_relaxed) ? 1 : 0));
    }
    if (ctx != nullptr && dsv != nullptr && g_depthMapEnabled.load(std::memory_order_relaxed))
    {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res != nullptr)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex != nullptr)
            {
                D3D11_TEXTURE2D_DESC d = {}; tex->GetDesc(&d);
                const UINT renderW = g_sceneRenderW.load(std::memory_order_acquire);
                const UINT renderH = g_sceneRenderH.load(std::memory_order_acquire);
                const bool sceneSized = (renderW != 0) && (d.Width == renderW) && (d.Height == renderH);
                if (!sceneSized && d.SampleDesc.Count == 1 && d.Width >= 512)
                {
                    const int dn = g_depthMissLogs.fetch_add(1, std::memory_order_relaxed);
                    if (dn < 20)
                        ME2VR::Log::Line("[DIBR] non-scene depth clear " + std::to_string(d.Width) + "x" +
                                         std::to_string(d.Height) + " (render=" + std::to_string(renderW) + "x" +
                                         std::to_string(renderH) + ")");
                }
                if (sceneSized && d.SampleDesc.Count == 1)
                {
                    uint32_t* counter = DepthDrawCounter(res);
                    const uint32_t drawsSince = (counter != nullptr) ? *counter : 0;
                    if (counter != nullptr) *counter = 0;
                    // FLICKER FIX (LE2): two scene-sized depth buffers compete each frame - the real full scene
                    // (~1200 draws) and a partial pre-pass/reflection (~58 draws). Capturing BOTH alternated the
                    // published depth -> flicker. Reject anything well below the running PEAK draw count (adapts to
                    // scene complexity instead of a brittle fixed threshold), so only the richest depth wins.
                    static uint32_t s_peakDraws = 0;
                    if (drawsSince > s_peakDraws) s_peakDraws = drawsSince;
                    else if (s_peakDraws > 8) s_peakDraws -= (s_peakDraws >> 7);   // slow decay (~0.8%/clear)
                    const uint32_t adaptiveGate = s_peakDraws / 3;
                    const uint32_t gate = (adaptiveGate > kSceneDepthDrawThreshold) ? adaptiveGate : kSceneDepthDrawThreshold;
                    const bool populated = drawsSince >= gate;
                    const int n = g_depthCapLogs.fetch_add(1, std::memory_order_relaxed);
                    if (n < 60 || (n % 600) == 0)
                    {
                        char clbuf[208];
                        std::snprintf(clbuf, sizeof(clbuf),
                                      "[DIBR] scene clear %ux%u fmt=%d drawsSince=%u gate=%u populated=%d clearVal=%.2f",
                                      d.Width, d.Height, static_cast<int>(d.Format), drawsSince, gate, populated ? 1 : 0, depthVal);
                        ME2VR::Log::Line(clbuf);
                    }
                    if (populated) RunDepthCapturePipeline(ctx, tex, d);
                }
                tex->Release();
            }
            res->Release();
        }
    }
    if (g_mhRealClearDSV != nullptr) g_mhRealClearDSV(ctx, dsv, flags, depthVal, stencil);
}

// Warp the finished color frame into 'outTex' using the captured depth. Full pipeline save/restore.
ID3D11Texture2D* RenderDibrEye(ID3D11Texture2D* backBuffer, ID3D11Texture2D* outTex, ID3D11RenderTargetView* outRtv, float eyeScale) noexcept
{
    if (!EnsureGameContext() || backBuffer == nullptr || outTex == nullptr || outRtv == nullptr) return nullptr;
    if (!g_depthReady.load(std::memory_order_acquire) || g_depthSrv == nullptr) return nullptr;
    if (!EnsureDepthVizShaders()) return nullptr;
    D3D11_TEXTURE2D_DESC bbd = {}; backBuffer->GetDesc(&bbd);
    g_gameContext->CopyResource(g_dibrColorCopy, backBuffer);

    ID3D11RenderTargetView* oldRtv[8] = {}; ID3D11DepthStencilView* oldDsv = nullptr;
    g_gameContext->OMGetRenderTargets(8, oldRtv, &oldDsv);
    D3D11_VIEWPORT oldVp[16] = {}; UINT oldVpN = 16; g_gameContext->RSGetViewports(&oldVpN, oldVp);
    ID3D11RasterizerState* oldRs = nullptr; g_gameContext->RSGetState(&oldRs);
    ID3D11DepthStencilState* oldDs = nullptr; UINT oldRef = 0; g_gameContext->OMGetDepthStencilState(&oldDs, &oldRef);
    float oldBlendFactor[4] = {}; UINT oldSampleMask = 0xffffffff; ID3D11BlendState* oldBlend = nullptr;
    g_gameContext->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo; g_gameContext->IAGetPrimitiveTopology(&oldTopo);
    ID3D11InputLayout* oldIl = nullptr; g_gameContext->IAGetInputLayout(&oldIl);
    ID3D11VertexShader* oldVs = nullptr; g_gameContext->VSGetShader(&oldVs, nullptr, nullptr);
    ID3D11PixelShader* oldPs = nullptr; g_gameContext->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv[2] = {}; g_gameContext->PSGetShaderResources(0, 2, oldSrv);
    ID3D11SamplerState* oldSamp = nullptr; g_gameContext->PSGetSamplers(0, 1, &oldSamp);

    D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(bbd.Width); vp.Height = static_cast<float>(bbd.Height); vp.MaxDepth = 1.0f;
    const float bf[4] = {0, 0, 0, 0};
    g_gameContext->OMSetRenderTargets(1, &outRtv, nullptr);
    g_gameContext->RSSetViewports(1, &vp);
    g_gameContext->RSSetState(g_depthVizRaster);
    g_gameContext->OMSetDepthStencilState(g_depthVizDepthState, 0);
    g_gameContext->OMSetBlendState(g_depthVizBlend, bf, 0xffffffff);
    g_gameContext->IASetInputLayout(nullptr);
    g_gameContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_gameContext->VSSetShader(g_depthVizVs, nullptr, 0);
    g_gameContext->PSSetShader(g_depthVizPs, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { g_dibrColorSrv, g_depthSrv };
    g_gameContext->PSSetShaderResources(0, 2, srvs);
    g_gameContext->PSSetSamplers(0, 1, &g_depthVizSampler);
    if (g_dibrParamsCb != nullptr)
    {
        D3D11_MAPPED_SUBRESOURCE mp = {};
        if (SUCCEEDED(g_gameContext->Map(g_dibrParamsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
        {
            FillDibrParamBuffer(static_cast<float*>(mp.pData), eyeScale);
            g_gameContext->Unmap(g_dibrParamsCb, 0);
        }
        g_gameContext->PSSetConstantBuffers(0, 1, &g_dibrParamsCb);
    }
    g_gameContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[2] = {}; g_gameContext->PSSetShaderResources(0, 2, nullSrv);
    g_gameContext->OMSetRenderTargets(8, oldRtv, oldDsv);
    g_gameContext->RSSetViewports(oldVpN, oldVp);
    g_gameContext->RSSetState(oldRs);
    g_gameContext->OMSetDepthStencilState(oldDs, oldRef);
    g_gameContext->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    g_gameContext->IASetPrimitiveTopology(oldTopo);
    g_gameContext->IASetInputLayout(oldIl);
    g_gameContext->VSSetShader(oldVs, nullptr, 0);
    g_gameContext->PSSetShader(oldPs, nullptr, 0);
    g_gameContext->PSSetShaderResources(0, 2, oldSrv);
    g_gameContext->PSSetSamplers(0, 1, &oldSamp);
    for (auto*& rt : oldRtv) SafeRelease(rt);
    SafeRelease(oldDsv); SafeRelease(oldRs); SafeRelease(oldDs); SafeRelease(oldBlend); SafeRelease(oldIl);
    SafeRelease(oldVs); SafeRelease(oldPs); SafeRelease(oldSrv[0]); SafeRelease(oldSrv[1]); SafeRelease(oldSamp);
    return outTex;
}
// ============================ end DIBR functions ============================

void InstallContextHooks(ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr ||
        (g_psSetShaderResourcesSlot != nullptr &&
         g_drawIndexedSlot != nullptr &&
         g_drawSlot != nullptr &&
         g_drawIndexedInstancedSlot != nullptr &&
         g_drawInstancedSlot != nullptr &&
         g_omSetRenderTargetsSlot != nullptr &&
         g_drawAutoSlot != nullptr &&
         g_drawIndexedInstancedIndirectSlot != nullptr &&
         g_drawInstancedIndirectSlot != nullptr &&
         g_dispatchSlot != nullptr &&
         g_dispatchIndirectSlot != nullptr &&
         g_rsSetViewportsSlot != nullptr &&
         g_rsSetScissorRectsSlot != nullptr &&
         g_copySubresourceRegionSlot != nullptr &&
         g_copyResourceSlot != nullptr &&
         g_resolveSubresourceSlot != nullptr &&
         g_executeCommandListSlot != nullptr))
    {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr) return;
    g_immediateVtable = vtable;

    // MinHook the real draw FUNCTIONS (not just vtable slots) so cached-pointer callers are caught.
    if (!g_mhDrawInstalled.exchange(true))
    {
        MH_Initialize();
        void* realDI = vtable[kDrawIndexedSlot];
        void* realD = vtable[kDrawSlot];
        const bool diOk = (MH_CreateHook(realDI, reinterpret_cast<void*>(&MhDrawIndexed),
                                         reinterpret_cast<void**>(&g_mhRealDrawIndexed)) == MH_OK) &&
                          (MH_EnableHook(realDI) == MH_OK);
        const bool dOk = (MH_CreateHook(realD, reinterpret_cast<void*>(&MhDraw),
                                        reinterpret_cast<void**>(&g_mhRealDraw)) == MH_OK) &&
                         (MH_EnableHook(realD) == MH_OK);
        // MinHook DrawIndexedInstanced too (Scaleform text). Read the REAL fn before any vtable patch.
        void* realDII = vtable[kDrawIndexedInstancedSlot];
        const bool diiOk = (MH_CreateHook(realDII, reinterpret_cast<void*>(&MhDrawIndexedInstanced),
                                          reinterpret_cast<void**>(&g_mhRealDrawIndexedInstanced)) == MH_OK) &&
                           (MH_EnableHook(realDII) == MH_OK);
        g_drawIndexedInstancedSlot = &vtable[kDrawIndexedInstancedSlot];   // mark done (MinHooked, not patched)
        ME2VR::Log::Line("[ME2DISC] MinHook real DrawIndexed=" + HexPointer(realDI) + " (" + std::to_string(diOk) +
                         ") Draw=" + HexPointer(realD) + " (" + std::to_string(dOk) + ")" +
                         " DrawIndexedInstanced=" + HexPointer(realDII) + " (" + std::to_string(diiOk) + ")");

        // DIBR: MinHook the REAL ClearDepthStencilView too - the vtable patch never fired (this game clears
        // depth on the deferred/RHI path, exactly like draws). Read the real fn from the slot BEFORE any patch.
        if (!g_mhClearDSVInstalled)
        {
            void* realClearDSV = vtable[kClearDepthStencilViewSlot];
            const bool cOk = (MH_CreateHook(realClearDSV, reinterpret_cast<void*>(&MhClearDepthStencilView),
                                            reinterpret_cast<void**>(&g_mhRealClearDSV)) == MH_OK) &&
                             (MH_EnableHook(realClearDSV) == MH_OK);
            g_mhClearDSVInstalled = true;
            ME2VR::Log::Line("[DIBR] MinHook real ClearDepthStencilView=" + HexPointer(realClearDSV) +
                             " (" + std::to_string(cOk) + ")");
        }
    }

    // [EXITKILL] ported from ME1 (2026-07-19 Quest Link exit-hang fix): on Meta's PC runtime the game
    // wedges on exit - once the game stops presenting the mod never drives a clean xrEndSession, and Meta's
    // runtime DLL then hangs inside its OWN DllMain(DETACH) during the ExitProcess unload chain (log
    // ends abruptly mid-present). Intercept ExitProcess at ENTRY and hard-terminate: no detach
    // handlers, no CRT atexit, no hang. Nothing is lost - the ini is saved during play (menu
    // interactions), never at exit. VD/SteamVR never hung; harmless there (ME1 ships this everywhere).
    if (!g_exitKillInstalled)
    {
        g_exitKillInstalled = true;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        void* ep = (k32 != nullptr) ? reinterpret_cast<void*>(GetProcAddress(k32, "ExitProcess")) : nullptr;
        const bool eOk = ep != nullptr &&
                         MH_CreateHook(ep, reinterpret_cast<void*>(&ExitProcessHook),
                                       reinterpret_cast<void**>(&g_origExitProcess)) == MH_OK &&
                         MH_EnableHook(ep) == MH_OK;
        ME2VR::Log::Line(std::string("[EXITKILL] ExitProcess entry hook installed (") + (eOk ? "1" : "0") + ")");
    }

    if (g_psSetShaderResourcesSlot == nullptr &&
        PatchPointerSlot(&vtable[kPSSetShaderResourcesSlot],
                         reinterpret_cast<void*>(&PSSetShaderResourcesHook),
                         reinterpret_cast<void**>(&g_originalPSSetShaderResources),
                         "ID3D11DeviceContext::PSSetShaderResources"))
    {
        g_psSetShaderResourcesSlot = &vtable[kPSSetShaderResourcesSlot];
    }

    if (g_drawIndexedSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawIndexedSlot],
                         reinterpret_cast<void*>(&DrawIndexedHook),
                         reinterpret_cast<void**>(&g_originalDrawIndexed),
                         "ID3D11DeviceContext::DrawIndexed"))
    {
        g_drawIndexedSlot = &vtable[kDrawIndexedSlot];
    }

    if (g_drawSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawSlot],
                         reinterpret_cast<void*>(&DrawHook),
                         reinterpret_cast<void**>(&g_originalDraw),
                         "ID3D11DeviceContext::Draw"))
    {
        g_drawSlot = &vtable[kDrawSlot];
    }

    // DrawIndexedInstanced is MinHooked above (not vtable-patched) so it actually fires for the
    // cached-pointer Scaleform text path.

    if (g_drawInstancedSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawInstancedSlot],
                         reinterpret_cast<void*>(&DrawInstancedHook),
                         reinterpret_cast<void**>(&g_originalDrawInstanced),
                         "ID3D11DeviceContext::DrawInstanced"))
    {
        g_drawInstancedSlot = &vtable[kDrawInstancedSlot];
    }

    // [RTVDURABLE] MinHook the REAL OMSetRenderTargets instead of patching the vtable slot.
    // Measured 2026-07-27: a vtable-slot patch here is NOT durable. Meta evicts it at XR-session
    // start, and on EVERY runtime an alt-tab tears down device state and replaces the vtable --
    // after which this tracker stops populating (UIGATE: fmt=0 rtvW=0, ui=0 of 480k draws), the
    // whole UI treatment stops running, and 16:9 UI drawn into a 0.889 eye view is stretched
    // exactly 2x vertically. [VTFIX] re-patching the slot did NOT bring the tracker back. The draw
    // and clear hooks are MinHook inline hooks and survive both events untouched -- so use the same
    // mechanism here: it patches the function body, so replacing the vtable cannot detach it.
    if (!g_mhOMSetRTInstalled)
    {
        g_mhOMSetRTInstalled = true;
        void* realOMSetRT = vtable[kOMSetRenderTargetsSlot];
        const bool oOk = (MH_CreateHook(realOMSetRT, reinterpret_cast<void*>(&OMSetRenderTargetsHook),
                                        reinterpret_cast<void**>(&g_originalOMSetRenderTargets)) == MH_OK) &&
                         (MH_EnableHook(realOMSetRT) == MH_OK);
        ME2VR::Log::Line("[RTVDURABLE] MinHook real OMSetRenderTargets=" + HexPointer(realOMSetRT) +
                         " (" + std::to_string(oOk) + ")");
        if (oOk) g_omSetRenderTargetsSlot = &vtable[kOMSetRenderTargetsSlot];
    }

    if (g_drawAutoSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawAutoSlot],
                         reinterpret_cast<void*>(&DrawAutoHook),
                         reinterpret_cast<void**>(&g_originalDrawAuto),
                         "ID3D11DeviceContext::DrawAuto"))
    {
        g_drawAutoSlot = &vtable[kDrawAutoSlot];
    }

    if (g_drawIndexedInstancedIndirectSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawIndexedInstancedIndirectSlot],
                         reinterpret_cast<void*>(&DrawIndexedInstancedIndirectHook),
                         reinterpret_cast<void**>(&g_originalDrawIndexedInstancedIndirect),
                         "ID3D11DeviceContext::DrawIndexedInstancedIndirect"))
    {
        g_drawIndexedInstancedIndirectSlot = &vtable[kDrawIndexedInstancedIndirectSlot];
    }

    if (g_drawInstancedIndirectSlot == nullptr &&
        PatchPointerSlot(&vtable[kDrawInstancedIndirectSlot],
                         reinterpret_cast<void*>(&DrawInstancedIndirectHook),
                         reinterpret_cast<void**>(&g_originalDrawInstancedIndirect),
                         "ID3D11DeviceContext::DrawInstancedIndirect"))
    {
        g_drawInstancedIndirectSlot = &vtable[kDrawInstancedIndirectSlot];
    }

    if (g_dispatchSlot == nullptr &&
        PatchPointerSlot(&vtable[kDispatchSlot],
                         reinterpret_cast<void*>(&DispatchHook),
                         reinterpret_cast<void**>(&g_originalDispatch),
                         "ID3D11DeviceContext::Dispatch"))
    {
        g_dispatchSlot = &vtable[kDispatchSlot];
    }

    if (g_dispatchIndirectSlot == nullptr &&
        PatchPointerSlot(&vtable[kDispatchIndirectSlot],
                         reinterpret_cast<void*>(&DispatchIndirectHook),
                         reinterpret_cast<void**>(&g_originalDispatchIndirect),
                         "ID3D11DeviceContext::DispatchIndirect"))
    {
        g_dispatchIndirectSlot = &vtable[kDispatchIndirectSlot];
    }

    if (g_rsSetViewportsSlot == nullptr &&
        PatchPointerSlot(&vtable[kRSSetViewportsSlot],
                         reinterpret_cast<void*>(&RSSetViewportsHook),
                         reinterpret_cast<void**>(&g_originalRSSetViewports),
                         "ID3D11DeviceContext::RSSetViewports"))
    {
        g_rsSetViewportsSlot = &vtable[kRSSetViewportsSlot];
    }

    if (g_rsSetScissorRectsSlot == nullptr &&
        PatchPointerSlot(&vtable[kRSSetScissorRectsSlot],
                         reinterpret_cast<void*>(&RSSetScissorRectsHook),
                         reinterpret_cast<void**>(&g_originalRSSetScissorRects),
                         "ID3D11DeviceContext::RSSetScissorRects"))
    {
        g_rsSetScissorRectsSlot = &vtable[kRSSetScissorRectsSlot];
    }

    if (g_copySubresourceRegionSlot == nullptr &&
        PatchPointerSlot(&vtable[kCopySubresourceRegionSlot],
                         reinterpret_cast<void*>(&CopySubresourceRegionHook),
                         reinterpret_cast<void**>(&g_originalCopySubresourceRegion),
                         "ID3D11DeviceContext::CopySubresourceRegion"))
    {
        g_copySubresourceRegionSlot = &vtable[kCopySubresourceRegionSlot];
    }

    if (g_copyResourceSlot == nullptr &&
        PatchPointerSlot(&vtable[kCopyResourceSlot],
                         reinterpret_cast<void*>(&CopyResourceHook),
                         reinterpret_cast<void**>(&g_originalCopyResource),
                         "ID3D11DeviceContext::CopyResource"))
    {
        g_copyResourceSlot = &vtable[kCopyResourceSlot];
    }

    if (g_resolveSubresourceSlot == nullptr &&
        PatchPointerSlot(&vtable[kResolveSubresourceSlot],
                         reinterpret_cast<void*>(&ResolveSubresourceHook),
                         reinterpret_cast<void**>(&g_originalResolveSubresource),
                         "ID3D11DeviceContext::ResolveSubresource"))
    {
        g_resolveSubresourceSlot = &vtable[kResolveSubresourceSlot];
    }

    if (g_executeCommandListSlot == nullptr &&
        PatchPointerSlot(&vtable[kExecuteCommandListSlot],
                         reinterpret_cast<void*>(&ExecuteCommandListHook),
                         reinterpret_cast<void**>(&g_originalExecuteCommandList),
                         "ID3D11DeviceContext::ExecuteCommandList"))
    {
        g_executeCommandListSlot = &vtable[kExecuteCommandListSlot];
    }
    // (DIBR ClearDepthStencilView is MinHooked in the MinHook block above, not vtable-patched - the vtable
    // patch never fired for this game's deferred depth-clear path.)
}

HRESULT STDMETHODCALLTYPE CreateDeferredContextHook(ID3D11Device* device, UINT flags, ID3D11DeviceContext** ppCtx) noexcept
{
    HRESULT hr = g_originalCreateDeferredContext ? g_originalCreateDeferredContext(device, flags, ppCtx) : E_FAIL;
    if (SUCCEEDED(hr) && ppCtx != nullptr && *ppCtx != nullptr && !g_deferredLogged.exchange(true))
    {
        void** vt = *reinterpret_cast<void***>(*ppCtx);
        ME2VR::Log::Line(std::string("[ME2DISC] *** CreateDeferredContext CALLED ctx=") + HexPointer(*ppCtx) +
                         " vtable=" + HexPointer(vt) + " immediateVtable=" + HexPointer(g_immediateVtable) +
                         " sameVtable=" + std::to_string(vt == g_immediateVtable ? 1 : 0) + " ***");
    }
    return hr;
}

void CaptureSwapChainInfoOnce(IDXGISwapChain* swapChain) noexcept
{
    bool expected = false;
    if (!g_captured.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    if (swapChain == nullptr) return;

    if (g_gameSwapChain == nullptr) { g_gameSwapChain = swapChain; swapChain->AddRef(); }

    ME2VR::Log::Line("[ME2DISC] game swapchain " + HexPointer(swapChain));

    DXGI_SWAP_CHAIN_DESC scd = {};
    HRESULT hr = swapChain->GetDesc(&scd);
    if (SUCCEEDED(hr))
    {
        ME2VR::Log::Line("[ME2DISC] swapchain desc buffer=" + std::to_string(scd.BufferDesc.Width) + "x" +
                         std::to_string(scd.BufferDesc.Height) +
                         " fmt=" + std::to_string(static_cast<int>(scd.BufferDesc.Format)) +
                         " windowed=" + std::to_string(scd.Windowed ? 1 : 0));
    }

    ID3D11Texture2D* backBuffer = nullptr;
    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || backBuffer == nullptr)
    {
        ME2VR::Log::Line("[ME2DISC] GetBuffer(0) failed: " + HexHRESULT(hr));
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    g_backbufferResourcePtr = reinterpret_cast<std::uintptr_t>(backBuffer);
    g_backbufferWidth = desc.Width;
    g_backbufferHeight = desc.Height;
    // DIBR: cache the immediate context (for the warp pass) + push the LIVE render res = full backbuffer
    // (NOT g_swapW, which is the SBS half). The scene-depth capture gate matches THIS. (TODO-3: verify.)
    if (g_gameContext == nullptr && g_gameDevice != nullptr) g_gameDevice->GetImmediateContext(&g_gameContext);
    g_sceneRenderW.store(desc.Width, std::memory_order_release);
    g_sceneRenderH.store(desc.Height, std::memory_order_release);
    ME2VR::Log::Line("[ME2DISC] backbuffer " + std::to_string(desc.Width) + "x" +
                     std::to_string(desc.Height) +
                     " fmt=" + std::to_string(static_cast<int>(desc.Format)) + " " + FormatName(desc.Format) +
                     " bind=0x" + std::to_string(desc.BindFlags));
    SafeRelease(backBuffer);

    ID3D11Device* device = nullptr;
    hr = swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
    if (SUCCEEDED(hr) && device != nullptr)
    {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        ME2VR::Log::Line("[ME2DISC] ID3D11Device " + HexPointer(device) +
                         " immediateContext=" + HexPointer(context));
        InstallContextHooks(context);
        ME2VR::Menu::Init(device, context, 900, 720);   // in-headset Insert menu
        if (g_gameDevice == nullptr) { g_gameDevice = device; device->AddRef(); }
        // Hook the device's CreateDeferredContext (vtable slot 27) to detect deferred-context rendering
        // - the likely home of the DrawIndexed calls (incl. the UI) that never hit the mod's immediate-context hooks.
        if (g_createDeferredContextSlot == nullptr)
        {
            void** dvt = *reinterpret_cast<void***>(device);
            if (dvt != nullptr &&
                PatchPointerSlot(&dvt[27], reinterpret_cast<void*>(&CreateDeferredContextHook),
                                 reinterpret_cast<void**>(&g_originalCreateDeferredContext),
                                 "ID3D11Device::CreateDeferredContext"))
            {
                g_createDeferredContextSlot = &dvt[27];
            }
        }
        SafeRelease(context);
        SafeRelease(device);
    }
}

void InstallPresentHook(IDXGISwapChain* swapChain) noexcept
{
    TryInstallBinkHooks();
    if (swapChain == nullptr || g_presentSlot != nullptr) return;
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (vtable == nullptr) return;
    if (PatchPointerSlot(&vtable[kPresentSlot],
                         reinterpret_cast<void*>(&PresentHook),
                         reinterpret_cast<void**>(&g_originalPresent),
                         "IDXGISwapChain::Present"))
    {
        g_presentSlot = &vtable[kPresentSlot];
    }
}

void OnSwapChainCreated(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr) return;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    CaptureSwapChainInfoOnce(swapChain);
    InstallPresentHook(swapChain);
}

// [VTFIX] Meta's Link runtime rewrites the game context's vtable when the XR session comes up
// (VD/SteamVR leave it alone), silently un-hooking the vtable-patched RTV tracker - [UIGATE] showed
// fmt=0/rtvW=0 on 100% of draws on Oculus only, which killed the whole UI treatment there (stretched
// HUD/subtitles, dead HUD controls). Re-assert the critical slot against the context's LIVE vtable
// every present: a pointer compare that no-ops on runtimes that never stomp (VD/SteamVR bit-identical),
// and re-patches through whatever the runtime installed (chaining into it) when they do.
void ReassertContextVtableHooks(ID3D11DeviceContext* ctx) noexcept
{
    if (ctx == nullptr) return;
    void** vt = *reinterpret_cast<void***>(ctx);
    if (vt == nullptr) return;
    void* const hook = reinterpret_cast<void*>(&OMSetRenderTargetsHook);
    // [RTVDURABLE] The slot is now backed by a MinHook inline hook, which survives vtable
    // replacement by itself. Skip the slot rewrite so it cannot clobber the MinHook trampoline.
    if (g_mhOMSetRTInstalled) return;
    if (vt[kOMSetRenderTargetsSlot] == hook) return;
    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[kOMSetRenderTargetsSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) return;
    void* prev = vt[kOMSetRenderTargetsSlot];
    g_originalOMSetRenderTargets = reinterpret_cast<OMSetRenderTargetsFn>(prev);   // chain into the stomper
    vt[kOMSetRenderTargetsSlot] = hook;
    VirtualProtect(&vt[kOMSetRenderTargetsSlot], sizeof(void*), oldProt, &oldProt);
    static std::atomic<int> s_vtfixLogs{0};
    if (s_vtfixLogs.fetch_add(1, std::memory_order_relaxed) < 8)
        ME2VR::Log::Line("[VTFIX] reasserted ID3D11DeviceContext::OMSetRenderTargets (slot held " +
                         HexPointer(prev) + ")");
}

// [MIRRORTHROTTLE] present the flat mirror 1 frame in N while VR is on (see the block in PresentHook).
// 8 => mirror refreshes ~8-15fps, enough to see the game is alive on the monitor, rare enough that the
// compositor never gates the render thread.
// [MIRRORREC 2026-08-22] Was a hard constant, so a screen recording of ME2 was stuck at the
// mirror's ~8-15fps with no way out. Now runtime-settable (ini MirrorPresentEvery + the Display
// tab): 8 = default, 2 = smooth enough to record, 1 = every frame. Raising the rate re-exposes the
// back-pressure this throttle exists to avoid, so it is opt-in and the menu says what it costs.
constexpr uint64_t kMirrorPresentEveryDefault = 8;
std::atomic<uint64_t> g_mirrorPresentEvery{kMirrorPresentEveryDefault};

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) noexcept
{
    if (g_gameContext == nullptr && g_gameDevice != nullptr) g_gameDevice->GetImmediateContext(&g_gameContext);
    ReassertContextVtableHooks(g_gameContext);
    const auto n = g_presentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    TryInstallBinkHooks();
    if (n == 1)
    {
        ME2VR::Log::Line("[ME2DISC] first Present seen. thread=" + std::to_string(GetCurrentThreadId()));
    }
    else if (n == 600)
    {
        ME2VR::Log::Line("[ME2DISC] 600 Presents seen; hook stable.");
    }

    // [CONVOFP] first-person conversations: refresh the staged-Shepard eye pose, then bump the frame
    // token that keeps the per-view relocation idempotent. Both are cheap no-ops when the feature is
    // off or the mod is not in a conversation, and Tick() never writes to a game object.
    ME2VR::ConvoFp::Tick();
    ME2VR::CalcViewHook::ConvoFpBeginFrame();

    // F2 toggles flat/mono mode (for full-screen menus). Manual = reliable (auto-detect can't separate
    // menus from conversations on ME2).
    {
        static bool s_f2Down = false;
        const bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (f2 && !s_f2Down)
        {
            const bool nm = !g_menuMode.load(std::memory_order_acquire);
            g_menuMode.store(nm, std::memory_order_release);
            ME2VR::Log::Line(std::string("[ME2DISC] flat/mono mode ") + (nm ? "ON (F2)" : "OFF (F2)"));
        }
        s_f2Down = f2;
    }

    // F4 toggles the MASTER VR switch (OFF = vanilla flat game, for first-person dev). Default OFF.
    {
        static bool s_f4Down = false;
        const bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (f4 && !s_f4Down)
        {
            const bool on = !ME2VR::CalcViewHook::GetVrEnabled();
            ME2VR::CalcViewHook::SetVrEnabled(on);
            ME2VR::Log::Line(std::string("[ME2DISC] VR ") + (on ? "ENABLED (F4)" : "DISABLED (F4) - flat"));
        }
        s_f4Down = f4;
    }

    // Rebindable FIRST-PERSON toggle (camera to the head). Works flat or in VR. Default K, ME1 parity.
    // Key is user-rebindable from the First Person tab; skip the poll while that rebind capture is
    // armed so the keypress used to CHOOSE the new key can't also fire the OLD toggle in the same frame.
    if (!ME2VR::Menu::IsRebindingFpToggle())
    {
        static bool s_fpKeyDown = false;
        const int fpVk = ME2VR::Menu::GetFpToggleKey();
        const bool fpKey = (GetAsyncKeyState(fpVk) & 0x8000) != 0;
        if (fpKey && !s_fpKeyDown)
        {
            const bool on = !ME2VR::EngineProbe::GetFirstPerson();
            ME2VR::EngineProbe::SetFirstPerson(on);
            ME2VR::Log::Line(std::string("[ME2DISC] first-person ") + (on ? "ON" : "OFF"));
        }
        s_fpKeyDown = fpKey;
    }

    // F7 toggles MESH-HIDE alone (own body invisible, weapon kept) - independent of the FP camera, to test it.
    {
        static bool s_f7Down = false;
        const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !s_f7Down)
        {
            const bool on = !ME2VR::EngineProbe::GetMeshHide();
            ME2VR::EngineProbe::SetMeshHide(on);
            ME2VR::Log::Line(std::string("[ME2DISC] mesh-hide ") + (on ? "ON (F7)" : "OFF (F7)"));
        }
        s_f7Down = f7;
    }

    // Rebindable recenter hotkey (default R) - only in gameplay (not while the menu is open / rebinding).
    {
        static bool s_rcDown = false;
        const int rk = ME2VR::Menu::GetRecenterKey();
        const bool rc = !ME2VR::Menu::IsOpen() && rk > 0 && (GetAsyncKeyState(rk) & 0x8000) != 0;
        if (rc && !s_rcDown) { ME2VR::Me2Xr::Recenter(); ME2VR::Log::Line("[ME2DISC] recenter (hotkey)"); }
        s_rcDown = rc;
    }

    // F8 dumps the weapon/inventory chain (to locate the held weapon for hide-weapon).
    {
        static bool s_f8Down = false;
        const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8 && !s_f8Down) ME2VR::EngineProbe::DumpWeaponChainNow();
        s_f8Down = f8;
    }

    // [HUDDISC] ask ME2's HUD movie which element names it actually has. Pure read; the per-element HUD
    // controls get built from whatever this logs (ME1's names came from ME1's movie and don't carry).
    // Bound to F11: F9 is the game's QUICKLOAD and F5 its quicksave - never bind a probe to either.
    {
        static bool s_hudDiscDown = false;
        const bool k = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (k && !s_hudDiscDown) ME2VR::PcHud::RunDiscovery();
        s_hudDiscDown = k;
    }

    // F3 dumps the camera/object graph (class names): press in gameplay, conversation, and cutscene to
    // find the distinct state signal (expected: a PlayerCamera mode) so the mod can detect them reliably.
    {
        static bool s_f3Down = false;
        const bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !s_f3Down) ME2VR::EngineProbe::DumpGfxState();
        s_f3Down = f3;
    }

    (void)g_menuEma;

    // Read-only engine-globals discovery; self-latches after one successful in-game dump.
    ME2VR::EngineProbe::TryDumpOnce();
    // First-person camera (basic): zero the active camera mode's boom each frame when enabled (F5).
    ME2VR::EngineProbe::ApplyFirstPerson();
    ME2VR::PcHud::Tick();            // per-element HUD: re-stamp only when a group is non-neutral
    // CalcSceneView confirmed at +0x6CEB70: hook it directly (safe, correct sig) and validate offsets.
    ME2VR::CalcViewHook::Tick();
    // Static xref scan for AllocateViewState (read-only, no hooking).
    ME2VR::AllocViewStateFinder::TryFindOnce();
    // Insert menu: poll the toggle (the texture is rendered + submitted as a quad inside Me2Xr::Tick).
    ME2VR::Menu::OnPresent(swapChain);
    // OpenXR frame: stereo submit + (when open) the menu quad.
    ME2VR::Me2Xr::Tick();

    // Flat mode (VR off): the VR path that composites the menu quad is skipped, so draw the Insert
    // menu straight onto the backbuffer here - otherwise it's invisible while tuning FP flat.
    if (!ME2VR::CalcViewHook::GetVrEnabled())
        ME2VR::Menu::RenderToBackbuffer(swapChain);

    // [SFRDIAG] scene-clear snapshots this present (want >=2 in SFR gameplay: pass 0 + pass 1). Throttled.
    if (SfrModeActive())
    {
        static uint64_t s_sfrDiagN = 0;
        if ((s_sfrDiagN++ % 120) == 0)
        {
            char db[192] = {};
            sprintf_s(db, "[SFRDIAG] sceneClears/present=%llu totalCaps=%llu anchorMisses=%llu anchor=%d",
                      static_cast<unsigned long long>(g_sfrClearsThisPresent),
                      static_cast<unsigned long long>(g_sfrPass0Caps.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_sfrAnchorMisses.load(std::memory_order_relaxed)),
                      g_pass0Anchor.load(std::memory_order_relaxed) ? 1 : 0);
            ME2VR::Log::Line(db);
        }
    }
    // [UIGATE] telemetry: per-second summary of the UI-draw classifier exits + the gate terms.
    {
        static uint64_t s_ugN = 0;
        if ((s_ugN++ % 120) == 0)
        {
            char gb[320] = {};
            sprintf_s(gb, "[UIGATE] calls=%llu ui=%llu pre=%llu vp=%llu blend=%llu excl=%llu "
                          "fmt=%u rtvW=%u bbW=%u dup=%d menu=%d cine=%d vron=%d mode=%d",
                      static_cast<unsigned long long>(g_ugCalls.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_ugUiTrue.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_ugPreFail.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_ugVpFail.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_ugBlendFail.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_ugExcl.exchange(0, std::memory_order_relaxed)),
                      g_ugLastFmt.load(std::memory_order_relaxed),
                      g_ugLastRtvW.load(std::memory_order_relaxed),
                      g_backbufferWidth,
                      g_uiDupEnabled.load(std::memory_order_relaxed) ? 1 : 0,
                      g_menuMode.load(std::memory_order_relaxed) ? 1 : 0,
                      ME2VR::CalcViewHook::GetCinematic() ? 1 : 0,
                      ME2VR::CalcViewHook::GetVrEnabled() ? 1 : 0,
                      ME2VR::CalcViewHook::GetVrMode());
            ME2VR::Log::Line(gb);
        }
    }
    // [CLEARMAP] flush the per-present clear map: always on a clear-count CHANGE (the exact moment the
    // frame structure shifts and stereo dies), plus a heartbeat every ~4s for the healthy baseline.
    if (SfrModeActive() && ME2VR::Log::DiagnosticsOn())
    {
        static uint64_t s_cmN = 0;
        const uint64_t cc = g_sceneClearsSeen;
        const bool changed = (cc != g_sfrLastClearCount);
        if ((changed || (s_cmN % 240) == 0) && g_clearMapLen > 0)
        {
            char cb[320] = {};
            sprintf_s(cb, "[CLEARMAP] clears=%llu bbDraws=%llu bbCopies=%llu gm=%d%s%s map=%s",
                      static_cast<unsigned long long>(cc),
                      static_cast<unsigned long long>(g_sfrBbDrawsThisPresent),
                      static_cast<unsigned long long>(g_sfrBbCopiesThisPresent),
                      g_autoGameMode.load(std::memory_order_relaxed),
                      ME2VR::CalcViewHook::GetVrCineActive() ? " vrCine" : "",
                      changed ? " CHANGED" : "", g_clearMap);
            ME2VR::Log::Line(cb);
        }
        ++s_cmN;
        g_sfrLastClearCount = cc;
    }
    g_clearMap[0] = '\0'; g_clearMapLen = 0; g_sfrBbDrawsThisPresent = 0;
    g_sceneClearsSeen = 0;
    g_sfrClearRunBbDraws = ~0ull; g_sfrClearRunLen = 0; g_sfrPassOpens = 0;   // [CLEARGROUP] per-present
    g_sfrBbCopiesThisPresent = 0;                                                      // [CINECOPY]

    // [PASS0ANCHOR] refresh the backbuffer identity (cheap, once per present) and re-arm the anchor.
    // A present that never captured means pass 0's composite was never seen - the left eye then keeps
    // its previous good frame (1 frame stale) instead of silently being handed pass 1's image.
    if (SfrModeActive())
    {
        if (!g_sfrCapturedThisPresent && g_pass0Anchor.load(std::memory_order_relaxed))
            g_sfrAnchorMisses.fetch_add(1, std::memory_order_relaxed);
        ID3D11Texture2D* bbTex = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bbTex))) && bbTex != nullptr)
        {
            g_bbResourcePtr = reinterpret_cast<std::uintptr_t>(static_cast<ID3D11Resource*>(bbTex));
            bbTex->Release();
        }
    }
    g_sfrBbBoundThisPresent = false;
    g_sfrCapturedThisPresent = false;
    g_sfrClearsThisPresent = 0;   // reset the per-present snapshot counter (g_sfrPass0Tex keeps its last value)

    PresentFn original = g_originalPresent;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    // The flat window is only a mirror while VR is on: the headset is paced by xrWaitFrame and the
    // mod's own pacer, so the game's vsync request is dropped here and the real pacers own the
    // cadence. VR off (F4 flat dev) keeps the game's own sync untouched.
    const bool vrOn = ME2VR::CalcViewHook::GetVrEnabled();
    const UINT effSync = vrOn ? 0u : syncInterval;

    // [MIRRORTHROTTLE] THE 60fps CAP (root-caused 2026-07-31 by comparing focused and unfocused
    // frame windows): focused ran a metronomic 59.6-60.2fps, exactly half the 119.88Hz display,
    // while unfocused ran a variable 76-113fps on identical engine work. Locked versus variable is
    // the tell. While the borderless window is composited by DWM, presentation back-pressure
    // quantizes the frame to two vblanks. It does NOT block inside Present (measured 0.14ms); the
    // runtime stalls the render thread later, when it needs a backbuffer DWM has not released. That
    // is why timing Present, xrWaitFrame, xrEndFrame, the vblank-wait APIs and every thread sleep
    // all came back clean. It also explains the same 16.7ms at both 6144x3456 and 4096x2304: both
    // sit between 60 and 120fps, so both quantize to 59.94, which made it look resolution-proof.
    // In VR nobody looks at the flat window, and the headset image is already submitted by
    // Me2Xr::Tick() above, before this call. So present it rarely: the queue never fills, the stall
    // never happens, and the engine free-runs at its real rate (measured 119.7fps after this
    // change). The mirror updating at ~8-15fps is the intended trade. VR off is untouched.
    static uint64_t s_mirrorN = 0;
    const uint64_t mirrorEvery = g_mirrorPresentEvery.load(std::memory_order_relaxed);
    const bool skipMirror = vrOn && (mirrorEvery > 1) && ((++s_mirrorN % mirrorEvery) != 0);
    if (skipMirror) return S_OK;
    return original(swapChain, effSync, flags);
}

void STDMETHODCALLTYPE RSSetViewportsHook(ID3D11DeviceContext* context,
                                          UINT numViewports,
                                          const D3D11_VIEWPORT* viewports) noexcept
{
    const auto n = g_viewportSetCount.fetch_add(1, std::memory_order_relaxed) + 1;
    bool viewportChanged = false;
    if (numViewports > 0 && viewports != nullptr)
    {
        const D3D11_VIEWPORT& vp = viewports[0];
        g_currentViewport = vp;
        g_haveCurrentViewport = true;
        viewportChanged =
            !g_haveLastLoggedViewport ||
            std::fabs(vp.TopLeftX - g_lastLoggedViewport.TopLeftX) > 0.5f ||
            std::fabs(vp.TopLeftY - g_lastLoggedViewport.TopLeftY) > 0.5f ||
            std::fabs(vp.Width - g_lastLoggedViewport.Width) > 0.5f ||
            std::fabs(vp.Height - g_lastLoggedViewport.Height) > 0.5f ||
            std::fabs(vp.MinDepth - g_lastLoggedViewport.MinDepth) > 0.01f ||
            std::fabs(vp.MaxDepth - g_lastLoggedViewport.MaxDepth) > 0.01f;
    }

    const bool shouldLog = n <= 80 || numViewports != 1 || viewportChanged;
    if (shouldLog && g_viewportLogCount.load(std::memory_order_relaxed) < 512)
    {
        const auto slot = g_viewportLogCount.fetch_add(1, std::memory_order_relaxed);
        if (slot < 512)
        {
            std::string line = "[ME2DISC] RSSetViewports call=" + std::to_string(n) +
                               " count=" + std::to_string(numViewports) +
                               " caller=" + CallerTag(_ReturnAddress());
            const UINT maxLog = (std::min)(numViewports, 4u);
            for (UINT i = 0; i < maxLog && viewports != nullptr; ++i)
            {
                char buffer[256] = {};
                sprintf_s(buffer,
                          " vp%u={x=%.1f y=%.1f w=%.1f h=%.1f minZ=%.2f maxZ=%.2f}",
                          i,
                          viewports[i].TopLeftX,
                          viewports[i].TopLeftY,
                          viewports[i].Width,
                          viewports[i].Height,
                          viewports[i].MinDepth,
                          viewports[i].MaxDepth);
                line += buffer;
            }
            ME2VR::Log::Line(line);

            if (IsInterestingViewport(numViewports, viewports) &&
                g_stackLogCount.fetch_add(1, std::memory_order_relaxed) < 96)
            {
                ME2VR::Log::Line("[ME2DISC] STACK RSSetViewports call=" + std::to_string(n) +
                                 " " + StackTag());
            }

            if (numViewports > 0 && viewports != nullptr)
            {
                g_lastLoggedViewport = viewports[0];
                g_haveLastLoggedViewport = true;
            }
        }
    }

    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[256] = {};
        if (numViewports > 0 && viewports != nullptr)
        {
            sprintf_s(detail,
                      "count=%u vp0=%.1f,%.1f %.1fx%.1f z=%.2f..%.2f",
                      numViewports,
                      viewports[0].TopLeftX,
                      viewports[0].TopLeftY,
                      viewports[0].Width,
                      viewports[0].Height,
                      viewports[0].MinDepth,
                      viewports[0].MaxDepth);
        }
        else
        {
            sprintf_s(detail, "count=%u", numViewports);
        }
        LogStateTrace("RSSetViewports", _ReturnAddress(), detail);
    }

    RSSetViewportsFn original = g_originalRSSetViewports;
    if (original != nullptr) original(context, numViewports, viewports);
}

void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* context,
                                                UINT startSlot,
                                                UINT numViews,
                                                ID3D11ShaderResourceView* const* shaderResourceViews) noexcept
{
    const auto n = g_psSetShaderResourcesCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (numViews > 0 && startSlot < _countof(g_boundPsTrackedResources))
    {
        const UINT trackedSlotLimit = (std::min)(numViews, static_cast<UINT>(_countof(g_boundPsTrackedResources) - startSlot));
        for (UINT i = 0; i < trackedSlotLimit; ++i)
        {
            g_boundPsTrackedResources[startSlot + i] = {};
        }
    }

    if (g_trackedStereoResourceCount > 0 && shaderResourceViews != nullptr && numViews > 0)
    {
        const UINT maxViews = (std::min)(numViews, static_cast<UINT>(_countof(g_boundPsTrackedResources) - (std::min)(startSlot, static_cast<UINT>(_countof(g_boundPsTrackedResources)))));
        for (UINT i = 0; i < maxViews; ++i)
        {
            ID3D11Resource* resource = nullptr;
            if (shaderResourceViews[i] != nullptr)
            {
                shaderResourceViews[i]->GetResource(&resource);
            }

            const auto resourcePtr = reinterpret_cast<std::uintptr_t>(resource);
            const TrackedResource* tracked = FindTrackedStereoResource(resourcePtr);
            if (tracked != nullptr)
            {
                g_boundPsTrackedResources[startSlot + i] = *tracked;

                const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
                const bool currentIsFullSizeTarget =
                    g_backbufferWidth != 0 &&
                    g_backbufferHeight != 0 &&
                    g_currentRtvWidth == g_backbufferWidth &&
                    g_currentRtvHeight == g_backbufferHeight;

                if (currentIsBackbuffer || currentIsFullSizeTarget)
                {
                    g_pendingStereoSlot = startSlot + i;
                    g_pendingStereoResource = *tracked;
                    g_pendingStereoCompositeDraws.store(32, std::memory_order_release);
                    g_traceAfterFullSizeStereoBind.store(48, std::memory_order_release);
                }

                const auto slot = g_psSetShaderResourcesLogCount.fetch_add(1, std::memory_order_relaxed);
                if (slot < 256)
                {
                    char buffer[512] = {};
                    sprintf_s(buffer,
                              "[ME2DISC] PSSetShaderResources uses stereoTex call=%llu start=%u slot=%u tex=%p %ux%u fmt=%u caller=%s currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d",
                              static_cast<unsigned long long>(n),
                              startSlot,
                              startSlot + i,
                              reinterpret_cast<void*>(resourcePtr),
                              tracked->width,
                              tracked->height,
                              static_cast<unsigned int>(tracked->format),
                              CallerTag(_ReturnAddress()).c_str(),
                              reinterpret_cast<void*>(g_currentRtvResourcePtr),
                              g_currentRtvWidth,
                              g_currentRtvHeight,
                              static_cast<unsigned int>(g_currentRtvFormat),
                              currentIsBackbuffer ? 1 : 0,
                              currentIsFullSizeTarget ? 1 : 0);
                    ME2VR::Log::Line(buffer);
                }
            }

            SafeRelease(resource);
        }
    }

    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[192] = {};
        sprintf_s(detail, "start=%u num=%u", startSlot, numViews);
        LogStateTrace("PSSetShaderResources", _ReturnAddress(), detail);
    }

    PSSetShaderResourcesFn original = g_originalPSSetShaderResources;
    if (original != nullptr) original(context, startSlot, numViews, shaderResourceViews);
}

void LogStereoBoundDraw(const char* drawKind,
                        void* caller,
                        UINT count,
                        UINT start,
                        INT baseVertex,
                        bool hasBaseVertex) noexcept
{
    const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
    const bool currentIsFullSizeTarget =
        g_backbufferWidth != 0 &&
        g_backbufferHeight != 0 &&
        g_currentRtvWidth == g_backbufferWidth &&
        g_currentRtvHeight == g_backbufferHeight;
    if (!currentIsBackbuffer && !currentIsFullSizeTarget) return;

    UINT boundSlot = 0;
    const TrackedResource* bound = FindBoundPsStereoResource(&boundSlot);
    if (bound == nullptr) return;

    const auto slot = g_stereoBoundDrawLogCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= 256) return;

    char buffer[768] = {};
    if (hasBaseVertex)
    {
        sprintf_s(buffer,
                  "[ME2DISC] STEREO_BOUND_DRAW %s caller=%s count=%u start=%u base=%d boundSlot=%u stereoTex=%p %ux%u fmt=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f}",
                  drawKind,
                  CallerTag(caller).c_str(),
                  count,
                  start,
                  baseVertex,
                  boundSlot,
                  reinterpret_cast<void*>(bound->ptr),
                  bound->width,
                  bound->height,
                  static_cast<unsigned int>(bound->format),
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Height : -1.0f);
    }
    else
    {
        sprintf_s(buffer,
                  "[ME2DISC] STEREO_BOUND_DRAW %s caller=%s count=%u start=%u boundSlot=%u stereoTex=%p %ux%u fmt=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f}",
                  drawKind,
                  CallerTag(caller).c_str(),
                  count,
                  start,
                  boundSlot,
                  reinterpret_cast<void*>(bound->ptr),
                  bound->width,
                  bound->height,
                  static_cast<unsigned int>(bound->format),
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Height : -1.0f);
    }
    ME2VR::Log::Line(buffer);
}

void LogPendingCompositeDraw(const char* drawKind,
                             void* caller,
                             UINT count,
                             UINT start,
                             INT baseVertex,
                             bool hasBaseVertex) noexcept
{
    unsigned expected = g_pendingStereoCompositeDraws.load(std::memory_order_acquire);
    while (expected != 0 &&
           !g_pendingStereoCompositeDraws.compare_exchange_weak(expected,
                                                                expected - 1,
                                                                std::memory_order_acq_rel,
                                                                std::memory_order_acquire))
    {
    }
    if (expected == 0) return;

    const unsigned remaining = expected - 1;
    const auto slot = g_drawCompositeLogCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= 128) return;

    const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
    const bool currentIsFullSizeTarget =
        g_backbufferWidth != 0 &&
        g_backbufferHeight != 0 &&
        g_currentRtvWidth == g_backbufferWidth &&
        g_currentRtvHeight == g_backbufferHeight;

    char buffer[768] = {};
    if (hasBaseVertex)
    {
        sprintf_s(buffer,
                  "[ME2DISC] COMPOSITE %s caller=%s count=%u start=%u base=%d pendingSlot=%u stereoTex=%p %ux%u fmt=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f} remaining=%u",
                  drawKind,
                  CallerTag(caller).c_str(),
                  count,
                  start,
                  baseVertex,
                  g_pendingStereoSlot,
                  reinterpret_cast<void*>(g_pendingStereoResource.ptr),
                  g_pendingStereoResource.width,
                  g_pendingStereoResource.height,
                  static_cast<unsigned int>(g_pendingStereoResource.format),
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Height : -1.0f,
                  remaining);
    }
    else
    {
        sprintf_s(buffer,
                  "[ME2DISC] COMPOSITE %s caller=%s count=%u start=%u pendingSlot=%u stereoTex=%p %ux%u fmt=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d vp={x=%.1f y=%.1f w=%.1f h=%.1f} remaining=%u",
                  drawKind,
                  CallerTag(caller).c_str(),
                  count,
                  start,
                  g_pendingStereoSlot,
                  reinterpret_cast<void*>(g_pendingStereoResource.ptr),
                  g_pendingStereoResource.width,
                  g_pendingStereoResource.height,
                  static_cast<unsigned int>(g_pendingStereoResource.format),
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftX : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.TopLeftY : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Width : -1.0f,
                  g_haveCurrentViewport ? g_currentViewport.Height : -1.0f,
                  remaining);
    }
    ME2VR::Log::Line(buffer);
}

void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* context,
                                       UINT indexCount,
                                       UINT startIndexLocation,
                                       INT baseVertexLocation) noexcept
{
    if (g_depthMapEnabled.load(std::memory_order_relaxed) && g_boundDepthRes != nullptr)   // DIBR draw-gate
    { uint32_t* dc = DepthDrawCounter(g_boundDepthRes); if (dc != nullptr) (*dc)++; }
    FirstFire("DrawIndexed", 0);
    LogStereoBoundDraw("DrawIndexed", _ReturnAddress(), indexCount, startIndexLocation, baseVertexLocation, true);
    LogUiSeamStack("DrawIndexed", indexCount);
    TraceOp("DrawIndexed", context, indexCount);
    char detail[160] = {};
    sprintf_s(detail, "count=%u start=%u base=%d", indexCount, startIndexLocation, baseVertexLocation);
    LogStateTrace("DrawIndexed", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DrawIndexed", _ReturnAddress(), indexCount, startIndexLocation, baseVertexLocation, true);

    if (CaptureUiCompositeAndSkip(context, indexCount)) return;
    DrawIndexedFn original = g_originalDrawIndexed;
    if (original != nullptr) original(context, indexCount, startIndexLocation, baseVertexLocation);
}

void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* context,
                                UINT vertexCount,
                                UINT startVertexLocation) noexcept
{
    if (g_depthMapEnabled.load(std::memory_order_relaxed) && g_boundDepthRes != nullptr)   // DIBR draw-gate
    { uint32_t* dc = DepthDrawCounter(g_boundDepthRes); if (dc != nullptr) (*dc)++; }
    FirstFire("Draw", 1);
    LogStereoBoundDraw("Draw", _ReturnAddress(), vertexCount, startVertexLocation, 0, false);
    LogUiSeamStack("Draw", vertexCount);
    TraceOp("Draw", context, vertexCount);
    char detail[160] = {};
    sprintf_s(detail, "count=%u start=%u", vertexCount, startVertexLocation);
    LogStateTrace("Draw", _ReturnAddress(), detail);
    LogPendingCompositeDraw("Draw", _ReturnAddress(), vertexCount, startVertexLocation, 0, false);

    if (CaptureUiCompositeAndSkip(context, vertexCount)) return;
    DrawFn original = g_originalDraw;
    if (original != nullptr) original(context, vertexCount, startVertexLocation);
}

void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* context,
                                                UINT indexCountPerInstance,
                                                UINT instanceCount,
                                                UINT startIndexLocation,
                                                INT baseVertexLocation,
                                                UINT startInstanceLocation) noexcept
{
    LogStereoBoundDraw("DrawIndexedInstanced",
                       _ReturnAddress(),
                       indexCountPerInstance,
                       startIndexLocation,
                       baseVertexLocation,
                       true);
    FirstFire("DrawIndexedInstanced", 2);
    LogUiSeamStack("DrawIndexedInstanced", indexCountPerInstance);
    TraceOp("DrawIdxInst", context, indexCountPerInstance);
    char detail[192] = {};
    sprintf_s(detail,
              "idxPerInst=%u inst=%u start=%u base=%d startInst=%u",
              indexCountPerInstance,
              instanceCount,
              startIndexLocation,
              baseVertexLocation,
              startInstanceLocation);
    LogStateTrace("DrawIndexedInstanced", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DrawIndexedInstanced",
                            _ReturnAddress(),
                            indexCountPerInstance,
                            startIndexLocation,
                            baseVertexLocation,
                            true);

    DrawIndexedInstancedFn original = g_originalDrawIndexedInstanced;
    if (original != nullptr)
    {
        original(context,
                 indexCountPerInstance,
                 instanceCount,
                 startIndexLocation,
                 baseVertexLocation,
                 startInstanceLocation);
    }
}

void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* context,
                                         UINT vertexCountPerInstance,
                                         UINT instanceCount,
                                         UINT startVertexLocation,
                                         UINT startInstanceLocation) noexcept
{
    FirstFire("DrawInstanced", 3);
    LogStereoBoundDraw("DrawInstanced", _ReturnAddress(), vertexCountPerInstance, startVertexLocation, 0, false);
    LogUiSeamStack("DrawInstanced", vertexCountPerInstance);
    TraceOp("DrawInst", context, vertexCountPerInstance);
    char detail[192] = {};
    sprintf_s(detail,
              "vtxPerInst=%u inst=%u start=%u startInst=%u",
              vertexCountPerInstance,
              instanceCount,
              startVertexLocation,
              startInstanceLocation);
    LogStateTrace("DrawInstanced", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DrawInstanced", _ReturnAddress(), vertexCountPerInstance, startVertexLocation, 0, false);

    DrawInstancedFn original = g_originalDrawInstanced;
    if (original != nullptr)
    {
        original(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
    }
}

void STDMETHODCALLTYPE DrawAutoHook(ID3D11DeviceContext* context) noexcept
{
    FirstFire("DrawAuto", 4);
    LogStereoBoundDraw("DrawAuto", _ReturnAddress(), 0, 0, 0, false);
    TraceOp("DrawAuto", context, 0);
    LogStateTrace("DrawAuto", _ReturnAddress(), "");
    LogPendingCompositeDraw("DrawAuto", _ReturnAddress(), 0, 0, 0, false);

    DrawAutoFn original = g_originalDrawAuto;
    if (original != nullptr) original(context);
}

void STDMETHODCALLTYPE DrawIndexedInstancedIndirectHook(ID3D11DeviceContext* context,
                                                        ID3D11Buffer* bufferForArgs,
                                                        UINT alignedByteOffsetForArgs) noexcept
{
    LogStereoBoundDraw("DrawIndexedInstancedIndirect",
                       _ReturnAddress(),
                       alignedByteOffsetForArgs,
                       0,
                       0,
                       false);
    FirstFire("DrawIndexedInstancedIndirect", 5);
    TraceOp("DrawIdxIndir", context, 0);
    char detail[160] = {};
    sprintf_s(detail, "args=%p offset=%u", bufferForArgs, alignedByteOffsetForArgs);
    LogStateTrace("DrawIndexedInstancedIndirect", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DrawIndexedInstancedIndirect",
                            _ReturnAddress(),
                            alignedByteOffsetForArgs,
                            0,
                            0,
                            false);

    DrawIndexedInstancedIndirectFn original = g_originalDrawIndexedInstancedIndirect;
    if (original != nullptr) original(context, bufferForArgs, alignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE DrawInstancedIndirectHook(ID3D11DeviceContext* context,
                                                 ID3D11Buffer* bufferForArgs,
                                                 UINT alignedByteOffsetForArgs) noexcept
{
    LogStereoBoundDraw("DrawInstancedIndirect", _ReturnAddress(), alignedByteOffsetForArgs, 0, 0, false);
    FirstFire("DrawInstancedIndirect", 6);
    TraceOp("DrawInstIndir", context, 0);
    char detail[160] = {};
    sprintf_s(detail, "args=%p offset=%u", bufferForArgs, alignedByteOffsetForArgs);
    LogStateTrace("DrawInstancedIndirect", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DrawInstancedIndirect",
                            _ReturnAddress(),
                            alignedByteOffsetForArgs,
                            0,
                            0,
                            false);

    DrawInstancedIndirectFn original = g_originalDrawInstancedIndirect;
    if (original != nullptr) original(context, bufferForArgs, alignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE DispatchHook(ID3D11DeviceContext* context,
                                    UINT threadGroupCountX,
                                    UINT threadGroupCountY,
                                    UINT threadGroupCountZ) noexcept
{
    char detail[160] = {};
    sprintf_s(detail, "groups=%u,%u,%u", threadGroupCountX, threadGroupCountY, threadGroupCountZ);
    LogStateTrace("Dispatch", _ReturnAddress(), detail);
    LogPendingCompositeDraw("Dispatch", _ReturnAddress(), threadGroupCountX, threadGroupCountY, threadGroupCountZ, true);

    DispatchFn original = g_originalDispatch;
    if (original != nullptr) original(context, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

void STDMETHODCALLTYPE DispatchIndirectHook(ID3D11DeviceContext* context,
                                            ID3D11Buffer* bufferForArgs,
                                            UINT alignedByteOffsetForArgs) noexcept
{
    char detail[160] = {};
    sprintf_s(detail, "args=%p offset=%u", bufferForArgs, alignedByteOffsetForArgs);
    LogStateTrace("DispatchIndirect", _ReturnAddress(), detail);
    LogPendingCompositeDraw("DispatchIndirect", _ReturnAddress(), alignedByteOffsetForArgs, 0, 0, false);

    DispatchIndirectFn original = g_originalDispatchIndirect;
    if (original != nullptr) original(context, bufferForArgs, alignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE ExecuteCommandListHook(ID3D11DeviceContext* context,
                                              ID3D11CommandList* commandList,
                                              BOOL restoreContextState) noexcept
{
    const auto n = g_executeCommandListCount.fetch_add(1, std::memory_order_relaxed) + 1;
    FirstFire("ExecuteCommandList", 7);
    TraceOp("ExecCmdList", nullptr, 0xFFFFFFFFu);
    const unsigned pending = g_pendingStereoCompositeDraws.load(std::memory_order_acquire);
    const auto slot = g_executeCommandListLogCount.fetch_add(1, std::memory_order_relaxed);
    if (pending != 0 || slot < 64)
    {
        const bool currentIsBackbuffer = g_currentRtvResourcePtr == g_backbufferResourcePtr;
        const bool currentIsFullSizeTarget =
            g_backbufferWidth != 0 &&
            g_backbufferHeight != 0 &&
            g_currentRtvWidth == g_backbufferWidth &&
            g_currentRtvHeight == g_backbufferHeight;

        char buffer[768] = {};
        sprintf_s(buffer,
                  "[ME2DISC] ExecuteCommandList call=%llu caller=%s commandList=%p restore=%d pendingStereo=%u pendingSlot=%u currentRT=%p %ux%u fmt=%u backbuffer=%d fullSizeRT=%d",
                  static_cast<unsigned long long>(n),
                  CallerTag(_ReturnAddress()).c_str(),
                  commandList,
                  restoreContextState ? 1 : 0,
                  pending,
                  g_pendingStereoSlot,
                  reinterpret_cast<void*>(g_currentRtvResourcePtr),
                  g_currentRtvWidth,
                  g_currentRtvHeight,
                  static_cast<unsigned int>(g_currentRtvFormat),
                  currentIsBackbuffer ? 1 : 0,
                  currentIsFullSizeTarget ? 1 : 0);
        ME2VR::Log::Line(buffer);
    }

    ExecuteCommandListFn original = g_originalExecuteCommandList;
    if (original != nullptr) original(context, commandList, restoreContextState);
}

void LogCopyPath(const char* kind,
                 void* caller,
                 const TrackedResource& dst,
                 const TrackedResource& src,
                 UINT extraA,
                 UINT extraB) noexcept
{
    if (!IsResourceCopyInteresting(dst, src)) return;
    const auto slot = g_copyPathLogCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= 256) return;

    char buffer[768] = {};
    sprintf_s(buffer,
              "[ME2DISC] COPY %s caller=%s dst=%p %ux%u fmt=%u src=%p %ux%u fmt=%u dstBackbuffer=%d srcBackbuffer=%d dstFull=%d srcFull=%d pendingStereo=%u a=%u b=%u",
              kind,
              CallerTag(caller).c_str(),
              reinterpret_cast<void*>(dst.ptr),
              dst.width,
              dst.height,
              static_cast<unsigned int>(dst.format),
              reinterpret_cast<void*>(src.ptr),
              src.width,
              src.height,
              static_cast<unsigned int>(src.format),
              dst.ptr == g_backbufferResourcePtr ? 1 : 0,
              src.ptr == g_backbufferResourcePtr ? 1 : 0,
              IsFullSizeTracked(dst) ? 1 : 0,
              IsFullSizeTracked(src) ? 1 : 0,
              g_pendingStereoCompositeDraws.load(std::memory_order_acquire),
              extraA,
              extraB);
    ME2VR::Log::Line(buffer);
}

void STDMETHODCALLTYPE CopySubresourceRegionHook(ID3D11DeviceContext* context,
                                                 ID3D11Resource* dstResource,
                                                 UINT dstSubresource,
                                                 UINT dstX,
                                                 UINT dstY,
                                                 UINT dstZ,
                                                 ID3D11Resource* srcResource,
                                                 UINT srcSubresource,
                                                 const D3D11_BOX* srcBox) noexcept
{
    TrackedResource dst = {};
    TrackedResource src = {};
    if (TryDescribeTextureResource(dstResource, &dst) && TryDescribeTextureResource(srcResource, &src))
    {
        LogCopyPath("CopySubresourceRegion", _ReturnAddress(), dst, src, dstSubresource, srcSubresource);
        TraceCopy("CopySubRegion", dst, src);
        char detail[320] = {};
        sprintf_s(detail,
                  "dst=%p/%ux%u/f%u src=%p/%ux%u/f%u dstSub=%u srcSub=%u dstXYZ=%u,%u,%u",
                  reinterpret_cast<void*>(dst.ptr),
                  dst.width,
                  dst.height,
                  static_cast<unsigned int>(dst.format),
                  reinterpret_cast<void*>(src.ptr),
                  src.width,
                  src.height,
                  static_cast<unsigned int>(src.format),
                  dstSubresource,
                  srcSubresource,
                  dstX,
                  dstY,
                  dstZ);
        LogStateTrace("CopySubresourceRegion", _ReturnAddress(), detail);
    }

    // [CINECOPY] count backbuffer writes that arrive as a COPY (measurement only, see globals).
    if (g_bbResourcePtr != 0 && reinterpret_cast<std::uintptr_t>(dstResource) == g_bbResourcePtr)
        ++g_sfrBbCopiesThisPresent;

    CopySubresourceRegionFn original = g_originalCopySubresourceRegion;
    if (original != nullptr)
    {
        original(context, dstResource, dstSubresource, dstX, dstY, dstZ, srcResource, srcSubresource, srcBox);
    }
}

void STDMETHODCALLTYPE CopyResourceHook(ID3D11DeviceContext* context,
                                        ID3D11Resource* dstResource,
                                        ID3D11Resource* srcResource) noexcept
{
    TrackedResource dst = {};
    TrackedResource src = {};
    if (TryDescribeTextureResource(dstResource, &dst) && TryDescribeTextureResource(srcResource, &src))
    {
        LogCopyPath("CopyResource", _ReturnAddress(), dst, src, 0, 0);
        TraceCopy("CopyResource", dst, src);
        char detail[256] = {};
        sprintf_s(detail,
                  "dst=%p/%ux%u/f%u src=%p/%ux%u/f%u",
                  reinterpret_cast<void*>(dst.ptr),
                  dst.width,
                  dst.height,
                  static_cast<unsigned int>(dst.format),
                  reinterpret_cast<void*>(src.ptr),
                  src.width,
                  src.height,
                  static_cast<unsigned int>(src.format));
        LogStateTrace("CopyResource", _ReturnAddress(), detail);
    }

    // [CINECOPY] count backbuffer writes that arrive as a COPY (measurement only, see globals).
    if (g_bbResourcePtr != 0 && reinterpret_cast<std::uintptr_t>(dstResource) == g_bbResourcePtr)
        ++g_sfrBbCopiesThisPresent;

    CopyResourceFn original = g_originalCopyResource;
    if (original != nullptr) original(context, dstResource, srcResource);
}

void STDMETHODCALLTYPE ResolveSubresourceHook(ID3D11DeviceContext* context,
                                              ID3D11Resource* dstResource,
                                              UINT dstSubresource,
                                              ID3D11Resource* srcResource,
                                              UINT srcSubresource,
                                              DXGI_FORMAT format) noexcept
{
    TrackedResource dst = {};
    TrackedResource src = {};
    if (TryDescribeTextureResource(dstResource, &dst) && TryDescribeTextureResource(srcResource, &src))
    {
        LogCopyPath("ResolveSubresource", _ReturnAddress(), dst, src, dstSubresource, srcSubresource);
        TraceCopy("Resolve", dst, src);
        char detail[288] = {};
        sprintf_s(detail,
                  "dst=%p/%ux%u/f%u src=%p/%ux%u/f%u dstSub=%u srcSub=%u resolveFmt=%u",
                  reinterpret_cast<void*>(dst.ptr),
                  dst.width,
                  dst.height,
                  static_cast<unsigned int>(dst.format),
                  reinterpret_cast<void*>(src.ptr),
                  src.width,
                  src.height,
                  static_cast<unsigned int>(src.format),
                  dstSubresource,
                  srcSubresource,
                  static_cast<unsigned int>(format));
        LogStateTrace("ResolveSubresource", _ReturnAddress(), detail);
    }

    // [CINECOPY] count backbuffer writes that arrive as a RESOLVE (measurement only, see globals).
    if (g_bbResourcePtr != 0 && reinterpret_cast<std::uintptr_t>(dstResource) == g_bbResourcePtr)
        ++g_sfrBbCopiesThisPresent;

    ResolveSubresourceFn original = g_originalResolveSubresource;
    if (original != nullptr) original(context, dstResource, dstSubresource, srcResource, srcSubresource, format);
}

void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* context,
                                              UINT numViews,
                                              ID3D11RenderTargetView* const* renderTargetViews,
                                              ID3D11DepthStencilView* depthStencilView) noexcept
{
    const auto n = g_omSetRenderTargetsCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // DIBR: resolve the bound depth-stencil RESOURCE so the draw hooks can attribute geometry to it
    // (draw-gated depth capture). Identity only - released immediately, never dereferenced.
    if (depthStencilView != nullptr)
    {
        ID3D11Resource* dres = nullptr;
        depthStencilView->GetResource(&dres);
        g_boundDepthRes = dres;
        if (dres != nullptr) dres->Release();
    }
    else
    {
        g_boundDepthRes = nullptr;
    }

    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    std::uintptr_t resourcePtr = 0;
    bool gotDesc = false;

    if (numViews > 0 && renderTargetViews != nullptr && renderTargetViews[0] != nullptr)
    {
        renderTargetViews[0]->GetResource(&resource);
        resourcePtr = reinterpret_cast<std::uintptr_t>(resource);
        if (resource != nullptr &&
            SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) &&
            texture != nullptr)
        {
            texture->GetDesc(&desc);
            gotDesc = true;
            TrackStereoResource(resourcePtr, desc);
        }
    }

    // [PASS0ANCHOR] pass 0 finishes by compositing into the BACKBUFFER; the next scene depth clear is
    // therefore pass 1's, and at that moment the backbuffer still holds pass 0's image. Same thread as
    // the clear hook, so this is an ordering observation, not a cross-thread flag.
    if (resourcePtr != 0 && resourcePtr == g_bbResourcePtr) g_sfrBbBoundThisPresent = true;

    g_currentRtvResourcePtr = resourcePtr;
    if (gotDesc)
    {
        g_currentRtvWidth = desc.Width;
        g_currentRtvHeight = desc.Height;
        g_currentRtvFormat = desc.Format;
    }
    else
    {
        g_currentRtvWidth = 0;
        g_currentRtvHeight = 0;
        g_currentRtvFormat = DXGI_FORMAT_UNKNOWN;
    }

    TraceOp("OMSetRT", nullptr, 0xFFFFFFFFu);

    const bool changed =
        n <= 80 ||
        numViews != g_lastLoggedRtvCount ||
        resourcePtr != g_lastLoggedRtvResourcePtr ||
        (gotDesc && (desc.Width != g_lastLoggedRtvWidth ||
                     desc.Height != g_lastLoggedRtvHeight ||
                     desc.Format != g_lastLoggedRtvFormat));

    if (changed && g_omSetRenderTargetsLogCount.load(std::memory_order_relaxed) < 768)
    {
        const auto slot = g_omSetRenderTargetsLogCount.fetch_add(1, std::memory_order_relaxed);
        if (slot < 768)
        {
            std::string line = "[ME2DISC] OMSetRenderTargets call=" + std::to_string(n) +
                               " count=" + std::to_string(numViews) +
                               " caller=" + CallerTag(_ReturnAddress());
            if (gotDesc)
            {
                char buffer[384] = {};
                sprintf_s(buffer,
                          " rtv0=%p tex=%p %ux%u fmt=%u %s bind=0x%X backbuffer=%d dsv=%p",
                          (numViews > 0 && renderTargetViews != nullptr) ? renderTargetViews[0] : nullptr,
                          reinterpret_cast<void*>(resourcePtr),
                          desc.Width,
                          desc.Height,
                          static_cast<unsigned int>(desc.Format),
                          FormatName(desc.Format),
                          desc.BindFlags,
                          resourcePtr == g_backbufferResourcePtr ? 1 : 0,
                          depthStencilView);
                line += buffer;
            }
            else
            {
                char buffer[192] = {};
                sprintf_s(buffer,
                          " rtv0=%p tex=%p desc=unavailable dsv=%p",
                          (numViews > 0 && renderTargetViews != nullptr) ? renderTargetViews[0] : nullptr,
                          reinterpret_cast<void*>(resourcePtr),
                          depthStencilView);
                line += buffer;
            }
            ME2VR::Log::Line(line);

            g_lastLoggedRtvCount = numViews;
            g_lastLoggedRtvResourcePtr = resourcePtr;
            if (gotDesc)
            {
                g_lastLoggedRtvWidth = desc.Width;
                g_lastLoggedRtvHeight = desc.Height;
                g_lastLoggedRtvFormat = desc.Format;
            }
        }
    }

    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[256] = {};
        if (gotDesc)
        {
            sprintf_s(detail,
                      "count=%u rtv0=%p tex=%p %ux%u f%u dsv=%p",
                      numViews,
                      (numViews > 0 && renderTargetViews != nullptr) ? renderTargetViews[0] : nullptr,
                      reinterpret_cast<void*>(resourcePtr),
                      desc.Width,
                      desc.Height,
                      static_cast<unsigned int>(desc.Format),
                      depthStencilView);
        }
        else
        {
            sprintf_s(detail,
                      "count=%u rtv0=%p tex=%p desc=none dsv=%p",
                      numViews,
                      (numViews > 0 && renderTargetViews != nullptr) ? renderTargetViews[0] : nullptr,
                      reinterpret_cast<void*>(resourcePtr),
                      depthStencilView);
        }
        LogStateTrace("OMSetRenderTargets", _ReturnAddress(), detail);
    }

    SafeRelease(texture);
    SafeRelease(resource);

    OMSetRenderTargetsFn original = g_originalOMSetRenderTargets;
    if (original != nullptr) original(context, numViews, renderTargetViews, depthStencilView);
}

void STDMETHODCALLTYPE RSSetScissorRectsHook(ID3D11DeviceContext* context,
                                             UINT numRects,
                                             const D3D11_RECT* rects) noexcept
{
    const auto n = g_scissorSetCount.fetch_add(1, std::memory_order_relaxed) + 1;
    bool rectChanged = false;
    if (numRects > 0 && rects != nullptr)
    {
        const D3D11_RECT& rect = rects[0];
        rectChanged =
            !g_haveLastLoggedScissor ||
            rect.left != g_lastLoggedScissor.left ||
            rect.top != g_lastLoggedScissor.top ||
            rect.right != g_lastLoggedScissor.right ||
            rect.bottom != g_lastLoggedScissor.bottom;
    }

    const bool shouldLog = n <= 120 || numRects != 1 || rectChanged;
    if (shouldLog && g_scissorLogCount.load(std::memory_order_relaxed) < 48)
    {
        const auto slot = g_scissorLogCount.fetch_add(1, std::memory_order_relaxed);
        if (slot < 48)
        {
            std::string line = "[ME2DISC] RSSetScissorRects call=" + std::to_string(n) +
                               " count=" + std::to_string(numRects) +
                               " caller=" + CallerTag(_ReturnAddress());
            const UINT maxLog = (std::min)(numRects, 4u);
            for (UINT i = 0; i < maxLog && rects != nullptr; ++i)
            {
                char buffer[192] = {};
                sprintf_s(buffer,
                          " rect%u={l=%ld t=%ld r=%ld b=%ld w=%ld h=%ld}",
                          i,
                          rects[i].left,
                          rects[i].top,
                          rects[i].right,
                          rects[i].bottom,
                          rects[i].right - rects[i].left,
                          rects[i].bottom - rects[i].top);
                line += buffer;
            }
            ME2VR::Log::Line(line);

            if (IsInterestingScissor(numRects, rects) &&
                g_stackLogCount.fetch_add(1, std::memory_order_relaxed) < 96)
            {
                ME2VR::Log::Line("[ME2DISC] STACK RSSetScissorRects call=" + std::to_string(n) +
                                 " " + StackTag());
            }

            if (numRects > 0 && rects != nullptr)
            {
                g_lastLoggedScissor = rects[0];
                g_haveLastLoggedScissor = true;
            }
        }
    }

    if (g_traceAfterFullSizeStereoBind.load(std::memory_order_acquire) != 0)
    {
        char detail[256] = {};
        if (numRects > 0 && rects != nullptr)
        {
            sprintf_s(detail,
                      "count=%u rect0=%ld,%ld %ldx%ld",
                      numRects,
                      rects[0].left,
                      rects[0].top,
                      rects[0].right - rects[0].left,
                      rects[0].bottom - rects[0].top);
        }
        else
        {
            sprintf_s(detail, "count=%u", numRects);
        }
        LogStateTrace("RSSetScissorRects", _ReturnAddress(), detail);
    }

    RSSetScissorRectsFn original = g_originalRSSetScissorRects;
    if (original != nullptr) original(context, numRects, rects);
}

HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* factory,
                                              IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** swapChain) noexcept
{
    CreateSwapChainFn original = g_originalCreateSwapChain;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = original(factory, device, desc, swapChain);
    ME2VR::Log::Line("[ME2DISC] CreateSwapChain returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr) OnSwapChainCreated(*swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IDXGIFactory2* factory,
                                                     IUnknown* device,
                                                     HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                     IDXGIOutput* output,
                                                     IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForHwndFn original = g_originalCreateSwapChainForHwnd;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = original(factory, device, hwnd, desc, fullscreenDesc, output, swapChain);
    ME2VR::Log::Line("[ME2DISC] CreateSwapChainForHwnd returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr) OnSwapChainCreated(*swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindowHook(IDXGIFactory2* factory,
                                                           IUnknown* device,
                                                           IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForCoreWindowFn original = g_originalCreateSwapChainForCoreWindow;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = original(factory, device, window, desc, output, swapChain);
    ME2VR::Log::Line("[ME2DISC] CreateSwapChainForCoreWindow returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr) OnSwapChainCreated(*swapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionHook(IDXGIFactory2* factory,
                                                           IUnknown* device,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForCompositionFn original = g_originalCreateSwapChainForComposition;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = original(factory, device, desc, output, swapChain);
    ME2VR::Log::Line("[ME2DISC] CreateSwapChainForComposition returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr) OnSwapChainCreated(*swapChain);
    return hr;
}

// ============================================================================
// [DISPQ] Render above display resolution - ported from ME1 LE1. LE2 clamps its render resolution to the
// primary monitor rect, read via GetMonitorInfoW, before DXGI ever sees a size - the clamp happens
// INSIDE the game, ahead of CreateSwapChain. Spoofing that one call makes the game render at
// whatever ResX/ResY is in GamerSettings.ini even if it's bigger than the physical monitor, which is
// the fix for "the VR image is soft/pixelated" on systems with sub-4K monitors: the mod only
// captures the backbuffer the game already rendered, so the sharpness ceiling is whatever
// resolution the game chose.
//
// Safety: only arms when the ini target exceeds the real desktop (never shrinks anything, so a
// system whose monitor already covers the target sees zero change), and only rewrites monitor-info
// results that exactly match the real primary rect (a second monitor is left untouched).
// ============================================================================
UINT g_dispqRealPrimaryW = 0;
UINT g_dispqRealPrimaryH = 0;
UINT g_dispqSpoofW = 0;   // 0 = spoof disabled
UINT g_dispqSpoofH = 0;

bool DispqSpoofActive() noexcept { return g_dispqSpoofW != 0 && g_dispqSpoofH != 0; }

// The primary monitor is the one at the desktop origin whose extent matches the real primary size.
bool DispqIsPrimaryRect(const RECT& r) noexcept
{
    return r.left == 0 && r.top == 0 &&
           static_cast<UINT>(r.right - r.left) == g_dispqRealPrimaryW &&
           static_cast<UINT>(r.bottom - r.top) == g_dispqRealPrimaryH;
}

using DispqGetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using DispqGetMonitorInfoAFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using DispqEnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using DispqEnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using DispqGetSystemMetricsFn = int(WINAPI*)(int);
using DispqGetDeviceCapsFn = int(WINAPI*)(HDC, int);

DispqGetMonitorInfoWFn g_origGetMonitorInfoW = nullptr;
DispqGetMonitorInfoAFn g_origGetMonitorInfoA = nullptr;
DispqEnumDisplaySettingsWFn g_origEnumDisplaySettingsW = nullptr;
DispqEnumDisplaySettingsAFn g_origEnumDisplaySettingsA = nullptr;
DispqGetSystemMetricsFn g_origGetSystemMetrics = nullptr;
DispqGetDeviceCapsFn g_origGetDeviceCaps = nullptr;

std::atomic<int> g_dispqMonInfoLogs{0};

// THE mechanism. LE2 asks for the primary monitor's rect; hand back the spoofed size.
BOOL WINAPI DispqGetMonitorInfoWHook(HMONITOR mon, LPMONITORINFO mi) noexcept
{
    const BOOL ok = (g_origGetMonitorInfoW != nullptr) ? g_origGetMonitorInfoW(mon, mi) : FALSE;
    bool rewrote = false;
    if (ok && mi != nullptr && DispqSpoofActive() &&
        (mi->dwFlags & MONITORINFOF_PRIMARY) != 0 && DispqIsPrimaryRect(mi->rcMonitor))
    {
        mi->rcMonitor.right = mi->rcMonitor.left + static_cast<LONG>(g_dispqSpoofW);
        mi->rcMonitor.bottom = mi->rcMonitor.top + static_cast<LONG>(g_dispqSpoofH);
        // rcWork must stay consistent with rcMonitor or window-placement math goes strange.
        mi->rcWork.right = mi->rcWork.left + static_cast<LONG>(g_dispqSpoofW);
        mi->rcWork.bottom = mi->rcWork.top + static_cast<LONG>(g_dispqSpoofH);
        rewrote = true;
    }
    const int n = g_dispqMonInfoLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 && ok && mi != nullptr)
    {
        const RECT& r = mi->rcMonitor;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DISPQ] GetMonitorInfoW -> rcMonitor=%ldx%ld primary=%d spoofed=%d",
                      r.right - r.left, r.bottom - r.top,
                      (mi->dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0, rewrote ? 1 : 0);
        ME2VR::Log::Line(line);
    }
    return ok;
}

// LE2 itself uses the W variant, but an ASI/overlay in another process could take the A path and
// re-introduce the real size. Same rewrite, same primary-only guard.
BOOL WINAPI DispqGetMonitorInfoAHook(HMONITOR mon, LPMONITORINFO mi) noexcept
{
    const BOOL ok = (g_origGetMonitorInfoA != nullptr) ? g_origGetMonitorInfoA(mon, mi) : FALSE;
    if (ok && mi != nullptr && DispqSpoofActive() &&
        (mi->dwFlags & MONITORINFOF_PRIMARY) != 0 && DispqIsPrimaryRect(mi->rcMonitor))
    {
        mi->rcMonitor.right = mi->rcMonitor.left + static_cast<LONG>(g_dispqSpoofW);
        mi->rcMonitor.bottom = mi->rcMonitor.top + static_cast<LONG>(g_dispqSpoofH);
        mi->rcWork.right = mi->rcWork.left + static_cast<LONG>(g_dispqSpoofW);
        mi->rcWork.bottom = mi->rcWork.top + static_cast<LONG>(g_dispqSpoofH);
    }
    return ok;
}

// Defensive/diagnostic only - LE1 proved these are called but ignored for the clamp. Kept so a
// divergence in LE2 shows up as a real backbuffer size instead of silently failing to spoof.
BOOL WINAPI DispqEnumDisplaySettingsWHook(LPCWSTR device, DWORD modeNum, DEVMODEW* dm) noexcept
{
    const BOOL ok = (g_origEnumDisplaySettingsW != nullptr) ? g_origEnumDisplaySettingsW(device, modeNum, dm) : FALSE;
    if (ok && dm != nullptr && DispqSpoofActive() &&
        dm->dmPelsWidth == g_dispqRealPrimaryW && dm->dmPelsHeight == g_dispqRealPrimaryH)
    {
        dm->dmPelsWidth = g_dispqSpoofW;
        dm->dmPelsHeight = g_dispqSpoofH;
    }
    return ok;
}

BOOL WINAPI DispqEnumDisplaySettingsAHook(LPCSTR device, DWORD modeNum, DEVMODEA* dm) noexcept
{
    const BOOL ok = (g_origEnumDisplaySettingsA != nullptr) ? g_origEnumDisplaySettingsA(device, modeNum, dm) : FALSE;
    if (ok && dm != nullptr && DispqSpoofActive() &&
        dm->dmPelsWidth == g_dispqRealPrimaryW && dm->dmPelsHeight == g_dispqRealPrimaryH)
    {
        dm->dmPelsWidth = g_dispqSpoofW;
        dm->dmPelsHeight = g_dispqSpoofH;
    }
    return ok;
}

int WINAPI DispqGetSystemMetricsHook(int index) noexcept
{
    int value = (g_origGetSystemMetrics != nullptr) ? g_origGetSystemMetrics(index) : 0;
    // Only the primary-screen extents. SM_CXVIRTUALSCREEN spans ALL monitors; rewriting it would
    // corrupt multi-monitor coordinate space, so it's left alone deliberately.
    if (DispqSpoofActive())
    {
        if ((index == SM_CXSCREEN || index == SM_CXFULLSCREEN) && value == static_cast<int>(g_dispqRealPrimaryW))
            value = static_cast<int>(g_dispqSpoofW);
        else if ((index == SM_CYSCREEN || index == SM_CYFULLSCREEN) && value == static_cast<int>(g_dispqRealPrimaryH))
            value = static_cast<int>(g_dispqSpoofH);
    }
    return value;
}

int WINAPI DispqGetDeviceCapsHook(HDC hdc, int index) noexcept
{
    // Diagnostic only - LE1 never used this for the clamp. No rewrite, just pass-through.
    return (g_origGetDeviceCaps != nullptr) ? g_origGetDeviceCaps(hdc, index) : 0;
}

// ...\Game\ME2\Binaries\Win64\MassEffect2.exe -> ...\Game\ME2\BioGame\Config\GamerSettings.ini
// Verified on-disk 2026-07-12: same \Binaries\ -> \BioGame\Config\ split as LE1.
std::wstring DispqGameConfigPath() noexcept
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L"";
    std::wstring path(exePath);
    const size_t cut = path.rfind(L"\\Binaries\\");
    if (cut == std::wstring::npos) return L"";
    return path.substr(0, cut) + L"\\BioGame\\Config\\GamerSettings.ini";
}

// Read ResX/ResY out of GamerSettings.ini by hand, straight from disk, at DLL attach - before the
// game (or the mod's own ini loader) has read anything.
bool DispqReadRequestedRes(UINT* outW, UINT* outH) noexcept
{
    const std::wstring path = DispqGameConfigPath();
    if (path.empty()) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return false;

    UINT w = 0, h = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f) != nullptr)
    {
        unsigned v = 0;
        if (sscanf_s(buf, " ResX = %u", &v) == 1 || sscanf_s(buf, " ResX=%u", &v) == 1) w = v;
        else if (sscanf_s(buf, " ResY = %u", &v) == 1 || sscanf_s(buf, " ResY=%u", &v) == 1) h = v;
    }
    fclose(f);

    if (w == 0 || h == 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

// MELE2VR.ini (the mod's config) next to MassEffect2.exe. Read at attach for the VR render-resolution
// override, so the in-menu resolution setting can apply without touching GamerSettings by hand.
std::wstring DispqOurIniPath() noexcept
{
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return L"";
    std::wstring p(exe);
    const size_t slash = p.rfind(L'\\');
    if (slash == std::wstring::npos) return L"";
    return p.substr(0, slash + 1) + L"MELE2VR.ini";
}

// Rewrite ResX/ResY in GamerSettings.ini in place (game renders at whatever these say). Hand-edit each
// matching line so the rest of the ini is byte-preserved; the DISPQ spoof then permits the >monitor size.
// Returns true if it wrote the file. Called ONCE at attach, before the engine reads its config.
bool DispqWriteRes(UINT w, UINT h) noexcept
{
    const std::wstring path = DispqGameConfigPath();
    if (path.empty() || w < 640 || h < 360) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return false;
    std::string text;
    char buf[512];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    // Replace each line that (after leading spaces) begins with ResX / ResY (with or without spaces
    // around '='). Preserve original newline style by scanning line-by-line.
    std::string out;
    out.reserve(text.size() + 32);
    size_t i = 0;
    bool changed = false;
    while (i < text.size())
    {
        size_t eol = text.find('\n', i);
        const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol + 1;
        std::string line = text.substr(i, lineEnd - i);
        size_t s = 0; while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        auto keyIs = [&](const char* key) {
            size_t k = s, j = 0;
            while (key[j] && k < line.size() && line[k] == key[j]) { ++k; ++j; }
            if (key[j] != '\0') return false;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
            return k < line.size() && line[k] == '=';
        };
        if (keyIs("ResX")) { char t[32]; std::snprintf(t, sizeof(t), "ResX=%u\r\n", w); out += t; changed = true; }
        else if (keyIs("ResY")) { char t[32]; std::snprintf(t, sizeof(t), "ResY=%u\r\n", h); out += t; changed = true; }
        else out += line;
        i = lineEnd;
    }
    if (!changed) return false;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

// [DOFOFF] ME1 parity: force DepthOfField=False in GamerSettings.ini. DoF blurs everything outside a
// focal plane, which in a headset just reads as "eyes won't focus" - ME1 ships this as a plain
// toggle. Same byte-preserving hand-edit as the resolution writer; only rewrites if the value differs,
// so the mod doesn't touch the file (or the engine's shader permutations) when it's already correct.
// Applies at NEXT LAUNCH, since the engine reads this config during its own init.
bool DispqWriteBoolKey(const char* key, bool value) noexcept
{
    const std::wstring path = DispqGameConfigPath();
    if (path.empty() || key == nullptr) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return false;
    std::string text;
    char buf[512];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    const char* want = value ? "True" : "False";
    std::string out;
    out.reserve(text.size() + 64);
    size_t i = 0;
    bool changed = false;
    bool found = false;
    size_t sysSectionEnd = std::string::npos;   // insertion point if the key doesn't exist yet
    bool inSysSection = false;
    while (i < text.size())
    {
        size_t eol = text.find('\n', i);
        const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol + 1;
        std::string line = text.substr(i, lineEnd - i);
        size_t s = 0; while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        if (s < line.size() && line[s] == '[')
        {
            if (inSysSection && sysSectionEnd == std::string::npos) sysSectionEnd = out.size();
            inSysSection = (line.compare(s, 16, "[SystemSettings]") == 0);
        }
        size_t k = s, j = 0;
        while (key[j] && k < line.size() && line[k] == key[j]) { ++k; ++j; }
        bool match = (key[j] == '\0');
        if (match)
        {
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
            match = (k < line.size() && line[k] == '=');
        }
        if (match)
        {
            found = true;
            if (line.find(want) == std::string::npos)
            {
                char t[96]; std::snprintf(t, sizeof(t), "%s=%s\r\n", key, want);
                out += t; changed = true;
            }
            else out += line;
        }
        else out += line;
        i = lineEnd;
    }
    // The key may simply not be in the file - LE2 ships GamerSettings.ini WITHOUT DepthOfField, so a
    // replace-only writer would silently do nothing and the toggle would look broken. Append it to
    // [SystemSettings] (end of that section, or end of file if the section is last/absent).
    if (!found)
    {
        char t[96]; std::snprintf(t, sizeof(t), "%s=%s\r\n", key, want);
        if (sysSectionEnd != std::string::npos) out.insert(sysSectionEnd, t);
        else if (inSysSection)                  out += t;      // section ran to EOF
        else                                    out += std::string("[SystemSettings]\r\n") + t;
        changed = true;
    }
    if (!changed) return true;    // key present and already correct -> nothing to do, but it IS correct
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr)
    {
        ME2VR::Log::Line(std::string("[DOFOFF] FAILED to open GamerSettings.ini for write: ") +
                         ME2VR::Log::WideToUtf8(path));
        return false;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);

    // Verify by reading the file back. The old code returned false both when the key was already
    // correct AND when the write failed, and the caller printed "already correct (no write)" for both
    // -- so a write that never landed looked like success. The key is currently ABSENT from that file
    // while the log claims it is set, which is exactly that bug showing.
    bool verified = false;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr)
    {
        std::string back;
        char vb[512];
        size_t vn = 0;
        while ((vn = fread(vb, 1, sizeof(vb), f)) > 0) back.append(vb, vn);
        fclose(f);
        const std::string needle = std::string(key) + "=" + want;
        verified = (back.find(needle) != std::string::npos);
    }
    if (!verified)
        ME2VR::Log::Line("[DOFOFF] wrote GamerSettings.ini but the key did NOT read back - "
                         "the game may be rewriting the file");
    return verified;
}

void DispqInstallHook(void* target, void* hook, void** original, const char* name) noexcept
{
    if (target == nullptr || hook == nullptr || original == nullptr) return;
    const MH_STATUS created = MH_CreateHook(target, hook, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
    {
        ME2VR::Log::Line(std::string("[DISPQ] MH_CreateHook failed for ") + name +
                         " status=" + std::to_string(static_cast<int>(created)));
        return;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
    {
        ME2VR::Log::Line(std::string("[DISPQ] MH_EnableHook failed for ") + name +
                         " status=" + std::to_string(static_cast<int>(enabled)));
        return;
    }
    ME2VR::Log::Line(std::string("[DISPQ] hooked ") + name);
}
}

namespace ME2VR::D3DCapture
{
ID3D11Device* GetGameDevice() noexcept { return g_gameDevice; }
IDXGISwapChain* GetGameSwapChain() noexcept { return g_gameSwapChain; }
// [SFR] fetch the captured pass-0 (left-eye) texture at submit (null before the first capture). The
// capture is driven entirely by the render-thread composite->clear marker (draw + clear hooks); the
// game thread / DrawDetour no longer touches D3D (that raced -> crash).
ID3D11Texture2D* GetSfrPass0Texture() noexcept { return g_sfrPass0Tex; }
// [STALEEYE] capture counter - the submit uses it to tell a FRESH pass-0 snapshot from a leftover one.
unsigned long long GetSfrPass0CaptureSeq() noexcept { return g_sfrPass0Caps.load(std::memory_order_relaxed); }
unsigned GetBackbufferWidth() noexcept { return g_backbufferWidth; }
unsigned GetBackbufferHeight() noexcept { return g_backbufferHeight; }
// [AUTOMENU] Composite: manual flat toggle OR (mono-menus on AND the engine says a full-screen menu/
// map/movie is up). Every existing call site (split gate, SFR replay gate, UI dup/mirror, XR submit)
// reads this, so menus/map auto-drop to mono exactly like ME1.
bool GetMenuMode() noexcept
{
    if (g_menuMode.load(std::memory_order_acquire)) return true;
    if (!g_monoMenus.load(std::memory_order_acquire)) return false;
    const int m = g_autoGameMode.load(std::memory_order_acquire);
    if (m == 8 || m == 9 || m == 10) return true;   // Movie / Galaxy / Orbital - genuinely flat, no depth exists
    if (m != 7) return false;                       // gameplay (and - 1 unreadable) -> fail OPEN to VR
    // [GM7PAUSE] GUI mode: flat only when a blocking menu has actually paused the engine. Without this,
    // the gameplay HUD (objectives/tutorials/aiming) also reports 7 and flattens live combat.
    if (!g_gm7NeedsPause.load(std::memory_order_acquire)) return true;   // toggle off = old always-flat behaviour
    return g_autoPaused.load(std::memory_order_acquire);
}
void SetMenuMode(bool on) noexcept { g_menuMode.store(on, std::memory_order_release); }
bool GetMenuModeManual() noexcept { return g_menuMode.load(std::memory_order_acquire); }
void SetAutoGameMode(int mode) noexcept { g_autoGameMode.store(mode, std::memory_order_release); }
int  GetAutoGameMode() noexcept { return g_autoGameMode.load(std::memory_order_acquire); }
int  GetMirrorPresentEvery() noexcept { return static_cast<int>(g_mirrorPresentEvery.load(std::memory_order_relaxed)); }
void SetMirrorPresentEvery(int n) noexcept
{
    if (n <= 0) n = static_cast<int>(kMirrorPresentEveryDefault);   // 0/unset -> default
    if (n > 32) n = 32;
    g_mirrorPresentEvery.store(static_cast<uint64_t>(n), std::memory_order_relaxed);
}

bool GetMonoMenus() noexcept { return g_monoMenus.load(std::memory_order_acquire); }
void SetMonoMenus(bool on) noexcept { g_monoMenus.store(on, std::memory_order_release); }
// [PASS0ANCHOR] one pass-0 snapshot per present, anchored to the backbuffer composite (see globals).
bool GetPass0Anchor() noexcept { return g_pass0Anchor.load(std::memory_order_relaxed); }
void SetPass0Anchor(bool on) noexcept { g_pass0Anchor.store(on, std::memory_order_relaxed); }
// [GM7PAUSE] pause flag published per frame from me2_xr, and the toggle that decides whether mode 7 needs it.
void SetAutoPaused(bool on) noexcept { g_autoPaused.store(on, std::memory_order_release); }
bool GetAutoPaused() noexcept { return g_autoPaused.load(std::memory_order_acquire); }
bool GetGm7NeedsPause() noexcept { return g_gm7NeedsPause.load(std::memory_order_acquire); }
void SetGm7NeedsPause(bool on) noexcept { g_gm7NeedsPause.store(on, std::memory_order_release); }
bool GetUiDupEnabled() noexcept { return g_uiDupEnabled.load(std::memory_order_acquire); }
void SetUiDupEnabled(bool on) noexcept { g_uiDupEnabled.store(on, std::memory_order_release); }
ID3D11Texture2D* GetUiOverlayTexture() noexcept { return g_uiOverlayTex; }
bool GetUiOverlayActive() noexcept { return g_overlayHasContent.load(std::memory_order_acquire); }
ID3D11Texture2D* GetUiTexture() noexcept { return g_uiSrcTexture; }
unsigned GetUiTexWidth() noexcept { return g_uiTexW; }
unsigned GetUiTexHeight() noexcept { return g_uiTexH; }

// Call from DllMain (DLL_PROCESS_ATTACH), BEFORE the worker thread - file I/O + GetSystemMetrics
// only, safe under the loader lock. The engine reads GamerSettings.ini during its own init, ahead of
// any thread the mod could spawn, so this has to run synchronously and first.
void ArmResolutionSpoof() noexcept
{
    g_dispqRealPrimaryW = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
    g_dispqRealPrimaryH = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));

    // [VRRES] the mod's in-menu render-resolution override (MELE2VR.ini [VR] RenderW/RenderH; 0 = leave
    // GamerSettings alone). When set, write it into GamerSettings.ini so the GAME renders at that size
    // (the actual GPU cost = double-rendered in SFR). Restart-to-apply by nature (engine reads config once).
    {
        const std::wstring ourIni = DispqOurIniPath();
        if (!ourIni.empty())
        {
            const UINT rw = static_cast<UINT>(GetPrivateProfileIntW(L"VR", L"RenderW", 0, ourIni.c_str()));
            const UINT rh = static_cast<UINT>(GetPrivateProfileIntW(L"VR", L"RenderH", 0, ourIni.c_str()));
            if (rw >= 1024 && rh >= 576)
            {
                const bool wrote = DispqWriteRes(rw, rh);
                ME2VR::Log::Line("[VRRES] override " + std::to_string(rw) + "x" + std::to_string(rh) +
                                 (wrote ? " -> wrote GamerSettings.ini" : " -> write FAILED (GamerSettings untouched)"));
            }
        }
    }

    // [DOFOFF] apply the depth-of-field preference for THIS launch (ME1 parity). Same attach-time
    // window as the resolution write - the engine hasn't read its config yet.
    {
        const std::wstring ourIni = DispqOurIniPath();
        if (!ourIni.empty())
        {
            const bool disableDof = GetPrivateProfileIntW(L"VR", L"DisableDof", 1, ourIni.c_str()) != 0;
            const bool ok = DispqWriteBoolKey("DepthOfField", !disableDof);
            ME2VR::Log::Line(std::string("[DOFOFF] [SystemSettings] DepthOfField=") +
                             (disableDof ? "False" : "True") +
                             (ok ? " -> VERIFIED in GamerSettings.ini"
                                 : " -> NOT APPLIED (write failed or the game rewrote the file)"));
        }
    }

    UINT wantW = 0, wantH = 0;
    const bool haveWant = DispqReadRequestedRes(&wantW, &wantH);

    // Arm ONLY when the target exceeds the desktop - at or below native this is a no-op, every
    // display hook stays a pure pass-through, and a system whose monitor already covers the target
    // sees zero change and zero risk.
    if (haveWant && g_dispqRealPrimaryW != 0 && g_dispqRealPrimaryH != 0 &&
        (wantW > g_dispqRealPrimaryW || wantH > g_dispqRealPrimaryH))
    {
        g_dispqSpoofW = wantW;
        g_dispqSpoofH = wantH;
    }

    char line[224];
    std::snprintf(line, sizeof(line),
                  "[DISPQ] realPrimary=%ux%u target=%ux%u(read=%d) spoof=%s -> %ux%u",
                  g_dispqRealPrimaryW, g_dispqRealPrimaryH, wantW, wantH, haveWant ? 1 : 0,
                  DispqSpoofActive() ? "ARMED" : "off", g_dispqSpoofW, g_dispqSpoofH);
    ME2VR::Log::Line(line);
}

// Installed from the worker thread, well ahead of the game's D3D init (which happens after DllMain
// returns) - no race with ArmResolutionSpoof, which already ran synchronously in DllMain.
// ================================================================================================
// [SVRFIX] / [SVRFIX2] SteamVR scene-submit repair. Ported from ME1 (_shipconv_build/d3d_capture.cpp)
// 2026-07-26 -- ME2 had the [LINKFOV] FOV half of the SteamVR story but none of the device half.
//
// Diagnosed on ME1 2026-07-20: SteamVR's in-process client creates a keyed-mutex "sync texture" on the
// session's D3D11 device before it will accept scene frames. If the game's device cannot back a
// keyed-mutex shared texture, every ComposeLayerProjection fails with
// VRCompositorError_SharedTexturesNotSupported while xrEndFrame STILL reports success -- so the
// projection layer is silently dropped and you get a black world with working audio and working quad
// overlays (menus). Two independent causes, both fixed at device creation because neither can be
// changed on a live device:
//   [SVRFIX]  D3D11_CREATE_DEVICE_SINGLETHREADED makes D3D refuse keyed-mutex shared textures
//             (CreateTexture2D KEYEDMUTEX -> E_INVALIDARG while plain SHARED succeeds). Clearing it
//             only restores D3D's own internal locking -- a strict superset of singlethreaded
//             semantics -- and the mod already drives this device from the Present and XR threads, so the
//             flag was a lie the moment the mod injected.
//   [SVRFIX2] An UNKNOWN driverType + explicit adapter device is keyed-mutex-incapable here, while a
//             null-adapter HARDWARE device on the same GPU/process/flags is capable. Force the proven
//             recipe only for that exact failing pattern.
// Both gate on SteamVR being the ACTIVE runtime, so on Meta/Quest Link and VDXR neither fires and
// device creation stays bit-identical to stock.
// ================================================================================================
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                             const D3D_FEATURE_LEVEL*, UINT, UINT,
                                             ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                         const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                         const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                         ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
D3D11CreateDeviceFn g_origD3D11CreateDevice = nullptr;
D3D11CreateDeviceAndSwapChainFn g_origD3D11CreateDeviceAndSwapChain = nullptr;

// [SVRFIX2] Is SteamVR the active OpenXR runtime? Read HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime
// (the JSON path the loader will use) BEFORE any XR instance exists, so device creation can gate on it.
// Cached: the hook fires several times and the registry value does not change mid-run.
bool SvrIsSteamVrActiveRuntime() noexcept
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        wchar_t path[1024] = {};
        DWORD cb = sizeof(path) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type,
                             reinterpret_cast<LPBYTE>(path), &cb) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            for (wchar_t* p = path; *p; ++p) *p = towlower(*p);
            if (wcsstr(path, L"steamvr") != nullptr || wcsstr(path, L"steamxr") != nullptr) cached = 1;
        }
        RegCloseKey(key);
    }
    ME2VR::Log::Line(std::string("[SVRFIX2] active OpenXR runtime is SteamVR: ") + (cached ? "yes" : "no"));
    return cached != 0;
}

UINT SvrStripSingleThreadedFlag(UINT flags, const char* which) noexcept
{
    if ((flags & D3D11_CREATE_DEVICE_SINGLETHREADED) == 0) return flags;
    ME2VR::Log::Line(std::string("[SVRFIX] ") + which + " requested SINGLETHREADED (flags=" +
                     std::to_string(flags) + ") - stripping so SteamVR's keyed-mutex sync texture can be created.");
    return flags & ~static_cast<UINT>(D3D11_CREATE_DEVICE_SINGLETHREADED);
}

void SvrMaybeForceKeyedMutexRecipe(IDXGIAdapter*& adapter, D3D_DRIVER_TYPE& driverType, const char* which) noexcept
{
    if (!SvrIsSteamVrActiveRuntime()) return;
    if (driverType != D3D_DRIVER_TYPE_UNKNOWN || adapter == nullptr) return;
    ME2VR::Log::Line(std::string("[SVRFIX2] ") + which +
                     " used UNKNOWN+explicit-adapter (keyed-mutex-incapable on SteamVR); forcing null-adapter HARDWARE.");
    adapter = nullptr;
    driverType = D3D_DRIVER_TYPE_HARDWARE;
}

HRESULT WINAPI SvrD3D11CreateDeviceHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
                                        UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                        UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel,
                                        ID3D11DeviceContext** context) noexcept
{
    // [SVRFIX] strip only when SteamVR is active: on Meta/VDXR the game's device is left exactly as it
    // asked for it, so this port cannot change behaviour on the runtime that is actually in use.
    if (SvrIsSteamVrActiveRuntime()) flags = SvrStripSingleThreadedFlag(flags, "D3D11CreateDevice");
    SvrMaybeForceKeyedMutexRecipe(adapter, driverType, "D3D11CreateDevice");
    return g_origD3D11CreateDevice != nullptr
               ? g_origD3D11CreateDevice(adapter, driverType, software, flags, levels, levelCount,
                                         sdkVersion, device, outLevel, context)
               : E_FAIL;
}

HRESULT WINAPI SvrD3D11CreateDeviceAndSwapChainHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
                                                    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
                                                    UINT levelCount, UINT sdkVersion,
                                                    const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** swapChain,
                                                    ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel,
                                                    ID3D11DeviceContext** context) noexcept
{
    if (SvrIsSteamVrActiveRuntime()) flags = SvrStripSingleThreadedFlag(flags, "D3D11CreateDeviceAndSwapChain");
    SvrMaybeForceKeyedMutexRecipe(adapter, driverType, "D3D11CreateDeviceAndSwapChain");
    return g_origD3D11CreateDeviceAndSwapChain != nullptr
               ? g_origD3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags, levels, levelCount,
                                                     sdkVersion, scDesc, swapChain, device, outLevel, context)
               : E_FAIL;
}

void InstallDisplayQueryHooks() noexcept
{
    MH_Initialize();   // idempotent if InstallBreakpointProbes or another path already called it

    // [SVRFIX] device-creation hooks FIRST. This runs from the startup worker seconds before the game
    // creates its device (measured on ME2: hooks ~12:32:35, CreateSwapChainForHwnd ~12:32:43), and both
    // the flag and the adapter/driverType recipe must be right AT creation -- neither can be changed on
    // a live device. Deliberately ahead of the user32 early-out below, which is unrelated to this.
    {
        HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (d3d11 == nullptr) d3d11 = LoadLibraryW(L"d3d11.dll");
        if (d3d11 != nullptr)
        {
            void* cd = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"));
            if (cd != nullptr)
                DispqInstallHook(cd, reinterpret_cast<void*>(&SvrD3D11CreateDeviceHook),
                                 reinterpret_cast<void**>(&g_origD3D11CreateDevice), "D3D11CreateDevice");
            void* cds = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
            if (cds != nullptr)
                DispqInstallHook(cds, reinterpret_cast<void*>(&SvrD3D11CreateDeviceAndSwapChainHook),
                                 reinterpret_cast<void**>(&g_origD3D11CreateDeviceAndSwapChain),
                                 "D3D11CreateDeviceAndSwapChain");
        }
        else
        {
            ME2VR::Log::Line("[SVRFIX] d3d11.dll not loadable - SteamVR device fixes unavailable this run.");
        }
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
    {
        ME2VR::Log::Line("[DISPQ] user32.dll not loaded - display query hooks skipped.");
        return;
    }

    void* monInfoW = reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoW"));
    DispqInstallHook(monInfoW, reinterpret_cast<void*>(&DispqGetMonitorInfoWHook),
                     reinterpret_cast<void**>(&g_origGetMonitorInfoW), "GetMonitorInfoW");

    void* monInfoA = reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoA"));
    DispqInstallHook(monInfoA, reinterpret_cast<void*>(&DispqGetMonitorInfoAHook),
                     reinterpret_cast<void**>(&g_origGetMonitorInfoA), "GetMonitorInfoA");

    void* enumW = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsW"));
    DispqInstallHook(enumW, reinterpret_cast<void*>(&DispqEnumDisplaySettingsWHook),
                     reinterpret_cast<void**>(&g_origEnumDisplaySettingsW), "EnumDisplaySettingsW");

    void* enumA = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsA"));
    DispqInstallHook(enumA, reinterpret_cast<void*>(&DispqEnumDisplaySettingsAHook),
                     reinterpret_cast<void**>(&g_origEnumDisplaySettingsA), "EnumDisplaySettingsA");

    void* metrics = reinterpret_cast<void*>(GetProcAddress(user32, "GetSystemMetrics"));
    DispqInstallHook(metrics, reinterpret_cast<void*>(&DispqGetSystemMetricsHook),
                     reinterpret_cast<void**>(&g_origGetSystemMetrics), "GetSystemMetrics");

    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (gdi32 == nullptr) gdi32 = LoadLibraryW(L"gdi32.dll");
    if (gdi32 != nullptr)
    {
        void* caps = reinterpret_cast<void*>(GetProcAddress(gdi32, "GetDeviceCaps"));
        DispqInstallHook(caps, reinterpret_cast<void*>(&DispqGetDeviceCapsHook),
                         reinterpret_cast<void**>(&g_origGetDeviceCaps), "GetDeviceCaps");
    }
}

unsigned long long LastBinkFrameAgeMs() noexcept
{
    const unsigned long long stamp = g_lastBinkFrameMs.load(std::memory_order_relaxed);
    if (stamp == 0) return ~0ull;
    const unsigned long long now = GetTickCount64();
    return now >= stamp ? now - stamp : 0;
}


// --- DIBR public API (called from me2_xr RunFrame; internals live in the anon namespace above) ---
void SetDepthMapEnabled(bool enabled) noexcept { g_depthMapEnabled.store(enabled, std::memory_order_relaxed); }
bool GetDepthMapEnabled() noexcept { return g_depthMapEnabled.load(std::memory_order_relaxed); }
bool IsDibrStereoReady() noexcept
{
    return g_depthMapEnabled.load(std::memory_order_relaxed) &&
           g_depthReady.load(std::memory_order_acquire) && g_depthSrv != nullptr;
}
void SetDibrWarp(float gain, float convergence, bool flip) noexcept
{
    auto clampf = [](float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); };
    g_dibrGain.store(clampf(gain, 0.0f, 10.0f), std::memory_order_relaxed);
    g_dibrConvergence.store(clampf(convergence, 0.90f, 1.005f), std::memory_order_relaxed);
    g_dibrSign.store(flip ? -1.0f : 1.0f, std::memory_order_relaxed);
}
void GetDepthProbe(float* center, float* tl, float* br, float* tr) noexcept
{
    if (center) *center = g_probeCenter.load(std::memory_order_relaxed);
    if (tl)     *tl     = g_probeTL.load(std::memory_order_relaxed);
    if (br)     *br     = g_probeBR.load(std::memory_order_relaxed);
    if (tr)     *tr     = g_probeTR.load(std::memory_order_relaxed);
}
// Synthesized right eye (backbuffer-sized). nullptr if not ready -> caller submits the backbuffer (never black).
ID3D11Texture2D* GetDibrRightEye(ID3D11Texture2D* backBuffer) noexcept
{
    if (g_gameDevice == nullptr || backBuffer == nullptr) return nullptr;
    D3D11_TEXTURE2D_DESC bbd = {}; backBuffer->GetDesc(&bbd);
    if (!EnsureColorCopy(bbd) || !EnsureWarpedTex(bbd)) return nullptr;
    return RenderDibrEye(backBuffer, g_dibrWarpedTex, g_dibrWarpedRtv, 1.0f);
}

void InstallBreakpointProbes() noexcept
{
    ME2VR::Log::Line("[ME2DISC] breakpoint probes disabled for composite draw trace build.");
    return;

    bool expected = false;
    if (!g_breakpointsInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr)
    {
        ME2VR::Log::Line("[ME2DISC] breakpoint probes skipped: exe module missing");
        return;
    }

    g_vectoredHandler = AddVectoredExceptionHandler(1, BreakpointVectoredHandler);
    if (g_vectoredHandler == nullptr)
    {
        ME2VR::Log::WindowsError("[ME2DISC] AddVectoredExceptionHandler", GetLastError());
        return;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(exe);
    unsigned installed = 0;
    for (BreakpointProbe& probe : g_breakpointProbes)
    {
        probe.address = reinterpret_cast<BYTE*>(base + probe.rva);
        probe.originalByte = *probe.address;
        if (probe.originalByte == 0xCC)
        {
            char buffer[192] = {};
            sprintf_s(buffer,
                      "[ME2DISC] breakpoint probe skipped %s rva=0x%llX already int3",
                      probe.name,
                      static_cast<unsigned long long>(probe.rva));
            ME2VR::Log::Line(buffer);
            continue;
        }

        if (WriteByte(probe.address, 0xCC))
        {
            probe.armed = true;
            ++installed;
            char buffer[192] = {};
            sprintf_s(buffer,
                      "[ME2DISC] breakpoint probe armed %s rva=0x%llX original=0x%02X",
                      probe.name,
                      static_cast<unsigned long long>(probe.rva),
                      static_cast<unsigned int>(probe.originalByte));
            ME2VR::Log::Line(buffer);
        }
        else
        {
            char buffer[192] = {};
            sprintf_s(buffer,
                      "[ME2DISC] breakpoint probe failed %s rva=0x%llX",
                      probe.name,
                      static_cast<unsigned long long>(probe.rva));
            ME2VR::Log::Line(buffer);
        }
    }

    ME2VR::Log::Line("[ME2DISC] breakpoint probes installed count=" + std::to_string(installed));
}

void TryInstallFactoryHooks(void* factory, const IID&) noexcept
{
    if (factory == nullptr) return;
    std::lock_guard<std::mutex> lock(g_hookMutex);

    void** vtable = *reinterpret_cast<void***>(factory);
    if (vtable == nullptr) return;

    if (g_createSwapChainSlot == nullptr &&
        PatchPointerSlot(&vtable[kCreateSwapChainSlot],
                         reinterpret_cast<void*>(&CreateSwapChainHook),
                         reinterpret_cast<void**>(&g_originalCreateSwapChain),
                         "IDXGIFactory::CreateSwapChain"))
    {
        g_createSwapChainSlot = &vtable[kCreateSwapChainSlot];
    }

    if (g_createSwapChainForHwndSlot == nullptr &&
        PatchPointerSlot(&vtable[kCreateSwapChainForHwndSlot],
                         reinterpret_cast<void*>(&CreateSwapChainForHwndHook),
                         reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd),
                         "IDXGIFactory2::CreateSwapChainForHwnd"))
    {
        g_createSwapChainForHwndSlot = &vtable[kCreateSwapChainForHwndSlot];
    }

    if (g_createSwapChainForCoreWindowSlot == nullptr &&
        PatchPointerSlot(&vtable[kCreateSwapChainForCoreWindowSlot],
                         reinterpret_cast<void*>(&CreateSwapChainForCoreWindowHook),
                         reinterpret_cast<void**>(&g_originalCreateSwapChainForCoreWindow),
                         "IDXGIFactory2::CreateSwapChainForCoreWindow"))
    {
        g_createSwapChainForCoreWindowSlot = &vtable[kCreateSwapChainForCoreWindowSlot];
    }

    if (g_createSwapChainForCompositionSlot == nullptr &&
        PatchPointerSlot(&vtable[kCreateSwapChainForCompositionSlot],
                         reinterpret_cast<void*>(&CreateSwapChainForCompositionHook),
                         reinterpret_cast<void**>(&g_originalCreateSwapChainForComposition),
                         "IDXGIFactory2::CreateSwapChainForComposition"))
    {
        g_createSwapChainForCompositionSlot = &vtable[kCreateSwapChainForCompositionSlot];
    }
}
}
