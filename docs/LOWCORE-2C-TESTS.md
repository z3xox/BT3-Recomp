# 2-core / low-core experiments -- hardware, tests and outcome (DISCARDED)

> **Status: discarded.** The low-core ("2-core mode") work was reverted to the backup point
> (`1d4f45e`, tag `backup-pre-raylib-plan`). This document keeps the record: what was tested, on
> which hardware, and the conclusion, so the experiment is not repeated blindly.
> The discarded code is gone: the branch that held it (`wip-2core-experiments`) was deleted.

## 1. Hardware

| role | machine | notes |
|---|---|---|
| **target** | **AMD Ryzen 3 3250U** -- 2 cores / 4 threads (Zen+, Vega 3 iGPU) | the low-end box the whole effort was for |
| **simulation** | AMD Ryzen 5 5500 -- 6c/12t (Zen 3) | pinned to 2 logical CPUs with the Windows **processor affinity mask**. Beware: on this CPU logical CPUs 0 and 1 are the **two SMT siblings of physical core 0**, so mask `0x3` is a *single core / 2 threads*, NOT a 2-core host. A faithful 2c/2t sim needs mask `0x5` (LP0 + LP2). `hardware_concurrency()` still reports 12, so pool counts had to be forced explicitly. |

## 2. Test setup

- Renderer **OpenGL** (`renderer = "opengl"`), `render_scale = 1`, 1024x768, windowed.
- Diagnostics **all on** during a capture (opt-in env, so a normal run stays quiet):
  `PS2X_FRAMEPROF=1 PS2X_FTSPIKE=1 PS2X_GUESTPROF=1 PS2X_EEPROF=1 PS2X_RAGSTAT=1 PS2X_BARSTAT=1
  PS2X_STATEDBG=1 PS2X_DECPOOLSTAT=1 PS2X_VU1PIPESTAT=1 PS2X_THREADLOG=1`.
- Assertion of correctness is **by eye in real play**, not by the instrumentation.
- Same combat scene, a few minutes per run.

Configs compared (engine defaults = "config A"):

| tag | delta from A |
|---|---|
| **A** | none -- engine defaults (decode pool 2, async kick on, two-stage VU1 pipe on) |
| **B** | low-core recommendations: `PS2X_DECPOOL=0 PS2X_ASYNC_KICK=0 PS2X_VU1PIPE=0 PS2X_RASTER_THREADS=0` |
| **A-nolim** | `PS2X_TARGETFPS=0` (raylib frame limiter off) |
| **A-s1gl** | `PS2X_S1FENCE=gl` (stage-1 SyncPath fence for the OpenGL presenter) |
| **A-kick4** | `PS2X_KICKQ_FRAMES=4` (raise the kick-queue frame depth from 2) |

## 3. Outcome

On the 2c/4t target the game runs at roughly **15-18 fps**, with visible stutter. That is the number
this document stands on; the per-second instrumentation was **not** trustworthy for the fps readout
and its numbers are deliberately not reproduced here.

## 4. Conclusions

- **A (engine defaults) is the best-playing configuration and stays the default.**
- **B was rejected**: on a 2-core host it plays worse than A (removing the pipeline trades
  responsiveness away), regardless of what the instrumentation suggested.
- The levers that keep A -- `PS2X_TARGETFPS=0`, `PS2X_S1FENCE=gl`, `PS2X_KICKQ_FRAMES=4` -- did not
  recover the +8 fps target.
- The two-stage pipeline costs more than it returns on a 2-core host: it adds hand-off latency
  without giving real parallelism. That is structural, not a missing tweak.

## 5. Decision

- **2 cores / 4 threads is the hard FLOOR, and it is NOT recommended** (~15-18 fps, stutter).
  The practical minimum is **4 cores / 8 threads or better** (see `docs/HARDWARE-ESTIMATE.md`).
- The "2-core mode" (config B and the low-core auto-profile idea) is **discarded**.
- Default remains the engine default (config A), i.e. the pipelined configuration.
