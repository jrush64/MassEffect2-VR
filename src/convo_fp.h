#pragma once

#include <cstdint>

// [CONVOFP] First-person conversations.
//
// In a Mass Effect conversation the possessed pawn is PARKED at the trigger spot and never ticks --
// ME1's CINEFP lane proved that with a 7-minute frozen-target log and died on it. The Shepard you
// actually see is a separate STAGED actor that the conversation animates. LE2 hands the mod that actor
// directly: ABioWorldInfo.m_oCurrentConversation -> UBioConversation.m_pConversationPlayer, alongside
// the live speaker/listener pointers and the engine's own look-at table.
//
// This module is pure READ. Its whole job: "given that the mod is in a conversation, where is Shepard's
// eye and which way should it face -- or say no." Nothing else in the codebase learns about
// conversations, and nothing here writes to a game object.
namespace ME2VR::ConvoFp
{
// World eye pose, Unreal units, UE3 axes (X forward, Y right, Z up).
struct EyePose
{
    float x, y, z;
    float baseYawDeg;   // world yaw the view should face; the HMD rotates on top of this
};

// Per-frame update. Cheap no-op when disabled or out of conversation. Call from the present hook.
void Tick() noexcept;

// True when the mod is in a conversation, the feature is on, and the pose below is trustworthy.
bool IsArmed() noexcept;

// The pose to render from. Only valid while IsArmed().
bool GetEyePose(EyePose* out) noexcept;

// Config (menu + MELE2VR.ini [FirstPerson]).
void  SetEnabled(bool on) noexcept;        bool  GetEnabled() noexcept;
void  SetTurnRate(float degPerSec) noexcept;  float GetTurnRate() noexcept;   // 0 = base never turns
void  SetAnimFollow(float f) noexcept;        float GetAnimFollow() noexcept; // 0 = steady, 1 = full animation
void  SetEyeUpUU(float uu) noexcept;          float GetEyeUpUU() noexcept;    // eye height above the actor origin (fallback path)
void  SetEyeFwdUU(float uu) noexcept;         float GetEyeFwdUU() noexcept;   // eye forward of the actor origin
// Magnification during a first-person conversation. 1 = the headset's own FOV. Implemented by
// NARROWING the rendered FOV while the DECLARED one stays put, so the world magnifies uniformly and
// the UI is untouched - ME1 proved that an honest cropped window instead warps the conversation wheel.
void  SetZoom(float z) noexcept;              float GetZoom() noexcept;
void  SetHideHead(bool on) noexcept;          bool  GetHideHead() noexcept;   // camera is inside the skull
void  SetKillDof(bool on) noexcept;           bool  GetKillDof() noexcept;    // convo DOF focuses the speaker

// Diagnostic snapshot for the menu readout (all zero/false when not armed).
struct Diag
{
    unsigned long long conv;         // UBioConversation*
    unsigned long long stagedPlayer; // m_pConversationPlayer
    unsigned long long speaker;      // m_pSpeaker
    unsigned long long faceTarget;   // actor the mod is facing
    int   source;                    // 0 none, 1 head component, 2 body mesh, 3 actor location, 4 hold
    int   speakerCount;
    float x, y, z, baseYawDeg;
};
void GetDiag(Diag* out) noexcept;
}
