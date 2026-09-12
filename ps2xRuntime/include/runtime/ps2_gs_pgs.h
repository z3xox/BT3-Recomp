#pragma once
// [pgs] paraLLEl-GS backend: the PS2 GS emulated in Vulkan compute (Arntzen Software, LGPLv3+), driven by the same
// GIF packet stream our GL renderer consumes. Enabled with PS2X_PGS=1 when the runtime was built with the checkout
// present (ps2xRuntime/third_party/parallel-gs -> PS2X_HAVE_PGS). PS2X_PGS_EXCLUSIVE=1 skips our own GS parse/GL
// renderer (perf configuration); the default runs both and presents the paraLLEl-GS scanout (first-light / A-B).
// PS2X_PGS_SSAA=1|2|4|8|16 sets the super-sampling rate, PS2X_PGS_HIRES=1 keeps a 2x scanout.
#include <cstddef>
#include <cstdint>
#include <vector>
struct GSRegisters;
class GS;
namespace ps2x_pgs
{
#if defined(PS2X_HAVE_PGS)
bool enabled();
bool exclusive();
// PS2X_PGS_PACK=1: texture replacement on the backend (our GS parse runs state-only to feed the hashes; not exclusive)
bool packMode();
void setGs(GS *gs);
void setForceBilinear(bool on);   // the overlay's Force Filtering toggle, mirrored to the backend
void setInkWidthPct(int pct);     // [pgsink] the overlay's Ink Width: outline stroke width in % of a PS2 texel (25..100)
void setInkColor(uint32_t rgb);   // [pgsink] the overlay's Ink Color, 0xRRGGBB (0 = the game's black)
void setPackEnabled(bool on);     // [pgslive] the overlay's Texture Replacement toggle (live: cached textures are dropped)
void setRenderScale(int scale);   // [pgslive] the overlay's Internal Resolution 1..4 (live: the backend is re-created at the matching SSAA)
// A GIF packet as the arbiter delivers it (path 1..3, qword multiple). Any thread; serialised inside.
bool gifTransfer(uint8_t pathId, const uint8_t *data, size_t size);   // true = consumed by the backend
// PS2X_PGS_COALESCE=1: stage 2 hands runs of same-path packets to the backend as ONE gif_transfer (6000 calls per
// frame otherwise); while a coalesced run is being processed the per-packet hook must stay quiet.
bool coalesce();
void setSuppressed(bool on);   // thread-local
// The guest's privileged register store (offset from 0x12000000, full 64-bit value after the merge).
void privWrite(uint32_t regOff, uint64_t value, GSRegisters *regs);
// The live privileged register block (PS2Memory::gs_regs): read at every swap, so every writer is covered.
void setRegs(GSRegisters *regs);
// The DISPFB1 flip as it passes through stage 2 (stream order); the swap scans out this buffer.
void streamFlip(uint64_t dispfb1);
bool gsProfOn();   // [gsprof]
// Called at the frame swap (GsGpuRenderer::swapFrame): flush, scan out, read the frame back for the present thread.
void onSwap();
// [pgsfit] Present thread: the size the frame is drawn at on screen. Auto scanout resolution picks the smallest 2x/4x
// shift that covers it (PS2X_PGS_HIRES unset); an explicit PS2X_PGS_HIRES=0|1|2 overrides.
void setPresentSize(uint32_t w, uint32_t h);
// Present thread: moves the newest scanout into `rgba` (w x h, RGBA8). False when nothing new arrived.
bool takeFrame(std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h);
void shutdown();
#else
inline bool enabled() { return false; }
inline bool exclusive() { return false; }
inline bool packMode() { return false; }
inline void setGs(GS *) {}
inline void setForceBilinear(bool) {}
inline void setInkWidthPct(int) {}
inline void setInkColor(uint32_t) {}
inline void setPackEnabled(bool) {}
inline void setRenderScale(int) {}
inline bool gifTransfer(uint8_t, const uint8_t *, size_t) { return false; }
inline bool coalesce() { return false; }
inline void setSuppressed(bool) {}
inline void privWrite(uint32_t, uint64_t, GSRegisters *) {}
inline void setRegs(GSRegisters *) {}
inline void streamFlip(uint64_t) {}
inline bool gsProfOn() { return false; }
inline void onSwap() {}
inline void setPresentSize(uint32_t, uint32_t) {}
inline bool takeFrame(std::vector<uint8_t> &, uint32_t &, uint32_t &) { return false; }
inline void shutdown() {}
#endif
}
