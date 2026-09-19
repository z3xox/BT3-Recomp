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
- Linux, Windows or macOS, x86-64 CPU with SSE4.1 (Windows builds natively via
  `scripts\build-windows.ps1`; macOS experimental). On macOS the build is native
  arm64 (Apple Silicon) or x86-64, one at a time; see [the port notes](docs/MACOS-PORT.md).
- ~16 GB RAM and ~10 GB free disk for the build.
- Linux packages: `cmake`, GCC or Clang with C++20, `python3`, `rsync`,
  `bsdtar` (libarchive) or `7z`, pkg-config, the FFmpeg development libraries,
  and the X11/OpenGL development headers (raylib is vendored in-tree and builds with the runtime).

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

  Native Windows (see `scripts\build-windows.ps1`):
  - VS Build Tools 2022 with the C++ Clang Compiler for Windows
    (`Microsoft.VisualStudio.Component.VC.Llvm.Clang`), installable with
    `winget install -e --id Microsoft.VisualStudio.2022.BuildTools`
  - CMake >= 3.21, Ninja, Python 3, Git for Windows
  - Qt 6.5.3 `win64_msvc2019_64` (downloaded automatically via aqtinstall on
    first run), plus the `aqtinstall` and `pefile` Python packages

## Build + deploy — one command

**Linux** (build + assemble the portable game tree):

```sh
git clone https://github.com/z3xox/BT3-Recomp.git
cd BT3-Recomp
./scripts/build-linux.sh --iso /path/to/your/bt3-usa.iso --output /path/where/deploy
```

The script asks for the ISO and output directory if they are not given, runs the
full `setup.py` pipeline, bundles the runner + its shared libraries into the
deploy tree, builds a Qt 6 launcher (GLFW gamepad support), and drops
`install game.sh` for the desktop-integration step. Pass `--skip-setup` to
reuse an existing `games/bt3/work/` tree and only rebuild the runner. The same
pipeline produces the release artifact (`BT3-Recomp-x86_64.tar.gz` + `.sha256`)
and asks where to send it (`--no-package` assembles the deploy tree only).
See `docs/DEPLOY.md` for the full picture.

The pipeline extracts and sha256-verifies the game files from your ISO, builds the
recompiler, generates ~7,800 C++ sources from the game's executable and overlay,
applies the committed patches, and builds the final binary. The compile is quick
on a modern machine: the job count is auto-sized from CPU/RAM (the conservative
fallback is `-j3`); pass `--jobs N` to force it.

**macOS (experimental):** install the Xcode Command Line Tools and Homebrew dependencies:

```sh
brew install cmake ninja pkg-config ffmpeg qt
./scripts/build-macos.sh --iso /path/to/bt3-usa.iso --jobs 3
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
`bt3-runner` with the extracted `data/SLUS_216.78` and the bundled `assets/lib/`
automatically.

**Windows (native, PowerShell):** the native build runs locally with Visual
Studio Build Tools 2022 (ClangCL + Win11 SDK), Ninja, Python 3 and Qt 6 (fetched
via aqtinstall). Double-click `scripts\build-windows.ps1` in PowerShell
or run it from a terminal; the first run installs all missing prerequisites via
winget and pip. To install only the dependencies, run the standalone script:

```powershell
.\scripts\install-deps-windows.ps1   # install all prerequisites
.\scripts\build-windows.ps1 -Iso "C:\path\to\bt3-usa.iso"
```

Call it without arguments to be prompted for the ISO and output directory. Pass
`-SkipSetup` to reuse an existing `games/bt3/work/` tree (rebuild only). Once the
stage passes the PE gate, package the release zip with:

```powershell
.\scripts\package-windows.ps1
```

Produce `build\release-windows\out\stage\` and
`build\release-windows\out\BT3-Recomp-x86_64.zip` + `.sha256`. The game data is
never shipped: the launcher's install wizard extracts it from your own ISO.

**Windows** (release): open the extracted folder and run `Launcher.exe`; it
starts `bt3-runner.exe` with the game data.

On Windows, `paraLLEl-GS` runs on a bundled Mesa **lavapipe** (software Vulkan)
ICD because several vendor Vulkan drivers (notably the AMD proprietary driver on
Polaris/GCN, `amdvlk64.dll`) access-violate during shader compilation. If the
runner still dies, the launcher automatically retries once with the OpenGL
renderer and records it in `logs\vulkan-fallback.log`. Set `PS2X_VK_NATIVE=1`
to use the system Vulkan driver instead of the bundled lavapipe.

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

## Texture replacement & cache

Textures are identified exactly like PCSX2 (`<TEX0Hash>-<CLUTHash>-<bits>`), so its
existing packs work unchanged. Drop a pack in `<deploy>/data/Textures/` (see
[textures/README.md](textures/README.md)) or set `PS2X_TEXREPLACE=<dir>`.

In the launcher/overlay Video tab, **Texture Replacement…** shows the pack status plus
**Video overlay (4K intro)** and **Buttons style (PS2/Xbox)**. Enabling and installing
live in the launcher's **Misc** tab (**Pack Lite** = 2D only, **Pack Full** = 3D + 2D);
the launcher opens the pack's download page (Open in browser / Copy link) and installs
a locally downloaded archive with **Browse…**.

The **texture cache** (`<deploy>/data/texcache.bin`) stores each texture once it is
fully resolved (PSMT decode + pack replacement applied) so later runs skip the VRAM
hash match, the pack lookup and the PNG/DDS decode — measured ~6x fewer texture
decodes, a CPU/I-O saving (the GPU path is unchanged). It fills as you play and is
rebuilt automatically when the pack, the Texture Replacement toggle or the button
layout changes. Toggle it in **Misc → Texture Cache** (`[video] texcache`); delete it
there (or set `PS2X_TEXCACHE_REGEN=1`) to force a rebuild.

## Repository layout

| Path | What it is |
| --- | --- |
| `scripts/build-linux.sh` | Linux build + package wrapper around `setup.py` (ISO prompt, portable tree, tar.gz + sha256) |
| `scripts/build-windows.ps1` | Windows native build + package wrapper (installs missing deps, builds runner + Qt launcher, PE gate, zip) |
| `scripts/install-deps-windows.ps1` | Windows dependency installer (VS Build Tools + ClangCL, CMake, Ninja, Python, Qt, Mesa lavapipe) |
| `scripts/package-windows.ps1` | Windows release packaging from the native stage (`BT3-Recomp-x86_64.zip` + `.sha256`) |
| `games/bt3/setup.py` | the single four-stage script: detect / deps / build / package (see `docs/DEPLOY.md`) |
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
