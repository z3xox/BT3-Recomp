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
#include <unordered_map>
#include <unordered_set>

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
    // vector (sqc2) stores keyed by (pc, ra): the store sites are shared math helpers, the caller tells them apart
    std::unordered_map<uint64_t, Site> g_vecSites;
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

// [addrwatch] PS2X_ADDRWATCH=<hex addr>: print every store (any size, any thread) that covers that address once the
// fight is underway -- pc, ra, old and new value -- so the writer of one particular slot can be named.
bool ps2HalfStepFightActive();   // defined below
static uint32_t g_addrWatch = 0; static std::atomic<uint32_t> g_addrWatchN{0};
static uint32_t g_addrWatchList[6] = {0}; static int g_addrWatchCnt = 0;
// PS2X_ADDRWATCH_TRIG=<hex addr>:<delta>: hold the log until the float at addr has moved more than delta from its
// value when the fight gate opened -- so the 500-line budget per slot lands on the move under study (a takeoff,
// a dash) instead of the idle frames before it
static uint32_t g_awTrigAddr = 0; static float g_awTrigDelta = 0.f; static uint32_t g_awTrigAddr2 = 0;
static void parseTrig()
{
    const char *tr = std::getenv("PS2X_ADDRWATCH_TRIG"); if (!tr || !tr[0] || g_awTrigAddr) return;
    char *end = nullptr; g_awTrigAddr = (uint32_t)std::strtoul(tr, &end, 16) & 0x1FFFFFFFu;
    g_awTrigDelta = (end && *end == ':') ? std::strtof(end + 1, &end) : 1.f;
    if (end && *end == ',') g_awTrigAddr2 = (uint32_t)std::strtoul(end + 1, nullptr, 16) & 0x1FFFFFFFu;   // "addr:delta,addr2": either slot fires it
}
// [storetrace] PS2X_STORETRACE=<hex pc lo>:<hex pc hi>[,<lo>:<hi>..][;frames]: once the trigger has fired, log EVERY
// store whose pc lies in one of the ranges for the next N frames (default 40, cap 40000 lines) -- a module's whole
// per-frame update (e.g. the camera) in one run, so its persistent slots and their writers can be read off
static uint32_t g_stRange[8][2]; static int g_stRanges = 0; static uint32_t g_stFrames = 40; static std::atomic<uint32_t> g_stLines{0};
// PS2X_STORETRACE_ADDR=<hex lo>:<hex hi>: additionally log every store (from ANY pc) into that address range after the
// trigger -- the writers of one struct when its module is unknown
static uint32_t g_stAddrLo = 0, g_stAddrHi = 0, g_stAddrLo2 = 0, g_stAddrHi2 = 0;   // up to two ranges: lo:hi[,lo:hi]
void ps2StoreTraceEnable(const char *spec)
{
    std::string t(spec); size_t semi = t.find(';');
    if (semi != std::string::npos) { g_stFrames = (uint32_t)std::strtoul(t.c_str() + semi + 1, nullptr, 10); t = t.substr(0, semi); }
    size_t p0 = 0;
    while (p0 < t.size() && g_stRanges < 8)
    {
        size_t p1 = t.find(',', p0); if (p1 == std::string::npos) p1 = t.size();
        std::string r = t.substr(p0, p1 - p0); size_t c = r.find(':');
        if (c != std::string::npos) { g_stRange[g_stRanges][0] = (uint32_t)std::strtoul(r.c_str(), nullptr, 16); g_stRange[g_stRanges][1] = (uint32_t)std::strtoul(r.c_str() + c + 1, nullptr, 16); ++g_stRanges; }
        p0 = p1 + 1;
    }
    if (const char *ar = std::getenv("PS2X_STORETRACE_ADDR"); ar && ar[0])
    {
        char *end = nullptr; g_stAddrLo = (uint32_t)std::strtoul(ar, &end, 16) & 0x1FFFFFFFu;
        if (end && *end == ':') g_stAddrHi = (uint32_t)std::strtoul(end + 1, &end, 16) & 0x1FFFFFFFu;
        if (end && *end == ',') { g_stAddrLo2 = (uint32_t)std::strtoul(end + 1, &end, 16) & 0x1FFFFFFFu; if (end && *end == ':') g_stAddrHi2 = (uint32_t)std::strtoul(end + 1, nullptr, 16) & 0x1FFFFFFFu; }
        std::fprintf(stderr, "[storetrace] address range 0x%x..0x%x + 0x%x..0x%x (any pc)\n", g_stAddrLo, g_stAddrHi, g_stAddrLo2, g_stAddrHi2);
    }
    parseTrig(); g_ps2StepCensus.store(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[storetrace] ON: %d pc range(s), %u frames after the trigger\n", g_stRanges, g_stFrames);
}
void ps2AddrWatchEnable(const char *list)
{   // comma-separated hex addresses (up to 3 requested; helper sources fill the remaining slots)
    std::string t(list); size_t p0 = 0;
    while (p0 < t.size() && g_addrWatchCnt < 3) { size_t p1 = t.find(',', p0); if (p1 == std::string::npos) p1 = t.size(); if (p1 > p0) g_addrWatchList[g_addrWatchCnt++] = (uint32_t)std::strtoul(t.substr(p0, p1 - p0).c_str(), nullptr, 16) & 0x1FFFFFFFu; p0 = p1 + 1; }
    g_addrWatch = g_addrWatchList[0]; g_ps2StepCensus.store(1, std::memory_order_relaxed);
    parseTrig();
    std::fprintf(stderr, "[addrwatch] ON %d slot(s), first 0x%x; logging CHANGING stores only%s\n", g_addrWatchCnt, g_addrWatch, g_awTrigAddr ? " (held until the trigger slot moves)" : "");
}
void ps2StepCensusStore(uint8_t *rdram, uint32_t guestAddr, uint32_t size, uint64_t valueLo, uint64_t valueHi, const R5900Context *ctx)
{
    if ((g_addrWatch || g_stRanges) && rdram && ctx)
    {
        // watch list: slot 0 = the requested address; a store by a vector-library helper (0x120000..0x122400,
        // dst=$a0 src=$a1) that covers a watched slot adds src+off, up to 6 levels -- the chain from a displayed
        // position back to the site that integrates it. Each slot logs up to 500 stores.
        static uint32_t s_w[6] = {0}; static uint32_t s_n[6] = {0}; static int s_cnt = 0;
        if (s_cnt == 0) { for (int k = 0; k < g_addrWatchCnt; ++k) s_w[k] = g_addrWatchList[k]; s_cnt = g_addrWatchCnt; }
        const uint32_t a = guestAddr & 0x1FFFFFFFu;
        const bool active = ps2HalfStepFightActive();
        static bool s_trig = (g_awTrigAddr == 0); static bool s_haveBase = false; static float s_base[2] = {0.f, 0.f};
        if (!s_trig && active)
        {
            const uint32_t ta[2] = { g_awTrigAddr, g_awTrigAddr2 };
            for (int t = 0; t < 2 && ta[t]; ++t)
            {
                float cur; std::memcpy(&cur, rdram + ta[t], 4);
                if (!s_haveBase) { s_base[t] = cur; continue; }
                if (std::isfinite(cur) && std::fabs(cur - s_base[t]) > g_awTrigDelta)
                { s_trig = true; std::fprintf(stderr, "[addrwatch] TRIGGERED: slot 0x%x moved %g -> %g at frame %llu\n", ta[t], s_base[t], cur, (unsigned long long)g_bt3FrameCount.load()); break; }
            }
            s_haveBase = true;
        }
        static uint32_t s_trigFrame = 0; static bool s_trigNoted = false;
        if (s_trig && !s_trigNoted) { s_trigNoted = true; s_trigFrame = (uint32_t)g_bt3FrameCount.load(); }
        if (s_trig && g_stRanges && active)
        {
            const uint32_t fr = (uint32_t)g_bt3FrameCount.load();
            // a store counts when its pc OR its caller ($ra) lies in a range: the game's vector library
            // (0x120000..0x122400) does most of a module's persistent stores on its behalf. Loops are capped at
            // 8 lines per pc per frame so matrix math cannot eat the budget (1100 stores/frame from one pc seen).
            const uint32_t ra = (uint32_t)ctx->r[31][0];
            static uint32_t s_capFrame = 0; static std::unordered_map<uint32_t, uint32_t> s_perPc;
            if (fr != s_capFrame) { s_capFrame = fr; s_perPc.clear(); }
            if (fr - s_trigFrame < g_stFrames)
                for (int r = 0; r < g_stRanges; ++r)
                    if ((ctx->pc >= g_stRange[r][0] && ctx->pc < g_stRange[r][1]) || (ra >= g_stRange[r][0] && ra < g_stRange[r][1])
                        || (g_stAddrHi && a + size > g_stAddrLo && a < g_stAddrHi) || (g_stAddrHi2 && a + size > g_stAddrLo2 && a < g_stAddrHi2))
                    {
                        if (s_perPc[ctx->pc ^ (ra << 8)]++ < 8u && g_stLines.fetch_add(1, std::memory_order_relaxed) < 60000u)
                        {
                            uint32_t ov = 0; std::memcpy(&ov, rdram + a, size < 4 ? size : 4); float fo, fn; const uint32_t nv = (uint32_t)valueLo;
                            std::memcpy(&fo, &ov, 4); std::memcpy(&fn, &nv, 4);
                            std::fprintf(stderr, "[storetrace] f=%u pc=0x%06x ra=0x%06x a=0x%x sz=%u old=0x%x(%g) new=0x%x(%g) a0=0x%x a1=0x%x a2=0x%x\n", fr, ctx->pc, (uint32_t)ctx->r[31][0], a, size, ov, fo, nv, fn, (uint32_t)ctx->r[4][0], (uint32_t)ctx->r[5][0], (uint32_t)ctx->r[6][0]);
                        }
                        break;
                    }
        }
        for (int k = 0; s_trig && g_addrWatch && k < s_cnt; ++k)
        {
            const uint32_t w = s_w[k];
            if (!(a <= w && w < a + size) || !active) continue;
            const uint32_t off = w - a; uint32_t oldv, nv;
            std::memcpy(&oldv, rdram + w, 4);
            if (size == 16) nv = off < 8 ? (uint32_t)(valueLo >> (8 * off)) : (uint32_t)(valueHi >> (8 * (off - 8)));
            else nv = (uint32_t)(valueLo >> (8 * off));
            float fo, fn; std::memcpy(&fo, &oldv, 4); std::memcpy(&fn, &nv, 4);
            const uint32_t a0 = (uint32_t)ctx->r[4][0], a1 = (uint32_t)ctx->r[5][0], a2 = (uint32_t)ctx->r[6][0];
            const bool changing = std::fabs(fn - fo) > 0.01f || (!std::isfinite(fn) != !std::isfinite(fo));
            if (changing && s_n[k]++ < 500u)
                std::fprintf(stderr, "[addrwatch%d] slot=0x%x pc=0x%06x ra=0x%06x size=%u old=%g new=%g a0=0x%x a1=0x%x a2=0x%x frame=%llu\n", k, w, ctx->pc, (uint32_t)ctx->r[31][0], size, fo, fn, a0, a1, a2, (unsigned long long)g_bt3FrameCount.load());
            if (size == 16 && ctx->pc >= 0x120000u && ctx->pc < 0x122400u && (a0 & 0x1FFFFFFFu) == a && s_cnt < 6 && k < g_addrWatchCnt)
            {   // follow $a1 (the source) of a vector helper, one level below the requested slots only
                const uint32_t srcs[1] = { ((a1 & 0x1FFFFFFFu) + off) & 0x1FFFFFFFu };
                for (uint32_t src : srcs)
                {
                    if (src < 0x100000u || src >= 32u * 1024u * 1024u) continue;
                    bool have = false; for (int m = 0; m < s_cnt; ++m) if (s_w[m] == src) have = true;
                    if (!have && s_cnt < 6) { s_w[s_cnt++] = src; std::fprintf(stderr, "[addrwatch] helper 0x%06x (from 0x%06x): also watching source slot 0x%x (level %d)\n", ctx->pc, (uint32_t)ctx->r[31][0], src, s_cnt - 1); }
                }
            }
        }
    }
    if (!g_sites || !rdram || !ctx || ctx != g_ctx.load(std::memory_order_relaxed)) return;
    // 128-bit stores (sqc2: a VU0 vector, e.g. position/velocity) are recorded as their x lane, with the site's
    // size mask flagging 16 so the classifier knows it is a vector; the half-step then halves every float lane
    if (size == 16u)
    {
        const uint32_t pc = ctx->pc;
        if (pc < kBase || pc >= kEnd) return;
        const uint32_t a = guestAddr & 0x1FFFFFFFu;
        if (a + 16u > 32u * 1024u * 1024u) return;
        const uint32_t ra = (uint32_t)ctx->r[31][0];
        Site &s = g_vecSites[((uint64_t)pc << 32) | ra];
        uint32_t oldv[4]; std::memcpy(oldv, rdram + a, 16);
        const uint32_t nw[4] = { (uint32_t)valueLo, (uint32_t)(valueLo >> 32), (uint32_t)valueHi, (uint32_t)(valueHi >> 32) };
        const uint32_t frame = (uint32_t)g_bt3FrameCount.load(std::memory_order_relaxed);
        s.count++; s.sizeMask |= 16u;
        if (frame != s.lastFrame) { s.lastFrame = frame; s.framesSeen++; }
        if (a < s.addrLo) s.addrLo = a;
        if (a > s.addrHi) s.addrHi = a;
        int slot = -1;
        for (int i = 0; i < 4; ++i) if (s.ringAddr[i] == a) { slot = i; break; }
        if (slot >= 0) { if (s.ringVal[slot] == oldv[0]) s.chain++; s.ringVal[slot] = nw[0]; }
        else { slot = s.ringPos; s.ringPos = (uint8_t)((s.ringPos + 1u) & 3u); s.ringAddr[slot] = a; s.ringVal[slot] = nw[0]; }
        if (nw[0] == oldv[0] && nw[1] == oldv[1] && nw[2] == oldv[2]) { s.zeroDelta++; return; }
        float fo[3], fn[3]; bool ok = true;
        for (int i = 0; i < 3; ++i) ok = ok && plausibleFloat(oldv[i], fo[i]) && plausibleFloat(nw[i], fn[i]);
        if (ok)
        {
            s.fplaus++;
            const float d = fn[0] - fo[0];
            if (s.dModeN == 0u) { s.dMode = d; s.dModeN = 1u; }
            else if (std::fabs(d - s.dMode) <= 1e-5f * std::fmax(1.f, std::fabs(s.dMode))) s.dModeN++;
            else s.dOtherN++;
        }
        return;
    }
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
    std::fprintf(f, "pc,count,frames,per_frame,size,addr_lo,addr_hi,chain_pct,same_pct,fplaus_pct,fdelta,fdelta_pct,idelta,idelta_pct,ra\n");
    uint32_t n = 0;
    const uint32_t total = (kEnd - kBase) >> 2;
    for (uint32_t i = 0; i < total; ++i)
    {
        const Site &s = g_sites[i];
        if (s.count < 20u || s.framesSeen < 5u) continue;
        const double c = s.count;
        const double nz = c - s.zeroDelta;
        std::fprintf(f, "0x%06x,%u,%u,%.2f,%u,0x%x,0x%x,%.0f,%.0f,%.0f,%.6g,%.0f,%d,%.0f,0\n",
                     kBase + i * 4u, s.count, s.framesSeen, c / (double)s.framesSeen, s.sizeMask, s.addrLo, s.addrHi,
                     100.0 * s.chain / c, 100.0 * s.zeroDelta / c, nz > 0 ? 100.0 * s.fplaus / nz : 0.0,
                     (double)s.dMode, s.fplaus ? 100.0 * s.dModeN / (double)s.fplaus : 0.0,
                     s.iMode, nz > 0 ? 100.0 * s.iModeN / nz : 0.0);
        ++n;
    }
    for (const auto &kv : g_vecSites)
    {
        const Site &s = kv.second;
        if (s.count < 20u || s.framesSeen < 5u) continue;
        const double c = s.count; const double nz = c - s.zeroDelta;
        std::fprintf(f, "0x%06x,%u,%u,%.2f,%u,0x%x,0x%x,%.0f,%.0f,%.0f,%.6g,%.0f,%d,%.0f,0x%x\n",
                     (uint32_t)(kv.first >> 32), s.count, s.framesSeen, c / (double)s.framesSeen, s.sizeMask, s.addrLo, s.addrHi,
                     100.0 * s.chain / c, 100.0 * s.zeroDelta / c, nz > 0 ? 100.0 * s.fplaus / nz : 0.0,
                     (double)s.dMode, s.fplaus ? 100.0 * s.dModeN / (double)s.fplaus : 0.0,
                     s.iMode, nz > 0 ? 100.0 * s.iModeN / nz : 0.0, (uint32_t)kv.first);
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
// [fightgate] the fight is "underway" once the per-frame fight update has run on 60 consecutive render frames and
// is still running (last update within 2 frames). The fight intro runs the same main loop but not the update, and
// running the intro at step 1 corrupts memory (half2..half5: DMA tags over the main stack, garbage pointers in the
// sound stream block) -- the loader/sound-stream race the loading-stall notes describe.
static std::atomic<uint32_t> g_hsStreak{0};
static std::atomic<uint64_t> g_hsPrevLogic{0};
void ps2HalfStepNoteLogic(uint64_t frame)
{
    const uint64_t prev = g_hsPrevLogic.exchange(frame, std::memory_order_relaxed);
    if (frame - prev <= 2u) g_hsStreak.fetch_add(1, std::memory_order_relaxed); else g_hsStreak.store(0, std::memory_order_relaxed);
    g_ps2HalfStepLogicFrame.store(frame, std::memory_order_relaxed);
}
bool ps2HalfStepFightActive()
{
    const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
    return fr - g_ps2HalfStepLogicFrame.load(std::memory_order_relaxed) <= 2u && g_hsStreak.load(std::memory_order_relaxed) >= 60u;
}
namespace
{
    uint8_t *g_hs = nullptr;                      // 0 none, 1 float-halve, 2 int-every-other-frame
    std::unordered_set<uint64_t> g_hsVec;         // vector sites: (pc << 32) | ra
    // one-shot guard keyed per (site, address): a hash table over all addresses (fd11: a smoke-particle lifetime
    // site touches 29 slots, >10 per frame -- an 8-entry per-site ring thrashed and re-doubled live particles)
    struct HsEntry { uint32_t addr, frame, pc; };
    constexpr uint32_t kHsBits = 22, kHsSize = 1u << kHsBits;
    HsEntry *g_hsLast = nullptr;
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
    if (k == 4)
    {   // 'h': a per-frame RATE assignment (the animation speed 2.0 set every frame by its setter): halve the value
        // itself, so every consumer -- the frame advance, the finish look-ahead, anything stepping by it -- runs at
        // the 30 fps pace. No gap logic: it is an assignment, applied on every store.
        if (size != 4) return value;
        float f; std::memcpy(&f, &value, 4);
        if (!std::isfinite(f) || std::fabs(f) > 1e6f) return value;
        f *= 0.5f; uint32_t bits; std::memcpy(&bits, &f, 4); g_hsFloat.fetch_add(1, std::memory_order_relaxed); return bits;
    }
    const uint32_t old = readOld(rdram, a, size);
    if (old == value) return value;
    // One-shot guard: a per-frame quantity is advanced on consecutive frames. A store at a listed site after a gap
    // of more than one frame is a one-shot event (a flag set, a state advance, a timer armed) and must land
    // unmodified: skipping it on an odd frame LOSES it (the half2 freeze: the fight intro polled a flag whose
    // one increment fell on an odd frame). The first tick after a gap therefore passes through at full rate.
    static uint16_t *s_siteLog = [](){ return new uint16_t[(kEnd - kBase) >> 2](); }();   // [halfstep] per-site log cap
    auto logOk = [&](uint32_t p) { uint16_t &n = s_siteLog[(p - kBase) >> 2]; return n < 60u ? (++n, true) : false; };
    HsEntry &e = g_hsLast[((a * 2654435761u) ^ (pc * 40503u)) >> (32u - kHsBits)];
    uint32_t last = 0xFFFF0000u;
    if (e.addr == a && e.pc == pc) last = e.frame;
    e.addr = a; e.pc = pc; e.frame = frame;
    const bool firstAfterGap = frame - last > 1u;
    if (k == 5)
    {   // 'j': an integer accumulator with a large step (ki -= 1600 per dash frame): halve the step, exact for |step| >= 2.
        // Same gap guard as the floats: the first store after a gap (a one-shot cost or refill) lands unmodified.
        if (firstAfterGap) return value;
        int64_t o = (size == 1) ? (int8_t)old : (size == 2) ? (int16_t)old : (int32_t)old;
        int64_t n = (size == 1) ? (int8_t)value : (size == 2) ? (int16_t)value : (int32_t)value;
        const int64_t d = n - o;
        if (d > -2 && d < 2) return value;
        const int64_t h = o + d / 2;
        g_hsFloat.fetch_add(1, std::memory_order_relaxed);
        return (uint32_t)h & ((size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu);
    }
    if (k == 3)
    {   // countdown timer ('d'): exact half rate with NO register/memory mismatch. The first decrement after a gap
        // is the first tick after the timer was armed with `old`; storing 2*old + delta makes the countdown take
        // twice as many 60 Hz frames. Every later tick lands unmodified.
        if (!firstAfterGap) return value;
        const int32_t o = sext(old, size), n = sext(value, size), d = n - o;
        if (d >= 0 || o <= 0 || o > 100000) return value;
        const int32_t doubled = 2 * o + d;
        const uint32_t limit = size == 1 ? 0x7Fu : size == 2 ? 0x7FFFu : 0x7FFFFFFFu;
        if ((uint32_t)doubled > limit) return value;
        g_hsOneShot.fetch_add(1, std::memory_order_relaxed);
        if (logOk(pc))
            std::fprintf(stderr, "[halfstep-mod] d pc=0x%06x addr=0x%x armed=%d first-tick %d -> stored %d frame=%u\n", pc, a, o, n, doubled, frame);
        return (uint32_t)doubled;
    }
    // an address this site did not touch on the previous frame (or never): a one-shot event, land it unmodified
    if (firstAfterGap) { g_hsOneShot.fetch_add(1, std::memory_order_relaxed); return value; }
    if (k == 1)
    {
        if (size != 4u) return value;
        float fo, fn;
        if (!plausibleFloat(old, fo) || !plausibleFloat(value, fn)) return value;
        // an angle wrapping across +-pi (vert60: Goku faced backwards) is a re-labelling, not an advance: pass it
        if (std::fabs(fn - fo) > 3.0f && std::fabs(fo) < 7.0f && std::fabs(fn) < 7.0f) return value;
        // a reset to exactly 0 through an accumulator's setter (the camera clock restarting for a new camera
        // move) is an assignment, never a step: pass it through, or the clock restarts at old/2
        if (fn == 0.0f) return value;
        const float h = fo + (fn - fo) * 0.5f;
        uint32_t bits; std::memcpy(&bits, &h, 4);
        g_hsFloat.fetch_add(1, std::memory_order_relaxed);
        if (logOk(pc))
            std::fprintf(stderr, "[halfstep-mod] f pc=0x%06x addr=0x%x old=%g new=%g stored=%g frame=%u\n", pc, a, fo, fn, h, frame);
        return bits;
    }
    // integer counter: keep the old value on odd render frames
    const bool skip = (frame & 1u) != 0u;
    if (logOk(pc))
        std::fprintf(stderr, "[halfstep-mod] i pc=0x%06x addr=0x%x old=%d new=%d %s frame=%u\n", pc, a, sext(old, size), sext(value, size), skip ? "SKIPPED" : "passed", frame);
    if (skip) { g_hsIntSkip.fetch_add(1, std::memory_order_relaxed); return old; }
    g_hsIntPass.fetch_add(1, std::memory_order_relaxed);
    return value;
}
void ps2HalfStepWrite128(uint8_t *rdram, uint32_t guestAddr, uint64_t &lo, uint64_t &hi, const R5900Context *ctx)
{
    if (!g_hs || !ctx || ctx != g_hsCtx.load(std::memory_order_relaxed)) return;
    const uint32_t pc = ctx->pc;
    if (pc < kBase || pc >= kEnd) return;
    if (g_hsVec.find(((uint64_t)pc << 32) | (uint32_t)ctx->r[31][0]) == g_hsVec.end()) return;   // keyed by (site, caller)
    const uint32_t a = guestAddr & 0x1FFFFFFFu;
    if (a + 16u > 32u * 1024u * 1024u) return;
    const uint32_t frame = (uint32_t)g_bt3FrameCount.load(std::memory_order_relaxed);
    if (frame - (uint32_t)g_ps2HalfStepLogicFrame.load(std::memory_order_relaxed) > 2u) return;
    HsEntry &e = g_hsLast[((a * 2654435761u) ^ (pc * 40503u)) >> (32u - kHsBits)];
    uint32_t last = 0xFFFF0000u;
    if (e.addr == a && e.pc == pc) last = e.frame;
    e.addr = a; e.pc = pc; e.frame = frame;
    if (frame - last > 1u) { g_hsOneShot.fetch_add(1, std::memory_order_relaxed); return; }
    uint32_t oldv[4]; std::memcpy(oldv, rdram + a, 16);
    uint32_t nw[4] = { (uint32_t)lo, (uint32_t)(lo >> 32), (uint32_t)hi, (uint32_t)(hi >> 32) };
    bool any = false;
    for (int i = 0; i < 4; ++i)
    {
        if (nw[i] == oldv[i]) continue;
        float fo, fn;
        if (!plausibleFloat(oldv[i], fo) || !plausibleFloat(nw[i], fn)) continue;
        const float h = fo + (fn - fo) * 0.5f;
        std::memcpy(&nw[i], &h, 4); any = true;
    }
    if (!any) return;
    g_hsFloat.fetch_add(1, std::memory_order_relaxed);
    lo = (uint64_t)nw[0] | ((uint64_t)nw[1] << 32); hi = (uint64_t)nw[2] | ((uint64_t)nw[3] << 32);
}
void ps2HalfStepEnable(const char *sitesPath)
{
    FILE *f = std::fopen(sitesPath, "r");
    if (!f) { std::fprintf(stderr, "[halfstep] cannot read %s\n", sitesPath); return; }
    g_hs = new uint8_t[(kEnd - kBase) >> 2]();
    g_hsLast = new HsEntry[kHsSize];
    for (uint32_t i = 0; i < kHsSize; ++i) { g_hsLast[i].addr = 0xFFFFFFFFu; g_hsLast[i].frame = 0xFFFF0000u; g_hsLast[i].pc = 0; }
    char line[128]; unsigned nf = 0, ni = 0, nd = 0, nv = 0;
    while (std::fgets(line, sizeof line, f))
    {
        unsigned pc = 0, ra = 0; char kind = 0;
        if (std::sscanf(line, "%x:%x %c", &pc, &ra, &kind) == 3) { if (pc >= kBase && pc < kEnd && kind == 'f') { g_hsVec.insert(((uint64_t)pc << 32) | ra); ++nv; } continue; }
        if (std::sscanf(line, "%x %c", &pc, &kind) != 2 || pc < kBase || pc >= kEnd) continue;
        if (kind == 'f') { g_hs[(pc - kBase) >> 2] = 1; ++nf; }
        else if (kind == 'i' || kind == 'u') { g_hs[(pc - kBase) >> 2] = 2; ++ni; }
        else if (kind == 'd') { g_hs[(pc - kBase) >> 2] = 3; ++nd; }
        else if (kind == 'h') { g_hs[(pc - kBase) >> 2] = 4; ++nf; }   // rate assignment: value * 0.5 on every store
        else if (kind == 'j') { g_hs[(pc - kBase) >> 2] = 5; ++ni; }   // integer step: store old + (new - old) / 2 (ki drain/charge)
    }
    std::fclose(f);
    g_hsEnabled.store(1, std::memory_order_relaxed);   // the macro switch itself is raised per fight frame (zero cost elsewhere)
    std::fprintf(stderr, "[halfstep] ON: %u float sites halved, %u integer sites on even frames only, %u countdowns doubled on arm, %u vector sites by caller (%s)\n", nf, ni, nd, nv, sitesPath);
}
void ps2HalfStepFrame(const R5900Context *ctx)
{
    if (!g_hs || !g_hsEnabled.load(std::memory_order_relaxed)) return;
    if (!g_hsCtx.load(std::memory_order_relaxed)) g_hsCtx.store(ctx);
    const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
    // raise the macros' switch only while the fight update is running (last update within 2 render frames), so the
    // loader / intro / menus run the untouched fast path -- not even the hook call (half4 froze in the loader with
    // every modification gated off: the per-store call overhead alone shifts the loader's timing)
    const bool active = ps2HalfStepFightActive();
    g_ps2HalfStep.store(active ? 1 : 0, std::memory_order_relaxed);
    if (fr - g_hsLastReport >= 600u)
    {
        g_hsLastReport = fr;
        std::fprintf(stderr, "[halfstep] frame %llu: float halved %llu, int skipped %llu / passed %llu, one-shot passthrough %llu\n", (unsigned long long)fr,
                     (unsigned long long)g_hsFloat.exchange(0), (unsigned long long)g_hsIntSkip.exchange(0), (unsigned long long)g_hsIntPass.exchange(0),
                     (unsigned long long)g_hsOneShot.exchange(0));
    }
}
