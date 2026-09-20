#pragma once

#include <cstdint>

namespace ME2VR::EngineProbe
{
// Read-only discovery: resolve GEngine at the community SDK's LE2 RVA, walk to the
// primary ULocalPlayer, confirm the (LE1-identical) ULocalPlayer field offsets read sane
// live, and dump the FViewportClient::Draw vtable slot. Self-latches after one success.
// Call freely from the Present hook; it no-ops once it has dumped or until the engine is up.
void TryDumpOnce() noexcept;

// Live re-resolve of the primary (P1) ULocalPlayer via GEngine->GamePlayers[0].
// Returns 0 until the engine + a local player exist. SEH-guarded.
std::uintptr_t GetPrimaryLocalPlayer() noexcept;

// True when the game is paused (AWorldInfo.Pauser != null) - i.e. a full-screen blocking menu
// (journal/squad/pause) is up. Gameplay and conversations are NOT paused. Reliable, noise-free.
bool IsGamePaused() noexcept;

// Engine EGameModes byte (0..12; 7=GUI menu, 8=movie, 9=galaxy map, 10=orbital/planet scan), -1 if
// unreadable. ME1's authoritative UI-context read, LE2 offsets (PC+0xA68 -> GMM+0xB8).
int ReadGameModeSEH() noexcept;

// USFXEngine.LoadMovieManager -> USFXLoadMovieManager.PlaybackState.  Returns 1 while a
// fullscreen loading/prerendered movie is playing, 0 while idle, and -1 when unreadable.
// This separates Bink/loading content from in-engine scenes that also report game mode 8.
int ReadLoadMoviePlaybackStateSEH() noexcept;

// Diagnostic: fill out[0..4] = {P1, PlayerController, WorldInfo, Pauser, bPlayersOnly} for logging.
void GetPauseChain(unsigned long long out[5]) noexcept;

// Discovery: dump the camera/object graph (class names) reachable from P1, to find the conversation /
// cutscene state signal (expected: a distinct PlayerCamera mode). Read-only; trigger on a hotkey.
void DumpGfxState() noexcept;

// First-person (basic): zero the active camera mode's Offset (the 3rd-person boom) each frame so the
// camera sits at the head. Call every frame from the present hook. Toggle with SetFirstPerson.
void ApplyFirstPerson() noexcept;
void SetFirstPerson(bool on) noexcept;
bool GetFirstPerson() noexcept;
void SetMeshHide(bool on) noexcept;   // F7: hide own body (independent of FP camera, for testing)
bool GetMeshHide() noexcept;
void DumpWeaponChainNow() noexcept;   // F8: log weapon fields + inventory chain to locate the held weapon

// Per-SFXCameraMode-state first-person config, matching ME1's shipped set: Explore, ExploreStorm, Combat,
// CombatStorm, TightAim. Chosen by exact live camera-mode class name. Eye offset is the mode Offset (X=fwd,
// Y=right, Z=up). Everything not listed - cover, sniper scope, the Mako, cutscenes - stays third-person.
struct FpStateCfg { bool on; bool hideHead; bool hideBody; bool hideWeapon; float x; float y; float z; };
int FpStateCount() noexcept;                  // number of states exposed (5)
FpStateCfg* GetFpStateCfg(int id) noexcept;   // mutable config for state id (0..count-1), nullptr if OOB
const char* FpStateLabel(int id) noexcept;    // human label, e.g. "Explore (unarmed)"
FpStateCfg FpStateDefault(int id) noexcept;   // code default for state id (menu Reset/Restore-ALL)

// Head-aim: while a weapon is out, the HMD drives the controller's ControlRotation (gun aims where you
// look), injection-style on top of stick/mouse. Call DriveHeadAim every frame with the HMD yaw/pitch.
// allowAim=false (storm latch) releases the injection and keeps head-look in charge.
// pitchOnly: drive aim PITCH from the head but leave YAW on the stick. Used while sprinting, where head
// yaw must not touch the heading but pitch can track freely - so aim is already raised at the moment of firing.
void DriveHeadAim(float headYawDeg, float headPitchDeg, bool allowAim, bool pitchOnly = false) noexcept;
// [AIMSEED v2] untransferred look->aim handoff remainder (render it as head-look during the ramp).
void GetAimSeedRem(int* yawUU, int* pitchUU) noexcept;
// [VEHAIM] Hammerhead/Firewalker head behavior. Mode 0=off, 1=free-look (head turns the VR view,
// aim on stick), 2=camera-aim (head drives DriverViewYaw/Pitch = the chase camera).
bool DriveVehicleAim(float headYawDeg, float headPitchDeg) noexcept;   // mode 2 write; true if it wrote
bool IsInDrivableVehicle() noexcept;                                   // true while driving a vehicle
int  GetVehicleHeadMode() noexcept;
void SetVehicleHeadMode(int m) noexcept;
// [VEHDIAG] pure-read: log ControlRotation vs DriverViewYaw/Pitch while driving (find the fire source).
void ProbeVehicleFields() noexcept;
// [FPSTORM] live camera-mode class for the storm latch.
bool IsCamModeStorm() noexcept;
bool IsCamModeInterp() noexcept;
bool IsCamModeAiming() noexcept;
bool IsHeadAimActive() noexcept;              // true when head-aim is enabled AND a weapon is out
bool IsWeaponOut() noexcept;
// [COVERFLASH probe] live camera-mode name + weapon-verdict internals (hold frames, raw match).
void GetCoverFlashState(char* out, int cap, int* hold, bool* raw) noexcept;                  // combat/ADS/sniper/cover camera (vs holstered) - suppresses cinematic mono
bool IsWeaponFiring() noexcept;               // trigger held (ABioPawn.bWeaponFiring) - aim-intent for the storm latch
// [CONVOFP] Hide/restore the head on an arbitrary actor (the conversation's staged Shepard, which the
// gameplay FP path does not own). Head + hair + headgear + visor + faceplate + accessories only.
int  ConvoFpSetHeadHidden(std::uintptr_t actor, bool hide) noexcept;
// [CONVOFP] Call ABioPlayerController.DisableDOF(). Conversation depth-of-field is a matinee track that
// focuses the current speaker, so from Shepard's eyes the person you are looking at goes soft.
bool ConvoFpDisableDof() noexcept;
// [CONVOFP] Widen ACamera.CameraCache.POV.FOV (@0x47C) before CalcSceneView reads it, so the engine
// builds its CULLING frustum wide. A conversation camera is ~20deg while the mod renders at the headset's
// ~50, and everything outside that cone is culled before submission - that is the "character vanishes
// but their legs are still visible" case. Returns the previous value so the caller can restore it.
bool ConvoFpSetCameraFovDeg(float deg, float* outPrev) noexcept;
// [FILLSRC] read-only twin of the setter above: the camera's current cached FOV in degrees.
bool GetCameraFovDeg(float* outDeg) noexcept;
// [CONVOFP] ABioStage.m_bDOFActive (bit 0x08 @ 0x344) - BioWare's own per-conversation DOF switch.
// Clearing it kills DOF for this conversation ONLY; the global setting is untouched.
bool ConvoFpReadStageDofActive(std::uintptr_t stage, bool* outActive) noexcept;
bool ConvoFpSetStageDofActive(std::uintptr_t stage, bool active) noexcept;

// [HEADDELAY] frames of first person before the head is hidden (ME1 parity, default 20). Lets the
// camera blend reach the eye first so you never see a headless body mid-transition.
int  GetFpHeadHideDelay() noexcept;
void SetFpHeadHideDelay(int frames) noexcept;
bool IsFpBlend() noexcept;                    // diagnostic: an Interpolate blend is active while FP is held
void SetHeadAimEnabled(bool on) noexcept;
bool GetHeadAimEnabled() noexcept;
void SetHeadAimInvertYaw(bool on) noexcept;   bool GetHeadAimInvertYaw() noexcept;
void SetHeadAimInvertPitch(bool on) noexcept; bool GetHeadAimInvertPitch() noexcept;
}
