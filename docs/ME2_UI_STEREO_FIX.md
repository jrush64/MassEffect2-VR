# ME2 VR — UI Stereo Fix (cross-eyed / stretched / scrambled HUD + menus)

**Status:** FULLY FIXED 2026-06-27. The 2D UI/HUD/menus — **panels AND text** — now render correctly
per-eye (fused, correct aspect, readable) in stereo, with the world stereo intact and **no mode
snapping**. This is the genuinely hard part of UE3 stereo VR — LE1 has the same bug and the same fix
applies there.

The fix landed in three distinct stages, each solving a different symptom. Read all three — the final
working build needs all of them together.

| Symptom | Root cause | Fix |
|---|---|---|
| HUD/menu **cross-eyed** (each eye half the UI) | UI drawn once full-width onto the SBS backbuffer | per-eye duplicate the UI draws |
| menu **stretched vertically** | duplicating a full-width draw into a half-*width* box squishes it 2:1 | halve the **height** too + center (preserve aspect) |
| codex/journal/squad **text scrambled** | **text is non-indexed `Draw`** — that detour wasn't duplicating | dup the UI path in `MhDraw` too |
| view **snapped** when toggling menu mode | world switched stereo↔mono per menu | world is **always stereo**; UI handled entirely in the draw hooks |

---

## The problem

In SBS stereo (the P1-layout split renders two world views into one side-by-side backbuffer), the
Scaleform GFx UI was drawn **once, full-width, across the whole backbuffer, after** the world. Each
eye then samples its half → each eye gets half of a full-width UI → **cross-eyed, unreadable**.

## Why every early attempt did nothing (the real root cause)

Repeated attempts to intercept/redirect the UI draws had **zero effect**. The reason, found by
instrumentation, not theory:

- The mod hooks the D3D11 immediate context by **patching its vtable slots**. State-setters
  (`OMSetRenderTargets` slot 33, `RSSetViewports` 44, `PSSetShaderResources` 8) fired thousands of
  times — but **no draw method ever fired** (`DrawIndexed` slot 12, etc.), across whole sessions.
- Not a deferred context (`CreateDeferredContext` hooked → never called) and no d3d11 wrapper DLL.
- **The engine caches the draw function POINTERS at init** (reads the vtable entry once, then calls
  the cached address directly). So vtable-slot patching is bypassed *for draws* while per-call vtable
  methods like `OMSetRenderTargets` still route through the hook.

Evidence (per-frame counters): `vtDI=0` (vtable DrawIndexed) but `mhDI≈200/frame` once hooking
function body. That's the whole reason the UI couldn't be touched.

## The diagnostic ladder that cracked it

1. `FIRST FIRE` one-shot log per draw method → only `Draw` (non-indexed) ever fired; `DrawIndexed` never.
2. Per-frame draw-type counters → `DrawIndexed=0` on the tracked context while the game clearly draws meshes.
3. Hooked `CreateDeferredContext` → never called (not deferred).
4. **MinHook the real DrawIndexed/Draw FUNCTION addresses** (read from the vtable, patch the function
   prologue) → `mhDI≈25k/120 frames`. MinHook catches cached-pointer callers; vtable patching can't.

## The fix

All in `src/M0PassiveDxgiProbe/d3d_capture.cpp` (draw hooks) + `me2_xr.cpp` (submit).

### 0. The UI-draw signature (one shared test: `IsUiDrawNow`)
Discriminate UI draws from world draws by their D3D state, *not* by call site:
- **World** geometry renders with the **HALF-width** viewport into **HDR** intermediates.
- **UI** (Scaleform HUD/menus/text) renders with a **FULL-width** viewport onto the **LDR backbuffer**
  (`R8G8B8A8_UNORM`, fmt 28) and is **alpha-blended**.

So a draw is UI when: `viewport.Width > 0.75 × backbufferWidth && currentRtvFormat == R8G8B8A8_UNORM
&& currentRtvWidth == backbufferWidth && OMGetBlendState→RenderTarget[0].BlendEnable`. The
**blend-enable gate is critical**: the world's full-screen *composite* pass is also full-viewport on
the LDR target but **opaque** — without the gate it gets duplicated too and re-splits the whole image
into SBS-per-eye. (UI alpha-blends; the composite doesn't.)

### 1. Hook ALL the draw paths via MinHook (not vtable patches)
The UI does **not** use one draw entry point. Measured live (the `UISTATE` log):
- **Panels / backgrounds → `DrawIndexed`** (slot 12) — the bulk, ~24k/120-frames in a menu.
- **Text glyphs → non-indexed `Draw`** (slot 13) — `Dnon` scaled exactly with on-screen text
  (**9/120 in gameplay → 3,816/120 in the codex**). This was the scrambled-text culprit.
- `DrawIndexedInstanced` (slot 20) is **MinHooked too but unused by ME2's UI** (`DII=0` every frame).
  Kept for safety / other games.

Each is **MinHooked on the real function body** (read the address from the vtable, then
`MH_CreateHook`), because the engine caches draw pointers and vtable-slot patches never fire (see the
diagnostic ladder above). All three route into the same per-eye dup.

### 2. Per-eye duplication, aspect-preserving (`DupUiDraw`)
For a UI draw, render it into the **left half then the right half** (set viewport via the real
`RSSetViewports`, re-issue the real draw twice, restore). Each eye gets the *complete* UI → fuses at
screen depth.

**Aspect:** a full-width UI draw squished into a half-*width* box is stretched 2:1 (the
"stretched vertically" symptom). So each half-viewport is **half width AND half height, centered
vertically** (`TopLeftY += 0.25×H`). The UI keeps its shape, letterboxed top/bottom, in each eye.
`DupUiDraw` is a template taking a thunk so `MhDrawIndexed`, `MhDraw`, and `MhDrawIndexedInstanced`
all share the identical dup.

### 3. World is ALWAYS stereo (no menu mode switch)
Earlier builds tried to detect a full-screen menu and present it **mono** (flat). ME2 exposes no clean
menu signal (`AWorldInfo.Pauser` never fires for journal/squad; UI-draw volume can't separate menus
from conversations), and worse, *switching* world stereo↔mono **snaps** the whole view. So that was removed
menu detection from the world path entirely: the world stays stereo, and the UI is made readable purely
by the per-eye dup above. A manual **F2 / Insert-menu "Flat screen mode"** toggle remains as an
optional convenience but is **off by default and not needed** for readable menus.

Mono is only a genuine fallback when the CalcSceneView split has been idle `kSbsHoldFrames` (30, ~0.5s)
— i.e. loading screens — via the SBS/mono hysteresis in `me2_xr.cpp` RunFrame (this also kills the old
gap-frame flashing: gap frames re-submit the unchanged SBS backbuffer).

## How it was found (systematic debugging, not guessing)
Three wrong guesses (dup DrawIndexed only; then DrawIndexedInstanced; then a scissor hypothesis) all
came back unchanged. The fix only came from **instrumentation first**: per-draw-type UI counters
(`DI`/`DII`/`Dnon`) + scissor sampling, dumped every 120 frames as `UISTATE`. One run with the codex
open showed `DII=0`, `scissorOn=0`, and `Dnon` tracking the text 1:1 → the text path was non-indexed
`Draw`, undup'd. Measure the running system before theorizing.

## Key knobs / signatures
- `g_uiDupEnabled` (default on) — the per-eye UI duplication, shared by all three draw detours.
- `IsUiDrawNow` — the UI-draw test (viewport > 0.75× bb width + RT `R8G8B8A8_UNORM` full-size + blend).
- `DupUiDraw` — aspect-preserving per-eye dup (half W, half H, centered).
- `kSbsHoldFrames = 30` — SBS hysteresis hold (mono only after the split is idle this long).

## Reusable lessons (LE1 / ME3 / any UE3 game)
- **If draw vtable-hooks never fire but state-setters do → the engine cached the draw pointers.
  MinHook the function body, not the vtable slot.** THE unlock; without it the UI is untouchable.
- **The UI uses multiple draw entry points — hook DrawIndexed AND non-indexed Draw (and instanced for
  safety).** Panels were DrawIndexed, text was Draw. Hooking only one fixes panels but leaves text
  scrambled.
- Distinguish UI from world/composite by **viewport size + RT format + blend-enable**, not by hunting
  a dedicated UI render target (ME has none — UI draws straight onto the backbuffer).
- Per-eye UI = duplicate each UI draw into each half-viewport, **half W and half H** (preserve aspect).
- **Don't switch the world between stereo and mono** to handle menus — it snaps. Keep the world stereo;
  fix the UI in the draw hooks.
- When fixes keep failing, **stop guessing and instrument** (per-draw-type counters) — that's what
  actually located the text path.
