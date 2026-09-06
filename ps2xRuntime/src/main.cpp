#include "ps2_runtime.h"
#include "ps2_compat.h"
#if defined(_WIN32)
extern "C" void ps2xWinTimerBegin();   // ps2_win_timer.cpp (a linkage specification must be at namespace scope)
extern "C" void ps2xWinCrashHandlerInstall();   // ps2_win_timer.cpp: [wincrash] print the exception before Windows swallows it
extern "C" void ps2xWinHostInfo();             // ps2_win_timer.cpp: [host] cpu / clock / core count
#endif
#include "runtime/ps2_gs_gpu_renderer.h"
#include "games_database.h"
#if !defined(PLATFORM_VITA)
#include "ps2_settings_overlay.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <cstring>
#include <cstdio>
#include <chrono>
#include <vector>
#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <csignal>

namespace
{
    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

// ---------------------------------------------------------------------------------------
// Vsync counter for the replay harness. The dev tree defines this in the renderer (its
// probes frame-gate on it); this build has no such probes, so the harness owns it here.
int g_ps2ReplayVsync = 0;

static double g_benchGsMs = 0.0, g_benchRenderMs = 0.0; static long g_benchFrames = 0;   // [replaybench] deterministic cost of record (GS parse) vs GL replay
// PS2X_GS_REPLAY: offline GS-dump replay harness. DIAGNOSTIC ONLY -- inert unless the env var
// is set. Feeds a PCSX2 .gs dump's packet stream through our own GS, so one frame can be
// reproduced deterministically and diffed against the console screenshot embedded in the
// dump: no gameplay, no timing. Ported from the dev tree 2026-08-22 so that work on this
// clean repo build is measurable. Adds no behaviour to a normal run.
// ---------------------------------------------------------------------------------------
// [wshudinv] The replay never runs ps2_runtime.cpp's present, which is where the widescreen
// squeeze factor g_ps2xWsHudInv is computed -- so it stays 1.0 and EVERY widescreen effect is
// inert offline. That is why a widescreen-only artifact could not be reproduced from a dump.
// PS2X_WSHUDINV=<f> forces it (live value for a 1280x720 window over a 512x448 source is
// (720/448 * 1.08) / (1280/512) = 0.694).
static void ps2xReplayApplyWsHudInv()
{
    static const float f = [](){ const char *v = std::getenv("PS2X_WSHUDINV");
                                 const float x = v && v[0] ? (float)std::atof(v) : 0.0f;
                                 return (x > 0.2f && x <= 1.0f) ? x : 0.0f; }();
    if (f > 0.0f) { extern float g_ps2xWsHudInv; g_ps2xWsHudInv = f; }
}

// [replayw] the replay has always rendered at a hardcoded 512 while the LIVE path passes
// FB_WIDTH (640). That asymmetry means the replay cannot reproduce anything that depends on
// the render width -- e.g. content occupying only 512 of a 640-wide scene FBO. PS2X_REPLAYW
// lets the replay match live. Default stays 512 so every existing measurement is unchanged.
static int ps2xReplayRenderW()
{
    static const int w = [](){ const char *v = std::getenv("PS2X_REPLAYW");
                               const int n = v && v[0] ? std::atoi(v) : 512;
                               return (n >= 256 && n <= 1024) ? n : 512; }();
    return w;
}
static void gsReplayDumpBuf(GS &gs, uint32_t fbp, uint32_t fbw, int w, int h, int vsync)
{
    char p[160];
    std::snprintf(p, sizeof p, "/home/z3/Desktop/bt3/work/gsreplay_v%d_f%u.ppm", vsync, fbp);
    FILE *f = std::fopen(p, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> alpha((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const uint32_t v = GSMem::ReadCT32(gs.vramData(), fbp * 32u, fbw, (uint32_t)x, (uint32_t)y);
            std::fputc(v & 0xFF, f); std::fputc((v >> 8) & 0xFF, f); std::fputc((v >> 16) & 0xFF, f);
            alpha[(size_t)y * w + x] = (uint8_t)((v >> 24) & 0xFF);
        }
    std::fclose(f);
    std::snprintf(p, sizeof p, "/home/z3/Desktop/bt3/work/gsreplay_v%d_f%u_a.pgm", vsync, fbp);
    if (FILE *fa = std::fopen(p, "wb"))
    {
        std::fprintf(fa, "P5\n%d %d\n255\n", w, h);
        std::fwrite(alpha.data(), 1, alpha.size(), fa);
        std::fclose(fa);
    }
}

static int runGsReplay(PS2Runtime &rt, const char *path)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "[gsreplay] cannot open %s\n", path); return 1; }
    std::fseek(f, 0, SEEK_END); const long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d((size_t)sz);
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) { std::fclose(f); return 1; }
    std::fclose(f);
    uint32_t magic; std::memcpy(&magic, d.data(), 4);
    GS &gs = rt.gs();
    size_t off = 0; int sliceBase = 0;
    if (magic == 0xFFFFFFFFu)
    {
        // PCSX2 dump: header + state (leads with 4MB VRAM) + privileged regs + packets.
        uint32_t stateSize, ssize;
        std::memcpy(&stateSize, d.data() + 0x0C, 4);
        std::memcpy(&ssize, d.data() + 0x28, 4);
        const size_t state = 0x36 + ssize;
        // PS2X_GS_REPLAY_NOSEED=1: skip the VRAM seed. The seed hands every pass real console
        // content; without it the replay starts from empty VRAM, closer to how a live run
        // begins. Used to test whether the seed is what makes the effect column strips visible
        // in replay when they are invisible live.
        static const bool s_noSeed = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_NOSEED"); return v && v[0] && v[0] != '0'; }();
        if (!s_noSeed && gs.vramData() && stateSize >= 4u * 1024 * 1024 && state + stateSize <= (size_t)sz)
        {
            // [vramseedfix] 2026-08-31: VRAM is the LAST 4MB of the state blob, not its head
            // (gsvram.py layout, validated): seeding from `state` copied a 509-byte register
            // prefix into VRAM and shifted every texel -- the mottled terrain on every
            // console-dump replay. Live runs never seed; replays of PCSX2 dumps before this
            // fix carried a corrupted static-texture base.
            std::memcpy(gs.vramData(), d.data() + state + stateSize - 4u * 1024 * 1024, std::min<size_t>(4u * 1024 * 1024, gs.vramSize()));
            // Seeding VRAM behind the renderer's back leaves its texture/CLUT caches keyed to
            // content that no longer exists, so terrain tiles decode through stale palettes --
            // that is the red vertical striping the first port produced. Stamp the whole of
            // VRAM as freshly uploaded so every cached decode is re-keyed. 4MB = 16384 blocks
            // of 256 bytes.
            ps2GpuRenderer().onVramUpload(0u, 16384u);
        }
        off = state + stateSize + 8192;
    }
    else if (magic == 0xFFFFFFFEu)
    {
        // [slice] PS2X replay slice written by PS2X_GS_REPLAY_SLICE (see the vsync branch below):
        //   u32 magic 0xFFFFFFFE | u32 baseVsync | u32 nregs | nregs x {u8 addr, u64 value}
        //   | u32 vramBytes | VRAM | raw records...
        // Restores the register file through one synthetic A+D GIF packet, seeds VRAM, and
        // continues the vsync count from baseVsync so FROM/TO keep their full-recording meaning.
        uint32_t base = 0, nregs = 0; std::memcpy(&base, d.data() + 4, 4); std::memcpy(&nregs, d.data() + 8, 4);
        size_t p = 12;
        std::vector<uint8_t> pkt; pkt.reserve(16 + 16 * nregs);
        auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) pkt.push_back((uint8_t)(v >> (8 * i))); };
        put64((uint64_t)nregs | (1ull << 15) | (1ull << 60)); put64(0xEull);   // NLOOP, EOP, NREG=1, REGS=A+D
        for (uint32_t i = 0; i < nregs; ++i)
        { const uint8_t a = d[p]; uint64_t v; std::memcpy(&v, d.data() + p + 1, 8); p += 9; put64(v); put64(a); }
        uint32_t vb = 0; std::memcpy(&vb, d.data() + p, 4); p += 4;
        if (gs.vramData() && vb && p + vb <= (size_t)sz)
        { std::memcpy(gs.vramData(), d.data() + p, std::min<size_t>(vb, gs.vramSize())); ps2GpuRenderer().onVramUpload(0u, 16384u); }
        p += vb;
        gs.processGIFPacket(pkt.data(), (uint32_t)pkt.size());
        off = p; sliceBase = (int)base;
        std::fprintf(stderr, "[slice] loaded: base vsync %u, %u registers restored, %u VRAM bytes, records from %zu\n", base, nregs, vb, off);
    }
    else if (d[0] == 0u)
    {
        // Raw PS2X_GS_RECORD stream: packets from byte 0, no VRAM snapshot (the
        // recording contains every upload since boot, so VRAM fills as it plays).
        off = 0;
    }
    else { std::fprintf(stderr, "[gsreplay] unrecognized file format\n"); return 1; }

    // (PS2X_GS_REPLAY_VRAMSEED omitted: it needs the dev tree's noup seeding helper.)

    // have no vsync markers).
    const long dumpEvery = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_DUMPEVERY"); return v ? std::atol(v) : 0L; }();
    const long dumpFrom = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_FROM"); return v ? std::atol(v) : 0L; }();
    const long dumpTo = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_TO"); return v ? std::atol(v) : 0L; }();
    const long fdrawAt = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_FDRAWAT"); return v ? std::atol(v) : 0L; }();
    // PS2X_GS_REPLAY_LOOP=<n>: replay the whole dump n times. ref_native.gs holds only 4
    // vsyncs, which is too few for BT3's multi-stage feedback chain (Z -> scene alpha -> fbp224
    // -> outline composite) to converge -- each stage sees the previous frame's writeback.
    const long replayLoops = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_LOOP");
                                   const long n = v ? std::atol(v) : 1L; return n > 0 ? n : 1L; }();
    const size_t offStart = off;
    int vsyncs = sliceBase; uint64_t xfers = 0; bool sawVsync = false;
    // PS2X_GS_REPLAY_SLICE=<out> + PS2X_GS_REPLAY_SLICEAT=<vsync> [+ PS2X_GS_REPLAY_SLICEEND=<vsync>]:
    // at that vsync, write a slice (registers + VRAM + the records up to SLICEEND) and stop.
    const char *slicePath = std::getenv("PS2X_GS_REPLAY_SLICE");
    const long sliceAt = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_SLICEAT"); return v ? std::atol(v) : 0L; }();
    const long sliceEnd = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_SLICEEND"); return v ? std::atol(v) : 0L; }();
    std::fprintf(stderr, "[gsreplay] %s: %ld bytes, packets from %zu (loops=%ld)\n", path, sz, off, replayLoops);
    for (long loopIter = 0; loopIter < replayLoops; ++loopIter)
    {
    if (loopIter > 0) { off = offStart; std::fprintf(stderr, "[gsreplay] --- loop %ld (vsyncs so far %d) ---\n", loopIter, vsyncs); }
    while (off < (size_t)sz)
    {
        const uint8_t t = d[off++];
        if (t == 0)
        {
            ++off; // path id (all paths carry GIF-format data)
            uint32_t n; std::memcpy(&n, d.data() + off, 4); off += 4;
            if (off + n > (size_t)sz) break;
            { const auto _b0 = std::chrono::steady_clock::now();
              gs.processGIFPacket(d.data() + off, n);
              g_benchGsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _b0).count(); }
            off += n; ++xfers;
            // PS2X_BARRIER: drain read-after-write barriers. A draw that samples a page an
            // earlier queued draw wrote cannot see it, because textures are decoded while the
            // command list is BUILT and the list is rendered later. Here (single-threaded
            // replay, GL owned by this thread) we can publish + render what is built so far and
            // push that page into VRAM, so the next decode reads fresh bytes. FBOs persist
            // across render calls, so splitting a frame into segments is safe.
            if (GsGpuRenderer::enabled())
            {
                static const bool s_bar = [](){ const char *v = std::getenv("PS2X_BARRIER");
                                                return v && v[0] && v[0] != '0'; }();
                static const bool s_livepath = [](){ const char *v = std::getenv("PS2X_LIVEPATH"); return v && v[0] && v[0] != '0'; }();
                uint32_t bpage = 0;
                while (s_bar && !s_livepath && ps2GpuRenderer().takeBarrierRequest(bpage))
                {
                    // renderRange, NOT swapFrame + renderAndGetTextureId: publishing mid-frame
                    // and running the full render made the display pick, the per-frame extent
                    // census and the present all fire against a PREFIX of the frame, which
                    // wrecked the picture (MAE 11.9 -> 72.7). renderRange draws only the
                    // not-yet-drawn commands into the FBOs and returns.
                    ps2GpuRenderer().renderRange(512, 448);
                    ps2GpuRenderer().flushPageToVram(bpage);
                    static int nb = 0;
                    if (++nb <= 40)
                        std::fprintf(stderr, "[barrier] #%d rendered pending + flushed page %u\n", nb, bpage);
                }
            }
            if (!sawVsync && dumpEvery > 0 && (xfers % (uint64_t)dumpEvery) == 0)
            {
                // Our live game renders 512-wide (fbw 8); the console dump 640 (fbw 10).
                const int tag = (int)(xfers / (uint64_t)dumpEvery);
                std::fprintf(stderr, "[gsreplay] periodic dump %d at xfer %llu\n", tag, (unsigned long long)xfers);
                if (GsGpuRenderer::enabled())
                {
                    // GPU-path replay: publish the accumulated DrawCmds, render on this
                    // (GL-owning) thread, save the present — the GPU-vs-SW diff harness.
                    ps2GpuRenderer().swapFrame();
                    ps2xReplayApplyWsHudInv(); ps2GpuRenderer().renderAndGetTextureId(ps2xReplayRenderW(), 448);
                    char gp[160];
                    std::snprintf(gp, sizeof gp, "/home/z3/Desktop/bt3/work/gsreplay_gpu_%02d.png", tag);
                    ps2GpuRenderer().debugSavePresent(gp);
                }
                else
                {
                    gsReplayDumpBuf(gs, 0u, 8u, 512, 448, 1000 + tag);
                    gsReplayDumpBuf(gs, 112u, 8u, 512, 448, 2000 + tag);
                }
            }
        }
        else if (t == 1)
        {
            ++off; ++vsyncs; sawVsync = true;
            if (magic == 0xFFFFFFFFu && dumpTo <= 0)
            {
                // Console dump: one-frame capture — dump every buffer and stop.
                std::fprintf(stderr, "[gsreplay] vsync %d after %llu transfers\n", vsyncs, (unsigned long long)xfers);
                gsReplayDumpBuf(gs, 0u, 10u, 640, 448, vsyncs);
                gsReplayDumpBuf(gs, 112u, 10u, 640, 448, vsyncs);
                gsReplayDumpBuf(gs, 224u, 8u, 512, 448, vsyncs);
                gsReplayDumpBuf(gs, 336u, 4u, 256, 256, vsyncs);
                gsReplayDumpBuf(gs, 368u, 2u, 128, 128, vsyncs);
                gsReplayDumpBuf(gs, 502u, 1u, 64, 64, vsyncs);
                if (vsyncs >= 2) break;
            }
            else
            {
                // Raw recording with frame markers: publish per frame (exact live
                // cadence); render+save every dumpEvery-th frame in GPU mode.
                const bool inWin = dumpTo <= 0 || (vsyncs >= dumpFrom && vsyncs <= dumpTo);
                // Warm-up: RENDER (without saving) the frames just before the window. The
                // GPU queue drops published lists nobody renders, so a window opened cold
                // starts from empty FBOs — the effect chain (f336/f368/f502 feedback) needs
                // a few real frames of history before its content is meaningful.
                const long kWarm = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_WARM"); return v ? std::atol(v) : 40L; }();
                const bool inWarm = dumpTo > 0 && vsyncs >= dumpFrom - kWarm && vsyncs < dumpFrom;
                { extern int g_ps2ReplayVsync; g_ps2ReplayVsync = vsyncs; }
                if (GsGpuRenderer::enabled())
                {
                    if (fdrawAt > 0 && vsyncs == fdrawAt)
                    if (std::getenv("PS2X_BODYDUMP"))
                        std::fprintf(stderr, "[bodyfr] %d\n", vsyncs);
                    ps2GpuRenderer().swapFrame();
                    { extern bool g_replayInWindow; g_replayInWindow = inWin; }
                    if ((dumpEvery > 0 && (vsyncs % (int)dumpEvery) == 0 && inWin) || (dumpTo > 0 && inWin))
                    {
                        { const auto _r0 = std::chrono::steady_clock::now();
                          ps2xReplayApplyWsHudInv(); ps2GpuRenderer().renderAndGetTextureId(ps2xReplayRenderW(), 448);
                          g_benchRenderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _r0).count(); ++g_benchFrames; }
                        // PS2X_GS_REPLAY_OUT=<dir>: where the per-frame PNGs land (default
                        // work/). Per-variant dirs let A/B replays run in parallel.
                        static const char *outDir = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_OUT");
                                                          return (v && v[0]) ? v : "/home/z3/Desktop/bt3/work"; }();
                        char gp[256];
                        std::snprintf(gp, sizeof gp, "%s/gsreplay_gpu_fr%04d.png", outDir, vsyncs);
                        ps2GpuRenderer().debugSavePresent(gp);
                    }
                    else if (inWarm)
                    {   // [replaybench] warm frames render without a PNG save: the timing window
                        const auto _r0 = std::chrono::steady_clock::now();
                        ps2xReplayApplyWsHudInv(); ps2GpuRenderer().renderAndGetTextureId(ps2xReplayRenderW(), 448);
                        g_benchRenderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _r0).count(); ++g_benchFrames;
                    }
                    // PS2X_GS_REPLAY_VDUMP=1: in GPU mode ALSO dump the effect-chain buffers
                    // from the VRAM MIRROR (what PS2X_SWEFFECT rasterized) — shows whether the
                    // pyramid content exists in VRAM independently of the GL composite.
                    static const bool s_vdump = [](){ const char *v = std::getenv("PS2X_GS_REPLAY_VDUMP"); return v && v[0] && v[0] != '0'; }();
                    if (s_vdump && dumpTo > 0 && vsyncs >= dumpFrom && vsyncs <= dumpTo)
                    {
                        gsReplayDumpBuf(gs, 224u, 4u, 256, 256, 30000 + vsyncs);
                        gsReplayDumpBuf(gs, 336u, 4u, 256, 256, 40000 + vsyncs);
                        gsReplayDumpBuf(gs, 368u, 2u, 128, 128, 50000 + vsyncs);
                        gsReplayDumpBuf(gs, 502u, 1u, 64, 64, 60000 + vsyncs);
                        gsReplayDumpBuf(gs, 112u, 8u, 512, 448, 70000 + vsyncs);
                    }
                }
                else if ((dumpEvery > 0 && (vsyncs % (int)dumpEvery) == 0 && inWin) || (dumpTo > 0 && inWin))
                {
                    // BT3 renders the scene at FBW=8 (512px) on console too — the 640 in
                    // DISPFB is the display window, not the render stride. Dumping at 640
                    // shears the frame into diagonal staircase bands.
                    gsReplayDumpBuf(gs, 0u, 8u, 512, 448, 10000 + vsyncs);
                    gsReplayDumpBuf(gs, 112u, 8u, 512, 448, 20000 + vsyncs);
                    gsReplayDumpBuf(gs, 224u, 8u, 512, 448, 30000 + vsyncs);
                    gsReplayDumpBuf(gs, 336u, 4u, 256, 256, 40000 + vsyncs);
                    gsReplayDumpBuf(gs, 368u, 2u, 128, 128, 50000 + vsyncs);
                    gsReplayDumpBuf(gs, 502u, 1u, 64, 64, 60000 + vsyncs);
                }
            }
            if (slicePath && slicePath[0] && sliceAt > 0 && vsyncs == sliceAt)
            {
                if (GsGpuRenderer::enabled()) ps2GpuRenderer().flushRecentPagesToVram(vsyncs - 3);
                std::vector<uint8_t> o; auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i))); };
                auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back((uint8_t)(v >> (8 * i))); };
                put32(0xFFFFFFFEu); put32((uint32_t)vsyncs);
                const uint64_t *rr = gs.rawRegs(); const bool *rs = gs.rawRegsSet();
                std::vector<uint8_t> order;
                for (int a = 1; a < 0x63; ++a)
                {   // vertex kicks (0x02-0x05, 0x0C-0x0D), TEXFLUSH, TRXDIR, SIGNAL/FINISH/LABEL are not state
                    if (a == 0x02 || a == 0x03 || a == 0x04 || a == 0x05 || a == 0x0C || a == 0x0D || a == 0x3F || a == 0x53 || a >= 0x60) continue;
                    if (rs[a]) order.push_back((uint8_t)a);
                }
                if (rs[0]) order.push_back(0u);   // PRIM last (after PRMODECONT/PRMODE)
                put32((uint32_t)order.size());
                for (uint8_t a : order) { o.push_back(a); put64(rr[a]); }
                put32(gs.vramSize()); o.insert(o.end(), gs.vramData(), gs.vramData() + gs.vramSize());
                size_t e = off; int vs2 = vsyncs;   // records up to SLICEEND
                while (e < (size_t)sz)
                {
                    const uint8_t t2 = d[e];
                    if (t2 == 0) { uint32_t n2; std::memcpy(&n2, d.data() + e + 2, 4); e += 6 + n2; }
                    else if (t2 == 1) { if (sliceEnd > 0 && vs2 >= sliceEnd) break; e += 2; ++vs2; }
                    else if (t2 == 2) e += 5; else if (t2 == 3) e += 8193;
                    else if (t2 == 4) e += 57;                     // [recpriv] CRTC snapshot
                    else break;
                }
                if (e > (size_t)sz) e = (size_t)sz;
                o.insert(o.end(), d.data() + off, d.data() + e);
                if (FILE *sf = std::fopen(slicePath, "wb")) { std::fwrite(o.data(), 1, o.size(), sf); std::fclose(sf); }
                std::fprintf(stderr, "[slice] wrote %s: %.1f MB, base vsync %d, %zu registers, records for vsyncs %d..%d\n",
                             slicePath, o.size() / 1e6, vsyncs, order.size(), vsyncs, vs2);
                break;
            }
            // Window done: the rest of the recording can't change what was already dumped.
            if (dumpTo > 0 && vsyncs > dumpTo)
            {
                // PS2X_GS_REPLAY_VRAMDUMP=<path>: raw 4MB VRAM snapshot at window end. Diffing
                // a SW-mode dump against a GPU-mode one isolates what GS RENDERING put in VRAM
                // (GPU mode rasterizes into FBOs, never back into VRAM) — i.e. exactly which
                // sampled regions are render-target readback rather than uploaded textures.
                if (const char *vp = std::getenv("PS2X_GS_REPLAY_VRAMDUMP"))
                    if (gs.vramData())
                        if (FILE *vf = std::fopen(vp, "wb"))
                        { std::fwrite(gs.vramData(), 1, gs.vramSize(), vf); std::fclose(vf);
                          std::fprintf(stderr, "[gsreplay] VRAM dumped to %s\n", vp); }
                std::fprintf(stderr, "[gsreplay] window end at vsync %d\n", vsyncs);
                break;
            }
        }
        else if (t == 2) { off += 4; }
        else if (t == 3) { off += 8192; }
        else if (t == 4)
        {   // [recpriv] CRTC snapshot: pmode, dispfb1, display1, dispfb2, display2, bgcolor, smode2
            if (off + 56 > (size_t)sz) break;
            uint64_t v[7]; std::memcpy(v, d.data() + off, 56); off += 56;
            gs.setPrivRegsFromRecord(v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
            static int s_pn = 0;
            if (s_pn < 8) { ++s_pn;
                std::fprintf(stderr, "[recpriv] CRTC restored: pmode=%016llx EN1=%d EN2=%d | "
                                     "dispfb1 fbp=%u fbw=%u psm=%u  display1 DX=%u DY=%u DW=%u DH=%u | "
                                     "dispfb2 fbp=%u fbw=%u psm=%u  display2 DX=%u DY=%u DW=%u DH=%u\n",
                             (unsigned long long)v[0], (int)(v[0] & 1), (int)((v[0] >> 1) & 1),
                             (unsigned)(v[1] & 0x1FF), (unsigned)((v[1] >> 9) & 0x3F), (unsigned)((v[1] >> 15) & 0x1F),
                             (unsigned)(v[2] & 0xFFF), (unsigned)((v[2] >> 12) & 0x7FF),
                             (unsigned)((v[2] >> 32) & 0xFFF), (unsigned)((v[2] >> 44) & 0x7FF),
                             (unsigned)(v[3] & 0x1FF), (unsigned)((v[3] >> 9) & 0x3F), (unsigned)((v[3] >> 15) & 0x1F),
                             (unsigned)(v[4] & 0xFFF), (unsigned)((v[4] >> 12) & 0x7FF),
                             (unsigned)((v[4] >> 32) & 0xFFF), (unsigned)((v[4] >> 44) & 0x7FF)); }
        }
        else { std::fprintf(stderr, "[gsreplay] unknown packet type %u at %zu\n", t, off - 1); break; }
    }
    }

    std::fprintf(stderr, "[gsreplay] done: %d vsyncs, %llu transfers\n", vsyncs, (unsigned long long)xfers);
    { extern std::atomic<unsigned long> g_texDecodeCount;
      {   // [glhoist] the replay prints no [fps] line, and a hoist gate that never holds looks exactly like
          // a hoist that does nothing -- so the parity gate has to be able to say it actually engaged
          extern std::atomic<unsigned long> g_glHoistCmds, g_glHoistTris;
          std::fprintf(stderr, "[replaybench] glhoist: %lu commands, %lu triangles\n",
                       g_glHoistCmds.load(std::memory_order_relaxed), g_glHoistTris.load(std::memory_order_relaxed)); }
      std::fprintf(stderr, "[replaybench] frames=%ld gs_ms=%.1f render_ms=%.1f | per frame: gs(record)=%.2f ms render(GL)=%.2f ms | texdecodes=%lu\n",
                 g_benchFrames, g_benchGsMs, g_benchRenderMs, g_benchFrames ? g_benchGsMs / g_benchFrames : 0.0, g_benchFrames ? g_benchRenderMs / g_benchFrames : 0.0,
                 g_texDecodeCount.load()); }
    return 0;
}

extern "C" void ps2xGsRecordFlush();      // PS2X_GS_RECORD ring buffer (ps2_gs_gpu.cpp)
extern "C" void ps2xGsRecordOnSignal(int);

int main(int argc, char *argv[])
{
#if defined(_WIN32)
    ps2xWinTimerBegin();   // [wintimer] 1 ms tick: timed waits stop rounding to 15.6 ms
    ps2xWinCrashHandlerInstall();
    ps2xWinHostInfo();
#endif
    // Write the captured tail out however we exit, so a long play session is not lost.
    // atexit alone was NOT enough -- closing the window skipped it and lost a whole session.
    std::atexit(ps2xGsRecordFlush);
    std::signal(SIGINT, ps2xGsRecordOnSignal);
    std::signal(SIGTERM, ps2xGsRecordOnSignal);
    setupTerminateLogger();

    // ---- BT3 defaults ----------------------------------------------------------
    // The validated playing configuration (GPU path + outline/ink chain + graded DoF
    // + the barrier-elimination serving + grass anti-swim) used to require ~25 env
    // vars; a bare run gave the stripped software renderer. Bake them in as DEFAULTS:
    // setenv(..., 0) never overwrites, so every PS2X_* override still works exactly
    // as before (including =0 to disable a single flag).
    // PS2X_NODEFAULTS=1 skips the whole block (bare engine, for debugging).
    if (!std::getenv("PS2X_NODEFAULTS"))
    {
        // Track which vars WE defaulted (vs user-set): consumers that must respect an
        // EXPLICIT user env override (the overlay's INI loader) check PS2X_DEFAULTED.
        static std::string s_defaulted = ",";
        auto def = [](const char *n, const char *v)
        { if (!std::getenv(n)) { s_defaulted += n; s_defaulted += ','; setenv(n, v, 1); } };
        def("PS2X_GPU", "1"); def("PS2X_GPU_DEPTH", "1"); def("PS2X_ZSCALE", "256");
        def("PS2X_TEXAKEY", "1"); def("PS2X_BARGATE", "1"); def("PS2X_NOSHCOMP", "1");
        def("PS2X_FLUSHCT32", "1"); def("PS2X_ALPHA128", "1"); def("PS2X_DUALSRC", "1");
        def("PS2X_ASPLIT", "1"); def("PS2X_FBODIRTY", "1"); def("PS2X_BARRIER", "1");
        def("PS2X_AFLUSH", "1"); def("PS2X_CT32ALPHA", "1"); def("PS2X_EDGEWRAP", "1");
        def("PS2X_TEXCACHEMB", "64"); def("PS2X_BARBLOCK", "1");
        def("PS2X_GPUALIAS", "4"); def("PS2X_DOFMASK", "2"); def("PS2X_DOFZFAR", "200000");
        def("PS2X_ALPHAONLYFBO", "1"); def("PS2X_SCENESKIP", "1"); def("PS2X_F336SKIP", "1");
        def("PS2X_IDXRT", "1"); def("PS2X_IDXONLY", "1"); def("PS2X_MASKBUILDSKIP", "1");
        def("PS2X_P8TWIN", "1"); def("PS2X_SHCOMPSKIP", "15972,16012,16004,16008,16016,16020,16024");
        def("PS2X_BT3_CDTICK", "1"); def("PS2X_SCHED", "1"); def("PS2X_FORCE_MC", "1");
        // [asyncdefault] ON by user decision 2026-09-07. The old reason for holding it back --
        // "better averages but 80-157 ms hitch frames" -- no longer applies: [asyncpace] fixed the
        // fast-forward (readIORegister was clearing CHCR.STR while the worker still had the channel,
        // so the guest saw every DMA complete instantly) and [framegate] fixed the pacing. Measured
        // on an i5-12400: 1P 22.7 -> 29.9 fps, splitscreen 16.8 -> 22.6, guest 19.77 -> 3.33 ms.
        //
        // ⚠ STILL OWED, and the reason this was held back until now: the async races are not
        // cleaned up -- guest GS-priv-reg writes vs the worker, VIF1 register writes, the SW-mode
        // latch -- and no corruption soak has been run. Two user access-violation crash reports
        // exist (2026-09-07, see [[runtime-threading-race]]); one had async on, one had it off, so
        // async is not the sole cause, but it is the one subsystem with known-unresolved races.
        // PS2X_ASYNC_KICK=0 opts out (def() never overwrites an explicit value).
        def("PS2X_ASYNC_KICK", "1");
        setenv("PS2X_DEFAULTED", s_defaulted.c_str(), 1);
        // Deliberately NOT defaulted: PS2X_BARSTAT (diagnostic spam), PS2X_TIMERMULT.
    }

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        PS2Runtime runtime;
#if !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2SettingsOverlay settingsOverlay;
        settingsOverlay.preloadSettings();
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2SettingsOverlay *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2SettingsOverlay *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2SettingsOverlay *>(userData)->shutdown();
            },
            &settingsOverlay);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (const char *rp = std::getenv("PS2X_GS_REPLAY"))
            return runGsReplay(runtime, rp);

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        runtime.run();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
