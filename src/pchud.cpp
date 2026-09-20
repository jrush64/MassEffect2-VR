// ME2 per-element HUD control - DISCOVERY STAGE.
//
// ME1 moves/scales individual HUD pieces by calling the game's own
// BioSFPanel.SetVariableFloat on Flash paths like "_root.radarMC._x". The mechanism is engine-level
// and carries straight over to LE2. What does NOT carry over is the ELEMENT NAMES: ME1's list of 13
// came from an offline parse of ME1's HUD movie, and ME2 ships a different HUD.
//
// So this file starts as a pure-READ probe. It resolves the two UFunctions, finds the live HUD panel
// objects, and asks the movie which element names actually exist (GetVariableFloat on <name>._x
// succeeds only for a real display object). The answer goes in the log, and the real controls get
// built from that list - the same instrument-first route that cracked the vehicle cannon in one build
// after three builds of guessing.
//
// NOTHING here writes to the game. Bound to F11 (NOT F9 - that is the game quickload).

#include "pchud.h"

#include "logger.h"

#include "MinHook.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <atomic>
#include <string>

namespace ME2VR::PcHud
{
namespace
{
// These MUST match engine_probe.cpp exactly. The first version of this probe guessed 0x38 for the
// name offset and invented a flat name array; LE2 uses 0x48 and a CHUNKED name pool, so every name
// read failed and the probe reported "class missing" when the class was there all along.
constexpr std::uintptr_t kGObjects      = 0x173CC48;   // TArray<UObject*>
constexpr std::uintptr_t kNamePoolsRva  = 0x1668A10;   // chunked FName pool table
constexpr std::uintptr_t kObjNameOff    = 0x48;
constexpr std::uintptr_t kObjOuterOff   = 0x40;
constexpr std::uintptr_t kObjClassOff   = 0x50;
constexpr std::uintptr_t kProcessEvent  = 0xD8D50;

using tProcessEvent = void(__fastcall*)(void*, void*, void*, void*);

struct FStringParam { const wchar_t* data; int32_t count; int32_t capacity; };
struct GetVariableFloatParams { FStringParam variable; float result; };

// [HUDSAFE] Writes MUST happen on the GAME thread, from inside the HUD handler's own ProcessEvent,
// with the panel read live at that instant. Driving it from the present thread with a pointer cached
// across frames is the freed-panel crash ME1 documented - and is exactly what crashed during
// a drag of the weapon group. thread_local because the mod's own SetVariableFloat calls re-enter the hook.
thread_local bool g_applying = false;
std::uintptr_t g_hudClassPtr = 0;      // UClass* for SFXSFHandler_PCHUD; class objects are never freed
// TWO power-wheel classes exist at runtime: SFXSFHandler_PowerWheel (base - its lone instance has NO
// panel, writes to it vanish) and SFXSFHandler_PCPowerWheel (the subclass whose movie is actually on
// screen and owns the objective-circle radar). The old substring match latched onto whichever the
// GObjects scan met first, which is why the radar control worked or died by boot order.
std::uintptr_t g_pwClassPtr = 0;       // UClass* for SFXSFHandler_PCPowerWheel (the live one)
std::uintptr_t g_pwClassBase = 0;      // UClass* for SFXSFHandler_PowerWheel (accepted, panel-checked)
std::uintptr_t g_pwPanel = 0;

bool IsPwClass(std::uintptr_t cls) noexcept
{
    return cls != 0 && (cls == g_pwClassPtr || cls == g_pwClassBase);
}
void* g_peTarget = nullptr;
void* g_getVarFn = nullptr;
void* g_setVarFn = nullptr;
bool  g_resolved = false;

// ---- SFName decode (mirrors engine_probe's ReadSfxName) ----
bool ReadName(unsigned long long packed, char* out, size_t cap) noexcept
{
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const unsigned int p32 = static_cast<unsigned int>(packed & 0xFFFFFFFFull);
        const unsigned int offset = p32 & 0x1FFFFFFFu;
        const unsigned int chunk = (p32 >> 29) & 0x7u;
        auto* pools = reinterpret_cast<unsigned char* volatile*>(base + kNamePoolsRva);
        unsigned char* pool = pools[chunk];
        if (pool == nullptr) return false;
        const char* ansi = reinterpret_cast<const char*>(pool + offset + 12);
        size_t i = 0;
        for (; i + 1 < cap && ansi[i] >= 32 && ansi[i] < 127; ++i) out[i] = ansi[i];
        out[i] = '\0';
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ObjName(std::uintptr_t obj, char* out, size_t cap) noexcept
{
    __try
    {
        if (obj < 0x10000) return false;
        return ReadName(*reinterpret_cast<unsigned long long volatile*>(obj + kObjNameOff), out, cap);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ClassNameOf(std::uintptr_t obj, char* out, size_t cap) noexcept
{
    __try
    {
        if (obj < 0x10000) return false;
        const std::uintptr_t cls = *reinterpret_cast<std::uintptr_t volatile*>(obj + kObjClassOff);
        if (cls < 0x10000) return false;
        return ReadName(*reinterpret_cast<unsigned long long volatile*>(cls + kObjNameOff), out, cap);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Find a UFunction by its own name + its outer (class) name, exactly like engine_probe does for
// SetOwnerNoSee. ME1 uses BioSFPanel for both; if LE2 renamed the class this is where it shows up.
void* FindFn(const char* fnName, const char* outerName) noexcept
{
    void* found = nullptr;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + kGObjects);
        const int num = *reinterpret_cast<int volatile*>(base + kGObjects + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return nullptr;
        char nm[64] = {}, onm[64] = {};
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            if (!ObjName(obj, nm, sizeof(nm)) || std::strcmp(nm, fnName) != 0) continue;
            const std::uintptr_t outer = *reinterpret_cast<std::uintptr_t volatile*>(obj + kObjOuterOff);
            if (outer < 0x10000) continue;
            if (ObjName(outer, onm, sizeof(onm)) && std::strcmp(onm, outerName) == 0) { found = reinterpret_cast<void*>(obj); break; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// Resolve the HUD handler's CLASS object once. Class objects live for the process, so caching this
// pointer is safe - and it turns the per-event test into one deref plus a compare.
void ResolveHudClass() noexcept
{
    if (g_hudClassPtr != 0 && g_pwClassPtr != 0 && g_pwClassBase != 0) return;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + kGObjects);
        const int num = *reinterpret_cast<int volatile*>(base + kGObjects + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return;
        char nm[64] = {}, cls[64] = {};
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            if (!ObjName(obj, nm, sizeof(nm))) continue;
            const bool isHud    = (std::strcmp(nm, "SFXSFHandler_PCHUD") == 0);
            const bool isPwPc   = (std::strcmp(nm, "SFXSFHandler_PCPowerWheel") == 0);
            const bool isPwBase = (std::strcmp(nm, "SFXSFHandler_PowerWheel") == 0);
            if (!isHud && !isPwPc && !isPwBase) continue;
            if (!ClassNameOf(obj, cls, sizeof(cls)) || std::strcmp(cls, "Class") != 0) continue;
            if (isHud)      g_hudClassPtr = obj;   // these ARE the UClasses
            if (isPwPc)     g_pwClassPtr  = obj;
            if (isPwBase)   g_pwClassBase = obj;
            if (g_hudClassPtr != 0 && g_pwClassPtr != 0 && g_pwClassBase != 0) break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Resolve() noexcept
{
    if (g_resolved) return;
    g_resolved = true;
    g_getVarFn = FindFn("GetVariableFloat", "BioSFPanel");
    g_setVarFn = FindFn("SetVariableFloat", "BioSFPanel");
    ME2VR::Log::Line(std::string("[HUDDISC] BioSFPanel.GetVariableFloat=") +
                     (g_getVarFn ? "FOUND" : "missing") + "  SetVariableFloat=" + (g_setVarFn ? "FOUND" : "missing"));
}

bool Resolved() noexcept { Resolve(); return g_getVarFn != nullptr && g_setVarFn != nullptr; }

// Read one Flash variable. Returns false if the path doesn't exist in the movie - which is exactly
// the signal the mod is mining: a name that answers is a real element.
bool ReadVar(void* panel, const wchar_t* path, float* out) noexcept
{
    if (panel == nullptr || g_getVarFn == nullptr) return false;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + kProcessEvent);
        GetVariableFloatParams p = {};
        p.variable.data = path;
        p.variable.count = static_cast<int32_t>(wcslen(path)) + 1;
        p.variable.capacity = p.variable.count;
        p.result = -123456.0f;                    // sentinel: unchanged => the path didn't resolve
        pe(panel, g_getVarFn, &p, nullptr);
        if (p.result == -123456.0f) return false;
        *out = p.result;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// THE ANSWER, and why ME2 is easier than ME1 here: LE2's HUD handler stores every Flash path as a
// plain FString FIELD on the handler object (USFXSFHandler_HUD in the LE2 SDK). ME1 had no such
// table, which is why its 13 names had to come from an offline parse of the movie. Here the mod just reads
// them out of the live object - no guessing, and they're automatically correct for whatever build
// the game is running. Offsets from LESDK LE2 SFXGame_classes.hpp (class USFXSFHandler_HUD).
constexpr std::uintptr_t kHandlerPanel = 0x0080;   // UBioSFHandler.oPanel (Engine_classes.hpp)
// The bottom-middle block (your health/shields plus both squadmates) is NOT in the flat path list -
// it lives in three FSFXHUDSquadMemberInfo structs on the handler, whose sPath (+0x00) is the
// container for that member's whole display. Moving that container moves the icon and every bar with it.
constexpr std::uintptr_t kSquadInfo[3] = { 0x009C, 0x0150, 0x0204 };   // Shepard, Hench1, Hench2
constexpr int kSquadCount = 3;
char g_squadPath[kSquadCount][160] = {};

// SECOND handler: USFXSFHandler_PowerWheel. Same trick, different object - it owns the radar and the
// power-wheel overlay, none of which the HUD handler knows about. Offsets from LESDK LE2.
constexpr std::uintptr_t kPwRadar   = 0x0344;   // m_sRadarPath
constexpr std::uintptr_t kPwFields[] = {
    0x0264, 0x0274, 0x0284, 0x0294, 0x02A4, 0x02B4,          // title/name/info/infoBG/useButton/useText
    0x02C4, 0x02D4, 0x02E4, 0x02F4, 0x0394,                  // map buttons + texts, wheel arrow
    0x0314, 0x0324, 0x0334, 0x0354, 0x0364, 0x0374, 0x0384,  // blockers + team status/power texts
    0x0344,                                                   // radar (last so it can be its own group)
};
constexpr int kPwCount = static_cast<int>(sizeof(kPwFields) / sizeof(kPwFields[0]));
constexpr int kPwRadarIdx = kPwCount - 1;
char g_pwPath[kPwCount][160] = {};

struct PathField { std::uintptr_t off; const char* label; };
const PathField kPathFields[] = {
    { 0x0308, "WeaponIcon" },      { 0x0318, "WeaponAmmo" },      { 0x0328, "WeaponClipAmmo" },
    { 0x0338, "TargetName" },      { 0x0348, "TargetStatus" },    { 0x0358, "ButtonA" },
    { 0x0368, "TargetBackground" },{ 0x0378, "HealthBar" },       { 0x0388, "ArmourBar" },
    { 0x0398, "BioticBar" },       { 0x03A8, "ShieldBar" },       { 0x03B8, "ResistBarOutline" },
    { 0x03C8, "Notification" },    { 0x03D8, "CenterStatus" },    { 0x03E8, "OverheatIndicator" },
    { 0x03F8, "OverheatText" },    { 0x0408, "OverheatTextField" },
    { 0x0418, "PlayerPower1" },    { 0x0428, "PlayerPower2" },
};
constexpr int kPathFieldCount = static_cast<int>(sizeof(kPathFields) / sizeof(kPathFields[0]));

// Read an FString { wchar_t* data; int32 count; int32 max } into ASCII for logging.
bool ReadFString(std::uintptr_t addr, char* out, size_t cap) noexcept
{
    __try
    {
        const wchar_t* data = *reinterpret_cast<wchar_t* volatile*>(addr);
        const int count = *reinterpret_cast<int volatile*>(addr + 8);
        if (data == nullptr || count <= 0 || count > 512) return false;
        size_t i = 0;
        for (; i + 1 < cap && i < static_cast<size_t>(count) && data[i] != L'\0'; ++i)
            out[i] = (data[i] > 0 && data[i] < 127) ? static_cast<char>(data[i]) : '?';
        out[i] = '\0';
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// POD-only so it can hold the SEH frame: C2712 says __try can't live in a function that needs object
// unwinding, and any std::string temporary (i.e. any Log::Line call) triggers that. So the scan
// collects into plain arrays here and all logging happens in the caller.
struct FoundHandler
{
    std::uintptr_t obj;
    std::uintptr_t panel;
    char cls[64];
    char paths[kPathFieldCount][160];
};

// Find the live HUD handler(s) and pull their panel + Flash-path table in one SEH-guarded pass.
int CollectHandlers(FoundHandler* out, int cap) noexcept
{
    int n = 0;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + kGObjects);
        const int num = *reinterpret_cast<int volatile*>(base + kGObjects + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return 0;
        for (int i = 0; i < num && n < cap; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            char cls[64] = {}, nm[64] = {};
            if (!ClassNameOf(obj, cls, sizeof(cls))) continue;
            // EVERY live Scaleform handler, not just the HUD one. Filtering to the HUD handler is why
            // the bottom-right element kept coming up empty - it may simply belong to another panel.
            if (std::strstr(cls, "SFHandler") == nullptr) continue;
            if (!ObjName(obj, nm, sizeof(nm))) continue;
            if (std::strncmp(nm, "Default__", 9) == 0) continue;   // class template, not a live handler

            out[n].obj = obj;
            out[n].panel = *reinterpret_cast<std::uintptr_t volatile*>(obj + kHandlerPanel);
            std::memcpy(out[n].cls, cls, sizeof(cls));
            for (int f = 0; f < kPathFieldCount; ++f)
                if (!ReadFString(obj + kPathFields[f].off, out[n].paths[f], sizeof(out[n].paths[f])))
                    out[n].paths[f][0] = 0;
            ++n;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

// ===================== APPLY: per-element move/scale =====================
// The paths above are grouped into the clusters a player actually thinks about. Each group gets an
// offset and a scale, applied as a DELTA on top of the element's own baseline, so the game keeps
// owning the real layout and the mod only nudges it. Neutral groups are skipped entirely, so with
// everything at defaults this costs nothing.
struct HudGroup { const char* label; const char* paths[10]; };
const HudGroup kGroups[] = {
    { "Weapon + ammo (bottom left)", { "Weapon", "SpareAmmo", "MagazineAmmo", "WeaponOverheat", nullptr } },
    { "Target info (top)",           { "TargetBackground", "TargetName", "TargetStatus", "ButtonA",
                                       "targetHealth", "targetArmour", "targetBarrier", "targetShield",
                                       "targetBarOutline", nullptr } },
    { "Powers",                      { "PlayerPower1", "PlayerPower2", nullptr } },
    // Sits at x=1114 y=582 on the 1280-wide stage - i.e. the bottom-right block. Labelled by what the
    // game calls it ("Notification"), which made it unrecognisable as the thing on screen.
    { "Notifications (bottom right)", { "_root.mcNotification", nullptr } },
    { "Centre status text",          { "CenterStatus", nullptr } },
    { "Health + squad (bottom middle)", { nullptr } },   // paths read live into g_squadPath
    // The circular objective/radar display shown during normal play. It lives in the POWER WHEEL's
    // movie (m_sRadarPath) - the engine's "objective radar" (m_bObjectiveRadarOn) is this same radar
    // in objective mode - so it has to be written into the PCPowerWheel panel, not the HUD one.
    { "Objective circle (radar)",      { nullptr } },   // g_pwPath[kPwRadarIdx]
    // The designer-driven mission block in the bottom right ("Object Rho / Power Level" and friends).
    // It is NOT in the HUD movie: it belongs to a DesignerUI panel, which exposes no path fields at
    // all, so the only handle is the movie's own _root - moved as a whole.
    { "Mission / objective panel (bottom right)", { nullptr } },   // whole DesignerUI movie via _root
};
// The Reticle / Conversation / MessageBox whole-panel groups are gone: writes reached those movies
// and nothing visibly moved. The DesignerUI one is BACK because that earlier verdict was measured
// with the same bug that hid the radar - the mod wrote to the first instance of the class, not the
// instance that is actually on screen. Panel visibility is now checked before writing.

constexpr int kGroupSquad     = 5;
constexpr int kGroupPwRadar   = 6;
constexpr int kGroupObjPanel  = 7;
bool GroupIsPowerWheel(int g) noexcept { return g == kGroupPwRadar; }
bool GroupIsPanelWide(int g)  noexcept { return g == kGroupObjPanel; }

// Resolve the path list for a group into g_scratch; returns how many entries are valid.
int GroupPaths(int g, const char** out, int cap) noexcept
{
    int n = 0;
    if (g == kGroupSquad)
    {
        for (int i = 0; i < kSquadCount && n < cap; ++i) if (g_squadPath[i][0]) out[n++] = g_squadPath[i];
    }
    else if (g == kGroupPwRadar)
    {
        if (g_pwPath[kPwRadarIdx][0] && n < cap) out[n++] = g_pwPath[kPwRadarIdx];
    }
    else
    {
        for (int i = 0; kGroups[g].paths[i] != nullptr && n < cap; ++i) out[n++] = kGroups[g].paths[i];
    }
    return n;
}
constexpr int kGroupCount = static_cast<int>(sizeof(kGroups) / sizeof(kGroups[0]));

GroupCfg g_groups[kGroupCount] = {};
bool g_groupsInit = false;
std::atomic_bool g_hudEnabled{true};
volatile bool g_anyActive = false;   // set from the present thread, read by the hook

// Baselines, captured live the first time the mod touches a path on a given panel. Re-captured whenever the
// panel changes (level load / HUD rebuild), which is also what stops the mod writing through a stale one.
struct Baseline { char path[64]; float x, y, sx, sy; bool valid; };
Baseline g_base[48] = {};
int g_baseCount = 0;
std::uintptr_t g_basePanel = 0;

void EnsureGroupsInit() noexcept
{
    if (g_groupsInit) return;
    for (int i = 0; i < kGroupCount; ++i) { g_groups[i].offX = 0.0f; g_groups[i].offY = 0.0f; g_groups[i].scaleX = 1.0f; g_groups[i].scaleY = 1.0f; }
    g_groupsInit = true;
}

bool WriteVar(void* panel, const wchar_t* path, float value) noexcept
{
    if (panel == nullptr || g_setVarFn == nullptr) return false;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + kProcessEvent);
        struct { FStringParam variable; float value; } p = {};
        p.variable.data = path;
        p.variable.count = static_cast<int32_t>(wcslen(path)) + 1;
        p.variable.capacity = p.variable.count;
        p.value = value;
        pe(panel, g_setVarFn, &p, nullptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Baseline* FindOrCaptureBaseline(void* panel, const char* path) noexcept
{
    for (int i = 0; i < g_baseCount; ++i)
        if (std::strcmp(g_base[i].path, path) == 0) return g_base[i].valid ? &g_base[i] : nullptr;
    if (g_baseCount >= 48) return nullptr;
    Baseline& b = g_base[g_baseCount++];
    strcpy_s(b.path, path);
    wchar_t wp[160] = {};
    float x = 0.0f, y = 0.0f, sx = 0.0f, sy = 0.0f;
    swprintf_s(wp, L"%hs._x", path);      const bool okx = ReadVar(panel, wp, &x);
    swprintf_s(wp, L"%hs._y", path);      ReadVar(panel, wp, &y);
    swprintf_s(wp, L"%hs._xscale", path); ReadVar(panel, wp, &sx);
    swprintf_s(wp, L"%hs._yscale", path); ReadVar(panel, wp, &sy);
    // xscale 0 means the path didn't resolve (GetVariableFloat returns 0 for a missing path) - the
    // same false-positive that made the first probe report 348 elements. Treat it as unusable.
    b.valid = okx && sx != 0.0f;
    b.x = x; b.y = y; b.sx = sx; b.sy = sy;
    return b.valid ? &b : nullptr;
}

// Locate the live PCHUD handler + panel. Cheap enough to redo periodically, and revalidating by class
// name every time is what keeps the mod off a freed pointer after a load.
bool FindLiveHud(std::uintptr_t* handlerOut, std::uintptr_t* panelOut) noexcept
{
    static FoundHandler s_h[4] = {};
    const int n = CollectHandlers(s_h, 4);
    for (int i = 0; i < n; ++i)
    {
        if (s_h[i].panel < 0x10000) continue;
        *handlerOut = s_h[i].obj; *panelOut = s_h[i].panel;
        return true;
    }
    return false;
}


// ===================== objective circle: per-panel baseline =====================
// Own state, separate from the HUD handler's baselines, because the wheel panel this writes to is
// picked by validating which movie actually holds the radar rather than by class.
//
// The invariant that matters: a baseline is captured ONCE per panel, before this code has written
// anything to that panel, and is never re-derived from a value the mod might have authored itself.
// Everything written is an absolute value computed from that fixed baseline.
struct ObjFollow
{
    void* panel;
    float bx, by, bsx, bsy;    // the game's own transform, captured ONCE before the mod ever wrote here
    bool  valid;
    bool  dirty;               // the mod has written to this panel since the last restore
};
ObjFollow g_objFollow[8] = {};    // radar targets
ObjFollow g_panFollow[8] = {};    // whole-movie targets (the designer objective panel)

unsigned g_objApplyCount = 0;

ObjFollow* ObjSlot(ObjFollow* arr, int cap, void* panel) noexcept
{
    for (int i = 0; i < cap; ++i) if (arr[i].panel == panel) return &arr[i];
    for (int i = 0; i < cap; ++i) if (arr[i].panel == nullptr) { arr[i] = {}; arr[i].panel = panel; return &arr[i]; }
    arr[0] = {};
    arr[0].panel = panel;
    return &arr[0];
}

// panel/path come from a validated target list: the caller has already confirmed this movie holds
// the element AND that it is on screen. Writing a path into a movie that isn't the right one is how
// the HUD got wrecked, so this is never called speculatively.
//
// Used for both the radar clip and the whole designer movie (path "_root") - same rules apply.
void ApplyClipXform(ObjFollow* s, void* panel, const char* path, const GroupCfg& c, const char* tag) noexcept
{
    if (path == nullptr || path[0] == 0 || s == nullptr) return;
    const bool neutral = (c.offX == 0.0f && c.offY == 0.0f && c.scaleX == 1.0f && c.scaleY == 1.0f);
    wchar_t wp[160] = {};

    // Capture the game's own transform ONCE per panel, and only while the mod has provably never written
    // to this panel (a slot is created empty and cleared whenever the chosen movie changes).
    //
    // This replaces a read-current-and-guess-who-wrote-it scheme, which is what made the
    // circle creep and then disappear: any transient read failure dropped the "that value is the mod's"
    // memory, so the next pass adopted THE MOD'S OWN last output as the game's baseline and applied the
    // offset and scale on top of it again - +off, +off, x1.75, x1.75 - until it walked off the stage.
    // A baseline that is never re-derived from a value the mod might have authored cannot compound.
    if (!s->valid)
    {
        float x = 0.0f, y = 0.0f, sx = 0.0f, sy = 0.0f;
        swprintf_s(wp, L"%hs._xscale", path);
        if (!ReadVar(panel, wp, &sx) || sx == 0.0f) return;   // not in this movie yet - retry, capture nothing
        swprintf_s(wp, L"%hs._x", path);      ReadVar(panel, wp, &x);
        swprintf_s(wp, L"%hs._y", path);      ReadVar(panel, wp, &y);
        swprintf_s(wp, L"%hs._yscale", path); ReadVar(panel, wp, &sy);
        s->bx = x; s->by = y; s->bsx = sx; s->bsy = sy;
        s->valid = true;
        s->dirty = false;
        char b[288] = {};
        sprintf_s(b, "[%s] baseline captured panel=%p path='%s' x=%.1f y=%.1f xs=%.1f ys=%.1f",
                  tag, panel, path, x, y, sx, sy);
        ME2VR::Log::Line(b);
    }

    if (neutral)
    {
        // Hand the element back to the game once, then stay out of the way entirely.
        if (s->dirty)
        {
            swprintf_s(wp, L"%hs._x", path);      WriteVar(panel, wp, s->bx);
            swprintf_s(wp, L"%hs._y", path);      WriteVar(panel, wp, s->by);
            swprintf_s(wp, L"%hs._xscale", path); WriteVar(panel, wp, s->bsx);
            swprintf_s(wp, L"%hs._yscale", path); WriteVar(panel, wp, s->bsy);
            s->dirty = false;
        }
        return;
    }

    // Absolute target, always derived from the immutable baseline - never from what is on screen now.
    // Clamped so a write can never put the circle somewhere it cannot be seen.
    auto clamp = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    const float nx  = clamp(s->bx + c.offX, -400.0f, 1680.0f);
    const float ny  = clamp(s->by + c.offY, -300.0f, 1020.0f);
    const float nsx = clamp(s->bsx * c.scaleX, 5.0f, 600.0f);
    const float nsy = clamp(s->bsy * c.scaleY, 5.0f, 600.0f);
    swprintf_s(wp, L"%hs._x", path);      WriteVar(panel, wp, nx);
    swprintf_s(wp, L"%hs._y", path);      WriteVar(panel, wp, ny);
    swprintf_s(wp, L"%hs._xscale", path); WriteVar(panel, wp, nsx);
    swprintf_s(wp, L"%hs._yscale", path); WriteVar(panel, wp, nsy);
    s->dirty = true;

    if ((g_objApplyCount++ % 240) == 0)
    {
        float cx = 0.0f, cy = 0.0f, csx = 0.0f, alpha = -1.0f, visible = -1.0f;
        swprintf_s(wp, L"%hs._x", path);       ReadVar(panel, wp, &cx);
        swprintf_s(wp, L"%hs._y", path);       ReadVar(panel, wp, &cy);
        swprintf_s(wp, L"%hs._xscale", path);  ReadVar(panel, wp, &csx);
        swprintf_s(wp, L"%hs._alpha", path);   ReadVar(panel, wp, &alpha);
        swprintf_s(wp, L"%hs._visible", path); ReadVar(panel, wp, &visible);
        // base is fixed, wrote is what the mod asked for, cur is what the movie holds after the write:
        // cur drifting away from wrote over time means something else is moving the element.
        char b[352] = {};
        sprintf_s(b, "[%s] panel=%p cur=(%.1f,%.1f xs=%.1f) base=(%.1f,%.1f) wrote=(%.1f,%.1f) alpha=%.0f visible=%.0f",
                  tag, panel, cx, cy, csx, s->bx, s->by, nx, ny, alpha, visible);
        ME2VR::Log::Line(b);
    }
}

// Apply every non-neutral group to a panel that is live RIGHT NOW - the mod is inside the handler's own
// ProcessEvent, on the game thread, so the pointer cannot be freed underneath it.
void ApplyToLivePanel(void* panel) noexcept
{
    const char* paths[24] = {};
    for (int g = 0; g < kGroupCount; ++g)
    {
        // The radar and the designer objective panel live in other movies and are driven separately.
        if (GroupIsPowerWheel(g) || GroupIsPanelWide(g)) continue;
        const GroupCfg& c = g_groups[g];
        if (c.offX == 0.0f && c.offY == 0.0f && c.scaleX == 1.0f && c.scaleY == 1.0f) continue;
        const int np = GroupPaths(g, paths, 24);
        if (np == 0) continue;

        // Scaling each element about its OWN origin left them where they were and just made them
        // bigger, so a group's pieces grew into each other. Scale their POSITIONS about the group's
        // centre too, and the group expands as a unit the way you'd expect.
        float cx = 0.0f, cy = 0.0f;
        int n = 0;
        for (int i = 0; i < np; ++i)
        {
            Baseline* b = FindOrCaptureBaseline(panel, paths[i]);
            if (b == nullptr) continue;
            cx += b->x; cy += b->y; ++n;
        }
        if (n == 0) continue;
        cx /= static_cast<float>(n); cy /= static_cast<float>(n);

        for (int i = 0; i < np; ++i)
        {
            Baseline* b = FindOrCaptureBaseline(panel, paths[i]);
            if (b == nullptr) continue;
            const float nx = cx + (b->x - cx) * c.scaleX + c.offX;
            const float ny = cy + (b->y - cy) * c.scaleY + c.offY;
            wchar_t wp[160] = {};
            swprintf_s(wp, L"%hs._x", paths[i]);      WriteVar(panel, wp, nx);
            swprintf_s(wp, L"%hs._y", paths[i]);      WriteVar(panel, wp, ny);
            swprintf_s(wp, L"%hs._xscale", paths[i]); WriteVar(panel, wp, b->sx * c.scaleX);
            swprintf_s(wp, L"%hs._yscale", paths[i]); WriteVar(panel, wp, b->sy * c.scaleY);
        }
    }
}

// The PowerWheel handler is IDLE during normal play - its movie is LOADED and on screen (that is why
// the objective circle is visible without the wheel up), but the handler only receives ProcessEvent
// while the wheel is actually open. So it is driven from the HUD handler's event instead, which fires
// every frame. The panel is re-read from the handler at apply time and the handler's class
// re-validated first, so the mod never writes through a pointer it cached and stopped checking.
std::uintptr_t g_pwObj = 0;
bool g_pwPathsDirty = false;
bool g_pwLogged = false;
bool g_pwApplied = false;   // reached the live power-wheel panel at least once (for the log)

// Collect every live power-wheel handler instance with a panel. Several exist at once, so the caller
// picks between them by asking which panel's movie actually contains the radar - a class name alone
// cannot tell them apart. POD out-params: the SEH frame forbids anything that needs unwinding.
int CollectWheels(std::uintptr_t* objs, std::uintptr_t* panels, int cap) noexcept
{
    int n = 0;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + kGObjects);
        const int num = *reinterpret_cast<int volatile*>(base + kGObjects + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return 0;
        char nm[64] = {};
        for (int i = 0; i < num && n < cap; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            if (!IsPwClass(*reinterpret_cast<std::uintptr_t volatile*>(obj + kObjClassOff))) continue;
            const std::uintptr_t pnl = *reinterpret_cast<std::uintptr_t volatile*>(obj + kHandlerPanel);
            if (pnl < 0x10000) continue;
            if (!ObjName(obj, nm, sizeof(nm)) || std::strncmp(nm, "Default__", 9) == 0) continue;
            objs[n] = obj; panels[n] = pnl; ++n;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

// One live wheel movie that holds a VISIBLE radar. There are many wheel instances at once and the
// game recreates them constantly, so this is a list refreshed often, not a single cached pick.
struct WheelTarget { std::uintptr_t obj, panel; char path[160]; };
WheelTarget g_pwTargets[8] = {};
int g_pwTargetN = 0;
bool g_chainLogged = false;

// Walk a Flash path's ancestors and log each one's geometry. Which clip in the chain is actually the
// circle on screen is not something the mod can reason about from the outside - this measures it.
void LogClipChain(void* panel, const char* path) noexcept
{
    char prefix[160] = {};
    const int len = static_cast<int>(std::strlen(path));
    for (int cut = 0; cut <= len; ++cut)
    {
        if (cut != len && path[cut] != '.') continue;
        std::memcpy(prefix, path, static_cast<size_t>(cut));
        prefix[cut] = '\0';
        wchar_t wp[176] = {};
        float x = 0.0f, y = 0.0f, sx = 0.0f, sy = 0.0f, a = -1.0f, v = -1.0f;
        swprintf_s(wp, L"%hs._x", prefix);       ReadVar(panel, wp, &x);
        swprintf_s(wp, L"%hs._y", prefix);       ReadVar(panel, wp, &y);
        swprintf_s(wp, L"%hs._xscale", prefix);  ReadVar(panel, wp, &sx);
        swprintf_s(wp, L"%hs._yscale", prefix);  ReadVar(panel, wp, &sy);
        swprintf_s(wp, L"%hs._alpha", prefix);   ReadVar(panel, wp, &a);
        swprintf_s(wp, L"%hs._visible", prefix); ReadVar(panel, wp, &v);
        char b[320] = {};
        sprintf_s(b, "[OBJCHAIN] '%s'  x=%.1f y=%.1f xs=%.1f ys=%.1f alpha=%.0f visible=%.0f",
                  prefix, x, y, sx, sy, a, v);
        ME2VR::Log::Line(b);
    }
    float rx = 0.0f, ry = 0.0f, rsx = 0.0f;
    ReadVar(panel, L"_root._x", &rx); ReadVar(panel, L"_root._y", &ry); ReadVar(panel, L"_root._xscale", &rsx);
    char b[192] = {};
    sprintf_s(b, "[OBJCHAIN] '_root'  x=%.1f y=%.1f xs=%.1f", rx, ry, rsx);
    ME2VR::Log::Line(b);
}

// Collect EVERY wheel movie whose radar is present and actually VISIBLE.
//
// The measured failure: writes landed perfectly (cur == wrote in the telemetry) on a clip whose
// _alpha was 0 - an off-screen copy - while the circle the player sees lives in a different
// instance. Existence (_xscale != 0) is not enough to tell them apart; visibility is. And because
// the game churns these panels every few seconds, pinning one pick goes stale almost immediately,
// which is why it would move once and then stop. So: apply to all of them, refreshed often.
void FindPowerWheel() noexcept
{
    std::uintptr_t objs[16] = {}, panels[16] = {};
    const int n = CollectWheels(objs, panels, 16);
    int found = 0;
    std::uintptr_t bestPanel = 0, bestObj = 0;
    float bestAlpha = -1.0f;
    for (int i = 0; i < n && found < 8; ++i)
    {
        char rp[160] = {};
        if (!ReadFString(objs[i] + kPwRadar, rp, sizeof(rp)) || rp[0] == 0) continue;
        void* pnl = reinterpret_cast<void*>(panels[i]);
        wchar_t wp[176] = {};
        float sx = 0.0f, alpha = 0.0f, vis = 0.0f;
        swprintf_s(wp, L"%hs._xscale", rp);
        if (!ReadVar(pnl, wp, &sx) || sx == 0.0f) continue;         // path not in this movie
        swprintf_s(wp, L"%hs._alpha", rp);   ReadVar(pnl, wp, &alpha);
        swprintf_s(wp, L"%hs._visible", rp); ReadVar(pnl, wp, &vis);
        if (alpha <= 0.0f || vis == 0.0f) continue;                  // present but not on screen

        g_pwTargets[found].obj = objs[i];
        g_pwTargets[found].panel = panels[i];
        strcpy_s(g_pwTargets[found].path, rp);
        ++found;
        if (alpha > bestAlpha) { bestAlpha = alpha; bestPanel = panels[i]; bestObj = objs[i]; }
    }
    g_pwTargetN = found;
    if (found == 0) { g_pwObj = 0; g_pwPanel = 0; return; }

    if (bestPanel != g_pwPanel)
    {
        g_pwPanel = bestPanel;
        for (int q = 0; q < kPwCount; ++q)
            if (!ReadFString(bestObj + kPwFields[q], g_pwPath[q], sizeof(g_pwPath[q])))
                g_pwPath[q][0] = 0;
        g_pwPathsDirty = true;
    }
    g_pwObj = bestObj;
    if (!g_chainLogged && g_pwPath[kPwRadarIdx][0])
    {
        g_chainLogged = true;
        LogClipChain(reinterpret_cast<void*>(bestPanel), g_pwPath[kPwRadarIdx]);
    }
}

// ===================== mission / objective panel (bottom right) =====================
// This block is designer-driven and lives in its own movie, whose handler exposes NO path fields, so
// the only handle is the movie's _root. It was dropped once as "writes reached it, nothing moved" -
// but that was measured while writing to the first instance of the class rather than the one on
// screen, the same mistake that hid the radar. UBioSFPanel carries its own IsVisible bit and a name
// tag, so the on-screen instance can be identified directly instead of inferred.
constexpr std::uintptr_t kPanelFlags = 0x0170;   // UBioSFPanel bitfield; bit0 = IsVisible
constexpr std::uintptr_t kPanelTag   = 0x013C;   // UBioSFPanel.nmTag (SFXName)

struct PanelTarget { std::uintptr_t obj, panel; };
PanelTarget g_objPanels[8] = {};
int  g_objPanelN = 0;
bool g_panelTagsLogged = false;

bool PanelIsVisible(std::uintptr_t panel) noexcept
{
    __try { return (*reinterpret_cast<unsigned volatile*>(panel + kPanelFlags) & 0x1u) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Separate function purely so the __try has no std::string temporaries in scope: C2712 says a
// function needing object unwinding cannot host one, and Log::Line's argument is exactly that.
bool PanelTagName(std::uintptr_t panel, char* out, size_t cap) noexcept
{
    __try { return ReadName(*reinterpret_cast<unsigned long long volatile*>(panel + kPanelTag), out, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// One-shot census of every live panel: its tag, its handler class, whether the engine considers it
// visible, and where its movie root sits. If the bottom-right block turns out not to be DesignerUI,
// this names the panel that owns it instead of costing another guess.
void LogPanelCensus() noexcept
{
    static FoundHandler s_h[32] = {};
    const int n = CollectHandlers(s_h, 32);
    for (int i = 0; i < n; ++i)
    {
        if (s_h[i].panel < 0x10000) continue;
        char tag[64] = {};
        if (!PanelTagName(s_h[i].panel, tag, sizeof(tag))) tag[0] = 0;
        void* p = reinterpret_cast<void*>(s_h[i].panel);
        float x = 0.0f, y = 0.0f, sx = 0.0f, a = -1.0f, v = -1.0f;
        ReadVar(p, L"_root._x", &x);      ReadVar(p, L"_root._y", &y);
        ReadVar(p, L"_root._xscale", &sx); ReadVar(p, L"_root._alpha", &a);
        ReadVar(p, L"_root._visible", &v);
        char b[384] = {};
        sprintf_s(b, "[PANELCENSUS] %-34s tag='%s' panel=0x%llX engineVisible=%d _root(x=%.1f y=%.1f xs=%.1f alpha=%.0f vis=%.0f)",
                  s_h[i].cls, tag, static_cast<unsigned long long>(s_h[i].panel),
                  PanelIsVisible(s_h[i].panel) ? 1 : 0, x, y, sx, a, v);
        ME2VR::Log::Line(b);
    }
}

// Collect the DesignerUI panels that the engine says are visible AND whose movie root is on screen.
void FindObjectivePanels() noexcept
{
    static FoundHandler s_h[32] = {};
    const int n = CollectHandlers(s_h, 32);
    int found = 0;
    for (int i = 0; i < n && found < 8; ++i)
    {
        if (s_h[i].panel < 0x10000) continue;
        if (std::strstr(s_h[i].cls, "DesignerUI") == nullptr) continue;
        if (!PanelIsVisible(s_h[i].panel)) continue;
        void* p = reinterpret_cast<void*>(s_h[i].panel);
        float sx = 0.0f, a = 0.0f, v = 0.0f;
        if (!ReadVar(p, L"_root._xscale", &sx) || sx == 0.0f) continue;   // movie not loaded
        ReadVar(p, L"_root._alpha", &a);
        ReadVar(p, L"_root._visible", &v);
        if (a <= 0.0f || v == 0.0f) continue;                             // loaded but not on screen
        g_objPanels[found].obj = s_h[i].obj;
        g_objPanels[found].panel = s_h[i].panel;
        ++found;
    }
    g_objPanelN = found;
    if (!g_panelTagsLogged) { g_panelTagsLogged = true; LogPanelCensus(); }
}

void ApplyObjectivePanels() noexcept
{
    for (int i = 0; i < g_objPanelN; ++i)
    {
        const PanelTarget& t = g_objPanels[i];
        if (t.obj < 0x10000 || t.panel < 0x10000) continue;
        __try
        {
            // Re-read the panel from its handler and confirm it is still the one the mod validated.
            if (*reinterpret_cast<std::uintptr_t volatile*>(t.obj + kHandlerPanel) != t.panel) continue;
            ApplyClipXform(ObjSlot(g_panFollow, 8, reinterpret_cast<void*>(t.panel)),
                           reinterpret_cast<void*>(t.panel), "_root",
                           g_groups[kGroupObjPanel], "OBJPANEL");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void ApplyPowerWheel() noexcept
{
    for (int i = 0; i < g_pwTargetN; ++i)
    {
        const WheelTarget& t = g_pwTargets[i];
        if (t.obj < 0x10000 || t.panel < 0x10000) continue;
        __try
        {
            // Re-validate: same object, still a power-wheel class, still pointing at the panel that was
            // validated. A handler that has swapped movies is skipped until the next refresh -
            // stale-target writes are what wrecked the HUD.
            if (!IsPwClass(*reinterpret_cast<std::uintptr_t volatile*>(t.obj + kObjClassOff))) continue;
            if (*reinterpret_cast<std::uintptr_t volatile*>(t.obj + kHandlerPanel) != t.panel) continue;
            ApplyClipXform(ObjSlot(g_objFollow, 8, reinterpret_cast<void*>(t.panel)),
                           reinterpret_cast<void*>(t.panel), t.path,
                           g_groups[kGroupPwRadar], "OBJCIRC");
            g_pwApplied = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// [HUDSAFE] Runs on the game thread for EVERY ProcessEvent in the game, so the reject path has to be
// nearly free: one deref of the object's class pointer and a compare against the cached HUD class.
void __fastcall ProcessEventHook(void* context, void* function, void* params, void* result) noexcept
{
    const auto obj = reinterpret_cast<std::uintptr_t>(context);
    if (!g_applying && g_hudClassPtr != 0 && obj >= 0x10000 && g_anyActive &&
        g_hudEnabled.load(std::memory_order_relaxed))
    {
        __try
        {
            // ONLY the HUD handler drives this. Wheel-class events used to apply too, and because
            // several wheel instances are live with different movies, that sprayed radar writes into
            // panels that were not the radar's. The wheel is now reached solely through
            // FindPowerWheel/ApplyPowerWheel below, against one validated panel.
            if (*reinterpret_cast<std::uintptr_t volatile*>(obj + kObjClassOff) == g_hudClassPtr)
            {
                const std::uintptr_t panel = *reinterpret_cast<std::uintptr_t volatile*>(obj + kHandlerPanel);
                if (panel >= 0x10000)
                {
                    if (panel != g_basePanel)
                    {
                        g_baseCount = 0; g_basePanel = panel;                 // movie rebuilt
                        for (int q = 0; q < kSquadCount; ++q)                  // re-read squad containers
                            if (!ReadFString(obj + kSquadInfo[q], g_squadPath[q], sizeof(g_squadPath[q])))
                                g_squadPath[q][0] = 0;
                    }
                    g_applying = true;
                    ApplyToLivePanel(reinterpret_cast<void*>(panel));
                    // The game recreates wheel panels every few seconds, so the target list has to be
                    // rebuilt on that timescale or it goes stale and the circle stops responding.
                    static unsigned s_refresh = 0;
                    if ((s_refresh++ % 240) == 0) { FindPowerWheel(); FindObjectivePanels(); }
                    ApplyPowerWheel();
                    ApplyObjectivePanels();
                    g_applying = false;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_applying = false; }
    }
    reinterpret_cast<tProcessEvent>(g_peTarget)(context, function, params, result);
}

}  // namespace

void RunDiscovery() noexcept
{
    Resolve();

    static FoundHandler s_found[32] = {};
    const int count = CollectHandlers(s_found, 32);
    if (count == 0)
    {
        ME2VR::Log::Line("[HUDDISC] no live SFXSFHandler_HUD found. Run this in normal gameplay with the "
                         "HUD on screen (not in a menu, cutscene or on the galaxy map).");
        return;
    }

    for (int h = 0; h < count; ++h)
    {
        char hdr[256] = {};
        sprintf_s(hdr, "[HUDDISC] handler #%d class=%s obj=0x%llX panel=0x%llX",
                  h + 1, s_found[h].cls,
                  static_cast<unsigned long long>(s_found[h].obj),
                  static_cast<unsigned long long>(s_found[h].panel));
        ME2VR::Log::Line(hdr);

        void* panel = (s_found[h].panel >= 0x10000) ? reinterpret_cast<void*>(s_found[h].panel) : nullptr;
        for (int f = 0; f < kPathFieldCount; ++f)
        {
            if (s_found[h].paths[f][0] == 0) continue;
            // Live geometry for the path, so the mod can see where each piece currently sits. _xscale is the
            // useful existence signal: a real display object reads ~100, a bogus path reads 0.
            float x = 0.0f, y = 0.0f, sx = 0.0f, sy = 0.0f;
            if (panel != nullptr)
            {
                wchar_t wp[256] = {};
                swprintf_s(wp, L"%hs._x", s_found[h].paths[f]);      ReadVar(panel, wp, &x);
                swprintf_s(wp, L"%hs._y", s_found[h].paths[f]);      ReadVar(panel, wp, &y);
                swprintf_s(wp, L"%hs._xscale", s_found[h].paths[f]); ReadVar(panel, wp, &sx);
                swprintf_s(wp, L"%hs._yscale", s_found[h].paths[f]); ReadVar(panel, wp, &sy);
            }
            char line[384] = {};
            sprintf_s(line, "[HUDDISC]   %-18s path='%s'  x=%.1f y=%.1f xscale=%.1f yscale=%.1f",
                      kPathFields[f].label, s_found[h].paths[f], x, y, sx, sy);
            ME2VR::Log::Line(line);
        }
    }
    for (int h = 0; h < count; ++h)
        for (int q = 0; q < kSquadCount; ++q)
        {
            char sp[160] = {};
            if (!ReadFString(s_found[h].obj + kSquadInfo[q], sp, sizeof(sp))) continue;
            char line[256] = {};
            sprintf_s(line, "[HUDDISC]   SquadMember%-8d path='%s'", q, sp);
            ME2VR::Log::Line(line);
        }
    // Sweep for movie children the handler exposes no path field for (radar, objective, compass...).
    // Existence test is _xscale != 0 - GetVariableFloat returns 0.0 for a path that doesn't exist,
    // which is exactly what made the first probe "find" 348 imaginary elements.
    if (count > 0 && s_found[0].panel >= 0x10000)
    {
        void* panel = reinterpret_cast<void*>(s_found[0].panel);
        static const char* const kSweep[] = {
            "mcNotification","mcObjective","Objective","mcRadar","Radar","mcCompass","Compass",
            "mcMinimap","Minimap","mcMap","Map","mcSquad","Squad","SquadUI","mcPower","PowerWheel",
            "mcAmmo","AmmoPower","mcSubtitle","Subtitle","Subtitles","mcHint","Hint","mcPrompt",
            "Prompt","mcQuest","Quest","mcMission","Mission","mcWaypoint","Waypoint","mcTracker",
            "Tracker","mcReticle","Reticle","Crosshair","mcCrosshair","mcHealth","mcShield",
            "mcWeapon","mcTarget","mcStatus","mcCooldown","Cooldown","mcIndicator","DamageIndicator",
        };
        const int kSweepN = static_cast<int>(sizeof(kSweep) / sizeof(kSweep[0]));
        int found = 0;
        for (int c = 0; c < kSweepN; ++c)
        {
            for (int variant = 0; variant < 2; ++variant)
            {
                const wchar_t* pfx = (variant == 1) ? L"_root." : L"";
                wchar_t wp[160] = {};
                float sx = 0.0f;
                swprintf_s(wp, L"%s%hs._xscale", pfx, kSweep[c]);
                if (!ReadVar(panel, wp, &sx) || sx == 0.0f) continue;
                float x = 0.0f, y = 0.0f, sy = 0.0f;
                swprintf_s(wp, L"%s%hs._x", pfx, kSweep[c]);      ReadVar(panel, wp, &x);
                swprintf_s(wp, L"%s%hs._y", pfx, kSweep[c]);      ReadVar(panel, wp, &y);
                swprintf_s(wp, L"%s%hs._yscale", pfx, kSweep[c]); ReadVar(panel, wp, &sy);
                char line[256] = {};
                sprintf_s(line, "[HUDDISC]   SWEEP '%ls%hs'  x=%.1f y=%.1f xscale=%.1f yscale=%.1f%s",
                          pfx, kSweep[c], x, y, sx, sy,
                          (x > 850.0f && y > 450.0f) ? "   <== BOTTOM RIGHT" : "");
                ME2VR::Log::Line(line);
                ++found;
                break;
            }
        }
        char sm[160] = {};
        sprintf_s(sm, "[HUDDISC] sweep found %d extra element(s) beyond the handler's path table", found);
        ME2VR::Log::Line(sm);
    }
    // Which handler owns each live panel, and is its movie actually loaded? This is the question that
    // matters now: if the bottom-right block isn't in the HUD movie, it belongs to one of these.
    for (int h = 0; h < count; ++h)
    {
        if (s_found[h].panel < 0x10000) continue;
        void* pnl = reinterpret_cast<void*>(s_found[h].panel);
        float rx = 0.0f;
        const bool live = ReadVar(pnl, L"_root._xscale", &rx) && rx != 0.0f;
        char line[256] = {};
        sprintf_s(line, "[HUDDISC] PANELMAP handler=%-34s panel=0x%llX  movie=%s",
                  s_found[h].cls, static_cast<unsigned long long>(s_found[h].panel),
                  live ? "LOADED" : "not loaded");
        ME2VR::Log::Line(line);
    }
    // The objective circle lives in the POWER WHEEL's movie, not the HUD one, so dump that panel too:
    // every path field the wheel handler exposes, with live geometry, plus a sweep for radar-ish
    // children. If the circle ever stops moving, this is the data that says why.
    for (int h = 0; h < count; ++h)
    {
        if (std::strstr(s_found[h].cls, "PowerWheel") == nullptr || s_found[h].panel < 0x10000) continue;
        void* pnl = reinterpret_cast<void*>(s_found[h].panel);
        for (int q = 0; q < kPwCount; ++q)
        {
            char pp[160] = {};
            if (!ReadFString(s_found[h].obj + kPwFields[q], pp, sizeof(pp)) || pp[0] == 0) continue;
            float x = 0.0f, y = 0.0f, sx = 0.0f, sy = 0.0f;
            wchar_t wp[256] = {};
            swprintf_s(wp, L"%hs._x", pp);      ReadVar(pnl, wp, &x);
            swprintf_s(wp, L"%hs._y", pp);      ReadVar(pnl, wp, &y);
            swprintf_s(wp, L"%hs._xscale", pp); ReadVar(pnl, wp, &sx);
            swprintf_s(wp, L"%hs._yscale", pp); ReadVar(pnl, wp, &sy);
            char line[384] = {};
            sprintf_s(line, "[HUDDISC]   PW[+0x%llX]%s path='%s'  x=%.1f y=%.1f xscale=%.1f yscale=%.1f",
                      static_cast<unsigned long long>(kPwFields[q]),
                      (kPwFields[q] == kPwRadar) ? " <== RADAR/OBJECTIVE CIRCLE" : "",
                      pp, x, y, sx, sy);
            ME2VR::Log::Line(line);
        }
        static const char* const kPwSweep[] = {
            "radarMC", "mcRadar", "Radar", "radar", "objectiveRadar", "mcObjective", "compass",
        };
        for (int c = 0; c < static_cast<int>(sizeof(kPwSweep) / sizeof(kPwSweep[0])); ++c)
        {
            for (int variant = 0; variant < 2; ++variant)
            {
                const wchar_t* pfx = (variant == 1) ? L"_root." : L"";
                wchar_t wp[160] = {};
                float sx = 0.0f;
                swprintf_s(wp, L"%s%hs._xscale", pfx, kPwSweep[c]);
                if (!ReadVar(pnl, wp, &sx) || sx == 0.0f) continue;
                float x = 0.0f, y = 0.0f;
                swprintf_s(wp, L"%s%hs._x", pfx, kPwSweep[c]); ReadVar(pnl, wp, &x);
                swprintf_s(wp, L"%s%hs._y", pfx, kPwSweep[c]); ReadVar(pnl, wp, &y);
                char line[256] = {};
                sprintf_s(line, "[HUDDISC]   PW SWEEP '%ls%hs'  x=%.1f y=%.1f xscale=%.1f",
                          pfx, kPwSweep[c], x, y, sx);
                ME2VR::Log::Line(line);
                break;
            }
        }
        break;   // one live wheel panel is the answer
    }
    ME2VR::Log::Line("[HUDDISC] done. Paths above are read straight from the game's own HUD handler, so "
                     "they are the real element paths - no guessing needed.");
}


void SetEnabled(bool on) noexcept { g_hudEnabled.store(on, std::memory_order_release); }
bool GetEnabled() noexcept { return g_hudEnabled.load(std::memory_order_acquire); }
int  GroupCount() noexcept { return kGroupCount; }
const char* GroupLabel(int i) noexcept
{
    return (i >= 0 && i < kGroupCount) ? kGroups[i].label : "";
}
GroupCfg* GetGroup(int i) noexcept
{
    EnsureGroupsInit();
    return (i >= 0 && i < kGroupCount) ? &g_groups[i] : nullptr;
}

// Called once per present. Does NOT touch the game - it only installs the hook and recomputes the
// cheap "is anything non-neutral" flag the hook reads. All game writes happen on the game thread.
void Tick() noexcept
{
    EnsureGroupsInit();
    bool any = false;
    for (int g = 0; g < kGroupCount && !any; ++g)
        any = (g_groups[g].offX != 0.0f || g_groups[g].offY != 0.0f ||
               g_groups[g].scaleX != 1.0f || g_groups[g].scaleY != 1.0f);
    g_anyActive = any;
    if (!any || !g_hudEnabled.load(std::memory_order_acquire)) return;

    // One-shot report of whether the objective-circle writes actually reached the wheel's movie, so a
    // dead control is visible in the log instead of just feeling broken in the headset.
    if (g_pwPathsDirty)
    {
        g_pwPathsDirty = false;
        char b[256] = {};
        sprintf_s(b, "[HUDPANEL] power-wheel paths read: radar='%s' panel=0x%llX",
                  g_pwPath[kPwRadarIdx], static_cast<unsigned long long>(g_pwPanel));
        ME2VR::Log::Line(b);
    }
    if (g_pwApplied && !g_pwLogged)
    {
        g_pwLogged = true;
        ME2VR::Log::Line("[HUDPANEL] objective circle (radar) writes reached the live power-wheel panel");
    }

    static bool s_installed = false;
    if (s_installed) return;
    ResolveHudClass();
    if (!Resolved() || (g_hudClassPtr == 0 && g_pwClassPtr == 0)) return;   // retry next frame until the HUD exists

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* target = reinterpret_cast<void*>(base + kProcessEvent);
    void* orig = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&ProcessEventHook), &orig) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
    {
        g_peTarget = orig;
        s_installed = true;
        ME2VR::Log::Line("[HUDSAFE] ProcessEvent hook installed - HUD writes now run on the game thread");
    }
}
}  // namespace ME2VR::PcHud
