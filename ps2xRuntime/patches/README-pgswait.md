# [pgswait] GPU-blocking instrumentation

Answers one question: is the CPU time attributed to paraLLEl-GS `gif_transfer`
real work, or the CPU blocked waiting on the GPU? `perf`'s cpu-clock cannot see
off-CPU blocking, so these counters measure it directly.

`ps2_gs_pgs.cpp` (tracked in this repo) prints the counters, but they are
**defined in the paraLLEl-GS fork and its Granite submodule**, which are not
tracked here (`third_party/parallel-gs` is git-ignored). Without the two patches
below the build fails to link.

## Apply

    cd ps2xRuntime/third_party/parallel-gs
    git apply ../../patches/pgswait-parallel-gs.patch      # base: 42c6701 (fork/bt3-texreplace)
    git -C Granite apply ../../../patches/pgswait-granite.patch   # base: 16e7395f (submodule pin)

Then rebuild as usual. Confirm it took effect: the log must contain
`[pgswait] frame contexts = 2 (PS2X_PGS_FRAMECTX, default 2)`.

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
* `PS2X_PGS_FRAMECTX=3` (deeper ring) is a **null result** - it cannot change the
  blocking frequency. Do not retry it.
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
