#pragma once

namespace ME2VR::CalcViewHook
{
// Hook the confirmed ULocalPlayer::CalcSceneView (MassEffect2.exe+0x6CEB70) with the correct
// 6-arg signature (pointer args only -> register-safe), and log the returned FSceneView's
// Proj/View/ViewOrigin to confirm identity + validate the LE1-identical offsets live.
// Call once per Present; installs once, logs a few frames, then stays quiet. Pure observe+forward.
void Tick() noexcept;

// Game's per-eye rendered half-FOV (radians), published from CalcSceneView's projection matrix.
// The OpenXR bridge declares THESE (not the headset FOV) so the stereo image isn't stretched/zoomed.
// Returns 0 until a perspective view has been seen.
float GetGameHalfFovH() noexcept;
float GetGameHalfFovV() noexcept;
// RAW game half-FOV (never widened by fill) - used for reliable cinematic detection.
float GetGameRawFovH() noexcept;
float GetGameRawFovV() noexcept;
// [UIRATIO] half-FOV the view was actually RENDERED with (raw game FOV, or the fill-widened target).
float GetRenderHalfFovH() noexcept;
float GetRenderHalfFovV() noexcept;
// [PERF] drain the SFR replay-cost accumulators (avg/max microseconds + samples since last call).
void GetSfrReplayStats(long long* avgUs, long long* maxUs, long long* count) noexcept;

// FOV-fill: widen narrow (conversation/cutscene) views to this target half-FOV (radians) so they fill
// the headset instead of a small black-bordered box. Pushed from the XR side; matched on submit.
void SetFovFill(float hHalfRad, float vHalfRad, bool on) noexcept;

// Conversation/cutscene = 16:9 cinematic FOV -> render mono (flat panel). Detected XR-side off the FOV
// aspect and set here; calcview then renders one full view instead of the stereo split.
void SetCinematic(bool on) noexcept;
bool GetCinematic() noexcept;
// [VRCINE] experimental: render conversations/cutscenes in VR + head-tracking-during-cine toggle.
// [CONVOFP] first-person conversations: per-present frame token (idempotent per-view relocation) and
// the facing-sign toggle. Signs that two builds could not settle become a click, not a third build.
void ConvoFpBeginFrame() noexcept;
void SetConvoFpInvertFacing(bool on) noexcept;  bool GetConvoFpInvertFacing() noexcept;
// Relocation applied vs skipped, cumulative. A rising skip count while armed IS the flicker.
void GetConvoFpCounts(unsigned long long* applies, unsigned long long* skips) noexcept;

bool GetCineVrConvo() noexcept;        void SetCineVrConvo(bool on) noexcept;
bool GetCineVrCutscene() noexcept;     void SetCineVrCutscene(bool on) noexcept;
bool GetCineVrHeadTracking() noexcept; void SetCineVrHeadTracking(bool on) noexcept;
bool GetVrCineActive() noexcept;       void SetVrCineActive(bool on) noexcept;
// [ENGCINE] P1 CalcSceneView heartbeat - advances every frame the engine builds a real scene view.
// Dead (~0/s) during prerendered biks; alive during in-engine gm-8 cutscenes. ME3's discriminator.
unsigned GetP1CalcSeq() noexcept;
bool GetEngCineAlive() noexcept;   void SetEngCineAlive(bool on) noexcept;   // gm8+alive -> treat as cutscene
// [CINEDEBOUNCE] consecutive narrow-FOV frames required before the cine detector ENTERS (exit immediate).
int  GetCineEnterFrames() noexcept; void SetCineEnterFrames(int f) noexcept;
// [STEREODEAD] replay-skip streak + reason bits (1=menuMode, 2=cinematic) for the submit watchdog.
void GetSfrSkipInfo(unsigned* streak, unsigned* reasons) noexcept;
// [NOOPREPLAY] consecutive replays that ran but rebuilt no scene view = no second eye despite the
// replay counter looking healthy. Non-zero means the eyes are identical.
unsigned GetSfrNoopReplays() noexcept;
void GetSfrNoopDiag(long long* pass0UsLast, unsigned* pass0Noops, unsigned* retryAttempts, unsigned* retrySuccesses) noexcept;

// Master VR switch. OFF = bypass the whole VR pipeline (split/mono/XR/UI-dup) -> vanilla flat game.
void SetVrEnabled(bool on) noexcept;
bool GetVrEnabled() noexcept;

// Drive free head-look: yaw/pitch as UE rotation units (65536 = 360 deg). enabled=false clears it.
void SetHeadLook(int yawUU, int pitchUU, bool enabled) noexcept;

// Head-look tuning (ME1 parity). smoothing = low-pass that kills the raw-jitter shake; sensitivity scales
// the delta; user-enable is the master view-look toggle; invert flips each axis.
bool  GetHeadLookUserEnabled() noexcept;   void SetHeadLookUserEnabled(bool v) noexcept;
float GetHeadLookSmoothing() noexcept;     void SetHeadLookSmoothing(float v) noexcept;   // 0..0.95
float GetLookSensitivity() noexcept;       void SetLookSensitivity(float v) noexcept;     // 0.1..3.0
bool  GetInvertLookYaw() noexcept;         void SetInvertLookYaw(bool v) noexcept;
bool  GetInvertLookPitch() noexcept;       void SetInvertLookPitch(bool v) noexcept;

// Positional 6DOF: head translation in METERS (XR axes, relative to recenter). Applied to the camera
// origin scaled to the stereo world-scale. Toggle with SetHeadPosEnabled (default on).
void SetHeadPos(float x, float y, float z) noexcept;
void SetHeadPosEnabled(bool on) noexcept;
bool GetHeadPosEnabled() noexcept;
void SetHeadPosScale(float s) noexcept;   // amplify head movement (1.0 = true 1:1)
float GetHeadPosScale() noexcept;
// [LEANFWD] flip 6DOF forward/back if leaning in moves the view out.
void SetLeanInvertFwd(bool on) noexcept;
bool GetLeanInvertFwd() noexcept;
void LogPosDiag() noexcept;   // throttled diagnostic of head input + basis + world delta

// Monotonic counter, bumped every frame the SBS split runs. Lets the bridge tell stereo frames
// (gameplay) from mono frames (menus/loading) and present those full-screen to both eyes.
unsigned long long GetSplitSeq() noexcept;

// Stereo knobs (Insert menu): half-eye separation in UE units (~1.6 = 1:1), and eye swap.
float GetHalfEyeUU() noexcept;
void  SetHalfEyeUU(float v) noexcept;
bool  GetSwapEyes() noexcept;
void  SetSwapEyes(bool v) noexcept;
// [SFR] convergence (fusion-plane pull-in, fixes distant-marker doubling without lowering IPD) + HUD scale.
float GetSfrConvergence() noexcept;
void  SetSfrConvergence(float v) noexcept;
float GetSfrUiScale() noexcept;
void  SetSfrUiScale(float v) noexcept;
// [MOVEFIX/AIMSEED] the APPLIED render-side head-look offset (0 when look is off).
int HeadLookYawUU() noexcept;
int HeadLookPitchUU() noexcept;
// [POSETAG] submit pose-tag delay in presents (Bug 1/2 smoothness fix); 0 = tag with current pose.
float GetPoseTagDelayFrames() noexcept;
void  SetPoseTagDelayFrames(float v) noexcept;
// [SFR-UI] HUD master transform (ME1 HUD-tab parity): independent X/Y scale + center-relative offset.
float GetSfrUiScaleX() noexcept;  void SetSfrUiScaleX(float v) noexcept;
float GetSfrUiScaleY() noexcept;  void SetSfrUiScaleY(float v) noexcept;
float GetSfrUiOffX() noexcept;    void SetSfrUiOffX(float v) noexcept;
float GetSfrUiOffY() noexcept;    void SetSfrUiOffY(float v) noexcept;

// --- VR mode (Mono=0 / Stereo=1 / AER=2) + AER knobs (2026-07-04 AER port from ME1). ---
int  GetVrMode() noexcept;              void SetVrMode(int m) noexcept;
float GetAerHalfEyeUU() noexcept;       void SetAerHalfEyeUU(float v) noexcept;   // IPD/scale 0..8 uu
bool  GetAerSwapEyes() noexcept;        void SetAerSwapEyes(bool v) noexcept;     // invert depth
bool  GetAerFramePacing() noexcept;     void SetAerFramePacing(bool v) noexcept;  // display-locked pacing
int   GetAerFramePacingHz() noexcept;   void SetAerFramePacingHz(int v) noexcept; // 0 = auto (shared: same headset Hz for stereo)
bool  GetStereoFramePacing() noexcept;  void SetStereoFramePacing(bool v) noexcept;
// [PACE1X] true = present every display frame (up to headset refresh). false = every other (half).
bool  GetFullRefreshPacing() noexcept; void SetFullRefreshPacing(bool v) noexcept; // display-locked pacing for same-frame stereo (kills 60fps-into-120Hz judder)
// --- DIBR (depth-warp) config (persisted; pushed to the d3d_capture warp shader each frame). ---
float GetDepthWarpGain() noexcept;      void SetDepthWarpGain(float v) noexcept;   // depth strength 0..5
float GetDepthWarpConv() noexcept;      void SetDepthWarpConv(float v) noexcept;   // manual convergence plane 0.90..1.005
bool  GetDepthWarpFlip() noexcept;      void SetDepthWarpFlip(bool v) noexcept;    // flip depth sign (forward-Z)
bool  GetDibrAutoConverge() noexcept;   void SetDibrAutoConverge(bool v) noexcept; // track subject depth
int   GetDibrResX() noexcept;           void SetDibrResX(int v) noexcept;          // square render res (restart)
int   GetDibrResY() noexcept;           void SetDibrResY(int v) noexcept;
// [INVMAT] stale-inverse-matrix fix (LE1 dark-panel root cause; default ON, self-validating scan).
bool  GetInvMatrixFix() noexcept;       void SetInvMatrixFix(bool v) noexcept;
// AER render<->present eye handshake: present arms the next eye; the detour stamps the eye it built.
int  GetAerRenderEye() noexcept;        void SetAerRenderEye(int e) noexcept;
bool GetAerStamp(unsigned long long* seq, int* eye) noexcept;   // ME2 analogue of ME1 GetLastRenderStamp
// [AERSHAKE] FIFO stamp consume: oldest build after lastSeq, one per present - label matches pixels.
bool ConsumeAerStamp(unsigned long long lastSeq, unsigned long long* outSeq, int* outEye, bool* resynced) noexcept;
}
