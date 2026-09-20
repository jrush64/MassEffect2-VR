#include "engine_probe.h"

#include "calcview_hook.h"   // [AIMSEED] HeadLookYawUU/PitchUU (applied render-side look) for the look->aim handoff
#include "logger.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
// --- LE2 engine globals / struct offsets ----------------------------------
// RVAs are from the ME3Tweaks unified SDK (LExSDKv2 Init.hpp, SDK_TARGET_LE2 block).
// If GEngine derefs to garbage on this build, the installed exe differs from the
// SDK's target build and the mod falls back to a pattern scan -- the log will say so.
constexpr std::uintptr_t kGEngineRva = 0x175ef28;   // UEngine* GEngine
constexpr std::uintptr_t kGObjectsRva = 0x173cc48;  // (catalogued for later)

// Offsets are confirmed byte-identical to LE1/LE3 from the SDK class layouts.
constexpr std::uintptr_t kEngineGamePlayers = 0x498;   // UGameEngine: TArray<ULocalPlayer*>
constexpr std::uintptr_t kLpViewState = 0x46C;
constexpr std::uintptr_t kLpViewportClient = 0x594;
constexpr std::uintptr_t kLpOrigin = 0x59C;   // FVector2D (2 floats)
constexpr std::uintptr_t kLpSize = 0x5A4;     // FVector2D (2 floats)
constexpr std::uintptr_t kLpControllerId = 0x5B4;   // int
constexpr std::uintptr_t kVpcFViewportClientVtable = 0x60;   // UGameViewportClient: FViewportClient vtable
constexpr int kDrawVtableSlot = 2;            // FViewportClient::Draw(FViewport*, FCanvas*)

// UE3 TArray<T> = { T* Data; int Num; int Max; }
constexpr std::uintptr_t kTArrayData = 0x0;
constexpr std::uintptr_t kTArrayNum = 0x8;

std::atomic_bool g_dumped{false};
std::atomic<unsigned long long> g_waitLogs{0};

// All raw reads of possibly-invalid game pointers go through these SEH-guarded helpers.
// Keep them free of C++ objects so the compiler accepts __try here.
bool SafeReadPtr(std::uintptr_t addr, std::uintptr_t* out) noexcept
{
    __try
    {
        *out = *reinterpret_cast<volatile std::uintptr_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SafeReadInt(std::uintptr_t addr, int* out) noexcept
{
    __try
    {
        *out = *reinterpret_cast<volatile int*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SafeReadFloat2(std::uintptr_t addr, float* out2) noexcept
{
    __try
    {
        out2[0] = *reinterpret_cast<volatile float*>(addr);
        out2[1] = *reinterpret_cast<volatile float*>(addr + 4);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::string Hex(std::uintptr_t v)
{
    char buf[32] = {};
    sprintf_s(buf, "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

// --- UObject reflection (LE2): resolve an object's class name from memory ------------------
// SFXName name pool (GBioNamePools) at MassEffect2.exe + 0x1668A10 (array of chunk base pointers).
// SFXName(8B): first DWORD = Offset:29 | Chunk:3. Entry = chunk[Offset]; ANSI string at +12.
// UObject.Class@0x50, UClass.Name@0x48 (byte-identical core layout across LE).
constexpr std::uintptr_t kNamePoolsRva = 0x1668A10;
constexpr std::uintptr_t kObjClass = 0x50;
constexpr std::uintptr_t kObjNameOff = 0x48;

bool ReadSfxName(unsigned long long sfxname, char* out, int outSize) noexcept
{
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const unsigned int packed = static_cast<unsigned int>(sfxname & 0xFFFFFFFFull);
        const unsigned int offset = packed & 0x1FFFFFFFu;
        const unsigned int chunk = (packed >> 29) & 0x7u;
        auto* pools = reinterpret_cast<unsigned char* volatile*>(base + kNamePoolsRva);
        unsigned char* pool = pools[chunk];
        if (pool == nullptr) return false;
        const char* ansi = reinterpret_cast<const char*>(pool + offset + 12);
        int i = 0;
        for (; i < outSize - 1 && ansi[i] >= 32 && ansi[i] < 127; ++i) out[i] = ansi[i];
        out[i] = 0;
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ClassNameOf(std::uintptr_t obj, char* out, int outSize) noexcept
{
    if (obj < 0x10000) return false;
    std::uintptr_t cls = 0;
    if (!SafeReadPtr(obj + kObjClass, &cls) || cls < 0x10000) return false;
    unsigned long long nm = 0;
    __try { nm = *reinterpret_cast<unsigned long long volatile*>(cls + kObjNameOff); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return ReadSfxName(nm, out, outSize);
}

// True if an AActor is being destroyed: bDeleteMe (bit 0x08 @0x278) or bPendingDelete (bit 0x00400000
// @0x27C). The game sets these the instant a save/level load begins tearing the actor down, BEFORE the
// memory is freed - so checking it lets the mod stop writing to the pawn before the dying-pawn race crashes.
// Unreadable -> treated as dying (skip). SEH-safe on garbage/freed pointers.
bool ActorIsDying(std::uintptr_t actor) noexcept
{
    if (actor < 0x10000) return true;
    __try
    {
        if (*reinterpret_cast<std::uint32_t volatile*>(actor + 0x278) & 0x00000008u) return true;  // bDeleteMe
        if (*reinterpret_cast<std::uint32_t volatile*>(actor + 0x27C) & 0x00400000u) return true;  // bPendingDelete
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
}

namespace ME2VR::EngineProbe
{
std::uintptr_t GetPrimaryLocalPlayer() noexcept
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) return 0;
    std::uintptr_t gEngine = 0;
    if (!SafeReadPtr(base + kGEngineRva, &gEngine) || gEngine == 0) return 0;
    std::uintptr_t playersData = 0;
    int playersNum = 0;
    if (!SafeReadPtr(gEngine + kEngineGamePlayers + kTArrayData, &playersData) ||
        !SafeReadInt(gEngine + kEngineGamePlayers + kTArrayNum, &playersNum))
        return 0;
    if (playersData == 0 || playersNum < 1 || playersNum > 8) return 0;
    std::uintptr_t p1 = 0;
    if (!SafeReadPtr(playersData, &p1)) return 0;
    return p1;
}

bool IsGamePaused() noexcept
{
    // P1 ULocalPlayer -> Actor(0x68, PlayerController) -> WorldInfo(0x1B0) -> Pauser(0x684).
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return false;
    std::uintptr_t pc = 0, wi = 0, pauser = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc == 0) return false;
    if (!SafeReadPtr(pc + 0x1B0, &wi) || wi == 0) return false;
    if (!SafeReadPtr(wi + 0x684, &pauser)) return false;
    return pauser != 0;
}

// Engine game-mode context (ME1's ReadGameModeSEH, LE2 offsets). The engine's single authoritative
// "what UI context is the mod in" enum (EGameModes, LE2 SDK SFXGame_classes.hpp:4054): 0 Default(gameplay),
// 1 PowerWheel, 2 WeaponWheel, 3 Command, 4 Vehicle, 5 Conversation, 6 Cinematic, 7 GUI (full-screen
// menu: pause/inventory/squad/journal/map/options), 8 Movie, 9 Galaxy, 10 Orbital(planet scan),
// 11 Photo, 12 CheatMenu. Chain: P1 ULocalPlayer -> Actor(0x68, ABioPlayerController) ->
// GameModeManager2(0xA68, USFXGameModeManager) -> CurrentMode byte(0xB8). Pure READ; -1 if unreadable
// (caller fails OPEN to VR so a bad read can never latch mono into live gameplay).
int ReadGameModeSEH() noexcept
{
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return - 1;
    std::uintptr_t pc = 0, gmm = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc == 0) return - 1;
    if (!SafeReadPtr(pc + 0xA68, &gmm) || gmm == 0) return - 1;     // LE2 ABioPlayerController.GameModeManager2
    int raw = 0;
    if (!SafeReadInt(gmm + 0xB8, &raw)) return - 1;                  // USFXGameModeManager.CurrentMode (byte)
    const int mode = raw & 0xFF;
    return (mode <= 12) ? mode : -1;
}

int ReadLoadMoviePlaybackStateSEH() noexcept
{
    // LE2 SDK layout: GEngine is a USFXEngine.  LoadMovieManager is +0xC08 and the
    // USFXLoadMovieManager PlaybackState enum is +0xA8.  This chain was validated live:
    // 0 at idle, 1 for the entire loading/prerendered movie interval.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) return -1;
    std::uintptr_t gEngine = 0, manager = 0;
    if (!SafeReadPtr(base + kGEngineRva, &gEngine) || gEngine < 0x10000) return -1;
    if (!SafeReadPtr(gEngine + 0xC08, &manager) || manager < 0x10000) return -1;
    int raw = 0;
    if (!SafeReadInt(manager + 0xA8, &raw)) return -1;
    const int state = raw & 0xFF;
    return state <= 4 ? state : -1;
}

void GetPauseChain(unsigned long long out[5]) noexcept
{
    for (int i = 0; i < 5; ++i) out[i] = 0;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    out[0] = lp; if (lp == 0) return;
    std::uintptr_t pc = 0, wi = 0, pauser = 0;
    if (!SafeReadPtr(lp + 0x68, &pc)) return; out[1] = pc; if (pc == 0) return;
    if (!SafeReadPtr(pc + 0x1B0, &wi)) return; out[2] = wi; if (wi == 0) return;
    if (SafeReadPtr(wi + 0x684, &pauser)) out[3] = pauser;
    int playersOnly = 0;
    if (SafeReadInt(wi + 0x7D8, &playersOnly)) out[4] = static_cast<unsigned long long>(playersOnly);
}

// Discovery: dump the camera / object graph (class names) reachable from P1, to find the signal that a
// CONVERSATION or CUTSCENE is active (expected: a distinct camera mode at PlayerCamera). Read-only.
// P1 ULocalPlayer -> Actor(0x68, PlayerController) -> PlayerCamera(0x6A0, LE2). Also ViewportClient(0x594).
void DumpGfxState() noexcept
{
    char nm[96] = {};
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) { ME2VR::Log::Line("[ME2DISC] GFXDUMP: no P1"); return; }
    ME2VR::Log::Line("[ME2DISC] ===== GFXDUMP =====");

    std::uintptr_t pc = 0; SafeReadPtr(lp + 0x68, &pc);
    if (pc && ClassNameOf(pc, nm, sizeof(nm))) ME2VR::Log::Line(std::string("[ME2DISC] GFXDUMP PC(Actor) ") + Hex(pc) + " class=" + nm);
    std::uintptr_t cam = 0; if (pc) SafeReadPtr(pc + 0x6A0, &cam);   // LE2 APlayerController.PlayerCamera
    if (cam && ClassNameOf(cam, nm, sizeof(nm))) ME2VR::Log::Line(std::string("[ME2DISC] GFXDUMP PlayerCamera ") + Hex(cam) + " class=" + nm);
    std::uintptr_t vpc = 0; SafeReadPtr(lp + 0x594, &vpc);
    if (vpc && ClassNameOf(vpc, nm, sizeof(nm))) ME2VR::Log::Line(std::string("[ME2DISC] GFXDUMP ViewportClient ") + Hex(vpc) + " class=" + nm);

    const std::uintptr_t bases[3] = { pc, cam, vpc };
    const char* baseName[3] = { "pc", "cam", "vpc" };
    for (int b = 0; b < 3; ++b)
    {
        if (bases[b] == 0) continue;
        for (std::uintptr_t off = 0x40; off <= 0x900; off += 8)
        {
            std::uintptr_t p = 0;
            if (!SafeReadPtr(bases[b] + off, &p) || p < 0x10000) continue;
            if (ClassNameOf(p, nm, sizeof(nm)))
            {
                ME2VR::Log::Line(std::string("[ME2DISC] GFXDUMP ") + baseName[b] + "+" + Hex(off) + " -> obj class=" + nm);
                continue;
            }
            int num = 0; std::uintptr_t e0 = 0;
            if (SafeReadInt(bases[b] + off + 8, &num) && num >= 1 && num <= 64 &&
                SafeReadPtr(p, &e0) && e0 >= 0x10000 && ClassNameOf(e0, nm, sizeof(nm)))
            {
                ME2VR::Log::Line(std::string("[ME2DISC] GFXDUMP ") + baseName[b] + "+" + Hex(off) + " -> TArray num=" + std::to_string(num) + ":");
                const int cap = num < 16 ? num : 16;
                for (int i = 0; i < cap; ++i)
                {
                    std::uintptr_t e = 0;
                    if (SafeReadPtr(p + static_cast<std::uintptr_t>(i) * 8, &e) && e >= 0x10000 && ClassNameOf(e, nm, sizeof(nm)))
                        ME2VR::Log::Line(std::string("[ME2DISC] GFXDUMP     [") + std::to_string(i) + "] " + nm);
                }
            }
        }
    }
    ME2VR::Log::Line("[ME2DISC] ===== GFXDUMP end =====");
}

// --- First-person camera (basic) ---------------------------------------------------------
// The active camera mode (PlayerCamera+0x580 = CurrentCameraMode, a USFXCameraMode) has an Offset
// (0x19C, FVector X=fwd/back, Y=shoulder, Z=up) = the 3rd-person boom. Zero it -> camera at the hook
// (~head) = first person. Chain: P1 -> Actor(0x68) -> PlayerCamera(0x6A0) -> CurrentCameraMode(0x580).
namespace
{
std::atomic_bool g_fpEnabled{true};   // FP is the primary mode now -> default ON (persisted in MELE2VR.ini)
std::atomic_bool g_meshHidden{false};    // F7: hide own body (independent of the FP camera, for testing)
constexpr std::uintptr_t kPcPlayerCamera = 0x6A0;   // LE2 APlayerController.PlayerCamera
constexpr std::uintptr_t kCamCurrentMode = 0x580;   // SFXCamera.CurrentCameraMode (USFXCameraMode*)
constexpr std::uintptr_t kModeOffset = 0x19C;       // USFXCameraMode.Offset (FVector)

// First-person camera states, one per SFXCameraMode class (full LE2/LE3 hierarchy). The active state is
// chosen by EXACT live class-name match (safer than substring with this many similar names). Indices 0-3
// are kept first so previously-saved MELE2VR.ini values (S0..S3) still load. Cfg = {on, hideHead,
// hideBody, hideWeapon, x, y, z}. Modes not listed (Vehicle/Spectator/IllusiveManConflict/transitions)
// stay third-person. Default eye = ME1's (35 fwd, 65 up, head hidden); action/transitional states
// default OFF (on=false) so they stay 3rd-person until enabled + tuned.
struct FpStateDef { const char* cls; const char* label; ME2VR::EngineProbe::FpStateCfg def; };
// ME1's shipped FP state set, and nothing more. FIVE states, each matching a live SFXCameraMode class name
// exactly. Everything else - sniper scope, all of cover, the Mako, conversations, cutscenes, transitions -
// simply isn't listed, so it stays third-person by omission. That omission IS the design: ME1 started with a
// full cover/crouch/lean sub-state matrix driven off volatile pawn bytes, and it was ripped out because the
// per-frame reads and writes it needed were a crash source. Do not reintroduce it. Indices 0-4 are unchanged
// so saved MELE2VR.ini values (S0..S4) still load; older S5..S15 cover keys are ignored.
// Baked 2026-07-26 from a tuned + saved ini (struct order: on, hideHead, hideBody, hideWeapon, x, y, z).
const FpStateDef g_fpDefs[] = {
    { "SFXCameraMode_Explore",      "Explore (unarmed)",   { true, true, true,  true,  -35.0f, -0.0f, 35.0f } },  // 0
    { "SFXCameraMode_ExploreStorm", "Explore sprint",      { true, true, true,  true,  -40.0f,  0.0f, 35.0f } },  // 1
    { "SFXCameraMode_Combat",       "Combat (weapon out)", { true, true, false, false, -60.0f,  5.0f, 25.0f } },  // 2
    { "SFXCameraMode_CombatStorm",  "Combat sprint",       { true, true, true,  false, -70.0f,  5.0f, 25.0f } },  // 3
    { "SFXCameraMode_TightAim",     "Aim down sights",     { true, true, false, false, -30.0f,  5.0f, 20.0f } },  // 4
};
constexpr int kFpCount = static_cast<int>(sizeof(g_fpDefs) / sizeof(g_fpDefs[0]));
ME2VR::EngineProbe::FpStateCfg g_fpStates[kFpCount] = {};
bool g_fpStatesInit = false;

void FpEnsureInit() noexcept   // copy defaults into the live array once (before ini load / first apply)
{
    if (g_fpStatesInit) return;
    for (int i = 0; i < kFpCount; ++i) g_fpStates[i] = g_fpDefs[i].def;
    g_fpStatesInit = true;
}

// Does this resolved state render first-person? Asked by all three paths that write an FP offset (the per-frame
// apply, the prewarm sweep, and the blend-endpoint owner) so they can never disagree about a mode.
bool FpStateAllowed(int idx) noexcept
{
    return idx >= 0 && idx < kFpCount && g_fpStates[idx].on;
}

// Pick the FP state from the live mode class name. Exact match against the five listed classes; anything else
// (cover, sniper, vehicle, cutscene, Interpolate) returns - 1 and stays third person.
int FpStateIndexForMode(const char* cls) noexcept
{
    if (cls == nullptr) return - 1;
    for (int i = 0; i < kFpCount; ++i)
        if (std::strcmp(cls, g_fpDefs[i].cls) == 0) return i;
    return - 1;
}

// Vanilla (3rd-person) Offset per camera-mode class, from ME1's CDO harvest (ME2 shares the engine
// offsets - confirmed: Explore reads 200,0,40 live in both). Used to RESTORE a mode from a known
// constant instead of a captured/cached value, so the mod never has to write through a stale pointer.
// Runtime vanilla-offset cache, keyed by mode CLASS NAME (not a pointer, so it survives loads safely).
// The mod captures a mode's real 3rd-person offset the first time the mod is about to write FP to it (current value
// is still vanilla then), and restore from it when FP turns off - works for ALL states, no hardcoding.
// [COVSTOMP] `written` is the fix for the cover camera skip. The template snapshot (prewarm) seeds this
// cache for EVERY camera-mode class - including the cover family, whose Offset is DYNAMIC and game-owned
// (TightAimCover's template = (135,0,0); the live aim offset is per-lean, e.g. (125,-115,20)). The
// restore path used to write the cached value into ANY cached class, so the per-frame TP apply + prewarm
// stomped the game's live cover-aim offset with the template value in the frames before the game's lean
// update re-asserts it - the [COVPOS]-proven 112uu out-and-back teleport, FP on or off. A restore exists
// to undo THE MOD'S writes, so it must only fire for classes the mod actually WROTE (`written`); a snapshot alone
// authorizes nothing.
struct ClassVanilla { char cls[64]; float x, y, z; bool written; };
ClassVanilla g_vanilla[40] = {};
int g_vanillaCount = 0;

void CaptureVanilla(const char* cls, float x, float y, float z) noexcept   // first capture per class wins
{
    if (cls == nullptr || cls[0] == '\0') return;
    for (int i = 0; i < g_vanillaCount; ++i) if (std::strcmp(g_vanilla[i].cls, cls) == 0) return;
    if (g_vanillaCount >= static_cast<int>(sizeof(g_vanilla) / sizeof(g_vanilla[0]))) return;
    ClassVanilla& v = g_vanilla[g_vanillaCount++];
    std::strncpy(v.cls, cls, sizeof(v.cls) - 1); v.cls[sizeof(v.cls) - 1] = '\0';
    v.x = x; v.y = y; v.z = z; v.written = false;
}
void MarkVanillaWritten(const char* cls) noexcept   // the mod is about to own this class's offset
{
    if (cls == nullptr) return;
    for (int i = 0; i < g_vanillaCount; ++i)
        if (std::strcmp(g_vanilla[i].cls, cls) == 0) { g_vanilla[i].written = true; return; }
}
bool LookupVanilla(const char* cls, float* x, float* y, float* z) noexcept
{
    if (cls == nullptr) return false;
    for (int i = 0; i < g_vanillaCount; ++i)
        if (std::strcmp(g_vanilla[i].cls, cls) == 0)
        {
            if (!g_vanilla[i].written) return false;   // [COVSTOMP] never "restore" a class the mod never wrote
            *x = g_vanilla[i].x; *y = g_vanilla[i].y; *z = g_vanilla[i].z; return true;
        }
    return false;
}

// POD-only SEH helper. Writes ONLY the current (fresh, read-this-frame) mode: on=true -> capture vanilla
// once (before overwriting) then write the per-state FP eye offset + disable camera-collision; off=false
// -> restore the captured vanilla (only if the mod ever wrote this class; else leave it, it's still vanilla).
// No cached old-mode pointer is ever written, so a save/level load that frees the mode can't corrupt state.
constexpr std::uintptr_t kModeCollision = 0x214;          // USFXCameraMode flag dword
constexpr std::uint32_t  kModeCollisionEnabled = 0x02u;   // bCollisionEnabled (gates DoCameraCollision)

bool FpApplyOffset(std::uintptr_t mode, bool on, float x, float y, float z, const char* cls) noexcept
{
    if (mode < 0x10000) return false;
    __try
    {
        volatile float* off = reinterpret_cast<volatile float*>(mode + kModeOffset);
        volatile std::uint32_t* coll = reinterpret_cast<volatile std::uint32_t*>(mode + kModeCollision);
        if (on)
        {
            CaptureVanilla(cls, off[0], off[1], off[2]);   // first sight = real vanilla (before the mod writes)
            MarkVanillaWritten(cls);                       // [COVSTOMP] restore is armed ONLY by a real write
            off[0] = x; off[1] = y; off[2] = z;            // reassert each frame (picks up live menu tuning)
            *coll &= ~kModeCollisionEnabled;               // disable camera-collision (cull + camera-shove)
        }
        else
        {
            float vx = 0, vy = 0, vz = 0;
            if (LookupVanilla(cls, &vx, &vy, &vz)) { off[0] = vx; off[1] = vy; off[2] = vz; *coll |= kModeCollisionEnabled; }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- PREWARM (anti-flicker camera-mode ownership) ----------------------------------------------------------
// The per-state apply above runs at Present, ONE frame behind the game's camera math, and only touches the
// CURRENTLY-active mode. So the instant the game switches to a DIFFERENT mode object - walk<->run (Explore<->
// ExploreStorm), hip<->ADS, cover variants - that mode still holds its vanilla 3rd-person boom for a frame (or
// the whole blend) before the tick catches it: the camera flicks to 3rd person and snaps back. Fix (ME1's
// proven approach): pre-apply each mode's INTENDED state to EVERY live SFXCameraMode instance ahead of time, so
// a switch is already correct on arrival. Prewarm writes a NEUTRAL placeholder (35,0,65 - ME1 found per-state
// offsets with negative X crash the engine's preset-interpolation during blends); the tick refines the active
// mode to its exact per-state offset. Templates (Default__*) are NEVER written (that poisons fresh spawns) -
// only snapshotted as the vanilla source. Fully SEH/liveness-guarded.
struct PrewarmMode { std::uintptr_t ptr; char cls[64]; };
PrewarmMode g_prewarm[48] = {};
int g_prewarmCount = 0;

// Apply ONE mode's intended state: FP neutral placeholder if VR-FP is on and this mode's state is on; else
// restore vanilla. Skips the tick-owned active mode (the per-state apply owns it with the exact offset).
void FpPrewarmApplyOne(std::uintptr_t obj, const char* cls, bool fpCam, std::uintptr_t skipMode) noexcept
{
    if (obj == skipMode) return;
    const int idx = fpCam ? FpStateIndexForMode(cls) : -1;
    if (FpStateAllowed(idx))
    {
        // Pre-own with this mode's REAL per-state offset (NOT a neutral placeholder). ME2's FP offsets are far
        // from any neutral (e.g. Combat eye = -75,5,27), so a blend gliding to a neutral DESTINATION is itself a
        // visible jump (this was the "fixed in 3rd person, not 1st" flick). Writing the real offset to every
        // on-state mode means From AND To of every Interpolate blend are already at their correct FP positions ->
        // the blend glides cleanly. Only recognized FP SFXCameraMode classes are written, never a vehicle/cutscene
        // mode -> no wrong-field crash (that, not negative-X, was ME1's actual crash).
        const FpStateCfg& s = g_fpStates[idx];
        FpApplyOffset(obj, true, s.x, s.y, s.z, cls);
    }
    else
    {
        FpApplyOffset(obj, false, 0.0f, 0.0f, 0.0f, cls);   // restore vanilla (off / non-FP / idle blend mode)
    }
}

// Full GObjects walk: (re)discover every live SFXCameraMode instance, cache it, and pre-apply its intended
// state. POD-only (no std::string) so the __try is legal. Cost is a whole object-table scan -> call RARELY.
void FpPrewarmFullScanInner(bool fpCam, std::uintptr_t skipMode) noexcept
{
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x173CC48);
        const int num = *reinterpret_cast<int volatile*>(base + 0x173CC48 + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return;
        g_prewarmCount = 0;
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            char cls[64] = {};
            if (!ClassNameOf(obj, cls, sizeof(cls)) || std::strncmp(cls, "SFXCameraMode_", 14) != 0) continue;
            // Template? Snapshot its offset as the poison-proof class vanilla, then leave it untouched.
            char onm[96] = {};
            const unsigned long long nfn = *reinterpret_cast<unsigned long long volatile*>(obj + kObjNameOff);
            if (ReadSfxName(nfn, onm, sizeof(onm)) && std::strncmp(onm, "Default__", 9) == 0)
            {
                volatile float* off = reinterpret_cast<volatile float*>(obj + kModeOffset);
                CaptureVanilla(cls, off[0], off[1], off[2]);
                continue;
            }
            if (g_prewarmCount < static_cast<int>(sizeof(g_prewarm) / sizeof(g_prewarm[0])))
            {
                PrewarmMode& m = g_prewarm[g_prewarmCount++];
                m.ptr = obj;
                std::strncpy(m.cls, cls, sizeof(m.cls) - 1); m.cls[sizeof(m.cls) - 1] = '\0';
            }
            FpPrewarmApplyOne(obj, cls, fpCam, skipMode);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void FpPrewarmFullScan(bool fpCam, std::uintptr_t skipMode) noexcept   // wrapper: SEH walk + (unwinding) logging
{
    FpPrewarmFullScanInner(fpCam, skipMode);
    char b[96] = {};
    sprintf_s(b, "[ME2PW] full scan: %d camera modes owned (fpCam=%d)", g_prewarmCount, fpCam ? 1 : 0);
    ME2VR::Log::Line(b);
}

// Cheap re-assert over the CACHED modes only (no object-table walk): verify each cached ptr still reads as its
// SFXCameraMode class (never scribble a freed/reused slot), then re-apply its intended state. Picks up menu
// on/off toggles and re-owns anything the game reset. Safe to call frequently.
void FpPrewarmReassert(bool fpCam, std::uintptr_t skipMode) noexcept
{
    for (int i = 0; i < g_prewarmCount; ++i)
    {
        const std::uintptr_t obj = g_prewarm[i].ptr;
        if (obj < 0x10000) continue;
        char cls[64] = {};
        if (!ClassNameOf(obj, cls, sizeof(cls)) || std::strcmp(cls, g_prewarm[i].cls) != 0) continue;  // freed/reused
        FpPrewarmApplyOne(obj, cls, fpCam, skipMode);
    }
}

// ---- INTERPOLATE BLEND ENDPOINTS (the real run/stop flicker fix, ported from ME1) -------------------------
// During a camera transition the game makes SFXCameraMode_Interpolate the active mode and glides the camera
// From -> To using those two modes' OWN offsets. Holding the Interpolate mode's own offset does nothing - the
// blend reads From.Offset and To.Offset. If To (e.g. CombatStorm) is still at its vanilla 3rd-person boom, the
// blend dips/snaps through 3rd person (camera swings behind, then ABOVE Shepard, then to the sprint cam) before
// settling = exactly the flicker. Fix: every blend frame, read the LIVE From/To off the active Interpolate mode
// and own each with ITS per-state FP offset, so the blend gildes straight between two first-person positions.
constexpr std::uintptr_t kInterpFrom      = 0x250;   // USFXCameraMode_Interpolate.From      (LE2 SDK)
constexpr std::uintptr_t kInterpTo        = 0x258;   // USFXCameraMode_Interpolate.To        (LE2 SDK)
constexpr std::uintptr_t kInterpTotalTime = 0x260;   // USFXCameraMode_Interpolate.TotalTime (LE2 SDK)
constexpr std::uintptr_t kInterpCurTime   = 0x264;   // USFXCameraMode_Interpolate.CurrentTime (LE2 SDK)

// Where is this blend GOING? >=0 = an FP state, -1 = definitely third-person (cover / cutscene / vehicle),
// -2 = couldn't tell (the To pointer is occasionally stale mid-transition). Used to take the head off the
// hidden list the moment the mod starts blending OUT of first person, instead of when the blend finishes.
int FpBlendDestState(std::uintptr_t interpMode) noexcept
{
    std::uintptr_t to = 0;
    if (!SafeReadPtr(interpMode + kInterpTo, &to) || to < 0x10000) return - 2;
    char cls[64] = {};
    if (!ClassNameOf(to, cls, sizeof(cls)) || std::strncmp(cls, "SFXCameraMode_", 14) != 0) return - 2;
    return FpStateIndexForMode(cls);
}

void FpOwnOneBlendMode(std::uintptr_t m) noexcept   // own a From/To endpoint with its own per-state FP offset
{
    if (m < 0x10000) return;
    char cls[64] = {};
    if (!ClassNameOf(m, cls, sizeof(cls)) || std::strncmp(cls, "SFXCameraMode_", 14) != 0) return;  // not a cam mode
    const int idx = FpStateIndexForMode(cls);
    if (!FpStateAllowed(idx)) return;   // only own a recognized, ENABLED FP destination (never vehicle/cover/sniper)
    const FpStateCfg& s = g_fpStates[idx];
    FpApplyOffset(m, true, s.x, s.y, s.z, cls);    // first-person eye, collision off
}

// THE transition fix. The engine's Interpolate blend arcs the camera UP between the two cameras (fine on a flat
// monitor, nauseating in VR - the transition viewpoint rides higher than both endpoints). The mod can't stop the
// engine computing that arc, but writing the blend mode's OWN Offset overrides what the camera actually uses
// (proven: an earlier constant write held the camera there). So replace the arc with a STRAIGHT lerp between
// the From and To camera offsets, driven by the blend's own progress (CurrentTime/TotalTime): no upward swoop,
// and it lands exactly on the destination at t=1 so there's no snap when the blend ends. FP first owns the
// endpoints to their first-person offsets (straight eye->eye); TP lerps the vanilla offsets (straight, no arc).
// Applies uniformly to every blend, cover included: cover-aim offsets are dynamic and game-owned, but as
// long as the mod only ever READS them here (never restore a stale value into them - see [COVSTOMP] above), a
// straight lerp toward whatever the engine currently has staged is correct camera math either way.
void FpStraightenBlend(std::uintptr_t interpMode, bool fpActive) noexcept
{
    if (interpMode < 0x10000) return;
    std::uintptr_t from = 0, to = 0;
    if (!SafeReadPtr(interpMode + kInterpFrom, &from) || from < 0x10000) return;
    if (!SafeReadPtr(interpMode + kInterpTo,   &to)   || to   < 0x10000) return;
    if (fpActive) { FpOwnOneBlendMode(from); FpOwnOneBlendMode(to); }   // endpoints + post-blend mode = first person
    __try
    {
        const volatile float* fo = reinterpret_cast<const volatile float*>(from + kModeOffset);
        const volatile float* tno = reinterpret_cast<const volatile float*>(to + kModeOffset);
        const float fx = fo[0], fy = fo[1], fz = fo[2];
        const float tx = tno[0], ty = tno[1], tz = tno[2];
        const float total = *reinterpret_cast<const volatile float*>(interpMode + kInterpTotalTime);
        const float cur   = *reinterpret_cast<const volatile float*>(interpMode + kInterpCurTime);
        float t = (total > 0.0001f) ? (cur / total) : 1.0f;
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        volatile float* io = reinterpret_cast<volatile float*>(interpMode + kModeOffset);
        io[0] = fx + (tx - fx) * t;   // straight line, no vertical arc
        io[1] = fy + (ty - fy) * t;
        io[2] = fz + (tz - fz) * t;
        volatile std::uint32_t* coll = reinterpret_cast<volatile std::uint32_t*>(interpMode + kModeCollision);
        *coll &= ~kModeCollisionEnabled;   // no camera-collision shove during the transition
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Hide the player's own body in first person (keep the weapon - it's a separate actor, not a mesh here).
// Set/clear bOwnerNoSee (bit 0x10 of the dword at UPrimitiveComponent+0x168) on the body mesh + the
// ABioPawn accessory meshes (head/hair/headgear/visor/faceplate) + the m_aoAccessories list. POD-only.
// ProcessEvent (LE2 +0xD8D50) + GObjects (LE2 +0x173CC48, a TArray<UObject*>) to call the native
// SetOwnerNoSee UFunction (flips bOwnerNoSee AND marks the render proxy dirty - direct flag writes
// don't, which is why nothing hid). The UFunction is found once by name and cached.
using tProcessEvent = void(__fastcall*)(void* ctx, void* fn, void* parms, void* res);
void* g_setOwnerNoSeeFn = nullptr;
bool g_fnSearched = false;

void* FindSetOwnerNoSeeFn() noexcept
{
    if (g_fnSearched) return g_setOwnerNoSeeFn;
    g_fnSearched = true;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x173CC48);
        const int num = *reinterpret_cast<int volatile*>(base + 0x173CC48 + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return nullptr;
        char nm[64] = {}, onm[64] = {};
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            const unsigned long long n = *reinterpret_cast<unsigned long long volatile*>(obj + kObjNameOff);
            if (!ReadSfxName(n, nm, sizeof(nm)) || std::strcmp(nm, "SetOwnerNoSee") != 0) continue;
            const std::uintptr_t outer = *reinterpret_cast<std::uintptr_t volatile*>(obj + 0x40);
            if (outer < 0x10000) continue;
            const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(outer + kObjNameOff);
            if (ReadSfxName(on, onm, sizeof(onm)) && std::strcmp(onm, "PrimitiveComponent") == 0)
            {
                g_setOwnerNoSeeFn = reinterpret_cast<void*>(obj);
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_setOwnerNoSeeFn;
}

void FpSetOwnerNoSee(std::uintptr_t comp, bool on) noexcept
{
    void* fn = FindSetOwnerNoSeeFn();
    if (fn == nullptr || comp < 0x10000) return;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + 0xD8D50);
        unsigned long parm = on ? 1ul : 0ul;
        pe(reinterpret_cast<void*>(comp), fn, &parm, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// [CONVOFP] ABioPlayerController.DisableDOF() -- the engine's own "turn depth of field off" entry point.
// Conversations drive DOF from a matinee track that focuses on whoever is speaking; from Shepard's eyes
// that reads as the person you are looking at going soft whenever they are not the current speaker.
// The GamerSettings.ini [DOFOFF] preference does not cover it, because the track sets DOF directly.
// Found once by name, same GObjects scan as SetOwnerNoSee.
void* g_disableDofFn = nullptr;
bool g_disableDofSearched = false;

void* FindDisableDofFn() noexcept
{
    if (g_disableDofSearched) return g_disableDofFn;
    g_disableDofSearched = true;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x173CC48);
        const int num = *reinterpret_cast<int volatile*>(base + 0x173CC48 + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return nullptr;
        char nm[64] = {}, onm[64] = {};
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            const unsigned long long n = *reinterpret_cast<unsigned long long volatile*>(obj + kObjNameOff);
            if (!ReadSfxName(n, nm, sizeof(nm)) || std::strcmp(nm, "DisableDOF") != 0) continue;
            const std::uintptr_t outer = *reinterpret_cast<std::uintptr_t volatile*>(obj + 0x40);
            if (outer < 0x10000) continue;
            const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(outer + kObjNameOff);
            // Exact outer match -- never substring; the wrong class here writes into the wrong object.
            if (ReadSfxName(on, onm, sizeof(onm)) && std::strcmp(onm, "BioPlayerController") == 0)
            {
                g_disableDofFn = reinterpret_cast<void*>(obj);
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_disableDofFn;
}

// The weapon is a separate actor whose mesh's owner ISN'T the player pawn, so bOwnerNoSee can't hide it
// from the player's view. Hide the whole weapon ACTOR instead via Engine.Actor.SetHidden (renders for no
// one - fine in single-player FP). UFunction found once by GObjects name scan, like SetOwnerNoSee.
void* g_actorSetHiddenFn = nullptr;
bool g_hiddenSearched = false;

void* FindActorSetHiddenFn() noexcept
{
    if (g_hiddenSearched) return g_actorSetHiddenFn;
    g_hiddenSearched = true;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x173CC48);
        const int num = *reinterpret_cast<int volatile*>(base + 0x173CC48 + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return nullptr;
        char nm[64] = {}, onm[64] = {};
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            const unsigned long long n = *reinterpret_cast<unsigned long long volatile*>(obj + kObjNameOff);
            if (!ReadSfxName(n, nm, sizeof(nm)) || std::strcmp(nm, "SetHidden") != 0) continue;
            const std::uintptr_t outer = *reinterpret_cast<std::uintptr_t volatile*>(obj + 0x40);
            if (outer < 0x10000) continue;
            const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(outer + kObjNameOff);
            if (ReadSfxName(on, onm, sizeof(onm)) && std::strcmp(onm, "Actor") == 0)
            {
                g_actorSetHiddenFn = reinterpret_cast<void*>(obj);
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_actorSetHiddenFn;
}

void CallActorSetHidden(std::uintptr_t actor, bool hidden) noexcept
{
    void* fn = FindActorSetHiddenFn();
    if (fn == nullptr || actor < 0x10000) return;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + 0xD8D50);
        unsigned long parm = hidden ? 1ul : 0ul;
        pe(reinterpret_cast<void*>(actor), fn, &parm, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The weapon mesh is attached to the pawn's hand socket, so neither the weapon's bOwnerNoSee nor the
// weapon ACTOR's bHidden hide it. HiddenGame on the mesh component itself does (unconditional). Call
// Engine.PrimitiveComponent.SetHidden(true) on the weapon mesh.
void* g_primSetHiddenFn = nullptr;
bool g_primHiddenSearched = false;

void* FindPrimSetHiddenFn() noexcept
{
    if (g_primHiddenSearched) return g_primSetHiddenFn;
    g_primHiddenSearched = true;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x173CC48);
        const int num = *reinterpret_cast<int volatile*>(base + 0x173CC48 + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return nullptr;
        char nm[64] = {}, onm[64] = {};
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            const unsigned long long n = *reinterpret_cast<unsigned long long volatile*>(obj + kObjNameOff);
            if (!ReadSfxName(n, nm, sizeof(nm)) || std::strcmp(nm, "SetHidden") != 0) continue;
            const std::uintptr_t outer = *reinterpret_cast<std::uintptr_t volatile*>(obj + 0x40);
            if (outer < 0x10000) continue;
            const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(outer + kObjNameOff);
            if (ReadSfxName(on, onm, sizeof(onm)) && std::strcmp(onm, "PrimitiveComponent") == 0)
            {
                g_primSetHiddenFn = reinterpret_cast<void*>(obj);
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_primSetHiddenFn;
}

void CallPrimSetHidden(std::uintptr_t comp, bool hidden) noexcept
{
    void* fn = FindPrimSetHiddenFn();
    if (fn == nullptr || comp < 0x10000) return;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + 0xD8D50);
        unsigned long parm = hidden ? 1ul : 0ul;
        pe(reinterpret_cast<void*>(comp), fn, &parm, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// SetOwnerNoSee on every primitive component of an actor (Components 0x60 + AllComponents 0x80).
// Used to hide/show the weapon (an attached actor) for cover blind-fire states. Returns comps touched.
int FpSetActorCompsOwnerNoSee(std::uintptr_t actor, bool hide) noexcept
{
    int n = 0;
    if (actor < 0x10000) return 0;
    __try
    {
        const std::uintptr_t arrs[2] = { 0x60, 0x80 };
        for (int a = 0; a < 2; ++a)
        {
            std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(actor + arrs[a]);
            const int num = *reinterpret_cast<int volatile*>(actor + arrs[a] + 8);
            if (data < 0x10000 || num <= 0 || num > 256) continue;
            for (int i = 0; i < num; ++i)
            {
                std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
                if (comp >= 0x10000) { FpSetOwnerNoSee(comp, hide); ++n; }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

// Apply per-state visibility: body mesh (0x458) hidden per hideBody, head + accessories per hideHead,
// the weapon (attached actors) per hideWeapon. ME1-style real FP = hideHead=true, hideBody/Weapon=false
// (arms + gun stay visible). All false = fully un-hide. Applied via SetOwnerNoSee (render-correct).
int FpHideMeshes(std::uintptr_t pawn, bool hideHead, bool hideBody, bool hideWeapon,
                 std::uintptr_t* outBodyMesh, int* outWeaponComps) noexcept
{
    int count = 0;
    int weaponComps = 0;
    *outBodyMesh = 0;
    __try
    {
        std::uintptr_t body = *reinterpret_cast<std::uintptr_t volatile*>(pawn + 0x458);   // body mesh
        *outBodyMesh = body;
        if (body >= 0x10000) { FpSetOwnerNoSee(body, hideBody); ++count; }
        const std::uintptr_t headOffs[5] = { 0x85C, 0x86C, 0x874, 0x87C, 0x884 };   // Head, Hair, HeadGear, Visor, FacePlate
        for (int i = 0; i < 5; ++i)
        {
            std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(pawn + headOffs[i]);
            if (comp >= 0x10000) { FpSetOwnerNoSee(comp, hideHead); ++count; }
        }
        // m_aoAccessories (0x698): helmet/breather/extra headgear attachments -> hide WITH the head, so
        // nothing pokes into the first-person view (the 5 explicit fields don't cover all headgear).
        std::uintptr_t adata = *reinterpret_cast<std::uintptr_t volatile*>(pawn + 0x698);
        const int anum = *reinterpret_cast<int volatile*>(pawn + 0x698 + 8);
        if (adata >= 0x10000 && anum > 0 && anum <= 64)
            for (int i = 0; i < anum; ++i)
            {
                std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(adata + static_cast<std::uintptr_t>(i) * 8);
                if (comp >= 0x10000) { FpSetOwnerNoSee(comp, hideHead); ++count; }
            }
        // Weapon hide. The VISIBLE gun is a component BONE-ATTACHED to the pawn's own skeletal mesh
        // (pawn.Mesh.Attachments @ 0x3BC), NOT on the weapon actor (its component arrays were empty). Hide
        // each attached component via HiddenGame (unconditional) + SetOwnerNoSee. This is the ME1 path.
        if (body >= 0x10000)
        {
            std::uintptr_t adata = *reinterpret_cast<std::uintptr_t volatile*>(body + 0x3BC);          // Attachments.Data
            const int anum = *reinterpret_cast<int volatile*>(body + 0x3BC + 8);                       // .Num
            if (adata >= 0x10000 && anum > 0 && anum <= 64)
                for (int i = 0; i < anum; ++i)
                {
                    std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(adata + static_cast<std::uintptr_t>(i) * 0x34);  // FAttachment.Component
                    if (comp >= 0x10000) { CallPrimSetHidden(comp, hideWeapon); FpSetOwnerNoSee(comp, hideWeapon); ++weaponComps; }
                }
        }
        // Fallback: also hide the weapon ACTOR (WeaponFromLastGameState 0x844 / WeaponOnDeck 0x84C).
        const std::uintptr_t weaponFields[2] = { 0x844, 0x84C };
        std::uintptr_t lastWeapon = 0;
        for (int wf = 0; wf < 2; ++wf)
        {
            std::uintptr_t w = *reinterpret_cast<std::uintptr_t volatile*>(pawn + weaponFields[wf]);
            if (w < 0x10000 || w == lastWeapon) continue;
            lastWeapon = w;
            CallActorSetHidden(w, hideWeapon);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (outWeaponComps != nullptr) *outWeaponComps = weaponComps;
    return count;
}

// ---- First-person visibility guard (the combat-gun-cull fix, ported from ME1) -------------------------
// In combat the game's camera-collision script (DoCameraCollision/DoSniperCameraCollision) adds the pawn
// AND its attached weapon to APlayerController.HiddenActors (0x5C0) when the camera gets close, so the gun
// vanishes in first person. Counter it each frame: pull pawn+attached back out of HiddenActors, and set
// bIgnoreHiddenActorsMembership (bit 0x00080000 of the dword at UPrimitiveComponent+0x16C) on their
// components so they render even if the script re-adds them. This flag is evaluated per-frame at render
// time, so a DIRECT write takes effect (unlike bOwnerNoSee, which needed ProcessEvent).
constexpr std::uintptr_t kPcHiddenActors = 0x5C0;   // APlayerController.HiddenActors (TArray<AActor*>)
constexpr std::uintptr_t kActorComponents = 0x60;   // AActor.Components
constexpr std::uintptr_t kActorAllComps   = 0x80;   // AActor.AllComponents
constexpr std::uintptr_t kActorAttached   = 0xD0;   // AActor.Attached (TArray<AActor*>) -> the weapon
constexpr std::uintptr_t kPrimFlags2 = 0x16C;       // UPrimitiveComponent second flag dword
constexpr std::uint32_t  kPrimIgnoreHidden = 0x00080000u;  // bIgnoreHiddenActorsMembership

struct FpVisComp { std::uintptr_t comp; std::uint32_t orig; };
FpVisComp g_fpVis[256] = {};
int g_fpVisCount = 0;
bool g_fpVisApplied = false;
std::uintptr_t g_fpVisPawn = 0;   // the pawn whose components the mod marked; restore only if it's still current
void FpRestoreVisibility(std::uintptr_t curPawn) noexcept;   // fwd decl (defined below; used by the guard)

bool FpVisTracked(std::uintptr_t comp) noexcept
{
    for (int i = 0; i < g_fpVisCount; ++i) if (g_fpVis[i].comp == comp) return true;
    return false;
}

void FpMarkActorComps(std::uintptr_t actor) noexcept   // set IgnoreHiddenActorsMembership on all components
{
    if (actor < 0x10000) return;
    __try
    {
        const std::uintptr_t arrs[2] = { kActorComponents, kActorAllComps };
        for (int a = 0; a < 2; ++a)
        {
            std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(actor + arrs[a]);
            const int num = *reinterpret_cast<int volatile*>(actor + arrs[a] + 8);
            if (data < 0x10000 || num <= 0 || num > 256) continue;
            for (int i = 0; i < num; ++i)
            {
                std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
                if (comp < 0x10000) continue;
                volatile std::uint32_t* f = reinterpret_cast<volatile std::uint32_t*>(comp + kPrimFlags2);
                if (!FpVisTracked(comp) && g_fpVisCount < 256) { g_fpVis[g_fpVisCount].comp = comp; g_fpVis[g_fpVisCount].orig = *f; ++g_fpVisCount; }
                *f |= kPrimIgnoreHidden;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// SAFE visibility guard. The crash was MUTATING HiddenActors (a cross-thread TArray write). The mod never does
// that again - the mod only READS HiddenActors and set the per-component bIgnoreHiddenActorsMembership flag (a
// plain dword write, evaluated per-frame at render, no race). Each frame the mod first clears last frame's
// flags then re-apply, so toggling hideWeapon takes effect: pawn is always un-culled (body/arms visible);
// the other culled actors (the weapon) are un-culled only when !hideWeapon, else left culled = hidden.
void FpVisibilityGuard(std::uintptr_t pc, std::uintptr_t pawn, bool hideWeapon) noexcept
{
    if (pc < 0x10000 || pawn < 0x10000) return;
    FpRestoreVisibility(pawn);          // clear last frame's flags (same-pawn safe; abandons if pawn changed)
    FpMarkActorComps(pawn);             // body/arms always visible
    if (!hideWeapon)
        __try
        {
            void** data = *reinterpret_cast<void** volatile*>(pc + kPcHiddenActors);   // READ-ONLY
            const int num = *reinterpret_cast<int volatile*>(pc + kPcHiddenActors + 8);
            if (data != nullptr && num > 0 && num <= 64)
                for (int i = 0; i < num; ++i)
                {
                    const std::uintptr_t act = reinterpret_cast<std::uintptr_t>(data[i]);
                    if (act >= 0x10000 && act != pawn) FpMarkActorComps(act);   // un-cull the weapon (+any other)
                }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_fpVisApplied = true;
    g_fpVisPawn = pawn;
}

// FP off / state inactive: clear the flag the mod sets - but ONLY if the pawn that was marked is still the current,
// live pawn. After a save/level load the old pawn + its components are freed; writing their cached
// pointers would corrupt memory (this is what crashed). If the pawn changed (or is gone), abandon the
// tracked entries WITHOUT writing - the freed components are gone, and the flag is harmless anyway.
void FpRestoreVisibility(std::uintptr_t curPawn) noexcept
{
    if (!g_fpVisApplied) return;
    const bool samePawn = (curPawn != 0) && (curPawn == g_fpVisPawn);
    if (samePawn)
    {
        __try
        {
            for (int i = 0; i < g_fpVisCount; ++i)
            {
                if (g_fpVis[i].comp < 0x10000) continue;
                *reinterpret_cast<volatile std::uint32_t*>(g_fpVis[i].comp + kPrimFlags2) = g_fpVis[i].orig;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_fpVisCount = 0;
    g_fpVisApplied = false;
    g_fpVisPawn = 0;
}

// ===== Head-aim (combat): HMD yaw/pitch -> PlayerController ControlRotation (AActor.Rotation 0x124),
// injection model ported from ME1 - the head ADDS a delta onto the stick/mouse aim, tracked so it can
// rebase when you move the stick and reset cleanly when the weapon holsters. Pitch clamped to +/-80deg. ==
constexpr std::uintptr_t kCtrlRotation = 0x124;   // PlayerController(AActor).Rotation = control/aim rotation
std::atomic_bool g_headAimOn{true};
std::atomic_bool g_headAimInvYaw{false};
std::atomic_bool g_headAimInvPitch{false};
std::atomic_bool g_weaponOut{false};              // set by ApplyFirstPerson from the live camera mode
// [COVERFLASH probe] last camera mode name + the weapon-verdict internals, for the flash probe.
char g_cfMode[64] = {};
std::atomic<int> g_cfHold{0};
std::atomic_bool g_cfRaw{false};
std::atomic_bool g_weaponFiring{false};           // ABioPawn.bWeaponFiring, published by ApplyFirstPerson
// [HEADDELAY] ME1 parity: wait N frames after first person engages before hiding the head, so the
// camera blend has landed at the eye first. Hiding instantly makes the head vanish while the camera
// is still outside the body = you watch a headless Shepard for the length of the blend.
std::atomic<int> g_fpHeadHideDelay{20};
int g_fpActiveFrames = 0;
constexpr std::uintptr_t kPawnWeaponFiring = 0x93C;   // bit 0 (confirmed live by the [VEHDIAG] probe)
// [FPSTORM] camera-mode class, published each ApplyFirstPerson pass for the me2_xr storm latch.
std::atomic_bool g_camModeStorm{false};
std::atomic_bool g_camModeInterp{false};
std::atomic_bool g_camModeAiming{false};
std::atomic_bool g_fpBlendActive{false};          // true while an Interpolate blend is held in an active FP state (diag)
float g_aimRefYawDeg = 0.0f, g_aimRefPitchDeg = 0.0f;   // HMD orientation at the aim center
int g_aimAppliedYawUU = 0, g_aimAppliedPitchUU = 0;     // last frame's injected delta (to strip it)
int g_aimBaseYawUU = 0, g_aimBasePitchUU = 0;           // stick aim at the center (to detect manual moves)
bool g_aimBaseValid = false, g_aimActive = false;

int NormUU(int v) noexcept { while (v > 32768) v -= 65536; while (v < -32768) v += 65536; return v; }
float WrapDegF(float d) noexcept { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }
bool ReadCtrlRot(std::uintptr_t pc, int* p, int* y, int* r) noexcept
{
    __try { *p = *reinterpret_cast<int volatile*>(pc + kCtrlRotation); *y = *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 4); *r = *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 8); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteCtrlRot(std::uintptr_t pc, int p, int y, int r) noexcept
{
    __try { *reinterpret_cast<int volatile*>(pc + kCtrlRotation) = p; *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 4) = y; *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 8) = r; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// [VEHAIM] POD SEH read/write of ASVehicle DriverViewYaw/Pitch (0x7A0/0x79C). Separate function so the
// caller can construct std::string (logging) without tripping C2712 (__try + object unwinding).
constexpr std::uintptr_t kVehDVPitch = 0x79C;
constexpr std::uintptr_t kVehDVYaw   = 0x7A0;
bool ReadVehAim(std::uintptr_t pawn, int* yaw, int* pitch) noexcept
{
    __try { *yaw = *reinterpret_cast<int volatile*>(pawn + kVehDVYaw); *pitch = *reinterpret_cast<int volatile*>(pawn + kVehDVPitch); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteVehAim(std::uintptr_t pawn, int yaw, int pitch) noexcept
{
    __try { *reinterpret_cast<int volatile*>(pawn + kVehDVYaw) = yaw; *reinterpret_cast<int volatile*>(pawn + kVehDVPitch) = pitch; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

void ApplyFirstPerson() noexcept
{
    FpEnsureInit();   // populate g_fpStates from g_fpDefs once (before any read)
    const bool fpCam = g_fpEnabled.load(std::memory_order_acquire);
    const bool meshOverride = g_meshHidden.load(std::memory_order_acquire);   // F7: force head-hide for testing
    static unsigned g_fpFrame = 0; ++g_fpFrame;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) { if (g_fpFrame % 15 == 0) ME2VR::Log::Line("[ME2FPDIAG] hb no-lp"); return; }

    // --- PAUSE FREEZE: a save load is launched from the pause menu, which pauses the game and tears the
    // world down on the game thread while the mod runs on the present thread. Touching anything then races the
    // teardown -> crash "the moment load is clicked". While any blocking menu is up (paused), write NOTHING. ---
    if (IsGamePaused())
    {
        FpRestoreVisibility(0);   // abandon cached visibility tracking WITHOUT writing
        if (g_fpFrame % 15 == 0) ME2VR::Log::Line("[ME2FPDIAG] hb PAUSED (frozen)");
        return;
    }

    std::uintptr_t pc = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return;

    // --- LIVENESS GATE: only ever touch a VALIDATED-live controller. During a save/level load the old
    // controller is torn down; its class read stops looking like a PlayerController, so the mod skips ALL FP
    // writes and can't corrupt freed memory. (ClassNameOf is SEH-safe on garbage/freed pointers.) ---
    char pcName[96] = {};
    if (!ClassNameOf(pc, pcName, sizeof(pcName)) || std::strstr(pcName, "Controller") == nullptr || ActorIsDying(pc))
    {
        FpRestoreVisibility(0);   // not a live controller (or it's being destroyed) -> abandon, no writes
        return;
    }

    // --- Resolve the active camera mode + its class name (state chosen below, after cover-state read) ---
    std::uintptr_t cam = 0, mode = 0;
    char modeName[96] = {};
    if (SafeReadPtr(pc + kPcPlayerCamera, &cam) && cam >= 0x10000 &&
        SafeReadPtr(cam + kCamCurrentMode, &mode) && mode >= 0x10000)
        ClassNameOf(mode, modeName, sizeof(modeName));

    // [FPSTORM] publish the camera-mode class for the storm latch (me2_xr aim gate). LE2 aiming modes:
    // TightAim/TightAimCover/SniperZoom/SniperZoomCover (no HipAimCover in ME2's list).
    g_camModeStorm.store(std::strstr(modeName, "Storm") != nullptr, std::memory_order_relaxed);
    g_camModeInterp.store(std::strstr(modeName, "Interpolate") != nullptr, std::memory_order_relaxed);
    g_camModeAiming.store(std::strstr(modeName, "TightAim") != nullptr ||
                          std::strstr(modeName, "Sniper") != nullptr, std::memory_order_relaxed);

    // --- STABILITY GATE: a save/level load (or respawn) frees the old pawn and builds a new one over
    // several frames. Touching it then - especially ProcessEvent into a half-initialized pawn - corrupts
    // memory (the load crash). Only manipulate the pawn after its pointer has been CONSTANT for a short
    // settle window, so the object is fully built before the mod ever writes to it. ---
    std::uintptr_t pawn = 0;
    char pawnName[96] = {};
    const bool havePawn = SafeReadPtr(pc + 0x294, &pawn) && pawn >= 0x10000 &&
                          ClassNameOf(pawn, pawnName, sizeof(pawnName)) && std::strstr(pawnName, "Pawn") != nullptr &&
                          !ActorIsDying(pawn);   // skip the instant the load marks the pawn for destruction
    static std::uintptr_t s_lastPawn = 0;
    static int s_stableFrames = 0;
    if (havePawn && pawn == s_lastPawn) { if (s_stableFrames < 100000) ++s_stableFrames; }
    else { s_stableFrames = 0; s_lastPawn = havePawn ? pawn : 0; }
    const bool stable = havePawn && s_stableFrames >= 30;   // ~0.5s of a constant pawn before any write

    // --- In cover? CoverType byte at pawn+0xAA2 (1=stand, 2=mid, 3=low). ONE read, ONE bool: cover is always
    // third-person, so the mod never needs the direction or action bytes. Trust the byte only after the game has
    // actually shown a named "Cover" camera (s_coverSeen), because a stray read in open play would otherwise
    // drop the camera out of first-person for no visible reason. ---
    int coverType = 0;
    if (havePawn) { int cd = 0; if (SafeReadInt(pawn + 0xAA0, &cd)) coverType = (cd >> 16) & 0xFF; }
    static int s_coverSeen = 0;
    if (std::strstr(modeName, "Cover") != nullptr) s_coverSeen = 90;                    // game showed a cover cam
    else if ((coverType == 1 || coverType == 2) && s_coverSeen > 0) s_coverSeen = 90;   // hold through a long hug
    else if (s_coverSeen > 0) --s_coverSeen;
    const bool inCover = (coverType >= 1 && coverType <= 3) && s_coverSeen > 0;

    // --- Pick the FP state. SFXCameraMode_Interpolate is the transient BLEND the game makes the ACTIVE mode
    // while morphing between two real modes (Combat<->CombatStorm on sprint start/stop, hip<->ADS, ...). It
    // isn't an FP state, so naively it reads as third person and the camera booms out + the body/head reappear
    // for the whole ~0.5s blend = a jarring repeated cut. HOLD the last real resolved state through the blend
    // so FP and the mesh hide stay continuous. Cover overrides everything: third person, immediately. ---
    static int s_lastRealIdx = -1;
    const bool isBlend = (modeName[0] == '\0') || std::strstr(modeName, "Interpolate") != nullptr;
    int stateIdx;
    bool blendLeavingFp = false;
    if (inCover)      { stateIdx = -1; s_lastRealIdx = -1; }
    else if (isBlend)
    {
        // The camera offset keeps following the state the mod came FROM, so the blend still starts at the eye.
        // But if the mod is blending toward something third-person (cover, a cutscene), first person is already
        // over: the head has to come back NOW, at the start of the blend, or the camera pulls out far
        // enough to show a headless Shepard for the ~0.5s it takes to arrive. Same reasoning in reverse
        // covers the way back out - s_lastRealIdx is - 1 while in cover, so the head only hides once the
        // blend has actually landed on the first-person eye.
        stateIdx = s_lastRealIdx;
        blendLeavingFp = (mode >= 0x10000) && (FpBlendDestState(mode) == -1);
    }
    else              { stateIdx = fpCam ? FpStateIndexForMode(modeName) : -1; s_lastRealIdx = stateIdx; }
    // Weapon-out = any combat-family camera mode (not Explore/Mako/cutscene). Drives head-aim gating AND
    // the ADS/sniper zoom-fill (vs cinematic mono). STICKY: the hipfire<->ADS blend briefly swaps in a
    // transient "Interpolate" mode where the name doesn't match; if weapon-out dropped for those frames the
    // narrow blend FOV would be mis-read as a cutscene and flash to mono. Hold it ~18 frames past the last
    // match so the transition stays stereo+zoomed end to end.
    const bool rawWeaponOut = std::strstr(modeName, "Combat") != nullptr || std::strstr(modeName, "TightAim") != nullptr ||
                              std::strstr(modeName, "Sniper") != nullptr || std::strstr(modeName, "Cover") != nullptr ||
                              std::strstr(modeName, "HipAim") != nullptr;
    // [WPNHOLD 2026-08-22] HOLD the verdict through Interpolate instead of counting down in it.
    // Interpolate is the blend the game routes EVERY camera change through, and it says nothing
    // about whether the gun is up - so letting the 18-frame countdown run inside it drops the
    // verdict mid-blend. Measured from a pop-up-from-crouched-cover repro: the cycle is
    // Cover -> Interpolate -> TightAimCover, the hold expired 148ms into the blend (18 frames at
    // 120fps), and it expired while the FOV was already at the 20deg aim value. ApplyFov then hit
    // "if (gh < 0.59f && !weaponOut) FovExit(narrowNoWeapon)", turned FOV fill OFF, and rendered a
    // 20deg window inside a ~50deg headset: a small picture in the middle surrounded by black,
    // for the length of the blend. Every weaponOut=0 line in that log was on Interpolate.
    // Holding through it only DELAYS an honest drop to the moment a real camera lands (holstering
    // still ends at Explore and drops correctly), and a wide FOV never trips that exit anyway.
    // This is the rule ME3 already arrived at: update the verdict only from cameras that state it.
    const bool camStatesWeapon = std::strstr(modeName, "Interpolate") == nullptr;
    static int s_weaponOutHold = 0;
    if (rawWeaponOut) s_weaponOutHold = 18;
    else if (camStatesWeapon && s_weaponOutHold > 0) --s_weaponOutHold;
    g_weaponOut.store(rawWeaponOut || s_weaponOutHold > 0, std::memory_order_release);
    // [COVERFLASH probe 2026-08-22] Publish what the weapon verdict was built from, so the flash
    // probe can say WHICH camera mode was live when the verdict dropped and how much hold was left.
    g_cfHold.store(s_weaponOutHold, std::memory_order_relaxed);
    g_cfRaw.store(rawWeaponOut, std::memory_order_relaxed);
    {
        char* dst = g_cfMode;
        int i = 0; for (; i < 63 && modeName[i] != 0; ++i) dst[i] = modeName[i];
        dst[i] = 0;
    }
    // Publish "trigger held" from the pawn the mod already validated this frame - the storm latch uses it as an
    // aim-intent signal (firing while sprinting must hand aim to the head immediately, see me2_xr).
    int wfBits = 0;
    g_weaponFiring.store(havePawn && SafeReadInt(pawn + kPawnWeaponFiring, &wfBits) && (wfBits & 0x1) != 0,
                         std::memory_order_release);
    const FpStateCfg* st = (stateIdx >= 0) ? &g_fpStates[stateIdx] : nullptr;
    const bool active = FpStateAllowed(stateIdx) && !blendLeavingFp;   // FP now (else stays 3rd person)
    g_fpBlendActive.store(isBlend && active, std::memory_order_release);   // diag: in an Interpolate blend, FP held
    // [HEADDELAY] count consecutive first-person frames; the head hide waits for the blend to land.
    if (active) { if (g_fpActiveFrames < 100000) ++g_fpActiveFrames; } else g_fpActiveFrames = 0;
    const bool headDelayMet = g_fpActiveFrames > g_fpHeadHideDelay.load(std::memory_order_relaxed);
    const bool hideHead = (active && st->hideHead && headDelayMet) || meshOverride;
    const bool hideBody = active && st->hideBody;
    const bool hideWeapon = active && st->hideWeapon;

    int weaponComps = 0;
    if (!stable)
    {
        // Transition/load in progress: do NOT write any game object. Abandon cached vis tracking safely.
        FpRestoreVisibility(0);   // samePawn=false -> clears tracking WITHOUT writing freed pointers
    }
    else
    {
        // Camera offset on the current mode.
        if (mode >= 0x10000)
        {
            if (isBlend)
            {
                // A real transition: the engine arcs the camera UP. Override with a STRAIGHT lerp between the
                // From/To cameras. FP owns the endpoints; TP uses vanilla.
                FpStraightenBlend(mode, active);
            }
            else
            {
                FpApplyOffset(mode, active, active ? st->x : 0.0f, active ? st->y : 0.0f,
                              active ? st->z : 0.0f, modeName);
            }
        }

        // Body/head/weapon hide (ProcessEvent) - change-gated; the pawn is stable so it's safe to write.
        static bool s_prevHead = false, s_prevBody = false, s_prevWeapon = false;
        static std::uintptr_t s_hidPawn = 0;
        if (pawn != s_hidPawn || hideHead != s_prevHead || hideBody != s_prevBody || hideWeapon != s_prevWeapon)
        {
            std::uintptr_t bodyMesh = 0;
            FpHideMeshes(pawn, hideHead, hideBody, hideWeapon, &bodyMesh, &weaponComps);
            s_prevHead = hideHead; s_prevBody = hideBody; s_prevWeapon = hideWeapon; s_hidPawn = pawn;
            char hb[224] = {};
            sprintf_s(hb, "[ME2FP] hide applied: head=%d body=%d weapon=%d weaponComps=%d pawn=0x%llX primFn=0x%llX actorFn=0x%llX",
                      hideHead ? 1 : 0, hideBody ? 1 : 0, hideWeapon ? 1 : 0, weaponComps,
                      static_cast<unsigned long long>(pawn),
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(FindPrimSetHiddenFn())),
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(FindActorSetHiddenFn())));
            ME2VR::Log::Line(hb);
        }

        // Camera-collision is disabled at the source (mode flag, in FpApplyOffset) so the weapon/pawn are
        // no longer culled - no per-frame pawn-component writes, which is what raced the load teardown.
        (void)hideWeapon; (void)pc;
    }

    // --- PREWARM every camera mode to its intended FP/TP state so a mode SWITCH (run/ADS/cover) never shows a
    // 1-frame 3rd-person boom = the flicker. Full object-table scan only when needed (FP toggle, a load, or
    // until the modes are found); cheap cached re-assert otherwise (also applies live menu on/off toggles). ---
    if (stable)
    {
        static bool s_prevFpCam = false;
        static unsigned s_pwTick = 0;
        const bool needFull = (fpCam != s_prevFpCam) || (s_stableFrames == 30) ||
                              (g_prewarmCount < 4 && (s_pwTick % 90) == 0);
        s_prevFpCam = fpCam;
        ++s_pwTick;
        if (needFull)                       FpPrewarmFullScan(fpCam, mode);
        else if ((s_pwTick % 30) == 0)      FpPrewarmReassert(fpCam, mode);
    }

    // --- Log the detected state on change (so the mod can verify ME2 reports each gameplay state) ---
    static int s_prevState = -2;
    static bool s_prevActive = false;
    if (stateIdx != s_prevState || active != s_prevActive)
    {
        char b[256] = {};
        sprintf_s(b, "[ME2FP] state=%s%s mode=%s eye=(%.0f,%.0f,%.0f) hideHead=%d hideBody=%d hideWeapon=%d",
                  (stateIdx >= 0) ? g_fpDefs[stateIdx].label : "(third person)",
                  (stateIdx >= 0 && !active) ? " [3rd person]" : "", modeName,
                  active ? st->x : 0.0f, active ? st->y : 0.0f, active ? st->z : 0.0f,
                  hideHead ? 1 : 0, hideBody ? 1 : 0, hideWeapon ? 1 : 0);
        ME2VR::Log::Line(b);
        s_prevState = stateIdx; s_prevActive = active;
    }

}

// F8 diagnostic: locate the held weapon. Logs the pawn's weapon-actor fields + walks the inventory
// chain (APawn.Inventory 0x294 -> AInventory.Inventory 0x2C4) so the mod can see where the SFXWeapon lives.
void DumpWeaponChainNow() noexcept
{
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) { ME2VR::Log::Line("[ME2WPN] no lp"); return; }
    std::uintptr_t pc = 0, pawn = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) { ME2VR::Log::Line("[ME2WPN] no pc"); return; }
    if (!SafeReadPtr(pc + 0x294, &pawn) || pawn < 0x10000) { ME2VR::Log::Line("[ME2WPN] no pawn"); return; }
    char nm[96] = {}, b[224] = {};
    std::uintptr_t w = 0;
    if (SafeReadPtr(pawn + 0x844, &w) && w >= 0x10000 && ClassNameOf(w, nm, sizeof(nm)))
    {
        sprintf_s(b, "[ME2WPN] WeaponFromLastGameState=%s 0x%llX", nm, static_cast<unsigned long long>(w)); ME2VR::Log::Line(b);
        // dump the weapon actor's component arrays (0x60 Components, 0x80 AllComponents) to confirm the mesh is hideable
        const std::uintptr_t arrs[2] = { 0x60, 0x80 };
        for (int a = 0; a < 2; ++a)
        {
            std::uintptr_t data = 0; int num = 0;
            if (SafeReadPtr(w + arrs[a], &data) && SafeReadInt(w + arrs[a] + 8, &num) && data >= 0x10000 && num > 0 && num <= 64)
                for (int i = 0; i < num; ++i)
                {
                    std::uintptr_t comp = 0;
                    if (SafeReadPtr(data + static_cast<std::uintptr_t>(i) * 8, &comp) && comp >= 0x10000 && ClassNameOf(comp, nm, sizeof(nm)))
                        { sprintf_s(b, "[ME2WPN]   wpnComp[%s %d]=%s 0x%llX", a == 0 ? "C" : "All", i, nm, static_cast<unsigned long long>(comp)); ME2VR::Log::Line(b); }
                }
            else { sprintf_s(b, "[ME2WPN]   wpn arr 0x%llX num=%d (empty)", static_cast<unsigned long long>(arrs[a]), num); ME2VR::Log::Line(b); }
        }
        // Scan the weapon actor's fields for ANY component/mesh pointer (the visible mesh isn't in the
        // 0x60/0x80 arrays). Logs offset + class so the mod can find the real drawn gun mesh.
        for (std::uintptr_t off = 0x40; off < 0x600; off += 8)
        {
            std::uintptr_t p = 0;
            if (!SafeReadPtr(w + off, &p) || p < 0x10000 || (p & 0xF) != 0) continue;
            if (ClassNameOf(p, nm, sizeof(nm)) && (std::strstr(nm, "Component") != nullptr || std::strstr(nm, "Mesh") != nullptr))
                { sprintf_s(b, "[ME2WPN]   wfield 0x%llX -> %s 0x%llX", static_cast<unsigned long long>(off), nm, static_cast<unsigned long long>(p)); ME2VR::Log::Line(b); }
        }
    }
    else ME2VR::Log::Line("[ME2WPN] WeaponFromLastGameState=null");
    if (SafeReadPtr(pawn + 0x84C, &w) && w >= 0x10000 && ClassNameOf(w, nm, sizeof(nm)))
        { sprintf_s(b, "[ME2WPN] WeaponOnDeck=%s 0x%llX", nm, static_cast<unsigned long long>(w)); ME2VR::Log::Line(b); }
    else ME2VR::Log::Line("[ME2WPN] WeaponOnDeck=null");
    // The decisive scan: pawn.Mesh(0x458).Attachments(0x3BC) bone-attached components = the visible weapon.
    std::uintptr_t bmesh = 0;
    if (SafeReadPtr(pawn + 0x458, &bmesh) && bmesh >= 0x10000)
    {
        std::uintptr_t adata = 0; int anum = 0;
        if (SafeReadPtr(bmesh + 0x3BC, &adata) && SafeReadInt(bmesh + 0x3BC + 8, &anum) && adata >= 0x10000 && anum > 0 && anum <= 64)
        {
            sprintf_s(b, "[ME2WPN] Mesh.Attachments num=%d", anum); ME2VR::Log::Line(b);
            for (int i = 0; i < anum; ++i)
            {
                std::uintptr_t comp = 0;
                if (SafeReadPtr(adata + static_cast<std::uintptr_t>(i) * 0x34, &comp) && comp >= 0x10000 && ClassNameOf(comp, nm, sizeof(nm)))
                    { sprintf_s(b, "[ME2WPN]   attach[%d]=%s 0x%llX", i, nm, static_cast<unsigned long long>(comp)); ME2VR::Log::Line(b); }
            }
        }
        else { sprintf_s(b, "[ME2WPN] Mesh.Attachments num=%d (empty)", anum); ME2VR::Log::Line(b); }
    }
    std::uintptr_t inv = 0;
    if (SafeReadPtr(pawn + 0x294, &inv))
        for (int i = 0; i < 24 && inv >= 0x10000; ++i)
        {
            if (ClassNameOf(inv, nm, sizeof(nm))) { sprintf_s(b, "[ME2WPN] inv[%d]=%s 0x%llX", i, nm, static_cast<unsigned long long>(inv)); ME2VR::Log::Line(b); }
            std::uintptr_t next = 0;
            if (!SafeReadPtr(inv + 0x2C4, &next)) break;
            inv = next;
        }
    ME2VR::Log::Line("[ME2WPN] --- end dump ---");
}

int FpStateCount() noexcept { return kFpCount; }
FpStateCfg* GetFpStateCfg(int id) noexcept { FpEnsureInit(); return (id >= 0 && id < kFpCount) ? &g_fpStates[id] : nullptr; }
const char* FpStateLabel(int id) noexcept { return (id >= 0 && id < kFpCount) ? g_fpDefs[id].label : ""; }
FpStateCfg FpStateDefault(int id) noexcept { return (id >= 0 && id < kFpCount) ? g_fpDefs[id].def : FpStateCfg{}; }

void SetFirstPerson(bool on) noexcept { g_fpEnabled.store(on, std::memory_order_release); }
bool GetFirstPerson() noexcept { return g_fpEnabled.load(std::memory_order_acquire); }
void SetMeshHide(bool on) noexcept { g_meshHidden.store(on, std::memory_order_release); }
bool GetMeshHide() noexcept { return g_meshHidden.load(std::memory_order_acquire); }

bool IsHeadAimActive() noexcept { return g_headAimOn.load(std::memory_order_acquire) && g_weaponOut.load(std::memory_order_acquire); }
void GetCoverFlashState(char* out, int cap, int* hold, bool* raw) noexcept
{
    if (out != nullptr && cap > 0)
    {
        int i = 0; for (; i < cap - 1 && g_cfMode[i] != 0; ++i) out[i] = g_cfMode[i];
        out[i] = 0;
    }
    if (hold != nullptr) *hold = g_cfHold.load(std::memory_order_relaxed);
    if (raw != nullptr)  *raw  = g_cfRaw.load(std::memory_order_relaxed);
}

bool IsWeaponOut() noexcept { return g_weaponOut.load(std::memory_order_acquire); }   // combat/ADS/sniper/cover (vs holstered)
bool IsWeaponFiring() noexcept { return g_weaponFiring.load(std::memory_order_acquire); }
// [CONVOFP] Hide (or restore) the head on an ARBITRARY actor -- the conversation's staged Shepard,
// which is not necessarily the pawn the gameplay FP path owns. Head + hair + headgear + visor +
// faceplate + accessories only; body and weapon are left alone so you still see yourself sitting there.
// Returns the number of components touched (0 = nothing done).
int ConvoFpSetHeadHidden(std::uintptr_t actor, bool hide) noexcept
{
    if (actor < 0x10000 || ActorIsDying(actor)) return 0;
    int count = 0;
    __try
    {
        // HiddenGame, not just bOwnerNoSee. bOwnerNoSee only suppresses a component in views OWNED by
        // its owner, and a conversation renders through the conversation camera -- ME1's lane hit this
        // exact wall ("OwnerNoSee may not hide in convo views"), which is why the head kept showing up.
        // HiddenGame hides it in EVERY view, which is unconditional and fine in single player; the
        // weapon hide in FpHideMeshes already relies on it for the same reason. Both are applied so the
        // component is suppressed whichever path the view takes, and both are undone on restore.
        const std::uintptr_t headOffs[5] = { 0x85C, 0x86C, 0x874, 0x87C, 0x884 };   // Head, Hair, HeadGear, Visor, FacePlate
        for (int i = 0; i < 5; ++i)
        {
            std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(actor + headOffs[i]);
            if (comp < 0x10000) continue;
            CallPrimSetHidden(comp, hide);
            FpSetOwnerNoSee(comp, hide);
            ++count;
        }
        // m_aoAccessories (0x698): helmet / breather / extra headgear -- hide WITH the head, or a
        // helmet rim hangs in the middle of the view with the face gone from behind it.
        std::uintptr_t adata = *reinterpret_cast<std::uintptr_t volatile*>(actor + 0x698);
        const int anum = *reinterpret_cast<int volatile*>(actor + 0x698 + 8);
        if (adata >= 0x10000 && anum > 0 && anum <= 64)
            for (int i = 0; i < anum; ++i)
            {
                std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(adata + static_cast<std::uintptr_t>(i) * 8);
                if (comp < 0x10000) continue;
                CallPrimSetHidden(comp, hide);
                FpSetOwnerNoSee(comp, hide);
                ++count;
            }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// [CONVOFP] ACamera.CameraCache.POV.FOV. CameraCache @0x464, FTPOV is Location(0x0)+Rotation(0xC)+
// FOV(0x18) = 0x1C, so the FOV float is at 0x47C. PlayerCamera hangs off the PC at 0x6A0.
//
// This is the other half of relocating at the source. Moving the camera position early was not enough:
// the engine also builds its CULLING frustum from the camera's FOV, and a conversation's is ~20 deg
// while the mod renders at the headset's ~50. Everything outside that narrow cone is culled before it is
// ever submitted, so parts of the person you are talking to are simply absent and turning your head
// slides different parts in and out of the cone. Widening the projection AFTER the view is built (what
// ApplyFov does) changes the picture but not that decision.
//
// Deliberately over-wide: the culling cone only needs to be GENEROUS, because ApplyFov still sets the
// exact render FOV afterwards. Over-widening costs a few extra submitted primitives and buys immunity
// to head rotation, which is applied render-side and therefore invisible to the engine's frustum.
// Returns the previous value through outPrev so the caller can put it back.
bool ConvoFpSetCameraFovDeg(float deg, float* outPrev) noexcept
{
    if (!(deg > 1.0f && deg < 179.0f)) return false;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp < 0x10000) return false;
    std::uintptr_t pc = 0, cam = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return false;
    if (!SafeReadPtr(pc + 0x6A0, &cam) || cam < 0x10000) return false;   // APlayerController.PlayerCamera
    if (ActorIsDying(cam)) return false;
    __try
    {
        auto* f = reinterpret_cast<volatile float*>(cam + 0x47C);
        const float prev = *f;
        if (!(prev > 1.0f && prev < 179.0f)) return false;   // not a sane FOV -> wrong object, do not write
        if (outPrev != nullptr) *outPrev = prev;
        *f = deg;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [FILLSRC] read-only twin of ConvoFpSetCameraFovDeg: the camera's current cached FOV in degrees.
bool GetCameraFovDeg(float* outDeg) noexcept
{
    if (outDeg == nullptr) return false;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp < 0x10000) return false;
    std::uintptr_t pc = 0, cam = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return false;
    if (!SafeReadPtr(pc + 0x6A0, &cam) || cam < 0x10000) return false;
    __try
    {
        const float v = *reinterpret_cast<volatile float*>(cam + 0x47C);
        if (!(v > 1.0f && v < 179.0f)) return false;   // not a sane FOV -> wrong object
        *outDeg = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [CONVOFP] ABioStage.m_bDOFActive -- bit 0x00000008 in the dword at 0x344. This is BioWare's own
// per-conversation depth-of-field switch, so clearing it turns DOF off for THIS conversation only and
// leaves the global setting alone. A single bit on an actor that outlives the scene, which is the same
// risk class as the camera-mode writes the FP path already relies on (and unlike per-frame pawn or
// component writes, which is the family that crashed ME2 on load).
bool ConvoFpReadStageDofActive(std::uintptr_t stage, bool* outActive) noexcept
{
    if (stage < 0x10000 || outActive == nullptr) return false;
    __try
    {
        *outActive = (*reinterpret_cast<std::uint32_t volatile*>(stage + 0x344) & 0x00000008u) != 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ConvoFpSetStageDofActive(std::uintptr_t stage, bool active) noexcept
{
    if (stage < 0x10000 || ActorIsDying(stage)) return false;
    __try
    {
        auto* p = reinterpret_cast<std::uint32_t volatile*>(stage + 0x344);
        const std::uint32_t v = *p;
        *p = active ? (v | 0x00000008u) : (v & ~0x00000008u);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [CONVOFP] Ask the engine to drop depth of field. Conversation DOF is driven by a matinee track that
// pulls focus to the current speaker; from inside Shepard's head that blurs whoever you are looking at.
// Returns true if the call was made.
bool ConvoFpDisableDof() noexcept
{
    void* fn = FindDisableDofFn();
    if (fn == nullptr) return false;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp < 0x10000) return false;
    std::uintptr_t pc = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return false;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + 0xD8D50);
        pe(reinterpret_cast<void*>(pc), fn, nullptr, nullptr);   // DisableDOF() takes no parameters
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int  GetFpHeadHideDelay() noexcept { return g_fpHeadHideDelay.load(std::memory_order_acquire); }
void SetFpHeadHideDelay(int f) noexcept { g_fpHeadHideDelay.store((f < 0) ? 0 : (f > 60 ? 60 : f), std::memory_order_release); }
bool IsCamModeStorm() noexcept { return g_camModeStorm.load(std::memory_order_relaxed); }
bool IsCamModeInterp() noexcept { return g_camModeInterp.load(std::memory_order_relaxed); }
bool IsCamModeAiming() noexcept { return g_camModeAiming.load(std::memory_order_relaxed); }
bool IsFpBlend() noexcept { return g_fpBlendActive.load(std::memory_order_acquire); }   // diag: held FP through a blend
void SetHeadAimEnabled(bool on) noexcept { g_headAimOn.store(on, std::memory_order_release); }
bool GetHeadAimEnabled() noexcept { return g_headAimOn.load(std::memory_order_acquire); }
void SetHeadAimInvertYaw(bool on) noexcept { g_headAimInvYaw.store(on, std::memory_order_release); }
bool GetHeadAimInvertYaw() noexcept { return g_headAimInvYaw.load(std::memory_order_acquire); }
void SetHeadAimInvertPitch(bool on) noexcept { g_headAimInvPitch.store(on, std::memory_order_release); }
bool GetHeadAimInvertPitch() noexcept { return g_headAimInvPitch.load(std::memory_order_acquire); }

// Drive the gun/aim from the HMD while a weapon is out. Injection model: read the live ControlRotation,
// strip last frame's head delta to recover the stick/mouse aim, and write stick + head (pitch-clamped).
// If the stick/mouse moved the aim, rebase the head center to it (no snap). When the weapon holsters or
// head-aim turns off, subtract the mod's injection so the underlying aim is clean. Call every frame; it self-gates.
// [AIMSEED v2] look->aim handoff ramp state (ME1 xr_session model, ported 2026-07-20). When aim
// activates while render-side head-look was applied (storm exit, weapon draw while free-looking),
// the look offset is captured as a ramp target and transferred into the ControlRotation injection
// ~25%/frame; the caller renders the untransferred REMAINDER as head-look so the view never moves
// while CR (and the pawn's ANIMATED weapon aim, which only tracks CR *changes* per frame) glides to
// the gaze. One-shot CR steps leave the animated aim stranded = "bullets don't go where the cursor
// is" (ME1 lesson: never step ControlRotation in one frame - ramp it).
int g_seedRemYawUU = 0, g_seedRemPitchUU = 0;
int g_seedDoneYawUU = 0, g_seedDonePitchUU = 0;
// [STORMPITCH] pitch-only mode. During a sprint the head must not drive aim YAW (the sprint heading
// doesn't track ControlRotation 1:1 - that's the "runs the wrong way" regression), but PITCH is free:
// it has nothing to do with heading. Driving pitch continuously through the sprint means the aim is
// already looking where the player is when they stop and pull the trigger, instead of sitting flat and then
// ramping up over ~0.15s ("shots go straight before correcting up"). Nothing to transfer = nothing to wait for.
bool g_aimPitchOnly = false;

void DriveHeadAim(float headYawDeg, float headPitchDeg, bool allowAim, bool pitchOnly) noexcept
{
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0 || IsGamePaused()) return;
    std::uintptr_t pc = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000 || ActorIsDying(pc)) return;

    const bool want = g_headAimOn.load(std::memory_order_acquire) &&
                      g_weaponOut.load(std::memory_order_acquire) && (allowAim || pitchOnly);
    int p = 0, y = 0, r = 0;
    if (!want)
    {
        if (g_aimActive && ReadCtrlRot(pc, &p, &y, &r))   // leaving aim -> remove the mod's injection cleanly
            WriteCtrlRot(pc, p - g_aimAppliedPitchUU, y - g_aimAppliedYawUU, r);
        g_aimActive = false; g_aimBaseValid = false; g_aimAppliedYawUU = 0; g_aimAppliedPitchUU = 0;
        g_seedRemYawUU = 0; g_seedRemPitchUU = 0; g_seedDoneYawUU = 0; g_seedDonePitchUU = 0;
        g_aimPitchOnly = false;
        return;
    }
    if (!ReadCtrlRot(pc, &p, &y, &r)) return;
    constexpr float kDegToUU = 65536.0f / 360.0f;
    if (!g_aimActive)   // entering aim: center on the current head; seed the look->aim ramp
    {
        // Signs (ME1-verified, same SetHeadLook/ApplyHeadRotation convention here): look +UU = view
        // turned LEFT, ControlRotation +UU = turn RIGHT -> yaw flips; pitch +up on both sides -> 1:1.
        g_seedRemYawUU   = pitchOnly ? 0 : -static_cast<int>(ME2VR::CalcViewHook::HeadLookYawUU());
        g_seedRemPitchUU =  static_cast<int>(ME2VR::CalcViewHook::HeadLookPitchUU());
        g_seedDoneYawUU = 0; g_seedDonePitchUU = 0;
        g_aimRefYawDeg = headYawDeg; g_aimRefPitchDeg = headPitchDeg;
        g_aimAppliedYawUU = 0; g_aimAppliedPitchUU = 0;
        g_aimActive = true; g_aimPitchOnly = pitchOnly;
        if (g_seedRemYawUU != 0 || g_seedRemPitchUU != 0)
            ME2VR::Log::Line("[HEADAIM] head aim ON, ramping in head-look offset yawUU=" +
                             std::to_string(g_seedRemYawUU) + " pitchUU=" + std::to_string(g_seedRemPitchUU));
    }
    else if (g_aimPitchOnly != pitchOnly)
    {
        // [STORMPITCH] switching between pitch-only (sprinting) and full aim.
        if (pitchOnly)
        {
            // Full -> pitch-only: hand YAW back to render head-look. No unwind needed here - headYawUU
            // becomes 0 below and the strip-last-frame step removes the old injection on this very frame,
            // while head-look picks the yaw up the same frame, so the view doesn't move.
            g_seedRemYawUU = 0; g_seedDoneYawUU = 0;
        }
        else
        {
            // Pitch-only -> full (sprint ended, or the trigger was pulled): take yaw back from head-look,
            // ramped as usual. Pitch is already where it should be, so it is NOT re-seeded - that is the
            // whole point: the shot goes up immediately, only the yaw half glides.
            g_seedRemYawUU = -static_cast<int>(ME2VR::CalcViewHook::HeadLookYawUU());
            g_seedDoneYawUU = 0;
            g_aimRefYawDeg = headYawDeg;
        }
        g_aimPitchOnly = pitchOnly;
    }

    // [AIMSEED v2] advance the ramp: ~25% of the remainder per frame (min 150 UU so the tail doesn't crawl).
    const auto rampStep = [](int& rem, int& done) noexcept {
        if (rem == 0) return;
        constexpr int kMinStep = 150;
        int step = rem / 4;
        if (rem > 0) { if (step < kMinStep) step = (rem < kMinStep ? rem : kMinStep); }
        else         { if (step > -kMinStep) step = (rem > -kMinStep ? rem : -kMinStep); }
        done += step; rem -= step;
    };
    rampStep(g_seedRemYawUU, g_seedDoneYawUU);
    rampStep(g_seedRemPitchUU, g_seedDonePitchUU);

    const float sy = g_headAimInvYaw.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const float sp = g_headAimInvPitch.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const int headYawUU = pitchOnly ? 0   // [STORMPITCH] sprint: yaw stays on the stick, head owns pitch only
                                    : static_cast<int>(sy * WrapDegF(headYawDeg - g_aimRefYawDeg) * kDegToUU) + g_seedDoneYawUU;
    const int headPitchUU = static_cast<int>(sp * (headPitchDeg - g_aimRefPitchDeg) * kDegToUU) + g_seedDonePitchUU;
    // ME1 model: strip last frame's injection, add this frame's on top of the game's own (stick) aim.
    // NO stick-move rebase (the old ME2 clause reset the head center on ANY stick twitch > ~0.35deg,
    // so the injected aim kept collapsing mid-firefight = "aim isn't accurate").
    const int rawStickYaw = y - g_aimAppliedYawUU;
    const int rawStickPitch = p - g_aimAppliedPitchUU;
    int newYaw = rawStickYaw + headYawUU;
    int newPitch = NormUU(rawStickPitch) + headPitchUU;
    const int clampUU = static_cast<int>(80.0f * kDegToUU);
    if (newPitch > clampUU) newPitch = clampUU; else if (newPitch < -clampUU) newPitch = -clampUU;
    WriteCtrlRot(pc, newPitch, newYaw, r);
    g_aimAppliedYawUU = headYawUU; g_aimAppliedPitchUU = headPitchUU;
}

// [AIMSEED v2] the untransferred look->aim remainder; the caller renders it as head-look so view
// stays put during the handoff (and [MOVEFIX] steers by it automatically via HeadLookYawUU).
void GetAimSeedRem(int* yawUU, int* pitchUU) noexcept
{
    if (yawUU) *yawUU = g_seedRemYawUU;
    if (pitchUU) *pitchUU = g_seedRemPitchUU;
}

// ===== [VEHAIM] Vehicle head-aim (Hammerhead / Firewalker). The hover vehicle (ASFXVehicleHover :
// ASVehicle) aims via DriverViewYaw/Pitch (int UU, stick-ACCUMULATED - that's why the crosshair
// "moves freely"). On-foot head-aim writes ControlRotation gated on weaponOut, which is false in a
// vehicle, so it never runs. Mirror the on-foot additive model onto DriverViewYaw/Pitch instead.
// INSTRUMENTED: ME1 never fully solved vehicle aim (the Mako had 3 fighting aim reps); the [VEHAIM]
// log shows whether the mod's write STICKS or the game re-issues it from the stick each tick. =====
// [VEHAIM] vehicle head mode: 0 = off (head does nothing in a vehicle), 1 = FREE-LOOK (head turns the
// VR view independently; aim + camera stay on the stick), 2 = CAMERA-AIM (head drives DriverViewYaw/
// Pitch = the chase camera; the log confirmed this moves the CAMERA, not the cannon reticle).
std::atomic<int> g_vehHeadMode{1};   // default free-look (the comfortable VR default)
float g_vehRefYawDeg = 0.0f, g_vehRefPitchDeg = 0.0f;
int g_vehAppliedYawUU = 0, g_vehAppliedPitchUU = 0;
bool g_vehAimActive = false;

// True when the player is DRIVING a vehicle (so the caller can pick the head behavior). Fills *pawnOut.
bool InDrivableVehicle(std::uintptr_t* pawnOut) noexcept
{
    if (pawnOut) *pawnOut = 0;
    if (ReadGameModeSEH() != 4) return false;   // 4 = Vehicle (LE2 EGameModes)
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0 || IsGamePaused()) return false;
    std::uintptr_t pc = 0, pawn = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000 || ActorIsDying(pc)) return false;
    if (!SafeReadPtr(pc + 0x294, &pawn) || pawn < 0x10000 || ActorIsDying(pawn)) return false;
    char nm[96] = {};
    if (!ClassNameOf(pawn, nm, sizeof(nm)) || std::strstr(nm, "Vehicle") == nullptr) return false;
    if (pawnOut) *pawnOut = pawn;
    return true;
}

// CAMERA-AIM (mode 2): drive DriverViewYaw/Pitch (the chase camera) from the head. Caller parks render
// head-look. Returns true if it wrote. Not the cannon reticle (see log) - kept as an option.
bool DriveVehicleAim(float headYawDeg, float headPitchDeg) noexcept
{
    std::uintptr_t pawn = 0;
    if (!InDrivableVehicle(&pawn) || pawn == 0) { g_vehAimActive = false; return false; }
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    std::uintptr_t pc = 0;
    if (lp == 0 || !SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) { g_vehAimActive = false; return false; }

    // [VEHDIAG] proved ControlRotation (controller) == DriverViewYaw/Pitch (pawn), always - the game
    // keeps them synced and BOTH are the aim (the cannon fires along it). Writing only one gets reverted
    // to the other next frame (= "camera moved, aim didn't"). So write BOTH to the same head value.
    int cyaw = 0, cpitch = 0, croll = 0, dyaw = 0, dpitch = 0;
    if (!ReadCtrlRot(pc, &cpitch, &cyaw, &croll)) { g_vehAimActive = false; return false; }
    ReadVehAim(pawn, &dyaw, &dpitch);   // == ctrl; used only as fallback raw
    const int yaw = cyaw, pitch = cpitch;

    constexpr float kDegToUU = 65536.0f / 360.0f;
    if (!g_vehAimActive)   // entering vehicle aim: center on the current head + turret
    {
        g_vehRefYawDeg = headYawDeg; g_vehRefPitchDeg = headPitchDeg;
        g_vehAppliedYawUU = 0; g_vehAppliedPitchUU = 0; g_vehAimActive = true;
    }
    const float sy = g_headAimInvYaw.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const float sp = g_headAimInvPitch.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const int headYawUU = static_cast<int>(sy * WrapDegF(headYawDeg - g_vehRefYawDeg) * kDegToUU);
    const int headPitchUU = static_cast<int>(sp * (headPitchDeg - g_vehRefPitchDeg) * kDegToUU);
    const int rawYaw = yaw - g_vehAppliedYawUU;          // strip last frame's injection (stick-only value)
    const int rawPitch = pitch - g_vehAppliedPitchUU;
    int newYaw = rawYaw + headYawUU;
    int newPitch = NormUU(rawPitch) + headPitchUU;
    const int clampUU = static_cast<int>(45.0f * kDegToUU);   // cannon elevation cone
    if (newPitch > clampUU) newPitch = clampUU; else if (newPitch < -clampUU) newPitch = -clampUU;
    // Mode 2 = cannon + CAMERA follow head (write both; crosshair stays centered, view swings).
    // Mode 3 = cannon only, CAMERA stays on the stick (write ControlRotation only; the crosshair/cannon
    //          leave screen-center to where you look, the view holds still). DriverView is the camera and
    //          is separately writable (proven: writing it alone moved only the camera), so leaving it
    //          un-written keeps the camera on the stick.
    const bool alsoCamera = (g_vehHeadMode.load(std::memory_order_relaxed) != 3);
    WriteCtrlRot(pc, newPitch, newYaw, croll);   // the aim/fire source + crosshair (on-foot uses this too)
    if (alsoCamera) WriteVehAim(pawn, newYaw, newPitch);
    g_vehAppliedYawUU = headYawUU; g_vehAppliedPitchUU = headPitchUU;

    static uint64_t s_n = 0;
    if ((s_n++ % 60) == 0)
    {
        char b[176] = {};
        sprintf_s(b, "[VEHAIM] ctrl y=%d p=%d drv y=%d p=%d wrote y=%d p=%d headYUU=%d headPUU=%d",
                  cyaw, cpitch, dyaw, dpitch, newYaw, newPitch, headYawUU, headPitchUU);
        ME2VR::Log::Line(b);
    }
    return true;
}
int  GetVehicleHeadMode() noexcept { return g_vehHeadMode.load(std::memory_order_acquire); }
void SetVehicleHeadMode(int m) noexcept { g_vehHeadMode.store((m < 0) ? 0 : (m > 3 ? 3 : m), std::memory_order_release); }
bool IsInDrivableVehicle() noexcept { return InDrivableVehicle(nullptr); }

// [VEHDIAG] PURE-READ diagnostic: log the TWO aim candidates side by side while driving, so the mod can see
// which one tracks the crosshair/fire when aiming with the stick - ControlRotation (the on-foot
// fire source, on the CONTROLLER) vs DriverViewYaw/Pitch (the chase camera, on the PAWN). No writes.
// (needs its own POD frame for the SEH reads.)
bool ReadCRandDV(std::uintptr_t pc, std::uintptr_t pawn, int* crP, int* crY, int* dvP, int* dvY, int* firing) noexcept
{
    __try {
        *crP = *reinterpret_cast<int volatile*>(pc + kCtrlRotation);
        *crY = *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 4);
        *dvP = *reinterpret_cast<int volatile*>(pawn + kVehDVPitch);
        *dvY = *reinterpret_cast<int volatile*>(pawn + kVehDVYaw);
        *firing = (*reinterpret_cast<unsigned int volatile*>(pawn + 0x93C) & 0x1u) ? 1 : 0;   // bWeaponFiring
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ProbeVehicleFields() noexcept
{
    std::uintptr_t pawn = 0;
    if (!InDrivableVehicle(&pawn) || pawn == 0) return;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    std::uintptr_t pc = 0;
    if (lp == 0 || !SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return;
    int crP = 0, crY = 0, dvP = 0, dvY = 0, firing = 0;
    if (!ReadCRandDV(pc, pawn, &crP, &crY, &dvP, &dvY, &firing)) return;
    static uint64_t s_n = 0;
    // Log ~3/s, and ALWAYS on a firing frame (marked *FIRE*) so the mod can correlate aim with the shot.
    if (firing || (s_n++ % 20) == 0)
    {
        char b[192] = {};
        sprintf_s(b, "[VEHDIAG]%s CtrlRot y=%d p=%d | DriverView y=%d p=%d",
                  firing ? " *FIRE*" : "", crY, crP, dvY, dvP);
        ME2VR::Log::Line(b);
    }
}

void TryDumpOnce() noexcept
{
    if (g_dumped.load(std::memory_order_acquire)) return;

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) return;

    // 1) GEngine
    std::uintptr_t gEngine = 0;
    if (!SafeReadPtr(base + kGEngineRva, &gEngine) || gEngine == 0)
    {
        // Engine global not populated yet (very early), or wrong RVA. Don't latch; retry.
        return;
    }

    // 2) GamePlayers TArray on the engine
    std::uintptr_t playersData = 0;
    int playersNum = 0;
    if (!SafeReadPtr(gEngine + kEngineGamePlayers + kTArrayData, &playersData) ||
        !SafeReadInt(gEngine + kEngineGamePlayers + kTArrayNum, &playersNum))
    {
        return;
    }
    if (playersData == 0 || playersNum < 1 || playersNum > 8)
    {
        // Not in a game world yet (no local player). Log a few times, then go quiet.
        const auto w = g_waitLogs.fetch_add(1, std::memory_order_relaxed);
        if (w < 4)
        {
            ME2VR::Log::Line("[ME2DISC] EngineProbe waiting: GEngine=" + Hex(gEngine) +
                             " GamePlayers.Num=" + std::to_string(playersNum) +
                             " (load a save / reach gameplay)");
        }
        return;
    }

    // 3) Primary ULocalPlayer (P1) = GamePlayers[0]
    std::uintptr_t p1 = 0;
    if (!SafeReadPtr(playersData + 0 * sizeof(void*), &p1) || p1 == 0) return;

    // 4) ULocalPlayer fields (the offsets the mod claims are LE1-identical -- confirm live)
    std::uintptr_t viewState = 0, viewportClient = 0;
    float origin[2] = {-1, -1}, size[2] = {-1, -1};
    int controllerId = -999;
    SafeReadPtr(p1 + kLpViewState, &viewState);
    SafeReadPtr(p1 + kLpViewportClient, &viewportClient);
    SafeReadFloat2(p1 + kLpOrigin, origin);
    SafeReadFloat2(p1 + kLpSize, size);
    SafeReadInt(p1 + kLpControllerId, &controllerId);

    // 5) FViewportClient vtable off the viewport client -> Draw slot
    std::uintptr_t vpcPrimaryVtable = 0, fvpVtable = 0, drawAddr = 0;
    if (viewportClient != 0)
    {
        SafeReadPtr(viewportClient + 0, &vpcPrimaryVtable);
        if (SafeReadPtr(viewportClient + kVpcFViewportClientVtable, &fvpVtable) && fvpVtable != 0)
        {
            SafeReadPtr(fvpVtable + static_cast<std::uintptr_t>(kDrawVtableSlot) * sizeof(void*), &drawAddr);
        }
    }

    // Latch BEFORE logging so the mod dumps exactly once.
    bool expected = false;
    if (!g_dumped.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    ME2VR::Log::Line("[ME2DISC] ===== EngineProbe dump (read-only) =====");
    ME2VR::Log::Line("[ME2DISC] base=" + Hex(base) + " GEngine=" + Hex(gEngine) +
                     " (rva " + Hex(kGEngineRva) + ") GamePlayers.Num=" + std::to_string(playersNum));
    ME2VR::Log::Line("[ME2DISC] P1 ULocalPlayer=" + Hex(p1));

    char fbuf[256] = {};
    sprintf_s(fbuf,
              "[ME2DISC]   Origin=(%.3f,%.3f) Size=(%.3f,%.3f) ControllerId=%d  <-- expect Origin~(0,0) Size~(1,1)",
              origin[0], origin[1], size[0], size[1], controllerId);
    ME2VR::Log::Line(fbuf);

    ME2VR::Log::Line("[ME2DISC]   ViewState=" + Hex(viewState) +
                     " ViewportClient=" + Hex(viewportClient));

    if (drawAddr != 0)
    {
        const std::uintptr_t drawRva = (drawAddr > base) ? (drawAddr - base) : 0;
        ME2VR::Log::Line("[ME2DISC]   FViewportClient vtable=" + Hex(fvpVtable) +
                         " (rva " + Hex((fvpVtable > base) ? fvpVtable - base : 0) + ")");
        ME2VR::Log::Line("[ME2DISC]   >>> FViewportClient::Draw (slot 2) = " + Hex(drawAddr) +
                         "  rva=MassEffect2.exe+" + Hex(drawRva) + " <<<");
        ME2VR::Log::Line("[ME2DISC]   VPC primary vtable rva=" +
                         Hex((vpcPrimaryVtable > base) ? vpcPrimaryVtable - base : 0));
    }
    else
    {
        ME2VR::Log::Line("[ME2DISC]   Draw slot resolve FAILED (viewportClient=" + Hex(viewportClient) +
                         " fvpVtable=" + Hex(fvpVtable) + ")");
    }

    const bool offsetsSane =
        origin[0] > -2.0f && origin[0] < 2.0f && size[0] > 0.1f && size[0] < 2.0f &&
        viewportClient > 0x10000 && viewState != 0;
    ME2VR::Log::Line(std::string("[ME2DISC] EngineProbe verdict: ") +
                     (offsetsSane ? "OFFSETS LOOK VALID for this build -> proceed to CalcSceneView hunt"
                                  : "OFFSETS LOOK WRONG -> installed exe likely differs from SDK build; switch to pattern scan"));
    ME2VR::Log::Line("[ME2DISC] ==========================================");
}
}
