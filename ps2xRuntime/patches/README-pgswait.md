# [pgswait] GPU-blocking instrumentation

Answers one question: is the CPU time attributed to paraLLEl-GS `gif_transfer`
real work, or the CPU blocked waiting on the GPU? `perf`'s cpu-clock cannot see
off-CPU blocking, so these counters measure it directly.

`ps2_gs_pgs.cpp` (tracked in this repo) prints the counters, but they are
**defined in the paraLLEl-GS fork and its Granite submodule**, which are not
tracked here (`third_party/parallel-gs` is git-ignored). Without the two patches
below the build fails to link.

## Apply

**These patches are NOT idempotent.** `git apply` fails with `patch does not apply`
both when a patch is already applied and when it is genuinely broken, and the two
look identical. Always reset the files first — that makes re-applying safe and makes
a real failure mean something:

    cd ps2xRuntime/third_party/parallel-gs
    git checkout -- gs/ tools/
    git apply ../../patches/pgswait-parallel-gs.patch            # base: 42c6701 (fork/bt3-texreplace)
    git -C Granite checkout -- vulkan/
    git -C Granite apply ../../../patches/pgswait-granite.patch  # base: 16e7395f (submodule pin)

The resets discard only a previously applied version of these same patches. If you
have other local edits in `third_party/parallel-gs`, save them first.

To ask which state you are in without changing anything:

    git apply --reverse --check ../../patches/pgswait-parallel-gs.patch   # succeeds => already applied

Then rebuild as usual. Confirm it took effect: the log must contain
`[pgswait] frame contexts = 2 (PS2X_PGS_FRAMECTX, default 2)`, and the `[pgs]` line
must carry a `gpuwait-by-thread` field (that field is the marker for the 2026-09-12
refresh specifically — if it is missing, the Granite patch is the old version).

## Run

    PS2X_PGS=1 PS2X_PGS_EXCLUSIVE=1 PS2X_PGS_TIMESTAMPS=1 PS2X_GUESTPROF=1 PS2X_FRAMEPROF=1

Get into a fight, hold ~60 s, and read the `gpuwait:` field of the `[pgs]` line:

    gpuwait: FRAMECTX <ms/s> (<calls/s>, <blocked/s>) timeline <ms/s> (<calls/s>) fence <ms/s> idle <ms/s> quirk <n>

The number that matters is **blocked/s divided by swaps/s**.

## What is already known (measured on a 9800X3D + RTX 5080, do not re-derive)

* Blocking is **structural, not slack**: per-swap costs held flat within 5% when
  the frame budget halved (30 -> 60 fps). Frame-context blocking 2.76 -> 2.90 ms/swap.
* **Exactly one blocking wait per swap**, invariant to ring depth.
* Root cause: `flush_submit()` -> `next_frame_context()` -> `frame().begin()` ->
  `wait(UINT64_MAX)`, an unconditional wait so the incoming context's command
  pools can be recycled.
* ~~`PS2X_PGS_FRAMECTX=3` is a null result~~ **WRONG, corrected 2026-09-12.** The ring
  is **flush-depth, not frame-depth**: FRAMECTX runs 429 calls/s at 52 swaps/s =
  **8.2 advances per frame**, 1.75 of them blocking. At depth 2 an advance waits on
  work two flushes old - a quarter of a frame - so 3 was far too small a step to
  measure anything. The knob was also clamped to [2,4] on the same wrong assumption.
  Cap is now 32 and **the default is 16**, user-measured on both fight modes:
  1P vs COM 52.5 -> 59.8 swaps/s (locked 60), splitscreen ~42 -> 45.6 (peaks 54.7),
  worst single gifTransfer call 10.66 ms -> 3.0-3.4 ms, `calls>1ms/s` 61-70 -> 17-44,
  no visual cost in either mode. `PS2X_PGS_FRAMECTX=2` restores upstream behaviour.
  Cost: `PerFrame::begin()` is where deferred Vulkan destruction happens, so resources
  retire ~2 frames later at depth 16 - worth watching memory over a long session.
* Skipping the advance is **not** a safe fix: `PerFrame::begin()` is also the only
  place deferred Vulkan destruction happens (images, buffers, views, samplers,
  semaphores, descriptor pools, device memory), so skipping trades a stall for
  unbounded growth.

## Other knobs carried by these patches (default off, no behaviour change)

* `PS2X_PGS_SSCENSUS=<seconds>` - per-VRAM-page census of super-sampled texture
  uploads. Established the cost is 62-65% one page (the Z/mask page) and 13-14%
  the outline page.
* `PS2X_PGS_SSPAGES=<page list>` - restrict which pages may be super-sampled.
  **Narrowing is not free**: every variant visibly aliases terrain silhouettes.

## Update 2026-09-12: per-thread attribution, and what it changed

The `[pgs]` line now also prints:

    gpuwait-by-thread ms/s: other=<n>(fence .. tl .. fctx ..) game=<n>(..) kick=<n>(..) gs=<n>(..)

Measured in a fight: `other=560 (100% timeline)`, `game=210 (100% frame context)`,
`kick=0`, `gs=0`.

**`other` is `PGS-Waiter`** — the Granite thread in `gs/gs_renderer.cpp:761` whose
entire job is to sit in `wait_timeline` and publish completions. Its 560 ms/s is
by design and on no critical path. **Do not read it as a stall.** The real GPU
stall is 210 ms/s and it is all on the game thread. GsThread blocks on the GPU
exactly zero — its ~583 ms/s of busy is genuine CPU parse work.

That reprices the whole problem. Per swap at 35.3 fps (28.3 ms wall):
KickWorker 17.0 ms, GsThread 16.5 ms, GameThread EE ~11.3 ms, GPU 8.3 ms — 53 ms
of work in 28.3 ms of wall, i.e. only **1.9x overlap out of a possible 4x**. The
slowest single unit is 17.0 ms = **59 fps available from overlap alone, with
nothing made faster**. The port is serialization-bound, not throughput-bound.

## The serialization: `PS2X_ASYNC_GSQUEUE=3`

`sceGsSwapDBuff` = 1 `applyGsDispEnv` + 2 `applyGsRegPairs`, and every one of them
drained the whole kick pipeline (`kick_drain` 350 ms/s on the game thread; also why
`worker_idle`=369 and `stage2_idle`=290 — three times a frame both downstream
threads run dry).

    0  drain then apply (default, unchanged behaviour)
    1  queue everything            -- BREAKS THE PICTURE, do not use
    2  queue all but display writes -- nearly useless on its own, see below
    3  2 + split applyGsDispEnv     -- no drain left in the swap path  <-- TEST THIS

Mode 1 broke because it deferred `regs.*` onto the worker while the presenter reads
`r->display1` directly on the game thread (`ps2_gs_pgs.cpp:414`) and `display1`
carries MAGH — hence the display alternating squished/normal. Mode 2 is nearly
useless because `applyGsDispEnv` runs *first*, so the drain it keeps is the one that
finds the queue full; the two it queues hit an already-empty queue. Mode 3 splits the
call: `regs.*` inline (presenter reads them here), flip hook queued (`streamFlip`
must be stream-ordered or it presents an unfinished frame).

Verified on Linux in a splitscreen fight — drain counts (waits/10s):
mode 0 = `kick_drain` 2340 / `fence_syncpath` 1170; mode 3 = 1152 / 1152, i.e. every
swap-path drain gone. Picture correct by screenshot, `stream!=live` 0%, no crash.

**It cannot be priced on that box**: there `fence_syncpath == kick_drain`, so 100% of
the paying drains come from `sceGsSyncPath`, which fires 115x/s and empties the queue
just before the swap path would. On Windows `fence_syncpath=0.0(0)` and
`kick_drain=350.5` — all of it on the path mode 3 clears. Measure it there:

    PS2X_ASYNC_GSQUEUE=3   vs unset, in a fight, comparing swaps/s and kick_drain.
