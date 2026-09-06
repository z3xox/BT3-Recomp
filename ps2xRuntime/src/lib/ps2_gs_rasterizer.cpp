static thread_local int g_subDx0 = 0, g_subDxW = 0;   // [subdecode] decode window of the texture being recorded (0 = whole)
#include "runtime/ps2_guestprof.h"
#include "runtime/ps2_texreplace.h"   // [texreplace]
#include <map>
#include <array>
#include <set>
#include "runtime/ps2_gs_rasterizer.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_gpu_renderer.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt4.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <memory>
static const struct AlphaLut { uint8_t v[256]; AlphaLut() { for (int i = 0; i < 256; ++i) v[i] = (uint8_t)std::min(255u, (unsigned)i * 255u / 128u); } uint8_t operator[](uint32_t i) const { return v[i & 0xFFu]; } } kAlpha128To255;   // [fastdec] GS alpha (128 = 1.0) -> texture alpha
extern "C" uint32_t ps2xDeferCoverFor(uint32_t page);   // [defercover] ps2_gs_gpu_renderer.cpp
extern "C" uint32_t ps2xDeferCoverFbp(uint32_t page);   // [clutcover] ps2_gs_gpu_renderer.cpp
extern "C" bool ps2xLinearFetch(uint32_t tbp0, uint32_t psm, uint32_t tbw, uint32_t u0, int subW, int H, uint32_t *dst);   // [linvram]
namespace GSMem { void ReadRowZ16(const u8* data, u32 bp, u32 bw, u32 x0, u32 x1, u32 y, u16* dst); void ReadRowZ16S(const u8* data, u32 bp, u32 bw, u32 x0, u32 x1, u32 y, u16* dst); }   // [fastdec] ps2_gs_memory.cpp
namespace GSMem { void ReadRowP8(const u8* data, u32 bp, u32 bw, u32 x0, u32 x1, u32 y, u8* dst); void ReadRowP4(const u8* data, u32 bp, u32 bw, u32 x0, u32 x1, u32 y, u8* dst); }   // [fastdec]

// [pixcenter] destinations that receive the GS-corner -> GL-centre half-pixel shift (display buffers only).
static bool pixCenterDestOk(uint32_t fbp)
{
    static const std::vector<uint32_t> s_list = [](){ std::vector<uint32_t> r; const char *v = std::getenv("PS2X_PIXCENTER_FBPS");
        const char *p = v && v[0] ? v : "0,112"; while (*p) { char *e = nullptr; long n = std::strtol(p, &e, 10); if (e == p) break; r.push_back((uint32_t)n); p = (*e == ',') ? e + 1 : e; } return r; }();
    for (uint32_t f : s_list) if (f == fbp) return true;
    return false;
}

// [deferdec] PS2X_DEFERDEC=1: a texture whose source page is GL-dirty is decoded by the GL thread
// (in command order, right after it writes the page back) instead of making the guest wait.
static const bool s_deferDec = [](){ const char *v = std::getenv("PS2X_DEFERDEC"); return v && v[0] && v[0] != '0'; }();
// [p8twinskip] 2026-09-03: PSMT8H reads of the MASK page (tbp0 7168, not drawn back into f224) are served at
// replay by the decode-twin (renderer [p8twin], baked default) and never touch the decoded texture -- yet the
// record path still barrier-requested page 224 (which staged a full-resolution depth readback every frame) and
// decoded the 512x512 T8H view (~5 per frame, ~20 ms/s). Mirror the renderer's serve condition here and skip
// both. PS2X_P8TWINSKIPDEC=0 restores the old behaviour.
// [reqcensus] PS2X_DECCENSUS=1: which draw classes barrier-request a page (tbp0/psm/w/h/dest, site)
static inline void reqCensus(int site, uint32_t tbp0, uint32_t psm, int w, int h, uint32_t dest)
{
    static const bool s_on = [](){ const char *v = std::getenv("PS2X_DECCENSUS"); return v && v[0] && v[0] != '0'; }();
    if (!s_on) return;
    static std::map<uint64_t, unsigned long> m; static auto t0 = std::chrono::steady_clock::now(); static unsigned long tot = 0;
    ++tot; m[((uint64_t)site << 60) | ((uint64_t)tbp0 << 40) | ((uint64_t)psm << 32) | ((uint64_t)(w & 0xFFF) << 20) | ((uint64_t)(h & 0xFFF) << 8) | (uint64_t)(dest & 0xFF)]++;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - t0).count() >= 2.0)
    {
        std::vector<std::pair<uint64_t, unsigned long>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b){ return a.second > b.second; });
        std::fprintf(stderr, "[reqcensus] %lu barrier requests in 2 s, %zu classes:", tot, v.size());
        for (size_t i = 0; i < v.size() && i < 10; ++i)
            std::fprintf(stderr, "  s%llu tbp%llu/psm%llu %llux%llu->f%llu n=%lu", (unsigned long long)(v[i].first >> 60), (unsigned long long)((v[i].first >> 40) & 0xFFFFF),
                         (unsigned long long)((v[i].first >> 32) & 0xFF), (unsigned long long)((v[i].first >> 20) & 0xFFF), (unsigned long long)((v[i].first >> 8) & 0xFFF), (unsigned long long)(v[i].first & 0xFF), v[i].second);
        std::fprintf(stderr, "\n");
        m.clear(); tot = 0; t0 = now;
    }
}
// [rtreadskip] 2026-09-03: a page-aligned texture read of a page GL rendered (and the guest has not re-uploaded
// since) is served at replay from the FBO / view / twin / depth texture in every branch of the renderer's
// texture-selection chain (self-reads excluded) -- the record-time VRAM decode and the barrier request that
// staged the page were pure waste (~230 requests/s = 7 pages staged per frame, ~85 ms/s GL + the guest's
// staged-write application). PS2X_RTREADSKIP=0 restores the old behaviour.
static inline bool rtServedRead(uint32_t destFbp, const GSTex0Reg &tex)
{
    if ((tex.tbp0 % 32u) != 0u) return false;
    const uint32_t pg = tex.tbp0 / 32u;
    if (pg == destFbp) return false;   // feedback read: keep the old path
    return GsGpuRenderer::rtReadServedClass(pg, tex.psm, destFbp) && ps2GpuRenderer().pageRenderedNotUploaded(pg);
}
static inline bool p8twinServedRead(uint32_t destFbp, const GSTex0Reg &tex)
{
    static const bool s_on = [](){ const char *v = std::getenv("PS2X_P8TWINSKIPDEC"); return !(v && v[0] == '0'); }();
    static const bool s_tw = [](){ const char *v = std::getenv("PS2X_P8TWIN"); return v && v[0] && v[0] != '0'; }();
    if (!s_on || !s_tw) return false;
    if (tex.psm != GS_PSM_T8H || tex.tbp0 != 7168u) return false;
    if (destFbp == 224u) return false;
    return ps2GpuRenderer().fbpRenderedOnce(224u);
}

using namespace GSInternal;

// BT3 HUD debug: current scene-graph draw method (set by FUN_00218848), stamped onto HUD prims.
extern std::atomic<uint32_t> g_bt3DrawMethod;

namespace
{
    // ---- Scanline rasterization thread pool (PS2X_RASTER_THREADS) ----------
    // BT3's UI screens (popups, menus, title) are software-rasterized as large
    // full-screen alpha-blended sprites every frame. That fill work is the
    // systemic bottleneck (single-threaded ~2fps). Pixels within one primitive
    // are independent and write disjoint framebuffer addresses per scanline, so
    // we split a primitive's row range across worker threads. The per-pixel math
    // is UNCHANGED -> output is bit-identical to the serial path; only large
    // fills are parallelized (small sprites/glyphs run inline to avoid overhead).
    class RowRasterPool
    {
    public:
        static RowRasterPool &get()
        {
            static RowRasterPool inst;
            return inst;
        }

        int lanes() const { return m_lanes; }

        // Execute body(y) for every y in [y0, y1] inclusive, across lanes when
        // the range is large enough and the pool is idle; otherwise inline.
        void run(int y0, int y1, const std::function<void(int)> &body)
        {
            const int rows = y1 - y0 + 1;
            if (rows <= 0)
                return;
            static const bool s_prof = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_DMAPROF"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            static std::atomic<uint64_t> s_inlineN{0}, s_parN{0}, s_parNs{0}, s_inlineNs{0};
            auto reportPool = [&](){
                static std::mutex pm; static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lk(pm);
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - last).count();
                if (dt >= 1.0) {
                    std::cerr << "[poolprof] inline=" << (uint64_t)(s_inlineN.load()/dt) << "/s," << (s_inlineNs.load()/1e6/dt)
                              << "ms/s parallel=" << (uint64_t)(s_parN.load()/dt) << "/s," << (s_parNs.load()/1e6/dt) << "ms/s" << std::endl;
                    s_inlineN=0; s_parN=0; s_parNs=0; s_inlineNs=0; last=now;
                }
            };
            if (m_lanes <= 1 || rows < m_minRows)
            {
                const auto t0 = s_prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                for (int y = y0; y <= y1; ++y)
                    body(y);
                if (s_prof) { s_inlineN.fetch_add(1,std::memory_order_relaxed); s_inlineNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count(),std::memory_order_relaxed); reportPool(); }
                return;
            }
            const auto _rp0 = s_prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            bool expected = false;
            if (!m_busy.compare_exchange_strong(expected, true, std::memory_order_acquire))
            {
                // Pool already owned by another host thread -> run inline.
                for (int y = y0; y <= y1; ++y)
                    body(y);
                if (s_prof) { s_inlineN.fetch_add(1,std::memory_order_relaxed); reportPool(); }
                return;
            }

            const int lanes = m_lanes;
            const int workers = lanes - 1; // lane 0 runs on the calling thread
            const int base = rows / lanes;
            const int rem = rows % lanes;
            int starts[kMaxLanes + 1];
            {
                int cur = y0;
                for (int i = 0; i < lanes; ++i)
                {
                    starts[i] = cur;
                    cur += base + (i < rem ? 1 : 0);
                }
                starts[lanes] = cur; // == y1 + 1
            }
            {
                std::lock_guard<std::mutex> lk(m_mx);
                m_body = &body;
                for (int i = 0; i < workers; ++i)
                {
                    m_lo[i] = starts[i + 1];
                    m_hi[i] = starts[i + 2] - 1;
                }
                m_remaining.store(workers, std::memory_order_relaxed);
                ++m_seq;
            }
            m_wake.notify_all();

            for (int y = starts[0]; y <= starts[1] - 1; ++y)
                body(y);

            if (workers > 0)
            {
                std::unique_lock<std::mutex> lk(m_doneMx);
                m_done.wait(lk, [&] { return m_remaining.load(std::memory_order_acquire) == 0; });
            }
            m_busy.store(false, std::memory_order_release);
            if (s_prof) { s_parN.fetch_add(1,std::memory_order_relaxed); s_parNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-_rp0).count(),std::memory_order_relaxed); reportPool(); }
        }

    private:
        static constexpr int kMaxLanes = 32;

        RowRasterPool()
        {
            m_minRows = envInt("PS2X_RASTER_MINROWS", 32);
            unsigned hc = std::thread::hardware_concurrency();
            int defWorkers = (hc > 3u) ? static_cast<int>(std::min<unsigned>(hc - 2u, kMaxLanes - 1)) : 0;
            int workers = envInt("PS2X_RASTER_THREADS", defWorkers);
            if (workers < 0)
                workers = 0;
            if (workers > kMaxLanes - 1)
                workers = kMaxLanes - 1;
            m_lanes = workers + 1;
            for (int i = 0; i < workers; ++i)
                m_threads.emplace_back([this, i] { workerLoop(i); });
            if (workers > 0)
                std::cerr << "[raster] scanline pool: " << m_lanes << " lanes ("
                          << workers << " workers), minRows=" << m_minRows << std::endl;
        }

        ~RowRasterPool()
        {
            {
                std::lock_guard<std::mutex> lk(m_mx);
                m_stop = true;
                ++m_seq;
            }
            m_wake.notify_all();
            for (auto &t : m_threads)
                if (t.joinable())
                    t.join();
        }

        void workerLoop(int idx)
        {
            uint64_t localSeq = 0;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lk(m_mx);
                    m_wake.wait(lk, [&] { return m_seq != localSeq; });
                    localSeq = m_seq;
                    if (m_stop)
                        return;
                }
                const int lo = m_lo[idx];
                const int hi = m_hi[idx];
                const std::function<void(int)> *body = m_body;
                for (int y = lo; y <= hi; ++y)
                    (*body)(y);
                if (m_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    std::lock_guard<std::mutex> lk(m_doneMx);
                    m_done.notify_one();
                }
            }
        }

        static int envInt(const char *n, int def)
        {
            const char *v = std::getenv(n);
            if (!v || !v[0])
                return def;
            return static_cast<int>(std::strtol(v, nullptr, 10));
        }

        int m_lanes = 1;
        int m_minRows = 32;
        std::vector<std::thread> m_threads;
        std::mutex m_mx;
        std::condition_variable m_wake;
        std::mutex m_doneMx;
        std::condition_variable m_done;
        std::atomic<bool> m_busy{false};
        std::atomic<int> m_remaining{0};
        bool m_stop = false;
        uint64_t m_seq = 0;
        const std::function<void(int)> *m_body = nullptr;
        int m_lo[kMaxLanes];
        int m_hi[kMaxLanes];
    };

    inline void parallelRows(int y0, int y1, const std::function<void(int)> &body)
    {
        RowRasterPool::get().run(y0, y1, body);
    }

    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0)  & 0xFF) >> 3;
        uint32_t g = ((c >> 8)  & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0)  & 0x1F) << 3;
        u32 g = ((c >> 5)  & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
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

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};
    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct AlphaTestResult
    {
        bool writeFramebuffer;
        bool preserveDestinationAlpha;
    };

    AlphaTestResult classifyAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {true, false};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, false};
        case 3: // RGB_ONLY
            return {true, true};
        case 0: // KEEP
        case 2: // ZB_ONLY
        default:
            return {false, false};
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        // TEST (PS2X_FORCE_FONT): draw font-atlas glyphs bright green using the
        // texture's own coverage alpha, ignoring the game's vertex color/alpha.
        static const int s_forceFont = [](){ const char *v=[](){ static const char *s_env = std::getenv("PS2X_FORCE_FONT"); return s_env; }(); return (v&&v[0]&&v[0]!='0')?1:0; }();
        if (s_forceFont && tex.tbp0 == 10760u)
            return TextureCombineResult{0u, 255u, 0u, 128u}; // fully-opaque green

        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        return (index & 0xE7u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    // TODO: clut cache
    uint32_t resolveClutIndex(uint8_t index, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        uint32_t clutIndex = static_cast<uint32_t>(index);

        switch (sourcePsm)
        {
        case GS_PSM_T4:
        case GS_PSM_T4HH:
        case GS_PSM_T4HL:
        {
            clutIndex = (static_cast<uint32_t>(csa) << 4u) | (clutIndex & 0x0Fu);

            if (csm == 0u)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
        }
        break;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            if (csm == 0)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
            break;
        default:
            break;
        }

        return clutIndex;
    }

    bool tex1UsesLinearFilter(uint64_t tex1)
    {
        const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5) & 0x1u);
        const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6) & 0x7u);
        return mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

// Rasterizer workload counters (read by the host present loop for the FPS line).
std::atomic<uint64_t> g_rasterPrimCount{0};
std::atomic<uint64_t> g_rasterPixelCount{0};
std::atomic<uint64_t> g_rasterPrimNs{0}; // total wall-time inside drawPrimitive (PS2X_DMAPROF)
static const bool g_rasterTimeProf = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_DMAPROF"); return s_env; }(); return v && v[0] && v[0] != '0'; }();

void GSRasterizer::drawPrimitive(GS *gs)
{
    g_rasterPrimCount.fetch_add(1, std::memory_order_relaxed);
    const auto _dp0 = g_rasterTimeProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct DpTimer { const std::chrono::steady_clock::time_point &t0; ~DpTimer(){ if (g_rasterTimeProf) g_rasterPrimNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count(), std::memory_order_relaxed); } } _dpt{_dp0};
    const auto &ctx = gs->activeContext();

    // PS2X_POPUP: catch the random map-texture popup primitives in the act — ANY primitive
    // (sprite OR triangle, either mode) whose screen extent exceeds 800px, with frame number
    // for correlating against sightings. Legit big prims (sky, fullscreen filters) recur
    // every frame; popups are sporadic — the frame column separates them.
    {
        static const bool s_pp = [](){ const char *v = std::getenv("PS2X_POPUP"); return v && v[0] && v[0] != '0'; }();
        if (s_pp)
        {
            const int nv = (gs->m_prim.type == 6u) ? 2 : ((gs->m_prim.type >= 3u && gs->m_prim.type <= 5u) ? 3 : 0);
            if (nv)
            {
                float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
                for (int i = 0; i < nv; ++i)
                {
                    const GSVertex &v = gs->m_vtxQueue[i];
                    mnx = std::min(mnx, v.x); mxx = std::max(mxx, v.x);
                    mny = std::min(mny, v.y); mxy = std::max(mxy, v.y);
                }
                const float ext = std::max(mxx - mnx, mxy - mny);
                if (ext > 800.0f)
                {
                    extern std::atomic<uint64_t> g_bt3FrameCount;
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t n = s_n.fetch_add(1) + 1u;
                    // Visibility: does the prim's bbox intersect the scissor rect?
                    const bool vis = (mxx >= (float)ctx.scissor.x0 && mnx <= (float)ctx.scissor.x1 &&
                                      mxy >= (float)ctx.scissor.y0 && mny <= (float)ctx.scissor.y1);
                    if ((n <= 60 || (n % 512u) == 0u) || vis)
                        std::fprintf(stderr, "[popup] #%u frame=%llu prim=%u ext=%.0f tme=%d tbp0=%u psm=%u path=%d xy0=(%.0f,%.0f) xy1=(%.0f,%.0f) sc=[%d,%d..%d,%d]%s%s\n",
                                     n, (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed),
                                     gs->m_prim.type, ext, gs->m_prim.tme ? 1 : 0,
                                     ctx.tex0.tbp0, ctx.tex0.psm, (int)gs->m_curSrcPath,
                                     gs->m_vtxQueue[0].x, gs->m_vtxQueue[0].y,
                                     gs->m_vtxQueue[1].x, gs->m_vtxQueue[1].y,
                                     ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                                     nv == 3 ? "" : " SPRITE", vis ? " VISIBLE" : "");
                }
            }
        }
    }

    // [floorseq] (default on, PS2X_SRCDIAG=0 disables): OUR pipeline's floor event
    // sequence — prim-batch runs per active floor-TEX0 tw — for diffing against the
    // real-HW template extracted from the PCSX2 GS dump (memory: bt3-gpu-fight-pipeline).
    {
        static const bool s_fsq = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_SRCDIAG"); return s_env; }(); return !(v && v[0] == '0'); }();
        if (s_fsq)
        {
            static int s_lastTw = -1; static uint32_t s_run = 0; static uint32_t s_ev = 0;
            const bool isFloor = ctx.tex0.tbp0 == 10752u && ctx.tex0.psm == 19u &&
                                 ctx.tex0.cbp == 12992u && gs->m_prim.tme;
            const int tw = isFloor ? (int)ctx.tex0.tw : -1;
            if (tw != s_lastTw)
            {
                if (s_run && s_lastTw >= 0 && s_ev < 400)
                {
                    ++s_ev;
                    static FILE *f = std::fopen("/home/z3/Desktop/bt3/work/floorseq.txt", "w");
                    if (f) { std::fprintf(f, "[floorseq] tw=%d -> %u prims\n", s_lastTw, s_run); std::fflush(f); }
                }
                s_run = 0; s_lastTw = tw;
            }
            if (isFloor) ++s_run;

            // [floorvtx] diag v2: sample the FIRST prim of each floor tw-run across many
            // frames. If coords are frozen across frames while the fight camera moves, the
            // floor verts are pre-baked (misrouted around the VU1 transform); if they track
            // the camera, VU1 transforms them and the bug is missing clip/cull.
            if (isFloor && s_run == 1)
            {
                static FILE *fv = std::fopen("/home/z3/Desktop/bt3/work/floorvtx.txt", "w");
                static int s_n = 0;
                if (fv && s_n < 400)
                {
                    ++s_n;
                    const GSVertex *vq = gs->m_vtxQueue;
                    std::fprintf(fv, "[floorvtx] #%d tw=%d fst=%u v0=(%.1f,%.1f,%.0f q=%g st=%.3f,%.3f) v1=(%.1f,%.1f,%.0f) v2=(%.1f,%.1f,%.0f)\n",
                                 s_n, tw, (unsigned)gs->m_prim.fst,
                                 vq[0].x, vq[0].y, vq[0].z, vq[0].q, vq[0].s, vq[0].t,
                                 vq[1].x, vq[1].y, vq[1].z,
                                 vq[2].x, vq[2].y, vq[2].z);
                    std::fflush(fv);
                }
            }
        }
    }

    // PS2X_GRASSHACK: ground-base STQ draws (fst=0, tbp0=10752, whole triangle in the lower
    // screen) execute one upload EARLY and sample the sky-resident slot. Swap in the shadow
    // captured at mountains+grass-resident time for the duration of this draw — a LOOK-TEST
    // simulating the correct upload<->draw pairing (see FINAL MODEL in session memory).
    extern bool g_ps2xGrassHack;
    extern uint8_t g_ps2xGrassShadow[131072];
    extern std::atomic<bool> g_ps2xGrassShadowValid;
    struct GrassSwap
    {
        uint8_t *vramRegion = nullptr;
        GS *gsp = nullptr;
        uint8_t saved[131072];
        ~GrassSwap()
        {
            if (vramRegion)
            {
                std::memcpy(vramRegion, saved, sizeof(saved));
                ++gsp->m_texUploadGen; gsp->invalidateClutCache(); // [clutpagegen] sky content back — invalidate the grass decode too
            }
        }
    } _grassSwap;
    if (g_ps2xGrassHack && gs->m_prim.tme && !gs->m_prim.fst &&
        ctx.tex0.tbp0 == 10752u && g_ps2xGrassShadowValid.load(std::memory_order_acquire) &&
        gs->m_vram)
    {
        const int ofy = (int)(ctx.xyoffset.ofy >> 4);
        const float yMin = 250.0f + (float)ofy;
        if (gs->m_vtxQueue[0].y >= yMin && gs->m_vtxQueue[1].y >= yMin && gs->m_vtxQueue[2].y >= yMin)
        {
            uint8_t *region = gs->m_vram + 10752u * 256u;
            std::memcpy(_grassSwap.saved, region, sizeof(_grassSwap.saved));
            std::memcpy(region, g_ps2xGrassShadow, sizeof(_grassSwap.saved));
            _grassSwap.vramRegion = region;
            _grassSwap.gsp = gs;
            // The swap changes the texel content under the draw — invalidate the CLUT/tex
            // cache key so sampling doesn't reuse the sky decode.
            ++gs->m_texUploadGen; gs->invalidateClutCache();   // [clutpagegen]
        }
    }

    // Stage-0 GPU-renderer recon (env PS2X_GIFRECON): aggregate exactly which
    // primitive types / texture+framebuffer formats / blend+alpha+z modes BT3 issues,
    // so the GPU backend implements only what's actually used. Read-only.
    {
        static const bool s_recon = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GIFRECON"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_recon)
        {
            static std::mutex rm;
            static std::map<uint32_t, uint64_t> primType, texPsm, framePsm, zPsm, alphaMode, testMode;
            static std::map<uint32_t, uint64_t> frameFbp, texTbp0, tfxHist, tccHist, vcolHist;
            static std::set<uint32_t> seenFbp;
            static uint64_t total = 0, textured = 0, fstCnt = 0, abeCnt = 0, zwriteCnt = 0, rtt = 0;
            std::lock_guard<std::mutex> lk(rm);
            ++total;
            primType[gs->m_prim.type]++;
            framePsm[ctx.frame.psm]++;
            zPsm[ctx.zbuf.psm]++;
            frameFbp[ctx.frame.fbp]++;
            seenFbp.insert(ctx.frame.fbp);
            if (gs->m_prim.tme) { textured++; texPsm[ctx.tex0.psm]++; texTbp0[ctx.tex0.tbp0]++;
                tfxHist[ctx.tex0.tfx]++; tccHist[ctx.tex0.tcc]++;
                // vertex color bucket (max channel of v1) to see if the game sends dim colors
                { uint32_t mx = std::max({gs->m_vtxQueue[1].r, gs->m_vtxQueue[1].g, gs->m_vtxQueue[1].b}); vcolHist[mx & 0xF0]++; }
                // render-to-texture signal: sampling a texture whose base is a framebuffer we render to
                if (seenFbp.count(ctx.tex0.tbp0)) ++rtt; }
            if (gs->m_prim.fst) fstCnt++;
            if (gs->m_prim.abe) { abeCnt++; alphaMode[static_cast<uint32_t>(ctx.alpha & 0xFFu)]++; }
            if (!ctx.zbuf.zmask) zwriteCnt++;
            // pack: ATE | ATST<<1 | ZTE<<4 | ZTST<<5
            uint32_t tk = (ctx.test & 1u) | (((ctx.test >> 1) & 7u) << 1) | (((ctx.test >> 16) & 1u) << 4) | (((ctx.test >> 17) & 3u) << 5);
            testMode[tk]++;
            if ((total % 40000u) == 0u)
            {
                auto dump = [](const char *name, std::map<uint32_t, uint64_t> &m, uint64_t tot) {
                    std::cerr << "  " << name << ":";
                    for (auto &kv : m) std::cerr << " 0x" << std::hex << kv.first << std::dec << "=" << (kv.second * 100 / (tot ? tot : 1)) << "%";
                    std::cerr << std::endl;
                };
                std::cerr << "[gifrecon] total=" << total << " textured=" << (textured*100/total) << "% fst=" << (fstCnt*100/total)
                          << "% abe=" << (abeCnt*100/total) << "% zwrite=" << (zwriteCnt*100/total)
                          << "% RENDER-TO-TEXTURE=" << (textured ? rtt*100/textured : 0) << "% of textured"
                          << " (distinctFbp=" << frameFbp.size() << " distinctTbp0=" << texTbp0.size() << ")" << std::endl;
                dump("tfx(0=MOD,1=DECAL,2=HL,3=HL2)", tfxHist, textured);
                dump("tcc(0=noTexAlpha,1=texAlpha)", tccHist, textured);
                dump("alphaMode(A|B<<2|C<<4|D<<6; C: 0=As 1=Ad 2=FIX)", alphaMode, abeCnt);
                dump("testMode(ATE|ATST<<1|ZTE<<4|ZTST<<5)", testMode, total);
                dump("vtxMaxChan&0xF0", vcolHist, textured);
                dump("frameFbp", frameFbp, total);
                dump("texTbp0", texTbp0, textured);
            }
        }
    }
    // PS2X_SWTEXDUMP: software-mode texture extractor. For each distinct (tbp0,psm) seen on a
    // textured draw, decode the full texture via the authoritative sampleTexture() path (handles
    // any PSM incl. CLUT) and write it as a BMP under /home/z3/Desktop/bt3/texdump/. Lets us eyeball
    // whether textures are real image data or coming out uniformly blue/empty.
    {
        static const bool s_swtd = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_SWTEXDUMP"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_swtd && gs->m_prim.tme)
        {
            static std::mutex swm;
            static std::set<uint32_t> swSeen;
            const auto &t = ctx.tex0;
            const uint32_t key = (t.tbp0 << 8) | (static_cast<uint32_t>(t.psm) & 0xFFu);
            std::lock_guard<std::mutex> lk(swm);
            const int W = 1 << t.tw, H = 1 << t.th;
            if (swSeen.size() < 96u && !swSeen.count(key) && W >= 1 && H >= 1 && W <= 1024 && H <= 1024)
            {
                swSeen.insert(key);
                const uint32_t savedFst = gs->m_prim.fst;
                gs->m_prim.fst = 1u; // force FST so sampleTexture uses the (u,v) texel path
                char path[256];
                std::snprintf(path, sizeof(path),
                              "/home/z3/Desktop/bt3/texdump/tex_tbp%05u_psm%02x_%dx%d.bmp",
                              t.tbp0, static_cast<uint32_t>(t.psm) & 0xFFu, W, H);
                FILE *bf = std::fopen(path, "wb");
                if (bf)
                {
                    const uint32_t rowBytes = (static_cast<uint32_t>(W) * 3u + 3u) & ~3u;
                    const uint32_t imgSize = rowBytes * static_cast<uint32_t>(H);
                    uint8_t fh[14] = {'B','M'}; uint32_t fsize = 54u + imgSize; uint32_t off = 54u;
                    std::memcpy(fh + 2, &fsize, 4); std::memcpy(fh + 10, &off, 4);
                    std::fwrite(fh, 1, 14, bf);
                    uint8_t ih[40] = {40,0,0,0}; int32_t w = W, h = H; uint16_t planes = 1, bpp = 24;
                    std::memcpy(ih + 4, &w, 4); std::memcpy(ih + 8, &h, 4);
                    std::memcpy(ih + 12, &planes, 2); std::memcpy(ih + 14, &bpp, 2);
                    std::fwrite(ih, 1, 40, bf);
                    std::vector<uint8_t> row(rowBytes, 0);
                    for (int y = H - 1; y >= 0; --y)
                    {
                        for (int x = 0; x < W; ++x)
                        {
                            uint32_t texel = sampleTexture(gs, 0.f, 0.f, 1.f,
                                                           static_cast<uint16_t>(x * 16 + 8),
                                                           static_cast<uint16_t>(y * 16 + 8));
                            row[x * 3 + 0] = (texel >> 16) & 0xFF; // B
                            row[x * 3 + 1] = (texel >> 8) & 0xFF;  // G
                            row[x * 3 + 2] = texel & 0xFF;         // R
                        }
                        std::fwrite(row.data(), 1, rowBytes, bf);
                    }
                    std::fclose(bf);
                    std::fprintf(stderr, "[swtexdump] %s tbw=%u tfx=%u tcc=%u\n",
                                 path, t.tbw, static_cast<uint32_t>(t.tfx), static_cast<uint32_t>(t.tcc));
                }
                gs->m_prim.fst = savedFst;
            }
        }
    }

    // PS2X_SKIPBLUE: A/B test — skip the fullscreen tbp12288 overlay quad so we can see what's
    // behind it (is the scene fine and just buried, or genuinely collapsed to black?).
    {
        static const bool s_sb = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_SKIPBLUE"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_sb && gs->m_prim.tme && ctx.tex0.tbp0 == 12288u)
            return;
    }

    // PS2X_CLUTPROBE[=tbp0] (default 10760 = the invisible GROUND band, avgA=11 vs the
    // game's palette alpha 127): print the draw's full TEX0/CLUT state, the 16 decoded
    // CLUT entries our sampler will actually use, and the vertex alpha — names whether
    // the alpha collapse is palette ADDRESSING (cbp/csa/cpsm) or entry DECODE.
    {
        static const uint32_t s_cp = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_CLUTPROBE"); return s_env; }();
            if (!v || !v[0] || v[0] == '0') return 0u;
            const uint32_t t = (uint32_t)std::strtoul(v, nullptr, 0);
            return t <= 1u ? 10760u : t; }();
        if (s_cp && gs->m_prim.tme && ctx.tex0.tbp0 == s_cp)
        {
            static std::atomic<uint32_t> s_cn{0};
            const uint32_t n = s_cn.fetch_add(1);
            if (n < 6u || (n % 40000u) < 2u)
            {
                ensureClutCache(gs);
                const auto &t = ctx.tex0;
                std::fprintf(stderr, "[clutprobe] #%u tbp0=%u tbw=%u psm=%u %ux%u | cbp=%u cpsm=%u csm=%u csa=%u tcc=%u tfx=%u"
                                     " | prim fst=%u abe=%u ctxt=%u vtxA=%u | alphaReg=%016llx texa=%016llx\n",
                             n, t.tbp0, t.tbw, t.psm, 1 << t.tw, 1 << t.th,
                             t.cbp, t.cpsm, t.csm, t.csa, t.tcc, t.tfx,
                             gs->m_prim.fst ? 1u : 0u, gs->m_prim.abe ? 1u : 0u, gs->m_prim.ctxt ? 1u : 0u,
                             (uint32_t)gs->m_vtxQueue[0].a, (unsigned long long)ctx.alpha,
                             (unsigned long long)((uint64_t)gs->m_texa.ta0 | ((uint64_t)gs->m_texa.aem << 15) | ((uint64_t)gs->m_texa.ta1 << 32)));
                std::fprintf(stderr, "[clutprobe]   clut16(RGBA):");
                for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %08x", gs->m_clutCache[i]);
                std::fprintf(stderr, "\n[clutprobe]   clut16 alphas:");
                for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %u", (gs->m_clutCache[i] >> 24) & 0xFFu);
                std::fprintf(stderr, "\n");
                // Raw-packet dump (fst-agnostic, unlike PS2X_KICKRAW): the watched draws are
                // VU1-built XGKICK packets with vertex alpha 0 — dump the exact GIF bytes to
                // see whether the VU1 EMITS a=0 (RGBAQ fields in the packet) or our parse
                // loses the alpha en route.
                static std::atomic<uint32_t> s_pk{0};
                if (gs->m_curPktData && gs->m_curPktSize && s_pk.load() < 3u)
                {
                    const uint32_t pn = s_pk.fetch_add(1);
                    if (pn < 3u)
                    {
                        char pb[160];
                        std::snprintf(pb, sizeof(pb), "/home/z3/Desktop/bt3/work/clutpkt_%u.bin", pn);
                        if (FILE *f = std::fopen(pb, "wb"))
                        { std::fwrite(gs->m_curPktData, 1, gs->m_curPktSize, f); std::fclose(f); }
                        std::fprintf(stderr, "[clutprobe] dumped raw packet #%u size=%u srcPath=%u -> %s\n",
                                     pn, gs->m_curPktSize, gs->m_curSrcPath, pb);
                        std::fprintf(stderr, "[clutprobe]   vtx0 rgba=(%u,%u,%u,%u) vtx1 a=%u vtx2 a=%u curA=%u\n",
                                     gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a,
                                     gs->m_vtxQueue[1].a, gs->m_vtxQueue[2].a, gs->m_curA);
                    }
                }
            }
        }
    }

    // PS2X_BIGDRAW: identify big screen-covering draws (the "blue"). For each large-bbox primitive,
    // report textured?/color/texture — so we can tell if the blue is a flat untextured fill (clear/
    // backdrop) or a textured quad decoding blue.
    {
        static const bool s_bf = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_BIGDRAW"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_bf)
        {
            const int ofx = ctx.xyoffset.ofx >> 4, ofy = ctx.xyoffset.ofy >> 4;
            const GSVertex &a = gs->m_vtxQueue[0], &b = gs->m_vtxQueue[1], &c = gs->m_vtxQueue[2];
            const int xa=(int)a.x-ofx, ya=(int)a.y-ofy, xb=(int)b.x-ofx, yb=(int)b.y-ofy, xc=(int)c.x-ofx, yc=(int)c.y-ofy;
            const int minx=std::min({xa,xb,xc}), maxx=std::max({xa,xb,xc});
            const int miny=std::min({ya,yb,yc}), maxy=std::max({ya,yb,yc});
            const int w=maxx-minx, h=maxy-miny;
            if (w >= 200 && h >= 150)
            {
                static std::atomic<int> bc{0};
                if (bc.fetch_add(1) < 50)
                    std::fprintf(stderr, "[bigdraw] prim=%u ctxt=%u tme=%u tbp0=%u psm=%u fbp=%u col0=(%u,%u,%u,a%u) bbox=(%d,%d)-(%d,%d) %dx%d "
                                         "of1=(%u,%u) of2=(%u,%u) sc1=(%u..%u,%u..%u) sc2=(%u..%u,%u..%u)\n",
                        gs->m_prim.type, gs->m_prim.ctxt ? 1u : 0u, gs->m_prim.tme, ctx.tex0.tbp0, ctx.tex0.psm, ctx.frame.fbp,
                        a.r, a.g, a.b, a.a, minx, miny, maxx, maxy, w, h,
                        (unsigned)(gs->m_ctx[0].xyoffset.ofx >> 4), (unsigned)(gs->m_ctx[0].xyoffset.ofy >> 4),
                        (unsigned)(gs->m_ctx[1].xyoffset.ofx >> 4), (unsigned)(gs->m_ctx[1].xyoffset.ofy >> 4),
                        (unsigned)gs->m_ctx[0].scissor.x0, (unsigned)gs->m_ctx[0].scissor.x1,
                        (unsigned)gs->m_ctx[0].scissor.y0, (unsigned)gs->m_ctx[0].scissor.y1,
                        (unsigned)gs->m_ctx[1].scissor.x0, (unsigned)gs->m_ctx[1].scissor.x1,
                        (unsigned)gs->m_ctx[1].scissor.y0, (unsigned)gs->m_ctx[1].scissor.y1);
            }
        }
    }

    // [logo2] Dual-mode (software AND GPU) log of 128x256-textured draws, to compare the
    // exact primitives the game issues in each mode (does GPU-mode feedback change them?).
    {
        static const bool s_l2 = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_l2 && gs->m_prim.tme)
        {
            const int tW = 1 << ctx.tex0.tw, tH = 1 << ctx.tex0.th;
            if (tW == 128 && tH == 256)
            {
                static std::atomic<uint32_t> s_l2c{0};
                uint32_t n = s_l2c.fetch_add(1) + 1u;
                if (n <= 24)
                {
                    const int ofx = ctx.xyoffset.ofx >> 4, ofy = ctx.xyoffset.ofy >> 4;
                    const GSVertex &q0 = gs->m_vtxQueue[0], &q1 = gs->m_vtxQueue[1];
                    // texel UV (fst) so we can see if tiles sample increasing regions.
                    const float u0 = (q0.u >> 4) / 16.0f, v0 = (q0.v >> 4) / 16.0f;
                    const float u1 = (q1.u >> 4) / 16.0f, v1 = (q1.v >> 4) / 16.0f;
                    std::fprintf(stderr, "[logo2] %s #%u prim=%u fst=%u tbp0=%u tbw=%u wrap=%u destFbp=%u xy=(%d,%d)-(%d,%d) uvTexel=(%.1f,%.1f)-(%.1f,%.1f)\n",
                                 GsGpuRenderer::enabled() ? "GPU" : "SW", n, gs->m_prim.type, gs->m_prim.fst,
                                 ctx.tex0.tbp0, ctx.tex0.tbw, static_cast<uint32_t>(ctx.clamp & 0xFu),
                                 ctx.frame.fbp,
                                 static_cast<int>(q0.x) - ofx, static_cast<int>(q0.y) - ofy,
                                 static_cast<int>(q1.x) - ofx, static_cast<int>(q1.y) - ofy,
                                 u0, v0, u1, v1);
                }
            }
        }
    }

    // PS2X_SPREAD: gradient signal, placed BEFORE the renderer split so it works in BOTH software and
    // GPU mode (geometry coords are renderer-independent — VU1 computes them upstream). Accumulates the
    // screen bbox of 3D scene triangles; spanX/spanY grows from ~50px (collapse) toward ~512x448 as the
    // MVP is fixed. (For SPREAD, the XYOFFSET cancels, so raw px = (int)vtx/16 is fine.)
    {
        static const bool s_sp = [](){ static const char *s_env = std::getenv("PS2X_SPREAD"); return s_env; }() != nullptr;
        // PS2X_TEX3D lives inside this block: let it open the block on its own so it does
        // not silently require PS2X_SPREAD as well.
        static const bool s_t3gate = [](){ static const char *s_env = std::getenv("PS2X_TEX3D"); return s_env; }() != nullptr;
        if ((s_sp || s_t3gate) && ctx.tex0.tw >= 5u && gs->m_prim.tme &&
            (gs->m_prim.type == GS_PRIM_TRIANGLE || gs->m_prim.type == GS_PRIM_TRISTRIP || gs->m_prim.type == GS_PRIM_TRIFAN))
        {
            // PER-TRIANGLE size: does each individual triangle have real screen area, or is it
            // collapsed to a dot? This is the clean degeneracy signal (unconfounded by object
            // positions). Track the biggest per-triangle bbox seen, and the % of non-degenerate
            // triangles (max edge > 2px). Degenerate MVP => nearly all triangles are dots.
            static std::mutex spm;
            static float maxTri=0.0f; static uint64_t cnt=0, nonDegen=0;
            std::lock_guard<std::mutex> lk(spm);
            // NOTE: m_vtxQueue coords are ALREADY pixels (decode handlers divide by 16). The old
            // >>4 here double-divided, misclassifying every real tri under 32px as a "dot".
            const float x0=gs->m_vtxQueue[0].x, y0=gs->m_vtxQueue[0].y;
            const float x1=gs->m_vtxQueue[1].x, y1=gs->m_vtxQueue[1].y;
            const float x2=gs->m_vtxQueue[2].x, y2=gs->m_vtxQueue[2].y;
            const float triW = std::max({x0,x1,x2}) - std::min({x0,x1,x2});
            const float triH = std::max({y0,y1,y2}) - std::min({y0,y1,y2});
            const float triSz = std::max(triW, triH);
            if (triSz > maxTri) maxTri = triSz;
            if (triSz > 2.0f) {
                ++nonDegen;
                static uint64_t rl = 0;
                if ((rl++ % 30000u) < 4u)
                    std::fprintf(stderr, "[real] src=%u at=(%.0f,%.0f) sz=%.0fpx prim=%u fbp=%u tbp0=%u\n",
                                 gs->m_curSrcPath, x0, y0, triSz, gs->m_prim.type, ctx.frame.fbp, ctx.tex0.tbp0);
            }
            // PS2X_TEX3D: full texture state of 3D (fst=0) draws + VRAM content sample under
            // tbp0/cbp. Textures upload fine but 3D samples BLACK — is it CLUT (psm/cbp) or
            // texel data addressing?
            if (!gs->m_prim.fst) {
                static uint64_t t3 = 0;
                if ((t3++ % 40000u) < 3u && [](){ static const char *s_env = std::getenv("PS2X_TEX3D"); return s_env; }()) {
                    const uint8_t *v1 = gs->m_vram + ((uint64_t)ctx.tex0.tbp0 * 256u) % gs->m_vramSize;
                    const uint8_t *v2 = gs->m_vram + ((uint64_t)ctx.tex0.cbp * 256u) % gs->m_vramSize;
                    uint32_t nz1 = 0, nz2 = 0;
                    for (int b = 0; b < 256; ++b) { if (v1[b]) ++nz1; if (v2[b]) ++nz2; }
                    // Sample the texture CENTER through the authoritative sampler — what color
                    // does the rasterizer actually get for this draw's texture?
                    const uint32_t cSample = sampleTexture(gs, 0.5f, 0.5f, 1.0f, 0, 0);
                    std::fprintf(stderr, "[tex3d] tbp0=%u psm=%u tw=%u th=%u cbp=%u cpsm=%u csm=%u tfx=%u tcc=%u | texNZ=%u/256 clutNZ=%u/256 q=%.3f st=(%.3f,%.3f) centerRGBA=%08x\n",
                                 ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tw, ctx.tex0.th,
                                 ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.tfx, ctx.tex0.tcc,
                                 nz1, nz2, gs->m_vtxQueue[0].q, gs->m_vtxQueue[0].s, gs->m_vtxQueue[0].t, cSample);
                }
                // The MAP's draws (the 1024x256 terrain atlas at tbp0=10752) arrive with ZERO
                // st/q while char draws are healthy -> log the map family's per-vertex raw
                // s,t,q + prim + source path (1=XGKICK/VU1-computed, 2=DIRECT, 3=path3 DMA).
                // Splits "VIF unpack of the terrain ST format is broken" (path3/DIRECT data
                // already zero) from "VU1 microprogram texcoord path broken" (XGKICK).
                // [mapmat] census: every DISTINCT texture state (tbp0/cbp/psm + CLAMP fields)
                // carried by 3D STQ triangles. Answers whether the fight map really uses ONE
                // atlas (backdrop smeared everywhere = UV/region bug) or many materials that
                // our pipeline funnels into one (texture-switching bug, e.g. TEX2-only CLUT
                // swaps between packets).
                if ([](){ static const char *s_env = std::getenv("PS2X_TEX3D"); return s_env; }()) {
                    static std::mutex s_mm;
                    static std::set<uint64_t> s_seen;
                    const uint32_t wms = (uint32_t)(ctx.clamp & 3u), wmt = (uint32_t)((ctx.clamp >> 2) & 3u);
                    const uint64_t sig = ((uint64_t)ctx.tex0.tbp0 << 40) ^ ((uint64_t)ctx.tex0.cbp << 16) ^
                                         ((uint64_t)ctx.tex0.psm << 8) ^ (uint64_t)(ctx.clamp & 0xFFFFFFFFFFFull);
                    std::lock_guard<std::mutex> lk(s_mm);
                    if (s_seen.size() < 64 && s_seen.insert(sig).second)
                        std::fprintf(stderr, "[mapmat] tbp0=%u tbw=%u psm=%u %ux%u cbp=%u | wms=%u wmt=%u minU=%u maxU=%u minV=%u maxV=%u | destFbp=%u\n",
                                     ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 1u << ctx.tex0.tw, 1u << ctx.tex0.th, ctx.tex0.cbp,
                                     wms, wmt, (uint32_t)((ctx.clamp >> 4) & 0x3FFu), (uint32_t)((ctx.clamp >> 14) & 0x3FFu),
                                     (uint32_t)((ctx.clamp >> 24) & 0x3FFu), (uint32_t)((ctx.clamp >> 34) & 0x3FFu), ctx.frame.fbp);
                }
                // [maphash]: at every map draw (tbp0=10752), hash the LIVE VRAM under the
                // texels (2KB @10752) and the palette (1KB @cbp). If the game streams
                // materials/palettes through this slot, the hashes MUST cycle between draws;
                // a constant hash means the content never changes at draw time (streaming
                // absent or arriving elsewhere) and the multi-material look must come from
                // UV cells / CLUT selection instead.
                if ([](){ static const char *s_env = std::getenv("PS2X_TEX3D"); return s_env; }() && (ctx.tex0.tw >= 6u) && (ctx.frame.fbp == 0u || ctx.frame.fbp == 112u)) {
                    extern std::atomic<bool> g_ps2xMapDrawSeen;
                    g_ps2xMapDrawSeen.store(true, std::memory_order_relaxed);
                    static std::mutex s_hm;
                    static std::map<uint64_t, uint32_t> s_texH, s_palH;
                    static uint64_t s_hn = 0;
                    uint64_t th = 1469598103934665603ull, ph = th;
                    th = (th ^ ctx.tex0.tbp0) * 1099511628211ull; // separate per texture base
                    const uint8_t *tp = gs->m_vram + ((uint64_t)ctx.tex0.tbp0 * 256u) % gs->m_vramSize;
                    const uint8_t *pp2 = gs->m_vram + ((uint64_t)ctx.tex0.cbp * 256u) % gs->m_vramSize;
                    for (int b = 0; b < 2048; b += 8) { th = (th ^ tp[b]) * 1099511628211ull; }
                    for (int b = 0; b < 1024; b += 4) { ph = (ph ^ pp2[b]) * 1099511628211ull; }
                    th = (th ^ ph) * 1099511628211ull; // palette swaps count as distinct looks
                    // [mapseq] (PS2X_MAPSEQ): print every CONTENT TRANSITION seen by draws per
                    // tbp0. Interleaved with [cwatch] BIG upload lines in stderr order, this
                    // shows the upload<->draw phase: which resident content each draw batch
                    // actually pairs with (the ground draws pair with SKY = one-slot streaming
                    // mismatch).
                    {
                        static const bool s_mseq = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_MAPSEQ"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                        if (s_mseq)
                        {
                            static std::mutex s_qm;
                            static std::map<uint32_t, uint64_t> s_last;
                            static uint32_t s_qn = 0;
                            std::lock_guard<std::mutex> lk(s_qm);
                            uint64_t &lastH = s_last[ctx.tex0.tbp0];
                            // 15680 (cel band LUT) churns per character pair and ate 3774 of
                            // the 4000-line budget last run — exclude it.
                            if (lastH != th && s_qn < 4000u && ctx.tex0.tbp0 != 15680u)
                            {
                                ++s_qn;
                                lastH = th;
                                std::fprintf(stderr, "[mapseq] DRAW tbp0=%u sees content=%08x (%ux%u psm=%u)\n",
                                             ctx.tex0.tbp0, (uint32_t)(th >> 32) ^ (uint32_t)th,
                                             1u << ctx.tex0.tw, 1u << ctx.tex0.th, ctx.tex0.psm);
                            }
                        }
                    }
                    bool newContent = false;
                    size_t contentIdx = 0;
                    {
                        std::lock_guard<std::mutex> lk(s_hm);
                        newContent = (s_texH.find(th) == s_texH.end());
                        s_texH[th]++; s_palH[ph]++;
                        contentIdx = s_texH.size();
                        if ((++s_hn % 4000u) == 1u) {
                            std::fprintf(stderr, "[maphash] draws=%llu distinctTexContent=%zu distinctPalContent=%zu (cbp=%u)\n",
                                         (unsigned long long)s_hn, s_texH.size(), s_palH.size(), ctx.tex0.cbp);
                        }
                    }
                    // Dump each DISTINCT content the map draws actually sample, as seen at
                    // draw time (live VRAM through the live palette) -> work/mapdraw_N.ppm.
                    // Shows exactly WHICH texture the map is trying to use.
                    if (newContent && contentIdx <= 40) {
                        const int W = 1 << ctx.tex0.tw, H = 1 << ctx.tex0.th;
                        const uint32_t savedFst = gs->m_prim.fst;
                        gs->m_prim.fst = 1u;
                        ensureClutCache(gs); // decode through THIS draw's palette, not the previous one's
                        char path[128];
                        std::snprintf(path, sizeof(path), "/home/z3/Desktop/bt3/work/mapdraw_%zu_tbp%u.ppm", contentIdx, ctx.tex0.tbp0);
                        if (FILE *fp = std::fopen(path, "wb")) {
                            std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
                            for (int ty = 0; ty < H; ++ty)
                                for (int tx = 0; tx < W; ++tx) {
                                    const uint32_t texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f,
                                                                         (uint16_t)(tx * 16 + 8), (uint16_t)(ty * 16 + 8));
                                    const uint8_t rgb[3] = { (uint8_t)(texel & 0xFF), (uint8_t)((texel >> 8) & 0xFF), (uint8_t)((texel >> 16) & 0xFF) };
                                    std::fwrite(rgb, 1, 3, fp);
                                }
                            std::fclose(fp);
                            std::fprintf(stderr, "[maphash] NEW content #%zu -> %s (cbp=%u)\n", contentIdx, path, ctx.tex0.cbp);
                            // The 16 palette entries THIS draw decodes through (T4), plus the
                            // CLUT addressing state — flat output with structured indices means
                            // these entries are wrong/uniform; compare against what the material
                            // should look like and against csa/texclut addressing.
                            if (ctx.tex0.psm == GS_PSM_T4 && gs->m_clutCacheKey != ~0ull)
                            {
                                std::fprintf(stderr, "[maphash]   csa=%u csm=%u cpsm=%u texclut=%u,%u,%u clut16:",
                                             ctx.tex0.csa, ctx.tex0.csm, ctx.tex0.cpsm,
                                             (uint32_t)gs->m_texclut.cbw, (uint32_t)gs->m_texclut.cou, (uint32_t)gs->m_texclut.cov);
                                for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %08x", gs->m_clutCache[i]);
                                std::fprintf(stderr, "\n");
                            }
                        }
                        // For T4 textures also dump the RAW INDEX nibbles as grayscale:
                        // structure visible => indices fine, palette lookup is the bug;
                        // uniform gray => the T4 swizzle/alias read itself is broken.
                        if (ctx.tex0.psm == GS_PSM_T4 && gs->m_vram)
                        {
                            std::snprintf(path, sizeof(path), "/home/z3/Desktop/bt3/work/mapdraw_%zu_tbp%u_idx.ppm", contentIdx, ctx.tex0.tbp0);
                            if (FILE *fp = std::fopen(path, "wb")) {
                                std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
                                const uint32_t vmask = gs->m_vramSize ? (gs->m_vramSize - 1u) : 0x3FFFFFu;
                                for (int ty = 0; ty < H; ++ty)
                                    for (int tx = 0; tx < W; ++tx) {
                                        const uint32_t na = GSPSMT4::addrPSMT4(ctx.tex0.tbp0, ctx.tex0.tbw, (uint32_t)tx, (uint32_t)ty);
                                        const uint8_t bval = gs->m_vram[(na >> 1) & vmask];
                                        const uint8_t idx = ((na & 1u) ? (bval >> 4) : (bval & 0x0Fu)) & 0x0Fu;
                                        const uint8_t g = (uint8_t)(idx * 17u);
                                        const uint8_t rgb[3] = { g, g, g };
                                        std::fwrite(rgb, 1, 3, fp);
                                    }
                                std::fclose(fp);
                            }
                        }
                        gs->m_prim.fst = savedFst;
                    }
                }
                // [groundtest]: the ground mesh (tbp 11008 detail tile) is SUBMITTED but zero
                // pixels survive to sampling. Log its full rejection-relevant state: TEST reg
                // (ATE/ATST/AREF + ZTE/ZTST), vertex xyz (z vs what the sky wrote), scissor.
                if ([](){ static const char *s_env = std::getenv("PS2X_TEX3D"); return s_env; }() && ctx.tex0.tbp0 == 11008u) {
                    static uint64_t g3 = 0;
                    if ((g3++ % 2000u) < 4u) {
                        const uint64_t tst = ctx.test;
                        const GSVertex &a = gs->m_vtxQueue[0], &b = gs->m_vtxQueue[1], &c2 = gs->m_vtxQueue[2];
                        // NOTE: z is a DOUBLE — must be cast for %u. The uncast version
                        // shifted every later vararg and fabricated the infamous
                        // "z=(0,0,0) + garbage ctx2 scissor/zbp" red herring.
                        std::fprintf(stderr, "[groundtest] prim=%u abe=%u | ATE=%u ATST=%u AREF=%u ZTE=%u ZTST=%u | z=(%u,%u,%u) xy0=(%.0f,%.0f) xy1=(%.0f,%.0f) xy2=(%.0f,%.0f) | scissor=(%d,%d)-(%d,%d) fbp=%u zbp=%u zmsk=%u\n",
                                     gs->m_prim.type, gs->m_prim.abe ? 1u : 0u,
                                     (uint32_t)(tst & 1u), (uint32_t)((tst >> 1) & 7u), (uint32_t)((tst >> 4) & 0xFFu),
                                     (uint32_t)((tst >> 16) & 1u), (uint32_t)((tst >> 17) & 3u),
                                     (uint32_t)a.z, (uint32_t)b.z, (uint32_t)c2.z, a.x, a.y, b.x, b.y, c2.x, c2.y,
                                     ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                                     ctx.frame.fbp, ctx.zbuf.zbp, (uint32_t)ctx.zbuf.zmask);
                    }
                }
                // PS2X_KICKRAW[=tbp0]: dump the ENTIRE raw GIF packet (the exact bytes our
                // parser is walking, stashed by processGIFPacket) the first few times a draw
                // sampling that tbp0 (default 11008 = ground detail tile) is kicked from it,
                // plus a parse-state snapshot. Hand-parse the .bin against the GS spec to find
                // where the GIF walk mis-steps (ground mesh z=0 / garbage-ctx2 bug).
                {
                    static const uint32_t s_kr = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_KICKRAW"); return s_env; }();
                        if (!v || !v[0] || v[0] == '0') return 0u;
                        const uint32_t t = (uint32_t)std::strtoul(v, nullptr, 0);
                        return t <= 1u ? 11008u : t; }();
                    // PS2X_KICKRAW_YMIN=<py>: only capture packets whose draw sits fully BELOW
                    // this screen row (after XYOFFSET). Targets ground-region draws when the
                    // watched tbp0 (e.g. the 10752 atlas) is also used by the sky.
                    static const int s_krYmin = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_KICKRAW_YMIN"); return s_env; }();
                        return v ? (int)std::strtol(v, nullptr, 0) : 0; }();
                    const int krOfy = (int)(ctx.xyoffset.ofy >> 4);
                    const bool krYok = s_krYmin <= 0 ||
                        (gs->m_vtxQueue[0].y - krOfy >= s_krYmin &&
                         gs->m_vtxQueue[1].y - krOfy >= s_krYmin &&
                         gs->m_vtxQueue[2].y - krOfy >= s_krYmin);
                    if (s_kr && ctx.tex0.tbp0 == s_kr && krYok && gs->m_curPktData && gs->m_curPktSize)
                    {
                        static std::mutex s_km;
                        static std::set<uint64_t> s_krHashes;
                        static uint32_t s_kn = 0;
                        std::lock_guard<std::mutex> lk(s_km);
                        uint64_t h = 1469598103934665603ull;
                        for (uint32_t i = 0; i < gs->m_curPktSize; ++i)
                            h = (h ^ gs->m_curPktData[i]) * 1099511628211ull;
                        if (s_kn < 6u && s_krHashes.insert(h).second)
                        {
                            char pb[160];
                            std::snprintf(pb, sizeof(pb), "/home/z3/Desktop/bt3/work/kickraw_%u.bin", s_kn);
                            if (FILE *f = std::fopen(pb, "wb"))
                            { std::fwrite(gs->m_curPktData, 1, gs->m_curPktSize, f); std::fclose(f); }
                            std::snprintf(pb, sizeof(pb), "/home/z3/Desktop/bt3/work/kickraw_%u.txt", s_kn);
                            if (FILE *f = std::fopen(pb, "w"))
                            {
                                std::fprintf(f, "packet: size=%u srcPath=%u hash=%016llx\n",
                                             gs->m_curPktSize, gs->m_curSrcPath, (unsigned long long)h);
                                std::fprintf(f, "prim: type=%u iip=%u tme=%u fge=%u abe=%u aa1=%u fst=%u ctxt=%u fix=%u\n",
                                             (uint32_t)gs->m_prim.type, gs->m_prim.iip, gs->m_prim.tme, gs->m_prim.fge,
                                             gs->m_prim.abe, gs->m_prim.aa1, gs->m_prim.fst, gs->m_prim.ctxt, gs->m_prim.fix);
                                std::fprintf(f, "cur: q=%f s=%f t=%f rgba=(%u,%u,%u,%u)\n",
                                             gs->m_curQ, gs->m_curS, gs->m_curT,
                                             gs->m_curR, gs->m_curG, gs->m_curB, gs->m_curA);
                                for (int ci = 0; ci < 2; ++ci)
                                {
                                    const GSContext &cc = gs->m_ctx[ci];
                                    std::fprintf(f, "ctx%d: frame(fbp=%u fbw=%u psm=%u fbmsk=%08x) zbuf(zbp=%u psm? zmsk=%u)"
                                                    " scissor=(%d,%d)-(%d,%d) xyoff=(%u,%u) tex0(tbp0=%u tbw=%u psm=%u tw=%u th=%u"
                                                    " tcc=%u tfx=%u cbp=%u cpsm=%u csm=%u csa=%u) test=%016llx clamp=%016llx alpha=%016llx\n",
                                                 ci, cc.frame.fbp, cc.frame.fbw, (uint32_t)cc.frame.psm, cc.frame.fbmsk,
                                                 cc.zbuf.zbp, (uint32_t)cc.zbuf.zmask,
                                                 cc.scissor.x0, cc.scissor.y0, cc.scissor.x1, cc.scissor.y1,
                                                 (uint32_t)cc.xyoffset.ofx, (uint32_t)cc.xyoffset.ofy,
                                                 cc.tex0.tbp0, cc.tex0.tbw, cc.tex0.psm, cc.tex0.tw, cc.tex0.th,
                                                 cc.tex0.tcc, cc.tex0.tfx, cc.tex0.cbp, cc.tex0.cpsm, cc.tex0.csm, cc.tex0.csa,
                                                 (unsigned long long)cc.test, (unsigned long long)cc.clamp, (unsigned long long)cc.alpha);
                                }
                                for (int vi = 0; vi < 3; ++vi)
                                {
                                    const GSVertex &vv = gs->m_vtxQueue[vi];
                                    std::fprintf(f, "vtx%d: xy=(%.2f,%.2f) z=%.0f q=%f st=(%f,%f) uv=(%u,%u) rgba=(%u,%u,%u,%u)\n",
                                                 vi, vv.x, vv.y, vv.z, vv.q, vv.s, vv.t,
                                                 (uint32_t)vv.u, (uint32_t)vv.v, vv.r, vv.g, vv.b, vv.a);
                                }
                                for (uint32_t q = 0; q * 16u + 16u <= gs->m_curPktSize; ++q)
                                {
                                    uint64_t qlo, qhi;
                                    std::memcpy(&qlo, gs->m_curPktData + q * 16u, 8);
                                    std::memcpy(&qhi, gs->m_curPktData + q * 16u + 8u, 8);
                                    std::fprintf(f, "qw%03u: %016llx %016llx\n",
                                                 q, (unsigned long long)qhi, (unsigned long long)qlo);
                                }
                                std::fclose(f);
                            }
                            std::fprintf(stderr, "[kickraw] dumped #%u size=%u srcPath=%u tbp0=%u -> work/kickraw_%u.{bin,txt}\n",
                                         s_kn, gs->m_curPktSize, gs->m_curSrcPath, s_kr, s_kn);
                            ++s_kn;
                        }
                    }
                    // [grounduv]: for the watched tbp0 in the ground region, print each draw's
                    // raw q/s/t and the texel row/col it resolves to. If ground draws resolve
                    // to the atlas' SKY rows (or negative/wrapped V), the "sky as ground" UV
                    // bug is confirmed with exact numbers.
                    if (s_kr && ctx.tex0.tbp0 == s_kr && s_krYmin > 0 && krYok)
                    {
                        static std::atomic<uint32_t> s_gu{0};
                        const uint32_t gn = s_gu.fetch_add(1);
                        if (gn < 40u || (gn % 4000u) < 2u)
                        {
                            const int W = 1 << ctx.tex0.tw, H = 1 << ctx.tex0.th;
                            const int krOfx = (int)(ctx.xyoffset.ofx >> 4);
                            char line[512]; int p = 0;
                            p += std::snprintf(line + p, sizeof(line) - (size_t)p,
                                               "[grounduv] #%u src=%u prim=%u %dx%d |", gn,
                                               gs->m_curSrcPath, (uint32_t)gs->m_prim.type, W, H);
                            for (int vi = 0; vi < 3 && p < (int)sizeof(line) - 96; ++vi)
                            {
                                const GSVertex &vv = gs->m_vtxQueue[vi];
                                const float qq = (vv.q != 0.0f) ? vv.q : 1.0f;
                                p += std::snprintf(line + p, sizeof(line) - (size_t)p,
                                                   " v%d@(%.0f,%.0f) q=%.6g st=(%.6g,%.6g) uvTexel=(%.1f,%.1f)",
                                                   vi, vv.x - krOfx, vv.y - krOfy, vv.q, vv.s, vv.t,
                                                   vv.s / qq * W, vv.t / qq * H);
                            }
                            // Resident-content id: hash the live texel VRAM + palette exactly
                            // like [maphash], so this draw's content can be matched against the
                            // mapdraw_N dumps (sky vs grass) without ambiguity.
                            uint64_t th = 1469598103934665603ull;
                            const uint8_t *tp = gs->m_vram + ((uint64_t)ctx.tex0.tbp0 * 256u) % gs->m_vramSize;
                            const uint8_t *pp = gs->m_vram + ((uint64_t)ctx.tex0.cbp * 256u) % gs->m_vramSize;
                            for (int b2 = 0; b2 < 2048; b2 += 8) th = (th ^ tp[b2]) * 1099511628211ull;
                            for (int b2 = 0; b2 < 1024; b2 += 4) th = (th ^ pp[b2]) * 1099511628211ull;
                            p += std::snprintf(line + p, sizeof(line) - (size_t)p, " content=%08x",
                                               (uint32_t)(th >> 32) ^ (uint32_t)th);
                            std::fprintf(stderr, "%s\n", line);
                            // First few ground draws: dump the FULL texture AS THIS DRAW SEES IT
                            // (its own tw/th/psm/CLUT) -> work/grounddraw_N.ppm. Sky picture =>
                            // content-timing mismatch; grass in the sampled band => UV detail.
                            if (gn < 4u)
                            {
                                const uint32_t savedFst = gs->m_prim.fst;
                                gs->m_prim.fst = 1u;
                                ensureClutCache(gs);
                                char gp[128];
                                std::snprintf(gp, sizeof(gp), "/home/z3/Desktop/bt3/work/grounddraw_%u.ppm", gn);
                                if (FILE *fp = std::fopen(gp, "wb"))
                                {
                                    std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
                                    for (int ty = 0; ty < H; ++ty)
                                        for (int tx = 0; tx < W; ++tx)
                                        {
                                            const uint32_t texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f,
                                                                                 (uint16_t)(tx * 16 + 8), (uint16_t)(ty * 16 + 8));
                                            const uint8_t rgb[3] = { (uint8_t)(texel & 0xFF), (uint8_t)((texel >> 8) & 0xFF), (uint8_t)((texel >> 16) & 0xFF) };
                                            std::fwrite(rgb, 1, 3, fp);
                                        }
                                    std::fclose(fp);
                                    std::fprintf(stderr, "[grounduv] dumped %s (content=%08x)\n",
                                                 gp, (uint32_t)(th >> 32) ^ (uint32_t)th);
                                }
                                gs->m_prim.fst = savedFst;
                            }
                        }
                    }
                }
                if ([](){ static const char *s_env = std::getenv("PS2X_TEX3D"); return s_env; }() && ctx.tex0.tbp0 == 10752u) {
                    static uint64_t m3 = 0;
                    if ((m3++ % 5000u) < 4u) {
                        const int W = 1 << ctx.tex0.tw, H = 1 << ctx.tex0.th;
                        auto uv = [&](const GSVertex &v, float &u, float &vv) {
                            const float q = (v.q != 0.0f) ? v.q : 1.0f;
                            u = v.s / q * W; vv = v.t / q * H;
                        };
                        const GSVertex &a = gs->m_vtxQueue[0], &b = gs->m_vtxQueue[1], &c2 = gs->m_vtxQueue[2];
                        float u0,v0,u1,v1,u2,v2; uv(a,u0,v0); uv(b,u1,v1); uv(c2,u2,v2);
                        std::fprintf(stderr, "[mapstq] src=%u prim=%u | q=(%.6f,%.6f,%.6f) uvTexel0=(%.1f,%.1f) uv1=(%.1f,%.1f) uv2=(%.1f,%.1f) | rawST0=(%.6f,%.6f) xy0=(%.0f,%.0f)\n",
                                     gs->m_curSrcPath, gs->m_prim.type,
                                     a.q, b.q, c2.q, u0, v0, u1, v1, u2, v2, a.s, a.t, a.x, a.y);
                    }
                }
            }
            // Fingerprint the DOTS: where are the degenerate tris on screen, and what draw state
            // (prim/tex/fbp) do they carry? Identifies which subsystem emits them.
            if (triSz <= 2.0f) {
                static uint64_t dg = 0;
                if ((dg++ % 30000u) < 4u)
                    std::fprintf(stderr, "[dot] src=%u at=(%.0f,%.0f) z=%.0f prim=%u fst=%u tbp0=%u tw=%u th=%u fbp=%u rgba=(%u,%u,%u,%u)\n",
                                 gs->m_curSrcPath, x0, y0, (float)gs->m_vtxQueue[0].z, gs->m_prim.type, gs->m_prim.fst,
                                 ctx.tex0.tbp0, ctx.tex0.tw, ctx.tex0.th, ctx.frame.fbp,
                                 gs->m_vtxQueue[0].r, gs->m_vtxQueue[0].g, gs->m_vtxQueue[0].b, gs->m_vtxQueue[0].a);
            }
            if (++cnt % 1500u == 0u) {
                std::fprintf(stderr, "[spread] per-tri: biggest=%.0fpx  nonDegenerate=%llu%% of 1500  (correct=big tris; degenerate=all dots)\n",
                             maxTri, (unsigned long long)(nonDegen*100u/1500u));
                maxTri=0.0f; nonDegen=0;
            }
        }
    }

    // PS2X_SWALIAS=1: run BT3's scene-alpha REBUILD pass in the SOFTWARE rasterizer.
    // That pass reads the Z buffer as PSMZ16 and writes it into the scene buffer RE-VIEWED as
    // PSMCT16, through a partial FBMSK (0x00003fff) -- 8-px columns that together tile the whole
    // buffer. An FBO-per-address renderer cannot express "same bytes, other bit depth, partial
    // mask", so the GPU path can only drop it (leaving the scene alpha empty, which starves the
    // outline and the shadows) or paint it literally (visible columns). The software rasterizer
    // already implements FBMSK and the aliased views exactly, and it works in VRAM -- where the
    // barrier has just put our depth, and where the downstream PSMT8H composites read from.
    {   // PS2X_FBPTALLY=<n>: every n draws, tally the DESTINATION fbp as the stream states it,
        // at rasterizer entry. Console draws vsync-3's scene into f112 and runs the whole outline
        // chain there; our f112 FBO measures near-black (RGB 1.9/6.8/20.5) with a uniform 255
        // alpha, while the scene sits in fbp0. This says whether the stream really asks for f112
        // (so we are dropping/redirecting those draws) or whether we are reading FRAME wrong.
        static const int s_ft = [](){ const char *v = std::getenv("PS2X_FBPTALLY");
                                      return (v && v[0]) ? std::atoi(v) : 0; }();
        if (s_ft > 0)
        {
            static std::map<uint32_t, unsigned long> tally;
            static unsigned long n = 0;
            ++tally[gs->activeContext().frame.fbp];
            if ((++n % (unsigned long)s_ft) == 0ul)
            {
                std::fprintf(stderr, "[fbptally] after %lu draws:", n);
                for (auto &kv : tally) std::fprintf(stderr, "  f%u=%lu", kv.first, kv.second);
                std::fprintf(stderr, "\n");
            }
        }
    }
    {   // PS2X_F336ALL: every draw that targets page 336, whatever route it ends up taking.
        static const bool s_fa = [](){ const char *v = std::getenv("PS2X_F336ALL");
                                       return v && v[0] && v[0] != '0'; }();
        if (s_fa && gs->activeContext().frame.fbp == 336u)
        {   // No first-N cap: the first draws to any page are frame-1 warm-up, and reading
            // them as representative has misled this investigation twice. Emit every one and
            // let the caller aggregate.
            const auto &a = gs->activeContext();
            std::fprintf(stderr, "[f336all] fbmsk=%08x dpsm=%02x spsm=%02x tbp=%u tme=%u prim=%u abe=%u ctxt=%u\n",
                         a.frame.fbmsk, a.frame.psm, a.tex0.psm, a.tex0.tbp0,
                         gs->m_prim.tme, gs->m_prim.type, gs->m_prim.abe, gs->m_prim.ctxt);
        }
    }
    {   // PS2X_EDGEUSE=1: census of every draw that SAMPLES the outline edge map (tbp 10752),
        // logged here at the rasterizer entry so it is directly comparable with the same census
        // taken off the console stream -- before any GPU/SW routing decision can hide one.
        // Console runs SEVEN classes; the visible dark outline is the bm0x54 pass (lerp by DEST
        // alpha), gated by two alpha-only passes that read fbp336 as CT16 tbw8 with alpha-test
        // NOTEQUAL 0. The additive bm0x68 pass alone cannot darken anything.
        static const bool s_eu = [](){ const char *v = std::getenv("PS2X_EDGEUSE");
                                       return v && v[0] && v[0] != '0'; }();
        if (s_eu && !gs->m_prim.tme
            && (gs->activeContext().frame.fbp == 0u || gs->activeContext().frame.fbp == 112u))
        {   // The DARKENER: an UNTEXTURED bm0x52 sprite (Cd - Cs*Ad) with vertex colour ~100.
            // It is what actually paints BT3's outline: the CT16/TEXA passes set the scene's
            // ALPHA wherever the fbp336 edge map is non-zero, and this pass subtracts 100*Ad
            // from the colour there. Without it the edge map can be perfect and nothing darkens.
            const auto &a = gs->activeContext();
            const uint64_t al = a.alpha;
            const unsigned bm = (unsigned)((((al>>6)&3)<<6)|(((al>>4)&3)<<4)|(((al>>2)&3)<<2)|(al&3));
            if (bm == 0x52u)
            {
                static std::map<uint32_t, unsigned long> n;
                if ((++n[a.frame.fbp] % 16ul) == 1ul)
                    std::fprintf(stderr, "[dark52] dest f%u UNTEXTURED bm0x52 fix%u fbmsk%08x rgba%08x\n",
                                 a.frame.fbp, (unsigned)((al>>32)&0xFF), a.frame.fbmsk,
                                 (unsigned)(gs->m_vtxQueue[0].r | (gs->m_vtxQueue[0].g << 8) |
                                            (gs->m_vtxQueue[0].b << 16) | (gs->m_vtxQueue[0].a << 24)));
            }
        }
        if (s_eu && gs->m_prim.tme && gs->activeContext().tex0.tbp0 == 10752u
            && (gs->activeContext().tex0.psm == 0u || gs->activeContext().tex0.psm == 1u
                || gs->activeContext().tex0.psm == 2u))
        {
            const auto &a = gs->activeContext();
            const uint64_t al = a.alpha; const uint64_t te = a.test;
            std::fprintf(stderr, "[edgeuse] dest f%u fbmsk%08x tpsm%u tbw%u tcc%u tfx%u "
                                 "bm0x%02x fix%u ate%u/atst%u/aref%u prim%u TA0=%u TA1=%u AEM=%u\n",
                         a.frame.fbp, a.frame.fbmsk, a.tex0.psm, a.tex0.tbw, a.tex0.tcc, a.tex0.tfx,
                         (unsigned)((((al>>6)&3)<<6)|(((al>>4)&3)<<4)|(((al>>2)&3)<<2)|(al&3)),
                         (unsigned)((al>>32)&0xFF),
                         (unsigned)(te & 1u), (unsigned)((te >> 1) & 7u), (unsigned)((te >> 4) & 0xFFu),
                         (unsigned)gs->m_prim.type,
                         (unsigned)gs->m_texa.ta0, (unsigned)gs->m_texa.ta1,
                         (unsigned)(gs->m_texa.aem ? 1u : 0u));
            if (a.tex0.psm == 2u)
            {   // The OUTLINE consumer. Dump VRAM page 336 in the CT16 view it samples, AT THE
                // MOMENT IT READS -- the generator wrote a real contour there earlier in the
                // frame, so if this comes back empty the content is gone by read time (the
                // second generator run's clear) rather than never having existed.
                extern void ps2xReadbackFromVram(uint32_t, uint32_t, uint32_t, int, int, uint32_t *);
                static int rn = 0;
                if (rn < 40)
                {
                    std::vector<uint32_t> px((size_t)512 * 448, 0u);
                    ps2xReadbackFromVram(336u, 8u, 0x02u, 512, 448, px.data());
                    size_t nz = 0; for (uint32_t v : px) if (v & 0x00FFFFFFu) ++nz;
                    std::fprintf(stderr, "[edgeread] #%d page336 CT16 at READ time: non-black %.3f%%\n",
                                 rn, 100.0 * (double)nz / (double)px.size());
                    const char *dr = std::getenv("PS2X_GS_REPLAY_OUT");
                    char q[256];
                    std::snprintf(q, sizeof(q), "%s/edgeread_%03d.raw", dr ? dr : ".", rn);
                    if (FILE *qf = std::fopen(q, "wb"))
                    { int hh[2] = { 512, 448 }; std::fwrite(hh, sizeof(hh), 1, qf);
                      std::fwrite(px.data(), 4, px.size(), qf); std::fclose(qf); }
                    ++rn;
                }
            }
        }
    }
    bool aliasZPass = false;
    // Fires when this draw returns, after the software rasterizer has written the pixels.
    struct AliasNotify {
        uint32_t page = 0; bool armed = false;
        ~AliasNotify() {
            if (!armed) return;
            ps2GpuRenderer().onVramWriteback(page * 32u, 512u * 448u * 4u / 256u);
        }
    } s_aliasNotify;
    // Disarms the software dirty mask when this draw returns, whichever path it took.
    struct DirtyDisarm { ~DirtyDisarm() {
        extern bool g_swDirtyActive; g_swDirtyActive = false;
        extern bool g_swoCelZSnap; g_swoCelZSnap = false;
        extern bool g_swoDiffOnly; g_swoDiffOnly = false; } } s_dirtyDisarm;
    {
        static const bool s_swAlias = [](){ const char *v = std::getenv("PS2X_SWALIAS");
                                            return v && v[0] && v[0] != '0'; }();
        if (s_swAlias && GsGpuRenderer::enabled())
        {
            const auto &actx = gs->activeContext();
            const uint32_t dpsm = actx.frame.psm, spsm = actx.tex0.psm, msk = actx.frame.fbmsk;
            bool partial = false;
            for (int by = 0; by < 4 && !partial; ++by)
            { const uint32_t mb = (msk >> (8 * by)) & 0xFFu; if (mb != 0x00u && mb != 0xFFu) partial = true; }
            // dest re-viewed as PSMCT16, source is a Z format, and the write is partially masked
            aliasZPass = gs->m_prim.tme && (dpsm == 0x02u) &&
                         (spsm == 0x30u || spsm == 0x31u || spsm == 0x32u) && partial;
            // SECOND class, same problem: BT3's outline/shadow edge detector writes into fbp336
            // RE-VIEWED as PSMCT16 through BYTE masks (ffffff00 / ffff00ff / ff000000) while
            // sampling the mask as PSMT8H. We hold fbp336 as one RGBA8 FBO, so a CT32-style byte
            // colour-mask lands on the wrong channels of a 16-bit pixel -- which is what makes
            // that pass stripe (high-frequency energy 7.07 vs 2.97 without it). The software
            // rasterizer addresses the CT16 view and its FBMSK exactly.
            const bool byteMask = (msk == 0xFFFFFF00u || msk == 0xFFFF00FFu || msk == 0xFF000000u);
            if (!aliasZPass && gs->m_prim.tme && dpsm == 0x02u && spsm == 0x1Bu && byteMask)
                aliasZPass = true;
            // THIRD class: the CLEAR that opens the chain. Those three masked passes are only
            // three of FOUR -- BT3 first clears the page with an UNTEXTURED, unmasked, opaque
            // sprite (fbmsk=0, tme=0, blend 0x64 with FIX=128, so the result is just the vertex
            // colour, which is 0). Because it carries no texture it failed the tme requirement
            // above and went to the GPU, which split the chain across two copies of page 336:
            // the clear landed in the FBO, the gradient in VRAM, and the readback's barrier
            // then flushed the cleared FBO straight over the gradient. That is what made
            // fbp336 read back as EXACTLY zero -- 0 of 229376 pixels non-zero in every channel
            // -- even though the passes ran and consumed real data.
            //
            // Tightly scoped on purpose: across the whole frame the only untextured, unmasked
            // draws to a CT16-viewed destination are these 32 clear sprites. The other CT16
            // destinations (fbp0 and fbp336's own masked passes) are all textured and masked.
            if (!aliasZPass && !gs->m_prim.tme && dpsm == 0x02u && msk == 0u) aliasZPass = true;
            // PS2X_SWOUTLINE=1: run BT3's cel/OUTLINE pass in the software rasterizer.
            //
            // It is drawn as triangle strips of sub-pixel SLIVERS -- console's own geometry
            // has a MEDIAN of one covered pixel per triangle and 39.9% cover no pixel centre
            // at all -- and GL's top-left fill rule is exclusive on right/bottom edges, so it
            // drops them. Measured against a CPU rasterization of the console stream: we cover
            // 11629 of its 16438 effective pixels (70.7%) and the loss is graded by the toon
            // ramp coordinate -- 7.5% of the u=0.19-0.44 band, 32.0% of 0.44-0.81, and 55.8%
            // of u>0.81, which is the silhouette rim itself. That is the whole missing outline;
            // every other ratio agrees (rim darkening 31.6 vs console 44.8 = 70.5%).
            //
            // The software rasterizer uses an inclusive edge test (kEdgeEpsilon) and covers
            // them. It writes VRAM, so swOutlineBegin/End bracket the run to hand the scene
            // page down and bring it back.
            // Mode 1 routes ONLY the 0x62 pass; that is 994 separate runs per frame because the
            // three cel passes interleave per body part, and each run would need its own
            // page round trip. Mode 2 routes all THREE character passes as one class, which
            // collapses to 3 runs per frame (only 1.0% of the kicks inside the character phase
            // belong to anything else) and keeps them in stream order inside one VRAM domain,
            // so the 0x62 subtract still lands between its neighbours exactly as on console.
            static const int s_swOutline = [](){ const char *v = std::getenv("PS2X_SWOUTLINE");
                                                 return v && v[0] ? std::atoi(v) : 0; }();
            const uint8_t bmNow = static_cast<uint8_t>(actx.alpha & 0xFFu);
            // All three character passes are TSTRIP with a distinct TCC/TFX pair, and with
            // those constraints each matches EXACTLY the console stream's 10055 kicks -- an
            // unscoped "0x44 + CT32" also swept up terrain decals, which the software path
            // then painted as pale quads over the grass.
            const bool celStrip = gs->m_prim.tme && gs->m_prim.abe &&
                                  gs->m_prim.type == 4 /* TSTRIP */;
            const bool celOutline = celStrip && actx.tex0.tbp0 == 15680u && bmNow == 0x62u &&
                                    actx.tex0.tcc == 1u && actx.tex0.tfx == 1u;
            const bool celOther   = celStrip &&
                                    ((bmNow == 0x64u && actx.tex0.psm == 0x14u &&
                                      actx.tex0.tcc == 1u && actx.tex0.tfx == 0u) ||
                                     (bmNow == 0x44u && actx.tex0.psm == 0x00u &&
                                      actx.tex0.tcc == 0u && actx.tex0.tfx == 0u));
            // Scope to the SCENE pages: these passes only ever target the frame buffer BT3 is
            // drawing into, and an unscoped match pulled the fbp336 edge-detect chain in too.
            const bool celDest = (actx.frame.fbp == 0u || actx.frame.fbp == 112u);
            // Mode 5: route ONLY the 0x62 subtract, and hold ONE bracket across the whole
            // character phase. Modes 1/2 both had to close the bracket on every foreign draw
            // because the blit rewrote the whole page; the per-pixel dirty mask removes that
            // constraint, so GPU draws in between survive untouched. Keeping 0x64 and 0x44 on
            // the GPU also keeps writing the character's DEPTH there -- routing them away is
            // what left bright quads on the ground, since later GPU draws then depth-tested
            // against a buffer with no character in it.
            const bool inCelPhase = celOutline || celOther;
            // Bisect which pass carries the ground brightening:
            //   7 = 0x62 + 0x64 (the PSMT4 base), leaving the 0x44 PSMCT32 pass on the GPU
            //   8 = 0x62 + 0x44, leaving the 0x64 PSMT4 pass on the GPU
            const bool cel64 = celStrip && bmNow == 0x64u && actx.tex0.psm == 0x14u &&
                               actx.tex0.tcc == 1u && actx.tex0.tfx == 0u;
            const bool cel44 = celStrip && bmNow == 0x44u && actx.tex0.psm == 0x00u &&
                               actx.tex0.tcc == 0u && actx.tex0.tfx == 0u;
            const bool isOutline = !celDest ? false
                                 : (s_swOutline == 1 || s_swOutline == 5) ? celOutline
                                 : (s_swOutline == 2 || s_swOutline == 3) ? inCelPhase
                                 : (s_swOutline == 7) ? (celOutline || cel64)
                                 : (s_swOutline == 8) ? (celOutline || cel44)
                                 // Mode 9 -- DIFFERENCE coverage: the 0x62 pass stays on the
                                 // GPU exactly as in the control config (its broad interior
                                 // shading is what mode 2's routing REMOVED -- the real-fight
                                 // whole-frame wash), and the software pass adds ONLY the
                                 // pixels GL's fill rule drops (the sliver rim). No
                                 // double-draw: SW skips strictly-interior pixels.
                                 : (s_swOutline == 9) ? celOutline
                                 : false;
            {   // PS2X_SWOCENSUS=1: how many draws we route per class. Console sends exactly
                // 10055 kicks for each of the three, so a larger count here means the predicate
                // is sweeping in extra geometry (the ground shadow decals share the 0x44 state).
                static const bool s_sc2 = [](){ const char *v = std::getenv("PS2X_SWOCENSUS");
                                                return v && v[0] && v[0] != '0'; }();
                if (s_sc2 && (celOutline || celOther))
                {
                    static unsigned long n62 = 0, n64 = 0, n44 = 0;
                    if (celOutline) ++n62;
                    else if (bmNow == 0x64u) ++n64; else ++n44;
                    static std::map<uint32_t, unsigned long> destPage;
                    ++destPage[actx.frame.fbp];
                    if (((n62 + n64 + n44) % 10000ul) == 0ul)
                    {
                        std::fprintf(stderr, "[swocensus] cel draws by DEST PAGE:");
                        for (auto &kv : destPage) std::fprintf(stderr, " f%u=%lu", kv.first, kv.second);
                        std::fprintf(stderr, "\n");
                    }
                    if (((n62 + n64 + n44) % 10000ul) == 0ul)
                        std::fprintf(stderr, "[swocensus] routed 0x62=%lu 0x64=%lu 0x44=%lu"
                                             "   (console: 10055 each)\n", n62, n64, n44);
                }
            }
            if (s_swOutline == 6 && celDest && inCelPhase)
                return;   // CONTROL: drop the cel class from BOTH paths. If the ground quads
                          // are bright here too, the cel class is what darkens them and the
                          // software path is simply failing to draw them; if they stay dark,
                          // something else darkens them and the routing broke that instead.
            if (s_swOutline == 4 && (celOutline || celOther))
            {   // IDENTITY TEST: bracket the page and immediately hand it back, routing
                // NOTHING to software. VRAM equals the FBO and no software draw happens in
                // between, so the blit must be a no-op. Any artifact under mode 4 belongs to
                // the round trip itself, not to software rasterization.
                static uint32_t lastFbp = 0xFFFFFFFFu;
                if (lastFbp != actx.frame.fbp)
                {
                    lastFbp = actx.frame.fbp;
                    ps2GpuRenderer().swOutlineBegin(actx.frame.fbp);
                    ps2GpuRenderer().swOutlineEnd();
                }
            }
            else if (isOutline)
            {
                {
                    extern bool g_swoCelZSnap;
                    g_swoCelZSnap = celOutline;   // cleared by s_dirtyDisarm alongside the mask
                }
                {   // The ramp CLUTs are RENDER TARGETS (pages 499-500, animated per frame).
                    // The routed pass's decode reads them from VRAM, but only the TEXTURE page
                    // was ever barrier-flushed -- the CLUT page was not, so the SW ramp came
                    // from stale VRAM: at u~0.74 console's ramp subtracts ~0, ours 25-50 (the
                    // interior wireframe). Flush the CLUT page before the decode.
                    // NOTE: needs the page in PS2X_BARONLY to pass the gate (add 499,500).
                    if (celOutline && actx.tex0.cbp != 0u)
                        { extern std::atomic<unsigned long> g_barReqPushed; const unsigned long b0 = g_barReqPushed.load(std::memory_order_relaxed);
                          ps2GpuRenderer().barrierBeforeRead(actx.tex0.cbp, false, false);
                          if (g_barReqPushed.load(std::memory_order_relaxed) != b0) reqCensus(1, actx.tex0.cbp, actx.tex0.psm, 1 << actx.tex0.tw, 1 << actx.tex0.th, actx.frame.fbp); }
                }
                {   // Mark the dirty mask ONLY while a routed cel draw is rasterizing. Arming
                    // it for the whole bracket was wrong: BT3's other software passes (the CT16
                    // alpha rebuild and the fbp336 edge chain, kicks 15179..41599) run inside
                    // the character phase, and their writePixel calls marked scene coordinates
                    // dirty -- so the blit then uploaded CT16-aliased VRAM into the scene as
                    // RGB. That is what put bright quads on the ground (measured 116.7 where
                    // console is 78.6 and dropping the cel class entirely gives 79.4).
                    extern bool g_swDirtyActive;
                    g_swDirtyActive = true;
                }
                {   // Depth-only companion: the software path takes the colour, but this draw
                    // also writes Z on the GS, so record it to the GPU with the colour mask
                    // off so the depth buffer still sees the character.
                    // Mode 9: FULL record instead -- the GPU keeps drawing the pass exactly as
                    // control; the SW half only adds the difference pixels.
                    extern bool g_recordDepthOnly;
                    extern bool g_swoDiffOnly;
                    static const bool s_zc = [](){ const char *v = std::getenv("PS2X_SWODEPTH");
                                                   return !(v && v[0] == '0'); }();
                    if (s_swOutline == 9)
                    {
                        g_swoDiffOnly = true;
                        if (GsGpuRenderer::enabled()) recordSpriteGPU(gs);
                    }
                    else if (s_zc && GsGpuRenderer::enabled())
                    {
                        g_recordDepthOnly = true;
                        recordSpriteGPU(gs);
                        g_recordDepthOnly = false;
                    }
                }
                // BT3 double-buffers the scene, and the cel passes switch between fbp0 and
                // fbp112. A bracket tracks ONE page, so a mid-run switch would flush and blit
                // the wrong one -- close the current bracket and open the new page's.
                if (ps2GpuRenderer().swOutlineActive() &&
                    ps2GpuRenderer().swOutlinePage() != actx.frame.fbp)
                    ps2GpuRenderer().swOutlineEnd();
                if (!ps2GpuRenderer().swOutlineActive())
                    ps2GpuRenderer().swOutlineBegin(actx.frame.fbp);
                aliasZPass = true;
            }
            else if (ps2GpuRenderer().swOutlineActive() &&
                     !(s_swOutline == 5 && celDest && inCelPhase))
            {
                // The run has ended: fold the software result back into the FBO before this
                // draw renders on top of it. In mode 5 the other two cel passes do NOT end it.
                ps2GpuRenderer().swOutlineEnd();
            }
            if (aliasZPass)
            {
                // The GPU record path is what normally raises the read barrier; this draw skips
                // it, so ask for the flush here or the decode below reads stale VRAM.
                //
                // A PSMT8H source takes its palette index from the page's ALPHA byte. That alpha
                // lives in the FBO and is invisible to the barrier's write tracking, which only
                // records draws whose DESTINATION is the page -- so the default gating returned
                // early here and the decode read stale VRAM every time. BT3's fbp336 outline
                // generator is exactly such a reader: it samples the fbp224 character mask as
                // PSMT8H, writes a base ramp through CLUT16012, then SUBTRACTS the same ramp
                // sampled one texel over (two 0x62 passes, byte-masked to R and to G). Fed a
                // CONSTANT index those three passes cancel exactly, which is why the edge map
                // measured 0 of 229376 non-zero while the passes demonstrably ran.
                // Same fix, same reason, as the wantsAlphaAsData argument at the texture-resolve
                // call site below.
                // PS2X_ALIASBAR=1 restores the unconditional alpha-as-data flush. It is OFF by
                // default because `wantsAlphaAsData` BYPASSES the write-tracking guard, so page
                // 224 was re-flushed on all 128 of the generator's alias passes -- each one
                // overwriting the good VRAM mask with the FBO's current content. Measured: the
                // mask is 20 levels at pass 31 and a uniform 255 by pass 64, after which the
                // base ramp writes nothing and the second batch's clear leaves fbp336 empty for
                // the consumer. The destination of the mask build IS page 224, so ordinary dirty
                // tracking already flushes it exactly once, when it changes.
                static const bool s_aliasBar = [](){ const char *v = std::getenv("PS2X_ALIASBAR");
                                                     return v && v[0] && v[0] != '0'; }();
                if (gs->m_prim.tme)
                    { extern std::atomic<unsigned long> g_barReqPushed; const unsigned long b0 = g_barReqPushed.load(std::memory_order_relaxed);
                      ps2GpuRenderer().barrierBeforeRead(actx.tex0.tbp0, true,
                                                       s_aliasBar && spsm == 0x1Bu);
                      if (g_barReqPushed.load(std::memory_order_relaxed) != b0) reqCensus(2, actx.tex0.tbp0, actx.tex0.psm, 1 << actx.tex0.tw, 1 << actx.tex0.th, actx.frame.fbp); }
                // The scene-alpha rebuild is about to write VRAM page 0 directly. Hand the
                // GPU's own scene alpha over first: it carries the characters' FBA-forced
                // MSB, which is what gives them a silhouette in the mask the outline and
                // shadow composites read. Once per frame; a no-op unless PS2X_ASEED=1.
                if (actx.frame.fbp == 0u && gs->m_prim.tme &&
                    (spsm == 0x30u || spsm == 0x31u || spsm == 0x32u))
                    ps2GpuRenderer().seedSceneAlphaForRebuild(0u);
                ps2GpuRenderer().reportFboAlpha(actx.frame.fbp, "at-rebuild");
                static int n = 0;
                static const bool s_d = [](){ const char *v = std::getenv("PS2X_BARDIAG");
                                              return v && v[0] && v[0] != '0'; }();
                if (s_d && n++ < 400)
                    std::fprintf(stderr, "[swalias] SW pass dest fbp=%u psm=%02x fbmsk=%08x src tbp=%u psm=%02x\n",
                                 actx.frame.fbp, dpsm, msk, actx.tex0.tbp0, spsm);
                // The software path writes VRAM directly, which does NOT bump the texture
                // cache's content sequence -- so the composites that read this buffer back a
                // few draws later would decode the copy from BEFORE the rebuild. Notify on the
                // way out of this draw, once the pixels are actually written.
                {   static const bool s_fp = [](){ const char *v = std::getenv("PS2X_F336PRE");
                                                  return v && v[0] && v[0] != '0'; }();
                    if (s_fp && actx.frame.fbp == 336u)
                    {   static int n = 0;
                        if (n < 6)
                        { extern void ps2xVramCt16Stats(uint32_t, uint32_t, const char *);
                          char lbl[48]; std::snprintf(lbl, sizeof(lbl), "before pass fbmsk=%08x", actx.frame.fbmsk);
                          ps2xVramCt16Stats(336u, actx.frame.fbw ? actx.frame.fbw : 8u, lbl); ++n; } }
                }
                {   // [rebuildcen] PS2X_REBUILDCEN=1: the scene-alpha rebuild (dest fbp0/112
                    // CT16 view, fbmsk 00003fff, src Z-as-PSMZ16). Census, at each pass entry,
                    // BOTH sides in VRAM: how much CT16 alpha the dest page already carries and
                    // how much nonzero Z the source page offers. fr0 reads snapshot (console) Z;
                    // fr1 reads our own depth writeback -- if fr1's source is degenerate the
                    // whole outline chain starves from here.
                    static const bool s_rc = [](){ const char *v = std::getenv("PS2X_REBUILDCEN");
                                                   return v && v[0] && v[0] != '0'; }();
                    if (s_rc && (actx.frame.fbp == 0u || actx.frame.fbp == 112u)
                        && (spsm == 0x30u || spsm == 0x31u || spsm == 0x32u))
                    {
                        static int rn = 0;
                        if (rn < 140)
                        {
                            ++rn;
                            extern void ps2xReadbackFromVram(uint32_t, uint32_t, uint32_t, int, int, uint32_t *);
                            // FULL CT16 view: the re-cut has 896 rows; the character's rows sit
                            // at ~360-880, so a 448-row window missed nearly all of them.
                            std::vector<uint32_t> sc((size_t)512 * 896, 0u);
                            ps2xReadbackFromVram(actx.frame.fbp, 8u, 0x02u, 512, 896, sc.data());
                            size_t anz = 0;
                            // ReadVram(psm 0x02) returns RAW 16-bit CT16 texels: the alpha bit
                            // is bit15, NOT an expanded 0xFF000000 byte (checking the byte made
                            // this census read 0.00% while a read-after-write probe showed the
                            // rebuild landing 26.7% alpha bits -- a blind instrument, not a
                            // broken rebuild).
                            for (uint32_t v2 : sc) if (v2 & 0x8000u) ++anz;
                            std::vector<uint32_t> zz((size_t)512 * 448, 0u);
                            ps2xReadbackFromVram(actx.tex0.tbp0 / 32u, 8u, 0x30u, 512, 448, zz.data());
                            size_t znz = 0, lowNz = 0; uint32_t zmx = 0;
                            for (uint32_t v2 : zz)
                            { if (v2) { ++znz; if (v2 > zmx) zmx = v2; if (v2 & 0xFFu) ++lowNz; } }
                            static int lonce = 0;
                            if (lonce++ < 4)
                                std::fprintf(stderr, "[rebuildcen] RAW Z32 words: nz=%zu zmax=%u lowbyte!=0 on %zu (%.1f%%) -> %s\n",
                                             znz, zmx, lowNz, znz ? 100.0 * (double)lowNz / (double)znz : 0.0,
                                             (znz && lowNz * 10 < znz) ? "QUANTIZED (D24-era)" : "exact-ish");
                            std::fprintf(stderr, "[rebuildcen] #%d dest=f%u msk=%08x destA!=0 %.2f%% | srcZpage=%u Znz %.2f%% zmax24=%u\n",
                                         rn, actx.frame.fbp, actx.frame.fbmsk,
                                         100.0 * (double)anz / (double)sc.size(),
                                         actx.tex0.tbp0 / 32u,
                                         100.0 * (double)znz / (double)zz.size(), zmx);
                        }
                    }
                }
                {   // PS2X_F336VDUMP=1: dump VRAM page 336 AFTER each alias pass, in the CT32
                    // view the downstream composite actually samples (tbp10752, fbw 4). The
                    // GPU-side texture dump reads a different copy of this page, so it showed
                    // "exactly zero" while VRAM measurably held content -- dump the bytes the
                    // generator really wrote instead of inferring from the consumer.
                    static const bool s_vd = [](){ const char *v = std::getenv("PS2X_F336VDUMP");
                                                   return v && v[0] && v[0] != '0'; }();
                    if (s_vd && actx.frame.fbp == 336u)
                    {
                        extern void ps2xReadbackFromVram(uint32_t, uint32_t, uint32_t, int, int, uint32_t *);
                        static int dn = 0;
                        if (dn < 200)
                        {
                            // Read it in the view the GENERATOR writes (PSMCT16, fbw 8 = 512px),
                            // not the CT32 view the consumer uses -- a CT32 window over CT16
                            // bytes packs two pixels into one word and the shape is unreadable.
                            const char *dir = std::getenv("PS2X_GS_REPLAY_OUT");
                            const int w = 512, h = 448;
                            std::vector<uint32_t> px((size_t)w * h, 0u);
                            ps2xReadbackFromVram(336u, 8u, 0x02u, w, h, px.data());
                            {   // Dump the SOURCE the base pass reads (page 224, whose ALPHA is
                                // the PSMT8H index) alongside the destination. The generator runs
                                // more than once per frame and only some runs produce content;
                                // this says whether the dead runs are fed a saturated mask rather
                                // than failing in the passes themselves.
                                std::vector<uint32_t> mk((size_t)512 * 448, 0u);
                                ps2xReadbackFromVram(224u, 8u, 0x00u, 512, 448, mk.data());
                                char mp[256];
                                std::snprintf(mp, sizeof(mp), "%s/f224src_%03d_msk%08x.raw",
                                              dir ? dir : ".", dn, actx.frame.fbmsk);
                                if (FILE *mf = std::fopen(mp, "wb"))
                                { int mh[2] = { 512, 448 }; std::fwrite(mh, sizeof(mh), 1, mf);
                                  std::fwrite(mk.data(), 4, mk.size(), mf); std::fclose(mf); }
                            }
                            char pth[256];
                            std::snprintf(pth, sizeof(pth), "%s/f336vram_%03d_msk%08x.raw",
                                          dir ? dir : ".", dn, actx.frame.fbmsk);
                            if (FILE *df = std::fopen(pth, "wb"))
                            { int hd[2] = { w, h }; std::fwrite(hd, sizeof(hd), 1, df);
                              std::fwrite(px.data(), 4, px.size(), df); std::fclose(df); }
                            ++dn;
                        }
                    }
                }
                {   // Mode 9 (difference-only outline): do NOT arm the writeback notify.
                    // It fires onVramWriteback on the SCENE page after every routed draw,
                    // invalidating every live texture decode of pages 0/112 -- the DoF and
                    // composite chain then re-decodes the scene from one-frame-old VRAM,
                    // which was the whole-frame wash (constant MAE ~77.6 through five other
                    // attribution toggles). The blit carries the SW pixels explicitly; no
                    // consumer needs a VRAM re-decode of the scene page for this mode.
                    extern bool g_swoDiffOnly;
                    if (!g_swoDiffOnly)
                    {
                        s_aliasNotify.page = actx.frame.fbp;
                        s_aliasNotify.armed = true;
                    }
                }
            }
        }
    }

    // GPU mode: record to the hardware renderer instead of software-rasterizing.
    // Stage 1 handles sprites (98% of the menu); other primitives are skipped for now.
    struct AliasKindClear { ~AliasKindClear() { extern int g_recordAliasKind; g_recordAliasKind = 0; } } s_akClear;
    {   // [gpualias] PS2X_GPUALIAS=1: census of the CT16-view alias passes. The SWALIAS block above is OFF in the
        // current stack, so these draws flow through the NORMAL GPU record below -- classify them here and tag the
        // recorded command via g_recordAliasKind (cleared by s_akClear at every exit). Census only: nothing else changes.
        static const int s_ga = [](){ const char *v = std::getenv("PS2X_GPUALIAS"); return v && v[0] ? std::atoi(v) : 0; }();
        if (s_ga > 0 && GsGpuRenderer::enabled() && gs->activeContext().frame.psm == 0x02u)
        {
            const auto &actx = gs->activeContext();
            const uint32_t spsm = actx.tex0.psm, msk = actx.frame.fbmsk;
            const bool tme = gs->m_prim.tme != 0;
            int kind = 0;
            if (gs->m_prim.type == 6u)
                kind = !tme ? (msk == 0u ? 2 : 0)
                            : ((spsm == 0x30u || spsm == 0x31u || spsm == 0x32u) ? 1 : (spsm == 27u ? 3 : 0));
            {   // hook census incl. unclassified, to check inventory completeness
                static unsigned long n = 0;
                if (++n <= 8 || (n % 4000ul) == 0ul)
                    std::fprintf(stderr, "[gpualias-hook] #%lu kind=%d type=%u tme=%u spsm=0x%x dfbp=%u msk=%08x\n",
                                 n, kind, (unsigned)gs->m_prim.type, (unsigned)tme, spsm, actx.frame.fbp, msk);
            }
            extern int g_recordAliasKind;
            g_recordAliasKind = kind;
            if (kind == 3 && s_ga >= 4)
            {   // [gpualias] hand the executor the SAME palette the SW chain semantics use
                // (applyTexa'd m_clutCache); the GPU-published palette disagreed above idx 15.
                ensureClutCache(gs);
                extern uint32_t g_gaClutData[256]; extern std::atomic<unsigned> g_gaClutSeq;
                static uint64_t lastKey = 0;
                if (gs->m_clutCacheKey != ~0ull && gs->m_clutCacheKey != lastKey)
                {
                    lastKey = gs->m_clutCacheKey;
                    std::memcpy(g_gaClutData, gs->m_clutCache, sizeof(uint32_t) * 256);
                    g_gaClutSeq.fetch_add(1);
                }
            }
        }
    }
    if (GsGpuRenderer::enabled() && !aliasZPass)
    {
        switch (gs->m_prim.type)
        {
        case GS_PRIM_SPRITE:
        case GS_PRIM_TRIANGLE:
        case GS_PRIM_TRISTRIP:
        case GS_PRIM_TRIFAN:
            {   // [cpuclut] a palette-page sprite is blended in VRAM by the PS2X_CPUCLUT block below (exact GS blend
                // against the uploaded palette). It must NOT also be recorded to the GPU: that created a 64x448 FBO
                // for fbp480 holding a blend against garbage (the FBO never sees uploads), and the barrier flush
                // then wrote that over the correct palette -> the hit character turned solid black and stayed so
                // (every later CLUT read re-flushed the stale FBO). User capture hit.gs frame 614.
                static const bool s_cclutG = [](){ const char *v = std::getenv("PS2X_CPUCLUT"); return !(v && v[0] == '0'); }();
                const bool cpuClut = s_cclutG && ctx.frame.fbp == 480u && gs->m_prim.type == 6u && gs->m_prim.tme == 0;
                if (!cpuClut) recordSpriteGPU(gs); // handles both sprite (2 tris) and triangle (1 tri)
            }
            break;
        case GS_PRIM_LINE:
        case GS_PRIM_LINESTRIP:
        {
            // The popup's ornate frame + corner brackets are drawn as flat 1px lines
            // (software: drawLine). Emit each as a thin (1px) untextured quad = 2 tris
            // so the GPU renderer draws it. drawLine ignores texture, so lines are flat.
            const auto &lctx = gs->activeContext();
            const int ofx = lctx.xyoffset.ofx >> 4;
            const int ofy = lctx.xyoffset.ofy >> 4;
            const GSVertex &lv0 = gs->m_vtxQueue[0];
            const GSVertex &lv1 = gs->m_vtxQueue[1];
            const float lx0 = static_cast<float>(static_cast<int>(lv0.x) - ofx);
            const float ly0 = static_cast<float>(static_cast<int>(lv0.y) - ofy);
            const float lx1 = static_cast<float>(static_cast<int>(lv1.x) - ofx);
            const float ly1 = static_cast<float>(static_cast<int>(lv1.y) - ofy);
            // Untextured flat color: pre-scale rgb by 128/255 (÷128 modulate shader
            // scales it back); alpha gets the blend 255/128. Matches colorBytes' flat path.
            auto colU = [](const GSVertex &v, uint8_t &cr, uint8_t &cg, uint8_t &cb, uint8_t &ca) {
                cr = static_cast<uint8_t>((v.r * 128u) / 255u);
                cg = static_cast<uint8_t>((v.g * 128u) / 255u);
                cb = static_cast<uint8_t>((v.b * 128u) / 255u);
                ca = static_cast<uint8_t>(std::min(255u, (v.a * 255u) >> 7));
            };
            uint8_t r0, g0, b0, a0, r1, g1, b1, a1;
            if (gs->m_prim.iip) { colU(lv0, r0, g0, b0, a0); colU(lv1, r1, g1, b1, a1); }
            else { colU(lv1, r0, g0, b0, a0); r1 = r0; g1 = g0; b1 = b0; a1 = a0; }
            // Perpendicular offset for a ~1px-thick quad.
            const float ldx = lx1 - lx0, ldy = ly1 - ly0;
            const float llen = std::sqrt(ldx * ldx + ldy * ldy);
            const float pxo = (llen < 1e-3f) ? 0.5f : (-ldy / llen * 0.5f);
            const float pyo = (llen < 1e-3f) ? 0.0f : (ldx / llen * 0.5f);
            GsGpuRenderer::DrawCmd lbase{};
            lbase.texKey = 0;
            lbase.isTriangle = true;
            lbase.destFbp = lctx.frame.fbp;
            lbase.destFbw = lctx.frame.fbw;
            lbase.destPsm = static_cast<uint8_t>(lctx.frame.psm);
            lbase.srcTbp0 = 0u;
            lbase.sx = lctx.scissor.x0;
            lbase.sy = lctx.scissor.y0;
            lbase.sw = std::max(0, lctx.scissor.x1 - lctx.scissor.x0 + 1);
            lbase.sh = std::max(0, lctx.scissor.y1 - lctx.scissor.y0 + 1);
            struct LP { float x, y; uint8_t r, g, b, a; };
            const LP A{lx0 + pxo, ly0 + pyo, r0, g0, b0, a0};
            const LP B{lx0 - pxo, ly0 - pyo, r0, g0, b0, a0};
            const LP C{lx1 + pxo, ly1 + pyo, r1, g1, b1, a1};
            const LP D{lx1 - pxo, ly1 - pyo, r1, g1, b1, a1};
            GsGpuRenderer &lr = ps2GpuRenderer();
            auto emitTri = [&](const LP &p0, const LP &p1, const LP &p2) {
                GsGpuRenderer::DrawCmd c = lbase;
                const LP ps[3] = {p0, p1, p2};
                for (int i = 0; i < 3; ++i) {
                    c.tri[i].x = ps[i].x; c.tri[i].y = ps[i].y;
                    c.tri[i].u = 0.0f; c.tri[i].v = 0.0f;
                    c.tri[i].r = ps[i].r; c.tri[i].g = ps[i].g; c.tri[i].b = ps[i].b; c.tri[i].a = ps[i].a;
                }
                lr.recordCmd(c);
            };
            emitTri(A, B, C);
            emitTri(B, D, C);
            {
                static const bool s_ld = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                static std::atomic<uint32_t> s_lc{0};
                uint32_t n = s_lc.fetch_add(1) + 1u;
                if (s_ld && n <= 24)
                    std::fprintf(stderr, "[lineGPU] #%u (%.0f,%.0f)-(%.0f,%.0f) col=(%u,%u,%u,%u) iip=%u\n",
                                 n, lx0, ly0, lx1, ly1, r0, g0, b0, a0, gs->m_prim.iip);
            }
            break;
        }
        default:
            break; // points: not yet (rare in BT3 UI)
        }
        // [cpuclut] PCSX2-style "CPU CLUT Render" (default ON, PS2X_CPUCLUT=0 disables):
        // draws that RENDER INTO the char-palette arena (fbp 480 — e.g. the Kaioken buff's
        // 64x64 tint sprite, blend (Cs-Cd)*FIX/128+Cd with an animated FIX) apply to guest
        // VRAM at RECORD time with exact GS integer blending. Palettes are DATA the CPU-side
        // texture decoder consumes — a GPU render would need a same-frame readback (a hard
        // sync stall; and our record runs a full list ahead, so late readbacks get clobbered
        // by the game's per-frame palette re-upload — measured). ~4096 flat pixels/frame:
        // a palette operation, not scene rasterization. Resolution-independent (upscale-safe).
        {
            static const bool s_cclut = [](){ const char *v = std::getenv("PS2X_CPUCLUT"); return !(v && v[0] == '0'); }();
            if (s_cclut && ctx.frame.fbp == 480u && gs->m_prim.type == 6u && gs->m_prim.tme == 0)
            {
                const int cofx = ctx.xyoffset.ofx >> 4, cofy = ctx.xyoffset.ofy >> 4;
                const GSVertex &q0 = gs->m_vtxQueue[0], &q1 = gs->m_vtxQueue[1];
                int cx0 = (int)q0.x - cofx, cy0 = (int)q0.y - cofy;
                int cx1 = (int)q1.x - cofx, cy1 = (int)q1.y - cofy;
                if (cx1 < cx0) std::swap(cx0, cx1);
                if (cy1 < cy0) std::swap(cy0, cy1);
                cx0 = std::max(cx0, (int)ctx.scissor.x0); cy0 = std::max(cy0, (int)ctx.scissor.y0);
                cx1 = std::min(cx1, (int)ctx.scissor.x1 + 1); cy1 = std::min(cy1, (int)ctx.scissor.y1 + 1);
                const uint32_t cbw = ctx.frame.fbw ? (uint32_t)ctx.frame.fbw : 1u;
                const uint32_t cbase = ctx.frame.fbp * 32u;
                const uint32_t cmsk = ctx.frame.fbmsk;
                const bool cabe = gs->m_prim.abe != 0;
                const uint32_t alw = (uint32_t)(ctx.alpha & 0xFFu);
                const uint32_t selA = alw & 3u, selB = (alw >> 2) & 3u, selC = (alw >> 4) & 3u, selD = (alw >> 6) & 3u;
                const int cfix = (int)((ctx.alpha >> 32) & 0xFFu);
                // GS sprites take the flat color from the SECOND vertex.
                const int csr = q1.r, csg = q1.g, csb = q1.b, csa = q1.a;
                const bool cclamp = gs->m_colclamp;
                for (int py = cy0; py < cy1; ++py)
                    for (int px = cx0; px < cx1; ++px)
                    {
                        const uint32_t dpx = GSMem::ReadCT32(gs->m_vram, cbase, cbw, (uint32_t)px, (uint32_t)py);
                        const int ddr = dpx & 0xFF, ddg = (dpx >> 8) & 0xFF, ddb = (dpx >> 16) & 0xFF, dda = (dpx >> 24) & 0xFF;
                        int orr, org, orb;
                        if (!cabe) { orr = csr; org = csg; orb = csb; }
                        else
                        {
                            const int Cc = (selC == 0u) ? csa : (selC == 1u) ? dda : cfix;
                            auto cterm = [&](uint32_t sel, int sv, int dv) -> int { return sel == 0u ? sv : (sel == 1u ? dv : 0); };
                            auto cblend = [&](int sv, int dv) -> int {
                                int r = (((cterm(selA, sv, dv) - cterm(selB, sv, dv)) * Cc) >> 7) + cterm(selD, sv, dv);
                                if (cclamp) r = r < 0 ? 0 : (r > 255 ? 255 : r); else r &= 0xFF;
                                return r;
                            };
                            orr = cblend(csr, ddr); org = cblend(csg, ddg); orb = cblend(csb, ddb);
                        }
                        uint32_t outw = (uint32_t)orr | ((uint32_t)org << 8) | ((uint32_t)orb << 16) | ((uint32_t)csa << 24);
                        outw = (outw & ~cmsk) | (dpx & cmsk); // FBMSK: masked bits keep dest
                        GSMem::WriteCT32(gs->m_vram, cbase, cbw, (uint32_t)px, (uint32_t)py, outw);
                    }
                ps2GpuRenderer().onVramUpload(cbase, 64u); // 2 pages: re-key the palette decodes
                return; // applied in place; nothing else to do for this draw
            }
        }
        return;
    }

    {
        // TEST (PS2X_SKIP_DARK_FONT): skip dark font-atlas draws to see if the
        // white text is being covered by the dark (shadow/fade) pass.
        static const int s_skipDark = [](){ const char *v=[](){ static const char *s_env = std::getenv("PS2X_SKIP_DARK_FONT"); return s_env; }(); return (v&&v[0]&&v[0]!='0')?1:0; }();
        if (s_skipDark && gs->m_prim.tme && ctx.tex0.tbp0 == 10760u && gs->m_vtxQueue[0].r < 64u)
            return;
    }
    {
        // Lightweight env-gated draw probe (PS2X_PRIM_PROBE): confirms primitives
        // rasterize and to which framebuffer (fbp) vs the displayed one.
        static const bool s_primProbe = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_PRIM_PROBE"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_primProbe)
        {
            static std::atomic<uint32_t> s_pn{0};
            uint32_t n = s_pn.fetch_add(1) + 1u;
            // Histogram of (tbp0,tpsm,cbp,tfx,abe) combos for textured draws so we
            // see exactly how the font atlas (tbp0=10760/0x2a08) is drawn (CLUT/TFX).
            const bool textured = gs->m_prim.tme != 0;
            if (textured)
            {
                static std::mutex s_cm; static std::map<uint64_t,uint32_t> s_combo;
                std::lock_guard<std::mutex> lk(s_cm);
                uint64_t key = ((uint64_t)ctx.tex0.tbp0<<32) | ((uint64_t)ctx.tex0.cbp<<8)
                             | ((uint64_t)(ctx.tex0.psm&0x3f)<<2) | ((uint64_t)ctx.tex0.tfx&3);
                uint32_t &c = s_combo[key]; c++;
                static std::atomic<uint32_t> s_cc{0};
                if ((s_cc.fetch_add(1) % 20000u)==1u) {
                    std::cerr << "[texcombo]";
                    for (auto &kv : s_combo)
                        std::cerr << " tbp0=" << (uint32_t)(kv.first>>32)
                                  << ":tpsm=0x" << std::hex << ((kv.first>>2)&0x3f)
                                  << ":cbp=" << std::dec << (uint32_t)((kv.first>>8)&0xFFFFFF)
                                  << ":tfx=" << (uint32_t)(kv.first&3) << "x" << kv.second;
                    std::cerr << std::endl;
                }
            }
            // Font-draw state: for the font atlas (tbp0=10760/0x2a08), log vertex
            // color, tcc, alpha-blend + alpha-test regs -> why glyphs are invisible.
            if (textured && ctx.tex0.tbp0 == 10760u)
            {
                static std::mutex s_fm;
                // bounding box for WHITE(r>=200) vs DARK(r<64) font draws
                static int wx0=99999,wy0=99999,wx1=-1,wy1=-1,dx0=99999,dy0=99999,dx1=-1,dy1=-1;
                static uint32_t wc=0,dc=0;
                std::lock_guard<std::mutex> lk(s_fm);
                const GSVertex &v = gs->m_vtxQueue[0];
                int px=(int)v.x, py=(int)v.y;
                if (v.r>=200){ wc++; if(px<wx0)wx0=px; if(py<wy0)wy0=py; if(px>wx1)wx1=px; if(py>wy1)wy1=py; }
                else if (v.r<64){ dc++; if(px<dx0)dx0=px; if(py<dy0)dy0=py; if(px>dx1)dx1=px; if(py>dy1)dy1=py; }
                static std::atomic<uint32_t> s_fc{0};
                if ((s_fc.fetch_add(1)%20000u)==1u){
                    int ofx=ctx.xyoffset.ofx>>4, ofy=ctx.xyoffset.ofy>>4;
                    const GSVertex &v0=gs->m_vtxQueue[0], &v1=gs->m_vtxQueue[1], &v2=gs->m_vtxQueue[2];
                    float fx0=v0.x-ofx,fy0=v0.y-ofy,fx1=v1.x-ofx,fy1=v1.y-ofy,fx2=v2.x-ofx,fy2=v2.y-ofy;
                    float denom=(fy1-fy2)*(fx0-fx2)+(fx2-fx1)*(fy0-fy2);
                    std::cerr<<"[font-tri] type="<<(uint32_t)gs->m_prim.type<<" iip="<<(uint32_t)gs->m_prim.iip
                             <<" fst="<<(uint32_t)gs->m_prim.fst
                             <<" v0=("<<fx0<<","<<fy0<<") v1=("<<fx1<<","<<fy1<<") v2=("<<fx2<<","<<fy2<<")"
                             <<" area="<<denom
                             <<" uv0=("<<(v0.u>>4)<<","<<(v0.v>>4)<<") uv1=("<<(v1.u>>4)<<","<<(v1.v>>4)<<") uv2=("<<(v2.u>>4)<<","<<(v2.v>>4)<<")"
                             <<std::endl;
                }
            }
            static std::atomic<uint32_t> s_texn{0};
            const uint32_t tn = textured ? (s_texn.fetch_add(1) + 1u) : 0u;
            if (n <= 20u || (n % 5000u) == 0u || (textured && tn <= 40u))
                std::cerr << "[prim] #" << n
                          << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                          << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                          << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                          << " fbp=" << ctx.frame.fbp
                          << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                          << " | tbp0=" << ctx.tex0.tbp0
                          << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                          << " tpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                          << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                          << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                          << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                          << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                          << " cbp=" << ctx.tex0.cbp
                          << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                          << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                          << std::endl;
        }
    }
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                      << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_texclut.cou)
                      << " cov=" << gs->m_texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << gs->m_vtxQueue[0].x << "," << gs->m_vtxQueue[0].y << ")"
                      << " uv0=(" << (gs->m_vtxQueue[0].u >> 4) << "," << (gs->m_vtxQueue[0].v >> 4) << ")"
                      << " stq0=(" << gs->m_vtxQueue[0].s << "," << gs->m_vtxQueue[0].t << "," << gs->m_vtxQueue[0].q << ")"
                      << " v1=(" << gs->m_vtxQueue[1].x << "," << gs->m_vtxQueue[1].y << ")"
                      << " uv1=(" << (gs->m_vtxQueue[1].u >> 4) << "," << (gs->m_vtxQueue[1].v >> 4) << ")"
                      << " stq1=(" << gs->m_vtxQueue[1].s << "," << gs->m_vtxQueue[1].t << "," << gs->m_vtxQueue[1].q << ")"
                      << " v2=(" << gs->m_vtxQueue[2].x << "," << gs->m_vtxQueue[2].y << ")"
                      << " uv2=(" << (gs->m_vtxQueue[2].u >> 4) << "," << (gs->m_vtxQueue[2].v >> 4) << ")"
                      << " stq2=(" << gs->m_vtxQueue[2].s << "," << gs->m_vtxQueue[2].t << "," << gs->m_vtxQueue[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(gs->m_vtxQueue[0].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(gs->m_vtxQueue[1].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(gs->m_vtxQueue[2].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((gs->m_prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                      << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_texclut.cou)
                      << " cov=" << gs->m_texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    if (gs->m_hasPreferredDisplaySource && ctx.frame.fbp == gs->m_preferredDisplayDestFbp)
    {
        gs->m_hasPreferredDisplaySource = false;
    }

    // Decode the palette once for this primitive (no-op if not paletted / already
    // valid) so per-pixel texture sampling can index a flat table instead of
    // re-fetching a swizzled CLUT entry from VRAM every pixel.
    if (gs->m_prim.tme)
        ensureClutCache(gs);

    switch (gs->m_prim.type)
    {
    case GS_PRIM_SPRITE:
        drawSprite(gs);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        drawTriangle(gs);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        drawLine(gs);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = gs->m_vtxQueue[0];
        const auto &ctx = gs->activeContext();
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        writePixel(gs, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a);
        break;
    }
    default:
        break;
    }
}

// Set while a run of draws is being handed to the software rasterizer (PS2X_SWOUTLINE), so
// the pixels it actually touches can be blitted back INDIVIDUALLY. A whole-page blit is not
// safe: BT3's CT16 alias passes write into the same words of page 0, so VRAM's RGB legitimately
// differs from the FBO's and uploading all of it imports those aliased writes as visible colour
// (measured: an identity round trip alone costs MAE 11.96 -> 14.26).
bool g_recordDepthOnly = false;
bool g_swDirtyActive = false;
std::vector<uint8_t> g_swDirty;
int g_swDirtyW = 0, g_swDirtyH = 0;
// [swozsnap] pre-bracket VRAM z snapshot: the routed 0x62 outline class z-tests against THIS
// instead of live z, because the routed base pass's own z-writes are what reject the ink
// ((cf): float z interpolation diverges between strip topologies by thousands of ints on
// edge-on triangles). Base/shading classes keep live z, so self-occlusion is preserved.
std::vector<uint32_t> g_swoZSnap;
int g_swoZSnapW = 0, g_swoZSnapH = 0;
bool g_swoZSnapValid = false;
bool g_swoCelZSnap = false;   // set while a routed 0x62 outline draw is rasterizing
bool g_swoDiffOnly = false;   // mode 9: SW writes only the pixels GL's fill rule drops
float *g_umapPtr = nullptr;   // [umap] per-pixel ramp-u dump buffer

void GSRasterizer::writePixel(GS *gs, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool widened)
{
    if (g_swDirtyActive && x >= 0 && y >= 0 && x < g_swDirtyW && y < g_swDirtyH)
        g_swDirty[(size_t)y * g_swDirtyW + x] = 255u;
    const auto &ctx = gs->activeContext();

    // PS2X_SCIFIX=1: the fight ground mesh draws on context 2 whose scissor reads
    // INVERTED garbage (x0>x1) that no writeRegister ever produced — every pixel is
    // rejected here. Real GS treats inverted scissors as empty too, but this state is
    // corruption (uninitialized/raced), not a game value: ignore the scissor for
    // inverted rects so the geometry behind it becomes visible while the corruption
    // source is hunted.
    static const bool s_sciFix = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_SCIFIX"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
    const bool sciInverted = ctx.scissor.x0 > ctx.scissor.x1 || ctx.scissor.y0 > ctx.scissor.y1;
    if (!(s_sciFix && sciInverted))
        if (x < ctx.scissor.x0 || x > ctx.scissor.x1 ||
            y < ctx.scissor.y0 || y > ctx.scissor.y1)
            return;

    const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, a);

    if (!alphaTest.writeFramebuffer)
        return;

    u8* vram = gs->m_vram;

    const u32 fbp  = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw  = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    u32 fmsk = ctx.frame.fbmsk;
    {   // A routed cel draw must NOT write VRAM's scene ALPHA. On the GPU path that byte is
        // protected on every flush by PS2X_BARKEEPA, because it is not our alpha to own -- it
        // is the field BT3's own masked passes rebuild and the outline/shadow composites read
        // back as a PSMT8H index. The software rasterizer writes VRAM directly and so bypasses
        // that protection entirely, repainting the scene alpha with the character's alpha and
        // feeding the mask chain different data. That is what changed the ground, not the blit
        // (whose draw measures as an exact no-op: ratio 1.000, 0.00% of pixels differing).
        extern bool g_swDirtyActive;
        static const bool s_ka = [](){ const char *v = std::getenv("PS2X_SWOKEEPA");
                                       return !(v && v[0] == '0'); }();
        if (s_ka && g_swDirtyActive) fmsk |= 0xFF000000u;
    }
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    // Fast path for the common 32bpp framebuffer formats (CT32/CT24, which share the
    // C32 page layout): compute the byte address ONCE and do a direct read-modify-
    // write, avoiding two swizzled std::function VRAM lookups per pixel.
    const bool fbCT24 = (fpsm == GS_PSM_CT24);
    const bool fbDirect = (fpsm == GS_PSM_CT32) || fbCT24;
    const u32 fbAddr = fbDirect ? GSMem::AddrCT32(fbp, fbw, x, y) : 0u;

    const bool alphaBlendEnabled = gs->m_prim.abe;
    const bool destinationAlpha  = alphaTest.preserveDestinationAlpha;

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = (ctx.frame.fbmsk != 0) || alphaBlendEnabled || destinationAlpha;

    u32 fbrgba = 0;
    if (frmw)
    {
        if (fbDirect)
        {
            std::memcpy(&fbrgba, vram + fbAddr, sizeof(u32));
            if (fbCT24)
                fbrgba &= 0x00FFFFFFu; // ReadCT24 masks off the alpha byte
        }
        else
        {
            fbrgba = gs->ReadVram(fpsm, fbp, fbw, x, y);
            if (bitsPerPixel(fpsm) == 16)
            {
                fbrgba = Rgba5551ToRgba8888(fbrgba);
            }
        }
    }

    // PS2X_ZSAT (default ON): z values above the game's legitimate range are WRAPPED NEGATIVES
    // (this projection maps [near..far] to ~[1..5.44M]; a vertex behind the near plane converts
    // to a negative int and packs to ~16.7M). Real hardware never sees these (the game keeps z
    // positive); ours would write a nearest-z WALL that z-kills the whole stage (invisible
    // grass/rocks). Saturate them to 0 = farthest. PS2X_ZSAT=0 disables.
    {
        static const bool s_zsat = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_ZSAT"); return s_env; }(); return !(v && v[0] == '0'); }();
        if (s_zsat && z > 12000000u)
            z = 0u;
    }

    // ZTE (bit 16) enables the depth test; ZTST (bits 17-18) is the comparison.
    // When ZTE=0 the depth test is disabled (always pass). ZTST=0 (NEVER) is a
    // PROHIBITED setting on the GS, so a TEST reg left at 0 must not cull every
    // pixel — treat both cases as "pass" (this is why untested font sprites, which
    // have TEST=0, were being fully culled).
    bool ztest_enabled = ((ctx.test >> 16) & 1u) != 0u;
    uint ztest_method = (ctx.test >> 17) & 3;

    bool zpass = false;
    {   // PS2X_SWONOZ=1: drop the depth test for draws currently routed to the software
        // rasterizer. Their Z comes from a GPU depth buffer round-tripped through VRAM, and
        // near-coplanar geometry (the character's ground shadow) is exactly what a small
        // error there rejects. If this recovers the shadow, the round trip is the problem.
        extern bool g_swDirtyActive;
        static const bool s_nz = [](){ const char *v = std::getenv("PS2X_SWONOZ");
                                       return v && v[0] && v[0] != '0'; }();
        if (s_nz && g_swDirtyActive) { ztest_enabled = false; }
    }
    // [swozeps] PS2X_SWOZEPS=<n>: GEQUAL tie tolerance for draws routed to the software
    // rasterizer (g_swDirtyActive). BT3's cel/outline passes re-draw the SAME mesh as the base
    // pass and pass GEQUAL on console by exact fixed-point z equality; our float interpolation
    // differs per strip topology, so ties break and ~40% of routed pixels are rejected within a
    // <=4096-int margin ([swozstat]) while the >4096 rejects are legitimate hidden surfaces.
    // The epsilon passes the former and keeps the latter -- same spirit as kEdgeEpsilon for
    // coverage. Scoped strictly to routed draws; 0 (default) = exact GS compare everywhere.
    uint32_t zeps = 0u;
    bool widenedFree = false;
    {
        extern bool g_swDirtyActive;
        static const uint32_t s_zeps = [](){ const char *v = std::getenv("PS2X_SWOZEPS");
                                             return v && v[0] ? (uint32_t)std::strtoul(v, nullptr, 0) : 0u; }();
        if (s_zeps && g_swDirtyActive) zeps = s_zeps;
        // PS2X_WIDENZ=1 (default OFF -- MEASURED WORSE, rim -6.1 / MAE 17.0): widened
        // pixels of routed draws skip z-test + z-write. Kept only as an A/B: its failure
        // proves the INK sits on INTERIOR sliver pixels rejected against the body's own z,
        // not on the widened ring.
        static const bool s_wz = [](){ const char *v = std::getenv("PS2X_WIDENZ");
                                       return v && v[0] && v[0] != '0'; }();
        widenedFree = s_wz && widened && g_swDirtyActive;
    }
    // PS2X_WIDENZW=1: the SPLIT variant -- widened pixels skip only the z-WRITE (so the base
    // pass cannot poison the field with fabricated near z at contour-adjacent pixels) but KEEP
    // the z-test (background passes at the contour = the ink; interior widened pixels are
    // correctly rejected by the adjacent face). WIDENZ bundled both exemptions and failed.
    bool widenedNoZW = false;
    {
        extern bool g_swDirtyActive;
        static const bool s_wzw = [](){ const char *v = std::getenv("PS2X_WIDENZW");
                                        return v && v[0] && v[0] != '0'; }();
        widenedNoZW = s_wzw && widened && g_swDirtyActive;
    }
    if (widenedFree)
        ztest_enabled = false;
    if (!ztest_enabled)
    {
        zpass = true;
    }
    else
    switch (ztest_method)
    {
    case 0:
        zpass = true; // NEVER is prohibited on real hardware; do not cull.
        break;
    case 1:
        zpass = true;
        break;
    case 2:
    case 3:
    {
        uint32_t zb;
        extern std::vector<uint32_t> g_swoZSnap; extern int g_swoZSnapW, g_swoZSnapH;
        extern bool g_swoZSnapValid, g_swoCelZSnap;
        static const bool s_zsnap = [](){ const char *v = std::getenv("PS2X_SWOZSNAP");
                                          return !(v && v[0] == '0'); }();
        if (s_zsnap && g_swoCelZSnap && g_swoZSnapValid &&
            x >= 0 && y >= 0 && x < g_swoZSnapW && y < g_swoZSnapH)
            zb = g_swoZSnap[(size_t)y * g_swoZSnapW + x];
        else
            zb = gs->ReadVram(zpsm, zbp, fbw, x, y);
        zpass = (ztest_method == 2) ? ((uint32_t)z + zeps >= zb)
                                    : ((uint32_t)z + zeps > zb);
        break;
    }
    }

    {   // PS2X_SWOZSTAT=1: z-test outcome for the draws currently routed to software
        // (g_swDirtyActive marks the bracket). The character renders but its ground shadow
        // does not, and the shadow is near-coplanar with the ground, so a slightly wrong Z
        // in VRAM would reject exactly that.
        extern bool g_swDirtyActive;
        static const bool s_zs = [](){ const char *v = std::getenv("PS2X_SWOZSTAT");
                                       return v && v[0] && v[0] != '0'; }();
        if (s_zs && g_swDirtyActive)
        {
            static unsigned long nPass = 0, nFail = 0; static double sumZ = 0.0, sumB = 0.0;
            static unsigned long m16 = 0, m256 = 0, m4k = 0, mBig = 0;
            const uint32_t zb = gs->ReadVram(zpsm, zbp, fbw, x, y);
            if (zpass) ++nPass; else {
                ++nFail;
                const uint32_t margin = zb - (uint32_t)z;   // reject => zb > z
                if (margin <= 16u) ++m16; else if (margin <= 256u) ++m256;
                else if (margin <= 4096u) ++m4k; else ++mBig;
            }
            sumZ += (double)z; sumB += (double)zb;
            const unsigned long n = nPass + nFail;
            if ((n % 40000ul) == 0ul)
                std::fprintf(stderr, "[swozstat] software cel pixels: pass %lu fail %lu (%.1f%% rejected)"
                                     "   mean z=%.0f  mean zbuffer=%.0f  zte=%d ztst=%u"
                                     "  | reject margin: <=16:%lu <=256:%lu <=4096:%lu >4096:%lu\n",
                             nPass, nFail, 100.0 * (double)nFail / (double)n,
                             sumZ / n, sumB / n, (int)ztest_enabled, ztest_method,
                             m16, m256, m4k, mBig);
            if ((n % 40000ul) == 0ul)
            {   extern uint32_t g_zwbBp, g_zwbPsm, g_zwbBw; extern double g_zwbZMax;
                std::fprintf(stderr, "[swozstat]   SW reads zbp=%u zpsm=%02x fbw=%u |"
                                     " writeback used g_zwbBp=%u g_zwbPsm=%02x g_zwbBw=%u zMax=%.0f\n",
                             zbp, zpsm, fbw, g_zwbBp, g_zwbPsm, g_zwbBw, g_zwbZMax); }
        }
    }
    // PS2X_ZKILL: why doesn't the grass ground show? Tally z-test outcomes for the stage
    // textures (tbp0 13400-14000 = the grass/rock CLUT family) and sample z vs buffer z.
    {
        static const bool s_zk = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_ZKILL"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        // NOTE: no !fst gate — the terrain layer draws FST=1 (UV) and was invisible to
        // every fst-gated probe for a whole session. Track everything textured.
        // 1/8 pixel sampling (see PIXSTAT) — the per-pixel mutex was the fps hit.
        if (s_zk && gs->m_prim.tme && (((x ^ (y << 1)) & 7) == 0))
        {
            // Per-texture pass/kill tally: separates stage objects from characters without
            // guessing tbp ranges. Killed-heavy textures = the invisible ground/rocks/trees.
            static std::mutex s_zm;
            static std::map<uint32_t, std::pair<uint64_t,uint64_t>> s_t; // tbp0 -> {pass, kill}
            static uint64_t s_n = 0;
            std::lock_guard<std::mutex> lk(s_zm);
            auto &e = s_t[ctx.tex0.tbp0];
            if (zpass) ++e.first; else ++e.second;
            // z samples per texture: wrong-projection (z systematically below buffer) vs
            // corrupt-buffer (buffer z absurdly high) distinguish here.
            static std::map<uint32_t, std::array<uint32_t,2>> s_z; // tbp0 -> {last z, last bufZ}
            if (!zpass) s_z[ctx.tex0.tbp0] = {static_cast<uint32_t>(z), static_cast<uint32_t>(gs->ReadVram(zpsm, zbp, fbw, x, y))};
            static std::map<uint32_t, std::array<int,2>> s_xy; // tbp0 -> kill x,y sample
            if (!zpass) s_xy[ctx.tex0.tbp0] = {(int)x, (int)y};
            static std::map<uint32_t, std::array<uint32_t,2>> &s_zr = s_z;
            if ((++s_n % 2000000u) == 1u && s_n > 1)
            {
                std::fprintf(stderr, "[zkill] per-tbp0 (pass/kill):");
                for (auto &kv : s_t)
                {
                    const uint64_t tot = kv.second.first + kv.second.second;
                    if (tot < 300) continue;
                    const auto zi = s_zr.find(kv.first);
                    std::fprintf(stderr, " %u=%llu/%llu(%.0f%%k", kv.first,
                                 (unsigned long long)kv.second.first, (unsigned long long)kv.second.second,
                                 100.0 * kv.second.second / tot);
                    if (zi != s_zr.end()) std::fprintf(stderr, ",z%u vs %u", zi->second[0], zi->second[1]);
                    { auto xi = s_xy.find(kv.first); if (xi != s_xy.end()) std::fprintf(stderr, "@%d,%d", xi->second[0], xi->second[1]); }
                    std::fprintf(stderr, ")");
                }
                std::fprintf(stderr, "\n");
            }
        }
    }

    if (!zpass)
    {
        return;
    }

    const u8 srcR = r;
    const u8 srcG = g;
    const u8 srcB = b;

    if (gs->m_prim.abe)
    {
        uint8_t dr = fbrgba & 0xFF;
        uint8_t dg = (fbrgba >> 8) & 0xFF;
        uint8_t db = (fbrgba >> 16) & 0xFF;
        uint8_t da = (fbrgba >> 24) & 0xFF;

        // PABE disables alpha blending when the source alpha MSB is clear.
        if (!(gs->m_pabe && (a & 0x80u) == 0u))
        {
            uint64_t alphaReg = ctx.alpha;
            uint8_t asel = alphaReg & 3;
            uint8_t bsel = (alphaReg >> 2) & 3;
            uint8_t csel = (alphaReg >> 4) & 3;
            uint8_t dsel = (alphaReg >> 6) & 3;
            uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

            auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
            {
                if (sel == 0)
                    return cs;
                if (sel == 1)
                    return cd;
                return 0;
            };
            int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                       : fix;

            r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
            g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
            b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
        }
        else
        {
            r = srcR;
            g = srcG;
            b = srcB;
        }
    }

    {   // PS2X_REBSTAT=1: does the software alpha-rebuild actually cover the full CT16 view?
        // Its draws span y 0..896 with SCISSOR [0..511]x[0..895], so if anything clamps to the
        // 448-row framebuffer we lose half of every strip -- which would explain the field
        // ending 97.4% zero despite the CT16 mapping covering every pixel's alpha byte.
        static const bool s_rs = [](){ const char *v = std::getenv("PS2X_REBSTAT");
                                       return v && v[0] && v[0] != '0'; }();
        if (s_rs && ctx.frame.psm == 0x02u && ctx.frame.fbmsk == 0x00003fffu)
        {
            static long n = 0; static int ymin = 1 << 30, ymax = -1, xmin = 1 << 30, xmax = -1;
            ++n; if (y < ymin) ymin = y; if (y > ymax) ymax = y;
            if (x < xmin) xmin = x; if (x > xmax) xmax = x;
            if ((n % 5000L) == 1L)
                std::fprintf(stderr, "[rebstat] rebuild wrote %ld px  x %d..%d  y %d..%d\n",
                             n, xmin, xmax, ymin, ymax);
        }
    }
    u32 fbmask = ctx.frame.fbmsk;
    bool zmask = ctx.zbuf.zmask;

    if (!alphaTest.preserveDestinationAlpha &&
        (ctx.fba & 0x1ull) != 0ull &&
        ctx.frame.psm != GS_PSM_CT24)
    {
        a = static_cast<uint8_t>(a | 0x80u);
    }

    u32 pixel = pack32(r, g, b, a);

    // FBMSK is applied in the DESTINATION's own pixel format. For PSMCT16/CT16S that is the
    // 16-bit pixel, not the expanded 32-bit RGBA form. BT3's outline edge-detect writes fbp336
    // with fbmsk=0xffff00ff, whose low half leaves bits 8-15 writable -- and bit 15 IS the CT16
    // alpha. Masking in 32-bit space instead sourced alpha from bits 24-31, which that mask
    // protects, so the alpha bit was never set: fbp336 read back 99.1% alpha-zero and the
    // outline readback (96 draws/frame) wrote zeros into the scene instead of ink.
    // PS2X_FBMSK32=1: apply FBMSK in the EXPANDED 32-bit RGBA space even for 16-bit targets.
    // 0x00003fff then writes G[6:7] + all of B + all of A, which for the high-half CT16 pixel
    // reaches CT32 alpha bits 2-7 -- 6 bits, 64 levels -- instead of the 2 bits (4 levels) the
    // 16-bit-space reading gives. Console's field has 256 levels, so 4 is far too coarse for
    // the edge detect to make contours from.
    static const bool s_fbmsk32 = [](){ const char *v = std::getenv("PS2X_FBMSK32");
                                        return v && v[0] && v[0] != '0'; }();
    bool packed16 = false;
    if (fbmask != 0 && bitsPerPixel(fpsm) == 16 && !s_fbmsk32)
    {
        const u16 m16 = static_cast<u16>(fbmask & 0xFFFFu);
        const u16 s16 = Rgba8888ToRgba5551(pixel);
        const u16 d16 = Rgba8888ToRgba5551(fbrgba);
        pixel = static_cast<u32>(static_cast<u16>((s16 & static_cast<u16>(~m16)) | (d16 & m16)));
        packed16 = true;
        {   // PS2X_WPROBE=<fbp>: the ACTUAL runtime values of the CT16 masked write. Every
            // inference about why fbp336's alpha bit never lands has checked out on paper, so
            // log the numbers instead of reasoning about them.
            static const int s_wpr = [](){ const char *v = std::getenv("PS2X_WPROBE");
                                           return v && v[0] ? std::atoi(v) : -1; }();
            if (s_wpr >= 0 && (int)ctx.frame.fbp == s_wpr)
            {
                static int n = 0;
                if (n++ < 200000 && (n % 5000) == 1)
                    std::fprintf(stderr, "[wprobe] fbp%u fbmsk=%08x m16=%04x | a=%u (in rgba %08x) "
                                         "s16=%04x d16=%04x -> out=%04x  bit15 s=%d d=%d out=%d\n",
                                 ctx.frame.fbp, fbmask, m16, (unsigned)a, pack32(r, g, b, a),
                                 s16, d16, (unsigned)(pixel & 0xFFFFu),
                                 (s16 >> 15) & 1, (d16 >> 15) & 1, (int)((pixel >> 15) & 1));
            }
        }
    }
    else if (fbmask != 0)
    {
        pixel = (pixel & ~fbmask) | (fbrgba & fbmask);
    }

    if (alphaTest.preserveDestinationAlpha)
    {
        pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
    }
    
    // DIAGNOSTIC: are the RGBA values written to the framebuffer bright or dim?
    {
        static const bool s_wp = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_WRITE_PROBE"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_wp)
        {
            static std::atomic<uint64_t> s_n{0}, s_sumMax{0}, s_bright{0};
            uint8_t mx = std::max({(uint8_t)(pixel&0xff),(uint8_t)((pixel>>8)&0xff),(uint8_t)((pixel>>16)&0xff)});
            uint64_t n = s_n.fetch_add(1) + 1;
            s_sumMax.fetch_add(mx);
            if (mx > 64) s_bright.fetch_add(1);
            if ((n % 2000000ull) == 1ull)
                std::cerr << "[write] fpsm=0x" << std::hex << (int)fpsm << std::dec
                          << " fbp=" << fbp << " abe=" << (int)alphaBlendEnabled
                          << " | avgMaxChan=" << (s_sumMax.load()/n) << " brightFrac=" << (s_bright.load()*100/n) << "%"
                          << " thisPixel=0x" << std::hex << pixel << std::dec << std::endl;
        }
        // WHERE does text land? Per (fbp) bounding box of text-sprite pixel writes.
        static const bool s_tl = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_TEXTLOC"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_tl && gs->m_prim.tme && ctx.tex0.psm == GS_PSM_T4)
        {
            static std::mutex m; static std::map<uint64_t, std::array<int,5>> box; // (tbp0<<12|fbp) -> {minx,miny,maxx,maxy,count}
            std::lock_guard<std::mutex> lk(m);
            uint64_t key = ((uint64_t)ctx.tex0.tbp0 << 20) | (fbp & 0xFFF);
            auto &b = box[key];
            if (b[4]==0){ b[0]=b[2]=x; b[1]=b[3]=y; }
            b[0]=std::min(b[0],x); b[1]=std::min(b[1],y); b[2]=std::max(b[2],x); b[3]=std::max(b[3],y); b[4]++;
            static std::atomic<uint64_t> c{0};
            if ((c.fetch_add(1)%200000ull)==1ull){
                std::cerr << "[textloc]";
                for (auto &kv:box) std::cerr << " src0x"<<std::hex<<(kv.first>>20)<<std::dec<<"@fbp"<<(kv.first&0xFFF)<<"=["<<kv.second[0]<<","<<kv.second[1]<<".."<<kv.second[2]<<","<<kv.second[3]<<" n="<<kv.second[4]<<"]";
                std::cerr << " (visible frame ~512x448)" << std::endl;
            }
        }
    }

    // PS2X_PIXSTAT: per-tbp0 accounting of 3D textured pixels that SURVIVE scissor+atest+ztest
    // and reach the framebuffer write. Pairs with PS2X_ZKILL (which tallies the z-killed side).
    // 'inv' = written but RGB-identical to the dest (draw succeeded yet invisible — e.g. an
    // additive blend contributing ~0 because As/texel alpha decodes to 0). bbox = where on
    // screen the writes land. If a ground-family tbp shows writes>0 with low inv%, the ground
    // IS rendering and gets overdrawn later instead.
    {
        static const bool s_ps = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_PIXSTAT"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        // No !fst gate — see ZKILL note (terrain layer is FST=1). 1/8 pixel sampling keeps
        // the per-pixel mutex cost bearable (ratios/bboxes unaffected). avgA = mean SOURCE
        // fragment alpha at write time (pre-blend `a`): ~0 for a tbp whose writes are inv%
        // ~100 proves the As=0 blend-no-op story vs "redrawing identical content".
        if (s_ps && gs->m_prim.tme && (((x ^ (y << 1)) & 7) == 0))
        {
            static std::mutex s_pm;
            struct PixTally { uint64_t writes = 0, inv = 0, aSum = 0; int x0 = 1<<30, y0 = 1<<30, x1 = -1, y1 = -1; };
            static std::map<uint32_t, PixTally> s_t;
            static uint64_t s_n = 0;
            const bool invisible = frmw && ((pixel & 0x00FFFFFFu) == (fbrgba & 0x00FFFFFFu));
            std::lock_guard<std::mutex> lk(s_pm);
            auto &e = s_t[ctx.tex0.tbp0];
            ++e.writes; if (invisible) ++e.inv;
            e.aSum += a;
            e.x0 = std::min(e.x0, x); e.y0 = std::min(e.y0, y);
            e.x1 = std::max(e.x1, x); e.y1 = std::max(e.y1, y);
            if ((++s_n % 250000u) == 1u && s_n > 1)
            {
                std::fprintf(stderr, "[pixstat] per-tbp0 writes(inv%%,avgA)[bbox]:");
                for (auto &kv : s_t)
                {
                    if (kv.second.writes < 100) continue;
                    std::fprintf(stderr, " %u=%llu(%.0f%%,a%llu)[%d,%d..%d,%d]", kv.first,
                                 (unsigned long long)kv.second.writes,
                                 100.0 * kv.second.inv / kv.second.writes,
                                 (unsigned long long)(kv.second.aSum / kv.second.writes),
                                 kv.second.x0, kv.second.y0, kv.second.x1, kv.second.y1);
                }
                std::fprintf(stderr, "\n");
            }
        }
    }

    if (fbDirect)
    {
        // Direct RMW at the precomputed address (no swizzle / std::function).
        if (fbCT24)
        {
            u32 old;
            std::memcpy(&old, vram + fbAddr, sizeof(u32));
            const u32 w = (old & 0xFF000000u) | (pixel & 0x00FFFFFFu); // preserve alpha byte
            std::memcpy(vram + fbAddr, &w, sizeof(u32));
        }
        else
        {
            std::memcpy(vram + fbAddr, &pixel, sizeof(u32));
        }
    }
    else
    {
        // format conversion
        if (bitsPerPixel(fpsm) == 16 && !packed16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }
        gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
        {   // [wverify] PS2X_WVERIFY=1: read-after-write for the scene-alpha rebuild. The pass
            // samples 26.6% bit15 texels and writes ~410k px, yet the dest census reads 0%
            // alpha -- so verify the byte actually written at this (x,y), through the same
            // ReadVram the census path uses.
            static const bool s_wv = [](){ const char *v = std::getenv("PS2X_WVERIFY");
                                           return v && v[0] && v[0] != '0'; }();
            if (s_wv && ctx.frame.fbmsk == 0x00003fffu && fpsm == 0x02u)
            {
                static std::atomic<uint64_t> nW{0}, nA{0}, nMatch{0}, nRbA{0};
                const uint64_t n = nW.fetch_add(1) + 1;
                const bool aw = (pixel & 0x8000u) != 0u;
                if (aw) nA.fetch_add(1);
                const uint32_t rb = gs->ReadVram(fpsm, fbp, fbw, x, y);
                if ((rb & 0xFFFFu) == (pixel & 0xFFFFu)) nMatch.fetch_add(1);
                if (rb & 0x8000u) nRbA.fetch_add(1);
                if ((n % 100000ull) == 1ull && n > 1)
                    std::fprintf(stderr, "[wverify] %llu rebuild writes: bit15-written %.2f%%  readback==written %.2f%%"
                                         "  readback-bit15 %.2f%%  last w=%04x rb=%04x @(%d,%d)\n",
                                 (unsigned long long)n, 100.0 * (double)nA.load() / (double)n,
                                 100.0 * (double)nMatch.load() / (double)n,
                                 100.0 * (double)nRbA.load() / (double)n,
                                 pixel & 0xFFFFu, rb & 0xFFFFu, x, y);
            }
        }
    }

    if (widenedFree || widenedNoZW) zmask = true;   // widened pixels write no z
    {   // PS2X_SWONOZW=1: NO z-writes for the whole routed bracket -- the cel passes then
        // z-test against the PRE-BRACKET buffer (GL scene z, which the character beats).
        // Discriminator: rim ~-17.9 here => our routed base pass's z-writes at silhouette
        // pixels are what rejects the ink; rim still bad => the scene z itself rejects it.
        extern bool g_swDirtyActive;
        static const bool s_nzw = [](){ const char *v = std::getenv("PS2X_SWONOZW");
                                        return v && v[0] && v[0] != '0'; }();
        if (s_nzw && g_swDirtyActive) zmask = true;
    }
    if (!zmask)
    {
        // PS2X_ZKILL: who writes NEAR-MAX z (>16M)? Those walls are what z-kills the stage.
        {
            static const bool s_zk2 = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_ZKILL"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            if (s_zk2 && z > 16000000u)
            {
                static std::atomic<uint64_t> s_bw{0};
                if ((s_bw.fetch_add(1) % 500000u) == 1u)
                    std::fprintf(stderr, "[zbigwrite] z=%u @%d,%d prim=%u tme=%d fst=%d tbp0=%u fbp=%u zbp=%u\n",
                                 z, (int)x, (int)y, gs->m_prim.type, gs->m_prim.tme?1:0, gs->m_prim.fst?1:0,
                                 gs->m_prim.tme ? ctx.tex0.tbp0 : 0u, fbp, zbp);
            }
        }
        gs->WriteVram(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSRasterizer::lookupCLUT(GS *gs, uint8_t index, uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
{
    return lookupCLUTFrom(gs->m_vram, gs->m_texa, gs->m_texclut, index, cbp, cpsm, csm, csa, sourcePsm);
}
uint32_t GSRasterizer::lookupCLUTFrom(uint8_t *vram, const GSTexaReg &texa, const GSTexClutReg &texclut, uint8_t index, uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
{
    const uint32_t clutIndex = resolveClutIndex(index, csm, csa, sourcePsm);
    const uint32_t clutWidth = (texclut.cbw != 0u) ? static_cast<uint32_t>(texclut.cbw) : 1u;
    const uint32_t clutX = static_cast<uint32_t>(texclut.cou) + (clutIndex & 0x0Fu);
    const uint32_t clutY = static_cast<uint32_t>(texclut.cov) + (clutIndex >> 4);


    {   // PS2X_CLUTDUMP=<cbp>: one-shot dump of the palette AS THIS CODE PATH RESOLVES IT, so it
        // can be diffed against the palette read straight out of the .gs. The fbp336 outline
        // generator fetched entry rotl5(i,2) instead of entry i for every level measured
        // (1->4, 3->12, 5->20, 7->28, 11->13, 13->21, 23->30), which collapses the ramp and is
        // why the edge map came out ~14x too sparse.
        static const int s_cd = [](){ const char *v = std::getenv("PS2X_CLUTDUMP");
                                      return (v && v[0]) ? std::atoi(v) : -1; }();
        static bool s_done = false;
        if (s_cd >= 0 && !s_done && (uint32_t)s_cd == cbp && cpsm == GS_PSM_CT32)
        {
            s_done = true;
            const char *dr = std::getenv("PS2X_GS_REPLAY_OUT");
            char q[256]; std::snprintf(q, sizeof(q), "%s/clutdump_%u.raw", dr ? dr : ".", cbp);
            if (FILE *qf = std::fopen(q, "wb"))
            {
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    const uint32_t ci = resolveClutIndex((uint8_t)i, csm, csa, sourcePsm);
                    const uint32_t cx = (uint32_t)texclut.cou + (ci & 0x0Fu);
                    const uint32_t cy = (uint32_t)texclut.cov + (ci >> 4);
                    uint32_t val = GSMem::ReadCT32(vram, cbp, clutWidth, cx, cy);
                    std::fwrite(&val, 4, 1, qf);
                }
                std::fclose(qf);
            }
            std::fprintf(stderr, "[clutdump] cbp=%u csm=%u csa=%u srcpsm=%u clutWidth=%u cou=%u cov=%u\n",
                         cbp, (unsigned)csm, (unsigned)csa, (unsigned)sourcePsm, clutWidth,
                         (unsigned)texclut.cou, (unsigned)texclut.cov);
        }
    }
    switch (cpsm)
    {
    case GS_PSM_CT32:
        return applyTexa(texa, cpsm, GSMem::ReadCT32(vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT24:
        return applyTexa(texa, cpsm, GSMem::ReadCT24(vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT16:
        return applyTexa(texa, cpsm, Rgba5551ToRgba8888(GSMem::ReadCT16(vram, cbp, clutWidth, clutX, clutY)));
    case GS_PSM_CT16S:
        return applyTexa(texa, cpsm, Rgba5551ToRgba8888(GSMem::ReadCT16S(vram, cbp, clutWidth, clutX, clutY)));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

// [deferdec] palette build shared by ensureClutCache (guest, cached by key) and decodeDeferred (GL thread).
int GSRasterizer::fillClutFrom(uint32_t *out, uint8_t *vram, const GSTexaReg &texa, const GSTexClutReg &texclut, const GSTex0Reg &tex0)
{
    const GSTex0Reg &tex = tex0;
    int count;
    switch (tex.psm)
    {
    case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: count = 16; break;
    case GS_PSM_T8: case GS_PSM_T8H: count = 256; break;
    default: return 0;
    }
    for (int i = 0; i < count; ++i)
        out[i] = lookupCLUTFrom(vram, texa, texclut, static_cast<uint8_t>(i), tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
    {   // [edgewrap] PS2X_EDGEWRAP=1: BT3's edge generator subtracts the ramp-decoded mask from
        // itself shifted one column (blend 0x62, COLCLAMP=0). The GS WRAPS, so any index change
        // yields a nonzero G; GL clamps at 0 and only keeps k[x] > k[x+1] -- half the contour.
        // Emulate the wrap with data: give the edge palette (CBP 16012, R=G=8k) an inverted B
        // (255-R). The clamped pass then lands k[x]>k[x+1] in G and k[x]<k[x+1] in B, and the
        // stamps' TEXA/AEM test (RGB != 0) sees both. The renderer unmasks B for that pass.
        static const bool s_ew = [](){ const char *v = std::getenv("PS2X_EDGEWRAP"); return v && v[0] && v[0] != '0'; }();
        if (s_ew && tex.cbp == 16012u)
            for (int i = 0; i < count; ++i)
            { const uint32_t e = out[i]; out[i] = (e & 0xFF00FFFFu) | ((255u - (e & 0xFFu)) << 16); }
    }
    return count;
}

uint32_t g_gvGroupOrdinal = 0;   // [groupviz] palette-upload ordinal within the current frame

void GSRasterizer::ensureClutCache(GS *gs)
{
    const auto &tex = gs->activeContext().tex0;
    int count;
    switch (tex.psm)
    {
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        count = 16;
        break;
    case GS_PSM_T8:
    case GS_PSM_T8H:
        count = 256;
        break;
    default:
        return; // not a paletted texture -> nothing to cache
    }

    // Key over everything the decoded palette depends on, plus a texture-upload
    // generation so a palette overwrite in VRAM forces a rebuild.
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t val) { h = (h ^ val) * 16777619u; };
    mix(tex.cbp); mix(tex.cpsm); mix(tex.csm); mix(tex.csa); mix(tex.psm);
    mix(gs->m_texclut.cbw); mix(gs->m_texclut.cou); mix(gs->m_texclut.cov);
    mix(gs->m_texa.ta0); mix(gs->m_texa.aem ? 1u : 0u); mix(gs->m_texa.ta1);
    // [clutpagegen] key on the palette page(s)' own upload generation, not the global m_texUploadGen. CSM2 palettes
    // (cbw/cou/cov addressing) keep the global generation.
    const uint32_t cpg = (tex.cbp >> 5) & 511u;
    const uint32_t pgen = (tex.csm != 0u) ? gs->m_texUploadGen
                        : (gs->m_pageUploadGen[cpg] * 0x9E3779B1u) ^ (gs->m_pageUploadGen[(cpg + 1u) & 511u] * 0x85EBCA6Bu);
    const uint64_t key = (static_cast<uint64_t>(pgen) << 32) | h;
    if (key == gs->m_clutCacheKey)
        return; // cache already valid for this palette state

    // [clutmulti] 2026-09-03: a 64-entry, 2-way cache of built palettes keyed by the same key. Consecutive prims alternate
    // palettes, so the single-entry cache refilled ~50k times/s from swizzled VRAM. invalidateClutCache() leaves the
    // ~0 sentinel in m_clutCacheKey -> drop every entry (a writeback also bumps the page gens in the key). PS2X_CLUTMULTI=0 disables.
    struct ClutMulti { uint64_t key; uint64_t h16, h256; uint32_t tbl[256]; };
    static ClutMulti s_cm[64] = {};
    static const bool s_cmOn = [](){ const char *v = std::getenv("PS2X_CLUTMULTI"); return !(v && v[0] == '0'); }();
    const unsigned cmSlot = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 58) & 62u;   // even slot; +1 = its 2-way partner
    if (s_cmOn)
    {
        if (gs->m_clutCacheKey == ~0ull) for (auto &e : s_cm) e.key = 0ull;
        for (unsigned w = 0; w < 2; ++w)
            if (s_cm[cmSlot + w].key == key)
            {
                std::memcpy(gs->m_clutCache, s_cm[cmSlot + w].tbl, sizeof(gs->m_clutCache));
                gs->m_clutCacheKey = key;
                gs->m_clutCacheHash16 = s_cm[cmSlot + w].h16; gs->m_clutCacheHash256 = s_cm[cmSlot + w].h256;   // [cluthash] as if rebuilt
                return;
            }
    }
    {   // [clutcensus] PS2X_DECCENSUS=1: how often the single-entry CLUT cache refills, and how many distinct keys
        // it cycles through -- a refill is 256 swizzled VRAM reads (ReadCT32 + lookupCLUT ~4% of the guest thread)
        static const bool s_cc = [](){ const char *v = std::getenv("PS2X_DECCENSUS"); return v && v[0] && v[0] != '0'; }();
        if (s_cc)
        {
            static unsigned long s_n = 0; static std::unordered_set<uint64_t> s_keys; static auto s_t = std::chrono::steady_clock::now();
            ++s_n; s_keys.insert(key);
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - s_t).count() >= 2.0)
            { std::fprintf(stderr, "[clutcensus] fills=%lu in 2 s, distinct keys=%zu\n", s_n, s_keys.size()); s_n = 0; s_keys.clear(); s_t = now; }
        }
    }
    fillClutFrom(gs->m_clutCache, gs->m_vram, gs->m_texa, gs->m_texclut, tex);   // [deferdec] shared with the GL-thread decode
    {   // [groupviz] PS2X_GROUPVIZ=<cbp>: replace this palette with a solid ID color per
        // upload ordinal -> the rendered frame maps lighting groups to colors.
        extern uint32_t g_gvGroupOrdinal;
        static const uint32_t s_gv = [](){ const char *v = std::getenv("PS2X_GROUPVIZ");
                                           return v && v[0] ? (uint32_t)std::atoi(v) : 0u; }();
        if (s_gv && tex.cbp == s_gv)
        {
            // v2: derive the ID color from the palette's ACTUAL MEAN (light level), not the
            // upload ordinal -- comparable across streams whose slot counts differ.
            unsigned long sum = 0;
            for (int i = 0; i < 256; ++i)
            { const uint32_t e = gs->m_clutCache[i]; sum += (e & 0xFF) + ((e >> 8) & 0xFF) + ((e >> 16) & 0xFF); }
            const int mean = (int)(sum / 768u);              // ~45..140 for the terrain groups
            const int band = std::min(11, std::max(0, (mean - 45) / 8));   // 12 bands x 8 levels
            static const uint32_t pal[12] = {0xFF2020C0u,0xFF2060FFu,0xFF20C0FFu,0xFF20FFC0u,
                                             0xFF20FF40u,0xFF80FF20u,0xFFE0FF20u,0xFFFFC020u,
                                             0xFFFF8020u,0xFFFF4020u,0xFFFF20A0u,0xFFFF20FFu};
            const uint32_t col = pal[band] & 0x80FFFFFFu;
            for (int i = 0; i < 256; ++i) gs->m_clutCache[i] = col;
        }
    }
    gs->m_clutCacheKey = key;
    {   // [cluthash] hash the rebuilt palette once (was: 256 serial FNV mixes per textured draw = 8% of the guest thread)
        uint64_t hh = 1469598103934665603ull;
        for (int i = 0; i < 16; ++i) hh = (hh ^ (uint64_t)gs->m_clutCache[i]) * 1099511628211ull;
        gs->m_clutCacheHash16 = hh;
        for (int i = 16; i < 256; ++i) hh = (hh ^ (uint64_t)gs->m_clutCache[i]) * 1099511628211ull;
        gs->m_clutCacheHash256 = hh;
    }
    if (s_cmOn)
    {   // [clutmulti] store the built palette + its hashes (after [groupviz]) in the older of the two ways
        static uint32_t s_cmTick = 0; static uint32_t s_cmAge[64] = {};
        unsigned w = (s_cmAge[cmSlot] <= s_cmAge[cmSlot + 1]) ? 0u : 1u;
        if (s_cm[cmSlot].key == 0ull) w = 0u; else if (s_cm[cmSlot + 1].key == 0ull) w = 1u;
        ClutMulti &e = s_cm[cmSlot + w];
        e.key = key; e.h16 = gs->m_clutCacheHash16; e.h256 = gs->m_clutCacheHash256;
        std::memcpy(e.tbl, gs->m_clutCache, sizeof(gs->m_clutCache)); s_cmAge[cmSlot + w] = ++s_cmTick;
    }
    {   // PS2X_CLUTDUMP=<cbp>: print the whole decoded palette once, to diff against a
        // console-derived palette (index -> colour read straight off a PCSX2 texture dump).
        static const int s_cd = [](){ const char *v = std::getenv("PS2X_CLUTDUMP");
                                      return v && v[0] ? std::atoi(v) : -1; }();
        if (s_cd >= 0 && (int)tex.cbp == s_cd)
        {
            static int done = 0;
            if (done++ < 1)
            {
                std::fprintf(stderr, "[clutdump] cbp=%u cpsm=%u csm=%u csa=%u srcpsm=%u count=%d\n",
                             tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm, count);
                for (int i = 0; i < count; ++i)
                {
                    const uint32_t v = gs->m_clutCache[i];
                    std::fprintf(stderr, "%u:%u,%u,%u,%u%s", i, v & 0xFFu, (v >> 8) & 0xFFu,
                                 (v >> 16) & 0xFFu, (v >> 24) & 0xFFu, ((i & 7) == 7) ? "\n" : "  ");
                }
                std::fprintf(stderr, "\n");
            }
        }
    }

    {   // [vramdump] PS2X_VRAMDUMP="<startBlock>,<bytes>": write a raw VRAM range to
        // work/vramdump_<block>.bin. Comparing raw bytes against a console .gs rebuilt with
        // work/gsreplay_vram.py needs NO swizzle knowledge on either side, which is how the
        // toon-ramp CLUT was cleared: BT3 uploads its indexed textures as PSMCT32 and reads
        // them back as T8/T4, so the upload replay only ever needs addr32.
        static const char *s_vd = std::getenv("PS2X_VRAMDUMP");
        if (s_vd && s_vd[0])
        {
            // Optional 3rd field = wait this many calls before dumping. A dump on the FIRST
            // paletted draw catches whatever was resident before the frame's own uploads land,
            // which reads as a texture mismatch that is really a timing mismatch.
            static unsigned long s_wait = [](){ unsigned long b=0,n=0,w=0;
                const char *v = std::getenv("PS2X_VRAMDUMP");
                if (v && std::sscanf(v, "%lu,%lu,%lu", &b, &n, &w) == 3) return w;
                return 0ul; }();
            static unsigned long s_seen = 0;
            static bool done = false;
            if (!done && ++s_seen > s_wait)
            {
                done = true;
                unsigned long blk = 0, nb = 0;
                if (std::sscanf(s_vd, "%lu,%lu", &blk, &nb) == 2 && nb)
                {
                    char pth[192];
                    std::snprintf(pth, sizeof pth, "/home/z3/Desktop/bt3/work/vramdump_%lu.bin", blk);
                    const size_t off = (size_t)blk * 256;
                    if (off + nb <= gs->m_vramSize)
                        if (FILE *f = std::fopen(pth, "wb"))
                        {
                            std::fwrite(gs->m_vram + off, 1, nb, f);
                            std::fclose(f);
                            std::fprintf(stderr, "[vramdump] block %lu (+%lu bytes) -> %s\n", blk, nb, pth);
                        }
                }
            }
        }
    }
    {   // [clutdump] PS2X_CLUTDUMP=<cbp>: write this palette AND the raw VRAM bytes it was
        // built from to work/clutdump_<cbp>.bin. Lets an independent decoder (work/psmt8.py
        // against the console .gs) diff the palette in isolation from the index map -- the
        // two error sources otherwise mask each other.
        static const int s_cd = [](){ const char *v = std::getenv("PS2X_CLUTDUMP");
                                      return v && v[0] ? std::atoi(v) : -1; }();
        if (s_cd >= 0 && (int)tex.cbp == s_cd)
        {
            {   // Track how many DISTINCT palettes we actually build for this cbp. BT3 reuses
                // one CBP for several materials inside a single frame, so a cache that misses a
                // re-upload silently serves the previous character's colours.
                uint32_t ph = 2166136261u;
                for (int i = 0; i < count; ++i) ph = (ph ^ gs->m_clutCache[i]) * 16777619u;
                static std::set<uint32_t> seen; static unsigned long calls = 0, rebuilds = 0;
                ++rebuilds;
                const bool isNew = seen.insert(ph).second;
                if (isNew || (rebuilds % 200) == 0)
                    std::fprintf(stderr, "[clutdump] cbp=%u rebuild #%lu distinct-palettes=%zu%s\n",
                                 tex.cbp, rebuilds, seen.size(), isNew ? "  (NEW)" : "");
                (void)calls;
                // PS2X_CLUTDUMP_ALL=1: write EVERY distinct palette (cap 32) to numbered files —
                // this cbp hosts several materials; single dumps compare the wrong pairs.
                static const bool s_cda = [](){ const char *v = std::getenv("PS2X_CLUTDUMP_ALL"); return v && v[0] && v[0] != '0'; }();
                if (s_cda && isNew && seen.size() <= 32)
                {
                    char pth2[192];
                    std::snprintf(pth2, sizeof pth2, "/home/z3/Desktop/bt3/work/clutdump_%u_%02zu.bin", tex.cbp, seen.size());
                    if (FILE *f2 = std::fopen(pth2, "wb"))
                    {
                        uint32_t hdr2[4] = {tex.cbp, tex.cpsm, tex.csm, tex.csa};
                        std::fwrite(hdr2, 4, 4, f2);
                        std::fwrite(gs->m_clutCache, 4, 256, f2);
                        const size_t off2 = (size_t)tex.cbp * 256;
                        if (off2 + 1024 <= gs->m_vramSize) std::fwrite(gs->m_vram + off2, 1, 1024, f2);
                        std::fclose(f2);
                    }
                }
            }
            static bool done = false;
            // PS2X_CLUTDUMP_N=<n>: dump on the nth rebuild instead of the first — the first
            // bind lands in the fight fade-in, where the lighting-scaled palette is dark.
            static const unsigned long s_cdN = [](){ const char *v = std::getenv("PS2X_CLUTDUMP_N");
                                                     return v && v[0] ? std::strtoul(v, nullptr, 0) : 1ul; }();
            static unsigned long s_cdSeen = 0;
            if (!done && ++s_cdSeen < s_cdN) { /* not yet */ }
            else if (!done)
            {
                done = true;
                char pth[192];
                std::snprintf(pth, sizeof pth,
                              "/home/z3/Desktop/bt3/work/clutdump_%u.bin", tex.cbp);
                if (FILE *f = std::fopen(pth, "wb"))
                {
                    uint32_t hdr[4] = {tex.cbp, tex.cpsm, tex.csm, tex.csa};
                    std::fwrite(hdr, 4, 4, f);
                    std::fwrite(gs->m_clutCache, 4, 256, f);          // our decoded palette
                    const size_t off = (size_t)tex.cbp * 256;         // raw VRAM behind it
                    if (off + 1024 <= gs->m_vramSize)
                        std::fwrite(gs->m_vram + off, 1, 1024, f);
                    std::fclose(f);
                    std::fprintf(stderr, "[clutdump] cbp=%u cpsm=%u csm=%u csa=%u -> %s\n",
                                 tex.cbp, tex.cpsm, tex.csm, tex.csa, pth);
                }
            }
        }
    }
}

// Records a SPRITE (as 2 triangles) or a TRIANGLE (1 triangle) to the GPU renderer.
// [decodefn] The inline texture decode of recordSpriteGPU, extracted verbatim so the
// decode can later run off the guest thread (deferred no-wait decode). Pure move:
// same reads, same output. subW = decoded width (sub-decode aware), rgba = texels.
void GSRasterizer::decodeTexRGBA(GS *gs, TexDecodeSrc src, int texW, int texH, bool rawAlphaDec, uint64_t texKey, int &subW, std::vector<uint8_t> &rgba)
{
    const GSTex0Reg &tex0 = *src.tex0;
    if (gs) { ensureClutCache(gs); src.clutKey = gs->m_clutCacheKey; }   // [deferdec] refresh the key the cache pointer belongs to
    subW = src.subDxW ? src.subDxW : texW; const int subX0 = src.subDxW ? src.subDx0 : 0;   // [subdecode]
    rgba.assign(static_cast<size_t>(subW) * texH * 4u, 0u);
    // sampleTexture() interprets the (u,v) args only on the FST path; on the
    // STQ path it uses s/t/q (which we pass as 0,0,1) and would collapse every
    // texel to (0,0). The box quads are STQ, so force FST + point sampling here
    // to read the real per-texel content. Restore afterwards.
    const uint32_t savedFst = gs ? gs->m_prim.fst : 0u;
    if (gs) gs->m_prim.fst = 1u;
    // FAST PATH -- 8-bit paletted (PSMT8) point sampling. This is the hot case:
    // BT3's animated 128x256 T8 background re-detextures every frame (~3.9M
    // texels/s). The generic sampleTexture() pays a std::function ReadVram
    // dispatch + full psm/clamp/fst branching PER TEXEL. Here the CLUT was already
    // decoded into src.clut[256] (raw-index keyed, swizzle baked in), and we
    // iterate strictly in-range, so we can inline the swizzle address and read the
    // index byte straight from VRAM. ~3-5x faster -> unlocks the animated screens.
    const bool fastT8 = (tex0.psm == GS_PSM_T8) &&
                        (src.clutKey != ~0ull) && src.vram;
    // Same fast path for 4-bit paletted (PSMT4) -- BT3's logos alternate T8/T4 at
    // the same tbp0, and the T4 ones were falling through to the ~220ns/texel
    // sampleTexture path (862ms/s = the whole 20fps boot cap). addrPSMT4 yields a
    // NIBBLE address; the low/high nibble of the byte is the CLUT index (16 entries).
    const bool fastT4 = (tex0.psm == GS_PSM_T4) &&
                        (src.clutKey != ~0ull) && src.vram;
    if (fastT4)
    {
        const uint8_t *vram = src.vram;
        const uint32_t vmask = src.vramSize ? (src.vramSize - 1u) : 0x3FFFFFu;
        const uint32_t *clut = src.clut;
        const uint32_t blk = tex0.tbp0;
        const uint32_t bw = tex0.tbw;
        const int W = subW;
        uint8_t *dst = rgba.data();
        // The fastT4 / fastT8 decoders expanded alpha (At * 255/128) UNCONDITIONALLY
        // while the generic path honours PS2X_RAWTEXA. That inconsistency is why
        // RAWTEXA appeared to have no effect on BT3's character textures -- they take
        // the fast path. Same flag here so all three decode paths agree.
        static const bool s_rawA4 = [](){ const char *v = std::getenv("PS2X_RAWTEXA");
                                          return v && v[0] && v[0] != '0'; }();
                uint32_t clutR[16];   // [fastdec] palette with the alpha already rescaled
        for (int i = 0; i < 16; ++i) { const uint32_t c = clut[i]; const uint32_t al = c >> 24; clutR[i] = (c & 0x00FFFFFFu) | ((uint32_t)(rawAlphaDec ? al : kAlpha128To255[al]) << 24); }
        (void)vmask;
        static const bool s_ft4 = [](){ const char *v = std::getenv("PS2X_FASTT4"); return !(v && v[0] == '0'); }();
        const bool rowOk4 = s_ft4 && bw >= 2u;   // [fastdec] page = 128 px wide: a 64-px buffer (tbw 1) needs the exact per-texel address
        parallelRows(0, texH - 1, [&](int ty)
        {   // [fastdec] one row of nibble indices through the page table, one 32-bit store per texel
            uint32_t *drow = reinterpret_cast<uint32_t *>(dst + (size_t)ty * W * 4u);
            if (rowOk4)
            {
                thread_local std::vector<uint8_t> idx; if (idx.size() < (size_t)W) idx.resize((size_t)W);
                GSMem::ReadRowP4(vram, blk, bw, (u32)subX0, (u32)(subX0 + W), (u32)ty, idx.data());
                for (int tx = 0; tx < W; ++tx) drow[tx] = clutR[idx[(size_t)tx] & 0x0Fu];
                return;
            }
            for (int tx = 0; tx < W; ++tx)
            {
                const uint32_t na = GSPSMT4::addrPSMT4(blk, bw, static_cast<uint32_t>(tx + subX0), static_cast<uint32_t>(ty));
                const uint8_t bval = vram[(na >> 1) & vmask];
                drow[tx] = clutR[((na & 1u) ? (bval >> 4) : (bval & 0x0Fu)) & 0x0Fu];
            }
        });
    }
    else if (fastT8)
    {
        const uint8_t *vram = src.vram;
        const uint32_t vmask = src.vramSize ? (src.vramSize - 1u) : 0x3FFFFFu;
        const uint32_t *clut = src.clut;
        const uint32_t blk = tex0.tbp0;
        const uint32_t bw = tex0.tbw;
        const int W = subW;
        uint8_t *dst = rgba.data();
        // Split the rows across the (GPU-mode-idle) scanline pool -- each row writes
        // a disjoint RGBA span so it's race-free. This is the per-frame animated
        // background, so parallel detexture directly buys wall-clock / fps.
                uint32_t clutR[256];   // [fastdec]
        for (int i = 0; i < 256; ++i) { const uint32_t c = clut[i]; const uint32_t al = c >> 24; clutR[i] = (c & 0x00FFFFFFu) | ((uint32_t)(rawAlphaDec ? al : kAlpha128To255[al]) << 24); }
        static const bool s_ft8 = [](){ const char *v = std::getenv("PS2X_FASTT8"); return !(v && v[0] == '0'); }();
        const bool rowOk8 = s_ft8 && bw >= 2u;   // [fastdec] T8 page = 128 px wide (see T4)
        parallelRows(0, texH - 1, [&](int ty)
        {
            uint32_t *drow = reinterpret_cast<uint32_t *>(dst + (size_t)ty * W * 4u);
            if (rowOk8)
            {
                thread_local std::vector<uint8_t> idx; if (idx.size() < (size_t)W) idx.resize((size_t)W);
                GSMem::ReadRowP8(vram, blk, bw, (u32)subX0, (u32)(subX0 + W), (u32)ty, idx.data());
                for (int tx = 0; tx < W; ++tx) drow[tx] = clutR[idx[(size_t)tx]];
                return;
            }
            for (int tx = 0; tx < W; ++tx)
            {
                const uint32_t off = GSPSMT8::addrPSMT8(blk, bw, static_cast<uint32_t>(tx + subX0), static_cast<uint32_t>(ty)) & vmask;
                drow[tx] = clutR[vram[off]];
            }
        });
    }
    else if ([&]() {   // [fastdecode] direct-colour / T8H fast path (see below)
        static const bool s_fd = [](){ const char *v = std::getenv("PS2X_FASTDECODE"); return !(v && v[0] == '0'); }();
        static const bool s_diagOff = [](){ return !std::getenv("PS2X_TEXHL") && !std::getenv("PS2X_TEXEL_PROBE") && !std::getenv("PS2X_UVLOG") && !std::getenv("PS2X_Z16SPY") && !std::getenv("PS2X_RAMPSPY"); }();
        const uint32_t pm = tex0.psm;
        const bool direct = pm == GS_PSM_CT32 || pm == GS_PSM_CT24 || pm == GS_PSM_Z32 || pm == GS_PSM_Z24 ||
                            pm == GS_PSM_CT16 || pm == GS_PSM_CT16S || pm == GS_PSM_Z16 || pm == GS_PSM_Z16S;
        const bool idx8h = pm == GS_PSM_T8H && src.clutKey != ~0ull;
        return s_fd && s_diagOff && src.vram && (direct || idx8h); }())
    {
        // [fastdecode] The barrier chain re-decodes whole 512x448 RT pages (CT32 scene,
        // T8H mask, CT16 edge map) after every flush, and those formats fell through to
        // the generic per-texel sampleTexture (std::function ReadVram dispatch, full
        // clamp/fst branching): 34% of a rig run. Same maths as samplePoint(), inlined
        // and parallel. PS2X_FASTDECODE=0 restores the generic loop.
        const uint32_t pm = tex0.psm; const uint32_t blkF = tex0.tbp0, bwF = tex0.tbw;
        uint8_t *vramF = src.vram; const int W = texW, H = texH;
        static const bool s_oldSampler = [](){ const char *v = std::getenv("PS2X_OLDSAMPLER"); return v && v[0] && v[0] != '0'; }();
        const uint32_t wms = s_oldSampler ? 1u : (uint32_t)(src.clamp & 3u), wmt = s_oldSampler ? 1u : (uint32_t)((src.clamp >> 2) & 3u);
        const int minU = (int)((src.clamp >> 4) & 0x3FFu), maxU = (int)((src.clamp >> 14) & 0x3FFu);
        const int minV = (int)((src.clamp >> 24) & 0x3FFu), maxV = (int)((src.clamp >> 34) & 0x3FFu);
        auto wrapC = [](int c, int size, uint32_t mode, int mn, int mx) -> int {
            switch (mode) { case 0u: return c & (size - 1); case 2u: return clampInt(c, mn, mx > mn ? mx : size - 1);
                            case 3u: return ((c & mn) | mx) & (size - 1); default: return clampInt(c, 0, size - 1); } };
        u32 (*rd)(u8 *, u32, u32, u32, u32) = nullptr; int kind = 0;   // 0 = 32-bit direct, 1 = 16-bit direct, 2 = T8H
        switch (pm) {
            case GS_PSM_CT32: rd = GSMem::ReadCT32; break; case GS_PSM_CT24: rd = GSMem::ReadCT24; break;
            case GS_PSM_Z32: rd = GSMem::ReadZ32; break; case GS_PSM_Z24: rd = GSMem::ReadZ24; break;
            case GS_PSM_CT16: rd = GSMem::ReadCT16; kind = 1; break; case GS_PSM_CT16S: rd = GSMem::ReadCT16S; kind = 1; break;
            case GS_PSM_Z16: rd = GSMem::ReadZ16; kind = 1; break; case GS_PSM_Z16S: rd = GSMem::ReadZ16S; kind = 1; break;
            default: rd = GSMem::ReadP8H; kind = 2; break; }
        const uint32_t *clutF = src.clut; const GSTexaReg texaF = (*src.texa);
        static const bool s_rawA2 = [](){ const char *v = std::getenv("PS2X_RAWTEXA"); return v && v[0] && v[0] != '0'; }();
        static const bool s_texaExp2 = [](){ const char *v = std::getenv("PS2X_TEXAEXP"); return v && v[0] && v[0] != '0'; }();
        const bool ct16Src2 = (pm == GS_PSM_CT16 || pm == GS_PSM_CT16S);
        const bool keepRaw2 = (s_rawA2 && !(s_texaExp2 && ct16Src2)) || rawAlphaDec;
        uint8_t *dst = rgba.data();
        // [rowdecode] the column wrap is the same for every row: compute it once, and when it is a
        // contiguous ascending run use a bulk row reader (page once per 64-px column, one table
        // lookup per texel) instead of a swizzled function-pointer read per texel.
        static const bool s_rowDec = [](){ const char *v = std::getenv("PS2X_ROWDECODE"); return !(v && v[0] == '0'); }();
        std::vector<int> uIdx((size_t)subW);
        bool uContig = s_rowDec && subW > 0;
        for (int tx = 0; tx < subW; ++tx) { uIdx[(size_t)tx] = wrapC(tx + subX0, W, wms, minU, maxU); if (tx && uIdx[(size_t)tx] != uIdx[0] + tx) uContig = false; }
        static const bool s_fz16 = [](){ const char *v = std::getenv("PS2X_FASTZ16"); return !(v && v[0] == '0'); }();
        const bool rowRead = uContig && (pm == GS_PSM_CT32 || pm == GS_PSM_CT24 || pm == GS_PSM_CT16 || pm == GS_PSM_CT16S || (s_fz16 && (pm == GS_PSM_Z16 || pm == GS_PSM_Z16S)) || kind == 2);
        // [fastdec] 16-bit texel -> RGBA8 through a 64K LUT cached per (psm, TEXA, raw-alpha): one lookup per texel
        struct Lut16 { uint64_t key = ~0ull; std::vector<uint32_t> t; };
        thread_local Lut16 lut16;
        const uint32_t *lut16p = nullptr;
        if (rowRead && kind == 1)
        {
            const uint64_t key = ((uint64_t)pm << 56) | ((uint64_t)(keepRaw2 ? 1u : 0u) << 55) | ((uint64_t)texaF.ta0 << 40) | ((uint64_t)texaF.ta1 << 32) | ((uint64_t)(texaF.aem ? 1u : 0u) << 31);
            if (lut16.key != key)
            {
                lut16.t.resize(65536u);
                for (uint32_t i = 0; i < 65536u; ++i) { const uint32_t texel = applyTexa(texaF, (uint8_t)pm, Rgba5551ToRgba8888((u16)i)); const uint32_t a = texel >> 24; lut16.t[i] = (texel & 0x00FFFFFFu) | ((uint32_t)(keepRaw2 ? a : kAlpha128To255[a]) << 24); }
                lut16.key = key;
            }
            lut16p = lut16.t.data();
        }
        uint32_t clutR[256];   // [fastdec] palette with the alpha already rescaled (P8H row path: one lookup per texel)
        if (rowRead && kind == 2) for (int i = 0; i < 256; ++i) { const uint32_t c = clutF[i]; const uint32_t a = c >> 24; clutR[i] = (c & 0x00FFFFFFu) | ((uint32_t)(keepRaw2 ? a : kAlpha128To255[a]) << 24); }
        // [linvram] render-target content: read the rows from the flush's LINEAR image instead of swizzled VRAM
        // (live VRAM only -- a deferred decode's private scratch is not mirrored). Falls back per texture when the
        // image does not exist / the view or stride differ.
        thread_local std::vector<uint32_t> linBuf; bool linOk = false;
        if (rowRead && (pm == GS_PSM_CT32 || pm == GS_PSM_CT16 || pm == GS_PSM_CT16S || kind == 2) && gs && vramF == gs->m_vram && H > 0 && subW > 0)   // gs == nullptr on the deferred (scratch) path
        {
            if (linBuf.size() < (size_t)subW * (size_t)H) linBuf.resize((size_t)subW * (size_t)H);
            linOk = ps2xLinearFetch(src.tex0->tbp0, pm, src.tex0->tbw, (uint32_t)uIdx[0], subW, H, linBuf.data());
        }
        const uint32_t *const linData = linOk ? linBuf.data() : nullptr;   // plain local: the row lambdas run on POOL threads, where the thread_local above is a different (empty) object
        parallelRows(0, H - 1, [&](int ty)
        {
            const int v = wrapC(ty, H, wmt, minV, maxV);
            if (rowRead)
            {
                const u32 u0 = (u32)uIdx[0], u1 = u0 + (u32)subW;
                uint32_t *drow = reinterpret_cast<uint32_t *>(dst + (size_t)ty * subW * 4u);
                thread_local std::vector<uint32_t> buf32; thread_local std::vector<uint16_t> buf16; thread_local std::vector<uint8_t> buf8;
                const bool lin = linOk && v >= 0 && v < H;
                const uint32_t *lrow = lin ? linData + (size_t)v * subW : nullptr;
                if (kind == 0)
                {
                    if (buf32.size() < (size_t)subW) buf32.resize((size_t)subW);
                    const uint32_t *srow = lin ? lrow : buf32.data();
                    if (!lin) GSMem::ReadRowCT32(vramF, blkF, bwF, u0, u1, (u32)v, buf32.data());
                    if (pm == GS_PSM_CT32)
                    {   // [fastdec] applyTexa is the identity for CT32: raw alpha = memcpy, else one LUT pass
                        if (keepRaw2) std::memcpy(drow, srow, (size_t)subW * 4u);
                        else for (int tx = 0; tx < subW; ++tx) { const uint32_t t = srow[tx]; drow[tx] = (t & 0x00FFFFFFu) | ((uint32_t)kAlpha128To255[t >> 24] << 24); }
                    }
                    else
                    for (int tx = 0; tx < subW; ++tx)
                    {
                        const uint32_t texel = applyTexa(texaF, (uint8_t)pm, srow[tx]);
                        const uint32_t a = (texel >> 24) & 0xFFu;
                        drow[tx] = (texel & 0x00FFFFFFu) | ((uint32_t)(keepRaw2 ? a : kAlpha128To255[a]) << 24);
                    }
                }
                else if (kind == 1)
                {
                    if (buf16.size() < (size_t)subW) buf16.resize((size_t)subW);
                    if (lin) { for (int tx = 0; tx < subW; ++tx) buf16[(size_t)tx] = (uint16_t)lrow[tx]; }
                    else if (pm == GS_PSM_CT16S) GSMem::ReadRowCT16S(vramF, blkF, bwF, u0, u1, (u32)v, buf16.data());
                    else if (pm == GS_PSM_Z16) GSMem::ReadRowZ16(vramF, blkF, bwF, u0, u1, (u32)v, buf16.data());
                    else if (pm == GS_PSM_Z16S) GSMem::ReadRowZ16S(vramF, blkF, bwF, u0, u1, (u32)v, buf16.data());
                    else GSMem::ReadRowCT16(vramF, blkF, bwF, u0, u1, (u32)v, buf16.data());
                    for (int tx = 0; tx < subW; ++tx) drow[tx] = lut16p[buf16[(size_t)tx]];   // [fastdec]
                }
                else
                {
                    if (buf8.size() < (size_t)subW) buf8.resize((size_t)subW);
                    if (lin) { for (int tx = 0; tx < subW; ++tx) buf8[(size_t)tx] = (uint8_t)(lrow[tx] >> 24); }
                    else GSMem::ReadRowP8H(vramF, blkF, bwF, u0, u1, (u32)v, buf8.data());
                    for (int tx = 0; tx < subW; ++tx) drow[tx] = clutR[buf8[(size_t)tx]];   // [fastdec] palette pre-rescaled once per texture
                }
                return;
            }
            for (int tx = 0; tx < subW; ++tx)
            {
                const int u = uIdx[(size_t)tx];
                const u32 out = rd(vramF, blkF, bwF, (u32)u, (u32)v);
                uint32_t texel;
                if (kind == 0) texel = applyTexa(texaF, (uint8_t)pm, out);
                else if (kind == 1) texel = applyTexa(texaF, (uint8_t)pm, Rgba5551ToRgba8888((u16)out));
                else texel = clutF[(u8)out];
                const size_t o = ((size_t)ty * subW + tx) * 4u;
                dst[o + 0] = texel & 0xFF; dst[o + 1] = (texel >> 8) & 0xFF; dst[o + 2] = (texel >> 16) & 0xFF;
                const uint32_t a = (texel >> 24) & 0xFFu;
                dst[o + 3] = keepRaw2 ? (uint8_t)a : (uint8_t)std::min(255u, a * 255u / 128u);
            }
        });
    }
    else if (!gs)
    {   // [deferdec] the generic sampleTexture() path needs the live GS; decodeIsDeferrable() keeps it off-thread
        static int n = 0; if (n++ < 4) std::fprintf(stderr, "[deferdec] generic decode path reached off-thread (psm %u) -- blank texture\n", (unsigned)tex0.psm);
    }
    else
    for (int ty = 0; ty < texH; ++ty)
        for (int tx = 0; tx < subW; ++tx)
        {
            uint32_t texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f,
                                           static_cast<uint16_t>((tx + subX0) * 16 + 8),
                                           static_cast<uint16_t>(ty * 16 + 8));
            size_t o = (static_cast<size_t>(ty) * subW + tx) * 4u;
            rgba[o + 0] = texel & 0xFF; rgba[o + 1] = (texel >> 8) & 0xFF;
            rgba[o + 2] = (texel >> 16) & 0xFF;
            // PS2 texture/CLUT alpha is 0..128 (0x80 = fully opaque). Scale to
            // 0..255 for the GL texture so blending isn't ~2x too transparent.
            //
            // PS2X_RAWTEXA=1: keep the RAW byte instead. That expansion CLAMPS, and a
            // CLUT used as DATA spans the full 0..255 -- BT3's mask palette is exactly
            // `255 - i`, so every index <= 127 saturated to 255 and the mask collapsed
            // to 2 distinct values (measured: mean 127.5, uniq 2, vs console's 189).
            // With this on, blending must take its factor from aBlend (PS2X_DUALSRC).
            static const bool s_rawA = [](){ const char *v = std::getenv("PS2X_RAWTEXA");
                                             return v && v[0] && v[0] != '0'; }();
            // PS2X_RAWTEXA exists for CLUT-AS-DATA reads: BT3's mask palette is literally
            // `255 - i`, so expanding it would clamp every index <= 127 to 255. That only
            // applies to INDEXED sources. For a DIRECT-COLOUR source the alpha is a blend
            // COEFFICIENT -- the GS divides Ad by 128, GL by 255 -- so keeping the raw byte
            // makes every such blend run at half strength. That is exactly what hid BT3's
            // outline: the CT16 edge pass stores TEXA's TA0=48 raw, so the untextured
            // bm0x52 darkener (Cd - Cs*Ad) subtracted 100*48/255 = 19 instead of
            // 100*48/128 = 37. PS2X_TEXAEXP=0 restores the old behaviour.
            static const bool s_texaExp = [](){ const char *v = std::getenv("PS2X_TEXAEXP");
                                                return v && v[0] && v[0] != '0'; }();
            const uint32_t dpsm2 = tex0.psm;
            // Scoped to PSMCT16 only. Expanding every non-indexed source also hits the
            // CT32 alpha writers feeding the mask chain, and measured alpha at the
            // darkener then fell 0.550% -> 0.000%. CT16 is the one format whose alpha is
            // purely TEXA-derived and purely a blend coefficient.
            const bool ct16Src = (dpsm2 == GS_PSM_CT16 || dpsm2 == GS_PSM_CT16S);
            const bool keepRaw = (s_rawA && !(s_texaExp && ct16Src)) || rawAlphaDec;
            rgba[o + 3] = keepRaw
                ? static_cast<uint8_t>((texel >> 24) & 0xFFu)
                : static_cast<uint8_t>(std::min(255u, ((texel >> 24) & 0xFFu) * 255u / 128u));
        }
    // Self-check (PS2X_GPU_DIAG): the fast T4/T8 paths bypass sampleTexture, so
    // verify a subsample matches the authoritative per-texel path. Catches a
    // swapped nibble / wrong swizzle for the fast formats.
    {
        static const bool s_vf = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_vf && (fastT4 || fastT8))
        {
            static std::atomic<int> s_checks{0};
            if (gs && s_checks.fetch_add(1) < 6)
            {
                const uint32_t sf = gs->m_prim.fst; gs->m_prim.fst = 1u;
                int mism = 0, tested = 0;
                for (int ty = 0; ty < texH; ty += std::max(1, texH/16))
                    for (int tx = 0; tx < texW; tx += std::max(1, texW/16))
                    {
                        uint32_t ref = sampleTexture(gs, 0.f,0.f,1.f, (uint16_t)(tx*16+8), (uint16_t)(ty*16+8));
                        size_t o = (static_cast<size_t>(ty)*texW+tx)*4u;
                        uint8_t rr=ref&0xFF, rg=(ref>>8)&0xFF, rb=(ref>>16)&0xFF;
                        ++tested;
                        if (rgba[o]!=rr || rgba[o+1]!=rg || rgba[o+2]!=rb) ++mism;
                    }
                gs->m_prim.fst = sf;
                std::fprintf(stderr, "[fastcheck] psm=%u %dx%d mismatch=%d/%d\n", tex0.psm, texW, texH, mism, tested);
            }
        }
    }
    if (gs) gs->m_prim.fst = savedFst;
    {
        static const bool s_d = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        static std::set<uint64_t> s_logged;
        if (s_d && s_logged.insert(texKey).second)
        {
            uint8_t maxR = 0, maxG = 0, maxB = 0, maxA = 0;
            uint32_t brightPix = 0, opaquePix = 0;
            for (size_t p = 0; p + 3 < rgba.size(); p += 4)
            {
                maxR = std::max(maxR, rgba[p]); maxG = std::max(maxG, rgba[p+1]);
                maxB = std::max(maxB, rgba[p+2]); maxA = std::max(maxA, rgba[p+3]);
                if (rgba[p+3] > 32) { ++opaquePix; if (std::max({rgba[p],rgba[p+1],rgba[p+2]}) > 128) ++brightPix; }
            }
            // Neighbor-difference "noise score": real art is locally smooth, a
            // misaddressed decode (VRAM that isn't a texture) is white noise. Mean
            // |RGB(x)-RGB(x+1)| > ~60 flags it for triage without opening the PNG.
            uint64_t nd = 0, nc = 0;
            for (size_t p = 0; p + 7 < rgba.size(); p += 32)
            {
                nd += std::abs((int)rgba[p] - (int)rgba[p+4]) + std::abs((int)rgba[p+1] - (int)rgba[p+5]) + std::abs((int)rgba[p+2] - (int)rgba[p+6]);
                nc += 3;
            }
            const unsigned noise = nc ? (unsigned)(nd / nc) : 0u;
            // Indexed textures: distinguish "palette collapsed" (CLUT-read bug -> all
            // entries ~identical) from "indices flat" (texel VRAM stale/constant).
            char clutInfo[96] = "";
            {
                const uint32_t p = tex0.psm;
                const bool i8 = (p == GS_PSM_T8 || p == GS_PSM_T8H);
                const bool i4 = (p == GS_PSM_T4 || p == GS_PSM_T4HL || p == GS_PSM_T4HH);
                if ((i8 || i4) && src.clutKey != ~0ull)
                {
                    const int n = i4 ? 16 : 256;
                    std::set<uint32_t> uniq;
                    for (int i = 0; i < n; ++i) uniq.insert(src.clut[i]);
                    std::snprintf(clutInfo, sizeof(clutInfo), " clutDistinct=%zu/%d clut[0]=%08x clut[%d]=%08x",
                                  uniq.size(), n, src.clut[0], n/2, src.clut[n/2]);
                }
            }
            std::fprintf(stderr, "[texdec] key=%llu %dx%d psm=%u tbp0=%u tbw=%u | CLUT cbp=%u cpsm=%u csm=%u csa=%u cld=%u texclut=%u,%u,%u | maxRGBA=(%u,%u,%u,%u) opaque=%u bright=%u noise=%u%s%s\n",
                         (unsigned long long)texKey, texW, texH, tex0.psm, tex0.tbp0, tex0.tbw,
                         tex0.cbp, tex0.cpsm, tex0.csm, tex0.csa, tex0.cld,
                         (*src.texclut).cbw, (*src.texclut).cou, (*src.texclut).cov,
                         maxR, maxG, maxB, maxA, opaquePix, brightPix, noise, noise > 60 ? " NOISY" : "", clutInfo);
        }
    }
    // PS2X_HUDTEX: uncapped decode trace for the HUD's black textures -> WHY do they decode black?
    {
        static const bool s_ht = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_HUDTEX"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_ht && (texKey == 2382948793199915546ull || texKey == 8431140355099953178ull || texKey == 18369432953290548053ull)) {
            uint8_t mA = 0, mRGB = 0; for (size_t p = 0; p + 3 < rgba.size(); p += 4) { mA = std::max(mA, rgba[p+3]); mRGB = std::max({mRGB, rgba[p], rgba[p+1], rgba[p+2]}); }
            static int s_n = 0;
            if (s_n++ < 10) std::fprintf(stderr, "[hudtex] key=%llu %dx%d psm=%u tbp0=%u tbw=%u tcc=%u tfx=%u | CLUT cbp=%u cpsm=%u csm=%u csa=%u cld=%u | decoded maxRGB=%u maxA=%u\n",
                                         (unsigned long long)texKey, texW, texH, tex0.psm, tex0.tbp0, tex0.tbw, tex0.tcc, tex0.tfx,
                                         tex0.cbp, tex0.cpsm, tex0.csm, tex0.csa, tex0.cld, mRGB, mA);
        }
    }
    // PS2X_TEXDUMP_TBP=<tbp0>: log the first decodes of that tbp0 (key + CLUT regs +
    // mean). The GPU_DIAG gputex export writes the PNG as gputex_<key>.png — this
    // line maps tbp -> key so the file can be found without hunting content hashes.
    {
        static const uint32_t s_tdt = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_TEXDUMP_TBP"); return s_env; }(); return v ? (uint32_t)std::strtoul(v, nullptr, 0) : 0u; }();
        if (s_tdt && tex0.tbp0 == s_tdt)
        {
            static std::atomic<int> s_tn{0};
            int n = s_tn.fetch_add(1);
            if (n < 8)
            {
                uint64_t sum = 0; size_t cnt = 0;
                uint8_t mn = 255, mx = 0;
                for (size_t p = 0; p + 3 < rgba.size(); p += 16)
                {
                    sum += rgba[p] + rgba[p+1] + rgba[p+2]; cnt += 3;
                    mn = std::min({mn, rgba[p], rgba[p+1], rgba[p+2]});
                    mx = std::max({mx, rgba[p], rgba[p+1], rgba[p+2]});
                }
                std::fprintf(stderr, "[texdump] tbp0=%u #%d key=%llu %dx%d psm=%u | CLUT cbp=%u cpsm=%u csm=%u csa=%u cld=%u | meanRGB=%llu min=%u max=%u -> gputex_%llu.png\n",
                             tex0.tbp0, n, (unsigned long long)texKey, texW, texH, tex0.psm,
                             tex0.cbp, tex0.cpsm, tex0.csm, tex0.csa, tex0.cld,
                             (unsigned long long)(cnt ? sum / cnt : 0), mn, mx, (unsigned long long)texKey);
            }
        }
    }
    {   // PS2X_TEXCENSUS=<psm>: what did this decode actually produce? For the mask
        // chain the answer that matters is how many DISTINCT values came back -- an
        // indexed read of a live buffer that returns one value is a flat wash, and
        // looks identical to "the pass never ran".
        static const int s_tc = [](){ const char *v = std::getenv("PS2X_TEXCENSUS");
                                      return v && v[0] ? (int)std::strtol(v, nullptr, 16) : -1; }();
        if (s_tc >= 0 && (int)tex0.psm == s_tc)
        {
            static int n = 0;
            if (n++ < 400)
            {
                unsigned uniqA = 0, uniqRGB = 0;
                bool seenA[256] = {false};
                std::set<uint32_t> rgbSet;
                for (size_t i = 0; i + 3 < rgba.size(); i += 4)
                {
                    if (!seenA[rgba[i + 3]]) { seenA[rgba[i + 3]] = true; ++uniqA; }
                    if (rgbSet.size() < 64)
                        rgbSet.insert((uint32_t)rgba[i] | ((uint32_t)rgba[i+1] << 8) | ((uint32_t)rgba[i+2] << 16));
                }
                uniqRGB = (unsigned)rgbSet.size();
                // For an ADDITIVE composite (blend 0x48) the number that decides
                // "outline strokes" vs "wash over the whole frame" is how much of the
                // decoded texture is BLACK: adding 0 is a no-op, adding anything else
                // brightens. Same for alpha-blended passes with alpha 0.
                size_t nBlack = 0, nZeroA = 0, npx = 0;
                unsigned long sr = 0, sg = 0, sb = 0, sa2 = 0;
                for (size_t i = 0; i + 3 < rgba.size(); i += 4)
                {
                    ++npx;
                    if (!rgba[i] && !rgba[i+1] && !rgba[i+2]) ++nBlack;
                    if (!rgba[i+3]) ++nZeroA;
                    sr += rgba[i]; sg += rgba[i+1]; sb += rgba[i+2];
                    sa2 += rgba[i+3];
                }
                std::fprintf(stderr, "[texcensus] psm=%02x tbp=%u cbp=%u %dx%d -> distinct alpha=%u rgb=%u%s"
                                     " | black %.1f%%  a==0 %.1f%%  mean rgb=(%.0f,%.0f,%.0f) meanA=%.1f\n",
                             tex0.psm, tex0.tbp0, tex0.cbp, texW, texH,
                             uniqA, uniqRGB, uniqRGB >= 64 ? "+" : "",
                             100.0 * (double)nBlack / (double)npx,
                             100.0 * (double)nZeroA / (double)npx,
                             (double)sr / npx, (double)sg / npx, (double)sb / npx,
                             (double)sa2 / npx);
            }
        }
    }
    {   // PS2X_TEXDUMP=<psm>: write each decode of that PSM as raw RGBA (w,h header).
        // For a PSMT8H read through CLUT 16000 (which is exactly 255-i) the decoded
        // ALPHA is 255 minus the source index, so this shows precisely what the mask
        // composites see when they sample the scene.
        static const int s_td = [](){ const char *v = std::getenv("PS2X_TEXDUMP");
                                      return v && v[0] ? (int)std::strtol(v, nullptr, 16) : -1; }();
        if (s_td >= 0 && (int)tex0.psm == s_td)
        {
            // Keep the LAST decodes, not the first: the first are frame-1 warm-up,
            // before the alpha rebuild has run, and reading them as representative sent
            // this investigation off twice.
            static int s_i = 0;
            if (s_i < 400)
            {
                char path[256];
                const char *dir = std::getenv("PS2X_GS_REPLAY_OUT");
                std::snprintf(path, sizeof(path), "%s/texlast_tbp%u_cbp%u.raw",
                              dir ? dir : ".", tex0.tbp0, tex0.cbp);
                ++s_i;
                if (FILE *f = std::fopen(path, "wb"))
                {
                    int hdr[2] = { texW, texH };
                    std::fwrite(hdr, sizeof(hdr), 1, f);
                    std::fwrite(rgba.data(), 1, rgba.size(), f);
                    std::fclose(f);
                }
            }
        }
    }
    {   // [idxhist] PS2X_IDXHIST=1: for PSMT8H decodes of page 224 (the outline stamps' source),
        // histogram the RAW INDEX BYTES the decode read from VRAM. Uniform => the mask never
        // reached VRAM where this decode looks.
        static const bool s_ih = [](){ const char *v = std::getenv("PS2X_IDXHIST"); return v && v[0] && v[0] != '0'; }();
        static int ihN = 0;
        if (s_ih && ihN < 6 && (gs && gs->m_prim.tme != 0) && tex0.psm == GS_PSM_T8H && (tex0.tbp0 / 32u) == 224u)
        {
            ++ihN;
            std::map<unsigned, unsigned long> hh; unsigned long n = 0;
            const uint8_t *vram = src.vram; const uint32_t vmask = src.vramSize ? (src.vramSize - 1u) : 0x3FFFFFu;
            for (int ty = 0; ty < texH; ty += 2) for (int tx = 0; tx < texW; tx += 2)
            {   const uint32_t wa = GSPSMCT32::addrPSMCT32(tex0.tbp0, tex0.tbw, (uint32_t)tx, (uint32_t)ty);
                const uint32_t word = *reinterpret_cast<const uint32_t *>(vram + ((wa * 4u) & vmask));
                ++n; ++hh[(word >> 24) & 0xFFu]; }
            std::vector<std::pair<unsigned long, unsigned>> t; for (auto &kv : hh) t.push_back({kv.second, kv.first});
            std::sort(t.rbegin(), t.rend());
            std::fprintf(stderr, "[idxhist] T8H page224 tbp=%u tbw=%u %dx%d cbp=%u raw index bytes:", tex0.tbp0, tex0.tbw, texW, texH, tex0.cbp);
            for (size_t i = 0; i < t.size() && i < 6; ++i) std::fprintf(stderr, " %u:%.1f%%", t[i].second, 100.0 * t[i].first / (double)n);
            std::fprintf(stderr, "  distinct=%zu\n", hh.size());
        }
    }
}

// [deferdec] GL-thread entry: rebuild the palette from (now written-back) VRAM, then run the fast decode.
void GSRasterizer::decodeDeferred(const TexDecodeReq &req, uint8_t *vram, size_t vramSize, int &subW, std::vector<uint8_t> &rgba)
{
    thread_local uint32_t clut[256];
    const int n = fillClutFrom(clut, vram, req.texa, req.texclut, req.tex0);
    TexDecodeSrc src;
    src.tex0 = &req.tex0; src.clamp = req.clamp; src.vram = vram; src.vramSize = vramSize;
    src.clut = clut; src.clutKey = n ? 1ull : ~0ull; src.texa = &req.texa; src.texclut = &req.texclut;
    src.subDxW = req.subDxW; src.subDx0 = req.subDx0;
    decodeTexRGBA(nullptr, src, req.texW, req.texH, req.rawAlphaDec, req.texKey, subW, rgba);
}
bool GSRasterizer::decodeIsDeferrable(uint32_t pm)
{
    static const bool s_fd = [](){ const char *v = std::getenv("PS2X_FASTDECODE"); return !(v && v[0] == '0'); }();
    static const bool s_diagOff = [](){ return !std::getenv("PS2X_TEXHL") && !std::getenv("PS2X_TEXEL_PROBE") && !std::getenv("PS2X_UVLOG") && !std::getenv("PS2X_Z16SPY") && !std::getenv("PS2X_RAMPSPY"); }();
    if (!s_fd || !s_diagOff) return false;
    return pm == GS_PSM_T8 || pm == GS_PSM_T4 || pm == GS_PSM_T8H ||
           pm == GS_PSM_CT32 || pm == GS_PSM_CT24 || pm == GS_PSM_Z32 || pm == GS_PSM_Z24 ||
           pm == GS_PSM_CT16 || pm == GS_PSM_CT16S || pm == GS_PSM_Z16 || pm == GS_PSM_Z16S;
}

std::atomic<unsigned long> g_recTplHit{0}, g_recTplMiss{0}, g_recTplWhy[5];   // [rectemplate] read by ps2_runtime.cpp
std::atomic<uint64_t> g_scissorCulled{0};   // [scissorcull] primitives dropped as fully outside SCISSOR

bool GSRasterizer::recordSpriteGPU(GS *gs)
{
    gprof::Scope gpScope(gprof::REC_PRE);   // [guestprof] sub-phases via gprof::mark below
    const auto &ctx = gs->activeContext();
    const bool isSprite = (gs->m_prim.type == GS_PRIM_SPRITE);
    const bool tme = gs->m_prim.tme != 0;
    const bool fst = gs->m_prim.fst != 0;

    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    // XYOFFSET is 12.4 FIXED POINT and `>> 4` throws the fraction away. BT3's outline edge
    // detect is built out of exactly that fraction: the three fbp224 -> fbp336 passes are the
    // same image at XYOFFSET (1792.000, 1824.000), then subtracted at (1793.000, 1824.000)
    // -- a whole pixel in x, which survives -- and at (1792.000, **1824.500**), half a pixel
    // in y, which did not. The vertices are integral; the SHIFT LIVES IN THE OFFSET.
    const float ofxF = static_cast<float>(ctx.xyoffset.ofx) / 16.0f;
    const float ofyF = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;

    // [primcensus] PS2X_PRIMCENSUS=1: classify EVERY primitive reaching the record path, to find
    // classes that cost full price and draw nothing. Rationale: every guest phase is per-primitive
    // (vu1, recbuild, gif, vif, rectex, push), so deleting a primitive removes its cost from all of
    // them at once -- which is why the zero-area cull was the biggest single win in the project
    // (fights 22-25 -> 28-30 fps) while per-phase micro-optimisation has been yielding ~10%.
    // Counting ONLY here, before the cull below, so the buckets are mutually comparable.
    // Diagnostic: no behaviour change, one predictable branch when off.
    {
        static const bool s_census = [](){ const char *v = std::getenv("PS2X_PRIMCENSUS"); return v && v[0] && v[0] != '0'; }();
        if (s_census)
        {
            static std::atomic<uint64_t> c_tot{0}, c_spr{0}, c_degen{0}, c_tiny{0}, c_off{0}, c_a0{0}, c_notme{0};
            // c_offlive is the bucket that actually matters: offscreen AND not already dropped by
            // the zero-area cull below. The raw offscreen count overstates the prize, because
            // distant geometry is both more likely to be off the scissor AND more likely to be a
            // micro-triangle -- the two sets correlate, so they must be measured exclusively.
            static std::atomic<uint64_t> c_offlive{0};
            const int nv = isSprite ? 2 : 3;
            float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
            bool allA0 = true;
            for (int i = 0; i < nv; ++i)
            {
                const GSVertex &v = gs->m_vtxQueue[i];
                const float sx = v.x - ofxF, sy = v.y - ofyF;   // window coords: scissor's space
                mnx = std::min(mnx, sx); mxx = std::max(mxx, sx);
                mny = std::min(mny, sy); mxy = std::max(mxy, sy);
                if (v.a != 0) allA0 = false;
            }
            c_tot.fetch_add(1, std::memory_order_relaxed);
            long long ar = 1;   // sprites are never zero-area
            if (isSprite) c_spr.fetch_add(1, std::memory_order_relaxed);
            else
            {   // same exact 12.4 integer cross product the cull below uses, so the buckets agree.
                // area is 2x the triangle area in (1/16 px)^2, so 1 px^2 == 512.
                const long long x0 = std::lround(gs->m_vtxQueue[0].x * 16.0f), y0 = std::lround(gs->m_vtxQueue[0].y * 16.0f);
                const long long x1 = std::lround(gs->m_vtxQueue[1].x * 16.0f), y1 = std::lround(gs->m_vtxQueue[1].y * 16.0f);
                const long long x2 = std::lround(gs->m_vtxQueue[2].x * 16.0f), y2 = std::lround(gs->m_vtxQueue[2].y * 16.0f);
                ar = std::llabs((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0));
                if (ar == 0) c_degen.fetch_add(1, std::memory_order_relaxed);
                else if (ar < 512) c_tiny.fetch_add(1, std::memory_order_relaxed);
            }
            // fully outside the scissor rect -> the GS draws nothing, but we pay the whole record path
            if (mxx < (float)ctx.scissor.x0 || mnx > (float)ctx.scissor.x1 ||
                mxy < (float)ctx.scissor.y0 || mny > (float)ctx.scissor.y1)
            {
                c_off.fetch_add(1, std::memory_order_relaxed);
                if (ar != 0) c_offlive.fetch_add(1, std::memory_order_relaxed);   // the real prize
            }
            if (allA0 && gs->m_prim.abe) c_a0.fetch_add(1, std::memory_order_relaxed);
            if (!tme) c_notme.fetch_add(1, std::memory_order_relaxed);

            static std::atomic<uint64_t> s_lastNs{0};
            const uint64_t nowNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch()).count();
            uint64_t prev = s_lastNs.load(std::memory_order_relaxed);
            if (prev == 0) s_lastNs.compare_exchange_strong(prev, nowNs, std::memory_order_relaxed);
            else if (nowNs - prev >= 1000000000ull &&
                     s_lastNs.compare_exchange_strong(prev, nowNs, std::memory_order_relaxed))
            {
                const double dt = (double)(nowNs - prev) / 1e9;
                const uint64_t t = c_tot.exchange(0), sp = c_spr.exchange(0), dg = c_degen.exchange(0),
                               ti = c_tiny.exchange(0), of = c_off.exchange(0), a0 = c_a0.exchange(0),
                               nt = c_notme.exchange(0), ol = c_offlive.exchange(0);
                const auto pct = [t](uint64_t n) { return t ? 100.0 * (double)n / (double)t : 0.0; };
                std::fprintf(stderr,
                    "[primcensus] %.0f prim/s | sprite %.1f%% tri %.1f%% | degenerate %.1f%% subpixel %.1f%% "
                    "OFFSCREEN %.1f%% (LIVE %.1f%%) alpha0 %.1f%% untextured %.1f%%\n",
                    (double)t / dt, pct(sp), pct(t - sp), pct(dg), pct(ti), pct(of), pct(ol), pct(a0), pct(nt));
            }
        }
    }

    // [scissorcull] EARLY OFF-SCISSOR CULL (default ON, PS2X_SCISSORCULL=0 restores).
    //
    // A primitive whose bounding box lies entirely outside the scissor rect draws NOTHING on real
    // hardware -- the GS rejects it -- but we were recording it in full: the record path resolves
    // its texture, builds the DrawCmd, batches it and ships it to the GL thread, which hands it to
    // the GPU, which then discards it against the same rect (recordDrawCmd copies the scissor into
    // cmd.sx/sy/sw/sh further down). Nothing downstream rejected it first; this is the only place
    // that does.
    //
    // Measured by [primcensus] over 41 in-fight seconds: 9.0% of all primitives are fully
    // off-scissor, of which 6.6% of the total are NOT already dropped by the zero-area cull below
    // (the two sets correlate -- distant geometry is both more likely to be off-screen and more
    // likely to be a micro-triangle -- so the exclusive figure is the one to quote, not the 9.0%).
    // That is ~3% of the guest thread: the cull saves recpre+rectex+recbuild+push (433 of 899
    // ns/prim on a slow machine) but NOT vu1/gif/vif, which have already run by the time we get
    // here. Modest, but provably pixel-identical and it stacks with everything else.
    //
    // Deliberately AFTER the census above so the census still sees every primitive, and BEFORE the
    // zero-area cull so the cheaper test runs first.
    {
        static const bool s_scCull = [](){ const char *v = std::getenv("PS2X_SCISSORCULL");
                                           return !(v && v[0] == '0'); }();
        if (s_scCull)
        {
            const int nvS = isSprite ? 2 : 3;
            float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
            for (int i = 0; i < nvS; ++i)
            {
                const GSVertex &v = gs->m_vtxQueue[i];
                const float sx = v.x - ofxF, sy = v.y - ofyF;   // window coords, as SCISSOR is
                mnx = std::min(mnx, sx); mxx = std::max(mxx, sx);
                mny = std::min(mny, sy); mxy = std::max(mxy, sy);
            }
            // SCISSOR is inclusive on both edges (x0..x1), hence >= / <= rather than > / <.
            if (mxx < (float)ctx.scissor.x0 || mnx > (float)ctx.scissor.x1 ||
                mxy < (float)ctx.scissor.y0 || mny > (float)ctx.scissor.y1)
            {
                extern std::atomic<uint64_t> g_scissorCulled;
                g_scissorCulled.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }

    // EARLY ZERO-AREA CULL (default ON, PS2X_KEEPDOTS=1 keeps them): ~90% of fight
    // triangles are collapsed micro-tris whose integer coords are identical after 12.4
    // truncation — GL rasterizes nothing for them. Culling HERE (before the per-draw
    // texture-cache resolution, CLUT checks and probe blocks, which all cost mutexes and
    // hashing) removes the dominant per-draw overhead on the kick worker.
    if (!isSprite)
    {
        static const bool s_keepDots = [](){ const char *v = std::getenv("PS2X_KEEPDOTS"); return v && v[0] && v[0] != '0'; }();
        if (!s_keepDots)
        {
            // PS2X_SUBPIXCULL (default ON): do the zero-area test at the GS's own 12.4 sub-pixel
            // precision instead of on truncated whole pixels.
            //
            // The integer test's premise -- "GL rasterizes nothing for them" -- held only while
            // the POSITIONS handed to GL were truncated too. PS2X_SUBPIXEL (default ON since
            // 2026-08-24) now passes the fractional coords through, so a triangle whose three
            // vertices merely land inside the SAME pixel can still have real sub-pixel area and
            // still cover a pixel centre. Truncating first collapses it to zero and drops it.
            // Distant characters are almost entirely such micro-triangles, so the further the
            // opponent is the more of them are culled and the more the model breaks up -- an
            // artifact console does not have, because the GS rasterizes at 1/16 pixel.
            // Measured on ref_native: the old test culled 39.2% of all triangles, a correct
            // sub-pixel test culls 24.9%, so 14.3% of every triangle in the scene was being
            // thrown away wrongly.
            //
            // vtx.x is raw12_4/16.0f and 16 is a power of two, so x*16 recovers the ORIGINAL
            // 12.4 integer exactly; the cross product is then exact integer math in int64 (terms
            // reach ~65535^2). Only genuinely degenerate triangles -- real lines and points --
            // are culled now. PS2X_SUBPIXCULL=0 restores the truncating test.
            static const bool s_subCull = [](){ const char *v = std::getenv("PS2X_SUBPIXCULL");
                                                return !(v && v[0] == '0'); }();
            if (s_subCull)
            {
                const long long x0 = std::lround(gs->m_vtxQueue[0].x * 16.0f);
                const long long y0 = std::lround(gs->m_vtxQueue[0].y * 16.0f);
                const long long x1 = std::lround(gs->m_vtxQueue[1].x * 16.0f);
                const long long y1 = std::lround(gs->m_vtxQueue[1].y * 16.0f);
                const long long x2 = std::lround(gs->m_vtxQueue[2].x * 16.0f);
                const long long y2 = std::lround(gs->m_vtxQueue[2].y * 16.0f);
                const long long area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
                // SLIVER GUARD (regression fix): a triangle collapses to zero INTEGER area in two
                // different ways -- (a) it is genuinely tiny, all three vertices inside one pixel,
                // which is the distant-character case worth rescuing, and (b) it is a long thin
                // SLIVER whose vertices merely truncate collinear. Rescuing (b) as well made the
                // full-power explosion paint large dark opaque slashes. Only trust the sub-pixel
                // area for compact triangles; anything spanning more than a few pixels falls back
                // to the old truncating test, which historically suppressed that class.
                const long long bw = std::max({x0, x1, x2}) - std::min({x0, x1, x2});
                const long long bh = std::max({y0, y1, y2}) - std::min({y0, y1, y2});
                static const long long s_slivMax = [](){ const char *v = std::getenv("PS2X_SLIVERPX");
                                                         return v && v[0] ? (long long)std::atoll(v) * 16 : 64LL; }();
                const bool compact = (bw <= s_slivMax && bh <= s_slivMax);   // 12.4 units: 64 = 4 px
                // The sliver guard exists to suppress the full-power-explosion slashes, but the
                // routed cel/outline pass IS the long-thin-sliver class by construction -- 40%
                // of its triangles are non-compact -- and truncating their coordinates to whole
                // pixels declares them degenerate when they have real sub-pixel area. Trust the
                // exact 12.4 area for them; the conservative edge test below then covers them.
                extern bool g_swDirtyActive;
                static const bool s_keepSliv = [](){ const char *v = std::getenv("PS2X_SWOSLIVER");
                                                     return !(v && v[0] == '0'); }();
                if (!compact && !(s_keepSliv && g_swDirtyActive))
                {
                    const int ix0 = static_cast<int>(gs->m_vtxQueue[0].x), iy0 = static_cast<int>(gs->m_vtxQueue[0].y);
                    const int ix1 = static_cast<int>(gs->m_vtxQueue[1].x), iy1 = static_cast<int>(gs->m_vtxQueue[1].y);
                    const int ix2 = static_cast<int>(gs->m_vtxQueue[2].x), iy2 = static_cast<int>(gs->m_vtxQueue[2].y);
                    if (((ix1 - ix0) * (iy2 - iy0) - (iy1 - iy0) * (ix2 - ix0)) == 0)
                        return true;   // non-compact and integer-degenerate: the sliver class
                }
                // [cullstat] PS2X_CULLSTAT=1: how many triangles the two tests disagree on, i.e.
                // how many the old test was throwing away. Reported every 200k triangles.
                static const bool s_cs = [](){ const char *v = std::getenv("PS2X_CULLSTAT");
                                               return v && v[0] && v[0] != '0'; }();
                if (s_cs)
                {
                    const int ix0 = static_cast<int>(gs->m_vtxQueue[0].x), iy0 = static_cast<int>(gs->m_vtxQueue[0].y);
                    const int ix1 = static_cast<int>(gs->m_vtxQueue[1].x), iy1 = static_cast<int>(gs->m_vtxQueue[1].y);
                    const int ix2 = static_cast<int>(gs->m_vtxQueue[2].x), iy2 = static_cast<int>(gs->m_vtxQueue[2].y);
                    const bool oldCull = ((ix1 - ix0) * (iy2 - iy0) - (iy1 - iy0) * (ix2 - ix0)) == 0;
                    static std::atomic<unsigned long> s_tot{0}, s_old{0}, s_new{0}, s_saved{0};
                    const unsigned long n = s_tot.fetch_add(1) + 1;
                    if (oldCull) s_old.fetch_add(1);
                    if (area == 0) s_new.fetch_add(1);
                    if (oldCull && area != 0) s_saved.fetch_add(1);
                    if ((n % 200000ul) == 0ul)
                        std::fprintf(stderr, "[cullstat] tris=%lu  old-test culled=%lu (%.1f%%)  "
                                             "subpixel culled=%lu (%.1f%%)  RESCUED=%lu (%.1f%%)\n",
                                     n, s_old.load(), 100.0 * s_old.load() / n,
                                     s_new.load(), 100.0 * s_new.load() / n,
                                     s_saved.load(), 100.0 * s_saved.load() / n);
                }
                {   // PS2X_CULLRAMP=1: cull rate for BT3's cel/outline pass specifically.
                    // Console's own stream has 1936 exactly-degenerate triangles out of 8067
                    // for this pass (24.0%), so a faithful renderer should cull about that many
                    // and no more. Anything above that is outline geometry we are throwing away.
                    static const bool s_cr = [](){ const char *v = std::getenv("PS2X_CULLRAMP");
                                                   return v && v[0] && v[0] != '0'; }();
                    if (s_cr && ctx.tex0.tbp0 == 15680u)
                    {
                        static unsigned long nSeen = 0, nCull = 0, nSliv = 0;
                        ++nSeen; if (area == 0) ++nCull;
                        if (!compact) ++nSliv;
                        if ((nSeen % 8000ul) == 0ul)
                            std::fprintf(stderr, "[cullramp] tbp15680 tris=%lu  zero-area culled=%lu "
                                                 "(%.1f%%)  non-compact=%lu (%.1f%%)   "
                                                 "(console: 24.0%% degenerate)\n",
                                         nSeen, nCull, 100.0 * nCull / nSeen,
                                         nSliv, 100.0 * nSliv / nSeen);
                    }
                }
                if (area == 0)
                    return true; // genuinely degenerate: a line or a point
            }
            else
            {
            const int x0 = static_cast<int>(gs->m_vtxQueue[0].x), y0 = static_cast<int>(gs->m_vtxQueue[0].y);
            const int x1 = static_cast<int>(gs->m_vtxQueue[1].x), y1 = static_cast<int>(gs->m_vtxQueue[1].y);
            const int x2 = static_cast<int>(gs->m_vtxQueue[2].x), y2 = static_cast<int>(gs->m_vtxQueue[2].y);
            if ((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0) == 0)
                return true; // degenerate: draws no pixels
            }
        }
    }

    // ---- GS depth (Z) capture, behind PS2X_GPU_DEPTH (default OFF) ----
    // When off, none of the DrawCmd depth fields are written (they keep their false/ALWAYS
    // defaults) and no vertex z is set, so the GPU replay path is byte-for-byte unchanged.
    static const bool s_depthOn = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DEPTH"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
    bool zTestEnable = false;   // TEST.ZTE
    uint8_t zTestFunc = 1u;     // TEST.ZTST (0=NEVER,1=ALWAYS,2=GEQUAL,3=GREATER)
    bool zWrite = false;        // ZBUF.ZMSK==0 -> z-write enabled
    double zMax = 4294967295.0; // 2^bits - 1 for the ZBUF PSM
    if (s_depthOn)
    {
        const uint64_t test = ctx.test;
        zTestEnable = ((test >> 16) & 1u) != 0u;      // bit16  ZTE
        zTestFunc = static_cast<uint8_t>((test >> 17) & 3u); // bits17-18 ZTST
        zWrite = !ctx.zbuf.zmask;                     // ZBUF.ZMSK==1 -> writes disabled
        // The GS depth COMPARISON is on the raw 32-bit Z value regardless of ZBUF.psm
        // (psm only truncates STORAGE). Normalizing per-psm re-scales passes by different
        // constants, which INVERTS depth ordering between passes when a game mixes Z
        // formats (BT3 fights: far backdrop normalized /2^24 came out "nearer" than
        // ground normalized /2^32 — the arena wedge artifacts). Use one constant so the
        // normalized ordering always matches the hardware's raw-integer ordering.
        // PS2X_ZPSMNORM=1 restores the old per-psm behavior for A/B.
        static const bool s_perPsm = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_ZPSMNORM"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_perPsm)
        {
            switch (ctx.zbuf.psm)                     // psm = ((v>>24)&0xF)|0x30 (see GS::writeRegister)
            {
            case GS_PSM_Z32: zMax = 4294967295.0; break;  // 2^32-1
            case GS_PSM_Z24: zMax = 16777215.0; break;    // 2^24-1
            case GS_PSM_Z16:
            case GS_PSM_Z16S: zMax = 65535.0; break;      // 2^16-1
            default: zMax = 4294967295.0; break;
            }
        }
        else
            zMax = 4294967295.0;
    }
    // GS Z is an integer where LARGER = NEARER. Normalize to [0,1]; the replay clears depth
    // to 0.0 (far) and uses GL_GREATER/GEQUAL so the larger value wins (stored directly).
    // PS2X_ZSCALE=<f> (default 1): spread the normalized depth over more of the buffer.
    // BT3's raw Z here is ~112,000 out of 2^32, so the whole scene lands in the bottom
    // 0.0026% of [0,1] and adjacent draws differ by ~1e-8 -- a fraction of ONE bucket in a
    // 24-bit depth buffer. They quantize together, and wherever the cel/outline pass rounds
    // one bucket below the body it fails GEQUAL and the line is dropped (measured: 25% of
    // outline runs missing; disabling depth entirely recovers them). The GS compares RAW
    // integers, so any monotonic rescale preserves ordering.
    static const double s_zScale = [](){ const char *v = std::getenv("PS2X_ZSCALE");
                                         const double f = v ? std::atof(v) : 0.0;
                                         return (f > 0.0) ? f : 1.0; }();
    auto zNorm = [&](const GSVertex &v) -> float {
        double d = (v.z / zMax) * s_zScale;
        if (d < 0.0) d = 0.0; else if (d > 1.0) d = 1.0;
        return static_cast<float>(d);
    };

    // --- shared texture setup: key + one-time detexture to linear RGBA ---
    uint64_t texKey = 0;
    gprof::mark(gprof::REC_TEX);   // [guestprof]
    int texW = 1, texH = 1;
    // [rawmask] PS2X_RAWMASK=1: alpha-as-data draws (RGB fully masked, alpha open) decode texel
    // alpha RAW. The GS alpha byte they deposit is a palette index downstream (BT3's outline
    // mask chain), and the blend-oriented *255/128 expansion corrupts it -- the fbp224 mask held
    // 253 (= expanded CLUT alpha 127) instead of the per-material IDs console shows (1..24).
    // Scoped: these draws write no RGB, so raw alpha cannot change any visible blending.
    // [rectemplate] the texture-resolve phase below depends only on GS register state (m_stateGen), the renderer's VRAM
    // upload / writeback / eviction stamps (recTplEpoch) and the frame number ([zwbwant] stamps once per frame), so
    // consecutive primitives with identical inputs reuse the previous result. PS2X_RECTPL=0 disables; auto-off when a
    // feature folds VERTEX data into the key (PS2X_SUBDECODE) or FBO draw seqs into the version (PS2X_FBODIRTY).
    static const bool s_tplOn = [](){ const char *v = std::getenv("PS2X_RECTPL"); if (v && v[0] == '0') return false;
                                       const char *a = std::getenv("PS2X_SUBDECODE"); return !(a && a[0] && a[0] != '0'); }();
    // PS2X_FBODIRTY (a main.cpp default) folds the FBO draw seqs of the texture's source pages into the version check;
    // those move only when a draw lands in a source page (self-feedback), so the template carries their max (drawStamp).
    struct RecTpl { bool valid = false; uint32_t gen = 0; uint64_t epoch = 0, frame = 0; uint8_t type = 0; bool tme = false, fst = false; uint64_t texKey = 0; int texW = 1, texH = 1;
                    uint32_t pageLo = 1, pageHi = 0; uint64_t drawStamp = 0; };
    uint32_t tplPageLo = 1, tplPageHi = 0;   // empty range unless the body computes a texture footprint
    static thread_local RecTpl s_tpl;
    extern std::atomic<uint64_t> g_bt3FrameCount;
    const uint64_t tplFrame = g_bt3FrameCount.load(std::memory_order_relaxed);
    bool tplHit = false;
    if (s_tplOn && s_tpl.valid)
    {   // miss-reason census (g_recTplWhy: 0 gen, 1 epoch, 2 frame, 3 prim/tme/fst, 4 drawStamp) under PS2X_GUESTPROF=1
        int why = -1;
        if (s_tpl.gen != gs->m_stateGen + gs->m_texUploadGen * 0x9E3779B1u) why = 0;   // m_texUploadGen: uploads + the grass VRAM swap
        else if (s_tpl.epoch != ps2GpuRenderer().recTplEpoch()) why = 1;
        else if (s_tpl.frame != tplFrame) why = 2;
        else if (s_tpl.type != (uint8_t)gs->m_prim.type || s_tpl.tme != tme || s_tpl.fst != fst) why = 3;
        else if (s_tpl.drawStamp != ps2GpuRenderer().pageDrawStamp(s_tpl.pageLo, s_tpl.pageHi)) why = 4;
        tplHit = (why < 0);
        if (why >= 0 && gprof::g_on) g_recTplWhy[why].fetch_add(1, std::memory_order_relaxed);
    }
    if (tplHit) { texKey = s_tpl.texKey; texW = s_tpl.texW; texH = s_tpl.texH; g_recTplHit.fetch_add(1, std::memory_order_relaxed); }
    else
    {
    static bool g_rawAlphaDec_s = false;
    {
        static const bool s_rawMask = [](){ const char *v = std::getenv("PS2X_RAWMASK");
                                            return v && v[0] && v[0] != '0'; }();
        g_rawAlphaDec_s = s_rawMask && ((ctx.frame.fbmsk & 0x00ffffffu) == 0x00ffffffu);
    }
    const bool rawAlphaDec = g_rawAlphaDec_s;
    bool deferTex = false, deferClut = false, deferAlpha = false;   // [deferdec]
    if (tme)
    {
        const auto &tex = ctx.tex0;
        // Read-after-write barrier (PS2X_BARRIER, GPU path): if an earlier draw of this frame
        // wrote the page we are about to sample, its pixels are still sitting in an FBO and
        // VRAM holds stale bytes. Render what is built so far and push that page back to VRAM
        // first, so the decode below sees what the GS would have seen. Must happen BEFORE the
        // key/decode -- afterwards is too late for the very draw that needed it.
        if (GsGpuRenderer::enabled())
        {
            {   static const bool s_gi = [](){ const char *v = std::getenv("PS2X_GSID");
                                               return v && v[0] && v[0] != '0'; }();
                if (s_gi && tex.psm == GS_PSM_T8H) { static int n = 0; if (n++ < 3)
                    std::fprintf(stderr, "[gsid] PSMT8H decode gs=%p tbp0=%u tbw=%u tw=%d th=%d\n",
                                 (void*)gs, tex.tbp0, tex.tbw, 1<<tex.tw, 1<<tex.th); } }
            // "Wants this page's ALPHA as data" is not a PSMT8H-only property. BT3 builds its
            // mask TWICE per frame from the same scene page: cycle 1 reads it as PSMT8H (kicks
            // 15051..15082, before any character is drawn) and cycle 2 as PSMCT32 (kicks
            // 35501..35532, after all 10055 character kicks). Only the first was flagged, so
            // PS2X_BARKEEPA protected the alpha on the cycle-2 flush and the character
            // silhouette -- which by then sits in the scene FBO's alpha -- never reached VRAM.
            // The mask, and the outline built from it, therefore held terrain depth only.
            // A PSMCT32 sample reads all four channels by definition, so withholding alpha
            // hands it data that is wrong by construction.
            static const bool s_ct32a = [](){ const char *v = std::getenv("PS2X_CT32ALPHA");
                                              return v && v[0] && v[0] != '0'; }();
            const bool wantsAlpha = (tex.psm == GS_PSM_T8H) ||
                                    (s_ct32a && tex.psm == GS_PSM_CT32);
            { extern uint32_t g_barReqTbp, g_barReqCbp, g_barReqPsm, g_barReqTbw; g_barReqTbp = tex.tbp0; g_barReqCbp = tex.cbp; g_barReqPsm = tex.psm; g_barReqTbw = tex.tbw; }
            const bool deferOk = s_deferDec && GSRasterizer::decodeIsDeferrable(tex.psm);   // [deferdec]
            {   // [gpualias] mode>=4: the CT16-view read of fbp336 is served from the GPU view
                // texture (renderer-side flip) -- the VRAM this barrier would flush+decode is
                // never consumed for those draws, so skip the whole flush/readback round-trip.
                static const int s_ga4 = [](){ const char *v = std::getenv("PS2X_GPUALIAS"); return v && v[0] ? std::atoi(v) : 0; }();
                static const bool s_noskip = [](){ const char *v = std::getenv("PS2X_GPUALIAS_NOSKIP"); return v && v[0] && v[0] != '0'; }();   // A/B: keep the flush (its VRAM bytes feed OTHER readers)
                static const bool s_aliasZon = [](){ const char *v = std::getenv("PS2X_ALIASZ"); if (v && v[0] && v[0] != '0') return true;
                                                      const char *w = std::getenv("PS2X_ALIASZSW"); return w && w[0] && w[0] != '0'; }();   // [dofmask] ALIASZSW = barrier/SW side only (GPU gate keeps dropping -> no stripes)
                const bool gaZ16Dropped = !s_aliasZon && tex.tbp0 == 7168u &&
                                          (tex.psm == 0x30u || tex.psm == 0x31u || tex.psm == 0x32u);   // kind1 reads: the draw is ALIASSKIP-dropped, decode+flush feed nothing
                static const bool s_aoFbo2 = [](){ const char *v = std::getenv("PS2X_ALPHAONLYFBO"); return v && v[0] && v[0] != '0'; }();
                static const bool s_sceneSkip = [](){ const char *v = std::getenv("PS2X_SCENESKIP"); return v && v[0] && v[0] != '0'; }();
                const bool gaSceneCT32 = s_sceneSkip &&
                                         (tex.tbp0 == 0u || tex.tbp0 == 3584u) &&
                                         (tex.psm == 0x00u || tex.psm == 0x01u);   // [sceneskip] scene CT32 reads are FBO-served; with alpha-only served + Z16S dropped, the scene flush feeds nothing
                static const bool s_f336Skip = [](){ const char *v = std::getenv("PS2X_F336SKIP"); return v && v[0] && v[0] != '0'; }();
                const bool gaF336Self = s_f336Skip && tex.tbp0 == 10752u &&
                                        (tex.psm == 0x00u || tex.psm == 0x01u);   // [f336skip] the DoF chain reading back its own downsample buffer (fbw4 CT32/CT24) -- FBO-served RT self-reads; measured as ALL of f336's dirty barriers (bargate336: 250/250 psm 0/1, zero T8)
                const bool gaAlphaOnlyFbo = s_aoFbo2 &&
                                            gs->activeContext().frame.fbmsk == 0x00FFFFFFu &&
                                            (tex.tbp0 == 0u || tex.tbp0 == 3584u) &&
                                            (tex.psm == 0x00u || tex.psm == 0x01u);   // [alphaonlyfbo] the renderer serves these from the rtsnap FBO copy; the scene flush feeds nothing for them
                static const bool s_mbSkip = [](){ const char *v = std::getenv("PS2X_MASKBUILDSKIP"); return v && v[0] && v[0] != '0'; }();
                const bool gaMaskBuild = s_mbSkip && tex.psm == GS_PSM_T8H && (tex.tbp0 == 0u || tex.tbp0 == 3584u);   // [idxrt] mask-build reads served from the scene FBO (pair with PS2X_IDXRT=1 PS2X_IDXONLY=1)
                static const std::vector<uint32_t> s_shcCbps = [](){
                    // PS2X_SHCOMPSKIP: "1" = {15972} (the NOSHCOMP-neutered composite class);
                    // or an explicit comma list of cbps whose f224 PSMT8H reads are FBO-served
                    // (must mirror the renderer's idxOnlyGate serve set!).
                    std::vector<uint32_t> v; const char *e = std::getenv("PS2X_SHCOMPSKIP");
                    if (!e || !e[0] || e[0] == '0') return v;
                    if (!std::strchr(e, ',')) { if (std::atoi(e) == 1) { v.push_back(15972u); return v; } }
                    const char *p = e;
                    while (*p) { char *q = nullptr; unsigned long x = std::strtoul(p, &q, 10); if (q == p) break; v.push_back((uint32_t)x); p = (*q == ',') ? q + 1 : q; }
                    return v; }();
                const bool gaShcomp = !s_shcCbps.empty() && tex.psm == GS_PSM_T8H && tex.tbp0 == 7168u &&
                                      std::find(s_shcCbps.begin(), s_shcCbps.end(), tex.cbp) != s_shcCbps.end();
                const bool gaServed = !s_noskip && s_ga4 >= 4 && ((tex.tbp0 == 10752u && (tex.psm == 0x02u || tex.psm == 0x0Au))
                    // ink-composite signature ONLY (must mirror the renderer flip): other CT16
                    // readers of page 336 (HUD composite, menus) still need the VRAM round-trip.
                    && gs->m_texa.aem && gs->m_texa.ta1 == 0u
                    && (gs->m_texa.ta0 == 0x30u ||
                        (gs->m_texa.ta0 == 0x80u && (gs->activeContext().frame.fbmsk & 0x00FFFFFFu) == 0x00FFFFFFu))
                    || gaZ16Dropped || gaAlphaOnlyFbo || gaSceneCT32 || gaF336Self || gaMaskBuild || gaShcomp);
                if (!gaServed)
                {
                    {   // [gpualias] census the CT16 readers of f336 we do NOT serve (the striping class?)
                        static const int s_gx = [](){ const char *v = std::getenv("PS2X_GPUALIAS"); return v && v[0] ? std::atoi(v) : 0; }();
                        if (s_gx >= 4 && tex.tbp0 == 10752u && (tex.psm == 0x02u || tex.psm == 0x0Au))
                        {
                            static unsigned long n = 0;
                            if (++n <= 12 || (n % 500ul) == 0ul)
                                std::fprintf(stderr, "[gaunserved] #%lu ta0=%02x ta1=%02x aem=%d dest=f%u dpsm=%u fbmsk=%08x prim=%u tw=%u th=%u\n",
                                             n, (unsigned)gs->m_texa.ta0, (unsigned)gs->m_texa.ta1, gs->m_texa.aem ? 1 : 0,
                                             gs->activeContext().frame.fbp, (unsigned)gs->activeContext().frame.psm,
                                             gs->activeContext().frame.fbmsk, (unsigned)gs->m_prim.type,
                                             1u << gs->activeContext().tex0.tw, 1u << gs->activeContext().tex0.th);
                        }
                    }
                    {   // [barwho2] PS2X_BARWHO2=1: per-(tbp,cbp) census of the PSMT8H reads that
                        // still take the VRAM round-trip -- sizes the NOSHCOMP-neutered share.
                        static const bool s_bw2 = [](){ const char *v = std::getenv("PS2X_BARWHO2"); return v && v[0] && v[0] != '0'; }();
                        if (s_bw2 && tex.psm == GS_PSM_T8H)
                        {
                            static std::map<uint64_t, unsigned long> h; static unsigned long n = 0;
                            ++h[((uint64_t)tex.tbp0 << 32) | tex.cbp];
                            if (++n <= 16 || (n % 400ul) == 0ul)
                            {
                                std::fprintf(stderr, "[barwho2] #%lu tbp=%u cbp=%u dest=f%u fbmsk=%08x prim=%u |", n,
                                             tex.tbp0, tex.cbp, (unsigned)gs->activeContext().frame.fbp,
                                             gs->activeContext().frame.fbmsk, (unsigned)gs->m_prim.type);
                                for (auto &kv : h) std::fprintf(stderr, " %u/%u=%lu", (unsigned)(kv.first >> 32), (unsigned)(kv.first & 0xffffffffu), kv.second);
                                std::fprintf(stderr, "\n");
                            }
                        }
                    }
                    if (!p8twinServedRead(gs->activeContext().frame.fbp, tex) && !rtServedRead(gs->activeContext().frame.fbp, tex))
                    {
                        extern std::atomic<unsigned long> g_barReqPushed;
                        const unsigned long before = g_barReqPushed.load(std::memory_order_relaxed);
                        ps2GpuRenderer().barrierBeforeRead(tex.tbp0, true, wantsAlpha, deferOk ? &deferTex : nullptr);
                        if (g_barReqPushed.load(std::memory_order_relaxed) != before)
                            reqCensus(3, tex.tbp0, tex.psm, 1 << tex.tw, 1 << tex.th, gs->activeContext().frame.fbp);
                    }
                }
            }
            deferAlpha = wantsAlpha;
            // The PALETTE too. BT3's outline/shadow CLUTs live in pages 499-500, which the game
            // RENDERS into -- so their VRAM copy is stale and every composite decoded a dead
            // palette (129 distinct indices collapsing to 1 alpha). The CLUT base is not
            // page-aligned, so flush the page that contains it.
            const uint32_t p2 = tex.psm;
            if (p2 == GS_PSM_T8 || p2 == GS_PSM_T8H || p2 == GS_PSM_T4 ||
                p2 == GS_PSM_T4HL || p2 == GS_PSM_T4HH)
                ps2GpuRenderer().barrierBeforeRead(tex.cbp, false, false, deferOk ? &deferClut : nullptr);
        }
        {   // PS2X_RAMPCLUT=1: which CLUT do we select for BT3's cel/outline ramp? Console
            // drives tbp 15680 with SIX different palettes -- 15620/15628/15632/15640/15616/
            // 15624 -- of very different amplitude (max luminance 61/28/71/49/36/24), one per
            // material. Collapsing them onto one palette would shade every material with the
            // same ramp. Census what we actually pick, to compare against the stream.
            static const bool s_rc = [](){ const char *v = std::getenv("PS2X_RAMPCLUT");
                                           return v && v[0] && v[0] != '0'; }();
            if (s_rc && tex.tbp0 == 15680u)
            {
                static std::map<uint32_t, unsigned long> seen;
                static unsigned long n = 0;
                ++seen[tex.cbp];
                if ((++n % 4000ul) == 1ul)
                {
                    std::fprintf(stderr, "[rampclut] n=%lu  cbp:", n);
                    for (auto &kv : seen) std::fprintf(stderr, " %u=%lu", kv.first, kv.second);
                    std::fprintf(stderr, "\n");
                }
            }
        }
        texW = 1 << tex.tw; texH = 1 << tex.th;
        if (texW <= 0) texW = 1;
        if (texH <= 0) texH = 1;
        g_subDx0 = 0; g_subDxW = 0;
        {   // [subdecode] PS2X_SUBDECODE=1: BT3's post chain samples 512x448 RT pages in 16 column
            // strips, and every barrier flush invalidates the page -- so each strip re-decoded and
            // re-uploaded the WHOLE page (~290 GL texture creations/frame). Decode only the strip's
            // U window (+1 texel for bilinear), keyed by the window; the sprite's UVs are remapped.
            // STATUS 2026-08-27: NOT viable as-is -- 45k GL textures/frame (every wide-texture sprite
            // gets a window key) and 45% of pixels change (renderer heuristics key on srcTexW).
            // Would need: apply only to RT-page sources, and a stable identity separate from srcTexW.
            static const bool s_sub = [](){ const char *v = std::getenv("PS2X_SUBDECODE"); return v && v[0] && v[0] != '0'; }();
            const uint32_t wms = (uint32_t)(ctx.clamp & 3u);
            if (s_sub && gs->m_prim.tme && texW >= 256 && texH >= 64 && wms == 1u)
            {
                auto uvOf = [&](const GSVertex &v) -> float {
                    if (gs->m_prim.fst) return static_cast<float>(v.u >> 4);
                    const float q = (v.q != 0.0f) ? v.q : 1.0f; return (v.s / q) * texW; };
                const float ua = uvOf(gs->m_vtxQueue[0]), ub = uvOf(gs->m_vtxQueue[1]);
                int lo = (int)std::floor(std::min(ua, ub)) - 1, hi = (int)std::ceil(std::max(ua, ub)) + 1;
                lo = std::max(0, lo); hi = std::min(texW, hi);
                if (hi > lo && (hi - lo) * 4 <= texW) { g_subDx0 = lo; g_subDxW = hi - lo; }
            }
        }
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t val) { h = (h ^ val) * 1099511628211ull; };
        mix(tex.tbp0); mix(tex.tbw); mix(tex.psm); mix(tex.tw); mix(tex.th);
        mix(tex.cbp); mix(tex.cpsm); mix(tex.csa); mix(tex.csm);
        mix(gs->m_texclut.cbw); mix(gs->m_texclut.cou); mix(gs->m_texclut.cov);
        // Indexed (T4/T8) textures: the palette CONTENT can change under the same cbp
        // (reused CLUT region), and that change may not bump the upload gen — so a cached
        // decode goes stale (wrong colors, e.g. the logo). Fold the decoded palette into
        // the key so a palette change forces a re-decode with the correct colors. NOTE: this
        // MUST stay in the key (not a separate validity stamp) -- the popup border and fill
        // share one tbp0 but different CLUT regions in the SAME frame, so they need distinct
        // cache entries / GL textures. A stable key merges them -> border loses its shape.
        const uint32_t psmv = tex.psm;
        const bool indexed = (psmv == GS_PSM_T8 || psmv == GS_PSM_T8H ||
                              psmv == GS_PSM_T4 || psmv == GS_PSM_T4HL || psmv == GS_PSM_T4HH);
        static const bool s_noClutKey = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_NOCLUTKEY"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (indexed && !s_noClutKey)
        {
            ensureClutCache(gs);
            {   // [clut15972] PS2X_CLUTDUMP=<cbp>: print the decoded palette for one CLUT base
                static const uint32_t s_cd = [](){ const char *v = std::getenv("PS2X_CLUTDUMP"); return v && v[0] ? (uint32_t)std::atoi(v) : 0xFFFFFFFFu; }();
                static int cdn = 0;
                if (s_cd != 0xFFFFFFFFu && tex.cbp == s_cd && cdn < 3)
                {   ++cdn; unsigned long sr = 0, sg = 0, sb = 0, bright = 0; std::map<unsigned, int> ah;
                    for (int i = 0; i < 256; ++i) { const uint32_t e = gs->m_clutCache[i]; const unsigned r = e & 0xFF, g = (e >> 8) & 0xFF, b = (e >> 16) & 0xFF, a = (e >> 24) & 0xFF;
                        sr += r; sg += g; sb += b; if (std::max({r, g, b}) > 96) ++bright; ++ah[a]; }
                    std::fprintf(stderr, "[clutdump] cbp=%u csa=%u: mean RGB (%lu,%lu,%lu) bright(>96) %lu/256 | alpha distinct %zu | e[0]=%08x e[1]=%08x e[5]=%08x e[128]=%08x e[255]=%08x\n",
                                 tex.cbp, tex.csa, sr / 256, sg / 256, sb / 256, bright, ah.size(), gs->m_clutCache[0], gs->m_clutCache[1], gs->m_clutCache[5], gs->m_clutCache[128], gs->m_clutCache[255]); }
            }
            {   // [palpair] PS2X_PALPAIR=1: which palette content does each terrain draw pair with?
                // Sampled every 200th terrain draw; diff offline vs the gsparse upload-order truth.
                static const bool s_pp2 = [](){ const char *v = std::getenv("PS2X_PALPAIR"); return v && v[0] && v[0] != '0'; }();
                if (s_pp2 && tex.cbp == 12992u)
                {
                    static unsigned long pn = 0; ++pn;
                    if ((pn % 200ul) == 1ul)
                    {
                        unsigned long sum = 0;
                        for (int i2 = 0; i2 < 256; ++i2)
                        { const uint32_t e = gs->m_clutCache[i2]; sum += (e & 0xFF) + ((e >> 8) & 0xFF) + ((e >> 16) & 0xFF); }
                        std::fprintf(stderr, "[palpair] %lu %.1f\n", pn, sum / 768.0);
                    }
                }
            }
            const int nclut = (psmv == GS_PSM_T4 || psmv == GS_PSM_T4HL || psmv == GS_PSM_T4HH) ? 16 : 256;
            mix(nclut == 16 ? gs->m_clutCacheHash16 : gs->m_clutCacheHash256);   // [cluthash] same dependence, one mix
        }
        {   // [texakey] PS2X_TEXAKEY=1: the CT16/CT24 decode BAKES TEXA (TA0/TA1/AEM) into the texel
            // alpha, but the cache key ignored TEXA -- BT3's two edge-stamp classes read the same
            // CT16 edge map with TA0=48 and TA0=128 and the second got the first's decode (measured:
            // both stamp classes decoded alpha 95, so the 128-tier strokes never existed).
            static const bool s_tk = [](){ const char *v = std::getenv("PS2X_TEXAKEY"); return v && v[0] && v[0] != '0'; }();
            const bool texaBaked = (psmv == GS_PSM_CT16 || psmv == GS_PSM_CT16S || psmv == GS_PSM_CT24);
            if (s_tk && texaBaked)
            {   mix((uint32_t)gs->m_texa.ta0 | ((uint32_t)gs->m_texa.ta1 << 8) | ((uint32_t)(gs->m_texa.aem ? 1u : 0u) << 16) | 0xA5000000u); }
        }
        if (g_subDxW) { mix(0x5B000000u | (uint32_t)g_subDx0); mix((uint32_t)g_subDxW); }   // [subdecode]
        texKey = h ? h : 1ull;
        if (rawAlphaDec) texKey ^= 0x9E3779B97F4A7C15ull;   // [rawmask] raw-alpha decode variant

        {   // [texreplace] PS2X_TEXNAME=1: emit the PCSX2-COMPATIBLE identity for every decoded
            // texture, one line per unique name. Hooked HERE because this is where TEX0 and TEXA
            // are both in scope. Validation is exact and needs no game knowledge: compare these
            // against the 17,398 real PCSX2 dumps for this title in
            // ~/.config/PCSX2/textures/SLUS-21678/dumps -- a match means our VRAM layout, block
            // order and hashing all agree with PCSX2's, which is the whole prerequisite for
            // loading its replacement packs.
            static const bool s_tn = [](){ const char *v = std::getenv("PS2X_TEXNAME"); return v && v[0] && v[0] != '0'; }();
            if (s_tn)
            {
                ps2tex::TexIdent id;
                const bool paletted = (ctx.tex0.psm == 19 || ctx.tex0.psm == 20);
                if (ps2tex::identify(gs->vramData(), ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm,
                                     ctx.tex0.tw, ctx.tex0.th,
                                     paletted ? gs->m_clutCache : nullptr,
                                     gs->m_texa.ta0, gs->m_texa.aem, gs->m_texa.ta1, id))
                {
                    static std::unordered_set<std::string> s_seen;
                    const std::string nm = id.name();
                    if (s_seen.insert(nm).second)
                        std::fprintf(stderr, "[texname] %s\n", nm.c_str());
                }
            }
        }

        // PS2X_SKYKICK (record-side): the first SKY-panorama triangle (tbp0=10752 1024x256)
        // dumps its originating VU1 kick — entry PC, TOP, kick addr, full VU data memory and
        // microcode — for offline dissection of the collapsed sky transform.
        {
            static const bool s_sky = [](){ const char *v = std::getenv("PS2X_SKYKICK"); return v && v[0] && v[0] != '0'; }();
            // Companion capture: a WORKING chunk (256x256 grass terrain, same program) from
            // the same scene — diffing its matrix vs the sky's isolates the EE-side breakage.
            if (s_sky && tex.tbp0 == 10752u && texW == 256 && texH == 256)
            {
                extern thread_local uint32_t g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr;
                extern thread_local const uint8_t *g_xgkickVuData;
                extern thread_local uint32_t g_xgkickVuDataSize;
                static std::atomic<bool> s_gDumped{false};
                bool exp1 = false;
                if (g_xgkickVuData && s_gDumped.compare_exchange_strong(exp1, true))
                {
                    FILE *dm = std::fopen("/home/z3/Desktop/bt3/work/ground_data.bin", "wb");
                    if (dm) { std::fwrite(g_xgkickVuData, 1, g_xgkickVuDataSize, dm); std::fclose(dm); }
                    std::fprintf(stderr, "[groundrec] snapshot written entryPc=%u top=%u kickAddr=%u\n",
                                 g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr);
                }
            }
            if (s_sky && tex.tbp0 == 10752u && texW == 1024 && texH == 256)
            {
                extern thread_local uint32_t g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr;
                extern thread_local const uint8_t *g_xgkickVuData;
                extern thread_local uint32_t g_xgkickVuDataSize;
                extern thread_local const uint8_t *g_xgkickVuCode;
                extern thread_local uint32_t g_xgkickVuCodeSize;
                static std::atomic<uint32_t> s_n{0};
                const uint32_t n = s_n.fetch_add(1);
                if (n < 6u)
                    std::fprintf(stderr, "[skyrec] #%u entryPc=%u top=%u kickAddr=%u vuData=%p prim=%u\n",
                                 n, g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr,
                                 (const void *)g_xgkickVuData, gs->m_prim.type);
                static std::atomic<bool> s_dumped{false};
                bool exp0 = false;
                if (g_xgkickVuData && s_dumped.compare_exchange_strong(exp0, true))
                {
                    FILE *dm = std::fopen("/home/z3/Desktop/bt3/work/sky_data.bin", "wb");
                    if (dm) { std::fwrite(g_xgkickVuData, 1, g_xgkickVuDataSize, dm); std::fclose(dm); }
                    if (g_xgkickVuCode)
                    {
                        FILE *mc = std::fopen("/home/z3/Desktop/bt3/work/sky_micro.bin", "wb");
                        if (mc) { std::fwrite(g_xgkickVuCode, 1, g_xgkickVuCodeSize, mc); std::fclose(mc); }
                    }
                    std::fprintf(stderr, "[skyrec] snapshot written entryPc=%u top=%u kickAddr=%u dataBytes=%u\n",
                                 g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr, g_xgkickVuDataSize);
                }
            }
        }
        // PS2X_3DPROBE: characterize 3D scene triangles (sizeable textures) — textured vs flat,
        // texture base/format, vertex color (silhouette = flat color), and texcoords present.
        {
            static const bool s_3d = [](){ static const char *s_env = std::getenv("PS2X_3DPROBE"); return s_env; }() != nullptr;
            if (s_3d && ctx.tex0.tw >= 5u) {
                static int n3 = 0;
                if (n3++ < 40) {
                    const auto &v0 = gs->m_vtxQueue[0];
                    std::fprintf(stderr, "[3d] prim=%u tme=%d abe=%d tbp0=%u psm=%u %ux%u | col=(%u,%u,%u,%u) st=(%.3f,%.3f) uv=(%u,%u) | scr=(%.0f,%.0f)\n",
                        gs->m_prim.type, gs->m_prim.tme?1:0, gs->m_prim.abe?1:0,
                        ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tw, ctx.tex0.th,
                        v0.r, v0.g, v0.b, v0.a, v0.s, v0.t, v0.u, v0.v, v0.x, v0.y);
                }
            }
        }

        // PS2X_HUDRAW: dump the RAW GS vertices (game output, renderer-independent) + XYOFFSET for the
        // known HUD element -> is the collapse in the game's computed coords or in the offset math?
        {
            static const bool s_hr = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_HUDRAW"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            // Collapsed health bar vs the ONE HUD element that renders (survivor). Comparing prim
            // type + coords + FST shows WHY the survivor survives (2D direct coords vs VU1 transform).
            const char *hudName = (texKey == 15929281520409323171ull) ? "HEALTHBAR" :
                                  (texKey == 8060548139135559745ull) ? "SURVIVOR" :
                                  (texKey == 2747326780238319538ull) ? "FRAME" :
                                  (texKey == 5396665041240350493ull) ? "PORTRAIT" : nullptr;
            if (s_hr && hudName) {
                static int nFrame = 0, nHb = 0, nSurv = 0;
                int &cnt = (hudName[0]=='F') ? nFrame : (hudName[0]=='H') ? nHb : nSurv;
                const bool collapsed = ((int)gs->m_vtxQueue[0].x==ofx && (int)gs->m_vtxQueue[0].y==ofy)
                                    && ((int)gs->m_vtxQueue[1].x==ofx && (int)gs->m_vtxQueue[1].y==ofy);
                if (cnt++ < 6) std::fprintf(stderr, "[hudraw:%s] method=%08x destFbp=%u fbw=%u %s prim=%u fst=%u | tbp0=%u psm=%u %ux%u | px v0=(%d,%d) v1=(%d,%d) v2=(%d,%d) ofx=%d ofy=%d\n",
                                           hudName, g_bt3DrawMethod.load(std::memory_order_relaxed), ctx.frame.fbp, ctx.frame.fbw, collapsed?"COLLAPSED":"placed",
                                           gs->m_prim.type, gs->m_prim.fst, ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tw, ctx.tex0.th,
                                           ((int)gs->m_vtxQueue[0].x)>>4,((int)gs->m_vtxQueue[0].y)>>4,((int)gs->m_vtxQueue[1].x)>>4,((int)gs->m_vtxQueue[1].y)>>4,
                                           ((int)gs->m_vtxQueue[2].x)>>4,((int)gs->m_vtxQueue[2].y)>>4, ofx>>4, ofy>>4);
            }
        }

        GsGpuRenderer &r = ps2GpuRenderer();
        // VRAM page range this texture's texels occupy. tbp0 is in 64-word (256-byte)
        // blocks; a page is 2048 words = 8192 bytes. Use the ACTUAL bits-per-texel for the
        // footprint -- the old tbw*texH assumed 32bpp and over-estimated T8/T4 by 4-8x, so
        // unrelated nearby uploads falsely invalidated the font/UI textures every frame
        // (100% re-decode). H-variants (T8H/T4HL/T4HH) alias a 32bpp buffer -> use 32.
        uint32_t bpp;
        switch (tex.psm)
        {
        case GS_PSM_T4: bpp = 4u; break;
        case GS_PSM_T8: bpp = 8u; break;
        case GS_PSM_CT16: case GS_PSM_CT16S: bpp = 16u; break;
        default: bpp = 32u; break; // CT32/CT24/Z*/T8H/T4HL/T4HH (alias 32bpp)
        }
        const uint32_t footBytes = static_cast<uint32_t>(texW) * static_cast<uint32_t>(texH) * bpp / 8u;
        const uint32_t texPageLo = tex.tbp0 / 32u;
        const uint32_t texPageHi = texPageLo + (footBytes / 8192u);
        tplPageLo = texPageLo; tplPageHi = texPageHi;   // [rectemplate]
        {
            // Diagnostic (PS2X_GPU_DIAG): how many texture DECODES + texels/sec, and how
            // many textured prims/sec total -> is the cache thrashing (re-decode churn)?
            static const bool s_dc = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            if (s_dc)
            {
                static std::atomic<uint64_t> s_prims{0}, s_decodes{0}, s_texels{0};
                s_prims.fetch_add(1, std::memory_order_relaxed);
                const bool miss = !r.hasTexture(texKey, texPageLo, texPageHi);
                if (miss) { s_decodes.fetch_add(1, std::memory_order_relaxed); s_texels.fetch_add(static_cast<uint64_t>(texW) * texH, std::memory_order_relaxed); }
                static std::mutex s_dm; static std::chrono::steady_clock::time_point s_t = std::chrono::steady_clock::now();
                // Which textures are churning: sum re-decoded texels per (tbp0,psm,w,h).
                static std::map<uint64_t,uint32_t> s_missKey;
                std::lock_guard<std::mutex> lk(s_dm);
                if (miss) { uint64_t k = ((uint64_t)tex.tbp0<<32)|((uint64_t)tex.psm<<24)|((uint64_t)(texW&0xFFF)<<12)|(texH&0xFFF); s_missKey[k]++; }
                double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t).count();
                if (dt >= 1.0) {
                    std::fprintf(stderr, "[decode] texPrims/s=%llu decodes/s=%llu texels/s=%lluK\n",
                                 (unsigned long long)(s_prims.exchange(0)/dt), (unsigned long long)(s_decodes.exchange(0)/dt),
                                 (unsigned long long)(s_texels.exchange(0)/dt/1000));
                    std::vector<std::pair<uint64_t,uint32_t>> mv(s_missKey.begin(), s_missKey.end());
                    std::sort(mv.begin(), mv.end(), [](auto&a,auto&b){return a.second>b.second;});
                    for (size_t i=0;i<mv.size()&&i<4;++i)
                        std::fprintf(stderr, "  [miss] tbp0=%llu psm=%llu %llux%llu n=%u\n",
                            (unsigned long long)(mv[i].first>>32), (unsigned long long)((mv[i].first>>24)&0xFF),
                            (unsigned long long)((mv[i].first>>12)&0xFFF), (unsigned long long)(mv[i].first&0xFFF), mv[i].second);
                    s_missKey.clear();
                    s_t = std::chrono::steady_clock::now();
                }
            }
        }
        // [gpualias] mode>=4: reads the renderer serves from the view texture never use the
        // decoded texture OR its version key -- skip resolveTextureVersion + decodeTexRGBA
        // (pure guest-time waste, measured ~17%% of guest in decode). Env PS2X_GPUALIAS_DECSKIP=0
        // restores the old behavior for A/B.
        static const int s_gaDs4 = [](){ const char *v = std::getenv("PS2X_GPUALIAS"); return v && v[0] ? std::atoi(v) : 0; }();
        static const bool s_gaDecSkip = [](){ const char *v = std::getenv("PS2X_GPUALIAS_DECSKIP"); return !(v && v[0] == '0'); }();
        static const bool s_gaAliasZ2 = [](){ const char *v = std::getenv("PS2X_ALIASZ"); if (v && v[0] && v[0] != '0') return true;
                                              const char *w = std::getenv("PS2X_ALIASZSW"); return w && w[0] && w[0] != '0'; }();   // [dofmask] see ALIASZSW above
        const bool gaZ16DropRead = s_gaDs4 >= 4 && s_gaDecSkip && !s_gaAliasZ2 && ctx.tex0.tbp0 == 7168u &&
                                   (ctx.tex0.psm == 0x30u || ctx.tex0.psm == 0x31u || ctx.tex0.psm == 0x32u);
        {   // [zwbwant] a Z-format texture read that is NOT dropped is the only consumer of the VRAM depth writeback
            extern std::atomic<uint64_t> g_zReadWantFrame; extern std::atomic<uint64_t> g_bt3FrameCount;
            if (!gaZ16DropRead && (ctx.tex0.psm == 0x30u || ctx.tex0.psm == 0x31u || ctx.tex0.psm == 0x32u || ctx.tex0.psm == 0x3Au))
                g_zReadWantFrame.store(g_bt3FrameCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        const bool gaServedRead = gaZ16DropRead || p8twinServedRead(ctx.frame.fbp, ctx.tex0) || rtServedRead(ctx.frame.fbp, ctx.tex0) || (s_gaDs4 >= 4 && s_gaDecSkip && ctx.tex0.tbp0 == 10752u &&
                                  (ctx.tex0.psm == 0x02u || ctx.tex0.psm == 0x0Au) &&
                                  gs->m_texa.aem && gs->m_texa.ta1 == 0u &&
                                  (gs->m_texa.ta0 == 0x30u ||
                                   (gs->m_texa.ta0 == 0x80u && (ctx.frame.fbmsk & 0x00FFFFFFu) == 0x00FFFFFFu)));
        // Resolve the CONTENT-VERSIONED key (see resolveTextureVersion): streamed materials
        // sharing one tbp0 get distinct cache entries instead of overwriting each other.
        bool texNeedDecode = false;
        {   extern bool g_resolveAlphaData;
            const uint32_t pv = ctx.tex0.psm;
            g_resolveAlphaData = (pv == GS_PSM_T8 || pv == GS_PSM_T8H || pv == GS_PSM_T4 || pv == GS_PSM_T4HL || pv == GS_PSM_T4HH)
                                 || ((ctx.frame.fbmsk & 0x00ffffffu) == 0x00ffffffu); }
        const uint64_t texKeyBase = texKey;   // [dectime] pre-version key (material + CLUT content)
        if (!gaServedRead) texKey = r.resolveTextureVersion(texKey, texPageLo, texPageHi, gs->m_vram, gs->m_vramSize, texNeedDecode);
        if (deferTex || deferClut) texNeedDecode = true;   // [deferdec] GL-dirty source: the record-time hash saw stale VRAM
        // [deferpend] a page whose deferred flush is still queued is stale in VRAM: a read that needs a decode
        // (a different view / key of the same page) must queue behind that flush, never decode synchronously.
        // [defercover] PS2X_DEFERCOVER (default on, =0 old): the pending flush that covers the page is what the post must
        // flush (sync flushes fbp336 for a read of page 368; posting flushPage=368 flushed fbp368's own 128x128 FBO -> the
        // timer plate got content sync never has); a BARSKIP page (502,504,368) is never posted, as sync never barriers it.
        static const bool s_dcv = [](){ const char *v = std::getenv("PS2X_DEFERCOVER"); return !(v && v[0] == '0'); }();
        const uint32_t pendCover = (s_deferDec && !deferTex && texNeedDecode) ? (s_dcv ? ps2xDeferCoverFor(ctx.tex0.tbp0 / 32u) : (r.flushPending(ctx.tex0.tbp0 / 32u) ? ctx.tex0.tbp0 / 32u : 0xFFFFFFFFu)) : 0xFFFFFFFFu;
        // [pendsheet] PS2X_PENDSHEET=0 = old: a PSMT8/PSMT4 read is an uploaded sheet (RT-produced indexed reads are T8H/T4HH);
        // sync never barriers a non-fbp page, so decode it NOW from VRAM (= the post-time bytes a deferred decode would use)
        // instead of a post + page snapshots + a GL-thread decode (BT3: thousands of HUD sheet reads per run).
        static const bool s_psh = [](){ const char *v = std::getenv("PS2X_PENDSHEET"); return !(v && v[0] == '0'); }();
        const bool sheetPsm = s_psh && (ctx.tex0.psm == GS_PSM_T8 || ctx.tex0.psm == GS_PSM_T4);
        const bool pendTex = s_deferDec && !deferTex && texNeedDecode && pendCover != 0xFFFFFFFFu && !sheetPsm && GSRasterizer::decodeIsDeferrable(ctx.tex0.psm);
        const bool pendClut = s_deferDec && !deferClut && texNeedDecode && (ctx.tex0.psm == GS_PSM_T8 || ctx.tex0.psm == GS_PSM_T8H || ctx.tex0.psm == GS_PSM_T4 || ctx.tex0.psm == GS_PSM_T4HL || ctx.tex0.psm == GS_PSM_T4HH) && r.flushPending(ctx.tex0.cbp / 32u);
        if (pendTex) deferTex = true;
        if (pendClut) deferClut = true;
        { static int n = 0; if ((deferTex || deferClut) && n < 4) { ++n; std::fprintf(stderr, "[deferdec] record: tbp0 %u psm %u need=%d deferTex=%d deferClut=%d\n", ctx.tex0.tbp0, (unsigned)ctx.tex0.psm, (int)texNeedDecode, (int)deferTex, (int)deferClut); } }
        if (texNeedDecode && (deferTex || deferClut))
        {   // [deferdec] no guest wait: the GL thread flushes the page(s) and decodes at this point of
            // the command stream, so every draw recorded after this sees the decoded texture.
            auto req = std::make_shared<TexDecodeReq>();
            req->tex0 = ctx.tex0; req->clamp = ctx.clamp; req->texa = gs->m_texa; req->texclut = gs->m_texclut;
            req->texW = texW; req->texH = texH; req->rawAlphaDec = rawAlphaDec; req->texKey = texKey;
            req->pageLo = texPageLo; req->pageHi = texPageHi; req->subDxW = g_subDxW; req->subDx0 = g_subDx0;
            // [defercover] a pend-post flushes NOTHING: the sync barrier for a non-fbp page with nothing dirty flushes nothing
            // (it decodes VRAM as uploaded); the post only orders the decode behind the pending flush of the covering fbp.
            // Flushing the read's own page flushed fbp368's stray 128x128 FBO (timer plate wrong); flushing the cover
            // (fbp336) re-clobbered the sheets and cost 60k flushes (11 fps).
            req->flushPage = deferTex ? ((pendTex && s_dcv) ? 0xFFFFFFFFu : (ctx.tex0.tbp0 / 32u)) : 0xFFFFFFFFu; req->flushAlpha = deferAlpha;
            {   // [clutcover] a pend-post's palette flush targets the fbp whose pending flush covers the CLUT page (fbp480 for
                // BT3's rendered outline palettes at 499-500); the page itself has no FBO and the flush would bail.
                static const bool s_cc = [](){ const char *v = std::getenv("PS2X_CLUTCOVER"); return !(v && v[0] == '0'); }();
                req->flushClutPage = deferClut ? ((pendClut && s_cc) ? ps2xDeferCoverFbp(ctx.tex0.cbp / 32u) : (ctx.tex0.cbp / 32u)) : 0xFFFFFFFFu;
            }
            r.postDecode(std::move(req));
            {   // [ddmaxq] PS2X_DDMAXQ=K (default 4, 0 = unbounded): bound how far the guest runs ahead of the GL thread's decode
                // services. Deep queues (GL thread slowed by another GPU client) are where every shared-page protection erodes
                // (rig: full deferral clean at 30 fps, striped/garbled at 17-25); a shallow queue keeps the race window small.
                static const int s_maxq = [](){ const char *v = std::getenv("PS2X_DDMAXQ"); return v && v[0] ? std::atoi(v) : 0; }();   // default 0: a bound of 2-4 removed the stripes under load but starved the HUD (late decodes -> placeholders)
                extern unsigned long g_deferPosted, g_deferServed; extern unsigned long g_ddMaxqWaits;
                if (s_maxq > 0 && g_deferPosted > g_deferServed + (unsigned long)s_maxq)
                {
                    ++g_ddMaxqWaits;
                    const auto t0 = std::chrono::steady_clock::now();
                    while (g_deferPosted > g_deferServed + (unsigned long)s_maxq
                           && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(40))
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        }
        else if (texNeedDecode && !gaServedRead)
        {
            TexDecodeSrc src;
            src.tex0 = &ctx.tex0; src.clamp = ctx.clamp; src.vram = gs->m_vram; src.vramSize = gs->m_vramSize;
            src.clut = gs->m_clutCache; src.clutKey = gs->m_clutCacheKey; src.texa = &gs->m_texa; src.texclut = &gs->m_texclut;
            src.subDxW = g_subDxW; src.subDx0 = g_subDx0;
            int subW = 0; std::vector<uint8_t> rgba;
            // [dectime] PS2X_DECCENSUS=1: guest-thread wall time spent in inline decodes (incl. the pool wait) and a census by class
            static const bool s_dcs = [](){ const char *v = std::getenv("PS2X_DECCENSUS"); return v && v[0] && v[0] != '0'; }();
            const auto _d0 = s_dcs ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            {   // [guestprof] DEC = inline texture decode + the upload hand-off
                gprof::Scope gpScope(gprof::DEC);
                decodeTexRGBA(gs, src, texW, texH, rawAlphaDec, texKey, subW, rgba);   // [decodefn]
                int upW = subW, upH = texH;
                {   // [texreplace] Swap in a PCSX2-pack replacement if one exists for this texture.
                    // Sampling is NORMALISED (texture(texture0, uv), not texelFetch), so a 4x
                    // replacement needs no UV rescaling -- just hand putTexture the bigger buffer
                    // and its real dimensions.
                    //
                    // Only reached on a texture-cache MISS, so the PNG decode happens once per
                    // texture rather than per draw; no separate PNG cache is needed.
                    // No RT guard here: a rendered page "cannot be re-decoded -- GPU mode never
                    // writes rendered pixels to VRAM" (see srcRendered in ps2_gs_gpu_renderer.h),
                    // so render targets never reach this decode path in the first place. If a
                    // profile later shows RT hashing burning guest time, add the check then.
                    if (GsGpuRenderer::texPackEnabled() && ps2tex::replacementsEnabled())
                    {
                        ps2tex::TexIdent id;
                        const bool pal = (ctx.tex0.psm == 19 || ctx.tex0.psm == 20);
                        if (ps2tex::identify(gs->vramData(), ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm,
                                             ctx.tex0.tw, ctx.tex0.th, pal ? gs->m_clutCache : nullptr,
                                             gs->m_texa.ta0, gs->m_texa.aem, gs->m_texa.ta1, id))
                        {
                            std::vector<uint8_t> rep; int rw = 0, rh = 0;
                            if (ps2tex::loadReplacement(id, rep, rw, rh))
                            {
                                rgba = std::move(rep); upW = rw; upH = rh;
                                static std::atomic<unsigned long> s_hits{0};
                                const unsigned long k = s_hits.fetch_add(1) + 1ul;
                                if (k <= 5 || (k % 100ul) == 0ul)
                                    std::fprintf(stderr, "[texreplace] hit #%lu %s -> %dx%d\n",
                                                 k, id.name().c_str(), rw, rh);
                            }
                        }
                    }
                }
                r.putTexture(texKey, std::move(rgba), upW, upH, texPageLo, texPageHi);
            }
            if (s_dcs)
            {
                static double s_ms = 0; static unsigned long s_n = 0; static std::map<uint64_t, std::pair<unsigned long, double>> s_m; static auto s_t = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                const double dms = std::chrono::duration<double, std::milli>(now - _d0).count();
                s_ms += dms; ++s_n;
                auto &e = s_m[((uint64_t)ctx.tex0.tbp0 << 40) | ((uint64_t)ctx.tex0.psm << 32) | ((uint64_t)(texW & 0xFFFF) << 16) | (uint64_t)(texH & 0xFFFF)];
                e.first++; e.second += dms;
                {   // why: base key never seen (new material/CLUT combination) vs same base re-versioned (span content moved)
                    static std::unordered_set<uint64_t> s_seenBase; static unsigned long s_newBase = 0, s_reVer = 0;
                    if (s_seenBase.insert(texKeyBase).second) ++s_newBase; else ++s_reVer;
                    static auto s_t2 = std::chrono::steady_clock::now();
                    if (std::chrono::duration<double>(now - s_t2).count() >= 2.0)
                    { std::fprintf(stderr, "[decwhy] decodes: newBaseKey=%lu sameBaseReversioned=%lu (distinct bases seen %zu)\n", s_newBase, s_reVer, s_seenBase.size()); s_newBase = s_reVer = 0; s_t2 = now; }
                }
                if (std::chrono::duration<double>(now - s_t).count() >= 2.0)
                {
                    std::vector<std::pair<uint64_t, std::pair<unsigned long, double>>> v(s_m.begin(), s_m.end());
                    std::sort(v.begin(), v.end(), [](auto &a, auto &b){ return a.second.second > b.second.second; });
                    std::fprintf(stderr, "[dectime] inline decodes %lu in 2 s = %.1f ms (%.1f ms/s), %zu classes; top by time:", s_n, s_ms, s_ms / 2.0, v.size());
                    for (size_t i = 0; i < v.size() && i < 8; ++i)
                        std::fprintf(stderr, "  tbp%llu/psm%llu %llux%llu n=%lu %.1fms", (unsigned long long)(v[i].first >> 40), (unsigned long long)((v[i].first >> 32) & 0xFF),
                                     (unsigned long long)((v[i].first >> 16) & 0xFFFF), (unsigned long long)(v[i].first & 0xFFFF), v[i].second.first, v[i].second.second);
                    std::fprintf(stderr, "\n");
                    s_m.clear(); s_ms = 0; s_n = 0; s_t = now;
                }
            }
        }
    }
        s_tpl.valid = s_tplOn; s_tpl.gen = gs->m_stateGen + gs->m_texUploadGen * 0x9E3779B1u; s_tpl.epoch = ps2GpuRenderer().recTplEpoch(); s_tpl.frame = tplFrame;
        s_tpl.type = (uint8_t)gs->m_prim.type; s_tpl.tme = tme; s_tpl.fst = fst; s_tpl.texKey = texKey; s_tpl.texW = texW; s_tpl.texH = texH;
        s_tpl.pageLo = tplPageLo; s_tpl.pageHi = tplPageHi; s_tpl.drawStamp = ps2GpuRenderer().pageDrawStamp(tplPageLo, tplPageHi);
        g_recTplMiss.fetch_add(1, std::memory_order_relaxed);
    }

    const uint32_t tfx = tme ? ctx.tex0.tfx : 0u;
    const uint32_t tcc = tme ? ctx.tex0.tcc : 0u; // 1 = texture provides alpha (coverage)
    // PS2 vertex-color modulation. TFX MODULATE is (texel*vc)>>7 (÷128); raylib's
    // modulate + SRC_ALPHA blend is ÷255. Scale RGB by 255/128.
    // Colors are consumed by the PS2-modulate shader, which does rgb *= 255/128
    // (÷128 modulate). Pre-scale per primitive type so that one factor is correct:
    //  - textured MODULATE: pass RAW vc  -> shader gives texel*vc/128 (overbright ok).
    //  - textured DECAL: pass 128 (identity after *255/128) -> texel unchanged.
    //  - untextured flat: pre-scale rgb DOWN by 128/255 -> shader scales back to vc.
    // Alpha never gets the *255/128 (shader leaves a as-is). Texture alpha (0..128)
    // is already scaled to 0..255 in the decode; untextured alpha gets the blend
    // 255/128 here; TCC=1 uses the texture's coverage (tint a = 255).
    auto colorBytes = [&](const GSVertex &v, uint8_t &cr, uint8_t &cg, uint8_t &cb, uint8_t &ca)
    {
        if (tme && tfx == 1u) // DECAL
        {
            cr = cg = cb = 128u; ca = 255u;
        }
        else if (tme) // MODULATE
        {
            cr = v.r; cg = v.g; cb = v.b;
            // GS MODULATE alpha with TCC=1 is At*Av>>7 — BOTH texture AND vertex alpha.
            // Passing 255 here dropped the vertex-alpha term, so per-vertex alpha fade
            // gradients (the terrain material-blend zones) rendered at FULL strength:
            // hard-edged dark-green patches where grass variants should crossfade.
            // The shader multiplies t.a * fragColor.a, so this one factor restores it
            // (va=0x80 -> 255 -> identical to before for the common opaque case).
            ca = static_cast<uint8_t>(std::min(255u, (v.a * 255u) >> 7));
        }
        else // untextured flat
        {
            cr = static_cast<uint8_t>((v.r * 128u) / 255u);
            cg = static_cast<uint8_t>((v.g * 128u) / 255u);
            cb = static_cast<uint8_t>((v.b * 128u) / 255u);
            ca = static_cast<uint8_t>(std::min(255u, (v.a * 255u) >> 7));
        }
    };
    auto texelUV = [&](const GSVertex &v, float &u, float &tv)
    {
        if (!tme) { u = 0.0f; tv = 0.0f; return; }
        if (fst) { u = static_cast<float>(v.u >> 4); tv = static_cast<float>(v.v >> 4); }
        else { float q = (v.q != 0.0f) ? v.q : 1.0f; u = (v.s / q) * texW; tv = (v.t / q) * texH; }
    };

    {   // PS2X_ZWB: publish the frame's ZBUF so the renderer can write our depth buffer back
        // into VRAM at the right address/format. BT3 READS that address as a texture (fbp224 IS
        // the Z buffer), and once the bytes are in VRAM every aliased view of them decodes
        // correctly for free -- which is the only way to express the CT16/CT32 dual view.
        extern uint32_t g_zwbBp, g_zwbPsm, g_zwbBw;
        g_zwbBp  = ctx.zbuf.zbp;
        g_zwbPsm = (uint32_t)(((ctx.zbuf.psm) & 0xFu) | 0x30u);
        g_zwbBw  = ctx.frame.fbw ? ctx.frame.fbw : 1u;
        // Publish the zMax actually used by zNorm, so the renderer derives the GS Z byte scale
        // from it instead of a fitted constant. It depends on PS2X_ZPSMNORM: with it off every
        // ZBUF normalises by 2^32, with it on a PSMZ24 buffer normalises by 2^24-1.
        extern double g_zwbZMax;
        g_zwbZMax = zMax;
    }
    GsGpuRenderer &r = ps2GpuRenderer();
    gprof::mark(gprof::REC_BUILD);   // [guestprof]
    // [drawbatch] A CONTINUATION of the renderer's open plain-class batch does not fill the state
    // part at all: it only writes the three vertices and appends them (see GsGpuRenderer::DrawCmd
    // ::triCount). The retained command is provably still correct because
    //   - tplHit means gs->m_stateGen is unchanged, and writeRegister bumps it for EVERY register
    //     except the per-vertex ones (RGBAQ/ST/UV/XYZ*/FOG), plus the GIF IMAGE branch and the SW
    //     draw path -- so every ctx.* field the fill below reads is byte-identical;
    //   - it also means recTplEpoch() (VRAM upload/writeback/eviction stamps) and the frame are
    //     unchanged, which is what srcUploaded/srcRendered/texKey are derived from;
    //   - batchSeq() unchanged means OUR command is still the batch head -- nothing else recorded
    //     in between, on this thread or any other;
    //   - batchEligible() (checked when the head was pushed) excludes the classes whose state
    //     comes from anywhere else: depth-only companions, alias passes, PSMT8H CLUT publishers,
    //     self-feedback reads, HUD/FST=1 draws, decals.
    // A build-phase template that COPIED a saved command was tried before and lost (a ~330 B copy
    // costs what the field fill costs, ledger item 40); this one copies nothing.
    static thread_local GsGpuRenderer::DrawCmd s_bcmd;
    static thread_local uint64_t s_bcmdSeq = ~0ull;
    extern bool g_recordDepthOnly;
    extern int g_recordAliasKind;
    const bool batchCont = s_tplOn && tplHit && !isSprite && GsGpuRenderer::batchingEnabled()
                           && !g_recordDepthOnly && g_recordAliasKind == 0
                           && r.batchOpen() && r.batchSeq() == s_bcmdSeq;
    GsGpuRenderer::DrawCmd &cmd = s_bcmd;
    // Invalidate FIRST: a fresh fill that is then dropped (the ZSAT/near-plane culls below return
    // without recording) must not leave the next primitive thinking the retained state is the open
    // batch head's. Only a completed record re-arms it, at the bottom of the function.
    if (!batchCont) { s_bcmdSeq = ~0ull;
    cmd = GsGpuRenderer::DrawCmd{};
    {   // (a state-part snapshot of the command was tried here: a 500-byte copy costs what the field fill costs -- no gain)
    cmd.texKey = texKey;
    // Destination framebuffer + source texture base: route to per-fbp FBOs and enable
    // render-to-texture (sampling a framebuffer that was rendered to).
    cmd.destFbp = ctx.frame.fbp;
    cmd.fba = ((ctx.fba & 1ull) != 0ull) && ctx.frame.psm != GS_PSM_CT24;
    cmd.destFbw = ctx.frame.fbw;
    cmd.destPsm = static_cast<uint8_t>(ctx.frame.psm);
    cmd.bilinear = ((ctx.tex1 >> 5) & 1u) != 0u;   // GS TEX1.MMAG
    {   // [terrainpoint] PS2X_TERRAINPOINT=1: force point sampling for the terrain/hills palette
        // class (psm19/20, CLUT 12992). GL bilinear on this tiled carousel sheet bleeds across
        // tile edges and washes the distant strata (replay-verified: PS2X_BILINEAR=0 restores
        // console banding); scope the point-sampling so gradients elsewhere keep bilinear.
        static const int s_tp = [](){ const char *v = std::getenv("PS2X_TERRAINPOINT"); return v && v[0] ? std::atoi(v) : 0; }();
        if ((s_tp == 1 && (ctx.tex0.psm == 19u || ctx.tex0.psm == 20u) && (ctx.tex0.cbp == 12992u || (ctx.tex0.cbp >= 15616u && ctx.tex0.cbp <= 15640u)))
            || (s_tp == 2 && (ctx.tex0.psm == 19u || ctx.tex0.psm == 20u)))
        {
            cmd.bilinear = false;
            static unsigned long s_tpn = 0;
            if ((++s_tpn % 20000ul) == 1ul) std::fprintf(stderr, "[terrainpoint] forced %lu draws\n", s_tpn);
        }
    }
    cmd.srcTbp0 = tme ? ctx.tex0.tbp0 : 0u;
    cmd.srcTexW = tme ? (g_subDxW ? g_subDxW : texW) : 0;   // [subdecode]
    cmd.srcTexH = tme ? texH : 0;
    {
        // Cumulative census of every distinct destination fbp (+ its tbp0-equivalent
        // fbp*32) and source tbp0, to locate where render targets like the logo live.
        static const bool s_fd = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_fd)
        {
            static std::mutex s_fm;
            static std::set<uint32_t> s_dest, s_src;
            std::lock_guard<std::mutex> lk(s_fm);
            bool nd = s_dest.insert(ctx.frame.fbp).second;
            bool ns = tme && s_src.insert(ctx.tex0.tbp0).second;
            if (nd) std::fprintf(stderr, "[fbpcensus] NEW destFbp=%u (tbp0-equiv=%u) fbw=%u\n", ctx.frame.fbp, ctx.frame.fbp*32u, ctx.frame.fbw);
            if (ns) std::fprintf(stderr, "[fbpcensus] NEW srcTbp0=%u (fbp-equiv=%u) %dx%d psm=%u\n", ctx.tex0.tbp0, ctx.tex0.tbp0/32u, texW, texH, ctx.tex0.psm);
        }
    }
    // PS2X_CELPROBE[=tbp0]: everything about the draws that SAMPLE the cel-shade map
    // (default tbp0 15680) — prim/tfx/blend + per-vertex texel UV and color. Answers
    // whether the subtract passes sample the band row (v<1) or the flat body, and
    // whether vertex colors attenuate the subtract (MODULATE) or not (DECAL).
    {
        static const uint32_t s_celTbp = [](){
            const char *v = [](){ static const char *s_env = std::getenv("PS2X_CELPROBE"); return s_env; }();
            if (!v || !v[0] || v[0] == '0') return 0u;
            uint32_t t = (uint32_t)std::strtoul(v, nullptr, 0);
            return t > 1u ? t : 15680u;
        }();
        if (s_celTbp && tme && ctx.tex0.tbp0 == s_celTbp)
        {
            static std::atomic<int> s_cn{0};
            int n = s_cn.fetch_add(1);
            if (n < 40 || (n % 500) == 0)
            {
                float u0,v0,u1,v1,u2,v2;
                texelUV(gs->m_vtxQueue[0], u0, v0);
                texelUV(gs->m_vtxQueue[1], u1, v1);
                texelUV(gs->m_vtxQueue[2], u2, v2);
                const auto &a = gs->m_vtxQueue[0];
                std::fprintf(stderr, "[celprobe] #%d prim=%u destFbp=%u tfx=%u tcc=%u abe=%u blend=%02llx fix=%02llx fbmsk=%08x cbp=%u | uvTexel=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) vc=(%u,%u,%u,%u) scr=(%.0f,%.0f)\n",
                             n, gs->m_prim.type, ctx.frame.fbp, ctx.tex0.tfx, ctx.tex0.tcc, gs->m_prim.abe ? 1u : 0u,
                             (unsigned long long)(ctx.alpha & 0xFFu), (unsigned long long)((ctx.alpha >> 32) & 0xFFu),
                             ctx.frame.fbmsk, ctx.tex0.cbp,
                             u0, v0, u1, v1, u2, v2, a.r, a.g, a.b, a.a, a.x, a.y);
            }
        }
    }
    // [hpbar] (PS2X_HPBAR): full state of the health-bar fill draws (srcTbp0=10880) —
    // psm/CLUT regs/CLUT alpha content/TEXA/scissor — the fill quad is always full-width,
    // so the partial-fill mechanism must live in one of these.
    {
        static const bool s_hp = [](){ const char *v = std::getenv("PS2X_HPBAR"); return v && v[0] && v[0] != '0'; }();
        if (s_hp && tme && ctx.tex0.tbp0 == 10880u)
        {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t n = s_n.fetch_add(1);
            if (n < 20u || (n % 512u) == 0u)
            {
                ensureClutCache(gs);
                char cl[220] = "";
                if (gs->m_clutCacheKey != ~0ull)
                {
                    int o = 0;
                    for (int i = 0; i < 32 && o < 200; i += 2)
                        o += std::snprintf(cl + o, sizeof(cl) - o, "%02x,", (gs->m_clutCache[i] >> 24) & 0xFF);
                }
                std::fprintf(stderr, "[hpbar] #%u psm=%u tw=%u th=%u cbp=%u cpsm=%u csa=%u csm=%u cld=%u | texa ta0=%u aem=%d ta1=%u | sci=(%d,%d)-(%d,%d) | fst=%u prim=%u | clutA[0..30,2]=%s\n",
                             n, ctx.tex0.psm, ctx.tex0.tw, ctx.tex0.th,
                             ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csa, ctx.tex0.csm, ctx.tex0.cld,
                             gs->m_texa.ta0, gs->m_texa.aem ? 1 : 0, gs->m_texa.ta1,
                             ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                             gs->m_prim.fst, gs->m_prim.type, cl);
            }
        }
    }
    // GS scissor -> clip rect (top-left origin, framebuffer pixels). The GPU renderer
    // must respect this or full-width bands (e.g. the popup box) span the whole screen.
    cmd.sx = ctx.scissor.x0;
    cmd.sy = ctx.scissor.y0;
    cmd.sw = std::max(0, ctx.scissor.x1 - ctx.scissor.x0 + 1);
    cmd.sh = std::max(0, ctx.scissor.y1 - ctx.scissor.y0 + 1);

    // GS depth-test state (only meaningful when PS2X_GPU_DEPTH is on; else left default).
    if (s_depthOn)
    {
        cmd.depthTest = zTestEnable;
        cmd.depthFunc = zTestFunc;
        cmd.depthWrite = zWrite;
        cmd.zbufBp = ctx.zbuf.zbp;   // [zbufbp]
    }
    { extern bool g_recordDepthOnly; cmd.depthOnly = g_recordDepthOnly; }

    // GS TEST alpha-test state: ATE bit0, ATST bits 1-3, AREF bits 4-11, AFAIL bits 12-13.
    // The GPU replay discards failing fragments in its shader (AFAIL=0 KEEP case).
    cmd.alphaTest = (ctx.test & 1u) != 0u;
    cmd.alphaFunc = static_cast<uint8_t>((ctx.test >> 1) & 7u);
    cmd.alphaRef = static_cast<uint8_t>((ctx.test >> 4) & 0xFFu);
    cmd.alphaFail = static_cast<uint8_t>((ctx.test >> 12) & 3u);
    // GS TEST.DATE/DATM (destination-alpha test) — the HUD bar partial-fill mechanism.
    cmd.dateEnable = ((ctx.test >> 14) & 1u) != 0u;
    cmd.dateMode = static_cast<uint8_t>((ctx.test >> 15) & 1u);
    cmd.fst = static_cast<uint8_t>(gs->m_prim.fst & 1u);

    // GS PRIM.ABE: alpha-blend enable. Opaque prims (abe=0) must be drawn without blending
    // in the GPU renderer, matching the software rasterizer (which gates on m_prim.abe).
    cmd.abe = gs->m_prim.abe;
    // GS ALPHA register: blend equation + FIX, so the replay can map (Cs-Cd)*FIX+Cd (opaque
    // when FIX>=0x80) and Cd-Cs*FIX (subtractive shadows) to correct GL blend modes instead
    // of blanket texture-alpha blending (which erased the low-CLUT-alpha stage to black).
    cmd.blendMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
    cmd.blendFix = static_cast<uint8_t>((ctx.alpha >> 32) & 0xFFu);
    // GS FRAME.FBMSK write mask — the Z-as-texture / destination-alpha passes depend on it
    // (they'd otherwise paint opaque columns over the scene; SW honors it, GPU must too).
    cmd.fbmsk = ctx.frame.fbmsk;
    // GS CLAMP wrap modes: wms bits0-1, wmt bits2-3. 0=REPEAT, 1=CLAMP, 2=REGION_CLAMP,
    // 3=REGION_REPEAT (approximate region modes by their base behavior).
    {
        const uint32_t wms = static_cast<uint32_t>(ctx.clamp & 3u);
        const uint32_t wmt = static_cast<uint32_t>((ctx.clamp >> 2) & 3u);
        cmd.wrapU = (wms == 1u || wms == 2u) ? 1u : 0u;
        cmd.wrapV = (wmt == 1u || wmt == 2u) ? 1u : 0u;
        cmd.wms = static_cast<uint8_t>(wms); cmd.wmt = static_cast<uint8_t>(wmt);
        cmd.minu = static_cast<uint16_t>((ctx.clamp >> 4) & 0x3FFu);
        cmd.maxu = static_cast<uint16_t>((ctx.clamp >> 14) & 0x3FFu);
        cmd.minv = static_cast<uint16_t>((ctx.clamp >> 24) & 0x3FFu);
        cmd.maxv = static_cast<uint16_t>((ctx.clamp >> 34) & 0x3FFu);
        cmd.tcc = tme ? static_cast<uint8_t>(ctx.tex0.tcc & 1u) : 1u;
        // [regionrec] PS2X_REGIONREC=1: does the stage/backdrop class (tbp0=10752) use
        // REGION_* wrap modes whose window params (MINU/MAXU/MINV/MAXV) we currently drop?
        {
            static const bool s_rr = [](){ const char *v = std::getenv("PS2X_REGIONREC"); return v && v[0] && v[0] != '0'; }();
            // Log REGION-mode draws on ANY texture (window params are currently dropped by
            // the GPU DrawCmd — the 2026-07-30 window-decode experiment was reverted after
            // it caused 2D-screen flashing; the only in-fight user is the FB-copy 512x448).
            if (s_rr && tme && (wms >= 2u || wmt >= 2u))
            {
                static std::atomic<uint32_t> s_rn{0};
                const uint32_t n = s_rn.fetch_add(1);
                if (n < 64u || (n % 4096u) == 0u)
                    std::fprintf(stderr, "[regionrec] #%u tbp0=%u wms=%u wmt=%u minu=%u maxu=%u minv=%u maxv=%u tex=%ux%u prim=%u\n",
                                 n, ctx.tex0.tbp0, wms, wmt,
                                 (unsigned)((ctx.clamp >> 4) & 0x3FFu), (unsigned)((ctx.clamp >> 14) & 0x3FFu),
                                 (unsigned)((ctx.clamp >> 24) & 0x3FFu), (unsigned)((ctx.clamp >> 34) & 0x3FFu),
                                 1u << ctx.tex0.tw, 1u << ctx.tex0.th, gs->m_prim.type);
            }
        }

        // [clamprec] (default on, PS2X_SRCDIAG=0 disables): record-side CLAMP forensics for
        // the fight's character textures — the GPU DrawCmd drops the REGION_* parameters, so
        // log the raw mode + MIN/MAX + first-vertex STQ to see what GL sampling should be.
        static const bool s_cr = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_SRCDIAG"); return s_env; }(); return !(v && v[0] == '0'); }();
        if (s_cr && tme && ctx.tex0.tbp0 >= 13000u && ctx.tex0.tbp0 < 14100u)
        {
            static unsigned s_n = 0;
            if (s_n < 12)
            {
                ++s_n;
                static FILE *f = std::fopen("/home/z3/Desktop/bt3/work/clamprec.txt", "w");
                if (f)
                {
                    const auto &v0 = gs->m_vtxQueue[0];
                    std::fprintf(f, "[clamprec] tbp0=%u %ux%u psm=%u clamp=%016llx wms=%u wmt=%u minU=%u maxU=%u minV=%u maxV=%u fst=%u | v0 s=%.5f t=%.5f q=%.5f uv=(%u,%u)\n",
                                 ctx.tex0.tbp0, 1 << ctx.tex0.tw, 1 << ctx.tex0.th, ctx.tex0.psm,
                                 (unsigned long long)ctx.clamp, wms, wmt,
                                 (uint32_t)((ctx.clamp >> 4) & 0x3FFu), (uint32_t)((ctx.clamp >> 14) & 0x3FFu),
                                 (uint32_t)((ctx.clamp >> 24) & 0x3FFu), (uint32_t)((ctx.clamp >> 34) & 0x3FFu),
                                 gs->m_prim.fst, v0.s, v0.t, v0.q, v0.u, v0.v);
                    std::fflush(f);
                }
            }
        }
    }

    // Indexed/CLUT texture formats (PSMT8=19, PSMT4=20, PSMT8H=27, PSMT4HL=36, PSMT4HH=44) can
    // never be render targets -> never composite from an FBO slot, always decode. Fixes the HUD
    // frame texture (psm=19 at tbp0 aliasing a render-target base) rendering as blank rectangles.
    if (tme) {
        const uint32_t p = ctx.tex0.psm;
        cmd.srcIndexed = (p == 19u || p == 20u || p == 27u || p == 36u || p == 44u);
        // Carry the palette base for indexed sources: the renderer needs it to tell whether the
        // CLUT was RENDERED (unrecoverable) rather than uploaded.
        if (cmd.srcIndexed) cmd.srcClutTbp = ctx.tex0.cbp;
        // Publish the decoded palette for PSMT8H sources so the renderer can look it up.
        // Only T8H: that is the alpha-as-index read BT3 uses to filter the scene buffer, and it
        // is the only class allowed to sample a live FBO (see PS2X_IDXRT in the renderer).
        if (cmd.srcIndexed && p == 27u)
        {
            ensureClutCache(gs);
            const uint64_t h2 = gs->m_clutCacheHash256;   // [cluthash] fresh: ensureClutCache just ran
            cmd.srcClutKey = h2 ? h2 : 1ull;
            extern void ps2xPublishClut(uint64_t key, const uint32_t *pal);
            ps2xPublishClut(cmd.srcClutKey, gs->m_clutCache);
        }
        cmd.srcPsm = static_cast<uint8_t>(ctx.tex0.psm);
        cmd.tfx = static_cast<uint8_t>(ctx.tex0.tfx);
        // GS TEXA -> GPU path (the SW/decode path applies it in applyTexa()).
        cmd.texaTa0 = static_cast<uint8_t>(gs->m_texa.ta0);
        cmd.texaTa1 = static_cast<uint8_t>(gs->m_texa.ta1);
        cmd.texaAem = gs->m_texa.aem;
    }

    {
        // [logo] Log every textured draw of a 128x256 texture (the duplicating logo),
        // anywhere on screen, with geometry + per-vertex UV + FST/wrap, to see why it
        // tiles 4x3. Gated by PS2X_GPU_DIAG.
        static const bool s_ud = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_ud && tme && texW == 128 && texH == 256)
        {
            static std::atomic<uint32_t> s_lg{0};
            uint32_t n = s_lg.fetch_add(1) + 1u;
            if (n <= 40)
            {
                const GSVertex &q0 = gs->m_vtxQueue[0];
                const GSVertex &q1 = gs->m_vtxQueue[1];
                auto uvOf = [&](const GSVertex &v, float &u, float &tv) {
                    if (gs->m_prim.fst) { u = (v.u >> 4) / static_cast<float>(texW); tv = (v.v >> 4) / static_cast<float>(texH); }
                    else { float q = (v.q != 0.0f) ? v.q : 1.0f; u = (v.s / q); tv = (v.t / q); }
                };
                float u0, tv0, u1, tv1; uvOf(q0, u0, tv0); uvOf(q1, u1, tv1);
                std::fprintf(stderr, "[logo] #%u key=%llu prim=%u fst=%u tbp0=%u tbw=%u psm=%u | DEST fbp=%u fbw=%u fpsm=%u | xy=(%d,%d)-(%d,%d) uv=(%.3f,%.3f)-(%.3f,%.3f)\n",
                             n, (unsigned long long)texKey, gs->m_prim.type, gs->m_prim.fst, ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm,
                             ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm,
                             static_cast<int>(q0.x) - ofx, static_cast<int>(q0.y) - ofy,
                             static_cast<int>(q1.x) - ofx, static_cast<int>(q1.y) - ofy, u0, tv0, u1, tv1);
            }
        }
    }

    }
    }   // [drawbatch] end of the state fill (skipped entirely on a batch continuation)
    if (isSprite)
    {
        // Sprite -> DrawTexturePro quad (src rect in TEXELS; single color from v1).
        cmd.isTriangle = false;
        const GSVertex &v0 = gs->m_vtxQueue[0];
        const GSVertex &v1 = gs->m_vtxQueue[1];
        // GSVertex x/y are PIXELS WITH A FRACTION. Truncating to int here throws the fraction
        // away, and BT3's outline composite is built out of exactly such offsets: its three
        // fbp224 -> fbp336 passes are the same image written at y=0.000, then SUBTRACTED at
        // y=-0.500 (R channel) and x=-1.000 (G channel). The -1.0 survives truncation, the
        // -0.5 does not -- so the R subtraction lands exactly on top of the base write and
        // cancels it, which is why our composite came out at 0.2% coverage against console's
        // ~2.6% and produced no edge. PS2X_SUBPIXSPR=1 keeps the fraction.
        static const bool s_subSpr = [](){ const char *v = std::getenv("PS2X_SUBPIXSPR");
                                           return v && v[0] && v[0] != '0'; }();
        float x0, y0, x1, y1;
        if (s_subSpr)
        {
            x0 = v0.x - ofxF; y0 = v0.y - ofyF;
            x1 = v1.x - ofxF; y1 = v1.y - ofyF;
        }
        else
        {
            x0 = static_cast<float>(static_cast<int>(v0.x) - ofx);
            y0 = static_cast<float>(static_cast<int>(v0.y) - ofy);
            x1 = static_cast<float>(static_cast<int>(v1.x) - ofx);
            y1 = static_cast<float>(static_cast<int>(v1.y) - ofy);
        }
        float u0, tv0, u1, tv1;
        texelUV(v0, u0, tv0); texelUV(v1, u1, tv1);
        {   // [subspr] is the sub-pixel offset actually arriving here?
            static const bool s_sd = [](){ const char *v = std::getenv("PS2X_SUBPIXDBG");
                                           return v && v[0] && v[0] != '0'; }();
            if (s_sd)
            {
                static std::atomic<uint32_t> nFrac{0}, nAll{0};
                const bool frac = (v0.y != std::floor(v0.y)) || (v0.x != std::floor(v0.x));
                uint32_t a = nAll.fetch_add(1) + 1u;
                if (frac)
                {
                    uint32_t f = nFrac.fetch_add(1) + 1u;
                    if (f <= 6)
                        std::fprintf(stderr, "[subspr] FRACTIONAL vtx: v0=(%.3f,%.3f) v1=(%.3f,%.3f) ofx=%d ofy=%d -> y0=%.3f  fbp=%u tbp=%u\n",
                                     v0.x, v0.y, v1.x, v1.y, ofx, ofy, y0, ctx.frame.fbp, ctx.tex0.tbp0);
                }
                if (ctx.frame.fbp == 336u && ctx.tex0.tbp0 == 7168u)
                {
                    static std::atomic<uint32_t> nOut{0};
                    uint32_t o2 = nOut.fetch_add(1) + 1u;
                    if (o2 <= 6)
                        std::fprintf(stderr, "[subspr] OUTLINE #%u v0=(%.4f,%.4f) v1=(%.4f,%.4f) ofx=%d ofy=%d fbmsk=%08x -> y0=%.4f\n",
                                     o2, v0.x, v0.y, v1.x, v1.y, ofx, ofy, ctx.frame.fbmsk, y0);
                }
                if ((a % 5000u) == 0u)
                    std::fprintf(stderr, "[subspr] sprites with a fractional vertex: %u / %u\n", nFrac.load(), a);
            }
        }
        if (x0 > x1) { std::swap(x0, x1); std::swap(u0, u1); }
        if (y0 > y1) { std::swap(y0, y1); std::swap(tv0, tv1); }
        {   // [pixcenter] PS2X_PIXCENTER="dx,dy": the GS addresses pixel CORNERS, GL samples pixel CENTRES; the game's
            // half-texel UVs (u = x + 0.5 for 1:1 HUD art) only hit texel centres when the geometry is shifted by the
            // same half pixel -- otherwise bilinear draws land on texel BOUNDARIES (blur) and point draws one texel off,
            // and which of the two you get depends on batch breaks (the outline env's HUD wobble). Sprites too.
            static const std::array<float,2> s_pcS = [](){ std::array<float,2> r{0.5f,0.5f};   // default ON (display buffers only, see pixCenterDestOk); PS2X_PIXCENTER=0,0 reverts
                if (const char *v = std::getenv("PS2X_PIXCENTER")) std::sscanf(v, "%f,%f", &r[0], &r[1]);
                if (const char *v = std::getenv("PS2X_PIXCENTER_SPR")) std::sscanf(v, "%f,%f", &r[0], &r[1]);   // sprites separately (A/B)
                return r; }();
            if (pixCenterDestOk(ctx.frame.fbp)) { x0 += s_pcS[0]; x1 += s_pcS[0]; y0 += s_pcS[1]; y1 += s_pcS[1]; }
        }
        cmd.dx0 = x0; cmd.dy0 = y0; cmd.dx1 = x1; cmd.dy1 = y1;
        if (g_subDxW) { u0 -= (float)g_subDx0; u1 -= (float)g_subDx0; }   // [subdecode] window-relative
        cmd.su0 = u0; cmd.sv0 = tv0; cmd.su1 = u1; cmd.sv1 = tv1;
        if (s_depthOn) cmd.z = zNorm(v1);
        colorBytes(v1, cmd.r, cmd.g, cmd.b, cmd.a);
        {
            static const bool s_d = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            static int s_n = 0;
            const float area = (x1 - x0) * (y1 - y0);
            if (s_d && s_n < 40 && area > 150.0f && tme) // textured sprites (box fill / text glyphs)
            {
                std::fprintf(stderr, "[sprdiag] area=%.0f xy=(%.0f,%.0f)-(%.0f,%.0f) rawVC=(%u,%u,%u,%u) tint=(%u,%u,%u,%u) tfx=%u tbp0=%u\n",
                             area, x0, y0, x1, y1, v1.r, v1.g, v1.b, v1.a, cmd.r, cmd.g, cmd.b, cmd.a,
                             tfx, ctx.tex0.tbp0);
                ++s_n;
            }
        }
        r.recordCmd(cmd);
    }
    else
    {
        // PS2X_ZSAT (GPU side): cull triangles with WRAPPED z (negative z packed to ~16.7M —
        // near-plane crossers real hardware clips). With GPU depth off they'd paint giant
        // garbage wedges over the whole scene in draw order.
        {
            static const bool s_zsat = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_ZSAT"); return s_env; }(); return !(v && v[0] == '0'); }();
            {   // [zsatcount] how often does this cull actually fire? The .gs has ~2960 far
                // terrain verts/frame and our DrawCmd census sees ZERO far triangles, so either
                // this drops them or something upstream does.
                static const bool s_zsc = [](){ const char *v = std::getenv("PS2X_ZSATCOUNT"); return v && v[0] && v[0] != '0'; }();
                if (s_zsc) { static unsigned long nn=0, dd=0;
                    const bool farTri = (gs->m_vtxQueue[0].z > 12000000.0 || gs->m_vtxQueue[1].z > 12000000.0 || gs->m_vtxQueue[2].z > 12000000.0);
                    ++nn; if (farTri) ++dd;
                    if ((nn % 20000ul) == 0ul)
                        std::fprintf(stderr, "[zsatcount] tris reaching ZSAT=%lu  with a far vertex=%lu (%.2f%%)  zsat_on=%d\n",
                                     nn, dd, 100.0*dd/nn, s_zsat ? 1 : 0); }
            }
            if (s_zsat &&
                (gs->m_vtxQueue[0].z > 12000000.0 || gs->m_vtxQueue[1].z > 12000000.0 || gs->m_vtxQueue[2].z > 12000000.0))
                return true; // drop this triangle
        }
        // PS2X_QCULL (experiment): drop textured STQ triangles carrying a NEGATIVE q on any
        // vertex — behind-camera vertices that escaped the VU1 near-plane clip project with
        // w<0 (mirrored onto screen; the sky corner-patch / wedge class). Real hardware
        // never receives these because its clipper culls them. Visual A/B for the mechanism.
        {
            static const bool s_qcull = [](){ const char *v = std::getenv("PS2X_QCULL"); return v && v[0] && v[0] != '0'; }();
            if (s_qcull && tme && !fst &&
                (gs->m_vtxQueue[0].q < 0.0f || gs->m_vtxQueue[1].q < 0.0f || gs->m_vtxQueue[2].q < 0.0f))
                return true; // drop
        }
        // Triangle -> rlgl (normalized UV, per-vertex color).
        cmd.isTriangle = true;
        // Per-pixel S/Q is only valid when q is well behaved across the whole triangle. Console
        // q for tbp10752 runs -4.06 .. +2.30 -- it CROSSES ZERO -- so a per-pixel divide there
        // explodes (that is what flattened the sky and ground at PS2X_PERSPQ=2). The GS clips
        // such triangles before rasterising; we do not, so guard instead: all three q the same
        // sign and comfortably away from zero, otherwise fall back to the per-vertex quotient.
        bool perspSafe = false;
        if (tme && !fst)
        {
            {   // [qcensus] PS2X_QCENSUS=1: OUR per-source q range, to compare against the same
                // census taken from the .gs stream.
                static const bool s_qc = [](){ const char *e = std::getenv("PS2X_QCENSUS");
                                               return e && e[0] && e[0] != '0'; }();
                if (s_qc)
                {
                    static std::map<uint32_t, std::array<double,4>> st;  // min,max,sum,count
                    static unsigned long n = 0;
                    for (int k2 = 0; k2 < 3; ++k2)
                    {
                        const float q2 = gs->m_vtxQueue[k2].q;
                        if (!(q2 == q2)) continue;
                        auto &e2 = st[ctx.tex0.tbp0];
                        if (e2[3] == 0) { e2[0] = e2[1] = q2; }
                        e2[0] = std::min(e2[0], (double)q2); e2[1] = std::max(e2[1], (double)q2);
                        e2[2] += q2; e2[3] += 1;
                    }
                    if ((++n % 40000ul) == 0ul)
                    {
                        std::fprintf(stderr, "[qcensus] OUR q per source:\n");
                        std::vector<std::pair<double,uint32_t>> ord;
                        for (auto &kv : st) ord.push_back({-kv.second[3], kv.first});
                        std::sort(ord.begin(), ord.end());
                        for (size_t i2 = 0; i2 < ord.size() && i2 < 8; ++i2)
                        {
                            auto &e2 = st[ord[i2].second];
                            std::fprintf(stderr, "   tbp%-6u n=%-7.0f q min %.4f max %.4f mean %.4f\n",
                                         ord[i2].second, e2[3], e2[0], e2[1], e2[2]/e2[3]);
                        }
                    }
                }
            }
            const float qa = gs->m_vtxQueue[0].q, qb = gs->m_vtxQueue[1].q, qc = gs->m_vtxQueue[2].q;
            const float amin = std::min(std::min(std::fabs(qa), std::fabs(qb)), std::fabs(qc));
            perspSafe = (amin > 1e-4f) && ((qa > 0.0f) == (qb > 0.0f)) && ((qb > 0.0f) == (qc > 0.0f));
        }
        // [decalq] the ground-shadow decal class (CT24 read of the Pass-1 silhouette, DATE, blend 0x44)
        const bool shadowDecalClass = tme && ctx.tex0.tbp0 == 10752u && ctx.tex0.psm == 1u && (((ctx.test >> 14) & 1u) != 0u) && ((ctx.alpha & 0xFFu) == 0x44u);
        for (int i = 0; i < 3; ++i)
        {
            const GSVertex &v = gs->m_vtxQueue[i];
            // PS2X_SUBPIXEL (default ON): keep the GS 12.4 sub-pixel fraction instead of truncating.
            // Console puts 99.5% of BT3's character vertices on a fractional coordinate, so
            // truncating snaps every silhouette edge inwards to a pixel boundary and eats the
            // outermost texel row -- which is where the cel texture's dark outline lives.
            //
            // This was tried once before (2026-08-04) and reverted for "massive lag, no visible
            // gain; the final image is replaced by the RT strip chain anyway". Both halves of
            // that have changed: the RT strip chain no longer replaces the image (PS2X_RTNEUTRAL
            // is default OFF since 2026-08-24), and the lag came from sub-pixel coords defeating
            // the integer zero-area cull above, so every micro-triangle survived. The cull is
            // left on its integer test here -- only the POSITION handed to GL gains precision, so
            // surviving-triangle count is bit-identical and a live in-fight A/B measured no cost
            // (30.00 -> 29.98 fps, 1,008,500 -> 1,006,787 prims/sec). PS2X_SUBPIXEL=0 disables.
            static const bool s_subpx = [](){ const char *v = std::getenv("PS2X_SUBPIXEL");
                                              return !(v && v[0] == '0'); }();
            // PS2X_PIXCENTER="dx,dy" (default off): shift triangle positions by a constant.
            // The GS addresses pixel CORNERS while GL samples at pixel CENTRES, which would be a
            // uniform half-pixel bias. Cross-correlating our frame against the console screenshot
            // over the CHARACTER (the only high-frequency region, so the only well-constrained
            // one) puts the optimum at exactly half a pixel diagonally.
            static const std::array<float,2> s_pc0 = [](){ std::array<float,2> r{0.5f,0.5f};   // default ON (display buffers only, see pixCenterDestOk); PS2X_PIXCENTER=0,0 reverts
                if (const char *v = std::getenv("PS2X_PIXCENTER")) std::sscanf(v, "%f,%f", &r[0], &r[1]);
                return r; }();
            // [pixcenter] only draws INTO THE DISPLAY buffers get the shift (PS2X_PIXCENTER_FBPS, default "0,112"):
            // shifting the render-target passes moved the shadow silhouette into its region's border row/column and
            // the decal clamp smeared it into a dark ground band (user 2026-08-29, replay hud.gs fr345).
            const std::array<float,2> s_pc = pixCenterDestOk(ctx.frame.fbp) ? s_pc0 : std::array<float,2>{0.0f, 0.0f};
            if (s_subpx)
            {
                cmd.tri[i].x = v.x - static_cast<float>(ofx) + s_pc[0];
                cmd.tri[i].y = v.y - static_cast<float>(ofy) + s_pc[1];
            }
            else
            {
                cmd.tri[i].x = static_cast<float>(static_cast<int>(v.x) - ofx);
                cmd.tri[i].y = static_cast<float>(static_cast<int>(v.y) - ofy);
            }
            float u, tv; texelUV(v, u, tv);
            cmd.tri[i].u = tme ? (u / static_cast<float>(texW)) : 0.0f;
            cmd.tri[i].v = tme ? (tv / static_cast<float>(texH)) : 0.0f;
            // PS2X_PERSPQ=1: hand the shader RAW s,t and q instead of the per-vertex quotient,
            // so the divide happens per pixel the way the GS does it.
            // 2026-08-29 [prmode]: the ground-shadow decal (CT24 read of the Pass-1 silhouette at tbp 10752, DATE,
            // blend 0x44) is a projective decal -- its s/q,t/q sweep across each floor tile, so the per-vertex
            // quotient lands only a sliver of the silhouette (228 px vs console's 4649). Mode 5 = that class only,
            // and it is the DEFAULT when PS2X_PERSPQ is unset; PS2X_PERSPQ=0 disables every per-pixel divide.
            static const bool s_perspQ = [](){ const char *e = std::getenv("PS2X_PERSPQ");
                                               return !(e && e[0] == '0'); }();
            // Scope: PS2X_PERSPQ=1 applies ONLY to the cel/outline ramp (tbp 15680), which is
            // what needs it. Applied to every FST=0 draw it also re-samples the sky and terrain,
            // whose large quads visibly lose detail. PS2X_PERSPQ=2 = all FST=0 draws (A/B).
            static const int s_perspMode = [](){ const char *e = std::getenv("PS2X_PERSPQ");
                                                 return e && e[0] ? std::atoi(e) : 5; }();
            // 1 = cel ramp only (tbp 15680); 3 = characters + ramp (tbp 10752 is the character
            // texture staging base, tbp 13xxx is terrain/sky and is what visibly degrades);
            // 2 = every FST=0 draw, which is the faithful GS behaviour but exposes bad q on the
            // background geometry.
            // Mode 4 = every FST=0 source EXCEPT tbp10752. Our q census matches console exactly
            // for 15680/13440/13672/13840/16064, but tbp10752 does NOT (ours 0.0000..0.3806 vs
            // console -4.0635..2.2991) -- console clips those triangles and we keep them, so a
            // per-pixel divide there uses a q we got wrong and wrecks the sky.
            const bool perspThis = s_perspQ &&
                (s_perspMode == 5 ? (shadowDecalClass && [](){ static const bool r = [](){ const char *v = std::getenv("PS2X_DECALRAW"); return v && v[0] == '1'; }(); return r; }())
                 : s_perspMode == 6 ? ((ctx.tex0.psm == 19u || ctx.tex0.psm == 20u) && ctx.tex0.cbp == 12992u
                                        && std::min(std::min(std::fabs(gs->m_vtxQueue[0].q), std::fabs(gs->m_vtxQueue[1].q)), std::fabs(gs->m_vtxQueue[2].q)) > 0.01f)   // [grassq] NEAR ground only (q>0.01): per-pixel S/Q stops the affine texture swim; far tris (tiny q) keep the per-vertex quotient -- their affine error is invisible and tiny-q division smears
                 : s_perspMode == 4 ? (ctx.tex0.tbp0 != 10752u)
                 : s_perspMode == 3 ? (ctx.tex0.tbp0 == 10752u || ctx.tex0.tbp0 == 15680u)
                 : s_perspMode == 2 ? true
                 : ctx.tex0.tbp0 == 15680u);
            // [decalq] the shadow decal keeps the per-pixel path even when q changes sign across the tile: the GS divides
            // per pixel (q<=0 -> coordinates fly to +-inf -> CLAMP border = black = nothing); the affine fallback swept the
            // silhouette across the whole tile (the "wedge" on the grass). The shader discards fragments with q <= 0.
            if (perspThis && (perspSafe || shadowDecalClass))
            {
                cmd.tri[i].u = v.s;
                cmd.tri[i].v = v.t;
                cmd.tri[i].q = v.q;
            }
            else cmd.tri[i].q = 1.0f;
            // [stq] PS2X_STQ=<tbp>: the RAW per-vertex S/T/Q the runtime holds, next to the u we
            // derive from them. The .gs stream says this class's u splits ~49/51 either side of
            // the ramp's step at u=0.5; if our histogram is not that, the ST/Q the runtime
            // latched differ from the ones the stream actually sent.
            {
                static const int s_stq = [](){ const char *e = std::getenv("PS2X_STQ");
                                               return e && e[0] ? std::atoi(e) : -1; }();
                if (s_stq >= 0 && tme && (int)ctx.tex0.tbp0 == s_stq)
                {
                    static unsigned long n = 0, lo = 0, hi = 0, zero = 0;
                    const float uu = cmd.tri[i].u;
                    ++n; if (uu < 0.5f) ++lo; else ++hi;
                    if (uu < 0.004f) ++zero;
                    if (n <= 4 || (n % 20000ul) == 0ul)
                        std::fprintf(stderr, "[stq] #%lu s=%.6f t=%.6f q=%.6f -> u=%.4f v=%.4f | u<0.5: %.1f%%  u>=0.5: %.1f%%  u~0: %.1f%%  (texW=%d fst=%u)\n",
                                     n, v.s, v.t, v.q, cmd.tri[i].u, cmd.tri[i].v,
                                     100.0*lo/n, 100.0*hi/n, 100.0*zero/n, texW, (unsigned)fst);
                }
            }
            if (s_depthOn) cmd.tri[i].z = zNorm(v);
            colorBytes(v, cmd.tri[i].r, cmd.tri[i].g, cmd.tri[i].b, cmd.tri[i].a);
        }
        {   // [flattri] GS IIP=0 = FLAT shading: the whole triangle takes the LAST vertex's
            // colour. We recorded per-vertex colours and rendered them Gouraud, which smears
            // BT3's noisy flat-lit terrain colours (console-identical data, verified vs
            // ref_native.gs) into large soft dark/light patches on hills -- the "moving
            // dark areas". Console's flat facets are ~6px and read as grass texture instead.
            // The line path already did this (colU/iip above); triangles now match.
            // PS2X_FLATTRI=0 restores the old Gouraud-always behaviour.
            static const bool s_flatTri = [](){ const char *v = std::getenv("PS2X_FLATTRI"); return !(v && v[0] == '0'); }();
            {   static const bool s_ftd = [](){ const char *v = std::getenv("PS2X_FLATTRIDIAG"); return v && v[0] && v[0] != '0'; }();
                static unsigned long fn2 = 0, neq = 0;
                if (s_ftd && !gs->m_prim.iip && tme && ctx.tex0.psm == 0x13)
                {
                    ++fn2;
                    if (cmd.tri[0].r != cmd.tri[2].r || cmd.tri[1].r != cmd.tri[2].r) ++neq;
                    if (fn2 <= 6 || (fn2 % 100000ul) == 0ul)
                        std::fprintf(stderr, "[flattridiag] #%lu tri cols r=(%u,%u,%u) unequal-so-far=%lu/%lu\n",
                                     fn2, cmd.tri[0].r, cmd.tri[1].r, cmd.tri[2].r, neq, fn2);
                }
            }
            if (s_flatTri && !gs->m_prim.iip)
                for (int k2 = 0; k2 < 2; ++k2)
                { cmd.tri[k2].r = cmd.tri[2].r; cmd.tri[k2].g = cmd.tri[2].g;
                  cmd.tri[k2].b = cmd.tri[2].b; cmd.tri[k2].a = cmd.tri[2].a; }
        }
        // [wedgerec2] (PS2X_WEDGEREC): THE intro-pan wedge signature nailed by the F9 capture
        // (gen2687 ci=673-675 etc): a huge far-z terrain-textured clip-fan whose verts pin at
        // BOTH guard clamps (x=-1499 left, ~2010 right-edge apex) with z<0.001. Dump the
        // originating VU1 kick (data+micro+entry state) for offline replay.
        {
            static const bool s_wr = [](){ const char *v = std::getenv("PS2X_WEDGEREC"); return v && v[0] && v[0] != '0'; }();
            if (s_wr && tme)
            {
                float mnx = cmd.tri[0].x, mxx = mnx, mxz = cmd.tri[0].z;
                for (int i = 1; i < 3; ++i)
                {
                    mnx = std::min(mnx, cmd.tri[i].x); mxx = std::max(mxx, cmd.tri[i].x);
                    mxz = std::max(mxz, cmd.tri[i].z);
                }
                if (mnx < -1400.0f && mxx > 1900.0f && mxz < 0.001f)
                {
                    extern thread_local uint32_t g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr;
                    extern thread_local const uint8_t *g_xgkickVuData;
                    extern thread_local uint32_t g_xgkickVuDataSize;
                    extern thread_local const uint8_t *g_xgkickVuCode;
                    extern thread_local uint32_t g_xgkickVuCodeSize;
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t n = s_n.fetch_add(1);
                    if (n < 12u)
                        std::fprintf(stderr, "[wedgerec2] #%u entryPc=%u top=%u kickAddr=%u v0=(%.0f,%.0f) v1=(%.0f,%.0f) v2=(%.0f,%.0f)\n",
                                     n, g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr,
                                     cmd.tri[0].x, cmd.tri[0].y, cmd.tri[1].x, cmd.tri[1].y, cmd.tri[2].x, cmd.tri[2].y);
                    static std::atomic<bool> s_dumped{false};
                    bool exp0 = false;
                    if (g_xgkickVuData && s_dumped.compare_exchange_strong(exp0, true))
                    {
                        FILE *dm = std::fopen("/home/z3/Desktop/bt3/work/wedge2_data.bin", "wb");
                        if (dm) { std::fwrite(g_xgkickVuData, 1, g_xgkickVuDataSize, dm); std::fclose(dm); }
                        if (g_xgkickVuCode)
                        {
                            FILE *mc = std::fopen("/home/z3/Desktop/bt3/work/wedge2_micro.bin", "wb");
                            if (mc) { std::fwrite(g_xgkickVuCode, 1, g_xgkickVuCodeSize, mc); std::fclose(mc); }
                        }
                        extern thread_local const uint8_t *g_xgkickEntryStateBytes;
                        extern thread_local uint32_t g_xgkickEntryStateSize;
                        if (g_xgkickEntryStateBytes)
                        {
                            FILE *st = std::fopen("/home/z3/Desktop/bt3/work/wedge2_state.bin", "wb");
                            if (st) { std::fwrite(g_xgkickEntryStateBytes, 1, g_xgkickEntryStateSize, st); std::fclose(st); }
                        }
                        std::fprintf(stderr, "[wedgerec2] snapshot written entryPc=%u top=%u kickAddr=%u\n",
                                     g_xgkickEntryPc, g_xgkickTop, g_xgkickKickAddr);
                    }
                }
            }
        }
        // SPS spike triangles: 1-2 garbage vertices out of VU1 land far across the 4096px GS
        // space, making triangles that span thousands of px (screen diagonal is ~780px; the
        // observed spikes are 3500px+).
        //   PS2X_SPIKE=1        log each offender's raw vertices (rate-limited) to identify the source
        //   PS2X_SPIKE_CULL=N   drop triangles whose screen-space extent exceeds N px (0=off)
        {
            static const bool s_spkLog = [](){ const char *v = std::getenv("PS2X_SPIKE"); return v && v[0] && v[0] != '0'; }();
            static const float s_spkCull = [](){ const char *v = std::getenv("PS2X_SPIKE_CULL"); return v ? (float)std::strtoul(v, nullptr, 0) : 0.0f; }();
            // PS2X_SPIKE_MIN: log threshold in px (default 1200 = the classic VU1 SPS spikes;
            // lower it to catch the moderate screen-sized popup triangles).
            static const float s_spkMin = [](){ const char *v = std::getenv("PS2X_SPIKE_MIN"); return v ? (float)std::strtoul(v, nullptr, 0) : 1200.0f; }();
            if (s_spkLog || s_spkCull > 0.0f)
            {
                float mnx = cmd.tri[0].x, mxx = mnx, mny = cmd.tri[0].y, mxy = mny;
                for (int i = 1; i < 3; ++i)
                {
                    mnx = std::min(mnx, cmd.tri[i].x); mxx = std::max(mxx, cmd.tri[i].x);
                    mny = std::min(mny, cmd.tri[i].y); mxy = std::max(mxy, cmd.tri[i].y);
                }
                const float ext = std::max(mxx - mnx, mxy - mny) / 16.0f; // 12.4 -> px
                if (s_spkLog && ext > s_spkMin)
                {
                    static std::atomic<uint32_t> s_sn{0};
                    uint32_t n = s_sn.fetch_add(1) + 1u;
                    if (n <= 40 || (n % 512u) == 0u)
                    {
                        std::fprintf(stderr, "[spike] #%u ext=%.0fpx tme=%d tex=%dx%d destFbp=%u |", n, ext, tme ? 1 : 0, texW, texH, ctx.frame.fbp);
                        for (int i = 0; i < 3; ++i)
                        {
                            const GSVertex &v = gs->m_vtxQueue[i];
                            std::fprintf(stderr, " v%d raw=(0x%04x,0x%04x) px=(%d,%d) z=%.0f q=%g stq=(%g,%g)", i,
                                         (unsigned)(uint16_t)v.x, (unsigned)(uint16_t)v.y,
                                         ((int)v.x - ofx) >> 4, ((int)v.y - ofy) >> 4, (double)v.z, v.q, v.s, v.t);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
                if (s_spkCull > 0.0f && ext > s_spkCull)
                    return true; // drop the spike triangle
            }
        }
        // PS2X_3DBBOX: track the screen bounding box (in px) of TRIANGLE geometry, split by whether
        // the texture is "big" (fighter/stage skins, >=64x64) vs small (HUD). Reveals if the 3D
        // fighters are collapsed to a sliver or rendering at a normal size but unseen.
        {
            static const bool s_bb = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_3DBBOX"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            if (s_bb && tme && texW >= 64 && texH >= 64)
            {
                static std::atomic<uint32_t> s_bn{0};
                static int minx=1<<20, miny=1<<20, maxx=-(1<<20), maxy=-(1<<20);
                for (int i = 0; i < 3; ++i) {
                    const int px = ((int)gs->m_vtxQueue[i].x - ofx) >> 4;
                    const int py = ((int)gs->m_vtxQueue[i].y - ofy) >> 4;
                    if (px<minx)minx=px; if (px>maxx)maxx=px; if (py<miny)miny=py; if (py>maxy)maxy=py;
                }
                const uint32_t n = s_bn.fetch_add(1);
                if ((n % 8000u) == 1u) {
                    std::fprintf(stderr, "[3dbbox] after %u big-tex tris: screen bbox x=[%d..%d] y=[%d..%d] (w=%d h=%d) destFbp=%u lastTex=%dx%d\n",
                                 n, minx, maxx, miny, maxy, maxx-minx, maxy-miny, ctx.frame.fbp, texW, texH);
                }
            }
        }
        // PS2X_DARKPROBE: sample the fight's 3D triangles -> where do they render (destFbp vs
        // display), what modulate mode, raw vs computed vertex color + texture alpha coverage.
        {
            static const bool s_dk = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_DARKPROBE"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            if (s_dk)
            {
                static std::atomic<uint32_t> s_dn{0};
                const uint32_t n = s_dn.fetch_add(1);
                if ((n % 4000u) == 0u)
                {
                    const GSVertex &v0 = gs->m_vtxQueue[0];
                    std::fprintf(stderr, "[dark] #%u destFbp=%u tme=%u tfx=%u tcc=%u abe=%u | rawVC=(%u,%u,%u,%u) -> cmdVC=(%u,%u,%u,%u) tex=%llu %dx%d\n",
                                 n, ctx.frame.fbp, tme, tfx, tcc, gs->m_prim.abe,
                                 v0.r, v0.g, v0.b, v0.a, cmd.tri[0].r, cmd.tri[0].g, cmd.tri[0].b, cmd.tri[0].a,
                                 (unsigned long long)texKey, texW, texH);
                }
            }
        }
        {
            static const bool s_rv = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_GPU_DIAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            static int s_rn = 0;
            const int spanX = std::abs(static_cast<int>(gs->m_vtxQueue[0].x) - static_cast<int>(gs->m_vtxQueue[1].x));
            if (s_rv && texKey == 17974423536168675289ull && s_rn < 14 && spanX > 60)
            {
                ++s_rn;
                std::fprintf(stderr, "[boxraw] tw=%u th=%u texWH=%dx%d fst=%u tex1=%llu | ",
                             ctx.tex0.tw, ctx.tex0.th, texW, texH, gs->m_prim.fst, (unsigned long long)ctx.tex1);
                for (int i = 0; i < 3; ++i) {
                    const GSVertex &v = gs->m_vtxQueue[i];
                    std::fprintf(stderr, "v%d xy=(%d,%d) uFP=%u vFP=%u s=%.4f t=%.4f q=%.4f -> uv=(%.3f,%.3f)  ",
                                 i, static_cast<int>(v.x) - ofx, static_cast<int>(v.y) - ofy,
                                 v.u, v.v, v.s, v.t, v.q, cmd.tri[i].u, cmd.tri[i].v);
                }
                std::fprintf(stderr, "\n");
            }
        }
        // PERSPECTIVE-CORRECT TEXTURING (default ON, PS2X_PERSPFIX=0 disables): the GL
        // replay draws screen-space triangles under an ORTHO projection (w=1), so UVs
        // interpolate AFFINELY. Small triangles are fine; the big near-field terrain
        // triangles smear their texture into streaks/flat color (the "dark green ground",
        // and the banding family). GS STQ attributes are LINEAR in screen space, so
        // subdividing large tris and re-dividing s/q,t/q per new vertex converges to the
        // hardware's per-pixel perspective mapping.
        {
            static const bool s_pf = [](){ const char *v = std::getenv("PS2X_PERSPFIX"); return !(v && v[0] == '0'); }(); // default ON (error-driven, cheap)
            bool subdivided = false;
            if (s_pf && tme && !fst)
            {
                struct SV { float x, y, zn; float s, t, q; uint8_t r, g, b, a; };
                SV sv[3];
                bool qOk = true;
                float qMin = 1e30f, qMax = -1e30f;
                for (int i = 0; i < 3; ++i)
                {
                    const GSVertex &v = gs->m_vtxQueue[i];
                    sv[i].x = cmd.tri[i].x; sv[i].y = cmd.tri[i].y; sv[i].zn = cmd.tri[i].z;
                    sv[i].s = v.s; sv[i].t = v.t; sv[i].q = (v.q != 0.0f) ? v.q : 1.0f;
                    sv[i].r = cmd.tri[i].r; sv[i].g = cmd.tri[i].g; sv[i].b = cmd.tri[i].b; sv[i].a = cmd.tri[i].a;
                    if (!(sv[i].q > 0.0f)) qOk = false;
                    qMin = std::min(qMin, sv[i].q); qMax = std::max(qMax, sv[i].q);
                }
                // [grasssub] PS2X_GRASSSUB=<depthCap>,<errTexels>: tighter subdivision for the
                // ground/hills family (psm19/20, CLUT 12992). The generic 3-texel error cap is
                // invisible on minified textures but the NEAR ground is MAGNIFIED (1 texel =
                // several screen px), so its residual affine error swims with camera motion
                // ("grass feels like water"). Unset = off (generic caps).
                static int s_gcap = 7; static float s_gerr = 1.2f;   // [grasspx] err cap in SCREEN PIXELS for the grass class (texels for the others); 1.2 measured visually identical to 0.8 on the near ground (diff 0.21) while running at the grass-off fps ceiling (36.7 vs 28.5 at 0.8)
                // DEFAULT ON (user-accepted 2026-09-02, "grass feels like water" fixed): unset = 6,0.7
                // for every indexed STQ terrain draw (psm 19/20, any CLUT — the palette base is
                // stage-specific). PS2X_GRASSSUB=0 disables; =<d>,<e> overrides the caps.
                static const bool s_gsub = [](){ const char *v = std::getenv("PS2X_GRASSSUB");
                    if (!v || !v[0]) return true;
                    if (v[0] == '0' && !v[1]) return false;
                    std::sscanf(v, "%d,%f", &s_gcap, &s_gerr); return true; }();
                const bool grassClass = s_gsub && (ctx.tex0.psm == 19u || ctx.tex0.psm == 20u);
                // Only bother when there is real perspective across the triangle AND it is
                // large on screen — flat-q or small tris are exact enough affinely.
                const float ex = std::max({sv[0].x, sv[1].x, sv[2].x}) - std::min({sv[0].x, sv[1].x, sv[2].x});
                const float ey = std::max({sv[0].y, sv[1].y, sv[2].y}) - std::min({sv[0].y, sv[1].y, sv[2].y});
                const float extent = std::max(ex, ey); // cmd coords are already PIXELS
                // Error-driven gate: affine-vs-perspective UV divergence at the edge
                // midpoints, in TEXELS. Only triangles whose smear would actually be
                // visible (> ~3 texels) get subdivided — the overwhelming majority of
                // draws skip this entirely (the blanket size gate was a slideshow).
                float uvErrTexels = 0.0f;
                if (qOk && extent > 24.0f && qMax > 1.01f * qMin)
                {
                    for (int e = 0; e < 3; ++e)
                    {
                        const SV &A = sv[e], &B = sv[(e + 1) % 3];
                        const float qm = (A.q + B.q) * 0.5f;
                        if (qm <= 0.0f) continue;
                        const float uAff = (A.s / A.q + B.s / B.q) * 0.5f;
                        const float vAff = (A.t / A.q + B.t / B.q) * 0.5f;
                        const float uPer = ((A.s + B.s) * 0.5f) / qm;
                        const float vPer = ((A.t + B.t) * 0.5f) / qm;
                        const float du = std::fabs(uAff - uPer) * static_cast<float>(texW);
                        const float dv = std::fabs(vAff - vPer) * static_cast<float>(texH);
                        uvErrTexels = std::max({uvErrTexels, du, dv});
                    }
                }
                // [decalq] a shadow-decal tile whose q changes sign straddles the projector plane: subdivide it anyway and
                // drop the pieces that contain q <= 0 (the GS maps them to +-inf = clamped border = nothing); the affine
                // fallback used to sweep the silhouette across the whole tile (the "wedge" on the grass).
                static const bool s_decalQ = [](){ const char *v = std::getenv("PS2X_DECALQ"); return !(v && v[0] == '0'); }();   // =0: old affine fallback (A/B)
                if (shadowDecalClass && !qOk)
                {   // [decalq] log every mixed-sign-q decal tile (the wedge source) with a timestamp for screenshot matching
                    static const auto t0 = std::chrono::steady_clock::now(); static int n = 0;
                    if (n++ < 2000)
                    {
                        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                        std::fprintf(stderr, "[decalq] t=%ldms unsafe tile q %.4f %.4f %.4f xy (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) %s\n", ms, sv[0].q, sv[1].q, sv[2].q,
                                     sv[0].x, sv[0].y, sv[1].x, sv[1].y, sv[2].x, sv[2].y, s_decalQ ? "subdivide+drop" : "AFFINE (old)");
                    }
                }
                if (s_decalQ && shadowDecalClass && !qOk) uvErrTexels = 1e9f;
                if ((qOk || shadowDecalClass) && uvErrTexels > (grassClass ? 0.0f : 3.0f) && cmd.tri[0].q == 1.0f)
                {
                    static const int s_gbB = [](){ const char *v = std::getenv("PS2X_GRASSBUDGET");
                                                   return v && v[0] ? std::atoi(v) : 1200; }();   // [grassbudget]
                    static thread_local int s_gbCount = 0;
                    static thread_local float s_gbScale = 1.0f;
                    static thread_local std::chrono::steady_clock::time_point s_gbT0{};
                    // [drawbatch] Every piece of a subdivided triangle shares the whole command but its
                    // three vertices -- the batch's exact shape. The first piece opens the batch through
                    // recordCmd (which does the barrier/format bookkeeping); the rest append straight
                    // into it, skipping the ~330 B copy and the whole bookkeeping the head already did.
                    // pieceSeq goes stale the moment anything else records, which drops us back to the
                    // full path. This is the hot path at Kame House: the grass subdivision is ~70% of
                    // every primitive recorded in a fight.
                    uint64_t pieceSeq = ~0ull;
                    auto emitTri = [&](const SV &a, const SV &b, const SV &c2)
                    {
                        const SV *vv[3] = {&a, &b, &c2};
                        if (s_decalQ && shadowDecalClass && (a.q <= 1e-4f || b.q <= 1e-4f || c2.q <= 1e-4f)) return;   // [decalq]
                        if (shadowDecalClass)
                        {   // [decalgarb] BT3 hands the far fighter's floor tiles vertices with GARBAGE s/t (console 1853: s=-496.6
                            // t=-214 at q=0.0037 -> s/q = -133000). The GS's per-pixel divide sweeps those past the texture into the
                            // clamped black border within a sub-pixel band; an affine piece emitted at the depth cap next to such
                            // a corner sweeps the whole silhouette across a visible band instead (the "wedge" near a distant Gohan).
                            const SV *pv[3] = {&a, &b, &c2};
                            for (int i = 0; i < 3; ++i)
                                if (std::fabs(pv[i]->s / pv[i]->q) > 8.0f || std::fabs(pv[i]->t / pv[i]->q) > 8.0f) return;
                        }
                        for (int i = 0; i < 3; ++i)
                        {
                            cmd.tri[i].x = vv[i]->x; cmd.tri[i].y = vv[i]->y; cmd.tri[i].z = vv[i]->zn;
                            // texelUV normalization is u = (s/q)*texW / texW = s/q (atlas 0..1)
                            cmd.tri[i].u = vv[i]->s / vv[i]->q;
                            cmd.tri[i].v = vv[i]->t / vv[i]->q;
                            cmd.tri[i].q = 1.0f;   // [perspq] sub-vertices carry the quotient: no per-pixel divide
                            cmd.tri[i].r = vv[i]->r; cmd.tri[i].g = vv[i]->g; cmd.tri[i].b = vv[i]->b; cmd.tri[i].a = vv[i]->a;
                        }
                        if (pieceSeq != r.batchSeq() || !r.batchOpen() || !r.appendBatchTri(cmd.tri))
                            r.recordCmd(cmd);   // [drawbatch] first piece (or a batch that closed): full command
                        pieceSeq = r.batchSeq();
                        if (grassClass) ++s_gbCount;   // [grassbudget]
                    };
                    auto mid = [](const SV &a, const SV &b)
                    {
                        SV m;
                        m.x = (a.x + b.x) * 0.5f; m.y = (a.y + b.y) * 0.5f; m.zn = (a.zn + b.zn) * 0.5f;
                        m.s = (a.s + b.s) * 0.5f; m.t = (a.t + b.t) * 0.5f; m.q = (a.q + b.q) * 0.5f;
                        m.r = static_cast<uint8_t>((static_cast<int>(a.r) + b.r) >> 1); m.g = static_cast<uint8_t>((static_cast<int>(a.g) + b.g) >> 1);
                        m.b = static_cast<uint8_t>((static_cast<int>(a.b) + b.b) >> 1); m.a = static_cast<uint8_t>((static_cast<int>(a.a) + b.a) >> 1);
                        return m;
                    };
                    // Iterative 4-way subdivision, terminating per PIECE as soon as its own
                    // affine error drops under the texel threshold (error shrinks ~4x per
                    // level, so this converges in 1-3 levels; depth cap is a backstop).
                    // [grassdiv] perf: this loop was the top block of recordSpriteGPU in the
                    // spin-dip profile (the annotate's divss cluster around s_gerr). The err and
                    // cap formulas below share the per-vertex reciprocals (3 divides) instead of
                    // recomputing s/q per edge per pass (~20 divides/piece before).
                    auto pieceErr = [&](const SV &a, const SV &b, const SV &c2,
                                        const float *ru, const float *rv) -> float
                    {   // ru/rv = per-vertex s/q, t/q (atlas units) precomputed by the caller
                        float err = 0.0f;
                        const SV *pv[3] = {&a, &b, &c2};
                        for (int e = 0; e < 3; ++e)
                        {
                            const SV &A = *pv[e], &B = *pv[(e + 1) % 3];
                            const float qm = (A.q + B.q) * 0.5f;
                            if (qm <= 0.0f) continue;
                            const float rqm = 1.0f / qm;
                            const int e2 = (e + 1) % 3;
                            const float du = std::fabs((ru[e] + ru[e2]) * 0.5f - (A.s + B.s) * 0.5f * rqm) * static_cast<float>(texW);
                            const float dv = std::fabs((rv[e] + rv[e2]) * 0.5f - (A.t + B.t) * 0.5f * rqm) * static_cast<float>(texH);
                            err = std::max({err, du, dv});
                        }
                        return err;
                    };
                    // [grassbudget] load governor: the spin-dip class (sweep+X drops to ~20 fps)
                    // is ENTIRELY the grass-piece record volume (GRASSSUB=0 sweep = 29.96/29.0,
                    // zero dips). Pieces emitted in the current ~33 ms window scale the NEXT
                    // window's error cap: standing still emits few pieces (full anti-swim
                    // quality -- exactly where swim is visible), a camera spin coarsens the cap
                    // instead of dropping frames (motion masks the residual swim).
                    // PS2X_GRASSBUDGET=<pieces> (default 1200; 0 = governor off).
                    if (s_gbB > 0)
                    {
                        const auto nowG = std::chrono::steady_clock::now();
                        if (s_gbT0.time_since_epoch().count() == 0) s_gbT0 = nowG;
                        else if (nowG - s_gbT0 > std::chrono::milliseconds(33))
                        {
                            if (s_gbCount > s_gbB)              s_gbScale = std::min(s_gbScale * 1.3f, 8.0f);
                            else if (s_gbCount < s_gbB / 2)     s_gbScale = std::max(s_gbScale * 0.8f, 1.0f);
                            s_gbCount = 0; s_gbT0 = nowG;
                        }
                    }
                    struct Item { SV a, b, c; int depth; };
                    static thread_local std::vector<Item> s_work;
                    s_work.clear();
                    s_work.push_back({sv[0], sv[1], sv[2], 0});
                    while (!s_work.empty())
                    {
                        Item it = s_work.back();
                        s_work.pop_back();
                        // [decalq] the shadow decal needs a finer split: an affine piece with 3 texels of error at the
                        // silhouette edge paints a whole sliver the GS does not (console decal 1853: 15 px, ours 16+38).
                        // [decalsub] PS2X_DECALSUB=<depthCap>,<errTexels> for the shadow-decal class (default 5,3 = the generic rule);
                        // errCap 1/depth 7 removed the far-fighter sliver but cost 30 -> 20 fps (7000 pieces/frame).
                        // Default for the decal: depth cap 7, err 3 -- pieces that hit the old cap of 5 were emitted with a
                        // large residual error and painted the sliver; cap 7 removes it at +35% pieces (2311 vs 1705/frame).
                        static int s_dcap = 7; static float s_derr = 3.0f;
                        static const bool s_dsub = [](){ const char *v = std::getenv("PS2X_DECALSUB"); return v && std::sscanf(v, "%d,%f", &s_dcap, &s_derr) == 2; }();
                        (void)s_dsub;
                        const int depthCap = shadowDecalClass ? s_dcap : (grassClass ? s_gcap : 5); const float errCap = shadowDecalClass ? s_derr : (grassClass ? s_gerr : 3.0f);
                        if (it.depth >= depthCap) { emitTri(it.a, it.b, it.c); continue; }   // [grassdiv] no math needed
                        if (it.depth > 0)
                        {   // [grassdiv] the root always splits (it.depth > 0 emit guard -- the
                            // grasspop fix), so its cap/err math was pure waste; and both passes
                            // now share the per-vertex s/q, t/q reciprocals computed once here.
                            const SV *pv[3] = {&it.a, &it.b, &it.c};
                            float ru[3], rv[3];
                            for (int i3 = 0; i3 < 3; ++i3)
                            {
                                const float q = (std::fabs(pv[i3]->q) > 1e-6f) ? pv[i3]->q : 1.0f;
                                const float rq = 1.0f / q;
                                ru[i3] = pv[i3]->s * rq; rv[i3] = pv[i3]->t * rq;
                            }
                            float pieceCap = errCap;
                            if (grassClass)
                            {   // [grasspx] convert the SCREEN-PIXEL cap to this piece's texel scale: the near
                                // ground magnifies texels (tight cap where swim was visible) while far terrain
                                // minifies them (its sub-pixel affine error never mattered — stop immediately;
                                // always-subdivide-everything cost ~12 fps: 36.5 -> 23.8 in the fight A/B).
                                const float exPx = std::max({it.a.x, it.b.x, it.c.x}) - std::min({it.a.x, it.b.x, it.c.x});
                                const float eyPx = std::max({it.a.y, it.b.y, it.c.y}) - std::min({it.a.y, it.b.y, it.c.y});
                                const float extPx = std::max(exPx, eyPx);
                                const float u0 = ru[0] * (float)texW, u1 = ru[1] * (float)texW, u2 = ru[2] * (float)texW;
                                const float v0 = rv[0] * (float)texH, v1 = rv[1] * (float)texH, v2 = rv[2] * (float)texH;
                                const float uvSpan = std::max(std::max({u0,u1,u2}) - std::min({u0,u1,u2}),
                                                              std::max({v0,v1,v2}) - std::min({v0,v1,v2}));
                                const float pxPerTex = (uvSpan > 1e-3f) ? (extPx / uvSpan) : 1.0f;
                                pieceCap = (s_gerr * s_gbScale) / std::max(pxPerTex, 1e-3f);   // err_texels * pxPerTex <= s_gerr*scale px  [grassbudget]
                            }
                            if (pieceErr(it.a, it.b, it.c, ru, rv) <= pieceCap)
                            {
                                emitTri(it.a, it.b, it.c);
                                continue;
                            }
                        }
                        const SV ab = mid(it.a, it.b), bc = mid(it.b, it.c), ca = mid(it.c, it.a);
                        s_work.push_back({it.a, ab, ca, it.depth + 1});
                        s_work.push_back({ab, it.b, bc, it.depth + 1});
                        s_work.push_back({ca, bc, it.c, it.depth + 1});
                        s_work.push_back({ab, bc, ca, it.depth + 1});
                    }
                    subdivided = true;
                }
            }
            if (!subdivided)
            {   // [drawbatch] three vertex stores into the open batch instead of a whole command
                if (!batchCont || !r.appendBatchTri(cmd.tri))
                    r.recordCmd(cmd);
            }
        }
    }
    s_bcmdSeq = r.batchSeq();   // [drawbatch] our command is the batch head until someone else records
    return true;
}

uint32_t GSRasterizer::sampleTexture(GS *gs, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = gs->activeContext();
    const auto &tex = ctx.tex0;
    {   // PS2X_UVLOG=1: the edge detect subtracts a +1-TEXEL-SHIFTED sample from an unshifted
        // one. If both passes sample the same texel the difference is zero everywhere, which is
        // exactly the all-black fbp336 we measure. Log (u,v) per pass at a fixed screen column.
        static const bool s_uv = [](){ const char *e = std::getenv("PS2X_UVLOG");
                                       return e && e[0] && e[0] != '0'; }();
        if (s_uv && ctx.frame.fbp == 336u && gs->m_prim.fst)
        {
            static std::map<uint32_t, int> s_seen;
            const uint32_t key = ctx.frame.fbmsk;
            if (s_seen[key]++ < 3)
                std::fprintf(stderr, "[uvlog] fbmsk=%08x  u=%.2f v=%.2f  (fst raw u=%u v=%u)\n",
                             key, u / 16.0f, v / 16.0f, (unsigned)u, (unsigned)v);
        }
    }

    // PS2X_TEXHL=<tbp0>[,<tw>]: paint every pixel sampled from that texture base (and,
    // if given, that log2-width — e.g. "10752,10" = only the 1024-wide sky panorama)
    // OPAQUE MAGENTA. Visual map of which on-screen surface uses which texture.
    {
        struct Hl { uint32_t tbp; int tw; };
        static const Hl s_hl = [](){
            Hl h{0u, -1};
            if (const char *v = [](){ static const char *s_env = std::getenv("PS2X_TEXHL"); return s_env; }()) {
                char *end = nullptr;
                h.tbp = (uint32_t)std::strtoul(v, &end, 0);
                if (end && *end == ',') h.tw = (int)std::strtol(end + 1, nullptr, 0);
            }
            return h;
        }();
        if (s_hl.tbp && tex.tbp0 == s_hl.tbp && (s_hl.tw < 0 || (int)tex.tw == s_hl.tw))
            return 0x80FF00FFu; // A=0x80 (opaque), B=0xFF, G=0x00, R=0xFF
    }

    int texW = 1 << tex.tw;
    int texH = 1 << tex.th;

    float texUf, texVf;
    if (gs->m_prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    // GS CLAMP register (wms bits0-1, wmt bits2-3, MINU 4-13, MAXU 14-23, MINV 24-33,
    // MAXV 34-43): 0=REPEAT, 1=CLAMP, 2=REGION_CLAMP (clamp into [MIN,MAX]),
    // 3=REGION_REPEAT (u = (u & MINU) | MAXU — tile a sub-rectangle of an atlas).
    // This sampler used to CLAMP unconditionally: the fight map's tiled negative UVs all
    // collapsed to texel (0,0) = the atlas' sky-blue corner -> flat blue stage in software.
    // Plain REPEAT then tiled the WHOLE atlas (sky+mountains) across the ground; the map
    // actually uses the REGION modes to select its atlas cell, so honor MIN/MAX for real.
    // PS2X_OLDSAMPLER=1: restore the working-era (bt3-software backup) sampler behavior for
    // A/B bisecting the grass-floor regression. The old sampler CLAMPED unconditionally
    // (its q handling was already fabsQ, same as now — the wrap is the ONLY delta).
    static const bool s_oldSampler = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_OLDSAMPLER"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
    const uint32_t wmsMode = s_oldSampler ? 1u : static_cast<uint32_t>(ctx.clamp & 3u);
    const uint32_t wmtMode = s_oldSampler ? 1u : static_cast<uint32_t>((ctx.clamp >> 2) & 3u);
    const int minU = static_cast<int>((ctx.clamp >> 4) & 0x3FFu);
    const int maxU = static_cast<int>((ctx.clamp >> 14) & 0x3FFu);
    const int minV = static_cast<int>((ctx.clamp >> 24) & 0x3FFu);
    const int maxV = static_cast<int>((ctx.clamp >> 34) & 0x3FFu);
    auto wrapCoord = [](int c, int size, uint32_t mode, int mn, int mx) -> int
    {
        switch (mode)
        {
        case 0u: // REPEAT (size is a power of two; & handles negatives correctly)
            return c & (size - 1);
        case 2u: // REGION_CLAMP: clamp into [MIN, MAX] (fall back to full range if MAX unset)
            return clampInt(c, mn, mx > mn ? mx : size - 1);
        case 3u: // REGION_REPEAT: MIN acts as mask, MAX as fixed offset
            return ((c & mn) | mx) & (size - 1);
        default: // CLAMP
            return clampInt(c, 0, size - 1);
        }
    };
    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapCoord(sampleU, texW, wmsMode, minU, maxU);
        sampleV = wrapCoord(sampleV, texH, wmtMode, minV, maxV);

        u32 out = gs->ReadVram(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(gs->m_texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            const uint32_t rgba = applyTexa(gs->m_texa, tex.psm, Rgba5551ToRgba8888(out));
            {   // [z16spy] PS2X_Z16SPY=1: the scene-alpha rebuild samples the Z page as PSMZ16
                // and its output alpha bit == sampled bit15 (TEXA 0/128). It writes ~410k px
                // yet the dest census reads 0% alpha -- so census the SAMPLED texels here.
                static const bool s_zs2 = [](){ const char *v = std::getenv("PS2X_Z16SPY");
                                                return v && v[0] && v[0] != '0'; }();
                if (s_zs2 && (tex.psm == GS_PSM_Z16 || tex.psm == GS_PSM_Z16S))
                {
                    static std::atomic<uint64_t> nS{0}, nBit{0}, nRaw0{0};
                    const uint64_t n = nS.fetch_add(1) + 1;
                    if (out & 0x8000u) nBit.fetch_add(1);
                    if ((out & 0xFFFFu) == 0u) nRaw0.fetch_add(1);
                    if ((n % 200000ull) == 1ull && n > 1)
                        std::fprintf(stderr, "[z16spy] sampled %llu Z16 texels: bit15 set %.2f%%  raw==0 %.2f%%"
                                             "  last raw=%04x uv=(%d,%d) tbp0=%u tbw=%u\n",
                                     (unsigned long long)n, 100.0 * (double)nBit.load() / (double)n,
                                     100.0 * (double)nRaw0.load() / (double)n,
                                     out & 0xFFFFu, sampleU, sampleV, tex.tbp0, tex.tbw);
                }
            }
            return rgba;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            // Fast path: the palette was decoded once per primitive into a flat
            // table by ensureClutCache(); a valid key means we can index directly
            // instead of re-fetching a swizzled CLUT entry from VRAM per pixel.
            uint32_t rgba = (gs->m_clutCacheKey != ~0ull)
                                ? gs->m_clutCache[static_cast<u8>(out)]
                                : lookupCLUT(gs, static_cast<u8>(out), tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            {   // [rampspy] PS2X_RAMPSPY=1: what does the routed 0x62 toon pass actually sample?
                // Interior slivers subtract ~21 where console subtracts <12 -- either the u
                // lands on the wrong ramp step or the CLUT decode is wrong (the ramp CLUTs are
                // render targets and ANIMATE). Histogram index + final subtract magnitude.
                static const bool s_rsp = [](){ const char *v = std::getenv("PS2X_RAMPSPY");
                                                return v && v[0] && v[0] != '0'; }();
                extern bool g_swoCelZSnap;
                {   // [shadeclut] same census for the 0x64 PSMT4 SHADING class: console's
                    // shadow side is ~60 darker than ours -- if this CLUT decodes all-bright,
                    // the toon shadow bands never render (GL path suffers the same symptom).
                    extern bool g_swDirtyActive;
                    if (s_rsp && g_swDirtyActive && !g_swoCelZSnap && tex.psm == GS_PSM_T4)
                    {
                        static std::set<uint64_t> dumped2;
                        static std::mutex dm2;
                        static std::atomic<uint64_t> n2{0}, hist2[16];
                        {
                            std::lock_guard<std::mutex> lk(dm2);
                            if (gs->m_clutCacheKey != ~0ull && dumped2.insert(gs->m_clutCacheKey).second && dumped2.size() <= 10)
                            {
                                std::fprintf(stderr, "[shadeclut] tbp=%u cbp=%u csa=%u maxRGB@idx0-15:",
                                             tex.tbp0, tex.cbp, tex.csa);
                                for (int i = 0; i < 16; ++i)
                                {
                                    const uint32_t c2 = gs->m_clutCache[i];
                                    std::fprintf(stderr, " %u", std::max({c2 & 0xFFu, (c2 >> 8) & 0xFFu, (c2 >> 16) & 0xFFu}));
                                }
                                std::fprintf(stderr, "\n");
                            }
                        }
                        hist2[out & 0xFu].fetch_add(1);
                        const uint64_t k2 = n2.fetch_add(1) + 1;
                        if ((k2 % 300000ull) == 1ull && k2 > 1)
                        {
                            std::fprintf(stderr, "[shadeidx] %llu samples idx0-15:", (unsigned long long)k2);
                            for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %llu", (unsigned long long)hist2[i].load());
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
                if (s_rsp && g_swoCelZSnap && tex.tbp0 == 15680u)
                {
                    {   // one-shot ramp dump per distinct CLUT cache: subtract magnitude at 16
                        // index steps, plus cbp/csa -- compared against console's empirical
                        // curve (subtract ~0 through u=0.74, dark only at the top step).
                        static std::set<uint64_t> dumped;
                        static std::mutex dm;
                        std::lock_guard<std::mutex> lk(dm);
                        if (gs->m_clutCacheKey != ~0ull && dumped.insert(gs->m_clutCacheKey).second && dumped.size() <= 12)
                        {
                            std::fprintf(stderr, "[rampclut] cbp=%u csa=%u cpsm=%u key=%llx maxRGB@idx:",
                                         tex.cbp, tex.csa, tex.cpsm, (unsigned long long)gs->m_clutCacheKey);
                            for (int i = 0; i < 256; i += 16)
                            {
                                const uint32_t c2 = gs->m_clutCache[i];
                                std::fprintf(stderr, " %u", std::max({c2 & 0xFFu, (c2 >> 8) & 0xFFu, (c2 >> 16) & 0xFFu}));
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                    static std::atomic<uint64_t> n{0};
                    static std::atomic<uint64_t> idxH[16], magH[16];
                    const unsigned ix = (unsigned)(out & 0xFFu) >> 4;
                    const unsigned mg = std::max({rgba & 0xFFu, (rgba >> 8) & 0xFFu, (rgba >> 16) & 0xFFu});
                    idxH[ix].fetch_add(1); magH[mg >> 4].fetch_add(1);
                    const uint64_t k = n.fetch_add(1) + 1;
                    if ((k % 100000ull) == 1ull && k > 1)
                    {
                        std::fprintf(stderr, "[rampspy] %llu samples idx16:", (unsigned long long)k);
                        for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %llu", (unsigned long long)idxH[i].load());
                        std::fprintf(stderr, "  | subtractMag16:");
                        for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %llu", (unsigned long long)magH[i].load());
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            static const bool s_tp = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_TEXEL_PROBE"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            if (s_tp && tex.psm == GS_PSM_T4)
            {
                // Per-tbp0 aggregate: how many sampled texels are non-zero (glyph)
                // and what alpha the CLUT gives them -> tells us if the runtime is
                // producing visible glyphs at the real atlas tbp0=0x2a08.
                static std::mutex s_m2;
                struct Agg { uint64_t samples=0, nzIdx=0, nzAlpha=0, alphaSum=0; };
                static std::map<uint32_t, Agg> s_agg;
                {
                    std::lock_guard<std::mutex> lk(s_m2);
                    Agg &a = s_agg[tex.tbp0];
                    a.samples++;
                    if (out & 0xf) a.nzIdx++;
                    uint32_t al = (rgba >> 24) & 0xff;
                    if (al) a.nzAlpha++;
                    a.alphaSum += al;
                    static std::atomic<uint64_t> s_c{0};
                    if ((s_c.fetch_add(1) % 40000u) == 1u)
                    {
                        std::cerr << "[t4-render]";
                        for (auto &kv : s_agg)
                            std::cerr << " tbp0=0x" << std::hex << kv.first << std::dec
                                      << "{n=" << kv.second.samples << " nzIdx=" << kv.second.nzIdx
                                      << " nzAlpha=" << kv.second.nzAlpha
                                      << " avgA=" << (kv.second.samples ? kv.second.alphaSum/kv.second.samples : 0) << "}";
                        std::cerr << std::endl;
                    }
                }
            }
            return rgba;
        }
        }

        return 0xFFFF00FFu;
    };

    if (!tex1UsesLinearFilter(ctx.tex1))
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void GSRasterizer::drawSprite(GS *gs)
{
    gprof::Scope gpScope(gprof::SW);   // [guestprof]
    ++gs->m_stateGen;   // [rectemplate] software draws change VRAM
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
    {
        // maybe a log here idk ?
        return;
    }

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    g_rasterPixelCount.fetch_add((uint64_t)std::max(0, drawX1 - drawX0 + 1) * (uint64_t)std::max(0, drawY1 - drawY0 + 1), std::memory_order_relaxed);

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
    const bool looksLikeDisplayCopy =
        gs->m_prim.tme &&
        gs->m_prim.abe &&
        gs->m_prim.fst &&
        gs->m_prim.ctxt &&
        ctx.frame.fbp != ctx.tex0.tbp0 &&
        alphaMode == 0x64u &&
        (alphaFix == 0x60u || alphaFix == 0x80u) &&
        unclippedX0 <= 0 &&
        unclippedY0 <= 0 &&
        unclippedX1 >= 639 &&
        unclippedY1 >= 447;
    if (looksLikeDisplayCopy)
    {
        gs->m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
        gs->m_hasPreferredDisplaySource = true;
    }

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (gs->m_prim.tme)
    {
        const auto &tex = ctx.tex0;
        int texW = 1 << tex.tw;
        int texH = 1 << tex.th;
        if (texW == 0)
            texW = 1;
        if (texH == 0)
            texH = 1;

        float u0f, v0f, u1f, v1f;
        if (gs->m_prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        // Per-primitive invariants hoisted out of the per-pixel loop: the texcoord
        // step per x is constant, so walk it incrementally instead of a divide +
        // lround every pixel (the hot path is small textured UI sprites).
        const bool fst = gs->m_prim.fst != 0;
        const float invSpriteW = 1.0f / spriteW;
        const float invSpriteH = 1.0f / spriteH;
        const float dUf = (u1f - u0f) * invSpriteW;            // texUf step per x
        const float invTexW = 1.0f / static_cast<float>(texW);
        const float invTexH = 1.0f / static_cast<float>(texH);
        const float uStart = u0f + (u1f - u0f) * ((static_cast<float>(drawX0 - unclippedX0) + 0.5f) * invSpriteW);

        parallelRows(drawY0, drawY1, [&](int y)
        {
            const float ty = (static_cast<float>(y - unclippedY0) + 0.5f) * invSpriteH;
            const float texVf = v0f + (v1f - v0f) * ty;
            // texVf is constant across the row -> resolve its sample coord once.
            const uint16_t sampleV = fst ? static_cast<uint16_t>(clampInt(static_cast<int>(texVf * 16.0f + 0.5f), 0, 0xFFFF)) : 0u;
            const float sTexV = texVf * invTexH;
            float texUf = uStart;

            for (int x = drawX0; x <= drawX1; ++x, texUf += dUf)
            {
                uint32_t texel;
                if (fst)
                {
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(static_cast<int>(texUf * 16.0f + 0.5f), 0, 0xFFFF));
                    texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = sampleTexture(gs, texUf * invTexW, sTexV, 1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                writePixel(gs, x, y, z1, color.r, color.g, color.b, color.a);
            }
        });
    }
    else
    {
        parallelRows(drawY0, drawY1, [&](int y)
        {
            for (int x = drawX0; x <= drawX1; ++x)
                writePixel(gs, x, y, z1, r, g, b, a);
        });
    }
}

void GSRasterizer::drawTriangle(GS *gs)
{
    gprof::Scope gpScope(gprof::SW);   // [guestprof]
    ++gs->m_stateGen;   // [rectemplate] software draws change VRAM
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const GSVertex &v2 = gs->m_vtxQueue[2];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    {
        static const bool s_3dt = [](){ static const char *s_env = std::getenv("PS2X_3DPROBE"); return s_env; }() != nullptr;
        if (s_3dt) {
            static int n3 = 0;
            if (n3++ < 50) {
                std::fprintf(stderr, "[3dt] tme=%d iip=%d tbp0=%u %ux%u | col=(%u,%u,%u,%u) z=%.0f | scr v0=(%.0f,%.0f) v1=(%.0f,%.0f) v2=(%.0f,%.0f)\n",
                    gs->m_prim.tme?1:0, gs->m_prim.iip?1:0, ctx.tex0.tbp0, ctx.tex0.tw, ctx.tex0.th,
                    v0.r, v0.g, v0.b, v0.a, (double)v0.z, fx0, fy0, fx1, fy1, fx2, fy2);
            }
        }
    }


    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    {   // Mode 9: only SLIVER triangles (the geometry GL's rasterizer actually drops --
        // console median coverage is ONE pixel and 39.9% cover no centre) contribute
        // difference pixels. Large triangles are fully handled by GL, and their edge
        // shells are covered by neighbouring triangles.
        extern bool g_swoDiffOnly;
        if (g_swoDiffOnly && std::fabs(denom) > 32.0f)  // |denom| = 2*area; skip area > 16px^2 (long thin outline strips pass; real interior triangles are 100s of px^2)
            return;
    }
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    // CONSERVATIVE RASTERIZATION for the routed cel/outline pass (PS2X_SWOCONS, default ON
    // while a routed draw is rasterizing). BT3 draws its outline as triangle strips of
    // sub-pixel SLIVERS -- console's own geometry has a MEDIAN of one covered pixel per
    // triangle and 39.9% cover no pixel centre at all. Whether such a triangle lights a pixel
    // is then decided by whether a centre happens to fall inside, and kEdgeEpsilon (1e-4 in
    // BARYCENTRIC units) is far too small to change that. The GS rasterizes at 1/16 pixel and
    // lights more of them, which is why our silhouette contour is only 65.0% continuous
    // against console's 82.6%.
    //
    // Widen each edge outward by half a pixel. w_i is the distance to the opposite edge
    // normalised by that edge's height h_i = |denom| / len_i, so half a pixel is
    //     d_i = 0.5 / h_i = 0.5 * len_i / |denom|
    // in barycentric units. Large triangles get a negligible d_i, so this is self-limiting:
    // it only affects geometry thin enough for the fill rule to be deciding coverage.
    float eps0 = kEdgeEpsilon, eps1 = kEdgeEpsilon, eps2 = kEdgeEpsilon;
    {
        extern bool g_swDirtyActive;
        // PS2X_SWOCONS = the widening in PIXELS (default 0.5 = exact half-pixel conservative
        // coverage); 0 disables. Larger values trade a thicker contour for more continuity.
        static const float s_cons = [](){ const char *v = std::getenv("PS2X_SWOCONS");
                                          return (v && v[0]) ? (float)std::atof(v) : 0.5f; }();
        // Widen ONLY the 0x62 sliver class -- widening the SHADING classes painted
        // edge-clamped texels along every interior mesh edge (the pants "wireframe";
        // attribution overlay work/wzw_attrib.png, blue population).
        extern bool g_swoCelZSnap;
        if (s_cons > 0.0f && g_swoCelZSnap)
        {
            auto elen = [](float ax, float ay, float bx, float by) {
                const float dx = ax - bx, dy = ay - by;
                return std::sqrt(dx * dx + dy * dy);
            };
            const float l12 = elen(fx1, fy1, fx2, fy2);   // edge opposite vertex 0
            const float l20 = elen(fx2, fy2, fx0, fy0);   // opposite vertex 1
            const float l01 = elen(fx0, fy0, fx1, fy1);   // opposite vertex 2
            eps0 = std::max(kEdgeEpsilon, s_cons * l12 * invAbsDenom);
            eps1 = std::max(kEdgeEpsilon, s_cons * l20 * invAbsDenom);
            eps2 = std::max(kEdgeEpsilon, s_cons * l01 * invAbsDenom);
        }
    }

    g_rasterPixelCount.fetch_add((uint64_t)std::max(0, maxX - minX + 1) * (uint64_t)std::max(0, maxY - minY + 1) / 2u, std::memory_order_relaxed);

    parallelRows(minY, maxY, [&](int y)
    {
        float py = static_cast<float>(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -eps0 || w1 < -eps1 || w2 < -eps2)
                continue;
            {   // Mode 9 difference coverage: only pixels GL's fill rule misses. The strict-
                // interior skip alone is NOT enough -- interior triangles' edge shells are
                // covered by their NEIGHBOURS on GL (shared edges), which a per-triangle test
                // cannot see, and admitting them wireframed the whole mesh. The area gate
                // below this loop restricts the pass to sliver triangles; here we addition-
                // ally skip strictly-interior pixels (GL covers those even on slivers).
                extern bool g_swoDiffOnly;
                if (g_swoDiffOnly && w0 > 1.0e-4f && w1 > 1.0e-4f && w2 > 1.0e-4f)
                    continue;
            }

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;
            {   // Interpolated attributes must stay inside the vertex range. The conservative
                // widening admits pixels whose barycentrics lie OUTSIDE [0,1] -- for sliver
                // triangles eps is huge in barycentric units, so z EXTRAPOLATES far past the
                // vertex z (measured: the routed cel/outline pixels rejected GEQUAL with
                // >4096-int margins while console tie-passes them). GS fixed-point
                // interpolation can never leave the vertex range; clamp to it. A no-op for
                // interior pixels (convex combination). PS2X_ZCLAMP=0 restores.
                static const bool s_zclamp = [](){ const char *v = std::getenv("PS2X_ZCLAMP");
                                                   return v && v[0] && v[0] != '0'; }();   // opt-in
                if (s_zclamp)
                {
                    const double zvMin = std::min({(double)v0.z, (double)v1.z, (double)v2.z});
                    const double zvMax = std::max({(double)v0.z, (double)v1.z, (double)v2.z});
                    if (z < zvMin) z = zvMin; else if (z > zvMax) z = zvMax;
                }
            }

            uint8_t r, g, b, a;
            if (gs->m_prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (gs->m_prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (gs->m_prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    const float invQ0 = 1.0f / fabsQ(v0.q);
                    const float invQ1 = 1.0f / fabsQ(v1.q);
                    const float invQ2 = 1.0f / fabsQ(v2.q);
                    const float sOverQ = (v0.s * invQ0) * w0 + (v1.s * invQ1) * w1 + (v2.s * invQ2) * w2;
                    const float tOverQ = (v0.t * invQ0) * w0 + (v1.t * invQ1) * w1 + (v2.t * invQ2) * w2;
                    const float invQ = invQ0 * w0 + invQ1 * w1 + invQ2 * w2;
                    iq = (std::fabs(invQ) > 1.0e-8f) ? (1.0f / invQ) : 1.0f;
                    is = sOverQ * iq;
                    it = tOverQ * iq;
                    {   // Same vertex-range rule as the z clamp: perspective-correct S/T at any
                        // TRUE interior pixel is a convex combination of the vertex S/T ("is" at
                        // vertex i is exactly v.s_i), so it can never leave [min,max]. Widened
                        // pixels EXTRAPOLATE -- on the 0x62 ramp slivers that lands on the dark
                        // top steps and painted the whole mesh edge network as a wireframe.
                        // PS2X_STCLAMP=0 restores.
                        static const bool s_stc = [](){ const char *v = std::getenv("PS2X_STCLAMP");
                                                        return v && v[0] && v[0] != '0'; }();   // opt-in
                        if (s_stc)
                        {
                            const float sMin = std::min({v0.s, v1.s, v2.s}), sMax = std::max({v0.s, v1.s, v2.s});
                            const float tMin = std::min({v0.t, v1.t, v2.t}), tMax = std::max({v0.t, v1.t, v2.t});
                            if (is < sMin) is = sMin; else if (is > sMax) is = sMax;
                            if (it < tMin) it = tMin; else if (it > tMax) it = tMax;
                        }
                    }
                    iu = 0;
                    iv = 0;
                }

                {   // [stqspy] PS2X_UMAP=1 also prints raw vertex STQ for the first few 0x62
                    // triangles: our per-pixel u is ~0.02 where console's is 0.24-0.88 (30x off)
                    // -- a scale or double-divide bug lives in these inputs.
                    static const bool s_um2 = [](){ const char *v = std::getenv("PS2X_UMAP");
                                                    return v && v[0] && v[0] != '0'; }();
                    extern bool g_swoCelZSnap;
                    if (s_um2 && g_swoCelZSnap && ctx.tex0.tbp0 == 15680u)
                    {
                        static int n = 0;
                        if (n < 12 && x == minX && y == minY)
                        {
                            ++n;
                            std::fprintf(stderr, "[stqspy] #%d v0(s=%.6f t=%.6f q=%.6f) v1(s=%.6f q=%.6f) v2(s=%.6f q=%.6f) fst=%d | pix is=%.6f iq=%.6f\n",
                                         n, v0.s, v0.t, v0.q, v1.s, v1.q, v2.s, v2.q,
                                         gs->m_prim.fst ? 1 : 0, is, iq);
                        }
                    }
                }
                uint32_t texel = sampleTexture(gs, is, it, iq, iu, iv);
                {   // [umap] PS2X_UMAP=1: per-pixel ramp u of the routed 0x62 pass, dumped for
                    // a direct diff against console's own u map (work/console_umap.npy).
                    static const bool s_um = [](){ const char *v = std::getenv("PS2X_UMAP");
                                                   return v && v[0] && v[0] != '0'; }();
                    extern bool g_swoCelZSnap;
                    if (s_um && g_swoCelZSnap && ctx.tex0.tbp0 == 15680u &&
                        x >= 0 && y >= 0 && x < 512 && y < 448)
                    {
                        static float *um = [](){
                            float *m = new float[512 * 448];
                            for (int i = 0; i < 512 * 448; ++i) m[i] = -1.0f;
                            std::atexit([](){
                                extern float *g_umapPtr;
                                if (FILE *f = std::fopen("/home/z3/Desktop/bt3/work/ours_umap.raw", "wb"))
                                { std::fwrite(g_umapPtr, 4, 512 * 448, f); std::fclose(f); }
                            });
                            extern float *g_umapPtr; g_umapPtr = m;
                            return m;
                        }();
                        um[(size_t)y * 512 + x] = is / std::max(1e-8f, std::fabs(iq));   // sampled u = S/Q
                    }
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            // A pixel admitted only by the conservative widening lies OUTSIDE the true
            // triangle: the GS never generated it, so it must not fight the z-buffer -- it
            // carries edge color for coverage, writes no z, and is not z-rejected (its
            // interpolated z is fabricated). Interior pixels keep the exact GS z-test.
            const bool widened = (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f);
            writePixel(gs, x, y, static_cast<u32>(z + 0.5), r, g, b, a, widened);
        }
    });
}

void GSRasterizer::drawLine(GS *gs)
{
    gprof::Scope gpScope(gprof::SW);   // [guestprof]
    ++gs->m_stateGen;   // [rectemplate] software draws change VRAM
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (gs->m_prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);

        writePixel(gs, x0, y0, static_cast<u32>(z), r, g, b, a);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}
