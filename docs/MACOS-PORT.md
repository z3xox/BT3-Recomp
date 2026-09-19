# macOS port — current status and plan

## Implementation update · 2026-09-09

The original report below is kept as the initial diagnosis; its "does not compile"
and "has not been compiled" statements describe the checkout before the port.

Changes implemented:

- B1–B3: EE sampler disabled on platforms without a backend; the phase counter
  uses `mach_absolute_time()` on macOS, including the runtime calibration.
- B4: evdev reader stubbed outside Linux, without removing the bindings tab.
  Capture is disabled and the logical pad indices and saved configurations are
  kept. The game's pad input still goes through GLFW.
- Another real blocker: `Kernel/Syscalls/Thread.cpp` included `xmmintrin.h`
  without a guard. It now uses the runtime's SIMD header.
- CMake selects SIMD by output architecture and rejects Universal 2; macOS builds
  without FMA contraction to preserve separate roundings.
- The runner resolves its directory with `_NSGetExecutablePath()`; the previous
  `/proc/self/exe` fallback mislocated preferences and assets on macOS.
- The first real generation exposed a recompiler race: a freshly finished result
  could still sit in `readyCode` while checking whether a function was missing.
  The check and the pending-result cap were fixed.
- `scripts/build-macos.sh` and `tools/macos/deploy.py` prepare a `.app` with
  `macdeployqt`, a dependency/architecture/minimum-version audit and ad-hoc
  signing. Data, saves and settings stay outside the bundle, in
  `~/Library/Application Support/BT3-Recomp/`.

Port commands:

```sh
brew install cmake ninja pkg-config ffmpeg qt
python3 games/bt3/setup.py /path/bt3-usa.iso --jobs 3
./scripts/build-macos.sh --skip-setup --output /path/BT3-Recomp.app
```

The default minimum version is that of the build Mac. Nothing older than what the
Homebrew dylibs require is promised. Ad-hoc signing is for local use: it is not
equivalent to Developer ID or notarization.

Tests done so far on Apple Silicon, macOS 26.2, AppleClang 17:

- SHA-256 verification of the USA ISO and full runner and overlay generation.
- Build of the runtime and the Qt launcher; brief launcher start, also from a
  working directory other than the bundle's.
- Self-contained 178 MB ARM64 bundle: external dependencies audited, ad-hoc
  signature verified with `codesign --verify --deep --strict`, Cocoa plugin included.
- Running the packaged runner with the data extracted from `SLUS-21678`: it starts
  Cocoa/OpenGL 4.1 on an Apple M1 Pro, Core Audio, loads the ELF and enters the game
  loop; a requested quit ends cleanly.
- Twenty parallel gap-map generations match the sequential generation byte for byte
  (`tools/tests/recomp_parallel_smoke.py`).
- MMI/SIMD tests against scalar references: pass on ARM64 and on x86-64 under
  Rosetta. Phase counter and stubs also tested on both architectures.

The full runner build and the basic graphical test pass on Apple Silicon. Visual
parity with a physical Intel Mac, combat performance, audio and real pads need
further validation. The Metal/MoltenVK backend and the native EE profiler of the
optional phase were not implemented.

---

> **Technical report** · BT3-Recomp (SLUS-21678) · 2026-09-09
>
> | | |
> |---|---|
> | Commit analysed | `813b6a6` (`main`, clean) |
> | Method | Static analysis of the tree |
> | Compiled / run | **No** |
> | Support declared today | Linux · Windows (experimental) |

Repository status with respect to macOS, the four points that block a build today,
and a phased plan with decision gates. **The core is portable; the packaging is not
remotely portable.**

---

## Index

1. [Verdict](#0-verdict)
2. [What this is, for newcomers](#1-what-this-is-for-newcomers)
3. [What already works untouched](#2-what-already-works-untouched)
4. [Build blockers](#3-build-blockers)
5. [Packaging: the real work lives here](#4-packaging-the-real-work-lives-here)
6. [Graphics ceiling: the cost to accept](#5-graphics-ceiling-the-cost-to-accept)
7. [SIMD risk on Apple Silicon](#6-simd-risk-on-apple-silicon)
8. [Phased plan](#7-phased-plan)
9. [Known pitfalls](#8-known-pitfalls)
10. [Entry point](#9-entry-point)
11. [What has not been verified](#10-what-has-not-been-verified)

---

## 0. Verdict

macOS **is not supported today** and does not compile. But the work is smaller than
the README suggests: there is no `mmap`, no JIT, no executable memory, no `dlopen`,
no inline assembly. Guest memory is plain `new uint8_t[]`. The renderer is OpenGL 3.3,
within macOS's ceiling. The ARM64 path via `sse2neon` already exists in CMake, with an
explicit `APPLE` case.

The build blockers are **four sites in three files**, all in optional code (two
profilers and the launcher's evdev reader). A day's work should yield a binary that
starts on an Intel Mac.

The real cost is in two places:

- **Packaging**, which is ELF end to end and has to be rewritten, not patched.
- **Performance**, because macOS loses the fast vertex path that the repository
  itself describes as decisive on modest machines.

---

## 1. What this is, for newcomers

BT3-Recomp is not an emulator. It is a **static recompilation**: the game's MIPS
executable (PS2, USA, SLUS-21678) and its overlay are translated to **~7,800 C++
files** at build time, from the user's own ISO, and linked against a runtime that
emulates the surrounding hardware (GS, VIF, VU1, IOP, pads) with an OpenGL renderer.
The repository contains no game code or assets.

An important consequence for the port: **there is no code generation at run time**.
No `PROT_EXEC` pages, nothing that clashes with code signing or macOS's *hardened
runtime*. That removes at a stroke the part that normally makes porting an emulator
to Apple hard.

| Component | Role | Relevance to macOS |
|---|---|---|
| `ps2xRecomp` | The recompiler: ELF → C++ | Pure C++, portable. No findings. |
| `ps2xRuntime` | Runtime + renderer + game runner | Where the 4 blockers are. |
| `ps2xRuntime/src/launcher` | Qt6 launcher (settings, wizard) | No platform guards. Blocker. |
| `ps2xAnalyzer` | ELF analysis tool | No platform findings. |
| `games/bt3/setup.py` | Pipeline: ISO → generation → build | Only two branches: Windows and "the rest". |
| `scripts/build-linux.sh` | Assembles the self-extracting ELF | Useless on macOS. Rewrite. |
| `scripts/check_floor.sh` | Linux glibc floor gate | No macOS equivalent; `-mmacosx-version-min` + `MACOSX_DEPLOYMENT_TARGET` cover it. |

> The generation pipeline takes a few minutes at `--jobs 16`; by default the job count
> is auto-sized from CPU/RAM (conservative fallback `-j3`). It asks for ~16 GB of RAM
> and ~10 GB of disk.

---

## 2. What already works untouched

It is worth inventorying it first, because it is most of the system and it avoids
speculative work.

| Piece | Evidence | Why it is not a problem |
|---|---|---|
| Guest memory | `ps2_memory.cpp:362-403` | RDRAM, scratchpad, IOP RAM, GS VRAM and VU0/VU1 are `new uint8_t[]`. Zero `mmap`, zero `MAP_FIXED`. |
| Architecture detection | `CMakeLists.txt:43-49` | `CMAKE_SYSTEM_PROCESSOR` matches `arm64`, which is what macOS reports on Apple Silicon. |
| SIMD path for ARM | `CMakeLists.txt:56-90` | Fetches `sse2neon` v1.9.1 via FetchContent and defines `USE_SSE2NEON`. The `AARCH64 AND APPLE` branch (lines 72-74) is already written. |
| SIMD macro header | `ps2_runtime_macros.h:8-14` | `_MSC_VER` → `USE_SSE2NEON` → `immintrin.h`. All three paths already exist. |
| The ~7,800 generated files | `ps2_recompiler.cpp:105`<br>`function_emitter.cpp:45` | The codegen emits **only** `#include "ps2_runtime_macros.h"`. Fixing that header fixes all the generated code at once. |
| Thread names | `ThreadNaming.h:47` | Already has an `__APPLE__` branch with the correct `pthread_setname_np` signature. |
| Runtime evdev | `ps2xRuntime/CMakeLists.txt:426` | `if(UNIX AND NOT APPLE)` already excludes `pad_evdev_linux.cpp`. |
| Pads | `pad_config.cpp:7` | `PadEvdevStub` replaces the native reader where there is no evdev. Pads come in through GLFW, which uses IOKit on macOS. **Not a blocker.** |
| The 27 `__linux__` | 6 files | All are evdev or thread pinning (`ps2_runtime.cpp:360`, `:4133`, `:5285`). Optional functionality; already compiles cleanly elsewhere. |
| GL loading by name | `ps2_gs_gpu_renderer.cpp:389` | Uses `dlsym(RTLD_DEFAULT, …)`, which works the same on macOS. |
| Shaders | 11 sites, `#version 330` | GLSL 3.30 fits macOS's GL 4.1 ceiling. No *compute*, no SSBO. |
| Dependencies | `ps2xRuntime/CMakeLists.txt:65-170`, `:361-372` | raylib 5.5, imgui (`docking`) and rlImGui via FetchContent; FFmpeg via `pkg-config`. All resolvable with Homebrew. |
| raylib patch | `patches/raylib-5.5-ps2x.patch` | Touches only `src/config.h` and `src/rlgl.h`. Platform-neutral; applies the same. |
| ISO extraction | `setup.py:50-58` | Looks for `bsdtar` and falls back to `tar`. macOS's `tar` *is* bsdtar and reads ISO9660 directly. |
| Compiler | README | The generated VU1 code requires Clang (MSVC cannot compile it). On macOS Clang is the default compiler: an advantage, not an obstacle. |

> [!TIP]
> **The finding that saves the most.** The generated files include only
> `ps2_runtime_macros.h`. There is no need to touch the codegen or regenerate anything
> to support Apple Silicon at the SIMD level: the header already resolves the three
> intrinsic families and the runtime's 30,084 uses of `_mm_*` go through it.

---

## 3. Build blockers

Four sites. None affects game functionality: two opt-in profilers and the launcher's
evdev reader. All are resolved with platform guards and stubs.

### B1 — The EE profiler uses Linux/glibc-only APIs

**File:** `ps2xRuntime/src/lib/ps2_eeprof.cpp:20-35` and following

**What breaks:**

- `timer_create(CLOCK_THREAD_CPUTIME_ID, …)` (`:255`) and `timer_settime` (`:259`) —
  POSIX timers do not exist on macOS.
- `SIGEV_THREAD_ID` and `sev._sigev_un._tid` (`:254`) — a Linux extension.
- `syscall(SYS_gettid)` (`:254`) — there is no `SYS_gettid` on macOS.
- `((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP]` (`:150`) — glibc x86-64 layout; on
  macOS it is `uc_mcontext->__ss.__rip`, and on ARM64 there is no RIP.

**Why it hurts:** the file is in `ps2_runtime`'s source list
(`ps2xRuntime/CMakeLists.txt:398`), so it is always compiled. The `#else` branch
assumes Linux without checking.

**Fix:** guard the branch as `#if defined(__linux__)` and leave no-op stubs on macOS.
It is a profiler enabled by `PS2X_EEPROF`: nothing of the game is lost. A full port
would reimplement it with `dispatch_source` + `thread_get_state`.

**Effort:** ≈1 h for the stub. 1-2 days if the profiler is wanted working for real
(not urgent).

### B2 — `x86intrin.h` and `__rdtsc()` without a guard

**File:** `ps2xRuntime/include/runtime/ps2_guestprof.h:7`, used at `:20`, `:29`, `:38`

**What breaks:** unconditional `#include <x86intrin.h>`. On Apple Silicon the header
does not exist and neither does `__rdtsc()` (`sse2neon` offers `_rdtsc()`, with one
underscore).

**Fix:** a shim: on `__aarch64__` read `cntvct_el0` via `__builtin_arm_rsr64`, or
`mach_absolute_time()`. Careful: the ARM counter frequency is not the TSC's, and
`ps2_runtime.cpp` calibrates ticks against the wall clock on every print, so the ratio
corrects itself.

**Effort:** ≈30 min.

### B3 — One more `__rdtsc()`, outside the guard

**File:** `ps2xRuntime/src/lib/ps2_runtime.cpp:5226`

**What breaks:** the same intrinsic, in `guestprof`'s calibration block. Fixed with
B2's shim; listed separately so it is not missed when grepping only headers.

**Effort:** included in B2.

### B4 — The Qt launcher had no platform guards *(resolved upstream)*

**Status:** nothing is needed here any more. The original diagnosis was that
`src/launcher/evdev_reader.cpp` and `src/launcher/tab_bindings.cpp` included
`<linux/input.h>` without guards and that `src/launcher/CMakeLists.txt` pulled them
into the build with a `file(GLOB *.cpp)`, so the launcher had never compiled outside
Linux.

Upstream's multiplatform input port (`input_reader.{h,cpp}`, GLFW for joysticks and Qt
key events) removed `evdev_reader` entirely and with it the root of the problem: not a
single `linux/input.h` or `/dev/input` is left in the launcher, and the same code
compiles on Linux, Windows and macOS. This port no longer touches those files;
upstream's solution is better than the "unavailable" stub that had been planned here.

**Effort:** 0 — resolved upstream.

> Verified **absent** across the whole tree (excluding `thirdparty/`): inline assembly,
> `__builtin_ia32_*`, `__cpuid`, `mmap`, `VirtualAlloc`, `dlopen`. The only
> `sys/syscall.h` is B1's and the only `linux/input.h` are B4's plus
> `pad_evdev_linux.cpp`, which is already excluded.

---

## 4. Packaging: the real work lives here

> **Update note.** Upstream has since moved to shipping an **identical portable
> folder** on Linux, Windows and macOS, instead of the self-extracting ELF. The table
> below is kept because the analysis of each Linux dependency is still valid, and
> because it explains why the macOS path is a `.app` bundle; what no longer applies is
> the self-extracting stub.

The Linux distribution format is **a single self-extracting ELF**:
`[static stub][tar+zstd payload][32 B footer]`. On first start it unpacks into
`~/.cache/bt3-recomp/<seed>/`, where the *seed* comes from the payload hash, so each
rebuild invalidates its own cache. It is documented in `docs/DEPLOY.md`.

**None of this carries over to macOS.** It is not a matter of patching the script: the
whole concept (statically linked stub, `LD_LIBRARY_PATH`, ELF format) has no
equivalent.

| Linux dependency | Where | macOS equivalent |
|---|---|---|
| `gcc -static` | `scripts/build-linux.sh:88` | **None.** macOS does not allow linking libc statically. The stub is not viable. |
| `readlink("/proc/self/exe")` | `tools/selfx/stub.c:276` | `_NSGetExecutablePath()` |
| `LD_LIBRARY_PATH` | `stub.c:372`, `:417` | `@rpath` / `@executable_path` via `install_name_tool` |
| `ldd` + `mapfile` | `scripts/build-linux.sh:97` | `otool -L`. And `mapfile` does not exist in Apple's bash 3.2. |
| glibc blacklist | `scripts/build-linux.sh:112` | Unnecessary: macOS does not have the `GLIBC_PRIVATE` problem. |
| `sha256sum` | `scripts/build-linux.sh:125` | `shasum -a 256` |
| `nproc` | `scripts/build-linux.sh:27` | `sysctl -n hw.ncpu` |
| `realpath -m` | `scripts/build-linux.sh:49` | Does not exist. `python3 -c os.path.abspath` or brew's coreutils. |
| Hard-coded Qt6 path | `scripts/build-linux.sh:148` | `/usr/lib/cmake/Qt6/Qt6Config.cmake` will never exist; use `CMAKE_PREFIX_PATH` with `brew --prefix qt6`. |
| Concatenate ELF + footer | `scripts/build-linux.sh:128-135` | `.app` bundle, or DMG. Mach-O does not support this trick as-is. |
| Runner copy | `setup.py:212-217` | The `else` branch assumes Linux; on macOS it lands here through `os.name == "posix"` (`setup.py:36`). |
| Linux glibc floor | `scripts/check_floor.sh` | A portable Linux artifact targets glibc 2.35 (Ubuntu 22.04) via `BT3_GLIBC_MAX`; a native build defaults to the host glibc. On macOS the equivalent is `-mmacosx-version-min` + `MACOSX_DEPLOYMENT_TARGET`, and a Mac is needed to produce it. |

### The shape it should take

- A `BT3-Recomp.app` bundle: binary in `Contents/MacOS`, dylibs in `Contents/Frameworks`,
  assets in `Contents/Resources`, plus an `Info.plist` with the minimum system version.
- `install_name_tool` / `@rpath` to relocate dylibs. There are `dylibbundler` and
  `macdeployqt`; **`macdeployqt`** also resolves the Qt plugins, which is the part that
  is always forgotten and produces the "could not find the Qt platform plugin cocoa"
  failure.
- `codesign --sign -` (ad-hoc) at minimum. Without a signature, Gatekeeper kills any
  binary the user has downloaded. To distribute for real, Developer ID + notarization
  are needed, which implies a paid Apple account: **a product decision, not a technical
  one**.
- Universal 2 (`x86_64;arm64`) is possible with `CMAKE_OSX_ARCHITECTURES`, but it
  duplicates a build that is already 7,800 translation units. Recommendation: two
  separate artifacts.

---

## 5. Graphics ceiling: the cost to accept

macOS froze OpenGL at **4.1** (and deprecated it in 10.14). The renderer uses two
things above that line. Both degrade on their own, without failing — and that is the
problem: it does not break, it slows down silently.

### R1 — The persistent vertex ring is lost

**What:** `glBufferStorage` with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` requires
`GL_ARB_buffer_storage` (GL 4.4). It is the `[vbring]` optimization of the raylib patch.

**Behaviour:** it is gated in `patches/raylib-5.5-ps2x.patch:104` by
`glBufferStorage != NULL`, so macOS falls back to the original `glBufferSubData` path.
It works. But according to the patch's own comment, that path cost **~80 ms of GPU per
second and ~2.5 µs of CPU per flush**, with ~3,000 flushes per frame, and "dominated
low-end machines".

**Implication:** measure before promising anything. If a modern Mac absorbs the cost,
there is no problem; if not, the way out is a Metal or Vulkan backend via MoltenVK, and
that is another project, not a port.

### R2 — `glTextureBarrier` falls back to `glFinish()`

**What:** `glTextureBarrier` is GL 4.5 (or `NV_texture_barrier`).
`ps2_gs_gpu_renderer.cpp:392-396` looks it up with `dlsym`, tries the NV variant, and if
neither is present runs `glFinish()`: a full pipeline sync where only a texture barrier
was requested.

**Mitigating:** the comment at `:866` says that for the `[rtsnap]` case neither
`glFinish` nor `glTextureBarrier` solved anything and that the fix was a framebuffer
blit. That is: the critical path no longer depends on the barrier. The impact is
smaller than R1.

---

## 6. SIMD risk on Apple Silicon

The runtime has **30,084 uses of `_mm_*` intrinsics** (excluding `thirdparty/`) spread
across the rasterizer, the GS, the VIF1 interpreter and VU1. On ARM64 all of them go
through `sse2neon`. That is where a port can produce subtle bugs that are expensive to
diagnose: not build failures, but pixels and geometry slightly off from rounding or
saturation differences.

The good news is that the set of SSE4.1 intrinsics actually used is short, and
`sse2neon` v1.9.1 covers all of them:

```
_mm_blendv_epi8   _mm_blendv_ps     _mm_cvtepi32_ps
_mm_extract_epi32 _mm_extract_epi64 _mm_insert_epi8
_mm_max_epi32     _mm_min_epi32     _mm_mullo_epi32
```

Also, `ps2_runtime_macros.h` does not use the intrinsics raw: it wraps them in `PS2_*`
macros per R5900 MMI instruction (`PS2_PEXTLW`, `PS2_PADDW`, `PS2_PMAXW`…). That
concentrates the risk surface in one file and gives a natural place to write x86 ↔ ARM
differential tests if discrepancies appear.

> [!IMPORTANT]
> **Do not start on Apple Silicon.** An Intel Mac validates the port *without* putting
> `sse2neon` into the equation: if the game renders wrong on Intel, the problem is the
> port; if it renders fine on Intel and wrong on ARM, the problem is SIMD. Separating
> those two variables saves days.

---

## 7. Phased plan

The phases are numbered because the order matters: each has a gate that avoids spending
work on the next one on a foundation that does not hold.

### Phase 1 — Build on an Intel Mac · ≈1 day · B1·B2·B3·B4

Resolve the four blockers with guards and stubs. No reimplementing profilers or input
backends: the goal is a binary that links. Dependencies via Homebrew
(`cmake ninja pkg-config ffmpeg qt6 zstd`) and configure with `CMAKE_PREFIX_PATH`
pointing at Qt6.

**Gate:** `ps2EntryRunner` links and starts without crashing before the first frame.

### Phase 2 — Make it render · ≈1-3 days · no packaging

Run from `build/ps2xRuntime` with `PS2X_CD_IMAGE` pointing at the ISO, without a bundle
or the launcher. Here it is discovered whether macOS's strict *core* profile accepts the
renderer, whether pads come in through GLFW, and whether audio and video (FFmpeg) work.
It is the phase with the most real uncertainty and where the project's risk lives.

**Gate:** a playable fight at a reasonable speed. Measure R1's impact here before
deciding anything about graphics backends.

### Phase 3 — Apple Silicon · ≈2-5 days · high, poorly predictable risk

Build on arm64 and validate `sse2neon` against the behaviour observed on Intel: same
scenes, same capture, compare. The failures here are visual and silent, not crashes.
This is the least reliable estimate in the report.

**Gate:** visual parity with the Intel build on an agreed set of scenes.

### Phase 4 — `.app` bundle and distribution · ≈2-3 days · rewrite, not patch

A new `scripts/build-macos.sh`, sibling of the Linux one, not an `if` version.
Bundle, `macdeployqt`, ad-hoc signing, and a declared `MACOSX_DEPLOYMENT_TARGET` that
plays the role the glibc 2.35 floor plays on Linux. Update `README.md` and
`docs/DEPLOY.md`, which today state "Linux or Windows".

**Gate:** the `.app` starts on a Mac that is not the build machine and without
development tools installed.

### Phase 5 — Optional: recover what degraded · only if phase 2 asks for it

Native EE profiler (`dispatch_source` + `thread_get_state`), native input via
IOKit/GameController for what GLFW does not map, and — if performance did not get
there — a Metal or MoltenVK backend. Each point is independent and none blocks
distribution.

### Estimates summary

| Milestone | Estimate | Confidence |
|---|---|---|
| Compiles and starts (Intel) | 1-2 days | High — the work is identified line by line |
| Playable without packaging (Intel) | +1-3 days | Medium — depends on GL driver surprises |
| Parity on Apple Silicon | +2-5 days | Low — depends on how many `sse2neon` bugs appear |
| Distributable `.app` | +2-3 days | High — known work, just laborious |
| **Total, both architectures** | **1-2 weeks** | Medium |

Not counting Apple notarization, which is administrative paperwork and a paid account.

---

## 8. Known pitfalls

Things that will cost time to anyone who does not know them in advance.

- **Do not patch `scripts/build-linux.sh`.** It has seven intertwined Linux dependencies,
  one of them (`gcc -static`) with no equivalent. A sibling script comes out cleaner and
  does not break the Linux path, which works.
- **macOS's bash is 3.2.** Any new script using `mapfile`, `${x,,}` or associative arrays
  fails on a clean Mac. Either stick to bash 3.2, or explicitly declare that it needs
  Homebrew's bash.
- **The Qt6 launcher without `macdeployqt` will start on the build machine and nowhere
  else.** The typical failure is "could not find the Qt platform plugin cocoa", and it
  confuses because the binary exists and has permissions.
- **Gatekeeper.** An unsigned `.app` that gets downloaded is quarantined and will not
  start; it works locally, but not in someone else's hands. It is the kind of bug that
  shows up right when you publish.
- **The region is fixed.** The committed function maps are for the USA executable
  (SLUS-21678). The port does not change that and no other regions should be promised.
- **The default build is `-j3`.** With 7,800 translation units that is an eternity. Pass
  `--jobs` with the core count, watching RAM (~16 GB recommended).

---

## 9. Entry point

For whoever continues: this is what to run first, before writing a line.

```sh
# 1. Dependencies (Intel Mac for phase 1)
brew install cmake ninja pkg-config ffmpeg qt6 zstd python@3.12

# 2. Confirm the 4 blockers in the current checkout (these and only these should show)
grep -rn "x86intrin\|__rdtsc" ps2xRuntime/include ps2xRuntime/src
grep -rn "linux/input.h" ps2xRuntime/src/launcher
sed -n '20,35p;150p;250,260p' ps2xRuntime/src/lib/ps2_eeprof.cpp

# 3. Configure only the runtime; Qt6 located by brew
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt6)"

# 4. Full pipeline from the ISO (adjust --jobs to the real cores)
python3 games/bt3/setup.py /path/bt3-usa.iso --jobs 10

# 5. Run without packaging (phase 2)
cd build/ps2xRuntime
PS2X_CD_IMAGE=/path/bt3-usa.iso ./ps2EntryRunner ../../games/bt3/work/SLUS_216.78
```

Environment variables useful during diagnosis, all already in the code: `PS2X_VBRING=0`
(disables the vertex ring, to compare with the slow path), `PS2X_RTSNAP=0`,
`PS2X_GUESTPROF=1`, `PS2X_EEPROF`, `PS2X_PIN` (Linux only).

---

## 10. What has not been verified

Honesty about the scope of this report, so nobody takes it for more than it is.

- **Nothing has been compiled.** The whole report is a reading of the code at commit
  `813b6a6`. Blockers B1-B4 are identified by inspection, not by a compiler error. It is
  very likely that the first real build uncovers more, typically transitive headers and
  warnings-as-errors.
- **The game has not been run** on any platform, so the renderer's behaviour under
  macOS's strict *core* profile is reasoned prediction, not data.
- **`thirdparty/` has not been audited** beyond checking that `xxhash.h` brings no
  problems. There may be more in there.
- **The ~7,800 generated files have not been reviewed** one by one; the conclusion that
  `ps2_runtime_macros.h` is enough comes from the codegen emitting only that include
  (`ps2_recompiler.cpp:105`, `function_emitter.cpp:45`).
- **No CI.** The repository has no GitHub workflows; the release artifact is produced
  natively per OS (Linux, Windows, macOS). macOS in CI would need macOS runners, which
  is a cost and a decision apart.

---

*Report produced by static analysis of the tree at `main` @ `813b6a6`, 2026-09-09. All
`file:line` references correspond to that commit and will drift with future changes.*

*BT3-Recomp is built on PS2Recomp. The repository contains no game code, assets or
media: the recompilation starts from the user's own ISO.*
