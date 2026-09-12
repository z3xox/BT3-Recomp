# paraLLEl-GS frame-context fixes (ring depth + advance frequency)

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


---

# The companion fix: `parallel-gs-gcframe.patch`

Ring depth alone is only half of it, and the smaller half.

`flush_submit` ends with `device->next_frame_context()` under the comment *"Only do
garbage collection at frame boundaries"* -- but it is **not** a frame boundary here.
Measured at BT3's packet rate it runs **7.0 times per frame**: one from
`GSInterface::flush()` (the real boundary) and six from `FlushReason::HostAccess` sync
points, where the CPU needs VRAM the GPU may still be writing. Every advance waits in
`Device::PerFrame::wait` for the context it is about to reuse.

That was **4.3 ms per swap of blocking against a GPU only 59% busy**. It is also why
raising the ring depth stopped paying beyond 16: a deeper ring means more deferred
destruction per advance, since `PerFrame::begin()` is where that destruction happens.
The fix is the advance **count**, not the depth.

`GSInterface::flush()` sets `renderer.frame_boundary_flush`; `flush_submit` consumes it
and advances only then.

## Measured (default on)

| | before | after |
|---|---|---|
| 3x, 1P vs COM | 52.3 fps | **58.8** (holds 60) |
| 4x, 1P vs COM | 39.4 | 41.6 |
| splitscreen 2x | 45.6 | **49.2**, window spread 41-55 -> 46-51 |
| frame-context blocking | 251.8 ms/s | 0.9 |

For splitscreen the tighter spread matters more than the average: load differed 4.8%
between those runs, so the honest range on the mean is +2.6% to +7.9%, but the variance
reduction does not depend on that.

4x gains least because clearing the stall exposes a real limit rather than a fake one --
GPU time per swap actually *rises* (20.36 -> 21.27 ms) once the CPU stops throttling
submission, leaving it 89% busy and genuinely GPU-bound.

## Cost

Seven flushes' worth of resource destruction is deferred to the frame boundary. Checked
over a long splitscreen fight: GPU 7.3-8.3 ms/swap, copies 234-289, CopyVRAM 29-35,
TextureUpload 35.7-41.4 -- flat window to window, no drift, no validation errors.

`PS2X_PGS_GCFRAME=0` restores an advance on every `flush_submit`.
