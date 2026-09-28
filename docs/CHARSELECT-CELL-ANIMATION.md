# Character-select cell "animation" — NOT an animation: the widescreen HUD squeeze

**Status: root-caused and the roster-select half is FIXED. The fight half is unvalidated.**
**Date: 2026-09-27/28.**

## The short version

The "the cursor cell is drawn at the wrong scale for ~16 frames" artifact is **not the game's
selected-cell grow animation** and **not a texture-replacement problem**. It is the **widescreen HUD
squeeze** (`g_ps2xWsHudInv`, the `[wshud]` path in `ps2_gs_gpu_renderer.cpp`) running on the roster
screen, where it has no business running. Roster select has a 3D sky/terrain background, so the
squeeze's "this frame has a depth-compared 3D triangle" heuristic is TRUE there and squeezes the
strip. The 1P label and the two cursor-adjacent cells get compressed 1.44x (`inv=0.694`) and the
portraits fall out of their own cell borders.

The squeeze exists because in true widescreen the 3D scene is pre-squeezed by the projection patch
and un-squeezed at the present, so the **fight HUD bars** — authored for 4:3 — must be squeezed
back or they come out too wide (confirmed by the user: with the squeeze off the fight bars are
"much wider"). Nothing else wants that. Roster select's strip is authored to the full 4:3 width.

## Evidence, measured not eyeballed

The user supplied two same-instant captures of the roster strip (`Captura de pantalla_20260927_233120`
and `_233345`; portraits differ only because a different roster line was selected). Per-image
measurements (so the different crop offsets do not matter — only within-image ratios):

| | 233120 | 233345 | ratio |
| --- | --- | --- | --- |
| 1P label width | 74 px | 83 px | 1.12x |
| 1P label height | 39 px | 48 px | 1.23x |
| cell pitch | 157 px | 188 px | **1.20x** |
| strip band height | 117 px | 117 px | 1.00x (unchanged) |

The 1P glyphs grow, the cell pitch grows, the strip frame does not. `inv = 0.694` ⇒ the squeeze
magnifies by `1/0.694 = 1.44x`, which is the scale of the defect.

## The fix (in `ps2_gs_gpu_renderer.cpp`, the `[wshud]` block)

Gate the squeeze on the game's **own top-level state** instead of the draw-call heuristic:

```cpp
extern std::atomic<uint32_t> g_bt3StateLive;              // existing, refreshed every tick by the run loop
const uint32_t wsState = g_bt3StateLive.load(std::memory_order_relaxed);
const bool wsInFight  = (wsState == 0x2du);              // 0x2D == IN_FIGHT
...
if (wsInFight && s_wsActive) wsHudInv = g_ps2xWsHudInv;
```

`g_bt3StateLive` was already the established cross-file signal (this file already read it for the
loading/0x2d probes). The 3D-frame heuristic is kept only as a *secondary arm* so unrecognised
duellist states (training 0x2c, ultimate 0x0d) can still opt in, but the state gate must be true to
apply the squeeze at all — so a menu can never be squeezed regardless of its 3D background.

State names come from the one canonical table in `game_overrides.cpp` (`bt3StateName`):
`0x27 = CHARACTER_SELECT`, `0x2D = IN_FIGHT`, `0x2C = ULTIMATE_TRAINING`, `0x0D = ULTIMATE_BATTLE`.
The comment at `game_overrides.cpp:2990` warns there used to be two disagreeing tables; this is that
one table.

## What is verified and what is not

- **Roster select: FIXED.** With the state gate, the 1P and the cell row render at correct
  proportions, no 1.20x pitch, no 1.23x 1P. User-confirmed.
- **Fight: NOT yet validated.** The verification run never reached a fight — `PS2X_MENU_JUMP=39`
  stops at roster select, and the `[wsstate]` log shows only `0x01` (BOOT) and `0x27`
  (CHARACTER_SELECT). `0x2D` never appeared, so the squeeze stayed OFF the whole run and the bars
  were unsqueezed. **This is a test-harness gap, not a code failure.** The next step is a run that
  actually enters a duel, and confirm the state flips to `0x2D` in `[wsstate]` and the bars regain
  correct proportions.

A first attempt used a call-count disarm (turn the squeeze off after N render calls with no 3D)
instead of the state gate. The user caught that it made the artifact reappear during a
cell over-expand. That approach is **reverted**; the state gate is the fix.

## Diagnostic

- `PS2X_WSNOHUD=<existing-file>` forces the squeeze off (renderer half). With it, roster select was
  clean — that is how the squeeze was identified as the cause.
- `[wsstate]` (new, in this fix) logs the state and whether the squeeze is a candidate.

## Notes / prior history still valid

- The present crop oscillation was already fixed separately (`6f5b8a6`, `[presentclamp]`) and is not
  this bug.
- Texture replacement is ruled out: reproduces with the pack on and off.
- The doc's earlier "cell width is a constant 273 px" figure was measured on a differently-scaled
  capture; the invariant that matters is **equal pitch within a frame**, which holds in the good
  frames and breaks in the bad ones (157→188).
