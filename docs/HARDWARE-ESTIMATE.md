# BT3-Recomp — System Requirements & Hardware Estimate

> **TEMPORARY document** (not versioned). These are **estimates** derived from one reference
> measurement (Ryzen 5 5500 @ 3x), **validated** against a real user with a **Ryzen 3 3200G + 8 GB**
> (runs well at 1x). For exact numbers, see "How to measure (Phase C)" at the end.

---

## 1. System Requirements

| | **MINIMUM** | **RECOMMENDED** |
|---|---|---|
| **OS** | Windows 10 64-bit · Linux | Windows 10/11 64-bit · Linux |
| **CPU (AMD)** | Ryzen 3 3200G (4c/4t, Zen+) | Ryzen 5 3600 (6c/12t, Zen 2) |
| **CPU (Intel)** | Core i3-8100 (4c/4t) | Core i5-10400 (6c/12t) |
| **RAM** | 8 GB | 16 GB |
| **GPU (AMD)** | Radeon Vega 8 (iGPU) | Radeon RX 570 4 GB |
| **GPU (NVIDIA)** | GeForce GT 1030 (or Intel UHD 630 iGPU) | GeForce GTX 1650 4 GB |
| **VRAM** | 1 GB (or shared) | 4 GB |
| **Graphics API** | OpenGL 3.3 | OpenGL 4.x |
| **Storage** | ~6 GB | ~8 GB (with texture pack) |
| **Target** | **30 fps @ 1x** | **60 fps @ 2x–3x** |

**Minimum notes:** 4 cores **with** SMT/threads, i.e. **4c/8t or better** is the practical minimum.
At 1x the bottleneck is the **CPU** (emulation), not the GPU.

> **FLOOR (NOT recommended) — 2 cores / 4 threads** (e.g. Ryzen 3 3250U / Vega 3): it does run, but
> only at about **15-18 fps** with visible stutter, so it is the hard floor rather than a supported
> minimum. See `docs/LOWCORE-2C-TESTS.md` for the measurements and why the low-core ("2-core mode")
> configuration was **discarded**.

**Recommended notes:** the dedicated GPU allows a higher `render_scale`; 16 GB for the texture pack.

---

## 2. Limits of the MINIMUM for visual upgrades

Starting from the **minimum** hardware, to keep 30 fps stable it is **NOT recommended** to:

- ❌ Raise `render_scale` above **1x** → it won't raise fps (the CPU is the bottleneck) and on an
  iGPU it competes for shared-RAM bandwidth.
- ❌ Enable the **Full texture pack** → with **8 GB** of RAM (shared with the iGPU) it's the first
  thing that chokes; use **Off** (or **Lite** at most).
- ❌ Raise the **window resolution** to 1440p/4K → the present blit and shared RAM take a hit.
- ❌ Enable extra effects (DoF / outlines) if stutter appears.
- ✅ Worth doing: **frame cap = 30** (stabilizes and makes modest hardware viable) and **widescreen ON**
  (negligible cost).

**Recommended ceiling (minimum hardware):**
```
render_scale = 1
widescreen   = ON
texture_pack = Off   (Lite if RAM allows)
frame cap    = 30
```

---

## 3. Reference table — settings ceiling by hardware

> "Ceiling" = the **maximum** config that runs well (stable 30/60 fps).

| Hardware | render_scale (CEILING) | Widescreen | Texture pack | Cap | Bottleneck |
|---|---|---|---|---|---|
| **Ryzen 3 3200G + 8 GB (Vega 8)** *(validated)* | **1x** | ON | **Off/Lite** | **30** | CPU |
| 4c/8t R≈0.5 + iGPU (UHD 620/630, Vega 8) | 1x | ON | Lite | 30 | CPU |
| 4c/8t R≈0.6–0.8 + GTX 1050 / RX 560 | 2x–3x | ON | Full | 30 (60 at 2x) | CPU/GPU |
| 6c/12t R≈1.0 + GTX 1650 / RX 570 | 3x–4x | ON | Full | 60 | GPU |
| 6c/12t R≥1.3 + GTX 1660 / RX 580+ | 4x–6x | ON | Full | 60 | GPU |

**Rules:** from **1x to 2x the CPU rules**; from **3x up the GPU rules**. Raising the scale does
**not** rescue a slow CPU (`guest_ms` is the same at every scale).

---

## 4. Technical detail (model)

### 4.1 Reference point (measured)
| Item | Value |
|---|---|
| CPU | Ryzen 5 5500 (6c/12t, Zen 3) |
| GPU | Radeon RX 580 4 GB |
| RAM | 32 GB |
| `render_scale` | 3 (≈1536×1344) + widescreen |
| FPS | 30 (capped) |
| `guest_ms` | ≈ 15.6 ms (EE/VU emulation; **scale-independent**) |
| `gpu_ms` | ≈ 15 ms (at 3x ≈ 2.06 MP) |
| `host` | ≈ 57 fps |

### 4.2 Scale → resolution / pixels
| Scale | Internal resolution | MP |
|---|---|---|
| 1x | 512×448 | 0.23 |
| 2x | 1024×896 | 0.92 |
| 3x | 1536×1344 | 2.06 |
| 4x | 2048×1792 | 3.67 |
| 6x | 3072×2688 | 8.26 |

### 4.3 Model
- **GPU:** `gpu_ms ≈ 1.0 + 6.8 × MP` (RX 580). → 1x ≈ 2 ms · 2x ≈ 7 ms · 3x ≈ 15 ms · 4x ≈ 26 ms · 6x ≈ 57 ms.
- **CPU:** `guest_ms ≈ 15.6 / R` (R = single-thread relative to Zen 3; Zen3 = 1.0).
  - **30 fps**: `R ≳ 0.5`. **60 fps**: `R ≳ 1.1` (at 1x the GPU is not the limit).

### 4.4 What scales with resolution
- **GPU (primary):** rasterization (fill rate).
- **CPU (secondary):** per-pixel work in the GS backend — barrier/staging readbacks, **VRAM
  writeback** (`glReadPixels`) and the present blit. On an iGPU it hurts extra due to **shared RAM**.
- **Does not scale:** EE/VU emulation (`guest_ms`), which is the floor.

### 4.5 Reference tiers
- **CPU:** Tier A (1x–2x) i3-8100/10100, Ryzen 3 3200G/4300G · Tier B (3x) i5-10400, Ryzen 5 3600 ·
  Tier C (4x–6x) Ryzen 5 5600, i5-12400+.
- **GPU:** 1x–2x iGPU GL 3.3+ / GT 1030 · 3x GTX 1050/RX 560 · 4x GTX 1650/RX 570 · 6x GTX 1660/RX 580+.
- **VRAM:** 1–2 GB (1x–2x) · 2–4 GB (3x–4x) · 4 GB+ (6x with pack). **RAM:** 8 GB min · 16 GB rec.

---

## 5. Caveats

1. Derived from **one measurement** (Zen3 @ 3x); the CPU/GPU factors are approximate.
2. `guest_ms` is quantized (15.625) → CPU threshold ±20%.
3. The **texture pack** raises VRAM/RAM and upload cost (figures assume pack Off/Lite).
4. Drivers matter: on Windows the path is OpenGL/AltGL; on Linux (the reference) the cost differs.

---

## 6. How to measure (Phase C — for exact figures)

1. `render_scale=1`, base resolution, with `PS2X_FRAMEPROF=1` and `PS2X_FTSPIKE=1`.
2. Harness with CPU affinity (2c/4t and 4c/8t) + a RAM Job Object (4 GB and 8 GB).
3. Capture `[fps]` (`guest_ms`, `gpu_ms`, `host`), `[ftspike]` (worst frame) and RSS.
4. Reproducible scenario; repeat at 2x/3x/4x to validate the GPU model.
5. Replace the model constants (`15.6` and `6.8`) with the measured values.
