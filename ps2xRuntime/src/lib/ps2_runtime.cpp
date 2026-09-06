#include "runtime/ps2_guestprof.h"
#include "runtime/ps2_texreplace.h"   // [texreplace]
#include <filesystem>
#include "runtime/ps2_memory.h"
#include <iomanip>
#include <cstdlib>
#if !defined(_WIN32)
#include <execinfo.h> // glibc backtrace for the bad-jump diagnostic
#endif
#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "Kernel/Stubs/Pad.h"        // [hstate]/[fightprobe]: ps2_stubs::getPadDebugSnapshot()
#include "Kernel/Stubs/MemoryCard.h" // [hstate]: ps2_stubs::getMemoryCardDebugSnapshot()
#include "ps2_runtime_macros.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_gpu_renderer.h"

#if defined(__linux__)
#include "runtime/pad_evdev_linux.h"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <set>
#endif
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_settings_overlay.h"
// [wstrig] rig lever: PS2X_WSTRIG=<path> forces widescreen ON while <path> exists, so a
// drive can boot with the calibrated 4:3 screen-matching and flip to true widescreen at
// fight time (touch the file + resize the window). Checked once a second.
static bool wsTrigActive()
{
    static const char *s_p = std::getenv("PS2X_WSTRIG");
    if (!s_p || !s_p[0]) return false;
    static double s_last = -1.0; static bool s_on = false;
    const double now = GetTime();
    if (now - s_last > 1.0) { s_last = now; s_on = std::filesystem::exists(s_p); }
    return s_on;
}
#include <fstream>   // [wsknob] lever files carry a scale
// [wslever] rig levers that split the widescreen path in half WHILE THE GAME RUNS, so one
// live pause can A/B both halves without a rebuild or a second drive: while the file exists,
// PS2X_WSNOFOV neutralises the GUEST side (the projection/FOV patch: wsScale forced to 1) and
// PS2X_WSNOHUD neutralises the RENDERER side (the HUD squeeze: g_ps2xWsHudInv forced to 1).
// Both are inert unless the env names a path, and are polled once a second like PS2X_WSTRIG.
static bool wsLever(const char *env, double &last, bool &on)
{
    const char *p = std::getenv(env);
    if (!p || !p[0]) return false;
    const double now = GetTime();
    if (now - last > 1.0) { last = now; on = std::filesystem::exists(p); }
    return on;
}
static bool wsNoFov()
{
    static double s_last = -1.0; static bool s_on = false;
    return wsLever("PS2X_WSNOFOV", s_last, s_on);
}
// [wsknob] per-knob widescreen levers. PS2X_WSNOFOV disables ALL THREE pokes at once (both
// projection floats AND the sub_00130BA8 lui), which cannot say which one displaces the
// splitscreen pause text. These name one knob each: while the file exists its scale is
// overridden -- by the float parsed from the file, or 1.0 (= unpatched) when it is empty --
// so one live run can sweep a single knob without a rebuild. Returns <0 for "no override".
static float wsKnobFile(const char *env, double &last, float &cache)
{
    const char *p = std::getenv(env);
    if (!p || !p[0]) return -1.0f;
    const double now = GetTime();
    if (now - last > 1.0)
    {
        last = now; cache = -1.0f;
        std::ifstream f(p);
        if (f)
        {
            cache = 1.0f;                        // exists but empty -> unpatched
            float v = 0.0f;
            if ((f >> v) && v > 0.05f && v < 8.0f) cache = v;
        }
    }
    return cache;
}
static float wsKnob1()   { static double t = -1.0; static float c = -1.0f; return wsKnobFile("PS2X_WSK1",   t, c); }
static float wsKnob2()   { static double t = -1.0; static float c = -1.0f; return wsKnobFile("PS2X_WSK2",   t, c); }
static float wsKnobLui() { static double t = -1.0; static float c = -1.0f; return wsKnobFile("PS2X_WSKLUI", t, c); }
static float wsKnob3()   { static double t = -1.0; static float c = -1.0f; return wsKnobFile("PS2X_WSK3",   t, c); }
static float wsKnob4()   { static double t = -1.0; static float c = -1.0f; return wsKnobFile("PS2X_WSK4",   t, c); }
static bool wsNoHud()
{
    static double s_last = -1.0; static bool s_on = false;
    return wsLever("PS2X_WSNOHUD", s_last, s_on);
}
// [truews] 0.75/scale for the patched projection lui in sub_00130BA8 (0.75 = disabled).
float g_ps2xWsLui = 0.75f;
// [wshud] the widescreen aspect scale itself (1.0 = off), read by the GPU renderer to
// pre-squeeze HUD sprites so the present stretch restores their true proportions.
float g_ps2xWsScale = 1.0f;
// [wshud] per-frame squeeze factor for HUD geometry, computed at present time from the
// ACTUAL present mapping (desired authentic h-scale / widescreen h-scale). 1.0 = off.
float g_ps2xWsHudInv = 1.0f;
// [wshudmap] the present crop's source width (game px). The HUD layout map must use the
// VISIBLE content width (~493 in fights), not FRAME.FBW (640) -- scaling the breakpoints
// by FBW shoved the center window into a bridge zone and stretched the timer plaque.
float g_ps2xWsSrcW = 512.0f;
#include "rlgl.h" // rlSetBlendFactorsSeparate for the blend-free present blit
namespace ps2_syscalls { bool bt3WakeThreadByEntry(uint32_t entry); }

#include <iostream>
#include <fstream>
#include <algorithm>
#include <vector>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <sstream>

// BT3 debug tracing: extremely verbose ([main-pc] etc.). Off unless PS2X_TRACE=1.
// The per-instruction [main-pc] trace alone emits ~1000+ stderr lines/sec and
// dominates wall-clock, so keep it gated for normal (fast) runs.
static inline bool ps2xTraceEnabled()
{
    static const bool enabled = []() {
        const char *v = std::getenv("PS2X_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

// PS2X_CAMPROBE write-watch (see ps2_runtime.h). Armed by the camera probe to find the
// function that writes the battle camera-target vector.
extern "C" { extern unsigned long long g_rlglDrawCalls, g_rlglBatchFlushes; extern double g_rlglFlushNs; }   // [glcalls] raylib rlgl batch counters (patched rlgl.h)
std::atomic<uint32_t> g_ps2WatchLo{0};
std::atomic<uint32_t> g_ps2WatchHi{0};
std::atomic<uint32_t> g_ps2WatchAll{0};
uint8_t *g_ps2WatchRdram = nullptr;   // [wispsrc] guest RAM base (set at bind) for ps2WatchReport
void ps2WatchReport(uint32_t guestAddr, uint32_t size, uint64_t valueLo, uint64_t valueHi,
                    const char *op, const R5900Context *ctx)
{
    // Garbage-only filter: skip writes whose value is a valid code address (a legit callback)
    // or zero, so only the CORRUPTING (out-of-code) write to a watched pointer is reported.
    // PS2X_AWATCH sets g_ps2WatchAll to KEEP zero writes (needed to find the zero-world-matrix write).
    if (g_ps2WatchAll.load(std::memory_order_relaxed) == 0u)
    {
        const uint32_t v = static_cast<uint32_t>(valueLo);
        if (v == 0u || (v >= 0x100008u && v < 0x2bf69cu)) return;
    }
    extern std::atomic<uint32_t> g_watchReportN; // resettable: texwatch re-arms mid-run
    const uint32_t n = g_watchReportN.fetch_add(1);
    const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
    const uint32_t a1 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) : 0u;
    // Dedup by (addr, pc, value): scenes rewrite the same list slots every frame with the
    // same bytes, which used to exhaust the 200-line budget in the menus. A NEW value at
    // the same address (e.g. the stage-load display-list build) always prints.
    static std::mutex s_cwMx;
    static std::unordered_set<uint64_t> s_cwSeen;
    // PS2X_GWATCH_NODEDUP=1: report EVERY write (no (addr,pc,value) dedup, no budget) —
    // needed when the interesting event is a REPEAT write of the same value (re-arm vs
    // first-arm of the same struct), which the dedup otherwise makes invisible.
    static const bool s_noDedup = [](){ const char *v = std::getenv("PS2X_GWATCH_NODEDUP"); return v && v[0] && v[0] != '0'; }();
    if (!s_noDedup)
    {
        uint64_t k = 1469598103934665603ull;
        for (uint64_t x : {(uint64_t)guestAddr, (uint64_t)(ctx ? ctx->pc : 0u), valueLo, valueHi})
            k = (k ^ x) * 1099511628211ull;
        {
            std::lock_guard<std::mutex> lk(s_cwMx);
            if (s_cwSeen.size() < 400u && !s_cwSeen.insert(k).second) return;
            if (s_cwSeen.size() >= 400u) return;
        }
    }
    // [wispsrc] second-level return address ([sp+8], where the 0x1006e8 append helper saved
    // $ra before calling memcpy) and the bytes now at the watched qword -- names the BUILDER
    // of a memcpy'd packet and shows whether the copy carried the wisp colour bytes.
    uint32_t ra2 = 0u; char qw[48] = "";
    if (g_ps2WatchRdram)
    {
        const uint32_t sp = ctx ? (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) & 0x1FFFFFFFu) : 0u;
        if (ctx && sp + 12u <= 0x2000000u) std::memcpy(&ra2, g_ps2WatchRdram + sp + 8u, 4);
        const uint32_t wl = g_ps2WatchLo.load(std::memory_order_relaxed);
        if (wl && wl + 16u <= 0x2000000u)
            for (int i = 0; i < 16; ++i) std::snprintf(qw + i * 2, 3, "%02x", g_ps2WatchRdram[wl + i]);
    }
    // [wispalpha] inside FUN_00163980's vertex-write block (0x163f60..0x164080): dump the
    // factors of the wisp alpha product (alpha_i = shape[i] * [s1+0x6C] * 255 * f23, with
    // f23 = [s7+0x38] * [s7+0x50] * f12_arg) so the zero factor names itself. Once per 64 hits.
    if (ctx && g_ps2WatchRdram && ctx->pc >= 0x163f60u && ctx->pc < 0x164080u)
    {
        static unsigned s_wa = 0;
        if ((s_wa++ % 64u) == 0u)
        {
            const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0)) & 0x1FFFFFFFu;
            const uint32_t s7 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[23], 0)) & 0x1FFFFFFFu;
            const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) & 0x1FFFFFFFu;
            auto rf = [&](uint32_t a) { float v = 0.f; if (a + 4u <= 0x2000000u) std::memcpy(&v, g_ps2WatchRdram + a, 4); return v; };
            auto ru = [&](uint32_t a) { uint32_t v = 0u; if (a + 4u <= 0x2000000u) std::memcpy(&v, g_ps2WatchRdram + a, 4); return v; };
            const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)) & 0x1FFFFFFFu;
            const uint32_t g5868 = ru(gp - 0x5868u) & 0x1FFFFFFFu;
            std::fprintf(stderr, "[wispG] G=0x%x G+0..1c=(%g,%g,%g,%g,%g,%g,%g,%g) G+20/24/28=(%g,%g,%g) reent=0x%x seed=0x%08x%08x p+C=%g p+1C=%g p+0=0x%x p+4..b=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                         g5868, rf(g5868), rf(g5868 + 4u), rf(g5868 + 8u), rf(g5868 + 0xcu), rf(g5868 + 0x10u), rf(g5868 + 0x14u), rf(g5868 + 0x18u), rf(g5868 + 0x1cu), rf(g5868 + 0x20u), rf(g5868 + 0x24u), rf(g5868 + 0x28u), ru(0x2e9808u), ru((ru(0x2e9808u) & 0x1FFFFFFFu) + 0xacu), ru((ru(0x2e9808u) & 0x1FFFFFFFu) + 0xa8u),
                         rf(s1 + 0xcu), rf(s1 + 0x1cu), ru(s1),
                         g_ps2WatchRdram[s1 + 4u], g_ps2WatchRdram[s1 + 5u], g_ps2WatchRdram[s1 + 6u], g_ps2WatchRdram[s1 + 7u], g_ps2WatchRdram[s1 + 8u], g_ps2WatchRdram[s1 + 9u], g_ps2WatchRdram[s1 + 0xau], g_ps2WatchRdram[s1 + 0xbu]);
            std::fprintf(stderr, "[wisptpl] sys+3F0=(%g,%g,%g,%g) sys+400=(%g,%g,%g,%g) sys+410=(%g,%g,%g,%g) p+10/14/28=(%g,%g,%g) pvel+70=(%g,%g,%g,%g) g2c/g30=(%g,%g) sys+0=0x%x sys+8=0x%x sys+10=0x%x\n",
                         rf(s7 + 0x3f0u), rf(s7 + 0x3f4u), rf(s7 + 0x3f8u), rf(s7 + 0x3fcu),
                         rf(s7 + 0x400u), rf(s7 + 0x404u), rf(s7 + 0x408u), rf(s7 + 0x40cu),
                         rf(s7 + 0x410u), rf(s7 + 0x414u), rf(s7 + 0x418u), rf(s7 + 0x41cu),
                         rf(s1 + 0x10u), rf(s1 + 0x14u), rf(s1 + 0x28u),
                         rf(s1 + 0x70u), rf(s1 + 0x74u), rf(s1 + 0x78u), rf(s1 + 0x7cu),
                         rf(g5868 + 0x2cu), rf(g5868 + 0x30u), ru(s7), ru(s7 + 8u), ru(s7 + 0x10u));
            std::fprintf(stderr, "[wispalpha] pc=0x%x s1=0x%x s7=0x%x f23=%g pa[s1+6c]=%g s1flags=0x%x s1+60/64/68=(%g,%g,%g) sys38=%g sys50=%g sys60=%g shape=(%g,%g,%g,%g) abytes=%02x%02x%02x%02x tail[gp-78f4]=%g callerRA=0x%x\n",
                         ctx->pc, s1, s7, ctx->f[23], rf(s1 + 0x6cu), ru(s1), rf(s1 + 0x60u), rf(s1 + 0x64u), rf(s1 + 0x68u),
                         rf(s7 + 0x38u), rf(s7 + 0x50u), rf(s7 + 0x60u),
                         rf(sp + 0x150u), rf(sp + 0x154u), rf(sp + 0x158u), rf(sp + 0x15cu),
                         g_ps2WatchRdram[sp + 0x160u], g_ps2WatchRdram[sp + 0x161u], g_ps2WatchRdram[sp + 0x162u], g_ps2WatchRdram[sp + 0x163u],
                         rf((static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)) & 0x1FFFFFFFu) - 0x78f4u), ru(sp + 0x238u));
        }
    }
    // [wispsrc] mini backtrace: code-range words in the first 0x180 bytes of the guest stack
    // (saved $ra slots of this frame and its callers, whatever the frame layout).
    char stk[96] = ""; int sn = 0;
    if (g_ps2WatchRdram && ctx)
    {
        const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) & 0x1FFFFFFFu;
        for (uint32_t o = 0; o < 0x280u && sp + o + 4u <= 0x2000000u && sn < 6; o += 4)
        {
            uint32_t w; std::memcpy(&w, g_ps2WatchRdram + sp + o, 4);
            if (w >= 0x100000u && w < 0x2c0000u && (w & 3u) == 0u)
                sn += std::snprintf(stk + std::strlen(stk), sizeof stk - std::strlen(stk), "%s%x@%x", sn ? "," : "", w, o) > 0 ? 1 : 0;
        }
    }
    std::fprintf(stderr, "[camwrite] #%u addr=0x%x sz=%u pc=0x%x ra=0x%x ra2=0x%x a1src=0x%x val=0x%016llx%016llx op=%s qw=%s stk=%s\n",
                 n, guestAddr, size, ctx ? ctx->pc : 0u, ra, ra2, a1,
                 (unsigned long long)valueHi, (unsigned long long)valueLo, op, qw, stk);
}

std::atomic<uint32_t> g_watchReportN{0};
uint32_t g_txBad[8] = {}; // texwatch: stale-copy addresses to skip on rescan
int g_txBadN = 0;
std::atomic<uint32_t> g_ps2ValueWatch{0};
std::atomic<uint32_t> g_bt3FillObj{0};
std::atomic<uint32_t> g_bt3DrawMethod{0};
void ps2ValueWatchReport(uint32_t guestAddr, uint32_t size, uint64_t valueLo,
                         const char *op, const R5900Context *ctx)
{
    {   // [vwfr] frame stamp for every value-watch hit (pairs with the [vwatch] line that follows)
        extern std::atomic<uint64_t> g_bt3FrameCount;
        std::fprintf(stderr, "[vwfr] fr=%llu\n", (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed));
    }
    {   // [vwdump] one-shot hexdump around value-watch hits landing in the frame-DL region
        const uint32_t a_ = guestAddr & 0x1FFFFFFFu;
        uint8_t *rd_ = ps2GetRdramHostPtr();
        if (rd_ && a_ >= 0x600000u && a_ < 0x800000u)
        {
            static std::atomic<int> s_vwd{0};
            if (s_vwd.fetch_add(1) < 2)
            {
                const uint32_t b_ = (a_ - 0x80u) & ~0xFu;
                for (uint32_t o_ = 0; o_ < 0x140u; o_ += 16u)
                {
                    uint32_t w_[4]; std::memcpy(w_, rd_ + b_ + o_, 16);
                    std::fprintf(stderr, "[vwdump] 0x%08x: %08x %08x %08x %08x\n", b_ + o_, w_[0], w_[1], w_[2], w_[3]);
                }
                // [clobcatch] arm the store-watch on the REF tag qword: the next writer that
                // touches it before the kick is the clobberer (prints as [camwrite] with pc/ra).
                g_ps2WatchLo.store((a_ - 4u) & ~0xFu, std::memory_order_relaxed);
                g_ps2WatchHi.store(((a_ - 4u) & ~0xFu) + 0x10u, std::memory_order_relaxed);
                g_ps2WatchAll.store(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[clobcatch] armed 0x%08x..+0x10\n", (a_ - 4u) & ~0xFu);
            }
        }
    }
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1);
    if (n < 400u)
    {
        const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
        std::fprintf(stderr, "[vwatch] #%u wrote 0x%x -> addr=0x%x sz=%u pc=0x%x ra=0x%x op=%s\n",
                     n, (uint32_t)valueLo, guestAddr, size, ctx ? ctx->pc : 0u, ra, op);
    }
}

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
// 2x-ish the PS2 display: gives the settings overlay comfortable logical resolution
// (its panel is 1080px wide) while the present path scales the game to fit. (PR #1
// pairing; the overlay also self-clamps to smaller viewports.)
static constexpr int HOST_WINDOW_WIDTH = 1280;
static constexpr int HOST_WINDOW_HEIGHT = 720;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
#ifdef __linux__
namespace
{
    // [pinthreads] PS2X_PIN: keep the game thread and the GL/present thread on distinct
    // physical cores. Left to the OS on a 2-core host, two heavy CPU threads quietly land
    // on SMT siblings of the SAME physical core (e.g. cpus 0 and 2 on a 2C/4T box), halving
    // the pipeline capacity both depend on. Pinning (the trick PCSX2/AetherSX2 apply to their
    // EE/GS threads) buys ~5-15% and tighter 0.1% lows. Values (Linux only):
    //   unset    = auto: pin only when the host exposes exactly 2 physical cores
    //   "0" | "none" | "off" = never pin
    //   "A,B"    = game thread -> CPU A, GL/present thread -> CPU B
    std::vector<int> ps2xParseCpuList(const std::string &s)
    {
        std::vector<int> out;
        std::string t;
        auto emit = [&](const std::string &tok)
        {
            const size_t dash = tok.find('-');
            const int lo = std::atoi(tok.substr(0, dash).c_str());
            const int hi = (dash == std::string::npos) ? lo : std::atoi(tok.substr(dash + 1).c_str());
            if (lo >= 0 && hi >= lo) for (int c = lo; c <= hi; ++c) out.push_back(c);
        };
        for (char ch : s)
        {
            if (ch == ',' || ch == ' ') { if (!t.empty()) { emit(t); t.clear(); } }
            else if (ch != '\n') t += ch;
        }
        if (!t.empty()) emit(t);
        return out;
    }

    std::vector<int> ps2xCoreCpus(int cpu)
    {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        for (const char *leaf : {"core_cpus_list", "thread_siblings_list"})
        {
            std::ifstream f(base + leaf);
            std::string s;
            if (f >> s && !s.empty())
            {
                const std::vector<int> cpus = ps2xParseCpuList(s);
                if (!cpus.empty()) return cpus;
            }
        }
        return {cpu};
    }

    std::vector<int> ps2xOnlineCpus(const cpu_set_t *only = nullptr)
    {
        std::vector<int> out;
        const long n = sysconf(_SC_NPROCESSORS_CONF);
        for (long c = 0; c < n; ++c)
        {
            std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/online");
            bool on = true;
            if (f) { int x = 0; f >> x; on = (x != 0); }
            if (on && (!only || CPU_ISSET(c, only))) out.push_back((int)c);
        }
        return out;
    }

    std::vector<int> ps2xCoreFirstCpus(const cpu_set_t *only = nullptr)
    {
        std::set<int> reps;
        for (int c : ps2xOnlineCpus(only))
        {
            const std::vector<int> grp = ps2xCoreCpus(c);
            if (!grp.empty()) reps.insert(*std::min_element(grp.begin(), grp.end()));
        }
        return std::vector<int>(reps.begin(), reps.end());
    }

    std::string ps2xCpuMaskStr(const std::vector<int> &cpus)
    {
        std::string s;
        for (size_t i = 0; i < cpus.size(); ++i) { if (i) s += ','; s += std::to_string(cpus[i]); }
        return s;
    }

    // Fills the game-thread and GL-thread masks; returns false when pinning is off/inaplicable.
    bool ps2xPinMasks(cpu_set_t &game, cpu_set_t &gl)
    {
        CPU_ZERO(&game); CPU_ZERO(&gl);
        const char *v = std::getenv("PS2X_PIN");
        if (v && v[0] && (!strcmp(v, "0") || !strcmp(v, "none") || !strcmp(v, "off")))
            return false;
        cpu_set_t allowed; CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
        std::vector<int> g, m;
        if (v && v[0] && strcmp(v, "auto"))
        {
            const std::vector<int> picks = ps2xParseCpuList(v);
            if (picks.size() != 2)
            {
                std::fprintf(stderr, "[pinthreads] PS2X_PIN=\"%s\" not A,B (e.g. 0,1); ignoring\n", v);
                return false;
            }
            const std::vector<int> a = ps2xCoreCpus(picks[0]);
            if (std::find(a.begin(), a.end(), picks[1]) != a.end())
                std::fprintf(stderr, "[pinthreads] WARNING PS2X_PIN=%d,%d put both threads on the SAME physical core (SMT siblings); pick one CPU per core\n", picks[0], picks[1]);
            g = {picks[0]}; m = {picks[1]};
        }
        else
        {
            // Auto counts physical cores only within the affinity WE are allowed (so a
            // taskset -c 0,1 or a 2-core container engages exactly like a real 2-core box).
            const std::vector<int> cores = ps2xCoreFirstCpus(&allowed);
            // auto engages only on 2-physical-core hosts
            if (cores.size() != 2)
            {
                std::fprintf(stderr, "[pinthreads] auto: host exposes %zu physical cores, skipping pin\n", cores.size());
                return false;
            }
            g = ps2xCoreCpus(cores[0]);            // whole core A (both SMT threads)
            m = ps2xCoreCpus(cores[1]);            // whole core B
        }
        auto prune = [&](std::vector<int> &sel)
        {
            for (size_t i = 0; i < sel.size(); ++i)
                if (!CPU_ISSET(sel[i], &allowed)) sel.erase(sel.begin() + i--);
        };
        prune(g); prune(m);
        if (g.empty() || m.empty())
        {
            std::fprintf(stderr, "[pinthreads] requested cpus not allowed by the process; skipping\n");
            return false;
        }
        for (int c : g) CPU_SET(c, &game);
        for (int c : m) CPU_SET(c, &gl);

        std::fprintf(stderr, "[pinthreads] %s: game->{%s} GL/present->{%s}\n",
                     (v && v[0] && strcmp(v, "auto")) ? "explicit" : "auto",
                     ps2xCpuMaskStr(g).c_str(), ps2xCpuMaskStr(m).c_str());
        return true;
    }

    // [pinthreads] keeper: third-party init (GLFW/Mesa/audio) or a cgroup update can clear
    // the process affinity at ANY point (observed during boot and again ~40s in). Run for
    // the whole session -- the check is 5x/s of two getaffinity syscalls, negligible.
    void ps2xPinKeeperLoop(pthread_t gameTid, const cpu_set_t &sGame, const cpu_set_t &sGl,
                           std::atomic<bool> *stop)
    {
        int logs = 0;
        while (!stop->load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            cpu_set_t cg, cm; CPU_ZERO(&cg); CPU_ZERO(&cm);
            const bool dg = (pthread_getaffinity_np(gameTid, sizeof(cg), &cg) != 0) || !CPU_EQUAL(&cg, &sGame);
            const bool dm = (sched_getaffinity(getpid(), sizeof(cm), &cm) != 0) || !CPU_EQUAL(&cm, &sGl);
            if (dg) pthread_setaffinity_np(gameTid, sizeof(cg), &sGame);
            if (dm) sched_setaffinity(getpid(), sizeof(cm), &sGl);
            if ((dg || dm) && logs++ < 3)
                std::fprintf(stderr, "[pinthreads] keeper re-pinned game=%s main=%s\n", dg ? "yes" : "ok", dm ? "yes" : "ok");
        }
    }
}
#endif

    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;
    // The main thread's stack starts at PS2_RAM_SIZE-0x10 and grows DOWN. The async callback stack
    // allocator used to start its bump pointer at PS2_RAM_SIZE as well, so the very first
    // reserveAsyncCallbackStack() handed a callback the same top-of-RAM address the main thread was
    // already running on -- a vsync/alarm handler then pushed its frames straight over the main
    // thread's live saved registers. Keep the top of RAM for the main thread and carve callback
    // stacks below it.
    constexpr uint32_t kMainThreadStackReserve = 0x40000u; // 256 KiB

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;
    thread_local std::unordered_map<PS2Runtime *, uint32_t> g_guestExecutionDepths;
    // Per-host-thread guest tid for the deterministic scheduler (main = 1).
    thread_local int g_schedTid = 1;
    // Only true on real guest dispatch threads (main + StartThread workers). Host
    // service threads (vblank/audio/MPEG) must never touch the scheduler token.
    thread_local bool g_schedIsGuest = false;
    std::atomic<uint32_t> g_schedDbgCount{0};
    // Incremented each time the game swaps display buffers (DISPFB write). Lets the
    // host present frame-coherent snapshots instead of live-VRAM mid-render reads.
    std::atomic<uint64_t> g_displaySwapCounter{0};
    inline bool schedDbgEnabled() { static const bool on = [](){ const char *v = std::getenv("PS2X_SCHED_DEBUG"); return v && v[0] && v[0] != '0'; }(); return on; }
    std::atomic<int> g_guestMutexHolderTid{-1};
    inline bool schedDbgOn() { return schedDbgEnabled() && g_schedDbgCount.fetch_add(1) < 2000u; }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));   // VU0 R register: 23-bit LFSR, reads as 1.0|mantissa
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
                {
                    // [vf0basis] PS2X_VF0BASIS=1: seed the persistent VU0 identity basis rows the
                    // game establishes once via FUN_00120088 (MR32 chain) — hardware shares ONE
                    // physical VU0 across threads; per-thread contexts otherwise start them zero
                    // and func_121E50's normalize drops z^2 (vf3.x==0) => terrain band tears.
                    static const bool s_vb = [](){ const char *v = std::getenv("PS2X_VF0BASIS"); return v && v[0] && v[0] != '0'; }();
                    if (s_vb)
                    {
                        ctx->vu0_vf[1] = _mm_set_ps(0.0f, 1.0f, 0.0f, 0.0f); // (0,0,1,0)
                        ctx->vu0_vf[2] = _mm_set_ps(0.0f, 0.0f, 1.0f, 0.0f); // (0,1,0,0)
                        ctx->vu0_vf[3] = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.0f); // (1,0,0,0)
                    }
                }

        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }


    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

extern "C" bool ps2xThreadWaitInfo(int tid, int *waitType, int *waitId, int *semaCount, int *semaWaiters, int *wakeupCount);   // [schedwhy2] Thread.cpp
extern "C" bool ps2xCdTickStale(unsigned ms);   // [dispatchpump]
extern "C" void ps2xCdTickOnly(uint8_t *, R5900Context *, PS2Runtime *);
extern "C" void ps2xFixupRingDump();   // [fixupring]
extern "C" void *ps2xGuestWaitBegin();
extern "C" void ps2xGuestWaitEnd(void *);
extern "C" void ps2xSpinPump(uint8_t *, R5900Context *, PS2Runtime *);   // [spinpump] game_overrides.cpp

PS2Runtime::GuestExecutionScope::GuestExecutionScope(PS2Runtime *runtime) noexcept
    : m_runtime(runtime)
{
    if (m_runtime)
    {
        m_runtime->enterGuestExecution();
    }
}

PS2Runtime::GuestExecutionScope::~GuestExecutionScope()
{
    if (m_runtime)
    {
        m_runtime->leaveGuestExecution();
    }
}

PS2Runtime::GuestExecutionReleaseScope::GuestExecutionReleaseScope(PS2Runtime *runtime) noexcept
    : m_runtime(runtime)
{
    if (m_runtime)
    {
        m_depth = m_runtime->releaseGuestExecution();
        // A blocking guest thread must hand off the scheduler TOKEN so others aren't starved.
        // This is gated on being a scheduled guest thread, NOT on the guest-exec depth: a
        // thread can hold the token but no guest-exec lock (e.g. waitWhileSuspended / SleepThread
        // called from the worker loop BETWEEN per-step guest-exec scopes, so m_depth==0). If we
        // only released the token when m_depth!=0, a suspended/sleeping thread would keep the
        // token while parked -> total deadlock. schedBeginBlock is a no-op if we aren't current.
        if (m_runtime->m_schedEnabled && g_schedIsGuest)
        {
            m_runtime->schedBeginBlock(g_schedTid);
        }
    }
}

PS2Runtime::GuestExecutionReleaseScope::~GuestExecutionReleaseScope()
{
    if (!m_runtime)
        return;
    // Re-acquire the scheduler token independent of guest-exec depth (mirror of the ctor).
    // Do NOT re-acquire while an exception is unwinding: that means this guest thread is EXITING
    // (ThreadExitException), and re-grabbing the token would leave a dead thread holding it
    // (m_schedCurrent stuck on a corpse) -> everyone starves; schedUnregister cleans up presence.
    if (m_runtime->m_schedEnabled && g_schedIsGuest && std::uncaught_exceptions() == 0)
    {
        m_runtime->schedAcquire(g_schedTid, 0); // wait for the token back
    }
    if (m_depth != 0u)
    {
        m_runtime->reacquireGuestExecution(m_depth);
    }
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    static const bool s_swapLatch = []() { const char *v = std::getenv("PS2X_SWAP_LATCH"); return !(v && v[0] == '0'); }();
    static uint64_t s_lastSwapSeen = 0u;
    static uint64_t s_lastSwapTick = 0u;
    const uint64_t currentTick = ps2_syscalls::GetCurrentVSyncTick();
    const uint64_t swapNow = g_displaySwapCounter.load(std::memory_order_acquire);
    bool latchedThisCall = false;
    if (!s_hasLatchedInitialFrame)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
        latchedThisCall = true;
    }
    else if (s_swapLatch && swapNow != s_lastSwapSeen)
    {
        // A buffer swap happened -> the swap-callback already latched a COMPLETE
        // frame into m_hostPresentationFrame. Present it (no live-VRAM read).
        s_lastSwapSeen = swapNow;
        s_lastSwapTick = currentTick;
        latchedThisCall = true;
    }
    else if (currentTick != s_lastPresentationTick &&
             // FALLBACK: if the game isn't swapping (early boot / stalled), keep the
             // display alive with the old tick-latch so we never freeze on a stale
             // frame or the pink no-frame fallback. Skip while swaps are flowing.
             (!s_swapLatch || (currentTick - s_lastSwapTick) > 4u) &&
             rt->gs().tryLatchHostPresentationFrame())
    {
        s_lastPresentationTick = currentTick;
        latchedThisCall = true;
    }

    if (!latchedThisCall && s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    // Diagnostic (PS2X_SW_DUMP): dump the SOFTWARE-presented image (the correctly latched
    // DISPFB buffer) so we can see what software actually shows vs the GPU path.
    {
        static const bool s_sd = [](){ const char *v = std::getenv("PS2X_SW_DUMP"); return v && v[0] && v[0] != '0'; }();
        static int s_sc = 0; ++s_sc;
        if (s_sd && (s_sc % 90) == 45 && !s_scratch.empty() && width != 0u && height != 0u)
        {
            Image im{}; im.data = s_scratch.data(); im.width = static_cast<int>(width);
            im.height = static_cast<int>(height); im.mipmaps = 1;
            im.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            char p[160];
            std::snprintf(p, sizeof(p), "/home/z3/Desktop/bt3/work/sw_present_%d_fbp%u.png", s_sc, displayFbp);
            ExportImage(im, p);
            std::fprintf(stderr, "[sw-dump] %s %ux%u displayFbp=%u sourceFbp=%u\n", p, width, height, displayFbp, sourceFbp);
        }
    }

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    UpdateTexture(tex, s_uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    {
        const char *v = std::getenv("PS2X_SCHED");
        m_schedEnabled = (v && v[0] && v[0] != '0');
        if (m_schedEnabled)
            std::cerr << "[sched] deterministic cooperative guest scheduler ENABLED" << std::endl;
    }
    {
        const char *v = std::getenv("PS2X_VWATCH");
        if (v && v[0])
        {
            const uint32_t val = (uint32_t)std::strtoul(v, nullptr, 0);
            g_ps2ValueWatch.store(val, std::memory_order_relaxed);
            std::cerr << "[vwatch] value-watch armed for 0x" << std::hex << val << std::dec << std::endl;
        }
    }
    {
        // PS2X_AWATCH=0xLO[:0xHI]: watch a guest ADDRESS range, reporting the guest PC of every write
        // (including zero -- see g_ps2WatchAll). For finding who writes/doesn't-write the world matrix.
        const char *v = std::getenv("PS2X_AWATCH");
        if (v && v[0])
        {
            uint32_t lo = (uint32_t)std::strtoul(v, nullptr, 0);
            const char *c = std::strchr(v, ':');
            uint32_t hi = c ? (uint32_t)std::strtoul(c + 1, nullptr, 0) : (lo + 0x40u);
            g_ps2WatchLo.store(lo & 0x1FFFFFFFu, std::memory_order_relaxed);
            g_ps2WatchHi.store(hi & 0x1FFFFFFFu, std::memory_order_relaxed);
            g_ps2WatchAll.store(1u, std::memory_order_relaxed);
            std::cerr << "[awatch] address-watch armed 0x" << std::hex << lo << "..0x" << hi << std::dec << std::endl;
        }
    }
    std::memset(&m_cpuContext, 0, sizeof(m_cpuContext));

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);

    // VU0 vf0 is hardwired read-only to (0,0,0,1); a zero-init context leaves it (0,0,0,0),
    // which poisons every VU0 macro-mode matrix (the identity basis is built by rotating vf0).
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
                {
                    // [vf0basis] PS2X_VF0BASIS=1: seed the persistent VU0 identity basis rows the
                    // game establishes once via FUN_00120088 (MR32 chain) — hardware shares ONE
                    // physical VU0 across threads; per-thread contexts otherwise start them zero
                    // and func_121E50's normalize drops z^2 (vf3.x==0) => terrain band tears.
                    static const bool s_vb = [](){ const char *v = std::getenv("PS2X_VF0BASIS"); return v && v[0] && v[0] != '0'; }();
                    if (s_vb)
                    {
                        m_cpuContext.vu0_vf[1] = _mm_set_ps(0.0f, 1.0f, 0.0f, 0.0f); // (0,0,1,0)
                        m_cpuContext.vu0_vf[2] = _mm_set_ps(0.0f, 0.0f, 1.0f, 0.0f); // (0,1,0,0)
                        m_cpuContext.vu0_vf[3] = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.0f); // (1,0,0,0)
                    }
                }


    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE - kMainThreadStackReserve;
}

double g_fpPresent = 0, g_fpBar = 0, g_fpPre = 0, g_fpWait = 0, g_fpLoop = 0; int g_fpN = 0;   // [frameprof]
void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        ps2_syscalls::detachAllGuestHostThreads();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();

    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    {
                                        extern uint8_t g_gifArbCurPath; // set by GifArbiter::drain
                                        m_gs.m_curSrcPath = g_gifArbCurPath;
                                        m_gs.processGIFPacket(data, size);
                                    });
    m_memory.setGifArbiter(&m_gifArbiter);
    // On display-buffer swap, snapshot a frame-coherent presentation image so the
    // host shows completed frames (fixes mid-render jitter / disappearing text).
    m_memory.setDisplaySwapCallback([this]() {
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_SWAP_LATCH"); return !(v && v[0] == '0'); }();
        if (s_on)
        {
            // Snapshot the just-completed frame at the buffer swap (frame boundary).
            m_gs.latchHostPresentationFrame();
            g_displaySwapCounter.fetch_add(1u, std::memory_order_release);
        }
    });
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 { m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                 m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                 m_gs, &m_memory, startPC, top, itop, 65536); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 { m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                m_gs, &m_memory, top, itop, 65536); });
    m_iop.init(rdram);
    m_iop.reset();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    g_ps2WatchRdram = rdram;   // [wispsrc] lets ps2WatchReport read the guest stack ([sp+8]) and the watched qword
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    // PS2X_GWATCH_LO/HI (hex): arm the guest write-watch on an arbitrary region from
    // startup — reports every recompiled store into it as [camwrite] with pc/ra.
    // Implies keep-zero-writes (the never-written-matrix hunts need them).
    if (const char *lo = std::getenv("PS2X_GWATCH_LO"))
    {
        const char *hi = std::getenv("PS2X_GWATCH_HI");
        g_ps2WatchLo.store((uint32_t)std::strtoul(lo, nullptr, 16) & 0x1FFFFFFFu, std::memory_order_relaxed);
        g_ps2WatchHi.store(hi ? ((uint32_t)std::strtoul(hi, nullptr, 16) & 0x1FFFFFFFu) : 0x1FFFFFFFu, std::memory_order_relaxed);
        g_ps2WatchAll.store(1u, std::memory_order_relaxed);
        std::fprintf(stderr, "[gwatch] armed 0x%x..0x%x\n", g_ps2WatchLo.load(), g_ps2WatchHi.load());
    }
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }

#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        {   // [vsync] PS2X_VSYNC=1: enable vsync (GPU waits for v-blank -> idles -> lower temp).
            const char *v = std::getenv("PS2X_VSYNC");
            if (v && v[0] && v[0] != '0') SetConfigFlags(FLAG_VSYNC_HINT);
        }
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
        SetTargetFPS(60);
        {   // [texreplace] Index replacements at STARTUP rather than lazily on the first texture
            // decode, so the overlay's Texture Replacement switch is correctly enabled/disabled
            // from the moment it opens and the "[texreplace] indexed N" line appears at boot.
            // (The textures/ folder itself ships in the repo and is staged next to the binary by
            // CMake; the create-if-absent inside buildIndex is only a fallback for a foreign CWD.)
            ps2tex::replacementsEnabled();
        }
        // [barblock] manual pacing: the present thread must service guest barriers every few
        // hundred microseconds, which it cannot do while asleep inside EndDrawing's frame cap.
        // PS2X_BARPACE=0 keeps raylib's frame cap (barrier latency then <= one frame) -- A/B for
        // whether the manual pacing itself disturbs the guest.
        static const bool s_barPace = [](){ const char *v = std::getenv("PS2X_BARPACE"); return !(v && v[0] == '0'); }();
        if (GsGpuRenderer::blockingBarriersEnabled() && s_barPace) SetTargetFPS(0);
        // PS2X_TOPMOST=1: keep the game window above others — unattended (AFK) test runs
        // screenshot the whole desktop, and an occluded window can't be captured on Wayland.
        if (const char *tm = std::getenv("PS2X_TOPMOST"); tm && tm[0] && tm[0] != '0')
            SetWindowState(FLAG_WINDOW_TOPMOST);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        // PS2X_REPLAY=1: standalone VU1 replay of the spike snapshot (work/spike_{micro,
        // data,state}.bin) through the full normal kick path, then exit. Lets the popup
        // divergence be debugged offline with no game session.
        if (const char *rp = std::getenv("PS2X_REPLAY"); rp && rp[0] && rp[0] != '0')
        {
            syncCoreSubsystems();
            auto loadBin = [](const char *path, void *dst, size_t n) -> bool {
                FILE *f = std::fopen(path, "rb");
                if (!f) { std::fprintf(stderr, "[replay] missing %s\n", path); return false; }
                const size_t got = std::fread(dst, 1, n, f);
                std::fclose(f);
                if (got != n) { std::fprintf(stderr, "[replay] short read %s (%zu/%zu)\n", path, got, n); return false; }
                return true;
            };
            VU1State st{};
            if (loadBin("/home/z3/Desktop/bt3/work/spike_micro.bin", m_memory.getVU1Code(), PS2_VU1_CODE_SIZE) &&
                loadBin("/home/z3/Desktop/bt3/work/spike_data.bin", m_memory.getVU1Data(), PS2_VU1_DATA_SIZE) &&
                loadBin("/home/z3/Desktop/bt3/work/spike_state.bin", &st, sizeof(VU1State)))
            {
                std::fprintf(stderr, "[replay] entry pc=%u top=%u itop=%u vi10=%d vi2=%d vi15=%d\n",
                             st.pc, st.top & 0x3FFu, st.itop, st.vi[10], st.vi[2], st.vi[15]);
                m_vu1.reset();
                m_vu1.state() = st;
                m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                              m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                              m_gs, &m_memory, st.pc, st.top, st.itop, 262144);
                std::fprintf(stderr, "[replay] done (end pc=%u)\n", m_vu1.state().pc);
                // PS2X_REPLAY2: run the program a SECOND time (other double-buffer TOP) —
                // its opening XGKICK delivers the packet the first run built. This is how the
                // strip output actually reaches the GS in the live game.
                if (const char *r2 = std::getenv("PS2X_REPLAY2"); r2 && r2[0] && r2[0] != '0')
                {
                    const uint32_t top2 = (st.top & 0x3FFu) == 577u ? 141u : 577u;
                    m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                  m_gs, &m_memory, st.pc, top2, st.itop, 262144);
                    std::fprintf(stderr, "[replay] second run done (end pc=%u)\n", m_vu1.state().pc);
                }
                if (FILE *de = std::fopen("/home/z3/Desktop/bt3/work/replay_data_end.bin", "wb"))
                {
                    std::fwrite(m_memory.getVU1Data(), 1, PS2_VU1_DATA_SIZE, de);
                    std::fclose(de);
                    std::fprintf(stderr, "[replay] end-state VU data dumped\n");
                }
            }
            std::exit(0);
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE - kMainThreadStackReserve;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    ps2_game_overrides::applyMatching(*this, elfPath, m_cpuContext.pc);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    // Allow pointing the CDVD backend at a disc image via environment variable.
    if (const char *cdImageEnv = std::getenv("PS2X_CD_IMAGE"))
    {
        if (cdImageEnv[0] != '\0')
        {
            paths.cdImage = std::filesystem::path(cdImageEnv);
        }
    }

    setIoPaths(paths);
}

// Recompiled DBZP.BIN overlay (the game code, base 0x334c00). Defined in
// ps2xRuntime/src/runner_overlay/overlay_register.cpp. Declared here (not in the
// widely-included ps2_runtime.h) so adding the overlay does not force a full
// rebuild of the boot ELF's thousands of translation units.
extern const uint32_t g_ps2OverlayFunctionTableBase;
extern const uint32_t g_ps2OverlayFunctionTableEnd;
extern const uint32_t g_ps2OverlayFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2OverlayFunctionTable[];

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }

    // Same, for the recompiled DBZP.BIN overlay (game code) dense table.
    bool generatedOverlayTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2OverlayFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2OverlayFunctionTableBase || address >= g_ps2OverlayFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2OverlayFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2OverlayFunctionTableSlotCount;
    }

    // Resolve a guest PC against the boot ELF table first, then the overlay table.
    PS2Runtime::RecompiledFunction lookupGeneratedFunction(uint32_t address)
    {
        // [ksegfn] kseg0/kseg1 aliases of RAM (0x8xxxxxxx / 0xAxxxxxxx) are the same code on a PS2. BT3's loader path
        // calls through kseg0-form function pointers (JALR target 0x8026cbc8 = 0x26cbc8); unmasked they miss the tables,
        // fall to the overlay interpreter (which lacked lq/sq) and the load hangs with a garbage return address.
        if (address >= 0x80000000u && address < 0xC0000000u)
        {
            static std::atomic<uint32_t> s_n{0}; const uint32_t n = s_n.fetch_add(1u);
            if (n < 40u || (n % 5000u) == 0u) std::fprintf(stderr, "[ksegfn] #%u kseg-form function pointer 0x%08x -> 0x%08x\n", n, address, address & 0x1FFFFFFFu);
            address &= 0x1FFFFFFFu;
        }
        uint32_t slot = 0u;
        if (generatedFunctionTableSlot(address, slot))
        {
            PS2Runtime::RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
            if (fn != nullptr)
            {
                return fn;
            }
        }
        if (generatedOverlayTableSlot(address, slot))
        {
            PS2Runtime::RecompiledFunction fn = g_ps2OverlayFunctionTable[slot];
            if (fn != nullptr)
            {
                return fn;
            }
        }
        return nullptr;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    return lookupGeneratedFunction(address) != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

static thread_local R5900Context *t_bjCtx = nullptr; static thread_local uint8_t *t_bjRdram = nullptr;   // [thunkwatch]
PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    // Track the last successfully-dispatched in-code function so that when a garbage
    // (out-of-code, e.g. stack-address) target appears, we can name where it came from.
    static thread_local uint32_t s_lastValidDispatch = 0;
    static thread_local uint32_t s_prevValidDispatch = 0;

    if (RecompiledFunction fn = lookupGeneratedFunction(address))
    {
        s_prevValidDispatch = s_lastValidDispatch;
        s_lastValidDispatch = address;
        return fn;
    }

    if (!m_memory.isCodeAddress(address))
    {
        static std::atomic<int> s_bn{0};
        if (s_bn.fetch_add(1) < 8)
        {
            std::cerr << "[badjump] out-of-code target 0x" << std::hex << address
                      << " ; last valid fn entered = 0x" << s_lastValidDispatch
                      << " ; prev = 0x" << s_prevValidDispatch << std::dec << std::endl;
            {   // [thunkwatch] the sub_002722C0 case reloads $ra from [sp-16] (after its delay-slot addiu): show the slot
                if (t_bjCtx && t_bjRdram)
                {
                    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(t_bjCtx->r[29], 0));
                    uint32_t w[4] = {0, 0, 0, 0};
                    for (int k = 0; k < 4; ++k) { const uint32_t a = (sp - 16u + 4u * k) & 0x1FFFFFFFu; if (a + 4 <= 32u * 1024u * 1024u) std::memcpy(&w[k], t_bjRdram + a, 4); }
                    std::fprintf(stderr, "[badjump] sp=0x%x  [sp-16..sp)= %08x %08x %08x %08x  tid=%d ra=0x%x\n", sp, w[0], w[1], w[2], w[3], g_schedTid, static_cast<uint32_t>(_mm_extract_epi32(t_bjCtx->r[31], 0)));
                }
            }
            // Capture the C++ stack depth at the first bad jump: a huge depth == guest recursion
            // overflowed the (host) stack and clobbered the saved $ra. Small depth == a stray
            // write corrupted it. No gdb needed -> no timing perturbation.
#if !defined(_WIN32)
            void *bt[8192];
            const int n = backtrace(bt, 8192);
            std::cerr << "[badjump-bt] C++ stack frames=" << n << " (huge=>stack-overflow/deep recursion)" << std::endl;
            char **syms = backtrace_symbols(bt, n);
            if (syms)
            {
                for (int i = 0; i < n && i < 24; ++i) std::cerr << "   " << syms[i] << std::endl;
                free(syms);
            }
#endif
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " s0=0x" << getRegU32(ctx, 16) << " s1=0x" << getRegU32(ctx, 17) << " s2=0x" << getRegU32(ctx, 18) << " tid=" << g_schedTid   // [badjump] object regs
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;

        {   // [badjump] 64 bytes at s0 (the object whose method/vtable was bad) and 64 bytes at sp
            auto dump = [&](const char *what, uint32_t base) {
                std::ostringstream d; d << "[guest-branch:mem] " << what << "=0x" << std::hex << base << ":";
                for (uint32_t o = 0; o < 64u; o += 4u) { uint32_t w = 0; if (!readGuestU32Offset(base, o, w)) { d << " ??"; break; } d << ' ' << std::setw(8) << std::setfill('0') << w; }
                std::cerr << d.str() << std::endl;
            };
            dump("s0", getRegU32(ctx, 16)); dump("sp", sp);
        }        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

// ---- SUPER-TRACE (env PS2X_SUPERTRACE, F10 arms) ----
// Diff-trace rig for the "2nd+ super explodes at caster" hunt: while armed, every
// guest branch (except returns) records its target + (source->target) edge into
// fixed lock-free tables; on window expiry the winner thread flushes them to
// work/strace_<gen>.log. Capture one window around super #1 and one around super #2,
// then diff the function sets to find where the second cast's path diverges.
namespace
{
    constexpr uint32_t kStraceFnBits = 14, kStraceEdgeBits = 17;
    struct StraceFn { std::atomic<uint32_t> pc{0}; std::atomic<uint32_t> n{0}; std::atomic<uint32_t> seq{0}; };
    struct StraceEdge { std::atomic<uint64_t> key{0}; std::atomic<uint32_t> n{0}; };
    StraceFn g_straceFn[1u << kStraceFnBits];
    StraceEdge g_straceEdge[1u << kStraceEdgeBits];
    std::atomic<uint64_t> g_straceUntilMs{0};
    std::atomic<uint32_t> g_straceGen{0};
    std::atomic<uint32_t> g_straceSeq{0};
    std::atomic<uint32_t> g_straceFlushed{0};
    std::atomic<uint32_t> g_straceDropFn{0}, g_straceDropEdge{0};

    uint64_t straceNowMs()
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void straceFlush()
    {
        const uint32_t gen = g_straceGen.load();
        uint32_t expect = gen - 1u;
        if (!g_straceFlushed.compare_exchange_strong(expect, gen))
            return; // another thread is flushing (or already flushed) this gen
        char path[64];
        std::snprintf(path, sizeof(path), "work/strace_%u.log", gen);
        FILE *f = std::fopen(path, "w");
        if (!f)
        {
            std::snprintf(path, sizeof(path), "strace_%u.log", gen);
            f = std::fopen(path, "w");
        }
        if (f)
        {
            std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> fns; // (seq,(pc,n))
            for (auto &e : g_straceFn)
            {
                const uint32_t pc = e.pc.load();
                if (pc) fns.push_back({e.seq.load(), {pc, e.n.load()}});
            }
            std::sort(fns.begin(), fns.end());
            for (auto &e : fns)
                std::fprintf(f, "F 0x%06x %u seq=%u\n", e.second.first, e.second.second, e.first);
            for (auto &e : g_straceEdge)
            {
                const uint64_t k = e.key.load();
                if (k) std::fprintf(f, "E 0x%06x 0x%06x %u\n", (uint32_t)(k >> 32), (uint32_t)k, e.n.load());
            }
            std::fclose(f);
            std::fprintf(stderr, "[strace] flushed %s: %zu fns (dropFn=%u dropEdge=%u)\n",
                         path, fns.size(), g_straceDropFn.load(), g_straceDropEdge.load());
        }
        for (auto &e : g_straceFn) { e.pc.store(0); e.n.store(0); e.seq.store(0); }
        for (auto &e : g_straceEdge) { e.key.store(0); e.n.store(0); }
        g_straceDropFn.store(0);
        g_straceDropEdge.store(0);
    }

    void straceRecord(uint32_t targetPc, uint32_t sourcePc)
    {
        const uint64_t until = g_straceUntilMs.load(std::memory_order_relaxed);
        if (until == 0u) return;
        if (straceNowMs() > until)
        {
            g_straceUntilMs.store(0u);
            straceFlush();
            return;
        }
        {
            constexpr uint32_t mask = (1u << kStraceFnBits) - 1u;
            uint32_t h = ((targetPc >> 2) * 2654435761u) & mask;
            bool done = false;
            for (int i = 0; i < 64 && !done; ++i, h = (h + 1u) & mask)
            {
                const uint32_t cur = g_straceFn[h].pc.load(std::memory_order_relaxed);
                if (cur == targetPc)
                {
                    g_straceFn[h].n.fetch_add(1, std::memory_order_relaxed);
                    done = true;
                }
                else if (cur == 0u)
                {
                    uint32_t exp = 0u;
                    if (g_straceFn[h].pc.compare_exchange_strong(exp, targetPc))
                    {
                        g_straceFn[h].seq.store(g_straceSeq.fetch_add(1u) + 1u);
                        g_straceFn[h].n.fetch_add(1u);
                        done = true;
                    }
                    else if (exp == targetPc) // lost the race to the same pc
                    {
                        g_straceFn[h].n.fetch_add(1, std::memory_order_relaxed);
                        done = true;
                    }
                }
            }
            if (!done) g_straceDropFn.fetch_add(1, std::memory_order_relaxed);
        }
        {
            constexpr uint32_t mask = (1u << kStraceEdgeBits) - 1u;
            const uint64_t key = ((uint64_t)sourcePc << 32) | targetPc;
            uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
            bool done = false;
            for (int i = 0; i < 64 && !done; ++i, h = (h + 1u) & mask)
            {
                const uint64_t cur = g_straceEdge[h].key.load(std::memory_order_relaxed);
                if (cur == key)
                {
                    g_straceEdge[h].n.fetch_add(1, std::memory_order_relaxed);
                    done = true;
                }
                else if (cur == 0u)
                {
                    uint64_t exp = 0u;
                    if (g_straceEdge[h].key.compare_exchange_strong(exp, key))
                    {
                        g_straceEdge[h].n.fetch_add(1u);
                        done = true;
                    }
                    else if (exp == key)
                    {
                        g_straceEdge[h].n.fetch_add(1, std::memory_order_relaxed);
                        done = true;
                    }
                }
            }
            if (!done) g_straceDropEdge.fetch_add(1, std::memory_order_relaxed);
        }
    }
} // namespace

// Frame tick for generated-code hooks (g_displaySwapCounter itself has internal linkage).
uint64_t ps2xDisplaySwapCount()
{
    return g_displaySwapCounter.load(std::memory_order_relaxed);
}

void ps2xSuperTraceArm()
{
    static const uint64_t s_durMs = []() {
        const char *v = std::getenv("PS2X_SUPERTRACE");
        if (v) { const long n = std::atol(v); if (n > 1) return (uint64_t)n; }
        return (uint64_t)6000;
    }();
    if (g_straceGen.load() != g_straceFlushed.load())
    {
        g_straceUntilMs.store(0u);
        straceFlush(); // re-armed before the previous window flushed: flush it now
    }
    g_straceGen.fetch_add(1u);
    g_straceUntilMs.store(straceNowMs() + s_durMs);
    std::fprintf(stderr, "[strace] F10 — armed %llums, gen=%u\n",
                 (unsigned long long)s_durMs, g_straceGen.load());
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    t_bjCtx = ctx; t_bjRdram = rdram;   // [thunkwatch] for the [badjump] slot dump
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // SUPER-TRACE tap (PS2X_SUPERTRACE + F10): see the rig above dispatchGuestBranch.
    {
        static const bool s_stOn = []() { const char *v = std::getenv("PS2X_SUPERTRACE"); return v && v[0] && v[0] != '0'; }();
        if (s_stOn && kind != GuestBranchKind::Return)
            straceRecord(targetPc, sourcePc);
    }

    // Hot-call cache: recent (targetPc -> recompiled fn), per thread. The game's tight
    // service/critical-section loops call a handful of static functions millions of times
    // each; caching the resolved fn skips lookupFunction()'s dispatch-history push + table
    // lookup + hasFunction() on those. Only static-table fns are cached (never unloaded).
    static thread_local uint32_t s_hcPc[512] = {};
    static thread_local RecompiledFunction s_hcFn[512] = {};
    const uint32_t hcI = (targetPc >> 2) & 0x1FFu;

    // BT3 sound-service ack (env PS2X_BT3_SNDACK): the sound service thread
    // FUN_0026d070 is asleep (no IOP wakes it), so it never clears the cross-thread
    // handshake flag DAT_002c9f54 that FUN_0026cc38 spins on -- up to 200,000,000
    // iterations, then it errors out ("adxm_goto_svr_border"). That spin is the
    // popup/menu frame stall (profiler hot pc 0x26cd00). Stand in for the service
    // thread and clear the flag when set, exactly as FUN_0026d070 would. This is a
    // PURE memory write (no EE function call, no DMA/GS) so it cannot corrupt the
    // framebuffer the way running the op body from the pump did (SNDDISP -> pink).
    // Runs on all guest threads (throttled) since the spinning thread may not be main.
    {
        static const bool s_sndAck = [](){ const char *v = std::getenv("PS2X_BT3_SNDACK"); return v && v[0] && v[0] != '0'; }();
        if (s_sndAck)
        {
            static thread_local uint32_t s_ackCtr = 0u;
            if ((++s_ackCtr & 0x3Fu) == 0u)
            {
                uint32_t *p = reinterpret_cast<uint32_t *>(rdram + (0x2c9f54u & 0x01FFFFFFu));
                if (*p == 1u) *p = 0u;
            }
        }
    }

    // PROFILE (env PS2X_PROFILE): measure guest branch rate + hot pc across ALL
    // guest threads (the main thread mostly blocks; the real work is elsewhere).
    // Prints every ~4M branches with total branches/sec and the two hottest pcs.
    {
        static const bool s_prof = [](){ const char *v = std::getenv("PS2X_PROFILE"); return v && v[0] && v[0] != '0'; }();
        if (s_prof)
        {
            static std::atomic<uint64_t> s_gpc{0};
            static std::atomic<uint32_t> s_ghist[0x4000];
            static std::mutex s_pMx;
            static std::chrono::steady_clock::time_point s_gt = std::chrono::steady_clock::now();
            uint64_t n = s_gpc.fetch_add(1, std::memory_order_relaxed) + 1u;
            s_ghist[(targetPc >> 8) & 0x3FFFu].fetch_add(1, std::memory_order_relaxed);
            if ((n & 0x3FFFFFu) == 0u)
            {
                std::lock_guard<std::mutex> lk(s_pMx);
                auto now = std::chrono::steady_clock::now();
                double sec = std::chrono::duration<double>(now - s_gt).count();
                s_gt = now;
                uint32_t topIdx = 0, topN = 0, top2Idx = 0, top2N = 0;
                for (uint32_t i = 0; i < 0x4000u; ++i)
                {
                    uint32_t c = s_ghist[i].exchange(0u, std::memory_order_relaxed);
                    if (c > topN) { top2N = topN; top2Idx = topIdx; topN = c; topIdx = i; }
                    else if (c > top2N) { top2N = c; top2Idx = i; }
                }
                std::cerr << "[prof] " << (uint64_t)(0x400000 / sec) << " branches/sec, hottest=0x"
                          << std::hex << (topIdx << 8) << std::dec << " (" << topN << ") 2nd=0x"
                          << std::hex << (top2Idx << 8) << std::dec << " (" << top2N << ")" << std::endl;
            }
        }
    }

    // PROBE (env PS2X_FA0PROBE): every call to func_285FA0(slot) = the table-B
    // handler dispatcher. Logs which sound slots are dispatched -- if slot 6 (the
    // pending-op handler 0x26cd20 -> func_26CC38) is never dispatched, that's the
    // exact missing link that stops the sound-init countdown from decrementing.
    {
        static const bool s_fa0 = [](){ const char *v = std::getenv("PS2X_FA0PROBE"); return v && v[0] && v[0] != '0'; }();
        // Completion/decrement/registration functions: 26d9f0/26e628/26e848=decrement,
        // 26e388=decrement thunk, 26e740=0x31ec90 registrar, 26e7c8=state setter,
        // 26cc38=op body, 26cd20=slot-6 handler.
        if (s_fa0 && (targetPc == 0x0026d9f0u || targetPc == 0x0026e628u || targetPc == 0x0026e848u
                      || targetPc == 0x0026e388u || targetPc == 0x0026e740u || targetPc == 0x0026e7c8u
                      || targetPc == 0x0026cc38u || targetPc == 0x0026cd20u))
        {
            static std::atomic<uint32_t> s_fn{0};
            if (s_fn.fetch_add(1) < 300u)
                std::cerr << "[fa0] hit=0x" << std::hex << targetPc
                          << " a0=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]))
                          << " ra=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])) << std::dec << std::endl;
        }
    }

    // Central interrupt-tick pump (see rationale in dispatchLoop's constants).
    // MUST live here, not only at dispatchLoop's top: BT3's CDVD driver spins in
    // deeply-nested wait loops (e.g. func_23D0E0 -> ... -> func_27e910) that never
    // unwind back to dispatchLoop, so a top-of-loop pump starves. dispatchGuestBranch
    // is hit on every guest call/return at any depth, so pumping here lets those
    // nested waits observe read/stream completion. Main thread + reentrancy gated.
    if (ctx == &m_cpuContext)
    {
        static const bool s_tickPumpEnabled = []() { const char *v = std::getenv("PS2X_TICKPUMP"); return !(v && v[0] == '0'); }();
        static thread_local uint64_t s_tickCounter = 0u;
        static thread_local bool s_pumping = false;
        // Pump interval: running the (expensive) tick FUN_0028a3b0 every 2048 branches
        // is huge redundant overhead (the game calls it too). Default 1048576 (1M);
        // tunable via PS2X_TICKINTERVAL. Higher = faster main thread.
        static const uint64_t kTickPumpInterval = [](){ const char *v = std::getenv("PS2X_TICKINTERVAL"); uint64_t n = (v && v[0]) ? std::strtoull(v,nullptr,10) : 131072ull; return n < 256ull ? 256ull : n; }();
        if (s_tickPumpEnabled && !s_pumping && (++s_tickCounter % kTickPumpInterval) == 0u && hasFunction(0x0028a3b0u))
        {
            s_pumping = true;
            static const uint32_t kTickPcs[] = {0x0028a3b0u, 0x0028a530u};
            // The game's own FUN_00264b18 boot loop ticks FUN_0028a530 (the CRI ADX
            // file-read driver) every frame; the pump running it out-of-band on a
            // scratch context corrupts the ADX partition state and STALLS the AFS load
            // (adxf_GetPtStat returns the wrong partition). DEFAULT: skip 0x0028a530 in
            // the pump so only the game drives the ADX reader. PS2X_PUMP_ADX=1 restores
            // the old buggy both-tick behavior for debugging.
            static const bool s_noAdxPump = [](){ const char *v = std::getenv("PS2X_PUMP_ADX"); return !(v && v[0] && v[0] != '0'); }();
            GuestExecutionScope pumpScope(this);
            for (uint32_t tickPc : kTickPcs)
            {
                if (ctx != &m_cpuContext || !hasFunction(tickPc) || (s_noAdxPump && tickPc == 0x0028a530u))   // [pumpmain]
                {
                    continue;
                }
                R5900Context tctx = *ctx;
                tctx.r[31] = _mm_setzero_si128();
                tctx.pc = tickPc;
                uint32_t steps = 0u;
                while (tctx.pc != 0u && steps++ < 2000000u)
                {
                    RecompiledFunction step = lookupFunction(tctx.pc);
                    if (!step)
                    {
                        break;
                    }
                    step(rdram, &tctx, this);
                }
            }
            s_pumping = false;
        }

        // Opening-movie / AFS-load driver. FUN_0028a530 (the CRI ADX file-read driver)
        // is ticked every frame by the game's BOOT loop but NOT by the opening-movie
        // sequencer (FUN_0035de58), so the AFS load stalls at ~8 sectors -> infinite
        // loading screen. Callers invoke it via DIRECT C++ calls (not the table), so it
        // can't be hooked/detected; instead tick it here once per VSYNC on the MAIN
        // thread. This is hardware rate: unlike PS2X_PUMP_ADX (every ~131072 branches =
        // hundreds/frame, which corrupts the ADX partition), ~2x/frame during boot
        // (game + this) stays far below the corrupting rate and is tolerated, while the
        // movie phase (game doesn't tick it) gets the 1/frame it needs. Env PS2X_ADXVSYNC=0
        // disables. tctx is copied from the main-thread ctx so gp/sp are correct.
        {
            static const bool s_adxVsyncPump = []() { const char *v = std::getenv("PS2X_ADXVSYNC"); return !(v && v[0] == '0'); }();
            static thread_local uint64_t s_lastAdxPumpVsync = ~0ull;
            // Only test the vsync change every 512 branches -- checking (even the lock-free
            // GetCurrentVSyncTick + hasFunction) on EVERY guest branch is millions of calls
            // per frame. Once/512-branches is still far finer than the once/vsync we act on.
            static thread_local uint32_t s_adxCtr = 0u;
            if (s_adxVsyncPump && ((++s_adxCtr & 0x1FFu) == 0u) && s_tickPumpEnabled && !s_pumping && hasFunction(0x0028a530u))
            {
                const uint64_t vs = ps2_syscalls::GetCurrentVSyncTick();
                if (vs != s_lastAdxPumpVsync)
                {
                    s_lastAdxPumpVsync = vs;
                    {
                        static const bool s_lg = std::getenv("PS2X_OVLOG") != nullptr;
                        if (s_lg)
                        {
                            static uint32_t tc = 0u;
                            uint32_t adxf = *reinterpret_cast<const uint32_t *>(rdram + (0x2e6370u & 0x01FFFFFFu));
                            int st = -1, al = -1, to = -1;
                            if (adxf)
                            {
                                const uint32_t a = adxf & 0x01FFFFFFu;
                                st = *reinterpret_cast<const uint8_t *>(rdram + a + 1u);
                                al = *reinterpret_cast<const int *>(rdram + a + 0x18u);
                                to = *reinterpret_cast<const int *>(rdram + a + 0xcu);
                            }
                            if (tc < 40u || (tc % 120u) == 0u)
                                std::cerr << "[adxpump] tick#" << tc << " vsync=" << vs << " adxf=0x" << std::hex << adxf
                                          << std::dec << " state=" << st << " already=" << al << " total=" << to << std::endl;
                            ++tc;
                        }
                    }
                    s_pumping = true;
                    GuestExecutionScope pumpScope(this);
                    // Burst: each ADX-driver tick only nudges an in-flight read forward a
                    // little, so at once/vsync the AFS/movie load crawls. Run it repeatedly
                    // WHILE the current partition is actively reading (state byte == 2) and
                    // STOP the instant it isn't. This completes the read in ~one vsync, and
                    // -- crucially -- never ticks a done/idle partition (state 3), so it
                    // can't over-tick/corrupt the way the old every-N-branches pump did.
                    // Each burst iteration runs BOTH drivers: FUN_0028a3b0 (the CD read-
                    // completion tick -- advances the low-level disc read the ADX driver is
                    // waiting on) THEN FUN_0028a530 (the ADX file-read driver -- consumes the
                    // completed sectors and issues the next read). Running only the latter
                    // just issues a read and spins, since nothing completes it in-burst.
                    static const uint32_t kBurstPcs[] = {0x0028a3b0u, 0x0028a530u};
                    // CAP kept modest: a stalled/continuous stream (e.g. title-screen BGM)
                    // stays state==2 forever, so an unbounded burst would spin the main
                    // thread every vsync and FREEZE the game. We break as soon as the read
                    // stops making progress (already-read counter doesn't advance) so it
                    // only bursts through a genuinely in-flight finite load.
                    auto readAlready = [&]() -> uint32_t {
                        const uint32_t h = *reinterpret_cast<const uint32_t *>(rdram + (0x2e6370u & 0x01FFFFFFu));
                        if (h == 0u) return 0xffffffffu;
                        return *reinterpret_cast<const uint32_t *>(rdram + (h & 0x01FFFFFFu) + 0x18u);
                    };
                    uint32_t prevAlready = readAlready();
                    uint32_t noProg = 0u;
                    for (uint32_t burst = 0u; burst < 2048u; ++burst)
                    {
                        for (uint32_t bp : kBurstPcs)
                        {
                            if (ctx != &m_cpuContext || !hasFunction(bp))   // [pumpmain]
                            {
                                continue;
                            }
                            R5900Context tctx = *ctx;
                            tctx.r[31] = _mm_setzero_si128();
                            tctx.pc = bp;
                            uint32_t steps = 0u;
                            while (tctx.pc != 0u && steps++ < 2000000u)
                            {
                                RecompiledFunction step = lookupFunction(tctx.pc);
                                if (!step)
                                {
                                    break;
                                }
                                step(rdram, &tctx, this);
                            }
                        }
                        const uint32_t adxf = *reinterpret_cast<const uint32_t *>(rdram + (0x2e6370u & 0x01FFFFFFu));
                        if (adxf == 0u || *reinterpret_cast<const uint8_t *>(rdram + (adxf & 0x01FFFFFFu) + 1u) != 2u)
                        {
                            break; // partition idle/done
                        }
                        const uint32_t already = readAlready();
                        if (already == prevAlready)
                        {
                            // Tolerate brief pauses (waiting for a CD read to complete) but bail
                            // if it's genuinely stalled (>=256 iters no progress) so a stuck /
                            // continuous title-screen stream can't starve the main thread.
                            if (++noProg >= 256u) break;
                        }
                        else
                        {
                            noProg = 0u;
                            prevAlready = already;
                        }
                    }
                    s_pumping = false;
                }
            }
        }

        // Title-screen freeze workaround (env PS2X_GSBUSYCLR): the title loop
        // FUN_00337d70 spins on `*(*(0x2ff10c)+0x14) & 0x100` -- a GS/DMA-busy bit a
        // DMA-done interrupt should clear. In the HLE that interrupt doesn't fire, so
        // the bit stays set: the title renders but never completes a frame or reads
        // input. The GS DMA is already processed here, so clear the bit so the loop
        // can advance. Main-thread only.
        if (ctx == &m_cpuContext)
        {
            static const bool s_gsBusyClr = []() { const char *v = std::getenv("PS2X_GSBUSYCLR"); return !(v && v[0] == '0'); }();
            static thread_local uint32_t s_gsCtr = 0u;
            if (s_gsBusyClr && ((++s_gsCtr & 0x3Fu) == 0u)) // every 64 branches is plenty
            {
                const uint32_t base = *reinterpret_cast<const uint32_t *>(rdram + (0x2ff10cu & 0x01FFFFFFu));
                if (base != 0u)
                {
                    uint32_t *flag = reinterpret_cast<uint32_t *>(rdram + ((base + 0x14u) & 0x01FFFFFFu));
                    *flag &= ~0x100u;
                }
            }
        }

        // EMPIRICAL (env PS2X_SNDGATE): the game spins in FUN_0026cd88 on
        // `while (*(u64*)0x2c9fc8 == 0)` -- the sound-ready flag normally set by
        // FUN_0026d9a0 once IOP sound-init completes. That handshake is stubbed, so
        // the flag never flips and boot stalls after the sound preload (lbn ceilings
        // at 0xaec5a). Force the flag nonzero from the main-thread tick (which DOES
        // run during the spin, unlike the pad path) to test whether this is the gate.
        static const bool s_sndGate = [](){ const char *v = std::getenv("PS2X_SNDGATE"); return v && v[0] && v[0] != '0'; }();
        if (s_sndGate)
        {
            // Replicate FUN_0026d9a0 exactly: 6 u64 flags at 0x2c9fc8 stride 0x10.
            for (uint32_t a = 0x2c9fc8u; a <= 0x2ca018u; a += 0x10u)
                *reinterpret_cast<uint64_t *>(rdram + (a & 0x01FFFFFFu)) = 1u;
        }

        // DIAGNOSTIC (env PS2X_ADXLOG): the REAL banner gate is the CRI ADX/AFS
        // partition read (adxf_GetPtStat must return 3). Log the ADX driver state on
        // change: 0x2e6378=adxf status, 0x2e6380=driver state, 0x2e6370=partition ptr,
        // 0x2e6374=ptid, and the partition's own status byte *(part+1).
        static const bool s_adxLog = [](){ const char *v = std::getenv("PS2X_ADXLOG"); return v && v[0] && v[0] != '0'; }();
        if (s_adxLog)
        {
            uint32_t st  = *reinterpret_cast<uint32_t *>(rdram + (0x2e6378u & 0x01FFFFFFu));
            uint32_t drv = *reinterpret_cast<uint32_t *>(rdram + (0x2e6380u & 0x01FFFFFFu));
            uint32_t part= *reinterpret_cast<uint32_t *>(rdram + (0x2e6370u & 0x01FFFFFFu));
            uint32_t ptid= *reinterpret_cast<uint32_t *>(rdram + (0x2e6374u & 0x01FFFFFFu));
            uint32_t pstat = (part && part < 0x02000000u) ? rdram[(part + 1u) & 0x01FFFFFFu] : 0xffu;
            static thread_local uint32_t s_lastAdx = 0xffffffffu;
            uint32_t key = (st & 0xff) | ((drv & 0xff) << 8) | ((pstat & 0xff) << 16) | ((ptid & 0xff) << 24);
            if (key != s_lastAdx)
            {
                s_lastAdx = key;
                std::cerr << "[adx] status(2e6378)=" << (int)st << " driver(2e6380)=" << (int)(int32_t)drv
                          << " part(2e6370)=0x" << std::hex << part << std::dec << " ptid=" << (int)ptid
                          << " part_statusbyte=" << (int)pstat << std::endl;
            }
        }

        // DIAGNOSTIC (env PS2X_SNDLOG): log the sound-init countdown 0x2c9f14 and
        // state 0x2d184c whenever either changes, to see where init stalls.
        static const bool s_sndLog = [](){ const char *v = std::getenv("PS2X_SNDLOG"); return v && v[0] && v[0] != '0'; }();
        if (s_sndLog)
        {
            uint32_t cnt = *reinterpret_cast<uint32_t *>(rdram + (0x2c9f14u & 0x01FFFFFFu));
            uint32_t st  = *reinterpret_cast<uint32_t *>(rdram + (0x2d184cu & 0x01FFFFFFu));
            // Sound service thread (0x26d070) heartbeat counter at 0x2c9f90 (u64):
            // increments each loop iteration. Throttled log to see if it advances.
            static thread_local uint64_t s_hbTick = 0u;
            if ((++s_hbTick % 400u) == 0u)
            {
                uint64_t hb = *reinterpret_cast<uint64_t *>(rdram + (0x2c9f90u & 0x01FFFFFFu));
                std::cerr << "[sndhb] 0x2c9f90(sndthread-heartbeat)=" << hb << " count=" << cnt << std::endl;
            }
            static thread_local uint32_t s_lastCnt = 0xffffffffu, s_lastSt = 0xffffffffu;
            // Wake-chain gate flags for the service thread (0x2c9fb8=target tid):
            uint32_t g18 = *reinterpret_cast<uint32_t *>(rdram + (0x2c9f18u & 0x01FFFFFFu));
            uint32_t g6c = *reinterpret_cast<uint32_t *>(rdram + (0x2c9f6cu & 0x01FFFFFFu));
            uint32_t wtid = *reinterpret_cast<uint32_t *>(rdram + (0x2c9fb8u & 0x01FFFFFFu));
            uint32_t g54 = *reinterpret_cast<uint32_t *>(rdram + (0x2c9f54u & 0x01FFFFFFu));
            static thread_local uint32_t s_lastGate = 0xffffffffu;
            uint32_t gateKey = g18 ^ (g6c << 4) ^ (wtid << 8) ^ (g54 << 16);
            if (gateKey != s_lastGate)
            {
                s_lastGate = gateKey;
                std::cerr << "[sndgate] 0x2c9f18=" << g18 << " 0x2c9f6c=" << g6c
                          << " 0x2c9fb8(waketid)=" << wtid << " 0x2c9f54=" << g54
                          << " count=" << cnt << std::endl;
            }
            if (cnt != s_lastCnt || st != s_lastSt)
            {
                s_lastCnt = cnt; s_lastSt = st;
                std::cerr << "[sndlog] 0x2c9f14(count)=" << cnt << " 0x2d184c(state)=" << st << std::endl;
                // On count going nonzero, dump the slot-6 handler table (0x321750,
                // 5 x {func@0,arg@4} stride 0xC) that the sound service thread runs,
                // to see if any completion handler is registered.
                if (cnt == 1u)
                {
                    std::cerr << "[sndlog] slot-6 handler table @0x321750:";
                    for (uint32_t e = 0; e < 5u; ++e)
                    {
                        uint32_t base = 0x321750u + e * 0xCu;
                        uint32_t fn  = *reinterpret_cast<uint32_t *>(rdram + (base & 0x01FFFFFFu));
                        uint32_t arg = *reinterpret_cast<uint32_t *>(rdram + ((base + 4u) & 0x01FFFFFFu));
                        std::cerr << " [" << e << "] fn=0x" << std::hex << fn << " arg=0x" << arg << std::dec;
                    }
                    std::cerr << std::endl;
                }
            }
        }

        // EMPIRICAL (env PS2X_SNDWAKE): nothing ever wakes the sound service thread
        // (entry 0x26d070) after its first SleepThread, so its init never completes
        // and the countdown 0x2c9f14 stays at 1. Periodically wake it so it runs its
        // real loop iterations (non-destructive: only nudges it when actually asleep).
        static const bool s_sndWake = [](){ const char *v = std::getenv("PS2X_SNDWAKE"); return v && v[0] && v[0] != '0'; }();
        if (s_sndWake)
        {
            // Wake the sleeping sound service thread on every pump fire (no-op when
            // already awake). Waking ALL sound threads was tested and corrupts init
            // timing (froze pre-preload) -- tid6-only is the clean, non-destructive form.
            ps2_syscalls::bt3WakeThreadByEntry(0x0026d070u);
        }

        // EMPIRICAL (env PS2X_SNDDISP): the pending-op handler in table-B slot 6
        // (0x26cd20 -> func_26CC38) is registered with an open gate but never
        // dispatched (proven: 0 dispatch hits). Dispatch it the way the game's own
        // dispatcher does -- call func_285FA0(6) -- so the REAL op body runs its
        // handshake with the (SNDWAKE-running) sound thread. Non-destructive: runs
        // the game's own registered code. Repeats each pump like a real sound tick.
        static const bool s_sndDisp = [](){ const char *v = std::getenv("PS2X_SNDDISP"); return v && v[0] && v[0] != '0'; }();
        if (s_sndDisp && !s_pumping)
        {
            static thread_local uint64_t s_dispTick = 0u;
            // Only once slot 6 is registered (handler non-zero) and init settled.
            uint32_t slot6 = *reinterpret_cast<uint32_t *>(rdram + (0x321810u & 0x01FFFFFFu));
            if (ctx == &m_cpuContext && slot6 != 0u && ++s_dispTick > 3000u && hasFunction(0x00285fa0u))   // [pumpmain]
            {
                s_pumping = true;
                GuestExecutionScope dispScope(this);
                R5900Context tctx = *ctx;
                tctx.r[31] = _mm_setzero_si128();
                tctx.r[4] = _mm_cvtsi32_si128(6); // a0 = slot 6
                tctx.pc = 0x00285fa0u;
                uint32_t steps = 0u;
                while (tctx.pc != 0u && steps++ < 4000000u)
                {
                    RecompiledFunction step = lookupFunction(tctx.pc);
                    if (!step) break;
                    step(rdram, &tctx, this);
                }
                s_pumping = false;
            }
        }

        // AUDIT (env PS2X_SNDAUDIT): once, after init has settled (many pump fires),
        // dump every sound-engine callback slot/table + key state so we can see which
        // registration is empty-but-should-be-populated (the missing completion link).
        static const bool s_sndAudit = [](){ const char *v = std::getenv("PS2X_SNDAUDIT"); return v && v[0] && v[0] != '0'; }();
        if (s_sndAudit)
        {
            static thread_local uint64_t s_auditTick = 0u;
            static thread_local bool s_audited = false;
            if (!s_audited && ++s_auditTick > 3000u)
            {
                s_audited = true;
                // Scan guest RAM for the completion-callback addresses to find WHERE
                // they are registered (which memory slot / table dispatches them).
                {
                    const uint32_t targets[] = {0x0026e388u, 0x0026e628u, 0x0026d9f0u, 0x0026e740u, 0x0026e848u, 0x0026cd20u};
                    for (uint32_t off = 0; off < 0x02000000u; off += 4u)
                    {
                        uint32_t v = *reinterpret_cast<uint32_t *>(rdram + off);
                        for (uint32_t t : targets)
                            if (v == t) { std::cerr << "[cbscan] value 0x" << std::hex << t << " stored at guest 0x" << off << std::dec << std::endl; }
                    }
                    std::cerr << "[cbscan] done" << std::endl;
                }
                auto rd = [&](uint32_t a) -> uint32_t { return *reinterpret_cast<uint32_t *>(rdram + (a & 0x01FFFFFFu)); };
                std::cerr << "[sndaudit] count=" << rd(0x2c9f14) << " state(2d184c)=" << rd(0x2d184c)
                          << " hs(2c9f54)=" << rd(0x2c9f54) << " gate2c9fbc=0x" << std::hex << rd(0x2c9fbc)
                          << " 2c9f38=0x" << rd(0x2c9f38) << " 2c9f20=0x" << rd(0x2c9f20) << std::dec << std::endl;
                std::cerr << "[sndaudit] single-slot 0x31ec90 fn=0x" << std::hex << rd(0x31ec90)
                          << " arg=0x" << rd(0x31ec94) << std::dec << std::endl;
                // Table A (func_286050): base 0x3215a0, group*0x48, 5 x {fn@0,arg@4} stride 0xC
                for (uint32_t g = 5; g <= 7; ++g) {
                    std::cerr << "[sndaudit] tableA grp" << g << " @0x" << std::hex << (0x3215a0u + g*0x48u) << ":";
                    for (uint32_t e = 0; e < 5; ++e) { uint32_t b = 0x3215a0u + g*0x48u + e*0xCu; std::cerr << " fn=0x" << rd(b) << "/arg=0x" << rd(b+4); }
                    std::cerr << std::dec << std::endl;
                }
                // Table B (func_285F48/285FA0): base 0x3217e0, slot*8, {fn,arg}
                std::cerr << "[sndaudit] tableB @0x3217e0:";
                for (uint32_t s = 0; s < 12; ++s) { uint32_t b = 0x3217e0u + s*8u; std::cerr << " [" << std::dec << s << "]fn=0x" << std::hex << rd(b); }
                std::cerr << std::dec << std::endl;
            }
        }

        // EMPIRICAL (env PS2X_SNDKICK): the sound-init countdown 0x2c9f14 sticks at 1
        // because the sound service thread (0x26d070) never completes its one pending
        // init op (a stubbed IOP sound handler never signals). Once the main thread is
        // provably in the sound-ready spin (FUN_0026cd88 region ~0x26cc00-0x26ce00) with
        // count==1, the preload is done and everything else is ready -- so run the REAL
        // completion handler func_26D9F0 ONCE. Unlike faking the flags, this decrements
        // the counter and runs the genuine post-init (func_26D9A0/26DB58/26DCF8/...).
        static const bool s_sndKick = [](){ const char *v = std::getenv("PS2X_SNDKICK"); return v && v[0] && v[0] != '0'; }();
        if (s_sndKick && !s_pumping)
        {
            static thread_local bool s_sawSndWait = false;
            static thread_local bool s_kicked = false;
            static thread_local uint64_t s_kickStable = 0u;
            if (ctx->pc >= 0x26cc00u && ctx->pc < 0x26ce00u) s_sawSndWait = true;
            uint32_t cnt = *reinterpret_cast<uint32_t *>(rdram + (0x2c9f14u & 0x01FFFFFFu));
            // Fire the FULL completion FUN_0026e628 (env PS2X_SNDKICKF selects it over
            // the lesser func_26D9F0) only after count has been stably 1 for a while
            // (init settled). FUN_0026e628 does the extra post-init that the lesser
            // one skips, aiming for a clean post-banner state.
            static const bool s_full = [](){ const char *v = std::getenv("PS2X_SNDKICKF"); return v && v[0] && v[0] != '0'; }();
            if (cnt == 1u && s_sawSndWait) ++s_kickStable; else s_kickStable = 0u;
            const uint32_t kickPc = s_full ? 0x0026e628u : 0x0026d9f0u;
            // Wait ~PS2X_KICKDELAY seconds (default 150) so the boot sound preload has
            // time to load its data before we complete init out-of-sequence.
            static const uint64_t s_kickDelaySec = [](){ const char *v = std::getenv("PS2X_KICKDELAY"); return (v && v[0]) ? std::strtoull(v, nullptr, 10) : 150ull; }();
            static const std::chrono::steady_clock::time_point s_startTp = std::chrono::steady_clock::now();
            const uint64_t elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s_startTp).count();
            if (ctx == &m_cpuContext && !s_kicked && elapsedSec >= s_kickDelaySec && cnt == 1u && s_sawSndWait && hasFunction(kickPc))   // [pumpmain]
            {
                s_kicked = true;
                s_pumping = true;
                std::cerr << "[sndkick] running 0x" << std::hex << kickPc << std::dec << " to complete sound init (count was 1)" << std::endl;
                GuestExecutionScope kickScope(this);
                R5900Context tctx = *ctx;
                tctx.r[31] = _mm_setzero_si128();
                tctx.pc = kickPc;
                uint32_t steps = 0u;
                while (tctx.pc != 0u && steps++ < 2000000u)
                {
                    RecompiledFunction step = lookupFunction(tctx.pc);
                    if (!step) break;
                    step(rdram, &tctx, this);
                }
                s_pumping = false;
            }
        }
    }
    else
    {
        // FINE-GRAINED sound-init tracer (env PS2X_SNDTRACE): runs per guest branch
        // on the sound worker threads, so it catches the func_26CC38<->tid6 handshake
        // flag 0x2c9f54 transients the coarse main-thread pump misses. Logs the
        // countdown 0x2c9f14, handshake flag 0x2c9f54, and state 0x2d184c on change.
        static const bool s_sndTrace = [](){ const char *v = std::getenv("PS2X_SNDTRACE"); return v && v[0] && v[0] != '0'; }();
        if (s_sndTrace)
        {
            uint32_t cnt = *reinterpret_cast<uint32_t *>(rdram + (0x2c9f14u & 0x01FFFFFFu));
            uint32_t hs  = *reinterpret_cast<uint32_t *>(rdram + (0x2c9f54u & 0x01FFFFFFu));
            uint32_t stt = *reinterpret_cast<uint32_t *>(rdram + (0x2d184cu & 0x01FFFFFFu));
            static std::atomic<uint32_t> s_lastKey{0xffffffffu};
            uint32_t key = (cnt & 0xf) | ((hs & 0xf) << 4) | ((stt & 0xf) << 8);
            uint32_t prev = s_lastKey.exchange(key);
            if (key != prev)
            {
                static std::atomic<uint32_t> s_n{0};
                if (s_n.fetch_add(1) < 400u)
                    std::cerr << "[sndtrace] count=" << cnt << " handshake(0x2c9f54)=" << hs
                              << " state(0x2d184c)=" << stt << " pc=0x" << std::hex << targetPc << std::dec << std::endl;
            }
        }

        // Nested busy-wait fairness for non-main (sound/service) guest threads.
        // A guest thread can spin in a wait loop that never unwinds back to
        // dispatchLoop -- e.g. the sound thread at 0x26cd88 does
        // `while (*0x2c9fc8 == 0) { counter++; }` -- holding the single
        // guest-execution lock the whole time and STARVING the main thread (and
        // whichever thread must set that flag). dispatchLoop's top-of-loop
        // fairness yield never fires because the spin never returns there;
        // dispatchGuestBranch IS hit every iteration, so release the lock + yield
        // here periodically to let other guest threads make progress.
        static thread_local uint64_t s_nestedFairness = 0u;
        constexpr uint64_t kNestedFairnessInterval = 256u;
        if ((++s_nestedFairness % kNestedFairnessInterval) == 0u)
        {
            if (schedDbgEnabled() && (s_nestedFairness % (256u*4000u)) == 0u)
                std::cerr << "[sched-nf] non-main thread reached nested-fairness: schedTid=" << g_schedTid
                          << " isGuest=" << (int)g_schedIsGuest << " schedEn=" << (int)m_schedEnabled << std::endl;
            // Only REGISTERED guest threads (main + StartThread workers) touch the
            // scheduler token. Host service threads (INTC/vblank/audio/MPEG) run
            // guest code with the default g_schedTid=1 and would corrupt tid 1's
            // scheduling -- they just do the plain lock-release fairness instead.
            if (m_schedEnabled && g_schedIsGuest)
            {
                schedYield(g_schedTid);
            }
            else
            {
                GuestExecutionReleaseScope releaseForFairness(this);
                std::this_thread::yield();
            }
        }
    }

    // Deterministic scheduler: give the main thread a quantum yield during nested
    // spins too (the tick-pump branch above runs only for the main thread).
    if (m_schedEnabled && g_schedIsGuest && ctx == &m_cpuContext)
    {
        static thread_local uint64_t s_mainNestedYield = 0u;
        constexpr uint64_t kMainNestedInterval = 1024u;
        if ((++s_mainNestedYield % kMainNestedInterval) == 0u)
        {
            schedYield(1);
        }
    }

    // BT3 overlay/loading probe: the main loop (FUN_00100558) per-frame body is
    // FUN_00100280(0x100628); jal 0x336a90(0x100630); FUN_0012bd10(0x100638).
    // Determine which call the main thread reaches and whether the 0x336a90
    // overlay ever has loaded code / is ever invoked.
    {
        if (sourcePc == 0x100628u || sourcePc == 0x100630u || sourcePc == 0x100638u ||
            targetPc == 0x336a90u || (targetPc >= 0x334c00u && targetPc < 0x360000u))
        {
            static std::atomic<uint32_t> s_loopProbe{0};
            uint32_t n = s_loopProbe.fetch_add(1);
            // One-shot dump of the disc-loaded overlay region once steady state is
            // reached, so it can be disassembled offline (it only exists in RAM).
            if (n == 300u && rdram && std::getenv("PS2X_DUMP_OVERLAY"))
            {
                FILE *f = std::fopen("/home/z3/Desktop/bt3/work/overlay_dump.bin", "wb");
                if (f)
                {
                    std::fwrite(rdram + 0x334c00u, 1u, 0x2b400u, f); // 0x334c00..0x360000
                    std::fclose(f);
                    std::cerr << "[bt3-loop] dumped overlay 0x334c00..0x360000" << std::endl;
                }
            }
            if (ps2xTraceEnabled() && n < 4000u)
            {
                uint32_t ov0 = 0u, ov1 = 0u;
                if (rdram)
                {
                    std::memcpy(&ov0, rdram + (0x336a90u & 0x1FFFFFFFu), sizeof(ov0));
                    std::memcpy(&ov1, rdram + (0x336a94u & 0x1FFFFFFFu), sizeof(ov1));
                }
                std::cerr << "[bt3-loop] source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " *0x336a90=0x" << ov0 << " *0x336a94=0x" << ov1
                          << std::dec << " (call#" << n << ")" << std::endl;
            }
        }
    }

    if (targetPc == 0u)
    {
        static std::atomic<uint32_t> s_ret0{0};
        if (ps2xTraceEnabled() && s_ret0.fetch_add(1) < 20u)
            std::cerr << "[ret-to-0] kind=" << static_cast<int>(kind) << " source=0x" << std::hex << sourcePc
                      << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0))
                      << " sp=0x" << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) << std::dec << std::endl;
    }

    if (kind == GuestBranchKind::Return)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        // Prevent nested dispatch.
        ctx->pc = targetPc;
        return false;
    }

    // Hot-call cache fast path (skips lookupFunction + hasFunction for repeat calls).
    if (s_hcPc[hcI] == targetPc && s_hcFn[hcI] != nullptr && targetPc != 0u)
    {
        m_debugPc.store(targetPc, std::memory_order_relaxed);
        const uint32_t entryPc = ctx->pc;
        s_hcFn[hcI](rdram, ctx, this);
        if (isStopRequested() || ctx->pc == 0u)
            return false;
        if (!isCall)
            return false;
        if (ctx->pc == entryPc)
            ctx->pc = fallthroughPc;
        return ctx->pc == fallthroughPc;
    }

    if (!hasFunction(targetPc))
    {
        // Interpreter fallback for dynamically-loaded (overlay) code: if this is
        // a call into a RAM address that holds plausible code, interpret it until
        // it returns to the recompiled caller (fallthroughPc).
        if (isCall && rdram)
        {
            const uint32_t phys = targetPc & 0x1FFFFFFFu;
            if (phys >= 0x10000u && phys < 0x02000000u)
            {
                uint32_t firstInsn = 0u;
                std::memcpy(&firstInsn, rdram + phys, sizeof(firstInsn));
                if (firstInsn != 0u)
                {
                    ctx->pc = targetPc;
                    if (interpretUntil(rdram, ctx, fallthroughPc))
                    {
                        ctx->pc = fallthroughPc;
                        return true;
                    }
                    // interpretation failed (unknown opcode) — fall through to
                    // the normal missing-function handling below.
                }
            }
        }

        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        // PS2X_SKIP_MISSING: escape hatch to skip missing/garbage indirect calls (return to
        // fallthrough) instead of looping on the bad target -- lets the demo attempt to run
        // past its garbage callback so we can see whether anything downstream works.
        static const bool s_skipMissing = [](){ const char *v = std::getenv("PS2X_SKIP_MISSING"); return v && v[0] && v[0] != '0'; }();
        MissingFunctionPolicy policy = missingFunctionPolicy();
        if (s_skipMissing) policy = MissingFunctionPolicy::SkipCallDebug;

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            return true;
        }

        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    s_hcPc[hcI] = targetPc; s_hcFn[hcI] = targetFn; // populate hot-call cache (static fn)
    const uint32_t entryPc = ctx->pc;
    m_debugPc.store(targetPc, std::memory_order_relaxed);
    // Trace main()'s (FUN_00100558) init sequence to find which call breaks.
    static const uint32_t s_mainCalls[] = {
        0x264bd0u, 0x1259f0u, 0x263240u, 0x2630e8u, 0x263198u, 0x25de68u, 0x266608u, 0x263490u,
        0x121da8u, 0x100670u, 0x263098u, 0x122940u, 0x116ba8u, 0x239ff0u, 0x23d0e0u, 0x252be8u,
        0x268188u, 0x100280u, 0x12bd10u};
    bool traceThis = false;
    if (ps2xTraceEnabled())
        for (uint32_t a : s_mainCalls) if (a == targetPc) { traceThis = true; break; }
    if (traceThis)
        std::cerr << "[m>] 0x" << std::hex << targetPc << std::dec << std::endl;
    targetFn(rdram, ctx, this);
    if (traceThis)
        std::cerr << "[m<] 0x" << std::hex << targetPc << " ret pc=0x" << ctx->pc << std::dec << std::endl;

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    if (!isCall)
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

thread_local uint32_t g_schedLastPc = 0, g_schedLastRa = 0;   // [schedwhy] per guest thread, updated per dispatch
extern std::atomic<uint32_t> g_bt3StateLive; // defined below; [eeround2] gate
void PS2Runtime::dispatchLoop(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t lastPc = std::numeric_limits<uint32_t>::max();
    uint32_t samePcCount = 0;
    uint64_t fairnessCounter = 0;
    constexpr uint32_t kSamePcYieldInterval = 0x4000u;
    // Simulated preemption: on the real single-core EE a busy-waiting guest
    // thread is preempted by the timer interrupt so other threads (which it is
    // waiting on) run. The runtime serializes guest threads under one lock but
    // has no such preemption, so a spinner monopolizes the lock and starves the
    // thread it depends on -> non-deterministic boot stalls. Periodically release
    // the guest-execution lock so other guest threads make progress.
    constexpr uint64_t kFairnessYieldInterval = 1024u;
    // NOTE: the central interrupt-tick pump lives in dispatchGuestBranch (hit at
    // every nesting depth), not here -- BT3's nested CDVD wait loops never unwind
    // back to this top-level loop, so a pump here starves.

    // Deterministic scheduler: the main guest thread is tid 1. Register + take the
    // token before running any guest code.
    const bool schedMain = m_schedEnabled && (ctx == &m_cpuContext);
    if (schedMain)
    {
        schedSetTid(1);
        schedAcquire(1, 0);
    }

    while (!isStopRequested())
    {
        const uint32_t pc = ctx->pc;
        // [eeround2] PS2X_EEROUND2=1: EE chop+FTZ/DAZ only while in-fight (bt3state 0x2d).
        // EEROUND (global, at thread start) was falsified on runs now known to be rig flakes;
        // this variant flips MXCSR at the dispatch boundary from the live state gate.
        {
            static const bool s_eer2 = [](){ const char *v = std::getenv("PS2X_EEROUND2"); return v && v[0] && v[0] != '0'; }();
            if (s_eer2)
            {
                const bool want = g_bt3StateLive.load(std::memory_order_relaxed) == 0x2du;
                static thread_local bool s_eer2Cur = false;
                if (want != s_eer2Cur)
                {
                    s_eer2Cur = want;
                    if (want) _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
                    else      _mm_setcsr(_mm_getcsr() & ~0xE040u);
                    static std::atomic<bool> s_eer2Said{false}; bool e = false;
                    if (want && s_eer2Said.compare_exchange_strong(e, true))
                        std::fprintf(stderr, "[eeround2] ACTIVE (state=0x2d): EE chop+FTZ\n");
                }
            }
        }

        if (schedMain && (fairnessCounter % kFairnessYieldInterval) == 0u && fairnessCounter != 0u)
        {
            schedYield(1);
        }

        if (!schedMain && (++fairnessCounter % kFairnessYieldInterval) == 0u)
        {
            GuestExecutionReleaseScope releaseForFairness(this);
            std::this_thread::yield();
        }
        else if (schedMain)
        {
            ++fairnessCounter;
        }

        // Definitive hot-loop finder for the main thread: histogram of function
        // entries; dump the top offenders periodically. Own env flag (PS2X_HIST)
        // so it runs at full speed without the throttling [main-pc] trace.
        static const bool s_histEnabled = []() { const char *v = std::getenv("PS2X_HIST"); return v && v[0] && v[0] != '0'; }();
        if (s_histEnabled && ctx == &m_cpuContext)
        {
            static std::unordered_map<uint32_t, uint64_t> s_hist;
            static uint64_t s_histTick = 0;
            ++s_hist[pc];
            if ((++s_histTick % 20000ull) == 0ull)
            {
                std::vector<std::pair<uint32_t, uint64_t>> top(s_hist.begin(), s_hist.end());
                std::partial_sort(top.begin(), top.begin() + std::min<size_t>(12, top.size()), top.end(),
                                  [](auto &a, auto &b) { return a.second > b.second; });
                std::cerr << "[hot] main-thread top function entries:";
                for (size_t i = 0; i < std::min<size_t>(12, top.size()); ++i)
                    std::cerr << " 0x" << std::hex << top[i].first << std::dec << "(" << top[i].second << ")";
                std::cerr << std::endl;
                s_hist.clear();
            }
        }

        g_schedLastPc = pc; g_schedLastRa = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));   // [schedwhy]
        {   // [dispatchpump] the loading deadlock the spin detector misses: the main thread polls through SEVERAL pcs
            // (never the same one 2000x), the loader threads sit blocked on CD completions, and nothing ticks the CD
            // file server or hands the cooperative token over. Every 16k dispatches on the main thread: tick the
            // server if it has not run for 20 ms (no sleep), and yield once if another guest thread is runnable.
            static const bool s_dpOn = [](){ const char *v = std::getenv("PS2X_DISPATCHPUMP"); return !(v && v[0] == '0'); }();
            static thread_local uint32_t s_dp = 0;
            if (s_dpOn && m_schedEnabled && ctx == &m_cpuContext && ((++s_dp & 0x3FFFu) == 0u))
            {
                if (ps2xCdTickStale(20u)) ps2xCdTickOnly(rdram, ctx, this);
                bool othersPresent = false;
                {   // any other guest thread at all: a thread whose semaphore was just signalled from the RPC/interrupt
                    // path still shows blocked=true until it gets the token, and this loop is the only place the main
                    // thread's syscall-free polling ever gives it up (P17: sound thread tid4 signalled, never scheduled)
                    std::lock_guard<std::mutex> lk(m_schedMutex);
                    for (auto &kv : m_schedThreads) { const SchedThread &s = *kv.second; if (kv.first != m_schedCurrent && s.present) { othersPresent = true; break; } }
                }
                if (othersPresent)
                {
                    static std::atomic<uint32_t> s_dy{0};
                    const uint32_t k = s_dy.fetch_add(1u);
                    if (k < 4u || (k % 20000u) == 0u) std::fprintf(stderr, "[dispatchpump] main thread yield (x%u) at pc 0x%x\n", k + 1u, pc);
                    void *scope = ps2xGuestWaitBegin(); std::this_thread::yield(); ps2xGuestWaitEnd(scope);
                }
            }
        }
        if (pc == lastPc)
        {
            ++samePcCount;
            if (samePcCount == 2000u || samePcCount == 20000u || samePcCount == 200000u || samePcCount == 2000000u)
            {   // [stallprobe] the infinite-loading freeze re-dispatches at one pc forever (0x1149a0,
                // func_114860 spinning on a halfword at [$t6+8]). Name what it spins on.
                auto R = [&](int i) { return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[i], 0)); };
                const uint32_t t6 = R(14);
                const uint8_t *rd = m_memory.getRDRAM();
                auto rd16 = [&](uint32_t va) -> uint32_t { uint16_t v = 0; std::memcpy(&v, rd + ((va & 0x1FFFFFFFu) & PS2_RAM_MASK), 2); return v; };
                auto rd32 = [&](uint32_t va) -> uint32_t { uint32_t v = 0; std::memcpy(&v, rd + ((va & 0x1FFFFFFFu) & PS2_RAM_MASK), 4); return v; };
                std::fprintf(stderr, "[stallprobe] pc=0x%x x%u %s ra=0x%x | t6=0x%x [t6+8]=0x%x [t6+0]=0x%08x [t6+4]=0x%08x [t6+c]=0x%08x | a0=0x%x a1=0x%x a2=0x%x a3=0x%x v0=0x%x s0=0x%x s1=0x%x s2=0x%x t7=0x%x t8=0x%x\n",
                             pc, samePcCount, (ctx == &m_cpuContext) ? "main" : "thread", R(31), t6, rd16(t6 + 8u), rd32(t6), rd32(t6 + 4u), rd32(t6 + 0xCu),
                             R(4), R(5), R(6), R(7), R(2), R(16), R(17), R(18), R(15), R(24));
            }
            if ((samePcCount % 2000u) == 0u && ctx == &m_cpuContext)   // [pumpmain] never run the tick on a sound thread's 2 KB stack
            {   // [spinpump] a busy-poll: advance the CD file server and let other guest threads run (see game_overrides)
                ps2xSpinPump(rdram, ctx, this);
            }
            if ((samePcCount % kSamePcYieldInterval) == 0u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    RUNTIME_LOG("CPU is doing some work at PC 0x" << std::hex << pc << ". PC not updating.");
                });
                std::this_thread::yield();
            }
        }
        else
        {
            samePcCount = 0;
            lastPc = pc;
        }

        {
            static thread_local uint32_t s_lastLogPc = 0xffffffffu;
            static std::atomic<uint32_t> s_mainTrace{0};
            // Only trace the MAIN guest thread (its context is m_cpuContext), not
            // the sound/service threads that also run through this loop.
            if (ps2xTraceEnabled() && ctx == &m_cpuContext && pc != s_lastLogPc)
            {
                s_lastLogPc = pc;
                s_mainTrace.fetch_add(1);
                std::cerr << "[main-pc] 0x" << std::hex << pc
                          << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) << std::dec << std::endl;
            }
        }
        m_debugPc.store(pc, std::memory_order_relaxed);
        m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), std::memory_order_relaxed);
        m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)), std::memory_order_relaxed);
        m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)), std::memory_order_relaxed);

        RecompiledFunction fn = lookupFunction(pc);
        const uint32_t dispatchedPc = pc;
        const uint32_t dispatchedRa = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));

        {
            GuestExecutionScope guestExecution(this);
            fn(rdram, ctx, this);
        }

        if (ctx->pc == 0u)
        {
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
            const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
            if (ps2xTraceEnabled())
            std::cerr << "[dispatch:pc-zero] from=0x" << std::hex << dispatchedPc
                      << " fromRa=0x" << dispatchedRa
                      << " ra=0x" << ra
                      << " sp=0x" << sp
                      << " gp=0x" << gp
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;

            // PC=0 means this guest thread returned (usually via jr $ra with RA=0).
            // Do not request a global runtime stop here: other guest threads may still run.
            break;
        }
    }

    if (schedMain)
        schedUnregister(1);
}

void PS2Runtime::enterGuestExecution()
{
    m_guestExecutionWaiters.fetch_add(1u, std::memory_order_acq_rel);
    if (schedDbgEnabled())
    {
        static std::atomic<uint64_t> s_c{0};
        if ((s_c.fetch_add(1) % 20000u) == 1u)
            std::cerr << "[mutex] tid " << g_schedTid << " isGuest=" << (int)g_schedIsGuest
                      << " want-lock, curHolder=" << g_guestMutexHolderTid.load()
                      << " schedCur=" << m_schedCurrent << std::endl;
    }
    m_guestExecutionMutex.lock();
    g_guestMutexHolderTid.store(g_schedIsGuest ? g_schedTid : -100);
    m_guestExecutionWaiters.fetch_sub(1u, std::memory_order_acq_rel);
    ++g_guestExecutionDepths[this];
    markGuestExecutionAcquired();
}

void PS2Runtime::leaveGuestExecution()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || it->second == 0u)
    {
        return;
    }

    --it->second;
    m_guestExecutionMutex.unlock();
    if (it->second == 0u)
    {
        g_guestExecutionDepths.erase(it);
    }
}

uint32_t PS2Runtime::releaseGuestExecution()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || it->second == 0u)
    {
        return 0u;
    }

    const uint32_t depth = it->second;
    for (uint32_t i = 0; i < depth; ++i)
    {
        m_guestExecutionMutex.unlock();
    }
    g_guestExecutionDepths.erase(it);
    return depth;
}

void PS2Runtime::reacquireGuestExecution(uint32_t depth)
{
    if (depth == 0u)
    {
        return;
    }

    uint32_t &heldDepth = g_guestExecutionDepths[this];
    for (uint32_t i = 0; i < depth; ++i)
    {
        m_guestExecutionWaiters.fetch_add(1u, std::memory_order_acq_rel);
        m_guestExecutionMutex.lock();
        m_guestExecutionWaiters.fetch_sub(1u, std::memory_order_acq_rel);
        ++heldDepth;
        markGuestExecutionAcquired();
    }
}

void PS2Runtime::markGuestExecutionAcquired()
{
    {
        std::lock_guard<std::mutex> lock(m_guestExecutionHandoffMutex);
        m_guestExecutionHandoffEpoch.fetch_add(1u, std::memory_order_acq_rel);
    }
    m_guestExecutionHandoffCv.notify_all();
}

// ---- Deterministic cooperative guest scheduler (PS2X_SCHED) ----------------
int PS2Runtime::schedPickNextLocked(int afterTid)
{
    // Round-robin by registration order, starting just after afterTid. Only
    // present && !blocked threads are candidates. Deterministic given the same
    // set of runnable threads and the same afterTid.
    uint64_t afterOrder = 0;
    bool haveAfter = false;
    auto ait = m_schedThreads.find(afterTid);
    if (ait != m_schedThreads.end())
    {
        afterOrder = ait->second->order;
        haveAfter = true;
    }
    int best = -1;
    uint64_t bestOrder = 0;
    int wrap = -1;
    uint64_t wrapOrder = 0;
    for (auto &kv : m_schedThreads)
    {
        SchedThread &s = *kv.second;
        if (!s.present || s.blocked)
            continue;
        if (haveAfter && s.order > afterOrder)
        {
            if (best < 0 || s.order < bestOrder) { best = kv.first; bestOrder = s.order; }
        }
        else
        {
            if (wrap < 0 || s.order < wrapOrder) { wrap = kv.first; wrapOrder = s.order; }
        }
    }
    return (best >= 0) ? best : wrap;
}

void PS2Runtime::schedSetTid(int tid)
{
    g_schedTid = tid;
    g_schedIsGuest = true;
}

void PS2Runtime::schedRegister(int tid, int prio)
{
    std::lock_guard<std::mutex> lk(m_schedMutex);
    auto &slot = m_schedThreads[tid];
    if (!slot) { slot = std::make_unique<SchedThread>(); slot->order = m_schedOrderCounter++; }
    slot->prio = prio;
    slot->present = true;
    slot->blocked = false;
    if (m_schedCurrent < 0) m_schedCurrent = tid;
}

void PS2Runtime::schedUnregister(int tid)
{
    std::lock_guard<std::mutex> lk(m_schedMutex);
    auto it = m_schedThreads.find(tid);
    if (it == m_schedThreads.end()) return;
    it->second->present = false;
    it->second->blocked = false;
    if (schedDbgOn()) std::cerr << "[sched] tid " << tid << " UNREGISTER (was cur=" << m_schedCurrent << ")" << std::endl;
    if (m_schedCurrent == tid)
    {
        int nxt = schedPickNextLocked(tid);
        m_schedCurrent = nxt;
        if (nxt >= 0) m_schedThreads[nxt]->cv.notify_all();
    }
}

void PS2Runtime::schedAcquire(int tid, int prio)
{
    if (!m_schedEnabled) return;
    std::unique_lock<std::mutex> lk(m_schedMutex);
    auto &slot = m_schedThreads[tid];
    if (!slot) { slot = std::make_unique<SchedThread>(); slot->order = m_schedOrderCounter++; slot->prio = prio; }
    slot->present = true;
    slot->blocked = false;
    if (m_schedCurrent < 0) m_schedCurrent = tid;
    if (schedDbgOn()) std::cerr << "[sched] tid " << tid << " ACQUIRE-wait (cur=" << m_schedCurrent << ")" << std::endl;
    while (!(m_schedCurrent == tid || isStopRequested()))
    {
        if (slot->cv.wait_for(lk, std::chrono::milliseconds(250),
                              [&] { return m_schedCurrent == tid || isStopRequested(); }))
            break;
        // Watchdog: if the token-holder is gone (not present -> a thread that exited while
        // holding it) or invalid, reclaim/advance the token so we don't deadlock on a corpse.
        auto cit = m_schedThreads.find(m_schedCurrent);
        const bool holderDead = (m_schedCurrent < 0) || (cit == m_schedThreads.end()) || !cit->second->present;
        if (holderDead)
        {
            int nxt = schedPickNextLocked(m_schedCurrent);
            m_schedCurrent = (nxt >= 0) ? nxt : tid;
            if (m_schedCurrent != tid) m_schedThreads[m_schedCurrent]->cv.notify_all();
            if (schedDbgOn()) std::cerr << "[sched] tid " << tid << " watchdog reclaimed token (dead holder) -> cur=" << m_schedCurrent << std::endl;
        }
    }
    if (schedDbgOn()) std::cerr << "[sched] tid " << tid << " ACQUIRED" << std::endl;
}

void PS2Runtime::schedYield(int tid)
{
    if (!m_schedEnabled) return;
    {
        std::unique_lock<std::mutex> lk(m_schedMutex);
        if (m_schedCurrent != tid) return;
        int nxt = schedPickNextLocked(tid);
        if (nxt < 0 || nxt == tid) return; // nobody else runnable -> keep going
        if (schedDbgOn()) std::cerr << "[sched] tid " << tid << " YIELD -> " << nxt << std::endl;
        m_schedCurrent = nxt;
        m_schedThreads[nxt]->cv.notify_all();
    }
    // CRITICAL: schedYield is called from inside a recompiled function, so this
    // thread holds the guest-execution lock. Release it while parked so the next
    // guest thread AND host service threads (interrupt handlers) can actually run
    // -- otherwise everything that needs the lock deadlocks behind us.
    const uint32_t depth = releaseGuestExecution();
    {
        std::unique_lock<std::mutex> lk(m_schedMutex);
        auto it = m_schedThreads.find(tid);
        if (it != m_schedThreads.end())
            it->second->cv.wait(lk, [&] { return m_schedCurrent == tid || isStopRequested(); });
    }
    reacquireGuestExecution(depth);
}

void PS2Runtime::schedBeginBlock(int tid)
{
    if (!m_schedEnabled) return;
    std::lock_guard<std::mutex> lk(m_schedMutex);
    auto it = m_schedThreads.find(tid);
    extern thread_local uint32_t g_schedLastPc, g_schedLastRa;
    if (it != m_schedThreads.end()) { it->second->blocked = true; it->second->blockPc = g_schedLastPc; it->second->blockRa = g_schedLastRa; }   // [schedwhy]
    if (m_schedCurrent == tid)
    {
        int nxt = schedPickNextLocked(tid);
        if (schedDbgOn()) std::cerr << "[sched] tid " << tid << " BLOCK -> " << nxt << std::endl;
        m_schedCurrent = nxt;
        if (nxt >= 0) m_schedThreads[nxt]->cv.notify_all();
    }
}

void PS2Runtime::yieldGuestExecutionAfterWake()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || it->second == 0u)
    {
        std::this_thread::yield();
        return;
    }

    const uint64_t handoffEpoch = m_guestExecutionHandoffEpoch.load(std::memory_order_acquire);
    {
        GuestExecutionReleaseScope releaseGuestExecution(this);
        std::unique_lock<std::mutex> lock(m_guestExecutionHandoffMutex);
        m_guestExecutionHandoffCv.wait_for(lock, std::chrono::milliseconds(2), [&]()
                                           { return m_guestExecutionHandoffEpoch.load(std::memory_order_acquire) != handoffEpoch; });
    }
}

bool PS2Runtime::shouldPreemptGuestExecution()
{
    thread_local uint32_t s_backEdgeYieldCounter = 0u;
    const uint32_t waiterCount = m_guestExecutionWaiters.load(std::memory_order_acquire);
    const uint32_t yieldInterval = (waiterCount != 0u) ? 64u : 100u;
    if (++s_backEdgeYieldCounter < yieldInterval)
    {
        return false;
    }

    s_backEdgeYieldCounter = 0u;
    return true;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    ps2_syscalls::notifyRuntimeStop();
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

std::atomic<uint32_t> g_bt3StateLive{0xffffffffu};   // [barblock] last bt3state seen by the status probe
static PS2Runtime *g_waitHookRuntime = nullptr;   // [barblock]
void *PS2Runtime::guestWaitBegin() { return new GuestExecutionReleaseScope(this); }
void PS2Runtime::guestWaitEnd(void *handle) { delete static_cast<GuestExecutionReleaseScope *>(handle); }
extern "C" void *ps2xGuestWaitBegin() { gprof::enter(gprof::WAIT); return g_waitHookRuntime ? g_waitHookRuntime->guestWaitBegin() : nullptr; }   // [guestprof] WAIT
extern "C" void ps2xGuestWaitEnd(void *h) { gprof::leave(); if (h && g_waitHookRuntime) g_waitHookRuntime->guestWaitEnd(h); }
void PS2Runtime::run()
{
    g_waitHookRuntime = this;   // [barblock]
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    ps2_syscalls::resetSoundDriverRpcState();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetGsSyncVCallbackState();
    ps2_stubs::resetMpegStubState();
    ps2_syscalls::initializeGuestKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    g_activeThreads.store(1, std::memory_order_relaxed);
    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        // [eeround] PS2X_EEROUND=1: run the guest thread's float math (EE FPU and
        // VU0-macro ops, both SSE in recompiled code) under RZ+FTZ+DAZ — PCSX2's
        // default EE/VU "Chop/Zero" rounding. Host default (round-nearest,
        // denormals) diverges from console in the last bits of every FPU/VU0
        // chain; the terrain lighting-group matrix path (sub_002188B8 ->
        // sub_00120308/120C40 vmula/vmadda ACC chains) consumes exactly such
        // math, and boundary chunks flip groups on those last bits.
        {
            static const bool s_eeRound = [](){ const char *v = std::getenv("PS2X_EEROUND"); return v && v[0] && v[0] != '0'; }();
            if (s_eeRound) _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
        }
        try
        {
            dispatchLoop(m_memory.getRDRAM(), &m_cpuContext);
            std::cerr << "[GAMETHREAD-EXIT] final pc=0x" << std::hex << m_cpuContext.pc
                      << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0))
                      << " lastDebugPc=0x" << m_debugPc.load() << std::dec << std::endl;
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        g_activeThreads.fetch_sub(1, std::memory_order_relaxed);
        gameThreadFinished.store(true, std::memory_order_release); });

#ifdef __linux__
    std::thread pinKeeper;
    std::atomic<bool> keepStop{false};
    {   // [pinthreads] PS2X_PIN. Game thread = guest EE below; THIS thread = GL + present.
        cpu_set_t sGame, sGl;
        if (ps2xPinMasks(sGame, sGl))
        {
            const int r1 = pthread_setaffinity_np(gameThread.native_handle(), sizeof(cpu_set_t), &sGame);
            const int r2 = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &sGl);
            if (r1 != 0 || r2 != 0)
                std::fprintf(stderr, "[pinthreads] setaffinity failed r1=%d r2=%d\n", r1, r2);
            else
                pinKeeper = std::thread(ps2xPinKeeperLoop, gameThread.native_handle(), sGame, sGl, &keepStop);
        }
    }
#else
    std::thread pinKeeper; std::atomic<bool> keepStop{false};
    (void)pinKeeper; (void)keepStop;
#endif

    // EE PC sampler (PS2X_PCSAMPLE): samples the currently-dispatched guest function PC
    // to reveal whether the game thread is spinning in one hot function (fixable wait) or
    // spread across many (genuinely compute-bound recompiled code).
    if (std::getenv("PS2X_PCSAMPLE"))
    {
        std::thread([this, &gameThreadFinished]() {
            std::unordered_map<uint32_t, uint64_t> hist;
            uint64_t total = 0;
            while (!gameThreadFinished.load(std::memory_order_acquire))
            {
                // Sample the GAME thread's own PC (m_cpuContext.pc), NOT the shared
                // m_debugPc which the interrupt worker also writes. If the game thread is
                // blocked in a wait, this shows the wait-call site (the real bottleneck).
                hist[m_cpuContext.pc]++;
                ++total;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                if ((total % 8000) == 0) // ~every 4s
                {
                    std::vector<std::pair<uint32_t, uint64_t>> v(hist.begin(), hist.end());
                    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                    std::cerr << "[pcsample] n=" << total << " top:";
                    for (size_t i = 0; i < 10 && i < v.size(); ++i)
                        std::cerr << " 0x" << std::hex << v[i].first << std::dec << "=" << (v[i].second * 100 / total) << "%";
                    std::cerr << std::endl;
                }
            }
        }).detach();
    }

    ps2_syscalls::EnsureVSyncWorkerRunning(m_memory.getRDRAM(), this);

    uint64_t tick = 0;
    while (!isStopRequested() && g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const int activeThreads = g_activeThreads.load(std::memory_order_relaxed);

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << activeThreads
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);
            }
        });
        {
            static uint64_t s_hbTick = 0;
            if (ps2xTraceEnabled() && (s_hbTick++ % 60) == 0)
            {
                std::cerr << "[hb] pc=0x" << std::hex << m_debugPc.load(std::memory_order_relaxed)
                          << " ra=0x" << m_debugRa.load(std::memory_order_relaxed) << std::dec
                          << " dma=" << m_memory.dmaStartCount()
                          << " gif=" << m_memory.gifCopyCount()
                          << " gsw=" << m_memory.gsWriteCount()
                          << " vif=" << m_memory.vifWriteCount()
                          << " threads=" << g_activeThreads.load(std::memory_order_relaxed)
                          << " cpuPc=0x" << std::hex << m_cpuContext.pc << std::dec
                          << " | " << ps2_syscalls::dumpAllThreadStates()
                          << std::endl;
            }
            // Compact always-on progress line (~every 10s) for long unattended
            // runs, so progress is visible without full PS2X_TRACE.
            if ((s_hbTick % 600u) == 0u)
            {
                // BT3 overlay game-state: FUN_00336a90 switches on *(*(0x2ff10c)+0x18).
                // Also probe the intro fade: DAT_00301048 (fade state) / DAT_00301050
                // (fade level, starts 0x80) — if level decreases it's just slow.
                uint32_t bt3State = 0xffffffffu, bt3StatePtr = 0u, fadeState = 0u, fadeLevel = 0u;
                uint32_t s_introTimer = 0u, s_introSub = 0u;
                if (const uint8_t *rd = m_memory.getRDRAM())
                {
                    std::memcpy(&bt3StatePtr, rd + (0x2ff10cu & PS2_RAM_MASK), 4);
                    if (bt3StatePtr)
                        std::memcpy(&bt3State, rd + (((bt3StatePtr & 0x1FFFFFFFu) + 0x18u) & PS2_RAM_MASK), 4);
                    g_bt3StateLive.store(bt3State, std::memory_order_relaxed);   // [barblock] loading-state gate
                    std::memcpy(&fadeState, rd + (0x301048u & PS2_RAM_MASK), 4);
                    std::memcpy(&fadeLevel, rd + (0x301050u & PS2_RAM_MASK), 4);
                    // TEST (PS2X_FORCE_TRIANGLE): stamp the game's button flag with
                    // TRIANGLE (0x1000) at _DAT_0033398c to force the popup skip path.
                    static const bool s_forceTri = [](){ const char *v=std::getenv("PS2X_FORCE_TRIANGLE"); return v&&v[0]&&v[0]!='0'; }();
                    if (s_forceTri)
                    {
                        uint8_t *rw = m_memory.getRDRAM();
                        uint32_t tri = 0x1000u;
                        if (rw) { std::memcpy(rw + (0x33398cu & PS2_RAM_MASK), &tri, 4);
                                  std::memcpy(rw + (0x333990u & PS2_RAM_MASK), &tri, 4); }
                    }
                    // Intro auto-advance timer: *(*(0x3b0eb8)+0xc4) counts to 0x708.
                    uint32_t p = 0u;
                    std::memcpy(&p, rd + (0x3b0eb8u & PS2_RAM_MASK), 4);
                    if (p)
                    {
                        std::memcpy(&s_introTimer, rd + (((p & 0x1FFFFFFFu) + 0xc4u) & PS2_RAM_MASK), 4);
                        std::memcpy(&s_introSub, rd + (((p & 0x1FFFFFFFu) + 0xb8u) & PS2_RAM_MASK), 4);
                        // Popup context flags: +0x14 (gate &2), +0xa8 (flags &2), +0xbc
                        // (=func_0x0011ebc0; if !=0 the advance logic is skipped).
                        uint32_t f14=0,fa8=0,fbc=0;
                        std::memcpy(&f14, rd + (((p & 0x1FFFFFFFu) + 0x14u) & PS2_RAM_MASK), 4);
                        std::memcpy(&fa8, rd + (((p & 0x1FFFFFFFu) + 0xa8u) & PS2_RAM_MASK), 4);
                        std::memcpy(&fbc, rd + (((p & 0x1FFFFFFFu) + 0xbcu) & PS2_RAM_MASK), 4);
                        std::cerr << "[popup] ctx=0x" << std::hex << p << " +0x14=0x" << f14
                                  << " +0xa8=0x" << fa8 << " +0xbc=0x" << fbc << std::dec << std::endl;
                    }
                }
                {   // [sndblk] the sound stream control block 0x2c9350 (+8 / +4 sub-objects): the object the loading
                    // hangs corrupt (field +0x10 = 0x02xxxxxx tagged handle dereferenced on tid4). Dumped with every
                    // status line during loading (bt3state 0x2d) so a hung state can be diffed against a healthy one.
                    const uint8_t *rd2 = m_memory.getRDRAM();
                    auto r32 = [&](uint32_t a) -> uint32_t { uint32_t v = 0; std::memcpy(&v, rd2 + (a & PS2_RAM_MASK), 4); return v; };
                    ps2xFixupRingDump();   // [fixupring] last 16 loader fixup calls, only when new ones arrived
                    std::ostringstream sb; sb << "[sndblk] 0x2c9350:";
                    for (uint32_t o = 0; o < 0x40u; o += 4u) sb << ' ' << std::hex << std::setw(8) << std::setfill('0') << r32(0x2c9350u + o);
                    const uint32_t sub8 = r32(0x2c9358u), sub4 = r32(0x2c9354u);
                    if (sub8 >= 0x100000u && sub8 < 0x2000000u) { sb << " | +8@" << sub8 << ":"; for (uint32_t o = 0; o < 0x20u; o += 4u) sb << ' ' << std::setw(8) << r32(sub8 + o); }
                    if (sub4 >= 0x100000u && sub4 < 0x2000000u) { sb << " | +4@" << sub4 << ":"; for (uint32_t o = 0; o < 0x10u; o += 4u) sb << ' ' << std::setw(8) << r32(sub4 + o); }
                    { sb << " | slot3@2deb70:"; for (uint32_t o = 0; o < 0x40u; o += 4u) sb << ' ' << std::setw(8) << r32(0x2deb70u + o); }   // [sndblk] the sub-object slot the healthy init returns
                    std::cerr << sb.str() << std::dec << std::endl;
                }
                std::cerr << "[status] pc=0x" << std::hex << m_debugPc.load(std::memory_order_relaxed)
                          << " ra=0x" << m_debugRa.load(std::memory_order_relaxed) << std::dec
                          << " dma=" << m_memory.dmaStartCount()
                          << " gif=" << m_memory.gifCopyCount()
                          << " gsw=" << m_memory.gsWriteCount()
                          << " vif=" << m_memory.vifWriteCount()
                          << " bt3state=0x" << std::hex << bt3State
                          << " fade=" << fadeState << "/" << fadeLevel
                          << " introSub=" << s_introSub << " introTimer=" << s_introTimer << "/1800"
                          << std::dec << std::endl;
                // ===================== [hstate] Jerarquía legible de estados =====================
                // Traduce bt3State (raw) a fase + sub-fase humana. BOOT y MENU están mapeados con
                // offsets ya documentados en tasks/main_menu_state_machine.md y tasks/ESTATUS.md.
                // FIGHT/IN_FIGHT todavía no tienen los offsets de "tipo de combate"
                // jugador vs CPU / 2 jugadores) identificados -> ver el bloque [fightprobe] más abajo,
                // que es el que junta la evidencia para poder completar este switch.
                if (const uint8_t *rd = m_memory.getRDRAM())
                {
                    auto r32safe = [&](uint32_t addr) -> uint32_t {
                        uint32_t v = 0xffffffffu;
                        std::memcpy(&v, rd + (addr & PS2_RAM_MASK), 4);
                        return v;
                    };
                    auto hex32 = [](uint32_t v) -> std::string {
                        std::ostringstream o; o << "0x" << std::hex << v; return o.str();
                    };

                    std::string phase = "UNKNOWN";
                    std::string sub;

                    switch (bt3State)
                    {
                    case 0x01u:
                    {
                        phase = "BOOT";
                        extern std::atomic<uint32_t> g_ps2FmvActive; // [hstate] definido en ps2_gs_gpu.cpp
                        const bool fmv = g_ps2FmvActive.load(std::memory_order_relaxed) != 0u;
                        const ps2_stubs::MemoryCardDebugSnapshot mc = ps2_stubs::getMemoryCardDebugSnapshot();
                        // Heurística best-effort: refinar una vez que tengamos logs reales de un boot
                        // completo (memcard aparece antes de que exista introTimer > 0).
                        if (!mc.openFiles.empty())
                            sub = "MEMCARD_LOAD(openFiles=" + std::to_string(mc.openFiles.size())
                                + ",lastCmd=" + hex32(mc.lastCmd) + ")";
                        else if (mc.lastCmd != 0)
                            sub = "MEMCARD_CHECK(lastCmd=" + hex32(mc.lastCmd)
                                + ",lastResult=" + std::to_string(mc.lastResult) + ")";
                        else if (fmv)
                            sub = "INTRO_FMV";
                        else if (s_introTimer > 0)
                            sub = "TITLE_SPLASH(timer=" + std::to_string(s_introTimer) + "/1800)";
                        else
                            sub = "SPLASH_LOGOS(sin marcador exacto todavia)";
                        break;
                    }
                    case 0x04u:
                    {
                        phase = "MENU";
                        const uint32_t mainStruct = r32safe(0x3B38D8u);
                        if (mainStruct != 0u && mainStruct != 0xffffffffu)
                        {
                            const uint32_t globalFlags = r32safe(mainStruct + 0x3BCu);
                            const bool submenu = (globalFlags & 0x08u) != 0u;
                            const uint32_t subStruct = r32safe(mainStruct + 0x9A4u);
                            const uint32_t menuState = (subStruct && subStruct != 0xffffffffu)
                                ? r32safe(subStruct + 0x40u) : 0xffffffffu;
                            // Cursor/selección/estado del item activo: *(0x3B38E8)+0x12C/0x138/0x13C
                            // (offsets documentados en tasks/ESTATUS.md y main_menu_state_machine.md).
                            const uint32_t itemBase = r32safe(0x3B38E8u);
                            int32_t cursor = -1, selection = -1; uint32_t itemState = 0xffffffffu;
                            if (itemBase != 0u && itemBase != 0xffffffffu)
                            {
                                cursor    = static_cast<int32_t>(r32safe(itemBase + 0x12Cu));
                                selection = static_cast<int32_t>(r32safe(itemBase + 0x138u));
                                itemState = r32safe(itemBase + 0x13Cu);
                            }
                            static const char *kItemStateNames[9] = {
                                "PLATE_LOAD", "SECOND_PASS", "REFERENCE_COUNTER", "ANIMATION",
                                "CONFIRM_ACCEPT", "NAVIGATION", "CHARACTER_SELECT", "VISUAL_RENDER",
                                "FINAL_CONFIRM"
                            };
                            std::ostringstream o;
                            o << (menuState == 9u ? "DISPLAYED" : menuState == 10u ? "TRANSITIONING" : "STATE?");
                            o << (submenu ? "/SUBMENU" : "/ROOT");
                            o << " cursor=" << cursor << " sel=" << selection << " itemState=";
                            if (itemState < 9u) o << kItemStateNames[itemState];
                            else o << hex32(itemState);
                            sub = o.str();
                        }
                        else sub = "mainStruct=0 (menu aun no inicializado)";
                        break;
                    }
                    case 0x06u: phase = "LOADING"; break;
                    case 0x26u: case 0x28u: case 0x29u:
                        phase = "PREFIGHT_SETUP"; sub = "raw=" + hex32(bt3State); break;
                    case 0x27u: phase = "FIGHT";   sub = "modo=? (ver [fightprobe])"; break;
                    case 0x2Du: phase = "IN_FIGHT";   sub = "modo=? (ver [fightprobe])"; break;
                    case 0x38u: phase = "POST_FIGHT"; break;
                    default: break;
                    }

                    std::cerr << "[hstate] phase=" << phase << " sub=" << sub
                              << " raw=" << hex32(bt3State) << std::endl;
                }
                // ===================== [fightprobe] Diagnóstico de tipo de combate =====================
                // PS2X_FIGHTPROBE=1: en cada transición de bt3state hacia 0x26/0x27/0x2D vuelca:
                //  (a) qué puertos de pad están siendo efectivamente leídos (readCount creciendo)
                //      -> distingue CPU vs CPU (ningún puerto avanza) de P1 vs CPU (solo puerto 0)
                //      de P1 vs P2 local (puertos 0 y 1).
                //  (b) un hexdump de la región 0x3C00-0x3D00 del main-struct del menú (selección de
                //      personaje/escenario/dificultad que se hizo en 0x04 antes de entrar a la pelea).
                // Correr 3 veces (CPU-CPU, jugador-CPU, jugador-jugador local) y diffear las líneas
                // [fightprobe] pegadas: el/los bytes que cambien de forma consistente entre corridas
                // son el flag de "tipo de control" que hoy no está identificado.
                {
                    static const bool s_fightProbeOn = [](){
                        const char *v = std::getenv("PS2X_FIGHTPROBE"); return v && v[0] && v[0] != '0';
                    }();
                    static uint32_t s_lastProbedState = 0xffffffffu;
                    if (s_fightProbeOn && (bt3State == 0x26u || bt3State == 0x27u || bt3State == 0x2Du)
                        && bt3State != s_lastProbedState)
                    {
                        s_lastProbedState = bt3State;
                        const ps2_stubs::PadDebugSnapshot pad = ps2_stubs::getPadDebugSnapshot();
                        std::ostringstream o;
                        o << "[fightprobe] onEnter=0x" << std::hex << bt3State << std::dec;
                        for (size_t port = 0; port < ps2_stubs::kPadDebugPortCount; ++port)
                        {
                            const ps2_stubs::PadDebugPortSnapshot &p = pad.ports[port][0];
                            o << " port" << port << "(open=" << (p.open ? 1 : 0)
                              << ",reads=" << p.readCount
                              << ",lastBtn=0x" << std::hex << p.lastButtons << std::dec << ")";
                        }
                        if (const uint8_t *rd2 = m_memory.getRDRAM())
                        {
                            auto r32b = [&](uint32_t addr) -> uint32_t {
                                uint32_t v = 0; std::memcpy(&v, rd2 + (addr & PS2_RAM_MASK), 4); return v;
                            };
                            const uint32_t mainStruct = r32b(0x3B38D8u);
                            o << " mainStruct=0x" << std::hex << mainStruct << std::dec;
                            if (mainStruct)
                            {
                                o << " selectRegion[0x3C00:0x3D00)=";
                                for (uint32_t off = 0x3C00u; off < 0x3D00u; off += 4u)
                                    o << ' ' << std::hex << std::setw(8) << std::setfill('0') << r32b(mainStruct + off);
                                o << std::dec;
                            }
                        }
                        std::cerr << o.str() << std::endl;
                    }
                }
                // ===================== [menuhex] Estado del main menu =====================
                // PS2X_MENUHEX=1: sondea TODOS los estados (0x04 root, 0x3e OPTIONS, 0x2c etc).
                // vuelca cada vez que cambia el estado del menu.
                //   mainPtr   = *(0x2FF10C)      (main game-state struct, ya usada por hstate)
                //     +0x18   = screen_state_id  (bt3State)
                //     +0x2C   = selected_entry_ID (la entry actualmente seleccionada)
                //     +0x148  = cursor (indice en la lista de entries, 0-9)
                //     +0x14   = visibility_flags (bit 6 = menu visible)
                //     +0x68C  = transition_flags
                //   dispPtr   = *(0x2FF28C)
                //     +0x08   = display_filter (0-127)
                //     +0xA0C  = frame_counter (0-24)
                // Mas las jump tables fijas de overlay (ya confirmadas legibles):
                //   0x3B4290 jumpTable (10 handlers), 0x3B42C0 dispatch2 (5 handlers).
                {
                    static const bool s_menuHexOn = [](){
                        const char *v = std::getenv("PS2X_MENUHEX"); return v && v[0] && v[0] != '0';
                    }();
                    static std::string s_menuHexLastKey;   // fingerprint del ultimo estado volcado
                    if (s_menuHexOn)
                    {
                        if (const uint8_t *rd3 = m_memory.getRDRAM())
                        {
                            auto r32m = [&](uint32_t addr) -> uint32_t {
                                uint32_t v = 0; std::memcpy(&v, rd3 + (addr & PS2_RAM_MASK), 4); return v;
                            };
                            const uint32_t mainPtr = r32m(0x2FF10Cu);
                            if (mainPtr == 0u || mainPtr == 0xffffffffu)
                            {
                                s_menuHexLastKey.clear();   // aun no inicializado: resetear fingerprint
                            }
                            else
                            {
                                const uint32_t dispPtr = r32m(0x2FF28Cu);
                                const uint32_t screenState = r32m(mainPtr + 0x18u);
                                const uint32_t selectedEntry = r32m(mainPtr + 0x2Cu);
                                const uint32_t cursor = r32m(mainPtr + 0x148u);
                                const uint32_t visibility = r32m(mainPtr + 0x14u);
                                const uint32_t transition = r32m(mainPtr + 0x68Cu);
                                const uint32_t dispFilter = (dispPtr && dispPtr != 0xffffffffu)
                                    ? r32m(dispPtr + 0x08u) : 0xffffffffu;
                                const uint32_t frameCtr = (dispPtr && dispPtr != 0xffffffffu)
                                    ? r32m(dispPtr + 0xA0Cu) : 0xffffffffu;

                                std::ostringstream kk;
                                kk << std::hex << mainPtr << '/' << screenState << '/' << selectedEntry << '/'
                                   << std::dec << static_cast<int32_t>(cursor) << '/' << transition;
                                const std::string key = kk.str();
                                if (key != s_menuHexLastKey)
                                {
                                    s_menuHexLastKey = key;
                                    std::ostringstream m;
                                    m << "[menuhex] mainPtr=0x" << std::hex << mainPtr
                                      << " dispPtr=0x" << dispPtr << std::dec;
                                    m << " screen=0x" << std::hex << screenState
                                      << " selEntry=" << selectedEntry << std::dec
                                      << " cursor=" << static_cast<int32_t>(cursor)
                                      << " vis=0x" << std::hex << visibility
                                      << " trans=0x" << transition << std::dec
                                      << " dispFilter=" << (dispFilter == 0xffffffffu ? -1 : (int)(dispFilter & 0x7Fu))
                                      << " frame=" << frameCtr;
                                    std::cerr << m.str() << std::endl;

                                    // "caption" deducida: selEntry -> nombre de entry (10 entries)
                                    static const char *kEntryNames[10] = {
                                        "DRAGON_ROAD", "ULTIMATE_BATTLE", "WORLD_TOURNAMENT", "DUEL", "DRAGON_NET",
                                        "EVOLUCION_Z", "ENTRENAMIENTO", "DATA_CENTER", "REF_PERSONAJES", "OPCIONES"
                                    };
                                    const int32_t cur = static_cast<int32_t>(selectedEntry);
                                    if (cur >= 0 && cur < 10)
                                    {
                                    std::cerr << "[menuhex] selEntry=" << cur
                                              << " -> " << kEntryNames[cur] << std::endl;
                                    }
                                    else
                                    {
                                    std::cerr << "[menuhex] selEntry=" << cur
                                              << " (fuera de rango 0-9, indice de submenu?)" << std::endl;
                                    }

                                    // (b) Item state jump table (10 handlers)
                                    {
                                        std::ostringstream j;
                                        j << "[menuhex] jumpTable[0x3B4290]:";
                                        for (uint32_t a = 0x3B4290u; a < 0x3B42B8u; a += 4u)
                                            j << ' ' << std::hex << std::setw(8) << std::setfill('0') << r32m(a);
                                        std::cerr << j.str() << std::dec << std::endl;
                                    }

                                    // (c) Second dispatch table (5 handlers)
                                    {
                                        std::ostringstream d;
                                        d << "[menuhex] dispatch2[0x3B42C0]:";
                                        for (uint32_t a = 0x3B42C0u; a < 0x3B42D4u; a += 4u)
                                            d << ' ' << std::hex << std::setw(8) << std::setfill('0') << r32m(a);
                                        std::cerr << d.str() << std::dec << std::endl;
                                    }

                                    // (d) seleccion de la struct principal: ventana +0x00..+0x30 y +0x140..+0x150
                                    {
                                        std::ostringstream n;
                                        n << "[menuhex] mainPtr+0x00:";
                                        for (uint32_t off = 0x00u; off < 0x34u; off += 4u)
                                            n << ' ' << std::hex << std::setw(8) << std::setfill('0') << r32m(mainPtr + off);
                                        n << " | +0x140:";
                                        for (uint32_t off = 0x140u; off < 0x150u; off += 4u)
                                            n << ' ' << std::hex << std::setw(8) << std::setfill('0') << r32m(mainPtr + off);
                                        std::cerr << n.str() << std::dec << std::endl;
                                    }
                                }
                            }
                        }
                    }
                }
                // Scheduler-state dump (when PS2X_SCHED on): shows the deadlock -- which tid holds
                // the token (m_schedCurrent) and each thread's present/blocked/order/pc.
                if (m_schedEnabled)
                {
                    std::lock_guard<std::mutex> lk(m_schedMutex);
                    std::cerr << "[sched-state] current=" << m_schedCurrent << " |";
                    for (auto &kv : m_schedThreads)
                    {
                        SchedThread &s = *kv.second;
                        std::cerr << " tid" << kv.first << "(" << (s.present?"P":"-") << (s.blocked?"B":"-")
                                  << ",ord=" << s.order;
                        if (s.blocked) std::cerr << ",pc=0x" << std::hex << s.blockPc << ",ra=0x" << s.blockRa << std::dec;   // [schedwhy]
                        if (s.blocked)
                        {   // [schedwhy2]
                            int wt = 0, wid = 0, sc = -1, sw = -1, wk = 0;
                            if (ps2xThreadWaitInfo(kv.first, &wt, &wid, &sc, &sw, &wk))
                            {
                                std::cerr << ",wait=" << wt << "/" << wid;
                                if (sc >= 0) std::cerr << ",sema(cnt=" << sc << ",waiters=" << sw << ")";
                                std::cerr << ",wk=" << wk;
                            }
                        }
                        std::cerr << ")";
                    }
                    std::cerr << std::endl;
                }
                // Load-resource probe (PS2X_LOADPROBE): scan the resource array
                // (0x31c670 + id*96) that the demo-load spins on (FUN_002635c8 waits for
                // field +0x58 bit 0x4). Log entries that are IN PROGRESS (+0x58 != 0 && bit
                // 0x4 clear) -> which resource is stuck + its state/handle to trace it.
                {
                    static const bool s_lp = [](){ const char *v=std::getenv("PS2X_LOADPROBE"); return v&&v[0]&&v[0]!='0'; }();
                    const uint8_t *rd = m_memory.getRDRAM();
                    if (s_lp && rd)
                    {
                        for (uint32_t id = 0; id < 96; ++id)
                        {
                            uint32_t base = (0x31c670u + id * 96u) & PS2_RAM_MASK;
                            uint32_t f58 = 0, f00 = 0, f04 = 0, f54 = 0, f5c = 0;
                            std::memcpy(&f58, rd + ((base + 0x58u) & PS2_RAM_MASK), 4);
                            if (f58 == 0 || (f58 & 0x4u)) continue; // idle or ready -> skip
                            std::memcpy(&f00, rd + ((base + 0x00u) & PS2_RAM_MASK), 4);
                            std::memcpy(&f04, rd + ((base + 0x04u) & PS2_RAM_MASK), 4);
                            std::memcpy(&f54, rd + ((base + 0x54u) & PS2_RAM_MASK), 4);
                            std::memcpy(&f5c, rd + ((base + 0x5cu) & PS2_RAM_MASK), 4);
                            std::cerr << "[loadprobe] id=" << id << " @0x" << std::hex << (0x31c670u + id*96u)
                                      << " +0x58=0x" << f58 << " +0x54=0x" << f54 << " +0x5c=0x" << f5c
                                      << " +0x00=0x" << f00 << " +0x04=0x" << f04 << std::dec << std::endl;
                        }
                    }
                }
            }
        }

        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        // GPU mode: render the recorded GS command list into the FBO and present that
        // (its texture is bottom-up, so flip Y). Software mode: upload guest VRAM.
        const bool gpuMode = GsGpuRenderer::enabled();
#if !defined(PLATFORM_VITA)
        {   // [truews] aspect-aware TRUE widescreen. Generalizes the community 16:9 hack
            // (SLUS-21678_428113C2.pnach: FOV floats @2fe4cc/@2fe594 x4/3, lui 0.75->0.5625
            // @130bf0) to the ACTUAL window aspect: scale = aspect/(4:3). Originals are
            // captured before the first poke so toggling OFF restores them exactly; the lui
            // gets full-precision 0.75/scale via g_ps2xWsLui (the pnach could only patch the
            // upper 16 bits).
            static float s_wsOrig1 = 0.0f, s_wsOrig2 = 0.0f;
            float wsScale = 1.0f;
            if (PS2SettingsOverlay::isWidescreen() || wsTrigActive())
            {
                const float wsA = (float)GetScreenWidth() / (float)std::max(1, GetScreenHeight());
                wsScale = wsA / (4.0f / 3.0f);
                if (wsScale < 1.0f) wsScale = 1.0f;
                if (wsScale > 2.5f) wsScale = 2.5f;
            }
            if (wsNoFov()) wsScale = 1.0f;   // [wslever] guest half off
            if (const char *wsF = std::getenv("PS2X_WSFORCE"))
            {   // [wsforce] rig override: force the scale regardless of toggle/window aspect
                const float f = (float)std::atof(wsF);
                if (f >= 1.0f && f <= 2.5f) wsScale = f;
            }
            if (uint8_t *wsRd = m_memory.getRDRAM())
            {
                float *wp1 = reinterpret_cast<float *>(wsRd + 0x2fe4cc);
                float *wp2 = reinterpret_cast<float *>(wsRd + 0x2fe594);
                float *wp3 = reinterpret_cast<float *>(wsRd + 0x2fe58c);   // 149.333 = wp2/2
                static const uint32_t kK1Sib[] = {0x2c4384u, 0x2fc364u, 0x2fc368u,
                                                  0x2fcd48u, 0x2fcd60u, 0x2fe59cu, 0x2fe5a0u};
                if (s_wsOrig1 == 0.0f && *wp1 > 0.5f && *wp1 < 2.0f) s_wsOrig1 = *wp1;
                if (s_wsOrig2 == 0.0f && *wp2 > 100.0f && *wp2 < 500.0f) s_wsOrig2 = *wp2;
                static float s_wsOrig3 = 0.0f;
                if (s_wsOrig3 == 0.0f && *wp3 > 50.0f && *wp3 < 250.0f) s_wsOrig3 = *wp3;
                static float s_wsOrigSib[7] = {0,0,0,0,0,0,0};
                {   // [wsfovlog] PS2X_WSFOVLOG=1: what the game holds in the two patched
                    // projection floats vs what we force -- if the game rewrites them per
                    // mode (splitscreen), our once-captured originals clobber its value.
                    static const bool s_lg = [](){ const char *v = std::getenv("PS2X_WSFOVLOG");
                                                   return v && v[0] && v[0] != '0'; }();
                    static double s_t = -1.0;
                    if (s_lg && GetTime() - s_t > 1.0)
                    {
                        s_t = GetTime();
                        std::fprintf(stderr, "[wsfovlog] game=%.6f/%.3f orig=%.6f/%.3f scale=%.4f\n",
                                     *wp1, *wp2, s_wsOrig1, s_wsOrig2, wsScale);
                    }
                }
                // [wsfov2] @0x2fe594 is the CLIP/CULL extent as well as whatever places the
                // splitscreen pause menu. Not scaling it fixes the menu but culls geometry to
                // the old 4:3 window -- terrain at the sides pops in as the camera turns
                // (user-observed, and my FOV check missed it: it compared a static fight-start
                // camera, where nothing enters or leaves the frustum). So the default STAYS on
                // the pnach behaviour and PS2X_WSFOV2=0 is the opt-out.
                //   Threshold, swept live in a splitscreen pause: items survive up to 1.32 and
                // are gone from 1.3333 (= 4/3, the pnach's own value) upward -- so clamping at
                // 4/3 cannot save it either.
                // Old note kept for the record: scaling it is what threw the SPLITSCREEN menu's
                // items ~230 game px down, off the bottom of the frame (the box and the
                // "1P PAUSE" title, which do not go through this path, stayed put -- so the
                // menu read as "items missing"). Isolated with the per-knob levers in a live
                // P1-vs-P2 pause, all eight combinations: every state with this float scaled
                // loses the items, every state without it keeps them, independently of the
                // other two pokes.
                //   And it buys nothing. Two scripted runs that enter the fight with
                // widescreen already armed (it is latched at stage load, so a mid-fight
                // toggle measures nothing) differ by LESS than their own run-to-run noise:
                // background MAE 1.34 across the change vs 2.98 / 2.72 within each side, and
                // the best-fit view scale is kx=1.000 ky=1.000. The widening comes from
                // @0x2fe4cc and the sub_00130BA8 lui, both of which stay scaled.
                // PS2X_WSFOV2=1 restores the old both-floats behaviour.
                // [wsfov2] @0x2fe594 drives BOTH the clip/cull extent and whatever places the
                // splitscreen pause menu, and the two want different things:
                //   - the clipper wants the full wsScale (1.3333 at 16:9, 1.37 on a wider
                //     window); below that, terrain at the sides stops rendering until the
                //     camera turns toward it (user-observed live at scale 1.0).
                //   - the pause menu's item lines survive to 1.32 and are thrown ~230 game px
                //     off the bottom from 1.3333 up -- exactly 4/3, the value the community
                //     pnach itself uses, so clamping at 4/3 would not help.
                // 1.32 is the largest value that satisfies both: user-validated live for the
                // pop-in, rig-validated in a splitscreen pause for the menu. The margin is only
                // ~1% under the break point and was measured in one scene at one window size,
                // so if a mode ever loses its menu again, suspect this first.
                // PS2X_WSFOV2=1 restores the unclamped pnach behaviour.
                static const bool s_fov2 = [](){ const char *v = std::getenv("PS2X_WSFOV2");
                                                 return v && v[0] && v[0] != '0'; }();
                static const float s_k2Max = [](){ const char *v = std::getenv("PS2X_WSFOV2MAX");
                                                   const float f = v ? (float)std::atof(v) : 1.32f;
                                                   return (f > 0.5f && f <= 2.5f) ? f : 1.32f; }();
                const float k1 = wsKnob1(), k2 = wsKnob2();
                if (s_wsOrig1 != 0.0f) *wp1 = s_wsOrig1 * (k1 >= 0.0f ? k1 : wsScale);
                const float ws2 = s_fov2 ? wsScale : std::min(wsScale, s_k2Max);
                if (s_wsOrig2 != 0.0f) *wp2 = s_wsOrig2 * (k2 >= 0.0f ? k2 : ws2);
                const float k3 = wsKnob3(), k4 = wsKnob4();
                if (k3 >= 0.0f && s_wsOrig3 != 0.0f) *wp3 = s_wsOrig3 * k3;
                if (k4 >= 0.0f)
                    for (int si = 0; si < 7; ++si)
                    {
                        float *ws = reinterpret_cast<float *>(wsRd + kK1Sib[si]);
                        if (s_wsOrigSib[si] == 0.0f && *ws > 0.5f && *ws < 2.0f) s_wsOrigSib[si] = *ws;
                        if (s_wsOrigSib[si] != 0.0f) *ws = s_wsOrigSib[si] * k4;
                    }
            }
            extern float g_ps2xWsLui;
            const float kl = wsKnobLui();
            g_ps2xWsLui = 0.75f / (kl >= 0.0f ? kl : wsScale);
            extern float g_ps2xWsScale;
            g_ps2xWsScale = wsScale;
            static float s_wsLast = -1.0f;
            if (wsScale != s_wsLast)
            {
                s_wsLast = wsScale;
                std::fprintf(stderr, "[truews] scale=%.4f window=%dx%d orig=(%.4f, %.2f)\n",
                             wsScale, GetScreenWidth(), GetScreenHeight(), s_wsOrig1, s_wsOrig2);
            }
        }
#endif
        Texture2D presentTex = frameTex;
        bool flipY = false;
        if (gpuMode)
        {
            // FBO is the full framebuffer (FB_WIDTH x height); present only the DISPLAY
            // region (e.g. 512 wide) so the unrendered right edge isn't a black band.
            unsigned int texId = ps2GpuRenderer().renderAndGetTextureId(
                static_cast<int>(FB_WIDTH), static_cast<int>(DEFAULT_DISPLAY_HEIGHT));
            presentTex.id = texId;
            // The presented FBO is sized per-fbp (not necessarily FB_WIDTH x height);
            // normalize the present crop against its real GL texture size.
            presentTex.width = std::max(1, ps2GpuRenderer().presentTexWidth());
            presentTex.height = std::max(1, ps2GpuRenderer().presentTexHeight());
            presentTex.mipmaps = 1;
            presentTex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            flipY = true;
            int dw = ps2GpuRenderer().displayWidth();
            int dh = ps2GpuRenderer().displayHeight();
            if (dw > 0 && dw <= static_cast<int>(FB_WIDTH)) presentWidth = static_cast<uint32_t>(dw);
            if (dh > 0 && dh <= static_cast<int>(DEFAULT_DISPLAY_HEIGHT)) presentHeight = static_cast<uint32_t>(dh);
            static bool s_dlog = false;
            if (!s_dlog && dw > 0 && std::getenv("PS2X_GPU_DIAG")) { s_dlog = true; std::cerr << "[gpupresent] disp=" << dw << "x" << dh << " -> present=" << presentWidth << "x" << presentHeight << std::endl; }
        }
        else
        {
            UploadFrame(frameTex, this, presentWidth, presentHeight);
        }

#if defined(__linux__)
        // Refresh the native evdev reader for any Linux gamepad that GLFW cannot map.
        ps2_stubs::PadEvdevLinux::instance().update();
#endif
        if (gpuMode) ps2GpuRenderer().serviceBlockingBarriers();   // [barblock]
        BeginDrawing();
        {   // [presentstate] pre-render chunks and barrier services run GL work between presents and
            // leave the GS emulation state behind (blend off / GS blend factors, scissor, colour mask,
            // custom shader, texture unit). The frame blit and the ImGui overlay assume raylib's
            // defaults: the overlay's glyph quads drawn with blending off are solid white rectangles.
            rlDrawRenderBatchActive();
            rlDisableScissorTest();
            rlDisableDepthTest();
            rlEnableColorBlend();
            rlSetBlendMode(RL_BLEND_ALPHA);
            rlColorMask(true, true, true, true);
            rlActiveTextureSlot(0);
            rlDisableShader();
        }
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
        float dstWidth = srcWidth * scale;
        float dstHeight = srcHeight * scale;
        // [tv43] Authentic 4:3 presentation: the PS2 CRTC displays ANY mode width (512- or
        // 640-wide buffers) as a full 4:3 TV picture. Square-pixel letterboxing showed the
        // fight's ~512-wide buffer ~14% too narrow (the HUD's "6" badge measured as a tall
        // oval; on hardware it is a circle -- the widescreen path, which squeezes to true
        // 4:3 proportions, drew it round). PS2X_SQPIX=1 restores the old square-pixel
        // letterbox (the rig's boot screen-matching references were captured that way).
        {
            static const bool s_sqpix = [](){ const char *v = std::getenv("PS2X_SQPIX"); return v && v[0] && v[0] != '0'; }();
            static const float s_pixk = [](){ const char *v = std::getenv("PS2X_PIXK"); const float f = v ? (float)std::atof(v) : 1.08f; return (f > 0.5f && f < 2.0f) ? f : 1.08f; }();
            if (!s_sqpix)
            {   // measured against a native-4:3 Wii longplay capture: authentic TV pixels are
                // ~8% wider than square (portrait-frame k=1.076, infinity-glyph k=1.095) --
                // NOT the full 4:3-of-the-crop stretch (that overshot ~13%).
                const float ls = std::min((float)screenWidth / (srcWidth * s_pixk), (float)screenHeight / (float)srcHeight);
                dstWidth = srcWidth * s_pixk * ls;
                dstHeight = srcHeight * ls;
            }
        }
        // [overlay] Widescreen: stretch to fill instead of letterboxing.
#if !defined(PLATFORM_VITA)
        if (PS2SettingsOverlay::isWidescreen() || wsTrigActive())
        {
            dstWidth = screenWidth;
            dstHeight = screenHeight;
            // [wshud] target: HUD elements keep authentic proportions (square-pixel x k)
            // under the full-window stretch. inv = desired h-scale / actual h-scale.
            static const float s_pixk2 = [](){ const char *v = std::getenv("PS2X_PIXK"); const float f = v ? (float)std::atof(v) : 1.08f; return (f > 0.5f && f < 2.0f) ? f : 1.08f; }();
            extern float g_ps2xWsHudInv;
            extern float g_ps2xWsSrcW;
            g_ps2xWsSrcW = (float)srcWidth;
            const float hEff = dstWidth / (float)srcWidth;
            const float vEff = dstHeight / (float)srcHeight;
            float inv = (vEff * s_pixk2) / hEff;
            if (inv < 0.4f) inv = 0.4f; if (inv > 1.0f) inv = 1.0f;
            if (wsNoHud()) inv = 1.0f;       // [wslever] renderer half off
            g_ps2xWsHudInv = inv;
        }
        else
        {
            extern float g_ps2xWsHudInv;
            g_ps2xWsHudInv = 1.0f;   // letterbox mode: no HUD squeeze
        }
#endif
        // Atlas mode presents a sub-rect of the big atlas texture -> crop from the display slot origin.
        const float srcX = flipY ? static_cast<float>(ps2GpuRenderer().presentSrcX()) : 0.0f;
        const float srcY = flipY ? static_cast<float>(ps2GpuRenderer().presentSrcY()) : 0.0f;
        const Rectangle srcRect{srcX, srcY, srcWidth, flipY ? -srcHeight : srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        // Blend-free present: the GPU FBO's alpha channel now carries GS dest-alpha (the
        // game's per-pixel masks, legitimately 0 over most of the frame) — alpha-blending
        // the final blit would punch the frame transparent to the clear color.
        rlSetBlendFactorsSeparate(0x0001 /*GL_ONE*/, 0x0000 /*GL_ZERO*/, 0x0001, 0x0000, 0x8006 /*GL_FUNC_ADD*/, 0x8006);
        BeginBlendMode(BLEND_CUSTOM_SEPARATE);
        // [rscale] present the scaled scene with LINEAR sampling: at render scale N the
        // texture is N x native, and any non-integer window ratio point-decimates --
        // ground shake at 720p windows, HUD shimmer at 1080p. The renderer re-applies
        // its own per-draw filters, so this only affects the present.
        if (GsGpuRenderer::renderScale() > 1)
            SetTextureFilter(presentTex, TEXTURE_FILTER_BILINEAR);
        DrawTexturePro(presentTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        EndBlendMode();
        {   // [presentlog] PS2X_PRESENTLOG=1: print every CHANGE of the present geometry (a 60 Hz alternation shows as a
            // stream of transitions; a static picture shows two lines total).
            static const bool s_pl = [](){ const char *v = std::getenv("PS2X_PRESENTLOG"); return v && v[0] && v[0] != '0'; }();
            if (s_pl)
            {
                static unsigned long s_n = 0; ++s_n;
                static float prev[10] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
                const float cur[10] = {(float)presentTex.id, (float)presentTex.width, (float)presentTex.height, srcX, srcY, srcWidth, srcHeight, screenWidth, screenHeight, dstRect.x};
                bool ch = false; for (int i = 0; i < 10; ++i) ch |= (cur[i] != prev[i]);
                if (ch) { std::fprintf(stderr, "[presentlog] #%lu tex=%u %dx%d src=(%.1f,%.1f %gx%g) screen=%gx%g dst=(%.2f,%.2f %.2fx%.2f)\n", s_n, presentTex.id, presentTex.width, presentTex.height, srcX, srcY, srcWidth, srcHeight, screenWidth, screenHeight, dstRect.x, dstRect.y, dstRect.width, dstRect.height); for (int i = 0; i < 10; ++i) prev[i] = cur[i]; }
            }
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        {   // [frameprof] PS2X_FRAMEPROF=1: where does the main-loop frame go? (display-path dips)
            static const bool s_fp = [](){ const char *v = std::getenv("PS2X_FRAMEPROF"); return v && v[0] && v[0] != '0'; }();
            extern double g_fpPresent, g_fpBar, g_fpPre, g_fpWait, g_fpLoop; extern int g_fpN;
            const auto tP0 = std::chrono::steady_clock::now();
            EndDrawing();
            if (s_fp)
            {
                const auto t1 = std::chrono::steady_clock::now();
                g_fpPresent += std::chrono::duration<double, std::milli>(t1 - tP0).count();
                ++g_fpN;
                static auto lastPrint = t1;
                static auto lastLoop = t1;
                g_fpLoop += std::chrono::duration<double, std::milli>(t1 - lastLoop).count(); lastLoop = t1;
                if (std::chrono::duration<double>(t1 - lastPrint).count() >= 1.0)
                {
                    lastPrint = t1;
                    const double n = (double)std::max(1, g_fpN);
                    std::fprintf(stderr, "[frameprof] n=%d loop %.1f ms: present %.2f bar %.2f pre %.2f wait %.2f (other %.2f)\n",
                                 g_fpN, g_fpLoop / n, g_fpPresent / n, g_fpBar / n, g_fpPre / n, g_fpWait / n,
                                 (g_fpLoop - g_fpPresent - g_fpBar - g_fpPre - g_fpWait) / n);
                    g_fpPresent = g_fpBar = g_fpPre = g_fpWait = g_fpLoop = 0; g_fpN = 0;
                }
            }
        }
        {   // [ftspike] PS2X_FTSPIKE=1: PER-FRAME time spikes. The [fps] line is a ~1s average;
            // a 29.9 mean can hide 50-80 ms hitch frames that FEEL like dips ("the dips are
            // noticable" with min-28.8 logs). Tracks inter-present deltas: per second prints
            // the worst frame, a bucket histogram, and the count of frames over 40/50 ms.
            static const bool s_fts = [](){ const char *v = std::getenv("PS2X_FTSPIKE"); return v && v[0] && v[0] != '0'; }();
            if (s_fts)
            {
                static auto last = std::chrono::steady_clock::now();
                static auto lastPr = last;
                static double mx = 0; static int n = 0, b25 = 0, b33 = 0, b40 = 0, b50 = 0, b80 = 0;
                const auto now = std::chrono::steady_clock::now();
                const double dt = std::chrono::duration<double, std::milli>(now - last).count();
                last = now; ++n;
                if (dt > mx) mx = dt;
                if (dt > 80.0) ++b80; else if (dt > 50.0) ++b50; else if (dt > 40.0) ++b40; else if (dt > 34.5) ++b33; else if (dt > 26.0) ++b25;
                if (std::chrono::duration<double>(now - lastPr).count() >= 1.0)
                {
                    lastPr = now;
                    std::fprintf(stderr, "[ftspike] n=%d max=%.1fms  >26:%d >34.5:%d >40:%d >50:%d >80:%d\n",
                                 n, mx, b25, b33, b40, b50, b80);
                    mx = 0; n = 0; b25 = b33 = b40 = b50 = b80 = 0;
                }
            }
        }

        // Drain any streaming PCM into the audio device. Must run on the render thread --
        // raylib's AudioStream calls are not safe to make from the guest threads that
        // produce the data (see PS2AudioBackend::onStreamPcm / serviceStreams).
        audioBackend().serviceStreams();
        static const bool s_barPace2 = [](){ const char *v = std::getenv("PS2X_BARPACE"); return !(v && v[0] == '0'); }();
        if (gpuMode && GsGpuRenderer::blockingBarriersEnabled() && s_barPace2)
        {   // [barblock] pace to 60 Hz ourselves, servicing barrier requests while we wait.
            static auto s_next = std::chrono::steady_clock::now();
            const auto period = std::chrono::microseconds(16667);
            auto now = std::chrono::steady_clock::now();
            if (s_next + std::chrono::milliseconds(50) < now) s_next = now;   // fell behind: resync
            s_next += period;
            while ((now = std::chrono::steady_clock::now()) < s_next)
            {
                const auto tA = std::chrono::steady_clock::now();
                const bool svcIdle = !ps2GpuRenderer().serviceBlockingBarriers();
                { extern double g_fpBar; g_fpBar += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tA).count(); }
                if (svcIdle)
                {
                    // [pubbreak] a published frame is waiting: present it now instead of idling to the deadline.
                    // prerenderChunk() refuses to run while a list is pending (ordering), so every microsecond spent
                    // here after a publish is pre-render coverage lost -> a bigger render leg at the next barrier
                    // (outline SJ3: render leg 366 ms/60 frames vs 127 before the guest got faster).
                    static const bool s_pb = [](){ const char *v = std::getenv("PS2X_PUBBREAK"); return !(v && v[0] == '0'); }();
                    if (s_pb && ps2GpuRenderer().hasPendingFrame()) break;
                    const auto tB = std::chrono::steady_clock::now();
                    const bool didPre = ps2GpuRenderer().prerenderChunk();
                    { extern double g_fpPre; g_fpPre += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tB).count(); }
                    if (didPre) continue;   // [prerender] draw ahead while the guest records

                    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(s_next - now).count();
                    const auto tC = std::chrono::steady_clock::now();
                    ps2GpuRenderer().waitForBlockingBarrierRequest((int)std::max<long long>(1, left));   // cv-driven: wakes on a request, else sleeps to the frame deadline
                    { extern double g_fpWait; g_fpWait += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tC).count(); }
                }
            }
        }

        // PS2X_TEXWATCH (default ON, =0 disables): find the recompiled-EE writer of the
        // CORRUPTED floor TEX0 (tw=10 where real HW authors tw=8 — the stage-texture root
        // cause). Every ~2s, scan EE RAM for a TEX0-shaped qword with tbp0=10752, psm=19,
        // tw=10 in the display-list arena; on the first hit, arm the guest write-watch
        // (g_ps2WatchLo/Hi) on that qword. The arena is rebuilt every frame, so the next
        // rewrite reports the writer's guest PC/RA via [camwrite] on stderr.
        {
            // Disabled by default — enable with PS2X_TEXWATCH=1.  The full 32MB
            // per-frame scan is expensive and only needed to diagnose the
            // corrupted floor TEX0 (tw=10).
            static const bool s_txw = [](){ const char *v = std::getenv("PS2X_TEXWATCH"); return v && v[0] == '1'; }();
            static int s_txState = 0; // 0=scanning, 1=armed
            static std::chrono::steady_clock::time_point s_txLast{};
            static std::chrono::steady_clock::time_point s_txArmT{};
            static uint32_t s_txLastAddr = 0;
            // Self-correcting: an armed watch that stays SILENT for 5s hit a stale copy
            // (e.g. a loading-screen leftover) — the live display list rebuilds at shifting
            // addresses. Rescan until a slot reports writes.
            if (s_txState == 1)
            {
                extern std::atomic<uint32_t> g_watchReportN;
                if (g_watchReportN.load(std::memory_order_relaxed) == 0u &&
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - s_txArmT).count() >= 5.0)
                {
                    extern uint32_t g_txBad[8]; extern int g_txBadN;
                    g_txBad[g_txBadN & 7] = s_txLastAddr; ++g_txBadN;
                    s_txState = 0;
                    std::fprintf(stderr, "[texwatch] 0x%x silent 5s (stale copy) — rescanning\n", s_txLastAddr);
                }
            }
            // EVERY present (not every 2s): the corrupted TEX0 provably reaches the GS each
            // frame ([floortex0] raw=0x2006580629342a00) yet a 2s-interval full-RAM scan
            // never found it — the packet lives in RAM only transiently between display-list
            // build and DMA consumption. Scan the arena region per-frame until armed.
            if (s_txw && s_txState == 0 && m_boundRdram)
            {
                (void)s_txLast;
                const uint64_t *q = reinterpret_cast<const uint64_t *>(m_boundRdram);
                const size_t nq = (32u * 1024u * 1024u) / 8u;
                uint32_t best = 0u; uint64_t bestVal = 0; int nHits = 0;
                for (size_t i = 0; i < nq; ++i)
                {
                    const uint64_t v = q[i];
                    // TEX0 fields: tbp0[13:0] psm[25:20] tw[29:26] th[33:30] cpsm[54:51] cld[63:61].
                    // The corrupted floor TEX0 has tw=10 with th=8, cpsm=0, cld=1 (upper half
                    // known from eedump/GS-dump) — the extra fields kill float false-positives.
                    // Exact match: the live corrupted floor TEX0, verified at the GS register
                    // this session ([floortex0] raw=0x2006580629342a00, tw=10 tbw=16).
                    if (v == 0x2006580629342a00ull)
                    {
                        const uint32_t addr = static_cast<uint32_t>(i * 8u);
                        extern uint32_t g_txBad[8]; extern int g_txBadN;
                        bool bad = false;
                        for (int bi = 0; bi < 8 && bi < g_txBadN; ++bi)
                            if (g_txBad[bi] == addr) { bad = true; break; }
                        if (bad) continue;
                        ++nHits;
                        if (nHits <= 8)
                            std::fprintf(stderr, "[texwatch] match #%d at 0x%x val=0x%016llx\n",
                                         nHits, addr, (unsigned long long)v);
                        // Prefer a hit inside the display-list arena region (~0x40'0000-0x100'0000,
                        // where eedump found the live TEX0 at 0x6bda90 / twin ~0x7c17xx).
                        if (best == 0u || (addr >= 0x400000u && addr < 0x1000000u && (best < 0x400000u || best >= 0x1000000u)))
                        { best = addr; bestVal = v; }
                    }
                }
                if (best != 0u)
                {
                    extern std::atomic<uint32_t> g_watchReportN;
                    g_ps2WatchLo.store(best, std::memory_order_relaxed);
                    g_ps2WatchHi.store(best + 8u, std::memory_order_relaxed);
                    g_ps2WatchAll.store(1u, std::memory_order_relaxed);
                    g_watchReportN.store(0u, std::memory_order_relaxed); // the camera watch burns the budget at boot
                    s_txState = 1;
                    s_txArmT = std::chrono::steady_clock::now();
                    s_txLastAddr = best;
                    std::fprintf(stderr, "[texwatch] ARMED on 0x%x val=0x%016llx (%d matches) — writer PC follows as [camwrite]\n",
                                 best, (unsigned long long)bestVal, nHits);
                }
            }
        }

        // PS2X_EEDUMP=1: ~10s after the first 3D map draw (fight underway), dump the full
        // 32MB EE RAM once to work/eedump_fight.bin. Offline TEX0-scan against the PCSX2
        // mid-fight eeMemory.bin answers: does OUR port's stage builder still author the
        // terrain draw packets (tbp0 10816/10880/10944/10992) that never reach the GS?
        {
            // PS2X_EEDUMP=1: single dump 10s after fight detect (legacy behavior).
            // PS2X_EEDUMP=N (N>1): N dumps, first 6s after fight detect then every 3s, to
            // work/eedump_0..N-1.bin — for diffing idle vs held-input windows (movement hunt).
            static const int s_eedN = [](){ const char *v = std::getenv("PS2X_EEDUMP"); return v ? std::atoi(v) : 0; }();
            if (s_eedN > 0 && m_boundRdram)
            {
                extern std::atomic<bool> g_ps2xMapDrawSeen;
                static std::chrono::steady_clock::time_point s_t0;
                static int s_state = 0; // 0=waiting for fight, 1=armed
                static int s_done = 0;
                if (s_state == 0 && g_ps2xMapDrawSeen.load(std::memory_order_relaxed))
                {
                    s_t0 = std::chrono::steady_clock::now();
                    s_state = 1;
                    std::fprintf(stderr, "[eedump] fight detected, dump series starts in 6s (%d dumps)\n", s_eedN);
                }
                else if (s_state == 1 && s_done < s_eedN)
                {
                    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
                    const double due = (s_eedN == 1) ? 10.0 : (6.0 + 3.0 * s_done);
                    if (el >= due)
                    {
                        char dp[128];
                        if (s_eedN == 1)
                            std::snprintf(dp, sizeof dp, "/home/z3/Desktop/bt3/work/eedump_fight.bin");
                        else
                            std::snprintf(dp, sizeof dp, "/home/z3/Desktop/bt3/work/eedump_%d.bin", s_done);
                        if (FILE *f = std::fopen(dp, "wb"))
                        {
                            std::fwrite(m_boundRdram, 1, 32u * 1024u * 1024u, f);
                            std::fclose(f);
                            std::fprintf(stderr, "[eedump] wrote %s (t=%.1fs)\n", dp, el);
                        }
                        ++s_done;
                    }
                }
            }
        }

        // Lightweight always-on perf line: host present FPS + rasterizer workload
        // (primitives/sec). A jump in prims/sec after the scanline-parallel change
        // means the guest is completing frames faster (render no longer the wall).
        {
            extern std::atomic<uint64_t> g_rasterPrimCount;
            extern std::atomic<uint64_t> g_rasterPixelCount;
            static uint32_t s_fpsFrames = 0u;
            static uint64_t s_lastPrims = 0u;
            static uint64_t s_lastPix = 0u;
            static uint64_t s_lastSwaps = 0u;
            static std::chrono::steady_clock::time_point s_fpsT = std::chrono::steady_clock::now();
            ++s_fpsFrames;
            // Wall-clock sample of the MAIN thread's PC (m_cpuContext.pc): shows where
            // the main loop actually spends TIME (incl. blocked in a syscall), immune
            // to side-thread branch-count pollution. Confirms the real fps gate.
            static std::unordered_map<uint32_t, uint32_t> s_mainPcHist;
            s_mainPcHist[m_cpuContext.pc & 0xFFFFFFF0u]++;
            auto nowT = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(nowT - s_fpsT).count();
            if (dt >= 1.0)
            {
                uint32_t t1 = 0, n1 = 0, t2 = 0, n2 = 0, tot = 0;
                for (auto &kv : s_mainPcHist) { tot += kv.second;
                    if (kv.second > n1) { t2=t1;n2=n1; n1=kv.second; t1=kv.first; }
                    else if (kv.second > n2) { n2=kv.second; t2=kv.first; } }
                std::cerr << "[main] hot=0x" << std::hex << t1 << std::dec << " " << (tot?100*n1/tot:0)
                          << "% 2nd=0x" << std::hex << t2 << std::dec << " " << (tot?100*n2/tot:0)
                          << "% (samples=" << tot << ")" << std::endl;
                s_mainPcHist.clear();
                uint64_t prims = g_rasterPrimCount.load(std::memory_order_relaxed);
                uint64_t pix = g_rasterPixelCount.load(std::memory_order_relaxed);
                uint64_t swaps = g_displaySwapCounter.load(std::memory_order_relaxed);
                extern std::atomic<uint64_t> g_bt3FrameCount;
                static uint64_t s_lastGameFrames = 0u;
                uint64_t gameFrames = g_bt3FrameCount.load(std::memory_order_relaxed);
                extern std::atomic<uint64_t> g_guestBusyNs, g_guestBusyFrames, g_guestWallNs;
                static uint64_t s_lastBusyNs = 0, s_lastBusyFrames = 0, s_lastWallNs = 0;
                const uint64_t bn = g_guestBusyNs.load(std::memory_order_relaxed), bf = g_guestBusyFrames.load(std::memory_order_relaxed);
                const uint64_t wn = g_guestWallNs.load(std::memory_order_relaxed);
                const double guestMs = (bf > s_lastBusyFrames) ? (double)(bn - s_lastBusyNs) / 1.0e6 / (double)(bf - s_lastBusyFrames) : 0.0;   // [guestbusy] ms of guest CPU per published frame
                const double wallMs = (bf > s_lastBusyFrames) ? (double)(wn - s_lastWallNs) / 1.0e6 / (double)(bf - s_lastBusyFrames) : 0.0;   // [guestwall]
                s_lastBusyNs = bn; s_lastBusyFrames = bf; s_lastWallNs = wn;
                static unsigned long long s_lastGlCalls = 0, s_lastGlFlush = 0; static double s_lastFlushNs = 0.0;
                const unsigned long long glc = g_rlglDrawCalls, glf = g_rlglBatchFlushes;
                extern std::atomic<unsigned long> g_texDecodeCount; static unsigned long s_lastTdc = 0;
                static unsigned long s_lastHoistTris = 0;   // [glhoist]
                const unsigned long tdc = g_texDecodeCount.load(std::memory_order_relaxed);
                std::cerr << "[fps] GAME=" << (double)((gameFrames - s_lastGameFrames) / dt)
                          << " guest_ms=" << guestMs
                          << " wall_ms=" << wallMs
                          << " host=" << (uint32_t)(s_fpsFrames / dt)
                          << " prims/sec=" << (uint64_t)((prims - s_lastPrims) / dt)
                          << " glhoist/sec=" << [&]{ extern std::atomic<unsigned long> g_glHoistCmds, g_glHoistTris;   // [glhoist]
                                 static unsigned long s_lc = 0, s_lt = 0;
                                 const unsigned long cc = g_glHoistCmds.load(std::memory_order_relaxed), tt = g_glHoistTris.load(std::memory_order_relaxed);
                                 const unsigned long d = (unsigned long)((cc - s_lc) / dt); s_lastHoistTris = (unsigned long)((tt - s_lt) / dt);
                                 s_lc = cc; s_lt = tt; return d; }()
                          << " glhoisttris/sec=" << s_lastHoistTris
                          << " vu1pairs/sec=" << [&]{ extern std::atomic<uint64_t> g_vu1PairCount;   // [vupairs]
                                 static uint64_t s_lastVp = 0; const uint64_t vp = g_vu1PairCount.load(std::memory_order_relaxed);
                                 const uint64_t d = (uint64_t)((vp - s_lastVp) / dt); s_lastVp = vp; return d; }()
                          << " Mpix/sec=" << (double)((pix - s_lastPix) / dt / 1.0e6)
                          << " swaps/sec=" << (uint64_t)((swaps - s_lastSwaps) / dt)
                          << " glcalls/sec=" << (uint64_t)((glc - s_lastGlCalls) / dt)
                          << " glflush/sec=" << (uint64_t)((glf - s_lastGlFlush) / dt)
                          << " decodes/sec=" << (uint64_t)((tdc - s_lastTdc) / dt)
                          << " flush_ms/s=" << (g_rlglFlushNs - s_lastFlushNs) / 1.0e6 / dt << std::endl;
                s_lastGlCalls = glc; s_lastGlFlush = glf; s_lastTdc = tdc; s_lastFlushNs = g_rlglFlushNs;
                if (gprof::g_on)
                {   // [guestprof] exclusive phase time on the guest thread(s), ms per second; tsc calibrated over this interval
                    static uint64_t s_lastTsc = 0, s_lastAcc[gprof::NPHASE] = {0};
                    const uint64_t tsc = __rdtsc();
                    if (s_lastTsc)
                    {
                        const double nsPerTick = (dt * 1.0e9) / (double)(tsc - s_lastTsc);
                        std::cerr << "[guestprof]";
                        for (int i = 0; i < gprof::NPHASE; ++i)
                        {
                            const uint64_t a = gprof::g_acc[i].load(std::memory_order_relaxed);
                            if (i >= 12) { s_lastAcc[i] = a; continue; }
                            std::cerr << ' ' << gprof::kPhaseName[i] << '=' << (double)(a - s_lastAcc[i]) * nsPerTick / 1.0e6 / dt;
                            s_lastAcc[i] = a;
                        }
                        {   extern std::atomic<unsigned long> g_recTplHit, g_recTplMiss; static unsigned long s_lh = 0, s_lm = 0;
                            const unsigned long hh = g_recTplHit.load(std::memory_order_relaxed), mm = g_recTplMiss.load(std::memory_order_relaxed);
                            std::cerr << " ms/s | rectpl hit=" << (unsigned long)((hh - s_lh) / dt) << "/s miss=" << (unsigned long)((mm - s_lm) / dt) << "/s";
                            s_lh = hh; s_lm = mm; }
                        {   extern std::atomic<unsigned long> g_recTplWhy[5]; static unsigned long s_lw[5] = {0};
                            static const char *const kWhy[5] = {"gen", "epoch", "frame", "prim", "draw"};
                            std::cerr << " why:";
                            for (int i = 0; i < 5; ++i) { const unsigned long w = g_recTplWhy[i].load(std::memory_order_relaxed); std::cerr << ' ' << kWhy[i] << '=' << (unsigned long)((w - s_lw[i]) / dt); s_lw[i] = w; } }
                        {   // [drawbatch] tri/s appended into an open batch, batches opened, and how many of
                            // those appends came from the recordCmd MERGE rather than the recorder's fast path
                            extern std::atomic<unsigned long> g_batchTri, g_batchHead, g_batchMerge;
                            static unsigned long s_bt = 0, s_bh = 0, s_bm = 0;
                            const unsigned long bt = g_batchTri.load(std::memory_order_relaxed);
                            const unsigned long bh = g_batchHead.load(std::memory_order_relaxed);
                            const unsigned long bm = g_batchMerge.load(std::memory_order_relaxed);
                            std::cerr << " | batch tri=" << (unsigned long)((bt - s_bt) / dt)
                                      << "/s heads=" << (unsigned long)((bh - s_bh) / dt)
                                      << "/s merged=" << (unsigned long)((bm - s_bm) / dt) << "/s";
                            s_bt = bt; s_bh = bh; s_bm = bm; }
                        {   extern std::atomic<unsigned long> g_regWriteHist[0x63]; static unsigned long s_lr[0x63] = {0};
                            unsigned long d[0x63]; for (int i = 0; i < 0x63; ++i) { const unsigned long w = g_regWriteHist[i].load(std::memory_order_relaxed); d[i] = w - s_lr[i]; s_lr[i] = w; }
                            std::cerr << " | regwrites/s:";
                            for (int k = 0; k < 6; ++k) { int best = -1; for (int i = 0; i < 0x63; ++i) if (d[i] && (best < 0 || d[i] > d[best])) best = i; if (best < 0) break; std::cerr << " 0x" << std::hex << best << std::dec << '=' << (unsigned long)(d[best] / dt); d[best] = 0; } }
                        std::cerr << std::endl;
                    }
                    s_lastTsc = tsc;
                }
                s_lastGameFrames = gameFrames;
                s_lastPrims = prims;
                s_lastPix = pix;
                s_lastSwaps = swaps;
                s_fpsFrames = 0u;
                s_fpsT = nowT;
            }
        }

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (GsGpuRenderer::enabled()) ps2GpuRenderer().abortBlockingBarriers();   // [barblock]

#ifdef __linux__
    keepStop.store(true, std::memory_order_relaxed);
    if (pinKeeper.joinable()) pinKeeper.join();
#endif

    const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gameThreadFinished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < joinDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (gameThread.joinable())
    {
        if (gameThreadFinished.load(std::memory_order_acquire))
        {
            gameThread.join();
        }
        else
        {
            std::cerr << "[run] game thread did not stop within timeout; detaching" << std::endl;
            gameThread.detach();
        }
    }

    const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (g_activeThreads.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < workerDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        requestStop();
        const auto finalWorkerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (g_activeThreads.load(std::memory_order_relaxed) > 0 &&
               std::chrono::steady_clock::now() < finalWorkerDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (g_activeThreads.load(std::memory_order_relaxed) == 0)
    {
        ps2_syscalls::joinAllGuestHostThreads();
    }
    else
    {
        std::cerr << "[run] guest host threads did not stop within timeout; detaching remaining worker threads"
                  << std::endl;
        ps2_syscalls::detachAllGuestHostThreads();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    UnloadTexture(frameTex);
    CloseWindow();

    const int remainingThreads = g_activeThreads.load(std::memory_order_relaxed);
    RUNTIME_LOG("[run] exiting loop, activeThreads=" << remainingThreads);
    if (remainingThreads > 0)
    {
        std::cerr << "[run] warning: " << remainingThreads
                  << " guest worker thread(s) still active during shutdown." << std::endl;
    }
}
