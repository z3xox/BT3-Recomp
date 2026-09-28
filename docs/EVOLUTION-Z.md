# EVOLUTION Z — the room renders clipped and the character never appears

Screen: **Main Menu → Evolution Z**, BT3 top-level state **`0x30`** (row 5 of the
main menu, handler at `0x3365B0` — see `docs/MAIN-MENU.md`).

## Symptom

Three things are wrong at once, and they are one bug:

1. The pill labels are cut off mid-word.
2. The help text is cut off.
3. The character sprite is not on screen at all.

It has always done this. It also does it in 4:3, so it is not a window or
aspect-ratio problem. Only this menu is affected.

PCSX2 renders the screen correctly, so the game data and the GS state are fine.

## The conclusion

**We produced the same dump PCSX2 produces, and the menu's textures are not in it.**

`PS2X_TEXPNGD` writes the decoded RGBA of every texture the game actually samples,
named by its PCSX2-compatible identity — the same name PCSX2's own dumper emits. Run
over a full visit to this screen it is a like-for-like census of what the game
touched. PCSX2's dump of the same screen contains assets ours does not.

So the game is **not requesting these textures at all** in this build. Nothing is
being decoded wrong, nothing is being culled: the requests never happen.

That moves the bug out of the texture path entirely. The texture path is verified
correct where it can be checked (see below).

## The reference set was wrong — Krillin is not this menu

The 256x512 `a91b6817d8b91110-ec5261822b80e0e0-00002613` (Krillin) is **not an
Evolution Z asset**. PCSX2's entire dump folder for this title contains exactly two
256x512 files:

```
a91b6817d8b91110-ec5261822b80e0e0-00002613.png   Krillin
f3500a995d44e203-1d34f2349553c2af-00002613.png   Launch
```

Both carry the same vertical white bar on the left, so they are the same sprite
format. That slot is the **shared character-sprite slot**, reused across screens.
In our run this screen decodes Broly and Goku in it:

```
a607221a3719a9d2-7a591ccc1e9c31c5-00002613   Broly
e42d7096d996d3f2-c519c75a976a2dc9-00002613   Goku
ecac4e5eedfb7c45-3ec0a55b6e2b1a37-00002613   Goku
```

Measured with `PS2X_WATCHSIZE=256x512`, which reports **every** decode of that size
undeduplicated. The character is fixed on this screen, not a carousel, so these
three are the whole set for a normal visit. Chasing Krillin's identity here was a
wrong turn and cost several runs.

## What the menu actually loads

With the dump flushed at the `0x30` transition (`PS2X_EVOFLUSH`), this screen's
complete asset set is **44 textures**:

| count | dims |
|---|---|
| 8 | 128x256 — the room backdrops |
| 6 | 64x64 |
| 6 | 512x64 |
| 6 | 512x128 |
| 4 | 256x128 |
| 4 | 128x128 |
| 3 | 32x32 |
| 3 | 128x64 |
| 2 | 256x256 |
| 1 | 512x256 |
| 1 | 256x64 |

**No 256x512 in this set.** The character sprite the room should show is not among
the 44 the game loads here.

## The content that IS loaded decodes correctly

Two of the 512x128 assets that go to the visible target produce a **byte-identical
filename** to PCSX2's dump:

```
a62c48fbe3d29824-ebe0286214ff0ceb-00001e53   512x128   MATCH
f46ea52a48e0b374-5544e75444e3efa6-00001e53   512x128   MATCH
```

Same dimensions, same `bits`, same `TEX0Hash` **and** same `CLUTHash`. Our VRAM
layout, block order, swizzle handling and CLUT hashing all agree with PCSX2 for
those assets. The texture path is not the bug.

The 256x512 character slot also decodes cleanly when it is used elsewhere in the
game — Broly, Goku and an item-grid sheet all dumped as correct, uncorrupted RGBA.

## The assets are not in the obvious pack

`Evolution_Z_US.pak` (3,466,880 B, 110 indexed textures, CTE `qrs_matrix` layout
with 48-byte headers and the CLUT addressed separately) does not hold the art on
screen:

- it has **no 256x512 texture at all**
- a CLUT cross-match of every identifiable ref against its 110 items scores
  **Jaccard ≈ 0.00**

`Map_32_PS.pak` item 58 is the only 256x512 a dimension sweep finds across
`DATA/PZS3US1`, and it decodes to a sky/ground backdrop, not a character.

The room is drawn from **shared map assets**, not from a menu-specific pack.

## Nothing is being culled

`PS2X_OVERLAY=1` logs every draw in order, 20,000 draws for this screen:

| | |
|---|---|
| `FRAME.FBW` | 8 (→ **512 px** wide target), PSM CT32 |
| scissor | `(0,0)-(511,447)` — the full visible window |
| `TEST` | `0x00000000` on every draw — **ATE=0, ZTE=0** |
| `fbp` split | 10,014 to `fbp=112`, 9,986 to `fbp=0` |
| destination x | **1413 … 2304** |
| destination y | **1581 … 2270** |
| draws with x < 512 | **0 of 20,000** |

No alpha test, no depth test, no rejection of any kind. Every draw of this screen
targets coordinates outside the 512-wide target, while the presented window is
512x448 from `srcY=64`.

The draw for the labels:

```
#01795 fbp=0 FBW=8 psm=0 512x128 dst=(1463,2009)+(128x454) prim=6 abe=1 test=00000000
```

## Open question

`fbp=0` and `fbp=112` with ~10,000 draws each, all at coordinates in the 1400-2300
range, does not look like screen drawing. It looks like an **offscreen pass**. The
draw that reduces that to 512x448 is not among the 20,000.

The present reads `tex=12 512x512 src=(0,64 512x448)` — `tex=12` is fbp 12/32, a
target **neither** of the menu's two fbps writes to. So the one question left is:

> **Which fbp does the present read, and which fbp is the menu supposed to compose
> into?**

Untested. Everything underneath it is measured.

## Dead ends — do not re-tread

| tried | result |
|---|---|
| `PS2X_ZTESTOFF=1` (depth test off on every draw) | no change — not losing content to depth |
| `PS2X_SKIPBG=512x512` | matched **zero** draws; the backdrops are 128x256 |
| `PS2X_SKIPBG=512x256` | removes the room, reveals the menu behind — still clipped |
| `PS2X_GSMASK=1` (`X & (FBW*64-1)`, the GS coordinate truncation) | **broke the whole game and did not fix the menu.** Rules out "the coordinates need wrapping" |
| forcing `g_dispFboW` to 1024 | no change. Also not a valid A/B: that is *our* display FBO, not the game's target |
| texture-width, FBO-narrow, present-crop, FBO-size probes | all correct, ruled out earlier |
| `PS2X_SKIPHASH` on the 9 backdrops | works — empties the room, reveals the menu behind it. Confirms the backdrops are the occluder, and that what is behind is still clipped |
| menu-jump (`PS2X_MENU_JUMP=48 PS2X_MENU_AUTO=1`) | forces state from outside; the game's own transition never runs, so there is no clean boundary to flush on |
| `PS2X_TEXMEGA` via F9 | dead — the key was read on the wrong side. A working F9 exists in `ps2_runtime.cpp` (the RAM dump) |
| `PS2X_FILECENSUS` on disk | the game does read `Evolution_Z_US.pak` and `Evolution_Z_00/01_US.adx`; the art is not in them |
| `sceSifSetDma:DTX_MISS_DUMP` | dead end: DTX is CRI's **audio** driver, destinations are small RAM addresses |
| rebuilding the PSMT8/PSMCT32 swizzle in Python | two wrong attempts (`pagesPerRow` from pixel width instead of TEX1.TBW; `page << 13` written as `(page>>5) << 13`). Unnecessary — the project's own decoder already dumps these correctly and `identify()` names them |
| a raw 4 MB VRAM blob (`PS2X_VRAMDUMP`) | removed at the user's request; a flat dump is not legible anyway, PSMT8 needs its CLUT |

## Tooling added (uncommitted)

All diagnostics, deliberately **not** committed.

| flag | what it does |
|---|---|
| `PS2X_TEXPNGD=<dir>` | write the decoded RGBA of every sampled texture as PNG, named by the PCSX2 identity. Hooked in `decodeTexRGBA`, the one point **both** decode paths go through — `texmega` only sees the deferred path and produced 96 PNGs without the sprite |
| `PS2X_TEXPNGD_SIZE=<WxH\|*>` | restrict the dump to one texture size |
| `PS2X_TEXPNGD_MAX=<n>` | optional cap. **Default 0 = unlimited.** An earlier hardcoded cap of 400 silently truncated the census and made "only 143 textures" look like a finding |
| **F9** | re-arms `PS2X_TEXPNGD`: wipes the directory and forgets what was written, so the folder holds exactly one screen. Read in `ps2_runtime.cpp` on the per-frame tick, where the RAM dump's F9 already works |
| `PS2X_WATCHSIZE=<WxH>` | report **every** decode of that size, undeduplicated, with its identity |
| `PS2X_EVOFLUSH=1` + `PS2X_EVOFLUSH_STATE=<hex>` | reset the dump on entering a screen, so it holds only that screen |
| `PS2X_OVERLAY=1` + `PS2X_OVERLAY_STATE=<hex>` | **ordered** per-draw log: identity, `fbp`, `FBW`, PSM, dest rect, `TEST`, full ALPHA blend, scissor, `FBMASK` |
| `PS2X_REGIONLOG=1` + `PS2X_REGIONLOG_STATE=<hex>` | per-draw name in PCSX2's **region** form (`-r<W>x<H>`) |
| `PS2X_SKIPHASH=<n,n,…>` + `PS2X_SKIPHASH_STATE=<hex>` | drop draws by texture identity, on one screen only. Accepts the plain and the region name form |
| `PS2X_TEXWATCH=<file>` | cross-check identities against PCSX2 dump names → `MATCH` / `DIFF` / `NEVER`, with an atexit summary |
| `PS2X_TEXNAME=1` | print the PCSX2-compatible identity of every decoded texture |
| `PS2X_CELRECT=<WxH\|*>` | per-draw rect, quad corners, UV, TEXA, FBMASK |
| `PS2X_SKIPBG=<WxH>` | drop draws by texture size (guesswork; superseded by `SKIPHASH`) |
| `PS2X_ZTESTOFF=1` | depth test off everywhere (A/B, negative result) |
| `g_bt3MenuState` | current BT3 screen, published by the `[bt3state]` probe for the state-gated probes. Defined in `game_overrides.cpp`, written in `ps2_runtime.cpp` |

## Confirmed background assets

Identified by eye, skippable by name with `PS2X_SKIPHASH` + `PS2X_SKIPHASH_STATE=0x30`:

```
eef58894b08d9c88-d730c26dbdbc3407-000021d3   128x256
dbf419abb235ad61-d730c26dbdbc3407-000021d3   128x256
cf4a1fb1d43694d0-d730c26dbdbc3407-000021d3   128x256
863fb9879d339fa-d730c26dbdbc3407-000021d3   128x256
306b33e0de6a4b80-d730c26dbdbc3407-000021d3   128x256
82a5e25afd440922-d730c26dbdbc3407-000021d3   128x256
9a9c58b6aa8cfe40-d730c26dbdbc3407-000021d3   128x256
5cedd3905327a026-d730c26dbdbc3407-000021d3   128x256
dc51eaa65d919bd6-349f90098f14be09-00002213   256x256
```

Skipping these empties the room and shows the menu's own sprites behind it — still
clipped, which is the rest of the bug.

## The two 512x128 label textures

Confirmed present, confirmed byte-identical to PCSX2, confirmed visibly clipped.
These are the right thing to track: they are the only assets with all three facts
established at once (called, decoded correctly, rendered wrong). Krillin could not
serve as an anchor because he is never called here at all.

```
a62c48fbe3d29824-ebe0286214ff0ceb-00001e53
f46ea52a48e0b374-5544e75444e3efa6-00001e53
```

## How PCSX2 names a texture

From `pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp:33-38`:

```c
#define TEXTURE_FILENAME_FORMAT_STRING              "%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_CLUT_FORMAT_STRING         "%" PRIx64 "-%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_REGION_FORMAT_STRING       "%" PRIx64 "-r%ux%u-%08x"
#define TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING  "%" PRIx64 "-%" PRIx64 "-r%ux%u-%08x"
```

`{TEX0Hash}-{CLUTHash}-{bits}`, and `-r<W>x<H>` in the middle when the draw samples
a sub-rect. `bits` is a packed integer, not a hash:

```
psm = bits & 0x3F     tw = (bits >> 6) & 0xF     th = (bits >> 10) & 0xF
```

Verified against the dumps: `0x00002613` → psm 19 (PSMT8), 256x512.
`0x00001d3` → psm 19, 128x256.

The name is **content identity** — the same texels always produce the same name.
That is the only handle precise enough to name one draw, and the basis of the
`PS2X_SKIPHASH` work.

Our `ps2tex::TexIdent::name()` emits the same shape, so our identities and
PCSX2's dumps are comparable by filename.
