// [pgs] paraLLEl-GS backend -- see include/runtime/ps2_gs_pgs.h. First-light integration (2026-09-10):
//   * one Vulkan device (Granite, headless -- no surface; the GL window is untouched),
//   * every arbiter packet goes to GSInterface::gif_transfer on its own path index,
//   * the privileged registers are shadowed from the guest's stores and copied in at each swap,
//   * at the swap: flush + vsync, then a synchronous readback of the scanout to an RGBA8 buffer that the present
//     thread uploads as a texture. The readback is a full GPU sync per frame -- fine for first light, not for perf.
#include "runtime/ps2_gs_pgs.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_gs_gpu.h"        // [pgs-texreplace] GS (VRAM, palettes), register structs
#include "runtime/ps2_gs_gpu_renderer.h"   // [pgsink] the overlay's Cel Outline / ink strength (static getters)
#include "runtime/ps2_gs_rasterizer.h" // GSRasterizer::fillClutFrom
#include "runtime/ps2_texreplace.h"    // ps2tex::identify / loadReplacement
#include <unordered_map>
#include <string>
#include "gs_interface.hpp"
#include "device.hpp"
#include "context.hpp"
#include "thread_id.hpp"
#include <algorithm>
#include <atomic>
#include <new>
#include <vector>
#include <cmath>
#include <chrono>

// [pgswait] defined in Granite fence.cpp / device.cpp: wall time spent BLOCKED on the GPU.
// Declared at file scope (never inside an extern "C" body -- clang-cl rejects that).
void ps2xNotePmodeWrite(int src, unsigned long long v);   // [pmodesrc]
extern std::atomic<unsigned long long> g_pgsFenceWaitNs;
extern std::atomic<unsigned long long> g_pgsFenceWaitCalls;
extern std::atomic<unsigned long long> g_pgsFenceWaitBlocked;
extern std::atomic<unsigned long long> g_pgsIdleNs;
extern std::atomic<unsigned long long> g_pgsIdleCalls;
extern std::atomic<unsigned long long> g_pgsFrameWaitNs;
extern std::atomic<unsigned long long> g_pgsFrameWaitCalls;
extern std::atomic<unsigned long long> g_pgsFrameWaitBlocked;
extern std::atomic<unsigned long long> g_pgsSemWaitNs;
// [pgswait2] per-thread attribution (defined in Granite/vulkan/fence.cpp)
extern std::atomic<unsigned long long> g_pgsWaitNsBy[4][3];
extern std::atomic<unsigned long long> g_pgsWaitCallsBy[4][3];
extern "C" void ps2x_pgs_set_thread_tag(int tag);
extern "C" void ps2x_pgs_set_thread_tag_if_unset(int tag);
extern std::atomic<unsigned long long> g_pgsSemWaitCalls;
extern std::atomic<unsigned long long> g_pgsQueueIdleCalls;
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
const char *ps2xRtStalePageInfo(const GS *gs, uint32_t pg);   // [rtstale] debug text, defined in ps2_gs_gpu.cpp
bool ps2xGsRegionDrawnSinceWrite(const GS *gs, uint32_t bp, uint32_t bw, uint8_t psm, uint32_t w, uint32_t h, uint32_t *stalePg);   // [rtstale]

extern float g_ps2xWsHudInv;   // [wshud] per-frame HUD squeeze factor from the present (1.0 = off), ps2_runtime.cpp
extern std::atomic<int> g_wsHudLayout;   // overlay: 0 centered, 1 edge-pinned, 2 custom (-1 = unset)
extern std::atomic<int> g_wsHudOffLQ, g_wsHudOffCQ, g_wsHudOffRQ;   // custom offsets x16
namespace ps2x_pgs
{
static std::atomic<int> g_enabled{-1};
static std::atomic<uint32_t> g_presentW{0}, g_presentH{0};   // [pgsfit] on-screen size of the presented frame   // -1 = read the env on first use; 0/1 afterwards (init failure clears it)
static std::atomic<int> g_packOn{1};          // [pgslive] the overlay's Texture Replacement toggle (hook returns nothing when off)
static std::atomic<int> g_packFlushReq{0};    // [pgslive] toggle changed: drop the backend's cached textures at the next transfer
static std::atomic<int> g_wantScale{0};       // [pgslive] the overlay's Internal Resolution (1..4; 0 = not set, env/default apply)
static uint32_t scaleToSsaa(int scale) { return scale >= 4 ? 16u : scale == 3 ? 8u : scale == 2 ? 4u : 1u; }
static std::atomic<uint32_t> g_inkColor{0};   // [pgsink] outline colour 0xRRGGBB (0 = the game's black); the darkener subtracts, so its RGB is modulated by the complement
static std::atomic<int> g_inkWidthPct{100};   // [pgsink] outline stroke width, % of a PS2 texel (the edge-detect shift); 100 = native
std::atomic<int> g_pgsProbeReq{0};            // [vramprobe] set by the GS parse after the 32-sprite depth-mask pass (ps2x_pgs::)
std::atomic<unsigned> g_pgsProbeFbp{0}, g_pgsProbeZbp{0};
// [vramprobe] paraLLEl-GS VRAM byte address of a 32-bit pixel (PSMCT32 / PSMZ32 layout, the fork's swizzle_PS2)
static inline uint32_t pgsAddr32(uint32_t basePage, uint32_t pageStride, uint32_t x, uint32_t y, bool z)
{
    const uint32_t pageIndex = (y >> 5) * pageStride + (x >> 6);
    const uint32_t bx = (x & 63u) >> 3, by = (y & 31u) >> 3, col = (y & 7u) >> 1, px = x & 7u, py = y & 1u;
    uint32_t block = (bx & 1u) | ((by & 1u) << 1) | ((bx & 2u) << 1) | ((by & 2u) << 2) | ((bx & 4u) << 2);
    block += basePage * 32u;
    if (z) block ^= 0x18u;
    const uint32_t pix = (px & 1u) | ((py & 1u) << 1) | ((px & 2u) << 1) | ((px & 4u) << 1);
    return pageIndex * 8192u + block * 256u + col * 64u + pix * 4u;
}
namespace
{
using namespace Vulkan;
using namespace ParallelGS;

struct Signals final : SignalInterface
{   // exclusive mode: our GS parse is skipped, so FINISH/SIGNAL/LABEL must reach the guest's CSR from here
    GSRegisters *regs = nullptr;
    bool on_signal(uint64_t payload) override
    {
        if (!regs) return false;
        const uint32_t id = uint32_t(payload), mask = uint32_t(payload >> 32);
        uint32_t lo = uint32_t(regs->siglblid & 0xFFFFFFFFu);
        lo = (lo & ~mask) | (id & mask);
        regs->siglblid = (regs->siglblid & 0xFFFFFFFF00000000ull) | lo;
        regs->csr.fetch_or(0x1);
        return false;
    }
    bool on_finish(uint64_t) override { if (regs) regs->csr.fetch_or(0x2); return false; }
    bool on_label(uint64_t payload) override
    {
        if (!regs) return false;
        const uint32_t id = uint32_t(payload), mask = uint32_t(payload >> 32);
        uint32_t hi = uint32_t(regs->siglblid >> 32);
        hi = (hi & ~mask) | (id & mask);
        regs->siglblid = (uint64_t(hi) << 32) | (regs->siglblid & 0xFFFFFFFFu);
        return false;
    }
};

#if defined(PARALLEL_GS_TEXREPLACE)
// [pgs-texreplace] Texture pack on the paraLLEl-GS path. paraLLEl-GS asks at every texture-cache miss; we hash the
// texture exactly as the GL renderer does (PCSX2-compatible XXH3 over the swizzled VRAM blocks + the palette, from OUR
// GS parse running state-only in pack mode) and hand back a Vulkan image of the replacement, any size -- the
// ubershader samples with normalized coordinates, so a 4x image drops in without shader changes.
// [gatealpha] BC1/BC2/BC3 -> RGBA8 (raylib PixelFormat 14/15 = DXT1, 16 = DXT3, 17 = DXT5). Needed to rewrite the alpha of
// compressed replacements; only gate-style assets go through this (a few textures), everything else stays compressed.
static void bcDecodeColor(const uint8_t *b, bool bc1Mode, uint8_t (*out)[4])
{
    const uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    auto expand = [](uint32_t c, uint8_t *rgb) { rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31); rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63); rgb[2] = uint8_t((c & 31) * 255 / 31); };
    uint8_t pal[4][4] = {};
    expand(c0, pal[0]); expand(c1, pal[1]); pal[0][3] = pal[1][3] = 255;
    if (!bc1Mode || c0 > c1)
    {
        for (int k = 0; k < 3; k++) { pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3); }
        pal[2][3] = pal[3][3] = 255;
    }
    else
    {
        for (int k = 0; k < 3; k++) { pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    const uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) { const uint32_t k = (idx >> (2 * i)) & 3u; out[i][0] = pal[k][0]; out[i][1] = pal[k][1]; out[i][2] = pal[k][2]; out[i][3] = pal[k][3]; }
}
static bool bcDecode(int fmt, const std::vector<uint8_t> &src, int w, int h, std::vector<uint8_t> &rgba)
{
    const size_t blockBytes = (fmt == 14 || fmt == 15) ? 8u : 16u;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (src.size() < size_t(bw) * bh * blockBytes) return false;
    rgba.assign(size_t(w) * h * 4u, 0);
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
        {
            const uint8_t *b = src.data() + (size_t(by) * bw + bx) * blockBytes;
            uint8_t tex[16][4];
            uint8_t alpha[16];
            if (fmt == 14 || fmt == 15) { bcDecodeColor(b, true, tex); for (int i = 0; i < 16; i++) alpha[i] = tex[i][3]; }
            else if (fmt == 16)
            {   // BC2: 4-bit explicit alpha
                for (int i = 0; i < 16; i++) { const uint32_t nib = (b[i / 2] >> ((i & 1) * 4)) & 15u; alpha[i] = uint8_t(nib * 17u); }
                bcDecodeColor(b + 8, false, tex);
            }
            else
            {   // BC3: interpolated alpha
                const uint32_t a0 = b[0], a1 = b[1];
                uint8_t ramp[8]; ramp[0] = uint8_t(a0); ramp[1] = uint8_t(a1);
                if (a0 > a1) for (int k = 1; k < 7; k++) ramp[k + 1] = uint8_t(((7 - k) * a0 + k * a1) / 7);
                else { for (int k = 1; k < 5; k++) ramp[k + 1] = uint8_t(((5 - k) * a0 + k * a1) / 5); ramp[6] = 0; ramp[7] = 255; }
                uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = ramp[(bits >> (3 * i)) & 7u];
                bcDecodeColor(b + 8, false, tex);
            }
            for (int i = 0; i < 16; i++)
            {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint8_t *d = rgba.data() + (size_t(y) * w + x) * 4u;
                d[0] = tex[i][0]; d[1] = tex[i][1]; d[2] = tex[i][2]; d[3] = alpha[i];
            }
        }
    return true;
}
struct Replacer final : TextureReplacementInterface
{
    GS *gs = nullptr;
    std::unordered_map<std::string, ImageHandle> cache;   // name -> image (null = known miss)
    uint64_t hits = 0, misses = 0, skipped = 0, rtstale = 0, gateSkips = 0;
    ImageHandle replace(const TextureDescriptor &desc, uint64_t liveTex0, uint64_t liveTexclut, Device &device) override
    {
        if (!gs || !ps2tex::replacementsEnabled()) { skipped++; return {}; }
        if (!g_packOn.load(std::memory_order_relaxed)) { skipped++; return {}; }   // [pgslive] toggle off: native decode
        const uint32_t psm = uint32_t(desc.tex0.desc.PSM);
        if (psm != GS_PSM_T8 && psm != GS_PSM_T4) { skipped++; return {}; }   // identify() hashes paletted textures only
        GSTex0Reg t{};
        t.tbp0 = uint32_t(desc.tex0.desc.TBP0); t.tbw = uint8_t(desc.tex0.desc.TBW); t.psm = uint8_t(psm);
        t.tw = uint8_t(desc.tex0.desc.TW); t.th = uint8_t(desc.tex0.desc.TH); t.tcc = uint8_t(desc.tex0.desc.TCC); t.tfx = uint8_t(desc.tex0.desc.TFX);
        {   // [rtstale] pages the game drew into since their last upload/copy hold pixels our mirror never saw: the
            // hash would be of whatever texture lived there before (the portraits and energy bars that showed up on
            // the post-process quads). Refuse them; the backend samples its own VRAM.
            uint32_t stalePg = ~0u;
            if (ps2xGsRegionDrawnSinceWrite(gs, t.tbp0, t.tbw, uint8_t(psm), 1u << t.tw, 1u << t.th, &stalePg))
            {
                rtstale++;
                static const bool s_log = [](){ const char *v = std::getenv("PS2X_PGS_PACKLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_n = 0;
                if (s_log && s_n < 40) { s_n++; std::fprintf(stderr, "[pgs-rtstale] #%u refused tex tbp0=%u tbw=%u psm=%u tw=%u th=%u: %s\n", s_n, t.tbp0, t.tbw, psm, t.tw, t.th, ps2xRtStalePageInfo(gs, stalePg)); }
                return {};
            }
        }
        // palette address fields come from the LIVE TEX0 (the descriptor zeroes them: paraLLEl-GS keys palettes by bank)
        Reg64<TEX0Bits> lt; lt.bits = liveTex0;
        t.cbp = uint32_t(lt.desc.CBP); t.cpsm = uint8_t(lt.desc.CPSM); t.csm = uint8_t(lt.desc.CSM);
        t.csa = uint8_t(lt.desc.CSA); t.cld = uint8_t(lt.desc.CLD);
        if (ps2xGsRegionDrawnSinceWrite(gs, t.cbp, 1u, GS_PSM_CT32, 16u, 16u, nullptr))
        {   // [rtstale] a palette the game rendered (BT3 draws its outline CLUTs); a CSM1 CLUT is a 16x16 CT32 block group
            rtstale++;
            static const bool s_log = [](){ const char *v = std::getenv("PS2X_PGS_PACKLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_n = 0;
            if (s_log && s_n < 40) { s_n++; std::fprintf(stderr, "[pgs-rtstale] #%u refused CLUT cbp=%u for tex tbp0=%u: %s\n", s_n, t.cbp, t.tbp0, ps2xRtStalePageInfo(gs, t.cbp / 32u)); }
            return {};
        }
        Reg64<TEXCLUTBits> tc; tc.bits = liveTexclut;
        GSTexClutReg texclut{uint8_t(tc.desc.CBW), uint8_t(tc.desc.COU), uint16_t(tc.desc.COV)};
        GSTexaReg texa{uint8_t(desc.texa.desc.TA0), desc.texa.desc.AEM != 0, uint8_t(desc.texa.desc.TA1)};
        uint32_t clut[256] = {};
        const int n = GSRasterizer::fillClutForBackend(clut, gs->vramData(), texa, texclut, t);
        ps2tex::TexIdent id;
        if (n <= 0 || !ps2tex::identify(gs->vramData(), t.tbp0, t.tbw, t.psm, t.tw, t.th, clut, texa.ta0, texa.aem, texa.ta1, id)) { skipped++; return {}; }
        const std::string name = id.name();
        {   // PS2X_PGS_PACKLOG=1: the first identifications, to compare against the pack's file names
            static const bool s_log = [](){ const char *v = std::getenv("PS2X_PGS_PACKLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_n = 0;
            static const unsigned s_max = [](){ const char *v = std::getenv("PS2X_PGS_PACKLOGN"); return v && v[0] ? unsigned(std::atoi(v)) : 40u; }();
            if (s_log && s_n < s_max && !cache.count(name)) { s_n++; std::fprintf(stderr, "[pgs-pack] #%u %s psm=%u tbp0=%u tbw=%u tw=%u th=%u cbp=%u csa=%u cpsm=%u bank=%u\n", s_n, name.c_str(), psm, t.tbp0, t.tbw, t.tw, t.th, t.cbp, t.csa, t.cpsm, unsigned(desc.palette_bank)); }
        }
        auto it = cache.find(name);
        if (it != cache.end()) { if (it->second) hits++; else misses++; return it->second; }
        // [gatealpha] the OpenGL path's rule (ps2_gs_rasterizer.cpp [texreplace]): a texture whose native alpha is
        // BINARY (every palette entry clear or solid) is a destination-alpha gate asset -- BT3's health-bar strips
        // and plaques are written as a DATE gate first and colour-filled through it. A filtered, upscaled
        // replacement puts partial alpha and a differently drawn edge into that gate and the fills no longer
        // meet it (the yellow sliver at the opponent bar's cap). Uncompressed replacements get their alpha
        // snapped; a compressed one (the whole pack is DXT5) keeps the game's own texture, as in OpenGL.
        bool gateAlpha = false; uint32_t aSolid = 0, aClear = 255;
        {
            bool clear = false, solid = false, mid = false; uint32_t amax = 0;
            for (int i = 0; i < n; i++) amax = std::max(amax, clut[i] >> 24);
            const uint32_t loT = amax > 0x80u ? 4u : 2u, hiT = amax > 0x80u ? 251u : 0x7eu;   // expanded (0..255) or PS2 (0..0x80) range
            for (int i = 0; i < n && !mid; i++)
            {
                const uint32_t a = clut[i] >> 24;
                if (a <= loT) { clear = true; aClear = std::min<uint32_t>(aClear, a); }
                else if (a >= hiT) { solid = true; aSolid = std::max<uint32_t>(aSolid, a); }
                else mid = true;
            }
            gateAlpha = !mid && clear && solid;
            if (amax > 0x80u) { aSolid = aSolid * 128u / 255u; aClear = aClear * 128u / 255u; }   // back to the PS2 range the image is stored in
        }
        std::vector<uint8_t> px; int w = 0, h = 0, fmt = 0;
        if (!ps2tex::loadReplacement(id, px, w, h, fmt) || w <= 0 || h <= 0 || px.empty()) { cache.emplace(name, ImageHandle{}); misses++; return {}; }
        // The gate BUILD itself (the alpha-only write) never reaches this hook: the fork keys those texture uses
        // separately and decodes the game's own texture for them, so the gate keeps the native alpha shape while
        // the colour fills drawn through it use the pack. Uncompressed gate-style replacements still get a binary
        // alpha so their own edge is hard; compressed ones are used as they are.
        if (gateAlpha)
        {   // pack colours, NATIVE alpha semantics: the pack stores "opaque" as 126..131 (DXT5 noise around 128) and the
            // game's later destination-alpha tests key on bit 7 -- BT3's ki gauge (DATM=1) leaked into the health bar
            // wherever a fill texel landed at >= 128. Rewrite every texel's alpha to the palette's own clear/solid value.
            if (fmt != 7)
            {
                std::vector<uint8_t> dec;
                if (!bcDecode(fmt, px, w, h, dec)) { cache.emplace(name, ImageHandle{}); misses++; return {}; }
                px.swap(dec); fmt = 7;
            }
            for (size_t i = 3; i < px.size(); i += 4) px[i] = px[i] >= 64u ? uint8_t(aSolid) : uint8_t(aClear);
            gateSkips++;   // stat: gate-style assets rewritten
            static unsigned s_gl = 0;
            if (s_gl < 6) { s_gl++; std::fprintf(stderr, "[pgs-pack] gate-alpha %s: alpha rewritten to native %u/%u (%dx%d)\n", name.c_str(), aSolid, aClear, w, h); }
        }
        // raylib PixelFormat values (raylib.h): 7 = R8G8B8A8, 14 = DXT1 RGB, 15 = DXT1 RGBA, 16 = DXT3, 17 = DXT5
        VkFormat vkfmt = VK_FORMAT_UNDEFINED;
        switch (fmt)
        {
        case 7: vkfmt = VK_FORMAT_R8G8B8A8_UNORM; break;
        case 14: case 15: vkfmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
        case 16: vkfmt = VK_FORMAT_BC2_UNORM_BLOCK; break;
        case 17: vkfmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
        default: break;
        }
        if (vkfmt == VK_FORMAT_UNDEFINED) { cache.emplace(name, ImageHandle{}); misses++; return {}; }
        auto info = ImageCreateInfo::immutable_2d_image(uint32_t(w), uint32_t(h), vkfmt);
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;   // paraLLEl-GS's post-upload barrier moves GENERAL -> READ_ONLY
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ImageInitialData init = {}; init.data = px.data();
        ImageHandle img = device.create_image(info, &init);
        if (!img) { cache.emplace(name, ImageHandle{}); misses++; return {}; }
        cache.emplace(name, img); hits++;
        return img;
    }
};

#else
struct Replacer { GS *gs = nullptr; std::unordered_map<std::string, int> cache; uint64_t hits = 0, misses = 0, skipped = 0, rtstale = 0, gateSkips = 0; };   // upstream paraLLEl-GS without the hook: no pack path
#endif

struct State
{
    std::mutex mtx;
    bool inited = false, failed = false;
    Context ctx;
    Device device;
    GSInterface iface;
    Signals signals;
    Replacer replacer;   // [pgs-texreplace]
    Hacks hacks;
    // privileged register shadow, by hardware offset (0x0000.. and 0x1000..), 64-bit each
    uint64_t privLo[0x100] = {};
    uint64_t privHi[0x100] = {};
    // newest scanout
    std::vector<uint8_t> frame;
    uint32_t frameW = 0, frameH = 0;
    // [pgswshud] widescreen HUD squeeze on the backend path: the OpenGL renderer squeezes HUD draws per primitive
    // (ps2_gs_gpu_renderer.cpp [wshud]); paraLLEl-GS draws what it is given, so the same rule is applied by
    // rewriting the X of HUD vertices inside the GIF packets before they reach the backend.
    struct WsHud
    {
        struct Ctx { uint32_t fbp = 0, fbw = 0, fpsm = 0, zte = 0, ztst = 0; float ofx = 0.f, ofy = 0.f; uint64_t test = 0, frame = 0, alpha = 0, tex0 = 0, scissor = 0; size_t tex0Off = ~size_t(0), alphaOff = ~size_t(0); };
        Ctx ctx[2];
        uint64_t primRaw = 0, prmode = 0; bool prmodeCont = true;
        struct V { size_t off = 0; bool packed = false, mapped = false; uint16_t x = 0, y = 0; uint32_t z = 0; };
        V q[3]; int qn = 0;
        bool frameHad3d = false, active = false; int no3dRun = 0; uint64_t lastSwap = ~0ull;
        uint64_t mappedVerts = 0, hudPrims = 0, scissorsMapped = 0, splitPrims = 0; float lastInv = 1.0f;
        // [pgsink] RGBAQ writes since the last kick (packet offsets; packed = 16-byte qword, else 8-byte reg), for the darkener
        struct RgbaW { size_t off; bool packed; }; RgbaW rgba[8]; int rgbaN = 0;
        RgbaW uv[8]; int uvN = 0;   // [pgsink] UV writes since the last kick (the edge-detect shift rewrite)
        uint64_t lastRgbaq = 0x3f80000080808080ull;   // last RGBAQ value seen (A+D / REGLIST form), for register restores
        uint64_t inkDropped = 0, inkScaled = 0, inkShifted = 0, shadowDropped = 0, dofDropped = 0, maskNeutralized = 0, bloomEdits = 0, bloomRestores = 0;
    } wshud;
    std::vector<uint8_t> wsBuf;   // [pgswshud] rebuilt packet when quads are subdivided at layout breakpoints
    uint32_t ssaa = 1;                     // super-sampling factor the backend was created with (1/2/4/8/16)
    uint32_t baseW = 0, baseH = 0;         // [pgsfit] last scanout size in the 1x domain (image extent >> shift)
    uint32_t lastShift = 0;                // [pgsfit] scanout shift used for the last swap (0 = 1x, 1 = 2x, 2 = 4x)
    bool frameFresh = false;
    uint32_t field = 0;
    uint64_t swaps = 0, packets = 0, bytes = 0, noImage = 0;
    double xferMs = 0.0;   // CPU time inside gif_transfer (the packet parse on our GsThread)
    uint64_t fsPrims = 0, fsPasses = 0, fsCopies = 0, fsPal = 0;   // paraLLEl-GS flush stats per stats window
    struct SlowCall { double ms; uint32_t size, path, nloop, flg, nreg; uint64_t regs; uint32_t firstAD; };   // [pgs-slow] the 3 slowest gif_transfer calls per window
    SlowCall slowCalls[3] = {};
    uint32_t slowOver1ms = 0;
    uint64_t streamDispfb1 = 0; bool haveStreamFlip = false;   // the game's DISPFB1 flip, carried in stream order ([displatch] job)
    uint64_t streamFlips = 0, flipMismatch = 0, lastFb1 = 0, lastLive1 = 0, lastLive2 = 0;   // [pgsflip] diagnostics
    uint32_t privHist[0x20] = {};   // privileged stores per 16-byte slot since the last stats line (bus + pseudo regs)
    uint64_t pseudoSeen = 0;        // in-stream pseudo A+D registers applied (exclusive mode)
    bool timestamps = false;
    std::chrono::steady_clock::time_point tStat = std::chrono::steady_clock::now();
    double readbackMs = 0.0, vsyncMs = 0.0;
};
State &st() { static State *s = new State; return *s; }   // leaked on purpose: never destroy the device behind a running thread

bool envOn(const char *name) { const char *v = std::getenv(name); return v && v[0] && v[0] != '0'; }

void registerThread()
{   // Granite keys per-thread command pools by a registered index; unregistered threads log an error per call.
    // Every caller here is serialised by the state mutex, so they can all share index 0 (the replayer's main thread).
    static thread_local bool t_reg = false;
    if (!t_reg) { Util::register_thread_index(0); t_reg = true; }
}

bool initLocked(State &s)
{
    registerThread();
    if (s.inited) return true;
    if (s.failed) return false;
    s.failed = true;   // until proven otherwise
    auto fail = [](const char *why) { std::fprintf(stderr, "[pgs] %s -- backend unavailable, falling back to the OpenGL renderer\n", why); g_enabled.store(0, std::memory_order_relaxed); return false; };
    if (!Context::init_loader(nullptr)) return fail("Vulkan loader init failed");
    s.ctx.set_num_thread_indices(1);
    if (!s.ctx.init_instance_and_device(nullptr, 0, nullptr, 0,
                                        CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                        CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                        CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
        return fail("Vulkan instance/device init failed");
    s.device.set_context(s.ctx);
    s.device.init_frame_contexts(4);
    GSOptions opts = {};
    opts.vram_size = 4 * 1024 * 1024;
    {
        const char *v = std::getenv("PS2X_PGS_SSAA");
        const int ws = g_wantScale.load(std::memory_order_relaxed);
        const int r = ws > 0 ? int(scaleToSsaa(ws)) : (v && v[0] ? std::atoi(v) : 1);   // [pgslive] overlay scale > env > 1
        opts.super_sampling = r >= 16 ? SuperSampling::X16 : r >= 8 ? SuperSampling::X8 : r >= 4 ? SuperSampling::X4 : r >= 2 ? SuperSampling::X2 : SuperSampling::X1;
        opts.super_sampled_textures = [](){ const char *v = std::getenv("PS2X_PGS_SSTEX"); return !(v && v[0] == '0'); }();   // [pgsink] per-sample reads of render targets (the outline chain's silhouette buffer; needed for ink widths < 100%)
        s.ssaa = r >= 16 ? 16u : r >= 8 ? 8u : r >= 4 ? 4u : r >= 2 ? 2u : 1u;
    }
    if (!s.iface.init(&s.device, opts)) return fail("GSInterface init failed");
    s.iface.set_signal_interface(&s.signals);
    { Hacks hk; hk.force_bilinear = envOn("PS2X_PGS_FORCE_BILINEAR");
      hk.replaced_per_sample = [](){ const char *v = std::getenv("PS2X_PGS_REPLPERSAMPLE"); return !(v && v[0] == '0'); }();   // A/B: 0 = snapped evaluation for replaced sprites
      hk.skip_kick_mask = [](){ const char *v = std::getenv("PS2X_PGS_SKIPKICK"); return v && v[0] ? uint32_t(std::atoi(v)) : 0u; }();   // bisect: 1 = 16-bit frame kicks, 2 = 24/32-bit reads of block 10752, 4 = frame fbp 336
      s.hacks = hk; s.iface.set_hacks(hk); }
#if defined(PARALLEL_GS_TEXREPLACE)
    if (packMode()) s.iface.set_texture_replacement_interface(&s.replacer);   // [pgs-texreplace]
#else
    if (packMode()) std::fprintf(stderr, "[pgs] texture pack requested but this paraLLEl-GS checkout has no replacement hook (upstream); packs stay off\n");
#endif
    s.timestamps = envOn("PS2X_PGS_TIMESTAMPS");
    if (s.timestamps) { DebugMode dm = {}; dm.timestamps = true; s.iface.set_debug_mode(dm); }
    s.failed = false; s.inited = true;
    std::fprintf(stderr, "[pgs] paraLLEl-GS backend up: %s, ssaa=%u, force_bilinear=%d, replaced_per_sample=%d, skipkick=%u, %s\n",
                 s.device.get_gpu_properties().deviceName, unsigned(opts.super_sampling), s.hacks.force_bilinear ? 1 : 0, s.hacks.replaced_per_sample ? 1 : 0, unsigned(s.hacks.skip_kick_mask),
                 exclusive() ? "EXCLUSIVE (our GS parse skipped)" : packMode() ? "PACK mode (our GS parse state-only, replacements via the backend)" : "dual (our GL renderer keeps running)");
    return true;
}

void copyPrivLocked(State &s)
{
    auto &p = s.iface.get_priv_register_state();
    auto put = [](void *dst, uint64_t v) { std::memcpy(dst, &v, sizeof(v)); };
    if (const GSRegisters *r = s.signals.regs)
    {   // live block: bus stores, the sceGs stubs and our GS parse all land here
        put(&p.pmode, r->pmode);     put(&p.smode1, r->smode1 ? r->smode1 : 0x0000000740814504ULL);   put(&p.smode2, r->smode2);   // NTSC default when the CRTC was never programmed
        put(&p.srfsh, r->srfsh);     put(&p.synch1, r->synch1);   put(&p.synch2, r->synch2);
        static const bool s_useStream = !envOn("PS2X_PGS_LIVEFLIP");   // PS2X_PGS_LIVEFLIP=1: scan out whatever the bus says right now
        const uint64_t fb1 = (s_useStream && s.haveStreamFlip) ? s.streamDispfb1 : r->dispfb1;
        const uint64_t fb2 = (s_useStream && s.haveStreamFlip && r->dispfb2 == r->dispfb1) ? s.streamDispfb1 : r->dispfb2;
        if (fb1 != r->dispfb1) s.flipMismatch++;
        s.lastFb1 = fb1; s.lastLive1 = r->dispfb1; s.lastLive2 = r->dispfb2;
        put(&p.syncv, r->syncv);     put(&p.dispfb1, fb1);        put(&p.display1, r->display1);
        put(&p.dispfb2, fb2);        put(&p.display2, r->display2); put(&p.extbuf, r->extbuf);
        put(&p.extdata, r->extdata); put(&p.extwrite, r->extwrite); put(&p.bgcolor, r->bgcolor);
        put(&p.csr, r->csr.load(std::memory_order_relaxed)); put(&p.imr, r->imr); put(&p.busdir, r->busdir);
        put(&p.siglblid, r->siglblid);
        s.privLo[0] = r->pmode; s.privLo[2] = r->smode2; s.privLo[7] = r->dispfb1; s.privLo[8] = r->display1;
        s.privLo[9] = r->dispfb2; s.privLo[10] = r->display2;   // for the stats line
        return;
    }
    put(&p.pmode, s.privLo[0x00 >> 4]);   put(&p.smode1, s.privLo[0x10 >> 4]);  put(&p.smode2, s.privLo[0x20 >> 4]);
    put(&p.srfsh, s.privLo[0x30 >> 4]);   put(&p.synch1, s.privLo[0x40 >> 4]);  put(&p.synch2, s.privLo[0x50 >> 4]);
    put(&p.syncv, s.privLo[0x60 >> 4]);   put(&p.dispfb1, s.privLo[0x70 >> 4]); put(&p.display1, s.privLo[0x80 >> 4]);
    put(&p.dispfb2, s.privLo[0x90 >> 4]); put(&p.display2, s.privLo[0xA0 >> 4]); put(&p.extbuf, s.privLo[0xB0 >> 4]);
    put(&p.extdata, s.privLo[0xC0 >> 4]); put(&p.extwrite, s.privLo[0xD0 >> 4]); put(&p.bgcolor, s.privLo[0xE0 >> 4]);
    put(&p.csr, s.privHi[0x00 >> 4]);     put(&p.imr, s.privHi[0x10 >> 4]);     put(&p.busdir, s.privHi[0x40 >> 4]);
    put(&p.siglblid, s.privHi[0x80 >> 4]);
}

// [pgs-asyncrb] Asynchronous scanout readback: a three-deep ring of host buffers + fences. Each swap submits a copy of
// this frame's scanout and consumes the copy submitted two swaps ago IF its fence is already signalled (never waits).
// The first version waited for the GPU every frame (wait_idle); with the GPU idle at every frame start paraLLEl-GS
// took its CPU upload path for the frame's IMAGE transfers, a flat ~6 ms per frame ([pgs-slow] 2026-09-10).
struct RbSlot { BufferHandle buf; Fence fence; ImageHandle image; uint32_t w = 0, h = 0; bool pending = false; VkFormat fmt = VK_FORMAT_UNDEFINED; };
static RbSlot g_rb[3];
static uint32_t g_rbIdx = 0;
static void consumeSlotLocked(State &s, RbSlot &slot)
{
    const auto *src = static_cast<const uint32_t *>(s.device.map_host_buffer(*slot.buf, MEMORY_ACCESS_READ_BIT));
    const size_t n = size_t(slot.w) * slot.h;
    s.frame.resize(n * 4u);
    const bool bgra = slot.fmt == VK_FORMAT_B8G8R8A8_UNORM || slot.fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (!bgra)
    {
        std::memcpy(s.frame.data(), src, n * 4u);
        uint32_t *px = reinterpret_cast<uint32_t *>(s.frame.data());
        for (size_t i = 0; i < n; i++) px[i] |= 0xFF000000u;
    }
    else
        for (size_t i = 0; i < n; i++)
        {
            const uint32_t p = src[i];
            s.frame[i * 4 + 0] = (p >> 16) & 0xff; s.frame[i * 4 + 1] = (p >> 8) & 0xff; s.frame[i * 4 + 2] = p & 0xff; s.frame[i * 4 + 3] = 0xff;
        }
    s.device.unmap_host_buffer(*slot.buf, MEMORY_ACCESS_READ_BIT);
    s.frameW = slot.w; s.frameH = slot.h; s.frameFresh = true;
    slot.pending = false; slot.image.reset(); slot.fence.reset();
}
void readbackLocked(State &s, const ScanoutResult &res)
{
    static const bool s_sync = envOn("PS2X_PGS_SYNCREADBACK");   // the old behaviour, for A/B
    // 1. consume the oldest pending slot without waiting (or with a wait in sync mode)
    for (int k = 1; k <= 2; k++)
    {
        RbSlot &old = g_rb[(g_rbIdx + k) % 3];
        if (old.pending && old.fence && (s_sync ? (old.fence->wait(), true) : old.fence->wait_timeout(0))) consumeSlotLocked(s, old);
    }
    // 2. submit this frame's copy into the current slot (if it is still in flight, drop this frame)
    RbSlot &slot = g_rb[g_rbIdx];
    if (slot.pending) { if (slot.fence && slot.fence->wait_timeout(0)) consumeSlotLocked(s, slot); else return; }
    const uint32_t w = res.image->get_width(), h = res.image->get_height();
    if (!slot.buf || slot.w != w || slot.h != h)
    {
        BufferCreateInfo bi = {};
        bi.size = VkDeviceSize(w) * h * 4u;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.domain = BufferDomain::CachedHost;
        slot.buf = s.device.create_buffer(bi);
        slot.w = w; slot.h = h;
    }
    slot.fmt = res.image->get_format();
    auto cmd = s.device.request_command_buffer();
    cmd->image_barrier(*res.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cmd->copy_image_to_buffer(*slot.buf, *res.image, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    Fence fence;
    s.device.submit(cmd, &fence);
    slot.fence = std::move(fence); slot.image = res.image; slot.pending = true;
    g_rbIdx = (g_rbIdx + 1u) % 3u;
    if (s_sync) { slot.fence->wait(); consumeSlotLocked(s, slot); }
}

// [pgs-pseudo] Our sceGs stubs deliver display-environment writes IN-STREAM as A+D writes to pseudo registers
// (Kernel/Stubs/GS.cpp: 0x41 PMODE, 0x42 SMODE2, 0x59 DISPFB1, 0x5a DISPLAY1, 0x5b DISPFB2, 0x5c DISPLAY2, 0x5f BGCOLOR).
// Our own GS parse applies them (ps2_gs_gpu.cpp); in exclusive mode that parse is skipped, so walk the packet's tags here.
// paraLLEl-GS itself treats those addresses as NOPs. Only PACKED tags carrying an A+D descriptor are walked.
// [pgswshud] the OpenGL path's layout map (ps2_gs_gpu_renderer.cpp wsMapX), W = frame width in game px
static float wsHudMapX(float x, float W, float inv)
{
    const float half = 0.5f * W, k = W / 512.0f;
    auto cen = [&](float v) { return half + (v - half) * inv; };
    int layout = g_wsHudLayout.load(std::memory_order_relaxed); if (layout < 0) layout = 0;
    if (layout <= 0) return cen(x);
    const float offL = g_wsHudOffLQ.load(std::memory_order_relaxed) / 16.0f, offC = g_wsHudOffCQ.load(std::memory_order_relaxed) / 16.0f, offR = g_wsHudOffRQ.load(std::memory_order_relaxed) / 16.0f;
    const float s1 = 124.f * k, s2 = 216.f * k, s3 = 296.f * k, s4 = 388.f * k;
    float t0 = 0.f, t1 = s1 * inv, t2 = cen(s2), t3 = cen(s3), t4 = W - (W - s4) * inv, t5 = W;
    if (layout >= 2)
    {
        t0 += offL * k; t1 += offL * k; t2 += offC * k; t3 += offC * k; t4 += offR * k; t5 += offR * k;
        if (t1 > t2 - 2.f) t1 = t2 - 2.f;
        if (t3 > t4 - 2.f) t4 = t3 + 2.f;
    }
    if (x <= s1) return t0 + (x - 0.f) * (t1 - t0) / s1;
    if (x <= s2) return t1 + (x - s1) * (t2 - t1) / (s2 - s1);
    if (x <= s3) return t2 + (x - s2) * inv;
    if (x <= s4) return t3 + (x - s3) * (t4 - t3) / (s4 - s3);
    return t4 + (x - s4) * inv;
}
// One primitive is complete: decide with the OpenGL renderer's rule and map its vertices' X in place.
static void wsHudKickLocked(State &s, uint8_t *data, float inv)
{
    State::WsHud &h = s.wshud;
    const uint32_t primType = uint32_t(h.primRaw & 7u);
    const uint64_t attr = h.prmodeCont ? h.primRaw : h.prmode;
    const bool fst = ((attr >> 8) & 1u) != 0;
    const uint32_t ci = uint32_t((attr >> 9) & 1u);
    const State::WsHud::Ctx &c = h.ctx[ci];
    const bool isTri = (primType >= 3u && primType <= 5u), isSprite = (primType == 6u);
    if (isTri && c.zte && c.ztst >= 2u) h.frameHad3d = true;
    {   // [pgsink] outline EDGE-DETECT shift (PS2X_PGS_INKSHIFT=<texels>, experiment): BT3's chain draws page 336's CT16 view
        // with fbmsk ffff00ff and a Cd - Cs blend (0x62), the texture read one texel to the right of the destination;
        // that one texel is the stroke width. Rewrite U := X + shift for those sprites (needs PS2X_PGS_SSTEX=1 so the
        // silhouette is read per sample).
        static const float s_inkShiftEnv = [](){ const char *v = std::getenv("PS2X_PGS_INKSHIFT"); return v && v[0] ? float(std::atof(v)) : 0.0f; }();
        const float s_inkShift = s_inkShiftEnv > 0.0f ? s_inkShiftEnv : float(g_inkWidthPct.load(std::memory_order_relaxed)) / 100.0f;
        const bool tmeK = ((attr >> 4) & 1u) != 0, fstK = ((attr >> 8) & 1u) != 0;
        if (s_inkShift > 0.0f && s_inkShift < 1.0f && isSprite && tmeK && fstK && c.fbp == 336u && c.fpsm == 2u && uint32_t(c.frame >> 32) == 0xffff00ffu && (c.alpha & 0xFFu) == 0x62u && h.qn >= 2 && h.uvN >= 2)
        {
            for (int i = 0; i < 2 && i < h.uvN; i++)
            {
                uint8_t *q = data + h.uv[i].off;
                uint64_t lo; std::memcpy(&lo, q, 8);
                const uint32_t u = uint32_t(lo & 0x3FFFu);
                const float x16 = float(h.q[i].x);   // raw 12.4 X (offset included); U is in texels 12.4 relative to the texture
                (void)x16;
                // the game's U = X_frame + 1 texel: keep everything but replace the +1 texel by +shift
                const int32_t nu = int32_t(u) - 16 + int32_t(s_inkShift * 16.0f + 0.5f);
                const uint64_t nlo = (lo & ~0x3FFFull) | uint64_t(uint32_t(nu < 0 ? 0 : nu) & 0x3FFFu);
                std::memcpy(q, &nlo, 8);
            }
            h.inkShifted++;
        }
    }
    h.uvN = 0;
    {   // [pgsfx] DoF off, mode 3 (BLOOM): rebuild the blur buffer from bright pixels only, so the composite has nothing
        // blurred to drag along with an aura or a dash trail. (a) the buffer's clear sprites paint the threshold grey,
        // (b) the downsample sprites subtract the destination ((Cs - Cd) * FIX/128, FIX = 0x80 -> scene - grey, clamped),
        // (c) the composite becomes additive ((Cs - 0) * As + Cd), (d) the depth-mask sprites write "far" everywhere
        // (TCC := 0, vertex alpha 0) so the composite passes everywhere. PS2X_PGS_BLOOMTHR = the grey (default 0x90).
        static const int s_dofMode4 = [](){ const char *e = std::getenv("PS2X_PGS_DOFMODE"); return e && e[0] ? std::atoi(e) : 2; }();
        static const uint8_t s_thr = [](){ const char *e = std::getenv("PS2X_PGS_BLOOMTHR"); const int v = e && e[0] ? int(std::strtol(e, nullptr, 0)) : 0x90; return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }();
        if (s_dofMode4 == 3 && !GsGpuRenderer::dofBlurEnabled() && isSprite && h.qn >= 2)
        {
            const bool tmeB = ((attr >> 4) & 1u) != 0, abeB = ((attr >> 6) & 1u) != 0;
            const uint32_t tpsmB = uint32_t((c.tex0 >> 20) & 0x3Fu), ttbp0 = uint32_t(c.tex0 & 0x3FFFu), ttw = uint32_t((c.tex0 >> 26) & 0xFu), tth = uint32_t((c.tex0 >> 30) & 0xFu), ttbw = uint32_t((c.tex0 >> 14) & 0x3Fu);
            const bool blurBuf = c.fbp == 336u && c.fbw == 4u && c.fpsm == 0u;
            // (a) clear of the 256x256 buffer: untextured, no blend, into fbp 336 fbw 4
            if (blurBuf && !tmeB && !abeB)
            {
                for (int i = 0; i < h.rgbaN; i++) { uint8_t *q = data + h.rgba[i].off; if (h.rgba[i].packed) { q[0] = s_thr; q[4] = s_thr; q[8] = s_thr; } else { q[0] = s_thr; q[1] = s_thr; q[2] = s_thr; } }
                h.bloomEdits++;
            }
            // (b) downsample of the scene: textured from a scene buffer as CT32 512x512, blended, into the blur buffer
            else if (blurBuf && tmeB && abeB && tpsmB == 0u && ttw == 9u && tth == 9u && (ttbp0 == 0u || ttbp0 == 3584u))
            {
                if (c.alphaOff != ~size_t(0) && c.alpha != 0x80000000A4ull) { const uint64_t a = 0x80000000A4ull; std::memcpy(data + c.alphaOff, &a, 8); h.ctx[ci].alpha = a; }
                h.bloomEdits++;
            }
            // (c) the composite: CT24 256x256 read of block 10752 with a destination-alpha test into a scene buffer
            else if ((c.fbp == 0u || c.fbp == 112u) && (c.fpsm == 0u || c.fpsm == 1u) && tmeB && abeB && tpsmB == 1u && ttbp0 == 10752u && ttbw == 4u && ttw == 8u && tth == 8u && ((c.test >> 14) & 1u))
            {
                if (c.alphaOff != ~size_t(0) && (c.alpha & 0xFFu) != 0x48u) { const uint64_t a = (c.alpha & ~0xFFull) | 0x48ull; std::memcpy(data + c.alphaOff, &a, 8); h.ctx[ci].alpha = a; }
                h.bloomEdits++;
            }
            // (d) the depth-mask sprites: write "far" everywhere
            else if (tmeB && (c.fpsm == 2u || c.fpsm == 10u) && (c.fbp == 0u || c.fbp == 112u) && uint32_t(c.frame >> 32) == 0x3fffu && (tpsmB == 50u || tpsmB == 58u))
            {
                if (c.tex0Off != ~size_t(0) && (c.tex0 & (1ull << 34))) { uint64_t t0 = c.tex0 & ~(1ull << 34); std::memcpy(data + c.tex0Off, &t0, 8); h.ctx[ci].tex0 = t0; }
                for (int i = 0; i < h.rgbaN; i++) { uint8_t *q = data + h.rgba[i].off; if (h.rgba[i].packed) q[12] = 0x00; else q[3] = 0x00; }
                h.maskNeutralized++;
            }
        }
    }
    {   // [pgsfx] DoF off, mode 2: the depth-mask sprites (16-bit view of a scene buffer, FBMSK 0x3fff, reading the Z
        // buffer as PSMZ16) get their texture alpha turned off (TEX0.TCC := 0) and a vertex alpha of 0x80, so they write
        // "near" (alpha bit 7 = 1) everywhere: no far blur, while draws after them (the aura) still open the mask.
        static const int s_dofMode3 = [](){ const char *e = std::getenv("PS2X_PGS_DOFMODE"); return e && e[0] ? std::atoi(e) : 2; }();   // 2 = mask pass writes near (default), 1 = mask pass off, 0 = drop the composite (kills the glow)
        const bool tmeM = ((attr >> 4) & 1u) != 0;
        const uint32_t tpsmM = uint32_t((c.tex0 >> 20) & 0x3Fu);
        if (s_dofMode3 == 2 && !GsGpuRenderer::dofBlurEnabled() && isSprite && tmeM && (c.fpsm == 2u || c.fpsm == 10u) && (c.fbp == 0u || c.fbp == 112u)
            && uint32_t(c.frame >> 32) == 0x3fffu && (tpsmM == 50u || tpsmM == 58u))
        {
            if (c.tex0Off != ~size_t(0) && (c.tex0 & (1ull << 34)))
            {   // clear TCC once per TEX0 write
                uint64_t t0 = c.tex0 & ~(1ull << 34);
                std::memcpy(data + c.tex0Off, &t0, 8);
                h.ctx[ci].tex0 = t0;
            }
            for (int i = 0; i < h.rgbaN; i++) { uint8_t *q = data + h.rgba[i].off; if (h.rgba[i].packed) q[12] = 0x80; else q[3] = 0x80; }
            h.maskNeutralized++;
        }
    }
    {   // [pgsfx] Character Shadows / Depth-of-Field Blur toggles (the OpenGL renderer's draw classes, ps2_gs_gpu_renderer.cpp
        // PS2X_NODECAL=1 and PS2X_NODOF): the shadow decal tiles are triangles sampling block 10752 as PSMCT24 256x256 into
        // the scene; the DoF composite samples block 10752 as PSMCT32 into the scene. Off = collapse the primitive.
        const bool tmeF = ((attr >> 4) & 1u) != 0;
        const bool sceneF = (c.fbp == 0u || c.fbp == 112u) && (c.fpsm == 0u || c.fpsm == 1u);
        if (tmeF && sceneF && (isTri || isSprite) && uint32_t(c.tex0 & 0x3FFFu) == 10752u)
        {
            const uint32_t tpsm = uint32_t((c.tex0 >> 20) & 0x3Fu), tw = uint32_t((c.tex0 >> 26) & 0xFu), th = uint32_t((c.tex0 >> 30) & 0xFu);
            // CT24 256x256 reads of the blur buffer: with a destination-alpha test they are the blur/glow COMPOSITES
            // (full-screen, lerp where the depth mask allows); without it, the shadow decal tiles on the ground.
            const bool dateF = ((c.test >> 14) & 1u) != 0;
            const bool shadowTile = isTri && tpsm == 1u && tw == 8u && th == 8u && !dateF;
            const bool blurComp = tpsm == 1u && tw == 8u && th == 8u && dateF;
            // the DoF composite lerps by destination alpha (0x54 / 0x68); the Kaioken glow reads the same page additively
            const uint32_t bm = uint32_t(c.alpha & 0xFFu);
            const bool abeF = ((attr >> 6) & 1u) != 0;
            const uint32_t tbw = uint32_t((c.tex0 >> 14) & 0x3Fu);
            {   // PS2X_PGS_WSHUDLOG=1: the classes of scene draws that read block 10752 (DoF copy vs glow blur vs shadow tiles)
                static const bool s_fl = [](){ const char *v = std::getenv("PS2X_PGS_WSHUDLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_fn = 0;
                static uint64_t s_seen[32]; static int s_seenN = 0;
                const uint64_t key = (uint64_t(tpsm) << 40) | (uint64_t(tbw) << 32) | (uint64_t(tw) << 28) | (uint64_t(th) << 24) | (uint64_t(bm) << 8) | uint64_t(primType) | (uint64_t(abeF) << 4) | (uint64_t((c.test >> 14) & 1u) << 5);
                bool seen = false; for (int i = 0; i < s_seenN; i++) if (s_seen[i] == key) seen = true;
                if (s_fl && !seen && s_seenN < 32 && s_fn < 32) { s_seen[s_seenN++] = key; s_fn++; std::fprintf(stderr, "[pgsfx] read of 10752: psm %u tbw %u %ux%u blend %02x abe %d prim %u fbp %u test %llx w %.0f\n", tpsm, tbw, 1u << tw, 1u << th, bm, abeF ? 1 : 0, primType, c.fbp, (unsigned long long)c.test, (h.qn >= 2 ? std::fabs(float(h.q[1].x) - float(h.q[0].x)) / 16.0f : 0.f)); }
            }
            // the DoF composite reads the 512-wide (tbw 8) half-height scene copy and lerps by destination alpha; the
            // Kaioken glow reads the 256x256 blur (tbw 4)
            static const int s_dofMode2 = [](){ const char *e = std::getenv("PS2X_PGS_DOFMODE"); return e && e[0] ? std::atoi(e) : 2; }();   // 2 = mask pass writes near (default), 1 = mask pass off, 0 = drop the composite (kills the glow)
            const bool dofComp = s_dofMode2 == 0 && (blurComp || (tpsm == 0u && abeF && (bm == 0x54u || bm == 0x68u)));
            if ((shadowTile && !GsGpuRenderer::shadowsEnabled()) || (dofComp && !GsGpuRenderer::dofBlurEnabled()))
            {
                const int n = isSprite ? 2 : 3;
                if (h.qn >= n) { const uint16_t x0 = h.q[0].x; for (int i = 0; i < n; i++) { std::memcpy(data + h.q[i].off, &x0, 2); h.q[i].x = x0; h.q[i].mapped = true; } if (shadowTile) h.shadowDropped++; else h.dofDropped++; }
                h.rgbaN = 0; h.uvN = 0;
                return;
            }
        }
    }
    {   // [pgsink] the cel-outline DARKENER (the OpenGL path's gate): untextured, blended, ALPHA 0x52 = Cd - Cs * Ad into a
        // scene buffer. Cel Outline OFF collapses its vertices (zero area, nothing drawn); ink strength scales its
        // vertex colour by pct/199 (199% is the hardware coefficient, which the backend already applies).
        const bool tme = ((attr >> 4) & 1u) != 0, abe = ((attr >> 6) & 1u) != 0;
        if (!tme && abe && (c.alpha & 0xFFu) == 0x52u && (c.fbp == 0u || c.fbp == 112u) && (c.fpsm == 0u || c.fpsm == 1u) && (isTri || isSprite))
        {
            const int n = isSprite ? 2 : 3;
            if (!GsGpuRenderer::outlineEnabled())
            {
                if (h.qn >= n) { const uint16_t x0 = h.q[0].x; for (int i = 0; i < n; i++) { std::memcpy(data + h.q[i].off, &x0, 2); h.q[i].x = x0; h.q[i].mapped = true; } h.inkDropped++; }
            }
            else
            {
                const int pct = GsGpuRenderer::inkStrengthPct();
                const uint32_t col = g_inkColor.load(std::memory_order_relaxed);
                if (pct != 199 || col != 0u)
                {
                    const float k = float(pct) / 199.0f;
                    // colour: the draw SUBTRACTS Cs, so keep of the game's Cs only the complement of the wanted colour
                    const float kc[3] = { k * float(255u - ((col >> 16) & 0xFFu)) / 255.0f, k * float(255u - ((col >> 8) & 0xFFu)) / 255.0f, k * float(255u - (col & 0xFFu)) / 255.0f };
                    for (int i = 0; i < h.rgbaN; i++)
                    {
                        uint8_t *q = data + h.rgba[i].off;
                        if (h.rgba[i].packed) { for (int ch = 0; ch < 3; ch++) { const float v = float(q[ch * 4]) * kc[ch]; q[ch * 4] = uint8_t(v > 255.f ? 255.f : v + 0.5f); } }
                        else { for (int ch = 0; ch < 3; ch++) { const float v = float(q[ch]) * kc[ch]; q[ch] = uint8_t(v > 255.f ? 255.f : v + 0.5f); } }
                    }
                    if (h.rgbaN) h.inkScaled++;
                }
            }
            h.rgbaN = 0;
            return;
        }
        h.rgbaN = 0;
    }
    if (!h.active || inv >= 0.999f) return;
    if (!(isTri || isSprite)) return;
    if (!(c.fbp == 0u || c.fbp == 112u) || !(c.fpsm == 0u || c.fpsm == 1u)) return;
    const float W = (c.fbw * 64u >= 320u && c.fbw * 64u <= 1024u) ? float(c.fbw * 64u) : 512.0f;
    const int n = isSprite ? 2 : 3;
    if (h.qn < n) return;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f; bool zAllZero = true;
    for (int i = 0; i < n; i++)
    {
        const float x = h.q[i].x / 16.0f - c.ofx, y = h.q[i].y / 16.0f - c.ofy;
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        if (h.q[i].z != 0u) zAllZero = false;
    }
    const float w = x1 - x0, hh = y1 - y0;
    bool hud = false;
    if (isSprite) hud = (w > 0.f && w < 0.8f * W && hh > 0.f && hh < 300.f && y1 < 96.f && (!c.zte || c.ztst == 1u));
    // triangles: flat (z exactly 0), UV-mapped, in the top band. Width: narrower than 0.8 W, OR wide but not
    // full-width -- BT3's bar chain draws its gate/backing as ONE quad spanning both bars (x 59..530); left
    // unsqueezed it exposes the layer beneath at the squeezed bar's end. Full-frame fades never fit the band.
    else hud = (fst && zAllZero && hh < 300.f && y1 < 96.f && (w < 0.8f * W || x0 > 8.f));   // the gate quads run 59..572, past the frame edge
    {   // PS2X_PGS_WSHUDLOG=1: every top-band primitive on the scene buffers with its attributes and the decision
        static const bool s_log = [](){ const char *v = std::getenv("PS2X_PGS_WSHUDLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_n = 0;
        // the sliver region: the left end of the opponent's bar (frame x 250..330, y 10..70), pre-map coordinates
        if (s_log && s_n < 400 && y1 > 10.f && y0 < 70.f && x1 > 250.f && x0 < 330.f && (c.fbp == 0u || c.fbp == 112u))
        {
            s_n++;
            std::fprintf(stderr, "[wshudlog] #%u prim %u fst %d tme %d abe %d ctx %u fbp %u fbmsk %08x test %llx alpha %llx tex0 %llx scissor %llx z %u/%u/%u box (%.1f,%.1f)-(%.1f,%.1f) -> %s\n",
                         s_n, primType, fst ? 1 : 0, int((attr >> 4) & 1u), int((attr >> 6) & 1u), ci, c.fbp, uint32_t(c.frame >> 32), (unsigned long long)c.test, (unsigned long long)c.alpha,
                         (unsigned long long)c.tex0, (unsigned long long)c.scissor, h.q[0].z, h.q[1].z, n > 2 ? h.q[2].z : 0u, x0, y0, x1, y1, hud ? "MAP" : "keep");
        }
    }
    if (!hud) return;
    h.hudPrims++;
    static const bool s_vlog = [](){ const char *v = std::getenv("PS2X_PGS_WSHUDLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_vn = 0;
    const bool vlog = s_vlog && s_vn < 300 && y1 > 15.f && y0 < 60.f && x1 > 270.f && x0 < 330.f;
    if (vlog) { s_vn++; std::fprintf(stderr, "[wshudv] #%u prim %u tme %d abe %d fbp %u fbmsk %08x test %llx tex0 tbp0 %u psm %u %ux%u ofx %.2f verts:", s_vn, primType, int((attr >> 4) & 1u), int((attr >> 6) & 1u), c.fbp, uint32_t(c.frame >> 32), (unsigned long long)c.test, uint32_t(c.tex0 & 0x3FFFu), uint32_t((c.tex0 >> 20) & 0x3Fu), 1u << ((c.tex0 >> 26) & 0xFu), 1u << ((c.tex0 >> 30) & 0xFu), c.ofx); }
    for (int i = 0; i < n; i++)
    {
        State::WsHud::V &v = h.q[i];
        if (v.mapped) { if (vlog) std::fprintf(stderr, " [x %.2f already]", v.x / 16.0f - c.ofx); continue; }
        const float fx = v.x / 16.0f - c.ofx;
        float mx = (wsHudMapX(fx, W, inv) + c.ofx) * 16.0f;
        if (mx < 0.f) mx = 0.f; if (mx > 65535.f) mx = 65535.f;
        const uint16_t nx = uint16_t(mx + 0.5f);
        std::memcpy(data + v.off, &nx, 2);   // packed and reglist both keep X in the low 16 bits
        if (vlog) std::fprintf(stderr, " [x %.2f y %.2f -> %.2f]", fx, v.y / 16.0f - c.ofy, nx / 16.0f - c.ofx);
        v.x = nx; v.mapped = true; h.mappedVerts++;
    }
    if (vlog) std::fprintf(stderr, "\n");
}
static void wsHudVertexLocked(State &s, uint8_t *data, size_t off, bool packed, bool xyzf, bool kick, float inv)
{
    State::WsHud &h = s.wshud;
    uint64_t lo, hi = 0; std::memcpy(&lo, data + off, 8); if (packed) std::memcpy(&hi, data + off + 8, 8);
    State::WsHud::V v; v.off = off; v.packed = packed;
    if (packed) { v.x = uint16_t(lo & 0xFFFFu); v.y = uint16_t((lo >> 32) & 0xFFFFu); v.z = xyzf ? uint32_t((hi >> 4) & 0xFFFFFFu) : uint32_t(hi & 0xFFFFFFFFu); }
    else { v.x = uint16_t(lo & 0xFFFFu); v.y = uint16_t((lo >> 16) & 0xFFFFu); v.z = xyzf ? uint32_t((lo >> 32) & 0xFFFFFFu) : uint32_t(lo >> 32); }
    const uint32_t primType = uint32_t(h.primRaw & 7u);
    const int need = (primType == 6u) ? 2 : (primType >= 3u) ? 3 : (primType == 1u || primType == 2u) ? 2 : 1;
    if (h.qn >= 3) { h.q[0] = h.q[1]; h.q[1] = h.q[2]; h.qn = 2; }   // overflow: keep the newest two (strip semantics)
    h.q[h.qn++] = v;
    if (!kick || h.qn < need) return;
    wsHudKickLocked(s, data, inv);
    // queue maintenance per primitive type
    switch (primType)
    {
    case 4: h.q[0] = h.q[1]; h.q[1] = h.q[2]; h.qn = 2; break;          // triangle strip: keep the last two
    case 5: h.q[1] = h.q[2]; h.qn = 2; break;                          // triangle fan: keep first + last
    case 2: h.q[0] = h.q[1]; h.qn = 1; break;                          // line strip
    default: h.qn = 0; break;                                           // lists, sprites, points
    }
}
// A+D register write; `data + off` is the 64-bit value in the packet (rewritable). SCISSOR writes that look like the
// HUD's tight bar scissors (top band, narrower than 0.8 W) follow the layout map, as the OpenGL path's [wsscissor]:
// the squeezed bar otherwise extends past the unmapped scissor and its outer strip is clipped, exposing the layer
// underneath (the yellow sliver at the left end of the opponent's bar).
static void wsHudRegLocked(State &s, uint32_t reg, uint64_t v, uint8_t *data, size_t off, float inv, bool rewrite = true)
{
    State::WsHud &h = s.wshud;
    switch (reg)
    {
    case 0x00: h.primRaw = v; h.qn = 0; break;
    case 0x01: h.lastRgbaq = v; if (rewrite && data && h.rgbaN < 8) { h.rgba[h.rgbaN].off = off; h.rgba[h.rgbaN].packed = false; h.rgbaN++; } break;   // A+D RGBAQ: 64-bit value in the low half (R,G,B,A bytes 0..3)
    case 0x40: case 0x41:
    {
        h.ctx[reg - 0x40].scissor = v;
        if (!rewrite) break;
        {
            static const bool s_log2 = [](){ const char *v2 = std::getenv("PS2X_PGS_WSHUDLOG"); return v2 && v2[0] && v2[0] != '0'; }(); static unsigned s_n2 = 0;
            if (s_log2 && h.active && s_n2 < 150) { s_n2++; std::fprintf(stderr, "[wshudlog] SCISSORALL_%u x %u..%u y %u..%u fbp %u\n", reg - 0x40 + 1, uint32_t(v & 0x7FFu), uint32_t((v >> 16) & 0x7FFu), uint32_t((v >> 32) & 0x7FFu), uint32_t((v >> 48) & 0x7FFu), h.ctx[reg - 0x40].fbp); }
        }
        if (!h.active || inv >= 0.999f) break;
        const State::WsHud::Ctx &c = h.ctx[reg - 0x40];
        // scene buffers only: the character-palette render (FRAME 480, 64x64 at the origin) also sets a tight scissor
        if (!(c.fbp == 0u || c.fbp == 112u) || !(c.fpsm == 0u || c.fpsm == 1u)) break;
        const float W = (c.fbw * 64u >= 320u && c.fbw * 64u <= 1024u) ? float(c.fbw * 64u) : 512.0f;
        const uint32_t x0 = uint32_t(v & 0x7FFu), x1 = uint32_t((v >> 16) & 0x7FFu), y1 = uint32_t((v >> 48) & 0x7FFu);
        if (x0 < 8u || x1 < x0 + 16u) break;   // origin-anchored / tiny rects are render-to-texture work, not bar scissors
        {
            static const bool s_log = [](){ const char *v = std::getenv("PS2X_PGS_WSHUDLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_n = 0;
            if (s_log && s_n < 120 && y1 < 96u) { s_n++; std::fprintf(stderr, "[wshudlog] SCISSOR_%u x %u..%u y %u..%u -> %s\n", reg - 0x40 + 1, x0, x1, uint32_t((v >> 32) & 0x7FFu), y1, (x1 < x0 || float(x1 - x0 + 1u) >= 0.8f * W) ? "keep" : "MAP"); }
        }
        if (x1 < x0 || y1 >= 96u || float(x1 - x0 + 1u) >= 0.8f * W) break;
        float mx0 = std::floor(wsHudMapX(float(x0), W, inv)), mx1 = std::ceil(wsHudMapX(float(x1 + 1u), W, inv)) - 1.0f;
        if (mx0 < 0.f) mx0 = 0.f; if (mx1 > 2047.f) mx1 = 2047.f; if (mx1 < mx0) mx1 = mx0;
        const uint64_t nv = (v & ~(0x7FFull | (0x7FFull << 16))) | uint64_t(uint32_t(mx0)) | (uint64_t(uint32_t(mx1)) << 16);
        std::memcpy(data + off, &nv, 8);
        h.scissorsMapped++;
        break;
    }
    case 0x18: case 0x19: h.ctx[reg - 0x18].ofx = float(v & 0xFFFFu) / 16.0f; h.ctx[reg - 0x18].ofy = float((v >> 32) & 0xFFFFu) / 16.0f; break;
    case 0x1A: h.prmodeCont = (v & 1u) != 0; break;
    case 0x1B: h.prmode = (h.prmode & 7u) | (v & ~7ull); break;
    case 0x06: case 0x07: h.ctx[reg - 0x06].tex0 = v; h.ctx[reg - 0x06].tex0Off = (rewrite && data) ? off : ~size_t(0); break;
    case 0x42: case 0x43: h.ctx[reg - 0x42].alpha = v; h.ctx[reg - 0x42].alphaOff = (rewrite && data) ? off : ~size_t(0); break;
    case 0x47: case 0x48: h.ctx[reg - 0x47].test = v; h.ctx[reg - 0x47].zte = uint32_t((v >> 16) & 1u); h.ctx[reg - 0x47].ztst = uint32_t((v >> 17) & 3u); break;
    case 0x4C: case 0x4D:
    {   // [pgsfx] DoF off, mode 1: the depth-mask pass (FRAME = 16-bit view of a scene buffer, FBMSK 0x3fff) is made to
        // write nothing, so the composite sees the scene's own alpha instead of the Z-derived far mask
        static const int s_dofMode = [](){ const char *e = std::getenv("PS2X_PGS_DOFMODE"); return e && e[0] ? std::atoi(e) : 2; }();   // 2 = mask pass writes near (default), 1 = mask pass off, 0 = drop the composite (kills the glow)
        const uint32_t fpsm = uint32_t((v >> 24) & 0x3Fu), fbp = uint32_t(v & 0x1FFu), fbmsk = uint32_t(v >> 32);
        if (s_dofMode == 1 && rewrite && data && !GsGpuRenderer::dofBlurEnabled() && (fpsm == 2u || fpsm == 10u) && (fbp == 0u || fbp == 112u) && fbmsk == 0x3fffu)
        {
            v = (v & 0xFFFFFFFFull) | (0xFFFFFFFFull << 32);
            std::memcpy(data + off, &v, 8);
            h.maskNeutralized++;
        }
    }
    h.ctx[reg - 0x4C].frame = v; h.ctx[reg - 0x4C].fbp = uint32_t(v & 0x1FFu); h.ctx[reg - 0x4C].fbw = uint32_t((v >> 16) & 0x3Fu); h.ctx[reg - 0x4C].fpsm = uint32_t((v >> 24) & 0x3Fu); break;
    default: break;
    }
}
// [pgssplit] Piecewise layouts (edge-pinned / custom) map each vertex through a map with breakpoints; a quad that
// spans a breakpoint gets a straight interpolation between its mapped corners and drifts off the exactly-mapped
// elements drawn on it (the frame backing runs 257..513). This pre-pass rebuilds the packet with such HUD quads
// (four-vertex strips in the game's Z order, and sprites) subdivided at the breakpoints, attributes interpolated,
// so the mapper afterwards moves every piece exactly. PACKED tags only; anything unusual is copied verbatim.
struct WsVert
{
    uint8_t loop[16 * 16]; uint32_t nreg = 0;
    int xyzIdx = -1, uvIdx = -1, stIdx = -1, rgbaIdx = -1; bool xyzf = false;
    float x = 0, y = 0;   // frame space
};
static void wsVertLerp(const WsVert &a, const WsVert &b, float t, WsVert &o, float xFrame, float ofx)
{   // o = copy of a with X = xFrame and the interpolable attributes at parameter t between a and b
    o = a;
    auto qw = [&](WsVert &v, int idx) { return v.loop + size_t(idx) * 16u; };
    auto lerp = [&](float p, float q) { return p + (q - p) * t; };
    {
        uint8_t *q = qw(o, o.xyzIdx); const uint8_t *qa = a.loop + size_t(a.xyzIdx) * 16u, *qb = b.loop + size_t(b.xyzIdx) * 16u;
        float mx = (xFrame + ofx) * 16.0f; if (mx < 0.f) mx = 0.f; if (mx > 65535.f) mx = 65535.f;
        const uint16_t nx = uint16_t(mx + 0.5f); std::memcpy(q, &nx, 2);
        uint64_t ha, hb; std::memcpy(&ha, qa + 8, 8); std::memcpy(&hb, qb + 8, 8);
        uint64_t ho; std::memcpy(&ho, q + 8, 8);
        if (o.xyzf) { const uint32_t za = uint32_t((ha >> 4) & 0xFFFFFFu), zb = uint32_t((hb >> 4) & 0xFFFFFFu); const uint32_t z = uint32_t(lerp(float(za), float(zb)) + 0.5f) & 0xFFFFFFu; ho = (ho & ~(0xFFFFFFull << 4)) | (uint64_t(z) << 4); }
        else { const uint32_t za = uint32_t(ha & 0xFFFFFFFFu), zb = uint32_t(hb & 0xFFFFFFFFu); const double z = double(za) + (double(zb) - double(za)) * t; const uint32_t zi = uint32_t(z + 0.5); ho = (ho & ~0xFFFFFFFFull) | zi; }
        std::memcpy(q + 8, &ho, 8);
    }
    if (o.uvIdx >= 0)
    {
        uint8_t *q = qw(o, o.uvIdx); const uint8_t *qa = a.loop + size_t(a.uvIdx) * 16u, *qb = b.loop + size_t(b.uvIdx) * 16u;
        uint64_t la, lb; std::memcpy(&la, qa, 8); std::memcpy(&lb, qb, 8);
        const float ua = float(la & 0x3FFFu), ub = float(lb & 0x3FFFu), va = float((la >> 32) & 0x3FFFu), vb = float((lb >> 32) & 0x3FFFu);
        const uint64_t u = uint64_t(uint32_t(lerp(ua, ub) + 0.5f) & 0x3FFFu), v = uint64_t(uint32_t(lerp(va, vb) + 0.5f) & 0x3FFFu);
        uint64_t lo; std::memcpy(&lo, q, 8); lo = (lo & ~(0x3FFFull | (0x3FFFull << 32))) | u | (v << 32); std::memcpy(q, &lo, 8);
    }
    if (o.stIdx >= 0)
    {
        uint8_t *q = qw(o, o.stIdx); const uint8_t *qa = a.loop + size_t(a.stIdx) * 16u, *qb = b.loop + size_t(b.stIdx) * 16u;
        float fa[3], fb[3], fo[3]; std::memcpy(fa, qa, 12); std::memcpy(fb, qb, 12);
        for (int k = 0; k < 3; k++) fo[k] = lerp(fa[k], fb[k]);
        std::memcpy(q, fo, 12);
    }
    if (o.rgbaIdx >= 0)
    {
        uint8_t *q = qw(o, o.rgbaIdx); const uint8_t *qa = a.loop + size_t(a.rgbaIdx) * 16u, *qb = b.loop + size_t(b.rgbaIdx) * 16u;
        for (int k = 0; k < 4; k++) q[k * 4] = uint8_t(lerp(float(qa[k * 4]), float(qb[k * 4])) + 0.5f);
    }
}
// REGLIST variant: one loop = nreg 64-bit registers (two per qword)
struct WsVertRL
{
    uint64_t r[16]; uint32_t nreg = 0;
    int xyzIdx = -1, uvIdx = -1, stIdx = -1, rgbaIdx = -1; bool xyzf = false;
    float x = 0, y = 0;
};
static void wsVertLerpRL(const WsVertRL &a, const WsVertRL &b, float t, WsVertRL &o, float xFrame, float ofx)
{
    o = a;
    auto lerp = [&](float p, float q) { return p + (q - p) * t; };
    {
        uint64_t &v = o.r[o.xyzIdx]; const uint64_t va = a.r[a.xyzIdx], vb = b.r[b.xyzIdx];
        float mx = (xFrame + ofx) * 16.0f; if (mx < 0.f) mx = 0.f; if (mx > 65535.f) mx = 65535.f;
        const uint64_t nx = uint64_t(uint16_t(mx + 0.5f));
        if (o.xyzf) { const uint32_t za = uint32_t((va >> 32) & 0xFFFFFFu), zb = uint32_t((vb >> 32) & 0xFFFFFFu); const uint64_t z = uint64_t(uint32_t(lerp(float(za), float(zb)) + 0.5f) & 0xFFFFFFu); v = (v & ~(0xFFFFull | (0xFFFFFFull << 32))) | nx | (z << 32); }
        else { const double za = double(uint32_t(va >> 32)), zb = double(uint32_t(vb >> 32)); const uint64_t z = uint64_t(uint32_t(za + (zb - za) * t + 0.5)); v = (v & 0xFFFF0000ull) | nx | (z << 32); }
    }
    if (o.uvIdx >= 0)
    {
        const uint64_t ua = a.r[a.uvIdx], ub = b.r[b.uvIdx];
        const float u0 = float(ua & 0x3FFFu), u1 = float(ub & 0x3FFFu), v0 = float((ua >> 16) & 0x3FFFu), v1 = float((ub >> 16) & 0x3FFFu);
        o.r[o.uvIdx] = (ua & ~(0x3FFFull | (0x3FFFull << 16))) | uint64_t(uint32_t(lerp(u0, u1) + 0.5f) & 0x3FFFu) | (uint64_t(uint32_t(lerp(v0, v1) + 0.5f) & 0x3FFFu) << 16);
    }
    if (o.stIdx >= 0)
    {
        float fa[2], fb[2], fo[2]; std::memcpy(fa, &a.r[a.stIdx], 8); std::memcpy(fb, &b.r[b.stIdx], 8);
        fo[0] = lerp(fa[0], fb[0]); fo[1] = lerp(fa[1], fb[1]); std::memcpy(&o.r[o.stIdx], fo, 8);
    }
    if (o.rgbaIdx >= 0)
    {
        const uint64_t ca = a.r[a.rgbaIdx], cb = b.r[b.rgbaIdx]; uint64_t co = 0;
        for (int k = 0; k < 4; k++) co |= uint64_t(uint8_t(lerp(float((ca >> (8 * k)) & 0xFFu), float((cb >> (8 * k)) & 0xFFu)) + 0.5f)) << (8 * k);
        float qa, qb; std::memcpy(&qa, reinterpret_cast<const uint8_t *>(&ca) + 4, 4); std::memcpy(&qb, reinterpret_cast<const uint8_t *>(&cb) + 4, 4);
        const float qo = lerp(qa, qb); uint32_t qi; std::memcpy(&qi, &qo, 4);
        o.r[o.rgbaIdx] = co | (uint64_t(qi) << 32);
    }
}
// [pgsfx] bloom mode: does the tracked state + this tag's primitive describe one of the draws whose shared registers
// the mapper rewrites (blur-buffer clear / scene downsample / composite / depth-mask sprites)?
static int wsBloomTargetState(const State::WsHud &h, uint64_t primRaw)
{
    const uint64_t attr = h.prmodeCont ? primRaw : h.prmode;
    const uint32_t primType = uint32_t(primRaw & 7u), ci = uint32_t((attr >> 9) & 1u);
    const bool tme = ((attr >> 4) & 1u) != 0, abe = ((attr >> 6) & 1u) != 0;
    const State::WsHud::Ctx &c = h.ctx[ci];
    const uint32_t tpsm = uint32_t((c.tex0 >> 20) & 0x3Fu), ttbp0 = uint32_t(c.tex0 & 0x3FFFu), ttw = uint32_t((c.tex0 >> 26) & 0xFu), tth = uint32_t((c.tex0 >> 30) & 0xFu), ttbw = uint32_t((c.tex0 >> 14) & 0x3Fu);
    const bool sprite = primType == 6u, tri = primType >= 3u && primType <= 5u;
    const bool blurBuf = c.fbp == 336u && c.fbw == 4u && c.fpsm == 0u;
    if (blurBuf && sprite && !tme && !abe) return 1;                                                                    // clear
    if (blurBuf && sprite && tme && abe && tpsm == 0u && ttw == 9u && tth == 9u && (ttbp0 == 0u || ttbp0 == 3584u)) return 2;   // downsample
    if ((c.fbp == 0u || c.fbp == 112u) && (c.fpsm == 0u || c.fpsm == 1u) && (sprite || tri) && tme && abe && tpsm == 1u && ttbp0 == 10752u && ttbw == 4u && ttw == 8u && tth == 8u && ((c.test >> 14) & 1u)) return 3;   // composite
    if (sprite && tme && (c.fpsm == 2u || c.fpsm == 10u) && (c.fbp == 0u || c.fbp == 112u) && uint32_t(c.frame >> 32) == 0x3fffu && (tpsm == 50u || tpsm == 58u)) return 4;   // depth mask
    return 0;
}
static bool wsHudSubdivideLocked(State &s, const uint8_t *data, size_t size, float inv)
{
    State::WsHud &h = s.wshud;
    const int layout = g_wsHudLayout.load(std::memory_order_relaxed);
    static const int s_dofModeP = [](){ const char *e = std::getenv("PS2X_PGS_DOFMODE"); return e && e[0] ? std::atoi(e) : 2; }();
    const bool bloom = s_dofModeP == 3 && !GsGpuRenderer::dofBlurEnabled();
    const bool splitting = layout > 0 && h.active && inv < 0.999f;
    if (!splitting && !bloom) return false;
    const State::WsHud saved = h;   // the pre-pass tracks state like the mapper; restore afterwards so the mapper sees the same start
    std::vector<uint8_t> &out = s.wsBuf; out.clear(); out.reserve(size + 4096);
    bool changed = false;
    size_t off = 0;
    while (off + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + off, 8); std::memcpy(&hi, data + off + 8, 8);
        const uint32_t nloop = uint32_t(lo & 0x7FFFu), flg = uint32_t((lo >> 58) & 3u);
        uint32_t nreg = uint32_t((lo >> 60) & 0xFu); if (nreg == 0) nreg = 16;
        if ((lo >> 46) & 1u) h.primRaw = (lo >> 47) & 0x7FFu;
        const size_t tagOff = off; off += 16;
        size_t lastHdr = out.size();   // header position of the last tag emitted for this input tag (splits emit two)
        size_t bytes = 0;
        if (flg == 0u) bytes = size_t(nloop) * nreg * 16u;
        else if (flg == 1u) bytes = (size_t(nloop) * nreg + 1u) / 2u * 16u;
        else bytes = size_t(nloop) * 16u;
        if (off + bytes > size) { out.insert(out.end(), data + tagOff, data + size); off = size; break; }
        bool handled = false;
        if (flg == 0u && nloop > 0)
        {
            // register layout of one loop
            int xyzIdx = -1, uvIdx = -1, stIdx = -1, rgbaIdx = -1; bool xyzf = false, other = false;
            for (uint32_t i = 0; i < nreg; i++)
            {
                const uint32_t r = uint32_t((hi >> (4 * i)) & 0xFu);
                if (r == 0x4 || r == 0x5) { xyzIdx = int(i); xyzf = (r == 0x4); }
                else if (r == 0x3) uvIdx = int(i);
                else if (r == 0x2) stIdx = int(i);
                else if (r == 0x1) rgbaIdx = int(i);
                else if (r == 0xE) { other = true; }
                else if (r == 0xC || r == 0xD) { other = true; }
                else if (r == 0x0 || r == 0xA || r == 0xF) {}
                else other = true;
            }
            const uint32_t primType = uint32_t(h.primRaw & 7u);
            {   // PS2X_PGS_WSHUDLOG=1: the structure of vertex tags while the squeeze is active (why does nothing split?)
                static const bool s_tl = [](){ const char *v = std::getenv("PS2X_PGS_WSHUDLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_tn = 0;
                uint64_t xl0 = 0; if (xyzIdx >= 0) std::memcpy(&xl0, data + off + size_t(xyzIdx) * 16u, 8);
                const float fy0 = float((xl0 >> 32) & 0xFFFFu) / 16.0f - h.ctx[0].ofy;
                uint32_t z0 = 0; if (xyzIdx >= 0) { uint64_t hz; std::memcpy(&hz, data + off + size_t(xyzIdx) * 16u + 8, 8); z0 = xyzf ? uint32_t((hz >> 4) & 0xFFFFFFu) : uint32_t(hz & 0xFFFFFFFFu); }
                if (s_tl && s_tn < 80 && xyzIdx >= 0 && z0 == 0u && fy0 > -40.f && fy0 < 96.f)
                {
                    s_tn++;
                    char regs[40]; int rp = 0; for (uint32_t i = 0; i < nreg && rp < 36; i++) rp += std::snprintf(regs + rp, sizeof(regs) - rp, "%x", uint32_t((hi >> (4 * i)) & 0xFu));
                    uint64_t xl; std::memcpy(&xl, data + off + size_t(xyzIdx) * 16u, 8);
                    std::fprintf(stderr, "[wshudtag] #%u nloop %u nreg %u regs %s prim %u pre %d other %d first x %.1f y %.1f fbp %u\n", s_tn, nloop, nreg, regs, primType, int((lo >> 46) & 1u), other ? 1 : 0,
                                 float(xl & 0xFFFFu) / 16.0f - h.ctx[uint32_t(((h.prmodeCont ? h.primRaw : h.prmode) >> 9) & 1u)].ofx, float((xl >> 32) & 0xFFFFu) / 16.0f - h.ctx[0].ofy, h.ctx[0].fbp);
                }
            }
            if (xyzIdx >= 0 && !other && (primType == 4u || primType == 6u) && nreg <= 16)
            {
                // the HUD rule (same as the mapper) on the tag's vertices, per primitive group
                const uint64_t attr = h.prmodeCont ? h.primRaw : h.prmode;
                const bool fst = ((attr >> 8) & 1u) != 0;
                const uint32_t ci = uint32_t((attr >> 9) & 1u);
                const State::WsHud::Ctx &c = h.ctx[ci];
                const float W = (c.fbw * 64u >= 320u && c.fbw * 64u <= 1024u) ? float(c.fbw * 64u) : 512.0f;
                const bool sceneBuf = (c.fbp == 0u || c.fbp == 112u) && (c.fpsm == 0u || c.fpsm == 1u);
                std::vector<WsVert> vs(nloop);
                for (uint32_t l = 0; l < nloop; l++)
                {
                    WsVert &v = vs[l]; v.nreg = nreg; v.xyzIdx = xyzIdx; v.uvIdx = uvIdx; v.stIdx = stIdx; v.rgbaIdx = rgbaIdx; v.xyzf = xyzf;
                    std::memcpy(v.loop, data + off + size_t(l) * nreg * 16u, size_t(nreg) * 16u);
                    uint64_t xl; std::memcpy(&xl, v.loop + size_t(xyzIdx) * 16u, 8);
                    v.x = float(xl & 0xFFFFu) / 16.0f - c.ofx; v.y = float((xl >> 32) & 0xFFFFu) / 16.0f - c.ofy;
                }
                const float k = W / 512.0f;
                const float cuts[4] = { 124.f * k, 216.f * k, 296.f * k, 388.f * k };
                std::vector<uint8_t> tagOut;
                uint32_t newLoops = 0;
                auto emit = [&](const WsVert &v) { tagOut.insert(tagOut.end(), v.loop, v.loop + size_t(nreg) * 16u); newLoops++; };
                bool tagChanged = false;
                if (primType == 6u && (nloop % 2u) == 0u)
                {
                    for (uint32_t l = 0; l + 1 < nloop; l += 2)
                    {
                        const WsVert &a = vs[l], &b = vs[l + 1];
                        const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
                        const bool hud = sceneBuf && (x1 - x0) > 0.f && (x1 - x0) < 0.8f * W && (y1 - y0) > 0.f && (y1 - y0) < 300.f && y1 < 96.f && (!c.zte || c.ztst == 1u);
                        std::vector<float> ts;
                        if (hud && splitting) for (float cx : cuts) if (cx > x0 + 1.f && cx < x1 - 1.f) ts.push_back((cx - a.x) / (b.x - a.x));
                        if (ts.empty()) { emit(a); emit(b); continue; }
                        std::sort(ts.begin(), ts.end());
                        float tPrev = 0.f; WsVert p0, p1;
                        for (size_t i = 0; i <= ts.size(); i++)
                        {
                            const float tNext = i < ts.size() ? ts[i] : 1.f;
                            wsVertLerp(a, b, tPrev, p0, a.x + (b.x - a.x) * tPrev, c.ofx);
                            wsVertLerp(a, b, tNext, p1, a.x + (b.x - a.x) * tNext, c.ofx);
                            // sprite: first vertex keeps a's y/v/t, second keeps b's (the lerp only moved x/u/s along the span)
                            std::memcpy(p1.loop + size_t(xyzIdx) * 16u + 4, b.loop + size_t(xyzIdx) * 16u + 4, 4);
                            if (uvIdx >= 0) std::memcpy(p1.loop + size_t(uvIdx) * 16u + 4, b.loop + size_t(uvIdx) * 16u + 4, 4);
                            if (stIdx >= 0) std::memcpy(p1.loop + size_t(stIdx) * 16u + 4, b.loop + size_t(stIdx) * 16u + 4, 4);
                            std::memcpy(p0.loop + size_t(xyzIdx) * 16u + 4, a.loop + size_t(xyzIdx) * 16u + 4, 4);
                            emit(p0); emit(p1); tPrev = tNext;
                        }
                        tagChanged = true; h.splitPrims++;
                    }
                    handled = true;
                }
                else if (primType == 4u && nloop == 4u)
                {
                    const WsVert &t0 = vs[0], &t1 = vs[1], &b0 = vs[2], &b1 = vs[3];
                    const bool axisQuad = std::fabs(t0.y - t1.y) < 0.07f && std::fabs(b0.y - b1.y) < 0.07f && std::fabs(t0.x - b0.x) < 0.07f && std::fabs(t1.x - b1.x) < 0.07f;
                    const float x0 = std::min(t0.x, t1.x), x1 = std::max(t0.x, t1.x), y0 = std::min(t0.y, b0.y), y1 = std::max(t0.y, b0.y);
                    uint32_t zt0; std::memcpy(&zt0, t0.loop + size_t(xyzIdx) * 16u + 8, 4);
                    bool zAllZero = true; for (const WsVert &v : vs) { uint32_t z; std::memcpy(&z, v.loop + size_t(xyzIdx) * 16u + 8, 4); if (xyzf) z = (z >> 4) & 0xFFFFFFu; if (z) zAllZero = false; }
                    const bool hud = sceneBuf && fst && zAllZero && (y1 - y0) < 300.f && y1 < 96.f && ((x1 - x0) < 0.8f * W || x0 > 8.f);
                    std::vector<float> ts;
                    if (hud && axisQuad && splitting) for (float cx : cuts) if (cx > x0 + 1.f && cx < x1 - 1.f) ts.push_back((cx - t0.x) / (t1.x - t0.x));
                    if (!ts.empty())
                    {
                        std::sort(ts.begin(), ts.end());
                        std::vector<float> tt; tt.push_back(0.f); for (float t : ts) tt.push_back(t); tt.push_back(1.f);
                        WsVert top, bot;
                        for (float t : tt)
                        {
                            wsVertLerp(t0, t1, t, top, t0.x + (t1.x - t0.x) * t, c.ofx);
                            wsVertLerp(b0, b1, t, bot, b0.x + (b1.x - b0.x) * t, c.ofx);
                            emit(top); emit(bot);
                        }
                        tagChanged = true; h.splitPrims++;
                    }
                    else for (const WsVert &v : vs) emit(v);
                    handled = true;
                }
                if (handled)
                {
                    uint64_t nlo = (lo & ~0x7FFFull) | uint64_t(newLoops & 0x7FFFu);
                    uint8_t hdr[16]; std::memcpy(hdr, &nlo, 8); std::memcpy(hdr + 8, &hi, 8);
                    lastHdr = out.size();
                    out.insert(out.end(), hdr, hdr + 16);
                    out.insert(out.end(), tagOut.begin(), tagOut.end());
                    if (tagChanged) changed = true;
                }
            }
            // state tracking for the classification (registers inside this tag), no scissor rewrite
            for (uint32_t l = 0; l < nloop; l++)
                for (uint32_t i = 0; i < nreg; i++)
                {
                    const uint32_t r = uint32_t((hi >> (4 * i)) & 0xFu);
                    const size_t q = off + (size_t(l) * nreg + i) * 16u;
                    if (r == 0x0) { uint64_t v; std::memcpy(&v, data + q, 8); h.primRaw = v & 0x7FFu; }
                    else if (r == 0x1) { const uint8_t *pq = data + q; h.lastRgbaq = uint64_t(pq[0]) | (uint64_t(pq[4]) << 8) | (uint64_t(pq[8]) << 16) | (uint64_t(pq[12]) << 24) | (0x3f800000ull << 32); }
                    else if (r == 0x6 || r == 0x7) { uint64_t v; std::memcpy(&v, data + q, 8); h.ctx[r - 0x6].tex0 = v; }
                    else if (r == 0xE) { uint64_t v, a; std::memcpy(&v, data + q, 8); std::memcpy(&a, data + q + 8, 8); wsHudRegLocked(s, uint32_t(a & 0xFFu), v, nullptr, 0, inv, false); }
                }
        }
        if (flg == 1u && nloop == 1u && nreg <= 16)
        {   // [pgssplit] BT3's HUD quads: ONE REGLIST loop = [RGBAQ, A+D, TEX0, PRIM, (UV, XYZ2) x 4]. Split the loop into a
            // setup tag (the prefix registers) and a vertex tag (one UV+XYZ2 pair per loop) so vertices can be added.
            std::vector<int> xyzPos;
            for (uint32_t i = 0; i < nreg; i++) { const uint32_t r = uint32_t((hi >> (4 * i)) & 0xFu); if (r == 0x4 || r == 0x5) xyzPos.push_back(int(i)); }
            if (xyzPos.size() >= 2)
            {
                const int g = xyzPos[1] - xyzPos[0];
                bool regular = g >= 1 && xyzPos[0] >= g - 1;
                for (size_t i = 1; i < xyzPos.size() && regular; i++) if (xyzPos[i] - xyzPos[i - 1] != g) regular = false;
                const int prefixN = regular ? xyzPos[0] - (g - 1) : 0;
                const uint32_t V = uint32_t(xyzPos.size());
                // the group's descriptors and attribute indices (relative to the group)
                int uvIdx = -1, stIdx = -1, rgbaIdx = -1; bool xyzf = false, other = false;
                if (regular)
                    for (int i = 0; i < g; i++)
                    {
                        const uint32_t r = uint32_t((hi >> (4 * (prefixN + i))) & 0xFu);
                        if (i == g - 1) xyzf = (r == 0x4);
                        else if (r == 0x3) uvIdx = i; else if (r == 0x2) stIdx = i; else if (r == 0x1) rgbaIdx = i; else if (r == 0xA || r == 0xF) {} else other = true;
                    }
                // the prefix's PRIM (reg 0) decides the primitive for these vertices
                uint64_t primHere = h.primRaw;
                for (int i = 0; i < prefixN; i++) if (uint32_t((hi >> (4 * i)) & 0xFu) == 0x0) { uint64_t v; std::memcpy(&v, data + off + size_t(i) * 8u, 8); primHere = v & 0x7FFu; }
                const uint32_t primType = uint32_t(primHere & 7u);
                const uint64_t attr = h.prmodeCont ? primHere : h.prmode;
                const bool fst = ((attr >> 8) & 1u) != 0;
                const uint32_t ci = uint32_t((attr >> 9) & 1u);
                const State::WsHud::Ctx &c = h.ctx[ci];
                const float W = (c.fbw * 64u >= 320u && c.fbw * 64u <= 1024u) ? float(c.fbw * 64u) : 512.0f;
                const bool sceneBuf = (c.fbp == 0u || c.fbp == 112u) && (c.fpsm == 0u || c.fpsm == 1u);
                if (regular && !other && (primType == 4u || primType == 6u))
                {
                    std::vector<WsVertRL> vs(V);
                    for (uint32_t vi = 0; vi < V; vi++)
                    {
                        WsVertRL &v = vs[vi]; v.nreg = uint32_t(g); v.xyzIdx = g - 1; v.uvIdx = uvIdx; v.stIdx = stIdx; v.rgbaIdx = rgbaIdx; v.xyzf = xyzf;
                        for (int i = 0; i < g; i++) std::memcpy(&v.r[i], data + off + size_t(prefixN + vi * g + i) * 8u, 8);
                        v.x = float(v.r[g - 1] & 0xFFFFu) / 16.0f - c.ofx; v.y = float((v.r[g - 1] >> 16) & 0xFFFFu) / 16.0f - c.ofy;
                    }
                    const float k = W / 512.0f;
                    const float cuts[4] = { 124.f * k, 216.f * k, 296.f * k, 388.f * k };
                    std::vector<uint64_t> regsOut; uint32_t newV = 0; bool tagChanged = false;
                    auto emit = [&](const WsVertRL &v) { for (int i = 0; i < g; i++) regsOut.push_back(v.r[i]); newV++; };
                    auto zOf = [&](const WsVertRL &v) { return xyzf ? uint32_t((v.r[g - 1] >> 32) & 0xFFFFFFu) : uint32_t(v.r[g - 1] >> 32); };
                    if (primType == 6u && (V % 2u) == 0u)
                    {
                        for (uint32_t l = 0; l + 1 < V; l += 2)
                        {
                            const WsVertRL &a = vs[l], &b = vs[l + 1];
                            const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
                            const bool hud = sceneBuf && (x1 - x0) > 0.f && (x1 - x0) < 0.8f * W && (y1 - y0) > 0.f && (y1 - y0) < 300.f && y1 < 96.f && (!c.zte || c.ztst == 1u);
                            std::vector<float> ts;
                            if (hud && splitting) for (float cx : cuts) if (cx > x0 + 1.f && cx < x1 - 1.f) ts.push_back((cx - a.x) / (b.x - a.x));
                            if (ts.empty()) { emit(a); emit(b); continue; }
                            std::sort(ts.begin(), ts.end());
                            float tPrev = 0.f; WsVertRL p0, p1;
                            for (size_t i = 0; i <= ts.size(); i++)
                            {
                                const float tNext = i < ts.size() ? ts[i] : 1.f;
                                wsVertLerpRL(a, b, tPrev, p0, a.x + (b.x - a.x) * tPrev, c.ofx);
                                wsVertLerpRL(a, b, tNext, p1, a.x + (b.x - a.x) * tNext, c.ofx);
                                p0.r[g - 1] = (p0.r[g - 1] & ~(0xFFFFull << 16)) | (a.r[g - 1] & (0xFFFFull << 16));
                                p1.r[g - 1] = (p1.r[g - 1] & ~(0xFFFFull << 16)) | (b.r[g - 1] & (0xFFFFull << 16));
                                if (uvIdx >= 0) { p0.r[uvIdx] = (p0.r[uvIdx] & ~(0x3FFFull << 16)) | (a.r[uvIdx] & (0x3FFFull << 16)); p1.r[uvIdx] = (p1.r[uvIdx] & ~(0x3FFFull << 16)) | (b.r[uvIdx] & (0x3FFFull << 16)); }
                                if (stIdx >= 0) { p0.r[stIdx] = (p0.r[stIdx] & 0xFFFFFFFFull) | (a.r[stIdx] & ~0xFFFFFFFFull); p1.r[stIdx] = (p1.r[stIdx] & 0xFFFFFFFFull) | (b.r[stIdx] & ~0xFFFFFFFFull); }
                                emit(p0); emit(p1); tPrev = tNext;
                            }
                            tagChanged = true; h.splitPrims++;
                        }
                        handled = true;
                    }
                    else if (primType == 4u && V == 4u)
                    {
                        const WsVertRL &t0 = vs[0], &t1 = vs[1], &b0 = vs[2], &b1 = vs[3];
                        const bool axisQuad = std::fabs(t0.y - t1.y) < 0.07f && std::fabs(b0.y - b1.y) < 0.07f && std::fabs(t0.x - b0.x) < 0.07f && std::fabs(t1.x - b1.x) < 0.07f;
                        const float x0 = std::min(t0.x, t1.x), x1 = std::max(t0.x, t1.x), y0 = std::min(t0.y, b0.y), y1 = std::max(t0.y, b0.y);
                        bool zAllZero = true; for (const WsVertRL &v : vs) if (zOf(v)) zAllZero = false;
                        const bool hud = sceneBuf && fst && zAllZero && (y1 - y0) < 300.f && y1 < 96.f && ((x1 - x0) < 0.8f * W || x0 > 8.f);
                        std::vector<float> ts;
                        if (hud && axisQuad && splitting) for (float cx : cuts) if (cx > x0 + 1.f && cx < x1 - 1.f) ts.push_back((cx - t0.x) / (t1.x - t0.x));
                        if (!ts.empty())
                        {
                            std::sort(ts.begin(), ts.end());
                            std::vector<float> tt; tt.push_back(0.f); for (float t : ts) tt.push_back(t); tt.push_back(1.f);
                            WsVertRL top, bot;
                            for (float t : tt)
                            {
                                wsVertLerpRL(t0, t1, t, top, t0.x + (t1.x - t0.x) * t, c.ofx);
                                wsVertLerpRL(b0, b1, t, bot, b0.x + (b1.x - b0.x) * t, c.ofx);
                                emit(top); emit(bot);
                            }
                            tagChanged = true; h.splitPrims++;
                        }
                        else for (const WsVertRL &v : vs) emit(v);
                        handled = true;
                    }
                    if (handled)
                    {
                        if (!tagChanged) { out.insert(out.end(), data + tagOff, data + off + bytes); }   // untouched: copy the original tag
                        else
                        {
                            // tag A: the prefix registers (setup), EOP cleared; tag B: the vertices, g regs per loop, original EOP
                            if (prefixN > 0)
                            {
                                uint64_t alo = (lo & ~(0x7FFFull | (1ull << 15) | (0xFull << 60))) | 1ull | (uint64_t(prefixN & 15) << 60);
                                uint64_t ahi = hi & ((prefixN >= 16) ? ~0ull : ((1ull << (4 * prefixN)) - 1ull));
                                uint8_t hdr[16]; std::memcpy(hdr, &alo, 8); std::memcpy(hdr + 8, &ahi, 8);
                                out.insert(out.end(), hdr, hdr + 16);
                                std::vector<uint64_t> pre; for (int i = 0; i < prefixN; i++) { uint64_t v; std::memcpy(&v, data + off + size_t(i) * 8u, 8); pre.push_back(v); }
                                if (pre.size() & 1u) pre.push_back(0);
                                const uint8_t *pb = reinterpret_cast<const uint8_t *>(pre.data()); out.insert(out.end(), pb, pb + pre.size() * 8u);
                            }
                            uint64_t blo = (lo & ~(0x7FFFull | (1ull << 46) | (0x7FFull << 47) | (0xFull << 60))) | uint64_t(newV & 0x7FFFu) | (uint64_t(g & 15) << 60);
                            uint64_t bhi = 0; for (int i = 0; i < g; i++) bhi |= ((hi >> (4 * (prefixN + i))) & 0xFull) << (4 * i);
                            uint8_t hdr[16]; std::memcpy(hdr, &blo, 8); std::memcpy(hdr + 8, &bhi, 8);
                            lastHdr = out.size();
                            out.insert(out.end(), hdr, hdr + 16);
                            if (regsOut.size() & 1u) regsOut.push_back(0);
                            const uint8_t *rb = reinterpret_cast<const uint8_t *>(regsOut.data()); out.insert(out.end(), rb, rb + regsOut.size() * 8u);
                            changed = true;
                        }
                    }
                }
            }
            // state: PRIM / RGBAQ / TEX0 inside the loop
            for (uint32_t i = 0; i < nreg; i++)
            {
                const uint32_t r = uint32_t((hi >> (4 * i)) & 0xFu); uint64_t v; std::memcpy(&v, data + off + size_t(i) * 8u, 8);
                if (r == 0x0) h.primRaw = v & 0x7FFu; else if (r == 0x1) h.lastRgbaq = v; else if (r == 0x6 || r == 0x7) h.ctx[r - 0x6].tex0 = v;
            }
        }
        if (!handled && flg == 1u && nloop > 0 && nreg <= 16)
        {
            int xyzIdx = -1, uvIdx = -1, stIdx = -1, rgbaIdx = -1; bool xyzf = false, other = false;
            for (uint32_t i = 0; i < nreg; i++)
            {
                const uint32_t r = uint32_t((hi >> (4 * i)) & 0xFu);
                if (r == 0x4 || r == 0x5) { xyzIdx = int(i); xyzf = (r == 0x4); }
                else if (r == 0x3) uvIdx = int(i);
                else if (r == 0x2) stIdx = int(i);
                else if (r == 0x1) rgbaIdx = int(i);
                else if (r == 0x0 || r == 0xA || r == 0xF) {}
                else other = true;
            }
            const uint32_t primType = uint32_t(h.primRaw & 7u);
            {
                static const bool s_tl = [](){ const char *v = std::getenv("PS2X_PGS_WSHUDLOG"); return v && v[0] && v[0] != '0'; }(); static unsigned s_tn = 0;
                if (s_tl && s_tn < 80 && xyzIdx >= 0)
                {
                    uint64_t x0v; std::memcpy(&x0v, data + off + size_t(xyzIdx) * 8u, 8);
                    const float fy0 = float((x0v >> 16) & 0xFFFFu) / 16.0f - h.ctx[0].ofy;
                    const uint32_t z0 = xyzf ? uint32_t((x0v >> 32) & 0xFFFFFFu) : uint32_t(x0v >> 32);
                    if (z0 == 0u && fy0 > -40.f && fy0 < 96.f)
                    {
                        s_tn++;
                        char regs[40]; int rp = 0; for (uint32_t i = 0; i < nreg && rp < 36; i++) rp += std::snprintf(regs + rp, sizeof(regs) - rp, "%x", uint32_t((hi >> (4 * i)) & 0xFu));
                        std::fprintf(stderr, "[wshudtag] REGLIST #%u nloop %u nreg %u regs %s prim %u pre %d other %d first x %.1f y %.1f\\n", s_tn, nloop, nreg, regs, primType, int((lo >> 46) & 1u), other ? 1 : 0, float(x0v & 0xFFFFu) / 16.0f - h.ctx[0].ofx, fy0);
                    }
                }
            }
            if (xyzIdx >= 0 && !other && (primType == 4u || primType == 6u))
            {
                const uint64_t attr = h.prmodeCont ? h.primRaw : h.prmode;
                const bool fst = ((attr >> 8) & 1u) != 0;
                const uint32_t ci = uint32_t((attr >> 9) & 1u);
                const State::WsHud::Ctx &c = h.ctx[ci];
                const float W = (c.fbw * 64u >= 320u && c.fbw * 64u <= 1024u) ? float(c.fbw * 64u) : 512.0f;
                const bool sceneBuf = (c.fbp == 0u || c.fbp == 112u) && (c.fpsm == 0u || c.fpsm == 1u);
                std::vector<WsVertRL> vs(nloop);
                for (uint32_t l = 0; l < nloop; l++)
                {
                    WsVertRL &v = vs[l]; v.nreg = nreg; v.xyzIdx = xyzIdx; v.uvIdx = uvIdx; v.stIdx = stIdx; v.rgbaIdx = rgbaIdx; v.xyzf = xyzf;
                    for (uint32_t i = 0; i < nreg; i++) std::memcpy(&v.r[i], data + off + (size_t(l) * nreg + i) * 8u, 8);
                    v.x = float(v.r[xyzIdx] & 0xFFFFu) / 16.0f - c.ofx; v.y = float((v.r[xyzIdx] >> 16) & 0xFFFFu) / 16.0f - c.ofy;
                }
                const float k = W / 512.0f;
                const float cuts[4] = { 124.f * k, 216.f * k, 296.f * k, 388.f * k };
                std::vector<uint64_t> regsOut; uint32_t newLoops = 0; bool tagChanged = false;
                auto emit = [&](const WsVertRL &v) { for (uint32_t i = 0; i < nreg; i++) regsOut.push_back(v.r[i]); newLoops++; };
                auto zOf = [&](const WsVertRL &v) { return xyzf ? uint32_t((v.r[xyzIdx] >> 32) & 0xFFFFFFu) : uint32_t(v.r[xyzIdx] >> 32); };
                if (primType == 6u && (nloop % 2u) == 0u)
                {
                    for (uint32_t l = 0; l + 1 < nloop; l += 2)
                    {
                        const WsVertRL &a = vs[l], &b = vs[l + 1];
                        const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
                        const bool hud = sceneBuf && (x1 - x0) > 0.f && (x1 - x0) < 0.8f * W && (y1 - y0) > 0.f && (y1 - y0) < 300.f && y1 < 96.f && (!c.zte || c.ztst == 1u);
                        std::vector<float> ts;
                        if (hud && splitting) for (float cx : cuts) if (cx > x0 + 1.f && cx < x1 - 1.f) ts.push_back((cx - a.x) / (b.x - a.x));
                        if (ts.empty()) { emit(a); emit(b); continue; }
                        std::sort(ts.begin(), ts.end());
                        float tPrev = 0.f; WsVertRL p0, p1;
                        for (size_t i = 0; i <= ts.size(); i++)
                        {
                            const float tNext = i < ts.size() ? ts[i] : 1.f;
                            wsVertLerpRL(a, b, tPrev, p0, a.x + (b.x - a.x) * tPrev, c.ofx);
                            wsVertLerpRL(a, b, tNext, p1, a.x + (b.x - a.x) * tNext, c.ofx);
                            // keep each corner's own y / v / t (only x, u, s move along the span)
                            p0.r[xyzIdx] = (p0.r[xyzIdx] & ~(0xFFFFull << 16)) | (a.r[xyzIdx] & (0xFFFFull << 16));
                            p1.r[xyzIdx] = (p1.r[xyzIdx] & ~(0xFFFFull << 16)) | (b.r[xyzIdx] & (0xFFFFull << 16));
                            if (uvIdx >= 0) { p0.r[uvIdx] = (p0.r[uvIdx] & ~(0x3FFFull << 16)) | (a.r[uvIdx] & (0x3FFFull << 16)); p1.r[uvIdx] = (p1.r[uvIdx] & ~(0x3FFFull << 16)) | (b.r[uvIdx] & (0x3FFFull << 16)); }
                            if (stIdx >= 0) { p0.r[stIdx] = (p0.r[stIdx] & 0xFFFFFFFFull) | (a.r[stIdx] & ~0xFFFFFFFFull); p1.r[stIdx] = (p1.r[stIdx] & 0xFFFFFFFFull) | (b.r[stIdx] & ~0xFFFFFFFFull); }
                            emit(p0); emit(p1); tPrev = tNext;
                        }
                        tagChanged = true; h.splitPrims++;
                    }
                    handled = true;
                }
                else if (primType == 4u && nloop == 4u)
                {
                    const WsVertRL &t0 = vs[0], &t1 = vs[1], &b0 = vs[2], &b1 = vs[3];
                    const bool axisQuad = std::fabs(t0.y - t1.y) < 0.07f && std::fabs(b0.y - b1.y) < 0.07f && std::fabs(t0.x - b0.x) < 0.07f && std::fabs(t1.x - b1.x) < 0.07f;
                    const float x0 = std::min(t0.x, t1.x), x1 = std::max(t0.x, t1.x), y0 = std::min(t0.y, b0.y), y1 = std::max(t0.y, b0.y);
                    bool zAllZero = true; for (const WsVertRL &v : vs) if (zOf(v)) zAllZero = false;
                    const bool hud = sceneBuf && fst && zAllZero && (y1 - y0) < 300.f && y1 < 96.f && ((x1 - x0) < 0.8f * W || x0 > 8.f);
                    std::vector<float> ts;
                    if (hud && axisQuad && splitting) for (float cx : cuts) if (cx > x0 + 1.f && cx < x1 - 1.f) ts.push_back((cx - t0.x) / (t1.x - t0.x));
                    if (!ts.empty())
                    {
                        std::sort(ts.begin(), ts.end());
                        std::vector<float> tt; tt.push_back(0.f); for (float t : ts) tt.push_back(t); tt.push_back(1.f);
                        WsVertRL top, bot;
                        for (float t : tt)
                        {
                            wsVertLerpRL(t0, t1, t, top, t0.x + (t1.x - t0.x) * t, c.ofx);
                            wsVertLerpRL(b0, b1, t, bot, b0.x + (b1.x - b0.x) * t, c.ofx);
                            emit(top); emit(bot);
                        }
                        tagChanged = true; h.splitPrims++;
                    }
                    else for (const WsVertRL &v : vs) emit(v);
                    handled = true;
                }
                if (handled)
                {
                    uint64_t nlo = (lo & ~0x7FFFull) | uint64_t(newLoops & 0x7FFFu);
                    uint8_t hdr[16]; std::memcpy(hdr, &nlo, 8); std::memcpy(hdr + 8, &hi, 8);
                    lastHdr = out.size();
                    out.insert(out.end(), hdr, hdr + 16);
                    if (regsOut.size() & 1u) regsOut.push_back(0);   // odd register count: the last half-qword is padding
                    const uint8_t *rb = reinterpret_cast<const uint8_t *>(regsOut.data());
                    out.insert(out.end(), rb, rb + regsOut.size() * 8u);
                    if (tagChanged) changed = true;
                }
            }
            // state: PRIM / RGBAQ / TEX0 via REGLIST regs
            for (uint32_t l = 0; l < nloop; l++)
                for (uint32_t i = 0; i < nreg; i++)
                {
                    const uint32_t r = uint32_t((hi >> (4 * i)) & 0xFu); uint64_t v; std::memcpy(&v, data + off + (size_t(l) * nreg + i) * 8u, 8);
                    if (r == 0x0) h.primRaw = v & 0x7FFu; else if (r == 0x1) h.lastRgbaq = v; else if (r == 0x6 || r == 0x7) h.ctx[r - 0x6].tex0 = v;
                }
        }
        if (!handled) out.insert(out.end(), data + tagOff, data + off + bytes);
        if (bloom && flg <= 1u && nloop > 0)
        {   // vertex tag of a bloom target: append an A+D tag restoring ALPHA / RGBAQ / TEX0 (originals) so the mapper's
            // per-draw rewrites of those shared registers cannot leak into the draws that follow
            bool hasXyz = false;
            for (uint32_t i = 0; i < nreg; i++) { const uint32_t r = uint32_t((hi >> (4 * i)) & 0xFu); if (r == 0x4 || r == 0x5) hasXyz = true; }
            const int cls = hasXyz ? wsBloomTargetState(h, h.primRaw) : 0;
            if (cls != 0)
            {
                const uint64_t attr = h.prmodeCont ? h.primRaw : h.prmode;
                const uint32_t ci = uint32_t((attr >> 9) & 1u);
                // only what the mapper changes for this class: clear -> RGBAQ; downsample / composite -> ALPHA;
                // depth mask -> TEX0 (CLD cleared: a TEX0 write with its load bits set reloads the palette from memory
                // that may no longer hold it) + RGBAQ
                std::vector<std::pair<uint64_t, uint64_t>> regs;
                if (cls == 1 || cls == 4) regs.emplace_back(h.lastRgbaq, 0x01ull);
                if (cls == 2 || cls == 3) regs.emplace_back(h.ctx[ci].alpha, 0x42ull + ci);
                if (cls == 4) regs.emplace_back(h.ctx[ci].tex0 & ~(7ull << 61), 0x06ull + ci);
                // move the tag's EOP (if any) onto the restore tag
                uint64_t tlo; std::memcpy(&tlo, out.data() + lastHdr, 8);
                const bool eop = ((tlo >> 15) & 1u) != 0;
                if (eop) { tlo &= ~(1ull << 15); std::memcpy(out.data() + lastHdr, &tlo, 8); }
                uint64_t rlo = uint64_t(regs.size()) | (eop ? (1ull << 15) : 0ull) | (1ull << 60);   // NLOOP n, PACKED, NREG 1
                uint64_t rhi = 0xEull;                                                                 // A+D
                uint8_t hdr[16]; std::memcpy(hdr, &rlo, 8); std::memcpy(hdr + 8, &rhi, 8);
                out.insert(out.end(), hdr, hdr + 16);
                for (const auto &r : regs) { uint8_t q[16]; std::memcpy(q, &r.first, 8); std::memcpy(q + 8, &r.second, 8); out.insert(out.end(), q, q + 16); }
                changed = true; h.bloomRestores++;
            }
        }
        off += bytes;
    }
    if (off < size) out.insert(out.end(), data + off, data + size);
    h = saved;
    return changed;
}
// Walk one GIF packet (PACKED / REGLIST) and rewrite HUD vertex X in place. Cheap: a few branches per qword.
void wsHudRewriteLocked(State &s, uint8_t *data, size_t size)
{
    State::WsHud &h = s.wshud;
    if (h.lastSwap != s.swaps)
    {   // frame boundary: fold the finished frame's verdict into the sticky "scene present" gate (5-frame hysteresis)
        if (h.lastSwap != ~0ull) { if (h.frameHad3d) { h.active = true; h.no3dRun = 0; } else if (++h.no3dRun >= 5) h.active = false; }
        h.lastSwap = s.swaps; h.frameHad3d = false;
    }
    // The squeeze factor in FRAME pixels (HUD coordinates): desired h-scale (authentic TV pixel = v-scale x k) over
    // the actual h-scale of the full-window stretch. The present's g_ps2xWsHudInv is computed from the SCANOUT size,
    // which already carries the CRTC's 512 -> 640 magnification, so it under-squeezes the backend by 1.25.
    static const float s_pixk = [](){ const char *v = std::getenv("PS2X_PIXK"); const float f = v ? float(std::atof(v)) : 1.08f; return (f > 0.5f && f < 2.0f) ? f : 1.08f; }();
    float inv = 1.0f;
    {
        const uint32_t dw = g_presentW.load(std::memory_order_relaxed), dh = g_presentH.load(std::memory_order_relaxed);
        const float fw = (h.ctx[0].fbw * 64u >= 320u && h.ctx[0].fbw * 64u <= 1024u) ? float(h.ctx[0].fbw * 64u) : 512.0f;
        const float fh = s.baseH ? float(s.baseH) : 448.0f;
        if (g_ps2xWsHudInv < 0.999f && dw && dh)
        {
            inv = (float(dh) / fh * s_pixk) / (float(dw) / fw);
            if (inv < 0.4f) inv = 0.4f; if (inv > 1.0f) inv = 1.0f;
        }
    }
    h.lastInv = inv;
    size_t off = 0;
    while (off + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + off, 8); std::memcpy(&hi, data + off + 8, 8);
        const uint32_t nloop = uint32_t(lo & 0x7FFFu), flg = uint32_t((lo >> 58) & 3u);
        uint32_t nreg = uint32_t((lo >> 60) & 0xFu); if (nreg == 0) nreg = 16;
        if ((lo >> 46) & 1u) { h.primRaw = (lo >> 47) & 0x7FFu; h.qn = 0; }   // PRE: the tag carries PRIM
        off += 16;
        if (nloop == 0) continue;
        if (flg == 0u)
        {   // PACKED
            const size_t bytes = size_t(nloop) * nreg * 16u;
            if (off + bytes > size) break;
            for (uint32_t l = 0; l < nloop; l++)
                for (uint32_t i = 0; i < nreg; i++)
                {
                    const uint32_t r = uint32_t((hi >> (4 * i)) & 0xFu);
                    const size_t q = off + (size_t(l) * nreg + i) * 16u;
                    switch (r)
                    {
                    case 0x0: { uint64_t v; std::memcpy(&v, data + q, 8); h.primRaw = v & 0x7FFu; h.qn = 0; break; }
                    case 0x6: case 0x7: { uint64_t v; std::memcpy(&v, data + q, 8); h.ctx[r - 0x6].tex0 = v; h.ctx[r - 0x6].tex0Off = q; break; }
                    case 0x1: { const uint8_t *pq = data + q; h.lastRgbaq = uint64_t(pq[0]) | (uint64_t(pq[4]) << 8) | (uint64_t(pq[8]) << 16) | (uint64_t(pq[12]) << 24) | (0x3f800000ull << 32); if (h.rgbaN < 8) { h.rgba[h.rgbaN].off = q; h.rgba[h.rgbaN].packed = true; h.rgbaN++; } break; }
                    case 0x3: if (h.uvN < 8) { h.uv[h.uvN].off = q; h.uv[h.uvN].packed = true; h.uvN++; } break;
                    case 0x4: case 0x5: case 0xC: case 0xD:
                    {
                        uint64_t qhi; std::memcpy(&qhi, data + q + 8, 8);
                        const bool xyzf = (r == 0x4 || r == 0xC), kick = (r == 0x4 || r == 0x5) && ((qhi >> 47) & 1u) == 0u;
                        wsHudVertexLocked(s, data, q, true, xyzf, kick, inv);
                        break;
                    }
                    case 0xE: { uint64_t v, a; std::memcpy(&v, data + q, 8); std::memcpy(&a, data + q + 8, 8); wsHudRegLocked(s, uint32_t(a & 0xFFu), v, data, q, inv); break; }
                    default: break;
                    }
                }
            off += bytes;
        }
        else if (flg == 1u)
        {   // REGLIST: 64-bit registers, two per qword
            const size_t nregs = size_t(nloop) * nreg, bytes = (nregs + 1u) / 2u * 16u;
            if (off + bytes > size) break;
            for (size_t k = 0; k < nregs; k++)
            {
                const uint32_t r = uint32_t((hi >> (4 * (k % nreg))) & 0xFu);
                const size_t q = off + k * 8u;
                switch (r)
                {
                case 0x0: { uint64_t v; std::memcpy(&v, data + q, 8); h.primRaw = v & 0x7FFu; h.qn = 0; break; }
                case 0x4: case 0x5: case 0xC: case 0xD: wsHudVertexLocked(s, data, q, false, (r == 0x4 || r == 0xC), (r == 0x4 || r == 0x5), inv); break;
                case 0x6: case 0x7: { uint64_t v; std::memcpy(&v, data + q, 8); h.ctx[r - 0x6].tex0 = v; h.ctx[r - 0x6].tex0Off = q; break; }
                case 0x1: { uint64_t v; std::memcpy(&v, data + q, 8); h.lastRgbaq = v; if (h.rgbaN < 8) { h.rgba[h.rgbaN].off = q; h.rgba[h.rgbaN].packed = false; h.rgbaN++; } break; }
                case 0x3: if (h.uvN < 8) { h.uv[h.uvN].off = q; h.uv[h.uvN].packed = false; h.uvN++; } break;
                default: break;   // A+D is not valid in REGLIST
                }
            }
            off += bytes;
        }
        else off += size_t(nloop) * 16u;   // IMAGE / disabled
    }
}
void applyPseudoRegsLocked(State &s, const uint8_t *data, size_t size)
{
    GSRegisters *r = s.signals.regs;
    if (!r) return;
    size_t off = 0;
    while (off + 16 <= size)
    {
        uint64_t lo, hi; std::memcpy(&lo, data + off, 8); std::memcpy(&hi, data + off + 8, 8);
        const uint32_t nloop = uint32_t(lo & 0x7FFFu), flg = uint32_t((lo >> 58) & 3u);
        uint32_t nreg = uint32_t((lo >> 60) & 0xFu); if (nreg == 0) nreg = 16;
        off += 16;
        if (nloop == 0) continue;
        if (flg == 0u)
        {   // PACKED: nloop * nreg qwords
            bool hasAD = false;
            for (uint32_t i = 0; i < nreg; i++) if (((hi >> (4 * i)) & 0xFu) == 0xEu) hasAD = true;
            const size_t bytes = size_t(nloop) * nreg * 16u;
            if (hasAD && off + bytes <= size)
            {
                for (uint32_t l = 0; l < nloop; l++)
                    for (uint32_t i = 0; i < nreg; i++)
                    {
                        if (((hi >> (4 * i)) & 0xFu) != 0xEu) continue;
                        const uint8_t *q = data + off + (size_t(l) * nreg + i) * 16u;
                        uint64_t v, a; std::memcpy(&v, q, 8); std::memcpy(&a, q + 8, 8);
                        switch (a & 0xFFu)
                        {
                        case 0x41:
                            // [pmodeguard] PMODE has 16 meaningful bits (EN1, EN2, CRTMD, MMOD, AMOD,
                            // SLBG, ALP); anything above is reserved. This path was writing
                            // 0x1bf000001ff0000 thirty times a second -- a DISPLAY-shaped value landing
                            // in the PMODE slot. Harmless only because the game's own bus store
                            // (0x7f23, the value actually presented) lands after it once the kick queue
                            // is drained; remove the drain and this garbage reaches the scanout, which
                            // is the black/squished flicker seen under GSQUEUE=1/3. Drop it at source.
                            if (v <= 0xFFFFull) { r->pmode = v; ps2xNotePmodeWrite(1, v); }
                            else                { ps2xNotePmodeWrite(3, v); }
                            s.privHist[0x00 >> 4]++; s.pseudoSeen++; break;
                        case 0x42: r->smode2 = v; s.privHist[0x20 >> 4]++; s.pseudoSeen++; break;
                        case 0x59: r->dispfb1 = v; s.privHist[0x70 >> 4]++; s.pseudoSeen++; break;
                        case 0x5a: r->display1 = v; s.privHist[0x80 >> 4]++; s.pseudoSeen++; break;
                        case 0x5b: r->dispfb2 = v; s.privHist[0x90 >> 4]++; s.pseudoSeen++; break;
                        case 0x5c: r->display2 = v; s.privHist[0xA0 >> 4]++; s.pseudoSeen++; break;
                        case 0x5f: r->bgcolor = v; s.privHist[0xE0 >> 4]++; s.pseudoSeen++; break;
                        default: break;
                        }
                    }
            }
            off += bytes;
        }
        else if (flg == 1u) off += (size_t(nloop) * nreg + 1u) / 2u * 16u;   // REGLIST: 2 regs per qword
        else off += size_t(nloop) * 16u;                                       // IMAGE / disabled
    }
}
} // namespace

// [pgslive] re-create the backend at another super-sampling level, carrying VRAM (and, in pack mode, our parse's
// register file) across. Super-sampling is fixed at GSInterface creation; this makes the overlay's Internal
// Resolution live. Frame contexts and the Vulkan device stay; replacement images (owned by the hook's cache) survive.
static void drainReadbackLocked(State &s)
{
    for (RbSlot &slot : g_rb)
    {
        if (slot.pending && slot.fence) { slot.fence->wait(); consumeSlotLocked(s, slot); }
        slot = RbSlot{};
    }
    g_rbIdx = 0;
}
static bool reinitLocked(State &s, uint32_t ssaa)
{
    drainReadbackLocked(s);
    s.iface.flush();
    // the fork's own savestate mechanism: VRAM + the register file + the privileged registers, then clobber
    std::vector<uint8_t> vram(4u * 1024u * 1024u);
    if (const void *rd = s.iface.map_vram_read(0, vram.size())) std::memcpy(vram.data(), rd, vram.size());
    const RegisterState regs = s.iface.get_register_state();
    const PrivRegisterState priv = s.iface.get_priv_register_state();
    s.device.wait_idle();
    s.iface.~GSInterface();
    new (&s.iface) GSInterface();
    GSOptions opts = {};
    opts.vram_size = 4 * 1024 * 1024;
    opts.super_sampling = ssaa >= 16 ? SuperSampling::X16 : ssaa >= 8 ? SuperSampling::X8 : ssaa >= 4 ? SuperSampling::X4 : ssaa >= 2 ? SuperSampling::X2 : SuperSampling::X1;
    opts.super_sampled_textures = [](){ const char *v = std::getenv("PS2X_PGS_SSTEX"); return !(v && v[0] == '0'); }();
    if (!s.iface.init(&s.device, opts)) { std::fprintf(stderr, "[pgs] re-init at ssaa %u FAILED -- backend off\n", ssaa); s.failed = true; s.inited = false; g_enabled.store(0); return false; }
    s.iface.set_signal_interface(&s.signals);
    s.iface.set_hacks(s.hacks);
#if defined(PARALLEL_GS_TEXREPLACE)
    if (packMode()) s.iface.set_texture_replacement_interface(&s.replacer);
#endif
    if (s.timestamps) { DebugMode dm = {}; dm.timestamps = true; s.iface.set_debug_mode(dm); }
    if (void *w = s.iface.map_vram_write(0, vram.size())) { std::memcpy(w, vram.data(), vram.size()); s.iface.end_vram_write(0, vram.size()); }
    s.iface.get_register_state() = regs;
    s.iface.get_priv_register_state() = priv;
    s.iface.get_register_state().cached_cbp[0] = s.iface.get_register_state().cached_cbp[1] = UINT32_MAX;   // the CLUT cache is empty now
    s.iface.clobber_register_state();
    // the palette cache is gone with the old renderer: reload each context's CLUT from VRAM (BT3 keeps them there)
    for (int c = 0; c < 2; ++c)
    {
        const uint64_t tex0 = regs.ctx[c].tex0.bits;
        if (!tex0) continue;
        const uint32_t psm = uint32_t(tex0 >> 20) & 0x3f;
        if (!(psm == 0x13 || psm == 0x14 || psm == 0x1b || psm == 0x24 || psm == 0x2c)) continue;   // paletted only
        s.iface.write_register(c ? RegisterAddr::TEX0_2 : RegisterAddr::TEX0_1, (tex0 & ~(7ull << 61)) | (1ull << 61));
        s.iface.get_register_state().ctx[c].tex0.bits = tex0;
    }
    s.ssaa = ssaa; s.baseW = s.baseH = 0;
    std::fprintf(stderr, "[pgs] backend re-created live at ssaa=%u (VRAM + register file carried over)\n", ssaa);
    return true;
}
void setPackEnabled(bool on)
{
    const int prev = g_packOn.exchange(on ? 1 : 0);
    if (prev != (on ? 1 : 0)) g_packFlushReq.store(1);
}
void setRenderScale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    g_wantScale.store(scale, std::memory_order_relaxed);
}
bool enabled()
{
    int v = g_enabled.load(std::memory_order_relaxed);
    if (v < 0) { v = envOn("PS2X_PGS") ? 1 : 0; g_enabled.store(v, std::memory_order_relaxed); }
    return v != 0;
}
bool packMode() { static const bool p = envOn("PS2X_PGS") && envOn("PS2X_PGS_PACK"); return p; }
bool coalesce() { static const bool c = envOn("PS2X_PGS_COALESCE") && !packMode(); return c; }   // pack mode needs per-packet order
void setGs(GS *gs) { State &s = st(); std::lock_guard<std::mutex> lk(s.mtx); s.replacer.gs = gs; }
// [gsprof] PS2X_GSPROF=1: split the gifTransfer body. It is 83% of GsThread, which is the
// busiest unit in the pipeline (689 ms/s of a 23.8 ms swap), and it contains up to THREE walks of
// the same packet: applyPseudoRegs (ours), the widescreen-HUD rewrite (ours), and the backend's
// own parse. Which one owns the time has never been measured.
std::atomic<unsigned long long> g_gsProfPseudoNs{0}, g_gsProfWsHudNs{0}, g_gsProfBackendNs{0}, g_gsProfCalls{0};
// [gsprof] our own GS parse (GS::processGIFPacket via the arbiter). guestprof's gif= counter is
// per-guest-thread and this runs on GsThread, so gif=0 in every log said nothing about it.
std::atomic<unsigned long long> g_gsProfOursNs{0}, g_gsProfOursCalls{0};
bool gsProfOn() { static const bool v = envOn("PS2X_GSPROF"); return v; }
static thread_local bool t_suppressed = false;
void setSuppressed(bool on) { t_suppressed = on; }
bool exclusive() { static const bool ex = envOn("PS2X_PGS_EXCLUSIVE") && !packMode(); return ex; }   // pack mode keeps our (state-only) parse

bool gifTransfer(uint8_t pathId, const uint8_t *data, size_t size)
{
    if (!data || size < 16 || pathId < 1 || pathId > 3) return false;
    if (t_suppressed)
    {   // [skipourparse] Suppressed means the COALESCED bulk call already submitted this packet, so
        // it genuinely was consumed -- but returning false told GifArbiter::process otherwise, and
        // its exclusive-mode early-out is conditional on true. Result: our full GS parse ran on every
        // packet in EXCLUSIVE mode, the mode whose banner says "our GS parse skipped". Returning true
        // is the honest answer. Behind a flag because our parse also maintains the GS state mirror
        // (runtime->gs()), and anything still reading that in exclusive mode would go stale.
        static const bool s_skip = [](){ const bool on = envOn("PS2X_PGS_SKIPOURPARSE");
            std::fprintf(stderr, "[skipourparse] PS2X_PGS_SKIPOURPARSE=%d (1 = our GS parse really is skipped in EXCLUSIVE)\n", on ? 1 : 0);
            return on; }();
        return s_skip;
    }
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!initLocked(s)) return false;
    const auto t0 = std::chrono::steady_clock::now();
    if (g_packFlushReq.exchange(0)) { s.iface.invalidate_all_cached_textures(); std::fprintf(stderr, "[pgs] texture replacement %s: cached textures dropped\n", g_packOn.load() ? "ON" : "OFF"); }   // [pgslive]
    const bool prof = gsProfOn();
    if (prof) g_gsProfCalls.fetch_add(1, std::memory_order_relaxed);
    if (exclusive())
    {
        const auto tp = std::chrono::steady_clock::now();
        applyPseudoRegsLocked(s, data, size);
        if (prof) g_gsProfPseudoNs.fetch_add((unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tp).count(), std::memory_order_relaxed);
    }
    // [gsprof] clock reads only when profiling: gifTransfer runs ~450k times/s in a fight
    const auto tws = prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    {   // [pgswshud] widescreen HUD squeeze (PS2X_PGS_WSHUD=0 disables): rewrite HUD vertex X before the backend parses
        static const bool s_wshud = [](){ const char *v = std::getenv("PS2X_PGS_WSHUD"); return !(v && v[0] == '0'); }();
        static const bool s_inkShiftEnvOn = [](){ const char *v = std::getenv("PS2X_PGS_INKSHIFT"); return v && v[0] && std::atof(v) > 0.0; }();
        const bool inkWork = !GsGpuRenderer::outlineEnabled() || GsGpuRenderer::inkStrengthPct() != 199 || s_inkShiftEnvOn || g_inkWidthPct.load(std::memory_order_relaxed) < 100 || g_inkColor.load(std::memory_order_relaxed) != 0u
                          || !GsGpuRenderer::shadowsEnabled() || !GsGpuRenderer::dofBlurEnabled();   // [pgsink] [pgsfx]
        if (s_wshud && (g_ps2xWsHudInv < 0.999f || s.wshud.active || inkWork))
        {
            const uint8_t *xdata = data; size_t xsize = size;
            if (wsHudSubdivideLocked(s, data, size, s.wshud.lastInv)) { xdata = s.wsBuf.data(); xsize = s.wsBuf.size(); }
            wsHudRewriteLocked(s, const_cast<uint8_t *>(xdata), xsize);   // the packet buffer is the arbiter's copy (or our rebuilt one)
            data = xdata; size = xsize;
        }
    }
    if (prof) g_gsProfWsHudNs.fetch_add((unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - tws).count(), std::memory_order_relaxed);
    const auto tbe = prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    s.iface.gif_transfer(pathId - 1u, data, size);
    if (prof) g_gsProfBackendNs.fetch_add((unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - tbe).count(), std::memory_order_relaxed);
    {   // [vramprobe] PS2X_PGS_VRAMPROBE=1: after the depth-mask pass, print the frame's alpha per column and the Z top bytes
        static const bool s_probe = envOn("PS2X_PGS_VRAMPROBE"); static unsigned s_n = 0;
        if (s_probe && s_n < 4 && g_pgsProbeReq.exchange(0) != 0)
        {
            s_n++;
            const uint32_t fbp = g_pgsProbeFbp.load(), zbp = g_pgsProbeZbp.load();
            s.iface.flush();
            const uint8_t *v = static_cast<const uint8_t *>(s.iface.map_vram_read(0, 4u * 1024u * 1024u));
            if (v)
            {
                for (uint32_t y : { 60u, 200u, 300u })
                {
                    std::fprintf(stderr, "[vramprobe] #%u fbp %u y %u alpha:", s_n, fbp, y);
                    for (uint32_t x = 0; x < 24; x++) { uint32_t w; std::memcpy(&w, v + pgsAddr32(fbp, 8, x, y, false), 4); std::fprintf(stderr, " %02x", w >> 24); }
                    std::fprintf(stderr, " | rgb x0..7:");
                    for (uint32_t x = 0; x < 8; x++) { uint32_t w; std::memcpy(&w, v + pgsAddr32(fbp, 8, x, y, false), 4); std::fprintf(stderr, " %06x", w & 0xffffffu); }
                    std::fprintf(stderr, " | rgb x8..15:");
                    for (uint32_t x = 8; x < 16; x++) { uint32_t w; std::memcpy(&w, v + pgsAddr32(fbp, 8, x, y, false), 4); std::fprintf(stderr, " %06x", w & 0xffffffu); }
                    std::fprintf(stderr, "\n[vramprobe] #%u zbp %u y %u z words:", s_n, zbp, y);
                    for (uint32_t x = 0; x < 16; x++) { uint32_t w; std::memcpy(&w, v + pgsAddr32(zbp, 8, x, y, true), 4); std::fprintf(stderr, " %08x", w); }
                    std::fprintf(stderr, "\n");
                }
            }
        }
    }
    const double dtMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s.xferMs += dtMs;
    if (dtMs > 0.5)
    {   // [pgs-slow] remember the slowest calls with a sketch of their first GIF tag
        if (dtMs > 1.0) s.slowOver1ms++;
        uint64_t lo = 0, hi = 0; std::memcpy(&lo, data, 8); std::memcpy(&hi, data + 8, 8);
        uint32_t firstAD = 0;
        if (((lo >> 58) & 3u) == 0u && (hi & 0xFu) == 0xEu && size >= 32) { uint64_t a; std::memcpy(&a, data + 24, 8); firstAD = uint32_t(a & 0xFFu); }
        State::SlowCall c{dtMs, uint32_t(size), pathId, uint32_t(lo & 0x7FFFu), uint32_t((lo >> 58) & 3u), uint32_t((lo >> 60) & 0xFu), hi, firstAD};
        int worst = 0; for (int i = 1; i < 3; i++) if (s.slowCalls[i].ms < s.slowCalls[worst].ms) worst = i;
        if (c.ms > s.slowCalls[worst].ms) s.slowCalls[worst] = c;
    }
    s.packets++; s.bytes += size;
    return true;
}

void streamFlip(uint64_t dispfb1)
{   // [displatch] job executed by stage 2 in stream order: the frame this DISPFB1 belongs to is complete here
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.streamDispfb1 = dispfb1; s.haveStreamFlip = true;
    s.streamFlips++;
}

void setInkColor(uint32_t rgb)
{   // [pgsink] the overlay's Ink Color (0xRRGGBB)
    g_inkColor.store(rgb & 0xFFFFFFu, std::memory_order_relaxed);
}
void setInkWidthPct(int pct)
{   // [pgsink] the overlay's Ink Width (25..100 % of a texel)
    if (pct < 25) pct = 25; if (pct > 100) pct = 100;
    g_inkWidthPct.store(pct, std::memory_order_relaxed);
}
void setForceBilinear(bool on)
{   // overlay toggle (Force Filtering): applies to the next primitive
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.hacks.force_bilinear = on;
    if (s.inited) s.iface.set_hacks(s.hacks);
}

void setRegs(GSRegisters *regs)
{
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.signals.regs = regs;
}

void privWrite(uint32_t regOff, uint64_t value, GSRegisters *regs)
{
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.signals.regs = regs;
    if (regOff < 0x200u) s.privHist[regOff >> 4]++;
    if (regOff < 0x1000u) s.privLo[(regOff >> 4) & 0xFFu] = value;
    else s.privHi[((regOff - 0x1000u) >> 4) & 0xFFu] = value;
}

void onSwap()
{
    {   // [pgswait2] onSwap only ever runs on the game thread -- tag it once so GPU blocking is attributable
        // [pgswait2] swapFrame has many callers -- usually the game thread (game_overrides.cpp:4225)
        // but also the GsThread (ps2_memory.cpp:2435). Only tag a thread that has no tag yet, or this
        // relabels GsThread as "game" and the split is a fiction.
        static thread_local bool s_tagged = false;
        if (!s_tagged) { s_tagged = true; ps2x_pgs_set_thread_tag_if_unset(1); }
    }
    if (!enabled()) return;
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!initLocked(s)) return;
    { const int ws = g_wantScale.load(std::memory_order_relaxed); if (ws > 0 && scaleToSsaa(ws) != s.ssaa && !reinitLocked(s, scaleToSsaa(ws))) return; }   // [pgslive]
    const auto t0 = std::chrono::steady_clock::now();
    s.iface.flush();
    { const FlushStats fs = s.iface.consume_flush_stats(); s.fsPrims += fs.num_primitives; s.fsPasses += fs.num_render_passes; s.fsCopies += fs.num_copies; s.fsPal += fs.num_palette_updates; }
    copyPrivLocked(s);
    VSyncInfo info = {};
    info.phase = s.field ^= 1u;
    info.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    info.force_progressive = true;
    info.anti_blur = true;
    info.overscan = false;
    info.crtc_offsets = false;
    static const bool s_adapth = envOn("PS2X_PGS_ADAPTH");
    info.adapt_to_internal_horizontal_resolution = s_adapth;   // default off: scan out at the CRTC width (640) so the present keeps the GL path's aspect
    info.raw_circuit_scanout = true;
    {   // [pgsfit] scanout resolution: PS2X_PGS_HIRES=0|1|2 (1x / 2x / 4x) forces it; unset = AUTO, the smallest shift
        // whose scanout covers the on-screen size of the frame. The present downscales anything larger than the window
        // (2-tap bilinear drops columns at ratios above ~1.5, point sampling drops them at ANY ratio -- that was the
        // "rough edges around characters": a 2560x1792 scanout point-sampled onto 1920x1080). 2x needs SSAA >= 4, 4x needs 16.
        static const int s_hires = [](){ const char *v = std::getenv("PS2X_PGS_HIRES"); return v && v[0] ? std::atoi(v) : -1; }();
        const uint32_t maxShift = s.ssaa >= 16 ? 2u : s.ssaa >= 4 ? 1u : 0u;
        uint32_t shift = 0;
        if (s_hires >= 0) shift = std::min<uint32_t>(uint32_t(s_hires), maxShift);
        else
        {
            const uint32_t dw = g_presentW.load(std::memory_order_relaxed), dh = g_presentH.load(std::memory_order_relaxed);
            const uint32_t bw = s.baseW ? s.baseW : 640u, bh = s.baseH ? s.baseH : 448u;
            while (shift < maxShift && ((bw << shift) < dw || (bh << shift) < dh)) shift++;
        }
        info.high_resolution_scanout = shift >= 1;
        info.high_resolution_scanout_shift = shift >= 2 ? 2u : 1u;
        s.lastShift = shift;
    }
    ScanoutResult res = s.iface.vsync(info);
    if (res.image) { s.baseW = res.image->get_width() >> res.high_resolution_shift; s.baseH = res.image->get_height() >> res.high_resolution_shift; }
    const auto t1 = std::chrono::steady_clock::now();
    static const bool s_noReadback = envOn("PS2X_PGS_NOREADBACK");   // isolation: skip the sync scanout readback (nothing presented)
    if (res.image && !s_noReadback) readbackLocked(s, res); else if (!res.image) s.noImage++;
    const auto t2 = std::chrono::steady_clock::now();
    s.vsyncMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.readbackMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
    s.swaps++;
    const double dt = std::chrono::duration<double>(t2 - s.tStat).count();
    if (dt >= 5.0)
    {
        std::fprintf(stderr, "[pgs] %.1f swaps/s, %.0f packets/s, %.1f MB/s, scanout %ux%u shift %u (no image %llu), flush+vsync %.2f ms/swap, readback %.2f ms/swap | pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx\n",
                     s.swaps / dt, s.packets / dt, s.bytes / dt / 1048576.0, s.frameW, s.frameH, s.lastShift, (unsigned long long)s.noImage,
                     s.swaps ? s.vsyncMs / s.swaps : 0.0, s.swaps ? s.readbackMs / s.swaps : 0.0,
                     (unsigned long long)s.privLo[0], (unsigned long long)s.privLo[2], (unsigned long long)s.privLo[7], (unsigned long long)s.privLo[8],
                     (unsigned long long)s.privLo[9], (unsigned long long)s.privLo[10]);
        std::fprintf(stderr, "[pgs] cpu gif_transfer %.1f ms/s (%.2f ms/swap) | priv writes/s:", s.xferMs / dt, s.swaps ? s.xferMs / s.swaps : 0.0);
        for (int k = 0; k < 0x20; k++) if (s.privHist[k]) { std::fprintf(stderr, " %02x=%.0f", k << 4, s.privHist[k] / dt); s.privHist[k] = 0; }
        std::fprintf(stderr, " pseudo=%.0f", s.pseudoSeen / dt); s.pseudoSeen = 0;
        std::fprintf(stderr, " | wshud: inv %.3f (present %.3f) active %d prims/s %.0f verts/s %.0f", s.wshud.lastInv, g_ps2xWsHudInv, s.wshud.active ? 1 : 0, s.wshud.hudPrims / dt, s.wshud.mappedVerts / dt); std::fprintf(stderr, " scissors/s %.0f splits/s %.0f | ink: outline %d strength %d%% width %d%% dropped/s %.0f scaled/s %.0f shifted/s %.0f | fx: shadows %d dof %d dropped/s %.0f/%.0f masks/s %.0f bloom/s %.0f restores/s %.0f", s.wshud.scissorsMapped / dt, s.wshud.splitPrims / dt, GsGpuRenderer::outlineEnabled() ? 1 : 0, GsGpuRenderer::inkStrengthPct(), g_inkWidthPct.load(), s.wshud.inkDropped / dt, s.wshud.inkScaled / dt, s.wshud.inkShifted / dt, GsGpuRenderer::shadowsEnabled() ? 1 : 0, GsGpuRenderer::dofBlurEnabled() ? 1 : 0, s.wshud.shadowDropped / dt, s.wshud.dofDropped / dt, s.wshud.maskNeutralized / dt, s.wshud.bloomEdits / dt, s.wshud.bloomRestores / dt); s.wshud.bloomEdits = s.wshud.bloomRestores = 0; s.wshud.hudPrims = s.wshud.mappedVerts = s.wshud.scissorsMapped = s.wshud.splitPrims = s.wshud.inkDropped = s.wshud.inkScaled = s.wshud.inkShifted = s.wshud.shadowDropped = s.wshud.dofDropped = s.wshud.maskNeutralized = 0;
        if (packMode()) { std::fprintf(stderr, " | pack: hits %llu misses %llu skipped %llu rtstale %llu gate %llu cached %zu", (unsigned long long)s.replacer.hits, (unsigned long long)s.replacer.misses, (unsigned long long)s.replacer.skipped, (unsigned long long)s.replacer.rtstale, (unsigned long long)s.replacer.gateSkips, s.replacer.cache.size()); s.replacer.hits = s.replacer.misses = s.replacer.skipped = s.replacer.rtstale = s.replacer.gateSkips = 0; }
        {   // per swap: paraLLEl-GS render passes / copies / palette updates / primitives (consume_flush_stats)
            const double sw = s.swaps ? double(s.swaps) : 1.0;
            std::fprintf(stderr, " | per swap: passes %.1f copies %.1f pal %.1f prims %.0f", s.fsPasses / sw, s.fsCopies / sw, s.fsPal / sw, s.fsPrims / sw);
            s.fsPrims = s.fsPasses = s.fsCopies = s.fsPal = 0;
        }
        {   // [pgs-slow]
            std::fprintf(stderr, " | calls>1ms/s %.0f, slowest:", s.slowOver1ms / dt);
            for (int i = 0; i < 3; i++) if (s.slowCalls[i].ms > 0.0)
                std::fprintf(stderr, " [%.2fms path%u %uB nloop=%u flg=%u nreg=%u regs=%llx firstAD=0x%x]", s.slowCalls[i].ms, s.slowCalls[i].path, s.slowCalls[i].size,
                             s.slowCalls[i].nloop, s.slowCalls[i].flg, s.slowCalls[i].nreg, (unsigned long long)s.slowCalls[i].regs, s.slowCalls[i].firstAD);
            for (auto &c : s.slowCalls) c = State::SlowCall{}; s.slowOver1ms = 0;
        }
        {   // [pgswait] is the cpu gif_transfer time above real CPU work, or blocking on the GPU?
            static unsigned long long pw = 0, pc = 0, pb = 0, pi = 0, pic = 0;
            static unsigned long long pfw = 0, pfc = 0, pfb = 0, psw = 0, psc = 0;
            const unsigned long long w = g_pgsFenceWaitNs.load(std::memory_order_relaxed);
            const unsigned long long c = g_pgsFenceWaitCalls.load(std::memory_order_relaxed);
            const unsigned long long b = g_pgsFenceWaitBlocked.load(std::memory_order_relaxed);
            const unsigned long long i = g_pgsIdleNs.load(std::memory_order_relaxed);
            const unsigned long long ic = g_pgsIdleCalls.load(std::memory_order_relaxed);
            const unsigned long long fw = g_pgsFrameWaitNs.load(std::memory_order_relaxed);
            const unsigned long long fc = g_pgsFrameWaitCalls.load(std::memory_order_relaxed);
            const unsigned long long fb = g_pgsFrameWaitBlocked.load(std::memory_order_relaxed);
            const unsigned long long sw2 = g_pgsSemWaitNs.load(std::memory_order_relaxed);
            const unsigned long long sc = g_pgsSemWaitCalls.load(std::memory_order_relaxed);
            std::fprintf(stderr, " | gpuwait: FRAMECTX %.1f ms/s (%.0f calls/s, %.0f blocked/s) timeline %.1f ms/s (%.0f calls/s) fence %.1f ms/s idle %.1f ms/s quirk %llu",
                         double(fw - pfw) / 1e6 / dt, double(fc - pfc) / dt, double(fb - pfb) / dt,
                         double(sw2 - psw) / 1e6 / dt, double(sc - psc) / dt,
                         double(w - pw) / 1e6 / dt, double(i - pi) / 1e6 / dt,
                         (unsigned long long) g_pgsQueueIdleCalls.load(std::memory_order_relaxed));
            pw = w; pc = c; pb = b; pi = i; pic = ic; pfw = fw; pfc = fc; pfb = fb; psw = sw2; psc = sc;
            if (gsProfOn())
            {   // [gsprof] where the gifTransfer body goes
                static unsigned long long pp = 0, pw = 0, pb = 0, pc = 0;
                const unsigned long long a1 = g_gsProfPseudoNs.load(), b1 = g_gsProfWsHudNs.load(),
                                         c1 = g_gsProfBackendNs.load(), d1 = g_gsProfCalls.load();
                static unsigned long long po = 0, poc = 0;
                const unsigned long long o1 = g_gsProfOursNs.load(), o2 = g_gsProfOursCalls.load();
                std::fprintf(stderr, " | gsprof ms/s: pseudoRegs %.1f wsHud %.1f backendParse %.1f (%.0f calls/s) ourParse %.1f (%.0f calls/s)",
                             double(a1 - pp) / 1e6 / dt, double(b1 - pw) / 1e6 / dt,
                             double(c1 - pb) / 1e6 / dt, double(d1 - pc) / dt,
                             double(o1 - po) / 1e6 / dt, double(o2 - poc) / dt);
                pp = a1; pw = b1; pb = c1; pc = d1; po = o1; poc = o2;
            }
            {   // [pgswait2] the same blocking, split by thread: WHICH unit of the pipeline is stalling?
                static unsigned long long pby[4][3] = {};
                static const char *kTag[4] = { "other", "game", "kick", "gs" };
                std::fprintf(stderr, " | gpuwait-by-thread ms/s:");
                for (int t = 0; t < 4; t++)
                {
                    double tot = 0.0; double per[3];
                    for (int f = 0; f < 3; f++)
                    {
                        const unsigned long long v = g_pgsWaitNsBy[t][f].load(std::memory_order_relaxed);
                        per[f] = double(v - pby[t][f]) / 1e6 / dt; pby[t][f] = v; tot += per[f];
                    }
                    if (tot >= 0.05) std::fprintf(stderr, " %s=%.1f(fence %.1f tl %.1f fctx %.1f)", kTag[t], tot, per[0], per[1], per[2]);
                }
            }
        }
        std::fprintf(stderr, " | flip: stream calls/s %.0f, scanout fb1=%llx live1=%llx live2=%llx, stream!=live at %.0f%% of swaps",
                     s.streamFlips / dt, (unsigned long long)s.lastFb1, (unsigned long long)s.lastLive1, (unsigned long long)s.lastLive2,
                     s.swaps ? 100.0 * s.flipMismatch / s.swaps : 0.0);
        s.streamFlips = 0; s.flipMismatch = 0;
        if (s.timestamps)
        {
            static const char *const names[int(TimestampType::Count)] = { "SyncHostToVRAM", "CopyVRAM", "PaletteUpdate", "TextureUpload", "TriangleSetup", "Binning", "Shading", "Readback", "VSync" };
            static double last[int(TimestampType::Count)] = {};
            double total = 0.0;
            std::fprintf(stderr, " | gpu ms/s:");
            for (int t = 0; t < int(TimestampType::Count); t++)
            {
                const double acc = s.iface.get_accumulated_timestamps(TimestampType(t)) * 1e3;
                const double ms = (acc - last[t]) / dt; last[t] = acc; total += ms;
                std::fprintf(stderr, " %s=%.1f", names[t], ms);
            }
            std::fprintf(stderr, " total=%.1f (%.2f ms/swap)", total, s.swaps ? total * dt / s.swaps : 0.0);
        }
        std::fprintf(stderr, "\n");
        s.swaps = s.packets = s.bytes = 0; s.noImage = 0; s.vsyncMs = s.readbackMs = s.xferMs = 0.0; s.tStat = t2;
    }
}

void setPresentSize(uint32_t w, uint32_t h)
{   // [pgsfit]
    g_presentW.store(w, std::memory_order_relaxed); g_presentH.store(h, std::memory_order_relaxed);
}

bool takeFrame(std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h)
{
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.frameFresh) return false;
    rgba.swap(s.frame); s.frame.clear();
    w = s.frameW; h = s.frameH; s.frameFresh = false;
    return true;
}

void shutdown()
{
    State &s = st();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.inited) s.device.wait_idle();
}
}
