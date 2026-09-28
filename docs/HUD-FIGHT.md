# BT3 fight HUD — texture addresses, item identity, and what the pack breaks

**Status: addresses mapped, per-player HUD files found. One artifact still open.**
**Date: 2026-09-28.** Companion to `CHARSELECT-CELL-ANIMATION.md` (that one is the widescreen
squeeze on the roster strip; this one is the fight HUD and the texture pack).

## The short version

Three separate things were being chased as one:

1. **The bars did not drain with a texture pack on** — fixed, twice over, by two different causes
   (see below).
2. **A stray segment on the health bar** — still open. Leading candidate: the *damage flash*, which
   the game ships as its own per-player file and which upstream PCSX2 also patches.
3. **A HUD compositor** — the game already stores the HUD as one `.pak` per player with a
   parseable item table, which is a much better foundation than the draw-level approach.

The HUD's texture addresses are mapped. P1, P2 and the timer do **not** have separate VRAM
addresses — they share textures and are told apart by screen position and palette.

## HUD texture addresses (VRAM pages)

The fight HUD atlas occupies **0x2A00–0x2C00** (pages 10752–11264). Measured from a live fight
with `[barbox]`, and confirmed by a full-width census (`PS2X_HUDCENSUS`):

| `tbp0` | hex | dims | what |
| --- | --- | --- | --- |
| 10752 | `0x2A00` | 1024x256 | background atlas |
| 10784 | `0x2A20` | 256x256 | large art |
| 10816 | `0x2A40` | 256x64 | bar backing |
| **10880** | **`0x2A80`** | **256x64** | **health / ki fill** |
| **10992** | **`0x2AF0`** | **256x128** | **character portrait** |
| 11120 / 11172 / 11248 | `0x2B70`+ | 64x64 | small sprites: portrait frame, bar art, flash |
| 12288 | `0x3000` | 512x64 | roster strip |

## The game's own HUD files — FOUND

The HUD is **one `.pak` per player**, and the item table inside is parseable:

```
data/DATA/PZS3US1/Cui_1p.pak      647424 bytes   42 textures   <- HUD player 1
data/DATA/PZS3US1/Cui_2p.pak      647424 bytes   42 textures   <- HUD player 2
data/DATA/PZS3US1/Cui_1p_dmg.pak  647424 bytes                <- 1P damage variant
data/DATA/PZS3US1/Cui_2p_dmg.pak  647424 bytes                <- 2P damage variant
data/DATA/PZS3US1/Cui_eff.pak     538880 bytes  182 textures  <- HUD effects
```

`Cui_1p` and `Cui_2p` are the same size and have **identical dimensions item-for-item (42/42)** but
**different bytes**. The working read is that they are the same art mirrored — item `[4]` is the
256x128 portrait and P2's is P1's flipped for the opposite screen edge. Not verified by dumping
the pixels, and it does not block anything: a compositor can treat the two files as one asset plus
a flip flag.

The `_dmg` variants are the **damage flash** set — the same family that shows at both bar ends in a
stretched layout.

### The item table format

From `MatrixDJ96/DBZBT3` (a BT3 modding-tool repo). Its CTE-Plugin finds textures by a 12-word
header signature:

```
qrs_matrix[] = { 0, 0, 81, 0, 0, 0, 82, 0, 0, 0, 83, 0 }
```

A header is a run of 12 u32 where words **2, 6, 10** hold the markers `81, 82, 83`, words **4** and
**5** are `width/2` and `height/2`, and the rest are 0. The payload starts 48 bytes in: `BPP=8`
indexed, then the CLUT after `width*height` bytes. The markers are the "QRS" the plugin is named
for.

Scanning `Cui_1p.pak` with that signature gives all 42 items. The two `256x64` items and the one
`256x128` match the live-measured VRAM textures exactly:

| item | offset | dims | maps to |
| --- | --- | --- | --- |
| `[4]` | 385600 | **256x128** | the portrait (`tbp0=10992`) |
| `[8]` | 427200 | **256x64** | a bar fill (`tbp0=10880`) |
| `[24]` | 491200 | **256x64** | the other bar fill |
| `[5,7,9,11,…]` | — | 16x4 | **16-color CLUTs** — the per-bar tints |

The `16x4` items with a 64-byte palette are the palettes. **That is the `key=9fa9` / `key=5d72`
split**: one texture, two CLUTs. The game tints the same bar art to read as health vs ki.

### Not found: the timer

No non-per-player `.pak`/`.cpak` contains a `512x64`/`256x64` item that reads as the timer plaque.
Ruled out by name: `Battle_US.pak` (11 such items, but they are battle effects), `DataCenter_*`,
`Evolution_Z_*`, and the per-character `*_3p`/`*_4p` sets (those are 3P/4P arenas, not the HUD).
Most likely inside `Cui_eff.pak` (182 textures) or assembled from the `16x4` CLUT items — unresolved.

## The three elements are NOT separable by VRAM address

- Health and ki are **the same draw, split in two**. Same `tbp0=10880`, same texture, and the two
  halves move together — the right half's *width* changes to drain:

  ```
  box=(124,16)-(216,23)  key=5d72    <- left half, width fixed
  box=(216,16)-(284,23)  key=5d72    <- right half, width changes (284->278->276->274->272...)
  ```

  There is no way to grab "the health bar" without dragging the ki geometry with it.
- Roster and fight use the **same** `tbp0` values. The address does not tell them apart.

So a draw-level element is identified by **(x band, palette)**:

| element | x band (512-space) | source |
| --- | --- | --- |
| HUD P1 (portrait + health + ki) | `x <= 124` | `wsMapX` seam `s1` |
| timer plaque | `216..296` | `wsMapX` seams `s2..s3` |
| HUD P2 | `x >= 388` | `wsMapX` seam `s4` |
| stretch bridge | `124..216`, `296..388` | stretches between the clusters |

Those seams are the game's own authored layout — `wsMapX` in `ps2_gs_gpu_renderer.cpp` already
depends on them for the stretch layout, so they are not our invention.

## Why the pack broke the drain — and the two fixes

**Fix 1 (`22d45a8`) — the gate was switched off entirely.** `g_texAlphaBinary` is a census of the
alpha channel of the *uploaded* texture, and a pack file is an upscale, so its edges interpolate and
the census returns non-binary. That flag is a precondition of the DATE emulation on both of its
readers, so a pack replacement silently turned the destination-alpha test OFF for the bar fill.

**Fix 2 (`ffd49e7`) — the pack flattened the mask.** The drain itself is a plain alpha test, not
the gate:

```
at=1  atst=7  aref=0        <- ATE, NOTEQUAL vs 0: discard where texture alpha is zero
```

The native texture carries the bar's *shape* in its alpha. This pack's replacement is flat:

```
8b0b45d6ddd41afd-23d9ffa5dd6d4ce4-000061d4.dds   512x1024  DXT5
   a=128   262158 texels   100.00%
   a=0            0 texels     0.00%
```

Not one clear texel in the file. With no zero left, `alpha != 0` discards nothing and the bar
renders solid. No blend, gate or shader change recovers this — the data the test reads is not
there. `PS2X_TEXMASK=1` (on by default) keeps the native texture for that case, and is deliberately
narrower than "skip all gate assets": a replacement that still has both levels keeps its art.

## The open artifact: the damage flash

`Cui_1p_dmg.pak` / `Cui_2p_dmg.pak` are the game's own damage-flash set, and upstream PCSX2 patches
the same thing. `GSHwHack::OI_DBZBTGames`:

```cpp
if (!((r.m_r == GSVector4i(0,0,16,16)).alltrue() ||
      (r.m_r == GSVector4i(0,0,64,64)).alltrue())) return true;   // only 16x16 / 64x64
r.SwSpriteRender();      // force the software raster
return false;            // skip the hardware draw
```

It classifies by **texture size, not identity** — the same 64x64 HUD sprites we measure
(11120/11172/11248). This is the flash at both bar ends, and it is a *different* problem from the
drain. Not yet implemented here.

An older `GSC_DBZBT3` CRC hack (removed 2022) named VRAM addresses for the ghosting/blur effect,
not the HUD: `FBP 0x03400`/`0x02e00`, `TBP0 0x03f00`, `PSMCT32`.

## Dead ends — do not re-tread

- **`g_bt3FightPhase` does not separate roster from a duel.** Measured: non-zero at
  `0x27 CHARACTER_SELECT` on the roster *and* in a fight. Not a state discriminator. (This is also
  why the roster squeeze gate built on it failed.)
- **Deleting HUD draws to isolate them corrupts the sky.** Some draws reaching into the HUD band
  are full-height scenery passes. An earlier isolation filter tested the *top edge* (`y1 < 96`) so
  a quad spanning `y=-300..90` passed and removing it took the sky with it. Filter on **height**,
  never on the top edge — and prefer translating a draw off-screen over dropping it.
- **`Batt.unk` is not in this install**, and no `.unk` exists anywhere in the extracted AFS. The
  name from the modding community does not match this build; the HUD is the `Cui_*.pak` set.

## Tooling

| env | what it does |
| --- | --- |
| `PS2X_HUDCENSUS=1` | full-width census: one line per distinct (tbp0, dims, band, key) for HUD-band draws |
| `PS2X_UNKREAD=1` | every AFS read whose slot name ends in `.unk` (nothing matches in this install) |
| `PS2X_TEXMASK=0` | disable the lost-mask fallback |
| `PS2X_TEXGATE=0` | disable the gate-asset binary-alpha override |
| `PS2X_BARBOX=1` | full state of every draw in the P1 bar rect, as a census |

`[barbox]` and `[hudcensus]` are censuses (distinct geometry once), not per-draw. Per-draw printing
spent the whole 3000-line cap on the load fade and the fight logged nothing — a full-screen fade
quad crosses the bar rect on every frame, and the roster's animated text re-signs constantly.
