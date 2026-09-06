#include "runtime/ps2_guestprof.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt4.h"
#include "runtime/ps2_gs_psmt8.h"
#include "ps2_log.h"
#include "ps2_syscalls.h"
#include <map>
#include <set>
#include "runtime/ps2_memory.h"
#include "runtime/ps2_gs_memory.h"
#include "runtime/ps2_gs_gpu_renderer.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

// GIF-packet time profiler (env PS2X_DMAPROF): split processGIFPacket cost into
// image-upload (texture-to-VRAM swizzled writes) vs register/primitive processing.
namespace { std::atomic<uint64_t> g_gifTotalNs{0}, g_gifImageNs{0}, g_gifImageBytes{0};
    const bool g_gifTimeProf = [](){ const char *v = std::getenv("PS2X_DMAPROF"); return v && v[0] && v[0] != '0'; }(); }
#include <mutex>

// DIAGNOSTIC (PS2X_TEX_PROBE): record recent host->local uploads so a textured
// draw can report which uploads (if any) hit the page it samples from.
namespace {
    struct UploadRec { uint32_t dbp, dpsm, dbw, w, h, dsax, dsay; };
    std::mutex g_upMutex;
    std::vector<UploadRec> g_upRecs;
    void recordUpload(uint32_t dbp, uint32_t dpsm, uint32_t dbw, uint32_t w, uint32_t h, uint32_t dsax, uint32_t dsay) {
        std::lock_guard<std::mutex> lk(g_upMutex);
        if (g_upRecs.size() < 20000) g_upRecs.push_back({dbp, dpsm, dbw, w, h, dsax, dsay});
    }
}
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>

// [fmvblit] Defined further down; forward-declared so maybeEmitFmvBlit (in the anonymous
// namespace below) can see them.
extern std::atomic<uint32_t> g_ps2FmvActive;
extern std::atomic<uint32_t> g_fmvPendingFbp;
extern std::atomic<uint32_t> g_fmvPendingBw;
class GS;
extern GS *g_fmvGs;

namespace
{
    // Redundant-upload dedup: hash of the last IMAGE bytes uploaded to each dest block.
    // BT3 re-DMAs its menu/UI textures to VRAM every frame with byte-identical content
    // (~73% of uploads), and each upload is a swizzled per-pixel VRAM write (~262ms/s = the
    // menu fps cap). When the hash matches AND nothing else clobbered the region, the VRAM
    // already holds the data, so the write is skipped. Invalidated by local-to-local
    // transfers into the region (performLocalToLocalTransfer) so a clobber forces a rewrite.
    std::mutex g_uploadHashMx;
    std::unordered_map<uint32_t, uint64_t> g_uploadHash;

    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    // [flippub] Under PS2X_ASYNC_KICK, publish the GPU command list at the DISPFB flip
    // (which for BT3 arrives as a GIF A+D 0x59/0x5b write processed by the kick worker,
    // i.e. HERE, stream-ordered) instead of at the render kick — the kick fires one flip
    // earlier in the stream, so published frames were presented against the PREVIOUS
    // frame's display hint (inverted 0<->112 during Kaioken cutscenes = white scenes).
    // Deduped on fbp change so DISPFB1+DISPFB2 double-writes publish once per real flip.
    // PS2X_FLIP_PUBLISH=0 reverts to the render-kick publish.
    bool flipPublishEnabled()
    {
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_FLIP_PUBLISH"); return !(v && v[0] == '0'); }();
        return s_on && PS2Memory::asyncKickEnabled();
    }
    // [fmvblit] Emit the fullscreen blit the FMV never issues. Called at the flip, BEFORE the
    // publish, so the synthetic sprite is part of the list that gets rendered. Decodes the movie
    // buffer straight out of VRAM (that is where the macroblock uploads put it) and hands it to
    // the renderer as an ordinary textured sprite, which the existing present path then shows.
    void maybeEmitFmvBlit(GS *gs, uint32_t newFbp)
    {
        if (!GsGpuRenderer::enabled() || !gs)
            return;
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_FMVBLIT"); return !(v && v[0] == '0'); }();
        {   // why does this not fire? log the inputs, capped
            static std::atomic<uint32_t> s_d{0};
            if (s_d.fetch_add(1) < 20u)
                std::fprintf(stderr, "[fmvblit:why] flip->fbp=%u fmvActive=%u pendingFbp=%u bw=%u\n",
                             newFbp, ::g_ps2FmvActive.load(std::memory_order_relaxed),
                             ::g_fmvPendingFbp.load(std::memory_order_relaxed),
                             ::g_fmvPendingBw.load(std::memory_order_relaxed));
        }
        if (!s_on || ::g_ps2FmvActive.load(std::memory_order_relaxed) == 0u)
            return;
        if (::g_fmvPendingFbp.load(std::memory_order_relaxed) != newFbp)
            return; // the buffer being scanned out is not the one the movie filled

        const uint32_t bw = std::max(1u, ::g_fmvPendingBw.load(std::memory_order_relaxed));
        const uint32_t w = bw * 64u;
        const uint32_t h = 448u; // BT3's display height; the movie fills the visible field
        const uint32_t bp = newFbp * 32u;

        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t px = gs->ReadVram(0u /*PSMCT32*/, bp, bw, x, y);
                uint8_t *o = rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
                o[0] = static_cast<uint8_t>(px & 0xFFu);
                o[1] = static_cast<uint8_t>((px >> 8) & 0xFFu);
                o[2] = static_cast<uint8_t>((px >> 16) & 0xFFu);
                o[3] = 0xFFu; // scan-out ignores alpha
            }

        const uint64_t key = 0xF00D0000ull | newFbp;
        ps2GpuRenderer().putTexture(key, std::move(rgba), (int)w, (int)h, newFbp, newFbp + 1u);

        GsGpuRenderer::DrawCmd c{};
        c.texKey = key;
        c.isTriangle = false;
        c.destFbp = newFbp;
        c.destFbw = bw;
        c.srcTexW = (int)w; c.srcTexH = (int)h;
        c.sx = 0; c.sy = 0; c.sw = (int)w; c.sh = (int)h;
        c.dx0 = 0.0f; c.dy0 = 0.0f; c.dx1 = (float)w; c.dy1 = (float)h;
        c.su0 = 0.0f; c.sv0 = 0.0f; c.su1 = (float)w; c.sv1 = (float)h;
        c.r = 128; c.g = 128; c.b = 128; c.a = 128; // PS2 neutral modulate
        ps2GpuRenderer().recordCmd(c);

        ::g_fmvPendingFbp.store(0xFFFFFFFFu, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_n{0};
        if (s_n.fetch_add(1) < 4u)
            std::fprintf(stderr, "[fmvblit] movie buffer fbp=%u %ux%u -> synthetic fullscreen blit\n",
                         newFbp, w, h);
    }

    uint32_t s_lastFbpDbg = 0xFFFFFFFFu;
    void maybePublishOnFlip(uint32_t fbp)
    {
        s_lastFbpDbg = fbp;
        if (!flipPublishEnabled())
            return;
        // Publish ONLY on a real flip (scanned-out fbp CHANGE). BT3 rewrites DISPFB
        // every vsync with an unchanged fbp during fights (frame pacing there comes from
        // the render-kick marker, which stays active); publishing on every write emitted
        // a half-frame publish at the mid-frame vsync — doubled present workload (lag)
        // and visibly half-drawn frames. The fbp only changes during display-buffer
        // alternation (Kaioken cutscene scenes), which is exactly where the in-stream
        // publish + hint snapshot must be frame-accurate.
        {
            static std::atomic<uint32_t> s_f{0};
            if (::g_ps2FmvActive.load(std::memory_order_relaxed) && s_f.fetch_add(1) < 20u)
                std::fprintf(stderr, "[fmvblit:flip] DISPFB write fbp=%u (last=%u)\n", fbp, s_lastFbpDbg);
        }
        static uint32_t s_lastFbp = 0xFFFFFFFFu;
        if (fbp == s_lastFbp)
            return;
        s_lastFbp = fbp;
        maybeEmitFmvBlit(::g_fmvGs, fbp); // [fmvblit] before the publish, so it joins this list
        ps2GpuRenderer().swapFrame();
    }

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dx = static_cast<uint32_t>((display64 >> 0) & 0x0FFFu);
        const uint32_t dy = static_cast<uint32_t>((display64 >> 12) & 0x07FFu);
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;

        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }

        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        GSDisplayReadOrigin origin{};
        origin.x = static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu);
        origin.y = static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu);
        return origin;
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        GSPmodeState pmode{};
        pmode.enableCrt1 = (pmode64 & 0x1ull) != 0ull;
        pmode.enableCrt2 = (pmode64 & 0x2ull) != 0ull;
        pmode.mmod = ((pmode64 >> 5) & 0x1ull) != 0ull;
        pmode.amod = ((pmode64 >> 6) & 0x1ull) != 0ull;
        pmode.slbg = ((pmode64 >> 7) & 0x1ull) != 0ull;
        pmode.alp = static_cast<uint8_t>((pmode64 >> 8) & 0xFFu);
        return pmode;
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode264)
    {
        GSSmode2State smode2{};
        smode2.interlaced = (smode264 & 0x1ull) != 0ull;
        smode2.frameMode = ((smode264 >> 1) & 0x1ull) != 0ull;
        return smode2;
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
        {
            return;
        }

        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
            {
                sourceY = height - 1u;
            }

            const uint8_t *srcRow = source.data() + (sourceY * kHostFrameWidth * 4u);
            uint8_t *dstRow = pixels.data() + (y * kHostFrameWidth * 4u);
            std::memcpy(dstRow, srcRow, width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        if (pixels.empty() || width == 0u || height == 0u)
        {
            return;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x * 4u + 3u] = 255u;
            }
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t r = row[x * 4u + 0u];
                const uint8_t g = row[x * 4u + 1u];
                const uint8_t b = row[x * 4u + 2u];
                if (r != 0u || g != 0u || b != 0u)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    bool clearFramebufferRect(GS* gs, const GSContext &ctx, uint32_t rgba)
    {
        if (ctx.frame.fbw == 0u)
        {
            return false;
        }

        const uint32_t stride = GSInternal::fbStride(ctx.frame.fbw, ctx.frame.psm);
        if (stride == 0u)
        {
            return false;
        }

        const u32 x0 = static_cast<u32>(std::max<int>(0, ctx.scissor.x0));
        const u32 x1 = static_cast<u32>(std::max<int>(x0, ctx.scissor.x1));
        const u32 y0 = static_cast<u32>(std::max<int>(0, ctx.scissor.y0));
        const u32 y1 = static_cast<u32>(std::max<int>(y0, ctx.scissor.y1));

        uint8_t r = static_cast<uint8_t>(rgba & 0xFFu);
        uint8_t g = static_cast<uint8_t>((rgba >> 8) & 0xFFu);
        uint8_t b = static_cast<uint8_t>((rgba >> 16) & 0xFFu);
        uint8_t a = static_cast<uint8_t>((rgba >> 24) & 0xFFu);

        u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
        u32 fpsm = ctx.frame.psm;

        if ((ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        if (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
        {
            const uint32_t srcPixel =
                static_cast<uint32_t>(r) |
                (static_cast<uint32_t>(g) << 8) |
                (static_cast<uint32_t>(b) << 16) |
                (static_cast<uint32_t>(a) << 24);

            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    uint32_t pixel = srcPixel;
                    if (ctx.frame.fbmsk != 0u)
                    {
                        const u32 c = gs->ReadVram(fpsm, fbp, fbw, x, y);
                        pixel = (pixel & ~ctx.frame.fbmsk) | (c & ctx.frame.fbmsk);
                    }
                    gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
                }
            }
            return true;
        }

        if (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S)
        {
            const uint16_t srcPixel = encodeFramePixelPSMCT16(r, g, b, a);
            const uint16_t mask = static_cast<uint16_t>(ctx.frame.fbmsk & 0xFFFFu);
            const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
            const uint32_t basePtr = GSInternal::framePageBaseToBlock(ctx.frame.fbp);

            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    uint16_t pixel = srcPixel;
                    if (mask != 0u)
                    {
                        const u16 c = gs->ReadVram(fpsm, fbp, fbw, x, y);
                        pixel = static_cast<uint16_t>((pixel & ~mask) | (c & mask));
                    }
                    gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
                }
            }
            return true;
        }

        return false;
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};
}

using namespace GSInternal;

GS::GS()
{
    using namespace GSMem;

    InitLookupTables();

    for (usz i = 0; i < 0x3F; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_read_vram_funcs[i] = ReadCT32;
            m_write_vram_funcs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_read_vram_funcs[i] = ReadCT24;
            m_write_vram_funcs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_read_vram_funcs[i] = ReadCT16;
            m_write_vram_funcs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_read_vram_funcs[i] = ReadCT16S;
            m_write_vram_funcs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_read_vram_funcs[i] = ReadP8;
            m_write_vram_funcs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_read_vram_funcs[i] = ReadP8H;
            m_write_vram_funcs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_read_vram_funcs[i] = ReadP4;
            m_write_vram_funcs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_read_vram_funcs[i] = ReadP4HH;
            m_write_vram_funcs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_read_vram_funcs[i] = ReadP4HL;
            m_write_vram_funcs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_read_vram_funcs[i] = ReadZ32;
            m_write_vram_funcs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_read_vram_funcs[i] = ReadZ24;
            m_write_vram_funcs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_read_vram_funcs[i] = ReadZ16;
            m_write_vram_funcs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_read_vram_funcs[i] = ReadZ16S;
            m_write_vram_funcs[i] = WriteZ16S;
            break;
        default:
            m_read_vram_funcs[i] = ReadNull;
            m_write_vram_funcs[i] = WriteNull;
            break;
        }
    }

    reset();
}

static uint8_t *g_vramDumpPtr = nullptr;

// [subpix] PS2X_SUBPIXDBG2=1: do FRACTIONAL vertex positions reach the decode at all? BT3's
// outline subtract pass is offset by exactly half a pixel, and by the time the sprite builder
// runs every vertex is integral -- this pins whether the loss is before or after the decode.
static void ps2xSubpixTally(float x, float y)
{
    static const bool s_on = [](){ const char *v = std::getenv("PS2X_SUBPIXDBG2");
                                   return v && v[0] && v[0] != '0'; }();
    if (!s_on) return;
    static std::atomic<unsigned long> nAll{0}, nFrac{0}, nHalfY{0};
    const unsigned long a = nAll.fetch_add(1) + 1ul;
    if (x != std::floor(x) || y != std::floor(y)) nFrac.fetch_add(1);
    if (y - std::floor(y) == 0.5f) nHalfY.fetch_add(1);
    if ((a % 20000ul) == 0ul)
        std::fprintf(stderr, "[subpix] decode: %lu verts | fractional %lu | y half-pixel %lu\n",
                     a, nFrac.load(), nHalfY.load());
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_vram = vram;
    m_vramSize = vramSize;
    m_privRegs = privRegs;
    if (std::getenv("PS2X_VRAM_DUMP") && !g_vramDumpPtr)
    {
        g_vramDumpPtr = vram;
        std::fprintf(stderr, "[vram-dump] armed (vram=%p)\n", (void *)vram);
        std::atexit([]() {
            if (!g_vramDumpPtr)
                return;
            FILE *vf = std::fopen("/home/z3/Desktop/bt3/work/vram.bin", "wb");
            if (vf)
            {
                std::fwrite(g_vramDumpPtr, 1, 4u * 1024u * 1024u, vf);
                std::fclose(vf);
                std::fprintf(stderr, "[vram-dump] wrote 4MB VRAM at exit\n");
            }
        });
    }
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    m_hostPresentationFrame.clear();
    m_hostPresentationWidth = 0u;
    m_hostPresentationHeight = 0u;
    m_hostPresentationDisplayFbp = 0u;
    m_hostPresentationSourceFbp = 0u;
    m_hostPresentationUsedPreferred = false;
    m_hasHostPresentationFrame = false;

    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    if (!m_vram || m_vramSize == 0)
        return;
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.resize(m_vramSize);
    std::memcpy(m_displaySnapshot.data(), m_vram, m_vramSize);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_ctx[0];
    snapshot.ctx[1] = m_ctx[1];
    snapshot.prim = m_prim;
    snapshot.texa = m_texa;
    snapshot.texclut = m_texclut;
    snapshot.bitbltbuf = m_bitbltbuf;
    snapshot.trxpos = m_trxpos;
    snapshot.trxreg = m_trxreg;
    snapshot.trxdir = m_trxdir;
    snapshot.transferX = m_transferState.x;
    snapshot.transferY = m_transferState.y;
    snapshot.transferTotalPixels = m_transferState.total_pixels;
    snapshot.transferCopiedPixels = m_transferState.copied_pixels;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    snapshot.hostPresentationWidth = m_hostPresentationWidth;
    snapshot.hostPresentationHeight = m_hostPresentationHeight;
    snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
    snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
    snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
    snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    snapshot.localToHostPendingBytes = (m_localToHostReadPos < m_localToHostBuffer.size())
                                           ? (m_localToHostBuffer.size() - m_localToHostReadPos)
                                           : 0u;
    return snapshot;
}


// [gshist] The GS debug-history ring (one entry per GIF tag, register write, draw, transfer,
// present) was recorded unconditionally -- ~2% of the guest thread with the F1 panel closed and
// nobody reading it. Record only while a reader holds a lease: getDebugHistory() (the F1 panel's
// per-frame call) extends it 5 s. PS2X_GSHIST=1 = always record (old behaviour).
static std::atomic<uint64_t> g_gsHistLeaseTick{0};
static inline bool gsHistWanted()
{
    static const bool s_force = [](){ const char *v = std::getenv("PS2X_GSHIST"); return v && v[0] && v[0] != '0'; }();
    if (s_force) return true;
    return ps2_syscalls::GetCurrentVSyncTick() <= g_gsHistLeaseTick.load(std::memory_order_relaxed);
}

std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    g_gsHistLeaseTick.store(ps2_syscalls::GetCurrentVSyncTick() + 300u, std::memory_order_relaxed);   // [gshist]

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_prim;
    const uint32_t ci = m_prim.ctxt ? 1u : 0u;
    entry.frame = m_ctx[ci].frame;
    entry.zbuf = m_ctx[ci].zbuf;
    entry.tex0 = m_ctx[ci].tex0;
    entry.scissor = m_ctx[ci].scissor;
    entry.test = m_ctx[ci].test;
    entry.alpha = m_ctx[ci].alpha;
    entry.bitbltbuf = m_bitbltbuf;
    entry.trxpos = m_trxpos;
    entry.trxreg = m_trxreg;
    entry.trxdir = m_trxdir;
    entry.transferPixels = m_transferState.total_pixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    const uint64_t tick = ps2_syscalls::GetCurrentVSyncTick();
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (!gsHistWanted()) return;   // [gshist]
    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (!gsHistWanted()) return;   // [gshist]
    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    if (!gsHistWanted()) return;   // [gshist]
    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    if (!gsHistWanted()) return;   // [gshist]
    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_transferState.total_pixels;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (!gsHistWanted()) return;   // [gshist]
    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

bool GS::copyFrameToHostRgbaUnlocked(const GSFrameReg &frame,
                                     uint32_t width,
                                     uint32_t height,
                                     std::vector<uint8_t> &outPixels,
                                     bool preserveAlpha,
                                     bool useLocalMemoryLayout,
                                     bool frameBaseIsPages,
                                     uint32_t sourceOriginX,
                                     uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
    {
        return false;
    }

    outPixels.resize(kHostFrameWidth * kHostFrameHeight * 4u);
    auto failCopy = [&outPixels]() -> bool
    {
        outPixels.clear();
        return false;
    };

    const uint32_t baseBytes = frameBaseIsPages ? (frame.fbp * 8192u) : (frame.fbp * 256u);
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbwBlocks = frame.fbw ? frame.fbw : (kHostFrameWidth / 64u);
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t strideBytes = fbwBlocks * 64u * bytesPerPixel;

    if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
    {
        const uint32_t srcPixelBytes = (frame.psm == GS_PSM_CT24) ? 3u : 4u;
        if (useLocalMemoryLayout)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = outPixels.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    const uint32_t srcY = sourceOriginY + y;

                    const u32 c = ReadVram(frame.psm, basePtr, fbwBlocks, srcX, srcY);

                    const u32 r = c & 0xFF;
                    const u32 g = (c >> 8) & 0xFF;
                    const u32 b = (c >> 16) & 0xFF;

                    u32 a = 0xFF;
                    if (preserveAlpha && frame.psm != GS_PSM_CT24)
                    {
                        a = (c >> 24) & 0xFF;
                    }

                    dstRow[x * 4u + 0u] = r;
                    dstRow[x * 4u + 1u] = g;
                    dstRow[x * 4u + 2u] = b;
                    dstRow[x * 4u + 3u] = a;
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dstRow = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * srcPixelBytes);
                if (srcOff + srcPixelBytes > m_vramSize)
                {
                    return failCopy();
                }

                dstRow[x * 4u + 0u] = m_vram[srcOff + 0u];
                dstRow[x * 4u + 1u] = m_vram[srcOff + 1u];
                dstRow[x * 4u + 2u] = m_vram[srcOff + 2u];
                dstRow[x * 4u + 3u] =
                    (preserveAlpha && frame.psm != GS_PSM_CT24) ? m_vram[srcOff + 3u] : 255u;
            }
        }
        return true;
    }

    if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
    {
        if (useLocalMemoryLayout)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint32_t dstOff = y * kHostFrameWidth * 4u;
                uint8_t *dst = outPixels.data() + dstOff;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    const uint32_t srcY = sourceOriginY + y;

                    const u16 c = ReadVram(frame.psm, basePtr, fbwBlocks, srcX, srcY);

                    const uint32_t r = c & 31u;
                    const uint32_t g = (c >> 5) & 31u;
                    const uint32_t b = (c >> 10) & 31u;
                    dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                    dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                    dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                    dst[x * 4u + 3u] = preserveAlpha ? ((c & 0x8000u) ? 0x80u : 0x00u) : 255u;
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dst = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * 2u);
                if (srcOff + sizeof(uint16_t) > m_vramSize)
                {
                    return failCopy();
                }

                uint16_t pixel = 0u;
                std::memcpy(&pixel, m_vram + srcOff, sizeof(pixel));
                const uint32_t r = pixel & 31u;
                const uint32_t g = (pixel >> 5) & 31u;
                const uint32_t b = (pixel >> 10) & 31u;
                dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                dst[x * 4u + 3u] = preserveAlpha ? ((pixel & 0x8000u) ? 0x80u : 0x00u) : 255u;
            }
        }
        return true;
    }

    return failCopy();
}

void GS::latchHostPresentationFrame()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    latchHostPresentationFrameUnlocked();
}

bool GS::tryLatchHostPresentationFrame()
{
    if (!m_stateMutex.try_lock())
    {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex, std::adopt_lock);
    latchHostPresentationFrameUnlocked();
    return true;
}

void GS::latchHostPresentationFrameUnlocked()
{
    if (g_vramDumpPtr)
    {
        static uint32_t s_latchCount = 0;
        ++s_latchCount;
        if ((s_latchCount % 50u) == 0u)
            std::fprintf(stderr, "[vram-dump] latch #%u\n", s_latchCount);
        if (s_latchCount >= 200u && (s_latchCount % 100u) == 0u)
        {
            FILE *vf = std::fopen("/home/z3/Desktop/bt3/work/vram.bin", "wb");
            if (vf)
            {
                std::fwrite(g_vramDumpPtr, 1, 4u * 1024u * 1024u, vf);
                std::fclose(vf);
                std::fprintf(stderr, "[vram-dump] wrote 4MB VRAM at latch #%u\n", s_latchCount);
            }
        }
    }
    if (!m_privRegs || !m_vram || m_vramSize == 0u)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    const GSPmodeState pmode = decodePmode(m_privRegs->pmode);
    const GSSmode2State smode2 = decodeSMode2(m_privRegs->smode2);
    // Interlaced field-mode presentation. The old default reproduced the two
    // fields by alternating even/odd source rows each vsync (bob deinterlace),
    // which makes the whole image oscillate up/down by a line every frame --
    // very visible on static UI (logos, popups, banners). BT3 draws a full
    // progressive framebuffer each game-frame, so we WEAVE by default (present
    // all lines) => no bob and full vertical resolution. PS2X_FIELD_BOB=1
    // restores the old alternating-field behavior.
    static const bool s_fieldBob = [](){ const char *v = std::getenv("PS2X_FIELD_BOB"); return v && v[0] && v[0] != '0'; }();
    const bool applyFieldMode = s_fieldBob && smode2.interlaced && !smode2.frameMode;
    const bool oddField = (ps2_syscalls::GetCurrentVSyncTick() & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(m_privRegs->dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(m_privRegs->dispfb2);
    const GSDisplayReadOrigin displayOrigin1 = decodeDisplayReadOrigin(m_privRegs->dispfb1);
    const GSDisplayReadOrigin displayOrigin2 = decodeDisplayReadOrigin(m_privRegs->dispfb2);

    uint32_t width1 = 0u;
    uint32_t height1 = 0u;
    uint32_t width2 = 0u;
    uint32_t height2 = 0u;
    decodeDisplaySize(m_privRegs->display1, width1, height1);
    decodeDisplaySize(m_privRegs->display2, width2, height2);

    const bool validCrt1 = pmode.enableCrt1 && hasDisplaySetup(m_privRegs->display1, displayFrame1);
    const bool validCrt2 = pmode.enableCrt2 && hasDisplaySetup(m_privRegs->display2, displayFrame2);

    auto copyDisplaySource = [&](const GSFrameReg &displayFrame,
                                 const GSDisplayReadOrigin &displayOrigin,
                                 uint32_t width,
                                 uint32_t height,
                                 bool allowPreferred,
                                 bool preserveAlpha,
                                 GSFrameReg &selectedFrame,
                                 std::vector<uint8_t> &scratch,
                                 bool &usedPreferred) -> bool
    {
        selectedFrame = displayFrame;
        scratch.clear();
        usedPreferred = false;

        static const bool s_noPrefer = [](){ const char *v = std::getenv("PS2X_NO_PREFER"); return v && v[0] && v[0] != '0'; }();
        if (s_noPrefer) allowPreferred = false;

        if (allowPreferred &&
            m_hasPreferredDisplaySource &&
            m_preferredDisplayDestFbp == displayFrame.fbp &&
            (m_preferredDisplaySourceFrame.fbw != 0u || m_preferredDisplaySourceFrame.fbp != displayFrame.fbp))
        {
            if (copyFrameToHostRgbaUnlocked(m_preferredDisplaySourceFrame,
                                            width,
                                            height,
                                            scratch,
                                            preserveAlpha,
                                            true,
                                            false,
                                            0u,
                                            0u))
            {
                selectedFrame = m_preferredDisplaySourceFrame;
                usedPreferred = true;
            }
        }

        if (scratch.empty() &&
            !copyFrameToHostRgbaUnlocked(displayFrame,
                                         width,
                                         height,
                                         scratch,
                                         preserveAlpha,
                                         true,
                                         true,
                                         displayOrigin.x,
                                         displayOrigin.y))
        {
            return false;
        }

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(scratch, width, height) == 0u)
        {
            for (int contextIndex = 0; contextIndex < 2; ++contextIndex)
            {
                const GSFrameReg &candidate = m_ctx[contextIndex].frame;
                if (candidate.fbp == selectedFrame.fbp &&
                    candidate.fbw == selectedFrame.fbw &&
                    candidate.psm == selectedFrame.psm)
                {
                    continue;
                }

                std::vector<uint8_t> candidatePixels;
                if (!copyFrameToHostRgbaUnlocked(candidate,
                                                 width,
                                                 height,
                                                 candidatePixels,
                                                 preserveAlpha,
                                                 true,
                                                 true,
                                                 0u,
                                                 0u))
                {
                    continue;
                }

                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                {
                    continue;
                }

                selectedFrame = candidate;
                scratch.swap(candidatePixels);
                break;
            }
        }

        // PS2X_RTVRAM: dump the render-target chain regions from GUEST VRAM (software mode
        // renders everything into m_vram, so this shows the TRUE content of the light/bloom
        // maps that the ground samples — the flat-blue-stage investigation).
        if (std::getenv("PS2X_RTVRAM") && m_vram)
        {
            static uint32_t s_rv = 0;
            if ((++s_rv % 180u) == 90u)
            {
                struct Reg { uint32_t blk, bw, w, h; const char *nm; };
                const Reg regs[] = {
                    {10752u, 4u, 256u, 256u, "fbp336"}, {11776u, 2u, 128u, 128u, "fbp368"},
                    {16064u, 2u, 128u, 128u, "fbp502"}, {7168u, 8u, 512u, 448u, "fbp224"},
                };
                for (const Reg &rg : regs)
                {
                    char path[128];
                    std::snprintf(path, sizeof(path), "/home/z3/Desktop/bt3/work/rtvram_%s.bmp", rg.nm);
                    FILE *f = std::fopen(path, "wb");
                    if (!f) continue;
                    const uint32_t rowBytes = (rg.w * 3 + 3) & ~3u;
                    uint32_t imgSize = rowBytes * rg.h, fsize = 54 + imgSize, off = 54;
                    uint8_t fh[14] = {'B','M'}; std::memcpy(fh+2,&fsize,4); std::memcpy(fh+10,&off,4); std::fwrite(fh,1,14,f);
                    uint8_t ih[40] = {40,0,0,0}; int32_t w2 = (int32_t)rg.w, h2 = (int32_t)rg.h; uint16_t pl=1, bp=24;
                    std::memcpy(ih+4,&w2,4); std::memcpy(ih+8,&h2,4); std::memcpy(ih+12,&pl,2); std::memcpy(ih+14,&bp,2); std::fwrite(ih,1,40,f);
                    std::vector<uint8_t> row(rowBytes, 0);
                    for (int y = (int)rg.h - 1; y >= 0; --y) {
                        for (uint32_t x = 0; x < rg.w; ++x) {
                            const uint32_t px = GSMem::ReadCT32(m_vram, rg.blk, rg.bw, x, (uint32_t)y);
                            row[x*3] = (uint8_t)((px >> 16) & 0xFF); row[x*3+1] = (uint8_t)((px >> 8) & 0xFF); row[x*3+2] = (uint8_t)(px & 0xFF);
                        }
                        std::fwrite(row.data(), 1, rowBytes, f);
                    }
                    std::fclose(f);
                }
                std::fprintf(stderr, "[rtvram] dumped fbp336/368/502/224 regions\n");
            }
        }

        if (std::getenv("PS2X_FB_DUMP") && !scratch.empty() && width > 0 && height > 0)
        {
            static std::atomic<uint32_t> s_fb{0};
            uint32_t n = s_fb.fetch_add(1);
            if ((n % 120u) == 60u) // periodically, a settled frame
            {
                char path[128];
                std::snprintf(path, sizeof(path), "/home/z3/Desktop/bt3/work/fb_%03u.bmp", n);
                FILE *f = std::fopen(path, "wb");
                if (f)
                {
                    const uint32_t rowBytes = (width * 3 + 3) & ~3u;
                    uint32_t imgSize = rowBytes * height, fsize = 54 + imgSize, off = 54;
                    uint8_t fh[14] = {'B','M'}; std::memcpy(fh+2,&fsize,4); std::memcpy(fh+10,&off,4); std::fwrite(fh,1,14,f);
                    uint8_t ih[40] = {40,0,0,0}; int32_t w = width, h = height; uint16_t pl=1, bp=24;
                    std::memcpy(ih+4,&w,4); std::memcpy(ih+8,&h,4); std::memcpy(ih+12,&pl,2); std::memcpy(ih+14,&bp,2); std::fwrite(ih,1,40,f);
                    std::vector<uint8_t> row(rowBytes, 0);
                    for (int y = height - 1; y >= 0; --y) {
                        for (int x = 0; x < width; ++x) {
                            const uint8_t *p = &scratch[(y * width + x) * 4];
                            row[x*3] = p[2]; row[x*3+1] = p[1]; row[x*3+2] = p[0];
                        }
                        std::fwrite(row.data(), 1, rowBytes, f);
                    }
                    std::fclose(f);
                    std::cerr << "[fb-dump] wrote " << path << " " << width << "x" << height
                              << " dispFbp=" << displayFrame.fbp << " selFbp=" << selectedFrame.fbp
                              << " usedPreferred=" << usedPreferred << std::endl;
                }
                // Also dump the OTHER buffer (fbp=112) to see if the text lives there.
                std::vector<uint8_t> other;
                GSFrameReg f112 = selectedFrame; f112.fbp = 112u;
                if (copyFrameToHostRgbaUnlocked(f112, width, height, other, false, true, false, 0u, 0u) && !other.empty())
                {
                    char p2[128]; std::snprintf(p2, sizeof(p2), "/home/z3/Desktop/bt3/work/fbB_%03u.bmp", n);
                    FILE *f2 = std::fopen(p2, "wb");
                    if (f2) {
                        const uint32_t rb = (width*3+3)&~3u; uint32_t is=rb*height, fs=54+is, of=54;
                        uint8_t fh[14]={'B','M'}; std::memcpy(fh+2,&fs,4); std::memcpy(fh+10,&of,4); std::fwrite(fh,1,14,f2);
                        uint8_t ih[40]={40,0,0,0}; int32_t w=width,h=height; uint16_t pl=1,bp=24; std::memcpy(ih+4,&w,4);std::memcpy(ih+8,&h,4);std::memcpy(ih+12,&pl,2);std::memcpy(ih+14,&bp,2); std::fwrite(ih,1,40,f2);
                        std::vector<uint8_t> row(rb,0);
                        for (int y=height-1;y>=0;--y){ for(int x=0;x<width;++x){ const uint8_t*p=&other[(y*width+x)*4]; row[x*3]=p[2];row[x*3+1]=p[1];row[x*3+2]=p[0];} std::fwrite(row.data(),1,rb,f2);} std::fclose(f2);
                        std::cerr << "[fb-dumpB] wrote " << p2 << " (fbp=112)" << std::endl;
                    }
                }
            }
        }

        return true;
    };

    if (!validCrt1 && !validCrt2)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    if (validCrt1 && validCrt2)
    {
        GSFrameReg selectedFrame1{};
        GSFrameReg selectedFrame2{};
        std::vector<uint8_t> rc1;
        std::vector<uint8_t> rc2;
        bool usedPreferred1 = false;
        bool usedPreferred2 = false;

        const bool copiedCrt1 = copyDisplaySource(displayFrame1, displayOrigin1, width1, height1, false, true, selectedFrame1, rc1, usedPreferred1);
        const bool copiedCrt2 = copyDisplaySource(displayFrame2, displayOrigin2, width2, height2, false, true, selectedFrame2, rc2, usedPreferred2);

        if (copiedCrt1 && copiedCrt2)
        {
            const uint32_t width = std::max(width1, width2);
            const uint32_t height = std::max(height1, height2);
            const uint8_t bgR = static_cast<uint8_t>(m_privRegs->bgcolor & 0xFFu);
            const uint8_t bgG = static_cast<uint8_t>((m_privRegs->bgcolor >> 8) & 0xFFu);
            const uint8_t bgB = static_cast<uint8_t>((m_privRegs->bgcolor >> 16) & 0xFFu);
            const uint8_t bgA = pmode.alp;

            std::vector<uint8_t> merged(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    dstRow[x * 4u + 0u] = bgR;
                    dstRow[x * 4u + 1u] = bgG;
                    dstRow[x * 4u + 2u] = bgB;
                    dstRow[x * 4u + 3u] = bgA;
                }
            }

            if (!pmode.slbg)
            {
                for (uint32_t y = 0; y < height2; ++y)
                {
                    const uint8_t *srcRow = rc2.data() + (y * kHostFrameWidth * 4u);
                    uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0; x < width2; ++x)
                    {
                        dstRow[x * 4u + 0u] = srcRow[x * 4u + 0u];
                        dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                        dstRow[x * 4u + 2u] = srcRow[x * 4u + 2u];
                        dstRow[x * 4u + 3u] = srcRow[x * 4u + 3u];
                    }
                }
            }

            for (uint32_t y = 0; y < height1; ++y)
            {
                const uint8_t *srcRow = rc1.data() + (y * kHostFrameWidth * 4u);
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t srcR = srcRow[x * 4u + 0u];
                    const uint8_t srcG = srcRow[x * 4u + 1u];
                    const uint8_t srcB = srcRow[x * 4u + 2u];
                    const uint8_t srcA = srcRow[x * 4u + 3u];
                    const uint8_t dstR = dstRow[x * 4u + 0u];
                    const uint8_t dstG = dstRow[x * 4u + 1u];
                    const uint8_t dstB = dstRow[x * 4u + 2u];
                    const uint8_t dstA = dstRow[x * 4u + 3u];
                    const uint32_t factor = pmode.mmod
                                                ? static_cast<uint32_t>(pmode.alp)
                                                : std::min<uint32_t>(255u, static_cast<uint32_t>(srcA) * 2u);

                    dstRow[x * 4u + 0u] = blendPresentationChannel(srcR, dstR, factor);
                    dstRow[x * 4u + 1u] = blendPresentationChannel(srcG, dstG, factor);
                    dstRow[x * 4u + 2u] = blendPresentationChannel(srcB, dstB, factor);
                    dstRow[x * 4u + 3u] = pmode.amod ? dstA : srcA;
                }
            }

            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *row = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    row[x * 4u + 3u] = 255u;
                }
            }

            if (applyFieldMode)
            {
                applyFieldPresentation(merged, width, height, oddField);
            }

            m_hostPresentationFrame.swap(merged);
            m_hostPresentationWidth = width;
            m_hostPresentationHeight = height;
            m_hostPresentationDisplayFbp = displayFrame1.fbp;
            m_hostPresentationSourceFbp = selectedFrame1.fbp;
            m_hostPresentationUsedPreferred = false;
            m_hasHostPresentationFrame = true;
            recordPresentDebugEventUnlocked(m_hostPresentationDisplayFbp,
                                            m_hostPresentationSourceFbp,
                                            m_hostPresentationWidth,
                                            m_hostPresentationHeight,
                                            m_hostPresentationUsedPreferred);
            return;
        }
    }

    const GSFrameReg &displayFrame = validCrt1 ? displayFrame1 : displayFrame2;
    const uint32_t width = validCrt1 ? width1 : width2;
    const uint32_t height = validCrt1 ? height1 : height2;

    GSFrameReg selectedFrame = displayFrame;
    std::vector<uint8_t> scratch;
    bool usedPreferred = false;
    const GSDisplayReadOrigin &displayOrigin = validCrt1 ? displayOrigin1 : displayOrigin2;
    if (!copyDisplaySource(displayFrame, displayOrigin, width, height, true, false, selectedFrame, scratch, usedPreferred))
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = displayFrame.fbp;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    if (applyFieldMode)
    {
        applyFieldPresentation(scratch, width, height, oddField);
    }

    normalizePresentationAlpha(scratch, width, height);

    m_hostPresentationFrame.swap(scratch);
    m_hostPresentationWidth = width;
    m_hostPresentationHeight = height;
    m_hostPresentationDisplayFbp = displayFrame.fbp;
    m_hostPresentationSourceFbp = selectedFrame.fbp;
    m_hostPresentationUsedPreferred = usedPreferred;
    m_hasHostPresentationFrame = true;
    recordPresentDebugEventUnlocked(m_hostPresentationDisplayFbp,
                                    m_hostPresentationSourceFbp,
                                    m_hostPresentationWidth,
                                    m_hostPresentationHeight,
                                    m_hostPresentationUsedPreferred);
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.resize(packedRowBytes * static_cast<size_t>(outHeight));
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

// Set by the rasterizer (PS2X_TEX3D [maphash] block) once a 3D map draw has been seen;
// PS2X_CWATCH_FIGHT uses it to hold upload-content dumps until fight-era streaming starts.
std::atomic<bool> g_ps2xMapDrawSeen{false};

// [fmvphase] Set by sceMpegGetPicture; lets the VRAM probes filter to the actual movie.
std::atomic<uint32_t> g_ps2FmvActive{0u};
// [fmvblit] framebuffer currently being filled by movie macroblocks, and its FBW.
std::atomic<uint32_t> g_fmvPendingFbp{0xFFFFFFFFu};
std::atomic<uint32_t> g_fmvPendingBw{0u};
GS *g_fmvGs = nullptr; // [fmvblit] set on the first image upload; the emitter reads VRAM via it

// ---- FBO -> VRAM writeback -------------------------------------------------------------
// GPU mode never writes rendered pixels back to VRAM, so a texture whose source region is a
// render target cannot be decoded again once dropped -- that is the arena-corruption bug.
// Measured on this build: 120k indexed draws per match sample RENDERED pages, 94% of them
// fbp224 (a 1024x1024 mask sampled as PSMT8 indices through a CLUT). Handing those bytes back
// to VRAM makes the whole class ordinary: re-decodable, and therefore safely evictable.
GS *g_gsWb = nullptr;
// ZBUF of the current frame, published by the rasterizer for the depth writeback.
uint32_t g_zwbBp = 0, g_zwbPsm = 0x31u, g_zwbBw = 8u;
double g_zwbZMax = 4294967295.0;   // zNorm's divisor, published by the rasterizer
// Write our GL depth buffer back into VRAM as GS Z. Once these bytes are in VRAM, every aliased
// re-view of them (PSMZ32 reads, the CT16 512x896 column view, PSMT8H index reads) decodes
// correctly with no per-view special case.
// [livesync] Present-thread staging: in LIVE mode the FBO can only be read on the GL thread,
// but VRAM must only change at a defined point in the GUEST command stream (mid-stream writes
// are what flickered the whole frame). The present thread runs flushPageToVram with
// g_stageWrites set: these functions then CAPTURE the fully-masked write instead of applying
// it, and the guest drains the captures at swapFrame (its frame boundary) by calling them
// again normally. Same masks, same code path, correct thread for each half.
struct StagedWb
{
    uint32_t fbp, fbw, psm; int w, h; uint32_t fbmsk;
    bool isDepth; double zMax;
    std::vector<uint32_t> px; std::vector<float> depth;
};
std::mutex g_stageWbMx;
std::vector<StagedWb> g_stagedWb;
thread_local bool g_stageWrites = false;

void ps2xWritebackDepthToVram(uint32_t zbp, uint32_t zbw, uint32_t zpsm, int w, int h,
                              const float *depth, double zMax)
{
    if (g_stageWrites)
    {
        std::lock_guard<std::mutex> lk(g_stageWbMx);
        for (auto &e : g_stagedWb)
            if (e.isDepth && e.fbp == zbp)
            { e.fbw = zbw; e.psm = zpsm; e.w = w; e.h = h; e.zMax = zMax;
              e.depth.assign(depth, depth + (size_t)w * h); return; }
        StagedWb e; e.fbp = zbp; e.fbw = zbw; e.psm = zpsm; e.w = w; e.h = h;
        e.fbmsk = 0u; e.isDepth = true; e.zMax = zMax;
        e.depth.assign(depth, depth + (size_t)w * h);
        g_stagedWb.push_back(std::move(e));
        return;
    }
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    if (!gs || !depth || w <= 0 || h <= 0) return;
    const uint32_t base = GSInternal::framePageBaseToBlock(zbp);
    const uint32_t bw = zbw ? zbw : 1u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            double d = depth[(size_t)y * w + x];
            if (d < 0.0) d = 0.0; else if (d > 1.0) d = 1.0;
            const uint32_t z = (uint32_t)(d * zMax + 0.5);
            gs->WriteVram(zpsm, base, bw, (uint32_t)x, (uint32_t)y, z);
        }
    // A palette can BE a render target (BT3's outline CLUTs live in pages 499-500, which the
    // game renders into). The CLUT cache keys on m_texUploadGen, which only processImageData
    // bumps -- so without this a flushed palette would never be re-read.
    gs->bumpTexUploadGen();
    ps2GpuRenderer().onVramWriteback(base, (uint32_t)(((size_t)w * h * 4u) / 256u)); gs->bumpPageUploadGen(base, (uint32_t)(((size_t)w * h * 4u) / 256u));   // [clutpagegen]
}
// [wbpack16] GPU->VRAM writebacks used to hand WriteVram the raw RGBA8888 word for 16-bit
// pages, so a CT16 pixel got the low 16 bits (R&31 | G<<8...) instead of R5G5B5A1 -- G landed
// in B5 (>>2), B and A were dropped, and every CT16 flush wrote garbage colour bits (the reason
// PS2X_FLUSHCT32 was needed for the scene pages). Pack/unpack like the SW rasterizer does.
// PS2X_WBPACK16=0 restores the raw behaviour for A/B.
static inline bool wbIs16(uint32_t psm) { return psm == GS_PSM_CT16 || psm == GS_PSM_CT16S; }
static inline uint32_t wbPack16(uint32_t c)
{ return ((c >> 3) & 0x1Fu) | (((c >> 11) & 0x1Fu) << 5) | (((c >> 19) & 0x1Fu) << 10) | (((c >> 31) & 1u) << 15); }
static inline uint32_t wbUnpack16(uint32_t v)
{ const uint32_t r = v & 0x1Fu, g = (v >> 5) & 0x1Fu, b = (v >> 10) & 0x1Fu, a = (v >> 15) & 1u;
  return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | ((a ? 0x80u : 0u) << 24); }
int g_wbRectX0 = -1, g_wbRectY0 = -1, g_wbRectX1 = -1, g_wbRectY1 = -1;   // [flushrect] -1 = whole buffer
int g_wbLastWrittenRows = 0;   // [flushdiff] rows written by the last masked writeback
const uint8_t *g_wbSkipMask = nullptr;   // [flushdiff] per-pixel: 0 = unchanged since the last flush, skip the write
static const bool s_wbPack16 = [](){ const char *v = std::getenv("PS2X_WBPACK16"); return !(v && v[0] == '0'); }();
// Masked variant: GS FBMSK protects bits, so a writeback that ignores it destroys data the
// hardware keeps. BT3's CT16 stripes pass writes fbp0 through FBMSK=0x00003fff -- bits 0..13 are
// preserved -- so a full-pixel writeback zeroes most of the surface.
// PS2X_RTTEST=1: round-trip the two address paths that disagree. The barrier WRITES the
// scene through PSMCT32 (`WriteVram(psm=0, framePageBaseToBlock(fbp), bw=fbw, x, y)`) and the
// mask build READS the same page through PSMT8H (`ReadVram(PSMT8H, tbp0, tbw, u, v)`).
// PSMT8H must alias PSMCT32 exactly -- same page/block layout, taking byte 3 -- so writing a
// known ramp and reading it back at the same (x,y) says which side is wrong. No game data.
// Read page `fbp` back through the SAME call the decode uses (ReadVram PSMT8H) right after a
// flush, and report the alpha-byte distribution. If the flush wrote a dense field and this
// still reads ~0, the disagreement is inside ReadVram's PSMT8H path for this exact state.
void ps2xVramReadBackT8H(uint32_t fbp, uint32_t tbw)
{
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    if (!gs) return;
    const uint32_t tbp0 = GSInternal::framePageBaseToBlock(fbp);
    unsigned hist[256] = {0}; long n = 0;
    for (int y = 0; y < 448; y += 7)
        for (int x = 0; x < 512; x += 5)
        { ++hist[gs->ReadVram(GS_PSM_T8H, tbp0, tbw, (uint32_t)x, (uint32_t)y) & 0xFFu]; ++n; }
    int distinct = 0; unsigned top = 0; int topV = 0; double sum = 0;
    for (int i = 0; i < 256; ++i) if (hist[i]) { ++distinct; sum += (double)i * hist[i];
        if (hist[i] > top) { top = hist[i]; topV = i; } }
    std::fprintf(stderr, "[readback] fbp%u tbp0=%u tbw=%u : index mean %.1f  zero %.1f%%  distinct %d  top %d(%.1f%%)\n",
                 fbp, tbp0, tbw, sum / (double)n, 100.0 * (double)hist[0] / (double)n, distinct, topV,
                 100.0 * (double)top / (double)n);
}

// [livesync] guest-side drain: apply everything the present thread staged, at the caller's
// (guest) stream position. Runs the SAME writeback functions with staging off.
unsigned long g_ps2xWbGen = 0;
unsigned long g_wbPixelsWritten = 0;   // [shstat]
const std::pair<int,int> *g_wbRowRange = nullptr;   // [flushrows] per-row [x0,x1) to write, or null
int g_wbFlipY = 0;   // [noflip] 1 = px rows are bottom-up (buffer row for VRAM row y is h-1-y)   // [hashmemo] bumped by every VRAM writeback (WBSTAMP is off by default, so m_contentSeq does not see them)

static void wbHudLog(const char *who, uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h, uint32_t base, uint32_t bw);   // [wbhud]
void ps2xWritebackToVramMasked(uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h,
                               const uint32_t *px, uint32_t fbmsk);
void ps2xApplyStagedWritebacks()
{
    ++g_ps2xWbGen;   // [hashmemo] any VRAM writeback invalidates memoised range hashes
    { GS *gsc = g_gsWb ? g_gsWb : g_fmvGs; if (gsc) gsc->invalidateClutCache(); }   // [clutwb] a flushed page may hold a rendered palette: force the CLUT cache to re-decode

    std::vector<StagedWb> work;
    {
        std::lock_guard<std::mutex> lk(g_stageWbMx);
        if (g_stagedWb.empty()) return;
        work.swap(g_stagedWb);
    }
    for (const auto &e : work)
    {
        if (e.isDepth) ps2xWritebackDepthToVram(e.fbp, e.fbw, e.psm, e.w, e.h, e.depth.data(), e.zMax);
        else           ps2xWritebackToVramMasked(e.fbp, e.fbw, e.psm, e.w, e.h, e.px.data(), e.fbmsk);
    }
}

// PS2X_CT16MAP=1: which CT32 bits does a PSMCT16 write actually touch? BT3's alpha rebuild
// writes the scene RE-VIEWED as CT16 with FBMSK=0x00003fff (CT16 bits 14-15). Our SW pass
// reproduces the draws but only reaches ~25% of the CT32 alpha bytes at ~2 bits, while
// console's refill is a smooth full-coverage 256-value field. This probes the real mapping.
void ps2xCt16MapProbe()
{
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    if (!gs) return;
    const uint32_t base = GSInternal::framePageBaseToBlock(0u);
    // Clear a small CT32 window, then set ONE CT16 pixel's bits 14-15 and see what moved.
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x)
            gs->WriteVram(0u, base, 8u, (uint32_t)x, (uint32_t)y, 0u);
    for (int cy = 0; cy < 2; ++cy)
    {
        for (int cx = 0; cx < 16; ++cx)
        {
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 16; ++x)
                    gs->WriteVram(0u, base, 8u, (uint32_t)x, (uint32_t)y, 0u);
            gs->WriteVram(GS_PSM_CT16, base, 8u, (uint32_t)cx, (uint32_t)cy, 0xC000u); // bits 14,15
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 16; ++x)
                {
                    const uint32_t v = gs->ReadVram(0u, base, 8u, (uint32_t)x, (uint32_t)y);
                    if (v)
                        std::fprintf(stderr, "[ct16map] CT16(%d,%d) bits14-15 -> CT32(%d,%d) = %08x%s\n",
                                     cx, cy, x, y, v, (v & 0xFF000000u) ? "   [ALPHA byte]" : "");
                }
        }
    }
}

// PS2X_F336PRE=1: what does VRAM page `fbp` hold, read as CT16, at the moment it is called?
// The edge detect's two SUBTRACTIVE passes (blend 0x62) clamp to zero on a zero destination,
// and page 336 IS the stage-texture atlas (block 10752), so on console Cd is real texture.
void ps2xVramCt16Dump(uint32_t fbp, uint32_t fbw, const char *path)
{   // page `fbp` read as PSMCT16 (512x448) -> RGB png via raylib's ExportImage
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs; if (!gs) return;
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    std::vector<uint8_t> px((size_t)512 * 448 * 4);
    for (int y = 0; y < 448; ++y) for (int x = 0; x < 512; ++x)
    {   const uint32_t v = gs->ReadVram(GS_PSM_CT16, base, fbw, (uint32_t)x, (uint32_t)y) & 0xFFFFu;
        uint8_t *o = &px[((size_t)y * 512 + x) * 4];
        o[0] = (uint8_t)((v & 0x1F) << 3); o[1] = (uint8_t)(((v >> 5) & 0x1F) << 3); o[2] = (uint8_t)(((v >> 10) & 0x1F) << 3); o[3] = 255; }
    if (FILE *f = std::fopen(path, "wb"))
    {   std::fprintf(f, "P6\n512 448\n255\n");
        for (size_t i = 0; i < (size_t)512 * 448; ++i) std::fwrite(&px[i * 4], 1, 3, f);
        std::fclose(f); std::fprintf(stderr, "[ct16dump] wrote %s\n", path); }
}
void ps2xVramT8HDump(uint32_t fbp, uint32_t fbw, const char *path)
{   // page `fbp` read as PSMT8H (512x448 index bytes) -> P5 pgm
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs; if (!gs) return;
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    std::vector<uint8_t> px((size_t)512 * 448);
    for (int y = 0; y < 448; ++y) for (int x = 0; x < 512; ++x) px[(size_t)y * 512 + x] = (uint8_t)(gs->ReadVram(GS_PSM_T8H, base, fbw, (uint32_t)x, (uint32_t)y) & 0xFFu);
    if (FILE *f = std::fopen(path, "wb")) { std::fprintf(f, "P5\n512 448\n255\n"); std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); std::fprintf(stderr, "[t8hdump] wrote %s\n", path); }
}
void ps2xVramCt16Stats(uint32_t fbp, uint32_t fbw, const char *when)
{
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    if (!gs) return;
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    long n = 0, nz = 0; double sum = 0;
    for (int y = 0; y < 448; y += 7)
        for (int x = 0; x < 512; x += 5)
        {
            const uint32_t v = gs->ReadVram(GS_PSM_CT16, base, fbw, (uint32_t)x, (uint32_t)y) & 0xFFFFu;
            ++n; if (v & 0x7FFFu) ++nz; sum += (double)(v & 0x7FFFu);
        }
    std::fprintf(stderr, "[f336pre] %s fbp%u (CT16 fbw=%u): non-black %.1f%%  mean rgb-bits %.1f\n",
                 when, fbp, fbw, 100.0 * (double)nz / (double)n, sum / (double)n);
}

void ps2xVramRoundTripTest()
{
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    if (!gs) { std::fprintf(stderr, "[rttest] no GS\n"); return; }
    struct Case { const char *name; uint32_t fbp; uint32_t tbp0; uint32_t bw; };
    const Case cases[] = {
        { "fbp0   tbp0=0",    0u,   0u, 8u },
        { "fbp112 tbp0=3584", 112u, 3584u, 8u },
    };
    for (const Case &c : cases)
    {
        const uint32_t base = GSInternal::framePageBaseToBlock(c.fbp);
        int match = 0, total = 0, firstBadX = -1, firstBadY = -1;
        uint32_t gotFirstBad = 0, wantFirstBad = 0;
        for (int y = 0; y < 448; y += 37)
            for (int x = 0; x < 512; x += 41)
            {
                const uint8_t a = (uint8_t)(((x * 7 + y * 13) & 0x7F) + 1);
                gs->WriteVram(0u, base, c.bw, (uint32_t)x, (uint32_t)y, ((uint32_t)a << 24) | 0x00123456u);
                const uint32_t got = gs->ReadVram(GS_PSM_T8H, c.tbp0, c.bw, (uint32_t)x, (uint32_t)y);
                ++total;
                if ((got & 0xFFu) == a) ++match;
                else if (firstBadX < 0) { firstBadX = x; firstBadY = y; gotFirstBad = got & 0xFFu; wantFirstBad = a; }
            }
        std::fprintf(stderr, "[rttest] %s base=%u : CT32 write -> PSMT8H read matches %d/%d",
                     c.name, base, match, total);
        if (firstBadX >= 0)
            std::fprintf(stderr, "   first mismatch at (%d,%d): got %u want %u", firstBadX, firstBadY,
                         gotFirstBad, wantFirstBad);
        std::fprintf(stderr, "\n");
    }
}

void ps2xVramAlphaStats(const char *tag, uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h)
{
    // What does the scene buffer's ALPHA actually look like in VRAM? That byte is the palette
    // INDEX every outline/shadow composite reads, so its coverage is the single number that
    // says whether the rebuild pass did anything. Console reaches ~62.75% on this pass.
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    if (!gs || w <= 0 || h <= 0) return;
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    const uint32_t bw = fbw ? fbw : 1u;
    unsigned hist[256] = {0};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            ++hist[(gs->ReadVram(psm, base, bw, (uint32_t)x, (uint32_t)y) >> 24) & 0xFFu];
    const double n = (double)w * h;
    int distinct = 0; unsigned top = 0; int topV = 0;
    for (int i = 0; i < 256; ++i) if (hist[i]) { ++distinct; if (hist[i] > top) { top = hist[i]; topV = i; } }
    {   // Also write the field itself, so it can be put next to console's
        // itexraw_00000_P_8H dump (which IS this same byte, read as a palette index).
        static int s_n = 0;
        const char *dir = std::getenv("PS2X_GS_REPLAY_OUT");
        if (dir && s_n < 6)
        {
            char path[256];
            std::snprintf(path, sizeof(path), "%s/vramalpha_f%u_%d.raw", dir, fbp, s_n++);
            if (FILE *f = std::fopen(path, "wb"))
            {
                int hdr[2] = { w, h };
                std::fwrite(hdr, sizeof(hdr), 1, f);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const uint8_t a = (uint8_t)((gs->ReadVram(psm, base, bw, (uint32_t)x, (uint32_t)y) >> 24) & 0xFFu);
                        std::fwrite(&a, 1, 1, f);
                    }
                std::fclose(f);
            }
        }
    }
    {   // The rebuild pass writes bits 14-15 of each PSMCT16 pixel; half of those land in the
        // CT32 GREEN byte, which is visible colour. If our VRAM scene RGB shows 8-px bands
        // aligned to that pass's write pattern, the pass is painting colour it must not.
        double gW = 0, gO = 0; long nW = 0, nO = 0;
        for (int y = 0; y < h; ++y)
            for (int xx = 0; xx < w; ++xx)
            {
                const uint32_t px2 = gs->ReadVram(psm, base, bw, (uint32_t)xx, (uint32_t)y);
                const double g = (double)((px2 >> 8) & 0xFFu);
                if (((xx / 8) & 1) != 0) { gW += g; ++nW; } else { gO += g; ++nO; }
            }
        std::fprintf(stderr, "[vramrgb] %s fbp%u green: written-bands %.2f  other %.2f  delta %+.2f\n",
                     tag, fbp, nW ? gW / nW : 0.0, nO ? gO / nO : 0.0,
                     (nW && nO) ? (gW / nW - gO / nO) : 0.0);
    }
    std::fprintf(stderr, "[vramalpha] %s fbp%u psm=%02x %dx%d: a!=0 %.2f%%  a>=128 %.2f%%  vals=%d top=%d(%.1f%%)\n",
                 tag, fbp, psm, w, h,
                 100.0 * (n - hist[0]) / n,
                 100.0 * [&]{ double c = 0; for (int i = 128; i < 256; ++i) c += hist[i]; return c; }() / n,
                 distinct, topV, 100.0 * top / n);
}

// PS2X_BARMERGEA: fill the scene's VRAM alpha from the FBO only where VRAM alpha is still
// ZERO. The rebuild pass writes ~25% of the alpha bytes (8-px bands); the geometry's alpha --
// which console already has in that memory -- lives in our FBO. Protecting alpha entirely
// (BARKEEPA) keeps only the bands, and writing it wholesale destroys them. Filling the gaps
// keeps both, with no dependence on when the game's full-screen alpha wipe lands.
bool g_wbAlphaFillOnly = false;

// Inverse of ps2xWritebackToVramMasked: read a framebuffer page OUT of VRAM into a linear
// RGBA buffer, top-down. Needed because BT3's cel/outline pass has to run in the SOFTWARE
// rasterizer (its triangles are sub-pixel slivers that GL's top-left fill rule drops -- see
// the coverage census: we lose 55.8% of the top-u band), and the software path works in VRAM
// while the scene it draws into lives in a GPU FBO. The result has to come back.
void ps2xReadbackFromVram(uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h, uint32_t *px)
{
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    if (!gs || !px || w <= 0 || h <= 0) return;
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    const uint32_t bw = fbw ? fbw : 1u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            px[(size_t)y * w + x] = gs->ReadVram(psm, base, bw, (uint32_t)x, (uint32_t)y);
}

// [flushrectchk] count pixels OUTSIDE [x0,x1)x[y0,y1) where the FBO (px) differs from VRAM;
// returns the count and the bbox of the misses.
extern "C" unsigned ps2xVramDiffOutside(uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h, const uint32_t *px,
                                        int x0, int y0, int x1, int y1, int *bx0, int *by0, int *bx1, int *by1)
{
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs; if (!gs) return 0u;
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp); const uint32_t bw = fbw ? fbw : 1u;
    const bool p16 = s_wbPack16 && wbIs16(psm); unsigned n = 0; *bx0 = *by0 = 1 << 20; *bx1 = *by1 = -1;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
    {
        if (x >= x0 && x < x1 && y >= y0 && y < y1) continue;
        const uint32_t nv = px[(size_t)y * w + x]; const uint32_t ov = gs->ReadVram(psm, base, bw, (uint32_t)x, (uint32_t)y);
        const bool diff = p16 ? ((ov & 0xFFFFu) != wbPack16(nv)) : (psm == 1u ? ((ov ^ nv) & 0xFFFFFFu) != 0u : ov != nv);
        if (diff) { ++n; *bx0 = std::min(*bx0, x); *by0 = std::min(*by0, y); *bx1 = std::max(*bx1, x); *by1 = std::max(*by1, y); }
    }
    return n;
}
// [ddmirror] when set, every pixel the masked writeback lands in VRAM is ALSO written into this buffer (same swizzle,
// no page-skip mask): the deferred decode's private copy = post-time snapshots + the read-time flush, live VRAM untouched.
uint8_t *g_wbMirror = nullptr;
// [linvram] when set, every value the writeback stores also lands in this LINEAR image (row-major, stride g_wbLinearStride)
uint32_t *g_wbLinear = nullptr; int g_wbLinearStride = 0;
void ps2xWritebackToVramMasked(uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h,
                               const uint32_t *px, uint32_t fbmsk)
{
    wbHudLog("masked", fbp, fbw, psm, w, h, GSInternal::framePageBaseToBlock(fbp), fbw ? fbw : 1u);
    {   // [pagelog] writebacks whose page range touches PS2X_PAGELOG=lo-hi
        static const std::pair<int,int> s_pl = [](){ std::pair<int,int> r{-1,-1}; if (const char *v = std::getenv("PS2X_PAGELOG")) std::sscanf(v, "%d-%d", &r.first, &r.second); return r; }();
        if (s_pl.first >= 0)
        {
            const int rowBytes = (int)(fbw ? fbw : 1u) * 64 * ((psm == 2u || psm == 10u) ? 2 : 4);
            const int lo = (int)fbp, hi = (int)fbp + (int)(((long)h * rowBytes + 8191) / 8192) - 1;
            if (hi >= s_pl.first && lo <= s_pl.second) { static unsigned long n = 0; if (n++ < 400) std::fprintf(stderr, "[pagelog] WRITEBACK fbp=%u pages %d-%d psm=%u %dx%d fbmsk=%08x\n", fbp, lo, hi, psm, w, h, fbmsk); }
        }
    }
    ++g_ps2xWbGen;   // [hashmemo] any VRAM writeback invalidates memoised range hashes
    { GS *gsc = g_gsWb ? g_gsWb : g_fmvGs; if (gsc) gsc->invalidateClutCache(); }   // [clutwb] a flushed page may hold a rendered palette: force the CLUT cache to re-decode

    if (g_stageWrites)
    {
        std::lock_guard<std::mutex> lk(g_stageWbMx);
        for (auto &e : g_stagedWb)
            if (!e.isDepth && e.fbp == fbp)
            { e.fbw = fbw; e.psm = psm; e.w = w; e.h = h; e.fbmsk = fbmsk;
              e.px.assign(px, px + (size_t)w * h); return; }
        StagedWb e; e.fbp = fbp; e.fbw = fbw; e.psm = psm; e.w = w; e.h = h;
        e.fbmsk = fbmsk; e.isDepth = false; e.zMax = 0.0;
        e.px.assign(px, px + (size_t)w * h);
        g_stagedWb.push_back(std::move(e));
        return;
    }
    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    {   // Which GS does the writeback land in? The decode reads the `gs` the rasterizer holds.
        static const bool s_gi = [](){ const char *v = std::getenv("PS2X_GSID");
                                       return v && v[0] && v[0] != '0'; }();
        if (s_gi && gs) { static int n = 0; if (n++ < 3)
            std::fprintf(stderr, "[gsid] writeback gs=%p fbp=%u\n", (void*)gs, fbp); }
    }
    if (!gs || !px || w <= 0 || h <= 0) return;
    {   // Who writes the scene pages, and through what mask? The software alpha rebuild lives
        // in the alpha byte of page 0/112; any writeback of those pages with fbmsk==0 erases it.
        static const bool s_wd = [](){ const char *v = std::getenv("PS2X_WBDIAG");
                                       return v && v[0] && v[0] != '0'; }();
        if (s_wd && (fbp == 0u || fbp == 112u))
        {
            static int n = 0;
            if (n++ < 20)
                std::fprintf(stderr, "[wbdiag] writeback fbp%u psm=%02x %dx%d fbmsk=%08x%s\n",
                             fbp, psm, w, h, fbmsk,
                             (fbmsk & 0xFF000000u) ? "" : "   <-- OVERWRITES ALPHA");
        }
    }
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    const uint32_t bw = fbw ? fbw : 1u;
    // [rawflush] PS2X_RAWFLUSH=1: the FBO holds GL-scale alpha (GS 128 -> 255, needed for GL
    // blending); VRAM must hold GS-scale bytes -- console reads them back as PALETTE INDICES
    // (BT3's outline mask chain). Convert at this boundary: a_gs = round(a_gl*128/255), the
    // inverse of the decoder's floor(a*255/128), exact for all 0..128.
    static const bool s_rawFlush = [](){ const char *v = std::getenv("PS2X_RAWFLUSH");
                                         return v && v[0] && v[0] != '0'; }();
    auto cvA = [&](uint32_t v) -> uint32_t {
        if (!s_rawFlush || psm != 0u) return v;
        const uint32_t a = (v >> 24) & 0xFFu;
        return (v & 0x00FFFFFFu) | (((a * 128u + 127u) / 255u) << 24);
    };
    // [flushrect] optional sub-rectangle (set by flushPageToVram): only these rows/cols are
    // written; px keeps the full w-stride layout.
    const int ry0 = (g_wbRectY0 >= 0) ? std::max(0, g_wbRectY0) : 0, ry1 = (g_wbRectY0 >= 0) ? std::min(h, g_wbRectY1) : h;
    const int rx0 = (g_wbRectY0 >= 0) ? std::max(0, g_wbRectX0) : 0, rx1 = (g_wbRectY0 >= 0) ? std::min(w, g_wbRectX1) : w;
    const auto &wfn = gs->writeVramFn(psm); const auto &rfn = gs->readVramFn(psm);   // [wbhoist]
    auto PROW = [&](int y) -> size_t { return (size_t)(g_wbFlipY ? (h - 1 - y) : y) * (size_t)w; };   // [noflip]
    int wrY0 = 1 << 30, wrY1 = -1;   // [flushdiff] rows actually written
    for (int y = ry0; y < ry1; ++y)
    {
        int xs = rx0, xe = rx1;
        if (g_wbRowRange)
        {   // [flushrows] per-row changed x-range from the shadow diff: unchanged rows cost one test
            xs = std::max(xs, g_wbRowRange[(size_t)y].first); xe = std::min(xe, g_wbRowRange[(size_t)y].second);
            if (xs >= xe) continue;
        }
        if (psm == 0u && fbmsk == 0u && !g_wbAlphaFillOnly && !s_rawFlush)
        {   // [rowct32] unmasked CT32 (the scene / mask page flushes): bulk row write
            const u32 n = GSMem::WriteRowCT32(gs->vramData(), base, bw, (u32)xs, (u32)xe, (u32)y, px + PROW(y) + xs,   // [rowrel] src/mask relative to xs
                                              g_wbSkipMask ? g_wbSkipMask + (size_t)y * w + xs : nullptr);
            if (g_wbMirror) GSMem::WriteRowCT32(g_wbMirror, base, bw, (u32)xs, (u32)xe, (u32)y, px + PROW(y) + xs, g_wbSkipMask ? g_wbSkipMask + (size_t)y * w + xs : nullptr);   // [ddmirror] same skip mask: a page the guest uploaded after the read keeps its post-time snapshot
            if (g_wbLinear) std::memcpy(g_wbLinear + (size_t)y * g_wbLinearStride + xs, px + PROW(y) + xs, (size_t)(xe - xs) * 4u);   // [linvram]
            if (n) { if (y < wrY0) wrY0 = y; if (y > wrY1) wrY1 = y; g_wbPixelsWritten += n; }
            continue;
        }
        if ((psm == 0x02u || psm == 0x0Au) && fbmsk == 0u && !g_wbAlphaFillOnly && !s_rawFlush)
        {   // [rowct16] unmasked CT16/CT16S (the edge page f336): pack the row, then bulk write
            static std::vector<uint16_t> row16; if (row16.size() < (size_t)w) row16.resize((size_t)w);
            const uint32_t *srow = px + PROW(y);
            for (int x = xs; x < xe; ++x) row16[(size_t)x] = (uint16_t)(s_wbPack16 ? wbPack16(srow[x]) : srow[x]);
            const u32 n = (psm == 0x02u)
                ? GSMem::WriteRowCT16(gs->vramData(), base, bw, (u32)xs, (u32)xe, (u32)y, row16.data() + xs, g_wbSkipMask ? g_wbSkipMask + (size_t)y * w + xs : nullptr)
                : GSMem::WriteRowCT16S(gs->vramData(), base, bw, (u32)xs, (u32)xe, (u32)y, row16.data() + xs, g_wbSkipMask ? g_wbSkipMask + (size_t)y * w + xs : nullptr);
            if (g_wbMirror) { const uint8_t *mk = g_wbSkipMask ? g_wbSkipMask + (size_t)y * w + xs : nullptr;
                              if (psm == 0x02u) GSMem::WriteRowCT16(g_wbMirror, base, bw, (u32)xs, (u32)xe, (u32)y, row16.data() + xs, mk);
                              else              GSMem::WriteRowCT16S(g_wbMirror, base, bw, (u32)xs, (u32)xe, (u32)y, row16.data() + xs, mk); }   // [ddmirror]
            if (g_wbLinear) for (int x = xs; x < xe; ++x) g_wbLinear[(size_t)y * g_wbLinearStride + x] = row16[(size_t)x];   // [linvram]
            if (n) { if (y < wrY0) wrY0 = y; if (y > wrY1) wrY1 = y; g_wbPixelsWritten += n; }
            continue;
        }
        for (int x = xs; x < xe; ++x)
        {
            if (g_wbMirror && !(g_wbSkipMask && !g_wbSkipMask[(size_t)y * w + x]))
            {   // [ddmirror] the same pixel into the private copy, masked against the COPY's own bytes, same skip mask as live
                const uint32_t nvm = cvA(px[PROW(y) + x]);
                const bool p16m = s_wbPack16 && wbIs16(psm);
                if (g_wbAlphaFillOnly)
                {
                    const uint32_t ov = rfn(g_wbMirror, base, bw, (uint32_t)x, (uint32_t)y);
                    const uint32_t rgbMask = fbmsk & 0x00FFFFFFu;
                    const uint32_t rgb = (ov & rgbMask) | (nvm & ~rgbMask & 0x00FFFFFFu);
                    const uint32_t al  = (ov & 0xFF000000u) ? (ov & 0xFF000000u) : (nvm & 0xFF000000u);
                    wfn(g_wbMirror, base, bw, (uint32_t)x, (uint32_t)y, rgb | al);
                }
                else if (fbmsk == 0u) wfn(g_wbMirror, base, bw, (uint32_t)x, (uint32_t)y, p16m ? wbPack16(nvm) : nvm);
                else
                {
                    uint32_t ov = rfn(g_wbMirror, base, bw, (uint32_t)x, (uint32_t)y);
                    if (p16m) ov = wbUnpack16(ov & 0xFFFFu);
                    const uint32_t mv = (ov & fbmsk) | (nvm & ~fbmsk);
                    wfn(g_wbMirror, base, bw, (uint32_t)x, (uint32_t)y, p16m ? wbPack16(mv) : mv);
                }
            }
            if (g_wbSkipMask && !g_wbSkipMask[(size_t)y * w + x]) continue;
            if (y < wrY0) wrY0 = y; if (y > wrY1) wrY1 = y;
            ++g_wbPixelsWritten;
            const uint32_t nv = cvA(px[PROW(y) + x]);
            if (g_wbAlphaFillOnly)
            {   // RGB per the caller's mask (the previous version wrote it unconditionally,
                // which is what regressed the frame); ALPHA only where VRAM has none yet, so
                // the software rebuild's bands survive and the framebuffer's alpha fills the
                // gaps. Wholesale alpha writes darken the sky (65 -> 36); protecting alpha
                // entirely starves the mask (73.3% zero vs console's 24.1%).
                const uint32_t ov = rfn(gs->vramData(), base, bw, (uint32_t)x, (uint32_t)y);
                const uint32_t rgbMask = fbmsk & 0x00FFFFFFu;
                const uint32_t rgb = (ov & rgbMask) | (nv & ~rgbMask & 0x00FFFFFFu);
                const uint32_t al  = (ov & 0xFF000000u) ? (ov & 0xFF000000u) : (nv & 0xFF000000u);
                wfn(gs->vramData(), base, bw, (uint32_t)x, (uint32_t)y, rgb | al);
                if (g_wbLinear) g_wbLinear[(size_t)y * g_wbLinearStride + x] = rgb | al;   // [linvram]
                continue;
            }
            const bool p16 = s_wbPack16 && wbIs16(psm);
            if (fbmsk == 0u) { const uint32_t sv = p16 ? wbPack16(nv) : nv; wfn(gs->vramData(), base, bw, (uint32_t)x, (uint32_t)y, sv); if (g_wbLinear) g_wbLinear[(size_t)y * g_wbLinearStride + x] = sv; continue; }
            uint32_t ov = rfn(gs->vramData(), base, bw, (uint32_t)x, (uint32_t)y);
            if (p16) ov = wbUnpack16(ov & 0xFFFFu);
            const uint32_t mv = (ov & fbmsk) | (nv & ~fbmsk);
            { const uint32_t sv = p16 ? wbPack16(mv) : mv; wfn(gs->vramData(), base, bw, (uint32_t)x, (uint32_t)y, sv); if (g_wbLinear) g_wbLinear[(size_t)y * g_wbLinearStride + x] = sv; }
        }
    }
    // A palette can BE a render target (BT3's outline CLUTs live in pages 499-500, which the
    // game renders into). The CLUT cache keys on m_texUploadGen, which only processImageData
    // bumps -- so without this a flushed palette would never be re-read.
    gs->bumpTexUploadGen();
    g_wbLastWrittenRows = (wrY1 >= wrY0) ? (wrY1 - wrY0 + 1) : 0;
    // NOTE: invalidating only the rows written (or nothing on a zero-change flush) was tried
    // 2026-08-27 and is NOT byte-identical (1.4% px) for no measurable gain -- the full-range
    // bump stays.
    if (g_wbRectY0 >= 0)
    {   // rows ry0..ry1-1 only, rounded out to whole page rows (32 rows for 32-bit, 64 for 16/8-bit, 128 for 4-bit)
        const int y0i = ry0, y1i = ry1;
        const uint32_t ph = (psm == 0x02u || psm == 0x0Au || psm == 0x32u || psm == 0x3Au || psm == 0x13u || psm == 0x1Bu) ? 64u : (psm == 0x14u || psm == 0x24u || psm == 0x2Cu) ? 128u : 32u;
        const uint32_t pr0 = (uint32_t)y0i / ph, pr1 = ((uint32_t)std::max(y1i, y0i + 1) + ph - 1u) / ph;
        ps2GpuRenderer().onVramWriteback(base + pr0 * bw * 32u, (pr1 - pr0) * bw * 32u); gs->bumpPageUploadGen(base + pr0 * bw * 32u, (pr1 - pr0) * bw * 32u);   // [clutpagegen]
    }
    else
    {   // [clutpagegen] braces: without them the else took only the FIRST statement and the
        // bumpPageUploadGen below ran unconditionally -- so the if-branch bumped twice, once
        // for its page range and again for the whole buffer. clang's -Wmisleading-indentation
        // flagged it (it is what a Windows builder hit).
        ps2GpuRenderer().onVramWriteback(base, (uint32_t)(((size_t)w * h * 4u) / 256u));
        gs->bumpPageUploadGen(base, (uint32_t)(((size_t)w * h * 4u) / 256u));
    }
}


// [wbhud] PS2X_WBHUDLOG=1: BT3's HUD textures live at block 7168 (byte 0x1C0000) = fbp224's base. Log every writeback
// whose block range overlaps them, so the ordering against the game's HUD re-uploads can be read from the log.
static void wbHudLog(const char *who, uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h, uint32_t base, uint32_t bw)
{
    static const bool s_hl = [](){ const char *v = std::getenv("PS2X_WBHUDLOG"); return v && v[0] && v[0] != '0'; }();
    if (!s_hl) return;
    const bool is16 = (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S);
    const uint32_t end = base + (uint32_t)h * (is16 ? (bw + 1u) / 2u : bw);
    static unsigned long n = 0, nHit = 0; ++n;
    const bool hit = (base < 7680u && end > 7168u);
    if (hit) ++nHit;
    if ((hit && nHit <= 60) || (n % 2000u) == 0u)
        std::fprintf(stderr, "[wbhud] #%lu%s %s fbp=%u fbw=%u psm=%u %dx%d blocks [%u,%u) %s HUD pages [7168,7680)\n", n, hit ? " HIT" : "", who, fbp, fbw, psm, w, h, base, end, hit ? "OVERLAPS" : "misses");
}
void ps2xWritebackToVram(uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h, const uint32_t *px)
{
    ++g_ps2xWbGen;   // [hashmemo] any VRAM writeback invalidates memoised range hashes
    { GS *gsc = g_gsWb ? g_gsWb : g_fmvGs; if (gsc) gsc->invalidateClutCache(); }   // [clutwb] a flushed page may hold a rendered palette: force the CLUT cache to re-decode

    GS *gs = g_gsWb ? g_gsWb : g_fmvGs;
    if (!gs || !px || w <= 0 || h <= 0) return;
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    const uint32_t bw = fbw ? fbw : 1u;
    wbHudLog("plain", fbp, fbw, psm, w, h, base, bw);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            gs->WriteVram(psm, base, bw, (uint32_t)x, (uint32_t)y, (s_wbPack16 && wbIs16(psm)) ? wbPack16(px[(size_t)y * w + x]) : px[(size_t)y * w + x]);
    // Tell the renderer these VRAM pages changed, or every cached decode of them stays stale and
    // the writeback is invisible to reads. onVramWriteback() existed but was NEVER CALLED.
    // A palette can BE a render target (BT3's outline CLUTs live in pages 499-500, which the
    // game renders into). The CLUT cache keys on m_texUploadGen, which only processImageData
    // bumps -- so without this a flushed palette would never be re-read.
    gs->bumpTexUploadGen();
    ps2GpuRenderer().onVramWriteback(base, (uint32_t)(((size_t)w * h * 4u) / 256u)); gs->bumpPageUploadGen(base, (uint32_t)(((size_t)w * h * 4u) / 256u));   // [clutpagegen]
    // [wbrt] PS2X_WBDIAG=1: round-trip self-check. If WriteVram(x,y,v) followed by
    // ReadVram(x,y) does not give back v, the base/stride/format I am passing is wrong and
    // every byte handed to VRAM is landing in the wrong place -- which would corrupt exactly
    // like the arena does. Sampled, not exhaustive.
    {
        static const bool s_d = [](){ const char *v = std::getenv("PS2X_WBDIAG"); return v && v[0] && v[0] != '0'; }();
        // Sample PERIODICALLY, not the first N: the first calls all land during boot, so a
        // 5-minute fight produced only logo-screen readings.
        static int s_runs = 0;
        if (s_d && (++s_runs % 600) == 0)
        {
            unsigned bad = 0, n = 0;
            for (int y = 0; y < h; y += 17)
                for (int x = 0; x < w; x += 13)
                {
                    ++n;
                    if (gs->ReadVram(psm, base, bw, (uint32_t)x, (uint32_t)y) != px[(size_t)y * w + x]) ++bad;
                }
            // WHAT are we writing? If the FBO for this page is empty (an earlier diagnostic
            // reported fbp224 as 0% non-black, alpha 255) then the writeback is faithfully
            // copying blackness over real data -- correct mechanism, worthless source.
            unsigned long sr=0,sg=0,sb=0,sa=0; unsigned long nz=0, tot=0;
            for (int y = 0; y < h; y += 7)
                for (int x = 0; x < w; x += 5)
                {
                    const uint32_t v = px[(size_t)y * w + x];
                    sr += v & 0xFF; sg += (v >> 8) & 0xFF; sb += (v >> 16) & 0xFF; sa += (v >> 24) & 0xFF;
                    if (v & 0x00FFFFFFu) ++nz;
                    ++tot;
                }
            // Alpha DISTRIBUTION, not just the mean. The mask reads this alpha as a PSMT8H
            // palette INDEX, so what matters is spatial VARIATION -- a uniform alpha selects one
            // CLUT entry everywhere and yields the flat mask we observe. Console's palette has
            // 150/256 entries with alpha, so a real mask needs a spread of indices.
            unsigned long ah[8] = {0,0,0,0,0,0,0,0};
            for (int y = 0; y < h; y += 7)
                for (int x = 0; x < w; x += 5)
                    ++ah[(((px[(size_t)y * w + x] >> 24) & 0xFF) * 8u) / 256u];
            unsigned distinct = 0; for (int i = 0; i < 8; ++i) if (ah[i]) ++distinct;
            std::fprintf(stderr, "[wbrt] fbp%u psm=%u bw=%u rt %u/%u | RGBA=(%lu,%lu,%lu,%lu) non-black %.1f%%"
                         " | alpha buckets(32s):", fbp, psm, bw, bad, n, sr/tot, sg/tot, sb/tot, sa/tot, 100.0*nz/tot);
            for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %lu", ah[i]);
            std::fprintf(stderr, "  distinct=%u%s\n", distinct,
                         distinct <= 1 ? "  <== UNIFORM: one CLUT entry everywhere" : "");
        }
    }
    // PS2X_WBSTAMP=1: stamp the written pages so cached decodes of them are re-keyed.
    // Without this the texture cache keeps serving the decode made BEFORE the writeback --
    // which is why BT3's shadow Pass 1 (which SAMPLES the scene buffer to build its silhouette)
    // still reads an empty tbp=0 even with writeback on.
    // ⚠ Default OFF: srcUploaded is also what the renderer uses to choose between sampling the
    // live FBO and decoding from VRAM, so stamping flips the blur chain between those paths on
    // alternate frames and the arena visibly blurs/unblurs.
    {
        static const bool s_wbStamp = [](){ const char *v = std::getenv("PS2X_WBSTAMP"); return v && v[0] && v[0] != '0'; }();
        if (s_wbStamp) ps2GpuRenderer().onVramUpload(base, (uint32_t)(bw * h));
    }
}

// PS2X_GRASSHACK shared state (see processImageData tail + GSRasterizer::drawPrimitive):
// shadow of the 10752 slot captured when the mountains+grass image is resident.
bool g_ps2xGrassHack = [](){ const char *v = std::getenv("PS2X_GRASSHACK"); return v && v[0] && v[0] != '0'; }();
uint8_t g_ps2xGrassShadow[131072];
std::atomic<bool> g_ps2xGrassShadowValid{false};

// ---- PS2X_GS_RECORD: replayable GS stream capture (diagnostic; inert unless the env is set) ----
// RING BUFFER by default: keeps only the most recent PS2X_GS_RECORD_MB of stream in memory and
// writes it out at exit. A straight-to-disk recorder fills its cap during boot/menus and never
// reaches the moment you wanted, which is exactly what happened on the first attempt.
// PS2X_GS_RECORD_DIRECT=1 restores append-as-you-go.
#include <deque>
#include <condition_variable>
#include <thread>
#include <csignal>
namespace {
struct GsRec {
    std::mutex mx;
    std::deque<std::vector<uint8_t>> q;   // each entry is one complete record (header + payload)
    unsigned long long bytes = 0, cap = 0;
    FILE *direct = nullptr;
    const char *path = nullptr;
    bool init = false, ring = true, done = false;
    // [recio] direct mode used to fwrite ON THE GIF HOT PATH -- multi-MB synchronous writes
    // there stall the guest, and BT3's memory-card / loader device polls are timing-gated
    // (the loading-stall class), so recording DIRECT broke the mc check. A writer thread
    // drains this queue instead; the hot path only enqueues.
    std::deque<std::vector<uint8_t>> wq;
    std::condition_variable wcv;
    std::thread writer;
    bool writerStarted = false, writerStop = false;
    unsigned long long wqBytes = 0;
};
GsRec g_rec;
void gsRecInit()
{
    if (g_rec.init) return;
    g_rec.init = true;
    const char *p = std::getenv("PS2X_GS_RECORD");
    if (!p || !p[0]) return;
    g_rec.path = p;
    const char *mb = std::getenv("PS2X_GS_RECORD_MB");
    g_rec.cap = (unsigned long long)((mb && mb[0]) ? std::atoll(mb) : 512LL) * 1024ull * 1024ull;
    const char *dv = std::getenv("PS2X_GS_RECORD_DIRECT");
    g_rec.ring = !(dv && dv[0] && dv[0] != '0');
    if (!g_rec.ring) g_rec.direct = std::fopen(p, "wb");
    std::fprintf(stderr, "[gsrecord] %s -> %s (cap %llu MB)%s\n",
                 g_rec.ring ? "RING (keeps the LAST frames, written at exit)" : "direct append",
                 p, g_rec.cap / (1024ull*1024ull), g_rec.ring ? " -- just quit when you are done" : "");
}
void gsRecPush(const uint8_t *hdr, size_t hn, const uint8_t *pay, size_t pn)
{
    gsRecInit();
    if (!g_rec.path) return;
    std::lock_guard<std::mutex> lk(g_rec.mx);
    if (g_rec.done) return;
    if (!g_rec.ring)
    {
        if (!g_rec.direct || g_rec.bytes >= g_rec.cap) return;
        if (!g_rec.writerStarted)
        {
            g_rec.writerStarted = true;
            g_rec.writer = std::thread([](){
                std::unique_lock<std::mutex> lk(g_rec.mx);
                for (;;)
                {
                    g_rec.wcv.wait(lk, []{ return g_rec.writerStop || !g_rec.wq.empty(); });
                    while (!g_rec.wq.empty())
                    {
                        std::vector<uint8_t> e = std::move(g_rec.wq.front());
                        g_rec.wq.pop_front(); g_rec.wqBytes -= e.size();
                        lk.unlock();
                        std::fwrite(e.data(), 1, e.size(), g_rec.direct);
                        lk.lock();
                    }
                    if (g_rec.writerStop) return;
                }
            });
        }
        std::vector<uint8_t> e; e.reserve(hn + pn);
        e.insert(e.end(), hdr, hdr + hn);
        if (pn) e.insert(e.end(), pay, pay + pn);
        g_rec.bytes += e.size();
        // Backpressure guard: if the disk cannot keep up, drop rather than stall the guest
        // (a gap in the recording beats a broken game).
        if (g_rec.wqBytes < (512ull << 20))
        { g_rec.wqBytes += e.size(); g_rec.wq.push_back(std::move(e)); g_rec.wcv.notify_one(); }
        return;
    }
    std::vector<uint8_t> e; e.reserve(hn + pn);
    e.insert(e.end(), hdr, hdr + hn);
    if (pn) e.insert(e.end(), pay, pay + pn);
    g_rec.bytes += e.size();
    g_rec.q.push_back(std::move(e));
    while (g_rec.bytes > g_rec.cap && !g_rec.q.empty())
    { g_rec.bytes -= g_rec.q.front().size(); g_rec.q.pop_front(); }
}
} // namespace

// Flush the ring at exit. Drops any leading vsync markers: the replay picks the file format from
// byte 0 and rejects a stream that does not begin with a packet.
extern "C" void ps2xGsRecordFlush();
extern "C" void ps2xGsRecordFlush()
{
    std::unique_lock<std::mutex> lk(g_rec.mx);
    if (g_rec.done || !g_rec.path) return;
    g_rec.done = true;
    if (!g_rec.ring)
    {
        if (g_rec.writerStarted)
        {   // Let the writer drain its queue and exit, then join WITHOUT the lock held --
            // it re-acquires mx between records, so holding mx here would deadlock, and
            // writing concurrently from two threads would interleave the file.
            g_rec.writerStop = true;
            g_rec.wcv.notify_one();
            lk.unlock();
            if (g_rec.writer.joinable()) g_rec.writer.join();
            lk.lock();
        }
        if (g_rec.direct)
        {
            while (!g_rec.wq.empty())
            {   // anything enqueued after the stop flag
                std::vector<uint8_t> &e = g_rec.wq.front();
                std::fwrite(e.data(), 1, e.size(), g_rec.direct);
                g_rec.wq.pop_front();
            }
            std::fclose(g_rec.direct);
            g_rec.direct = nullptr;
        }
        return;
    }
    while (!g_rec.q.empty() && !g_rec.q.front().empty() && g_rec.q.front()[0] != 0u)
    { g_rec.bytes -= g_rec.q.front().size(); g_rec.q.pop_front(); }
    FILE *f = std::fopen(g_rec.path, "wb");
    if (!f) { std::fprintf(stderr, "[gsrecord] could not open %s\n", g_rec.path); return; }
    unsigned long long w = 0; unsigned long vs = 0;
    for (auto &e : g_rec.q) { std::fwrite(e.data(), 1, e.size(), f); w += e.size(); if (e[0] == 1u) ++vs; }
    std::fclose(f);
    std::fprintf(stderr, "[gsrecord] wrote %s: %.1f MB, %lu frames retained (~%.0f s at 30fps, %.2f MB/frame)"
                 " -- trigger the effect within this window of quitting\n",
                 g_rec.path, w / 1e6, vs, vs / 30.0, vs ? (w / 1e6) / vs : 0.0);
}

// [recpriv] replay side: put a recorded CRTC snapshot back so an offline replay presents the
// way the live run did. Only the display-affecting registers; everything else is left alone.
void GS::setPrivRegsFromRecord(uint64_t pmode, uint64_t dispfb1, uint64_t display1,
                               uint64_t dispfb2, uint64_t display2, uint64_t bgcolor, uint64_t smode2)
{
    if (!m_privRegs) return;
    m_privRegs->pmode = pmode;
    m_privRegs->dispfb1 = dispfb1; m_privRegs->display1 = display1;
    m_privRegs->dispfb2 = dispfb2; m_privRegs->display2 = display2;
    m_privRegs->bgcolor = bgcolor; m_privRegs->smode2 = smode2;
}

// [recpriv] g_gsWb is a C++ global (defined above); declaring it INSIDE the extern "C"
// function below gives the declaration C linkage and clang-cl rejects it outright
// ("declaration of 'g_gsWb' has a different language linkage"), which broke the Windows
// build. Declare it out here, at C++ linkage, and just use it in the function.
extern GS *g_gsWb;

extern "C" void ps2xGsRecordVsync()
{
    {   // [recpriv] record type 4: the CRTC state, once per vsync, BEFORE the vsync marker.
        // The stream used to carry only GIF packets and vsyncs, so a replay presented with
        // whatever the replayer happened to have in m_privRegs -- which makes every display
        // bug (DISPFB flips, DISPLAY DX/DY placement, circuit enables) invisible offline.
        // 2026-09-04: added for the underwater bug, where diving shifts the whole scene.
        // Old dumps simply have no type 4; the replay skips unknown types it does not know,
        // and types 2/3 from older recorders are still skipped as before.
        if (g_gsWb)
        {
            const GSRegisters *r = g_gsWb->privRegsForRecord();
            if (r)
            {
                uint64_t v[7] = {r->pmode, r->dispfb1, r->display1, r->dispfb2, r->display2,
                                 r->bgcolor, r->smode2};
                const uint8_t hdr[1] = {4u};
                gsRecPush(hdr, 1, reinterpret_cast<const uint8_t *>(v), sizeof v);
            }
        }
    }
    const uint8_t v[2] = {1u, 0u};
    gsRecPush(v, 2, nullptr, 0);
    // AUTOFLUSH: do not depend on a clean exit. atexit() did not run when the window was closed
    // and a whole play session was lost, so write the ring out periodically as well.
    // PS2X_GS_RECORD_EVERY=0 disables; default every 600 frames (~10 s of play).
    // Must be well UNDER how many frames the ring holds, or the periodic write can land after
    // the moment you wanted has already scrolled out. Fight frames are ~1 MB each (~46k kicks),
    // so a 512 MB ring holds ~500 of them; flushing every 120 keeps the file <=4 s stale.
    static const long s_every = [](){ const char *v2 = std::getenv("PS2X_GS_RECORD_EVERY");
                                      return v2 && v2[0] ? std::atol(v2) : 120L; }();
    if (s_every > 0)
    {
        static long n = 0;
        if (++n % s_every == 0)
        {
            bool ring;
            { std::lock_guard<std::mutex> lk(g_rec.mx); ring = g_rec.ring;
              if (!ring && g_rec.direct) std::fflush(g_rec.direct); }
            if (ring)
            {   // RING MODE ONLY: in direct mode this flush CLOSED the file and every later
                // record silently no-opped -- direct recordings always ended at exactly 120
                // vsyncs (~2 s of play). Direct mode streams continuously; its periodic
                // safety is the fflush above (a hard kill loses at most the OS buffer).
                ps2xGsRecordFlush();
                std::lock_guard<std::mutex> lk(g_rec.mx); g_rec.done = false;  // keep recording
            }
        }
    }
}

// Signal-safe-ish exit paths: closing the window / Ctrl-C did not run atexit.
extern "C" void ps2xGsRecordOnSignal(int sig)
{
    ps2xGsRecordFlush();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

static thread_local int t_gsStateHeld = 0;   // [relock] >0 while this thread holds m_stateMutex in processGIFPacket
void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    gprof::Scope gpScope(gprof::GIF);   // [guestprof]
    {   // PS2X_GS_RECORD=<path>: append this packet to a replayable stream so live-only bugs
        // (explosions, transformations) can be analysed offline with the validated tooling.
        // Format the replay expects: 0x00, path byte, uint32 length, payload -- and 0x01, pad
        // for a vsync. PS2X_GS_RECORD_MB caps the file (default 512 MB).
        if (sizeBytes > 0u)
        {
            // [recpath] the path byte carries the GIF path (1=XGKICK 2=DIRECT 3=PATH3) so an
            // offline census can tell VU1-built packets from EE-built ones (was always 0).
            uint8_t hdr[6] = {0u, (uint8_t)m_curSrcPath, 0u, 0u, 0u, 0u};
            std::memcpy(hdr + 2, &sizeBytes, 4);
            gsRecPush(hdr, 6, data, sizeBytes);
            {   // [wispsrc] PS2X_WISPSRC=<tbp>: which GIF path delivers the packets binding this
                // PSMT8 64x64 texture (the aura wisps) -- PATH1 means VU1 built the vertices.
                static const uint32_t s_wt = [](){ const char *v = std::getenv("PS2X_WISPSRC");
                                                   return v && v[0] ? (uint32_t)std::atoi(v) : 0u; }();
                static int s_wn = 0;
                if (s_wt && s_wn < 40)
                    for (uint32_t off = 0; off + 16 <= sizeBytes; off += 16)
                    {
                        uint64_t lo, hi; std::memcpy(&lo, data + off, 8); std::memcpy(&hi, data + off + 8, 8);
                        // REGLIST packets carry one register per 64-bit half: check both.
                        const bool mLo = (lo & 0x3FFFull) == s_wt && ((lo >> 20) & 0x3Full) == 19ull && ((lo >> 26) & 0xFull) == 6ull;
                        const bool mHi = (hi & 0x3FFFull) == s_wt && ((hi >> 20) & 0x3Full) == 19ull && ((hi >> 26) & 0xFull) == 6ull;
                        if (mLo || mHi)
                        { std::fprintf(stderr, "[wispsrc] GS packet path=%u len=%u tex0@+%u%s\n", (unsigned)m_curSrcPath, sizeBytes, off, mHi ? " (hi half/REGLIST)" : ""); ++s_wn; break; }
                    }
            }
        }
    }
    if (g_vramDumpPtr)
    {
        static uint32_t s_pktCount = 0;
        ++s_pktCount;
        if (s_pktCount >= 25000u && (s_pktCount % 25000u) == 0u)
        {
            FILE *vf = std::fopen("/home/z3/Desktop/bt3/work/vram.bin", "wb");
            if (vf)
            {
                std::fwrite(g_vramDumpPtr, 1, 4u * 1024u * 1024u, vf);
                std::fclose(vf);
                std::fprintf(stderr, "[vram-dump] wrote 4MB VRAM at gif pkt #%u\n", s_pktCount);
            }
        }
    }
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    { extern GS *g_gsWb; if (!g_gsWb) g_gsWb = this; }
    {   // [dispwatch] PS2X_DISPWATCH=1: log the FULL CRTC state -- PMODE, both DISPFBs and both
        // DISPLAYs decoded -- whenever ANY of them changes. The per-register census in the write
        // handler cannot see these: BT3 writes the privileged regs memory-mapped, bypassing it.
        // Added 2026-09-04 for the underwater bug (diving shifts the whole scene down-right and
        // splits the HUD into its own band, which is what an unhandled DISPLAY DX/DY would do).
        static const bool s_dw = [](){ const char *v = std::getenv("PS2X_DISPWATCH");
                                       return v && v[0] && v[0] != '0'; }();
        if (s_dw && m_privRegs)
        {
            static uint64_t lpm = ~0ull, ld1 = ~0ull, ld2 = ~0ull, ly1 = ~0ull, ly2 = ~0ull;
            const uint64_t pm = m_privRegs->pmode, d1 = m_privRegs->dispfb1, d2 = m_privRegs->dispfb2;
            const uint64_t y1 = m_privRegs->display1, y2 = m_privRegs->display2;
            if (pm != lpm || d1 != ld1 || d2 != ld2 || y1 != ly1 || y2 != ly2)
            {
                lpm = pm; ld1 = d1; ld2 = d2; ly1 = y1; ly2 = y2;
                static unsigned long n = 0;
                auto dsp = [](uint64_t v, const char *nm) {
                    std::fprintf(stderr, "  %s DX=%-5u DY=%-5u MAGH=%u MAGV=%u DW=%-5u DH=%-4u -> visible %ux%u\n",
                                 nm, (unsigned)(v & 0xFFFu), (unsigned)((v >> 12) & 0x7FFu),
                                 (unsigned)((v >> 23) & 0xFu), (unsigned)((v >> 27) & 0x3u),
                                 (unsigned)((v >> 32) & 0xFFFu), (unsigned)((v >> 44) & 0x7FFu),
                                 (unsigned)(((v >> 32 & 0xFFFu) + 1) / (((v >> 23) & 0xFu) + 1)),
                                 (unsigned)(((v >> 44 & 0x7FFu) + 1) / (((v >> 27) & 0x3u) + 1)));
                };
                auto fb = [](uint64_t v, const char *nm) {
                    std::fprintf(stderr, "  %s fbp=%-4u fbw=%-3u psm=0x%02x  read origin DBX=%u DBY=%u\n",
                                 nm, (unsigned)(v & 0x1FFu), (unsigned)((v >> 9) & 0x3Fu),
                                 (unsigned)((v >> 15) & 0x1Fu),
                                 (unsigned)((v >> 32) & 0x7FFu), (unsigned)((v >> 43) & 0x7FFu));
                };
                std::fprintf(stderr, "[dispwatch] #%lu CRTC CHANGED  pmode=%016llx  EN1=%d EN2=%d MMOD=%d AMOD=%d SLBG=%d ALP=%u\n",
                             ++n, (unsigned long long)pm, (int)(pm & 1), (int)((pm >> 1) & 1),
                             (int)((pm >> 5) & 1), (int)((pm >> 6) & 1), (int)((pm >> 7) & 1),
                             (unsigned)((pm >> 8) & 0xFF));
                fb(d1, "DISPFB1 "); dsp(y1, "DISPLAY1");
                fb(d2, "DISPFB2 "); dsp(y2, "DISPLAY2");
            }
        }
    }
    {   // [privlog] PS2X_PRIVLOG=1: the display registers (memory-mapped writes bypass the register handler) every 100 ms
        static const bool s_pl = [](){ const char *v = std::getenv("PS2X_PRIVLOG"); return v && v[0] && v[0] != '0'; }();
        if (s_pl && m_privRegs)
        {
            static auto s_t = std::chrono::steady_clock::now(); static uint64_t lp = ~0ull, ld = ~0ull;
            const auto now = std::chrono::steady_clock::now();
            const uint64_t pm = m_privRegs->pmode, d2 = m_privRegs->dispfb2;
            if (pm != lp || d2 != ld || std::chrono::duration<double>(now - s_t).count() >= 0.25)
            {
                s_t = now; lp = pm; ld = d2;
                const uint64_t d1 = m_privRegs->dispfb1;
                std::fprintf(stderr, "[privlog] pmode=%016llx en1=%d en2=%d mmod=%d amod=%d slbg=%d alp=%u bgcolor=%06llx | dispfb1 fbp=%u fbw=%u psm=%u dby=%u | dispfb2 fbp=%u fbw=%u psm=%u dby=%u\n",
                             (unsigned long long)pm, (int)(pm & 1), (int)((pm >> 1) & 1), (int)((pm >> 5) & 1), (int)((pm >> 6) & 1), (int)((pm >> 7) & 1),
                             (unsigned)((pm >> 8) & 0xFF), (unsigned long long)(m_privRegs->bgcolor & 0xFFFFFF),
                             (unsigned)(d1 & 0x1FF), (unsigned)((d1 >> 9) & 0x3F), (unsigned)((d1 >> 15) & 0x1F), (unsigned)((d1 >> 43) & 0x7FF),
                             (unsigned)(d2 & 0x1FF), (unsigned)((d2 >> 9) & 0x3F), (unsigned)((d2 >> 15) & 0x1F), (unsigned)((d2 >> 43) & 0x7FF));
            }
        }
    }   // [deferdec] the GL thread's decode needs the GS before any writeback happened (replay had none)
    // [relock] writeRegister() re-locked this recursive mutex for EVERY register in the packet
    // (~7% of the guest thread in pthread lock/unlock). Mark it held for this thread so the
    // nested writeRegister() calls skip the lock (same mutex, same thread: no semantic change).
    struct GsHeldGuard { GsHeldGuard() { ++t_gsStateHeld; } ~GsHeldGuard() { --t_gsStateHeld; } } _gsHeld;
    if (!data || sizeBytes < 16 || !m_vram)
        return;
    m_curPktData = data;
    m_curPktSize = sizeBytes;
    const auto _g0 = g_gifTimeProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });


    uint32_t offset = 0;
    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;

        recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegister(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                        return;
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    writeRegisterPacked(regs[r], lo, hi);
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                        return;
                    writeRegister(regs[r], loadLE64(data + offset));
                    offset += 8;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            gprof::Scope gpScope(gprof::XFER);   // [guestprof] IMAGE-mode host->VRAM transfer
            ++m_stateGen;   // [rectemplate] VRAM content changes
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            if (g_gifTimeProf)
            {
                const auto _i0 = std::chrono::steady_clock::now();
                processImageData(data + offset, imageBytes);
                g_gifImageNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _i0).count(), std::memory_order_relaxed);
                g_gifImageBytes.fetch_add(imageBytes, std::memory_order_relaxed);
            }
            else
                processImageData(data + offset, imageBytes);
            offset += imageBytes;
        }
    }

    if (g_gifTimeProf)
    {
        g_gifTotalNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _g0).count(), std::memory_order_relaxed);
        static std::chrono::steady_clock::time_point s_last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - s_last).count();
        if (dt >= 1.0)
        {
            extern std::atomic<uint64_t> g_rasterPrimNs;
            std::cerr << "[gifprof] total=" << (g_gifTotalNs.load() / 1e6 / dt)
                      << "ms/s image=" << (g_gifImageNs.load() / 1e6 / dt)
                      << "ms/s drawPrim=" << (g_rasterPrimNs.load() / 1e6 / dt)
                      << "ms/s (regDecode=" << ((g_gifTotalNs.load() - g_gifImageNs.load() - g_rasterPrimNs.load()) / 1e6 / dt) << "ms/s)" << std::endl;
            g_gifTotalNs = 0; g_gifImageNs = 0; g_gifImageBytes = 0; g_rasterPrimNs = 0; s_last = now;
        }
    }
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegister(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        {
            static const bool s_rp = [](){ const char *v=std::getenv("PS2X_RGBAQ_PROBE"); return v&&v[0]&&v[0]!='0'; }();
            if (s_rp) {
                static std::mutex s_rm; static std::map<uint32_t,uint32_t> s_h;
                std::lock_guard<std::mutex> lk(s_rm);
                uint32_t k=(m_curR)|(m_curG<<8)|(m_curB<<16)|(m_curA<<24);
                s_h[k]++;
                static std::atomic<uint32_t> s_c{0};
                if ((s_c.fetch_add(1)%20000u)==1u){
                    std::cerr<<"[rgbaq] (r,g,b,a=count):";
                    for(auto&kv:s_h) std::cerr<<" ("<<(kv.first&0xff)<<","<<((kv.first>>8)&0xff)<<","<<((kv.first>>16)&0xff)<<","<<((kv.first>>24)&0xff)<<")="<<kv.second;
                    std::cerr<<std::endl;
                }
            }
        }
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0x3FFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0x3FFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        // [kickchk] (PS2X_WEDGEREC): beyond-guard-band vertices that arrive DRAW-ENABLED
        // (adc=0) — the wedge-painting escapees. Uncapped tally, per-kick attribution, and
        // a one-shot snapshot of the offending kick's VU data for offline dissection.
        {
            static const bool s_kc = [](){ const char *v = std::getenv("PS2X_WEDGEREC"); return v && v[0] && v[0] != '0'; }();
            if (s_kc && !adk && (x < 4694u || x > 60842u))
            {
                extern thread_local uint32_t g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr;
                extern thread_local const uint8_t *g_xgkickVuData;
                extern thread_local uint32_t g_xgkickVuDataSize;
                extern thread_local const uint8_t *g_xgkickEntryStateBytes;
                extern thread_local uint32_t g_xgkickEntryStateSize;
                static std::atomic<uint32_t> s_n{0};
                const uint32_t n = s_n.fetch_add(1);
                if (n < 64u || (n % 1024u) == 0u)
                    std::fprintf(stderr, "[kickchk] ESCAPEE #%u x=%.1f y=%.1f z=%#x prim=%u path=%u kickPc=%u top=%u kickAddr=%u hi=%016llx\n",
                                 n, x / 16.0f - 1792.0f, y / 16.0f - 1824.0f, z,
                                 (unsigned)m_prim.type, (unsigned)m_curSrcPath,
                                 g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr,
                                 (unsigned long long)hi);
                static std::atomic<bool> s_dumped{false};
                bool exp0 = false;
                if (m_curSrcPath == 1u && g_xgkickVuData && s_dumped.compare_exchange_strong(exp0, true))
                {
                    FILE *dm = std::fopen("/home/z3/Desktop/bt3/work/esc_data.bin", "wb");
                    if (dm) { std::fwrite(g_xgkickVuData, 1, g_xgkickVuDataSize, dm); std::fclose(dm); }
                    if (g_xgkickEntryStateBytes)
                    {
                        FILE *st = std::fopen("/home/z3/Desktop/bt3/work/esc_state.bin", "wb");
                        if (st) { std::fwrite(g_xgkickEntryStateBytes, 1, g_xgkickEntryStateSize, st); std::fclose(st); }
                    }
                    std::fprintf(stderr, "[kickchk] escapee kick snapshot: pc=%u top=%u kickAddr=%u\n",
                                 g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr);
                }
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        ps2xSubpixTally(vtx.x, vtx.y);
        vtx.z = static_cast<float>(z);
        {   // [zraw] PS2X_ZRAW=1: raw z at the ACTIVE decode site (case 0x04/0x05 of
            // processGIFPacket). The .gs says ~2960 terrain verts/frame carry z>10M.
            static const bool s_zr = [](){ const char *v = std::getenv("PS2X_ZRAW"); return v && v[0] && v[0] != '0'; }();
            if (s_zr) { static unsigned long nn=0, ff=0; static uint32_t mx=0;
                ++nn; if (z > 10000000u) ++ff; if (z > mx) mx = z;
                static unsigned long byPrim[8]={0,0,0,0,0,0,0,0}, byAdc[2]={0,0}, slot[8]={0,0,0,0,0,0,0,0};
                if (z > 10000000u) { byPrim[m_prim.type & 7]++; byAdc[adk?1:0]++; slot[(m_vtxCount % 8)]++; }
                if ((nn % 50000ul) == 0ul) {
                    std::fprintf(stderr, "[zraw] line %d: verts=%lu  z>10M=%lu (%.2f%%)  maxz=%u\n",
                                 __LINE__, nn, ff, 100.0*ff/nn, mx);
                    std::fprintf(stderr, "[zraw]   far by PRIM: ");
                    for (int i=0;i<8;++i) std::fprintf(stderr, "%d=%lu ", i, byPrim[i]);
                    std::fprintf(stderr, "| far by ADC: draw=%lu skip=%lu | far by slot(m_vtxCount%%8): ", byAdc[0], byAdc[1]);
                    for (int i=0;i<8;++i) std::fprintf(stderr, "%d=%lu ", i, slot[i]);
                    std::fprintf(stderr, "\n"); } }
        }
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        ps2xSubpixTally(vtx.x, vtx.y);
        vtx.z = static_cast<float>(z);
        {   // [zraw] PS2X_ZRAW=1: raw z at the ACTIVE decode site (case 0x04/0x05 of
            // processGIFPacket). The .gs says ~2960 terrain verts/frame carry z>10M.
            static const bool s_zr = [](){ const char *v = std::getenv("PS2X_ZRAW"); return v && v[0] && v[0] != '0'; }();
            if (s_zr) { static unsigned long nn=0, ff=0; static uint32_t mx=0;
                ++nn; if (z > 10000000u) ++ff; if (z > mx) mx = z;
                if ((nn % 50000ul) == 0ul)
                    std::fprintf(stderr, "[zraw] line %d: verts=%lu  z>10M=%lu (%.2f%%)  maxz=%u\n",
                                 __LINE__, nn, ff, 100.0*ff/nn, mx); }
        }
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        ps2xSubpixTally(vtx.x, vtx.y);
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        {   // [zraw] PS2X_ZRAW=1: count vertices by raw z at the DECODE, before any cull can
            // touch them. The .gs says ~2960 terrain vertices/frame carry z>10M; if they never
            // appear here, the loss is in packet parsing, not in the rasteriser.
            static const bool s_zr = [](){ const char *v = std::getenv("PS2X_ZRAW"); return v && v[0] && v[0] != '0'; }();
            if (s_zr) { static unsigned long n=0, far=0; static float mx=0.0f;
                ++n; if (vtx.z > 10000000.0f) ++far; if (vtx.z > mx) mx = vtx.z;
                if ((n % 100000ul) == 0ul)
                    std::fprintf(stderr, "[zraw] XYZF2-packed verts=%lu  z>10M=%lu  maxz=%.0f\n", n, far, mx); }
        }
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        {   static const bool s_zr = [](){ const char *v = std::getenv("PS2X_ZRAW"); return v && v[0] && v[0] != '0'; }();
            if (s_zr) { static unsigned long n=0, far=0; static float mx=0.0f;
                ++n; if (vtx.z > 10000000.0f) ++far; if (vtx.z > mx) mx = vtx.z;
                if ((n % 20000ul) == 0ul)
                    std::fprintf(stderr, "[zraw] XYZ2-packed  verts=%lu  z>10M=%lu  maxz=%.0f\n", n, far, mx); }
        }
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegister(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegister(regDesc, lo);
        break;
    }
}

// [prmode] last raw PRIM / PRMODE values (single GS instance) so PRMODECONT switches can re-apply attributes
static uint64_t s_primRaw = 0, s_prmodeRaw = 0;
std::atomic<unsigned long> g_regWriteHist[0x63];   // [rectemplate] state-register writes per register (PS2X_GUESTPROF=1)

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    {   // [recdirect] PS2X_GS_RECORD only tapped processGIFPacket, so GS registers written
        // DIRECTLY -- the kernel draw-environment stubs in Kernel/Stubs/GS.cpp and
        // Helpers/Support.h call gs().writeRegister() -- never reached the dump. FRAME, ZBUF,
        // TEST and PRMODECONT arrive that way, so replays of our own dumps ran with a missing
        // draw environment and diverged from live (measured: a third of the mask build came
        // out untextured in replay and painted the page white, a class that does not exist
        // live). Emit each direct write as a one-register PACKED A+D packet so the dump is
        // self-contained and comparable with a PCSX2 dump.
        static const bool s_rec = [](){ const char *v = std::getenv("PS2X_GS_RECORD");
                                        return v && v[0]; }();
        if (s_rec && t_gsStateHeld == 0)
        {
            uint64_t q[4];
            q[0] = 1ull | (1ull << 15) | (1ull << 60);   // NLOOP=1, EOP, FLG=PACKED, NREG=1
            q[1] = 0x0Eull;                              // REGS = A+D
            q[2] = value;
            q[3] = regAddr;
            // Path byte 7 = "synthesised from a direct writeRegister". Do NOT use 2 here:
            // 2 is a REAL GIF path (VIF1 DIRECT), so tagging these 2 made kernel-stub writes
            // indistinguishable from the EE's DIRECT packets in every offline census.
            uint8_t hdr[6] = {0u, 7u /*synthetic direct write*/, 0u, 0u, 0u, 0u};
            const uint32_t nb = (uint32_t)sizeof q;
            std::memcpy(hdr + 2, &nb, 4);
            gsRecPush(hdr, 6, reinterpret_cast<const uint8_t *>(q), nb);
        }
    }
    auto applyPrimAttrs = [this](uint64_t v)
    {   // [prmode] attribute bits shared by PRIM and PRMODE
        m_prim.iip = ((v >> 3) & 1) != 0; m_prim.tme = ((v >> 4) & 1) != 0; m_prim.fge = ((v >> 5) & 1) != 0;
        m_prim.abe = ((v >> 6) & 1) != 0; m_prim.aa1 = ((v >> 7) & 1) != 0; m_prim.fst = ((v >> 8) & 1) != 0;
        m_prim.ctxt = ((v >> 9) & 1) != 0; m_prim.fix = ((v >> 10) & 1) != 0;
    };
    static const bool s_relock = [](){ const char *v = std::getenv("PS2X_RELOCK"); return !(v && v[0] == '0'); }();   // [relock] =0 -> always lock
    std::unique_lock<std::recursive_mutex> lock(m_stateMutex, std::defer_lock);
    if (!s_relock || t_gsStateHeld == 0) lock.lock();
    const bool sameVal = (regAddr < 0x63u) && m_rawSet[regAddr] && m_rawRegs[regAddr] == value;   // [rectemplate] a rewrite of the same value changes no state
    if (regAddr < 0x63u) { m_rawRegs[regAddr] = value; m_rawSet[regAddr] = true; }   // [slice] raw shadow
    switch (regAddr)
    {   // [rectemplate] per-vertex data registers leave the record template valid; everything else invalidates it
    case GS_REG_RGBAQ: case GS_REG_ST: case GS_REG_UV: case GS_REG_XYZF2: case GS_REG_XYZ2: case GS_REG_XYZF3: case GS_REG_XYZ3: case GS_REG_FOG: break;
    case GS_REG_BITBLTBUF: case GS_REG_TRXPOS: case GS_REG_TRXREG: case GS_REG_TRXDIR: ++m_stateGen; break;   // transfers touch VRAM even when re-issued
    default: if (!sameVal) ++m_stateGen; if (gprof::g_on && regAddr < 0x63u) g_regWriteHist[regAddr].fetch_add(1, std::memory_order_relaxed); break;
    }
    // [floortex0] (default on, PS2X_SRCDIAG=0 disables): the exact TEX0 the FLOOR draws
    // carry NOW — is the tw=10 corruption still present, or did it heal along the way?
    {
        static const bool s_ft = [](){ const char *v = std::getenv("PS2X_SRCDIAG"); return !(v && v[0] == '0'); }();
        if (s_ft && (regAddr == GS_REG_TEX0_1 || regAddr == GS_REG_TEX0_2) &&
            (value & 0x3FFFu) == 10752u)
        {
            static std::atomic<uint32_t> s_fn{0};
            const uint32_t n = s_fn.fetch_add(1);
            if (n < 8u || (n & 0x3FFu) == 0u)
            {
                static FILE *f = std::fopen("/home/z3/Desktop/bt3/work/floortex0.txt", "w");
                if (f)
                {
                    std::fprintf(f, "[floortex0] #%u TEX0_%c raw=0x%016llx tbw=%u psm=%u tw=%u th=%u cbp=%u\n",
                                 n, regAddr == GS_REG_TEX0_1 ? '1' : '2', (unsigned long long)value,
                                 (uint32_t)((value >> 14) & 0x3Fu), (uint32_t)((value >> 20) & 0x3Fu),
                                 (uint32_t)((value >> 26) & 0xFu), (uint32_t)((value >> 30) & 0xFu),
                                 (uint32_t)((value >> 37) & 0x3FFFu));
                    std::fflush(f);
                }
            }
        }
    }
    // PS2X_VIFTEX (GS side): does a terrain-material TEX0 ever reach the GS registers?
    // Pairs with the VIF-DIRECT-side [viftex] log to bracket where the packets vanish.
    {
        static const bool s_vt = [](){ const char *v = std::getenv("PS2X_VIFTEX"); return v && v[0] && v[0] != '0'; }();
        if (s_vt && (regAddr == GS_REG_TEX0_1 || regAddr == GS_REG_TEX0_2))
        {
            const uint32_t t = (uint32_t)(value & 0x3FFFu);
            if (t == 10816u || t == 10880u || t == 10944u || t == 10992u)
            {
                static std::atomic<uint32_t> s_gn{0};
                if (s_gn.fetch_add(1) < 60u)
                    std::fprintf(stderr, "[gstex] TEX0_%c tbp0=%u srcPath=%u\n",
                                 regAddr == GS_REG_TEX0_1 ? '1' : '2', t, m_curSrcPath);
            }
        }
    }
    // [vucell4] PS2X_VUCELL4=<minframe>: per-strip terrain BAND choice = the CLUT row (CSA)
    // in TEX0 writes whose CBP is the terrain band palette (12992). Histogram of csa values.
    {
        static const long s_v4 = [](){ const char *v = std::getenv("PS2X_VUCELL4"); return v && v[0] ? std::atol(v) : -1; }();
        if (s_v4 >= 0 && (regAddr == GS_REG_TEX0_1 || regAddr == GS_REG_TEX0_2) &&
            (uint32_t)((value >> 37) & 0x3FFFu) == 12992u)
        {
            extern std::atomic<uint64_t> g_bt3FrameCount;
            const long fr4 = (long)g_bt3FrameCount.load(std::memory_order_relaxed);
            if (fr4 >= s_v4)
            {
                const uint32_t csa = (uint32_t)((value >> 56) & 0x1Fu);
                // bind-time palette content: mean RGB of the VRAM palette at cbp 12992 (linear bytes)
                uint32_t pm = 0, pt = 0;
                {
                    const size_t off = (size_t)12992 * 256;
                    if (off + 1024 <= m_vramSize)
                    {
                        uint32_t s0 = 0, s1 = 0;
                        for (int i = 0; i < 224; ++i) { const uint8_t *e = m_vram + off + i*4; s0 += e[0]+e[1]+e[2]; }
                        for (int i = 224; i < 256; ++i) { const uint8_t *e = m_vram + off + i*4; s1 += e[0]+e[1]+e[2]; }
                        pm = s0 / (224*3); pt = s1 / (32*3);
                    }
                }
                static std::mutex s_m4; static std::map<uint32_t,uint32_t> s_h4; static std::atomic<uint32_t> s_n4{0};
                static std::atomic<uint64_t> s_pmSum{0}, s_ptSum{0};
                std::lock_guard<std::mutex> lk4(s_m4);
                ++s_h4[csa];
                s_pmSum.fetch_add(pm); s_ptSum.fetch_add(pt);
                const uint32_t n4 = s_n4.fetch_add(1u) + 1u;
                if (n4 <= 3u || (n4 % 200u) == 0u)
                {
                    char raw[64];
                    std::snprintf(raw, sizeof raw, " tbp0=%u tbw=%u tpsm=%u tw=%u th=%u",
                                  (uint32_t)(value & 0x3FFFu), (uint32_t)((value >> 14) & 0x3Fu),
                                  (uint32_t)((value >> 20) & 0x3Fu), (uint32_t)((value >> 26) & 0xFu),
                                  (uint32_t)((value >> 30) & 0xFu));
                    std::string ln = "[vucell4] fr=" + std::to_string(fr4) + " n=" + std::to_string(n4)
                        + " tpsm=" + std::to_string((uint32_t)((value >> 20) & 0x3Fu))
                        + " palMean=" + std::to_string(pm) + " palTop=" + std::to_string(pt)
                        + " avgMean=" + std::to_string(s_pmSum.load()/n4) + " avgTop=" + std::to_string(s_ptSum.load()/n4) + raw + " csa-hist:";
                    for (auto &kv : s_h4) ln += " " + std::to_string(kv.first) + "x" + std::to_string(kv.second);
                    std::fprintf(stderr, "%s\n", ln.c_str());
                }
            }
        }
    }
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        // [prmode] PRMODECONT=0 means the attribute bits (IIP/TME/FGE/ABE/AA1/FST/CTXT/FIX) come from PRMODE,
        // and a PRIM write (register or GIF-tag PRE) only selects the primitive TYPE. We used to copy the
        // attributes from PRIM regardless, so BT3's shadow-silhouette pass (VU1 packets: PRMODECONT=0,
        // PRMODE=0x48 = ctx1/TME off, GIF-tag PRIM with CTXT=1) landed in context 2 = the scene buffer
        // instead of FRAME_1 = fbp336, and the ground-shadow decal sampled an empty silhouette.
        // PS2X_PRMODE=0 restores the old behaviour (A/B).
        static const bool s_prmodeFix = [](){ const char *v = std::getenv("PS2X_PRMODE"); return !(v && v[0] == '0'); }();
        s_primRaw = value;
        m_prim.type = static_cast<GSPrimType>(value & 0x7);
        if (m_prmodecont || !s_prmodeFix) applyPrimAttrs(value);
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        {
            static const bool s_rp = [](){ const char *v=std::getenv("PS2X_RGBAQ_PROBE"); return v&&v[0]&&v[0]!='0'; }();
            if (s_rp) {
                static std::mutex s_rm2; static std::map<uint32_t,uint32_t> s_h2;
                std::lock_guard<std::mutex> lk(s_rm2);
                s_h2[(m_curR)|(m_curG<<8)|(m_curB<<16)|(m_curA<<24)]++;
                static std::atomic<uint32_t> s_c2{0};
                if ((s_c2.fetch_add(1)%20000u)==1u){
                    std::cerr<<"[rgbaq-rl] (r,g,b,a=count):";
                    for(auto&kv:s_h2) std::cerr<<" ("<<(kv.first&0xff)<<","<<((kv.first>>8)&0xff)<<","<<((kv.first>>16)&0xff)<<","<<((kv.first>>24)&0xff)<<")="<<kv.second;
                    std::cerr<<std::endl;
                }
            }
        }
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        {   static const bool s_zr = [](){ const char *v = std::getenv("PS2X_ZRAW"); return v && v[0] && v[0] != '0'; }();
            if (s_zr) { static unsigned long n=0, far=0; static double mx=0.0;
                ++n; if (vtx.z > 10000000.0) ++far; if (vtx.z > mx) mx = vtx.z;
                if ((n % 50000ul) == 0ul)
                    std::fprintf(stderr, "[zraw] REG XYZF2 verts=%lu z>10M=%lu maxz=%.0f\n", n, far, mx); }
        }
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        {   static const bool s_zr = [](){ const char *v = std::getenv("PS2X_ZRAW"); return v && v[0] && v[0] != '0'; }();
            if (s_zr) { static unsigned long n=0, far=0; static double mx=0.0;
                ++n; if (vtx.z > 10000000.0) ++far; if (vtx.z > mx) mx = vtx.z;
                if ((n % 50000ul) == 0ul)
                    std::fprintf(stderr, "[zraw] REG XYZ2  verts=%lu z>10M=%lu maxz=%.0f\n", n, far, mx); }
        }
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
    {   // [prmode] switching the attribute source re-applies the attributes from the now-active register
        static const bool s_prmodeFix = [](){ const char *v = std::getenv("PS2X_PRMODE"); return !(v && v[0] == '0'); }();
        m_prmodecont = (value & 1) != 0;
        if (s_prmodeFix) applyPrimAttrs(m_prmodecont ? s_primRaw : s_prmodeRaw);
        break;
    }
    case GS_REG_PRMODE:
        s_prmodeRaw = value;
        if (!m_prmodecont) applyPrimAttrs(value);
        break;
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    {   // [shadowpass] the Pass-1 shadow context = FRAME_1 fbp336/fbw4 + SCISSOR (1,1)-(254,254), in either order
        extern bool g_spInShadow, g_spFrame336, g_spScissor254; extern uint32_t g_spSets, g_spMscal, g_spLastPC, g_spKicks, g_spLoops, g_spUnpackQw;
        static const bool s_sp = [](){ const char *v = std::getenv("PS2X_SHADOWPASS"); return v && v[0] && v[0] != '0'; }();
        if (s_sp)
        {
            g_spScissor254 = ((value & 0x07FF07FF07FF07FFull) == 0x00fe000100fe0001ull);
            if (!g_spScissor254) g_spInShadow = false;
            else if (g_spFrame336 && !g_spInShadow)
            {
                g_spInShadow = true; const uint32_t n = ++g_spSets;
                if (n <= 4u || (n % 240u) == 0u)
                    std::fprintf(stderr, "[shadowpass] ctx set #%u | since previous set: mscal=%u lastPC=0x%x kicks=%u nloopSum=%u\n", n, g_spMscal, g_spLastPC, g_spKicks, g_spLoops);
                g_spMscal = 0; g_spKicks = 0; g_spLoops = 0; g_spUnpackQw = 0;
            }
        }
    }
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        // PS2X_SCIWATCH: context-2 draws (the fight's ground mesh) see an EMPTY scissor
        // (x0>x1) + garbage zbp — some write scatters junk into ctx2 registers. Log every
        // SCISSOR write; flag inverted rects with the raw value + source path
        // (1=XGKICK 2=DIRECT 3=path3) to identify the corrupting packet.
        {
            static const bool s_sw2 = [](){ const char *v = std::getenv("PS2X_SCIWATCH"); return v && v[0] && v[0] != '0'; }();
            if (s_sw2)
            {
                const bool empty = m_ctx[ci].scissor.x0 > m_ctx[ci].scissor.x1 ||
                                   m_ctx[ci].scissor.y0 > m_ctx[ci].scissor.y1;
                static std::atomic<uint32_t> s_nA{0}, s_nB{0};
                const uint32_t n = (ci == 1) ? s_nB.fetch_add(1) : s_nA.fetch_add(1);
                // SCISSOR_2 (ci==1) is the corrupted one: log its first 48 writes + all
                // empties unconditionally; SCISSOR_1 stays sampled.
                if (empty || (ci == 1 ? n < 48u : n < 8u) || (n % 5000u) == 0u)
                    std::fprintf(stderr, "[sciwatch] SCISSOR_%d = %08x_%08x -> (%u,%u)-(%u,%u)%s src=%u\n",
                                 ci + 1, (uint32_t)(value >> 32), (uint32_t)value,
                                 m_ctx[ci].scissor.x0, m_ctx[ci].scissor.y0,
                                 m_ctx[ci].scissor.x1, m_ctx[ci].scissor.y1,
                                 empty ? " EMPTY!" : "", (unsigned)m_curSrcPath);
            }
        }
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    {   // [shadowpass]
        extern bool g_spInShadow, g_spFrame336, g_spScissor254; extern uint32_t g_spSets, g_spMscal, g_spLastPC, g_spKicks, g_spLoops, g_spUnpackQw;
        g_spFrame336 = ((value & 0xFFFFFFFFull) == 0x40150ull);
        if (!g_spFrame336) g_spInShadow = false;
        else if (g_spScissor254 && !g_spInShadow)
        {
            g_spInShadow = true; const uint32_t n = ++g_spSets;
            if (n <= 4u || (n % 240u) == 0u)
                std::fprintf(stderr, "[shadowpass] ctx set #%u (frame after scissor) | since previous set: mscal=%u lastPC=0x%x kicks=%u nloopSum=%u\n", n, g_spMscal, g_spLastPC, g_spKicks, g_spLoops);
            g_spMscal = 0; g_spKicks = 0; g_spLoops = 0; g_spUnpackQw = 0;
        }
    }
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);

        // We need the transfer state to survive the call to performLocalTo*Transfer
        // This is because transfers can be broken into multiple IMAGE tags and we
        // don't want to start all over again from the initial state
        // The transfer starts officially when TRXDIR is accessed
        m_transferState.x = m_trxpos.dsax;
        m_transferState.y = m_trxpos.dsay;
        m_transferState.total_pixels = m_trxreg.rrw * m_trxreg.rrh;
        if ((m_trxdir == 0u || m_trxdir == 2u) && m_vram)
        {   // [uploadwait] the destination pages must not have a deferred flush still queued (see waitPendingFlush)
            const uint32_t bpp = (m_bitbltbuf.dpsm == GS_PSM_T8 || m_bitbltbuf.dpsm == GS_PSM_T8H) ? 1u
                               : (m_bitbltbuf.dpsm == GS_PSM_T4 || m_bitbltbuf.dpsm == GS_PSM_T4HL || m_bitbltbuf.dpsm == GS_PSM_T4HH) ? 1u
                               : (m_bitbltbuf.dpsm == GS_PSM_CT16 || m_bitbltbuf.dpsm == GS_PSM_CT16S || m_bitbltbuf.dpsm == GS_PSM_Z16 || m_bitbltbuf.dpsm == GS_PSM_Z16S) ? 2u : 4u;
            const uint32_t rowBytes = std::max(1u, (uint32_t)m_bitbltbuf.dbw) * 64u * bpp;
            const uint32_t pageLo = m_bitbltbuf.dbp / 32u;
            const uint32_t pageHi = pageLo + ((m_trxpos.dsay + m_trxreg.rrh) * rowBytes) / 8192u + 1u;
            for (uint32_t p = pageLo; p <= pageHi && p < 512u; ++p) ps2GpuRenderer().waitPendingFlush(p);
        }
        m_transferState.copied_pixels = 0;

        {   // [up10752] PS2X_UP10752=1: per-frame ORDER of uploads into the band-sheet block —
            // two tenants (entry-4 slice vs the real sheet) target dbp 10752; last writer wins.
            static const bool s_u7 = [](){ const char *v = std::getenv("PS2X_UP10752"); return v && v[0] && v[0] != '0'; }();
            if (s_u7 && m_trxdir == 0u && m_bitbltbuf.dbp == 10752u)
            {
                extern std::atomic<uint64_t> g_bt3FrameCount;
                static std::atomic<uint32_t> s_n7{0};
                if (s_n7.fetch_add(1) < 400u)
                    std::fprintf(stderr, "[up10752] fr=%llu dpsm=%u dbw=%u rr=%ux%u dsax=%u dsay=%u\n",
                                 (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed),
                                 (unsigned)m_bitbltbuf.dpsm, (unsigned)m_bitbltbuf.dbw,
                                 (unsigned)m_trxreg.rrw, (unsigned)m_trxreg.rrh,
                                 (unsigned)m_trxpos.dsax, (unsigned)m_trxpos.dsay);
            }
        }
        if (m_trxdir == 2 && m_vram)
        {
            performLocalToLocalTransfer();
        }
        else if (m_trxdir == 1 && m_vram)
        {
            performLocalToHostToBuffer();
        }
        recordTransferDebugEventUnlocked();
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_COLCLAMP:
        // COLCLAMP was ignored (we always clamped, like GL). The GS WRAPS over-range blend
        // results when this is 0, and the palette-arena blend below must match it bit-for-bit.
        m_colclamp = (value & 1u) != 0u;
        break;
    case GS_REG_TEXFLUSH:
    case GS_REG_SCANMSK:
    case GS_REG_FOGCOL:
    case GS_REG_DIMX:
    case GS_REG_DTHE:
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
        break;
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr.fetch_or(0x1);
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_privRegs)
            m_privRegs->csr.fetch_or(0x2);
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        // A DISPFB1 write = a buffer flip. Publish the just-completed frame HERE (aligned
        // to the real flip, not the mid-frame render-kick) and tell the renderer which
        // buffer the CRT now scans out (DISPFB1.FBP, 2048-word pages). One published
        // frame == one displayed frame -> stops the double-buffer flicker/jitter.
        if (GsGpuRenderer::enabled())
        {
            // Default: do NOT publish here — publishing on every DISPFB write can emit
            // partial/extra frames and make animation cadence uneven. Publish only on the
            // per-frame render kick (FUN_00100ab8). Just record the scanned-out buffer.
            ps2GpuRenderer().setDisplay(static_cast<uint32_t>(value & 0x1FFu),
                                        static_cast<uint32_t>((value >> 9) & 0x3Fu));
            // PS2X_DISPFB_PUBLISH: opt-in — publish the just-completed frame HERE (aligned to the
            // real flip) so one published frame == one displayed frame with ALL render targets +
            // the draws that sample them in order. Lets the HUD/composite resolve their RT sources.
            {
                static const bool s_dfPub = [](){ const char *v = std::getenv("PS2X_DISPFB_PUBLISH"); return v && v[0] && v[0] != '0'; }();
                if (s_dfPub) ps2GpuRenderer().swapFrame();
            }
            static const bool s_dfl = [](){ const char *v = std::getenv("PS2X_GPU_DIAG"); return v && v[0] && v[0] != '0'; }();
            static uint64_t s_lastv = ~0ull; static int s_dn = 0;
            if (s_dfl && value != s_lastv && s_dn < 16) { s_lastv = value; ++s_dn;
                std::fprintf(stderr, "[dispfb1] FBP=%u FBW=%u(=%upx) PSM=%u DBX=%u DBY=%u\n",
                             (unsigned)(value & 0x1FFu), (unsigned)((value >> 9) & 0x3Fu),
                             (unsigned)(((value >> 9) & 0x3Fu) * 64u), (unsigned)((value >> 15) & 0x1Fu),
                             (unsigned)((value >> 32) & 0x7FFu), (unsigned)((value >> 43) & 0x7FFu)); }
        }
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        {
            // DISPLAY1: DX[0:11] DY[12:22] MAGH[23:26] MAGV[27:28] DW[32:43] DH[44:54].
            // If the game magnifies a narrow buffer to full screen, the software present
            // honors it but the GPU present shows the raw buffer -> "tiling".
            static const bool s_dl = [](){ const char *v = std::getenv("PS2X_GPU_DIAG"); return v && v[0] && v[0] != '0'; }();
            static uint64_t s_last = ~0ull; static int s_n = 0;
            if (s_dl && value != s_last && s_n < 12)
            {
                s_last = value; ++s_n;
                const uint32_t dx = value & 0xFFFu, dy = (value >> 12) & 0x7FFu;
                const uint32_t magh = (value >> 23) & 0xFu, magv = (value >> 27) & 0x3u;
                const uint32_t dw = (value >> 32) & 0xFFFu, dh = (value >> 44) & 0x7FFu;
                std::fprintf(stderr, "[display1] DX=%u DY=%u MAGH=%u MAGV=%u DW=%u DH=%u -> visible %ux%u (magh+1=%u)\n",
                             dx, dy, magh, magv, dw, dh, (dw + 1) / (magh + 1), (dh + 1) / (magv + 1), magh + 1);
            }
        }
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        {   // [privlog] PS2X_PRIVLOG=1: PMODE / BGCOLOR as seen at each DISPFB2 write (the game's display flip)
            static const bool s_pl = [](){ const char *v = std::getenv("PS2X_PRIVLOG"); return v && v[0] && v[0] != '0'; }();
            if (s_pl && m_privRegs)
            {
                static uint64_t lp = ~0ull, lb = ~0ull; static unsigned n = 0; ++n;
                const uint64_t pm = m_privRegs->pmode, bg = m_privRegs->bgcolor;
                if (pm != lp || bg != lb || (n % 120u) == 0u)
                {
                    lp = pm; lb = bg;
                    std::fprintf(stderr, "[privlog] flip#%u pmode=%016llx en1=%d en2=%d mmod=%d amod=%d slbg=%d alp=%u bgcolor=%06llx dispfb2.fbp=%u\n",
                                 n, (unsigned long long)pm, (int)(pm & 1), (int)((pm >> 1) & 1), (int)((pm >> 5) & 1), (int)((pm >> 6) & 1), (int)((pm >> 7) & 1),
                                 (unsigned)((pm >> 8) & 0xFF), (unsigned long long)(bg & 0xFFFFFF), (unsigned)((value >> 9) & 0x1FF));
                }
            }
        }
        // BT3 (like most games) scans out via output circuit 2: after boot it flips ONLY
        // DISPFB2, never DISPFB1 — so the flip-aligned publish + display hint must live here
        // too (PS2X_DISPFB_PUBLISH on DISPFB1 alone published nothing: frozen black window).
        if (GsGpuRenderer::enabled())
        {
            ps2GpuRenderer().setDisplay(static_cast<uint32_t>(value & 0x1FFu),
                                        static_cast<uint32_t>((value >> 9) & 0x3Fu));
            {
                static const bool s_dfPub = [](){ const char *v = std::getenv("PS2X_DISPFB_PUBLISH"); return v && v[0] && v[0] != '0'; }();
                if (s_dfPub) ps2GpuRenderer().swapFrame();
            }
            static const bool s_dfl = [](){ const char *v = std::getenv("PS2X_GPU_DIAG"); return v && v[0] && v[0] != '0'; }();
            static uint64_t s_lastv = ~0ull; static int s_dn = 0;
            if (s_dfl && value != s_lastv && s_dn < 16) { s_lastv = value; ++s_dn;
                std::fprintf(stderr, "[dispfb2] FBP=%u FBW=%u(=%upx) PSM=%u DBX=%u DBY=%u\n",
                             (unsigned)(value & 0x1FFu), (unsigned)((value >> 9) & 0x3Fu),
                             (unsigned)(((value >> 9) & 0x3Fu) * 64u), (unsigned)((value >> 15) & 0x1Fu),
                             (unsigned)((value >> 32) & 0x7FFu), (unsigned)((value >> 43) & 0x7FFu)); }
        }
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        {   // [display2] same census as [display1] -- BT3 scans out via circuit 2, and the
            // CRTC MAGH here is the authoritative "how wide does the TV draw this buffer".
            static const bool s_d2 = [](){ const char *v = std::getenv("PS2X_GPU_DIAG"); return v && v[0] && v[0] != '0'; }();
            static uint64_t s_l2 = ~0ull; static int s_n2 = 0;
            if (s_d2 && value != s_l2 && s_n2 < 24)
            {
                s_l2 = value; ++s_n2;
                const uint32_t dx = value & 0xFFFu, dy = (value >> 12) & 0x7FFu;
                const uint32_t magh = (value >> 23) & 0xFu, magv = (value >> 27) & 0x3u;
                const uint32_t dw = (value >> 32) & 0xFFFu, dh = (value >> 44) & 0x7FFu;
                std::fprintf(stderr, "[display2] DX=%u DY=%u MAGH=%u MAGV=%u DW=%u DH=%u -> visible %ux%u (magh+1=%u)\n",
                             dx, dy, magh, magv, dw, dh, (dw + 1) / (magh + 1), (dh + 1) / (magv + 1), magh + 1);
            }
        }
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::performLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const u32 sbp = m_bitbltbuf.sbp;
    const u8 sbw = m_bitbltbuf.sbw;
    const u8 spsm = m_bitbltbuf.spsm;
    const u32 dbp = m_bitbltbuf.dbp;
    const u8 dbw = m_bitbltbuf.dbw;
    const u8 dpsm = m_bitbltbuf.dpsm;
    const u32 rrw = m_trxreg.rrw;
    const u32 rrh = m_trxreg.rrh;
    const u32 ssax = m_trxpos.ssax;
    const u32 ssay = m_trxpos.ssay;
    const u32 dsax = m_trxpos.dsax;
    const u32 dsay = m_trxpos.dsay;
    const u32 dir = m_trxpos.dir;

    const u32 total_pixels = rrw * rrh;

    // This VRAM->VRAM transfer clobbers the dest block, so a later byte-identical IMAGE
    // upload to dbp can NOT be skipped (VRAM no longer holds the uploaded content). Drop the
    // dedup entry so the next upload to dbp is treated as changed and actually re-written.
    {
        std::lock_guard<std::mutex> lk(g_uploadHashMx);
        g_uploadHash.erase(dbp);
    }

    // Stamp the DESTINATION pages as uploaded: BT3 streams its stage materials into the
    // atlas slot (tbp 10752, aliasing fbp336) via these local copies, and without the
    // stamp the sampling draws had srcUploaded=false -> the replay's postgate classified
    // them as RT-feedback and SKIPPED them (the flat dark-green terrain in GPU mode).
    // The old reason not to stamp (100% texture re-decode churn) is gone: content-
    // versioned texKeys re-decode only when the copied bytes actually changed.
    { extern GS *g_gsWb; g_gsWb = this; }   // writeback needs a GS to reach VRAM
    ps2GpuRenderer().onVramUpload(dbp, static_cast<uint32_t>(dbw) * rrh); bumpPageUploadGen(dbp, static_cast<uint32_t>(dbw) * rrh);   // [clutpagegen]

    {
        static const bool s_l2l = [](){ const char *v = std::getenv("PS2X_TEX_PROBE"); return v && v[0] && v[0] != '0'; }();
        if (s_l2l)
        {
            static std::atomic<uint32_t> s_n{0};
            uint32_t n = s_n.fetch_add(1);
            if (n < 60u)
                std::cerr << "[gs-l2l] #" << n << " sbp=0x" << std::hex << sbp << " spsm=0x" << (int)spsm
                          << " -> dbp=0x" << dbp << " dpsm=0x" << (int)dpsm << std::dec
                          << " w=" << rrw << " h=" << rrh << " dir=" << dir << std::endl;
        }
    }

    if (total_pixels == 0)
    {
        m_trxdir = 3;
        return;
    }

    // TODO: clean this up / optimize
    switch (dir)
    {
    case 0: // left -> right top -> bottom
    {
        u32 pixel_count = 0;
        while (pixel_count < total_pixels)
        {
            const u32 x = pixel_count % rrw;
            const u32 y = pixel_count / rrw;

            const u32 sx = x + ssax;
            const u32 sy = y + ssay;
            const u32 dx = x + dsax;
            const u32 dy = y + dsay;

            WriteVram(dpsm, dbp, dbw, dx, dy, ReadVram(spsm, sbp, sbw, sx, sy));

            pixel_count++;
        }
    }
    break;


    // left -> right
    // bottom -> top (invert y)
    case 1:
    {
        u32 pixel_count = 0;
        while (pixel_count < total_pixels)
        {
            const u32 x = pixel_count % rrw;
            const u32 y = rrh - (pixel_count / rrw) - 1;

            const u32 sx = x + ssax;
            const u32 sy = y + ssay;
            const u32 dx = x + dsax;
            const u32 dy = y + dsay;

            WriteVram(dpsm, dbp, dbw, dx, dy, ReadVram(spsm, sbp, sbw, sx, sy));

            pixel_count++;
        }
    }
    break;

    // right -> left (invert x)
    // top -> bottom
    case 2:
    {
        u32 pixel_count = 0;
        while (pixel_count < total_pixels)
        {
            const u32 x = rrw - (pixel_count % rrw) - 1;
            const u32 y = pixel_count / rrw;

            const u32 sx = x + ssax;
            const u32 sy = y + ssay;
            const u32 dx = x + dsax;
            const u32 dy = y + dsay;

            WriteVram(dpsm, dbp, dbw, dx, dy, ReadVram(spsm, sbp, sbw, sx, sy));

            pixel_count++;
        }
    }
    break;

    // right to left (invert x)
    // bottom to top (invert y)
    case 3:
    {
        u32 pixel_count = 0;
        while (pixel_count < total_pixels)
        {
            const u32 x = rrw - (pixel_count % rrw) - 1;
            const u32 y = rrh - (pixel_count / rrw) - 1;

            const u32 sx = x + ssax;
            const u32 sy = y + ssay;
            const u32 dx = x + dsax;
            const u32 dy = y + dsay;

            WriteVram(dpsm, dbp, dbw, dx, dy, ReadVram(spsm, sbp, sbw, sx, sy));

            pixel_count++;
        }
    }
    break;

    default:
        break;
    }

    // GPU renderer: also record this VRAM->VRAM copy as an ordered FBO->FBO blit, so
    // render targets the game stages by copying (e.g. the logo blitted into a scratch
    // page then tiled to the display) carry the real rendered content, not stale VRAM
    // (primitives never write PS2 VRAM in GPU mode). bp units (/64 words) -> fbp = bp/32.
    if (GsGpuRenderer::enabled() && rrw > 0 && rrh > 0)
    {
        GsGpuRenderer::DrawCmd t{};
        t.isTransfer = true;
        t.xSrcFbp = sbp / 32u;
        t.xDstFbp = dbp / 32u;
        t.xSX = static_cast<int>(ssax); t.xSY = static_cast<int>(ssay);
        t.xDX = static_cast<int>(dsax); t.xDY = static_cast<int>(dsay);
        t.xW = static_cast<int>(rrw);  t.xH = static_cast<int>(rrh);
        ps2GpuRenderer().recordCmd(t);
        // NOTE: intentionally NOT stamping the copy dest here -- BT3 does per-frame
        // VRAM->VRAM copies of unchanged content, and stamping caused 100% texture-cache
        // re-decode (halved fps). The FBO-blit path reflects the copy for render targets;
        // real texture-content changes come through processImageData (content-hashed).
    }

    m_trxdir = 3;
}

void GS::vertexKick(bool drawing)
{
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    // NOTE: `drawing` (ADC clear) decides ONLY whether this vertex's primitive is
    // rasterized. The vertex-queue bookkeeping below (completion check + window slide)
    // must run for EVERY kick — on hardware an ADC vertex advances the strip/fan window
    // exactly like a drawing vertex. The old early-return here skipped the slide, so an
    // ADC vertex mid-strip desynced the window and later draws assembled triangles from
    // stale slots INCLUDING the ADC-marked junk verts (the fight-arena wedge artifacts).

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    if (!drawing)
        goto slideWindow; // ADC kick: no rasterization, but the window still advances

    {
        {   // [zkick] PS2X_ZKICK=1: at the DRAWING kick, before any rasteriser cull, does the
            // vertex queue carry a far (z>12M) vertex? The .gs says ~8.3% of decoded vertices do,
            // yet ZERO triangles reach the rasteriser's ZSAT check with one. This says which side
            // of drawPrimitive() loses them.
            static const bool s_zk = [](){ const char *v = std::getenv("PS2X_ZKICK"); return v && v[0] && v[0] != '0'; }();
            if (s_zk && (m_prim.type == GS_PRIM_TRISTRIP || m_prim.type == GS_PRIM_TRIANGLE || m_prim.type == GS_PRIM_TRIFAN))
            {
                static unsigned long nn = 0, ff = 0; static float mx = 0.0f;
                const float z0 = m_vtxQueue[0].z, z1 = m_vtxQueue[1].z, z2 = m_vtxQueue[2].z;
                ++nn;
                if (z0 > 12000000.0f || z1 > 12000000.0f || z2 > 12000000.0f) ++ff;
                mx = std::max(mx, std::max(z0, std::max(z1, z2)));
                if ((nn % 20000ul) == 0ul)
                    std::fprintf(stderr, "[zkick] drawing tri-kicks=%lu  queue has far vert=%lu (%.2f%%)  max queue z=%.0f\n",
                                 nn, ff, 100.0 * ff / nn, mx);
            }
        }
        static const bool s_dp = [](){ const char *v = std::getenv("PS2X_DMAPROF"); return v && v[0] && v[0] != '0'; }();
        if (s_dp)
        {
            static std::atomic<uint64_t> s_ns{0}, s_n{0}, s_texN{0};
            auto t0 = std::chrono::steady_clock::now();
            m_rasterizer.drawPrimitive(this);
            s_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count(), std::memory_order_relaxed);
            s_n.fetch_add(1, std::memory_order_relaxed);
            if (m_prim.tme) s_texN.fetch_add(1, std::memory_order_relaxed);
            static std::mutex s_m; static std::chrono::steady_clock::time_point s_l = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(s_m);
            double dt = std::chrono::duration<double>(std::chrono::steady_clock::now()-s_l).count();
            if (dt >= 1.0) {
                std::cerr << "[drawprim] " << (s_ns.load()/1e6/dt) << "ms/s prims/s=" << (uint64_t)(s_n.load()/dt)
                          << " textured/s=" << (uint64_t)(s_texN.load()/dt) << "\n";
                s_ns=0; s_n=0; s_texN=0; s_l=std::chrono::steady_clock::now();
            }
        }
        else m_rasterizer.drawPrimitive(this);
    }
    recordDrawDebugEventUnlocked(needed);
    {
        static const bool s_drawProbe = [](){ const char *v = std::getenv("PS2X_DRAW_PROBE"); return v && v[0] && v[0] != '0'; }();
        if (s_drawProbe)
        {
            static std::atomic<uint64_t> s_draws{0};
            uint64_t d = s_draws.fetch_add(1) + 1u;
            const uint32_t ci = m_prim.ctxt ? 1u : 0u;
            // Histogram of textured-sprite source pointers (tbp0): text glyphs
            // sample the font atlas; if that tbp0 was never uploaded, text is blank.
            if (m_prim.tme)
            {
                static std::mutex s_m;
                static std::map<uint32_t, uint64_t> s_tbp; // key = (psm<<20)|tbp0
                // Also: for text sprites (T4), which DEST framebuffer do they draw to?
                static std::map<uint32_t, uint64_t> s_textDest; // tbp0 -> which fbp
                std::lock_guard<std::mutex> lk(s_m);
                s_tbp[((uint32_t)m_ctx[ci].tex0.psm << 20) | m_ctx[ci].tex0.tbp0]++;
                if (m_ctx[ci].tex0.psm == GS_PSM_T4)
                    s_textDest[(m_ctx[ci].tex0.tbp0 << 12) | (m_ctx[ci].frame.fbp & 0xFFF)]++;
                if ((d % 20000u) == 1u)
                {
                    std::cerr << "[gs-tex] sampled (psm:tbp0=count):";
                    for (auto &kv : s_tbp) std::cerr << " " << std::hex << (kv.first >> 20) << ":0x" << (kv.first & 0xFFFFF) << std::dec << "(" << kv.second << ")";
                    std::cerr << std::endl;
                    std::cerr << "[gs-textdest] T4 (srcTbp0->destFbp=count):";
                    for (auto &kv : s_textDest) std::cerr << " 0x" << std::hex << (kv.first >> 12) << "->fbp" << std::dec << (kv.first & 0xFFF) << "(" << kv.second << ")";
                    std::cerr << std::endl;
                }
            }
            if ((d % 2000u) == 1u)
            {
                const auto &t = m_ctx[ci].tex0;
                std::cerr << "[gs-draw] prims=" << d << " primType=" << (int)m_prim.type
                          << " tex=" << (int)m_prim.tme
                          << " tbp0=0x" << std::hex << t.tbp0 << std::dec
                          << " psm=0x" << std::hex << (int)t.psm << std::dec
                          << " tcc=" << (int)t.tcc << " tfx=" << (int)t.tfx
                          << " cbp=0x" << std::hex << t.cbp << " cpsm=0x" << (int)t.cpsm << std::dec
                          << " abe=" << (int)m_prim.abe << std::endl;
            }
            // One-shot: dump the sampled texture region as BMP under two
            // interpretations (P4 and T8 low-nibble) so we can eyeball which
            // swizzle recovers the glyph atlas.
            if (m_prim.tme && m_ctx[ci].tex0.psm == GS_PSM_T4 && std::getenv("PS2X_TEX_DUMP"))
            {
                static std::atomic<bool> s_done{false};
                bool expected = false;
                if (s_done.compare_exchange_strong(expected, true))
                {
                    const auto &t = m_ctx[ci].tex0;
                    const int W = 128, H = 128;
                    auto dumpBmp = [&](const char *path, uint32_t psm, bool nibble) {
                        FILE *f = std::fopen(path, "wb");
                        if (!f) return;
                        const uint32_t rowBytes = (W * 3 + 3) & ~3u;
                        const uint32_t imgSize = rowBytes * H;
                        uint8_t fh[14] = {'B','M'};
                        uint32_t fsize = 54 + imgSize;
                        std::memcpy(fh + 2, &fsize, 4); uint32_t off = 54; std::memcpy(fh + 10, &off, 4);
                        std::fwrite(fh, 1, 14, f);
                        uint8_t ih[40] = {40,0,0,0}; int32_t w = W, h = H; uint16_t planes = 1, bpp = 24;
                        std::memcpy(ih + 4, &w, 4); std::memcpy(ih + 8, &h, 4);
                        std::memcpy(ih + 12, &planes, 2); std::memcpy(ih + 14, &bpp, 2);
                        std::fwrite(ih, 1, 40, f);
                        std::vector<uint8_t> row(rowBytes, 0);
                        for (int y = H - 1; y >= 0; --y) {
                            for (int x = 0; x < W; ++x) {
                                uint32_t v = ReadVram(psm, t.tbp0, t.tbw ? t.tbw : 1, x, y);
                                uint8_t g = nibble ? static_cast<uint8_t>((v & 0xf) * 17) : static_cast<uint8_t>(v & 0xff);
                                row[x*3] = row[x*3+1] = row[x*3+2] = g;
                            }
                            std::fwrite(row.data(), 1, rowBytes, f);
                        }
                        std::fclose(f);
                    };
                    auto dumpBmpBw = [&](const char *path, uint32_t psm, bool nibble, uint32_t bw) {
                        FILE *f = std::fopen(path, "wb");
                        if (!f) return;
                        const uint32_t rowBytes = (W * 3 + 3) & ~3u; const uint32_t imgSize = rowBytes * H;
                        uint8_t fh[14] = {'B','M'}; uint32_t fsize = 54 + imgSize; std::memcpy(fh+2,&fsize,4); uint32_t off=54; std::memcpy(fh+10,&off,4); std::fwrite(fh,1,14,f);
                        uint8_t ih[40] = {40,0,0,0}; int32_t w=W,h=H; uint16_t planes=1,bpp=24; std::memcpy(ih+4,&w,4); std::memcpy(ih+8,&h,4); std::memcpy(ih+12,&planes,2); std::memcpy(ih+14,&bpp,2); std::fwrite(ih,1,40,f);
                        std::vector<uint8_t> row(rowBytes,0);
                        for (int y=H-1;y>=0;--y){ for(int x=0;x<W;++x){ uint32_t v=ReadVram(psm,t.tbp0,bw,x,y); uint8_t g=nibble?(uint8_t)((v&0xf)*17):(uint8_t)(v&0xff); row[x*3]=row[x*3+1]=row[x*3+2]=g;} std::fwrite(row.data(),1,rowBytes,f);} std::fclose(f);
                    };
                    dumpBmp("/home/z3/Desktop/bt3/work/tex_p4.bmp", GS_PSM_T4, true);
                    dumpBmp("/home/z3/Desktop/bt3/work/tex_t8.bmp", GS_PSM_T8, false);
                    dumpBmpBw("/home/z3/Desktop/bt3/work/tex_p4_bw2.bmp", GS_PSM_T4, true, 2);
                    dumpBmpBw("/home/z3/Desktop/bt3/work/tex_p4_bw4.bmp", GS_PSM_T4, true, 4);
                    dumpBmpBw("/home/z3/Desktop/bt3/work/tex_ct32.bmp", GS_PSM_CT32, false, 2);
                    std::cerr << "[tex-dump] wrote tex_p4.bmp / tex_t8.bmp for tbp0=0x" << std::hex << t.tbp0 << std::dec << std::endl;
                }
            }
            // For PSMT4 text sprites: dump uploads whose dbp page matches the
            // sampled texture's page, to see if/how the font was written there.
            if (m_prim.tme && m_ctx[ci].tex0.psm == GS_PSM_T4)
            {
                static std::atomic<uint32_t> s_corr{0};
                if (s_corr.fetch_add(1) < 3u)
                {
                    const uint32_t page = m_ctx[ci].tex0.tbp0 >> 5; // 32 blocks per page
                    std::lock_guard<std::mutex> lk(g_upMutex);
                    std::cerr << "[t4-corr] tbp0=0x" << std::hex << m_ctx[ci].tex0.tbp0
                              << " cbp=0x" << m_ctx[ci].tex0.cbp << std::dec
                              << " page=" << page << " -- uploads to same page:";
                    int hits = 0;
                    for (auto &r : g_upRecs)
                        if ((r.dbp >> 5) == page && hits++ < 12)
                            std::cerr << " {dbp=0x" << std::hex << r.dbp << " psm=0x" << r.dpsm << std::dec
                                      << " dbw=" << r.dbw << " " << r.w << "x" << r.h
                                      << " @" << r.dsax << "," << r.dsay << "}";
                    std::cerr << " (total-hits=" << hits << " of " << g_upRecs.size() << " uploads)" << std::endl;
                }
            }
        }
    }

slideWindow:
    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    // wrong direction set
    if (m_trxdir != 0 || !m_vram)
    {
        return;
    }
    // A VRAM upload may overwrite a CLUT/palette -> invalidate the per-primitive
    // CLUT cache so the rasterizer rebuilds it (see GSRasterizer::sampleTexture).
    ++m_texUploadGen;

    // no height and width means transfer is invalid
    if (m_trxreg.rrw == 0 || m_trxreg.rrh == 0)
    {
        return;
    }

    u32 dbp = m_bitbltbuf.dbp;
    u8 dbw = std::max<u8>(m_bitbltbuf.dbw, 1u);
    u8 dpsm = m_bitbltbuf.dpsm;

    u32 rrw = m_trxreg.rrw;
    u32 rrh = m_trxreg.rrh;

    // PS2X_TEXUP: log IMAGE-upload destinations. Cross-check against the tbp0 of the BLACK 3D
    // draws (13736/15680...) — if those pages are never uploaded, the fight textures never
    // reach VRAM (streaming/upload-path bug), which is why 3D renders black while HUD works.
    {
        static const bool s_tu = [](){ const char *v = std::getenv("PS2X_TEXUP"); return v && v[0] && v[0] != '0'; }();
        if (s_tu)
        {
            static std::map<uint32_t, uint32_t> s_dbps;
            static uint32_t s_n = 0;
            s_dbps[dbp]++;
            if ((s_n++ % 400u) == 1u)
            {
                std::fprintf(stderr, "[texup] dst blocks seen:");
                int c = 0;
                for (auto &kv : s_dbps) { std::fprintf(stderr, " %u(x%u)", kv.first, kv.second); if (++c > 48) { std::fprintf(stderr, " ..."); break; } }
                std::fprintf(stderr, " | this: dbp=%u psm=%u %ux%u\n", dbp, (unsigned)dpsm, rrw, rrh);
            }
        }
        // PS2X_UPWATCH=<dbp>: log EVERY upload landing in [dbp, dbp+64) with size + a
        // content signature (nonzero density + first bytes). Answers whether the terrain
        // tile slot (e.g. 11008) ever receives real image data during a fight, or only
        // flat/zero fills — i.e. is the delivery missing or the content?
        static const uint32_t s_uw = [](){ const char *v = std::getenv("PS2X_UPWATCH"); return v ? (uint32_t)std::strtoul(v, nullptr, 0) : 0u; }();
        if (s_uw && dbp >= s_uw && dbp < s_uw + 64u)
        {
            static std::atomic<uint32_t> s_un{0};
            const uint32_t n = s_un.fetch_add(1);
            if (n < 40u || (n % 200u) == 0u)
            {
                uint32_t nz = 0, tot = 0;
                uint32_t distinct[8] = {}; uint32_t dcount = 0;
                if (data)
                    for (uint32_t i = 0; i < sizeBytes && i < 4096u; ++i)
                    {
                        ++tot; if (data[i]) ++nz;
                        bool seen = false;
                        for (uint32_t k = 0; k < dcount; ++k) if (distinct[k] == data[i]) { seen = true; break; }
                        if (!seen && dcount < 8u) distinct[dcount++] = data[i];
                    }
                std::fprintf(stderr, "[upwatch] #%u dbp=%u psm=%u %ux%u bytes=%u nz=%u/%u distinctBytes=%u%s\n",
                             n, dbp, (unsigned)dpsm, rrw, rrh, sizeBytes, nz, tot, dcount, dcount >= 8u ? "+" : "");
            }
        }
        // PS2X_CWATCH[=lo,hi]: CONTENT watch over a whole VRAM block range (default
        // 10752..10900 = the fight terrain material slot). Answers the open fork: do the
        // big grass/rock/tree images (128x128 T4=8KB / T8=16KB) still ARRIVE during
        // fights, or did the stage loader stop delivering them? Every upload in range is
        // histogrammed; uploads >= 4KB are logged with content stats and each DISTINCT
        // content is dumped raw (.bin) + as a viewable image (.ppm/.pgm, linear transfer
        // order — no deswizzle needed to see if it's a real picture).
        static const auto s_cw = [](){ std::pair<uint32_t,uint32_t> r{0u,0u};
            const char *v = std::getenv("PS2X_CWATCH");
            if (!v || !v[0] || v[0] == '0') return r;
            char *end = nullptr;
            uint32_t lo = (uint32_t)std::strtoul(v, &end, 0);
            if (lo <= 1u) { r = {10752u, 10900u}; return r; }
            uint32_t hi = lo + 64u;
            if (end && (*end == ',' || *end == '-')) hi = (uint32_t)std::strtoul(end + 1, nullptr, 0);
            r = {lo, hi}; return r; }();
        if (s_cw.second > s_cw.first && dbp >= s_cw.first && dbp < s_cw.second)
        {
            static std::mutex s_cm;
            static std::map<uint64_t, uint32_t> s_hist;      // (dbp,psm,bytes) -> count
            static std::set<uint64_t> s_dumped;              // content hashes already dumped
            static uint32_t s_total = 0, s_big = 0, s_dumpN = 0;
            // Deferred deswizzled index views: the raw IMAGE stream is page-swizzled for
            // T4/T8 aliasing, so linear views are unreadable. After the PREVIOUS dumped
            // upload has landed in VRAM (i.e. on the next cwatch hit), read it back through
            // the real T4/T8 address mapping at plausible widths -> *_t4w128/t4w256/t8w128
            // .pgm. A square terrain tile (grass etc.) becomes recognizable regardless of
            // which slot/psm it streams through.
            static struct { bool armed = false; uint32_t dbp = 0, bytes = 0; char base[176]; } s_pend;
            // PS2X_CWATCH_FIGHT=1: hold dumps until the rasterizer has seen a 3D map draw
            // (set via g_ps2xMapDrawSeen) so the 48/96 dump slots aren't burned on the
            // boot logos/fonts like the first full-VRAM run.
            static const bool s_cwFight = [](){ const char *v = std::getenv("PS2X_CWATCH_FIGHT"); return v && v[0] && v[0] != '0'; }();
            static uint32_t s_distinct = 0;
            std::lock_guard<std::mutex> lk(s_cm);
            if (s_pend.armed && m_vram)
            {
                s_pend.armed = false;
                const uint32_t vmask = m_vramSize ? (m_vramSize - 1u) : 0x3FFFFFu;
                char pth[224];
                // T4 views (2 px/byte): width 128 (tbw=2) and 256 (tbw=4)
                for (uint32_t wv = 128; wv <= 256; wv += 128)
                {
                    const uint32_t hv = std::min<uint32_t>(s_pend.bytes * 2u / wv, 256u);
                    if (hv < 8) continue;
                    std::snprintf(pth, sizeof(pth), "%s_t4w%u.pgm", s_pend.base, wv);
                    if (FILE *f = std::fopen(pth, "wb"))
                    {
                        std::fprintf(f, "P5\n%u %u\n255\n", wv, hv);
                        for (uint32_t yy = 0; yy < hv; ++yy)
                            for (uint32_t xx = 0; xx < wv; ++xx)
                            {
                                const uint32_t na = GSPSMT4::addrPSMT4(s_pend.dbp, wv / 64u, xx, yy);
                                const uint8_t bv = m_vram[(na >> 1) & vmask];
                                const uint8_t g = (uint8_t)((((na & 1u) ? (bv >> 4) : bv) & 0x0Fu) * 17u);
                                std::fwrite(&g, 1, 1, f);
                            }
                        std::fclose(f);
                    }
                }
                // T8 view (1 px/byte): width 128 (tbw=2)
                {
                    const uint32_t wv = 128, hv = std::min<uint32_t>(s_pend.bytes / wv, 256u);
                    if (hv >= 8)
                    {
                        std::snprintf(pth, sizeof(pth), "%s_t8w%u.pgm", s_pend.base, wv);
                        if (FILE *f = std::fopen(pth, "wb"))
                        {
                            std::fprintf(f, "P5\n%u %u\n255\n", wv, hv);
                            for (uint32_t yy = 0; yy < hv; ++yy)
                                for (uint32_t xx = 0; xx < wv; ++xx)
                                {
                                    const uint32_t na = GSPSMT8::addrPSMT8(s_pend.dbp, wv / 64u, xx, yy);
                                    std::fwrite(&m_vram[na & vmask], 1, 1, f);
                                }
                            std::fclose(f);
                        }
                    }
                }
            }
            ++s_total;
            s_hist[((uint64_t)dbp << 32) | ((uint64_t)dpsm << 24) | (sizeBytes & 0xFFFFFFu)]++;
            if ((s_total % 400u) == 1u)
            {
                std::fprintf(stderr, "[cwatch-hist] total=%u big(>=4K)=%u distinctBig=%u | dbp/psm/bytes(xN):", s_total, s_big, s_distinct);
                int c = 0;
                for (auto &kv : s_hist)
                {
                    std::fprintf(stderr, " %u/%u/%u(x%u)", (uint32_t)(kv.first >> 32),
                                 (uint32_t)((kv.first >> 24) & 0xFFu), (uint32_t)(kv.first & 0xFFFFFFu), kv.second);
                    if (++c > 24) { std::fprintf(stderr, " ..."); break; }
                }
                std::fprintf(stderr, "\n");
            }
            if (data && sizeBytes >= 4096u)
            {
                ++s_big;
                uint32_t nz = 0; bool seen[256] = {};
                uint64_t h = 1469598103934665603ull;
                for (uint32_t i = 0; i < sizeBytes; ++i)
                {
                    const uint8_t bb = data[i];
                    if (bb) ++nz;
                    seen[bb] = true;
                    h = (h ^ bb) * 1099511628211ull;
                }
                uint32_t distinct = 0;
                for (int i = 0; i < 256; ++i) if (seen[i]) ++distinct;
                const bool rich = distinct >= 16u;
                std::fprintf(stderr, "[cwatch] BIG #%u dbp=%u psm=%u %ux%u bytes=%u nz=%.0f%% distinct=%u hash=%016llx%s\n",
                             s_big, dbp, (unsigned)dpsm, rrw, rrh, sizeBytes,
                             100.0 * nz / sizeBytes, distinct, (unsigned long long)h, rich ? " RICH" : "");
                // Register hashes ONLY while the gate is open — otherwise a content first
                // uploaded pre-fight (loading screen) poisons the dedup set and can never
                // be dumped once the fight starts (bug found via the 415x-identical 64KB
                // upload to 11280 that never dumped).
                const bool gateOpen = !s_cwFight || g_ps2xMapDrawSeen.load(std::memory_order_relaxed);
                const bool newBig = gateOpen && s_dumped.insert(h).second;
                if (newBig) ++s_distinct;
                if (newBig && s_dumpN < 96u)
                {
                    char pb[192];
                    std::snprintf(pb, sizeof(pb), "/home/z3/Desktop/bt3/work/cwatch_%02u_dbp%u_psm%u_%ux%u_%016llx.bin",
                                  s_dumpN, dbp, (unsigned)dpsm, rrw, rrh, (unsigned long long)h);
                    if (FILE *f = std::fopen(pb, "wb")) { std::fwrite(data, 1, sizeBytes, f); std::fclose(f); }
                    // Viewable image of the raw transfer stream. CT32: RGB. T8: index gray.
                    // T4: nibble gray (low nibble = first pixel).
                    uint32_t bpr = 0; // bytes per transfer row
                    if (dpsm == GS_PSM_CT32) bpr = rrw * 4u;
                    else if (dpsm == GS_PSM_T8) bpr = rrw;
                    else if (dpsm == GS_PSM_T4) bpr = (rrw + 1u) / 2u;
                    if (bpr)
                    {
                        const uint32_t rows = std::min(rrh, sizeBytes / bpr);
                        std::snprintf(pb, sizeof(pb), "/home/z3/Desktop/bt3/work/cwatch_%02u_dbp%u_psm%u_%ux%u_%016llx.%s",
                                      s_dumpN, dbp, (unsigned)dpsm, rrw, rrh, (unsigned long long)h,
                                      dpsm == GS_PSM_CT32 ? "ppm" : "pgm");
                        if (FILE *f = std::fopen(pb, "wb"))
                        {
                            if (dpsm == GS_PSM_CT32)
                            {
                                std::fprintf(f, "P6\n%u %u\n255\n", rrw, rows);
                                for (uint32_t i = 0; i < rrw * rows; ++i)
                                    std::fwrite(data + i * 4u, 1, 3, f);
                            }
                            else
                            {
                                std::fprintf(f, "P5\n%u %u\n255\n", rrw, rows);
                                for (uint32_t y2 = 0; y2 < rows; ++y2)
                                    for (uint32_t x2 = 0; x2 < rrw; ++x2)
                                    {
                                        uint8_t g;
                                        if (dpsm == GS_PSM_T8) g = data[y2 * bpr + x2];
                                        else { const uint8_t bv = data[y2 * bpr + x2 / 2u];
                                               g = (uint8_t)((((x2 & 1u) ? (bv >> 4) : bv) & 0x0Fu) * 17u); }
                                        std::fwrite(&g, 1, 1, f);
                                    }
                            }
                            std::fclose(f);
                        }
                    }
                    std::fprintf(stderr, "[cwatch] dumped content #%u -> work/cwatch_%02u_dbp%u_*\n", s_dumpN, s_dumpN, dbp);
                    // Arm the deferred deswizzled readback (runs on the next cwatch hit,
                    // after this upload has been written into VRAM).
                    s_pend.armed = true;
                    s_pend.dbp = dbp;
                    s_pend.bytes = sizeBytes;
                    std::snprintf(s_pend.base, sizeof(s_pend.base), "/home/z3/Desktop/bt3/work/cwatch_%02u_dbp%u_psm%u_%ux%u_%016llx",
                                  s_dumpN, dbp, (unsigned)dpsm, rrw, rrh, (unsigned long long)h);
                    ++s_dumpN;
                }
            }
        }
    }

    // Stamp only the VRAM pages this upload wrote so the GPU cache re-decodes just the
    // affected textures -- BUT skip that for REDUNDANT re-uploads (identical content to
    // the same dest). BT3 re-uploads its UI/font textures every frame; without this the
    // GPU cache misses 100% and re-decodes ~4M texels/s (~870ms/s = the fps gate). Hash
    // the source bytes; only invalidate when the content actually changed.
    bool uploadUnchanged = false; // set when this IMAGE is byte-identical to the last to dbp
    if (GsGpuRenderer::enabled())
    {
        bool changed = true;
        if (data && sizeBytes > 0u)
        {
            uint64_t h = 1469598103934665603ull;
            {   // [uphash8] 8 bytes per FNV step instead of 1 (the byte loop was 7% of the guest thread: a serial
                // multiply chain per byte over every upload). Only equality with the previous hash matters.
                uint32_t i = 0;
                for (; i + 8u <= sizeBytes; i += 8u) { uint64_t q; std::memcpy(&q, data + i, 8u); h = (h ^ q) * 1099511628211ull; }
                for (; i < sizeBytes; ++i) h = (h ^ data[i]) * 1099511628211ull;
            }
            h ^= (static_cast<uint64_t>(sizeBytes) << 1) ^ (static_cast<uint64_t>(dpsm) << 40);
            std::lock_guard<std::mutex> lk(g_uploadHashMx);
            uint64_t &last = g_uploadHash[dbp];
            changed = (last != h);
            last = h;
        }
        uploadUnchanged = !changed;
        static const bool s_updProbe = [](){ const char *v = std::getenv("PS2X_UPD_PROBE"); return v && v[0] && v[0] != '0'; }();
        if (s_updProbe)
        {
            static std::atomic<uint32_t> s_chg{0}, s_unchg{0};
            static std::map<uint32_t,uint32_t> s_chgDst; static std::mutex s_m;
            (changed ? s_chg : s_unchg).fetch_add(1);
            if (changed) { std::lock_guard<std::mutex> lk(s_m); s_chgDst[dbp]++; }
            static std::atomic<uint32_t> s_t{0};
            if ((s_t.fetch_add(1) % 240u) == 1u)
            {
                std::lock_guard<std::mutex> lk(s_m);
                std::cerr << "[upd] changed=" << s_chg.load() << " unchanged=" << s_unchg.load() << " | top-changed-dbp:";
                std::vector<std::pair<uint32_t,uint32_t>> v(s_chgDst.begin(), s_chgDst.end());
                std::sort(v.begin(), v.end(), [](auto&a,auto&b){return a.second>b.second;});
                for (size_t i=0;i<v.size()&&i<6;++i) std::cerr << " " << v[i].first << "(" << v[i].second << "b" << dbw << ")";
                std::cerr << "\n";
            }
        }
        if (changed)
            ps2GpuRenderer().onVramUpload(dbp, static_cast<uint32_t>(dbw) * rrh);
        {   // [groupviz] PS2X_GROUPVIZ=<dbp>: per-frame ordinal of palette uploads to this block;
            // the rasterizer tints each palette by it -> replay frames become group maps.
            static const uint32_t s_gvDbp = [](){ const char *v = std::getenv("PS2X_GROUPVIZ");
                                                  return v && v[0] ? (uint32_t)std::atoi(v) : 0u; }();
            if (s_gvDbp && dbp == s_gvDbp)
            {
                extern int g_ps2ReplayVsync;
                extern uint32_t g_gvGroupOrdinal;
                static int s_lastVs = -1;
                if (g_ps2ReplayVsync != s_lastVs) { s_lastVs = g_ps2ReplayVsync; g_gvGroupOrdinal = 0; }
                ++g_gvGroupOrdinal;
            }
        }
        // [atomicclut] bump the CLUT-page generation only when the TRANSFER COMPLETES, not per
        // GIF IMAGE packet: a palette upload spanning multiple packets used to bump the gen on
        // every chunk, so an interleaved draw's ensureClutCache rebuilt from HALF-WRITTEN VRAM.
        // Measured on the user's hillmove.gs: 16% of terrain draws paired with palette contents
        // that never existed in the stream (blends of adjacent versions) = the moving dark/light
        // terrain patches. Consumers of m_pageUploadGen = the CLUT cache key only.
        // PS2X_ATOMICCLUT=0 restores the per-chunk bump.
        {
            // DEFAULT OFF (2026-09-01): shipped default-on it broke LIVE menus (overbright) -- a
        // palette upload whose transfer never satisfies copied>=total leaves the page
        // generation stale forever. It also never fixed anything real (the "rogue" pairings
        // it targeted were authentic 64-byte palette patches). Opt-in for experiments only.
        static const bool s_atomic = [](){ const char *v = std::getenv("PS2X_ATOMICCLUT"); return v && v[0] && v[0] != '0'; }();
            if (!s_atomic || m_transferState.copied_pixels >= m_transferState.total_pixels)
                bumpPageUploadGen(dbp, static_cast<uint32_t>(dbw) * rrh);   // [clutpagegen]
        }
    }

    {
        static const bool s_texProbe = [](){ const char *v = std::getenv("PS2X_TEX_PROBE"); return v && v[0] && v[0] != '0'; }();
        if (s_texProbe)
        {
            // Histogram of upload DESTINATIONS so we can see which VRAM blocks get
            // texture data (does block 12288/0x3000 ever get one?).
            static std::mutex s_umDst; static std::map<uint32_t, uint32_t> s_dst;
            {
                std::lock_guard<std::mutex> lk(s_umDst);
                uint32_t &c = s_dst[dbp]; c++;
                static std::atomic<uint32_t> s_n{0};
                if ((s_n.fetch_add(1) % 2000u) == 1u) {
                    std::cerr << "[upload-dst]";
                    for (auto &kv : s_dst)
                        std::cerr << " dbp=" << kv.first << "(0x" << std::hex << kv.first << std::dec
                                  << ")x" << kv.second;
                    std::cerr << std::endl;
                }
            }
            recordUpload(dbp, dpsm, dbw, rrw, rrh, m_trxpos.dsax, m_trxpos.dsay);
            // Reliable offline diagnosis: dump the whole 4MB GS VRAM once, after
            // enough uploads that the font atlas is present, regardless of whether
            // text ever draws this run. Analyze swizzles offline in Python.
            if (std::getenv("PS2X_VRAM_DUMP"))
            {
                static std::atomic<uint32_t> s_uc{0};
                uint32_t uc = s_uc.fetch_add(1);
                // Re-dump every 2000 uploads past the first 6000 (overwrite): the final
                // file reflects VRAM at quit, not an arbitrary early moment.
                static const uint32_t s_thr = [](){ const char *v = std::getenv("PS2X_VRAM_DUMP"); uint32_t n = v ? (uint32_t)std::strtoul(v, nullptr, 0) : 0u; return n > 1u ? n : 6000u; }();
                if (uc >= s_thr && (uc % 500u) == 0u && m_vram)
                {
                    FILE *vf = std::fopen("/home/z3/Desktop/bt3/work/vram.bin", "wb");
                    if (vf) { std::fwrite(m_vram, 1, 4u * 1024u * 1024u, vf); std::fclose(vf); }
                    std::cerr << "[vram-dump] wrote 4MB VRAM at upload #" << uc << std::endl;
                }
            }
            static std::mutex s_um;
            static std::map<uint32_t, uint64_t> s_updbp; // key = (dbp<<8)|dpsm
            static std::atomic<uint32_t> s_up{0};
            uint32_t u = s_up.fetch_add(1) + 1u;
            std::lock_guard<std::mutex> lk(s_um);
            s_updbp[(dbp << 8) | dpsm]++;
            if ((u % 4000u) == 1u)
            {
                std::cerr << "[gs-upload-hist] (dbp:psm=count):";
                for (auto &kv : s_updbp) std::cerr << " 0x" << std::hex << (kv.first >> 8) << ":" << (kv.first & 0xff) << std::dec << "=" << kv.second;
                std::cerr << std::endl;
            }
        }
    }
    u32 dsax = m_trxpos.dsax;
    u32 dsay = m_trxpos.dsay;

    u32 data_offset = 0;

    // OPT-IN (PS2X_SKIPUP=1, default OFF): skip the swizzled per-pixel VRAM write when this
    // IMAGE is byte-identical to the last one to the same dest block (VRAM already holds it).
    // Modest win (~18% of upload cost on streaming screens) but UNSAFE in general: a loading
    // screen that uploads then READS BACK VRAM (local-to-host) gets stale data and hangs --
    // observed "stuck in loading after logos". Left opt-in for experiments; not default.
    {
        static const bool s_skip = [](){ const char *v = std::getenv("PS2X_SKIPUP"); return v && v[0] && v[0] != '0'; }();
        if (uploadUnchanged && s_skip)
            return;
    }

    static const bool s_uploadRow = [](){ const char *v = std::getenv("PS2X_UPLOADROW"); return !(v && v[0] == '0'); }();   // [uploadrow]
    // remove the format branching from the loops
    // TODO: fixup copypasta
    switch (dpsm)
    {
    case GS_PSM_CT32:
        // [uploadrow] whole row segments through the bulk writer (same per-pixel addresses, same wrap and
        // deactivation points as the per-pixel loop it replaces). PS2X_UPLOADROW=0 = per-pixel.
        if (s_uploadRow)
        {
            while (data_offset + 4u <= sizeBytes)
            {
                const uint32_t rowLeft = rrw - (m_transferState.copied_pixels % rrw);
                const uint32_t dataLeft = (sizeBytes - data_offset) / 4u;
                const uint32_t totLeft = m_transferState.total_pixels - m_transferState.copied_pixels;
                uint32_t run = std::min(rowLeft, std::min(dataLeft, totLeft));
                if (run == 0u) break;
                if (((uintptr_t)&data[data_offset] & 3u) == 0u)
                    GSMem::WriteRowCT32(m_vram, dbp, dbw, m_transferState.x, m_transferState.x + run, m_transferState.y, reinterpret_cast<const u32 *>(&data[data_offset]), nullptr);
                else
                {   static thread_local std::vector<u32> tmp; if (tmp.size() < run) tmp.resize(run);
                    std::memcpy(tmp.data(), &data[data_offset], (size_t)run * 4u);
                    GSMem::WriteRowCT32(m_vram, dbp, dbw, m_transferState.x, m_transferState.x + run, m_transferState.y, tmp.data(), nullptr); }
                m_transferState.x += run; m_transferState.copied_pixels += run; data_offset += run * 4u;
                if ((m_transferState.copied_pixels % rrw) == 0) { m_transferState.x = dsax; m_transferState.y++; }
                if (m_transferState.copied_pixels >= m_transferState.total_pixels) { m_trxdir = 3; m_transferState.total_pixels = 0; break; }
            }
            break;
        }
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WriteCT32(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 4;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_Z32:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WriteZ32(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 4;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_CT24:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WriteCT24(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 3;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_Z24:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WriteZ24(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 3;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_CT16:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WriteCT16(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_Z16:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WriteZ16(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_CT16S:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WriteCT16S(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_Z16S:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WriteZ16S(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_T8:
        if (s_uploadRow)
        {   // [uploadrow]
            while (data_offset < sizeBytes)
            {
                const uint32_t rowLeft = rrw - (m_transferState.copied_pixels % rrw);
                const uint32_t dataLeft = sizeBytes - data_offset;
                const uint32_t totLeft = m_transferState.total_pixels - m_transferState.copied_pixels;
                uint32_t run = std::min(rowLeft, std::min(dataLeft, totLeft));
                if (run == 0u) break;
                GSMem::WriteRowP8(m_vram, dbp, dbw, m_transferState.x, m_transferState.x + run, m_transferState.y, &data[data_offset]);
                m_transferState.x += run; m_transferState.copied_pixels += run; data_offset += run;
                if ((m_transferState.copied_pixels % rrw) == 0) { m_transferState.x = dsax; m_transferState.y++; }
                if (m_transferState.copied_pixels >= m_transferState.total_pixels) { m_trxdir = 3; m_transferState.total_pixels = 0; break; }
            }
            break;
        }
        while (data_offset < sizeBytes)
        {
            u8 c = data[data_offset];

            GSMem::WriteP8(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_T8H:
        while (data_offset < sizeBytes)
        {
            u8 c = data[data_offset];

            GSMem::WriteP8H(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;
    case GS_PSM_T4:
        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WriteP4(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WriteP4(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;
    case GS_PSM_T4HL:
        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WriteP4HL(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WriteP4HL(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;
    case GS_PSM_T4HH:
        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WriteP4HH(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WriteP4HH(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;
    }

    // PS2X_GRASSHACK: LOOK-TEST for the one remaining SW map bug (ground-base XGKICK draws
    // execute one upload early and sample the SKY-resident 10752 slot). Capture a shadow of
    // the slot right after the MOUNTAINS+GRASS image (128KB to 10752) lands; the rasterizer
    // swaps it in for ground-region draws (see drawPrimitive). NOT a fix — an ordering
    // simulation to finally SEE the grass.
    if (g_ps2xGrassHack && dbp == 10752u && sizeBytes == 131072u && m_vram)
    {
        std::memcpy(g_ps2xGrassShadow, m_vram + 10752u * 256u, 131072u);
        g_ps2xGrassShadowValid.store(true, std::memory_order_release);
    }

    // [fmvblit] FMVs are presented straight out of VRAM. Measured: the movie arrives as 16x16
    // MACROBLOCK uploads (psm=PSMCT32, bw=8 -> 512 wide) into the BACK buffer (fbp 0 while the
    // display is on 112), and the game then flips -- it issues no primitives at all for the movie
    // (prims/sec=0). Software presents by reading VRAM so it shows the picture; GPU mode composites
    // FBOs and picks the display buffer from those that RECEIVED DRAWS, so there is nothing to
    // present and the screen is black.
    //
    // Record which framebuffer the macroblocks are filling. The blit itself is emitted once, at the
    // flip that makes this buffer the display (see maybeEmitFmvBlit) -- emitting per upload would
    // mean ~800 fullscreen blits per frame.
    if (GsGpuRenderer::enabled() && m_vram && dpsm == 0u && dbw >= 8u)
    {
        g_fmvGs = this;
        g_fmvPendingFbp.store(dbp / 32u, std::memory_order_relaxed);
        g_fmvPendingBw.store(dbw, std::memory_order_relaxed);
    }
}


void GS::performLocalToHostToBuffer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;

    if (!m_vram)
        return;

    uint32_t sbp = m_bitbltbuf.sbp;
    uint8_t sbw = std::max<u8>(m_bitbltbuf.sbw, 1u);
    uint8_t spsm = m_bitbltbuf.spsm;
    uint32_t rrw = m_trxreg.rrw;
    uint32_t rrh = m_trxreg.rrh;
    uint32_t ssax = m_trxpos.ssax;
    uint32_t ssay = m_trxpos.ssay;

    u32 bpp = GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm));

    u32 pixel_total = rrw * rrh;
    u32 bytes_total = (pixel_total * bpp) / 8;

    m_localToHostBuffer.reserve(bytes_total);

    u32 pixel_count = 0;
    while (pixel_count < pixel_total)
    {
        const u32 x = pixel_count % rrw;
        const u32 y = pixel_count / rrw;

        const u32 v = ReadVram(spsm, sbp, sbw, x + ssax, y + ssay);

        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            m_localToHostBuffer.push_back((v >> 16) & 0xFF);
            m_localToHostBuffer.push_back((v >> 24) & 0xFF);
            break;
        case 24:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            m_localToHostBuffer.push_back((v >> 16) & 0xFF);
            break;
        case 16:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            break;
        case 8:
            m_localToHostBuffer.push_back(v);
            break;
        case 4:
        {
            const u32 v2 = ReadVram(spsm, sbp, sbw, x + ssax + 1, y + ssay);

            m_localToHostBuffer.push_back(v | ((v2 & 0xF) << 4));
            pixel_count++;
            break;
        }
        default:
            break;
        }

        pixel_count++;
    }
}

bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(this, m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(this, activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!dst || maxBytes == 0)
        return 0;
    size_t avail = m_localToHostBuffer.size() - m_localToHostReadPos;
    if (avail == 0)
        return 0;
    size_t toCopy = (avail < maxBytes) ? avail : static_cast<size_t>(maxBytes);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, toCopy);
    m_localToHostReadPos += toCopy;
    return static_cast<uint32_t>(toCopy);
}


// [fmvblit] Drive the movie from the decoder, not from the display registers.
//
// Measured: during an FMV the game writes the picture into VRAM as 16x16 macroblocks and issues
// NO primitives and NO DISPFB writes, so nothing in GPU mode ever records a draw or publishes a
// frame -- the renderer sits idle and the screen is black, while software presents by reading
// VRAM and shows the movie. sceMpegGetPicture is the one event that happens exactly once per
// movie frame, so emit the fullscreen blit there and publish it ourselves.
void ps2GsEmitFmvFrame()
{
    if (!GsGpuRenderer::enabled())
        return;
    static const bool s_on = [](){ const char *v = std::getenv("PS2X_FMVBLIT"); return !(v && v[0] == '0'); }();
    if (!s_on)
        return;
    GS *gs = g_fmvGs;
    const uint32_t fbp = g_fmvPendingFbp.load(std::memory_order_relaxed);
    if (!gs || fbp == 0xFFFFFFFFu)
        return;

    const uint32_t bw = std::max(1u, g_fmvPendingBw.load(std::memory_order_relaxed));
    const uint32_t w = bw * 64u;
    const uint32_t h = 448u;

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            const uint32_t px = gs->ReadVram(0u, fbp * 32u, bw, x, y);
            uint8_t *o = rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
            o[0] = (uint8_t)(px & 0xFFu);
            o[1] = (uint8_t)((px >> 8) & 0xFFu);
            o[2] = (uint8_t)((px >> 16) & 0xFFu);
            o[3] = 0xFFu;
        }

    const uint64_t key = 0xF00D0000ull | fbp;
    ps2GpuRenderer().putTexture(key, std::move(rgba), (int)w, (int)h, fbp, fbp + 1u);

    GsGpuRenderer::DrawCmd c{};
    c.texKey = key;
    c.isTriangle = false;
    c.destFbp = fbp;
    c.destFbw = bw;
    c.srcTexW = (int)w; c.srcTexH = (int)h;
    c.sx = 0; c.sy = 0; c.sw = (int)w; c.sh = (int)h;
    c.dx0 = 0.0f; c.dy0 = 0.0f; c.dx1 = (float)w; c.dy1 = (float)h;
    c.su0 = 0.0f; c.sv0 = 0.0f; c.su1 = (float)w; c.sv1 = (float)h;
    c.r = 128; c.g = 128; c.b = 128; c.a = 128;
    ps2GpuRenderer().recordCmd(c);
    ps2GpuRenderer().swapFrame(); // nothing else publishes during a movie

    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1) < 4u)
        std::fprintf(stderr, "[fmvblit] frame blit fbp=%u %ux%u published\n", fbp, w, h);
}
