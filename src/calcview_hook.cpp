#include "calcview_hook.h"

#include "convo_fp.h"
#include "d3d_capture.h"
#include "engine_probe.h"
#include "logger.h"
#include "me2_xr.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <MinHook.h>

namespace
{
constexpr std::uintptr_t kCalcSceneViewRva = 0x6CEB70;   // confirmed by the finder, 2026-06-26

// FSceneView offsets (LE1/LE3 identical per the ME3Tweaks SDK; confirming live here).
constexpr std::uintptr_t kFsvViewMatrix = 0x90;
constexpr std::uintptr_t kFsvProjectionMatrix = 0xD0;
constexpr std::uintptr_t kFsvTranslatedViewMatrix = 0x190;
constexpr std::uintptr_t kFsvTranslatedViewProjMatrix = 0x1D0;
constexpr std::uintptr_t kFsvPreViewTranslation = 0x250;
constexpr std::uintptr_t kFsvViewProjectionMatrix = 0x260;
constexpr std::uintptr_t kFsvViewOrigin = 0x320;
constexpr std::uintptr_t kFsvState = 0x8;        // FSceneView::State (FSceneViewStateInterface*)
constexpr std::uintptr_t kFsvFloatRect = 0x64;   // FSceneView X/Y/SizeX/SizeY (4 floats)

// B2b stereo separation. 50 uu = 1 m (LE1); ~1.6 uu half-eye ~= human IPD => ~1:1 scale.
// Runtime-adjustable from the Insert menu.
std::atomic<float> g_halfEyeUU{6.0f};   // baked 2026-07-26 from the tuned value
std::atomic_bool g_swapEyes{false};
std::atomic<float> g_gameHalfFovH{0.0f};   // SUBMITTED half-FOV (may be widened by fill, for gameplay)
std::atomic<float> g_gameHalfFovV{0.0f};
std::atomic<float> g_gameRawFovH{0.0f};    // RAW game half-FOV (never widened) -> cinematic detection
std::atomic<float> g_gameRawFovV{0.0f};
// [FILLSRC] When the gameplay fill pre-writes the camera FOV (see CalcViewDetour), the projection the
// engine builds IS the fill FOV, so raw-FOV detection can no longer come from the matrix. The detour
// stores the camera's true FOV (read at the source, before the write) here and PublishGameFov
// substitutes it; g_srcBuiltFovH/g_srcWrittenFovH exist only for the mechanism-proof telemetry.
std::atomic<float> g_srcRawFovH{0.0f};
std::atomic_bool   g_srcRawValid{false};
std::atomic<float> g_srcBuiltFovH{0.0f};   // half-FOV the ENGINE actually built (from the matrix)
std::atomic<float> g_srcWrittenFovH{0.0f}; // half-FOV the mod asked the camera for
// FOV-fill: conversations/cutscenes render a narrow cinematic FOV -> small image + black around it.
// Widen each eye's projection to a target half-FOV (pushed from the XR side, headset-filling). Only
// raises FOV below the target (a floor), so already-wide gameplay is left alone. Submit matches.
std::atomic_bool g_fovFillOn{false};
std::atomic<float> g_fovFillH{0.0f};
std::atomic<float> g_fovFillV{0.0f};
// Captured gameplay "rest" half-FOV (hipfire/explore = wide). ADS/sniper narrow the FOV below this; the
// ratio (gh/rest) tells the mod how far the game has zoomed in so ADS magnification matches the game's intent.
std::atomic<float> g_restHalfFov{0.61f};
// Conversation/cutscene detect: those render a wide 16:9 cinematic FOV (aspect ~1.78) vs gameplay's
// 0.889 (half-eye). When set, render MONO (one full 16:9 view) so it's a clean flat cinema panel
// instead of a squashed/tiny stereo split. Set from the XR side off the published FOV aspect.
std::atomic_bool g_cinematic{false};
// [VRCINE] EXPERIMENTAL opt-ins: render conversations (EGameModes 5) / cutscenes (6) in the active VR
// mode instead of the flat panel. Default OFF (flat is comfortable). cineVrHeadTracking (default ON)
// gates whether the head turns the view during a VR cine (OFF = a stable stereo 3D picture).
std::atomic_bool g_cineVrConvo{false};
std::atomic_bool g_cineVrCutscene{false};
std::atomic_bool g_cineVrHeadTracking{true};
std::atomic_bool g_vrCineActive{false};   // runtime: a VR cine is being rendered this frame
std::atomic<unsigned> g_p1CalcSeq{0};     // [ENGCINE] P1 CalcSceneView heartbeat (see bump site)
std::atomic_bool g_engCineAlive{true};    // [ENGCINE] gm8+alive scene = in-engine cutscene (toggle for A/B)
std::atomic<int> g_cineEnterFrames{8};    // [CINEDEBOUNCE] consecutive narrow frames before cine entry (0 = instant/old)
// [STEREODEAD] replay-skip streak while the mode is SFR: reset by every completed replay, bumped by
// every skipped one. me2_xr's watchdog reads it at submit - presenting a stereo pair while this climbs
// means both eyes carry the same image (the silent fake-stereo failure this exists to make loud).
std::atomic<unsigned> g_sfrSkipStreak{0};
std::atomic<unsigned> g_sfrSkipReasons{0};   // bit0 = menuMode, bit1 = cinematic (at last skip)
// [NOOPREPLAY] consecutive replays that ran but built NO new scene view (engine early-out) - the
// second eye is then identical to the first. Reset by any replay that does rebuild the view.
std::atomic<unsigned> g_sfrNoopReplays{0};
// [NOOPRETRY] Bekenstein (Kasumi loyalty): the engine declines the replay in ~21-frame bursts every
// few seconds of plain gameplay (gm=0), so the right eye keeps snapping to the left eye's image =
// constant right-eye flicker. Two candidate mechanisms need different fixes, so measure BOTH:
// pass-0 cost tells whether the WHOLE renderer was suppressed (streaming/priming) or only the
// second call was refused, and a single same-frame retry tests whether the refusal is transient.
std::atomic<long long> g_sfrPass0UsLast{0};   // pass 0 Draw cost last frame (us)
std::atomic<unsigned> g_sfrPass0Noops{0};     // consecutive pass-0 draws under 100us
std::atomic<unsigned> g_sfrRetryAttempts{0};
std::atomic<unsigned> g_sfrRetrySuccesses{0};

// Free head-look (orientation): yaw/pitch in UE units (65536 = 360 deg), set from the XR side.
std::atomic_bool g_headLookEnabled{false};
// [POSETAG] how many presents back to reach for the submit's render-time pose (Bug 1/2 fix). The
// captured backbuffer was rendered with the head-look armed ~1-2 presents ago (arm-on-present->capture
// -next-present + DXGI queueing); tagging it with the CURRENT pose = the shake. Fractional, dialed
// in-headset. ME1 landed on 2.0; re-verified per GPU. 0 = tag with the current pose (old behavior).
std::atomic<float> g_poseTagDelayFrames{2.0f};
std::atomic<int32_t> g_headYawUU{0};
std::atomic<int32_t> g_headPitchUU{0};
// Head-look TUNING (ported from ME1 vr_config - the missing subsystem that made ME2 shaky). smoothing is a
// low-pass on the head angle that kills raw HMD micro-jitter (the "shaky on look left/right" smear);
// sensitivity scales the head delta; invert flips each axis. Applied XR-side in DriveHeadLook before SetHeadLook.
std::atomic_bool  g_headLookUserEnabled{true};   // user master toggle for view head-look (ME1 headLookEnabled)
std::atomic<float> g_headLookSmoothing{0.4f};    // 0 = raw/sharpest .. ->1 = smoother/laggier (ME1 default 0.4)
std::atomic<float> g_lookSensitivity{1.0f};      // 0.25..2.0 scale on the head delta (ME1 default 1.0)
std::atomic_bool  g_invertLookYaw{false};
std::atomic_bool  g_invertLookPitch{false};
// Positional 6DOF: head translation in METERS, relative to recenter (XR axes: +x right, +y up, -z fwd).
// Applied to the camera origin scaled to the same world-scale as the stereo. Toggle = g_headPosOn.
std::atomic_bool g_headPosOn{true};
std::atomic<float> g_headPosScale{1.0f};   // ME1 leanGain (1.0 = true 1:1 = ME1 default; higher = more dramatic)
std::atomic<float> g_headPosX{0.0f};
std::atomic<float> g_headPosY{0.0f};
std::atomic<float> g_headPosZ{0.0f};
std::atomic_bool g_leanInvertFwd{false};   // [LEANFWD] user toggle; see ApplyHeadPosition
float g_diagBasis[9] = {};   // diagnostic: camera right/up/fwd from the last ApplyHeadPosition
float g_diagWorld[3] = {};   // diagnostic: world-space delta from the last ApplyHeadPosition
// ULocalPlayer viewport rect (validated live).
constexpr std::uintptr_t kLpOrigin = 0x59C;      // FVector2D (OriginX, OriginY)
constexpr std::uintptr_t kLpSize = 0x5A4;        // FVector2D (SizeX, SizeY)

constexpr std::uintptr_t kAllocateViewStateRva = 0x329750;   // found via ViewState xref

using CalcFn = void*(__fastcall*)(void*, void*, void*, void*, void*, void*);
CalcFn g_orig = nullptr;
using AllocViewStateFn = void*(__fastcall*)(uint32_t);   // LE1 passes 0
AllocViewStateFn g_allocViewState = nullptr;
void* g_eyeState = nullptr;                       // standalone FSceneViewState for the right eye
void* g_eyeStateLeft = nullptr;                   // standalone FSceneViewState for the LEFT eye (temporal-ghost fix)
std::atomic_bool g_eyeStateLogged{false};
std::atomic_bool g_eyeStateLeftLogged{false};

std::atomic_bool g_started{false};
std::atomic_bool g_installed{false};
std::atomic<int> g_logged{0};
std::atomic<int> g_splitLogged{0};
std::atomic_bool g_stereo{true};                 // Milestone B: P1-layout same-frame split
std::atomic<uint64_t> g_splitSeq{0};             // bumped each time the split (or AER single view) runs
// --- VR mode selector (Mono/Stereo/AER) - supersedes g_stereo for mode selection (2026-07-04 AER port). ---
enum class VrMode : int { Mono = 0, Stereo = 1, Aer = 2, Dibr = 3, Sfr = 4 };
std::atomic<int> g_vrMode{ (int)VrMode::Sfr };   // default SFR ("Stereo 2") - the ME1 same-frame fix (both eyes get bloom/UI)
// --- AER (alternate-eye rendering) config, ported from ME1 vr_config.h. ---
std::atomic<float> g_aerHalfEyeUU{2.5f};    // IPD/scale, 0..10 uu (baked 2026-08-21, tuned value)
// [AERSHAKE bake] FALSE now. The old TRUE default existed to compensate the single-slot stamp's
// constant pipeline mislabel (label one build ahead of pixels = a fixed eye swap). The FIFO stamp
// removed the mislabel, so the compensation would now INVERT depth. Confirmed in-headset.
std::atomic_bool   g_aerSwapEyes{false};
std::atomic_bool   g_aerFramePacing{true};  // display-locked pacing (the flicker fix)
std::atomic<int>   g_aerFramePacingHz{0};   // 0 = auto-learn headset Hz
std::atomic_bool   g_stereoFramePacing{true}; // display-locked pacing for same-frame stereo (60fps->120Hz judder fix)
// --- DIBR (depth-image-based rendering) live config, ported from ME1 vr_config.h. Persisted layer; pushed
// to the d3d_capture warp shader each frame via SetDibrWarp. ---
std::atomic<float> g_depthWarpGain{1.20f};   // depth strength (LE2: gentler than LE1's 2.0 for first-light comfort)
std::atomic<float> g_depthWarpConv{0.985f};  // manual convergence plane (used when auto off)
std::atomic_bool   g_depthWarpFlip{true};    // LE2 is FORWARD-Z (clears depth to 1.0) - opposite of LE1 reversed-Z
std::atomic_bool   g_dibrAutoConverge{true}; // track subject depth for convergence
std::atomic<int>   g_dibrResX{3072};         // square render res (restart-to-apply)
std::atomic<int>   g_dibrResY{3072};
// --- AER render<->present eye handshake (drift-free; NOT present-count parity, which drifts). ---
// Made BYTE-IDENTICAL to ME1's render_hook stamp (2026-07-04): a DEDICATED seq (not the shared g_splitSeq),
// an ARMED flag (so a stale eye from a prior AER session can't read as valid), and a seq-FIRST acquire read
// in GetAerStamp. ME1: g_renderStampSeq / g_renderStampAer / g_renderStampEye, read seq(acquire)->eye(relaxed).
std::atomic<int>      g_aerRenderEye{0};    // legacy arm slot (no longer read by the detour - see [AERSHAKE])
std::atomic<int>      g_aerStampEye{-1};    // eye the detour actually built this frame (-1 = none yet)
std::atomic<uint64_t> g_aerStampSeq{0};     // dedicated AER stamp seq (ME1 g_renderStampSeq) - NOT g_splitSeq
std::atomic_bool      g_aerStampArmed{false}; // set once the detour has stamped a real AER eye (ME1 g_renderStampAer)
// [AERSHAKE 2026-08-21] The single-slot stamp was the shake. The stamp is written at BUILD time
// (game thread) but the pixels land at PRESENT, one frame later - UE3 pipelines. Reading "the
// latest stamp" therefore labels the on-screen frame with the NEXT frame's eye whenever the game
// thread runs ahead. At display/2 the pacer's long wait held that offset constant (a constant
// eye swap - the reason AerSwapEyes defaults TRUE); at full rate the offset FLAPS, so each eye
// alternates between the two camera positions: Shepard and the scene visibly vibrate between a
// left and a right version. The "seq gaps" were the same artifact - a latest-read SKIPS entries,
// nothing was ever lost.
// Fix: a FIFO. The detour queues every build's {seq, eye}, alternating the eye from its own seq
// parity (deterministic - no cross-thread arm feedback left to flap), and the present consumes
// IN ORDER, oldest first, exactly one per present - matching how the swapchain delivers frames.
// The label can no longer disagree with the pixels.
constexpr int kAerRingN = 8;
struct AerRingSlot { std::atomic<uint64_t> seq{0}; std::atomic<int> eye{0}; };
AerRingSlot g_aerRing[kAerRingN];
// Master VR switch. OFF = the whole VR pipeline (split/mono/XR/UI-dup) is bypassed -> vanilla FLAT game
// (used for developing/tuning first-person flat). Default OFF while tuning FP per-state; F4 toggles.
std::atomic_bool g_vrEnabled{true};   // VR on by default (persisted via VR/Enabled; F4 toggles)
std::uintptr_t g_base = 0;

// --- SFR (Same-Frame stereo), ported from ME1 2026-07-15. Render the whole frame TWICE per present,
// one full PRIMARY render per eye, so bloom/lighting are correct in BOTH eyes (no secondary view =
// the ME1 secondary-view-mistreatment fix). The double render comes from a FViewportClient::Draw
// replay (DrawDetour below): pass 0 = the game's own Draw (-halfEye), pass 1 = the replay (+halfEye).
// t_replay distinguishes them; it is thread_local because CalcSceneView runs synchronously inside
// Draw on the SAME game thread, so the flag set in DrawDetour is seen by the CalcSceneView detour.
constexpr std::uintptr_t kDrawRva = 0x6D0A00;    // FViewportClient::Draw (calcview_finder kDrawRva)
using DrawFn = void(__fastcall*)(void*, void*, void*);   // Draw(FViewport*, FCanvas*) - vtable slot 2
DrawFn g_origDraw = nullptr;
thread_local bool t_replay = false;              // true = the pass-1 (replay) render
std::atomic<uint64_t> g_sfrReplays{0};           // replay Draws fired (log throttle)
std::atomic<uint64_t> g_sfrReplayFaults{0};      // SEH-caught faults in the replay Draw
// [PERF] replay-cost accumulators (game thread writes, me2_xr reads+resets for the [PERF] line).
std::atomic<long long> g_sfrReplayUsSum{0};
std::atomic<long long> g_sfrReplayUsCount{0};
std::atomic<long long> g_sfrReplayUsMax{0};
// [SFR] convergence: a small opposite per-eye horizontal projection shift that pulls the zero-disparity
// (fusion) plane in from infinity, so distant isolated elements (interaction prompts the game floats at a
// fixed distance) fuse without lowering the IPD/world-scale. UI scale shrinks the HUD toward center so
// flat-screen-sized notifications aren't huge across the headset FOV (read by the d3d_capture UI hooks).
std::atomic<float> g_sfrConvergence{0.04f};      // per-eye proj horizontal off-center shift (0 = at infinity); baked 2026-07-26
std::atomic<float> g_sfrUiScale{1.0f};           // 1.0 = full size; <1 shrinks the SFR HUD toward center
// [SFR-UI] HUD master transform (ME1 HUD-tab parity): independent X/Y scale + center-relative offset,
// applied to the SFR UI viewport in BOTH eyes. Lower Scale X to un-stretch the HUD (16:9->square FOV).
std::atomic<float> g_sfrUiScaleX{1.0f};
// disparity = the depth the HUD/subtitles fuse at, decoupled from world convergence (the whole-frame
// submit shift). 0 = old behavior (UI rides the convergence plane exactly).
std::atomic<float> g_sfrUiScaleY{1.0f};
std::atomic<float> g_sfrUiOffX{0.0f};            // fraction of viewport width  (-0.5..0.5)
std::atomic<float> g_sfrUiOffY{0.0f};            // fraction of viewport height (-0.5..0.5)

// Write the ULocalPlayer viewport rect (Origin/Size) the engine reads in CalcSceneView. SEH-guarded.
void WriteRect(std::uintptr_t lp, float ox, float oy, float sx, float sy) noexcept
{
    __try
    {
        volatile float* o = reinterpret_cast<volatile float*>(lp + kLpOrigin);
        volatile float* s = reinterpret_cast<volatile float*>(lp + kLpSize);
        o[0] = ox; o[1] = oy; s[0] = sx; s[1] = sy;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetViewState(std::uintptr_t sv, void* st) noexcept
{
    __try { *reinterpret_cast<void* volatile*>(sv + kFsvState) = st; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* CallAllocViewState() noexcept
{
    __try { return g_allocViewState ? g_allocViewState(0) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool Readable(void* p, size_t n) noexcept
{
    return p != nullptr && !IsBadReadPtr(p, n);
}

void CopyFloats(std::uintptr_t d, std::uintptr_t s, int n) noexcept
{
    volatile float* dd = reinterpret_cast<volatile float*>(d);
    const volatile float* ss = reinterpret_cast<const volatile float*>(s);
    for (int i = 0; i < n; ++i) dd[i] = ss[i];
}

// Sync the right eye's CAMERA (position + orientation) to the left eye's. The split builds each eye with its
// own CalcSceneView call; during a camera transition the engine's per-call blend can return slightly different
// positions, so the two eyes diverge ("snapping between two cameras" in VR). Deriving both eyes from ONE camera
// (left) + the IPD offset makes divergence impossible. Copies only the view fields (NOT the projection, which
// is per-half); the per-eye eye-offset/head-look/FOV below rebuild the view-projection. SEH-guarded.
void SyncEyeCamera(std::uintptr_t dst, std::uintptr_t src) noexcept
{
    __try
    {
        CopyFloats(dst + kFsvViewMatrix,           src + kFsvViewMatrix,           16);
        CopyFloats(dst + kFsvTranslatedViewMatrix, src + kFsvTranslatedViewMatrix, 16);
        CopyFloats(dst + kFsvViewOrigin,           src + kFsvViewOrigin,           3);
        CopyFloats(dst + kFsvPreViewTranslation,   src + kFsvPreViewTranslation,   3);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Move this view's camera by signedUU along the render-camera RIGHT axis (ViewMatrix col0),
// editing PreViewTranslation + ViewOrigin. A real camera move -> real binocular parallax (AER method).
void Mul4x4(float* out, const float* a, const float* b) noexcept;   // defined below

// [CONVOFP] Absolute camera relocation to Shepard's head during a conversation. Defined below (it
// needs ApplyHeadRotation); declared here because it must run BEFORE the eye offset. Idempotent per
// view per frame, so putting it at the head of both entry points covers all five submit paths
// without a per-path edit -- the [LINKFOV] lesson: ME2 has several paths, patch the choke point.
void EnsureConvoFpOrigin(std::uintptr_t sv) noexcept;

void ApplyEyeOffset(std::uintptr_t sv, float signedUU) noexcept
{
    EnsureConvoFpOrigin(sv);
    __try
    {
        auto* view = reinterpret_cast<float*>(sv + kFsvViewMatrix);
        const float rx = view[0], ry = view[4], rz = view[8];   // world-space right = column 0
        const float wx = signedUU * rx, wy = signedUU * ry, wz = signedUU * rz;
        volatile float* pvt = reinterpret_cast<volatile float*>(sv + kFsvPreViewTranslation);
        volatile float* vo = reinterpret_cast<volatile float*>(sv + kFsvViewOrigin);
        pvt[0] -= wx; pvt[1] -= wy; pvt[2] -= wz;
        vo[0] += wx;  vo[1] += wy;  vo[2] += wz;
        // [EYEVPM] Bake the shift into ViewMatrix too (translation row = -C*basis; the shift is along
        // camera-right so only the right-dot changes) and rebuild the NON-translated view-projection.
        // The world renders through TranslatedViewMatrix+PreViewTranslation (shifted above), but
        // screen-projected sprites (selection markers, lens flares) go through ViewMatrix/VPM - without
        // this they render from the HEAD-CENTER camera in both SFR passes = a fixed-disparity layer
        // ("two of each, fuse only up close"). One float: VM[12] -= signedUU.
        view[12] -= signedUU;
        const auto* proj = reinterpret_cast<const float*>(sv + kFsvProjectionMatrix);
        const auto* tview = reinterpret_cast<const float*>(sv + kFsvTranslatedViewMatrix);
        auto* vproj = reinterpret_cast<float*>(sv + kFsvViewProjectionMatrix);
        auto* tvproj = reinterpret_cast<float*>(sv + kFsvTranslatedViewProjMatrix);
        Mul4x4(vproj, view, proj);
        Mul4x4(tvproj, tview, proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Mul4x4(float* out, const float* a, const float* b) noexcept
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
}

// ---- [INVMAT] stale-inverse-matrix fix, ported from LE1 (2026-07-12; solved LE1's head-locked dark
// panel). Every edit below (eye offset, head look, FOV widen) rebuilds only the FORWARD matrix products;
// FSceneView also caches INVERSE matrices (LE1: inv(TVPM)@0x210, inv(PM)@0x2A0, inv(VPM)@0x2E0) that keep
// describing the pre-edit camera. Height fog / light shafts reconstruct world from screen+depth through
// those inverses -> injected head PITCH makes a hard screen-locked dark fog band. Fix: a one-shot scan of a
// pristine view finds which 64-byte blocks are numerically pure inverses, then RefreshInverseSlots
// recomputes exactly those after all edits, every view.
// [2026-07-13 HARDENING, ported from LE1 e12b559] "Self-validating: never writes an unproven slot" was
// proven WRONG in LE1: the scan is per-boot non-deterministic and false-positives extra slots on degenerate
// frames (LE1 logged a 12-match boot that validated OVERLAPPING 16-byte-apart blocks, then crashed the game
// the same millisecond the refresh first wrote them; the whole 2026-07-12-evening "random crash" family -
// heap/nvwgf2umx/VD-fastfail/game-code - was this one corruption). This lane's 0x340 window bound (below)
// already blocks the out-of-struct neighbor case, but in-struct false positives remain possible, so WRITES
// are additionally hard-bounded to the canonical proven trio (IsCanonicalInvSlot); extra matches log-only.
constexpr int kInvMaxSlots = 24;
struct InvSlot { std::uint32_t offset; int target; float scanErr; };
InvSlot g_invSlots[kInvMaxSlots] = {};
int g_invSlotCount = 0;
std::atomic_bool g_invScanDone{false};
std::atomic_bool g_invRecheckDone{false};
std::atomic_bool g_invFixEnabled{true};   // default ON (root cause proven in LE1; scan self-limits)
std::atomic<std::uint64_t> g_invScanViews{0};
std::atomic<std::uint64_t> g_invFixLogs{0};

struct InvTarget { const char* name; std::uintptr_t off; };
constexpr InvTarget kInvTargets[] = {
    {"ViewMatrix", kFsvViewMatrix},
    {"ProjectionMatrix", kFsvProjectionMatrix},
    {"TranslatedViewMatrix", kFsvTranslatedViewMatrix},
    {"TranslatedViewProjMatrix", kFsvTranslatedViewProjMatrix},
    {"ViewProjectionMatrix", kFsvViewProjectionMatrix},
};
constexpr int kInvTargetCount = static_cast<int>(sizeof(kInvTargets) / sizeof(kInvTargets[0]));
// SCAN WINDOW - bounded to the real FSceneView struct (2026-07-12, ME3 load-crash fix, applied here too:
// ME2 shares the LE2/3 layout). The last documented field is ViewOrigin @ 0x320; the three genuine cached
// inverses sit at 0x210/0x2A0/0x2E0, all below it. A 0x800 window reached PAST the struct into an adjacent
// allocation, where the LE2/3 layout has two perfect inverse-matches at 0x720/0x7B0 (a pooled neighbor
// scene view). Those passed the readable check but weren't THE MOD'S memory, so RefreshInverseSlots corrupted
// the neighbor every frame -> crash on load. 0x340 covers through 0x2E0+0x40=0x320 and can't reach a neighbor.
constexpr std::uintptr_t kInvScanBytes = 0x340;

// General 4x4 inverse (cofactor expansion; projection-bearing matrices too). False on near-singular.
bool Inverse4x4(const float* m, float* out) noexcept
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-25f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

// Max deviation of P from identity; translation row normalized by camera-translation magnitude (float32
// residue of inv(M)*M on a 1e5-uu translation is legitimately ~1e-2, not a mismatch).
float IdentityErr(const float* P, float transMag) noexcept
{
    float tDiv = transMag * 5e-5f;
    if (tDiv < 1.0f) tDiv = 1.0f;
    float err = 0.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float dv = P[i * 4 + j] - ((i == j) ? 1.0f : 0.0f);
            if (dv < 0.0f) dv = -dv;
            if (i == 3 && j < 3) dv /= tDiv;
            if (dv > err) err = dv;
        }
    return err;
}

float InvPairErr(const float* B, const float* M, float transMag) noexcept
{
    float P[16];
    Mul4x4(P, B, M);
    float err = IdentityErr(P, transMag);
    Mul4x4(P, M, B);
    const float err2 = IdentityErr(P, transMag);
    return (err2 < err) ? err2 : err;
}

// SEH leaves (no C++ objects - C2712). Scan returns match count, or <0 to retry on a later view.
int InvMatScanSEH(std::uintptr_t sv, InvSlot* outSlots, int maxSlots, float* outTransMag) noexcept
{
    __try
    {
        if (!Readable(reinterpret_cast<void*>(sv), kInvScanBytes)) return - 1;
        const float* vm = reinterpret_cast<const float*>(sv + kFsvViewMatrix);
        float transMag = 0.0f;
        for (int i = 12; i < 15; ++i)
        {
            const float a = (vm[i] < 0.0f) ? -vm[i] : vm[i];
            if (a > transMag) transMag = a;
        }
        if (outTransMag != nullptr) *outTransMag = transMag;
        if (transMag < 50.0f) return - 2;
        int n = 0;
        for (std::uint32_t off = 0; off + 64 <= kInvScanBytes && n < maxSlots; off += 0x10)
        {
            const float* B = reinterpret_cast<const float*>(sv + off);
            for (int t = 0; t < kInvTargetCount; ++t)
            {
                if (off == kInvTargets[t].off) continue;
                const float* M = reinterpret_cast<const float*>(sv + kInvTargets[t].off);
                const float err = InvPairErr(B, M, transMag);
                if (err < 0.05f)
                {
                    outSlots[n].offset = off;
                    outSlots[n].target = t;
                    outSlots[n].scanErr = err;
                    ++n;
                    break;
                }
            }
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return - 1; }
}

bool InvMatErrsSEH(std::uintptr_t sv, const InvSlot* slots, int count, float* outErrs) noexcept
{
    __try
    {
        for (int i = 0; i < count; ++i)
        {
            const float* B = reinterpret_cast<const float*>(sv + slots[i].offset);
            const float* M = reinterpret_cast<const float*>(sv + kInvTargets[slots[i].target].off);
            outErrs[i] = InvPairErr(B, M, 100000.0f);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [LE1 e12b559] Canonical WRITE whitelist: only the clean-room-proven trio may ever be written, and each
// only if the scan confirmed it at that exact offset+target (struct moves degrade to no-op, not corruption).
// Everything else the scan matches is diagnostic-only.
bool IsCanonicalInvSlot(std::uint32_t offset, int target) noexcept
{
    const std::uintptr_t targetOff = kInvTargets[target].off;
    return (offset == 0x210 && targetOff == kFsvTranslatedViewProjMatrix) ||
           (offset == 0x2A0 && targetOff == kFsvProjectionMatrix) ||
           (offset == 0x2E0 && targetOff == kFsvViewProjectionMatrix);
}

int InvMatRefreshSEH(std::uintptr_t sv, const InvSlot* slots, int count) noexcept
{
    __try
    {
        int fixedCount = 0;
        for (int i = 0; i < count; ++i)
        {
            if (!IsCanonicalInvSlot(slots[i].offset, slots[i].target)) continue;   // log-only candidate
            const float* M = reinterpret_cast<const float*>(sv + kInvTargets[slots[i].target].off);
            float invM[16];
            if (!Inverse4x4(M, invM)) continue;
            float* B = reinterpret_cast<float*>(sv + slots[i].offset);
            for (int k = 0; k < 16; ++k) B[k] = invM[k];
            ++fixedCount;
        }
        return fixedCount;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// One-shot scan of a PRISTINE view (call right after g_orig returns, before any edit). Retries until it
// sees a real scene view (camera >50uu from origin - identity-ish views match everything).
void InvMatScanTick(std::uintptr_t sv) noexcept
{
    if (g_invScanDone.load(std::memory_order_acquire)) return;
    const std::uint64_t views = g_invScanViews.fetch_add(1, std::memory_order_relaxed) + 1;
    if (views < 300) return;
    float transMag = 0.0f;
    InvSlot slots[kInvMaxSlots] = {};
    const int n = InvMatScanSEH(sv, slots, kInvMaxSlots, &transMag);
    if (n < 0) return;   // unreadable / near-origin view: retry later
    for (int i = 0; i < n; ++i) g_invSlots[i] = slots[i];
    g_invSlotCount = n;
    g_invScanDone.store(true, std::memory_order_release);
    for (int i = 0; i < n; ++i)
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "[INVMAT] scan: slot off=0x%X = inverse(%s) err=%.6f %s",
                      slots[i].offset, kInvTargets[slots[i].target].name, slots[i].scanErr,
                      IsCanonicalInvSlot(slots[i].offset, slots[i].target)
                          ? "[write-enabled]" : "[LOG-ONLY: non-canonical, never written]");
        ME2VR::Log::Line(buf);
    }
    char done[192];
    std::snprintf(done, sizeof(done), "[INVMAT] scan complete matches=%d viewTransMag=%.1f%s",
                  n, transMag, n == 0 ? " -> NO cached inverses found; fix will no-op" : "");
    ME2VR::Log::Line(done);
}

// Refresh every scan-validated inverse slot from its (now edited) target. Call after the LAST edit of each
// view (ApplyFov). Also emits the one-shot staleness proof the first time a real pitch is injected.
void RefreshInverseSlots(std::uintptr_t sv) noexcept
{
    if (!g_invScanDone.load(std::memory_order_acquire) || g_invSlotCount <= 0) return;

    const std::int32_t pitchUU = g_headPitchUU.load(std::memory_order_relaxed);
    const bool bigPitch = (pitchUU > 910) || (pitchUU < -910);   // >5 deg (65536 = 360 deg)
    if (bigPitch && !g_invRecheckDone.load(std::memory_order_acquire))
    {
        float errs[kInvMaxSlots] = {};
        if (InvMatErrsSEH(sv, g_invSlots, g_invSlotCount, errs))
        {
            g_invRecheckDone.store(true, std::memory_order_release);
            for (int i = 0; i < g_invSlotCount; ++i)
            {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "[INVMAT] post-edit off=0x%X target=%s err=%.6f pitchUU=%d -> %s",
                              g_invSlots[i].offset, kInvTargets[g_invSlots[i].target].name, errs[i],
                              static_cast<int>(pitchUU),
                              errs[i] > 0.05f ? "STALE (pre-edit camera)" : "consistent");
                ME2VR::Log::Line(buf);
            }
        }
    }

    if (!g_invFixEnabled.load(std::memory_order_acquire)) return;
    const int fixedCount = InvMatRefreshSEH(sv, g_invSlots, g_invSlotCount);
    const std::uint64_t k = g_invFixLogs.fetch_add(1, std::memory_order_relaxed);
    if (k < 4 || (k % 3600) == 0)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "[INVMAT] fix active: refreshed %d/%d inverse slots", fixedCount, g_invSlotCount);
        ME2VR::Log::Line(buf);
    }
}

// Rotate the rendered view by head yaw (about up) + pitch (about right): post-multiply ViewMatrix and
// TranslatedViewMatrix by the delta, then recompute the (translated) view-projection. Ported from LE1.
void ApplyHeadRotation(std::uintptr_t sv, float yawRad, float pitchRad) noexcept
{
    __try
    {
        auto* view = reinterpret_cast<float*>(sv + kFsvViewMatrix);
        auto* tview = reinterpret_cast<float*>(sv + kFsvTranslatedViewMatrix);
        const auto* proj = reinterpret_cast<const float*>(sv + kFsvProjectionMatrix);
        auto* vproj = reinterpret_cast<float*>(sv + kFsvViewProjectionMatrix);
        auto* tvproj = reinterpret_cast<float*>(sv + kFsvTranslatedViewProjMatrix);

        const float cy = cosf(yawRad), sy = sinf(yawRad), cp = cosf(pitchRad), sp = sinf(pitchRad);
        const float yaw[16] = { cy,0,-sy,0,  0,1,0,0,  sy,0,cy,0,  0,0,0,1 };
        const float pitch[16] = { 1,0,0,0,  0,cp,sp,0,  0,-sp,cp,0,  0,0,0,1 };
        // yaw*pitch (yaw about world-up, pitch about body-right): keeps pitch consistent at ALL
        // headings. pitch*yaw inverts pitch once you turn ~180 deg (the LE1 bug).
        float delta[16] = {}; Mul4x4(delta, yaw, pitch);
        float nv[16] = {}, ntv[16] = {};
        Mul4x4(nv, view, delta); Mul4x4(ntv, tview, delta);
        for (int i = 0; i < 16; ++i) { view[i] = nv[i]; tview[i] = ntv[i]; }
        Mul4x4(vproj, view, proj); Mul4x4(tvproj, tview, proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- [CONVOFP] first-person conversations --------------------------------------------------------
// Relocate the view from the director's scripted shot to Shepard's staged head, and face it at whoever
// the scene says Shepard is looking at. Runs before the eye offset and head rotation so IPD, lean and
// head-look all compose relative to the new position.
//
// TWO rules are baked in here because ME1's CINEFP lane paid for both:
//  1. A position move bigger than lean MUST rewrite the ViewMatrix translation row and rebuild
//     VPM/TVPM, or the engine keeps frustum-culling for the director's position and the person you
//     are talking to vanishes on reverse angles. (ME1 v1's headline bug.)
//  2. The relocation must be idempotent per view. The stereo paths call ApplyEyeOffset BEFORE
//     ApplyHeadLook; relocating in both would re-derive the delta from the already-shifted origin and
//     collapse both eyes onto the same point -- zero IPD, no stereo at all.
std::atomic<unsigned long long> g_convoFpFrame{0};
std::atomic_bool g_convoFpInvertFacing{false};
// Applied-vs-skipped counters. The first build's flicker was invisible in the ConvoFp log because the
// pose was rock steady the whole time -- the on/off was happening HERE. Count it so the next report is
// evidence rather than another suspect.
std::atomic<unsigned long long> g_convoFpApplies{0};
std::atomic<unsigned long long> g_convoFpSkips{0};

// NOTE: there is deliberately NO per-view "already relocated this frame" table any more.
//
// The first attempt keyed one on a token bumped once per PRESENT. SFR renders the frame TWICE between
// presents (the Draw replay), so both passes shared a token: pass 0 relocated, pass 1 was refused and
// rendered from the director's camera. Two passes showing two different cameras is exactly the
// "flickering between the regular convo camera and first person" report -- the guard WAS the bug.
//
// It is not needed. Both edits are naturally idempotent: after one apply the view origin IS the
// target, so the next delta is zero, and the camera yaw IS the base yaw, so the next rotation is zero.
// The one thing that is NOT idempotent is the eye offset being applied BETWEEN two relocations (the
// second would snap the eye back onto centre and collapse IPD). So the rule is simply: relocate once
// per view, BEFORE the eye offset -- never after. Hence the single call at the head of ApplyEyeOffset,
// with explicit calls on the two paths that never reach it (DIBR and flat).

void EnsureConvoFpOrigin(std::uintptr_t sv) noexcept
{
    // NOTE: this function does the REAL camera placement, and it is not optional. Writing the pose into
    // CalcSceneView's loc/rot parameters looks like it should be enough -- and it is what fixes the
    // engine's CULL frustum -- but the final view matrix does not come out where those parameters say.
    // A build that disabled this in favour of the source write turned first-person conversations off
    // entirely (log: applied=0 skipped=0, the signature of this early return firing every frame).
    if (!ME2VR::ConvoFp::IsArmed()) { g_convoFpSkips.fetch_add(1, std::memory_order_relaxed); return; }
    // No CineVrConvo gate here any more. me2_xr now derives renderCineInVr from ConvoFp being enabled,
    // so VR presentation is guaranteed whenever the mod is armed -- and gating on a toggle that only the
    // checkbox handler ever set is exactly what made a fresh boot need an off/on cycle to come alive.
    ME2VR::ConvoFp::EyePose pose{};
    if (!ME2VR::ConvoFp::GetEyePose(&pose)) { g_convoFpSkips.fetch_add(1, std::memory_order_relaxed); return; }

    float cam[9] = {};   // camera axes in world space: right(0..2), up(3..5), forward(6..8)
    __try
    {
        auto* view = reinterpret_cast<float*>(sv + kFsvViewMatrix);
        // World->view in row-vector form: the COLUMNS are the camera axes in world space.
        const float rX = view[0], rY = view[4], rZ = view[8];
        const float uX = view[1], uY = view[5], uZ = view[9];
        const float fX = view[2], fY = view[6], fZ = view[10];
        cam[0] = rX; cam[1] = rY; cam[2] = rZ;
        cam[3] = uX; cam[4] = uY; cam[5] = uZ;
        cam[6] = fX; cam[7] = fY; cam[8] = fZ;

        volatile float* vo = reinterpret_cast<volatile float*>(sv + kFsvViewOrigin);
        const float dx = pose.x - vo[0];
        const float dy = pose.y - vo[1];
        const float dz = pose.z - vo[2];
        // Sanity bound: a conversation stages within a room. A wild delta means the mod reads a stale or
        // wrong actor, and flinging the camera across the level is exactly the "black screen, no UI"
        // failure the HUD lane produced. Refuse rather than render garbage.
        if (!(dx * dx + dy * dy + dz * dz < 4.0e8f)) return;   // ~20000uu; NaN-safe (negated compare)

        volatile float* pvt = reinterpret_cast<volatile float*>(sv + kFsvPreViewTranslation);
        pvt[0] -= dx; pvt[1] -= dy; pvt[2] -= dz;
        vo[0] += dx;  vo[1] += dy;  vo[2] += dz;
        // Rule 1: bake the move into the ViewMatrix translation row (view-space = -C*basis), then the
        // products get rebuilt below by ApplyHeadRotation.
        view[12] -= (dx * rX + dy * rY + dz * rZ);
        view[13] -= (dx * uX + dy * uY + dz * uZ);
        view[14] -= (dx * fX + dy * fY + dz * fZ);

    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    // ---- orientation: EXACT basis -> basis, not a yaw/pitch delta ----------------------------------
    // The first version decomposed the director's camera into yaw + pitch and corrected each. That
    // silently assumed the shot had no roll and that yaw/pitch compose independently -- neither holds
    // for ME2's over-shoulder and dutch-angle framings. Every cut left a DIFFERENT residual error, so
    // the view landed somewhere new each time and the player had to hunt for the person being talked to.
    //
    // Instead: build the camera basis the mod wants outright and rotate the current basis onto it. Because
    // the destination is absolute (level, no roll, facing baseYaw), the director's framing cannot leak
    // in at all -- the result is bit-identical across every shot, so cuts cannot move the view.
    float yawRad = pose.baseYawDeg * (3.14159265358979f / 180.0f);
    if (g_convoFpInvertFacing.load(std::memory_order_relaxed)) yawRad += 3.14159265358979f;
    const float cy = std::cos(yawRad), sy = std::sin(yawRad);
    // UE3: X forward, Y right, Z up. Level camera => up is world up, right = up x forward.
    const float des[9] = { -sy, cy, 0.0f,      // right
                            0.0f, 0.0f, 1.0f,  // up
                            cy,  sy, 0.0f };   // forward

    // D maps the current basis onto the desired one: D[k][j] = dot(camAxis_k, desiredAxis_j). Valid
    // because a rotation matrix's inverse is its transpose. Post-multiplying by D is the same operation
    // ApplyHeadRotation performs, so the matrix bookkeeping below is the proven path.
    float D[16] = {};
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j)
            D[k * 4 + j] = cam[k * 3 + 0] * des[j * 3 + 0] +
                           cam[k * 3 + 1] * des[j * 3 + 1] +
                           cam[k * 3 + 2] * des[j * 3 + 2];
    D[15] = 1.0f;

    __try
    {
        auto* view = reinterpret_cast<float*>(sv + kFsvViewMatrix);
        auto* tview = reinterpret_cast<float*>(sv + kFsvTranslatedViewMatrix);
        const auto* proj = reinterpret_cast<const float*>(sv + kFsvProjectionMatrix);
        auto* vproj = reinterpret_cast<float*>(sv + kFsvViewProjectionMatrix);
        auto* tvproj = reinterpret_cast<float*>(sv + kFsvTranslatedViewProjMatrix);
        float nv[16] = {}, ntv[16] = {};
        Mul4x4(nv, view, D); Mul4x4(ntv, tview, D);
        for (int i = 0; i < 16; ++i) { view[i] = nv[i]; tview[i] = ntv[i]; }
        Mul4x4(vproj, view, proj); Mul4x4(tvproj, tview, proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    g_convoFpApplies.fetch_add(1, std::memory_order_relaxed);
}

// Positional 6DOF: translate the camera by the head's physical movement (XR meters, relative to recenter),
// mapped through the game-camera basis (ViewMatrix columns) into world units, scaled by halfEyeUU/0.032
// (uu per meter) so head movement matches the stereo world-scale. Edits ViewOrigin + PreViewTranslation
// (same AER mechanism as the eye offset). Call BEFORE the head rotation so the basis is the player's facing.
void ApplyHeadPosition(std::uintptr_t sv) noexcept
{
    if (!g_headPosOn.load(std::memory_order_acquire)) return;
    float hx = g_headPosX.load(std::memory_order_relaxed);
    float hy = g_headPosY.load(std::memory_order_relaxed);
    float hz = g_headPosZ.load(std::memory_order_relaxed);
    if (hx < -2.0f) hx = -2.0f; else if (hx > 2.0f) hx = 2.0f;   // clamp to sane head movement (ignore garbage)
    if (hy < -2.0f) hy = -2.0f; else if (hy > 2.0f) hy = 2.0f;
    if (hz < -2.0f) hz = -2.0f; else if (hz > 2.0f) hz = 2.0f;
    // ME1-PARITY 6DOF (2026-07-04): fixed 50 uu/m (WORLD_TO_METERS), NOT coupled to the stereo eye slider, and
    // forward/back emphasized 3x - matches ME1 exactly (xr_session.cpp kMetersToUU=50, kFwdLeanEmphasis=3).
    constexpr float kMetersToUU = 50.0f;
    constexpr float kFwdLeanEmphasis = 3.0f;
    const float w2m = kMetersToUU * g_headPosScale.load(std::memory_order_relaxed);   // uu/m * gain (ME1 leanGain)
    __try
    {
        const volatile float* VM = reinterpret_cast<const volatile float*>(sv + kFsvViewMatrix);
        const float rX = VM[0], rY = VM[4], rZ = VM[8];    // camera right   (col0)
        const float uX = VM[1], uY = VM[5], uZ = VM[9];    // camera up      (col1)
        const float fX = VM[2], fY = VM[6], fZ = VM[10];   // camera forward (col2)
        // [LEANFWD] XR: +x right, +y up, -z forward. The basis above IS correct (for a world->view
        // matrix in row-vector form the COLUMNS are the camera axes, which is what VM[0]/VM[4]/VM[8]
        // walks), so - hz is the mathematically right forward term. Guessing at this sign twice didn't
        // settle it, so it's a toggle now: whichever way it reads wrong in the headset, one click
        // fixes it instead of another build.
        const float fwdSign = g_leanInvertFwd.load(std::memory_order_relaxed) ? 1.0f : -1.0f;
        const float dR = hx, dU = hy, dF = fwdSign * hz * kFwdLeanEmphasis;
        const float wx = w2m * (dR * rX + dU * uX + dF * fX);
        const float wy = w2m * (dR * rY + dU * uY + dF * fY);
        const float wz = w2m * (dR * rZ + dU * uZ + dF * fZ);
        volatile float* pvt = reinterpret_cast<volatile float*>(sv + kFsvPreViewTranslation);
        volatile float* vo = reinterpret_cast<volatile float*>(sv + kFsvViewOrigin);
        pvt[0] -= wx; pvt[1] -= wy; pvt[2] -= wz;
        vo[0] += wx;  vo[1] += wy;  vo[2] += wz;
        // [EYEVPM] bake the lean into ViewMatrix's translation row too (see ApplyEyeOffset) so the
        // sprite/VPM path tracks 6DOF; ApplyHeadRotation runs right after and rebuilds the VPMs.
        volatile float* vmw = reinterpret_cast<volatile float*>(sv + kFsvViewMatrix);
        vmw[12] -= (wx * rX + wy * rY + wz * rZ);
        vmw[13] -= (wx * uX + wy * uY + wz * uZ);
        vmw[14] -= (wx * fX + wy * fY + wz * fZ);
        // capture for the diagnostic logger (POD globals; logged outside any __try in DriveHeadLook)
        g_diagBasis[0] = rX; g_diagBasis[1] = rY; g_diagBasis[2] = rZ;
        g_diagBasis[3] = uX; g_diagBasis[4] = uY; g_diagBasis[5] = uZ;
        g_diagBasis[6] = fX; g_diagBasis[7] = fY; g_diagBasis[8] = fZ;
        g_diagWorld[0] = wx; g_diagWorld[1] = wy; g_diagWorld[2] = wz;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ApplyHeadLook(std::uintptr_t sv) noexcept
{
    // [CONVOFP] deliberately NOT relocated here. ApplyEyeOffset always runs before this on the paths
    // that use both, and relocating AFTER an eye offset snaps the eye back onto centre = no IPD. The
    // paths that never call ApplyEyeOffset (DIBR, flat) call EnsureConvoFpOrigin explicitly instead.
    // Positional tracking is independent of camera-rotation ownership. Dialogue and
    // cutscene cameras may suppress rotation without suppressing physical lean.
    // CineVrHeadTracking=0 remains an explicit full cinematic tracking lock.
    const bool cineTrackingLocked =
        g_vrCineActive.load(std::memory_order_acquire) &&
        !g_cineVrHeadTracking.load(std::memory_order_acquire);
    if (!cineTrackingLocked)
        ApplyHeadPosition(sv);   // uses the director/game camera basis, pre-rotation

    if (!g_headLookEnabled.load(std::memory_order_acquire)) return;
    constexpr float kUUToRad = 6.28318530718f / 65536.0f;
    ApplyHeadRotation(sv,
                      static_cast<float>(g_headYawUU.load(std::memory_order_relaxed)) * kUUToRad,
                      static_cast<float>(g_headPitchUU.load(std::memory_order_relaxed)) * kUUToRad);
}

// Publish the game's per-eye half-FOV (radians) from the projection matrix: proj[0]=1/tan(halfH), proj[5]=1/tan(halfV).
void PublishGameFov(std::uintptr_t sv) noexcept
{
    __try
    {
        const volatile float* P = reinterpret_cast<const volatile float*>(sv + kFsvProjectionMatrix);
        const float p0 = P[0], p5 = P[5], p15 = P[15];
        if (p0 > 0.0001f && p5 > 0.0001f && p15 > -0.01f && p15 < 0.01f)   // perspective only
        {
            float h = atanf(1.0f / p0), v = atanf(1.0f / p5);
            g_srcBuiltFovH.store(h, std::memory_order_relaxed);   // [FILLSRC] what the engine built
            if (g_srcRawValid.load(std::memory_order_relaxed))
            {
                // [FILLSRC] the build already contains the fill FOV; raw detection uses the camera's
                // true FOV read at the source, vertical derived through the SAME built aspect.
                const float sh = g_srcRawFovH.load(std::memory_order_relaxed);
                if (sh > 0.01f) { h = sh; v = atanf(tanf(sh) * (p0 / p5)); }
            }
            g_gameRawFovH.store(h, std::memory_order_relaxed);   // raw -> detection
            g_gameRawFovV.store(v, std::memory_order_relaxed);
            g_gameHalfFovH.store(h, std::memory_order_relaxed);  // submit default = raw (fill may widen)
            g_gameHalfFovV.store(v, std::memory_order_relaxed);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Overwrite this view's projection half-FOV (proj[0]=1/tan(hHalf), proj[5]=1/tan(vHalf)) to widen it,
// then rebuild the (translated) view-projection. Must run AFTER eye-offset + head-look.
void WidenProjection(std::uintptr_t sv, float hHalf, float vHalf) noexcept
{
    __try
    {
        auto* proj = reinterpret_cast<float*>(sv + kFsvProjectionMatrix);
        const float th = tanf(hHalf), tv = tanf(vHalf);
        if (th > 0.0001f) proj[0] = 1.0f / th;
        if (tv > 0.0001f) proj[5] = 1.0f / tv;
        const auto* view = reinterpret_cast<const float*>(sv + kFsvViewMatrix);
        const auto* tview = reinterpret_cast<const float*>(sv + kFsvTranslatedViewMatrix);
        auto* vproj = reinterpret_cast<float*>(sv + kFsvViewProjectionMatrix);
        auto* tvproj = reinterpret_cast<float*>(sv + kFsvTranslatedViewProjMatrix);
        Mul4x4(vproj, view, proj);
        Mul4x4(tvproj, tview, proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// [SFR] Convergence: shift this eye's projection horizontally (an off-axis frustum) so a chosen depth
// has zero disparity. The lateral eye offset alone puts zero-disparity at infinity, so distant isolated
// elements double until you're close; an opposite per-eye shift pulls the fusion plane in WITHOUT
// touching the IPD (world scale). proj[8] is UE3's horizontal off-center term. Run AFTER ApplyFov (which
// rebuilds the view-projection) so it isn't overwritten; rebuild the (translated) view-projection here.
void ApplyConvergence(std::uintptr_t sv, float signedShift) noexcept
{
    if (signedShift == 0.0f) return;
    __try
    {
        auto* proj = reinterpret_cast<float*>(sv + kFsvProjectionMatrix);
        proj[8] += signedShift;
        const auto* view = reinterpret_cast<const float*>(sv + kFsvViewMatrix);
        const auto* tview = reinterpret_cast<const float*>(sv + kFsvTranslatedViewMatrix);
        auto* vproj = reinterpret_cast<float*>(sv + kFsvViewProjectionMatrix);
        auto* tvproj = reinterpret_cast<float*>(sv + kFsvTranslatedViewProjMatrix);
        Mul4x4(vproj, view, proj);
        Mul4x4(tvproj, tview, proj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// [UIRATIO] the half-FOV this view was actually RENDERED with (== raw game FOV unless fill widened
// it). The UI hooks scale game-projected HUD (crosshair!) by tan(game)/tan(render) per axis so UI
// screen positions land on their world points in the widened render - bullets meet the crosshair.
std::atomic<float> g_renderHalfFovH{0.0f};
std::atomic<float> g_renderHalfFovV{0.0f};

// Per-eye FOV: publish the game's FOV, then if fill is on and this view is NARROWER than the target
// (a conversation/cutscene), widen it to fill + publish the target as the submitted FOV. Wide gameplay
// (>= target) is left untouched, so it isn't disturbed.
// [FOVEXIT] Which early return leaves the render FOV at RAW. Alt-tab reproducibly collapses the
// UI ratio to 1.000/1.000 (= 2x vertical UI stretch) and it never recovers; this names the exit
// instead of inferring it. Logs only when the reason CHANGES, so it is a handful of lines.
void FovExit(const char* why) noexcept
{
    static const char* s_last = nullptr;
    if (s_last == why) return;
    s_last = why;
    ME2VR::Log::Line(std::string("[FOVEXIT] render FOV left at RAW, reason=") + why +
                     "  fillOn=" + (g_fovFillOn.load(std::memory_order_acquire) ? "1" : "0") +
                     " th=" + std::to_string(g_fovFillH.load(std::memory_order_relaxed)) +
                     " tv=" + std::to_string(g_fovFillV.load(std::memory_order_relaxed)) +
                     " rawH=" + std::to_string(g_gameRawFovH.load(std::memory_order_relaxed)));
}

void ApplyFov(std::uintptr_t sv) noexcept
{
    PublishGameFov(sv);
    // [FILLSRC] mechanism proof, one-shot: built == written means the engine consumed the pre-written
    // camera FOV (distortion-class constants consistent by construction); built == raw means the write
    // never reached the build and the smear fix is NOT active -- that line is the next clue, not a guess.
    if (g_srcRawValid.load(std::memory_order_relaxed))
    {
        static std::atomic<int> s_fillSrcLogged{0};
        const int n = s_fillSrcLogged.load(std::memory_order_relaxed);
        if (n < 4)
        {
            s_fillSrcLogged.store(n + 1, std::memory_order_relaxed);
            char b[192] = {};
            sprintf_s(b, "[FILLSRC] engine-built hHalf=%.4f written=%.4f raw(src)=%.4f (built==written => fix active)",
                      g_srcBuiltFovH.load(std::memory_order_relaxed),
                      g_srcWrittenFovH.load(std::memory_order_relaxed),
                      g_srcRawFovH.load(std::memory_order_relaxed));
            ME2VR::Log::Line(b);
        }
    }
    // Default: rendered == raw game FOV (any early return below leaves it that way).
    g_renderHalfFovH.store(g_gameRawFovH.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_renderHalfFovV.store(g_gameRawFovV.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (!g_fovFillOn.load(std::memory_order_acquire)) { FovExit("fillOff"); return; }
    const float th = g_fovFillH.load(std::memory_order_relaxed);   // headset fill target (half-FOV, aspect-corr)
    const float tv = g_fovFillV.load(std::memory_order_relaxed);
    if (th < 0.01f || tv < 0.01f) { FovExit("targetZero"); return; }
    const float gh = g_gameRawFovH.load(std::memory_order_relaxed);   // RAW game half-FOV this view
    const float gv = g_gameRawFovV.load(std::memory_order_relaxed);
    if (gh < 0.01f || gv < 0.01f) { FovExit("rawZero"); return; }

    const bool weaponOut = ME2VR::EngineProbe::IsWeaponOut();

    // Capture the gameplay "rest" FOV (hipfire/explore = wide). ADS/sniper narrow it; this holds the last
    // wide value, so (gh/rest) measures how far the game has zoomed in.
    if (gh >= 0.55f) g_restHalfFov.store(gh, std::memory_order_relaxed);
    float rest = g_restHalfFov.load(std::memory_order_relaxed);
    if (rest < 0.45f) rest = 0.61f;   // sane default until captured

    // [CONVOFP] First-person conversation: render AND submit at the headset FOV, flat, no zoom term.
    // The director's cine FOV is ~20 deg half; honouring it puts the whole scene in a 20-deg window
    // inside a ~50-deg headset -- a tiny picture in the middle of the view. The mod is not showing the
    // director's shot any more, so that framing FOV is not the mod's to honour. Taking the gameplay branch
    // below instead would apply the zoom term (gh/rest with a tiny gh = a huge magnifier, ME1's
    // "nearby faces fill the lens" bug), so this returns its own answer: rendered == declared ==
    // headset, exactly like standing in the world.
    if (ME2VR::ConvoFp::IsArmed())
    {
        // Zoom = render NARROWER while DECLARING the same window. The compositor is told a constant
        // FOV, so the 3D magnifies uniformly and the UI is bit-identical to zoom=1. ME1 tried the
        // honest alternative (actually cropping the declared window) and it warped the conversation
        // wheel vertically -- rejected on sight. The declared-vs-rendered gap here is deliberate.
        const float z = ME2VR::ConvoFp::GetZoom();
        const float rH = (z > 1.0f) ? std::atan(std::tan(th) / z) : th;
        const float rV = (z > 1.0f) ? std::atan(std::tan(tv) / z) : tv;
        WidenProjection(sv, rH, rV);
        g_renderHalfFovH.store(rH, std::memory_order_relaxed);
        g_renderHalfFovV.store(rV, std::memory_order_relaxed);
        g_gameHalfFovH.store(th, std::memory_order_relaxed);   // SUBMIT the unzoomed window, always
        g_gameHalfFovV.store(tv, std::memory_order_relaxed);
        return;
    }

    // [VRCINE FILL] In-VR conversations/cutscenes WITHOUT first person (CineVrConvo/CineVrCutscene on,
    // CONVOFP not armed). Same contract as the CONVOFP branch above: the director's per-shot framing
    // FOV is not the mod's to honour in a headset -- honouring it put the whole scene in a ~20-deg window
    // inside a ~100-deg view, and the window RESIZED on every camera cut (each shot has its own FOV).
    // Render AND declare the constant headset window instead, so the scene fills the view and shot cuts
    // change the picture, not the screen. The zoom slider renders NARROWER while declaring the same
    // window: uniform magnification, UI bit-identical (ME1's proven model; the honest declared-crop
    // alternative warped the conversation wheel and was rejected on sight). Raw FOV keeps publishing
    // the director's value, so the cine detector/hysteresis in me2_xr is untouched.
    if (g_vrCineActive.load(std::memory_order_acquire))
    {
        const float z = ME2VR::Me2Xr::GetCineScreenZoom();
        const float rH = (z > 0.01f) ? std::atan(std::tan(th) / z) : th;
        const float rV = (z > 0.01f) ? std::atan(std::tan(tv) / z) : tv;
        WidenProjection(sv, rH, rV);
        g_renderHalfFovH.store(rH, std::memory_order_relaxed);
        g_renderHalfFovV.store(rV, std::memory_order_relaxed);
        g_gameHalfFovH.store(th, std::memory_order_relaxed);   // declare the headset window, always
        g_gameHalfFovV.store(tv, std::memory_order_relaxed);
        return;
    }

    // Narrow + no weapon = a conversation/cutscene -> leave raw so it's detected and goes MONO (flat panel).
    if (gh < 0.59f && !weaponOut) { FovExit("narrowNoWeapon"); return; }

    // Continuous fill + zoom (this is what ME1's working ADS does): the mod always SUBMITS the headset target
    // (th/tv) so the image fills the headset; the RENDER FOV is the headset target at gameplay rest (=> 1:1,
    // gameplay untouched) and shrinks proportionally as the game zooms in below rest (ADS/sniper), so the
    // world MAGNIFIES into the fill. Because the render FOV tracks the game's zoom blend continuously, there
    // is NO pop/flash crossing into ADS. The submitted FOV is constant (headset), so the eye never jumps.
    const float frac = (gh < rest) ? (gh / rest) : 1.0f;   // 1.0 at rest, <1 zoomed in
    const float renderH = th * frac;
    const float renderV = tv * frac;
    WidenProjection(sv, renderH, renderV);
    g_renderHalfFovH.store(renderH, std::memory_order_relaxed);   // [UIRATIO] what this view really rendered
    g_renderHalfFovV.store(renderV, std::memory_order_relaxed);
    g_gameHalfFovH.store(th, std::memory_order_relaxed);   // SUBMIT headset target; render is narrower => zoom-fill
    g_gameHalfFovV.store(tv, std::memory_order_relaxed);
}

bool ReadRect(std::uintptr_t sv, float* r4) noexcept
{
    __try { const volatile float* R = reinterpret_cast<const volatile float*>(sv + kFsvFloatRect);
            r4[0]=R[0]; r4[1]=R[1]; r4[2]=R[2]; r4[3]=R[3]; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string Hex(std::uintptr_t v)
{
    char b[32] = {};
    sprintf_s(b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// SEH-guarded read of the FSceneView fields into plain locals (no C++ objects in the __try).
bool ReadView(std::uintptr_t sv, float* proj16, float* view16, float* origin3) noexcept
{
    __try
    {
        const volatile float* P = reinterpret_cast<const volatile float*>(sv + kFsvProjectionMatrix);
        const volatile float* V = reinterpret_cast<const volatile float*>(sv + kFsvViewMatrix);
        const volatile float* O = reinterpret_cast<const volatile float*>(sv + kFsvViewOrigin);
        for (int i = 0; i < 16; ++i) { proj16[i] = P[i]; view16[i] = V[i]; }
        origin3[0] = O[0]; origin3[1] = O[1]; origin3[2] = O[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// [CONVOFP] POD-only SEH wrapper: write the first-person eye into CalcSceneView's in/out
// ViewLocation / ViewRotation before the engine builds the view. Its own function because the detour
// holds C++ objects and MSVC forbids __try there (C2712) -- same reason ReadView/SfrReplayDrawSEH exist.
// FRotator is three int32 in UE3 units (65536 = 360deg), order Pitch, Yaw, Roll. Pitch and roll are
// zeroed: the director's framing is not the mod's to inherit.
bool ConvoFpWriteSourcePose(void* loc, void* rot, float x, float y, float z,
                            float yawDeg, float pitchDeg) noexcept
{
    __try
    {
        auto* L = reinterpret_cast<volatile float*>(loc);
        L[0] = x; L[1] = y; L[2] = z;
        auto* R = reinterpret_cast<volatile std::int32_t*>(rot);
        R[0] = static_cast<std::int32_t>(pitchDeg * (65536.0f / 360.0f));
        R[1] = static_cast<std::int32_t>(yawDeg * (65536.0f / 360.0f));
        R[2] = 0;   // roll: head-look tracks yaw/pitch only, so this stays level as it always has
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [SFR] POD-only SEH wrapper for the replay Draw (a function using __try may hold no C++ objects
// with destructors - keep this frame POD, like the ReadView pattern above). Returns false on fault.
bool SfrReplayDrawSEH(void* self, void* viewport, void* canvas) noexcept
{
    __try { g_origDraw(self, viewport, canvas); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [SFR] FViewportClient::Draw detour. Pass 0 = the game's own Draw (renders the frame once, -halfEye
// via t_replay=false). In SFR gameplay, run Draw a SECOND time (pass 1, +halfEye) = the whole frame
// re-rendered as a full primary view for the other eye. SEH-guarded; skipped in menu/cinematic (the
// engine's re-entrant-Draw crash territory, per the ME1 lessons). No D3D copies here - M0 only proves
// the double render is tolerated; capture/submit come in M1 on the render thread.
void __fastcall DrawDetour(void* self, void* viewport, void* canvas) noexcept
{
    if (g_origDraw == nullptr) return;
    // [GAMEPLAYVR] Backstop for the whole bug class: the replay is what makes the second eye exist, so
    // refusing it during GAMEPLAY is never correct - it yields a stereo submit built from one render
    // (silent fake stereo, the Archangel bug). me2_xr already clears the cine latch on the mode byte;
    // this enforces the same invariant at the gate itself, so any FUTURE path that latches cinematic
    // (or menu mode) cannot flatten live gameplay. Modes 0-4 are the engine's own gameplay contexts.
    const int gmNow = ME2VR::D3DCapture::GetAutoGameMode();
    const bool engineSaysGameplay = (gmNow >= 0 && gmNow <= 4);
    const bool sfrGameplay =
        g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Sfr &&
        !ME2VR::D3DCapture::GetMenuModeManual() &&   // deliberate flat toggle still wins (don't pay for 2 renders)
        (engineSaysGameplay ||
         (!ME2VR::D3DCapture::GetMenuMode() &&
          !g_cinematic.load(std::memory_order_acquire)));

    t_replay = false;
    // [NOOPRETRY] time pass 0 exactly like pass 1: this is the discriminator for the burst class.
    // Pass 0 also under 100us during a burst = the engine suppressed the WHOLE renderer for those
    // frames (streaming/texture priming); pass 0 at normal cost while pass 1 no-ops = a per-call
    // gate on the second Draw.
    {
        LARGE_INTEGER p0a = {}, p0b = {};
        static LARGE_INTEGER s_p0Qpf = {};
        if (s_p0Qpf.QuadPart == 0) QueryPerformanceFrequency(&s_p0Qpf);
        QueryPerformanceCounter(&p0a);
        g_origDraw(self, viewport, canvas);      // PASS 0 = the game's normal render (-halfEye)
        QueryPerformanceCounter(&p0b);
        const long long p0us = (p0b.QuadPart - p0a.QuadPart) * 1000000ll / s_p0Qpf.QuadPart;
        g_sfrPass0UsLast.store(p0us, std::memory_order_relaxed);
        if (p0us < 100) g_sfrPass0Noops.fetch_add(1, std::memory_order_relaxed);
        else            g_sfrPass0Noops.store(0, std::memory_order_relaxed);
    }
    if (!sfrGameplay)
    {
        // [STEREODEAD] record WHY the second eye was refused, so a submitted "stereo" pair with no
        // fresh pass 1 is a loud logged event instead of a silent flat frame (the Archangel bug class).
        if (g_vrEnabled.load(std::memory_order_acquire) &&
            g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Sfr)
        {
            g_sfrSkipReasons.store((ME2VR::D3DCapture::GetMenuMode() ? 1u : 0u) |
                                   (g_cinematic.load(std::memory_order_acquire) ? 2u : 0u),
                                   std::memory_order_relaxed);
            g_sfrSkipStreak.fetch_add(1, std::memory_order_relaxed);
        }
        else g_sfrSkipStreak.store(0, std::memory_order_relaxed);
        return;
    }

    // [SFR M1] pass-0 capture is driven ENTIRELY on the render thread (d3d_capture draw+clear hooks,
    // keyed off pass 0's backbuffer composite -> next scene clear). The game thread must NOT touch D3D
    // here: an immediate-context copy from this thread raced the render thread = the VR-load crash, and
    // a cross-thread arm flag raced the decoupled render thread = the left-eye flicker.
    t_replay = true;
    LARGE_INTEGER perfT0 = {}, perfT1 = {};                     // [PERF] replay (2nd render) cost
    QueryPerformanceCounter(&perfT0);
    bool ok = SfrReplayDrawSEH(self, viewport, canvas);         // PASS 1 = replay (+halfEye)
    QueryPerformanceCounter(&perfT1);
    t_replay = false;
    {
        static LARGE_INTEGER s_qpf = {};
        if (s_qpf.QuadPart == 0) QueryPerformanceFrequency(&s_qpf);
        long long us = (perfT1.QuadPart - perfT0.QuadPart) * 1000000ll / s_qpf.QuadPart;
        // [NOOPREPLAY] Did pass 1 actually RENDER? Measured 2026-07-31: replays=600/600 with
        // replayAvg=0.0ms - called every frame, rendering nothing, so pass 1 == pass 0 and both eyes
        // carried the same image while every counter read healthy. The scene-view counter does NOT
        // catch it (Draw still builds a view, then skips the render), so COST is the discriminator:
        // a real re-render submits in ~200-400us, a no-op returns in tens of us.
        // [NOOPRETRY] One immediate same-frame retry when the engine declined. If the refusal is a
        // transient per-call state, the retry lands a real second eye and the Bekenstein flicker
        // never reaches the compositor. If the engine is genuinely suppressing rendering, the retry
        // no-ops too at the same tens-of-us cost, and the success counter stays at zero, which is
        // itself the answer. Only ever ONE retry: a burst that persists across the retry persists
        // for the frame, and re-looping the engine's Draw would trade a flicker for a stall.
        if (us < 100)
        {
            g_sfrRetryAttempts.fetch_add(1, std::memory_order_relaxed);
            LARGE_INTEGER r0 = {}, r1 = {};
            QueryPerformanceCounter(&r0);
            t_replay = true;
            const bool okRetry = SfrReplayDrawSEH(self, viewport, canvas);
            t_replay = false;
            QueryPerformanceCounter(&r1);
            const long long rus = (r1.QuadPart - r0.QuadPart) * 1000000ll / s_qpf.QuadPart;
            if (okRetry && rus >= 100)
            {
                g_sfrRetrySuccesses.fetch_add(1, std::memory_order_relaxed);
                ok = okRetry;
                us = rus;   // the retry is the render that counts, for the noop test and the stats
            }
        }
        if (us < 100) g_sfrNoopReplays.fetch_add(1, std::memory_order_relaxed);
        else          g_sfrNoopReplays.store(0, std::memory_order_relaxed);
        g_sfrReplayUsSum.fetch_add(us, std::memory_order_relaxed);
        g_sfrReplayUsCount.fetch_add(1, std::memory_order_relaxed);
        long long prevMax = g_sfrReplayUsMax.load(std::memory_order_relaxed);
        while (us > prevMax && !g_sfrReplayUsMax.compare_exchange_weak(prevMax, us, std::memory_order_relaxed)) {}
    }

    const uint64_t n = g_sfrReplays.fetch_add(1, std::memory_order_relaxed) + 1;
    g_sfrSkipStreak.store(0, std::memory_order_relaxed);   // [STEREODEAD] fresh second eye delivered
    if (!ok) g_sfrReplayFaults.fetch_add(1, std::memory_order_relaxed);
    if (n <= 4 || (n % 600) == 0)
    {
        char b[128] = {};
        sprintf_s(b, "[SFR] Draw replay #%llu ok=%d faults=%llu",
                  static_cast<unsigned long long>(n), ok ? 1 : 0,
                  static_cast<unsigned long long>(g_sfrReplayFaults.load(std::memory_order_relaxed)));
        ME2VR::Log::Line(b);
    }
}

void* __fastcall CalcViewDetour(void* lp, void* family, void* loc, void* rot, void* vp, void* drawer) noexcept
{
    if (g_orig == nullptr) return nullptr;

    const std::uintptr_t lpu = reinterpret_cast<std::uintptr_t>(lp);
    const bool isP1 = (lpu == ME2VR::EngineProbe::GetPrimaryLocalPlayer());
    // [ENGCINE] bumped on EVERY P1 CalcSceneView, ALL branches (ME3's discriminator, ported): a
    // prerendered bik leaves the scene dead (~0 calcviews/s) while an in-engine gm-8 cutscene keeps
    // building views every frame. me2_xr reads this to tell them apart - gm 8 alone can't.
    if (isP1) g_p1CalcSeq.fetch_add(1, std::memory_order_relaxed);
    // [FILLSRC] re-armed each build below; cleared here so a cinematic/menu view entered right after
    // gameplay never inherits a stale source-FOV substitution (that would blind cinematic detection).
    if (isP1) g_srcRawValid.store(false, std::memory_order_relaxed);

    // ---- [CONVOFP] relocate at the SOURCE, before the engine builds anything --------------------
    // loc/rot are CalcSceneView's in/out FVector& ViewLocation and FRotator& ViewRotation. Writing
    // Shepard's eye here means the engine builds the ENTIRE view from it -- matrices, the cached view
    // frustum, visibility culling, occlusion, LOD, streaming -- all consistent by construction.
    //
    // Editing the finished FSceneView afterwards (what this did before) only moves what is DRAWN. The
    // engine had already decided what to draw using the director's camera, so on the player's turn,
    // when the director cuts to Shepard, the person you are talking to falls outside that frustum and
    // most of their components stop being submitted -- while one with different bounds survives. That
    // is the "character vanishes but their legs are still visible" symptom seen on Mordin and Tali.
    //
    // Standing rule in this codebase: hook where the engine WRITES the value, not where it is
    // read. EnsureConvoFpOrigin still runs afterwards and simply finds nothing left to do (its deltas
    // go to zero), so it costs nothing and stays as a backstop if a path ever bypasses this.
    if (isP1 && ME2VR::ConvoFp::IsArmed() && loc != nullptr && rot != nullptr)
    {
        ME2VR::ConvoFp::EyePose pose{};
        if (ME2VR::ConvoFp::GetEyePose(&pose))
        {
            float yawDeg = pose.baseYawDeg;
            if (g_convoFpInvertFacing.load(std::memory_order_relaxed)) yawDeg += 180.0f;

            while (yawDeg > 180.0f) yawDeg -= 360.0f;
            while (yawDeg < -180.0f) yawDeg += 360.0f;
            // Base pose only. Folding head rotation and lean in here was tried and REVERTED: the
            // engine does not build its final view matrix from these parameters, so it bought nothing
            // and (with the render-side path switched off to avoid double-applying) it killed the
            // feature outright. What this write DOES reach is the cull frustum, which is why it stays.
            ConvoFpWriteSourcePose(loc, rot, pose.x, pose.y, pose.z, yawDeg, 0.0f);

            // ---- FOV AT THE SOURCE -------------------------------------------------------------
            // Bisection showed that the glass/refraction smear disappears when headset FOV
            // FILL is turned off. Fill works by overwriting proj[0]/proj[5] AFTER the engine has built
            // the view (WidenProjection), so UE3's distortion pass keeps the projection constants it
            // captured at setup -- it computes its screen-space refraction offsets for a ~20deg cine
            // frustum while the scene renders at ~50. The offsets are wrong by that whole ratio, which
            // is why the smear rides the screen instead of sticking to the surface. Head tracking only
            // made it obvious; it was never the cause, and an earlier build chased the wrong thing.
            //
            // So hand the engine the FOV the mod actually wants and let it build the projection itself.
            // WidenProjection still runs afterwards and now writes the SAME numbers -- zero delta,
            // nothing left stale. If the engine's aspect ever disagrees with the fill's, it still
            // corrects the image and the residual error is tiny instead of the full fill ratio.
            //
            // The old 1.6x cull margin is gone with it: it existed because render-side head rotation
            // was invisible to the engine's frustum, and the rotation is now in ViewRotation above, so
            // the frustum already points where you are looking.
            if (g_fovFillOn.load(std::memory_order_acquire))
            {
                const float th = g_fovFillH.load(std::memory_order_relaxed);
                if (th > 0.05f)
                {
                    const float z = ME2VR::ConvoFp::GetZoom();
                    const float rH = (z > 1.0f) ? std::atan(std::tan(th) / z) : th;
                    float fovDeg = 2.0f * rH * (180.0f / 3.14159265358979f);
                    if (fovDeg > 170.0f) fovDeg = 170.0f;
                    else if (fovDeg < 10.0f) fovDeg = 10.0f;
                    float prevFov = 0.0f;
                    ME2VR::EngineProbe::ConvoFpSetCameraFovDeg(fovDeg, &prevFov);
                }
            }
        }
    }

    // ---- Milestone B1: P1-layout same-frame split ----
    // Rewrite P1's viewport to the LEFT half, build the view; rewrite to the RIGHT half, build a
    // second view into the same family (NULL state for B1 -> renders minus occlusion, never shares
    // P1's state). No eye offset yet (B2). The engine then renders both into an SBS backbuffer.
    // Flat/mono: full-screen menu (F2) OR a conversation/cutscene (16:9 cinematic FOV) -> render ONE
    // full view (skip the split) so it's a clean flat panel, not a squashed/tiny stereo split.
    if (g_vrEnabled.load(std::memory_order_acquire) && isP1 && (ME2VR::D3DCapture::GetMenuMode() || g_cinematic.load(std::memory_order_acquire)))
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr) PublishGameFov(reinterpret_cast<std::uintptr_t>(sv));   // keep FOV live so XR detects cinematic END
        return sv;
    }

    // ---- [FILLSRC] gameplay fill at the SOURCE --------------------------------------------------
    // The glass/refraction smear was bisected to the FOV fill. Fill worked by overwriting
    // proj[0]/proj[5] AFTER the engine had built the view (WidenProjection), so distortion-class
    // passes keep constants they derived at build time for the game's narrow FOV while the scene
    // renders wide -- their screen-space offsets are wrong by the whole ratio and the ghost rides
    // the screen. The CONVOFP branch above already fixes this by writing the camera's cached FOV
    // BEFORE the engine builds (everything derived then agrees by construction); this block is the
    // SAME fix for the gameplay paths (SFR/AER/DIBR/Stereo).
    //
    // The camera FOV is RESTORED when this call returns (the guard below): the game's own HUD
    // projection (crosshair) samples the camera later in the frame and must keep seeing the raw
    // FOV -- that is exactly what [UIRATIO]'s correction assumes. ApplyFov still runs after the
    // build, recomputes the same numbers (delta ~0) and stays the only writer of the published
    // render/submit FOVs; its zoom/rest/narrow logic is mirrored here on the SOURCE-read FOV so
    // behaviour is unchanged -- only WHO builds the projection moved.
    struct FovAtSourceGuard
    {
        bool  active = false;
        float prevDeg = 0.0f;
        ~FovAtSourceGuard() { if (active) ME2VR::EngineProbe::ConvoFpSetCameraFovDeg(prevDeg, nullptr); }
    } fovSrc;
    if (g_vrEnabled.load(std::memory_order_acquire) && isP1 &&
        !ME2VR::ConvoFp::IsArmed() &&                       // CONVOFP already owns the camera FOV
        g_fovFillOn.load(std::memory_order_acquire))
    {
        const int m = g_vrMode.load(std::memory_order_acquire);
        // Mono included 2026-07-26: it now gets real head-look + FOV-fill like every other mode (see the
        // MONO block below), so it needs the same source-side FOV write, or it inherits the exact
        // glass/refraction ghost this pre-write exists to prevent (FILLSRC, commit bee3069).
        const bool gameplayMode = (m == (int)VrMode::Sfr) || (m == (int)VrMode::Aer) ||
                                  (m == (int)VrMode::Dibr) || (m == (int)VrMode::Stereo) ||
                                  (m == (int)VrMode::Mono);
        const float th = g_fovFillH.load(std::memory_order_relaxed);
        float camDeg = 0.0f;
        if (gameplayMode && th > 0.05f && ME2VR::EngineProbe::GetCameraFovDeg(&camDeg))
        {
            const float gh = 0.5f * camDeg * (3.14159265358979f / 180.0f);   // camera FOV is horizontal, degrees
            const bool weaponOut = ME2VR::EngineProbe::IsWeaponOut();
            if (gh >= 0.55f) g_restHalfFov.store(gh, std::memory_order_relaxed);   // same rest capture as ApplyFov
            float rest = g_restHalfFov.load(std::memory_order_relaxed);
            if (rest < 0.45f) rest = 0.61f;
            if (!(gh < 0.59f && !weaponOut))   // narrow + no weapon stays raw -> mono/cinematic detection intact
            {
                const float frac = (gh < rest) ? (gh / rest) : 1.0f;   // ADS zoom-fill, same as ApplyFov
                const float renderH = th * frac;
                float deg = 2.0f * renderH * (180.0f / 3.14159265358979f);
                if (deg > 170.0f) deg = 170.0f; else if (deg < 10.0f) deg = 10.0f;
                if (ME2VR::EngineProbe::ConvoFpSetCameraFovDeg(deg, &fovSrc.prevDeg))
                {
                    fovSrc.active = true;
                    g_srcRawFovH.store(gh, std::memory_order_relaxed);
                    g_srcWrittenFovH.store(renderH, std::memory_order_relaxed);
                    g_srcRawValid.store(true, std::memory_order_relaxed);
                }
            }
        }
    }

    // ---- MONO: a flat panel to both eyes, but the GAME CAMERA still gets head-look + FOV-fill, exactly
    // like every other mode. Until 2026-07-26 this mode had NO block here at all -- Mono fell straight
    // through every gated branch below to the raw g_orig() call at the bottom of this function, the
    // same fallback used for menus and VR-off. So the picture appeared (via the mono-quad presentation),
    // but the game camera was never touched: no head tracking, no lean, no FOV fill, nothing. Structurally
    // identical to the DIBR block below (no eye offset -- there is no second eye to offset).
    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Mono && isP1)
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr)
        {
            const std::uintptr_t svu = reinterpret_cast<std::uintptr_t>(sv);
            InvMatScanTick(svu);
            EnsureConvoFpOrigin(svu);   // [CONVOFP] no ApplyEyeOffset on this path, so relocate here
            ApplyHeadLook(svu);        // head tracking + 6DOF lean
            ApplyFov(svu);             // widen to fill the headset (falls back to raw for convo/cutscene, by design)
            RefreshInverseSlots(svu);  // [INVMAT] keep fog/shaft screen->world matching the edited camera
        }
        return sv;
    }

    // ---- DIBR: render ONE full mono view with head-look + FOV-fill so it fills the headset exactly like
    // AER/stereo (stereo comes from the depth WARP, not a camera offset). Depth is captured off this render. ----
    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Dibr && isP1)
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr)
        {
            const std::uintptr_t svu = reinterpret_cast<std::uintptr_t>(sv);
            InvMatScanTick(svu);  // [INVMAT] pristine-view scan (one-shot; must precede any edit)
            EnsureConvoFpOrigin(svu);   // [CONVOFP] DIBR never calls ApplyEyeOffset, so relocate here
            ApplyHeadLook(svu);   // head tracking + 6DOF (NO eye offset - DIBR stereo is in the warped pixels)
            ApplyFov(svu);        // widen the render to fill the headset + publish the submit FOV (same as AER)
            RefreshInverseSlots(svu);   // [INVMAT] keep fog/shaft screen->world matching the edited camera
        }
        return sv;
    }

    // ---- AER: render ONE offset eye per present (mono panel, disparity across TIME, not space). ----
    // Ported from ME1: build a single full-viewport view (no SBS split), shift the camera by +/-aerHalfEyeUU
    // along camera-right for the currently-armed eye, apply head-look + FOV, then STAMP {eye, seq}. The
    // present loop reads the stamp to capture the fresh eye into a 2-slot history and arm the next eye.
    // NO WriteRect split, NO SyncEyeCamera (one view), NO AllocateViewState (that's a stereo-only need).
    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Aer && isP1)
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr)
        {
            const std::uintptr_t svu = reinterpret_cast<std::uintptr_t>(sv);
            InvMatScanTick(svu);             // [INVMAT] pristine-view scan (one-shot; must precede any edit)
            // [AERSHAKE] The eye comes from THIS build's own seq parity - deterministic L,R,L,R by
            // construction. The old present-armed g_aerRenderEye was a cross-thread feedback loop
            // whose phase flapped with pipeline depth; that flap was the visible shake.
            const uint64_t mySeq = g_aerStampSeq.load(std::memory_order_relaxed) + 1;
            const int   eye  = static_cast<int>(mySeq & 1ull);                   // 0=L, 1=R
            const float he   = g_aerHalfEyeUU.load(std::memory_order_relaxed);
            const float base = (eye == 0) ? -he : +he;                           // 0=L, 1=R (AerEyeSign inline)
            const float signedUU = g_aerSwapEyes.load(std::memory_order_relaxed) ? -base : base;
            ApplyEyeOffset(svu, signedUU);   // IPD ONLY (lean comes from ApplyHeadLook - do NOT double-add)
            ApplyHeadLook(svu);              // offset BEFORE head rotation (same order as stereo below)
            ApplyFov(svu);
            RefreshInverseSlots(svu);        // [INVMAT] keep fog/shaft screen->world matching the edited camera
            // Publish into the FIFO ring: eye first (relaxed), then seq with RELEASE so a reader
            // that sees the seq sees the eye. Legacy single-slot stamp kept live for any old reader.
            AerRingSlot& slot = g_aerRing[mySeq % kAerRingN];
            slot.eye.store(eye, std::memory_order_relaxed);
            slot.seq.store(mySeq, std::memory_order_release);
            g_aerStampEye.store(eye, std::memory_order_relaxed);
            g_aerStampArmed.store(true, std::memory_order_relaxed);
            g_aerStampSeq.store(mySeq, std::memory_order_release); // publish newest (single writer thread)
            g_splitSeq.fetch_add(1, std::memory_order_release);    // keep the shared seq live too (SBS hysteresis compat)
        }
        return sv;
    }

    // ---- SFR (same-frame stereo): render ONE full view here (no SBS split). The DrawDetour replay
    // renders the whole frame a SECOND time this present, so the mod gets two full PRIMARY renders (one per
    // eye) with correct bloom/lighting in BOTH eyes = the ME1 fix. Pass 0 (game's Draw) = -halfEye,
    // pass 1 (replay) = +halfEye, distinguished by the thread-local t_replay. Same edit order as AER. ----
    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Sfr && isP1)
    {
        void* sv = g_orig(lp, family, loc, rot, vp, drawer);
        if (sv != nullptr)
        {
            const std::uintptr_t svu = reinterpret_cast<std::uintptr_t>(sv);
            // PER-EYE VIEW STATE (the right-eye shake/flicker fix, same as Stereo B2a): each pass gets its
            // OWN persistent FSceneViewState. Both passes go through the game's normal Draw, which uses P1's
            // shared state; without this, pass 1 (replay) inherits the temporal history pass 0 JUST wrote
            // (different camera = -halfEye), so TAA/motion-blur/temporal effects smear + shake on the right
            // eye while the left (pass 0, always first) stays consistent. Reuses Stereo's two eye states.
            {
                void*& eyeSt = t_replay ? g_eyeState : g_eyeStateLeft;   // pass1=right state, pass0=left state
                if (eyeSt == nullptr && g_allocViewState != nullptr)
                {
                    void* st = CallAllocViewState();
                    if (Readable(st, 0x40)) eyeSt = st;
                }
                if (eyeSt != nullptr) SetViewState(svu, eyeSt);   // null-safe; never force NULL (that smears too)
            }
            InvMatScanTick(svu);             // [INVMAT] pristine-view scan (one-shot; must precede any edit)
            float he = g_halfEyeUU.load(std::memory_order_relaxed);
            if (g_swapEyes.load(std::memory_order_relaxed)) he = -he;
            const float signedUU = t_replay ? +he : -he;   // pass1 = right (+), pass0 = left (-)
            ApplyEyeOffset(svu, signedUU);   // IPD only (lean comes from ApplyHeadLook)
            ApplyHeadLook(svu);              // offset BEFORE head rotation (same order as stereo/AER)
            ApplyFov(svu);
            // [SFRCONV DECOUPLE 2026-07-20] convergence is NO LONGER applied at render (ported from ME1's
            // final fix). A render-side per-pass projection shift moves the pass-drawn/mirrored HUD along
            // with the scene in one pass but not identically in the other -> the crosshair/interact/health
            // copies split by 2*conv ("UI doubles with convergence"). Convergence now happens at SUBMIT
            // (me2_xr SFR pair copy): each finished eye IMAGE - world and UI already composited together -
            // is shifted horizontally by conv/2. A uniform NDC shift at render and an image shift at submit
            // are the same transform for the world (pixel-identical), but the UI, rendered unshifted in both
            // passes, coincides with itself and can never double. See CopyTextureToEyeFullFrame's shiftPx.
            // ApplyConvergence kept (unused here) for reference; g_sfrConvergence still drives the submit shift.
            RefreshInverseSlots(svu);        // [INVMAT] keep fog/shaft screen->world matching the edit
            g_splitSeq.fetch_add(1, std::memory_order_release);   // keep SBS hysteresis satisfied
        }
        return sv;
    }

    if (g_vrEnabled.load(std::memory_order_acquire) &&
        g_vrMode.load(std::memory_order_acquire) == (int)VrMode::Stereo && isP1)
    {
        WriteRect(lpu, 0.0f, 0.0f, 0.5f, 1.0f);                 // left half
        void* left = g_orig(lp, family, loc, rot, vp, drawer);
        WriteRect(lpu, 0.5f, 0.0f, 0.5f, 1.0f);                 // right half
        void* right = g_orig(lp, family, loc, rot, vp, nullptr);
        WriteRect(lpu, 0.0f, 0.0f, 1.0f, 1.0f);                 // restore full

        // B2a: give EACH eye its OWN persistent FSceneViewState (allocate once, reuse) so motion-blur/
        // temporal effects have valid, private per-eye history. Sharing P1's state corrupts occlusion AND
        // smears temporally: the game keeps repurposing P1's state for its own (mono/menu/HUD) rendering, so
        // an eye that borrows it inherits stale temporal history -> a moving ghost of the level. The right eye
        // already had its own state; the LEFT eye used to borrow P1's -> that was the left-eye ghost. Both eyes
        // now get dedicated states, fully symmetric.
        if (right != nullptr)
        {
            if (g_eyeState == nullptr && g_allocViewState != nullptr)
            {
                void* st = CallAllocViewState();
                if (Readable(st, 0x40)) g_eyeState = st;
                if (!g_eyeStateLogged.exchange(true))
                    ME2VR::Log::Line("[ME2DISC] B2 AllocateViewState(+0x329750)(0) -> " +
                                     Hex(reinterpret_cast<std::uintptr_t>(st)) +
                                     (g_eyeState ? " (eye state ready)" : " (unreadable -> NULL fallback)"));
            }
            SetViewState(reinterpret_cast<std::uintptr_t>(right), g_eyeState);   // null-safe
        }
        if (left != nullptr)
        {
            if (g_eyeStateLeft == nullptr && g_allocViewState != nullptr)
            {
                void* st = CallAllocViewState();
                if (Readable(st, 0x40)) g_eyeStateLeft = st;
                if (!g_eyeStateLeftLogged.exchange(true))
                    ME2VR::Log::Line("[ME2DISC] B2 AllocateViewState LEFT -> " +
                                     Hex(reinterpret_cast<std::uintptr_t>(st)) +
                                     (g_eyeStateLeft ? " (left eye state ready)" : " (unreadable -> NULL fallback)"));
            }
            // Only override once the mod actually has a dedicated state; never force NULL (that itself smears).
            if (g_eyeStateLeft != nullptr) SetViewState(reinterpret_cast<std::uintptr_t>(left), g_eyeStateLeft);
        }

        // Derive BOTH eyes from the LEFT eye's camera so a camera transition can't make them diverge
        // ("snapping between two cameras"). Right keeps its own projection (per-half) + its own view-state.
        if (left != nullptr && right != nullptr)
            SyncEyeCamera(reinterpret_cast<std::uintptr_t>(right), reinterpret_cast<std::uintptr_t>(left));

        // B2b: real parallax - shift each view's camera half-IPD along camera-right (left -, right +).
        // Eye offset BEFORE head rotation (body stays stable, doesn't swim with head-look).
        float he = g_halfEyeUU.load(std::memory_order_relaxed);
        if (g_swapEyes.load(std::memory_order_relaxed)) he = -he;
        if (left != nullptr)
        {
            InvMatScanTick(reinterpret_cast<std::uintptr_t>(left));   // [INVMAT] pristine scan (pre-edit)
            ApplyEyeOffset(reinterpret_cast<std::uintptr_t>(left), -he);
            ApplyHeadLook(reinterpret_cast<std::uintptr_t>(left));
            ApplyFov(reinterpret_cast<std::uintptr_t>(left));    // floor-fill narrow (convo) FOV
            RefreshInverseSlots(reinterpret_cast<std::uintptr_t>(left));   // [INVMAT] after ALL edits
        }
        if (right != nullptr)
        {
            ApplyEyeOffset(reinterpret_cast<std::uintptr_t>(right), +he);
            ApplyHeadLook(reinterpret_cast<std::uintptr_t>(right));
            ApplyFov(reinterpret_cast<std::uintptr_t>(right));
            RefreshInverseSlots(reinterpret_cast<std::uintptr_t>(right));  // [INVMAT] after ALL edits
        }

        if (g_splitLogged.load(std::memory_order_acquire) < 3)
        {
            g_splitLogged.fetch_add(1, std::memory_order_acq_rel);
            char buf[256] = {};
            sprintf_s(buf, "[ME2DISC] B split: left=%p right=%p twoDistinct=%d eyeState=%p",
                      left, right, (left && right && left != right) ? 1 : 0, g_eyeState);
            ME2VR::Log::Line(buf);
        }
        // DIAG: during a held FP blend, log the LEFT eye's real ViewOrigin (post-build) so the mod can see if the
        // camera escapes the FP position (e.g. high Z = "above Shepard"). Only logs during the blend -> no spam.
        if (left != nullptr && ME2VR::EngineProbe::IsFpBlend())
        {
            static unsigned s_bd = 0;
            if ((s_bd++ % 3) == 0)
            {
                float p16[16] = {}, v16[16] = {}, o3[3] = {};
                if (ReadView(reinterpret_cast<std::uintptr_t>(left), p16, v16, o3))
                {
                    char bb[160] = {};
                    sprintf_s(bb, "[ME2BLEND] BLEND ViewOrigin=(%.0f,%.0f,%.0f) rawFovH=%.3f",
                              o3[0], o3[1], o3[2], g_gameRawFovH.load(std::memory_order_relaxed));
                    ME2VR::Log::Line(bb);
                }
            }
        }
        g_splitSeq.fetch_add(1, std::memory_order_release);   // mark: this frame produced an SBS pair
        return left;   // hand the engine the left view as the "primary"
    }

    void* sv = g_orig(lp, family, loc, rot, vp, drawer);
    // DIBR/mono fall-through: keep the game's rendered FOV live so the XR side declares it correctly
    // (DIBR renders a plain mono frame here - no AER/stereo branch publishes the FOV for it).
    if (sv != nullptr && isP1) PublishGameFov(reinterpret_cast<std::uintptr_t>(sv));

    // [CONVOFP] FLAT-MODE path. Every VR branch above is gated on g_vrEnabled, so with VR off nothing
    // would ever relocate the camera. First person was originally tuned flat on this game for exactly
    // this reason -- it is far easier to judge on a monitor -- so drive the plain mono view here too.
    // Only when VR is OFF: with VR on the branches above already handled their own views.
    if (sv != nullptr && isP1 && !g_vrEnabled.load(std::memory_order_acquire))
        EnsureConvoFpOrigin(reinterpret_cast<std::uintptr_t>(sv));

    if (sv != nullptr && g_logged.load(std::memory_order_acquire) < 4)
    {
        float proj[16] = {}, view[16] = {}, origin[3] = {};
        if (ReadView(reinterpret_cast<std::uintptr_t>(sv), proj, view, origin))
        {
            const bool perspective = (proj[11] > 0.99f && proj[11] < 1.01f) &&
                                     (proj[15] > -0.01f && proj[15] < 0.01f) &&
                                     proj[0] > 0.0001f && proj[5] > 0.0001f;
            g_logged.fetch_add(1, std::memory_order_acq_rel);
            char buf[512] = {};
            sprintf_s(buf,
                      "[ME2DISC] CalcSceneView HOOK: this=%p isP1=%d sv=%p perspective=%d "
                      "proj[0]=%.4f proj[5]=%.4f proj[10]=%.4f proj[11]=%.2f proj[14]=%.3f proj[15]=%.2f",
                      lp, isP1 ? 1 : 0, sv, perspective ? 1 : 0,
                      proj[0], proj[5], proj[10], proj[11], proj[14], proj[15]);
            ME2VR::Log::Line(buf);
            char buf2[512] = {};
            sprintf_s(buf2,
                      "[ME2DISC]   ViewOrigin=(%.1f, %.1f, %.1f)  viewRow0=(%.3f %.3f %.3f) [camera-right]",
                      origin[0], origin[1], origin[2], view[0], view[4], view[8]);
            ME2VR::Log::Line(buf2);
            if (g_logged.load() == 1)
            {
                ME2VR::Log::Line(std::string("[ME2DISC]   VERDICT: ") +
                                 (perspective && isP1
                                      ? "CONFIRMED CalcSceneView + FSceneView offsets VALID (Proj@0xD0/View@0x90/Origin@0x320). Stage 4 done."
                                      : "unexpected -- review values above"));
            }
        }
    }
    return sv;
}
}

namespace ME2VR::CalcViewHook
{
void SetFovFill(float hHalfRad, float vHalfRad, bool on) noexcept
{
    g_fovFillH.store(hHalfRad, std::memory_order_relaxed);
    g_fovFillV.store(vHalfRad, std::memory_order_relaxed);
    g_fovFillOn.store(on, std::memory_order_release);
}
void SetCinematic(bool on) noexcept { g_cinematic.store(on, std::memory_order_release); }
bool GetCinematic() noexcept { return g_cinematic.load(std::memory_order_acquire); }
// [VRCINE] experimental toggles + runtime state.
bool GetCineVrConvo() noexcept { return g_cineVrConvo.load(std::memory_order_acquire); }
void SetCineVrConvo(bool on) noexcept { g_cineVrConvo.store(on, std::memory_order_release); }
bool GetCineVrCutscene() noexcept { return g_cineVrCutscene.load(std::memory_order_acquire); }
void SetCineVrCutscene(bool on) noexcept { g_cineVrCutscene.store(on, std::memory_order_release); }
bool GetCineVrHeadTracking() noexcept { return g_cineVrHeadTracking.load(std::memory_order_acquire); }

// [CONVOFP] Bump once per present, before any view is processed: it is what makes the relocation
// idempotent per view per frame (see EnsureConvoFpOrigin).
void ConvoFpBeginFrame() noexcept { g_convoFpFrame.fetch_add(1, std::memory_order_relaxed); }
void SetConvoFpInvertFacing(bool on) noexcept { g_convoFpInvertFacing.store(on, std::memory_order_relaxed); }
bool GetConvoFpInvertFacing() noexcept { return g_convoFpInvertFacing.load(std::memory_order_relaxed); }
void GetConvoFpCounts(unsigned long long* applies, unsigned long long* skips) noexcept
{
    if (applies != nullptr) *applies = g_convoFpApplies.load(std::memory_order_relaxed);
    if (skips != nullptr)   *skips   = g_convoFpSkips.load(std::memory_order_relaxed);
}
void SetCineVrHeadTracking(bool on) noexcept { g_cineVrHeadTracking.store(on, std::memory_order_release); }
bool GetVrCineActive() noexcept { return g_vrCineActive.load(std::memory_order_acquire); }
void SetVrCineActive(bool on) noexcept { g_vrCineActive.store(on, std::memory_order_release); }
unsigned GetP1CalcSeq() noexcept { return g_p1CalcSeq.load(std::memory_order_relaxed); }   // [ENGCINE]
bool GetEngCineAlive() noexcept { return g_engCineAlive.load(std::memory_order_acquire); }
void SetEngCineAlive(bool on) noexcept { g_engCineAlive.store(on, std::memory_order_release); }
int  GetCineEnterFrames() noexcept { return g_cineEnterFrames.load(std::memory_order_relaxed); }
void SetCineEnterFrames(int f) noexcept { g_cineEnterFrames.store(f < 0 ? 0 : (f > 60 ? 60 : f), std::memory_order_relaxed); }
void GetSfrSkipInfo(unsigned* streak, unsigned* reasons) noexcept
{
    if (streak)  *streak  = g_sfrSkipStreak.load(std::memory_order_relaxed);
    if (reasons) *reasons = g_sfrSkipReasons.load(std::memory_order_relaxed);
}
unsigned GetSfrNoopReplays() noexcept { return g_sfrNoopReplays.load(std::memory_order_relaxed); }
void GetSfrNoopDiag(long long* pass0UsLast, unsigned* pass0Noops,
                    unsigned* retryAttempts, unsigned* retrySuccesses) noexcept
{
    if (pass0UsLast != nullptr)    *pass0UsLast    = g_sfrPass0UsLast.load(std::memory_order_relaxed);
    if (pass0Noops != nullptr)     *pass0Noops     = g_sfrPass0Noops.load(std::memory_order_relaxed);
    if (retryAttempts != nullptr)  *retryAttempts  = g_sfrRetryAttempts.load(std::memory_order_relaxed);
    if (retrySuccesses != nullptr) *retrySuccesses = g_sfrRetrySuccesses.load(std::memory_order_relaxed);
}
void SetVrEnabled(bool on) noexcept { g_vrEnabled.store(on, std::memory_order_release); }
bool GetVrEnabled() noexcept { return g_vrEnabled.load(std::memory_order_acquire); }
float GetGameRawFovH() noexcept { return g_gameRawFovH.load(std::memory_order_relaxed); }
float GetGameRawFovV() noexcept { return g_gameRawFovV.load(std::memory_order_relaxed); }
float GetGameHalfFovH() noexcept { return g_gameHalfFovH.load(std::memory_order_relaxed); }
float GetGameHalfFovV() noexcept { return g_gameHalfFovV.load(std::memory_order_relaxed); }

uint64_t GetSplitSeq() noexcept { return g_splitSeq.load(std::memory_order_acquire); }

void SetHeadLook(int32_t yawUU, int32_t pitchUU, bool enabled) noexcept
{
    g_headYawUU.store(yawUU, std::memory_order_relaxed);
    g_headPitchUU.store(pitchUU, std::memory_order_relaxed);
    g_headLookEnabled.store(enabled, std::memory_order_release);
}

// Head-look tuning accessors (ME1 parity: smoothing kills the shake, sensitivity + invert per axis).
bool  GetHeadLookUserEnabled() noexcept { return g_headLookUserEnabled.load(std::memory_order_relaxed); }
void  SetHeadLookUserEnabled(bool v) noexcept { g_headLookUserEnabled.store(v, std::memory_order_relaxed); }
float GetHeadLookSmoothing() noexcept { return g_headLookSmoothing.load(std::memory_order_relaxed); }
void  SetHeadLookSmoothing(float v) noexcept { g_headLookSmoothing.store((v < 0.0f) ? 0.0f : (v > 0.95f ? 0.95f : v), std::memory_order_relaxed); }
float GetLookSensitivity() noexcept { return g_lookSensitivity.load(std::memory_order_relaxed); }
void  SetLookSensitivity(float v) noexcept { g_lookSensitivity.store((v < 0.1f) ? 0.1f : (v > 3.0f ? 3.0f : v), std::memory_order_relaxed); }
bool  GetInvertLookYaw() noexcept { return g_invertLookYaw.load(std::memory_order_relaxed); }
void  SetInvertLookYaw(bool v) noexcept { g_invertLookYaw.store(v, std::memory_order_relaxed); }
bool  GetInvertLookPitch() noexcept { return g_invertLookPitch.load(std::memory_order_relaxed); }
void  SetInvertLookPitch(bool v) noexcept { g_invertLookPitch.store(v, std::memory_order_relaxed); }

void SetHeadPos(float x, float y, float z) noexcept
{
    g_headPosX.store(x, std::memory_order_relaxed);
    g_headPosY.store(y, std::memory_order_relaxed);
    g_headPosZ.store(z, std::memory_order_relaxed);
}
void SetHeadPosEnabled(bool on) noexcept { g_headPosOn.store(on, std::memory_order_release); }
bool GetHeadPosEnabled() noexcept { return g_headPosOn.load(std::memory_order_acquire); }
void SetHeadPosScale(float s) noexcept { g_headPosScale.store(s, std::memory_order_relaxed); }
float GetHeadPosScale() noexcept { return g_headPosScale.load(std::memory_order_relaxed); }
void SetLeanInvertFwd(bool on) noexcept { g_leanInvertFwd.store(on, std::memory_order_relaxed); }
bool GetLeanInvertFwd() noexcept { return g_leanInvertFwd.load(std::memory_order_relaxed); }

void LogPosDiag() noexcept   // throttled diagnostic: head input + camera basis + world delta (no __try here)
{
    static int n = 0;
    if ((n++ % 30) != 0) return;
    char b[256] = {};
    sprintf_s(b, "[ME2POS] head(m)=(%.3f,%.3f,%.3f) right=(%.2f,%.2f,%.2f) up=(%.2f,%.2f,%.2f) fwd=(%.2f,%.2f,%.2f) world=(%.1f,%.1f,%.1f)",
              g_headPosX.load(), g_headPosY.load(), g_headPosZ.load(),
              g_diagBasis[0], g_diagBasis[1], g_diagBasis[2], g_diagBasis[3], g_diagBasis[4], g_diagBasis[5],
              g_diagBasis[6], g_diagBasis[7], g_diagBasis[8], g_diagWorld[0], g_diagWorld[1], g_diagWorld[2]);
    ME2VR::Log::Line(b);
}

float GetHalfEyeUU() noexcept { return g_halfEyeUU.load(std::memory_order_relaxed); }
void  SetHalfEyeUU(float v) noexcept { g_halfEyeUU.store(v, std::memory_order_relaxed); }
// [SFR] convergence + HUD scale (SFR-only tunables).
float GetSfrConvergence() noexcept { return g_sfrConvergence.load(std::memory_order_relaxed); }
void  SetSfrConvergence(float v) noexcept { g_sfrConvergence.store(v, std::memory_order_relaxed); }
float GetSfrUiScale() noexcept { return g_sfrUiScale.load(std::memory_order_relaxed); }
void  SetSfrUiScale(float v) noexcept { g_sfrUiScale.store((v < 0.3f) ? 0.3f : (v > 1.0f ? 1.0f : v), std::memory_order_relaxed); }
static float ClampScale(float v) noexcept { return (v < 0.2f) ? 0.2f : (v > 2.0f ? 2.0f : v); }
static float ClampOff(float v)   noexcept { return (v < -0.5f) ? -0.5f : (v > 0.5f ? 0.5f : v); }
float GetSfrUiScaleX() noexcept { return g_sfrUiScaleX.load(std::memory_order_relaxed); }
void  SetSfrUiScaleX(float v) noexcept { g_sfrUiScaleX.store(ClampScale(v), std::memory_order_relaxed); }
float GetSfrUiScaleY() noexcept { return g_sfrUiScaleY.load(std::memory_order_relaxed); }
void  SetSfrUiScaleY(float v) noexcept { g_sfrUiScaleY.store(ClampScale(v), std::memory_order_relaxed); }
float GetSfrUiOffX() noexcept { return g_sfrUiOffX.load(std::memory_order_relaxed); }
void  SetSfrUiOffX(float v) noexcept { g_sfrUiOffX.store(ClampOff(v), std::memory_order_relaxed); }
float GetSfrUiOffY() noexcept { return g_sfrUiOffY.load(std::memory_order_relaxed); }
void  SetSfrUiOffY(float v) noexcept { g_sfrUiOffY.store(ClampOff(v), std::memory_order_relaxed); }
bool  GetSwapEyes() noexcept { return g_swapEyes.load(std::memory_order_relaxed); }
void  SetSwapEyes(bool v) noexcept { g_swapEyes.store(v, std::memory_order_relaxed); }

// --- VR mode + AER accessors (2026-07-04 AER port from ME1). ---
int  GetVrMode() noexcept { return g_vrMode.load(std::memory_order_acquire); }
void SetVrMode(int m) noexcept { g_vrMode.store(m, std::memory_order_release); }

// [MOVEFIX/AIMSEED] The APPLIED render-side head-look offset (0 when look is off). The XInput movement
// steer and the look->aim handoff seed both read these - they must be the EXACT applied values.
// [PERF] drain the replay-cost accumulators (avg/max us since last call).
void GetSfrReplayStats(long long* avgUs, long long* maxUs, long long* count) noexcept
{
    const long long n = g_sfrReplayUsCount.exchange(0, std::memory_order_relaxed);
    const long long sum = g_sfrReplayUsSum.exchange(0, std::memory_order_relaxed);
    const long long mx = g_sfrReplayUsMax.exchange(0, std::memory_order_relaxed);
    if (count) *count = n;
    if (avgUs) *avgUs = (n > 0) ? (sum / n) : 0;
    if (maxUs) *maxUs = mx;
}

// [UIRATIO] the rendered half-FOV (== raw game FOV when fill is off/inactive).
float GetRenderHalfFovH() noexcept { return g_renderHalfFovH.load(std::memory_order_relaxed); }
float GetRenderHalfFovV() noexcept { return g_renderHalfFovV.load(std::memory_order_relaxed); }

float GetPoseTagDelayFrames() noexcept { return g_poseTagDelayFrames.load(std::memory_order_relaxed); }
void  SetPoseTagDelayFrames(float v) noexcept { g_poseTagDelayFrames.store((v < 0.0f) ? 0.0f : (v > 3.0f ? 3.0f : v), std::memory_order_relaxed); }

int HeadLookYawUU() noexcept
{ return g_headLookEnabled.load(std::memory_order_acquire) ? g_headYawUU.load(std::memory_order_relaxed) : 0; }
int HeadLookPitchUU() noexcept
{ return g_headLookEnabled.load(std::memory_order_acquire) ? g_headPitchUU.load(std::memory_order_relaxed) : 0; }
float GetAerHalfEyeUU() noexcept { return g_aerHalfEyeUU.load(std::memory_order_relaxed); }
void  SetAerHalfEyeUU(float v) noexcept { g_aerHalfEyeUU.store(v, std::memory_order_relaxed); }
bool  GetAerSwapEyes() noexcept { return g_aerSwapEyes.load(std::memory_order_relaxed); }
void  SetAerSwapEyes(bool v) noexcept { g_aerSwapEyes.store(v, std::memory_order_relaxed); }
bool  GetAerFramePacing() noexcept { return g_aerFramePacing.load(std::memory_order_relaxed); }
void  SetAerFramePacing(bool v) noexcept { g_aerFramePacing.store(v, std::memory_order_relaxed); }
int   GetAerFramePacingHz() noexcept { return g_aerFramePacingHz.load(std::memory_order_relaxed); }
void  SetAerFramePacingHz(int v) noexcept { g_aerFramePacingHz.store(v, std::memory_order_relaxed); }
// [PACE1X] Present cadence for SFR/Stereo. Default full: those modes put BOTH eyes in one present,
// so they never needed the half-rate eye-swap cadence that AER requires.
std::atomic_bool g_fullRefreshPacing{true};
bool GetFullRefreshPacing() noexcept { return g_fullRefreshPacing.load(std::memory_order_relaxed); }
void SetFullRefreshPacing(bool v) noexcept { g_fullRefreshPacing.store(v, std::memory_order_relaxed); }
bool  GetStereoFramePacing() noexcept { return g_stereoFramePacing.load(std::memory_order_relaxed); }
void  SetStereoFramePacing(bool v) noexcept { g_stereoFramePacing.store(v, std::memory_order_relaxed); }
// --- DIBR config accessors ---
float GetDepthWarpGain() noexcept { return g_depthWarpGain.load(std::memory_order_relaxed); }
void  SetDepthWarpGain(float v) noexcept { g_depthWarpGain.store(v, std::memory_order_relaxed); }
float GetDepthWarpConv() noexcept { return g_depthWarpConv.load(std::memory_order_relaxed); }
void  SetDepthWarpConv(float v) noexcept { g_depthWarpConv.store(v, std::memory_order_relaxed); }
bool  GetDepthWarpFlip() noexcept { return g_depthWarpFlip.load(std::memory_order_relaxed); }
void  SetDepthWarpFlip(bool v) noexcept { g_depthWarpFlip.store(v, std::memory_order_relaxed); }
bool  GetDibrAutoConverge() noexcept { return g_dibrAutoConverge.load(std::memory_order_relaxed); }
void  SetDibrAutoConverge(bool v) noexcept { g_dibrAutoConverge.store(v, std::memory_order_relaxed); }
bool  GetInvMatrixFix() noexcept { return g_invFixEnabled.load(std::memory_order_relaxed); }
void  SetInvMatrixFix(bool v) noexcept { g_invFixEnabled.store(v, std::memory_order_relaxed); }
int   GetDibrResX() noexcept { return g_dibrResX.load(std::memory_order_relaxed); }
void  SetDibrResX(int v) noexcept { g_dibrResX.store(v, std::memory_order_relaxed); }
int   GetDibrResY() noexcept { return g_dibrResY.load(std::memory_order_relaxed); }
void  SetDibrResY(int v) noexcept { g_dibrResY.store(v, std::memory_order_relaxed); }
int  GetAerRenderEye() noexcept { return g_aerRenderEye.load(std::memory_order_acquire); }
void SetAerRenderEye(int e) noexcept { g_aerRenderEye.store(e, std::memory_order_release); }
// [AERSHAKE] Consume the build AFTER lastSeq, oldest-first - one per present, matching swapchain
// delivery order, so the eye label always belongs to the pixels on screen. Returns false when no
// newer build exists. If the wanted slot was already overwritten (consumer fell a full ring behind:
// mode switch, long menu, load), resync to the newest build and say so via *resynced.
bool ConsumeAerStamp(unsigned long long lastSeq, unsigned long long* outSeq, int* outEye, bool* resynced) noexcept
{
    *resynced = false;
    const uint64_t newest = g_aerStampSeq.load(std::memory_order_acquire);
    if (newest <= lastSeq) return false;                      // nothing new this present
    uint64_t want = lastSeq + 1;
    const AerRingSlot* s = &g_aerRing[want % kAerRingN];
    if (s->seq.load(std::memory_order_acquire) != want)
    {
        want = newest;                                        // fell behind the ring: jump to newest
        *resynced = true;
        s = &g_aerRing[want % kAerRingN];
        if (s->seq.load(std::memory_order_acquire) != want) return false;   // slot mid-write; next present
    }
    *outSeq = want;
    *outEye = s->eye.load(std::memory_order_relaxed);
    return true;
}
// Combined stamp read - BYTE-IDENTICAL to ME1 GetLastRenderStamp (render_hook.cpp:1944-1953): read the
// dedicated AER seq FIRST with ACQUIRE (synchronizes-with the detour's release bump), THEN the eye with
// RELAXED - this guarantees the eye you read is the one published before that seq generation, so the present
// loop can never capture the backbuffer into the wrong history slot. Gated on the ARMED flag (a stale eye
// from a prior AER session can't read as valid). Was: eye-first + shared g_splitSeq (inverted handshake).
bool GetAerStamp(uint64_t* seq, int* eye) noexcept {
    if (!g_aerStampArmed.load(std::memory_order_acquire)) return false;
    const uint64_t s = g_aerStampSeq.load(std::memory_order_acquire);   // seq FIRST (acquire) - matches ME1
    const int e = g_aerStampEye.load(std::memory_order_relaxed);        // eye AFTER, published by the seq release
    if (seq) *seq = s;
    if (eye) *eye = e;
    return e >= 0 && e <= 1;
}

void Tick() noexcept
{
    if (g_installed.load(std::memory_order_acquire)) return;

    // Only install once a local player exists (engine is up).
    if (ME2VR::EngineProbe::GetPrimaryLocalPlayer() == 0) return;

    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_base == 0) return;

    g_allocViewState = reinterpret_cast<AllocViewStateFn>(g_base + kAllocateViewStateRva);

    MH_Initialize();   // idempotent; harmless if already initialized
    void* target = reinterpret_cast<void*>(g_base + kCalcSceneViewRva);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&CalcViewDetour),
                      reinterpret_cast<void**>(&g_orig)) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
    {
        g_installed.store(true, std::memory_order_release);
        ME2VR::Log::Line("[ME2DISC] CalcViewHook: installed at MassEffect2.exe+" + Hex(kCalcSceneViewRva));
    }
    else
    {
        ME2VR::Log::Line("[ME2DISC] CalcViewHook: MH_CreateHook/EnableHook FAILED at +" + Hex(kCalcSceneViewRva));
    }

    // [SFR] FViewportClient::Draw hook (the double-render replay). MinHook is already initialized above;
    // this detour is inert unless the SFR mode is selected, so it is always safe to install.
    void* drawTarget = reinterpret_cast<void*>(g_base + kDrawRva);
    if (MH_CreateHook(drawTarget, reinterpret_cast<void*>(&DrawDetour),
                      reinterpret_cast<void**>(&g_origDraw)) == MH_OK &&
        MH_EnableHook(drawTarget) == MH_OK)
    {
        ME2VR::Log::Line("[SFR] Draw hook installed at MassEffect2.exe+" + Hex(kDrawRva));
    }
    else
    {
        ME2VR::Log::Line("[SFR] Draw hook MH_CreateHook/EnableHook FAILED at +" + Hex(kDrawRva));
    }
}
}
