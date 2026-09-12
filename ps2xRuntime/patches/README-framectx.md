# Frame-context ring depth (paraLLEl-GS / Granite)

Worth ~7 fps in a BT3 fight.

## Applied automatically

CMake applies this at configure time (`ps2xRuntime/CMakeLists.txt`, the paraLLEl-GS
block). Nothing to run by hand. On configure you will see one of:

    -- granite framectx patch applied (frame-context ring 2 -> 16; PS2X_PGS_FRAMECTX overrides)
    -- granite framectx: PS2X_PGS_FRAMECTX already present, nothing to do

It detects the **effect**, not the patch: if `PS2X_PGS_FRAMECTX` is already in
`device.cpp` -- from this patch, or from the fuller `pgswait` diagnostic patch used on
dev branches -- it does nothing. It never resets or overwrites anything, so it cannot
clobber a more complete patch. If it cannot apply it warns and carries on at the
upstream depth of 2; the build still works, just slower.

Confirm at runtime: the log must read

    [pgswait] frame contexts = 16

`PS2X_PGS_FRAMECTX=<n>` overrides; `2` restores upstream behaviour.

## By hand, if you ever need to

    git -C ps2xRuntime/third_party/parallel-gs/Granite apply ps2xRuntime/patches/granite-framectx.patch

## What it does

Granite's frame-context ring is **flush-depth, not frame-depth**. paraLLEl-GS calls
`flush_submit` about **8 times per frame** at BT3's packet rate, and every advance
blocks in `Device::PerFrame::wait` until the GPU retires the context being reused.
At the upstream depth of 2 an advance waits on work submitted two flushes earlier --
a quarter of a frame -- which appears as 1-10 ms stalls inside `gif_transfer`, on
packets as small as 208 bytes. The GPU is only ~40% busy while that happens, so it is
latency, not throughput.

## Measured (DBZ Budokai Tenkaichi 3, depth 2 -> 16)

| | depth 2 | depth 16 |
|---|---|---|
| 1P vs COM | 52.5 fps | **59.8** (display-capped) |
| splitscreen | ~42 | **45.6** (peaks 54.7) |
| worst single `gif_transfer` call | 10.66 ms | 3.0 ms |
| calls over 1 ms, per second | 61-70 | 17-44 |

No visual difference in either mode (`pmode` and scanout identical).

## Cost

`PerFrame::begin()` is also where deferred Vulkan destruction happens, so resources
retire N advances later -- roughly two frames at depth 16. Watch memory over a long
session; `PS2X_PGS_FRAMECTX=8` keeps most of the win if it matters.
