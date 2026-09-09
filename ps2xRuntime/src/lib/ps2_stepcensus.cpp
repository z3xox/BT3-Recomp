// [stepcensus] PS2X_STEPCENSUS=<out.csv>: classify every recompiled STORE SITE by how it advances guest memory
// from frame to frame. The 60 fps question ("which per-frame quantities would have to advance by half?") cannot
// be answered statically -- the fight has ~2500 accumulate-in-place sites -- but it can be answered by watching a
// real fight: for each store PC we track whether the value it overwrites is the value the same site wrote to that
// address last time (an accumulator chain), and whether the delta is constant (a counter / timer / velocity).
// Recording is restricted to the thread that runs the per-frame render kick (the game's main thread), keyed on
// g_bt3FrameCount. The table is a direct-mapped array over the EE code range, so the per-store cost is a few
// loads; expect the guest to run several times slower while it is on.
#include "ps2_runtime.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

std::atomic<int> g_ps2StepCensus{0};
extern std::atomic<uint64_t> g_bt3FrameCount;

namespace
{
    struct Site
    {
        uint32_t count = 0, framesSeen = 0, lastFrame = 0xFFFFFFFFu, sizeMask = 0;
        uint32_t addrLo = 0xFFFFFFFFu, addrHi = 0;
        uint32_t ringAddr[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t ringVal[4] = {};
        uint8_t ringPos = 0;
        uint32_t chain = 0;      // old value == what this site last wrote to that address (accumulator)
        uint32_t fplaus = 0;     // both old and new plausible floats
        float dMode = 0.f; uint32_t dModeN = 0, dOtherN = 0;     // float delta: first-seen mode and its share
        int32_t iMode = 0; uint32_t iModeN = 0, iOtherN = 0;     // integer delta
        uint32_t zeroDelta = 0;  // new == old (rewrite of the same value)
    };
    constexpr uint32_t kBase = 0x100000u, kEnd = 0x340000u;
    Site *g_sites = nullptr;
    std::atomic<const R5900Context *> g_ctx{nullptr};
    std::string g_out;
    std::mutex g_dumpMtx;
    uint64_t g_lastDumpFrame = 0;

    inline bool plausibleFloat(uint32_t bits, float &f)
    {
        std::memcpy(&f, &bits, 4);
        if (!std::isfinite(f)) return false;
        const float a = std::fabs(f);
        return bits == 0u || (a >= 1e-6f && a <= 1e6f);
    }
    inline uint32_t readOld(const uint8_t *rdram, uint32_t a, uint32_t size)
    {
        uint32_t v = 0;
        if (size == 1) v = rdram[a];
        else if (size == 2) { uint16_t h; std::memcpy(&h, rdram + a, 2); v = h; }
        else std::memcpy(&v, rdram + a, 4);
        return v;
    }
    inline int32_t sext(uint32_t v, uint32_t size)
    {
        if (size == 1) return (int8_t)v;
        if (size == 2) return (int16_t)v;
        return (int32_t)v;
    }
}

void ps2StepCensusStore(uint8_t *rdram, uint32_t guestAddr, uint32_t size, uint64_t valueLo, const R5900Context *ctx)
{
    if (!g_sites || !rdram || !ctx || ctx != g_ctx.load(std::memory_order_relaxed)) return;
    if (size != 1u && size != 2u && size != 4u) return;
    const uint32_t pc = ctx->pc;
    if (pc < kBase || pc >= kEnd) return;
    const uint32_t a = guestAddr & 0x1FFFFFFFu;
    if (a + size > 32u * 1024u * 1024u) return;
    Site &s = g_sites[(pc - kBase) >> 2];
    const uint32_t old = readOld(rdram, a, size);
    const uint32_t nw = size == 4 ? (uint32_t)valueLo : size == 2 ? (uint32_t)(valueLo & 0xFFFFu) : (uint32_t)(valueLo & 0xFFu);
    const uint32_t frame = (uint32_t)g_bt3FrameCount.load(std::memory_order_relaxed);
    s.count++; s.sizeMask |= size;
    if (frame != s.lastFrame) { s.lastFrame = frame; s.framesSeen++; }
    if (a < s.addrLo) s.addrLo = a;
    if (a > s.addrHi) s.addrHi = a;
    // accumulator chain: did this site write the value we are overwriting?
    int slot = -1;
    for (int i = 0; i < 4; ++i) if (s.ringAddr[i] == a) { slot = i; break; }
    if (slot >= 0) { if (s.ringVal[slot] == old) s.chain++; s.ringVal[slot] = nw; }
    else { slot = s.ringPos; s.ringPos = (uint8_t)((s.ringPos + 1u) & 3u); s.ringAddr[slot] = a; s.ringVal[slot] = nw; }
    if (nw == old) { s.zeroDelta++; return; }
    if (size == 4u)
    {
        float fo, fn;
        if (plausibleFloat(old, fo) && plausibleFloat(nw, fn))
        {
            s.fplaus++;
            const float d = fn - fo;
            if (s.dModeN == 0u) { s.dMode = d; s.dModeN = 1u; }
            else if (std::fabs(d - s.dMode) <= 1e-5f * std::fmax(1.f, std::fabs(s.dMode))) s.dModeN++;
            else s.dOtherN++;
        }
    }
    const int32_t di = sext(nw, size) - sext(old, size);
    if (s.iModeN == 0u) { s.iMode = di; s.iModeN = 1u; }
    else if (di == s.iMode) s.iModeN++;
    else s.iOtherN++;
}

void ps2StepCensusDump()
{
    if (!g_sites || g_out.empty()) return;
    std::lock_guard<std::mutex> lk(g_dumpMtx);
    FILE *f = std::fopen(g_out.c_str(), "w");
    if (!f) { std::fprintf(stderr, "[stepcensus] cannot write %s\n", g_out.c_str()); return; }
    std::fprintf(f, "pc,count,frames,per_frame,size,addr_lo,addr_hi,chain_pct,same_pct,fplaus_pct,fdelta,fdelta_pct,idelta,idelta_pct\n");
    uint32_t n = 0;
    const uint32_t total = (kEnd - kBase) >> 2;
    for (uint32_t i = 0; i < total; ++i)
    {
        const Site &s = g_sites[i];
        if (s.count < 20u || s.framesSeen < 5u) continue;
        const double c = s.count;
        const double nz = c - s.zeroDelta;
        std::fprintf(f, "0x%06x,%u,%u,%.2f,%u,0x%x,0x%x,%.0f,%.0f,%.0f,%.6g,%.0f,%d,%.0f\n",
                     kBase + i * 4u, s.count, s.framesSeen, c / (double)s.framesSeen, s.sizeMask, s.addrLo, s.addrHi,
                     100.0 * s.chain / c, 100.0 * s.zeroDelta / c, nz > 0 ? 100.0 * s.fplaus / nz : 0.0,
                     (double)s.dMode, s.fplaus ? 100.0 * s.dModeN / (double)s.fplaus : 0.0,
                     s.iMode, nz > 0 ? 100.0 * s.iModeN / nz : 0.0);
        ++n;
    }
    std::fclose(f);
    std::fprintf(stderr, "[stepcensus] wrote %u store sites to %s (frame %llu)\n", n, g_out.c_str(), (unsigned long long)g_bt3FrameCount.load());
}

void ps2StepCensusEnable(const char *outPath)
{
    if (!outPath || !outPath[0]) return;
    g_out = outPath;
    g_sites = new Site[(kEnd - kBase) >> 2];
    std::atexit([]() { ps2StepCensusDump(); });
    g_ps2StepCensus.store(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[stepcensus] store census ON -> %s (%.0f MB table; the main thread's stores only; dump every 600 render frames + at exit)\n",
                 outPath, (double)sizeof(Site) * ((kEnd - kBase) >> 2) / 1e6);
}

void ps2StepCensusFrame(const R5900Context *ctx)
{
    if (!g_sites) return;
    if (!g_ctx.load(std::memory_order_relaxed)) { g_ctx.store(ctx); std::fprintf(stderr, "[stepcensus] recording thread bound at frame %llu\n", (unsigned long long)g_bt3FrameCount.load()); }
    const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
    if (fr - g_lastDumpFrame >= 600u) { g_lastDumpFrame = fr; ps2StepCensusDump(); }
}

// ---------------------------------------------------------------------------------------------------------------
// [halfstep] the experiment the census exists for. sites.txt lines: "<pc-hex> f|i" (f = float accumulator: store
// old + (new-old)/2; i = integer counter: apply the store on even render frames only, keep the old value on odd
// ones). Only the render-kick thread's stores are touched; everything else is untouched.
std::atomic<int> g_ps2HalfStep{0};          // the macros' switch: raised by ps2HalfStepFrame only on fight frames
static std::atomic<int> g_hsEnabled{0};     // the experiment is configured
std::atomic<uint64_t> g_ps2HalfStepLogicFrame{0};
namespace
{
    uint8_t *g_hs = nullptr;                      // 0 none, 1 float-halve, 2 int-every-other-frame
    struct HsRing { uint32_t addr[8]; uint32_t frame[8]; uint8_t pos; };   // per site: last frame per address
    HsRing *g_hsLast = nullptr;                   // one-shot guard, keyed per (site, address)
    std::atomic<uint64_t> g_hsOneShot{0};
    std::atomic<const R5900Context *> g_hsCtx{nullptr};
    std::atomic<uint64_t> g_hsFloat{0}, g_hsIntSkip{0}, g_hsIntPass{0};
    uint64_t g_hsLastReport = 0;
}
uint32_t ps2HalfStepWrite(uint8_t *rdram, uint32_t guestAddr, uint32_t size, uint32_t value, const R5900Context *ctx)
{
    if (!g_hs || !ctx || ctx != g_hsCtx.load(std::memory_order_relaxed)) return value;
    const uint32_t pc = ctx->pc;
    if (pc < kBase || pc >= kEnd) return value;
    const uint8_t k = g_hs[(pc - kBase) >> 2];
    if (!k) return value;
    const uint32_t a = guestAddr & 0x1FFFFFFFu;
    if (a + size > 32u * 1024u * 1024u) return value;
    // Gate: only frames on which the fight update ran. The loader, the fight intro and the menus run per-frame
    // counters on the same thread, and halving those crashed the loader (half3: wild jump to 0x10000000).
    const uint32_t frame = (uint32_t)g_bt3FrameCount.load(std::memory_order_relaxed);
    if (frame - (uint32_t)g_ps2HalfStepLogicFrame.load(std::memory_order_relaxed) > 2u) return value;
    const uint32_t old = readOld(rdram, a, size);
    if (old == value) return value;
    // One-shot guard: a per-frame quantity is advanced on consecutive frames. A store at a listed site after a gap
    // of more than one frame is a one-shot event (a flag set, a state advance, a timer armed) and must land
    // unmodified: skipping it on an odd frame LOSES it (the half2 freeze: the fight intro polled a flag whose
    // one increment fell on an odd frame). The first tick after a gap therefore passes through at full rate.
    HsRing &rg = g_hsLast[(pc - kBase) >> 2];
    int slot = -1;
    for (int i = 0; i < 8; ++i) if (rg.addr[i] == a) { slot = i; break; }
    uint32_t last = 0xFFFF0000u;
    if (slot >= 0) { last = rg.frame[slot]; rg.frame[slot] = frame; }
    else { slot = rg.pos; rg.pos = (uint8_t)((rg.pos + 1u) & 7u); rg.addr[slot] = a; rg.frame[slot] = frame; }
    // an address this site did not touch on the previous frame (or never): a one-shot event, land it unmodified
    if (frame - last > 1u) { g_hsOneShot.fetch_add(1, std::memory_order_relaxed); return value; }
    if (k == 1)
    {
        if (size != 4u) return value;
        float fo, fn;
        if (!plausibleFloat(old, fo) || !plausibleFloat(value, fn)) return value;
        const float h = fo + (fn - fo) * 0.5f;
        uint32_t bits; std::memcpy(&bits, &h, 4);
        g_hsFloat.fetch_add(1, std::memory_order_relaxed);
        return bits;
    }
    // integer counter: keep the old value on odd render frames
    if (frame & 1u) { g_hsIntSkip.fetch_add(1, std::memory_order_relaxed); return old; }
    g_hsIntPass.fetch_add(1, std::memory_order_relaxed);
    return value;
}
void ps2HalfStepEnable(const char *sitesPath)
{
    FILE *f = std::fopen(sitesPath, "r");
    if (!f) { std::fprintf(stderr, "[halfstep] cannot read %s\n", sitesPath); return; }
    g_hs = new uint8_t[(kEnd - kBase) >> 2]();
    g_hsLast = new HsRing[(kEnd - kBase) >> 2];
    for (uint32_t i = 0; i < ((kEnd - kBase) >> 2); ++i) { for (int j = 0; j < 8; ++j) { g_hsLast[i].addr[j] = 0xFFFFFFFFu; g_hsLast[i].frame[j] = 0xFFFF0000u; } g_hsLast[i].pos = 0; }
    char line[128]; unsigned nf = 0, ni = 0;
    while (std::fgets(line, sizeof line, f))
    {
        unsigned pc = 0; char kind = 0;
        if (std::sscanf(line, "%x %c", &pc, &kind) != 2 || pc < kBase || pc >= kEnd) continue;
        if (kind == 'f') { g_hs[(pc - kBase) >> 2] = 1; ++nf; }
        else if (kind == 'i') { g_hs[(pc - kBase) >> 2] = 2; ++ni; }
    }
    std::fclose(f);
    g_hsEnabled.store(1, std::memory_order_relaxed);   // the macro switch itself is raised per fight frame (zero cost elsewhere)
    std::fprintf(stderr, "[halfstep] ON: %u float sites halved, %u integer sites on even frames only (%s)\n", nf, ni, sitesPath);
}
void ps2HalfStepFrame(const R5900Context *ctx)
{
    if (!g_hs || !g_hsEnabled.load(std::memory_order_relaxed)) return;
    if (!g_hsCtx.load(std::memory_order_relaxed)) g_hsCtx.store(ctx);
    const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
    // raise the macros' switch only while the fight update is running (last update within 2 render frames), so the
    // loader / intro / menus run the untouched fast path -- not even the hook call (half4 froze in the loader with
    // every modification gated off: the per-store call overhead alone shifts the loader's timing)
    const bool active = fr - g_ps2HalfStepLogicFrame.load(std::memory_order_relaxed) <= 2u;
    g_ps2HalfStep.store(active ? 1 : 0, std::memory_order_relaxed);
    if (fr - g_hsLastReport >= 600u)
    {
        g_hsLastReport = fr;
        std::fprintf(stderr, "[halfstep] frame %llu: float halved %llu, int skipped %llu / passed %llu, one-shot passthrough %llu\n", (unsigned long long)fr,
                     (unsigned long long)g_hsFloat.exchange(0), (unsigned long long)g_hsIntSkip.exchange(0), (unsigned long long)g_hsIntPass.exchange(0),
                     (unsigned long long)g_hsOneShot.exchange(0));
    }
}
