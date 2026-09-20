#include "convo_fp.h"

#include "calcview_hook.h"
#include "engine_probe.h"
#include "logger.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
// --- LE2 offsets (ME3Tweaks LExSDKv2, SDK_TARGET_LE2) -----------------------------------------
// Chain: ULocalPlayer -> PlayerController(0x68) -> WorldInfo(0x1B0) -> m_oCurrentConversation(0xA4C).
// The WorldInfo hop is the same one IsGamePaused() already uses, so it is proven live on this build.
constexpr std::uintptr_t kLpPlayerController = 0x68;
constexpr std::uintptr_t kPcWorldInfo        = 0x1B0;
constexpr std::uintptr_t kWiCurrentConv      = 0xA4C;   // ABioWorldInfo.m_oCurrentConversation

// UBioConversation. Every one of these is CPF_Transient -- live, rewritten as the scene plays.
constexpr std::uintptr_t kConvSpeakerList    = 0x0D8;   // TArray<FBioDialogSpeaker>, stride 0x10, aSpeaker @+0x08
constexpr std::uintptr_t kConvLookAtList     = 0x0E8;   // TArray<FBioDialogLookat>,  stride 0x14
constexpr std::uintptr_t kConvOwner          = 0x2C0;   // m_pConversationOwner
constexpr std::uintptr_t kConvPlayer         = 0x2C8;   // m_pConversationPlayer  <- the STAGED Shepard
constexpr std::uintptr_t kConvSpeaker        = 0x2D0;   // m_pSpeaker (who is talking right now)
constexpr std::uintptr_t kConvListener       = 0x2E0;   // m_pListener
constexpr std::uintptr_t kConvStage          = 0x2F0;   // m_stage (ABioStage) -- owns the DOF switch

constexpr std::uintptr_t kLookAtStride       = 0x14;
constexpr std::uintptr_t kLookAtActor        = 0x00;
constexpr std::uintptr_t kLookAtTarget       = 0x08;
constexpr std::uintptr_t kSpeakerStride      = 0x10;
constexpr std::uintptr_t kSpeakerActor       = 0x08;

// AActor (LE2 -- NOT ME1's 0x108/0x114; the layouts differ and reading ME1's would be garbage).
constexpr std::uintptr_t kActorLocation      = 0x118;   // FVector
constexpr std::uintptr_t kActorRotation      = 0x124;   // FRotator (int32 Pitch, Yaw, Roll)

// ABioPawn meshes, and UPrimitiveComponent.LocalToWorld (FMatrix; translation = row 3 @ +0x30).
constexpr std::uintptr_t kPawnHeadMesh       = 0x85C;
constexpr std::uintptr_t kPawnBodyMesh       = 0x458;
constexpr std::uintptr_t kPrimLocalToWorld   = 0x0C0;
constexpr std::uintptr_t kMatrixTranslation  = 0x030;
// USkeletalMeshComponent.SpaceBases: TArray<FMatrix> of COMPONENT-space bone transforms.
constexpr std::uintptr_t kSkelSpaceBases     = 0x2B8;
// APawn.EyeHeight -- the engine's own viewpoint height above the pawn's capsule centre.
constexpr std::uintptr_t kPawnEyeHeight      = 0x564;

// A component's LocalToWorld origin is the MESH ROOT, which on a ME pawn sits at the FEET -- reading
// it as the head put the first build's camera 90uu underground. The head is a BONE, so it has to come
// out of SpaceBases. Bounds below keep a bad read or a raised arm from flinging the camera.
constexpr float kHeadMaxAboveEye = 45.0f;   // uu the bone may sit above the engine's eye height
constexpr float kHeadMaxBelowEye = 45.0f;
constexpr float kHeadMaxLateral  = 60.0f;

// UObject reflection -- copied verbatim from engine_probe.cpp. LE2 uses a CHUNKED name pool at
// 0x1668A10 and UObject.Class@0x50 / UClass.Name@0x48. Hand-writing a fresh decoder against guessed
// constants is what produced the "BioSFPanel missing" false negative during the HUD work.
constexpr std::uintptr_t kNamePoolsRva = 0x1668A10;
constexpr std::uintptr_t kObjClass     = 0x50;
constexpr std::uintptr_t kObjNameOff   = 0x48;

constexpr std::uintptr_t kTArrayData = 0x0;
constexpr std::uintptr_t kTArrayNum  = 0x8;

// Frames of last-good pose held across a failed read. Conversations briefly unpossess and re-stage
// actors; without a hold, every blip drops the mod back to the director's shot for a frame and it snaps.
constexpr int kHoldFrames = 60;

// --- config ---------------------------------------------------------------------------------
std::atomic_bool  g_enabled{false};
std::atomic<float> g_turnRate{90.0f};     // deg/s the base yaw eases toward the speaker; 0 = locked
std::atomic<float> g_animFollow{0.65f};   // 0 = steady at staged height, 1 = full animated head
std::atomic<float> g_eyeUpUU{65.0f};      // fallback eye height above actor origin (ME1's cineFpUpUU)
std::atomic<float> g_eyeFwdUU{8.0f};      // fallback eye forward of actor origin
std::atomic<float> g_zoom{1.0f};          // 1 = headset FOV; >1 magnifies (render narrows, declared fixed)
std::atomic_bool  g_hideHead{true};       // camera is INSIDE the skull -- on by default
std::atomic_bool  g_killDof{true};        // conversation DOF focuses the speaker -> blurs who you look at

// --- live state (present thread only) --------------------------------------------------------
bool  g_armed = false;
int   g_holdFrames = 0;
int   g_source = 0;
float g_eye[3] = {};        // published (smoothed) eye position
float g_anchor[3] = {};     // heavy EMA of the raw eye -- the "steady" component
bool  g_anchorInit = false;
float g_baseYaw = 0.0f;
bool  g_baseYawInit = false;

std::uintptr_t g_conv = 0;
std::uintptr_t g_staged = 0;
std::uintptr_t g_speaker = 0;
std::uintptr_t g_faceTarget = 0;
int  g_speakerCount = 0;

std::uintptr_t g_lastLoggedConv = 0;
std::uintptr_t g_lastLoggedSpeaker = 0;
double g_lastLogSec = 0.0;
double g_lastTickSec = 0.0;
unsigned long long g_lastApplies = 0;
unsigned long long g_lastSkips = 0;
// Head-bone diagnostics: why the animated head was or wasn't adopted.
float g_headDz = 0.0f;
float g_headLat = 0.0f;
bool  g_headRead = false;
// Head-hide + DoF state, so each is driven exactly on the edges rather than every frame.
std::uintptr_t g_hiddenActor = 0;
int  g_dofTick = 0;
// Stage DOF: the stage the mod has written to, and its ORIGINAL flag captured BEFORE the mod's first write.
// Capture-once is the rule the objective-circle lane paid two builds to learn -- never re-derive
// "what was this before?" from a value the mod might have authored itself.
std::uintptr_t g_dofStage = 0;
bool g_dofStageOrig = false;
bool g_dofStageCaptured = false;
// How often the game turned m_bDOFActive back ON behind the mod. Non-zero = the stage bit is the lever the
// matinee is fighting the mod for (expected). Zero WHILE blur is still visible = the blur is coming from
// somewhere else entirely -- the camera behaviour's own DOF data -- and the stage bit is a red herring.
unsigned long long g_dofRefights = 0;
double g_probeLastSec = 0.0;   // [ROMANCE] cutscene probe throttle
int g_unhideRetries = 0;       // [HEADBACK] bounded retry for a head-restore that failed mid-teardown

// --- SEH-guarded reads ------------------------------------------------------------------------
bool RdPtr(std::uintptr_t a, std::uintptr_t* out) noexcept
{
    __try { *out = *reinterpret_cast<volatile std::uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool RdInt(std::uintptr_t a, int* out) noexcept
{
    __try { *out = *reinterpret_cast<volatile int*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool RdFloat(std::uintptr_t a, float* out) noexcept
{
    __try { *out = *reinterpret_cast<volatile float*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool RdVec(std::uintptr_t a, float* out3) noexcept
{
    __try
    {
        out3[0] = *reinterpret_cast<volatile float*>(a);
        out3[1] = *reinterpret_cast<volatile float*>(a + 4);
        out3[2] = *reinterpret_cast<volatile float*>(a + 8);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadSfxName(unsigned long long sfxname, char* out, int outSize) noexcept
{
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const unsigned int packed = static_cast<unsigned int>(sfxname & 0xFFFFFFFFull);
        const unsigned int offset = packed & 0x1FFFFFFFu;
        const unsigned int chunk  = (packed >> 29) & 0x7u;
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
    if (!RdPtr(obj + kObjClass, &cls) || cls < 0x10000) return false;
    unsigned long long nm = 0;
    __try { nm = *reinterpret_cast<unsigned long long volatile*>(cls + kObjNameOff); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return ReadSfxName(nm, out, outSize);
}

// bDeleteMe (0x278 bit 0x08) / bPendingDelete (0x27C bit 0x400000). Unreadable -> treat as dying.
bool ActorIsDying(std::uintptr_t actor) noexcept
{
    if (actor < 0x10000) return true;
    __try
    {
        if (*reinterpret_cast<std::uint32_t volatile*>(actor + 0x278) & 0x00000008u) return true;
        if (*reinterpret_cast<std::uint32_t volatile*>(actor + 0x27C) & 0x00400000u) return true;
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

double NowSec() noexcept
{
    LARGE_INTEGER f{}, c{};
    if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) return 0.0;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

std::string Hex(std::uintptr_t v)
{
    char buf[32] = {};
    sprintf_s(buf, "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string F1(float v)
{
    char buf[32] = {};
    sprintf_s(buf, "%.1f", v);
    return buf;
}

// Resolve the live UBioConversation, or 0. Verified by EXACT class name: the power-wheel round-3 bug
// cost two builds because a substring match ("SFXSFHandler_PowerWheel" vs "..._PCPowerWheel") picked
// whichever object GObjects met first. Never substring-match a class again.
std::uintptr_t FindConversation() noexcept
{
    const std::uintptr_t lp = ME2VR::EngineProbe::GetPrimaryLocalPlayer();
    if (lp < 0x10000) return 0;
    std::uintptr_t pc = 0, wi = 0, conv = 0;
    if (!RdPtr(lp + kLpPlayerController, &pc) || pc < 0x10000) return 0;
    if (!RdPtr(pc + kPcWorldInfo, &wi) || wi < 0x10000) return 0;
    if (!RdPtr(wi + kWiCurrentConv, &conv) || conv < 0x10000) return 0;
    char cn[64] = {};
    if (!ClassNameOf(conv, cn, sizeof(cn))) return 0;
    if (std::strcmp(cn, "BioConversation") != 0) return 0;
    return conv;
}

// Highest bone in a skeletal mesh, in WORLD space. SpaceBases holds component-space bone transforms;
// world = boneTranslation * LocalToWorld (row-vector, so translate by row 3 last). On a HEAD mesh the
// topmost bone is the skull, which is what the mod wants and costs no bone-name lookup -- the SDK dumps
// RefSkeleton as an opaque TArray<int>, so indexing it by name would mean guessing FMeshBone's stride.
bool ReadTopBoneWorld(std::uintptr_t comp, float* out3) noexcept
{
    if (comp < 0x10000) return false;
    std::uintptr_t data = 0;
    int num = 0;
    if (!RdPtr(comp + kSkelSpaceBases + kTArrayData, &data) || data < 0x10000) return false;
    if (!RdInt(comp + kSkelSpaceBases + kTArrayNum, &num) || num < 1 || num > 512) return false;

    float best[3] = {};
    bool haveBest = false;
    for (int i = 0; i < num; ++i)
    {
        float t[3] = {};
        if (!RdVec(data + static_cast<std::uintptr_t>(i) * 0x40 + kMatrixTranslation, t)) continue;
        if (!(t[2] > -1.0e6f && t[2] < 1.0e6f)) continue;          // NaN-safe
        if (!haveBest || t[2] > best[2]) { best[0] = t[0]; best[1] = t[1]; best[2] = t[2]; haveBest = true; }
    }
    if (!haveBest) return false;

    float M[16] = {};
    __try
    {
        const volatile float* m = reinterpret_cast<const volatile float*>(comp + kPrimLocalToWorld);
        for (int i = 0; i < 16; ++i) M[i] = m[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    out3[0] = best[0] * M[0] + best[1] * M[4] + best[2] * M[8]  + M[12];
    out3[1] = best[0] * M[1] + best[1] * M[5] + best[2] * M[9]  + M[13];
    out3[2] = best[0] * M[2] + best[1] * M[6] + best[2] * M[10] + M[14];
    return true;
}

// Eye position for the staged actor. Returns the source id (see ConvoFp::Diag::source), 0 on failure.
//
// Base answer is the engine's OWN viewpoint: Location + APawn.EyeHeight. It is exact for a standing
// conversation and can never jitter. The animated head bone is then adopted only if it lands within a
// sane distance of that -- so a bad read, a shared body skeleton, or a raised arm degrades to the
// steady answer instead of throwing the camera across the room.
int ReadStagedEye(std::uintptr_t actor, float* out3) noexcept
{
    if (actor < 0x10000 || ActorIsDying(actor)) return 0;

    float loc[3] = {};
    if (!RdVec(actor + kActorLocation, loc)) return 0;

    float eyeH = 0.0f;
    const bool haveEyeH = RdFloat(actor + kPawnEyeHeight, &eyeH) && eyeH > 1.0f && eyeH < 200.0f;
    out3[0] = loc[0];
    out3[1] = loc[1];
    out3[2] = loc[2] + (haveEyeH ? eyeH : 80.0f);
    int src = haveEyeH ? 1 : 3;

    // Animated head, sanity-bounded against the answer above.
    std::uintptr_t comp = 0;
    float head[3] = {};
    bool haveHead = RdPtr(actor + kPawnHeadMesh, &comp) && comp >= 0x10000 && ReadTopBoneWorld(comp, head);
    if (!haveHead)
        haveHead = RdPtr(actor + kPawnBodyMesh, &comp) && comp >= 0x10000 && ReadTopBoneWorld(comp, head);
    if (haveHead)
    {
        const float dz = head[2] - out3[2];
        const float dx = head[0] - out3[0];
        const float dy = head[1] - out3[1];
        const float lat = std::sqrt(dx * dx + dy * dy);
        g_headDz = dz; g_headLat = lat; g_headRead = true;   // logged, so a rejection is visible not silent
        if (dz < kHeadMaxAboveEye && dz > -kHeadMaxBelowEye && lat < kHeadMaxLateral)
        {
            out3[0] = head[0]; out3[1] = head[1]; out3[2] = head[2];
            src = 2;
        }
    }
    else { g_headRead = false; }

    // Eye height trim ALWAYS applies. It used to be consulted only when APawn.EyeHeight failed to read,
    // which meant the slider did nothing in every real conversation -- one of the dead controls found in testing.
    out3[2] += g_eyeUpUU.load(std::memory_order_relaxed);
    return src;
}

// Who should Shepard be looking at? Ask the engine's own look-at table first -- that is the exact
// data the game uses to turn characters toward each other, so following it is following the scene's
// blocking rather than inventing a parallel camera rule.
std::uintptr_t ResolveFaceTarget(std::uintptr_t conv, std::uintptr_t staged) noexcept
{
    std::uintptr_t data = 0;
    int num = 0;
    if (RdPtr(conv + kConvLookAtList + kTArrayData, &data) && data >= 0x10000 &&
        RdInt(conv + kConvLookAtList + kTArrayNum, &num) && num > 0 && num <= 64)
    {
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t e = data + static_cast<std::uintptr_t>(i) * kLookAtStride;
            std::uintptr_t who = 0, target = 0;
            if (!RdPtr(e + kLookAtActor, &who) || who != staged) continue;
            if (RdPtr(e + kLookAtTarget, &target) && target >= 0x10000 && target != staged)
                return target;
        }
    }

    // Fall back to the live speaker; if Shepard IS the speaker, face whoever is being spoken to.
    std::uintptr_t speaker = 0, listener = 0, owner = 0;
    if (RdPtr(conv + kConvSpeaker, &speaker) && speaker >= 0x10000 && speaker != staged)
        return speaker;
    if (RdPtr(conv + kConvListener, &listener) && listener >= 0x10000 && listener != staged)
        return listener;
    if (RdPtr(conv + kConvOwner, &owner) && owner >= 0x10000 && owner != staged)
        return owner;
    return 0;
}

int CountSpeakers(std::uintptr_t conv) noexcept
{
    std::uintptr_t data = 0;
    int num = 0;
    if (!RdPtr(conv + kConvSpeakerList + kTArrayData, &data) || data < 0x10000) return 0;
    if (!RdInt(conv + kConvSpeakerList + kTArrayNum, &num) || num < 0 || num > 64) return 0;
    return num;
}

// Shortest-path angular step from a toward b, capped at maxStep degrees.
float StepAngle(float a, float b, float maxStep) noexcept
{
    float d = b - a;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    if (maxStep <= 0.0f) return a;
    if (d > maxStep) d = maxStep;
    else if (d < -maxStep) d = -maxStep;
    float r = a + d;
    while (r > 180.0f) r -= 360.0f;
    while (r < -180.0f) r += 360.0f;
    return r;
}

// Put ABioStage.m_bDOFActive back exactly as the mod found it. Restoring from the captured original (not
// from whatever the flag reads now) is what makes this safe to call from anywhere, any number of times.
void RestoreStageDof() noexcept
{
    if (g_dofStageCaptured && g_dofStage >= 0x10000)
        ME2VR::EngineProbe::ConvoFpSetStageDofActive(g_dofStage, g_dofStageOrig);
    g_dofStage = 0;
    g_dofStageOrig = false;
    g_dofStageCaptured = false;
}

// [ROMANCE] Feasibility probe for first-person CUTSCENES (EGameModes 6) -- romance scenes, the Omega 4
// sequence, anything the director drives outside a dialogue. Pure read + log, no writes anywhere.
//
// What it has to answer: does a cutscene give the mod the same handholds a conversation does?
//   conv        -- is a UBioConversation live at all? (cutscenes may run on pure matinee instead)
//   staged      -- m_pConversationPlayer: the actor playing Shepard, which is what the camera rides
//   eye/src     -- can an eye position on that actor
//   pawn        -- the possessed pawn, and whether it is the same object as the staged actor
//   stage       -- ABioStage, i.e. whether the per-scene DOF switch is reachable here too
// If conv resolves and staged reads an eye, first-person romance is the SAME mechanism as convo FP.
// If conv is null, the scene is matinee-only and the camera would have to come from somewhere else --
// most likely the possessed pawn, which the pawn line here also measures.
void ProbeCutscene() noexcept
{
    const double now = NowSec();
    if (now - g_probeLastSec < 2.0) return;   // throttled: cutscenes are long, one line every 2s
    g_probeLastSec = now;

    const std::uintptr_t conv = FindConversation();
    std::uintptr_t staged = 0, stage = 0;
    if (conv >= 0x10000)
    {
        if (!RdPtr(conv + kConvPlayer, &staged)) staged = 0;
        if (!RdPtr(conv + kConvStage,  &stage))  stage  = 0;
    }

    const std::uintptr_t lp = ME2VR::EngineProbe::GetPrimaryLocalPlayer();
    std::uintptr_t pc = 0, pawn = 0;
    if (lp >= 0x10000 && RdPtr(lp + kLpPlayerController, &pc) && pc >= 0x10000)
        RdPtr(pc + 0x294, &pawn);

    // Try the eye on the staged actor first, then on the possessed pawn -- whichever resolves is the
    // one a first-person cutscene camera would ride.
    float eye[3] = {};
    int src = 0;
    std::uintptr_t eyeActor = 0;
    if (staged >= 0x10000) { src = ReadStagedEye(staged, eye); if (src) eyeActor = staged; }
    if (src == 0 && pawn >= 0x10000) { src = ReadStagedEye(pawn, eye); if (src) eyeActor = pawn; }

    char sc[64] = {}, pcn[64] = {}, stc[64] = {};
    if (staged >= 0x10000) ClassNameOf(staged, sc, sizeof(sc));
    if (pawn   >= 0x10000) ClassNameOf(pawn,   pcn, sizeof(pcn));
    if (stage  >= 0x10000) ClassNameOf(stage,  stc, sizeof(stc));

    static const char* kSrc[5] = { "NONE", "eye-height", "head-bone", "actor-loc", "hold" };
    ME2VR::Log::Line(std::string("[ROMANCE] mode=6 conv=") + Hex(conv) +
                     " staged=" + Hex(staged) + (sc[0] ? std::string("(") + sc + ")" : "") +
                     " pawn=" + Hex(pawn) + (pcn[0] ? std::string("(") + pcn + ")" : "") +
                     (staged != 0 && staged == pawn ? " SAME" : " DIFFERENT") +
                     " stage=" + Hex(stage) + (stc[0] ? std::string("(") + stc + ")" : "") +
                     " eyeFrom=" + Hex(eyeActor) +
                     " src=" + kSrc[(src >= 0 && src <= 4) ? src : 0] +
                     " eye=(" + F1(eye[0]) + "," + F1(eye[1]) + "," + F1(eye[2]) + ")" +
                     "  => FP cutscene " + ((src != 0) ? "LOOKS FEASIBLE" : "needs another camera source"));
}

void Disarm() noexcept
{
    RestoreStageDof();   // never leave a conversation with its DOF switch still forced off
    // Put the head back BEFORE clearing state, or Shepard stays headless for the rest of the session.
    // [HEADBACK] The unhide can FAIL at scene end (actor mid-teardown -> ConvoFpSetHeadHidden bails and
    // returns 0) -- clearing g_hiddenActor on a failed write is exactly the "head never came back"
    // glitch. Keep the actor and let the caller's next tick retry (Disarm is called unconditionally
    // from every non-conversation tick now); bounded so a freed pointer can't be poked forever --
    // ConvoFpSetHeadHidden is ActorIsDying+SEH guarded, so the bounded retries themselves are safe.
    if (g_hiddenActor >= 0x10000)
    {
        const int n = ME2VR::EngineProbe::ConvoFpSetHeadHidden(g_hiddenActor, false);
        if (n > 0)
        {
            ME2VR::Log::Line("[CONVOFP] head restored on " + Hex(g_hiddenActor) +
                             " components=" + std::to_string(n));
            g_hiddenActor = 0; g_unhideRetries = 0;
        }
        else if (++g_unhideRetries > 120)   // ~2s of retries, then the actor is genuinely gone
        {
            ME2VR::Log::Line("[CONVOFP] head restore gave up (actor gone) " + Hex(g_hiddenActor));
            g_hiddenActor = 0; g_unhideRetries = 0;
        }
    }
    if (g_armed)
        ME2VR::Log::Line("[CONVOFP] disarmed (conversation ended or reads failed)");
    g_armed = false;
    g_holdFrames = 0;
    g_source = 0;
    g_conv = g_staged = g_speaker = g_faceTarget = 0;
    g_speakerCount = 0;
    g_anchorInit = false;
    g_baseYawInit = false;
    g_lastLoggedConv = 0;
    g_lastLoggedSpeaker = 0;
}
}   // namespace

namespace ME2VR::ConvoFp
{
void Tick() noexcept
{
    const double now = NowSec();
    const double dt = (g_lastTickSec > 0.0 && now > g_lastTickSec) ? (now - g_lastTickSec) : 0.0;
    g_lastTickSec = now;

    // [HEADBACK] Disarm is called UNCONDITIONALLY at every early-out (not gated on g_armed): the head
    // hide fires as soon as the staged actor resolves, BEFORE arming succeeds, so "hidden but never
    // armed" (arming failed, or the feature was toggled off mid-scene -- which [CINEXCL] now does
    // automatically) left Shepard headless with nothing ever restoring him. Disarm is a no-op when
    // there is nothing to restore, so the per-tick calls cost nothing.
    if (!g_enabled.load(std::memory_order_acquire)) { Disarm(); return; }

    // EGameModes 5 = Conversation. The conversation pointer can linger for a moment after the scene
    // ends; the mode byte is the engine's authoritative "the mod is in a conversation right now".
    const int gm = ME2VR::EngineProbe::ReadGameModeSEH();
    if (gm != 5)
    {
        // [ROMANCE] Read-only feasibility probe for first-person CUTSCENES (romance scenes are mode 6,
        // not mode 5, so the conversation path above never sees them). The whole question is whether a
        // cutscene exposes the same things a conversation does: a live UBioConversation, a staged
        // Shepard the mod can put the camera on, and a stage actor for the DOF switch. Changes nothing --
        // it only reports what is reachable, so the mod knows before building anything.
        if (gm == 6) ProbeCutscene();
        Disarm();   // [HEADBACK] unconditional -- see the enabled gate above
        return;
    }

    const std::uintptr_t conv = FindConversation();
    if (conv == 0) { Disarm(); return; }   // [HEADBACK] unconditional

    std::uintptr_t staged = 0;
    if (!RdPtr(conv + kConvPlayer, &staged)) staged = 0;

    // --- head hide, driven as soon as the staged actor resolves -----------------------------------
    // Deliberately BEFORE the eye read: waiting until the mod had a good pose is why the head was visible
    // for a moment at the start of every conversation. Edge-driven, never per frame -- these are
    // ProcessEvent writes into pawn components, the family behind ME2's load crashes when hammered.
    {
        const bool wantHide = g_hideHead.load(std::memory_order_relaxed);
        const std::uintptr_t wantActor = wantHide ? staged : 0;
        if (g_hiddenActor != wantActor)
        {
            if (g_hiddenActor >= 0x10000) ME2VR::EngineProbe::ConvoFpSetHeadHidden(g_hiddenActor, false);
            if (wantActor >= 0x10000)
            {
                const int n = ME2VR::EngineProbe::ConvoFpSetHeadHidden(wantActor, true);
                ME2VR::Log::Line("[CONVOFP] head hidden on staged actor " + Hex(wantActor) +
                                 " components=" + std::to_string(n) + " (HiddenGame + OwnerNoSee)");
            }
            g_hiddenActor = wantActor;
        }
    }

    float raw[3] = {};
    const int source = ReadStagedEye(staged, raw);

    if (source == 0)
    {
        // Read blip. Hold the last good pose rather than snapping back to the director's shot.
        if (g_armed && g_holdFrames < kHoldFrames) { ++g_holdFrames; g_source = 4; return; }
        if (g_armed) Disarm();
        return;
    }

    const bool fresh = (!g_armed || conv != g_conv);
    g_holdFrames = 0;
    g_conv = conv;
    g_staged = staged;
    g_source = source;
    g_speakerCount = CountSpeakers(conv);
    if (!RdPtr(conv + kConvSpeaker, &g_speaker)) g_speaker = 0;

    // --- position: split into a steady anchor + the animation riding on top ---------------------
    // A head bone bobs, and micro-motion at the eye is what makes people sick. The anchor is a heavy
    // EMA of the raw head position (tracks real staging changes over ~1.5s, ignores the bob); the
    // follow slider says how much of the difference to put back.
    if (fresh || !g_anchorInit)
    {
        g_anchor[0] = raw[0]; g_anchor[1] = raw[1]; g_anchor[2] = raw[2];
        g_anchorInit = true;
    }
    else if (dt > 0.0)
    {
        constexpr float kAnchorTau = 1.5f;   // seconds
        float a = static_cast<float>(dt) / kAnchorTau;
        if (a > 1.0f) a = 1.0f;
        for (int i = 0; i < 3; ++i) g_anchor[i] += (raw[i] - g_anchor[i]) * a;
    }
    const float follow = g_animFollow.load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) g_eye[i] = g_anchor[i] + (raw[i] - g_anchor[i]) * follow;

    // --- facing ---------------------------------------------------------------------------------
    g_faceTarget = ResolveFaceTarget(conv, staged);
    float targetYaw = g_baseYaw;
    bool haveTarget = false;
    if (g_faceTarget >= 0x10000)
    {
        float tp[3] = {};
        int tsrc = ReadStagedEye(g_faceTarget, tp);
        if (tsrc == 0) { if (RdVec(g_faceTarget + kActorLocation, tp)) tsrc = 3; }
        if (tsrc != 0)
        {
            const float dx = tp[0] - g_eye[0];
            const float dy = tp[1] - g_eye[1];
            if ((dx * dx + dy * dy) > 1.0f)
            {
                targetYaw = std::atan2(dy, dx) * (180.0f / 3.14159265358979f);
                haveTarget = true;
            }
        }
    }
    if (!haveTarget && !g_baseYawInit)
    {
        // Nobody to face yet -- start from the staged actor's own facing.
        int yawUU = 0;
        if (RdInt(staged + kActorRotation + 4, &yawUU))
            targetYaw = static_cast<float>(yawUU) * (360.0f / 65536.0f);
        while (targetYaw > 180.0f) targetYaw -= 360.0f;
        while (targetYaw < -180.0f) targetYaw += 360.0f;
        haveTarget = true;
    }

    if (fresh || !g_baseYawInit)
    {
        g_baseYaw = targetYaw;   // snap on entry; never slew in from a stale heading
        g_baseYawInit = true;
    }
    else if (haveTarget)
    {
        const float rate = g_turnRate.load(std::memory_order_relaxed);
        g_baseYaw = StepAngle(g_baseYaw, targetYaw, rate * static_cast<float>(dt));
    }

    g_armed = true;

    // --- depth of field --------------------------------------------------------------------------
    // The conversation's DOF track pulls focus onto the current SPEAKER. From the director's shot that
    // reads as film grammar; from inside Shepard's head it means whoever you are looking at goes soft
    // the moment they stop talking.
    //
    // ABioStage.m_bDOFActive is BioWare's own per-conversation DOF switch, so this turns it off for
    // THIS scene only and leaves the global GamerSettings value alone. Re-asserted on a slow cadence
    // because the matinee track keeps setting it back.
    if (g_killDof.load(std::memory_order_relaxed))
    {
        std::uintptr_t stage = 0;
        if (!RdPtr(conv + kConvStage, &stage)) stage = 0;
        if (stage >= 0x10000)
        {
            char cn[64] = {};
            // Exact class match. Writing a bit into the wrong object at a guessed offset is how the
            // HUD lane produced "black square, no UI".
            if (!ClassNameOf(stage, cn, sizeof(cn)) || std::strcmp(cn, "BioStage") != 0) stage = 0;
        }

        if (stage != g_dofStage)
        {
            RestoreStageDof();          // hand the previous stage back before adopting a new one
            if (stage >= 0x10000)
            {
                bool wasActive = false;
                if (ME2VR::EngineProbe::ConvoFpReadStageDofActive(stage, &wasActive))
                {
                    g_dofStage = stage;
                    g_dofStageOrig = wasActive;   // captured BEFORE the mod ever writes
                    g_dofStageCaptured = true;
                    ME2VR::Log::Line("[CONVOFP] stage " + Hex(stage) + " m_bDOFActive was " +
                                     (wasActive ? "ON" : "off") + " -> clearing for this conversation");
                }
            }
        }

        // Re-assert EVERY frame, not every 15. The matinee track sets this bit back whenever it likes,
        // and on a 15-frame cadence that left a window of up to ~0.17s per re-enable where the blur was
        // live -- long enough to read as depth of field flickering in and out.
        if (g_dofStageCaptured)
        {
            bool nowActive = false;
            if (ME2VR::EngineProbe::ConvoFpReadStageDofActive(g_dofStage, &nowActive) && nowActive)
                ++g_dofRefights;   // the game turned it back on -> the stage bit IS the contested lever
            ME2VR::EngineProbe::ConvoFpSetStageDofActive(g_dofStage, false);
        }
        // DisableDOF() is a ProcessEvent call, so it stays on a slow cadence -- it is supplementary and
        // the stage bit is what actually holds.
        if (fresh) g_dofTick = 0;
        if ((g_dofTick % 15) == 0) ME2VR::EngineProbe::ConvoFpDisableDof();
        ++g_dofTick;
    }
    else if (g_dofStageCaptured)
    {
        // The user unticked the option mid-conversation. Without this the stage stayed forced off until
        // the scene ended, so the checkbox looked like it did nothing until the next conversation.
        RestoreStageDof();
    }

    // --- logging --------------------------------------------------------------------------------
    // Automatic, not hotkeyed: this is the data that decides the next round, and it costs nothing to
    // have it already in the log when feedback arrives.
    const bool convEdge = (conv != g_lastLoggedConv);
    const bool spkEdge  = (g_speaker != g_lastLoggedSpeaker);
    if (convEdge || spkEdge || (now - g_lastLogSec) > 2.0)
    {
        static const char* kSrc[5] = { "none", "eye-height", "head-bone", "actor-loc", "hold" };
        std::string line = "[CONVOFP] conv=" + Hex(conv) +
                           " staged=" + Hex(staged) +
                           " speaker=" + Hex(g_speaker) +
                           " face=" + Hex(g_faceTarget) +
                           " src=" + kSrc[(g_source >= 0 && g_source <= 4) ? g_source : 0] +
                           " speakers=" + std::to_string(g_speakerCount) +
                           " eye=(" + F1(g_eye[0]) + "," + F1(g_eye[1]) + "," + F1(g_eye[2]) + ")" +
                           " raw=(" + F1(raw[0]) + "," + F1(raw[1]) + "," + F1(raw[2]) + ")" +
                           " yaw=" + F1(g_baseYaw) + " tgt=" + F1(targetYaw);
        // Render-side delivery. Applies climbing with skips flat = the camera is being relocated every
        // view. Skips climbing while armed = the relocation is being gated off mid-conversation, which
        // is what flicker looks like from here.
        unsigned long long ap = 0, sk = 0;
        ME2VR::CalcViewHook::GetConvoFpCounts(&ap, &sk);
        line += " applied=" + std::to_string(ap - g_lastApplies) +
                " skipped=" + std::to_string(sk - g_lastSkips) + "/interval";
        g_lastApplies = ap; g_lastSkips = sk;
        // Why the animated head was or wasn't adopted -- otherwise "animation follow does nothing" has
        // no visible cause. dz/lat are its distance from the engine's own eye point.
        if (g_headRead) line += " headbone(dz=" + F1(g_headDz) + " lat=" + F1(g_headLat) + ")";
        else            line += " headbone=unreadable";
        line += " dofRefights=" + std::to_string(g_dofRefights);
        if (convEdge) line += "  [NEW CONVERSATION]";

        // [CONVOVIS] 2026-07-24: during the player's turn to speak the other character mostly stops
        // rendering -- but not entirely (their legs stay visible). A whole-actor cull cannot do that, so
        // something is suppressing SOME of their components and not others. The log already proves it
        // is not the mod's head-hide (staged never changes off Shepard). Dump the partner's per-component
        // visibility bits so the next round reads the cause instead of guessing it.
        // UPrimitiveComponent flags @0x168: HiddenGame = bit 0x04, bOwnerNoSee = bit 0x10.
        if (g_faceTarget >= 0x10000)
        {
            float fp3[3] = {};
            const bool haveLoc = RdVec(g_faceTarget + kActorLocation, fp3);
            float dist = -1.0f;
            if (haveLoc)
            {
                const float ddx = fp3[0] - g_eye[0], ddy = fp3[1] - g_eye[1], ddz = fp3[2] - g_eye[2];
                dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
            }
            const std::uintptr_t parts[6] = { kPawnBodyMesh, 0x85C, 0x86C, 0x874, 0x87C, 0x884 };
            static const char* kNames[6] = { "body", "head", "hair", "gear", "visor", "face" };
            std::string vis;
            for (int i = 0; i < 6; ++i)
            {
                std::uintptr_t comp = 0;
                if (!RdPtr(g_faceTarget + parts[i], &comp) || comp < 0x10000) { vis += std::string(" ") + kNames[i] + "=-"; continue; }
                int fl = 0;
                if (!RdInt(comp + 0x168, &fl)) { vis += std::string(" ") + kNames[i] + "=?"; continue; }
                vis += std::string(" ") + kNames[i] + "=" +
                       ((fl & 0x04) ? "HID" : ".") + ((fl & 0x10) ? "ONS" : ".");
            }
            ME2VR::Log::Line("[CONVOVIS] partner=" + Hex(g_faceTarget) +
                             (g_speaker == g_staged ? " (PLAYER TURN)" : " (they speak)") +
                             " dist=" + F1(dist) +
                             " loc=(" + F1(fp3[0]) + "," + F1(fp3[1]) + "," + F1(fp3[2]) + ")" + vis);
        }
        ME2VR::Log::Line(line);

        if (convEdge)
        {
            // One-shot on entry: is ME2's possessed pawn parked like ME1's, and is the staged actor a
            // different object? That single comparison is what ME1's lane never had.
            const std::uintptr_t lp = ME2VR::EngineProbe::GetPrimaryLocalPlayer();
            std::uintptr_t pc = 0, pawn = 0;
            if (lp >= 0x10000 && RdPtr(lp + kLpPlayerController, &pc) && pc >= 0x10000)
                RdPtr(pc + 0x294, &pawn);
            float pawnLoc[3] = {};
            const bool havePawnLoc = (pawn >= 0x10000) && RdVec(pawn + kActorLocation, pawnLoc);
            char scn[64] = {}; ClassNameOf(staged, scn, sizeof(scn));
            char fcn[64] = {}; if (g_faceTarget >= 0x10000) ClassNameOf(g_faceTarget, fcn, sizeof(fcn));
            ME2VR::Log::Line(std::string("[CONVOFP]   staged class=") + scn +
                             " face class=" + fcn +
                             " possessedPawn=" + Hex(pawn) +
                             (pawn == staged ? " (SAME as staged)" : " (DIFFERENT from staged)") +
                             (havePawnLoc ? "  pawnLoc=(" + F1(pawnLoc[0]) + "," + F1(pawnLoc[1]) + "," +
                                            F1(pawnLoc[2]) + ")" : "  pawnLoc=?"));
        }
        g_lastLogSec = now;
        g_lastLoggedConv = conv;
        g_lastLoggedSpeaker = g_speaker;
    }
}

bool IsArmed() noexcept { return g_armed; }

bool GetEyePose(EyePose* out) noexcept
{
    if (!g_armed || out == nullptr) return false;
    out->x = g_eye[0];
    out->y = g_eye[1];
    out->z = g_eye[2];
    out->baseYawDeg = g_baseYaw;
    return true;
}

void GetDiag(Diag* out) noexcept
{
    if (out == nullptr) return;
    out->conv         = g_conv;
    out->stagedPlayer = g_staged;
    out->speaker      = g_speaker;
    out->faceTarget   = g_faceTarget;
    out->source       = g_source;
    out->speakerCount = g_speakerCount;
    out->x = g_eye[0]; out->y = g_eye[1]; out->z = g_eye[2];
    out->baseYawDeg = g_baseYaw;
}

void  SetEnabled(bool on) noexcept { g_enabled.store(on, std::memory_order_release); }
bool  GetEnabled() noexcept        { return g_enabled.load(std::memory_order_acquire); }
void  SetTurnRate(float d) noexcept { g_turnRate.store(d < 0.0f ? 0.0f : d, std::memory_order_relaxed); }
float GetTurnRate() noexcept        { return g_turnRate.load(std::memory_order_relaxed); }
void  SetAnimFollow(float f) noexcept
{
    if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
    g_animFollow.store(f, std::memory_order_relaxed);
}
float GetAnimFollow() noexcept { return g_animFollow.load(std::memory_order_relaxed); }
void  SetEyeUpUU(float uu) noexcept { g_eyeUpUU.store(uu, std::memory_order_relaxed); }
float GetEyeUpUU() noexcept         { return g_eyeUpUU.load(std::memory_order_relaxed); }
void  SetEyeFwdUU(float uu) noexcept { g_eyeFwdUU.store(uu, std::memory_order_relaxed); }
float GetEyeFwdUU() noexcept         { return g_eyeFwdUU.load(std::memory_order_relaxed); }
void  SetZoom(float z) noexcept
{
    if (z < 1.0f) z = 1.0f; else if (z > 2.5f) z = 2.5f;
    g_zoom.store(z, std::memory_order_relaxed);
}
float GetZoom() noexcept             { return g_zoom.load(std::memory_order_relaxed); }
void  SetHideHead(bool on) noexcept  { g_hideHead.store(on, std::memory_order_relaxed); }
bool  GetHideHead() noexcept         { return g_hideHead.load(std::memory_order_relaxed); }
void  SetKillDof(bool on) noexcept   { g_killDof.store(on, std::memory_order_relaxed); }
bool  GetKillDof() noexcept          { return g_killDof.load(std::memory_order_relaxed); }
}
