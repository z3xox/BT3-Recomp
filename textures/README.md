# Texture replacement

The runtime loads packs from **`data/Textures`** (the deploy's `data/` folder, next to the
extracted ISO tree). Drop a **PCSX2 texture pack** there and enable **Texture Replacement** in the
in-game overlay (Select+Start, or LShift+Tab → Video).

This repo folder, `textures/`, is only the staging source: at build time CMake copies its
contents into `<runner dir>/data/Textures`. To keep packs elsewhere at runtime, use
`PS2X_TEXREPLACE=<dir>`.

## It is PCSX2-compatible on purpose

Textures are identified exactly the way PCSX2 identifies them, so its existing packs work
unchanged — no conversion, no renaming:

    <TEX0Hash>-<CLUTHash>-<bits>.png

Both hashes are XXH3-64: the first over the raw texture data in GS block order, the second over
the palette. Verified against real PCSX2 dumps of this game — filenames come out byte-identical.

## Layout

Anything inside the pack folder is found, **at any depth**. You can unpack an archive there without
flattening it, and a pack that ships as `SLUS-xxxxx/replacements/*.png` works as-is.
PCSX2 searches its own replacements folder recursively too.

Higher-resolution replacements need no special handling: the renderer samples with normalised
texture coordinates, so a 4x or 8x texture drops straight in.

## Alpha range: the one thing that silently ruins a pack

The PS2 has no 0..1 alpha. Its alpha is a **byte where 128 is fully opaque**, which a PC reads
as 50%:

> *"Pixels that have 128 (50%) opacity were actually fully opaque on PS2, pixels that are 102
> (40%) opacity were 80% on the PS2, 64 (25%) is 50%."* — the MGS2 texture-replacement notes

So a replacement has to decide which convention it is in, and **the runtime reads it off the
file rather than assuming**: it measures the replacement's real alpha maximum and derives the
rescale from it. A pack whose opaque is 128 gets ×1.99; a full-range pack whose opaque is 255
gets ×1.0. Both come out right, with no setting to flip.

**For pack authors**, the practical consequence is the reverse one: a pack upscaled by a model
that rewrites the alpha lands in full range where a PS2 pack would not, and that is the case
that historically went wrong here. PCSX2 ships the same idea as a tool
(`tools/texture_dump_alpha_scaler.py`, `scale` / `unscale`), and warns:

> *"Not unscaling after editing may result in broken rendering!"*

If a pack comes out looking uniformly **translucent**, that is an alpha-range mismatch, not a
sampling bug. Check the maximum alpha in the file before assuming the renderer is at fault.

One more knob sits in this area and is *not* about replacements: `PS2X_ABLEND128` (0 by
default) leaves every texture-alpha blend at half GS strength, because `aBlend` wants
`GS_As/128` while texture alpha arrives as `GSbyte/255`. Its modes were tuned per CLUT family
because turning it on globally blows out the additive classes, so it is left off rather than
made a blanket default.

## Notes

- The toggle applies **live** — it flushes the texture cache so the change is immediate.
- Coverage is whatever the pack has. A pack with 1,500 textures will not cover every surface in
  every fight, so expect some upgraded and some untouched.
- Replacements cost VRAM: a 4x texture is 16x the memory of the original, 8x is 64x. Large packs
  on a GPU with little memory to spare are the case to watch.
- `PS2X_TEXREPLACE=<dir>` overrides the default `data/Textures` folder if you keep packs elsewhere.
