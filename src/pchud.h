#pragma once

// ME2 per-element HUD control.
//
// LE2's HUD handler (SFXSFHandler_PCHUD) stores every Flash path as a string field on itself, so the
// element paths are READ from the game rather than guessed - which is why this is far less work than
// ME1's equivalent, where the names had to come from an offline parse of the HUD movie.
//
// Elements are grouped into the clusters a player thinks about (weapon/ammo, target info, powers,
// notification, centre status). Each group gets an offset and a scale, applied as a delta on top of
// the element's own live baseline, so the game keeps owning the real layout.

namespace ME2VR::PcHud
{
// Per-group user transform. offX/offY are in movie pixels (the HUD stage is 1280 wide); scale is a
// multiplier on the element's own scale, 1.0 = untouched.
// scaleX/scaleY are per-axis multipliers on the element's own scale (1.0 = the game's own size).
// Positions scale about the GROUP's centre as well, so a group grows outward instead of its pieces
// swelling into each other while staying put.
struct GroupCfg { float offX; float offY; float scaleX; float scaleY; };

void SetEnabled(bool on) noexcept;
bool GetEnabled() noexcept;

int         GroupCount() noexcept;
const char* GroupLabel(int index) noexcept;
GroupCfg*   GetGroup(int index) noexcept;    // mutable; nullptr if out of range

// Re-stamp the active transforms onto the live HUD. Call once per present. Returns immediately when
// disabled or when every group is neutral, so the default state costs nothing.
void Tick() noexcept;

// Log every live HUD panel and the element paths its handler exposes, with live geometry. Bound to
// F11. (NOT F9 - that's the game's quickload. F5 is quicksave. Never bind a probe to either.)
void RunDiscovery() noexcept;
}
