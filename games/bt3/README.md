# Dragon Ball Z: Budokai Tenkaichi 3 — PC port (PS2Recomp)

A statically recompiled PC port of *Dragon Ball Z: Budokai Tenkaichi 3* (PS2, USA,
SLUS-21678), built on [PS2Recomp](https://github.com/ran-j/PS2Recomp). The game's
EE binary is translated to C++ at build time **from your own disc image** — this
repository contains no game code, assets, or media.

## Requirements

- Your own legally obtained BT3 USA ISO (SLUS-21678). Other regions are not supported
  (the committed function map is for the USA ELF).
- Linux, x86-64 CPU with SSE4.1, ~16 GB RAM and ~10 GB free disk for the build.
- `cmake`, a C++20 compiler, `python3`, `rsync`, `bsdtar` or `7z`, pkg-config,
  the FFmpeg development libraries, and (to build the raylib backend) the
  X11/OpenGL development headers.

  Debian/Ubuntu:
  ```sh
  sudo apt install build-essential cmake git python3 rsync libarchive-tools \
      pkg-config libavcodec-dev libavformat-dev libavutil-dev \
      libswresample-dev libswscale-dev xorg-dev libgl1-mesa-dev
  ```
  Arch and derivatives:
  ```sh
  sudo pacman -S --needed base-devel cmake git python rsync libarchive ffmpeg
  ```

## Build

```sh
./games/bt3/setup.sh /path/to/bt3-usa.iso
```

This extracts and verifies the ELF, builds the recompiler, generates ~7,800 C++
sources from the ELF (using `functions.csv`, the committed Ghidra-derived function
map), applies `apply_patches.py`, and builds `ps2EntryRunner`. The final compile is
the long step (~10–20 min). The script prints the run command when it finishes.

## Status

Playable: boots to title, menus, and fights render in the GPU path at close to the
engine's 30 fps cap. Known issues: stray textured triangle popups in arenas
(render-to-texture pass mismatch), shadow blending differences, some stage-texture
glitches in GPU mode, occasional arm-pose flip during ki charge.

Useful environment switches (all optional — the validated playing configuration is
built in as defaults): `PS2X_GPU=0` falls back to the software rasterizer,
`PS2X_DOFZFAR=<z>` tunes the depth-of-field reach (default 200000), and
`PS2X_NODEFAULTS=1` starts the bare engine with no defaults (debugging); any
individual `PS2X_*` flag can still be overridden or `=0`-disabled.
`PS2X_ASYNC_KICK` runs the VIF/VU1 and GIF work on a worker thread so the guest, the
geometry pipeline and the GS overlap the way they did on real hardware. **On by default
since 2026-09-07**; `=0` opts out. It used to be off because it added 80–157 ms hitch
frames, which is fixed — the guest was seeing every DMA complete instantly and running
ahead of the work it had queued. Measured on an i5-12400: 22.7 → 29.9 fps in a 1P fight,
16.8 → 22.6 in splitscreen.
⚠ If you set `PS2X_ASYNC_KICK=0` in your environment while testing, DELETE the variable to
get the new default — blanking it is not enough, an empty value still counts as set.

## How the port stays reproducible

- Recompiler bug fixes (EE FPU semantics, VU0 macro ops, SQRT operand decoding, …)
  live in `ps2xRecomp/` and apply during generation.
- Game-specific runtime fixes (camera-matrix stub, HLE acosf, demo-crash guards,
  sound/pad compatibility) live in `ps2xRuntime/` behind a game descriptor.
- `games/bt3/functions.csv` is the function map; `games/bt3/apply_patches.py` holds
  the few source-level patches that must live inside generated code.
- Generated sources are never committed.

## Credits

- [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) — the recompiler and runtime
  this port is built on (GPL-3.0, as is this repository).
- Spike Chunsoft / Bandai Namco — the game. This project is not affiliated with or
  endorsed by them; it exists for preservation and interoperability. No game content
  is distributed.
