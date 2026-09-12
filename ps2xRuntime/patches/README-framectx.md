# Frame-context ring depth (paraLLEl-GS / Granite)

Worth ~7 fps in a BT3 fight. Not applied automatically, because it patches the
Granite submodule, which points at upstream `Themaister/Granite`.

## Apply

    cd ps2xRuntime/third_party/parallel-gs
    git -C Granite checkout -- vulkan/
    git -C Granite apply ../../../patches/granite-framectx.patch

Then rebuild. Confirm it took: the log must read

    [pgswait] frame contexts = 16

`PS2X_PGS_FRAMECTX=<n>` overrides at runtime; `2` restores upstream behaviour.

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
