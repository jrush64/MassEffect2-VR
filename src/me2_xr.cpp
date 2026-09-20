#include "me2_xr.h"

#include "calcview_hook.h"
#include "convo_fp.h"
#include "d3d_capture.h"
#include "engine_probe.h"
#include "logger.h"
#include "me2_menu.h"
#include "xr_types.h"

#include <Windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace
{
using namespace MELEVR::Xr;

HMODULE g_loader = nullptr;
PFN_xrGetInstanceProcAddr g_getProc = nullptr;
PFN_xrPollEvent g_pollEvent = nullptr;
XrInstance g_instance = nullptr;
XrSystemId g_system = 0;
XrSession g_session = nullptr;
XrSpace g_localSpace = nullptr;  // re-origined on recenter; locate + submit happen against this
XrSpace g_baseSpace = nullptr;   // LOCAL, never re-origined; reads the absolute head at recenter
XrSpace g_viewSpace = nullptr;   // (unused since B2b; kept for reference)
Functions g_fn;
std::atomic_bool g_recenterRequested{false};

// Flat-mono quad placement (LE1 "theater" style; tune for comfort).
constexpr float kQuadDistanceM = 2.2f;   // metres in front of the head
constexpr float kQuadWidthM = 3.2f;      // panel width in metres

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;

// Per-eye swapchains (sized to the game backbuffer so CopyResource is a straight full-frame copy).
XrSwapchain g_swap[2] = {};
std::vector<ID3D11Texture2D*> g_images[2];
uint32_t g_swapW = 0, g_swapH = 0;   // per-EYE swapchain dims (half the SBS backbuffer width)
uint32_t g_bbW = 0;                  // full SBS backbuffer width
int64_t g_swapFormat = 0;

// UI quad layer (the captured GFx UI texture, shown zero-disparity over the world).
XrSwapchain g_uiSwap = nullptr;
std::vector<ID3D11Texture2D*> g_uiImages;
uint32_t g_uiW = 0, g_uiH = 0;
bool g_uiSwapTried = false;
// Game-UI overlay (HUD + menus, from D3DCapture's overlay RT) shown as one flat layer over the world.
XrSwapchain g_gameUiSwap = nullptr;
std::vector<ID3D11Texture2D*> g_gameUiImages;
uint32_t g_gameUiW = 0, g_gameUiH = 0;
constexpr XrFlags64 kLayerSrcAlpha = 0x00000002;   // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
constexpr float kUiQuadDistanceM = 2.0f;

// Mono fallback: full-screen frames (menus/loading) where the SBS split did NOT run are shown as a
// single full-backbuffer quad to BOTH eyes (no halving -> not cross-eyed).
XrSwapchain g_monoSwap = nullptr;
std::vector<ID3D11Texture2D*> g_monoImages;
unsigned long long g_prevSplitSeq = 0;
unsigned g_framesSinceSplit = 9999;          // hysteresis: stay SBS unless the split has been idle a while
constexpr unsigned kSbsHoldFrames = 30;      // ~0.5s - covers gaps where the split skips a present
std::atomic<unsigned long long> g_modeLogCount{0};
std::atomic<unsigned long long> g_aerModeLogCount{0};   // dedicated: the shared g_modeLogCount is spent by SBS startup lines before AER ever runs
// DIBR never-black fallback: last good located pose/FOV, reused on any frame the locate/backbuffer guard
// hiccups so DIBR (which has NO mono-quad fallback, unlike ME1) can never submit 0 layers = a hard black flash.
XrPosef g_dibrLastHeadPose = {};
XrFovf  g_dibrLastFovL = {}, g_dibrLastFovR = {};
bool    g_dibrHaveLast = false;
unsigned g_dibrFullFrames = 0, g_dibrFallbackFrames = 0, g_dibrBlackFrames = 0;
ULONGLONG g_dibrSubStatMs = 0;
int g_dibrSubEmitted = 0;   // stop after a bounded number of ~2s windows - health-check on launch, not forever
// Menu/flat-screen quad placement - tunable in the Insert menu (Tracking tab), persisted to the ini.
// Defaults: closer + bigger than the old constants (2.6m/2.3m read "too small and far" in-headset).
std::atomic<float> g_monoQuadDistanceM{1.6f};   // meters in front of the face
std::atomic<float> g_monoQuadWidthM{2.2f};      // meters wide (~69deg at 1.6m)
// [CINEFLAT] ME1-parity cutscene/conversation screen: the flat cine panel is WORLD-locked (LOCAL space,
// so head tracking lets you look around it and Insert recenters it) and gets its own zoom on top of the
// menu screen's dist/size. Menus keep the head-locked quad above untouched.
std::atomic<float> g_cineScreenZoom{1.0f};      // 1.0 = menu-screen size; ME1-style zoom slider
// [COMFORT] the Insert menu's OWN panel (distinct from the mono fallback quad above, which is the
// screen the GAME's menus/map/loading render onto). ME1 exposes all four; ME2 had them hardcoded.
std::atomic<float> g_menuQuadDistanceM{1.5f};
std::atomic<float> g_menuQuadWidthM{1.4f};
std::atomic<float> g_menuQuadOffXM{0.0f};
std::atomic<float> g_menuQuadOffYM{0.0f};

// --- AER (alternate-eye rendering), ported from ME1 xr_session.cpp 2026-07-04 -----------------
// Full-size eye swapchains: ME2's g_swap[2] are SBS-HALF (bbW/2) -> wrong for AER's full-frame copy.
XrSwapchain g_aerSwap[2] = {};
std::vector<ID3D11Texture2D*> g_aerImages[2];
uint32_t g_aerSwapW = 0, g_aerSwapH = 0;
// 2-slot full-backbuffer history bank: one eye captured fresh each present, the other held ~1 frame.
ID3D11Texture2D* g_aerHist[2] = { nullptr, nullptr };
bool g_aerHistValid[2] = { false, false };
// [AERBLINK] true once this eye's SWAPCHAIN holds real stereo content. OpenXR keeps presenting a
// swapchain's most recently RELEASED image until a new one is released, so an eye only needs an
// acquire/copy when its content actually changes - see SubmitAerFrame. Cleared by ResetAerHistory,
// which matters here because SFR/mono/DIBR submit through these same swapchains: after any of them
// has owned the image, AER must not assume its own content is still in there.
bool g_aerEyeFilled[2] = { false, false };
D3D11_TEXTURE2D_DESC g_aerHistDesc = {};
bool g_aerHistDescValid = false;
// THE anti-ghost pose-tag (sacred rule): each slot remembers the head pose it was CAPTURED at, so
// the STALE eye is submitted at the orientation it was actually rendered facing - not the current head yaw.
// Tagging the stale eye with the current pose (old shared-pose behavior, and ME1's too) is what ghosts on turn.
XrPosef g_aerHistPose[2] = {};
bool g_aerHistPoseValid[2] = { false, false };
uint64_t g_aerLastSeq = 0;           // last stamp seq consumed (the drift-free handshake)
uint64_t g_aerPresent = 0;
ULONGLONG g_aerHzWindowStartMs = 0;  // [AERHZ] cadence meter
unsigned int g_aerHzCaptures = 0;
int g_aerHzEmitted = 0;   // stop after a bounded number of ~2s windows - health-check on launch, not forever
// Alternation-health instrumentation (2026-07-04): PROVE whether the eye ping-pongs L,R,L,R cleanly or
// stalls/skips. sameEyeTwice>0 = the handshake mislabels (the shear source); seqGapBad>0 = renders were
// missed/doubled between presents (late-frame staleness). If both stay 0 the handshake is clean = the shake
// is pure late-frame staleness (perf), not a code bug. This makes the next headset test conclusive.
int g_aerPrevCaptureEye = -1;
unsigned int g_aerSameEyeTwice = 0;
unsigned int g_aerSeqGapBad = 0;
uint64_t g_aerMaxSeqGap = 0;
bool g_wasAer = false;               // edge-detect for ResetAerHistory on mode-leave
// [AERGAP] Attribution counters for the residual flicker, reported in [AERHZ]. seqGapBad says a
// render was skipped but not WHY. noRender = presents where the runtime returned shouldRender=false
// (the engine still rendered an eye, so the handshake must still be consumed - that was the bug).
// noStamp = presents where the engine produced no new view at all (benign: nothing to refresh).
unsigned int g_aerNoRenderPresents = 0;
unsigned int g_aerNoStampPresents = 0;
unsigned int g_aerPresentsWindow = 0;
// [AERBLACK] NEVER BLACK - the guard DIBR already has (see its g_dibrHaveLast fallback) and AER
// never got. AER builds its layer only inside "locateViews succeeded AND viewCount==2 AND backbuffer
// != null". A hiccup in any of those and NO layer is produced; since the AER branch also excludes
// the mono-quad fallback, the frame's layer count reaches ZERO, and xrEndFrame with 0 layers is a
// hard black frame on BOTH eyes. At 120Hz that is exactly one brief blink. Stash the last good
// pose/FOV so a hiccup frame re-presents the swapchain content it already holds instead.
XrPosef g_aerLastHeadPose = {};
XrFovf g_aerLastFovL = {}, g_aerLastFovR = {};
bool g_aerHaveLast = false;
unsigned int g_aerFallbackFrames = 0;   // frames rescued from black
unsigned int g_aerBlackFrames = 0;      // frames that still went black (true startup only)
ULONGLONG g_aerSubWindowMs = 0;
// AER display-locked pacing (the flicker fix): hold the present to display/2 so eye-swaps land on a stable
// integer fraction of the headset refresh. AER-only (pacing stereo/mono would halve their rate).
double g_displayPeriodSec = 1.0 / 120.0;
double g_paceWarmAccumSec = 0.0;
int g_paceWarmCount = 0;
XrTime g_lastPredictedDisplayTime = 0;
bool g_paceLocked = false;
bool g_refreshRateApplied = false;
bool g_hasRefreshRateExt = false;
// [LINKFOV] runtime quirks, set once at instance creation.
bool g_isOculusRuntime = false;   // Meta PC runtime: ignores declared FOV -> doubled image without the crop
bool g_isSteamVrRuntime = false;  // SteamVR: voids any view declared wider than its own frustum
std::atomic_bool g_questFovMatch{true};   // ini QuestFovMatch - user escape hatch for the Meta crop
// [VRFILL] ME1 parity. Fill was hardcoded to "widen to exactly the headset FOV"; ME1 lets you dial it
// per axis (and turn it off to render at the game's own FOV). 1.0 = fill the headset, lower = narrower.
std::atomic_bool g_vrFovFillEnabled{true};
std::atomic<float> g_vrFillH{1.0f};
std::atomic<float> g_vrFillV{1.0f};

void XLog(const std::string& s);
std::atomic_bool g_linkFov2Logged{false};
// [EYETAG2] last runtime-located per-eye FOV captured in the [LINKFOV] crop block (quirk runtimes only).
XrFovf g_lastRtFov[2] = {};
bool g_lastRtFovValid = false;

// Publish the fill target for this frame from the runtime's own vertical half-FOV + the render aspect.
// [LINKFOV] On the Meta runtime the fill must also COVER the runtime's asymmetric per-eye frustum:
// the compositor maps whatever rect the mod submits onto ITS OWN frustum, and the submit-side crop can only
// trim a declared window DOWN to that frustum - it cannot grow one that falls short. The aspect-derived
// target lands at ~+/-46 deg H while Quest's outer edge is ~54 deg, so each eye's image was stretched ~8 deg
// OUTWARD in opposite directions = the unfusable double (separation/convergence-independent). Clamping
// the fill up to the located per-eye extremes makes the crop land exactly on the frustum - the same
// geometry ME1 ships with. Gated on the quirk runtimes so VDXR stays bit-identical.
void ApplyFovFill(const XrView* views, float aspect) noexcept
{
    const float headVHalf = (views[0].fov.angleUp - views[0].fov.angleDown) * 0.5f;
    if (headVHalf <= 0.1f) return;
    if (!g_vrFovFillEnabled.load(std::memory_order_relaxed))
    {
        ME2VR::CalcViewHook::SetFovFill(0.0f, 0.0f, false);   // off -> render at the game's own FOV
        return;
    }
    const float fh = g_vrFillH.load(std::memory_order_relaxed);
    const float fv = g_vrFillV.load(std::memory_order_relaxed);
    float tV = atanf(tanf(headVHalf) * fv);
    float tH = atanf(tanf(tV) * aspect * (fh / (fv > 1e-4f ? fv : 1.0f)));
    if (g_isOculusRuntime && g_questFovMatch.load(std::memory_order_relaxed))
    {
        float hNeed = 0.0f, vNeed = 0.0f;
        for (int e = 0; e < 2; ++e)
        {
            const XrFovf& f = views[e].fov;
            if (-f.angleLeft  > hNeed) hNeed = -f.angleLeft;
            if (f.angleRight  > hNeed) hNeed = f.angleRight;
            if (f.angleUp     > vNeed) vNeed = f.angleUp;
            if (-f.angleDown  > vNeed) vNeed = -f.angleDown;
        }
        if (hNeed > 0.1f && hNeed < 1.4f && vNeed > 0.1f && vNeed < 1.4f)
        {
            // ONE uniform tan-space scale on BOTH axes, not per-axis clamps: the fill has always kept
            // tan(tV)/tan(tH) locked to the engine's render aspect, and [UIRATIO] shrinks the HUD per
            // axis by tan(game)/tan(render) - per-axis clamps changed the rendered H:V ratio and with
            // it the UI sub-viewport SHAPE (subtitles/wheel vertically stretched and sitting lower).
            // k is driven by whichever axis needs more, so the frustum stays covered on both.
            const float tanH = tanf(tH), tanV = tanf(tV);
            if (tanH > 1e-4f && tanV > 1e-4f)
            {
                float k = 1.0f;
                const float kH = tanf(hNeed) / tanH, kV = tanf(vNeed) / tanV;
                if (kH > k) k = kH;
                if (kV > k) k = kV;
                if (k > 1.0f)
                {
                    tH = atanf(tanH * k);
                    tV = atanf(tanV * k);
                    if (!g_linkFov2Logged.exchange(true))
                        XLog("[LINKFOV2] fill scaled uniformly to cover runtime frustum: k=" + std::to_string(k) +
                             " tH=" + std::to_string(tH) + " tV=" + std::to_string(tV) +
                             " (hNeed=" + std::to_string(hNeed) + " vNeed=" + std::to_string(vNeed) +
                             ") -> crop lands exactly on the frustum, UI aspect preserved");
                }
            }
        }
    }
    ME2VR::CalcViewHook::SetFovFill(tH, tV, true);
}
LARGE_INTEGER g_paceQpcFreq = {};
LARGE_INTEGER g_lastPaceQpc = {};
constexpr int kPaceWarmFrames = 90;
// [AERPACE2] per-~2s present-timing telemetry: how many presents arrived AFTER the display/2 deadline (the
// pace clamp can only slow a fast frame, never rescue a late one -> a late frame = a cadence slip = the
// flash). This MEASURES the flash directly instead of inferring it from the blind [AERHZ] rolling average.
ULONGLONG g_paceWinStartMs = 0;
unsigned int g_paceWinFrames = 0;
unsigned int g_paceWinLate = 0;
double g_paceWinMaxMs = 0.0;
double g_paceWinSumMs = 0.0;
int g_paceWinEmitted = 0;   // stop after a bounded number of ~2s windows - health-check on launch, not forever

std::atomic_bool g_tried{false};       // A1 init attempted
bool g_initOk = false;                 // session + swapchains ready
bool g_begun = false;                  // xrBeginSession done
XrSessionState g_state = 0;
std::atomic_bool g_submitLogged{false};

constexpr XrViewConfigurationType kStereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;

std::string FmtXr(XrResult r) { return std::to_string(static_cast<int>(r)); }
void XLog(const std::string& s) { ME2VR::Log::Line("[ME2XR] " + s); }
// [VRSTATE] frame-mode log: one line per presentation-mode CHANGE, uncapped. Replaces the old
// first-4-frames caps, which burned out at boot and left mid-session mode flips (the stereo-dead
// Archangel windows) completely unlogged. Changes are rare, so this stays quiet without a cap.
void LogFrameMode(int id, const char* line)
{
    static std::atomic<int> s_last{-1};
    if (s_last.exchange(id, std::memory_order_relaxed) != id) XLog(line);
}

bool Resolve(const char* name, PFN_xrVoidFunction* out) noexcept
{
    *out = nullptr;
    const XrResult r = g_getProc(g_instance, name, out);
    if (!XrSucceeded(r) || *out == nullptr) { XLog(std::string("resolve FAILED: ") + name + " (" + FmtXr(r) + ")"); return false; }
    return true;
}

#define RESOLVE(name, field, type)                                  \
    do { PFN_xrVoidFunction p = nullptr;                            \
         if (!Resolve(name, &p)) return false;                      \
         g_fn.field = reinterpret_cast<type>(p); } while (0)

bool ResolveFunctions() noexcept
{
    RESOLVE("xrDestroyInstance", destroyInstance, PFN_xrDestroyInstance);
    RESOLVE("xrGetSystem", getSystem, PFN_xrGetSystem);
    RESOLVE("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews, PFN_xrEnumerateViewConfigurationViews);
    RESOLVE("xrGetD3D11GraphicsRequirementsKHR", getD3D11GraphicsRequirements, PFN_xrGetD3D11GraphicsRequirementsKHR);
    RESOLVE("xrCreateSession", createSession, PFN_xrCreateSession);
    RESOLVE("xrDestroySession", destroySession, PFN_xrDestroySession);
    RESOLVE("xrEnumerateSwapchainFormats", enumerateSwapchainFormats, PFN_xrEnumerateSwapchainFormats);
    RESOLVE("xrCreateSwapchain", createSwapchain, PFN_xrCreateSwapchain);
    RESOLVE("xrEnumerateSwapchainImages", enumerateSwapchainImages, PFN_xrEnumerateSwapchainImages);
    RESOLVE("xrAcquireSwapchainImage", acquireSwapchainImage, PFN_xrAcquireSwapchainImage);
    RESOLVE("xrWaitSwapchainImage", waitSwapchainImage, PFN_xrWaitSwapchainImage);
    RESOLVE("xrReleaseSwapchainImage", releaseSwapchainImage, PFN_xrReleaseSwapchainImage);
    RESOLVE("xrBeginSession", beginSession, PFN_xrBeginSession);
    RESOLVE("xrEndSession", endSession, PFN_xrEndSession);
    RESOLVE("xrWaitFrame", waitFrame, PFN_xrWaitFrame);
    RESOLVE("xrBeginFrame", beginFrame, PFN_xrBeginFrame);
    RESOLVE("xrEndFrame", endFrame, PFN_xrEndFrame);
    RESOLVE("xrCreateReferenceSpace", createReferenceSpace, PFN_xrCreateReferenceSpace);
    RESOLVE("xrDestroySpace", destroySpace, PFN_xrDestroySpace);
    RESOLVE("xrLocateViews", locateViews, PFN_xrLocateViews);
    PFN_xrVoidFunction p = nullptr;
    if (!Resolve("xrPollEvent", &p)) return false;
    g_pollEvent = reinterpret_cast<PFN_xrPollEvent>(p);
    return true;
}

bool ChooseSwapchainFormat() noexcept
{
    uint32_t count = 0;
    if (!XrSucceeded(g_fn.enumerateSwapchainFormats(g_session, 0, &count, nullptr)) || count == 0) return false;
    std::vector<int64_t> formats(count);
    if (!XrSucceeded(g_fn.enumerateSwapchainFormats(g_session, count, &count, formats.data()))) return false;
    // Prefer an SRGB-declared format: the game backbuffer holds already display-encoded bytes, so an
    // SRGB swapchain tells the runtime not to re-apply gamma (fixes the washed-out color). LE1 does this.
    auto has = [&](int64_t f) { for (int64_t x : formats) if (x == f) return true; return false; };
    int64_t pick = formats[0];
    if (has(29)) pick = 29;          // R8G8B8A8_UNORM_SRGB
    else if (has(91)) pick = 91;     // B8G8R8A8_UNORM_SRGB
    else if (has(28)) pick = 28;     // R8G8B8A8_UNORM (no gamma correction available)
    g_swapFormat = pick;
    XLog("[XRAPI] swapchain format chosen=" + std::to_string(pick) + " (SRGB preferred; offered " + std::to_string(count) + ")");
    return true;
}

bool CreateEyeSwapchains() noexcept
{
    uint32_t viewCount = 0;
    if (!XrSucceeded(g_fn.enumerateViewConfigurationViews(g_instance, g_system, kStereo, 0, &viewCount, nullptr)) || viewCount < 2)
        return false;
    std::vector<XrViewConfigurationView> vcv(viewCount);
    for (auto& v : vcv) v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE;
    g_fn.enumerateViewConfigurationViews(g_instance, g_system, kStereo, viewCount, &viewCount, vcv.data());

    // B2b (stereo): per-eye swapchain = HALF the SBS backbuffer (left half / right half).
    g_bbW = ME2VR::D3DCapture::GetBackbufferWidth();
    const uint32_t bbH = ME2VR::D3DCapture::GetBackbufferHeight();
    if (g_bbW == 0 || bbH == 0) return false;
    g_swapW = g_bbW / 2;
    g_swapH = bbH;
    XLog("Rendering " + std::to_string(g_swapW * 2) + "x" + std::to_string(g_swapH) +
         " (" + std::to_string(g_swapW) + "x" + std::to_string(g_swapH) + " per eye). Headset suggests " +
         std::to_string(vcv[0].recommendedImageRectWidth) + "x" + std::to_string(vcv[0].recommendedImageRectHeight) + ".");

    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo sc = {};
        sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        sc.format = g_swapFormat;
        sc.sampleCount = 1;
        sc.width = g_swapW;
        sc.height = g_swapH;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        XrResult r = g_fn.createSwapchain(g_session, &sc, &g_swap[eye]);
        if (!XrSucceeded(r)) { XLog("xrCreateSwapchain eye " + std::to_string(eye) + " FAILED " + FmtXr(r)); return false; }

        uint32_t imgCount = 0;
        g_fn.enumerateSwapchainImages(g_swap[eye], 0, &imgCount, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount);
        for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
        g_fn.enumerateSwapchainImages(g_swap[eye], imgCount, &imgCount,
                                      reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        g_images[eye].clear();
        for (auto& im : imgs) g_images[eye].push_back(im.texture);
    }
    XLog("[XRAPI] eye swapchains created (" + std::to_string(g_images[0].size()) + " images/eye)");
    return true;
}

bool BringUp(ID3D11Device* device) noexcept
{
    g_device = device;
    g_device->GetImmediateContext(&g_ctx);

    g_loader = LoadLibraryW(L"openxr_loader.dll");
    if (g_loader == nullptr) { XLog("openxr_loader.dll not found. Skipping VR."); return false; }
    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
    if (g_getProc == nullptr) { XLog("xrGetInstanceProcAddr missing"); return false; }

    PFN_xrVoidFunction createInstanceRaw = nullptr;
    if (!XrSucceeded(g_getProc(nullptr, "xrCreateInstance", &createInstanceRaw)) || !createInstanceRaw) { XLog("no xrCreateInstance"); return false; }

    // Optional XR_FB_display_refresh_rate - used only for clean AER pacing. Request it only if the runtime
    // advertises it (requesting an unsupported extension makes xrCreateInstance fail outright).
    g_hasRefreshRateExt = false;
    {
        PFN_xrVoidFunction enumRaw = nullptr;
        if (XrSucceeded(g_getProc(nullptr, "xrEnumerateInstanceExtensionProperties", &enumRaw)) && enumRaw != nullptr)
        {
            auto enumFn = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(enumRaw);
            uint32_t n = 0;
            if (XrSucceeded(enumFn(nullptr, 0, &n, nullptr)) && n > 0)
            {
                std::vector<XrExtensionProperties> props(n);
                for (auto& p : props) { p.type = XR_TYPE_EXTENSION_PROPERTIES_VALUE; p.next = nullptr; }
                if (XrSucceeded(enumFn(nullptr, n, &n, props.data())))
                    for (auto& p : props)
                        if (strcmp(p.extensionName, "XR_FB_display_refresh_rate") == 0) { g_hasRefreshRateExt = true; break; }
            }
        }
    }
    const char* exts[2] = { kD3D11ExtensionName, "XR_FB_display_refresh_rate" };
    XrInstanceCreateInfo ci = {};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO_VALUE;
    ci.applicationInfo.apiVersion = MakeXrVersion(1, 0, 34);
    strcpy_s(ci.applicationInfo.applicationName, "ME2VR");
    strcpy_s(ci.applicationInfo.engineName, "ME2VR-M0");
    ci.enabledExtensionCount = g_hasRefreshRateExt ? 2u : 1u;
    ci.enabledExtensionNames = exts;
    XrResult r = reinterpret_cast<PFN_xrCreateInstance>(createInstanceRaw)(&ci, &g_instance);
    XLog("xrCreateInstance: " + FmtXr(r));
    if (!XrSucceeded(r)) return false;
    if (!ResolveFunctions()) return false;
    // [LINKFOV] Which runtime is the mod on? Meta's PC runtime (Quest Link / Air Link) composites
    // projection layers assuming ITS OWN per-eye frustum no matter what FOV the mod declares, which shows up
    // as a doubled image; SteamVR does the opposite and rejects any view declared wider than its
    // frustum. The same tan-space crop at submit fixes both. Non-fatal: on failure both flags stay
    // false and the submit path is bit-identical to before.
    g_isOculusRuntime = false;
    g_isSteamVrRuntime = false;
    {
        PFN_xrVoidFunction gipFn = nullptr;
        if (XrSucceeded(g_getProc(g_instance, "xrGetInstanceProperties", &gipFn)) && gipFn != nullptr)
        {
            XrInstanceProperties props = {};
            props.type = XR_TYPE_INSTANCE_PROPERTIES_VALUE;
            if (XrSucceeded(reinterpret_cast<PFN_xrGetInstanceProperties>(gipFn)(g_instance, &props)))
            {
                props.runtimeName[XR_MAX_RUNTIME_NAME_SIZE_VALUE - 1] = '\0';
                g_isOculusRuntime = std::strstr(props.runtimeName, "Oculus") != nullptr ||
                                    std::strstr(props.runtimeName, "Meta") != nullptr;
                g_isSteamVrRuntime = std::strstr(props.runtimeName, "SteamVR") != nullptr;
                XLog(std::string("VR runtime: ") + props.runtimeName + ".");
                XLog(std::string("[XRAPI] [XRRUNTIME] name='") + props.runtimeName +
                     "' oculusFovQuirk=" + (g_isOculusRuntime ? "1" : "0") +
                     " steamVrFovCrop=" + (g_isSteamVrRuntime ? "1" : "0"));
            }
        }
    }
    // Optional AER pacing helper - resolve non-fatally; pacing falls back to measurement if absent.
    if (g_hasRefreshRateExt)
    {
        PFN_xrVoidFunction p = nullptr;
        if (XrSucceeded(g_getProc(g_instance, "xrGetDisplayRefreshRateFB", &p)) && p != nullptr)
            g_fn.getDisplayRefreshRate = reinterpret_cast<PFN_xrGetDisplayRefreshRateFB>(p);
        XLog(std::string("[XRAPI] XR_FB_display_refresh_rate: ") + (g_fn.getDisplayRefreshRate ? "available" : "advertised-but-unresolved"));
    }

    XrSystemGetInfo sgi = {};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO_VALUE;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE;
    r = g_fn.getSystem(g_instance, &sgi, &g_system);
    XLog("[XRAPI] xrGetSystem(HMD): " + FmtXr(r));
    if (!XrSucceeded(r)) return false;

    XrGraphicsRequirementsD3D11KHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE;
    g_fn.getD3D11GraphicsRequirements(g_instance, g_system, &req);

    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE;
    binding.device = device;
    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO_VALUE;
    sci.next = &binding;
    sci.systemId = g_system;
    r = g_fn.createSession(g_instance, &sci, &g_session);
    XLog("[XRAPI] xrCreateSession(GAME device): " + FmtXr(r));
    if (!XrSucceeded(r)) return false;

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    rsci.poseInReferenceSpace = IdentityPose();
    g_fn.createReferenceSpace(g_session, &rsci, &g_localSpace);

    // Base LOCAL space, never re-origined - used to read the absolute head at recenter time.
    XrReferenceSpaceCreateInfo bsci = {};
    bsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    bsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    bsci.poseInReferenceSpace = IdentityPose();
    g_fn.createReferenceSpace(g_session, &bsci, &g_baseSpace);

    // Head-locked space for the flat mono quad (the panel follows your head, like LE1's menu).
    XrReferenceSpaceCreateInfo vsci = {};
    vsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW_VALUE;
    vsci.poseInReferenceSpace = IdentityPose();
    g_fn.createReferenceSpace(g_session, &vsci, &g_viewSpace);

    if (!ChooseSwapchainFormat()) { XLog("no swapchain format"); return false; }
    if (!CreateEyeSwapchains()) { XLog("eye swapchain creation failed"); return false; }

    g_initOk = true;
    XLog("VR session ready; starting frame submission.");
    return true;
}

void PumpEvents() noexcept
{
    XrEventDataBuffer ev = {};
    for (;;)
    {
        ev.type = XR_TYPE_EVENT_DATA_BUFFER_VALUE;
        const XrResult r = g_pollEvent(g_instance, &ev);
        if (r != XR_SUCCESS_VALUE) break;   // XR_EVENT_UNAVAILABLE or error
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE)
        {
            const auto* ss = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            g_state = ss->state;
            if (g_state == XR_SESSION_STATE_READY_VALUE && !g_begun)
            {
                XrSessionBeginInfo bi = {};
                bi.type = XR_TYPE_SESSION_BEGIN_INFO_VALUE;
                bi.primaryViewConfigurationType = kStereo;
                const XrResult br = g_fn.beginSession(g_session, &bi);
                XLog("[XRAPI] xrBeginSession: " + FmtXr(br));
                g_begun = XrSucceeded(br);
            }
            else if ((g_state == XR_SESSION_STATE_STOPPING_VALUE) && g_begun)
            {
                g_fn.endSession(g_session);
                g_begun = false;
            }
        }
    }
}

// Copy one HALF of the SBS backbuffer (starting at srcX) into this eye's swapchain image.
void CopyHalfToEye(int eye, ID3D11Texture2D* src, uint32_t srcX) noexcept
{
    if (g_swap[eye] == nullptr || src == nullptr) return;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_swap[eye], &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (XrSucceeded(g_fn.waitSwapchainImage(g_swap[eye], &wi)) && idx < g_images[eye].size())
    {
        D3D11_BOX box = {};
        box.left = srcX; box.right = srcX + g_swapW;
        box.top = 0;     box.bottom = g_swapH;
        box.front = 0;   box.back = 1;
        g_ctx->CopySubresourceRegion(g_images[eye][idx], 0, 0, 0, 0, src, 0, &box);
    }
    XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_swap[eye], &ri);
}

// --- free head-look (orientation), ported from LE1 ---------------------------------------
void HeadEulerDegrees(const XrQuaternionf& q, float& yawDeg, float& pitchDeg) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fy = -2.0f * (y * z - w * x);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    constexpr float kRad2Deg = 57.2957795f;
    const float cfy = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    yawDeg = atan2f(fx, -fz) * kRad2Deg;
    pitchDeg = asinf(cfy) * kRad2Deg;
}

// Strip head ROLL: keep where you're looking (forward) but force "up" toward world-up, so tilting
// your head never rolls the image. The submitted pose must be roll-free to match the roll-free render.
XrQuaternionf RemoveRoll(const XrQuaternionf& q) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    float fx = -2.0f * (x * z + w * y);
    float fy = -2.0f * (y * z - w * x);
    float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-5f) return q;
    fx /= fl; fy /= fl; fz /= fl;
    const float zx = -fx, zy = -fy, zz = -fz;             // camera back axis = -forward
    float rx = zz, ry = 0.0f, rz = -zx;                   // right = horizontal (worldUp x back)
    const float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-5f) return q;                             // looking straight up/down: leave as-is
    rx /= rl; ry /= rl; rz /= rl;
    const float ux = zy * rz - zz * ry;                   // up = back x right
    const float uy = zz * rx - zx * rz;
    const float uz = zx * ry - zy * rx;
    const float m00 = rx, m01 = ux, m02 = zx;
    const float m10 = ry, m11 = uy, m12 = zy;
    const float m20 = rz, m21 = uz, m22 = zz;
    XrQuaternionf o;
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f)
    {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        o.w = 0.25f * s; o.x = (m21 - m12) / s; o.y = (m02 - m20) / s; o.z = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        o.w = (m21 - m12) / s; o.x = 0.25f * s; o.y = (m01 + m10) / s; o.z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        o.w = (m02 - m20) / s; o.x = (m01 + m10) / s; o.y = 0.25f * s; o.z = (m12 + m21) / s;
    }
    else
    {
        const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        o.w = (m10 - m01) / s; o.x = (m02 + m20) / s; o.y = (m12 + m21) / s; o.z = 0.25f * s;
    }
    return o;
}

XrQuaternionf YawToQuat(float yawDeg) noexcept
{
    const float h = -yawDeg * 0.5f / 57.2957795f;
    XrQuaternionf q; q.x = 0.0f; q.y = sinf(h); q.z = 0.0f; q.w = cosf(h);
    return q;
}

float WrapDeg(float d) noexcept { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }

// Normalized-lerp between two quaternions (shortest arc via sign fix). Cheap + stable for the small
// per-frame deltas here - no slerp needed.
XrQuaternionf NlerpQuat(const XrQuaternionf& a, XrQuaternionf b, float t) noexcept
{
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    XrQuaternionf r;
    r.x = a.x + (b.x - a.x) * t; r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t; r.w = a.w + (b.w - a.w) * t;
    const float l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (l > 1e-6f) { r.x /= l; r.y /= l; r.z /= l; r.w /= l; }
    return r;
}
float QuatAngleDeg(const XrQuaternionf& a, const XrQuaternionf& b) noexcept
{
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    d = fabsf(d); if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * 57.2957795f;
}

// [POSETAG] unified-quat head smoothing (Bug 3) + render-time pose ring (Bug 1/2). Fed the raw located
// head each present; produces ONE smoothed roll-removed pose that BOTH the render (yaw/pitch -> SetHeadLook)
// and the submit tag read, and holds a 4-deep ring so the submit can tag the captured backbuffer with the
// pose that ACTUALLY rendered it (~poseTagDelayFrames presents back) instead of the current pose.
XrQuaternionf g_smoothQuat = { 0, 0, 0, 1 };
bool g_smoothQuatInit = false;
XrPosef g_tagRing[4] = { {{0,0,0,1},{0,0,0}}, {{0,0,0,1},{0,0,0}}, {{0,0,0,1},{0,0,0}}, {{0,0,0,1},{0,0,0}} };
bool g_tagRingInit = false;
XrPosef g_delayedTagPose = { {0,0,0,1}, {0,0,0} };   // read by the SFR submit
std::atomic_bool g_poseTagReset{ true };             // reset the ring on recenter / first use

// Push this present's smoothed render pose into the ring and compute the delayed tag. `smoothedRollFree`
// is the roll-removed smoothed orientation that fed the render; `pos` is the head position used.
void PoseTagPush(const XrQuaternionf& smoothedRollFree, const XrVector3f& pos) noexcept
{
    XrPosef cur; cur.orientation = smoothedRollFree; cur.position = pos;
    if (g_poseTagReset.exchange(false, std::memory_order_relaxed) || !g_tagRingInit)
    {
        for (int i = 0; i < 4; ++i) g_tagRing[i] = cur;
        g_tagRingInit = true;
    }
    else
    {
        g_tagRing[3] = g_tagRing[2]; g_tagRing[2] = g_tagRing[1]; g_tagRing[1] = g_tagRing[0];
        g_tagRing[0] = cur;
    }
    const float delay = ME2VR::CalcViewHook::GetPoseTagDelayFrames();
    int k0 = static_cast<int>(delay); if (k0 > 3) k0 = 3;
    int k1 = k0 + 1; if (k1 > 3) k1 = 3;
    const float frac = delay - static_cast<float>(k0);
    g_delayedTagPose.orientation = NlerpQuat(g_tagRing[k0].orientation, g_tagRing[k1].orientation, frac);
    g_delayedTagPose.position.x = g_tagRing[k0].position.x + (g_tagRing[k1].position.x - g_tagRing[k0].position.x) * frac;
    g_delayedTagPose.position.y = g_tagRing[k0].position.y + (g_tagRing[k1].position.y - g_tagRing[k0].position.y) * frac;
    g_delayedTagPose.position.z = g_tagRing[k0].position.z + (g_tagRing[k1].position.z - g_tagRing[k0].position.z) * frac;
}

// Re-origin g_localSpace to the current head yaw (level) + position, read from the never-moved base space.
void RecenterAppSpace(XrTime displayTime) noexcept
{
    if (g_baseSpace == nullptr) return;
    g_poseTagReset.store(true, std::memory_order_relaxed);   // [POSETAG] old ring samples are meaningless after re-origin
    XrViewLocateInfo li = {};
    li.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
    li.viewConfigurationType = kStereo;
    li.displayTime = displayTime;
    li.space = g_baseSpace;
    XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
    uint32_t cnt = 0;
    XrView bv[2] = {}; bv[0].type = bv[1].type = XR_TYPE_VIEW_VALUE;
    if (!XrSucceeded(g_fn.locateViews(g_session, &li, &vs, 2, &cnt, bv)) || cnt < 2 ||
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) == 0)
        return;
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    HeadEulerDegrees(bv[0].pose.orientation, yawDeg, pitchDeg);
    XrReferenceSpaceCreateInfo ci = {};
    ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    ci.poseInReferenceSpace.orientation = YawToQuat(yawDeg);
    ci.poseInReferenceSpace.position.x = (bv[0].pose.position.x + bv[1].pose.position.x) * 0.5f;
    ci.poseInReferenceSpace.position.y = (bv[0].pose.position.y + bv[1].pose.position.y) * 0.5f;
    ci.poseInReferenceSpace.position.z = (bv[0].pose.position.z + bv[1].pose.position.z) * 0.5f;
    XrSpace ns = nullptr;
    if (XrSucceeded(g_fn.createReferenceSpace(g_session, &ci, &ns)) && ns != nullptr)
    {
        if (g_localSpace != nullptr) g_fn.destroySpace(g_localSpace);
        g_localSpace = ns;
        XLog("Recentred view. Head yaw was " + std::to_string(static_cast<int>(yawDeg)));
    }
}

// Located head (already relative to the recentered localSpace) -> drive the render-side head-look +
// positional 6DOF. pose.position is the head's translation from recenter, in meters (XR axes).
void DriveHeadLook(const XrView& centerView) noexcept
{
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    ME2VR::CalcViewHook::SetHeadPos(centerView.pose.position.x, centerView.pose.position.y, centerView.pose.position.z);

    // [POSETAG] Bug 3 fix (ported from ME1): smooth ONE quaternion and feed BOTH the
    // render (yaw/pitch below -> SetHeadLook) and the submit tag (PoseTagPush) from it, so pixels and tag
    // can never disagree (the slow-turn "wobble then settle"). Adaptive follow: near 1:1 when the head
    // moves fast (no lag on real turns), heavy low-pass when nearly still (kills micro-jitter, lets the
    // game's TAA converge). Replaces the old per-axis Euler low-pass that only smoothed the render.
    const XrQuaternionf rawQ = centerView.pose.orientation;
    if (!g_smoothQuatInit) { g_smoothQuat = rawQ; g_smoothQuatInit = true; }
    const float smoothing = ME2VR::CalcViewHook::GetHeadLookSmoothing();   // 0=off .. ~0.4 default
    const float speedDeg  = QuatAngleDeg(g_smoothQuat, rawQ);              // per-frame head jump
    const float followSlow = 1.0f - smoothing * 0.95f;                    // heavy smoothing when still
    const float ramp = (speedDeg > 2.0f) ? 1.0f : (speedDeg * 0.5f);      // >2 deg/frame -> pass through ~1:1
    const float follow = followSlow + (1.0f - followSlow) * ramp;
    g_smoothQuat = NlerpQuat(g_smoothQuat, rawQ, (smoothing > 0.001f) ? follow : 1.0f);
    HeadEulerDegrees(g_smoothQuat, yawDeg, pitchDeg);                      // render consumes the SMOOTHED angle
    PoseTagPush(RemoveRoll(g_smoothQuat), centerView.pose.position);      // submit tag = same pose, delayed

    // [VRCINE] head-tracking-OFF during a VR cine: park render rotation so the head doesn't turn the
    // view; the submit anchors the stereo pair at the current head (head-locked 3D picture below).
    if (ME2VR::CalcViewHook::GetVrCineActive() && !ME2VR::CalcViewHook::GetCineVrHeadTracking())
    {
        ME2VR::CalcViewHook::SetHeadLook(0, 0, false);
        return;
    }

    // [VEHAIM] Hammerhead/Firewalker head behavior (both are offered as options):
    //   mode 1 FREE-LOOK  = head turns the VR view independently (fall through to render head-look);
    //                       aim + camera stay on the stick. The comfortable VR default.
    //   mode 2 CAMERA-AIM = head drives DriverViewYaw/Pitch (the chase camera); park render head-look.
    //   mode 0 OFF        = head does nothing in the vehicle.
    if (ME2VR::EngineProbe::IsInDrivableVehicle())
    {
        ME2VR::EngineProbe::ProbeVehicleFields();   // [VEHDIAG] pure-read: which field tracks the crosshair/fire?
        const int vmode = ME2VR::EngineProbe::GetVehicleHeadMode();
        if (vmode == 2 || vmode == 3) { ME2VR::EngineProbe::DriveVehicleAim(yawDeg, pitchDeg); ME2VR::CalcViewHook::SetHeadLook(0, 0, false); return; }
        if (vmode == 0) { ME2VR::CalcViewHook::SetHeadLook(0, 0, false); return; }
        // vmode 1 (free-look): fall through to the render head-look path below.
    }

    // [FPSTORM] storm latch (ME1 port, 2026-07-20): sprints use head-LOOK (+ [MOVEFIX] stick steer),
    // never head-aim - the sprint heading doesn't track ControlRotation 1:1, so head-aiming mid-sprint
    // runs the character the wrong way. ENTER on the Storm camera, or Interpolate + hard stick (bridges
    // the ~1s Combat->Storm blend) - but a high stick alone must NOT enter (walk-flap regression), and
    // the weapon-DRAW blend is an AIM-INTENT signal, not a sprint ([DRAWFIX]): suppress the Interpolate
    // enter for the draw blend's duration. HOLD while the stick stays pushed (the game flaps
    // Combat<->Storm mid-sprint); DROP after 12 quiet frames or INSTANTLY on a definitive aiming camera.
    // FIRING drops it instantly too, and blocks re-entry while the trigger is down: shooting cancels the
    // sprint but the player usually keeps the stick pushed, so the hold-while-pushed rule used to keep aim
    // on the stick heading for as long as the player ran - shots went where the body faced, not where the head looked.
    const bool weaponOut = ME2VR::EngineProbe::IsWeaponOut();
    const bool firing = ME2VR::EngineProbe::IsWeaponFiring();
    const bool modeStorm = ME2VR::EngineProbe::IsCamModeStorm();
    const bool inInterp = ME2VR::EngineProbe::IsCamModeInterp();
    const bool modeAiming = ME2VR::EngineProbe::IsCamModeAiming();
    const int stickMag = ME2VR::Menu::LeftStickMagnitude();
    static bool s_stormLatched = false;
    static int s_stormRelease = 0;
    static bool s_weaponPrev = false;
    static int s_drawGrace = 0;
    if (weaponOut && !s_weaponPrev) { s_stormLatched = false; s_stormRelease = 0; s_drawGrace = 120; }   // [DRAWFIX]
    s_weaponPrev = weaponOut;
    if (s_drawGrace > 0) --s_drawGrace;
    if (modeAiming || firing) { s_stormLatched = false; s_stormRelease = 0; }
    else if (modeStorm || (inInterp && stickMag > 24000 && s_drawGrace == 0)) { s_stormLatched = true; s_stormRelease = 0; }
    else if (s_stormLatched)
    {
        if (stickMag < 12000) { if (++s_stormRelease > 12) s_stormLatched = false; }
        else s_stormRelease = 0;
    }

    // Combat: the head drives the gun/aim (ControlRotation), which turns the camera -> suppress the extra
    // view-look so it doesn't double-rotate. Explore + storms: normal view head-look. DriveHeadAim
    // self-resets (and removes its injection) when excluded.
    // [STORMPITCH] while the storm latch holds, the head still drives aim PITCH (yaw stays on the stick so
    // the sprint heading is untouched). Looking up mid-sprint therefore raises the gun as you look, and the
    // shot goes up on the FIRST frame you fire instead of starting flat and ramping up afterwards.
    const bool stormAimPitch = s_stormLatched && weaponOut && ME2VR::EngineProbe::GetHeadAimEnabled();
    ME2VR::EngineProbe::DriveHeadAim(yawDeg, pitchDeg, !s_stormLatched, stormAimPitch);
    const bool aimOwns = ME2VR::EngineProbe::IsHeadAimActive() && !s_stormLatched;

    // [FPSTORM] diag: one glance at a sprint repro says which half (view path vs movement steer) misbehaves.
    if (modeStorm || inInterp)
    {
        static uint64_t s_fpStormLog = 0;
        if ((s_fpStormLog++ % 20ull) == 0)
        {
            char b[192] = {};
            sprintf_s(b, "[FPSTORM] storm=%d interp=%d aiming=%d fire=%d latched=%d aim=%d stick=%d moveYawUU=%d weap=%d",
                      modeStorm ? 1 : 0, inInterp ? 1 : 0, modeAiming ? 1 : 0, firing ? 1 : 0, s_stormLatched ? 1 : 0,
                      aimOwns ? 1 : 0, stickMag,
                      ME2VR::Menu::GetMoveFollowsHead() ? ME2VR::CalcViewHook::HeadLookYawUU() : 0,
                      weaponOut ? 1 : 0);
            XLog(b);
        }
    }

    if (aimOwns)
    {
        // [AIMSEED v2] during the look->aim handoff ramp, render the untransferred remainder as
        // head-look (signs converted back: CR-right -> view-left, pitch 1:1) so the two halves always
        // sum to the full offset = the view never moves while ControlRotation (and the pawn's animated
        // weapon aim, and the crosshair) glides to the gaze. Remainder exhausted -> aim owns rotation.
        int remY = 0, remP = 0;
        ME2VR::EngineProbe::GetAimSeedRem(&remY, &remP);
        if (remY != 0 || remP != 0) ME2VR::CalcViewHook::SetHeadLook(-remY, remP, true);
        else                        ME2VR::CalcViewHook::SetHeadLook(0, 0, false);
        return;
    }
    if (!ME2VR::CalcViewHook::GetHeadLookUserEnabled())
    {
        ME2VR::CalcViewHook::SetHeadLook(0, 0, false);
        return;
    }

    // View head-look (render-side): base signs yaw - 1 / pitch +1; invert flips each; sensitivity scales; pitch clamped +/-85.
    const float sens = ME2VR::CalcViewHook::GetLookSensitivity();
    const float yawSign   = ME2VR::CalcViewHook::GetInvertLookYaw()   ? +1.0f : -1.0f;
    const float pitchSign = ME2VR::CalcViewHook::GetInvertLookPitch() ? -1.0f : +1.0f;
    float dPitch = pitchSign * pitchDeg * sens;
    if (dPitch > 85.0f) dPitch = 85.0f;
    if (dPitch < -85.0f) dPitch = -85.0f;
    constexpr float kDegToUU = 65536.0f / 360.0f;
    const int yawUU = static_cast<int>(yawSign * WrapDeg(yawDeg) * sens * kDegToUU);
    const int pitchUU = static_cast<int>(dPitch * kDegToUU);
    if (stormAimPitch)
    {
        // Aim owns the pitch now, and turning ControlRotation pitches the camera with it - so render only
        // the untransferred pitch remainder (zero once the initial ramp finishes) or the view double-tilts.
        // Yaw is unchanged, which keeps [MOVEFIX] run-where-you-look working exactly as before.
        int remY = 0, remP = 0;
        ME2VR::EngineProbe::GetAimSeedRem(&remY, &remP);
        ME2VR::CalcViewHook::SetHeadLook(yawUU, remP, true);
        return;
    }
    ME2VR::CalcViewHook::SetHeadLook(yawUU, pitchUU, true);
}

void EnsureGameUiSwapchain(uint32_t w, uint32_t h) noexcept
{
    if (g_gameUiSwap != nullptr) return;
    if (w == 0 || h == 0) return;
    g_gameUiW = w; g_gameUiH = h;
    XrSwapchainCreateInfo sc = {};
    sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    sc.format = g_swapFormat; sc.sampleCount = 1; sc.width = w; sc.height = h;
    sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
    if (!XrSucceeded(g_fn.createSwapchain(g_session, &sc, &g_gameUiSwap)) || g_gameUiSwap == nullptr) { g_gameUiSwap = nullptr; return; }
    uint32_t c = 0;
    g_fn.enumerateSwapchainImages(g_gameUiSwap, 0, &c, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(c);
    for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
    g_fn.enumerateSwapchainImages(g_gameUiSwap, c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    g_gameUiImages.clear();
    for (auto& im : imgs) g_gameUiImages.push_back(im.texture);
    XLog("game-UI overlay swapchain created " + std::to_string(w) + "x" + std::to_string(h));
}

void EnsureUiSwapchain(uint32_t w, uint32_t h) noexcept
{
    if (g_uiSwap != nullptr) return;
    if (w == 0 || h == 0) return;
    g_uiW = w; g_uiH = h;
    XrSwapchainCreateInfo sc = {};
    sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    sc.format = g_swapFormat;
    sc.sampleCount = 1; sc.width = w; sc.height = h; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
    if (!XrSucceeded(g_fn.createSwapchain(g_session, &sc, &g_uiSwap)) || g_uiSwap == nullptr) { g_uiSwap = nullptr; return; }
    uint32_t c = 0;
    g_fn.enumerateSwapchainImages(g_uiSwap, 0, &c, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(c);
    for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
    g_fn.enumerateSwapchainImages(g_uiSwap, c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    g_uiImages.clear();
    for (auto& im : imgs) g_uiImages.push_back(im.texture);
    XLog("UI quad swapchain created " + std::to_string(w) + "x" + std::to_string(h));
}

void EnsureMonoSwapchain() noexcept
{
    if (g_monoSwap != nullptr || g_bbW == 0 || g_swapH == 0) return;
    XrSwapchainCreateInfo sc = {};
    sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    sc.format = g_swapFormat;
    sc.sampleCount = 1; sc.width = g_bbW; sc.height = g_swapH; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
    if (!XrSucceeded(g_fn.createSwapchain(g_session, &sc, &g_monoSwap)) || g_monoSwap == nullptr) { g_monoSwap = nullptr; return; }
    uint32_t c = 0;
    g_fn.enumerateSwapchainImages(g_monoSwap, 0, &c, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(c);
    for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
    g_fn.enumerateSwapchainImages(g_monoSwap, c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    g_monoImages.clear();
    for (auto& im : imgs) g_monoImages.push_back(im.texture);
    XLog("[XRAPI] mono fallback swapchain created " + std::to_string(g_bbW) + "x" + std::to_string(g_swapH));
}

// ============================ AER (alternate-eye rendering) ============================
// Full-size eye swapchains (full backbuffer, NOT the SBS half) - AER copies a whole frame per eye.
bool EnsureAerSwapchains() noexcept
{
    if (g_aerSwap[0] != nullptr && g_aerSwap[1] != nullptr) return true;
    g_aerSwapW = g_bbW;
    g_aerSwapH = g_swapH;   // = full backbuffer height
    if (g_aerSwapW == 0 || g_aerSwapH == 0) return false;
    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo sc = {};
        sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        sc.format = g_swapFormat;
        sc.sampleCount = 1; sc.width = g_aerSwapW; sc.height = g_aerSwapH;
        sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        if (!XrSucceeded(g_fn.createSwapchain(g_session, &sc, &g_aerSwap[eye])) || g_aerSwap[eye] == nullptr)
        { g_aerSwap[eye] = nullptr; return false; }
        uint32_t c = 0;
        g_fn.enumerateSwapchainImages(g_aerSwap[eye], 0, &c, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(c);
        for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
        g_fn.enumerateSwapchainImages(g_aerSwap[eye], c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        g_aerImages[eye].clear();
        for (auto& im : imgs) g_aerImages[eye].push_back(im.texture);
    }
    XLog("AER full-size eye swapchains created " + std::to_string(g_aerSwapW) + "x" + std::to_string(g_aerSwapH));
    return true;
}

void ResetAerHistory() noexcept
{
    for (int e = 0; e < 2; ++e)
    {
        if (g_aerHist[e] != nullptr) { g_aerHist[e]->Release(); g_aerHist[e] = nullptr; }
        g_aerHistValid[e] = false;
        g_aerEyeFilled[e] = false;   // [AERBLINK] swapchain content is not the mod's any more
    }
    g_aerHistDescValid = false;
    g_aerHistPoseValid[0] = false; g_aerHistPoseValid[1] = false;
    g_aerLastSeq = 0;
    g_aerPrevCaptureEye = -1;
    g_aerSameEyeTwice = 0; g_aerSeqGapBad = 0; g_aerMaxSeqGap = 0;
    ME2VR::CalcViewHook::SetAerRenderEye(0);
}

bool EnsureAerHistory(ID3D11Texture2D* src) noexcept
{
    if (g_device == nullptr || src == nullptr) return false;
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    d.BindFlags = 0; d.CPUAccessFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
    if (g_aerHistDescValid && g_aerHist[0] && g_aerHist[1] &&
        g_aerHistDesc.Width == d.Width && g_aerHistDesc.Height == d.Height && g_aerHistDesc.Format == d.Format)
        return true;
    ResetAerHistory();
    for (int e = 0; e < 2; ++e)
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_aerHist[e]))) { ResetAerHistory(); XLog("AER history texture creation FAILED"); return false; }
    g_aerHistDesc = d; g_aerHistDescValid = true;
    XLog("AER history bank created " + std::to_string(d.Width) + "x" + std::to_string(d.Height));
    return true;
}

// Full-frame -> whole eye (the UN-SQUASH). src/history/g_aerSwap are all full-backbuffer size, so a straight
// CopyResource works (no aspect crop = no squash of the game's ~square-FOV-rendered-into-16:9 backbuffer).
// shiftPx > 0 moves the copied image content toward +x (right), < 0 toward - x (left); 0 = plain full copy
// (byte-identical to the old behaviour, so every non-convergence caller is unchanged). Used by the SFR
// submit to apply convergence AFTER compositing - see [SFRCONV DECOUPLE] in calcview_hook.cpp.
bool CopyTextureToEyeFullFrame(int eye, ID3D11Texture2D* src, int shiftPx = 0) noexcept
{
    if (src == nullptr || g_aerSwap[eye] == nullptr) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_aerSwap[eye], &ai, &idx))) return false;
    bool copied = false;   // [AERBLINK] the caller must know if the image really changed
    XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (XrSucceeded(g_fn.waitSwapchainImage(g_aerSwap[eye], &wi)) && idx < g_aerImages[eye].size())
    {
        ID3D11Texture2D* dst = g_aerImages[eye][idx];
        copied = true;
        if (shiftPx == 0)
        {
            g_ctx->CopyResource(dst, src);
        }
        else
        {
            // [SFRCONV] convergence at submit: shift the fully-composited eye horizontally by shiftPx.
            // NO RTV/clear here on purpose - OpenXR runtimes often back swapchain images with a TYPELESS
            // resource, on which CreateRenderTargetView(dst, nullptr) FAILS, so the earlier clear-first
            // version silently fell through to a plain copy = zero convergence (the "convergence stopped
            // working" regression). Instead: full copy first (always valid, fills the whole eye), then
            // overwrite with the shifted region. The vacated strip keeps the unshifted edge sliver at the
            // extreme temporal periphery (behind the nose) rather than black - no RTV needed, and the
            // shift ALWAYS applies. World rides to the convergence plane; the identically-composited UI
            // shifts uniformly and cannot double.
            D3D11_TEXTURE2D_DESC sd = {}; src->GetDesc(&sd);
            const int W = static_cast<int>(sd.Width), H = static_cast<int>(sd.Height);
            int s = shiftPx;
            if (s > W - 1) s = W - 1; else if (s < -(W - 1)) s = -(W - 1);
            g_ctx->CopyResource(dst, src);                       // unshifted fill (guaranteed)
            D3D11_BOX box = {}; box.top = 0; box.bottom = static_cast<UINT>(H); box.front = 0; box.back = 1;
            UINT dstX;
            if (s >= 0) { box.left = 0; box.right = static_cast<UINT>(W - s); dstX = static_cast<UINT>(s); }
            else        { box.left = static_cast<UINT>(-s); box.right = static_cast<UINT>(W); dstX = 0; }
            g_ctx->CopySubresourceRegion(dst, 0, dstX, 0, 0, src, 0, &box);   // shifted overwrite
        }
    }
    XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_aerSwap[eye], &ri);
    return copied;
}

// Capture the freshly-rendered eye into its history slot (drift-free stamp handshake), then submit BOTH
// slots (one fresh, one ~1 frame old). The eye that actually landed drives which eye renders next.
// headPose = the current head pose (roll-removed, centered); stored per-slot so the STALE eye is later
// submitted at the orientation it was rendered facing (anti-ghost pose-tag).
// fillSwapchains=false: consume the render handshake and refresh history WITHOUT touching any
// OpenXR swapchain. Used on presents the runtime asked the mod not to render ([AERGAP]) - the engine
// rendered an eye regardless, and if nobody consumes that stamp the eye toggle never advances.
bool SubmitAerFrame(ID3D11Texture2D* backBuffer, const XrPosef& headPose, bool fillSwapchains) noexcept
{
    if (backBuffer == nullptr) return false;
    ++g_aerPresent;
    if (!EnsureAerHistory(backBuffer) || (fillSwapchains && !EnsureAerSwapchains()))
    {
        if (fillSwapchains)
        {
            CopyTextureToEyeFullFrame(0, backBuffer);   // bootstrap on alloc failure
            CopyTextureToEyeFullFrame(1, backBuffer);
        }
        return true;
    }
    // [AERSHAKE] FIFO consume: the OLDEST unconsumed build - the one whose pixels this present
    // actually shows - never "the latest stamp", whose eye can belong to a build still in flight.
    uint64_t seq = 0; int eye = -1; bool resynced = false;
    const bool newFrame = ME2VR::CalcViewHook::ConsumeAerStamp(g_aerLastSeq, &seq, &eye, &resynced) &&
                          eye >= 0 && eye <= 1;
    ++g_aerPresentsWindow;                                  // [AERGAP]
    if (!fillSwapchains) ++g_aerNoRenderPresents;           // [AERGAP] runtime said shouldRender=false
    if (!newFrame) ++g_aerNoStampPresents;                  // [AERGAP] engine produced no new view
    if (newFrame)
    {
        // Alternation health. With the FIFO, a resync (fell a whole ring behind) is the only
        // legitimate discontinuity; anything else showing here is a real bug again.
        if (resynced) { ++g_aerSeqGapBad; if (seq - g_aerLastSeq > g_aerMaxSeqGap) g_aerMaxSeqGap = seq - g_aerLastSeq; }
        if (eye == g_aerPrevCaptureEye) ++g_aerSameEyeTwice;   // same eye twice = alternation stall = shear
        g_aerPrevCaptureEye = eye;

        g_ctx->CopyResource(g_aerHist[eye], backBuffer);
        g_aerHistValid[eye] = true;
        g_aerHistPose[eye] = headPose;          // tag THIS eye with the pose it was rendered at (anti-ghost)
        g_aerHistPoseValid[eye] = true;
        g_aerLastSeq = seq;
        const ULONGLONG now = GetTickCount64();
        if (g_aerHzWindowStartMs == 0) { g_aerHzWindowStartMs = now; g_aerHzCaptures = 0; }
        ++g_aerHzCaptures;
        const ULONGLONG el = now - g_aerHzWindowStartMs;
        if (el >= 2000 && g_aerHzEmitted < 30)
        {
            const double capHz = static_cast<double>(g_aerHzCaptures) * 1000.0 / static_cast<double>(el);
            char line[256] = {};
            sprintf_s(line, "[AERHZ] captureHz=%.1f captures=%u sameEyeTwice=%u seqGapBad=%u maxSeqGap=%llu "
                            "presents=%u noRender=%u noStamp=%u halfEyeUU=%.2f",
                      capHz, g_aerHzCaptures, g_aerSameEyeTwice, g_aerSeqGapBad,
                      static_cast<unsigned long long>(g_aerMaxSeqGap),
                      g_aerPresentsWindow, g_aerNoRenderPresents, g_aerNoStampPresents,
                      ME2VR::CalcViewHook::GetAerHalfEyeUU());
            XLog(line);
            ++g_aerHzEmitted;
            g_aerHzWindowStartMs = now; g_aerHzCaptures = 0;
            g_aerSameEyeTwice = 0; g_aerSeqGapBad = 0; g_aerMaxSeqGap = 0;
            g_aerPresentsWindow = 0; g_aerNoRenderPresents = 0; g_aerNoStampPresents = 0;
        }
    }
    // [AERBLINK 2026-08-21, ported from ME3] Copy ONLY what changed. This used to acquire/wait/copy
    // BOTH full-size eyes every present - three full-backbuffer copies and two swapchain waits per
    // present, two of which re-uploaded content that had not changed since the previous present.
    // Every swapchain wait hands the render thread to the runtime's compositor, and when that
    // compositor hiccups (streaming runtimes especially) the game misses vsyncs it would otherwise
    // have made - which is what the AER flicker actually is. OpenXR keeps presenting a swapchain's
    // most recently RELEASED image, so the STALE eye needs no work at all, and a present with no
    // fresh render needs no swapchain work whatsoever.
    if (!fillSwapchains) return true;   // [AERGAP] handshake kept alive; nothing to present this frame
    if (g_aerHistValid[0] && g_aerHistValid[1])
    {
        if (newFrame && CopyTextureToEyeFullFrame(eye, g_aerHist[eye])) g_aerEyeFilled[eye] = true;
        // Catch-up: an eye whose swapchain has never held real stereo content (the first pair after
        // bootstrap, or one that SFR/mono/DIBR owned before AER was re-entered) gets one top-up copy.
        for (int e = 0; e < 2; ++e)
            if (!g_aerEyeFilled[e] && CopyTextureToEyeFullFrame(e, g_aerHist[e])) g_aerEyeFilled[e] = true;
    }
    else
    {
        // Bootstrap until both slots captured once: the same mono frame to both eyes, refreshed
        // every present. Deliberately does NOT set g_aerEyeFilled - the first real stereo pair must
        // overwrite this mono content via the catch-up above.
        CopyTextureToEyeFullFrame(0, backBuffer);
        CopyTextureToEyeFullFrame(1, backBuffer);
    }
    return true;
}

// Learn the headset's TRUE refresh. Order: pinned Hz -> XR_FB_display_refresh_rate -> measure. (ME1 verbatim.)
void UpdateAerPaceWarmup(XrTime predictedDisplayTime, int pinnedHz) noexcept
{
    if (pinnedHz > 0)
    {
        const double p = 1.0 / static_cast<double>(pinnedHz);
        if (!g_paceLocked || std::fabs(p - g_displayPeriodSec) > 1e-6)
        {
            g_displayPeriodSec = p; g_paceLocked = true; g_refreshRateApplied = true;
            XLog("[PACE] cadence: pinned " + std::to_string(pinnedHz) + "Hz -> stable session lock");
        }
        return;
    }
    if (!g_refreshRateApplied && g_fn.getDisplayRefreshRate != nullptr && g_session != nullptr)
    {
        float hz = 0.0f;
        if (XrSucceeded(g_fn.getDisplayRefreshRate(g_session, &hz)) && hz > 20.0f && hz < 1000.0f)
        {
            g_displayPeriodSec = 1.0 / static_cast<double>(hz); g_paceLocked = true; g_refreshRateApplied = true;
            XLog("[PACE] cadence: runtime reports " + std::to_string(static_cast<int>(hz + 0.5f)) +
                 "Hz -> stable session lock (no frame-delta guess)");
            return;
        }
    }
    if (g_paceLocked) return;
    if (g_lastPredictedDisplayTime != 0 && predictedDisplayTime > g_lastPredictedDisplayTime)
    {
        const double dt = static_cast<double>(predictedDisplayTime - g_lastPredictedDisplayTime) * 1e-9;
        if (dt > 0.004 && dt < 0.05) { g_paceWarmAccumSec += dt; ++g_paceWarmCount; }
    }
    g_lastPredictedDisplayTime = predictedDisplayTime;
    if (g_paceWarmCount >= kPaceWarmFrames && g_paceWarmAccumSec > 0.0)
    {
        const double hz = static_cast<double>(g_paceWarmCount) / g_paceWarmAccumSec;
        const double cands[] = { 60.0, 72.0, 80.0, 90.0, 100.0, 120.0, 144.0 };
        double best = 120.0, bestErr = 1e9;
        for (double c : cands) { const double e = (c > hz) ? (c - hz) : (hz - c); if (e < bestErr) { bestErr = e; best = c; } }
        g_displayPeriodSec = 1.0 / best; g_paceLocked = true;
        XLog("[PACE] cadence: measured ~" + std::to_string(static_cast<int>(hz + 0.5)) +
             "Hz -> stable session lock at " + std::to_string(static_cast<int>(best + 0.5)) + "Hz");
    }
}

// Display-lock presents to either one headset period (full refresh) or two periods (half refresh).
// The choice is explicit and shared by AER/SFR/Stereo; cadence learning only determines the headset
// period and locks it once per session. The old AER-only 2x clamp guarded an obsolete feedback loop:
// measured frame time used to select 1x/2x while the selection itself changed measured frame time.
// Runtime XR_FB_display_refresh_rate (or the one-shot fallback learner) now locks the period before
// this decision, so full-rate AER is stable. At full rate AER alternates on successive vsyncs and each
// eye refreshes at display/2 while the game renders only one eye per present. Coarse Sleep(1), then
// a tight QPC spin, holds the selected cadence.
void PaceDisplayLockedAer(bool allow1x) noexcept
{
    if (!g_paceLocked) { g_lastPaceQpc.QuadPart = 0; return; }
    if (g_paceQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_paceQpcFreq);
    // Fixed divisor chosen by the user. Full-rate AER is regular (alternate eyes on alternate
    // vsyncs); it does not need the old forced-half clamp. If a machine cannot feed full refresh,
    // the user can switch this setting off for a steady display/2 cadence without an adaptive loop.
    const bool full = allow1x && ME2VR::CalcViewHook::GetFullRefreshPacing();
    const double target = full ? g_displayPeriodSec : 2.0 * g_displayPeriodSec;
    // [PACEDIAG] The framerate lock is either this decision or the game simply being slow, and the two
    // look identical from outside (16.7ms measured at BOTH 6144x3456 and 4096x2304 - unchanged by a 2.25x
    // pixel cut, which is a cap, not load). Log the decision + its inputs once, and again whenever it
    // changes, so "is the pacer holding it at 60?" stops being a guess.
    {
        static int s_lastKey = -1;
        const int key = (full ? 1 : 0) | (allow1x ? 2 : 0) |
                        (ME2VR::CalcViewHook::GetFullRefreshPacing() ? 4 : 0) |
                        (static_cast<int>(1.0 / g_displayPeriodSec + 0.5) << 3);
        if (key != s_lastKey)
        {
            s_lastKey = key;
            XLog("[PACEDIAG] targetMs=" + std::to_string(target * 1000.0) +
                 " full=" + std::to_string(full ? 1 : 0) +
                 " allow1x=" + std::to_string(allow1x ? 1 : 0) +
                 " iniFullRefresh=" + std::to_string(ME2VR::CalcViewHook::GetFullRefreshPacing() ? 1 : 0) +
                 " displayHz=" + std::to_string(static_cast<int>(1.0 / g_displayPeriodSec + 0.5)));
        }
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double entryElapsed = -1.0;   // natural inter-present time BEFORE the mod paces (the perf truth)
    if (g_lastPaceQpc.QuadPart != 0)
    {
        entryElapsed = static_cast<double>(now.QuadPart - g_lastPaceQpc.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
        double elapsed = entryElapsed;
        while (target - elapsed > 0.0)
        {
            if (target - elapsed > 0.0015) Sleep(1);
            QueryPerformanceCounter(&now);
            elapsed = static_cast<double>(now.QuadPart - g_lastPaceQpc.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
        }
    }
    QueryPerformanceCounter(&g_lastPaceQpc);

    // Telemetry only: if natural frame time is already past the selected deadline, pacing cannot
    // recover that present. Count misses per window so headset tests show whether AER alternation
    // is missing the requested full- or half-refresh cadence.
    if (entryElapsed >= 0.0)
    {
        const double targetMs = target * 1000.0;
        const double rawMs = entryElapsed * 1000.0;
        ++g_paceWinFrames;
        g_paceWinSumMs += rawMs;
        if (rawMs > g_paceWinMaxMs) g_paceWinMaxMs = rawMs;
        if (rawMs > targetMs * 1.02) ++g_paceWinLate;   // >2% over the deadline = a genuine slip
        const ULONGLONG nowMs = GetTickCount64();
        if (g_paceWinStartMs == 0) g_paceWinStartMs = nowMs;
        if (nowMs - g_paceWinStartMs >= 2000 && g_paceWinFrames > 0 && g_paceWinEmitted < 30)
        {
            char line[220] = {};
            sprintf_s(line, "[AERPACE2] presents=%u lateFrames=%u (%.0f%%) targetMs=%.2f avgMs=%.2f maxMs=%.2f",
                      g_paceWinFrames, g_paceWinLate,
                      100.0 * static_cast<double>(g_paceWinLate) / static_cast<double>(g_paceWinFrames),
                      targetMs, g_paceWinSumMs / static_cast<double>(g_paceWinFrames), g_paceWinMaxMs);
            XLog(line);
            ++g_paceWinEmitted;
            g_paceWinStartMs = nowMs; g_paceWinFrames = 0; g_paceWinLate = 0; g_paceWinMaxMs = 0.0; g_paceWinSumMs = 0.0;
        }
    }
}


bool CaptureMovieBackbufferBmp(int sampleFrame) noexcept
{
    if (g_device == nullptr || g_ctx == nullptr) return false;
    IDXGISwapChain* swapChain = ME2VR::D3DCapture::GetGameSwapChain();
    if (swapChain == nullptr) return false;
    ID3D11Texture2D* source = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&source))) || source == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    ID3D11Texture2D* staging = nullptr;
    const HRESULT createHr = g_device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(createHr) || staging == nullptr)
    {
        source->Release();
        return false;
    }
    g_ctx->CopyResource(staging, source);
    source->Release();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT mapHr = g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapHr))
    {
        staging->Release();
        return false;
    }

    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0)
    {
        g_ctx->Unmap(staging, 0);
        staging->Release();
        return false;
    }
    std::wstring dir = std::wstring(localAppData) + L"\\MELE2VR";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t sampleName[64] = {};
    swprintf_s(sampleName, L"\\MELE2VR_MovieFrame_%03d.bmp", sampleFrame);
    const std::wstring path = dir + sampleName;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        g_ctx->Unmap(staging, 0);
        staging->Release();
        return false;
    }

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};
    const DWORD pixelBytes = desc.Width * desc.Height * 4;
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = static_cast<LONG>(desc.Width);
    infoHeader.biHeight = static_cast<LONG>(desc.Height);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    DWORD written = 0;
    WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
    WriteFile(file, &infoHeader, sizeof(infoHeader), &written, nullptr);

    std::vector<unsigned char> row(static_cast<size_t>(desc.Width) * 4);
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    for (int y = static_cast<int>(desc.Height) - 1; y >= 0; --y)
    {
        const auto* src = static_cast<const unsigned char*>(mapped.pData) +
                          static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x)
        {
            row[x * 4 + 0] = rgba ? src[x * 4 + 2] : src[x * 4 + 0];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = rgba ? src[x * 4 + 0] : src[x * 4 + 2];
            row[x * 4 + 3] = 0xFF;
        }
        WriteFile(file, row.data(), static_cast<DWORD>(row.size()), &written, nullptr);
    }
    CloseHandle(file);
    g_ctx->Unmap(staging, 0);
    staging->Release();
    XLog("[MOVIECAP] sample=" + std::to_string(sampleFrame) + " saved untouched backbuffer " +
         std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
         " fmt=" + std::to_string(static_cast<int>(desc.Format)));
    return true;
}
void RunFrame() noexcept
{
    // [PERF] present-cadence probe: avg/max present-to-present time + late count (> 1.5x refresh) +
    // SFR replay (2nd render) cost, one line every ~10s. Names the bottleneck when the frame rate is poor:
    // avg high = raw GPU/engine load (lower res); replayAvg ~ half of avg = the double render is the
    // cost (expected); late spikes with low avg = cadence/streaming hitches (pacer / loading).
    // Counters reset per window; the totals are advisory only. Hinny that GOAT.
    {
        static LARGE_INTEGER s_qpf = {}, s_prev = {};
        static double s_sumMs = 0.0, s_maxMs = 0.0;
        static unsigned s_frames = 0, s_late = 0;
        if (s_qpf.QuadPart == 0) QueryPerformanceFrequency(&s_qpf);
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        if (s_prev.QuadPart != 0)
        {
            const double ms = 1000.0 * static_cast<double>(now.QuadPart - s_prev.QuadPart) / static_cast<double>(s_qpf.QuadPart);
            if (ms < 1000.0)   // ignore pauses/loads parked >1s
            {
                s_sumMs += ms; if (ms > s_maxMs) s_maxMs = ms; ++s_frames;
                const double period = (g_displayPeriodSec > 1e-4) ? g_displayPeriodSec * 1000.0 : 8.33;
                if (ms > 3.0 * period) ++s_late;   // 2 display frames is the SFR budget; 3+ = a real slip
                if (s_frames >= 600)
                {
                    long long rAvg = 0, rMax = 0, rN = 0;
                    ME2VR::CalcViewHook::GetSfrReplayStats(&rAvg, &rMax, &rN);
                    char b[224] = {};
                    sprintf_s(b, "[PERF] avg=%.1fms max=%.1fms late=%u/%u (>%.1fms) replayAvg=%.1fms replayMax=%.1fms replays=%lld bb=%ux%u",
                              s_sumMs / s_frames, s_maxMs, s_late, s_frames, 3.0 * period,
                              rAvg / 1000.0, rMax / 1000.0, rN, g_bbW, g_swapH);
                    XLog(b);
                    s_sumMs = 0.0; s_maxMs = 0.0; s_frames = 0; s_late = 0;
                }
            }
        }
        s_prev = now;
    }

    // VR mode (0=Mono, 1=Stereo, 2=AER). AER adds display-locked pacing + a mode-leave history reset.
    constexpr int kModeStereo = 1;
    constexpr int kModeAer = 2;
    const int vrMode = ME2VR::CalcViewHook::GetVrMode();
    const bool aerMode = (vrMode == kModeAer);
    const bool stereoMode = (vrMode == kModeStereo);
    constexpr int kModeSfr = 4;
    const bool sfrMode = (vrMode == kModeSfr);   // [SFR] same-frame stereo: double-render, pass0->L pass1->R

    // Free the AER history bank + reset the eye toggle whenever AER isn't the active mode (edge-triggered).
    if (!aerMode && g_wasAer) ResetAerHistory();
    g_wasAer = aerMode;

    // DIBR (mode 3): the 4th VR mode. Depth is captured at ClearDSV (mode-gated), the warp shader synthesizes
    // the right eye from the mono frame + depth. Enable the whole capture/warp pipeline ONLY in mode 3.
    constexpr int kModeDibr = 3;
    const bool dibrMode = (vrMode == kModeDibr);
    // Mono (mode 0) as a GAMEPLAY choice, not the menu/loading fallback. Until now Mono fell into the
    // exact same "MONO fallback" quad branch below used for menus: a floating screen at a fixed
    // distance, submitted in VIEW space (head-locked -- it moves WITH your head, by construction).
    // calcview_hook's Mono branch (added 2026-07-26) does rotate the game camera with head-look, but
    // that fix could never be visible through a head-locked screen: pan the camera one way and the
    // screen displaying it pans the identical amount, and the two cancel out completely. On top of
    // that, DriveHeadLook() -- which reads the real HMD orientation into the values ApplyHeadLook
    // consumes -- is only ever called from the SFR/AER/DIBR paths below, never from the menu-fallback
    // branch, so the camera fix had nothing to read even where it could have shown through. Two
    // independent reasons "head tracking does nothing in Mono", both in presentation, neither in the
    // camera. Fixed the same way DIBR is: a real world-space projection layer, both eyes given the
    // identical backbuffer (no depth-warp synthesis needed for a genuinely mono mode).
    constexpr int kModeMono = 0;
    const bool monoMode = (vrMode == kModeMono);
    // Only take the new immersive path during actual gameplay -- same exclusion sbsFrame already uses,
    // so menus and cutscenes still get the comfortable head-locked reading screen either way.
    bool monoGameplay = false; // assigned after this frame's single presentation owner is resolved
    ME2VR::D3DCapture::SetDepthMapEnabled(dibrMode);
    if (dibrMode)
    {
        // Auto-convergence: put whatever the player is looking at on the screen plane.
        // STABILITY REWORK (2026-07-12: "keeps moving around"): the input is now a median-of-9
        // central region (d3d_capture) refreshed every 2nd capture, and this side is a DEADBAND + RATE
        // LIMIT instead of a bare low-pass: sub-noise wobble moves the plane not at all, a real subject
        // change glides it over ~1s, and a scene cut (big jump) snaps in a few frames. A bare low-pass
        // passes every bit of noise through at reduced amplitude - that was the visible hunting.
        static float s_dibrAutoConv = 0.985f;
        float pcC = 0.0f, tl = 0.0f, br = 0.0f, tr = 0.0f;
        ME2VR::D3DCapture::GetDepthProbe(&pcC, &tl, &br, &tr);
        const bool autoConv = ME2VR::CalcViewHook::GetDibrAutoConverge();
        if (autoConv && pcC > 0.0f)
        {
            const float err = pcC - s_dibrAutoConv;
            const float mag = (err < 0.0f) ? -err : err;
            if (mag > 0.0015f)   // deadband: depth noise smaller than this never moves the plane
            {
                float step = 0.10f * err;
                const float maxStep = (mag > 0.03f) ? 0.012f : 0.0012f;   // scene cut: fast; otherwise: glide
                if (step > maxStep) step = maxStep;
                else if (step < -maxStep) step = -maxStep;
                s_dibrAutoConv += step;
            }
        }
        const float conv = autoConv ? s_dibrAutoConv : ME2VR::CalcViewHook::GetDepthWarpConv();
        ME2VR::D3DCapture::SetDibrWarp(ME2VR::CalcViewHook::GetDepthWarpGain(), conv,
                                       ME2VR::CalcViewHook::GetDepthWarpFlip());
    }

    // Display-lock cadence before waitFrame; no-op until headset refresh is locked for the session.
    // FullRefreshPacing selects one display period, otherwise two. At full rate AER alternates eyes
    // on consecutive vsyncs (one eye render per present, each eye updated at display/2). SFR/Stereo
    // submit both eyes per present. Keeping the selection fixed avoids the old feedback oscillation
    // while letting users choose full throughput or a steadier half-refresh cadence. AER uses its
    // own pacing-enable toggle; SFR/Stereo share StereoFramePacing.
    const bool wantPace = (aerMode && ME2VR::CalcViewHook::GetAerFramePacing()) ||
                          ((stereoMode || sfrMode) && ME2VR::CalcViewHook::GetStereoFramePacing());
    if (wantPace) PaceDisplayLockedAer(true);

    XrFrameWaitInfo fwi = {}; fwi.type = XR_TYPE_FRAME_WAIT_INFO_VALUE;
    XrFrameState fs = {}; fs.type = XR_TYPE_FRAME_STATE_VALUE;
    if (!XrSucceeded(g_fn.waitFrame(g_session, &fwi, &fs))) return;

    // Learn the headset's true refresh (runtime FB query first, else measured) so the pace above locks correctly.
    // Same headset Hz regardless of mode, so stereo/SFR reuse the AER pacing-Hz control.
    if (aerMode || stereoMode || sfrMode) UpdateAerPaceWarmup(fs.predictedDisplayTime, ME2VR::CalcViewHook::GetAerFramePacingHz());

    XrFrameBeginInfo fbi = {}; fbi.type = XR_TYPE_FRAME_BEGIN_INFO_VALUE;
    g_fn.beginFrame(g_session, &fbi);

    // B2b stereo: project the two SBS halves to the two eyes. Each eye declares the GAME's rendered
    // FOV (not the headset FOV -> no zoom) and the runtime's located eye pose (head tracking).
    XrCompositionLayerProjectionView projViews[2] = {};
    XrCompositionLayerProjection layer = {};
    bool rendered = false;

    // Decoder activity is the authoritative prerecorded-video signal. Restrict it to
    // non-gameplay modes so an in-world Bink screen cannot flatten an otherwise live scene.
    const int gmRaw = ME2VR::EngineProbe::ReadGameModeSEH();
    const int loadMovieState = ME2VR::EngineProbe::ReadLoadMoviePlaybackStateSEH();
    const bool loadMoviePlaying = loadMovieState == 1;
    const bool binkRecentlyActive = ME2VR::D3DCapture::LastBinkFrameAgeMs() < 250ull;
    const bool binkFullscreen = binkRecentlyActive && gmRaw >= 5 && gmRaw <= 8;
    const bool moviePlayback = loadMoviePlaying || binkFullscreen;
    // [MOVIEOWNER] Real decoder/loading activity outranks ambiguous engine mode 8. With that
    // ground truth available, every non-movie mode-8 scene is an in-engine cinematic.
    const int gmEff = moviePlayback ? 8 : (gmRaw == 8 ? 6 : gmRaw);
    ME2VR::D3DCapture::SetAutoGameMode(gmEff);
    // Publish pause beside the effective mode so blocking GUI mode 7 remains a verified menu owner.
    ME2VR::D3DCapture::SetAutoPaused(ME2VR::EngineProbe::IsGamePaused());



    // SBS this frame? The split bumps a counter each gameplay frame, but it can skip the odd present
    // (~7 of 8). Switching mono<->SBS on a single skipped frame caused the flashing. Use hysteresis:
    // reset on any split, count idle presents, and stay in SBS until the split has been idle a while.
    const unsigned long long curSplit = ME2VR::CalcViewHook::GetSplitSeq();
    if (curSplit != g_prevSplitSeq) { g_prevSplitSeq = curSplit; g_framesSinceSplit = 0; }
    else if (g_framesSinceSplit < 100000u) { ++g_framesSinceSplit; }
    bool sbsFrame = false; // assigned after current-frame menu/cinematic ownership is resolved


    // Every frame: detect conversation/cutscene by FOV MAGNITUDE (cinematics are NARROW, ~15-30deg
    // half; gameplay is wide, ~45deg+). Magnitude works for both enter AND exit -- unlike aspect, which
    // reads ~1.78 forever once the mod is in the full-view mono path. calcview keeps publishing the FOV even
    // in mono so this stays live. kCineHalfMaxRad = ~38deg threshold.
    // Measured (ME2): cutscene/convo half-FOV <= 30deg; gameplay >= 35deg (45 split / 35 mono).
    // HYSTERESIS in the 30-35 gap: enter cinematic below 32deg, exit above 34deg, keep state between.
    // This is what fixes the stuck: gameplay's mono full-view (35deg) is ABOVE the exit threshold.
    // [AUTOMENU] ME1 parity: publish the engine's EGameModes byte every frame. GetMenuMode() then
    // auto-forces mono for GUI menu 7 / movie 8 / galaxy 9 / orbital 10 (when the mono-menus bool is
    // on) - pause menu, inventory, map etc. drop to a flat panel with no manual toggle. Read-only SEH.
    // [ENGCINE] (ME3 port, 2026-07-30) gm 8 "Movie" covers BOTH prerendered biks (no depth exists ->
    // flat panel is correct) and in-engine staged cutscenes (real 3D scene, often WIDE FOV -> the FOV
    // detector can't see them either). Archangel's mid-mission cutscenes are gm 8: AUTOMENU dropped them
    // to the head-locked flat quad AND the SFR replay skipped = the reported "lost VR" windows.
    // Discriminator (measured on ME3, Tuchanka): the P1 CalcSceneView heartbeat. A bik leaves the scene
    // dead (~0 calcviews/s); an in-engine cutscene builds views every frame. gm 8 + alive scene ->
    // publish mode 6 (Cinematic) instead, so the whole existing cine machinery (AUTOMENU escape, VRCINE
    // toggles, FOV detector) just works on it. gm 8 + dead scene stays 8 -> flat panel as before.
    static unsigned s_lastCalcSeq   = 0;
    static int      s_deadPresents  = 1000;                      // boot = dead until proven alive
    static int      s_aliveStreak   = 0;
    static bool     s_sceneAlive    = false;
    {
        // [ENGCINE] LATCHED both ways. A bare "any view in the last N presents" test FLAPPED at every
        // bik/scene boundary (measured 2026-07-31 04:49: gm 8 alternating dead/alive several times a
        // second), and each flap flipped presentation VR<->FLAT - visible as the mono blips at scene
        // ends. Require sustained evidence in BOTH directions so a boundary can't oscillate.
        const unsigned seq = ME2VR::CalcViewHook::GetP1CalcSeq();
        if (seq != s_lastCalcSeq) { s_lastCalcSeq = seq; s_deadPresents = 0; ++s_aliveStreak; }
        else { s_aliveStreak = 0; if (s_deadPresents < 1000) ++s_deadPresents; }
        if (s_aliveStreak >= 30) s_sceneAlive = true;            // ~0.5s of continuous view building
        else if (s_deadPresents >= 30) s_sceneAlive = false;     // ~0.5s with none = genuinely dead
    }
    const bool sceneAlive = s_sceneAlive;

    {
        constexpr float kEnterRad = 0.5585f;   // 32 deg -> below this = cinematic
        constexpr float kExitRad  = 0.5934f;   // 34 deg -> above this = gameplay
        const float gh = ME2VR::CalcViewHook::GetGameRawFovH();   // RAW (un-widened) so fill can't fool it
        static bool s_fovCine = false;         // hysteretic FOV cine detector (separate from the flat decision)
        // [CINEDEBOUNCE] entry requires the narrow FOV to PERSIST. The IsWeaponOut() guard below is
        // read a frame or two behind the camera at ADS onset, so aiming could flash 1-2 narrow frames
        // through the gap -> a momentary flat panel mid-combat (reproduced; matches the
        // "combat is 2D" report). Real cutscenes hold narrow FOV for seconds, an ADS transient doesn't, so a
        // few-frame debounce kills the flash without touching detection. EXIT stays immediate.
        static int s_narrowStreak = 0;
        // [COVERFLASH probe 2026-08-22] He reports the picture flashing to a small square in black
        // when entering cover with a pistol. That is the flat cine panel taking over for a few
        // frames, and this detector is the only thing that can do it mid-combat. The guard below is
        // IsWeaponOut(), which is itself a camera-mode-name match plus an 18-frame hold - so a
        // transient cover-entry camera that matches none of the names can drop the verdict, let the
        // narrow FOV through, and flip the panel. This prints exactly the values that decide it, on
        // CHANGE only, so one cover entry either convicts or clears the theory.
        if (ME2VR::Log::DiagnosticsOn())   // [COVERFLASH] kept, but off in play sessions
        {
            const bool wOut = ME2VR::EngineProbe::IsWeaponOut();
            char cm[64] = {}; int hold = 0; bool rawMatch = false;
            ME2VR::EngineProbe::GetCoverFlashState(cm, sizeof(cm), &hold, &rawMatch);
            static bool s_pw = false; static bool s_pc = false; static char s_pm[64] = {};
            const bool changed = (wOut != s_pw) || (s_fovCine != s_pc) || (std::strcmp(cm, s_pm) != 0);
            if (changed)
            {
                s_pw = wOut; s_pc = s_fovCine;
                strncpy_s(s_pm, cm, sizeof(s_pm) - 1);
                char b[288] = {};
                sprintf_s(b, "[COVERFLASH] cam=%s weaponOut=%d (rawNameMatch=%d hold=%d) fovCine=%d "
                             "rawFovH=%.1fdeg narrowStreak=%d enterFrames=%d",
                          cm[0] ? cm : "-", wOut ? 1 : 0, rawMatch ? 1 : 0, hold, s_fovCine ? 1 : 0,
                          gh * 57.29578f, s_narrowStreak, ME2VR::CalcViewHook::GetCineEnterFrames());
                ME2VR::Log::Line(b);
            }
        }
        if (ME2VR::EngineProbe::IsWeaponOut())
        {
            s_fovCine = false;   // ADS / sniper-zoom narrow the FOV but are GAMEPLAY -> never mono
            s_narrowStreak = 0;
        }
        else if (gh > 0.01f)
        {
            if (gh < kEnterRad)
            {
                if (++s_narrowStreak >= ME2VR::CalcViewHook::GetCineEnterFrames()) s_fovCine = true;
            }
            else
            {
                s_narrowStreak = 0;
                if (gh > kExitRad) s_fovCine = false;
            }
        }
        // [VRCINE] classify by the engine mode byte (5=Conversation, 6=Cinematic/cutscene). When the
        // matching experimental toggle is ON, render the cine in the active VR mode (don't force flat);
        // otherwise the FOV-detected cine takes the comfortable flat panel as before.
        const int gm = ME2VR::D3DCapture::GetAutoGameMode();
        // [CINELATCH] The mode byte is authoritative in the OTHER direction too: 5 Conversation and
        // 6 Cinematic ARE a cine, whatever the FOV says this frame. Conversation directors cut between
        // wide masters and close-ups, so the raw FOV crosses the 32/34deg thresholds repeatedly WITHIN
        // one scene; s_fovCine then flapped, VrCineActive flapped with it, and each flap dropped a few
        // frames to the head-locked mono quad with a mismatched FOV before recovering - the reported "the
        // image gets stuck to the headset, FOV looks weird, then it switches to a proper VR cutscene".
        // Measured 2026-07-31: [CLEARMAP] shows "gm=5 vrCine" and bare "gm=5" alternating mid-scene.
        // This is ME3's [CONVOLATCH] lesson (there the camera CLASS changed per shot, same flapping):
        // per-shot variation must never drive whole-scene presentation. Holding the mode byte also
        // makes entry instant, so [CINEDEBOUNCE]'s 8-frame delay only guards the gm-unknown path.
        if (gm == 5 || gm == 6) { s_fovCine = true; s_narrowStreak = 0; }
        // [GAMEPLAYVR] THE ENGINE MODE BYTE IS AUTHORITATIVE - a FOV heuristic may never hold GAMEPLAY
        // on the flat panel. Modes 0 Default / 1 PowerWheel / 2 WeaponWheel / 3 Command / 4 Vehicle are
        // all live gameplay; if the engine says the mod is in one of them, it is, whatever the FOV says.
        //
        // This is the Archangel bug (2026-07-31, measured): the cutscene ends, the engine goes
        // back to mode 0, but s_fovCine could not un-latch because EXIT needs the raw FOV back above
        // 34deg and gameplay's mono full view sits at ~35deg - a 1deg margin that the post-cutscene
        // camera does not always clear. Cinematic stayed true into gameplay, DrawDetour kept refusing
        // the pass-1 replay, and the submit kept presenting a "stereo" pair built from ONE render:
        //   [VRSTATE] gm 0 live alive -> FLAT cine panel
        //   [STEREODEAD] SFR presented but replay refused for 1871 frames (gate: cinematic)
        // 31 seconds of gameplay with no second eye - separation slid the world sideways instead of
        // adding depth, because there was no parallax to change. Clearing on the mode byte makes the
        // latch impossible to enter from gameplay AND impossible to keep once gameplay resumes.
        if (gm >= 0 && gm <= 4) { s_fovCine = false; s_narrowStreak = 0; }
        // [CONVOFP] First-person conversations imply VR presentation -- there is no sense relocating the
        // camera to Shepard's eyes and then showing the result on a flat panel. Deriving it here rather
        // than depending on the CineVrConvo toggle is what fixes "it doesn't work until the
        // checkbox is switched off and on": the toggle was only ever set by the checkbox HANDLER, so booting with
        // ConvoFirstPerson=1 in the ini left the prerequisite off and the feature inert.
        // [CINEFLAT] ...but "enabled" is not "armed": with ConvoFirstPerson=1 and CineVrConvo=0, a
        // conversation whose first-person staging never arms (multi-speaker scenes, failed reads) was
        // still forced into VR presentation, which renders the DIRECTOR's ~20-deg camera and declares
        // that FOV honestly -> a small centered window in a ~100-deg headset, black everywhere else
        // (the reported "white box" symptom). VR presentation is only earned by an ARMED
        // first-person conversation; sticky per conversation (gm stays 5) so a mid-scene disarm/rearm
        // can't flap the presentation between VR and the flat panel.
        // [CONVOFP-OFF] the latch has to distinguish two things that both look identical as "IsArmed()
        // just went false": a TRANSIENT read gap mid-scene (what the latch exists to ride out) versus
        // the user deliberately unticking the checkbox. Gating the reset on gm alone caught only the
        // first case -- turning the feature off mid-conversation left the scene stuck in VR presentation
        // (the director's camera, since ConvoFp is no longer relocating it) instead of dropping back to
        // the flat panel, because the latch itself never had a reason to clear until the scene ended.
        // GetEnabled() is the deliberate user signal; check it alongside gm so an explicit disable drops
        // the latch immediately, while a same-conversation transient gap still rides through untouched.
        static bool s_convoVrLatch = false;
        if (gm != 5 || !ME2VR::ConvoFp::GetEnabled()) s_convoVrLatch = false;
        else if (ME2VR::ConvoFp::IsArmed()) s_convoVrLatch = true;
        // [VRFAILOPEN] Decoder/loading activity now identifies real movies directly, so scene
        // liveness is diagnostic only. It may stall at transitions and must never demote a real
        // conversation/cutscene to the flat panel. Unknown/transitional modes also fail open to VR.
        const bool recognizedCine = gm == 5 || gm == 6;
        const bool renderCineInVr = (gm == 5 && (ME2VR::CalcViewHook::GetCineVrConvo() ||
                                                 s_convoVrLatch)) ||
                                    (gm == 6 && ME2VR::CalcViewHook::GetCineVrCutscene());
        ME2VR::CalcViewHook::SetVrCineActive(renderCineInVr && s_fovCine);
        ME2VR::CalcViewHook::SetCinematic(s_fovCine && recognizedCine && !renderCineInVr);

        // [VRSTATE] one line per CHANGE of the whole flat/VR decision, UNCAPPED. The old capped logs
        // (4 frame-mode lines, 20 GM7PAUSE lines) burned their budget at boot and went silent for the
        // exact mid-mission events the mod needed - the Archangel stereo-dead windows left zero log lines.
        // Transitions are rare (a few/minute), so uncapped transition logging keeps the shipped log
        // short AND guarantees the log can never again be silent while presentation changes.
        {
            static unsigned s_lastKey = 0xFFFFFFFFu;
            const bool flatNow = moviePlayback || ME2VR::D3DCapture::GetMenuMode();
            const bool cineNow = ME2VR::CalcViewHook::GetCinematic();
            const bool vrCine  = ME2VR::CalcViewHook::GetVrCineActive();
            const bool paused  = ME2VR::D3DCapture::GetAutoPaused();
            const unsigned key = (unsigned)(gmRaw & 0xFF) | ((unsigned)(gmEff & 0xFF) << 8) |
                                 (flatNow ? 1u<<16 : 0) | (cineNow ? 1u<<17 : 0) |
                                 (vrCine ? 1u<<18 : 0) | (paused ? 1u<<19 : 0) |
                                 (sceneAlive ? 1u<<20 : 0) | (loadMoviePlaying ? 1u<<21 : 0) | (binkFullscreen ? 1u<<22 : 0);
            if (key != s_lastKey)
            {
                s_lastKey = key;
                ME2VR::Log::Line(std::string("[VRSTATE] gm ") + std::to_string(gmRaw) +
                    (gmEff != gmRaw ? (" (eff " + std::to_string(gmEff) +
                        (loadMoviePlaying ? " LOADMOVIE)" : (binkFullscreen ? " BINK)" : " ENGCINE)"))) : std::string()) +
                    (paused ? " paused" : " live") + (sceneAlive ? " alive" : " dead") +
                    (binkFullscreen ? " BINK" : "") +
                    " -> " + (flatNow ? "FLAT menu panel" :
                              cineNow ? "FLAT cine panel" :
                              vrCine  ? "VR cine" : "VR"));
            }
        }
    }


    // [PRESENTATIONOWNER] Readiness can choose a fallback renderer, never a flat presentation.
    // Only verified movie/loading, menu/manual ownership, or an explicitly disabled VR cinematic
    // may select the head-locked quad. Everything else stays in an immersive projection.
    const bool flatPresentation = moviePlayback || ME2VR::D3DCapture::GetMenuMode() ||
                                  ME2VR::CalcViewHook::GetCinematic();
    const bool splitSourceFresh = g_framesSinceSplit < kSbsHoldFrames;
    const bool stereoMonoFallback = stereoMode && !splitSourceFresh && !flatPresentation;
    monoGameplay = (monoMode || stereoMonoFallback) && !flatPresentation;
    sbsFrame = !flatPresentation && (!stereoMode || splitSourceFresh);

    XrCompositionLayerQuad monoQuad = {};
    bool haveMono = false;

    if (fs.shouldRender && flatPresentation)
    {
        // Explicit flat owner: show the whole backbuffer to both eyes as one correctly-proportioned quad.
        EnsureMonoSwapchain();
        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
        if (g_monoSwap != nullptr && backbuffer != nullptr)
        {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
            if (XrSucceeded(g_fn.acquireSwapchainImage(g_monoSwap, &ai, &idx)))
            {
                XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
                if (XrSucceeded(g_fn.waitSwapchainImage(g_monoSwap, &wi)) && idx < g_monoImages.size())
                    g_ctx->CopyResource(g_monoImages[idx], backbuffer);
                XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
                g_fn.releaseSwapchainImage(g_monoSwap, &ri);

                const float aspect = (g_bbW != 0) ? static_cast<float>(g_swapH) / static_cast<float>(g_bbW) : 0.5625f;
                monoQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
                monoQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
                monoQuad.subImage.swapchain = g_monoSwap;
                monoQuad.subImage.imageRect.offset = { 0, 0 };
                monoQuad.subImage.imageRect.extent = { static_cast<int32_t>(g_bbW), static_cast<int32_t>(g_swapH) };
                monoQuad.subImage.imageArrayIndex = 0;
                monoQuad.pose = IdentityPose();
                const float mdist = g_monoQuadDistanceM.load(std::memory_order_relaxed);
                float mw          = g_monoQuadWidthM.load(std::memory_order_relaxed);
                // [CINEFLAT] cutscene/conversation screen = ME1's model: world-locked (head tracking;
                // Insert recenters), zoom slider on top. Menus stay head-locked exactly as before.
                if (ME2VR::CalcViewHook::GetCinematic())
                {
                    monoQuad.space = g_localSpace;
                    mw *= g_cineScreenZoom.load(std::memory_order_relaxed);
                }
                else
                {
                    monoQuad.space = g_viewSpace;
                }
                monoQuad.pose.position.z = -mdist;
                monoQuad.size.width = mw;
                monoQuad.size.height = mw * aspect;
                haveMono = true;
            }
        }
        if (backbuffer) backbuffer->Release();
        LogFrameMode(0, "frame mode = MONO (no split) -> full-screen quad to both eyes");
    }
    else if (aerMode && !fs.shouldRender)
    {
        // [AERGAP 2026-08-21] The runtime said not to render this frame - but the GAME rendered an
        // AER eye anyway, because the engine's frame loop knows nothing about xrWaitFrame. Every AER
        // path below is gated on shouldRender, so on these presents the stamp was never consumed and
        // SetAerRenderEye never advanced: the engine then rendered the SAME eye again, and the other
        // eye sat unrefreshed for two intervals instead of one. That is a real stereo defect (one eye
        // twice as stale as the other) and it is exactly what [AERHZ] has been reporting as
        // seqGapBad=1..5 in EVERY window, including windows with late=0 where pacing was clean.
        // So: consume the handshake and refresh history here, with no swapchain work at all (nothing
        // is being presented this frame). Cheap, and it keeps the eye alternation locked to the
        // engine's renders rather than to the compositor's schedule.
        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
        if (backbuffer)
        {
            XrViewLocateInfo vli = {};
            vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
            vli.viewConfigurationType = kStereo;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = g_localSpace;
            XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
            uint32_t viewCount = 0;
            XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
            XrPosef headPose = IdentityPose();
            if (XrSucceeded(g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views)) && viewCount == 2)
            {
                headPose = views[0].pose;
                headPose.orientation = RemoveRoll(views[0].pose.orientation);
                headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            }
            SubmitAerFrame(backbuffer, headPose, false);   // handshake + history only
            backbuffer->Release();
        }
    }
    else if (fs.shouldRender && aerMode)
    {
        // AER: calcview renders ONE laterally-offset eye per present (it arms g_aerRenderEye + stamps which
        // eye landed). The mod captures that into a 2-slot history bank and submit BOTH slots (one fresh, one ~1
        // frame old) under ONE shared latched head pose -- the stereo disparity lives in the PIXELS, never
        // in the pose tag. That shared pose is what stops AER from ghosting when you turn your head.
        static bool s_autoRecenteredAer = false;
        if (!s_autoRecenteredAer || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecenteredAer = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);

        // Drive free head-look from the located (recentered) head orientation.
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer)
        {
            // FOV-FILL (gameplay): widen the wide gameplay view to fill the headset (same target as stereo).
            {
                const float aspect = (g_swapH != 0) ? static_cast<float>(g_swapW) / static_cast<float>(g_swapH) : 0.889f;
                ApplyFovFill(views, aspect);
            }

            // Game's rendered half-FOV (radians) -> declared per eye (must EXACTLY match what was rendered).
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            // This frame's head pose (roll removed, centered). Position = head CENTER (average of both eye
            // positions), matching ME1's headPos - not eye 0 (which would shift the world half an IPD left).
            // Computed BEFORE submit so the freshly-captured eye is tagged with it.
            XrPosef headPose = views[0].pose;
            headPose.orientation = RemoveRoll(views[0].pose.orientation);
            headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;

            // Capture the freshly-rendered offset eye into history (tagged with headPose) + fill BOTH full-size
            // AER eye swapchains (full-frame -> whole eye = the un-squash). Allocates g_aerSwap/g_aerHist first use.
            SubmitAerFrame(backbuffer, headPose, true);

            if (g_aerSwap[0] != nullptr && g_aerSwap[1] != nullptr)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    XrFovf fov = views[eye].fov;
                    if (gh > 0.01f && gv > 0.01f) { fov.angleLeft = -gh; fov.angleRight = gh; fov.angleUp = gv; fov.angleDown = -gv; }

                    // ANTI-GHOST (the sacred pose-tag): submit each eye at the pose it was RENDERED at (per-slot),
                    // NOT the shared current pose. Fresh eye's stored pose == headPose (just captured); the STALE
                    // eye keeps its OLDER capture pose, so the compositor reprojects it to align -> no turn ghost.
                    projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                    projViews[eye].pose = g_aerHistPoseValid[eye] ? g_aerHistPose[eye] : headPose;
                    projViews[eye].fov = fov;
                    projViews[eye].subImage.swapchain = g_aerSwap[eye];
                    projViews[eye].subImage.imageRect.offset = { 0, 0 };
                    projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                    projViews[eye].subImage.imageArrayIndex = 0;
                }
                layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
                layer.space = g_localSpace;
                layer.viewCount = 2;
                layer.views = projViews;
                rendered = true;
                // [AERBLACK] stash this good pose/FOV so a hiccup frame can hold it instead of going black
                g_aerLastHeadPose = headPose;
                g_aerLastFovL = projViews[0].fov;
                g_aerLastFovR = projViews[1].fov;
                g_aerHaveLast = true;
                if (!g_submitLogged.exchange(true))
                    XLog("=== AER: first alternate-eye projection submitted (gameFovH=" + std::to_string(gh) + " rad) ===");
                LogFrameMode(1, "frame mode = AER (alternate-eye) -> both eyes, one shared pose");
            }
        }
        if (!rendered)
        {
            // [AERBLACK] NEVER BLACK. locateViews hiccupped, viewCount came back != 2, or the swapchain
            // handed the mod no backbuffer this present. Without this the layer count is 0 and both eyes go
            // hard black for one frame - the blink. The AER swapchains still hold the last good pair
            // (nothing overwrote them: [AERBLINK] only copies on a fresh render), so re-present them
            // under the last known-good pose/FOV and let the compositor reproject. Per-slot history
            // poses are kept where valid, so the anti-ghost tagging survives the hiccup too.
            if (g_aerHaveLast && g_aerSwap[0] != nullptr && g_aerSwap[1] != nullptr)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                    projViews[eye].pose = g_aerHistPoseValid[eye] ? g_aerHistPose[eye] : g_aerLastHeadPose;
                    projViews[eye].fov = (eye == 0) ? g_aerLastFovL : g_aerLastFovR;
                    projViews[eye].subImage.swapchain = g_aerSwap[eye];
                    projViews[eye].subImage.imageRect.offset = { 0, 0 };
                    projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                    projViews[eye].subImage.imageArrayIndex = 0;
                }
                layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
                layer.space = g_localSpace;
                layer.viewCount = 2;
                layer.views = projViews;
                rendered = true;
                ++g_aerFallbackFrames;
            }
            else ++g_aerBlackFrames;   // no prior good frame yet (true startup) -> unavoidable
        }
        // [AERSUB] confirm-the-fix line, same shape as [DIBRSUB]: after this, black=0 in steady
        // gameplay. fallback>0 = frames rescued that WOULD have been black blinks before.
        {
            const ULONGLONG nowMs = GetTickCount64();
            if (g_aerSubWindowMs == 0) g_aerSubWindowMs = nowMs;
            if (nowMs - g_aerSubWindowMs >= 2000)
            {
                char sb[160] = {};
                sprintf_s(sb, "[AERSUB] fallback=%u black=%u (frames rescued from a 0-layer black frame)",
                          g_aerFallbackFrames, g_aerBlackFrames);
                XLog(sb);
                g_aerSubWindowMs = nowMs; g_aerFallbackFrames = 0; g_aerBlackFrames = 0;
            }
        }
        if (backbuffer) backbuffer->Release();
    }
    else if (fs.shouldRender && sfrMode)
    {
        // SFR (same-frame stereo): the frame was rendered TWICE this present (DrawDetour replay). Pass 0
        // (-halfEye = LEFT) was captured mid-present at the replay's depth clear; pass 1 (+halfEye = RIGHT)
        // is the backbuffer now. Submit pass0->left, pass1->right (full-frame). BOTH eyes are from the
        // SAME instant, so ONE shared head pose (no AER staleness). Reuses the full-size AER eye swapchains.
        static bool s_autoRecenteredSfr = false;
        if (!s_autoRecenteredSfr || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecenteredSfr = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer)
        {
            // FOV-FILL (same as AER/stereo): widen the gameplay view to fill the headset.
            {
                const float aspect = (g_swapH != 0) ? static_cast<float>(g_swapW) / static_cast<float>(g_swapH) : 0.889f;
                ApplyFovFill(views, aspect);
            }
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            // [POSETAG] Bug 1/2 fix: tag the captured backbuffer with the pose that RENDERED it (the
            // smoothed head-look armed ~poseTagDelayFrames presents ago, from PoseTagPush in DriveHeadLook)
            // instead of the current located pose. The compositor then reprojects from the true render
            // pose = no one-frame drag/shake on head turns. Delay 0 falls back to ~the current smoothed pose.
            XrPosef headPose = g_delayedTagPose;
            // [VRCINE] head-tracking OFF: render was parked (no head rotation), so anchor the stereo pair
            // at the CURRENT head each frame -> a head-locked 3D picture that follows you (doesn't drift).
            if (ME2VR::CalcViewHook::GetVrCineActive() && !ME2VR::CalcViewHook::GetCineVrHeadTracking())
            {
                headPose.orientation = RemoveRoll(views[0].pose.orientation);
                headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            }

            if (EnsureAerSwapchains())
            {
                ID3D11Texture2D* pass0 = ME2VR::D3DCapture::GetSfrPass0Texture();   // left (-halfEye), mid-present
                const bool swap = ME2VR::CalcViewHook::GetSwapEyes();
                // [STALEEYE] Only show pass 0 if it was captured THIS frame. During VR conversations the
                // engine intermittently declines to render pass 1 ([STEREODEAD] NO-OP bursts); pass 1 then
                // never starts, the anchor never fires, and the old code happily submitted a pass-0 texture
                // from several frames ago as the left eye. A frozen left eye beside a live right eye reads
                // as "head tracking dies, then comes back" (2026-07-31). Falling back to the live
                // backbuffer costs stereo for those frames - which is honest, since no second eye exists -
                // but both eyes stay current, so head tracking never stalls.
                static unsigned long long s_lastCapSeq = 0;
                const unsigned long long capSeq = ME2VR::D3DCapture::GetSfrPass0CaptureSeq();
                const bool freshPass0 = (capSeq != s_lastCapSeq);
                s_lastCapSeq = capSeq;
                ID3D11Texture2D* leftSrc  = (pass0 != nullptr && freshPass0) ? pass0 : backbuffer;  // fall back to mono until 1st capture
                ID3D11Texture2D* rightSrc = backbuffer;                                // right (+halfEye) = pass 1 (now)
                // [SFRCONV DECOUPLE] convergence at SUBMIT (world+UI already composited). conv/2 in UV ->
                // pixels; sign bolted to the PASS (ME1 parity): pass0/left content shifts right (+), pass1/
                // right content shifts left (-). Swap-eyes flips the destination routing, so it inverts the
                // convergence plane exactly as ME2's swap toggle does - no swap term here. conv=0 -> plain
                // copy (no shift), so the UI stops doubling and gameplay is untouched when convergence is off.
                const float conv = ME2VR::CalcViewHook::GetSfrConvergence();
                const int convPx = static_cast<int>(lroundf(conv * 0.5f * static_cast<float>(g_aerSwapW)));
                CopyTextureToEyeFullFrame(swap ? 1 : 0, leftSrc,  +convPx);
                CopyTextureToEyeFullFrame(swap ? 0 : 1, rightSrc, -convPx);
                {
                    static float s_lastLoggedConv = -999.0f;
                    if (conv != s_lastLoggedConv)   // log only when the slider actually changes
                    {
                        s_lastLoggedConv = conv;
                        XLog("[SFRCONV] submit conv=" + std::to_string(conv) + " -> shiftPx=" +
                             std::to_string(convPx) + " (eyeW=" + std::to_string(g_aerSwapW) +
                             ", leftEye content right, rightEye content left)");
                    }
                }

                // [FREEZETAG] A projection layer only head-tracks if pose tag and PIXELS agree. During
                // conversation shot changes the engine skips rendering for ~20-frame bursts ([STEREODEAD]
                // NO-OP, one burst per camera cut); the image freezes, but this tag kept following the
                // live head - so the compositor saw "already aligned", did zero reprojection, and the
                // stale shot rode the headset ("tracking vanishes, comes back on the next cut"). Standard
                // missed-frame handling is the opposite: freeze the tag (and the declared FOV - the new
                // shot's FOV with the old shot's pixels is the "FOV looks weird" report) WITH the image,
                // and the runtime reprojects it as the head moves - tracking survives the stale frame.
                // Skipped for the deliberate head-locked mode (VrCine + head-tracking off), where the
                // current-head anchor IS the intent.
                float ghUse = gh, gvUse = gv;
                {
                    static XrPosef s_freshPose = {};
                    static float   s_freshGh = 0.0f, s_freshGv = 0.0f;
                    static bool    s_freshInit = false;
                    const bool lockedCine = ME2VR::CalcViewHook::GetVrCineActive() &&
                                            !ME2VR::CalcViewHook::GetCineVrHeadTracking();
                    if (freshPass0 || !s_freshInit || lockedCine)
                    {
                        s_freshPose = headPose; s_freshGh = gh; s_freshGv = gv; s_freshInit = true;
                    }
                    else
                    {
                        headPose = s_freshPose; ghUse = s_freshGh; gvUse = s_freshGv;
                    }
                }

                if (g_aerSwap[0] != nullptr && g_aerSwap[1] != nullptr)
                {
                    for (int eye = 0; eye < 2; ++eye)
                    {
                        XrFovf fov = views[eye].fov;
                        if (ghUse > 0.01f && gvUse > 0.01f) { fov.angleLeft = -ghUse; fov.angleRight = ghUse; fov.angleUp = gvUse; fov.angleDown = -gvUse; }
                        projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                        projViews[eye].pose = headPose;   // same-frame: ONE shared pose (both eyes fresh)
                        projViews[eye].fov = fov;
                        projViews[eye].subImage.swapchain = g_aerSwap[eye];
                        projViews[eye].subImage.imageRect.offset = { 0, 0 };
                        projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                        projViews[eye].subImage.imageArrayIndex = 0;
                    }
                    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
                    layer.space = g_localSpace;
                    layer.viewCount = 2;
                    layer.views = projViews;
                    rendered = true;
                    if (!g_submitLogged.exchange(true))
                        XLog("=== SFR: first same-frame stereo pair submitted (pass0=L pass1=R gameFovH=" + std::to_string(gh) + ") ===");
                    LogFrameMode(2, "frame mode = SFR (same-frame) -> pass0->L pass1->R, one shared pose");
                    // [STEREODEAD] invariant watchdog: a stereo pair just went to the compositor, so a
                    // fresh pass-1 replay MUST be landing. If the replay-skip streak is climbing instead,
                    // both eyes carry the same image - the exact silent failure behind "separation moves
                    // the scene sideways instead of adding depth". Log the episode enter/exit, uncapped,
                    // with the gate that refused the replay, so this bug CLASS can never be silent again.
                    {
                        static bool s_dead = false;
                        unsigned streak = 0, reasons = 0;
                        ME2VR::CalcViewHook::GetSfrSkipInfo(&streak, &reasons);
                        // [NOOPREPLAY] TWO ways the second eye can be missing, and the second one used
                        // to read as perfectly healthy: (a) the replay was REFUSED by a gate, (b) the
                        // replay RAN but the engine early-out of Draw so no new scene view was built -
                        // replays=600/600 with replayAvg=0.0ms. Both mean identical eyes; both are the
                        // same user-visible bug ("separation slides the world instead of adding depth").
                        const unsigned noop = ME2VR::CalcViewHook::GetSfrNoopReplays();
                        const bool dead = (streak > 20) || (noop > 20);   // ~1/3s either way
                        if (dead != s_dead)
                        {
                            s_dead = dead;
                            if (dead && streak > 20)
                                XLog(std::string("[STEREODEAD] SFR presented but replay REFUSED for ") +
                                     std::to_string(streak) + " frames (gate:" +
                                     ((reasons & 1u) ? " menuMode" : "") +
                                     ((reasons & 2u) ? " cinematic" : "") +
                                     ((reasons == 0u) ? " none?" : "") + ") -> both eyes same image");
                            else if (dead)
                            {
                                // [NOOPRETRY] pass0Noops>0 here = the WHOLE renderer is suppressed for the
                                // burst (streaming/priming class); pass0 at normal cost = a gate on the
                                // second Draw only. retry=N/M shows whether the same-frame retry rescues it.
                                long long p0us = 0; unsigned p0noops = 0, rAtt = 0, rOk = 0;
                                ME2VR::CalcViewHook::GetSfrNoopDiag(&p0us, &p0noops, &rAtt, &rOk);
                                XLog(std::string("[STEREODEAD] SFR presented but replay is a NO-OP for ") +
                                     std::to_string(noop) + " frames (Draw returned in <100us = rendered "
                                     "nothing) gm=" + std::to_string(ME2VR::D3DCapture::GetAutoGameMode()) +
                                     (ME2VR::CalcViewHook::GetVrCineActive() ? " vrCine" : "") +
                                     " pass0Last=" + std::to_string(p0us) + "us pass0Noops=" +
                                     std::to_string(p0noops) + " retriesOk=" + std::to_string(rOk) +
                                     "/" + std::to_string(rAtt) + " -> both eyes same image");
                            }
                            else
                                XLog("[STEREODEAD] recovered - second eye rendering again");
                        }
                    }
                }
            }
        }
        if (backbuffer) backbuffer->Release();
    }
    else if (fs.shouldRender && monoGameplay)
    {
        // Mono as an actual gameplay mode: a real world-space projection layer, both eyes given the
        // IDENTICAL backbuffer (there is no second eye to synthesize -- that is the whole point of
        // Mono). Modeled directly on the DIBR branch below, which proves this exact recipe already
        // works: DriveHeadLook so the HMD orientation actually reaches the game camera, FOV-fill widened
        // to the headset, and XR_TYPE_COMPOSITION_LAYER_PROJECTION (not a quad) so head-look is visible
        // instead of being cancelled by a screen that pans with your head.
        static bool s_autoRecenteredMono = false;
        if (!s_autoRecenteredMono || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecenteredMono = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer && EnsureAerSwapchains())
        {
            {
                const float aspect = (g_swapH != 0) ? static_cast<float>(g_swapW) / static_cast<float>(g_swapH) : 0.889f;
                ApplyFovFill(views, aspect);
            }

            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            CopyTextureToEyeFullFrame(0, backbuffer);
            CopyTextureToEyeFullFrame(1, backbuffer);

            XrPosef headPose = views[0].pose;
            headPose.orientation = RemoveRoll(views[0].pose.orientation);
            headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;

            for (int eye = 0; eye < 2; ++eye)
            {
                XrFovf fov = views[eye].fov;
                if (gh > 0.01f && gv > 0.01f) { fov.angleLeft = -gh; fov.angleRight = gh; fov.angleUp = gv; fov.angleDown = -gv; }
                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                projViews[eye].pose = headPose;
                projViews[eye].fov = fov;
                projViews[eye].subImage.swapchain = g_aerSwap[eye];
                projViews[eye].subImage.imageRect.offset = { 0, 0 };
                projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                projViews[eye].subImage.imageArrayIndex = 0;
            }
            layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            layer.space = g_localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            rendered = true;
            LogFrameMode(3, "frame mode = Mono (gameplay) -> both eyes, same frame");
        }
        if (backbuffer) backbuffer->Release();
    }
    else if (fs.shouldRender && dibrMode)
    {
        // DIBR: left eye = the real mono frame, right eye = the depth-warped synthesis of that SAME frame.
        // NOTE: this branch owns EVERY DIBR frame (not gated on depth-ready) so DIBR can never fall through to
        // the Stereo SBS split, which would mis-halve the mono frame (= the "squished/crossed" bug). Until depth
        // is ready GetDibrRightEye returns null -> both eyes get the backbuffer = clean MONO, never black/crossed.
        // Both eyes are from one moment -> ONE shared current pose (no staleness, no per-slot tag); the whole
        // stereo disparity lives in the warped pixels. Reuses the full-size AER eye swapchains + the full-frame
        // copy (never the aspect-fit copy, or the DIBR eye squashes vertically).
        static bool s_autoRecenteredDibr = false;
        if (!s_autoRecenteredDibr || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecenteredDibr = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer && EnsureAerSwapchains())
        {
            // FOV-FILL: widen the game view to fill the headset (identical to AER/stereo). ApplyFov (calcview
            // DIBR branch) widens the render to this target + publishes it as the submit FOV below.
            {
                const float aspect = (g_swapH != 0) ? static_cast<float>(g_swapW) / static_cast<float>(g_swapH) : 0.889f;
                ApplyFovFill(views, aspect);
            }

            // Game's rendered (now widened) half-FOV -> declared per eye (must EXACTLY match what was rendered).
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            // left = the real frame, right = the synth (null -> backbuffer, never black). No swap.
            ID3D11Texture2D* right = ME2VR::D3DCapture::GetDibrRightEye(backbuffer);
            CopyTextureToEyeFullFrame(0, backbuffer);
            CopyTextureToEyeFullFrame(1, right != nullptr ? right : backbuffer);

            // ONE shared current pose for both eyes (same-frame stereo; roll removed, centered position).
            XrPosef headPose = views[0].pose;
            headPose.orientation = RemoveRoll(views[0].pose.orientation);
            headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;

            for (int eye = 0; eye < 2; ++eye)
            {
                XrFovf fov = views[eye].fov;
                if (gh > 0.01f && gv > 0.01f) { fov.angleLeft = -gh; fov.angleRight = gh; fov.angleUp = gv; fov.angleDown = -gv; }
                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                projViews[eye].pose = headPose;
                projViews[eye].fov = fov;
                projViews[eye].subImage.swapchain = g_aerSwap[eye];
                projViews[eye].subImage.imageRect.offset = { 0, 0 };
                projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                projViews[eye].subImage.imageArrayIndex = 0;
            }
            layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            layer.space = g_localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            rendered = true;
            // Stash this good pose/FOV so a later hiccup frame can hold it instead of going black.
            g_dibrLastHeadPose = headPose;
            g_dibrLastFovL = projViews[0].fov;
            g_dibrLastFovR = projViews[1].fov;
            g_dibrHaveLast = true;
            ++g_dibrFullFrames;
            LogFrameMode(4, "frame mode = DIBR (depth warp) -> left real, right synth");
        }
        else if (backbuffer && EnsureAerSwapchains() && g_dibrHaveLast)
        {
            // NEVER BLACK (ME1 parity): the located-stereo guard failed this frame (locateViews hiccup /
            // transient viewCount!=2). DIBR has no mono-quad fallback, so without this the layer count would
            // hit 0 -> endFrame with 0 layers -> a hard black frame on BOTH eyes = the "flickers badly" flash.
            // Hold the last good pose/FOV and refresh both eyes to the current backbuffer (mono).
            CopyTextureToEyeFullFrame(0, backbuffer);
            CopyTextureToEyeFullFrame(1, backbuffer);
            for (int eye = 0; eye < 2; ++eye)
            {
                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                projViews[eye].pose = g_dibrLastHeadPose;
                projViews[eye].fov = (eye == 0) ? g_dibrLastFovL : g_dibrLastFovR;
                projViews[eye].subImage.swapchain = g_aerSwap[eye];
                projViews[eye].subImage.imageRect.offset = { 0, 0 };
                projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_aerSwapW), static_cast<int32_t>(g_aerSwapH) };
                projViews[eye].subImage.imageArrayIndex = 0;
            }
            layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            layer.space = g_localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            rendered = true;
            ++g_dibrFallbackFrames;
        }
        else
        {
            ++g_dibrBlackFrames;   // no backbuffer AND no prior good frame (true startup only) -> unavoidable
        }
        // [DIBRSUB] confirm-the-fix line: after the fix, black=0 in steady gameplay; any flicker left is NOT
        // the 0-layer path. fallback>0 = frames the mod rescued that ME1-parity would have blacked.
        {
            const ULONGLONG nowMs = GetTickCount64();
            if (g_dibrSubStatMs == 0) g_dibrSubStatMs = nowMs;
            if (nowMs - g_dibrSubStatMs >= 2000 && g_dibrSubEmitted < 30)
            {
                char line[160] = {};
                sprintf_s(line, "[DIBRSUB] full=%u fallback=%u black=%u (per ~2s; black must be 0)",
                          g_dibrFullFrames, g_dibrFallbackFrames, g_dibrBlackFrames);
                XLog(line);
                ++g_dibrSubEmitted;
                g_dibrSubStatMs = nowMs; g_dibrFullFrames = 0; g_dibrFallbackFrames = 0; g_dibrBlackFrames = 0;
            }
        }
        if (backbuffer) backbuffer->Release();
    }
    else if (fs.shouldRender)
    {
        // Recenter the app space (re-origin to the current head) once at startup or on request,
        // BEFORE locating - so the located head AND the submitted pose share the recentered frame.
        static bool s_autoRecentered = false;
        if (!s_autoRecentered || g_recenterRequested.exchange(false, std::memory_order_relaxed))
        {
            RecenterAppSpace(fs.predictedDisplayTime);
            s_autoRecentered = true;
        }

        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = kStereo;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_localSpace;
        XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t viewCount = 0;
        XrView views[2] = {}; views[0].type = views[1].type = XR_TYPE_VIEW_VALUE;
        const XrResult lr = g_fn.locateViews(g_session, &vli, &vs, 2, &viewCount, views);

        // Drive free head-look from the located (recentered) head orientation.
        if (XrSucceeded(lr) && viewCount == 2) DriveHeadLook(views[0]);

        ID3D11Texture2D* backbuffer = nullptr;
        IDXGISwapChain* sc = ME2VR::D3DCapture::GetGameSwapChain();
        if (sc) sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));

        if (XrSucceeded(lr) && viewCount == 2 && backbuffer)
        {
            // FOV-FILL (gameplay): widen the wide gameplay view to fill the headset (like ME3). Target =
            // headset vertical, horizontal aspect-corrected (undistorted). calcview's ApplyFov only
            // widens WIDE views (>34deg), so narrow cutscenes are excluded and still go mono.
            {
                const float aspect = (g_swapH != 0) ? static_cast<float>(g_swapW) / static_cast<float>(g_swapH) : 0.889f;
                ApplyFovFill(views, aspect);
            }

            // Game's rendered half-FOV (radians) for the per-eye submit (matches what was rendered).
            const float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
            const float gv = ME2VR::CalcViewHook::GetGameHalfFovV();

            for (int eye = 0; eye < 2; ++eye)
            {
                CopyHalfToEye(eye, backbuffer, (eye == 0) ? 0u : g_swapW);   // left half -> eye0, right half -> eye1

                XrFovf fov = views[eye].fov;
                if (gh > 0.01f && gv > 0.01f) { fov.angleLeft = -gh; fov.angleRight = gh; fov.angleUp = gv; fov.angleDown = -gv; }

                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                projViews[eye].pose = views[eye].pose;
                projViews[eye].pose.orientation = RemoveRoll(views[eye].pose.orientation);  // never tilt with head roll
                projViews[eye].fov = fov;
                projViews[eye].subImage.swapchain = g_swap[eye];
                projViews[eye].subImage.imageRect.offset = { 0, 0 };
                projViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(g_swapW), static_cast<int32_t>(g_swapH) };
                projViews[eye].subImage.imageArrayIndex = 0;
            }
            layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            layer.space = g_localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            rendered = true;
            if (!g_submitLogged.exchange(true))
                XLog("=== B2b: first STEREO projection submitted (gameFovH=" + std::to_string(gh) + " rad) ===");
            LogFrameMode(5, "frame mode = SBS (split ran) -> per-eye projection");
        }
        if (backbuffer) backbuffer->Release();
    }

    // Insert menu quad: the ImGui menu texture, head-locked, alpha-blended over the world.
    XrCompositionLayerQuad uiQuad = {};
    bool haveUi = false;
    ID3D11Texture2D* uiTex = ME2VR::Menu::RenderFrame();   // non-null only while the menu is open
    if (uiTex != nullptr)
    {
        D3D11_TEXTURE2D_DESC mdesc = {};
        uiTex->GetDesc(&mdesc);
        EnsureUiSwapchain(mdesc.Width, mdesc.Height);
        if (g_uiSwap != nullptr)
        {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
            if (XrSucceeded(g_fn.acquireSwapchainImage(g_uiSwap, &ai, &idx)))
            {
                XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
                if (XrSucceeded(g_fn.waitSwapchainImage(g_uiSwap, &wi)) && idx < g_uiImages.size())
                    g_ctx->CopyResource(g_uiImages[idx], uiTex);
                XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
                g_fn.releaseSwapchainImage(g_uiSwap, &ri);

                // [COMFORT] The settings panel itself, placed by the Comfort tab (ME1 parity). Was
                // hardcoded 1.4 m wide at 1.5 m with no way to move it, which is unusable if the menu
                // lands somewhere your headset can't comfortably show. Aspect stays exact (no FOV stretch).
                const float dist  = g_menuQuadDistanceM.load(std::memory_order_relaxed);
                const float width = g_menuQuadWidthM.load(std::memory_order_relaxed);
                const float aspect = (g_uiW != 0) ? static_cast<float>(g_uiH) / static_cast<float>(g_uiW) : 0.8f;
                uiQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
                uiQuad.layerFlags = kLayerSrcAlpha;
                uiQuad.space = g_viewSpace;                      // head-locked
                uiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
                uiQuad.subImage.swapchain = g_uiSwap;
                uiQuad.subImage.imageRect.offset = { 0, 0 };
                uiQuad.subImage.imageRect.extent = { static_cast<int32_t>(g_uiW), static_cast<int32_t>(g_uiH) };
                uiQuad.subImage.imageArrayIndex = 0;
                uiQuad.pose = IdentityPose();
                uiQuad.pose.position.x = g_menuQuadOffXM.load(std::memory_order_relaxed);
                uiQuad.pose.position.y = g_menuQuadOffYM.load(std::memory_order_relaxed);
                uiQuad.pose.position.z = -dist;
                uiQuad.size.width = width;
                uiQuad.size.height = width * aspect;
                haveUi = true;
                if (!g_submitLogged.load(std::memory_order_acquire)) {}   // (first-stereo log already fired)
            }
        }
    }

    // Game-UI overlay (HUD + menus): one flat layer over the always-stereo world. Sized to the game's
    // FOV so HUD elements land where the game drew them; head-locked so it follows your view.
    XrCompositionLayerQuad gameUiQuad = {};
    bool haveGameUi = false;
    if (ME2VR::D3DCapture::GetUiOverlayActive())
    {
        ID3D11Texture2D* ov = ME2VR::D3DCapture::GetUiOverlayTexture();
        if (ov != nullptr)
        {
            D3D11_TEXTURE2D_DESC od = {}; ov->GetDesc(&od);
            EnsureGameUiSwapchain(od.Width, od.Height);
            if (g_gameUiSwap != nullptr)
            {
                uint32_t gidx = 0;
                XrSwapchainImageAcquireInfo gai = {}; gai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
                if (XrSucceeded(g_fn.acquireSwapchainImage(g_gameUiSwap, &gai, &gidx)))
                {
                    XrSwapchainImageWaitInfo gwi = {}; gwi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; gwi.timeout = XR_INFINITE_DURATION_VALUE;
                    if (XrSucceeded(g_fn.waitSwapchainImage(g_gameUiSwap, &gwi)) && gidx < g_gameUiImages.size())
                        g_ctx->CopyResource(g_gameUiImages[gidx], ov);
                    XrSwapchainImageReleaseInfo gri = {}; gri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
                    g_fn.releaseSwapchainImage(g_gameUiSwap, &gri);

                    float gh = ME2VR::CalcViewHook::GetGameHalfFovH();
                    float gv = ME2VR::CalcViewHook::GetGameHalfFovV();
                    if (gh < 0.05f) gh = 0.60f;
                    if (gv < 0.05f) gv = 0.45f;
                    const float gdist = 1.2f;
                    gameUiQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
                    gameUiQuad.layerFlags = kLayerSrcAlpha;
                    gameUiQuad.space = g_viewSpace;                  // head-locked
                    gameUiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
                    gameUiQuad.subImage.swapchain = g_gameUiSwap;
                    gameUiQuad.subImage.imageRect.offset = { 0, 0 };
                    gameUiQuad.subImage.imageRect.extent = { static_cast<int32_t>(g_gameUiW), static_cast<int32_t>(g_gameUiH) };
                    gameUiQuad.subImage.imageArrayIndex = 0;
                    gameUiQuad.pose = IdentityPose();
                    gameUiQuad.pose.position.z = -gdist;
                    gameUiQuad.size.width = 2.0f * gdist * tanf(gh);
                    gameUiQuad.size.height = 2.0f * gdist * tanf(gv);
                    // [GAMEUIQ] telemetry (pure logging): the quad-vs-content aspect pair. The quad
                    // vertically stretches its 16:9 texture by (tan(gv)/tan(gh))*(texW/texH); the
                    // [UIRATIO] pre-squeeze (autoY/autoX) must cancel it exactly or all UI warps.
                    {
                        static float lgh = -1.0f, lgv = -1.0f;
                        if (fabsf(gh - lgh) > 0.01f || fabsf(gv - lgv) > 0.01f)
                        {
                            lgh = gh; lgv = gv;
                            XLog("[GAMEUIQ] gh=" + std::to_string(gh) + " gv=" + std::to_string(gv) +
                                 " tex=" + std::to_string(g_gameUiW) + "x" + std::to_string(g_gameUiH) +
                                 " quad=" + std::to_string(gameUiQuad.size.width) + "x" +
                                 std::to_string(gameUiQuad.size.height));
                        }
                    }
                    haveGameUi = true;
                }
            }
        }
    }

    // [LINKFOV] Runtime FOV reconciliation, applied to whichever mode filled projViews (SFR / AER /
    // DIBR / mono) - one place instead of four. Crop each eye's submitted rect to the intersection of
    // what the mod rendered (the declared window D) and the runtime's own per-eye frustum T, and declare
    // exactly that intersection. Meta's compositor assumes T regardless of what the mod declares, so once
    // declared == T-window == the submitted pixels, its assumption and the mod's agree and the doubling
    // goes away. SteamVR outright voids a view declared wider than T, and the same crop fixes it.
    // Pure rect math in tan space: no extra rendering, and "declared FOV must exactly match what was
    // rendered" stays true because the RECT shrinks along with the angles. When D is already narrower
    // than T (menus, flat cine at game FOV) the intersection IS D and this is a no-op. Any other
    // runtime (VDXR etc.) never enters here - path bit-identical to before.
    if (rendered && ((g_isOculusRuntime && g_questFovMatch.load(std::memory_order_relaxed)) || g_isSteamVrRuntime))
    {
        XrView rtViews[2] = {};
        rtViews[0].type = XR_TYPE_VIEW_VALUE; rtViews[1].type = XR_TYPE_VIEW_VALUE;
        XrViewState rvs = {}; rvs.type = XR_TYPE_VIEW_STATE_VALUE;
        XrViewLocateInfo rli = {};
        rli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        rli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
        rli.displayTime = fs.predictedDisplayTime;
        rli.space = g_localSpace;
        uint32_t rc = 0;
        if (XrSucceeded(g_fn.locateViews(g_session, &rli, &rvs, 2, &rc, rtViews)) && rc >= 2)
        {
            g_lastRtFov[0] = rtViews[0].fov; g_lastRtFov[1] = rtViews[1].fov; g_lastRtFovValid = true;
            for (int e = 0; e < 2; ++e)
            {
                const XrFovf d = projViews[e].fov;      // what the mod declared/rendered
                const XrFovf& t = rtViews[e].fov;       // the runtime's own frustum
                const float aL = (d.angleLeft  > t.angleLeft)  ? d.angleLeft  : t.angleLeft;
                const float aR = (d.angleRight < t.angleRight) ? d.angleRight : t.angleRight;
                const float aU = (d.angleUp    < t.angleUp)    ? d.angleUp    : t.angleUp;
                const float aD = (d.angleDown  > t.angleDown)  ? d.angleDown  : t.angleDown;
                const float tanDL = tanf(d.angleLeft), tanDR = tanf(d.angleRight);
                const float tanDU = tanf(d.angleUp),   tanDD = tanf(d.angleDown);
                const float hSpan = tanDR - tanDL, vSpan = tanDU - tanDD;
                if (aR - aL <= 0.05f || aU - aD <= 0.05f || hSpan <= 1e-4f || vSpan <= 1e-4f) continue;
                const int32_t w = projViews[e].subImage.imageRect.extent.width;
                const int32_t h = projViews[e].subImage.imageRect.extent.height;
                auto clampI = [](int32_t v, int32_t lo, int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); };
                const int32_t x0 = clampI(static_cast<int32_t>(lroundf((tanf(aL) - tanDL) / hSpan * w)), 0, w - 1);
                const int32_t x1 = clampI(static_cast<int32_t>(lroundf((tanf(aR) - tanDL) / hSpan * w)), x0 + 1, w);
                const int32_t y0 = clampI(static_cast<int32_t>(lroundf((tanDU - tanf(aU)) / vSpan * h)), 0, h - 1);
                const int32_t y1 = clampI(static_cast<int32_t>(lroundf((tanDU - tanf(aD)) / vSpan * h)), y0 + 1, h);
                projViews[e].subImage.imageRect.offset.x += x0;
                projViews[e].subImage.imageRect.offset.y += y0;
                projViews[e].subImage.imageRect.extent.width  = x1 - x0;
                projViews[e].subImage.imageRect.extent.height = y1 - y0;
                projViews[e].fov.angleLeft = aL; projViews[e].fov.angleRight = aR;
                projViews[e].fov.angleUp = aU;   projViews[e].fov.angleDown = aD;
            }
        }
    }

    // [EYETAG2] telemetry (pure logging, all runtimes): declared submit FOV per eye AFTER any crop,
    // plus the rendering views' located FOV - 1 line/sec, 20 lines/session. ME1's [EYETAG] equivalent.
    if (rendered)
    {
        static ULONGLONG s_etLastMs = 0; static int s_etCount = 0;
        const ULONGLONG now = GetTickCount64();
        if (s_etCount < 20 && now - s_etLastMs > 1000)
        {
            s_etLastMs = now; ++s_etCount;
            char b[352];
            std::snprintf(b, sizeof(b),
                          "[EYETAG2] decl L(%.3f,%.3f,%.3f,%.3f) R(%.3f,%.3f,%.3f,%.3f) "
                          "rt L(%.3f,%.3f,%.3f,%.3f) R(%.3f,%.3f,%.3f,%.3f) rtValid=%d rectL=%ld,%ld %ldx%ld",
                          projViews[0].fov.angleLeft, projViews[0].fov.angleRight, projViews[0].fov.angleUp, projViews[0].fov.angleDown,
                          projViews[1].fov.angleLeft, projViews[1].fov.angleRight, projViews[1].fov.angleUp, projViews[1].fov.angleDown,
                          g_lastRtFov[0].angleLeft, g_lastRtFov[0].angleRight, g_lastRtFov[0].angleUp, g_lastRtFov[0].angleDown,
                          g_lastRtFov[1].angleLeft, g_lastRtFov[1].angleRight, g_lastRtFov[1].angleUp, g_lastRtFov[1].angleDown,
                          g_lastRtFovValid ? 1 : 0,
                          static_cast<long>(projViews[0].subImage.imageRect.offset.x),
                          static_cast<long>(projViews[0].subImage.imageRect.offset.y),
                          static_cast<long>(projViews[0].subImage.imageRect.extent.width),
                          static_cast<long>(projViews[0].subImage.imageRect.extent.height));
            XLog(b);
        }
    }

    const XrCompositionLayerBaseHeader* layers[4] = {};
    uint32_t layerCount = 0;
    if (rendered)   layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
    if (haveMono)   layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&monoQuad);
    if (haveGameUi) layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&gameUiQuad);
    if (haveUi)     layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&uiQuad);

    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = layerCount;
    fei.layers = (layerCount > 0) ? layers : nullptr;
    g_fn.endFrame(g_session, &fei);
}
}

namespace ME2VR::Me2Xr
{
void Recenter() noexcept { g_recenterRequested.store(true, std::memory_order_relaxed); }

float GetMenuScreenDist() noexcept { return g_monoQuadDistanceM.load(std::memory_order_relaxed); }
void  SetMenuScreenDist(float m) noexcept { g_monoQuadDistanceM.store((m < 0.4f) ? 0.4f : (m > 6.0f ? 6.0f : m), std::memory_order_relaxed); }
float GetMenuScreenSize() noexcept { return g_monoQuadWidthM.load(std::memory_order_relaxed); }
void  SetMenuScreenSize(float m) noexcept { g_monoQuadWidthM.store((m < 0.8f) ? 0.8f : (m > 6.0f ? 6.0f : m), std::memory_order_relaxed); }
float GetCineScreenZoom() noexcept { return g_cineScreenZoom.load(std::memory_order_relaxed); }
void  SetCineScreenZoom(float z) noexcept { g_cineScreenZoom.store((z < 0.5f) ? 0.5f : (z > 3.0f ? 3.0f : z), std::memory_order_relaxed); }
// [COMFORT] Insert-menu panel placement (ME1's Comfort tab).
float GetMenuPanelDist() noexcept { return g_menuQuadDistanceM.load(std::memory_order_relaxed); }
void  SetMenuPanelDist(float m) noexcept { g_menuQuadDistanceM.store((m < 0.8f) ? 0.8f : (m > 4.0f ? 4.0f : m), std::memory_order_relaxed); }
float GetMenuPanelSize() noexcept { return g_menuQuadWidthM.load(std::memory_order_relaxed); }
void  SetMenuPanelSize(float m) noexcept { g_menuQuadWidthM.store((m < 0.5f) ? 0.5f : (m > 3.5f ? 3.5f : m), std::memory_order_relaxed); }
float GetMenuPanelOffX() noexcept { return g_menuQuadOffXM.load(std::memory_order_relaxed); }
void  SetMenuPanelOffX(float m) noexcept { g_menuQuadOffXM.store((m < -1.5f) ? -1.5f : (m > 1.5f ? 1.5f : m), std::memory_order_relaxed); }
float GetMenuPanelOffY() noexcept { return g_menuQuadOffYM.load(std::memory_order_relaxed); }
void  SetMenuPanelOffY(float m) noexcept { g_menuQuadOffYM.store((m < -1.0f) ? -1.0f : (m > 1.0f ? 1.0f : m), std::memory_order_relaxed); }
// [LINKFOV] Quest Link image fix (no-op on runtimes that don't need it).
bool GetVrFovFill() noexcept { return g_vrFovFillEnabled.load(std::memory_order_relaxed); }
void SetVrFovFill(bool on) noexcept { g_vrFovFillEnabled.store(on, std::memory_order_relaxed); }
float GetVrFillH() noexcept { return g_vrFillH.load(std::memory_order_relaxed); }
void  SetVrFillH(float v) noexcept { g_vrFillH.store((v < 0.5f) ? 0.5f : (v > 1.25f ? 1.25f : v), std::memory_order_relaxed); }
float GetVrFillV() noexcept { return g_vrFillV.load(std::memory_order_relaxed); }
void  SetVrFillV(float v) noexcept { g_vrFillV.store((v < 0.5f) ? 0.5f : (v > 1.25f ? 1.25f : v), std::memory_order_relaxed); }
bool GetQuestFovMatch() noexcept { return g_questFovMatch.load(std::memory_order_relaxed); }
void SetQuestFovMatch(bool on) noexcept { g_questFovMatch.store(on, std::memory_order_relaxed); }
bool IsOculusRuntime() noexcept { return g_isOculusRuntime; }

void Tick() noexcept
{
    // Master VR switch OFF -> don't init or submit anything (vanilla flat game for first-person dev).
    if (!ME2VR::CalcViewHook::GetVrEnabled()) return;
    // One-time bring-up once the game device is captured.
    if (!g_tried.load(std::memory_order_acquire))
    {
        ID3D11Device* device = ME2VR::D3DCapture::GetGameDevice();
        if (device == nullptr) return;
        bool expected = false;
        if (!g_tried.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
        XLog(std::string("[BUILD] me2_xr compiled ") + __DATE__ + " " + __TIME__ + " (AERFULL)");
        BringUp(device);
        return;
    }

    if (!g_initOk) return;
    PumpEvents();
    if (g_begun) RunFrame();
}
}
