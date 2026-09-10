# BT3-Recomp — Dragon Ball Z: Budokai Tenkaichi 3 on PC

A statically recompiled, native PC port of *Dragon Ball Z: Budokai Tenkaichi 3*
(PS2, USA, SLUS-21678), built on [PS2Recomp](https://github.com/ran-j/PS2Recomp).
The game's MIPS code is translated to C++ **at build time, from your own disc
image** — this repository contains no game code, assets, or media.

> This is not an emulator: the game's executable and its gameplay overlay are
> recompiled into a native, portable game tree with an OpenGL renderer. Ships
> with a Qt 6 launcher (GLFW gamepad support) for Linux, Windows and macOS.

## Requirements

- **Your own legally obtained BT3 USA ISO** (SLUS-21678). Other regions are not
  supported — the committed function maps are for the USA executable.
- Linux, Windows or macOS (Windows/macOS experimental), x86-64 CPU with SSE4.1.
  On macOS the build is native arm64 (Apple Silicon) or x86-64, one at a time;
  see [the port notes](docs/MACOS-PORT.md).
- ~16 GB RAM and ~10 GB free disk for the build.
- Packages: `cmake`, GCC or Clang with C++20, `python3`, `rsync`,
  `bsdtar` (libarchive) or `7z`, pkg-config, the FFmpeg development libraries,
  and the X11/OpenGL development headers (raylib builds from source).

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

## Build + deploy — one command

**Linux** (build + assemble the portable game tree):

```sh
git clone https://github.com/z3xox/BT3-Recomp.git
cd BT3-Recomp
./build_and_deploy.sh --iso /path/to/your/bt3-usa.iso --output /path/where/deploy
```

The script asks for the ISO and output directory if they are not given, runs the
full `setup.py` pipeline, bundles the runner + its shared libraries into the
deploy tree, builds a Qt 6 launcher (GLFW gamepad support), and drops
`install game.sh` for the desktop-integration step. Pass `--skip-setup` to
reuse an existing `games/bt3/work/` tree and only rebuild the runner.
`tools/release/package.sh` then wraps everything into the single release
artifact. See `docs/DEPLOY.md` for the full picture.

**Windows (experimental):** install [Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
(the "Desktop development with C++" workload, which includes CMake, plus its optional
components "C++ Clang Compiler for Windows" and "MSBuild support for LLVM (clang-cl) toolset":
the build needs Clang, MSVC cannot compile the generated VU1 code) and Python 3;
`tar` for ISO extraction ships with Windows 10+. Then, from a regular terminal:

```
git clone https://github.com/z3xox/BT3-Recomp.git
cd BT3-Recomp
python games\bt3\setup.py C:\path\to\bt3-usa.iso --deploy C:\path\where\deploy
```

12 GB+ RAM recommended on Windows; the Windows build is young — expect rough
edges and please report issues. The output is the same portable tree
(`Launcher.exe`, `bt3-runner.exe`, `data/`, …), zipped for distribution.

The pipeline extracts and sha256-verifies the game files from your ISO, builds the
recompiler, generates ~7,800 C++ sources from the game's executable and overlay,
applies the committed patches, and builds the final binary. The compile is quick
on a modern machine (a few minutes at `-j16`); the conservative default is `-j3` —
pass your core count with `--jobs N` if you have 8 GB+ of free RAM.

**macOS (experimental):** install the Xcode Command Line Tools and Homebrew dependencies:

```sh
brew install cmake ninja pkg-config ffmpeg qt
./build_and_deploy_macos.sh --iso /path/to/bt3-usa.iso --jobs 3
open build/macos-dist/BT3-Recomp.app
```

The app's installation wizard reads your USA ISO. Game files, settings and saves
live in `~/Library/Application Support/BT3-Recomp/`, outside the signed bundle.
The script defaults to the build Mac's OS version as its minimum and checks the
bundled libraries against it. Homebrew bottles can require a recent macOS release.
The signature is local/ad-hoc; Developer ID signing and notarization are not included.
For development without a bundle, use `python3 games/bt3/setup.py /path/to/bt3-usa.iso --jobs 3`.
See [deployment details](docs/DEPLOY.md#macos-app-experimental) for rebuilds and limitations.

## Run

The setup script prints the exact command when it finishes.

**Linux portable (release):** unpack the archive and launch from the folder:

```sh
cd "Dragon Ball Budokai Tenkaichi 3 Recompiled"
./Launcher
```

or run `install game.sh` for a desktop menu entry + icon. The launcher boots
`bt3-runner` with the extracted `data/SLUS_216.78` and the bundled `lib/`
automatically.

**Windows** (`cmd.exe`, release): open the extracted folder and run
`Launcher.exe`; it starts `bt3-runner.exe` with the game data.

For raw runner runs (no launcher):

```
cd build\ps2xRuntime\Release
set PS2X_CD_IMAGE=C:\path\to\your\bt3-usa.iso
bt3-runner.exe ..\..\..\games\bt3\work\SLUS_216.78
```

Gamepads are supported (GLFW mappings; tested with an 8BitDo pad — the launcher
and the runner read the same mapping database).

## Status

Playable: boots through logos and title, menus work, and fights render in the
GPU path at close to the engine's 30 fps cap.

Known issues:
- stray textured triangle popups in arenas (render-to-texture pass mismatch)
- shadow blending differences and some stage-texture glitches in GPU mode
- occasional arm-pose flip during ki charge
- FMVs are skipped

## Repository layout

| Path | What it is |
| --- | --- |
| `build_and_deploy.sh` | Linux one-command build + deploy (ISO prompt, portable game tree) |
| `tools/release/package.sh` | wraps the deploy tree into the release tarball (`BT3-Recomp-x86_64.tar.gz` + `.sha256`) |
| `games/bt3/setup.py` | cross-platform build pipeline (`--deploy`, `--skip-setup`, `--jobs`) |
| `docs/DEPLOY.md` | the deploy structure and cross-platform packaging documentation |
| `games/bt3/functions.csv`, `dbzp_*.csv` | function address maps (symbols only) |
| `games/bt3/vu1_programs.json` | ELF offsets + hashes of the VU1 microprograms (the translation is generated from your ELF at setup) |
| `games/bt3/gen_overlay.py`, `apply_patches.py` | generators for the game-specific pieces |
| `ps2xRecomp/` | the static recompiler (with EE FPU/VU semantics fixes) |
| `ps2xRuntime/` | runtime: memory, GS/GPU renderer, VU1, scheduler, game overrides |
| `PS2Recomp-README.md` | the upstream PS2Recomp documentation |

## Credits & license

- Built on [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) — thank you!
  Licensed GPL-3.0, as is this repository (see `LICENSE`).
- NTSC-U AFS file lists (`PZS3US1.AFL`/`PZS3US2.AFL`) by
  [ViveTheModder](https://github.com/ViveTheModder/vivethemodder.github.io),
  distributed under the Apache License 2.0 (see `ps2xRuntime/src/launcher/assets/NOTICE`).
- *Dragon Ball Z: Budokai Tenkaichi 3* © Spike / Bandai Namco. This project is
  not affiliated with or endorsed by them; it exists for preservation and
  interoperability, and distributes no game content.

## Experimental: paraLLEl-GS backend (branch `parallel-gs`)

This branch carries a second graphics backend: the PS2 GS emulated in Vulkan compute by
[paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) (Arntzen Software, LGPL-3.0-or-later), fed with the
same GIF packet stream our OpenGL renderer consumes. It is VRAM-exact, so the whole family of render-target aliasing
workarounds in the GL renderer is unnecessary on this path.

Status (2026-09-10): logos, movies, menus and fights (1P and splitscreen) render correctly in exclusive mode (our own
GS parse off). Measured on a 9800X3D + RTX 5080 at 1x, per game frame at 30 fps: 1P fight 2.8 ms CPU + 2.9 ms GPU
(GL path: 5.5 ms + 7.5 ms); splitscreen 5.0 ms CPU + 4.4 ms GPU; splitscreen at 16x supersampling 5.9 ms CPU +
8.9 ms GPU. Presentation still crosses Vulkan -> CPU -> OpenGL through a fenced readback ring (0.06 ms, one frame of
latency); texture packs and the Windows build are not wired up on this path yet.

Build (Linux; the checkout is a git submodule with its own Granite submodule):

```
git submodule update --init --recursive
cmake -S . -B build_pgs -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build_pgs ps2EntryRunner
```

CMake picks the backend up automatically when `ps2xRuntime/third_party/parallel-gs/CMakeLists.txt` exists
(`-DPS2X_DISABLE_PGS=ON` to leave it out). The backend is the DEFAULT renderer when built in: the overlay's
Renderer dropdown (`[video] renderer=` 0 OpenGL, 1 software, 2 paraLLEl-GS, takes effect on restart) selects it, and the
runtime falls back to OpenGL by itself if Vulkan is unavailable. Environment overrides still work (`PS2X_PGS=0/1`,
`PS2X_PGS_EXCLUSIVE=1`). Knobs: `PS2X_PGS_SSAA=1|2|4|8|16`
(4 = render-scale-2 geometry, 16 = render-scale-4), `PS2X_PGS_HIRES=0|1|2` (scanout 1x/2x/4x; unset = fit the window; 2x needs SSAA 4, 4x needs 16), `PS2X_PGS_PRESENTMIP=0` (no mipmapped downscale at the present), `PS2X_PGS_COALESCE=1`,
`PS2X_PGS_TIMESTAMPS=1` (per-stage GPU times in the `[pgs]` log line), `PS2X_PGS_DUMP=<dir>` (presented frames as PNG),
`PS2X_PGS_LIVEFLIP=1`, `PS2X_PGS_SYNCREADBACK=1` / `PS2X_PGS_NOREADBACK=1` (A/B switches). Keyboard input: set the
controller device to Keyboard in the overlay if a gamepad-like device is present (Auto prefers it).

Licence note: paraLLEl-GS is LGPL-3.0-or-later and this repository is GPL-3.0; the combination is distributed under
GPL-3.0. Its licence text is `ps2xRuntime/third_party/parallel-gs/COPYING.LGPLv3`.
