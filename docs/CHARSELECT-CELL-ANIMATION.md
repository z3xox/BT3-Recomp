# Character-select cell animation — OPEN

The selected roster cell is drawn at the wrong vertical scale for a short run of frames after the
screen changes. **It is still open.** It is cosmetic, it is pre-existing, and it only ever happens
on the OpenGL present — paraLLEl-GS renders it correctly.

## What it looks like

`docs/img/celldeform-1p.png` — the cell under the cursor is drawn stretched, the X placeholders
overflow their boxes.

`docs/img/celldeform-ref.png` — the same frame index range once it has settled; this is what it
should look like.

Both are frames out of a 60 fps capture of the roster strip
(`Videocaptura de pantalla_20260927_212100.webm`, 1596x141, 72 frames after the first second).
Frame 47 is inside the deformed run, frame 62 is the settled reference.

## Measured, not eyeballed

The vertical extent of the near-black cell box in each of the five roster cells, per frame:

| cell | frames 46-59 (deformed) | frame 62 (reference) |
| --- | --- | --- |
| 0 — the cursor cell | 116 px | 119 → 120 → **122 px** |
| 1 | 116 | 116 |
| 2 | 116 | 116 |
| 3 | 122 | 122 |
| 4 | 116 | 116 |

Cells 1-4 are pixel-stable across the whole capture; the cell **width** is a constant 273 px in all
72 frames. Only cell 0 moves, and it grows into a 122 px box as the selection settles — which is
the game's own selected-cell grow animation. So the artifact is not the grid deforming, it is the
**cursor cell's animation** being drawn at the wrong scale, and it lasts ~16 frames (~260 ms) from
the screen transition.

Note the run is 46-61, not a microsecond. The first reading of this ("a microsecond") came from
eyeballing a crop; the per-frame measurement is what corrected it.

## Ruled out

- **The present.** `PS2X_PRESENTLOG=1` shows two geometry changes for an entire run:
  `src=(0.0,64.0 512x448)` inside a 512-wide texture, then nothing. No alternation.
- **`displayWidth`/`displayHeight` scissor extents.** Fixed in `6f5b8a6` — a 640-wide extent was
  being presented from a 512-wide texture (128 columns past the end), and a short extent was
  scaling tall. Both clamped now, and the oscillation is gone.
- **Texture replacement.** Reproduces with the pack on and off, and with
  `PS2X_ABLEND128=1`.

## Where to look next

The strongest lead, and the reason this file exists, is that **P1 has it and P2 does not** — which
points at a difference between the two halves rather than at the animation itself.

### 1. The integer UV divisor on the sprite path

`ps2_gs_gpu_renderer.cpp`, the sprite branch of the draw:

```cpp
else if (c.texKey != 0)
    src = bt3Rectangle{c.su0, c.sv0, c.su1 - c.su0, c.sv1 - c.sv0};   // RAW
```

The manual-quad path next door normalises by `texH / rsTexScale`; the sprite path does not. A 4x
replacement sampled in native-texel coordinates against a texture 4x larger is the top-left quarter,
magnified. `rsTexScale` is the flag to read: **`div=1` on a 4x-sized texture would be the bug**,
and it would only bite P1 if P1's cell is the replaced one.

A diagnostic for this exists and is what the next step should run: `PS2X_ANIMLOG=1` logs, per half
of the screen, the dest rect, the source extent, the texture size and the divisor, on change.
**It has not produced output yet** — see the note below.

### 2. The gate build is not separated in the cache (paraLLEl-GS's answer)

paraLLEl-GS identifies an alpha-only write — a gate build, `FBMSK`'s RGB mask at `0xffffff`:

```cpp
// parallel-gs/gs/gs_interface.cpp:1289
desc.alpha_only_write = ((ctx.frame.desc.FBMSK & 0x00ffffffu) == 0x00ffffffu) ? 1u : 0u;
// parallel-gs/gs/gs_renderer.cpp:1391
if (replacement_iface && desc.samples == 1 && !desc.alpha_only_write)
// parallel-gs/gs/gs_interface.cpp:1622
hasher.u32(desc.alpha_only_write);   // gate builds cache separately from colour uses
```

It excludes the gate from replacement **and keys the texture cache on the flag**, so a gate build
and a colour use can never share a cache entry. That is why the cell's gate always carries the
game's own current shape and the animation never lags on Vulkan.

The OpenGL path has the replacement half of this — `rawAlphaDec`, and
`allowed = g_subDxW == 0 && !rawAlphaDec` — but **no cache key on the gate/colour distinction**.
The cell highlight is exactly a gate build plus colour fills drawn through it, which is the shape
of the bug.

## Note on the diagnostic

`PS2X_ANIMLOG` was placed twice in the wrong branch and logged nothing before that was found, so
its output has not been trusted yet. It is now on the sprite path, but **it has not been seen to
print a single line** even on the character-select screen — so either the sprite branch is itself
gated away for these draws, or the draws do not satisfy the `isTriangle && texKey != 0` filter.
That is the first thing to settle, and it needs no new logging: widen the filter to unconditional
and confirm the path emits at all before reading anything into the numbers.
