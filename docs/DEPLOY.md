# Deploy — portable, cross-platform game tree

For the first time, BT3-Recomp ships as a **portable folder**, not a
self-extracting single-file runtime. The delivered tree is identical on Linux,
Windows and macOS:

```
BT3-Recomp-x86_64.tar.gz          # Linux release payload
BT3-Recomp-x86_64.sha256
```

or the equivalent Windows/macOS ZIP. There is no SELFX stub any more and no
install step is required: unzip the archive, run the launcher.

## Deploy tree

Unpacking `Dragon Ball Budokai Tenkaichi 3 Recompiled/` gives:

```
Dragon Ball Budokai Tenkaichi 3 Recompiled/
├── Launcher                # Qt 6 config UI (Linux/X11; Windows → Launcher.exe)
├── bt3-runner              # the recompiled game (Windows → bt3-runner.exe)
├── install game.sh         # Linux helper: menu entry + desktop icon
├── lib/                    # runner's shared-library closure (Linux)
├── data/
│   ├── SLUS_216.78         # boot ELF (also the CD image's name)
│   ├── BIN/  DATA/  IRX/  SYSTEM.CNF   # game data, extracted from the ISO
├── savedata/
│   ├── bt3_settings.ini    # user settings ([logging], [video], …)
│   └── pad_p1.conf / pad_p2.conf       # launcher bindings
├── assets/                 # launcher artwork, fonts (background.png, icon.png, …)
└── savedata_slot1/         # BASLUS-21678DBZT3 memory-card slot, kept across runs
```

The bundle name is `Dragon Ball Budokai Tenkaichi 3 Recompiled` — matching the
window/taskbar-visible identity and the `.desktop` entry installed on Linux.

## How the pieces get there

Everything is driven by two entry points — one shell script for Linux, one
Python script that hosts the build pipeline (and is the base for Windows).

| Script | Platform | Role |
|---|---|---|
| `build_and_deploy.sh` | Linux | Interactive build + deploy. Asks for the ISO and output dir, runs the full `setup.py` pipeline, bundles the runner's `ldd` library closure into `OUT/lib`, renames the runner to `bt3-runner`, builds the launcher (Qt 6 + GLFW, pulled via FetchContent), copies the release assets and writes `install game.sh`. |
| `games/bt3/setup.py` | Both (Windows experimental) | The build pipeline: extract/verify ISO, build the recompiler, generate ~7,800 runner sources, apply patches, build the runner. Also has `--deploy` to assemble the playable tree. |
| `build_and_deploy_macos.sh` | macOS (experimental) | Interactive build + deploy for macOS: runs the same `setup.py` pipeline, then assembles a self-contained `.app` (relocated dylibs, `Info.plist`, ad-hoc signing) via `tools/macos/deploy.py`. |
| `tools/release/package.sh` | Linux | Wrap a finished deploy tree into `BT3-Recomp-x86_64.tar.gz` + `.sha256`. This is the only artifact that leaves the machine. |

## Build + deploy (Linux)

```sh
./build_and_deploy.sh                          # prompts for ISO + output dir
./build_and_deploy.sh --iso /path/game.iso --output /path/deploy
./build_and_deploy.sh --skip-setup --output /path/deploy   # reuse existing work/, rebuild runner only
```

`--skip-setup` skips ISO extraction and source generation, rebuilding only the
runner from the already-generated sources (fast; needs ccache/sccache warm).
`--jobs N` (env `BT3_JOBS`) sets the runner build parallelism
(default: `nproc`).

`package.sh` then produces the single release artifact (the tarball). Its
integrity is verified by the `.sha256` sibling; release users can also re-verify
forwards with `sha256sum -c`.

## Install (Linux desktop integration)

Inside the unpacked folder, `install game.sh` (copy made from
`tools/release/install-game.sh.in`) does three things:

1. copies the whole game tree to `~/.local/share/bt3-recomp/`,
2. writes a `~/.local/share/bt3-launcher.sh` wrapper,
3. installs `~/.local/share/applications/Dragon-Ball-Budokai-Tenkaichi-3.desktop`
   plus `~/.local/share/icons/bt3.png`, so the game shows in the applications
   menu with its artwork.

Existing `~/.local/share/bt3-recomp/savedata/` is preserved on re-run, so saves
and settings survive reinstallation. The folder itself remains fully portable:
you can skip the install script and run `Launcher` straight from the unpacked
tree.

## Build + deploy (Windows, experimental)

```sh
python games\bt3\setup.py C:\path\game.iso --deploy C:\path\deploy
```

The same pipeline runs; on Windows the runner is copied as `bt3-runner.exe`,
the launcher builds with MSVC/Clang (Qt 6 + GLFW), the DLL closure is copied to
the output dir, and the tree is zipped instead of a tarball. No
`LD_LIBRARY_PATH` games are needed — Windows resolves the bundled DLLs from the
executable's own directory.

## macOS .app (experimental)

Use a Mac with Xcode Command Line Tools and `brew install cmake ninja pkg-config ffmpeg qt`.
The Linux self-extracting ELF script is not used on macOS.

```sh
./build_and_deploy_macos.sh --iso /path/game.iso --jobs 3
# Reuse the generated sources and rebuild into a new output path:
./build_and_deploy_macos.sh --skip-setup --output /path/BT3-Recomp-new.app
# Package existing runner + Launcher.app without rebuilding:
./build_and_deploy_macos.sh --skip-build --output /path/BT3-Recomp-test.app
```

Output defaults to `build/macos-dist/BT3-Recomp.app`. An existing destination is
rejected so a failed build cannot overwrite a working app. `PS2X_BUILD_DIR` selects
an alternate build directory. Build each CPU architecture separately; Universal 2
is not supported by the shared SIMD configuration.

The script stages the launcher and `bt3-runner` in `Contents/MacOS`, assets in
`Contents/Resources`, and uses `macdeployqt` to collect dylibs, Qt frameworks and
plugins (including Cocoa). It checks architectures, rejects external absolute
library paths, checks the minimum OS versions in Mach-O load commands, then signs
inside out with an ad-hoc identity and verifies the bundle. The destination appears
only after these steps succeed. No game files are embedded in the app.

`--deployment-target VERSION` (or `MACOSX_DEPLOYMENT_TARGET`) controls the declared
minimum macOS version. The default is the build Mac's version. Lowering this flag
cannot make Homebrew binaries built for a newer OS compatible: supply dependencies
built for the chosen floor. Developer ID signing, notarization and clean-machine
validation are separate release steps, not performed by this script.

At first launch, select the USA ISO in the install wizard. Mutable files live in
`~/Library/Application Support/BT3-Recomp/`:

- `data/`: verified ELF and files extracted from the disc.
- `savedata/`: memory cards, settings and per-player bindings.
- `textures/`, `mods/`, `logs/`: replacements, mods and diagnostics.

The launcher keeps reading fonts and other bundled assets from Resources. Gamepad
capture/testing in the launcher is unavailable outside Linux; use the in-game
settings overlay. The EE sampling profiler is unavailable on macOS; the phase
profiler (`PS2X_GUESTPROF=1`) uses the native monotonic counter. OpenGL uses the
existing fallbacks for unsupported persistent-buffer and texture-barrier extensions.

## `setup.py` flags

```
python3 games/bt3/setup.py <iso|elf> [--jobs N] [--deploy OUT] [--skip-setup]
```

| Flag | Effect |
|---|---|
| `--jobs N` | runner build parallelism (default 3 — generated TUs are RAM-hungry) |
| `--deploy OUT` | after a successful build, assemble the playable tree in `OUT` |
| `--skip-setup` | reuse `games/bt3/work/` + generated sources; rebuild runner only |

## Input handling (launcher)

The launcher reads controllers through **GLFW** (same joystick mapping database
the runner uses via raylib) and the keyboard through **Qt key events** — there
is no evdev/`linux/input.h` anywhere, so the identical code builds on Linux,
Windows and macOS. Captured binds are stored as raylib `KEY_*` / `GAMEPAD_*`
codes so the runtime interprets them in-game without translation.

## Settings: `[logging] log_level`

Diagnostics go to `logs/bt3.log` next to the game (fast rotation: the previous
run is kept as `bt3.prev.log`). The verbosity is set in `savedata/bt3_settings.ini`:

```ini
[logging]
log_level=1
```

| level | enabled diagnostics |
|---|---|
| 0 | off (no log file) |
| 1 | `PS2X_PROFILE` (guest branch rate / hot PCs), `PS2X_MCLOG` (memory-card), `PS2X_SCHED_DEBUG` (thread scheduling) |
| 2 | + `PS2X_FTSPIKE` (frame-time spikes), `PS2X_FIGHTPROBE` (fight-state), `PS2X_REVEAL_HIDDEN_MENU_ENTRY` |
| 3 | + `PS2X_FRAMEPROF` (per-frame stats), `PS2X_CAMPROBE` |

`log_level` only *defaults* the matching `PS2X_*` environment variables
(`setenv(..., 0)`): an explicitly exported `PS2X_*` always wins.

## Checksums & reproducibility

The build pipeline does not ship game code. `setup.py` extracts the game from
your own USA ISO and verifies the boot ELF against a sha256 that is pinned in
the script; a different dump or region aborts the build. Generated sources are
never committed.

The Linux tarball ships with its own `.sha256` so a release can be verified
before unpacking.

## Notes / troubleshooting

- The launcher is Qt 6 (Widgets only) and builds its GLFW dependency from
  source via `FetchContent`, so no system GLFW install is needed on any
  platform.
- On Linux the runner's shared libraries travel in `lib/`, resolvable via
  `LD_LIBRARY_PATH`; macOS uses its own loader search-path semantics and Windows
  its DLL search order — the same tree, no per-OS tweaks in the game itself.
- If a run stops dead with a one-line `bt3.log` saying
  `Authorization required, but no authorization protocol specified`, that is an
  X11 auth failure of the launching shell, not a build problem.
