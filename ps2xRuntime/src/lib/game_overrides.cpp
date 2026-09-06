#include "ps2_runtime_macros.h"
#include "game_overrides.h"
#include "ps2_runtime.h"

// [guestbusy-tid] Defined in ps2_gs_gpu_renderer.cpp; declared HERE at file scope on purpose.
// Declaring it inside bt3FrameKick put it in this file's anonymous namespace, which silently
// created a SECOND internal-linkage symbol and failed at link with
// "undefined reference to (anonymous namespace)::g_guestThreadCpuNs".
extern std::atomic<uint64_t> g_guestThreadCpuNs;

// [framegate] vsync tick source, declared at file scope for the same reason as the above.
namespace ps2_syscalls { uint64_t GetCurrentVSyncTick(); }
extern std::atomic<uint64_t> g_workerFrameNs;   // [framegate] kick worker busy ns, last frame

// [guestbusy-tid] FILE SCOPE, not inside bt3FrameKick. Declaring this extern "C" inside a function
// body -- which sits in this file's anonymous namespace -- builds silently on Linux clang and
// FAILS on clang-cl, which is how the Windows build broke at 2026-09-06 23:0x. Same rule as
// g_guestThreadCpuNs above and the note in bt3-windows-build: file-scope externs go at file scope.
#if defined(_WIN32)
extern "C" unsigned long long ps2xWinThreadCpuNs();
#endif
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include "runtime/pad_config.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_gs_gpu_renderer.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <map>
#include <set>
#include <atomic>
#include <chrono>
#include <thread>   // [framegate] std::this_thread::sleep_for
#include <ctime>    // [guestbusy-tid] clock_gettime on POSIX
#include <optional>
#include <vector>
#include <unordered_map>
#include <cstring>

// [wlk] overlay-table externs (file scope — block-scope extern inside the anon namespace mislinks)
extern PS2Runtime::RecompiledFunction g_ps2OverlayFunctionTable[];
extern const uint32_t g_ps2OverlayFunctionTableBase;
extern const uint32_t g_ps2OverlayFunctionTableSlotCount;

// Live host input (keyboard + gamepad) as a 16-bit active-low PS2 button word +
// analog bytes. Defined in src/lib/pad_config.cpp. `player` selects the profile
// (0..3); BT3 routes socket 0 -> player 0, socket 1 -> player 1.
namespace ps2_stubs
{
    uint16_t ps2xLivePadButtons(int player, uint8_t &lx, uint8_t &ly, uint8_t &rx, uint8_t &ry);
}

// External-linkage game-frame counter (read by the [fps] line in ps2_runtime.cpp).
std::atomic<uint64_t> g_bt3FrameCount{0};

namespace
{
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t size)
    {
        static std::array<uint32_t, 256> table = []()
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t c = i;
                for (int bit = 0; bit < 8; ++bit)
                {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
                }
                values[i] = c;
            }
            return values;
        }();

        uint32_t out = crc;
        for (size_t i = 0; i < size; ++i)
        {
            out = table[(out ^ data[i]) & 0xFFu] ^ (out >> 8u);
        }
        return out;
    }

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        std::array<uint8_t, 4096> chunk{};
        uint32_t crc = 0xFFFFFFFFu;

        while (file.good())
        {
            file.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = file.gcount();
            if (got <= 0)
            {
                break;
            }
            crc = crc32Update(crc, chunk.data(), static_cast<size_t>(got));
        }

        crcOut = ~crc;
        return true;
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }
}

namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            std::cerr << "[game_overrides] ignoring descriptor with null apply callback." << std::endl;
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            std::cerr << "[game_overrides] unresolved handler '" << handlerName
                      << "' for address 0x" << std::hex << address << std::dec << std::endl;
            return false;
        }

        return runtime.replaceFunction(address, resolved.value());
    }

    void applyMatching(PS2Runtime &runtime, const std::string &elfPath, uint32_t entry)
    {
        ps2_syscalls::clearSoundDriverCompatLayout();
        ps2_syscalls::clearDtxCompatLayout();

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        if (descriptors.empty())
        {
            return;
        }

        const std::string elfName = basenameFromPath(elfPath);
        uint32_t fileCrc32 = 0u;
        bool fileCrcComputed = false;
        bool fileCrcValid = false;

        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcComputed)
                {
                    fileCrcComputed = true;
                    fileCrcValid = computeFileCrc32(elfPath, fileCrc32);
                    if (!fileCrcValid)
                    {
                        std::cerr << "[game_overrides] failed to compute CRC32 for '" << elfPath << "'" << std::endl;
                    }
                }

                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            RUNTIME_LOG("[game_overrides] applying '" << name << "'");
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            RUNTIME_LOG("[game_overrides] applied " << appliedCount << " matching override(s).");
        }
    }
}

namespace
{
    void applyRecvxSoundDriverCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        // Trying to explain a bit of Resident Evil Code: Veronica X sound-driver guest globals.
        // Update these guest addresses/callback PCs when porting the override to another build:
        // - checksum tables back the SE/MIDI status values mirrored through the snddrv RPC stubs
        // - busyFlagAddr is the guest-side "work in progress" word cleared on completion
        // - completion/clearBusy callbacks are guest PCs reached when async snddrv work finishes
        PS2SoundDriverCompatLayout layout{};
        layout.primarySeCheckAddr = 0x01E0EF10u;
        layout.primaryMidiCheckAddr = 0x01E0EF20u;
        layout.fallbackSeCheckAddr = 0x01E1EF10u;
        layout.fallbackMidiCheckAddr = 0x01E1EF20u;
        layout.busyFlagAddr = 0x01E212C8u;
        layout.completionCallbacks = {0x002EAC20u, 0x002EAC30u, 0x002FAC20u, 0x002FAC30u};
        layout.clearBusyCallbacks = {0x002EAC30u, 0x002FAC30u};
        ps2_syscalls::setSoundDriverCompatLayout(layout);
    }

    void applyRecvxDtxCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        // Trying to explain abit of Resident Evil Code: Veronica X DTX guest layout.
        // Update these guest values when porting the middleware override to another build:
        // - rpcSid identifies the DTX RPC service the guest binds/registers
        // - urpc object/table addresses back the SJX/PS2RNA/SJRMT command tables
        // - dispatcherFuncAddr is the guest-side DTX RPC handler used for URPC dispatch
        PS2DtxCompatLayout layout{};
        layout.rpcSid = 0x7D000000u;
        layout.urpcObjBase = 0x01F18000u;
        layout.urpcObjLimit = 0x01F1FF00u;
        layout.urpcObjStride = 0x20u;
        layout.urpcFnTableBase = 0x0034FED0u;
        layout.urpcObjTableBase = 0x0034FFD0u;
        layout.dispatcherFuncAddr = 0x002FABC0u;
        ps2_syscalls::setDtxCompatLayout(layout);
    }

    void applyLotrSoundRpcCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        PS2SoundDriverCompatLayout layout{};
        layout.completionCallbacks = {0x001FFD70u, 0u, 0u, 0u};
        ps2_syscalls::setSoundDriverCompatLayout(layout);
    }

    // Dragon Ball Z: Budokai Tenkaichi 3 (SLUS_216.78): the SJX/SVM sound
    // middleware's SJX_Init traps boot in an error loop because the IOP sound
    // subsystem is stubbed. Force the IOP init primitives whose zero return
    // gates the "can't allocate IOP Heap" / "can't create DTX" loops to report
    // success so SJX_Init falls through and boot proceeds. Triage bypass only.
    // ---- BT3 virtual controller (sceDbc pad) ----------------------------------
    // BT3 gates its first in-game screen on the sceDbc pad reporting a connected,
    // ready controller AND returning valid pad packets. The IOP DBC/pad module is
    // not emulated. Rather than half-report "ready" (which destabilises init) or
    // poke shared DBC state, replace the pad-accessor functions with a
    // consistent virtual controller: connected, ready, host input, sticks
    // centered. This is a complete pad, so init and the boot wait both proceed
    // cleanly. Per-player input is routed by socket index (a0): scePad2CreateSocket
    // is overridden to return the descriptor's player byte (0/1), and each read
    // accessor maps socket -> player profile from the host pad configurator.
    void writeNeutralPadPacket(uint8_t *rdram, uint32_t bufAddr, uint32_t socket)
    {
        // TEST (env PS2X_SOUNDREADY): force the sound-ready flags that FUN_0026d9a0
        // sets (0x2c9fc8..0x2ca028, +0x10) so we can see if the game's progression
        // is gated on the sound-init handshake completing.
        static const bool s_sr = [](){ const char *v = std::getenv("PS2X_SOUNDREADY"); return v && v[0] && v[0] != '0'; }();
        if (s_sr)
        {
            for (uint32_t a = 0x2c9fc8u; a <= 0x2ca028u; a += 0x10u)
                if (uint8_t *fp = getMemPtr(rdram, a))
                    *reinterpret_cast<uint32_t *>(fp) = 1u;
        }
        if (bufAddr == 0u)
        {
            return;
        }
        uint8_t *p = getMemPtr(rdram, bufAddr);
        if (!p)
        {
            return;
        }
        // Live host input (keyboard + gamepad) for the given socket/player ->
        // PS2 pad packet. buttons active-low (0xff = released); game does
        // (hi<<8|lo) ^ 0xffff.
        uint8_t lx = 0x80u, ly = 0x80u, rx = 0x80u, ry = 0x80u;
        const uint16_t buttons = ps2_stubs::ps2xLivePadButtons(static_cast<int>(socket & 3u), lx, ly, rx, ry);
        uint8_t b0 = static_cast<uint8_t>(buttons & 0xffu);
        uint8_t b1 = static_cast<uint8_t>((buttons >> 8) & 0xffu);
        // TEST (env PS2X_AUTOSTART): also tap START+CROSS periodically to auto-advance.
        static const bool s_autostart = [](){ const char *v = std::getenv("PS2X_AUTOSTART"); return v && v[0] && v[0] != '0'; }();
        if (s_autostart)
        {
            static std::atomic<uint32_t> s_n{0};
            if ((s_n.fetch_add(1) % 180u) < 12u)
            {
                b0 = static_cast<uint8_t>(b0 & ~0x08u); // START
                b1 = static_cast<uint8_t>(b1 & ~0x40u); // CROSS
            }
        }
        p[0] = b0; // buttons low
        p[1] = b1; // buttons high
        p[2] = rx; // analog: right stick X
        p[3] = ry; // right stick Y
        p[4] = lx; // left stick X
        p[5] = ly; // left stick Y
    }

    void bt3PadConnect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00295160
    {
        (void)rdram; (void)runtime;
        setReturnS32(ctx, 0); // >= 0 == connected/success
        ctx->pc = getRegU32(ctx, 31);
    }

    void bt3PadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00296160
    {
        (void)rdram; (void)runtime;
        setReturnU32(ctx, 1u); // 1 == controller ready
        ctx->pc = getRegU32(ctx, 31);
    }

    // FUN_00295e58 scePad2CreateSocket. The IOP pad server would assign a distinct
    // socket index per player; with it stubbed, every call returned the same index
    // so both players read identical input. The caller stores the player id in the
    // descriptor (byte at a0+4, set to 0/1 by the pad-init loop in sub_00122940),
    // so hand that back directly as the socket index. The DBC read accessors then
    // receive socket 0/1 in a0 and route to the matching player profile.
    void bt3PadCreateSocket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00295e58
    {
        (void)runtime;
        uint32_t player = 0u;
        if (uint8_t *desc = getMemPtr(rdram, getRegU32(ctx, 4) + 4u))
        {
            player = (*desc) & 0xFFu;
        }
        setReturnS32(ctx, static_cast<int32_t>(player & 3u));
        ctx->pc = getRegU32(ctx, 31);
    }

    void bt3PadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00296090
    {
        (void)runtime;
        writeNeutralPadPacket(rdram, getRegU32(ctx, 5), getRegU32(ctx, 4)); // a0 = socket, a1 = out buffer
        setReturnU32(ctx, 2u); // >= 0 so the pad state machine advances
        ctx->pc = getRegU32(ctx, 31);
    }

    void bt3PadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00295fb8
    {
        (void)runtime;
        writeNeutralPadPacket(rdram, getRegU32(ctx, 5), getRegU32(ctx, 4)); // a0 = socket, a1 = out buffer
        setReturnU32(ctx, 6u); // packet length
        ctx->pc = getRegU32(ctx, 31);
    }

    // BT3 CD read-completion, done reliably. The game's disc-read state machine
    // spins polling a read-state byte (via FUN_00270dd0 = *(handle+1)) that a CD
    // completion interrupt would advance on hardware. With no IOP, nothing drives
    // it. Instead of pumping from another thread (which races / starves against
    // the spinning main thread), replace the poll: run the game's own tick
    // dispatcher (FUN_0028a3b0) INLINE on this thread first, then return the
    // (now-advanced) state byte. Same thread => no race, no starvation.
    // Shared reentrancy guard for the CD file-server tick (FUN_0028a3b0). Both the
    // func_270dd0 poll (bt3CdReadStatePoll) and the AFS-status poll (bt3AfsStatusPoll)
    // pump this tick inline; the guard prevents nested double-ticking when the pump
    // itself reaches the other hooked poll.
    thread_local bool s_bt3CdTicking = false;
    // RAII, because the flag used to be set and cleared by hand around a loop that runs GUEST
    // code -- and guest code here throws (ThreadExitException). One throw left the flag stuck
    // true on that thread forever, after which every poll skipped the pump, the partition state
    // byte never left 2 ("reading"), and the main thread span in the poll for good: the
    // intermittent ~3fps first screen / infinite loading screen, with 0x26b900 measured 94% hot.
    struct Bt3CdTickGuard
    {
        bool engaged = false;
        Bt3CdTickGuard()
        {
            if (!s_bt3CdTicking)
            {
                s_bt3CdTicking = true;
                engaged = true;
            }
        }
        ~Bt3CdTickGuard() { if (engaged) s_bt3CdTicking = false; }
        Bt3CdTickGuard(const Bt3CdTickGuard &) = delete;
        Bt3CdTickGuard &operator=(const Bt3CdTickGuard &) = delete;
    };
    // If the pump is ever skipped for a long unbroken run of polls we are wedged again; say so
    // once rather than spinning silently.
    inline void bt3NoteCdTickSkipped(bool skipped, const char *site)
    {
        static thread_local uint32_t s_skips = 0u;
        if (!skipped) { s_skips = 0u; return; }
        if (++s_skips == 10000u)
            std::fprintf(stderr, "[bt3cdtick] %s: pump skipped %u polls in a row -- reentrancy "
                                 "guard stuck? state byte cannot advance\n", site, s_skips);
    }
    void bt3CdReadStatePoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00270dd0
    {
        const uint32_t handle = getRegU32(ctx, 4); // a0 = read handle
        Bt3CdTickGuard tickGuard;
        bt3NoteCdTickSkipped(!tickGuard.engaged, "cdReadStatePoll");
        if (tickGuard.engaged && handle != 0u && runtime->hasFunction(0x0028a3b0u))
        {
            R5900Context tctx = *ctx;          // inherit gp/sp
            setReturnU32(&tctx, 0u);            // (harmless)
            tctx.r[31] = _mm_setzero_si128();   // ra = 0 => run until return
            tctx.pc = 0x0028a3b0u;             // tick dispatcher
            uint32_t steps = 0u;
            while (tctx.pc != 0u && steps++ < 2000000u)
            {
                PS2Runtime::RecompiledFunction step = runtime->lookupFunction(tctx.pc);
                if (!step)
                {
                    break;
                }
                step(rdram, &tctx, runtime);
            }
        }
        uint32_t state = 0u;
        if (const uint8_t *p = getMemPtr(rdram, handle + 1u))
        {
            state = *p;
        }
        static const bool s_lp = [](){ const char *v=std::getenv("PS2X_LOADPROBE"); return v&&v[0]&&v[0]!='0'; }();
        if (s_lp)
        {
            static std::atomic<uint32_t> s_n{0};
            uint32_t n = s_n.fetch_add(1);
            if ((n % 300u) == 1u)
                std::cerr << "[cdpoll] FUN_00270dd0 calls=" << n << " handle=0x" << std::hex << handle
                          << " state=0x" << state << std::dec << std::endl;
        }
        setReturnU32(ctx, state);
        ctx->pc = getRegU32(ctx, 31);
    }

    // Second file-load path (FUN_00265298 state machine, spun on by the init
    // loop FUN_00263198 `while (FUN_00265298()==0)`). Its reads complete via the
    // tick FUN_0028a3b0, but this loop never pumps it. Trampoline: pump the tick
    // inline, then run the original FUN_00265298 so it observes the progress.
    // True game-frame counter: FUN_00100ab8 is the per-frame render kick (waits for
    // VIF1/GIF idle, sets the display regs, kicks the frame's VIF1 DMA). Counting it
    // gives an honest frames/sec (reported in the [fps] line) instead of proxies.
    // (definition has external linkage at global scope; see top of file)
    PS2Runtime::RecompiledFunction g_orig100ab8 = nullptr;
    // Resource-ready probe (PS2X_LOADPROBE): the fight-loader FUN_002635c8 spins calling
    // func_252D78(id) = "is resource[id] ready?" for the assets it's waiting on. Hook it,
    // run the original, and when it returns 0 (NOT ready) log the id + the resource's +0x58
    // state -> exactly which fight resource never becomes ready (the stuck load).
    PS2Runtime::RecompiledFunction g_orig252d78 = nullptr;
    // [sndspin] PS2X_SNDREG=1 also counts iterations of the sound thread's dispatch call
    // FUN_00286240 (which tail-calls FUN_00286050(slot=6)). The slot-6 table is empty, so
    // the in-handler flag never sets and the table dump alone cannot tell us whether the
    // thread is even running. If this count stays 0, the thread started but never gets
    // scheduled — a completely different problem from an unregistered handler.
    PS2Runtime::RecompiledFunction g_orig286240 = nullptr;
    void bt3SoundDispatchCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00286240
    {
        static std::atomic<uint64_t> n{0};
        const uint64_t k = n.fetch_add(1) + 1;
        if (k == 1 || (k % 2000ull) == 0ull)
            std::fprintf(stderr, "[sndspin] sound-thread dispatch iterations=%llu\n", (unsigned long long)k);
        if (g_orig286240) g_orig286240(rdram, ctx, runtime);
    }

    // [sndwake] PS2X_SNDWAKE=1. The sound service thread (tid6, entry 0x26d070) does ONE
    // loop pass, calls SleepThread at 0x26d15c, and is never woken again -- WakeupThread is
    // called with target 1/4/5 but NEVER 6. Its waker is FUN_0026e160 (main thread), which
    // gates the wake behind:
    //     if (sub_0026D338(tid6) == tid6) FUN_0026d2d0(tid6);
    // and sub_0026D338(tid) is
    //     status = ReferThreadStatus(tid).status;
    //     if (status == THS_SUSPEND(8) || status == THS_WAITSUSPEND(0xC))
    //         return ResumeThread(tid);      // game expects this to yield tid
    //     return 0;
    // A thread parked in SleepThread reports THS_WAIT(4), so the guard returns 0 and the
    // wake is skipped forever. Log (tid -> ret) at the decision point and count the waker's
    // calls, so we can tell "guard never passes" from "waker never runs".
    PS2Runtime::RecompiledFunction g_orig26d338 = nullptr;
    void bt3SndResumeIfSusp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_0026D338
    {
        const uint32_t tid = getRegU32(ctx, 4); // a0
        if (g_orig26d338) g_orig26d338(rdram, ctx, runtime);
        const uint32_t ret = getRegU32(ctx, 2); // v0
        static std::atomic<uint32_t> n{0};
        const uint32_t k = n.fetch_add(1);
        if (k < 40u || (k % 500u) == 0u)
            std::fprintf(stderr, "[sndwake] resumeIfSusp(tid=%u) -> %u  %s\n",
                         tid, ret, (ret == tid && tid != 0) ? "WAKE" : "skip");
    }
    PS2Runtime::RecompiledFunction g_orig26e160 = nullptr;
    void bt3SndKickProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_0026e160
    {
        static std::atomic<uint32_t> n{0};
        const uint32_t k = n.fetch_add(1);
        if (k < 4u || (k % 500u) == 0u)
            std::fprintf(stderr, "[sndwake] kicker FUN_0026e160 calls=%u\n", k + 1u);
        if (g_orig26e160) g_orig26e160(rdram, ctx, runtime);
    }

    // [sndcnt] PS2X_SNDCNT=1. The sound-ready handshake is a refcount at 0x2C9F14:
    //   FUN_0026d810  ends with cnt++            (enqueue a pending sound operation)
    //   sub_0026D9F0  does  cnt--; if(!cnt) FUN_0026d9a0()   (completion -> set ready)
    //   FUN_0026e628  same decrement idiom, but NOTHING calls it (callback-table only)
    //   sub_0026E290  the service routine; calls both, and gates its completion block on
    //                 `bnel cnt,0` at 0x26e464 -- with cnt!=0 the whole block is skipped.
    // cnt sticks at 1 forever: one enqueue, no matching completion. Count each stage so we
    // can see which half runs.
    std::atomic<uint32_t> g_sndSvc{0}, g_sndEnq{0}, g_sndDec{0};
    PS2Runtime::RecompiledFunction g_orig26e290 = nullptr, g_orig26d810 = nullptr, g_orig26d9f0 = nullptr;
    void bt3SndSvc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_0026E290
    {
        g_sndSvc.fetch_add(1);
        if (g_orig26e290) g_orig26e290(rdram, ctx, runtime);
    }
    void bt3SndEnq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_0026d810
    {
        const uint32_t k = g_sndEnq.fetch_add(1);
        if (k < 8u)
            std::fprintf(stderr, "[sndcnt] ENQUEUE cnt++ #%u (a0=0x%x) ra=0x%x\n",
                         k + 1u, getRegU32(ctx, 4), getRegU32(ctx, 31));
        if (g_orig26d810) g_orig26d810(rdram, ctx, runtime);
    }
    void bt3SndDec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_0026D9F0
    {
        const uint32_t k = g_sndDec.fetch_add(1);
        if (k < 8u)
            std::fprintf(stderr, "[sndcnt] COMPLETE cnt-- #%u ra=0x%x\n", k + 1u, getRegU32(ctx, 31));
        if (g_orig26d9f0) g_orig26d9f0(rdram, ctx, runtime);
    }

    // [sndapi] PS2X_SNDAPI=1. The game issues its 41 DTX URPCs at boot and then NEVER sends
    // another sound command (no chunk/stream traffic, menu included). Is that because the
    // game never asks, or because the engine swallows the ask? These are the game's
    // most-called sound-wrapper entry points (ELF call-graph: functions in 0x264000-0x26c000
    // called from outside it, ranked by distinct callers) -- 0x267ac8/0x267b00 are called
    // from the menu/UI code at 0x217xxx-0x22xxxx. If these fire while the RPC count stays
    // frozen, the request dies INSIDE the sound engine; if they never fire, the trigger is
    // upstream game logic.
    // Slots 0-2 are the game-facing wrapper API; 3-5 walk the URPC SEND chain, so we can see
    // how far a request travels before it dies:
    //   3 = 0x272d90  mid-level sound command  (callers 0x272930/0x272d60/0x272f90)
    //   4 = 0x26ecd0  -> 0x281908 URPC command wrapper
    //   5 = 0x27b998  the URPC sender itself (bottoms out in sceSifCallRpc; 41 calls at boot)
    // Broad net over the game's sound-wrapper API: every entry point in 0x264000-0x26c000
    // that the wider game calls, ranked by distinct callers (ELF call graph), plus the URPC
    // sender at the bottom. The title screen HAS music on PCSX2, so at least one of these
    // must fire there -- whichever does (or doesn't) tells us where the BGM request dies.
    constexpr uint32_t kSndApiAddr[] = {
        0x00267ac8u, // 26 callers (menu/UI)
        0x00267b00u, // 17 callers (menu/UI)
        0x002651c0u, //  8 callers -- the only one seen firing (4x, resource open)
        0x00267ab8u, //  6
        0x00265f40u, //  6
        0x00265f70u, //  5
        0x00265728u, //  5
        0x002654a0u, //  5
        0x00267958u, //  4
        0x00265298u, //  4
        0x00265108u, //  4
        0x0027b998u, // the URPC sender (bottoms out in sceSifCallRpc)
        // 12-19: the 8 DTX command wrappers that call the sender. If a post-init sound
        // request reaches any of these, the break is below them (wrapper -> sender);
        // if none is ever entered after init, the request dies higher up in the engine.
        0x00280730u, 0x00280de8u, 0x00280eb0u, 0x00281908u,
        0x00284bf8u, 0x00284da8u, 0x00284e00u, 0x00284fe0u,
        // 20-23: menu/UI functions that CALL the sfx API 0x267ac8. That API has 132 call sites
        // and was never invoked once during a full menu navigation, yet the call at 0x217374
        // (inside 0x217200) is straight-line with NO guard -- so if the function runs, the
        // sound fires. Therefore these functions must not be running at all. Hook them to
        // confirm, and to find which code actually drives the menu instead.
        0x00217200u, 0x002184a0u, 0x00219710u, 0x0021bb50u,
        // 24-25: the stream class START and STOP methods.
        //   0x28b428 START: pos[+0x3C]=0; state[+1]=1   (3 instructions)
        //   0x28b438 STOP : state[+1]=0; then cancels the in-flight DMA
        // The BGM goes state 1->0 at the title->menu transition and never returns to 1, with
        // or without our pump. If START is never called again, the menu never asks for a
        // stream at all; if it IS called and the stream still does not run, the fault is
        // inside the start path.
        0x0028b428u, 0x0028b438u,
        // 26-27: the stream-manager methods that call START/STOP. 0x281bb0 has no direct jal
        // callers (dispatched by pointer), so log its guest ra to get the next hop up the BGM
        // chain: ? -> 0x281bb0 -> 0x28b428(START). If whatever calls it for the title track
        // never runs at the menu, that caller is where the menu's BGM request dies.
        0x00281bb0u, 0x00281870u,
    };
    constexpr int kSndApiCount = 28;
    std::atomic<uint32_t> g_sndApi[kSndApiCount]{};
    PS2Runtime::RecompiledFunction g_origSndApi[kSndApiCount] = {};
    template <int N>
    void bt3SndApiProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t k = g_sndApi[N].fetch_add(1);
        const bool trace = (k < 6u) || (k % 500u) == 0u;
        if (trace)
            std::fprintf(stderr, "[sndapi] api%d call #%u ENTER a0=0x%x a1=0x%x ra=0x%x\n",
                         N, k + 1u, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 31));
        if (g_origSndApi[N]) g_origSndApi[N](rdram, ctx, runtime);
        // sub_002651C0 opens with `do { h = func_2654D8(id); } while (!h);` -- an unbounded
        // retry on a resource lookup. If an ENTER has no matching LEAVE, we are wedged in
        // that spin and every later sound request is unreachable.
        if (trace)
            std::fprintf(stderr, "[sndapi] api%d call #%u LEAVE v0=0x%x\n", N, k + 1u, getRegU32(ctx, 2));
    }

    // Sink -> the IOP ring buffers observed on its free list, learned by the pump while the
    // list is still armed. Hoisted to namespace scope because the STOP hook needs it too: it
    // maps a stream object to its audio stream id (bufferPtr >> 14) so it can ask the backend
    // whether that stream has actually drained.
    struct SinkRing { std::vector<std::pair<uint32_t, uint32_t>> bufs; size_t next = 0; };
    std::mutex g_sinkRingM;
    std::map<uint32_t, SinkRing> g_sinkRings;
    // The two sinks whose audio stream ids are 0 and 1 -- i.e. the L/R halves of the BGM.
    uint32_t g_pairSink[2] = {0u, 0u};
    uint64_t g_pairReturns[2] = {0u, 0u}; // buffers handed to each side, for balance

    // ===================== IOP-side ring consumer (honest playback progress) =============
    //
    // BT3 moves streamed PCM through two instances of one linked-list buffer class: a SOURCE
    // (the EE decoder's output) and a SINK (the IOP's ring). For both, list 0 is FREE SPACE
    // and list 1 is FILLED DATA, and both lists live at [obj + 0x18 + mode*4]:
    //
    //   vtbl+0x18  take(mode, max, &out)   sub_002842F8 -- pop up to `max` bytes off list
    //                                      `mode`; a full take unlinks the node and recycles
    //                                      it onto [obj+0x14], a partial take trims in place
    //   vtbl+0x1C  untake(mode, &desc)     hand an unused remainder back
    //   vtbl+0x20  append(mode, &desc)     sub_00284498 -- append to list `mode`, MERGING with
    //                                      the tail when it ends where the block starts
    //                                      (only if [obj+5] == 1), else taking a pool node
    //   node layout: +0 next, +8 ptr, +0xC len
    //
    // The stream tick sub_0028AE60 takes data from the source's list 1 and space from the
    // sink's list 0, DMAs source -> sink, then appends the written region to the sink's
    // list 1. On hardware the IOP closes the loop: it plays the sink's list 1 and returns
    // that space to list 0. Since this build issues no URPC after init, that return is the
    // ONLY playback-progress signal the EE ever receives -- it is simultaneously how the game
    // paces its streaming, how it knows how much has been played, and how it decides a sound
    // has drained.
    //
    // So emulate that consumer honestly: every tick, move exactly as many bytes from list 1
    // to list 0 as the host device has really played, using the same list surgery the game
    // performs itself. Nothing is invented -- no synthesised descriptors, no round-robined
    // stale lengths, no node reuse -- so the structures only ever hold states the game could
    // have produced, and the guest's own start/stop lifecycle stays in charge.
    constexpr uint32_t kSinkRecycler = 0x14u; // node pool head
    constexpr uint32_t kSinkList0 = 0x18u;    // free space
    constexpr uint32_t kSinkList1 = 0x1Cu;    // filled data
    constexpr uint32_t kSinkMerge = 0x05u;    // "may merge contiguous descriptors" flag
    constexpr uint32_t kNodeNext = 0x00u, kNodePtr = 0x08u, kNodeLen = 0x0Cu;

    inline uint32_t sndRd32(uint8_t *rdram, uint32_t addr)
    {
        const uint8_t *p = getMemPtr(rdram, addr & 0x1FFFFFFFu);
        return p ? *reinterpret_cast<const uint32_t *>(p) : 0u;
    }
    inline void sndWr32(uint8_t *rdram, uint32_t addr, uint32_t val)
    {
        if (uint8_t *p = getMemPtr(rdram, addr & 0x1FFFFFFFu))
            *reinterpret_cast<uint32_t *>(p) = val;
    }
    inline uint8_t sndRd8(uint8_t *rdram, uint32_t addr)
    {
        const uint8_t *p = getMemPtr(rdram, addr & 0x1FFFFFFFu);
        return p ? *p : 0u;
    }

    // Append {ptr,len} to one of the object's lists exactly as sub_00284498 does. Returns
    // false only when a node is needed and the pool is empty (the game raises its own error
    // callback in that case; we leave the caller to undo and retry).
    bool sndListAppend(uint8_t *rdram, uint32_t obj, uint32_t list, uint32_t ptr, uint32_t len)
    {
        if (!ptr || !len)
            return true;
        uint32_t link = obj + list; // slot the new node gets stored into
        uint32_t tail = 0u;
        for (uint32_t n = sndRd32(rdram, link); n; n = sndRd32(rdram, link))
        {
            tail = n;
            link = n + kNodeNext;
        }
        if (tail && sndRd8(rdram, obj + kSinkMerge) == 1u &&
            sndRd32(rdram, tail + kNodePtr) + sndRd32(rdram, tail + kNodeLen) == ptr)
        {
            sndWr32(rdram, tail + kNodeLen, sndRd32(rdram, tail + kNodeLen) + len);
            return true; // merged -- costs no node, which is why the ring never leaks any
        }
        const uint32_t node = sndRd32(rdram, obj + kSinkRecycler);
        if (!node)
            return false;
        sndWr32(rdram, obj + kSinkRecycler, sndRd32(rdram, node + kNodeNext));
        sndWr32(rdram, node + kNodePtr, ptr);
        sndWr32(rdram, node + kNodeLen, len);
        sndWr32(rdram, node + kNodeNext, 0u);
        sndWr32(rdram, link, node);
        return true;
    }

    // Total bytes sitting in one of the object's lists.
    uint64_t sndListBytes(uint8_t *rdram, uint32_t obj, uint32_t list)
    {
        uint64_t total = 0u;
        uint32_t n = sndRd32(rdram, obj + list);
        for (int guard = 0; n && guard < 256; ++guard)
        {
            total += sndRd32(rdram, n + kNodeLen);
            n = sndRd32(rdram, n + kNodeNext);
        }
        return total;
    }

    struct IopSink
    {
        uint32_t streamId = 0xFFFFFFFFu;
        uint64_t returnedBytes = 0u; // played bytes already handed back as free space
        uint64_t heldBytes = 0u;     // bytes queued but not yet played (diagnostics)
        bool wallClock = false;      // no host audio for this stream: fall back to a timer
        std::chrono::steady_clock::time_point wallBase{};
        uint64_t wallBaseBytes = 0u;
        bool ringFullIdle = false;   // ring full but the device has not started playing
        std::chrono::steady_clock::time_point ringFullSince{};
    };
    std::mutex g_iopSinkM;
    std::map<uint32_t, IopSink> g_iopSinks;

    bool sndIopEnabled()
    {
        static const bool s_on = []() {
            const char *v = std::getenv("PS2X_SNDIOP");
            return !(v && v[0] == '0'); // default ON; PS2X_SNDIOP=0 reverts to the old pump
        }();
        return s_on;
    }
    uint32_t sndDeclaredRate()
    {
        static const uint32_t s_rate = []() -> uint32_t {
            if (const char *v = std::getenv("PS2X_SNDRATE"))
            {
                const long n = std::strtol(v, nullptr, 10);
                if (n > 0) return static_cast<uint32_t>(n);
            }
            return 24000u; // same default SIF.cpp declares to the backend
        }();
        return s_rate;
    }
    bool sndIopLog()
    {
        static const bool s_on = []() {
            const char *v = std::getenv("PS2X_SNDIOPLOG");
            return v && v[0] && v[0] != '0';
        }();
        return s_on;
    }

    // Learn (or re-learn) which host audio stream a sink feeds. SIF.cpp splits streams by
    // `dst >> 14`, and the stream object records the IOP destination of its last DMA at
    // [obj+0x20], so the mapping comes straight from the transfer rather than a guess. The
    // free-list head is the fallback for the very first tick, before any DMA has been queued.
    void sndNoteSinkStream(uint8_t *rdram, PS2Runtime *runtime, uint32_t sink, uint32_t iopAddr)
    {
        if (!sink || !iopAddr || !runtime)
            return;
        // Resolve against the registered ring spans, never `iopAddr >> 14`: the rings are
        // 0x100-staggered, so a DMA into the tail of one ring would otherwise re-map its sink
        // onto the NEXT stream id and scramble that sink's accounting mid-playback.
        const uint32_t id = runtime->audioBackend().streamIdForAddress(iopAddr);
        std::lock_guard<std::mutex> lk(g_iopSinkM);
        IopSink &s = g_iopSinks[sink];
        if (s.streamId == id)
            return;
        // A fresh mapping must start from the CURRENT play count, not from zero -- otherwise
        // the stream's whole history would be credited as free space in one go.
        s.streamId = id;
        s.returnedBytes = 0u;
        s.wallClock = false;
        s.ringFullIdle = false;
        if (runtime)
        {
            const auto prog = runtime->audioBackend().streamProgress(id);
            if (prog.known)
                s.returnedBytes = (prog.consumedSamples + prog.gapSamples) * 2ull;
        }
        if (sndIopLog())
            std::fprintf(stderr, "[sndiop] sink 0x%x -> stream %u (iop 0x%x)\n", sink, id, iopAddr);
    }

    // Hand back exactly the space the host device has finished playing.
    void bt3SndIopConsume(uint8_t *rdram, PS2Runtime *runtime, uint32_t sink)
    {
        if (!sink || !runtime)
            return;
        std::lock_guard<std::mutex> lk(g_iopSinkM);
        auto it = g_iopSinks.find(sink);
        if (it == g_iopSinks.end() || it->second.streamId == 0xFFFFFFFFu)
            return;
        IopSink &s = it->second;
        const uint64_t queued = sndListBytes(rdram, sink, kSinkList1);
        s.heldBytes = queued;

        uint64_t playedBytes = 0u;
        const auto prog = runtime->audioBackend().streamProgress(s.streamId);
        if (prog.known)
        {
            s.wallClock = false;
            if (!prog.started)
            {
                // Still building the device's start cushion: genuinely nothing has played, so
                // no space may be returned. But if the guest has filled its ring it cannot
                // supply any more, and a cushion target above what the ring holds would then
                // deadlock -- no playback, no returns, no more data, forever. Give the normal
                // cushion a generous head start, then start with whatever is there.
                const bool ringFull = queued && sndRd32(rdram, sink + kSinkList0) == 0u;
                const auto now = std::chrono::steady_clock::now();
                if (!ringFull)
                {
                    s.ringFullIdle = false;
                }
                else
                {
                    if (!s.ringFullIdle)
                    {
                        s.ringFullIdle = true;
                        s.ringFullSince = now;
                    }
                    else if (std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - s.ringFullSince).count() >= 500)
                    {
                        runtime->audioBackend().requestStreamStart(s.streamId);
                    }
                }
                return;
            }
            s.ringFullIdle = false;
            // gapSamples: guest PCM that never reached the device. It occupied ring space all
            // the same, so it counts as consumed -- otherwise it is never returned and the ring
            // loses that much capacity permanently.
            playedBytes = (prog.consumedSamples + prog.gapSamples) * 2ull;
        }
        else
        {
            // Nothing is rendering this stream (PS2X_SNDPLAY off, or an id the DMA path never
            // feeds). Advance on a wall clock at the declared rate so the guest's sound engine
            // still runs instead of wedging on a ring that never drains.
            const auto now = std::chrono::steady_clock::now();
            if (!s.wallClock)
            {
                s.wallClock = true;
                s.wallBase = now;
                s.wallBaseBytes = s.returnedBytes;
            }
            const uint64_t ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - s.wallBase).count());
            playedBytes = s.wallBaseBytes + (ms * sndDeclaredRate() * 2ull) / 1000ull;
        }
        if (playedBytes <= s.returnedBytes)
            return;

        uint64_t want = playedBytes - s.returnedBytes;
        while (want)
        {
            const uint32_t head = sndRd32(rdram, sink + kSinkList1);
            if (!head)
                break; // the device is ahead of the guest; the credit stays banked
            const uint32_t ptr = sndRd32(rdram, head + kNodePtr);
            const uint32_t len = sndRd32(rdram, head + kNodeLen);
            if (!ptr || !len)
                break;
            const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(want, len));
            if (take == len)
            {
                sndWr32(rdram, sink + kSinkList1, sndRd32(rdram, head + kNodeNext));
                sndWr32(rdram, head + kNodeNext, sndRd32(rdram, sink + kSinkRecycler));
                sndWr32(rdram, sink + kSinkRecycler, head); // full take recycles the node
            }
            else
            {
                sndWr32(rdram, head + kNodePtr, ptr + take);
                sndWr32(rdram, head + kNodeLen, len - take);
            }
            if (!sndListAppend(rdram, sink, kSinkList0, ptr, take))
            {
                // Only reachable after a partial take, since a full take recycles the very node
                // the append would need. Undo it and try again next tick.
                sndWr32(rdram, head + kNodePtr, ptr);
                sndWr32(rdram, head + kNodeLen, len);
                break;
            }
            s.returnedBytes += take;
            want -= take;
        }

        if (sndIopLog())
        {
            static std::atomic<uint32_t> n{0};
            const uint32_t k = n.fetch_add(1);
            if (k < 8u || (k % 400u) == 0u)
                std::fprintf(stderr,
                             "[sndiop] #%u sink=0x%x stream=%u played=%llu returned=%llu "
                             "queued=%llu free=%llu pend=%zu merge=%u pool=%s%s\n",
                             k + 1u, sink, s.streamId, (unsigned long long)playedBytes,
                             (unsigned long long)s.returnedBytes, (unsigned long long)queued,
                             (unsigned long long)sndListBytes(rdram, sink, kSinkList0),
                             prog.pending, sndRd8(rdram, sink + kSinkMerge),
                             sndRd32(rdram, sink + kSinkRecycler) ? "ok" : "EMPTY",
                             s.wallClock ? " [wallclock]" : "");
        }
    }

    // A stream restart must find the ring exactly as the game left it at creation: the whole
    // buffer free, nothing queued. 0x281bb0 asserts on that (it takes the sink's entire free
    // list and infinite-loops at 0x281cf0 if the length is not the expected prefill), and the
    // IOP resets its ring on start too. So flush anything still filled back to the free list
    // and drop the matching host-side audio, then rebase the play clock.
    void bt3SndIopResetSink(uint8_t *rdram, PS2Runtime *runtime, uint32_t sink)
    {
        if (!sink)
            return;
        if (sndRd32(rdram, sink + kSinkList1) == 0u)
            return; // nothing queued: the ring is already whole, leave it alone

        // NEVER reset a ring whose audio is still playing. A reset only makes sense when the
        // previous sound is genuinely over; doing it on every START breaks one-shot SFX, which
        // restart constantly (measured: 57 stream 8 start/stops in one menu session):
        //   drop the tail  -> the blip is discarded before the device ever plays it
        //   keep the tail  -> the flush still hands the guest a whole 16KB free ring while we
        //                     hold the old audio, so each restart injects another 8192 samples
        //                     and the backlog grew to 113664 samples (~4.7s) and climbing
        // Neither is right, because the ring did not need clearing at all. Leave it alone and
        // let the IOP consumer drain it at the device's rate: the guest refills as space comes
        // back, latency stays bounded, and nothing is thrown away.
        if (runtime)
        {
            std::lock_guard<std::mutex> lk(g_iopSinkM);
            auto it = g_iopSinks.find(sink);
            if (it != g_iopSinks.end() && it->second.streamId != 0xFFFFFFFFu)
            {
                const auto prog = runtime->audioBackend().streamProgress(it->second.streamId);
                if (prog.pending > 0u)
                    return; // still audible: this is a retrigger, not a fresh stream
            }
        }
        std::lock_guard<std::mutex> lk(g_iopSinkM);
        auto it = g_iopSinks.find(sink);
        if (it == g_iopSinks.end())
            return;
        IopSink &s = it->second;
        uint64_t flushed = 0u;
        for (int guard = 0; guard < 256; ++guard)
        {
            const uint32_t head = sndRd32(rdram, sink + kSinkList1);
            if (!head)
                break;
            const uint32_t ptr = sndRd32(rdram, head + kNodePtr);
            const uint32_t len = sndRd32(rdram, head + kNodeLen);
            sndWr32(rdram, sink + kSinkList1, sndRd32(rdram, head + kNodeNext));
            sndWr32(rdram, head + kNodeNext, sndRd32(rdram, sink + kSinkRecycler));
            sndWr32(rdram, sink + kSinkRecycler, head);
            if (!sndListAppend(rdram, sink, kSinkList0, ptr, len))
                break;
            flushed += len;
        }
        // What to do with audio the previous stream queued but the device has not reached yet.
        // Hardware discards it -- the IOP resets its ring on start -- and our backend queue is
        // that ring's shadow, so dropping is the faithful model and keeps the two in step.
        // PS2X_SNDKEEPTAIL=1 instead lets the tail play out and simply refuses to credit it as
        // free space; use that if a legitimate line ever gets clipped at its end.
        static const bool s_keepTail = []() {
            const char *v = std::getenv("PS2X_SNDKEEPTAIL");
            return v && v[0] && v[0] != '0';
        }();
        size_t dropped = 0u, kept = 0u;
        if (runtime && s.streamId != 0xFFFFFFFFu)
        {
            if (!s_keepTail)
                dropped = runtime->audioBackend().dropStreamPending(s.streamId);
            const auto prog = runtime->audioBackend().streamProgress(s.streamId);
            kept = prog.pending;
            // Rebase the play clock. `pending` is audio already counted into the ring we just
            // flushed, so it must not be credited a second time as it drains.
            s.returnedBytes = prog.known
                                  ? (prog.consumedSamples + prog.gapSamples + prog.pending) * 2ull
                                  : 0u;
        }
        s.wallClock = false;
        s.ringFullIdle = false;
        if (flushed || dropped || kept)
            std::fprintf(stderr,
                         "[sndiop] reset sink=0x%x stream=%u flushed=%llu bytes | tail dropped=%zu kept=%zu samples\n",
                         sink, s.streamId, (unsigned long long)flushed, dropped, kept);
    }

    // ================== SYSTEM-SE PLAYBACK (menu blips, hit sounds) =====================
    //
    // The EE side is complete and verified: a keypress queues a command, the per-frame flush
    // copies it to 0x300EC0 and sends it with sceSifCallRpc rpcNum 0xD. Only the IOP end is
    // missing, so implement it here.
    //
    // Command payload (0x184 bytes at the send buffer):
    //     +0x00 u32 count, then `count` entries of 12 bytes:
    //     +0 u8 zero | +1 u8 t1 | +2 u16 seq | +4 u8 ID | +5 u8 a1 | +6 u8 VOL | +7 u8 PAN
    //     +8 u32 param
    //
    // Sample banks arrive by SIF DMA and our SIF layer copies them into guest RAM, so they can
    // simply be read back: chunked Sony sound-data format, big-endian FourCCs stored as LE
    // words -- `SCEI`+`Vers`/`Head`/`Vagi`/`Setb`. The Vagi chunk is
    //     payload: u32 count, u32 offsets[count] (relative to the payload),
    //              then per-sample 8-byte records { u16 sampleRate, u16 flags, u32 dataOffset }
    // dataOffset indexes the raw ADPCM blob uploaded to 0x1A00000. Observed rate 0x3e80 = 16000.
    // Both sample blobs are DMA'd to the SAME address (0x1A00000): that is a STAGING buffer the
    // IOP relocates into SPU2 RAM, so on hardware the banks coexist at different SPU2 addresses.
    // Reading samples back from the staging address only ever sees the LAST upload -- bank A's
    // 35 KB is overwritten by bank B's 656 KB -- which is why a correct index still produced the
    // wrong sound. Snapshot each blob as it arrives instead, in upload order.
    std::mutex g_seBlobM;
    std::vector<std::vector<uint8_t>> g_seBlobs;

    uint32_t seBlobCount()
    {
        std::lock_guard<std::mutex> lk(g_seBlobM);
        return static_cast<uint32_t>(g_seBlobs.size());
    }
    // Read a byte out of snapshot `idx`; returns false past the end.
    bool seBlobByte(uint32_t idx, uint32_t off, uint8_t &out)
    {
        std::lock_guard<std::mutex> lk(g_seBlobM);
        if (idx >= g_seBlobs.size() || off >= g_seBlobs[idx].size())
            return false;
        out = g_seBlobs[idx][off];
        return true;
    }

    constexpr uint32_t kSeBankData = 0x01a00000u;
    constexpr uint32_t kSeStreamId = 0xF0u; // reserved backend stream for one-shot SE

    // Bank header addresses, in upload order, so slot N pairs with blob N.
    //
    // The command's "bank" field is a BITMASK, not an index: observed values are 1, 2, 4, 8, 16
    // and 32, i.e. slot = ctz(bank). A fight loads SIX banks, not the two present at boot:
    //     slot 0  bank 1   AFS[330]   8 samples   menu/system
    //     slot 1  bank 2   AFS[331]  79           system SE
    //     slot 2  bank 4   AFS[329]  84           common fight SFX (punches, explosions)
    //     slot 3  bank 8   a 10-sample bank
    //     slot 4  bank 16  AFS[3194] 56           per-character
    //     slot 5  bank 32  AFS[3207] 56           per-character
    // Hardcoding two banks is why no hit ever sounded: every fight command was declined as
    // "bank out of range". Headers are uploaded ahead of their sample blob, each bank as a
    // (Vagi header, sequence) pair, so counting only Vagi-bearing uploads keeps slot == blob.
    // Headers are SNAPSHOTTED, not read back from the IOP address they were sent to. Reading
    // them back works for the two banks loaded at boot but not for the four a fight adds, which
    // parse as having no Vagi chunk even though the bytes we saw on the way past plainly had
    // one. Keeping our own copy sidesteps the question entirely, the same way blob snapshots
    // already sidestep the reuse of the sample staging address.
    struct SeBank
    {
        uint32_t addr = 0u;
        std::vector<uint8_t> hdr;
    };
    std::vector<SeBank> g_seBankHdrs;
    constexpr size_t kSeMaxBanks = 32u;

    // Little-endian scalar reads out of a snapshot.
    uint32_t seRd32(const std::vector<uint8_t> &b, uint32_t off)
    {
        if (off + 4u > b.size())
            return 0u;
        uint32_t v;
        std::memcpy(&v, b.data() + off, 4);
        return v;
    }
    uint32_t seRd16(const std::vector<uint8_t> &b, uint32_t off)
    {
        if (off + 2u > b.size())
            return 0u;
        uint16_t v;
        std::memcpy(&v, b.data() + off, 2);
        return v;
    }

}  // namespace

// Called from SIF.cpp for every DMA into the SE sample staging area. Each upload is snapshotted
// in order, because the staging address is REUSED: without this, bank B's 656 KB overwrites
// bank A's 35 KB and every bank-A sample decodes from the wrong bytes.
void bt3NoteSeBankBlob(const uint8_t *data, uint32_t size)
{
    if (!data || !size)
        return;
    std::lock_guard<std::mutex> lk(g_seBlobM);
    if (g_seBlobs.size() >= kSeMaxBanks)
        return;
    g_seBlobs.emplace_back(data, data + size);
    std::fprintf(stderr, "[se] bank blob %zu captured (%u bytes)\n", g_seBlobs.size() - 1u, size);
}

// Called from SIF.cpp for every DMA into the IOP sound region. A bank's header is uploaded
// before its sample blob, so recording the Vagi-bearing ones in order keeps header slot N
// paired with blob N. Sequence (Sequ/Sesq) uploads share the SCEI container but carry no Vagi
// chunk, and must not be counted or every slot after the first would be off by one.
void bt3NoteSeBankHeader(uint32_t dst, const uint8_t *data, uint32_t size)
{
    if (!data || size < 24u)
        return;
    bool hasVagi = false;
    for (uint32_t o = 0, guard = 0; o + 12u <= size && guard < 16u; ++guard)
    {
        uint32_t magic, tag, len;
        std::memcpy(&magic, data + o, 4);
        std::memcpy(&tag, data + o + 4, 4);
        std::memcpy(&len, data + o + 8, 4);
        if (magic != 0x53434549u || !len) // 'SCEI'
            break;
        if (tag == 0x56616769u) // 'Vagi'
        {
            hasVagi = true;
            break;
        }
        o += len;
    }
    if (!hasVagi)
        return;
    std::lock_guard<std::mutex> lk(g_seBlobM);
    if (g_seBankHdrs.size() >= kSeMaxBanks)
        return;
    g_seBankHdrs.push_back(SeBank{dst, std::vector<uint8_t>(data, data + size)});
    std::fprintf(stderr, "[se] bank header slot %zu at 0x%x (%u bytes)\n",
                 g_seBankHdrs.size() - 1u, dst, size);
}

std::atomic<int> g_rayHookArm{0};   // [rayhook] external linkage: set by the [raysrc] probe in ps2_memory.cpp
namespace
{
    // Locate the Vagi chunk in a SNAPSHOTTED bank header; offsets are snapshot-relative.
    bool seFindVagiSnap(const std::vector<uint8_t> &h, uint32_t &payloadOut, uint32_t &countOut)
    {
        uint32_t o = 0u;
        for (int guard = 0; guard < 16; ++guard)
        {
            if (seRd32(h, o) != 0x53434549u) // 'SCEI'
                return false;
            const uint32_t tag = seRd32(h, o + 4u);
            const uint32_t size = seRd32(h, o + 8u);
            if (!size)
                return false;
            if (tag == 0x56616769u) // 'Vagi'
            {
                payloadOut = o + 12u;
                countOut = seRd32(h, payloadOut);
                return true;
            }
            o += size;
        }
        return false;
    }

    // Locate the Vagi chunk in a bank header and return {payloadAddr, count}.
    bool seFindVagi(uint8_t *rdram, uint32_t hdr, uint32_t &payloadOut, uint32_t &countOut)
    {
        uint32_t o = hdr;
        for (int guard = 0; guard < 16; ++guard)
        {
            const uint32_t magic = sndRd32(rdram, o);
            if (magic != 0x53434549u) // 'SCEI'
                return false;
            const uint32_t tag = sndRd32(rdram, o + 4u);
            const uint32_t size = sndRd32(rdram, o + 8u);
            if (!size)
                return false;
            if (tag == 0x56616769u) // 'Vagi'
            {
                payloadOut = o + 12u;
                countOut = sndRd32(rdram, payloadOut);
                return true;
            }
            o += size;
        }
        return false;
    }

    // Sony 4-bit ADPCM, 16-byte blocks: [shift|filter][flags][14 data bytes].
    // Headerless -- the bank stores raw blocks, unlike a .VAG file which our ps2_vag::decode
    // expects to start with a 'VAGp' magic.
    void seDecodeAdpcm(uint32_t blob, uint32_t addr, std::vector<int16_t> &out, uint32_t maxBlocks)
    {
        static const int kF0[5] = {0, 60, 115, 98, 122};
        static const int kF1[5] = {0, 0, -52, -55, -60};
        int32_t s1 = 0, s2 = 0;
        uint8_t blk[16];
        for (uint32_t b = 0; b < maxBlocks; ++b)
        {
            bool ok = true;
            for (int j = 0; j < 16 && ok; ++j)
                ok = seBlobByte(blob, addr + b * 16u + static_cast<uint32_t>(j), blk[j]);
            if (!ok)
                return;
            uint32_t shift = blk[0] & 0x0Fu;
            uint32_t filter = (blk[0] >> 4) & 0x07u;
            if (shift > 12u) shift = 9u;
            if (filter > 4u) filter = 0u;
            const uint8_t flags = blk[1];
            if (flags == 7u) // end marker
                return;
            for (int i = 0; i < 28; ++i)
            {
                const uint8_t byte = blk[2 + (i >> 1)];
                int32_t nib = (i & 1) ? (byte >> 4) : (byte & 0x0F);
                if (nib > 7) nib -= 16;
                int32_t s = (nib << 12) >> shift;
                s += (s1 * kF0[filter] + s2 * kF1[filter]) >> 6;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                out.push_back(static_cast<int16_t>(s));
                s2 = s1;
                s1 = s;
            }
            if (flags & 1u) // loop/end of this sample
                return;
        }
    }

    // Locate one of the two UNLISTED samples parked at the front of a bank's ADPCM body.
    //
    // The Vagi table does not describe the whole body. Every bank stores two samples ahead of
    // the first Vagi-referenced one -- in bank A the Vagi records start at 0x2870 and leave the
    // preceding 10352 bytes unaccounted for; bank B leaves 9968 bytes the same way. Those two
    // samples are the game's ids 0 and 1, which is the whole reason ids are two ahead of Vagi
    // indices. Walking the block flags is the only way to find them: nothing points at them.
    //
    // Layout is plain Sony ADPCM framing -- blocks run until one sets flag bit 0 (end), then a
    // single flags==7 block terminates, then the next sample begins.
    bool seHeadSampleOffset(uint32_t blob, uint32_t want, uint32_t &offOut)
    {
        uint32_t sample = 0u, start = 0u;
        for (uint32_t b = 0; b < 8192u; ++b)
        {
            uint8_t flags = 0u;
            if (!seBlobByte(blob, b * 16u + 1u, flags))
                return false;
            if (flags == 7u) // terminator block; the next block opens the following sample
            {
                start = (b + 1u) * 16u;
                continue;
            }
            if (flags & 1u) // end of the current sample
            {
                if (sample == want)
                {
                    offOut = start;
                    return true;
                }
                ++sample;
                start = (b + 1u) * 16u;
            }
        }
        return false;
    }

    // PS2X_SELOG=1 -- log every sound effect the game asks for, uncapped, including the ones
    // we decline. A silently dropped command looks exactly like a command that was never sent,
    // which is what hid the menu cursor for so long, so misses are logged as loudly as hits.
    bool seLogEnabled()
    {
        static const bool on = []() {
            const char *v = std::getenv("PS2X_SELOG");
            return v && v[0] && v[0] != '0';
        }();
        return on;
    }

    // Names for the effects identified by ear, so the log reads as sounds rather than numbers.
    const char *seName(uint32_t bank, uint32_t idx)
    {
        if (bank == 1u)
        {
            if (idx == 0u) return " cursor";
            if (idx == 1u) return " confirm";
            if (idx == 4u) return " popup-open";
            if (idx == 5u) return " popup-close";
        }
        return "";
    }

    void seDrop(uint32_t bank, uint32_t idx, const char *why)
    {
        if (seLogEnabled())
            std::fprintf(stderr, "[se] bank%u id%u%s DROPPED -- %s\n",
                         bank, idx, seName(bank, idx), why);
    }

    // Play one SE command entry.
    // ===================== SE VOICES =====================
    //
    // Effects are held as ACTIVE VOICES and mixed incrementally, instead of decoding straight
    // into the backend and forgetting them. That is required for correctness, not tidiness: the
    // command queue has a THIRD entry type (producer 0x124248) that carries no bank and no index,
    // only the u16 at +2 -- which is the serial the type-0 producer wrote there and RETURNED to
    // its caller (`lhu $v0, 0x2($a0)` at 0x1241e0). So the game takes a handle when it starts a
    // sound and later stops it by that handle.
    //
    // Fire-and-forget playback has nothing to stop, so every effect ran to its full length. Short
    // ones finish before the stop arrives and sound correct (dash, hits); long ones do not -- the
    // teleport is a 2.54s sample the game cuts to ~1.36s, which is why it alone sounded wrong
    // while nothing in the bank distinguished it (mapping, decode, rate and head-sample count are
    // all verified identical to its neighbours).
    constexpr uint32_t kSeMixRate = 22050u;   // one rate for the shared stream
    constexpr size_t kSeChunk = 512;          // samples generated per top-up step
    constexpr size_t kSeTargetPending = 3072; // keep ~140ms queued ahead of the device

    struct SeVoice
    {
        uint32_t serial = 0xFFFFFFFFu; // 0xFFFFFFFF = untracked (cannot be stopped)
        std::vector<int16_t> pcm;
        size_t pos = 0;
    };
    std::mutex g_seVoiceM;
    std::vector<SeVoice> g_seVoices;

    void seAddVoice(uint32_t serial, std::vector<int16_t> &&pcm)
    {
        if (pcm.empty())
            return;
        std::lock_guard<std::mutex> lk(g_seVoiceM);
        if (g_seVoices.size() >= 32u) // SPU2 has 24 voices; a cap keeps a runaway bounded
            g_seVoices.erase(g_seVoices.begin());
        SeVoice v;
        v.serial = serial;
        v.pcm = std::move(pcm);
        g_seVoices.push_back(std::move(v));
    }

    void seStopVoice(uint32_t serial)
    {
        std::lock_guard<std::mutex> lk(g_seVoiceM);
        for (size_t i = 0; i < g_seVoices.size(); ++i)
        {
            if (g_seVoices[i].serial == serial)
            {
                if (seLogEnabled())
                    std::fprintf(stderr, "[se] STOP serial=%u (%zu/%zu samples played)\n",
                                 serial, g_seVoices[i].pos, g_seVoices[i].pcm.size());
                g_seVoices.erase(g_seVoices.begin() + static_cast<long>(i));
                return;
            }
        }
        if (seLogEnabled())
            std::fprintf(stderr, "[se] STOP serial=%u -- already finished\n", serial);
    }

    // Per-frame: top the backend up from the active voices. Generating incrementally is what
    // makes a stop possible -- the tail of a stopped voice is simply never produced.
    void seServiceVoices(PS2Runtime *runtime)
    {
        if (!runtime)
            return;
        for (int guard = 0; guard < 64; ++guard)
        {
            {
                std::lock_guard<std::mutex> lk(g_seVoiceM);
                if (g_seVoices.empty())
                    return;
            }
            const auto prog = runtime->audioBackend().streamProgress(kSeStreamId);
            if (prog.pending >= kSeTargetPending)
                return;
            int32_t acc[kSeChunk];
            std::memset(acc, 0, sizeof(acc));
            size_t used = 0;
            {
                std::lock_guard<std::mutex> lk(g_seVoiceM);
                for (auto it = g_seVoices.begin(); it != g_seVoices.end();)
                {
                    const size_t avail = it->pcm.size() - it->pos;
                    const size_t n = avail < kSeChunk ? avail : kSeChunk;
                    for (size_t i = 0; i < n; ++i)
                        acc[i] += it->pcm[it->pos + i];
                    it->pos += n;
                    if (n > used) used = n;
                    if (it->pos >= it->pcm.size())
                        it = g_seVoices.erase(it);
                    else
                        ++it;
                }
            }
            if (!used)
                return;
            int16_t out[kSeChunk];
            for (size_t i = 0; i < used; ++i)
            {
                int32_t v = acc[i];
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                out[i] = static_cast<int16_t>(v);
            }
            runtime->audioBackend().onStreamPcm(kSeStreamId, out,
                                                static_cast<uint32_t>(used), kSeMixRate);
        }
    }

    void sePlay(uint8_t *rdram, PS2Runtime *runtime, uint32_t bank, uint32_t idx,
                uint32_t vol, uint32_t pan, uint32_t serial)
    {
        if (!runtime)
            return;
        // Header <-> snapshot pairing follows UPLOAD ORDER: bank A's header (8 samples) arrives
        // with the first blob, bank B's (79) with the second. Bank A is the small system set --
        // menu cursor/confirm/cancel -- so try it first.
        // Command entry +4 selects the BANK (1 = bank A / 8 samples, 2 = bank B / 79) and +5 is
        // the sample index within it. Reading +4 as the sound id is why every menu action played
        // the same sample: +4 barely varies, +5 is the real selector.
        const uint32_t nblobs = seBlobCount();
        // `bank` is a bitmask -- one bit per loaded bank slot.
        if (bank == 0u || (bank & (bank - 1u)) != 0u)
        {
            seDrop(bank, idx, "bank is not a single slot bit");
            return;
        }
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(bank));
        {
            std::vector<uint8_t> hdrSnap;
            uint32_t hdrAddr = 0u;
            {
                std::lock_guard<std::mutex> lk(g_seBlobM);
                if (slot >= g_seBankHdrs.size())
                {
                    seDrop(bank, idx, "no header captured for this slot");
                    return;
                }
                hdrSnap = g_seBankHdrs[slot].hdr;
                hdrAddr = g_seBankHdrs[slot].addr;
            }
            const struct { uint32_t hdr; uint32_t blob; } bk{hdrAddr, slot};
            uint32_t pay = 0u, cnt = 0u;
            if (bk.blob >= nblobs)
            {
                seDrop(bank, idx, "bank data not captured yet");
                return;
            }
            // A bank's body holds TWO MORE samples than its Vagi table describes, sitting ahead
            // of every Vagi-referenced one, and the game numbers all of them from zero. So:
            //
            //     id 0, 1   -> the two unlisted head samples (seHeadSampleOffset)
            //     id 2+     -> Vagi entry (id - 2)
            //
            // That is where the -2 comes from -- not an off-by-one, just two samples the table
            // never mentions. Confirmed by ear on disc-decoded audio: in bank A id 0 is the menu
            // cursor and id 1 the confirm, while ids 4 and 5 are the popup open/close, which are
            // Vagi entries 2 and 3. Bank B is built identically, so the same rule serves its
            // ids (e.g. the observed 55/56).
            //
            // Do NOT "simplify" this to an identity map. Sesq, Setb and Vagi are each internally
            // an identity map, and reasoning from that alone once led to exactly that mistake --
            // the shift lives in the body layout, not in any of those tables.
            uint32_t rate = 16000u;
            uint32_t dataOff = 0u;
            const bool head = (idx < 2u);
            if (head)
            {
                if (!seHeadSampleOffset(bk.blob, idx, dataOff))
                {
                    seDrop(bank, idx, "head sample not found walking block flags");
                    return;
                }
                // No Vagi record means no stored rate; the listed samples of both banks lead
                // with 16 kHz, so follow entry 0 rather than hardcoding.
                if (seFindVagiSnap(hdrSnap, pay, cnt) && cnt)
                {
                    const uint32_t r0 = seRd32(hdrSnap, pay + 4u);
                    const uint32_t v = seRd16(hdrSnap, pay + r0);
                    if (v)
                        rate = v;
                }
            }
            else
            {
                const uint32_t sampleIdx = idx - 2u;
                if (!seFindVagiSnap(hdrSnap, pay, cnt) || sampleIdx >= cnt)
                {
                    seDrop(bank, idx, cnt ? "Vagi index past end of table" : "no Vagi chunk");
                    return;
                }
                const uint32_t recOff = seRd32(hdrSnap, pay + 4u + sampleIdx * 4u);
                const uint32_t rec = pay + recOff;
                rate = seRd16(hdrSnap, rec);
                dataOff = seRd32(hdrSnap, rec + 4u);
                // A sample's playback rate lives in the NEXT Vagi record, not its own.
                //
                // Found via the teleport (bank 4 id32 = vag30): stored at 11000, correct at
                // ~24000 by ear, and the only 24000 in that 84-sample bank is record 31 -- the
                // one immediately after it. The record itself looks self-consistent (8-byte
                // stride, rate and dataOffset together), so this was tested as a falsifiable
                // hypothesis rather than assumed: the shift changes 41 of 84 samples in the
                // fight bank, 29 of 78 in bank 2 and 2 of 8 in the menu bank, so if it were
                // wrong a pile of well-known effects would break at once. User-verified: with
                // it on, menu and fight audio are correct throughout.
                // PS2X_SERATESHIFT=0 reverts to using each record's own rate.
                {
                    static const bool s_shift = []() {
                        const char *v = std::getenv("PS2X_SERATESHIFT");
                        return !(v && v[0] == '0');
                    }();
                    if (s_shift && sampleIdx + 1u < cnt)
                    {
                        const uint32_t nextOff = seRd32(hdrSnap, pay + 4u + (sampleIdx + 1u) * 4u);
                        const uint32_t nr = seRd16(hdrSnap, pay + nextOff);
                        if (nr)
                            rate = nr;
                    }
                }
            }
            std::vector<int16_t> pcm;
            pcm.reserve(4096);
            seDecodeAdpcm(bk.blob, dataOff, pcm, 1024u);
            if (pcm.empty())
            {
                seDrop(bank, idx, "decoded to zero samples (empty slot?)");
                return;
            }
            const float g = static_cast<float>(vol) / 127.0f;
            if (g < 0.99f)
                for (auto &sm : pcm)
                    sm = static_cast<int16_t>(static_cast<float>(sm) * g);
            // A stored rate of 11000 does not mean 11000 Hz. It is the ONLY non-standard value
            // anywhere in the SE banks -- across the six banks a fight loads, the 292 samples
            // carry 11000 (x65), 11025 (x31), 16000 (x194) and 24000 (x2), and 11025/16000/24000
            // are all real PS2 rates while 11000 is not. Samples carrying it play an octave low
            // at face value: the teleport effect (bank 4 id32) is 2.51s against a 1.36s console
            // reference, i.e. 1.84x too slow, and is correct at 22050 by ear. An 11025 sample in
            // the SAME bank (id72) is correct as stored, so this is not a per-bank factor -- and
            // nothing in the bank data encodes a per-sample one (the Vagi +2 field is a constant
            // 0xff00, tone templates are byte-identical across all five banks, and the sequences
            // are all the same single NoteOn). Map it to the standard rate nearest 2x.
            // PS2X_SERATE11K=0 disables, to A/B if this ever looks wrong.
            if (rate == 11000u)
            {
                // DEFAULT OFF: this was WRONG. It fixes the teleport (bank 4 id32) but makes
                // dash and hit effects play too fast -- user-verified. So 11000 being the only
                // non-standard stored rate is NOT the discriminator, and id32's length is not a
                // rate problem: its decode is correct (997 blocks, proper end flag, genuinely
                // 2.54s at 11000). The likely explanation is that the ENGINE stops the voice
                // early -- samples shorter than some gate play whole (dash/hits, correct as
                // stored) while longer ones are cut (id32: 2.54s stored vs ~1.36s on console).
                // Doubling the rate only coincidentally matched that cut length.
                // PS2X_SERATE11K=1 re-enables for experiments.
                static const bool s_on = []() {
                    const char *v = std::getenv("PS2X_SERATE11K");
                    return v && v[0] == '1';
                }();
                if (s_on)
                    rate = 22050u;
            }
            // All effects share ONE backend stream, so they must share ONE sample rate: the
            // stream carries a single rate and the last writer would otherwise set it for
            // everything already queued. Menu effects are nearly all 16 kHz so that went
            // unnoticed, but fight banks mix 11000/11025/16000 and the rate flips per effect,
            // replaying queued audio at the wrong speed -- audibly wrong pitch. Resample each
            // effect to a fixed rate first. 22050 is exactly 2x the common 11025 and upsamples
            // 16000 without loss of the original band.
            const uint32_t srcRate = rate ? rate : 16000u;
            if (srcRate != kSeMixRate && !pcm.empty())
            {
                const size_t outN = static_cast<size_t>(
                    (static_cast<uint64_t>(pcm.size()) * kSeMixRate) / srcRate);
                std::vector<int16_t> rs;
                rs.reserve(outN);
                for (size_t i = 0; i < outN; ++i)
                {
                    // Linear interpolation; plenty for short one-shot effects.
                    const double srcPos = (static_cast<double>(i) * srcRate) / kSeMixRate;
                    const size_t i0 = static_cast<size_t>(srcPos);
                    const size_t i1 = (i0 + 1 < pcm.size()) ? i0 + 1 : i0;
                    const double frac = srcPos - static_cast<double>(i0);
                    rs.push_back(static_cast<int16_t>(pcm[i0] + (pcm[i1] - pcm[i0]) * frac));
                }
                pcm.swap(rs);
            }
            // Hand it to a voice; seServiceVoices() mixes the active voices incrementally so
            // a later stop-by-serial can cut the tail. Overlap still works -- voices sum.
            seAddVoice(serial, std::move(pcm));
            seServiceVoices(runtime);
            static std::atomic<uint32_t> n{0};
            const uint32_t k = n.fetch_add(1);
            if (seLogEnabled() || k < 12u)
            {
                const uint32_t r = kSeMixRate; // post-resample: pcm.size() is in THIS rate
                std::fprintf(stderr, "[se] #%-4u ser=%-5u slot%u(bank%-2u) id%-3u%-12s %-4s %5zu smp "
                                     "%4ums @%5uHz vol=%-3u pan=%-3u dataOff=0x%x\n",
                             k, serial, slot, bank, idx, seName(bank, idx), head ? "head" : "vagi",
                             pcm.size(), static_cast<uint32_t>(pcm.size() * 1000u / r), r,
                             vol, pan, dataOff);
            }
            return;
        }
        static std::atomic<uint32_t> miss{0};
        if (miss.fetch_add(1) < 8u)
            std::fprintf(stderr, "[se] bank%u sample%u NOT FOUND (blobs=%u)\n", bank, idx, nblobs);
    }

    // Hook on sceSifCallRpc: service the SE command the IOP would have handled.
    PS2Runtime::RecompiledFunction g_orig2b48f0 = nullptr;
    void bt3SeRpcSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x2b48f0
    {
        const uint32_t rpcNum = getRegU32(ctx, 5);
        const uint32_t ra = getRegU32(ctx, 31);
        const uint32_t sendBuf = getRegU32(ctx, 7);
        // Only the SE service's payload send (from inside 0x123F48); rpcNum 9 is a per-frame
        // prepare and 0/3/7 are bind/init.
        // Default ON, PS2X_SEPLAY=0 opts out -- must match the registration gate, or the hook is
        // installed and then declines every command.
        static const bool s_on = []() {
            const char *v = std::getenv("PS2X_SEPLAY");
            return !(v && v[0] == '0');
        }();
        if (s_on && ra == 0x00123f9cu && rpcNum == 0x0Du && sendBuf)
        {
            const uint32_t count = sndRd32(rdram, sendBuf);
            for (uint32_t i = 0; i < count && i < 32u; ++i)
            {
                const uint32_t e = sendBuf + 4u + i * 12u;
                const uint32_t type = sndRd8(rdram, e + 0u);
                const uint32_t bank = sndRd8(rdram, e + 4u);
                const uint32_t idx = sndRd8(rdram, e + 5u);
                const uint32_t vol = sndRd8(rdram, e + 6u);
                const uint32_t pan = sndRd8(rdram, e + 7u);
                // +2 is the u16 handle: the type-0 producer (0x124150) writes an
                // auto-incrementing serial there and returns it to its caller; the type-2
                // producer (0x124248) writes ONLY that field, to name the voice to stop.
                const uint32_t serial = static_cast<uint32_t>(sndRd8(rdram, e + 2u)) |
                                        (static_cast<uint32_t>(sndRd8(rdram, e + 3u)) << 8);
                // Log EVERY command, including ones sePlay declines -- a silently dropped
                // command is indistinguishable from a missing request, and that hid what the
                // menu cursor actually sends.
                static std::atomic<uint32_t> cn{0};
                const uint32_t ci = cn.fetch_add(1);
                if (seLogEnabled() || ci < 40u)
                {
                    const uint32_t b0 = sndRd8(rdram, e + 0u), b1 = sndRd8(rdram, e + 1u);
                    const uint32_t p8 = sndRd32(rdram, e + 8u);
                    std::fprintf(stderr, "[secmd] #%u type=%u serial=%u bank=%u idx=%u "
                                         "vol=%u pan=%u | +1=%u +8=0x%x\n",
                                 ci, type, serial, bank, idx, vol, pan, b1, p8);
                }
                // Type 2 = stop the voice with this handle. Types 0 and 1 start one (0 from
                // 0x124150 with vol/pan/pitch, 1 from 0x1241F0 with just bank+index).
                if (type == 2u)
                    seStopVoice(serial);
                else
                    sePlay(rdram, runtime, bank, idx, vol, pan, serial);
            }
        }
        if (g_orig2b48f0) g_orig2b48f0(rdram, ctx, runtime);
    }


    // [sndse] PS2X_SNDSE=1. Where do punch/explosion SFX actually go?
    //
    // Measured: they are NOT streamed PCM. Rings 4 and 10 each receive ONE ~250ms burst at
    // fight load and nothing per hit, so the "SE are EE-rendered like the BGM" theory is dead.
    // The open question is which path a hit sound takes instead, and the way to answer it is to
    // watch the sound engine's own lifecycle while someone punches:
    //   0x272930  create streamed-sound player (16 slots x 200 bytes at 0x2c9288)
    //   0x273030  player START   (sets state [player+1] = 1)
    //   0x2733a0  player STOP
    //   0x28b310  allocate a stream object (a0 = source, a1 = sink)
    //   0x2654a0  load/play-by-id -- the entry the overlay menu code uses (a0 = resource id)
    //   0x265728  the 6-slot wait/drain loop
    // If a hit produces player creates/starts, SE go through the streaming engine and the fault
    // is downstream of it. If it produces nothing, the request leaves the EE some other way and
    // the next place to look is the SIF DMA / RPC traffic that accompanies it.
    constexpr uint32_t kSndSeAddr[] = {0x00272930u, 0x00273030u, 0x002733a0u,
                                       0x0028b310u, 0x002654a0u, 0x00265728u};
    constexpr const char *kSndSeName[] = {"playerCreate", "playerSTART", "playerSTOP",
                                          "streamAlloc", "loadById", "waitSlots"};
    constexpr int kSndSeCount = 6;
    std::atomic<uint32_t> g_sndSe[kSndSeCount]{};
    PS2Runtime::RecompiledFunction g_origSndSe[kSndSeCount] = {};

    template <int N>
    void bt3SndSeProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t k = g_sndSe[N].fetch_add(1);
        // Every call for the first few, then sparse: a hit sound is a RATE question, so the
        // early ones are what matter and a flood would hide them.
        if (k < 24u || (k % 50u) == 0u)
            std::fprintf(stderr, "[sndse] %s #%u a0=0x%x a1=0x%x a2=0x%x ra=0x%x\n",
                         kSndSeName[N], k + 1u, getRegU32(ctx, 4), getRegU32(ctx, 5),
                         getRegU32(ctx, 6), getRegU32(ctx, 31));
        if (g_origSndSe[N]) g_origSndSe[N](rdram, ctx, runtime);
    }

    // 0x281bb0 START-WHEN-READY, run every frame for each stream group whose start flag
    // [group+0x58] is 1. Before it calls 0x28b428 it ASSERTS on the ring being whole: for each
    // channel it takes the sink's entire free list and infinite-loops at 0x281cf0 unless the
    // length equals the group's prefill [group+0x2C]. That check runs BEFORE the start method,
    // so the ring has to be reset here rather than in the 0x28b428 hook -- otherwise a restart
    // that finds anything still queued hangs the sound thread in that loop.
    // Group layout: [+0x52] channel count, [+0x10 + i*4] the stream objects (sink at [obj+8]).
    // [wisphook] PS2X_WISPHOOK=1 (2026-09-03 lingering aura): FUN_00131a20 is the generic
    // billboard-quad emitter (a0 = position vector, a1 = colour floats R,G,B,A, a2 = second
    // colour floats, t0 = destination packet buffer). Its byte stores at 0x131e74..0x131f3c
    // turn those floats into the vertex RGBA -- the aura wisps arrive with A = 0.0 here while
    // console gives 1..55. Log the caller and both colour vectors for wisp-purple calls
    // (R~146 G~90 B~169) plus the first calls of any colour, then run the original.
    // [cadence] PS2X_CADENCE=1 (Kaioken-white hunt, 2026-09-03): per-frame call counts of
    // the per-character fight sampler chain FUN_001c2218 -> 1D2D30 (charge-end sampler) ->
    // 1D0508 -> 1CF678 (condition LEAVE-edge test). The old dev-tree note measured the
    // sampler at "every ~6th frame" while the condition roller runs every frame, so 1-frame
    // edges (cond4 leave = flash stop) were missed. Expected on console: 1c2218/1d2d30 once
    // per character per frame. Prints once per 60 frames.
    // [rayhook] PS2X_RAYHOOK=1 (Kaioken white, 2026-09-03): FUN_00132b60 is the generic camera-facing
    // BILLBOARD QUAD emitter (a0=centre xyzw, a1=colour, a2/a3/t0 packet ctx, f12/f13 = half extents in
    // world units, f14..f17 = uv rect, f18 = roll angle, f19 = Q scale; corners = centre + camRot*rollRot*
    // (+-f12,+-f13,0); projected+clipped by FUN_00131478, emitted by FUN_00132e80). Our Kaioken flash
    // rays (tex 11236) come out clipped to the guard-band corners = enormous quads. Log every call's
    // args (with the caller's ra) once the [raysrc] probe has seen a REAL clamped ray (g_rayHookArm).
    // [clipin] second half of PS2X_RAYHOOK: FUN_00131478 (frustum clipper + emit) receives the 3
    // WORLD-space corner records (48 B each: pos, uv, colour) of one triangle of the quad. For a pure
    // rotation the edges must be 2h, 2h, 2h*sqrt2 (h = the half extent logged by [rayhook]); if the
    // corners come out inflated, the roll*camera matrix path (FUN_00120308/001201b8/00121fd0) scales.
    PS2Runtime::RecompiledFunction g_orig131478 = nullptr;
    void bt3ClipInHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_rayHookArm.load(std::memory_order_relaxed) > 0)
        {
            float v[3][4] = {};
            if (const uint8_t *vp = getMemPtr(rdram, getRegU32(ctx, 4)))
                for (int k = 0; k < 3; ++k) std::memcpy(v[k], vp + k * 48, 16);
            auto d = [&](int a, int b){ const float dx = v[a][0] - v[b][0], dy = v[a][1] - v[b][1], dz = v[a][2] - v[b][2]; return std::sqrt(dx * dx + dy * dy + dz * dz); };
            uint32_t qbits = getRegU32(ctx, 11); float qf; std::memcpy(&qf, &qbits, 4);
            std::fprintf(stderr, "[clipin] v0=(%.2f,%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f,%.2f) v2=(%.2f,%.2f,%.2f,%.2f) edges=%.2f/%.2f/%.2f t3=%.4f\n",
                         v[0][0], v[0][1], v[0][2], v[0][3], v[1][0], v[1][1], v[1][2], v[1][3], v[2][0], v[2][1], v[2][2], v[2][3], d(0, 1), d(1, 2), d(2, 0), qf);
        }
        if (g_orig131478) g_orig131478(rdram, ctx, runtime);
    }
    PS2Runtime::RecompiledFunction g_orig132b60 = nullptr;
    void bt3RayHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_rayHookArm.load(std::memory_order_relaxed) > 0)
        {
            g_rayHookArm.fetch_sub(1, std::memory_order_relaxed);
            const uint32_t a0 = getRegU32(ctx, 4);
            float c[4] = {0, 0, 0, 0};
            if (const uint8_t *cp = getMemPtr(rdram, a0)) std::memcpy(c, cp, 16);
            uint8_t col[16] = {0};
            if (const uint8_t *kp = getMemPtr(rdram, getRegU32(ctx, 5))) std::memcpy(col, kp, 16);
            // project the centre with the VU0 view-projection (vf16..vf19, as FUN_001210d8 does) and
            // estimate the on-screen half size: max screen delta of a +half offset along each world axis
            float M[4][4];
            for (int r = 0; r < 4; ++r) _mm_storeu_ps(M[r], ctx->vu0_vf[16 + r]);
            auto proj = [&](float x, float y, float z, float *o){
                for (int k = 0; k < 4; ++k) o[k] = M[0][k] * x + M[1][k] * y + M[2][k] * z + M[3][k];
            };
            float pc[4]; proj(c[0], c[1], c[2], pc);
            const float w = pc[3] != 0.0f ? pc[3] : 1e-9f;
            const float sx = pc[0] / w, sy = pc[1] / w;
            float rad = 0.0f;
            const float h = ctx->f[12];
            const float offs[3][3] = {{h, 0, 0}, {0, h, 0}, {0, 0, h}};
            for (int k = 0; k < 3; ++k)
            {
                float q[4]; proj(c[0] + offs[k][0], c[1] + offs[k][1], c[2] + offs[k][2], q);
                const float qw = q[3] != 0.0f ? q[3] : 1e-9f;
                const float dx = q[0] / qw - sx, dy = q[1] / qw - sy;
                rad = std::max(rad, std::sqrt(dx * dx + dy * dy));
            }
            {   // camera (billboard) matrix used by FUN_00132b60: [[gp-0x56a0]+0x40], 4x4 floats -- print the first 3 times
                static int s_cm = 0;
                if (s_cm < 3)
                {
                    const uint32_t gp = getRegU32(ctx, 28);
                    uint32_t camp = 0; if (const uint8_t *pp = getMemPtr(rdram, gp - 0x56a0u)) std::memcpy(&camp, pp, 4);
                    float cm[16] = {};
                    if (const uint8_t *mp = getMemPtr(rdram, camp + 0x40u)) std::memcpy(cm, mp, 64);
                    std::fprintf(stderr, "[rayhook] cammat@0x%x: [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n", camp + 0x40u,
                                 cm[0], cm[1], cm[2], cm[3], cm[4], cm[5], cm[6], cm[7], cm[8], cm[9], cm[10], cm[11], cm[12], cm[13], cm[14], cm[15]);
                    ++s_cm;
                }
            }
            std::fprintf(stderr, "[rayhook] ra=0x%x centre=(%.2f,%.2f,%.2f) half=%.3f roll=%.3f col=%02x%02x%02x%02x t0=0x%x | proj=(%.1f,%.1f) w=%.2f scrHalf~%.0fpx\n",
                         getRegU32(ctx, 31), c[0], c[1], c[2], ctx->f[12], ctx->f[18],
                         col[0], col[1], col[2], col[3], getRegU32(ctx, 8), sx, sy, pc[3], rad);
        }
        if (g_orig132b60) g_orig132b60(rdram, ctx, runtime);
    }
    PS2Runtime::RecompiledFunction g_orig1c2218 = nullptr, g_orig1d2d30 = nullptr, g_orig1d0508 = nullptr, g_orig1cf678c = nullptr;
    static std::atomic<uint32_t> s_cad1c2218{0}, s_cad1d2d30{0}, s_cad1d0508{0}, s_cad1cf678{0};
    static void bt3CadenceTick()
    {
        static uint64_t s_lastBucket = 0;
        const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
        const uint64_t bucket = fr / 60u;
        if (bucket != s_lastBucket)
        {
            std::fprintf(stderr, "[cadence] fr=%llu per-60-frames: 1c2218=%u 1d2d30=%u 1d0508=%u 1cf678=%u\n",
                         (unsigned long long)fr, s_cad1c2218.exchange(0), s_cad1d2d30.exchange(0), s_cad1d0508.exchange(0), s_cad1cf678.exchange(0));
            s_lastBucket = bucket;
        }
    }
    void bt3Cad1c2218(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { ++s_cad1c2218; bt3CadenceTick(); if (g_orig1c2218) g_orig1c2218(rdram, ctx, runtime); }
    void bt3Cad1d2d30(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { ++s_cad1d2d30; if (g_orig1d2d30) g_orig1d2d30(rdram, ctx, runtime); }
    void bt3Cad1d0508(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { ++s_cad1d0508; if (g_orig1d0508) g_orig1d0508(rdram, ctx, runtime); }
    void bt3Cad1cf678(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { ++s_cad1cf678; if (g_orig1cf678c) g_orig1cf678c(rdram, ctx, runtime); }

    PS2Runtime::RecompiledFunction g_orig131a20 = nullptr;
    void bt3QuadEmitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00131a20
    {
        static int s_n = 0, s_w = 0;
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6);
        const uint32_t a3 = getRegU32(ctx, 7), t0 = getRegU32(ctx, 8), ra = getRegU32(ctx, 31);
        auto fl = [&](uint32_t addr) -> const float * {
            const uint32_t a = addr & 0x1FFFFFFFu;
            return (a >= 0x100000u && a + 16u <= 0x2000000u) ? reinterpret_cast<const float *>(getMemPtr(rdram, a)) : nullptr;
        };
        const float *c1 = fl(a1), *c2 = fl(a2);
        const bool wisp = c1 && c1[0] > 140.f && c1[0] < 152.f && c1[1] > 85.f && c1[1] < 95.f && c1[2] > 163.f && c1[2] < 175.f;
        if (s_n < 24 || (wisp && s_w < 80))
        {
            std::fprintf(stderr, "[wisphook] #%d%s ra=0x%x a0=0x%x a1=0x%x c1=(%.1f,%.1f,%.1f,%.3f) a2=0x%x c2=(%.1f,%.1f,%.1f,%.3f) a3=0x%x t0=0x%x f12=%.2f f13=%.2f f14=%.2f\n",
                         s_n, wisp ? " WISP" : "", ra, a0, a1,
                         c1 ? c1[0] : 0.f, c1 ? c1[1] : 0.f, c1 ? c1[2] : 0.f, c1 ? c1[3] : 0.f, a2,
                         c2 ? c2[0] : 0.f, c2 ? c2[1] : 0.f, c2 ? c2[2] : 0.f, c2 ? c2[3] : 0.f,
                         a3, t0, ctx->f[12], ctx->f[13], ctx->f[14]);
            ++s_n; if (wisp) ++s_w;
        }
        if (g_orig131a20) g_orig131a20(rdram, ctx, runtime);
    }

    PS2Runtime::RecompiledFunction g_orig281bb0 = nullptr;
    void bt3StreamGroupStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x281bb0
    {
        if (sndIopEnabled())
        {
            const uint32_t group = getRegU32(ctx, 4);
            const uint8_t *req = getMemPtr(rdram, (group + 0x58u) & 0x1FFFFFFFu);
            const uint8_t *cnt = getMemPtr(rdram, (group + 0x52u) & 0x1FFFFFFFu);
            if (req && *req == 1u && cnt)
            {
                const int channels = static_cast<int8_t>(*cnt);
                for (int i = 0; i < channels && i < 8; ++i)
                {
                    const uint32_t so = sndRd32(rdram, group + 0x10u + static_cast<uint32_t>(i) * 4u);
                    if (!so || sndRd8(rdram, so + 1u) == 1u)
                        continue; // already running -- this loop is not its start
                    bt3SndIopResetSink(rdram, runtime, sndRd32(rdram, so + 8u));
                }
            }
        }
        if (g_orig281bb0) g_orig281bb0(rdram, ctx, runtime);
    }

    // [sndstream] PS2X_SNDSTREAM=1. sub_0028AE60(streamObj) is the audio-stream tick that
    // pushes PCM to the IOP by SIF DMA (its sceSifSetDma call returns to 0x28b13c). Layout:
    //   [+1]    stream state   (must be 1 or the tick exits immediately)
    //   [+2]    "DMA in flight" flag; set after a push, cleared on the poll path
    //   [+0x10] bytes the IOP reports consumed this poll
    //   [+0x3C] accumulated playback position (+= [+0x10])
    // Each destination got exactly ONE transfer, so either this tick stops running or the
    // consumed count stays 0 (nothing drains the buffer) and it never queues the next block.
    PS2Runtime::RecompiledFunction g_orig28ae60 = nullptr;
    void bt3SndStreamTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_0028AE60
    {
        const uint32_t obj = getRegU32(ctx, 4);
        auto rd8 = [&](uint32_t off) -> uint32_t {
            const uint8_t *p = getMemPtr(rdram, (obj + off) & 0x1FFFFFFFu);
            return p ? *p : 0xFFu; };
        auto rd32 = [&](uint32_t off) -> uint32_t {
            const uint8_t *p = getMemPtr(rdram, (obj + off) & 0x1FFFFFFFu);
            return p ? *reinterpret_cast<const uint32_t *>(p) : 0u; };
        static std::atomic<uint32_t> n{0};
        const uint32_t k = n.fetch_add(1);
        const uint32_t state = rd8(1);
        // NOTE: a plain "every Nth tick" sample aliases badly here -- the tick cycles over
        // ~10 stream objects, so k%300 lands on the SAME object forever and hides the others.
        // Log whenever a stream is actually ACTIVE (state!=0), which is the case of interest,
        // plus the first few ticks for layout confirmation.
        // Log every STATE TRANSITION per stream object, plus a periodic sample while active.
        // A flat "first N active ticks" cap only covers the opening seconds -- precisely the
        // window BEFORE the ~2s cutout -- so it can never show what changes AT the cutout.
        // If state flips 1 -> 0 there, the game stopped the stream itself (scene/state change)
        // and the pump is innocent; if it stays 1 while transfers dry up, we are starving it.
        static std::mutex s_stM;
        static std::map<uint32_t, uint32_t> s_lastState;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(s_stM);
            auto it = s_lastState.find(obj);
            if (it == s_lastState.end() || it->second != state)
            {
                changed = true;
                s_lastState[obj] = state;
            }
        }
        // This hook is now installed whenever audio is on, so every diagnostic in it has to
        // sit behind PS2X_SNDSTREAM -- an unconditional fprintf here runs on the sound thread
        // thousands of times a second.
        static const bool s_streamLog = []() {
            const char *v = std::getenv("PS2X_SNDSTREAM");
            return v && v[0] && v[0] != '0';
        }();
        static std::atomic<uint32_t> act{0};
        if (s_streamLog && (k < 10u || changed || (state != 0u && (act.fetch_add(1) % 120u) == 0u)))
        {
            // Resolve the two virtual calls that report playback progress:
            //   0x28aea8: [obj+4]->vtbl[+0x20](this, 0, &obj[+0x0C])
            //   0x28aec8: [obj+8]->vtbl[+0x20](this, 1, &obj[+0x14])
            // Their OUT param lands in [+0x10] (bytes consumed). Whichever function backs
            // vtbl+0x20 is what must report drain progress for the refill gate to reopen.
            const uint32_t o4 = rd32(4), o8 = rd32(8);
            auto deref = [&](uint32_t addr, uint32_t off) -> uint32_t {
                const uint8_t *p = getMemPtr(rdram, (addr + off) & 0x1FFFFFFFu);
                return p ? *reinterpret_cast<const uint32_t *>(p) : 0u; };
            const uint32_t vt4 = o4 ? deref(o4, 0) : 0u;
            const uint32_t vt8 = o8 ? deref(o8, 0) : 0u;
            // The refill gate is `blezl $s1` at 0x28b000, where s1 = the SINK's free space.
            // That space comes from a linked list of buffer descriptors at [sink+0x18+mode*4]
            // (mode 0 here): sub_002842F8 returns 0 when the list head is null (0x28438c),
            // and each pop recycles the node onto sink->freelist at [sink+0x14] (0x2843d8).
            // If freeList goes null while freeNodes accumulates, the ring drained into the
            // recycler and nothing ever hands buffers back -- that is the whole bug.
            std::fprintf(stderr,
                         "[sndstream] tick#%u obj=0x%x state=%u inflight=%u resid=%u pos=%u "
                         "| src=0x%x fn=0x%x | sink=0x%x fn=0x%x freeList[+0x18]=0x%x recycler[+0x14]=0x%x\n",
                         k + 1u, obj, state, rd8(2), rd32(0x10), rd32(0x3C),
                         o4, vt4 ? deref(vt4, 0x20) : 0u,
                         o8, vt8 ? deref(vt8, 0x20) : 0u,
                         o8 ? deref(o8, 0x18) : 0u, o8 ? deref(o8, 0x14) : 0u);
        }
        // ---- IOP-side ring consumer (default) ----------------------------------------
        // Return exactly the space the host device has played. Runs before the game's own
        // tick, on the same thread that drains the ring, and for stopped-but-allocated
        // streams too so a tail left queued at STOP still drains away.
        if (sndIopEnabled())
        {
            const uint32_t sink = rd32(8);
            if (sink)
            {
                // Ring GEOMETRY. Whenever a sink is idle its free list is exactly one node
                // spanning the whole ring, so that node is {base, size} -- and the geometry is
                // what makes `iopAddr -> stream id` correct at the ring ends. Keep trying on
                // every idle tick rather than only the first one: registerStreamRing dedupes,
                // and a sink first seen mid-flight would otherwise never register at all and
                // would fall back to the broken shift split for the rest of the run.
                {
                    const uint32_t head = sndRd32(rdram, sink + kSinkList0);
                    if (head && runtime &&
                        sndRd32(rdram, head + kNodeNext) == 0u &&
                        sndRd32(rdram, sink + kSinkList1) == 0u)
                    {
                        runtime->audioBackend().registerStreamRing(sndRd32(rdram, head + kNodePtr),
                                                                   sndRd32(rdram, head + kNodeLen));
                    }
                    // Then the sink -> stream mapping, if it does not have one yet. The DMA
                    // destination recorded at [obj+0x20] is the authoritative source; the
                    // free-list head only covers the ticks before the first transfer.
                    bool known = false;
                    {
                        std::lock_guard<std::mutex> lk(g_iopSinkM);
                        auto it = g_iopSinks.find(sink);
                        known = (it != g_iopSinks.end() && it->second.streamId != 0xFFFFFFFFu);
                    }
                    if (!known && head)
                        sndNoteSinkStream(rdram, runtime, sink, sndRd32(rdram, head + kNodePtr));
                }
                bt3SndIopConsume(rdram, runtime, sink);
            }
            if (g_orig28ae60) g_orig28ae60(rdram, ctx, runtime);
            // A queued transfer records its IOP destination at [obj+0x20]; that is the exact
            // same address SIF.cpp splits streams by, so take the mapping from the transfer
            // rather than inferring it.
            if (rd8(2) == 1u)
                sndNoteSinkStream(rdram, runtime, rd32(8), rd32(0x20));
            return;
        }

        // ---- PS2X_SNDPUMP=1: LEGACY. Superseded by the consumer above; kept behind
        // PS2X_SNDIOP=0 as a rollback path only. It fabricates buffer returns from cached
        // {ptr,len} descriptors on a backlog clock, which is what created the permanent L/R
        // one-buffer offset and forced the blanket stop suppression. Do not extend it.
        // On hardware the IOP hands each streaming buffer back once SPU2 has played it, which
        // re-arms the sink's free list at [sink+0x18]. With no IOP the list drains into the
        // recycler at [sink+0x14] and the refill gate (`blezl $s1` at 0x28b000) shuts forever
        // -- the game stops streaming after filling the ring once.
        //
        // A recycled node still describes its own buffer: sub_002842F8 pops it by rewriting
        // ONLY the two heads and node->next (0x2843c8/0x2843d0/0x2843d8) and never touches the
        // {ptr,len} pair at node+8/+0xC. So giving a buffer back is a pure relink -- no address
        // has to be invented. Layout: node+0 = next, node+8 = ptr, node+0xC = len.
        //
        // Only ever acts when the free list is EMPTY, so a list the game is actively using is
        // never touched, and only from inside the stream tick (the same thread that drains it).
        // Rate-limited to roughly playback speed; PS2X_SNDPUMP_MS overrides the per-buffer
        // interval (default 50ms ~= 4864 bytes of 16-bit mono at ~48kHz).
        static const bool s_pump = []() {
            const char *v = std::getenv("PS2X_SNDPUMP");
            return v && v[0] && v[0] != '0';
        }();
        if (s_pump && state == 1u)
        {
            static const long s_ms = []() -> long {
                if (const char *v = std::getenv("PS2X_SNDPUMP_MS"))
                {
                    const long n = std::strtol(v, nullptr, 10);
                    if (n > 0) return n;
                }
                return 50;
            }();
            const uint32_t sink = rd32(8);
            auto peek = [&](uint32_t addr, uint32_t off) -> uint32_t {
                const uint8_t *p = getMemPtr(rdram, (addr + off) & 0x1FFFFFFFu);
                return p ? *reinterpret_cast<const uint32_t *>(p) : 0u; };
            auto poke = [&](uint32_t addr, uint32_t off, uint32_t val) {
                if (uint8_t *p = getMemPtr(rdram, (addr + off) & 0x1FFFFFFFu))
                    *reinterpret_cast<uint32_t *>(p) = val; };

            // The recycler at [sink+0x14] is a general node pool: it holds popped nodes AND
            // virgin ones with {ptr,len} = {0,0}. Handing back a virgin node is useless -- the
            // query returns len 0, the gate stays shut, and the node cycles back to us forever.
            // So learn the real ring buffers by watching the free list while it is still
            // armed, then write those {ptr,len} into whatever node we hand back.
            const uint32_t head = sink ? peek(sink, 0x18) : 0u;
            if (head)
            {
                const uint32_t bp = peek(head, 0x08), bl = peek(head, 0x0C);
                if (bp && bl)
                {
                    std::lock_guard<std::mutex> lk(g_sinkRingM);
                    auto &ring = g_sinkRings[sink];
                    bool known = false;
                    for (const auto &e : ring.bufs)
                        if (e.first == bp) { known = true; break; }
                    if (!known && ring.bufs.size() < 16u)
                        ring.bufs.emplace_back(bp, bl);
                    const uint32_t sid = bp >> 14;
                    if (sid < 2u) g_pairSink[sid] = sink;
                }
            }

            // NODE POOL EXHAUSTION (the ~2s cutout): popping a node off the recycler on every
            // return drains the pool -- measured recycler 0x2db570 -> 0x590 -> 0x5c0 -> 0x620
            // -> 0x0, after which the pump has nothing to hand over and the stream starves
            // while state stays 1. Not all nodes come back through the recycler, so this is a
            // one-way leak. Once the pool is dry, reuse the node we last handed over instead:
            // the empty free list proves the game has already taken it, and the empty recycler
            // proves it is not queued there, so it is unlinked and safe to re-arm.
            static std::mutex s_nodeM;
            static std::map<uint32_t, uint32_t> s_lastNode;
            // STEREO LOCKSTEP: only advance a pair sink when its PARTNER is starved too.
            // Otherwise one side can receive a buffer the other does not; the game then writes
            // ~2432 extra samples into that channel and L[i]/R[i] refer to source times ~100ms
            // apart FOREVER after -- heard as one ear suddenly lagging and staying behind.
            // Gating both on the same backlog threshold (done earlier) equalises WHEN they are
            // due, but not WHETHER each is starved, so it cannot prevent this on its own.
            // Keep the two channels BALANCED rather than synchronised. Requiring both sides to
            // be starved at the same instant deadlocks: if one side's free list stays armed,
            // neither is ever fed, both run dry and the music cuts out. Instead just refuse to
            // let either channel get more than one buffer ahead of the other -- that is all
            // that is needed to stop a permanent L/R offset from forming, and it can never
            // stall, because the lagging side is always allowed to catch up.
            // NOTE: two attempts at enforcing L/R lockstep here were REVERTED --
            //   (a) "both sinks must be starved" deadlocked whenever one side stayed armed:
            //       neither got a buffer, both ran dry, BGM cut out;
            //   (b) "never let one side get more than one ahead" stopped the pair being fed.
            // The stereo offset is real, but it must be fixed WITHOUT gating the pump on the
            // partner's state. See notes before trying again.
            const bool pairReady = true;
            const int pairIdx = -1;

            if (sink && head == 0u && pairReady) // free list empty -> starved
            {
                uint32_t node = peek(sink, 0x14); // recycler head
                bool reused = false;
                if (!node)
                {
                    std::lock_guard<std::mutex> lk(s_nodeM);
                    auto it = s_lastNode.find(sink);
                    if (it != s_lastNode.end()) { node = it->second; reused = true; }
                }
                if (node)
                {
                    // SELF-CLOCKING: when the audio backend is consuming this stream, pace the
                    // buffer return by its backlog rather than a wall clock. A fixed interval
                    // has to guess the true sample rate; guess high and the backlog grows until
                    // samples are dropped, guess low and the device underruns -- either way it
                    // glitches. Gating on backlog makes the guest produce exactly as fast as
                    // audio is consumed, whatever the real rate turns out to be.
                    // PS2X_SNDPUMP_MS still applies as a fallback when nothing is consuming.
                    // Pick the buffer FIRST: its address identifies which audio stream this
                    // return feeds (streamId = ptr >> 14, same split SIF.cpp uses), so the
                    // backlog gate can be per-stream instead of global.
                    uint32_t bp = 0u, bl = 0u;
                    {
                        std::lock_guard<std::mutex> lk(g_sinkRingM);
                        auto it = g_sinkRings.find(sink);
                        if (it != g_sinkRings.end() && !it->second.bufs.empty())
                        {
                            auto &ring = it->second;
                            const auto &e = ring.bufs[ring.next % ring.bufs.size()];
                            bp = e.first;
                            bl = e.second;
                        }
                    }

                    bool due = false;
                    size_t backlog = 0u;
                    if (runtime && bp)
                    {
                        const uint32_t sid = bp >> 14;
                        if (sid == 0u || sid == 1u)
                        {
                            // Stereo pair: gate BOTH sides on the same value so buffers are
                            // handed back in lockstep and neither source position runs ahead.
                            backlog = std::min(runtime->audioBackend().streamBacklog(0u),
                                               runtime->audioBackend().streamBacklog(1u));
                        }
                        else
                        {
                            backlog = runtime->audioBackend().streamBacklog(sid);
                        }
                    }
                    if (backlog > 0u)
                    {
                        static const size_t s_target = []() -> size_t {
                            if (const char *v = std::getenv("PS2X_SNDBACKLOG"))
                            {
                                const long n = std::strtol(v, nullptr, 10);
                                if (n > 0) return static_cast<size_t>(n);
                            }
                            return 8192;  // known-good. MUST exceed the start cushion
                                          // (2 x kStreamChunkFrames = 4096) or playback never
                                          // starts. 16384 was ~680ms of audible lag behind the
                                          // game -- that latency is what "BGM desynced" was.
                        }();
                        due = backlog < s_target;
                    }
                    else
                    {
                        static std::mutex s_m;
                        static std::map<uint32_t, std::chrono::steady_clock::time_point> s_last;
                        const auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> lk(s_m);
                        auto it = s_last.find(sink);
                        if (it == s_last.end() ||
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() >= s_ms)
                        {
                            s_last[sink] = now;
                            due = true;
                        }
                    }
                    // Why did a starved stream NOT get a buffer back? Each guess costs a full
                    // build+boot to test, so record the actual reason instead.
                    if (!(due && bp && bl))
                    {
                        static std::atomic<uint32_t> dn{0};
                        const uint32_t d = dn.fetch_add(1);
                        if (d < 10u || (d % 2000u) == 0u)
                            std::fprintf(stderr,
                                         "[sndpump] DECLINE #%u sink=0x%x node=0x%x reused=%d due=%d "
                                         "bp=0x%x bl=%u backlog=%zu\n",
                                         d + 1u, sink, node, reused ? 1 : 0, due ? 1 : 0, bp, bl, backlog);
                    }
                    if (due && bp && bl)
                    {
                        {   // consume this ring slot only once the return actually happens
                            std::lock_guard<std::mutex> lk(g_sinkRingM);
                            auto it = g_sinkRings.find(sink);
                            if (it != g_sinkRings.end()) it->second.next++;
                        }
                        if (!reused)
                            poke(sink, 0x14, peek(node, 0x00)); // pop node off the recycler
                        {   // remember it so we can re-arm with it once the pool runs dry
                            std::lock_guard<std::mutex> lk(s_nodeM);
                            s_lastNode[sink] = node;
                        }
                        poke(node, 0x00, 0u);               // node->next = null (single entry)
                        poke(node, 0x08, bp);               // describe a real ring buffer
                        poke(node, 0x0C, bl);
                        poke(sink, 0x18, node);             // arm the free list with it
                        if (pairIdx >= 0) g_pairReturns[pairIdx]++;
                        static std::atomic<uint32_t> ret{0};
                        const uint32_t r = ret.fetch_add(1);
                        if (r < 8u || (r % 200u) == 0u)
                            std::fprintf(stderr,
                                         "[sndpump] returned buffer #%u to sink=0x%x node=0x%x ptr=0x%x len=%u\n",
                                         r + 1u, sink, node, bp, bl);
                    }
                }
            }
        }

        if (g_orig28ae60) g_orig28ae60(rdram, ctx, runtime);
    }

    // 0x28b428 STREAM START (`pos[+0x3C] = 0; state[+1] = 1`). The IOP resets its ring when a
    // stream starts, and the game asserts on that: 0x281bb0 takes the sink's ENTIRE free list
    // and infinite-loops at 0x281cf0 unless the length is the expected prefill. So put the ring
    // back to "all free, nothing queued" here and drop the host-side tail that belonged to the
    // previous stream, otherwise the leftovers would be credited to the new one.
    //
    // The timestamp is only used by the legacy PS2X_SNDNOSTOP=drain mode.
    std::mutex g_streamStartM;
    std::map<uint32_t, std::chrono::steady_clock::time_point> g_streamStart;
    PS2Runtime::RecompiledFunction g_orig28b428 = nullptr;
    void bt3StreamStartNote(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x28b428
    {
        const uint32_t obj = getRegU32(ctx, 4);
        {
            std::lock_guard<std::mutex> lk(g_streamStartM);
            g_streamStart[obj] = std::chrono::steady_clock::now();
        }
        if (sndIopEnabled())
        {
            const uint32_t sink = sndRd32(rdram, obj + 8u);
            bt3SndIopResetSink(rdram, runtime, sink);
        }
        if (g_orig28b428) g_orig28b428(rdram, ctx, runtime);
    }

    // [sndnostop] 0x28b438 STREAM STOP. RETIRED -- default is now plain pass-through.
    //
    // Suppression was only ever load-bearing because the ring never drained on its own: with
    // no playback progress the game saw every sound as finished the moment it started and tore
    // the stream down, so refusing the stop was the only way to keep a voice alive. It bought
    // that at the cost of the game's cleanup never running, which is what made it re-trigger
    // lines it believed had ended -- heard as fragments of a line playing over itself. With the
    // IOP consumer reporting real playback progress the lifecycle is the game's again, so the
    // stop is honoured; the streams keep ticking while stopped, so any tail still queued drains
    // away instead of being cut.
    //
    // Kept only as a rollback path:
    //   PS2X_SNDNOSTOP=blanket|1|2  suppress every stop (the old default)
    //   PS2X_SNDNOSTOP=drain        suppress only while the backend still has samples
    //   unset / off                 pass through (default)
    PS2Runtime::RecompiledFunction g_orig28b438 = nullptr;
    void bt3StreamStopSuppress(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x28b438
    {
        const uint32_t obj = getRegU32(ctx, 4);

        // Suppress ONLY the BGM pair by default. Blanket suppression also blocks the VOICE
        // streams, which legitimately stop and restart between lines -- ignoring those leaves
        // stale stream state and is heard as voice acting glitching mid-sentence. It also lets
        // the game reconfigure one half of the BGM pair without the other, which desyncs the
        // stereo image in a way the symmetric trim cannot repair.
        //   PS2X_SNDNOSTOP=1  -> BGM pair only (default, recommended)
        //   PS2X_SNDNOSTOP=2  -> suppress every stream stop (the old blunt behaviour)
        // The pair addresses have been stable across every run this session;
        // PS2X_SNDBGMOBJ=<hex>,<hex> overrides if a build ever moves them.
        // DRAIN CHECK (the proper fix, replacing blanket suppression).
        //
        // Our engine reports every sound "finished" the instant it starts, so the game tears
        // each stream down immediately -- that is why voices were silent until stops were
        // suppressed, and why suppressing them unconditionally makes the game re-trigger lines
        // it thinks already ended (heard as voice acting glitching mid-sentence).
        //
        // So: allow the stop only once the audio for that stream has ACTUALLY been consumed by
        // the device. While samples are still queued, the sound is genuinely still playing and
        // the teardown is premature, so suppress it. Once drained, let the game stop it exactly
        // as it intends -- lines end cleanly and are not re-triggered on top of themselves.
        //
        // Object -> stream id: the stream object holds its sink at [obj+8]; the pump has cached
        // that sink's ring buffers, and the audio stream id is bufferPtr >> 14 (same split
        // SIF.cpp uses when feeding).
        //   PS2X_SNDNOSTOP=blanket|1|2 -> old unconditional suppression (rollback only)
        //   PS2X_SNDNOSTOP=drain       -> suppress while the backend still holds samples
        //   unset / off                -> pass through (default, now that progress is honest)
        static const int s_mode = []() -> int {
            const char *v = std::getenv("PS2X_SNDNOSTOP");
            if (!v || !v[0]) return 0;
            if (v[0] == 'd') return 1;                       // drain check + grace
            if (v[0] == 'b' || v[0] == '1' || v[0] == '2') return 2; // blanket
            return 0;                                        // off / anything else
        }();
        if (s_mode == 0)
        {
            if (g_orig28b438) g_orig28b438(rdram, ctx, runtime);
            return;
        }

        bool stillPlaying = false;
        if (s_mode == 2)
        {
            stillPlaying = true; // blanket fallback
        }
        else if (runtime)
        {
            uint32_t sink = 0u;
            if (const uint8_t *p = getMemPtr(rdram, (obj + 8u) & 0x1FFFFFFFu))
                sink = *reinterpret_cast<const uint32_t *>(p);
            uint32_t bufPtr = 0u;
            if (sink)
            {
                std::lock_guard<std::mutex> lk(g_sinkRingM);
                auto it = g_sinkRings.find(sink);
                if (it != g_sinkRings.end() && !it->second.bufs.empty())
                    bufPtr = it->second.bufs.front().first;
            }
            if (bufPtr)
            {
                // One sub-buffer of slack: below that it is effectively done.
                const size_t backlog = runtime->audioBackend().streamBacklog(bufPtr >> 14);
                stillPlaying = backlog > 2048u;
            }
            else
            {
                // No mapping for this object: assume it IS still playing. Allowing the stop
                // here silences voices completely.
                stillPlaying = true;
            }

            // GRACE WINDOW: refuse any teardown that arrives right after the start, whatever
            // the backlog says. This is the case that kept killing the voice lines -- the stop
            // lands before the stream has produced a single sample, so the backlog is 0 and
            // looks "drained". PS2X_SNDGRACE_MS overrides (default 3000).
            if (!stillPlaying)
            {
                static const long s_graceMs = []() -> long {
                    if (const char *v = std::getenv("PS2X_SNDGRACE_MS"))
                    {
                        const long n = std::strtol(v, nullptr, 10);
                        if (n >= 0) return n;
                    }
                    return 3000;
                }();
                std::lock_guard<std::mutex> lk(g_streamStartM);
                auto it = g_streamStart.find(obj);
                if (it != g_streamStart.end())
                {
                    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - it->second).count();
                    if (age < s_graceMs)
                        stillPlaying = true;
                }
            }
        }

        if (stillPlaying)
        {
            static std::atomic<uint32_t> n{0};
            const uint32_t k = n.fetch_add(1);
            if (k < 12u || (k % 200u) == 0u)
                std::fprintf(stderr, "[sndnostop] deferred STOP #%u obj=0x%x (still draining)\n",
                             k + 1u, obj);
            setReturnS32(ctx, 0); // not finished yet -- refuse the premature teardown
            return;
        }

        // Everything else (voice, SE) stops normally.
        if (g_orig28b438) g_orig28b438(rdram, ctx, runtime);
    }

    // [sndreg] PS2X_SNDREG=1: log every handler registration into the per-slot dispatch
    // table at 0x3215A0 + slot*72. FUN_00286050(slot) walks 6 entries of 12 bytes there and
    // calls each non-null fn; the sound service thread (tid6, entry 0x26d070) dispatches
    // slot 6, whose table is EMPTY — so it loops forever doing nothing and the sound-preload
    // completion never runs. This shows which slots DO get handlers, and whether anything
    // ever tries to register one for slot 6.
    PS2Runtime::RecompiledFunction g_orig285c50 = nullptr;
    void bt3SoundRegProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00285c28
    {
        const uint32_t slot = getRegU32(ctx, 4);   // a0
        const uint32_t fn   = getRegU32(ctx, 5);   // a1 -> [entry+0]
        const uint32_t arg  = getRegU32(ctx, 6);   // a2 -> [entry+4]
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1) < 64u)
            std::fprintf(stderr, "[sndreg] register slot=%u fn=0x%x arg=0x%x  ra=0x%x\n",
                         slot, fn, arg, getRegU32(ctx, 31));
        if (g_orig285c50) g_orig285c50(rdram, ctx, runtime);
    }

    void bt3ResReadyProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00252d78
    {
        const uint32_t id = getRegU32(ctx, 4); // a0 = resource id
        if (g_orig252d78) g_orig252d78(rdram, ctx, runtime);
        static const bool s_lp = [](){ const char *v=std::getenv("PS2X_LOADPROBE"); return v&&v[0]&&v[0]!='0'; }();
        if (s_lp)
        {
            const uint32_t ready = getRegU32(ctx, 2); // $v0 return
            if (ready == 0u) // not ready -> this is (one of) the stuck resource(s)
            {
                static std::mutex m; static std::map<uint32_t,uint32_t> notReady; static std::atomic<uint32_t> n{0};
                uint32_t f58 = 0;
                if (const uint8_t *p = getMemPtr(rdram, 0x31c670u + id*96u + 0x58u)) f58 = *reinterpret_cast<const uint32_t*>(p);
                std::lock_guard<std::mutex> lk(m);
                notReady[id]++;
                if ((n.fetch_add(1) % 400u) == 1u)
                {
                    std::cerr << "[resready] NOT-READY ids:";
                    for (auto &kv : notReady) std::cerr << " id=" << kv.first << "(x" << kv.second << ")";
                    std::cerr << " | last id=" << id << " +0x58=0x" << std::hex << f58 << std::dec << std::endl;
                }
            }
        }
    }

    // Fight-load async read-completion (PS2X_FIGHTDONE, experimental). The fight streams
    // its assets via the DVCI async path; our HLE delivers the data (verified correct) but
    // never signals the async "read complete", so the loading-minigame loop (func_122A38)
    // spins forever while func_296160()==1. Menu/logos use synchronous sceCdRead (no such
    // poll) which is why they load fine. TEST: run the real func_296160, and once we've
    // been in the fight-load state (bt3state=0x27) long enough for the reads to land,
    // override its result to "done" (!=1) so the loop exits into the 3D battle.
    PS2Runtime::RecompiledFunction g_orig296160 = nullptr;
    void bt3LoadStatusDone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00296160
    {
        if (g_orig296160) g_orig296160(rdram, ctx, runtime);
        static const bool s_fd = [](){ const char *v=std::getenv("PS2X_FIGHTDONE"); return v&&v[0]&&v[0]!='0'; }();
        if (!s_fd) return;
        uint32_t bt3State = 0xffffffffu, sp = 0;
        if (const uint8_t *p = getMemPtr(rdram, 0x2ff10cu)) sp = *reinterpret_cast<const uint32_t*>(p);
        if (sp) { if (const uint8_t *p = getMemPtr(rdram, (sp & 0x1FFFFFFFu) + 0x18u)) bt3State = *reinterpret_cast<const uint32_t*>(p); }
        if (bt3State == 0x27u)
        {
            static std::atomic<uint32_t> s_n{0};
            if (s_n.fetch_add(1) > 400u) // settle: let the async reads deliver first
                setReturnU32(ctx, 0u); // != 1 -> the loading-minigame loop exits
        }
    }

    // Fight-load task-queue probe. FUN_00263508 walks the work-item queue at *(0x2FF120):
    //   head = *(0x2FF120); node = *(head+0xC); obj = *(node+4); callback = *(obj+4);
    // it calls callback(obj) and re-queues while the callback returns 1. The loader loop
    // (FUN_002635c8) keeps spinning while FUN_00263508 != 0, i.e. while the queue is non-
    // empty. Whatever task callback returns 1 forever IS the stuck subsystem. Dump it.
    PS2Runtime::RecompiledFunction g_orig263508 = nullptr;
    std::atomic<uint32_t> g_dvciCompleteCalls{0};
    void bt3TaskQueueProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00263508
    {
        auto rd = [&](uint32_t a) -> uint32_t {
            const uint8_t *p = getMemPtr(rdram, a & 0x1FFFFFFFu);
            return p ? *reinterpret_cast<const uint32_t*>(p) : 0u;
        };
        // Read the queue BEFORE the original runs (it may advance the node).
        const uint32_t head = rd(0x2FF120u);
        const uint32_t node = head ? rd(head + 0xCu) : 0u;
        const uint32_t obj  = node ? rd(node + 0x4u) : 0u;
        const uint32_t cb   = obj  ? rd(obj  + 0x4u) : 0u;
        if (g_orig263508) g_orig263508(rdram, ctx, runtime);
        const uint32_t ret = getRegU32(ctx, 2); // $v0: non-zero => queue still busy
        static std::mutex m;
        static std::map<uint32_t,uint32_t> cbHits; // callback addr -> times seen
        static std::atomic<uint32_t> n{0};
        {
            std::lock_guard<std::mutex> lk(m);
            if (cb) cbHits[cb]++;
            if ((n.fetch_add(1) % 240u) == 1u)
            {
                std::cerr << "[taskq] ret=" << ret << " head=0x" << std::hex << head
                          << " node=0x" << node << " obj=0x" << obj
                          << " cb=0x" << cb;
                if (obj) std::cerr << " obj[0]=0x" << rd(obj) << " obj[8]=0x" << rd(obj+8u)
                                   << " obj[c]=0x" << rd(obj+0xcu) << " obj[10]=0x" << rd(obj+0x10u);
                std::cerr << " | callbacks seen:";
                for (auto &kv : cbHits) std::cerr << " 0x" << kv.first << "(x" << std::dec << kv.second << std::hex << ")";
                std::cerr << std::dec << std::endl;
                // DVCI slot table dump: base=*(0x2FF18C), 8 entries stride 0x4C.
                const uint32_t base = rd(0x2FF18Cu);
                std::cerr << "[dvci] base=0x" << std::hex << base
                          << " completeCalls=" << std::dec << g_dvciCompleteCalls.load() << std::hex;
                if (base) for (uint32_t i = 0; i < 8u; ++i) {
                    const uint32_t s = base + i*0x4Cu;
                    std::cerr << " s" << std::dec << i << "[+30=0x" << std::hex << rd(s+0x30u)
                              << ",+34=0x" << rd(s+0x34u) << ",+0=0x" << rd(s) << "]";
                }
                std::cerr << std::dec << std::endl;
            }
        }
    }

    // ***** FIGHT-LOAD COMPLETION FIX *****
    // The fight streams its assets via the DVCI SPU-DMA path. The per-frame pump
    // FUN_00124a70 issues each slot's transfer (loop 2: FUN_001011b8/FUN_001244f0,
    // sets slot+0x34=1) then, on the next pump, polls completion (loop 1:
    // FUN_00124548) and clears slot+0x30/+0x34 IFF the poll returns 1. FUN_00124548
    // is sceSdRemote(BlockTransStatus) whose HLE stub returns the SPU block position
    // (never exactly 1) -> the slot never clears -> sub_00124E60 (all-slots-idle
    // check) stays 0 -> the load task FUN_00127c40 is frozen at state 3 -> the loader
    // FUN_002635c8 spins forever on the loading minigame. Our block transfers are
    // SYNCHRONOUS (the data is delivered the instant loop 2 issues the DMA, one pump
    // call before loop 1 checks), so the transfer is always already complete when
    // polled: report 1. FUN_00124548 is called ONLY by the DVCI pump (verified), so
    // this does not affect the sound streamer. This is the true async-completion
    // signal our HLE was missing -- the last blocker before a rendered 3D fight.
    void bt3DvciSlotComplete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00124548
    {
        (void)rdram; (void)runtime;
        g_dvciCompleteCalls.fetch_add(1, std::memory_order_relaxed);
        setReturnU32(ctx, 1u); // 1 == this slot's block transfer is complete
        ctx->pc = getRegU32(ctx, 31);
    }

    // ***** FIGHT-LOAD AFS-STREAM COMPLETION FIX *****
    // The fight streams its assets from the AFS archives via the CRI file server, whose
    // per-tick engine is FUN_0028a3b0 (CD file server, state 0x2E637C) -- NOT FUN_0028a530
    // (that is the AUDIO server, which bt3FileLoadPoll pumps). The AFS partition state byte
    // at handle+1 is advanced only by the chain 0x28a3b0 -> sub_0028A3D8 phase5 ->
    // func_26B388 -> func_26B3B0 -> func_26B2C0 -> func_270dd0 (writes handle+1). But the
    // AFS load loop polls THIS function (func_26B900, which just returns int8 *(handle+1)),
    // never func_270dd0 -- so the CD server is never ticked and handle+1 freezes at 2
    // ("reading"), never reaching 3 ("ready"). Fix (structural analog of bt3CdReadStatePoll,
    // which pumps the same tick for the func_270dd0 poll): on each AFS-status poll, pump
    // FUN_0028a3b0 inline so the partition read advances, then return the real state byte.
    // Confined to AFS status checks (func_26B900), so other phases are untouched.

    // [cdstate] One "done" observation per submitted read. Keyed on the SUBMIT, because the
    // previous value the guest saw is useless here: the wedge shows the device going 3 (previous
    // read) -> 1 with no 2 or 3 observed for the new read at all, so any rule based on the last
    // polled value cannot see it.
    struct Bt3DevDone { std::atomic<uint32_t> dev{0u}; std::atomic<uint32_t> reported{1u};
                        std::atomic<uint32_t> stream{0u}; std::atomic<uint32_t> idleWait{0u};
                        std::atomic<uint32_t> activeReq{0u}; };   // [cdedge2] pending request ([stream+8]) the device was seen busy/done for
    inline Bt3DevDone *bt3DevSlot(uint32_t dev)
    {
        static Bt3DevDone s_slots[8];
        for (Bt3DevDone &c : s_slots)
        {
            const uint32_t h = c.dev.load(std::memory_order_relaxed);
            if (h == dev) return &c;
            if (h == 0u)
            {
                uint32_t expected = 0u;
                if (c.dev.compare_exchange_strong(expected, dev, std::memory_order_relaxed) ||
                    c.dev.load(std::memory_order_relaxed) == dev)
                    return &c;
            }
        }
        return nullptr;
    }

    PS2Runtime::RecompiledFunction g_orig270dd0 = nullptr;
    void bt3CdStateEdge(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t handle = getRegU32(ctx, 4); // a0 = device object
        if (!g_orig270dd0)
            return;
        g_orig270dd0(rdram, ctx, runtime);
        uint32_t state = getRegU32(ctx, 2); // $v0 = device read-state byte

        // The wedge, expressed purely in state we can observe: the device reports idle (1) while
        // the stream it serves still says busy (state 2) with a request pending. Healthy operation
        // never looks like that -- the device reads 2, then 3.
        if (state == 1u)
        {
            if (Bt3DevDone *ds = bt3DevSlot(handle))
            {
                const uint32_t stream = ds->stream.load(std::memory_order_relaxed);
                bool waiting = false;
                if (stream)
                {
                    const uint8_t *st = getConstMemPtr(rdram, stream + 1u);
                    const uint8_t *pend = getConstMemPtr(rdram, stream + 8u);
                    uint32_t pendVal = 0u;
                    if (pend) std::memcpy(&pendVal, pend, sizeof(pendVal));
                    waiting = st && (*st == 2u) && (pendVal != 0u);
                }
                // Act on the FIRST such poll. There is never a second: the guest stores this
                // return into its stream state, and a 1 closes the gate at 0x26b2e0 (`bnel $v0,2`)
                // so func_26B2C0 never polls again -- which is exactly why every version of this
                // latch that waited for repetition could never fire. The gate also guarantees the
                // caller is mid-read, so a device reporting idle here cannot still be reading it.
                // PS2X_CDEDGE=0 keeps this hook installed (so its timing cost is unchanged) but
                // stops it substituting the value -- the A/B that says whether the fix is the
                // substitution or just the extra frame per poll shifting the race.
                static const bool s_edgeFix = [](){ const char *v = std::getenv("PS2X_CDEDGE"); return !(v && v[0] == '0'); }();
                static const bool s_edge2 = [](){ const char *v = std::getenv("PS2X_CDEDGE2"); return v && v[0] && v[0] != '0'; }();   // opt-in (see the pump)
                uint32_t pendNow = 0u;
                if (stream) { if (const uint8_t *pp = getConstMemPtr(rdram, stream + 8u)) std::memcpy(&pendNow, pp, sizeof(pendNow)); }
                if (waiting && s_edgeFix && s_edge2 && ds->activeReq.load(std::memory_order_relaxed) != pendNow)
                {   // [cdedge2] the device has not been seen working on THIS request: it is queued, not done.
                    // Hardware never shows idle here (the IOP starts the read at submission) -> report busy.
                    state = 2u;
                    static std::atomic<uint32_t> s_b{0};
                    const uint32_t k = s_b.fetch_add(1u);
                    if (k < 8u || (k % 200u) == 0u)
                        std::fprintf(stderr, "[cdstate] #%u dev=0x%x idle before request 0x%x was started (stream 0x%x); reporting BUSY, not done\n", k, handle, pendNow, stream);
                }
                else if (waiting && s_edgeFix)
                {
                    state = 3u;
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t k = s_n.fetch_add(1u);
                    if (k < 8u || (k % 200u) == 0u)
                        std::fprintf(stderr, "[cdstate] #%u dev=0x%x reported idle while stream 0x%x"
                                             " still waits on it; reporting 3 instead\n", k, handle, stream);
                }
                else if (waiting)
                {
                    static std::atomic<uint32_t> s_seen{0};
                    const uint32_t k = s_seen.fetch_add(1u);
                    if (k < 8u)
                        std::fprintf(stderr, "[cdstate] WEDGE CONDITION dev=0x%x stream=0x%x "
                                             "(substitution disabled -- expect a stall)\n", handle, stream);
                }
            }
        }

        setReturnU32(ctx, state);
    }

    // Run the CD file-server tick (FUN_0028a3b0) inline on the calling guest thread.
    // [dispatchpump] when the CD server last ran (any path); the dispatch-loop pump only fires when this is stale
    static std::atomic<int64_t> g_lastCdTickNs{0};
    extern "C" bool ps2xCdTickStale(unsigned ms)
    {
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t last = g_lastCdTickNs.load(std::memory_order_relaxed);
        return last == 0 || (now - last) > (int64_t)ms * 1000000LL;
    }
    static void bt3RunCdTickInline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_lastCdTickNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);   // [dispatchpump]
        R5900Context tctx = *ctx;             // inherit gp/sp
        tctx.r[31] = _mm_setzero_si128();     // ra = 0 => run until return
        tctx.pc = 0x0028a3b0u;                // CD file-server tick
        uint32_t steps = 0u;
        while (tctx.pc != 0u && steps++ < 2000000u)
        {
            PS2Runtime::RecompiledFunction step = runtime->lookupFunction(tctx.pc);
            if (!step) break;
            step(rdram, &tctx, runtime);
        }
    }
    // [spinpump] Called by the dispatch loop when a guest thread has re-dispatched at the same pc for
    // thousands of iterations (a busy-poll). The CD file server only advances from the read-poll hook,
    // so a thread polling MEMORY for a load result (the walkers at 0x1149a0 / 0x1134a0 / 0x256e00 with
    // every loader thread blocked) deadlocks: nothing ticks the server, no completion, no populate.
    // Hardware preempts the poller with the IOP completion. Emulate it: tick the server here, then
    // yield the guest token so a woken loader thread can run. PS2X_SPINPUMP=0 disables.
    // [dispatchpump] tick the CD file server WITHOUT sleeping (the dispatch-loop pump runs during fights too)
    extern "C" void ps2xCdTickOnly(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!runtime || !runtime->hasFunction(0x0028a3b0u)) return;
        Bt3CdTickGuard tickGuard;
        if (tickGuard.engaged) { bt3RunCdTickInline(rdram, ctx, runtime); s_bt3CdTicking = false; }
    }
    extern "C" void *ps2xGuestWaitBegin();
    extern "C" void ps2xGuestWaitEnd(void *);
    extern "C" void ps2xSpinPump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_SPINPUMP"); return !(v && v[0] == '0'); }();
        if (!s_on || !runtime || !runtime->hasFunction(0x0028a3b0u)) return;
        {
            Bt3CdTickGuard tickGuard;
            if (tickGuard.engaged) { bt3RunCdTickInline(rdram, ctx, runtime); s_bt3CdTicking = false; }
        }
        static std::atomic<uint32_t> s_n{0};
        const uint32_t k = s_n.fetch_add(1u);
        if (k < 6u || (k % 5000u) == 0u)
            std::fprintf(stderr, "[spinpump] guest thread spinning at pc 0x%x: ticked the CD server + yielded (x%u)\n", ctx->pc, k + 1u);
        void *scope = ps2xGuestWaitBegin();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ps2xGuestWaitEnd(scope);
    }

    PS2Runtime::RecompiledFunction g_orig26b900 = nullptr;
    void bt3AfsStatusPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_0026b900
    {
        const uint32_t handle = getRegU32(ctx, 4); // a0 = adxf partition handle
        // Remember which device this stream drives; the read-state hook only gets the device and
        // needs the stream to see whether a read is still outstanding.
        if (const uint8_t *pdev = getConstMemPtr(rdram, handle + 4u))
        {
            uint32_t dev = 0u;
            std::memcpy(&dev, pdev, sizeof(dev));
            if (dev)
                if (Bt3DevDone *slot = bt3DevSlot(dev))
                    slot->stream.store(handle, std::memory_order_relaxed);
        }
        Bt3CdTickGuard tickGuard;
        bt3NoteCdTickSkipped(!tickGuard.engaged, "afsStatusPoll");
        // [cdedge2] which request is the device working on? Snapshot the pending request and the
        // device read-state around the tick; if the device is busy/done at either end, that request
        // has genuinely been started -- the only case in which "idle" later means "completed".
        // The request-identity snapshot never distinguished requests ([stream+8] is the same buffer) and its
        // two device-state calls (a full R5900Context copy each) run on every poll: OFF by default now.
        static const bool s_edge2Pump = [](){ const char *v = std::getenv("PS2X_CDEDGE2"); return v && v[0] && v[0] != '0'; }();
        uint32_t cdDev = 0u, pendBefore = 0u, stBefore = 1u;
        if (s_edge2Pump)
        { if (const uint8_t *pdev = getConstMemPtr(rdram, handle + 4u)) std::memcpy(&cdDev, pdev, sizeof(cdDev));
          if (const uint8_t *pp = getConstMemPtr(rdram, handle + 8u)) std::memcpy(&pendBefore, pp, sizeof(pendBefore)); }
        auto devState = [&](uint32_t dev) -> uint32_t {
            if (!g_orig270dd0 || dev == 0u) return 1u;
            R5900Context t = *ctx; t.r[4] = _mm_set_epi64x(0, (int64_t)(int32_t)dev); t.r[31] = _mm_setzero_si128(); t.pc = 0x00270dd0u;
            g_orig270dd0(rdram, &t, runtime);
            return getRegU32(&t, 2);
        };
        if (tickGuard.engaged && cdDev) stBefore = devState(cdDev);
        if (tickGuard.engaged && handle != 0u && runtime->hasFunction(0x0028a3b0u))
        {
            bt3RunCdTickInline(rdram, ctx, runtime);
            s_bt3CdTicking = false;
            if (cdDev && pendBefore)
            {
                const uint32_t stAfter = devState(cdDev);
                if (stBefore == 2u || stBefore == 3u || stAfter == 2u || stAfter == 3u)
                    if (Bt3DevDone *slot = bt3DevSlot(cdDev)) slot->activeReq.store(pendBefore, std::memory_order_relaxed);
            }
        }
        if (g_orig26b900)
            g_orig26b900(rdram, ctx, runtime); // returns int8 *(handle+1) (now advanced)
        else
        {
            uint32_t st = 0u;
            if (const uint8_t *p = getMemPtr(rdram, (handle & 0x1FFFFFFFu) + 1u))
                st = (uint32_t)(int32_t)(int8_t)*p;
            setReturnU32(ctx, st);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    // Sound-ready diagnostics (PS2X_SNDPROBE). The fight loader busy-spins in
    // sub_0026CD88 while *(0x2C9FC8)==0; FUN_0026d9a0 (on a sound thread) sets that
    // flag (+5 siblings, stride 0x10) to 1 when sound init completes. These hooks tell
    // us whether the setter ever runs during the stall (sound thread progressing) or
    // never (sound thread blocked on its RPC).
    std::atomic<uint32_t> g_sndReadySetCalls{0};
    PS2Runtime::RecompiledFunction g_orig26d9a0 = nullptr;
    void bt3SoundReadySet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_0026d9a0
    {
        g_sndReadySetCalls.fetch_add(1, std::memory_order_relaxed);
        if (g_orig26d9a0) g_orig26d9a0(rdram, ctx, runtime);
    }
    PS2Runtime::RecompiledFunction g_orig26cd70 = nullptr;
    void bt3SoundSpinCounter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_0026cd70
    {
        if (g_orig26cd70) g_orig26cd70(rdram, ctx, runtime);
        const uint32_t nn = [](){ static std::atomic<uint32_t> n{0}; return n.fetch_add(1); }();
        // FIGHT-LOAD-ONLY sound-ready force (PS2X_FIGHTSNDGATE), for TESTING whether the
        // battle renders once past the sound gate. Unlike the global PS2X_SNDGATE (which
        // breaks BOOT per notes), this fires ONLY when bt3state==0x27 (the fight-load) and
        // only after the spin has clearly stalled -- so boot/menu are never affected. If
        // this reveals the battle rendering, the proper sound-handshake fix follows; if it
        // goes pink, sound genuinely must init first.
        static const bool s_fg = [](){ const char *v=std::getenv("PS2X_FIGHTSNDGATE"); return v&&v[0]&&v[0]!='0'; }();
        if (s_fg && nn > 200000u)
        {
            uint32_t sp = 0, bt3State = 0xffffffffu;
            if (const uint8_t *p = getMemPtr(rdram, 0x2ff10cu)) sp = *reinterpret_cast<const uint32_t*>(p);
            if (sp) { if (const uint8_t *p = getMemPtr(rdram, (sp & 0x1FFFFFFFu) + 0x18u)) bt3State = *reinterpret_cast<const uint32_t*>(p); }
            if (bt3State == 0x27u)
            {
                for (uint32_t a = 0x2c9fc8u; a <= 0x2ca018u; a += 0x10u)
                    if (uint8_t *p = getMemPtr(rdram, a)) *reinterpret_cast<uint64_t*>(p) = 1u;
            }
        }
        static const bool s_probe = [](){ const char *v=std::getenv("PS2X_SNDPROBE"); return v&&v[0]&&v[0]!='0'; }();
        if (s_probe && (nn % 200000u) == 1u)
        {
            uint32_t flag = 0;
            if (const uint8_t *p = getMemPtr(rdram, 0x2C9FC8u)) flag = *reinterpret_cast<const uint32_t*>(p);
            std::cerr << "[sndspin] spins=" << nn << " flag@0x2C9FC8=" << flag
                      << " setterCalls=" << g_sndReadySetCalls.load() << std::endl;
        }
    }

    // Battle-ready wait probe (PS2X_BATTLEPROBE). In the running battle, the main loop
    // sub_0012BBD0 spins `while (func_12AB10()==0)` where func_12AB10 = (*(0x331DC8+0x24)==1).
    // That flag is set by FUN_00128530 when the battle's streaming load-context queue at
    // *(0x2FF11C)+0x20 drains. It's stuck at 0 -> battle never proceeds -> fade stays black.
    // Dump the flag + load-context so we can see which stream never completes.
    PS2Runtime::RecompiledFunction g_orig12ab10 = nullptr;
    void bt3BattleWaitProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_0012ab10
    {
        if (g_orig12ab10) g_orig12ab10(rdram, ctx, runtime);
        auto rd = [&](uint32_t a)->uint32_t{ const uint8_t*p=getMemPtr(rdram,a&0x1FFFFFFFu); return p?*reinterpret_cast<const uint32_t*>(p):0u; };
        static std::atomic<uint32_t> n{0};
        // EXPERIMENT (PS2X_FORCEBATTLE): after a settle window, force the wait to report
        // "ready" so the battle main loop sub_0012BBD0 exits its stream-wait -> tells us if
        // the battle renders (data is there) or falls over (data genuinely missing).
        static const bool s_force = [](){ const char *v=std::getenv("PS2X_FORCEBATTLE"); return v&&v[0]&&v[0]!='0'; }();
        if (s_force && n.load() > 2000u) // ~3s at the observed ~600 calls/sec
        {
            setReturnU32(ctx, 1u);
            ctx->pc = getRegU32(ctx, 31);
        }
        if ((n.fetch_add(1) % 40000u) == 1u)
        {
            const uint32_t flag = rd(0x331DECu);        // 0x331DC8 + 0x24 (battle-ready)
            const uint32_t lc   = rd(0x2FF11Cu);        // load-context base (gp-0x5154)
            const uint32_t sb   = lc + 0x20u;           // FUN_00128530's $s0 struct base
            std::cerr << "[battlewait] n=" << n.load() << " flag@0x331DEC=" << flag
                      << " loadctx=0x" << std::hex << lc
                      << " sb+0x14=0x" << rd(sb+0x14u)
                      << " sb+0x0=0x" << rd(sb) << " sb+0x4=0x" << rd(sb+0x4u)
                      << " sb+0x8=0x" << rd(sb+0x8u) << " sb+0x10=0x" << rd(sb+0x10u)
                      << std::dec << std::endl;
        }
    }

    // Camera view-matrix builder probe (PS2X_CAMPROBE). FUN_001202a0 (found via PCSX2:
    // PC 0x120300 writes the camera view matrix) transposes the camera rotation + builds
    // the -R*T translation via VU0 macro ops, storing to $a0. Log input($a1)+output($a0)
    // to see if it runs, gets a valid input, and produces a valid view matrix or garbage.
    // Camera-CONFIG-setter probe (PS2X_CAMPROBE): do the fight's camera-activation calls run?
    // These setters attach the target fighter + enable tracking (write BASE+0x2C0/0x300). If the
    // fight never calls them, the focus gate stays closed => target/MVP zero => invisible 3D.
    std::atomic<uint32_t> g_bt3CamTarget{0}; // last attached target-object pointer
    std::atomic<uint32_t> g_bt3CamBase{0};   // camera struct base
    struct CamSetterHook { uint32_t addr; const char *name; PS2Runtime::RecompiledFunction orig; };
    CamSetterHook g_camSetters[] = {
        {0x0023d4c0u, "attachTarget(23d4c0)", nullptr},
        {0x0023dce0u, "enable300(23dce0)",    nullptr},
        {0x0023de60u, "setApi(23de60)",       nullptr},
        {0x0023df38u, "setApi(23df38)",       nullptr},
        {0x0023df98u, "setApi(23df98)",       nullptr},
        // upper-level callers of the enable-API (do these run during our battle?)
        {0x00217410u, "caller(217410)",       nullptr},
        {0x002179d0u, "caller(2179d0)",       nullptr},
        {0x00217200u, "caller(217200)",       nullptr},
        {0x00217730u, "caller(217730)",       nullptr},
        {0x001c1b20u, "caller(1c1b20)",       nullptr},
        {0x001c6de0u, "caller(1c6de0)",       nullptr},
    };
    void bt3CamSetterProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc & 0x1FFFFFFFu;
        const uint32_t ra = getRegU32(ctx, 31), a0 = getRegU32(ctx, 4);
        for (auto &h : g_camSetters)
            if (pc == h.addr)
            {
                if (h.addr == 0x0023d4c0u && a0) g_bt3CamTarget.store(a0, std::memory_order_relaxed);
                static std::mutex m; static std::map<uint32_t,uint32_t> seen;
                { std::lock_guard<std::mutex> lk(m); if (seen[h.addr]++ < 4)
                    std::cerr << "[camset] "<<h.name<<" RAN ra=0x"<<std::hex<<ra<<" a0=0x"<<a0<<std::dec<<std::endl; }
                if (h.orig) h.orig(rdram, ctx, runtime);
                return;
            }
    }

    // PS2X_CAMFORCE: force the camera-tracking gate open. FUN_0023d510 skips the focus/target
    // computation unless [BASE+0x300] (a target-object pointer) is non-zero. The fight attaches
    // the target (FUN_0023d4c0 -> g_bt3CamTarget) but never ENABLES tracking (never sets 0x300).
    // Inject the attached target pointer into 0x300 (+0x304) so the gate passes with a VALID
    // pointer; if the camera then computes a non-zero MVP (BASE+0x140), the enable is the fix.
    PS2Runtime::RecompiledFunction g_orig23d510 = nullptr;
    void bt3CamForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_0023d510
    {
        static const bool s_camForce = [](){ const char *v = std::getenv("PS2X_CAMFORCE"); return v && v[0] && v[0] != '0'; }();
        const uint32_t base = g_bt3CamBase.load(std::memory_order_relaxed);
        const uint32_t tgt  = g_bt3CamTarget.load(std::memory_order_relaxed);
        if (s_camForce && base && tgt)
        {
            uint8_t *p300 = getMemPtr(rdram, (base + 0x300u) & 0x1FFFFFFFu);
            uint8_t *p304 = getMemPtr(rdram, (base + 0x304u) & 0x1FFFFFFFu);
            if (p300) { uint32_t cur; std::memcpy(&cur, p300, 4); if (cur == 0u) std::memcpy(p300, &tgt, 4); }
            if (p304) { uint32_t cur; std::memcpy(&cur, p304, 4); if (cur == 0u) std::memcpy(p304, &tgt, 4); }
        }
        // [camround] PS2X_CAMROUND=1: run the camera update under PS2/PCSX2 chop rounding.
        // PALG37 proved CLIP flags bit-faithful; the divergent input = the per-frame matrix
        // values this function produces (host rounding upstream of the TERRROUND scope).
        static const bool s_camRound = [](){ const char *v = std::getenv("PS2X_CAMROUND"); return v && v[0] && v[0] != '0'; }();
        if (s_camRound)
        {
            const unsigned int saved = _mm_getcsr();
            _mm_setcsr((saved & ~0x6000u) | 0x6000u | 0x8040u);
            if (g_orig23d510) g_orig23d510(rdram, ctx, runtime);
            _mm_setcsr(saved);
        }
        else if (g_orig23d510) g_orig23d510(rdram, ctx, runtime);
    }

    // PS2X_CAMENABLE: the camera-tracking enable (func_23DF38) is gated at 0x1c6e30 by
    // func_1DAC78($s0, 0xD8) -- a bitfield test for flag bit 216 on the object. That bit is
    // NOT set in our run (set in PCSX2), so the enable is skipped. Force the lookup to return
    // 1 ONLY at that call site (ra=0x1C6E30) so the game's OWN enable path runs with the real
    // object -> should properly configure the camera + produce a non-zero MVP.
    PS2Runtime::RecompiledFunction g_orig1dac78 = nullptr;
    void bt3CamEnableForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_001DAC78
    {
        const uint32_t ra = getRegU32(ctx, 31);
        if (g_orig1dac78) g_orig1dac78(rdram, ctx, runtime);
        // 0x1C6E4C = the ENABLE gate (flag bit 217/0xD9): its nonzero result calls func_23DF98
        // -> sub_0023DCE0 which SETS BASE+0x300 = target -> opens the tracking gate. (Do NOT
        // force 0x1C6E30/bit-216: that path -> func_23DF38 -> sub_0023DD08 ZEROES 0x300.)
        if (ra == 0x1C6E4Cu) setReturnU32(ctx, 1u);
    }

    // PS2X_CAMPROBE: dump the global active-players table @0x31C640 (func_2499B0 lookup base).
    // PCSX2 has [0]=0x8c02f0,[1]=0x8c1970 (the two fighter ptrs); if ours are zero the fighters
    // were never registered = the true root of the camera-never-enables chain.
    PS2Runtime::RecompiledFunction g_orig2499b0 = nullptr;
    void bt3PlayerTableProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_002499b0
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t idx = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        if ((s_n.fetch_add(1) % 400u) == 1u)
        {
            auto ru=[&](uint32_t a)->uint32_t{ const uint8_t*p=getMemPtr(rdram,a&0x1FFFFFFFu); uint32_t u=0; if(p)std::memcpy(&u,p,4); return u; };
            std::fprintf(stderr, "[ptable] lookup idx=%u ra=0x%x | 0x31C640[0..7]:", idx, ra);
            for (int i=0;i<8;i++) std::fprintf(stderr, " [%d]=0x%x", i, ru(0x31C640u + i*4u));
            std::fprintf(stderr, "\n");
        }
        if (g_orig2499b0) g_orig2499b0(rdram, ctx, runtime);
    }

    // PS2X_DEMO_FIX (default ON): the demo intermittently passes a GARBAGE callback pointer
    // (out-of-code, e.g. 0x20b14780) to the scene-tree register/walk FUN_00231768 ($a3),
    // which then jalr's into nonsense -> crash. Normally the callback is valid (0x1b1400).
    // Sanitize: if $a3 is out of the recompiled code range, zero it so FUN_00231768 takes its
    // existing `beqz $a3 -> skip` path (skip that one tree) instead of crashing.
    PS2Runtime::RecompiledFunction g_orig231768 = nullptr;
    void bt3DemoCallbackFix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00231768
    {
        const uint32_t cb = getRegU32(ctx, 7); // $a3 = callback
        if (cb != 0u && (cb < 0x100008u || cb >= 0x2bf69cu))
        {
            static std::atomic<uint32_t> s_n{0};
            if (s_n.fetch_add(1) < 8)
                std::cerr << "[demofix] sanitized garbage callback 0x" << std::hex << cb << " -> 0 (skip tree)" << std::dec << std::endl;
            ctx->r[7] = _mm_setzero_si128(); // $a3 = 0 -> FUN_00231768 skips
        }
        // Trace who feeds the scene-walk a garbage tree base (e.g. 0x103fa3c = the intro ctx).
        // $a0 = tree base; log it + $ra (caller) whenever the base's child index at +0x18 is
        // implausible (a real tree index is small). Pins the upstream source of the bad pointer.
        {
            const uint32_t base = getRegU32(ctx, 4);   // $a0
            if (base >= 0x100008u && base < 0x2000000u)
            {
                const uint32_t c0 = *reinterpret_cast<uint32_t *>(rdram + ((base + 0x18u) & 0x1FFFFFFu));
                if (c0 > 0x100000u)                    // garbage child index => not a valid tree
                {
                    static std::atomic<uint32_t> s_g{0};
                    if (s_g.fetch_add(1) < 12)
                        std::cerr << "[treesrc] garbage tree base=0x" << std::hex << base
                                  << " child0=0x" << c0 << " caller(ra)=0x" << getRegU32(ctx, 31)
                                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6)
                                  << std::dec << std::endl;
                }
            }
        }
        if (g_orig231768) g_orig231768(rdram, ctx, runtime);
    }

    // PS2X_DEMOPROBE: hook the demo scene-tree walker FUN_002316d0 and dump each node + the
    // callback global (gp-0x56CC). Shows whether the tree POINTER is garbage or the tree DATA
    // is (the latter => unloaded demo assets), and who passes it (ra).
    // Demo scene-tree walker guard (default ON). A cyclic tree makes FUN_002316d0 recurse
    // unbounded -> stack overflow -> the saved $ra gets clobbered -> jr into 0x320000 -> crash.
    // Cap the recursion depth: above the cap, bail (jr $ra) instead of recursing deeper. Legit
    // scene hierarchies are shallow so they never hit it. Depth tunable via PS2X_DEMO_MAXDEPTH.
    // Host-acosf HLE for the game's acosf at 0x28f710 (see the apply block for rationale).
    void bt3Acosf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram; (void)runtime;
        const float in = ctx->f[12];
        const float c = (in > 1.0f) ? 1.0f : ((in < -1.0f) ? -1.0f : (std::isnan(in) ? 1.0f : in));
        ctx->f[0] = std::acos(c);
        ctx->pc = getRegU32(ctx, 31); // jr $ra
    }

    PS2Runtime::RecompiledFunction g_orig2316d0 = nullptr;
    // [thunkwatch] sub_002722C0 = `sd ra,0(sp); ld ra,0(sp); j func_2892F0 (jr ra)`: the reloaded $ra comes back as
    // garbage (0x30d49 / 0x30d71) in ~1 of 5 fight loads -> the loader thread's stack slot is overwritten between
    // two adjacent instructions. Arm the write-watch on exactly that slot for the duration of the thunk so the
    // store hook names the writer (guest pc). PS2X_THUNKWATCH=0 disables.
    // [fixupprobe] FUN_0010a028 = the loader's pointer-fixup loop (bank offsets -> absolute pointers). The loading hang's
    // corrupting store (pc 0x10a074 -> 0x2c9360) came from here with base a1 = 0x02f60500. Log every call's inputs.
    struct FixupRing { uint32_t n, a0, a1, a2, ra, cnt, entOff, sp; };
    FixupRing g_fixupRing[16] = {};
    uint32_t g_fixupRingPos = 0;
    PS2Runtime::RecompiledFunction g_orig10a028 = nullptr;
    void bt3FixupProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), ra = getRegU32(ctx, 31);
        auto r32 = [&](uint32_t a) -> uint32_t { const uint8_t *p = getMemPtr(rdram, a & 0x1FFFFFFFu); uint32_t v = 0; if (p) std::memcpy(&v, p, 4); return v; };
        static std::atomic<uint32_t> s_n{0}; const uint32_t n = s_n.fetch_add(1u);
        {   // [fixupring] keep the LAST 16 calls (the first-40 log never contains the hang's own call);
            // ps2xFixupRingDump() prints them from the [status] line when the game sits at 0 fps.
            static std::mutex s_rm; std::lock_guard<std::mutex> lk(s_rm);
            FixupRing &r = g_fixupRing[g_fixupRingPos++ & 15u];
            r.n = n; r.a0 = a0; r.a1 = a1; r.a2 = a2; r.ra = ra; r.cnt = r32(a2); r.entOff = r32(a2 + 4u); r.sp = getRegU32(ctx, 29);
        }
        if (n < 40u)
            std::fprintf(stderr, "[fixupprobe] #%u a0=0x%x a1=0x%x a2=0x%x ra=0x%x hdr[0..4]=%08x %08x %08x %08x %08x  a1[0..3]=%08x %08x %08x %08x  sp=0x%x\n",
                         n, a0, a1, a2, ra, r32(a2), r32(a2 + 4u), r32(a2 + 8u), r32(a2 + 12u), r32(a2 + 16u), r32(a1), r32(a1 + 4u), r32(a1 + 8u), r32(a1 + 12u), getRegU32(ctx, 29));
        {   // [fixupguard] the hung runs called this with a base 0x37000 past the real pack (stale pointer): the "header"
            // there is sample data, so the entry pointer becomes garbage and the loop writes over the sound stream block
            // 0x2c9350. Refuse a fixup whose header is implausible: count > 1024 or entries outside [a1, a1 + 2 MB).
            static const bool s_guard = [](){ const char *v = std::getenv("PS2X_FIXUPGUARD"); return !(v && v[0] == '0'); }();
            const uint32_t cnt = r32(a2), entOff = r32(a2 + 4u) * 4u;
            const uint32_t entBase = (a1 + entOff) & 0x1FFFFFFFu, a1m = a1 & 0x1FFFFFFFu;
            if (s_guard && (cnt > 1024u || entBase < a1m || entBase - a1m > 0x200000u || (a1 & 0x1FFFFFFFu) >= 0x2000000u))
            {
                std::fprintf(stderr, "[fixupguard] REJECTED fixup #%u: a1=0x%x a2=0x%x count=%u entries@+0x%x (garbage header) -- skipping to protect 0x2c9350\n", n, a1, a2, cnt, entOff);
                return;
            }
        }
        if (g_orig10a028) g_orig10a028(rdram, ctx, runtime);
    }

    // [shadowprobe] PS2X_SHADOWPROBE=1: the game's character-shadow pipeline (Pass 1 silhouette microcode into fbp336 +
    // Pass 2 floor decal) never runs its silhouette pass under our runtime (no triangle ever hits fbp336, the shadow
    // microcode pieces at 0x2c2d30/0x2c3080/0x2c3380 never upload). Static chain: sub_00115290 creates the shadow
    // system ([gp-0x595C] = ctx, gated on [[gp-0x5154]+0x24]) <- 0x23fc80; 0x115de0 (model draw driver) -> 0x115478
    // (bails when [gp-0x595C]==0 or [[gp-0x5690]+8]&1) -> returns 8 -> 0x115c98 -> sub_001231E0 (silhouette
    // microcode). Log entries + the gates to see which link breaks.
    struct ShadowProbeSlot { uint32_t addr; const char *name; PS2Runtime::RecompiledFunction orig; std::atomic<uint32_t> n; };
    ShadowProbeSlot g_shProbe[] = {
        { 0x00115290u, "create(115290)", nullptr, {0} }, { 0x00115370u, "destroy(115370)", nullptr, {0} },
        { 0x0023fc80u, "creator-caller(23fc80)", nullptr, {0} }, { 0x0023fc40u, "23fc40", nullptr, {0} },
        { 0x00115de0u, "drawdriver(115de0)", nullptr, {0} }, { 0x00115478u, "gate(115478)", nullptr, {0} },
        { 0x00115c98u, "silhouette(115c98)", nullptr, {0} }, { 0x00114508u, "114508", nullptr, {0} },
        { 0x0010fd98u, "10fd98", nullptr, {0} }, { 0x001231e0u, "mcode(1231e0)", nullptr, {0} },
        { 0x00123d50u, "mcode(123d50)", nullptr, {0} }, { 0x00123e40u, "mcode(123e40)", nullptr, {0} },
        // functions that form FRAME=0x00040150 (fbp336 fbw4 = the Pass-1 shadow-silhouette target, pcsx2dump draw 1849)
        { 0x00103600u, "frame336(103600)", nullptr, {0} }, { 0x00104060u, "frame336(104060)", nullptr, {0} },
        { 0x00247d98u, "frame336(247d98)", nullptr, {0} }, { 0x00107ee0u, "frame336(107ee0)", nullptr, {0} },
        { 0x00113c08u, "frame336(113c08)", nullptr, {0} }, { 0x00244890u, "frame336(244890)", nullptr, {0} },
    };
    template <int I> void bt3ShadowProbeFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ShadowProbeSlot &sl = g_shProbe[I];
        const uint32_t n = sl.n.fetch_add(1u);
        auto r32 = [&](uint32_t a) -> uint32_t { const uint8_t *p = getMemPtr(rdram, a & 0x1FFFFFFFu); uint32_t v = 0; if (p) std::memcpy(&v, p, 4); return v; };
        const uint32_t gp = getRegU32(ctx, 28), a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), ra = getRegU32(ctx, 31);
        const uint32_t shCtx = r32(gp - 0x595Cu), g5690 = r32(gp - 0x5690u), g5154 = r32(gp - 0x5154u);
        const bool say = n < 6u || (n % 2000u) == 0u;
        if (say)
            std::fprintf(stderr, "[shadowprobe] %s #%u a0=0x%x a1=0x%x a2=0x%x ra=0x%x | shCtx[gp-595C]=0x%x  [gp-5690]=0x%x +8=0x%x  [gp-5154]=0x%x +24=0x%x (+24/+28=0x%x/0x%x)\n",
                         sl.name, n, a0, a1, a2, ra, shCtx, g5690, g5690 ? r32(g5690 + 8u) : 0u, g5154, g5154 ? r32(g5154 + 0x24u) : 0u,
                         (g5154 && r32(g5154 + 0x24u)) ? r32(r32(g5154 + 0x24u) + 0x24u) : 0u, (g5154 && r32(g5154 + 0x24u)) ? r32(r32(g5154 + 0x24u) + 0x28u) : 0u);
        if (sl.orig) sl.orig(rdram, ctx, runtime);
        if (say && (I == 5 || I == 0)) std::fprintf(stderr, "[shadowprobe] %s #%u -> v0=0x%x shCtx=0x%x\n", sl.name, n, getRegU32(ctx, 2), r32(gp - 0x595Cu));
    }
    // [stagegate] PS2X_STAGEGATE=1: does the stage-bank upload dispatcher (sub_00115DE0) run
    // per frame, and do its emitters (116770/116860/116970) ever fire? Logs the obj flag gate
    // [gp-0x5690]+8 bit0 that early-exits the dispatcher. (Terrain-sheet-never-uploaded hunt.)
    struct SgSlot { uint32_t addr; const char *name; PS2Runtime::RecompiledFunction orig; std::atomic<uint32_t> n; };
    SgSlot g_sgProbe[4] = {
        { 0x00115de0u, "dispatch(115de0)", nullptr, {0} }, { 0x00116770u, "emit(116770)", nullptr, {0} },
        { 0x00116860u, "emit(116860)", nullptr, {0} }, { 0x00116970u, "emit(116970)", nullptr, {0} },
    };
    template <int I> void bt3StageGateFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SgSlot &sl = g_sgProbe[I];
        const uint32_t n = sl.n.fetch_add(1u);
        auto r32 = [&](uint32_t a) -> uint32_t { const uint8_t *p = getMemPtr(rdram, a & 0x1FFFFFFFu); uint32_t v = 0; if (p) std::memcpy(&v, p, 4); return v; };
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t g5690 = r32(gp - 0x5690u);
        const bool say = n < 8u || (n % 2000u) == 0u;
        if (say)
            std::fprintf(stderr, "[stagegate] %s #%u fr=%llu a0=0x%x ra=0x%x [gp-5690]=0x%x +8=0x%x +1C=0x%x\n",
                         sl.name, n, (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed),
                         getRegU32(ctx, 4), getRegU32(ctx, 31), g5690,
                         g5690 ? r32(g5690 + 8u) : 0u, g5690 ? r32(g5690 + 0x1Cu) : 0u);
        if (sl.orig) sl.orig(rdram, ctx, runtime);
    }
    // [rollback] PS2X_ROLLBACK=1: hook the DL finalizer sub_00100890(end). It sets the shared
    // cursor [gp-0x59D8] = end; if end < current cursor that is a CURSOR ROLLBACK — the event
    // that lets later passes overwrite the stage-sheet upload. Log the caller (ra).
    PS2Runtime::RecompiledFunction g_rbOrig = nullptr;
    void bt3RollbackProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u);
        const uint32_t gp = getRegU32(ctx, 28), a0 = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        uint32_t cur = 0;
        if (const uint8_t *pp = getMemPtr(rdram, (gp - 0x59D8u) & 0x1FFFFFFFu)) std::memcpy(&cur, pp, 4);
        const bool back = a0 < cur;
        static std::atomic<uint32_t> s_bk{0};
        if (n < 4u || (back && s_bk.fetch_add(1u) < 30u))
            std::fprintf(stderr, "[rollback] #%u fr=%llu end=0x%x cursor=0x%x ra=0x%x%s\n",
                         n, (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed),
                         a0, cur, ra, back ? "  <== ROLLBACK" : "");
        if (g_rbOrig) g_rbOrig(rdram, ctx, runtime);
    }
    // [carousel] PS2X_CAROUSEL=1: hook the DL blob-append func_1006E8(src,size) and log large
    // appends (the block-10752 texture carousel) with src + caller — names the selection site
    // that picks pak+0x417000 (pale) instead of pak+0x406b40 (correct).
    PS2Runtime::RecompiledFunction g_caOrig = nullptr;
    void bt3CarouselProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
        const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
        if (a1 >= 0x4000u && fr >= 1600u)
        {
            static std::atomic<uint32_t> s_n{0};
            if (s_n.fetch_add(1) < 400u)
                std::fprintf(stderr, "[carousel] fr=%llu ref=0x%x len=0x%x ra=0x%x\n",
                             (unsigned long long)fr, a0, a1, ra);
        }
        // [forcerich] PS2X_FORCERICH=1 (A/B): when the REF payload is the PALE texture
        // (content match), redirect the REF to the pak's RICH copy at 0x1446480. Diagnostic:
        // proves the pale slot is the visible washed hillside and the fix direction.
        {
            static const bool s_fr2 = [](){ const char *v = std::getenv("PS2X_FORCERICH"); return v && v[0] && v[0] != '0'; }();
            static const uint8_t s_paleHead[16] = {0xda,0xe0,0xf3,0xdc,0xdc,0xe2,0xf3,0xdc,0xda,0xe7,0xed,0xe2,0xdc,0xe5,0xe5,0xe0};
            if (a0 >= 0xac4300u && a0 < 0xbc4300u && fr >= 4300u)
            {   // FIGHT-TIME truth: stream-range REF appends after binds settle (any length)
                static std::atomic<uint32_t> s_dg{0};
                const uint8_t *dp = getMemPtr(rdram, (a0 + 0x80u) & 0x1FFFFFFFu);
                if (dp && s_dg.fetch_add(1) < 12u)
                    std::fprintf(stderr, "[fr-dbg] fr=%llu a0=0x%x len=0x%x head@+80=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                 (unsigned long long)fr, a0, a1, dp[0],dp[1],dp[2],dp[3],dp[4],dp[5],dp[6],dp[7],
                                 dp[8],dp[9],dp[10],dp[11],dp[12],dp[13],dp[14],dp[15]);
            }
            if (s_fr2 && a1 >= 0x8000u)
            {
                for (uint32_t off = 0x50u; off <= 0xA0u; off += 0x10u)
                {
                    uint8_t *pp = getMemPtr(rdram, (a0 + off) & 0x1FFFFFFFu);
                    if (pp && std::memcmp(pp, s_paleHead, 16) == 0)
                    {
                        const uint8_t *rich = getMemPtr(rdram, 0x1446500u); // pak pale->rich payload swap
                        if (rich)
                        {
                            std::memcpy(pp, rich, 0x10000u);
                            static std::atomic<uint32_t> s_rr{0};
                            if (s_rr.fetch_add(1) < 8u)
                                std::fprintf(stderr, "[forcerich] fr=%llu payload swap at 0x%x+0x%x\n",
                                             (unsigned long long)fr, a0, off);
                        }
                        break;
                    }
                }
            }
        }
        if (g_caOrig) g_caOrig(rdram, ctx, runtime);
    }
    // [tblcen] PS2X_TBLCEN=1: census of sub_0010C520(table, idx, arg) calls — which TABLE
    // (resident 0x135a620-family vs streamed-chunk tables) serves each carousel slot per frame.
    PS2Runtime::RecompiledFunction g_tcOrig = nullptr;
    void bt3TblCenProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
        const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
        if (fr >= 4300u)
        {
            static std::mutex s_mx; static std::set<uint64_t> s_seen; static std::atomic<uint32_t> s_n{0};
            const uint64_t key = ((uint64_t)a0 << 16) | (a1 & 0xFFFFu);
            bool fresh=false;
            { std::lock_guard<std::mutex> lk(s_mx); fresh = s_seen.insert(key).second; }
            if (fresh && s_n.fetch_add(1) < 120u)
                std::fprintf(stderr, "[tblcen] fr=%llu table=0x%x idx=%u ra=0x%x\n",
                             (unsigned long long)fr, a0, a1, ra);
        }
        if (g_tcOrig) g_tcOrig(rdram, ctx, runtime);
    }
    // [inst337] PS2X_INST337=1: hook overlay f_337090 (streamed-chunk INSTALL: decode+bind).
    // Logs each install's task object head — which chunks install, which never do.
    PS2Runtime::RecompiledFunction g_i337Orig = nullptr;
    void bt3Inst337Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u);
        const uint32_t a0 = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        if (n < 80u)
        {
            auto r32 = [&](uint32_t a) -> uint32_t { const uint8_t *pp = getMemPtr(rdram, a & 0x1FFFFFFFu); uint32_t v = 0; if (pp) std::memcpy(&v, pp, 4); return v; };
            std::fprintf(stderr, "[inst337] #%u fr=%llu a0=0x%x ra=0x%x w0=0x%x w4=0x%x w8=0x%x wC=0x%x w10=0x%x\n",
                         n, (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), a0, ra,
                         a0 ? r32(a0) : 0u, a0 ? r32(a0+4) : 0u, a0 ? r32(a0+8) : 0u, a0 ? r32(a0+0xC) : 0u, a0 ? r32(a0+0x10) : 0u);
        }
        if (g_i337Orig) g_i337Orig(rdram, ctx, runtime);
    }
    // [init114] PS2X_INIT114=1: hook stage-init table binder FUN_00114c60 — does it run, with
    // what object, per run? (resident texture table bind is nondeterministic across runs.)
    PS2Runtime::RecompiledFunction g_i114Orig = nullptr;
    void bt3Init114Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u);
        if (n < 20u)
        {
            auto r32 = [&](uint32_t a) -> uint32_t { const uint8_t *pp = getMemPtr(rdram, a & 0x1FFFFFFFu); uint32_t v = 0; if (pp) std::memcpy(&v, pp, 4); return v; };
            const uint32_t a0 = getRegU32(ctx, 4);
            std::fprintf(stderr, "[init114] #%u fr=%llu a0=0x%x ra=0x%x [a0+0x58]=0x%x [a0+0x5C]=0x%x [a0+0x54]=0x%x\n",
                         n, (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed),
                         a0, getRegU32(ctx, 31), a0 ? r32(a0+0x58) : 0u, a0 ? r32(a0+0x5C) : 0u, a0 ? r32(a0+0x54) : 0u);
        }
        if (g_i114Orig) g_i114Orig(rdram, ctx, runtime);
        {   // post-call: did this init bind the texture table? read rec0 ptr directly.
            auto r32 = [&](uint32_t a) -> uint32_t { const uint8_t *pp = getMemPtr(rdram, a & 0x1FFFFFFFu); uint32_t v = 0; if (pp) std::memcpy(&v, pp, 4); return v; };
            std::fprintf(stderr, "[init114] POST rec0ptr[0x135a658]=0x%x rec14=0x%x\n",
                         r32(0x135a658u), r32(0x135a658u + 14u*64u));
        }
    }
    // [force14] PS2X_FORCE14=1 (A/B): at the resident-table record append sub_0010A218,
    // remap record idx 15 (PALE texture state) -> 14 (RICH) — console holds 14 at this camera.
    PS2Runtime::RecompiledFunction g_f14Orig = nullptr;
    void bt3Force14Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
        const uint64_t frF = g_bt3FrameCount.load(std::memory_order_relaxed);
        static std::atomic<uint32_t> s_n{0};
        if (frF >= 4300u)
        {
            const uint32_t n = s_n.fetch_add(1u);
            if (n < 40u)
                std::fprintf(stderr, "[force14] #%u fr=%llu a0=0x%x a1=%u ra=0x%x\n",
                             n, (unsigned long long)frF, a0, a1, getRegU32(ctx, 31));
        }
        if (a0 == 0x135a5c0u && a1 == 15u)
        {
            SET_GPR_U32(ctx, 5, 14u);
            static std::atomic<uint32_t> s_r{0};
            if (s_r.fetch_add(1u) < 6u)
                std::fprintf(stderr, "[force14] remapped idx 15->14 (fr=%llu)\n",
                             (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed));
        }
        if (g_f14Orig) g_f14Orig(rdram, ctx, runtime);
    }
    void bt3StageGateArm(PS2Runtime &runtime)
    {
        PS2Runtime::RecompiledFunction fns[] = { &bt3StageGateFn<0>, &bt3StageGateFn<1>, &bt3StageGateFn<2>, &bt3StageGateFn<3> };
        for (int i = 0; i < 4; ++i)
        {
            g_sgProbe[i].orig = runtime.lookupFunction(g_sgProbe[i].addr);
            if (g_sgProbe[i].orig) runtime.replaceFunction(g_sgProbe[i].addr, fns[i]);
            std::fprintf(stderr, "[stagegate] hook %s %s\n", g_sgProbe[i].name, g_sgProbe[i].orig ? "ok" : "MISSING");
        }
    }
    void bt3ShadowProbeArm(PS2Runtime &runtime)
    {
        PS2Runtime::RecompiledFunction fns[] = { &bt3ShadowProbeFn<0>, &bt3ShadowProbeFn<1>, &bt3ShadowProbeFn<2>, &bt3ShadowProbeFn<3>, &bt3ShadowProbeFn<4>, &bt3ShadowProbeFn<5>,
                                                 &bt3ShadowProbeFn<6>, &bt3ShadowProbeFn<7>, &bt3ShadowProbeFn<8>, &bt3ShadowProbeFn<9>, &bt3ShadowProbeFn<10>, &bt3ShadowProbeFn<11>,
                                                 &bt3ShadowProbeFn<12>, &bt3ShadowProbeFn<13>, &bt3ShadowProbeFn<14>, &bt3ShadowProbeFn<15>, &bt3ShadowProbeFn<16>, &bt3ShadowProbeFn<17> };
        for (int i = 0; i < 18; ++i)
        {
            g_shProbe[i].orig = runtime.lookupFunction(g_shProbe[i].addr);
            if (g_shProbe[i].orig) runtime.replaceFunction(g_shProbe[i].addr, fns[i]);
            std::fprintf(stderr, "[shadowprobe] hook %s %s\n", g_shProbe[i].name, g_shProbe[i].orig ? "ok" : "MISSING");
        }
    }
    void bt3FixupRingDumpImpl()
    {
        static uint32_t s_lastPos = 0;
        if (g_fixupRingPos == s_lastPos) return;   // nothing new since the last status line
        s_lastPos = g_fixupRingPos;
        std::fprintf(stderr, "[fixupring] last %u fixup calls (newest last):\n", g_fixupRingPos < 16u ? g_fixupRingPos : 16u);
        const uint32_t start = g_fixupRingPos >= 16u ? g_fixupRingPos - 16u : 0u;
        for (uint32_t i = start; i < g_fixupRingPos; ++i)
        {
            const FixupRing &r = g_fixupRing[i & 15u];
            std::fprintf(stderr, "[fixupring]   #%u a0=0x%x a1=0x%x a2=0x%x ra=0x%x count=%u entOff=0x%x sp=0x%x\n", r.n, r.a0, r.a1, r.a2, r.ra, r.cnt, r.entOff * 4u, r.sp);
        }
    }
    extern "C" void ps2xFixupRingDump() { bt3FixupRingDumpImpl(); }
    PS2Runtime::RecompiledFunction g_orig2722c0 = nullptr;
    // [vstep] PS2X_VSTEP=<n>: override the fight loop's hard-coded frame step (0x12bce4 passes a0=2 to
    // func_102060 -> func_23D160 (30 Hz counter cadence) and func_264D98 (wait until the per-frame vblank
    // counter reaches a0)). n=1 = render every vblank. [logicrate] counts func_115950 (the per-frame fight
    // update) per second so a step-1 run can be checked for double-speed logic.
    PS2Runtime::RecompiledFunction g_orig102060 = nullptr, g_orig115950 = nullptr;
    void bt3VStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const int s_step = [](){ const char *v = std::getenv("PS2X_VSTEP"); return v && v[0] ? std::atoi(v) : 0; }();
        if (s_step > 0 && getRegU32(ctx, 4) == 2u) ctx->r[4] = _mm_set_epi64x(0, (int64_t)s_step);   // $a0 = step
        if (g_orig102060) g_orig102060(rdram, ctx, runtime);
    }
    // [vstepprobe] func_264D98(a0): the frame wait. Print a0, the per-frame vblank counter [gp-0x5148] at entry,
    // and the vsync ticks elapsed inside the call, for the first calls and then every 300th.
    PS2Runtime::RecompiledFunction g_orig264d98 = nullptr;
    void bt3WaitProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u);
        const uint32_t gp = getRegU32(ctx, 28);
        uint32_t cnt = 0; { const uint8_t *p = getMemPtr(rdram, (gp - 0x5148u) & 0x1FFFFFFFu); if (p) std::memcpy(&cnt, p, 4); }
        const uint32_t a0 = getRegU32(ctx, 4);
        const uint64_t t0 = ps2_syscalls::GetCurrentVSyncTick();
        if (g_orig264d98) g_orig264d98(rdram, ctx, runtime);
        const uint64_t t1 = ps2_syscalls::GetCurrentVSyncTick();
        if (n < 24u || (n % 300u) == 0u)
            std::fprintf(stderr, "[vstepprobe] #%u a0=%u counter@entry=%u ticks_in_wait=%llu\n", n, a0, cnt, (unsigned long long)(t1 - t0));
    }
    void bt3LogicRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> s_n{0};
        static auto s_t0 = std::chrono::steady_clock::now();
        const uint32_t n = s_n.fetch_add(1u) + 1u;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - s_t0).count();
        if (dt >= 5.0) { std::fprintf(stderr, "[logicrate] %.1f fight updates/s (%u in %.1f s)\n", (double)n / dt, n, dt); s_n.store(0u); s_t0 = now; }
        if (g_orig115950) g_orig115950(rdram, ctx, runtime);
    }
    // [vf3probe] PS2X_VF3PROBE=1: print the persistent VU0 basis rows (vf1-vf3) as seen by
    // the DL emitter FUN_00111358 — hardware shares ONE physical VU0 across threads; our
    // per-thread contexts zero-init all but vf0. vf3.x==0 here breaks func_121E50's
    // normalize (length drops z) => slope-dependent band misassignment.
    PS2Runtime::RecompiledFunction g_orig111358 = nullptr;
    void bt3Vf3Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<int> s_n{0};
        if (s_n.fetch_add(1) < 24)
        {
            float b[12];
            std::memcpy(&b[0], &ctx->vu0_vf[1], 16);
            std::memcpy(&b[4], &ctx->vu0_vf[2], 16);
            std::memcpy(&b[8], &ctx->vu0_vf[3], 16);
            std::fprintf(stderr, "[vf3probe] tid=%zx vf1=(%g %g %g %g) vf2=(%g %g %g %g) vf3=(%g %g %g %g)\n",
                         std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFu,
                         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
        }
        if (g_orig111358) g_orig111358(rdram, ctx, runtime);
    }
    // [slotprobe] PS2X_SLOTPROBE=1: histogram of FUN_0024f860 returns (terrain constant-slot
    // selector; -1 = no valid streamed record => caller falls back / stale constants).
    // [pakcpy] PS2X_PAKCPY=1: log memcpy (func_2A9A1C) calls whose SOURCE lies inside the
    // bulk-loaded stage pak at 0x103f9c0 (+6.2MB) — src pak-offset + ra = the walker call
    // site computing the (wrong) intra-pak offsets for the terrain band sheet.
    // [sprq] PS2X_SPRQ=1: log the DMA queue-driver FUN_002bb098 calls whose args reference
    // the resident stage pak (0x103f9c0+6.2MB) — the queuer's ra = who computes the offsets.
    // [wlk] PS2X_WLK=1: hook the pak walker f_399b18 (OVERLAY function — replaceFunction
    // rejects overlay addresses, so patch g_ps2OverlayFunctionTable[slot] directly).
    // [upb] PS2X_UPB=1: hook the terrain upload-builder family — log entry args + ra.
    PS2Runtime::RecompiledFunction g_orig13c300 = nullptr, g_orig13c638 = nullptr, g_orig13ca80 = nullptr;
    static void upbLog(const char *tag, R5900Context *ctx)
    {
        static std::atomic<int> s_u{0};
        if (s_u.fetch_add(1) < 48)
            std::fprintf(stderr, "[upb] %s a0=0x%x a1=0x%x a2=0x%x a3=0x%x t0=0x%x ra=0x%x\n",
                         tag, getRegU32(ctx,4), getRegU32(ctx,5), getRegU32(ctx,6), getRegU32(ctx,7),
                         getRegU32(ctx,8), getRegU32(ctx,31));
    }
    void bt3Upb13c300(uint8_t *r, R5900Context *c, PS2Runtime *rt) { upbLog("13c300", c); if (g_orig13c300) g_orig13c300(r, c, rt); }
    void bt3Upb13c638(uint8_t *r, R5900Context *c, PS2Runtime *rt) { upbLog("13c638", c); if (g_orig13c638) g_orig13c638(r, c, rt); }
    void bt3Upb13ca80(uint8_t *r, R5900Context *c, PS2Runtime *rt) { upbLog("13ca80", c); if (g_orig13ca80) g_orig13ca80(r, c, rt); }
    PS2Runtime::RecompiledFunction g_origWalker = nullptr;
    void bt3WalkerProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Log entries only (pc==0x399b18); continuation dispatches re-enter mid-function.
        if (ctx->pc == 0x399b18u)
        {
            static std::atomic<int> s_w{0};
            if (s_w.fetch_add(1) < 40)
                std::fprintf(stderr, "[wlk] a0=0x%x a1=0x%x a2=0x%x a3=0x%x ra=0x%x\n",
                             getRegU32(ctx,4), getRegU32(ctx,5), getRegU32(ctx,6), getRegU32(ctx,7),
                             getRegU32(ctx,31));
        }
        if (g_origWalker) g_origWalker(rdram, ctx, runtime);
    }
    PS2Runtime::RecompiledFunction g_orig2bb098 = nullptr;
    void bt3SprQProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a[4] = { getRegU32(ctx,4)&0x1FFFFFFFu, getRegU32(ctx,5)&0x1FFFFFFFu,
                                getRegU32(ctx,6)&0x1FFFFFFFu, getRegU32(ctx,7)&0x1FFFFFFFu };
        bool pak = false;
        for (int i = 0; i < 4; ++i) if (a[i] >= 0x103f9c0u && a[i] < 0x1631140u) pak = true;
        if (pak)
        {
            static std::atomic<int> s_q{0};
            if (s_q.fetch_add(1) < 60)
                std::fprintf(stderr, "[sprq] a0=0x%x a1=0x%x a2=0x%x a3=0x%x ra=0x%x\n",
                             a[0], a[1], a[2], a[3], getRegU32(ctx, 31));
        }
        if (g_orig2bb098) g_orig2bb098(rdram, ctx, runtime);
    }
    PS2Runtime::RecompiledFunction g_orig2a9a1c = nullptr;
    void bt3PakCpyProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dst = getRegU32(ctx, 4) & 0x1FFFFFFFu;
        const uint32_t src = getRegU32(ctx, 5) & 0x1FFFFFFFu;
        const uint32_t n   = getRegU32(ctx, 6);
        const uint32_t ra  = getRegU32(ctx, 31);
        if (src >= 0x103f9c0u && src < 0x103f9c0u + 0x5f1780u && n >= 1024u)
        {
            static std::atomic<int> s_pn{0};
            if (s_pn.fetch_add(1) < 60)
                std::fprintf(stderr, "[pakcpy] src=0x%x (pak+0x%x) dst=0x%x n=%u ra=0x%x\n",
                             src, src - 0x103f9c0u, dst, n, ra);
        }
        if (g_orig2a9a1c) g_orig2a9a1c(rdram, ctx, runtime);
    }
    PS2Runtime::RecompiledFunction g_orig24f860 = nullptr;
    thread_local uint32_t g_slotProbeObj = 0;
    void bt3SlotProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_slotProbeObj = getRegU32(ctx, 4) & 0x1FFFFFFFu;
        if (g_orig24f860) g_orig24f860(rdram, ctx, runtime);
        const int32_t r = (int32_t)getRegU32(ctx, 2);
        // why-analysis: walk the chain the selector walked (a0 preserved? a0 may be clobbered — use s-reg? read from entry
        // instead: the wrapper runs AFTER orig, a0 might be stale; capture BEFORE the call would be better, but a0 is
        // callee-preserved-enough here in practice: FUN_0024f860 keeps obj in v1. Use the captured entry value.)
        static std::mutex s_m; static std::map<int32_t, uint32_t> s_h; static std::map<int,uint32_t> s_why; static std::atomic<uint32_t> s_n{0};
        std::lock_guard<std::mutex> lk(s_m);
        ++s_h[r];
        if (r < 0)
        {
            int why = -9; uint32_t rec = 0, idx = 0, pay = 0;
            const uint32_t obj = g_slotProbeObj;
            if (obj)
            {
                const uint8_t *po = getMemPtr(rdram, obj);
                if (po) std::memcpy(&rec, po + 0x1664, 4);
                if (!rec) why = 0;                        // record chain empty
                else
                {
                    const uint8_t *pr = getMemPtr(rdram, rec & 0x1FFFFFFFu);
                    if (pr) std::memcpy(&idx, pr + 0x1C, 4);
                    if (idx == 0 || idx > 100u) why = 1;  // index invalid
                    else
                    {
                        const uint8_t *pp = getMemPtr(rdram, obj + 0x18u + (idx - 1u) * 4u);
                        if (pp) std::memcpy(&pay, pp + 0x44, 4);
                        why = pay ? 3 : 2;                // 2 = payload null, 3 = ??? (should have succeeded)
                    }
                }
            }
            ++s_why[why];
        }
        const uint32_t n = s_n.fetch_add(1u) + 1u;
        if ((n % 2000u) == 1u)
        {
            std::string line = "[slotprobe] n=" + std::to_string(n) + " hist:";
            for (auto &kv : s_h) line += " " + std::to_string(kv.first) + "x" + std::to_string(kv.second);
            line += " why:";
            for (auto &kv : s_why) line += " w" + std::to_string(kv.first) + "x" + std::to_string(kv.second);
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    }
    PS2Runtime::RecompiledFunction g_orig2188b8 = nullptr;
    void bt3TerrRoundScope(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // [terrround] PS2X_TERRROUND=1: run the terrain lighting-group matrix builder
        // (sub_002188B8 and everything it calls: 120308 rotZ ACC chains, 120C40 concat,
        // 11FFE8 sincos) under PS2/PCSX2 chop rounding (RZ+FTZ+DAZ). Surgical scope of
        // the [eeround] finding: the classifier's last-bit math decides lighting-group
        // membership at quantization boundaries; host round-nearest flips boundary
        // chunks per frame (the moving dark/light terrain patches). Restores MXCSR on
        // exit so nothing else in the frame is affected.
        // [nodecb] PS2X_NODECB=1: census of scene-graph node draw callbacks ([node+0x34],
        // consumed by jalr at 0x2189c0) — the band choice lives in these, not in 2188B8 itself.
        static const bool s_cb = [](){ const char *v = std::getenv("PS2X_NODECB"); return v && v[0] && v[0] != '0'; }();
        if (s_cb)
        {
            const uint32_t node = getRegU32(ctx, 4) & 0x1FFFFFFFu;
            uint32_t cb = 0, flags = 0, ang = 0;
            if (const uint8_t *pn = getMemPtr(rdram, node))
            {
                std::memcpy(&flags, pn + 0x00, 4);
                std::memcpy(&ang,   pn + 0x04, 4);
                std::memcpy(&cb,    pn + 0x34, 4);
            }
            static std::mutex s_m; static std::map<uint32_t, uint32_t> s_seen; static std::atomic<int> s_pr{0};
            std::lock_guard<std::mutex> lk(s_m);
            if (++s_seen[cb] == 1 && s_pr.fetch_add(1) < 40)
            {
                float af; std::memcpy(&af, &ang, 4);
                std::fprintf(stderr, "[nodecb] NEW cb=0x%06x node=0x%06x flags=0x%x ang=%.4g (unique=%zu)\n",
                             cb, node, flags, af, s_seen.size());
            }
        }
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_TERRROUND"); return v && v[0] && v[0] != '0'; }();
        if (!s_on) { if (g_orig2188b8) g_orig2188b8(rdram, ctx, runtime); return; }
        static std::atomic<int> s_engaged{0};
        if (s_engaged.fetch_add(1) == 0) std::fprintf(stderr, "[terrround] ENGAGED (chop rounding scoped to the terrain group classifier)\n");
        const unsigned int saved = _mm_getcsr();
        _mm_setcsr((saved & ~0x6000u) | 0x6000u | 0x8040u);
        if (g_orig2188b8) g_orig2188b8(rdram, ctx, runtime);
        _mm_setcsr(saved);
    }
    void bt3ThunkStackWatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // [watchgate] PS2X_THUNKWATCH=1 restores the stack write-watch around this thunk.
        // Default OFF: the unconditional arm/disarm stole the global watch from every other
        // probe (palsrc-geo flapped 32x/fight) and flooded ps2WatchReport with 113k lines.
        static const bool s_tw = [](){ const char *v = std::getenv("PS2X_THUNKWATCH"); return v && v[0] && v[0] != '0'; }();
        if (s_tw)
        {
            const uint32_t sp = getRegU32(ctx, 29) & 0x1FFFFFFFu;
            const uint32_t lo = (sp - 16u) & 0x1FFFFFFFu;
            g_ps2WatchHi.store(sp, std::memory_order_relaxed);
            g_ps2WatchLo.store(lo, std::memory_order_relaxed);
        }
        if (g_orig2722c0) g_orig2722c0(rdram, ctx, runtime);
        if (s_tw) g_ps2WatchLo.store(0u, std::memory_order_relaxed);
    }
    void bt3DemoWalkGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_002316d0
    {
        // Arm the write-watch on this tree-walk's STACK region (once) to catch the stray write
        // that corrupts FUN_002316d0's saved $ra (at $sp+0x18) with 0x2c9f80. ps2WatchReport
        // is garbage-filtered so only the out-of-code (0x2c9f80) write is reported, with its pc.
        // [watchgate] PS2X_DEMOSTACKWATCH=1 arms it; default OFF since 2026-08-28: the armed range stayed
        // live into fights and every store into it went through ps2WatchReport's mutex (2.4% of the guest thread).
        static const bool s_dsw = [](){ const char *v = std::getenv("PS2X_DEMOSTACKWATCH"); return v && v[0] && v[0] != '0'; }();
        if (s_dsw && g_ps2WatchLo.load(std::memory_order_relaxed) == 0u)
        {
            const uint32_t sp = getRegU32(ctx, 29) & 0x1FFFFFFFu;
            g_ps2WatchHi.store(sp + 0x800u, std::memory_order_relaxed);
            g_ps2WatchLo.store((sp - 0x3000u) & 0x1FFFFFFFu, std::memory_order_relaxed);
            std::cerr << "[demostackwatch] armed 0x" << std::hex << (sp - 0x3000u) << "..0x" << (sp + 0x800u) << std::dec << std::endl;
        }
        // Bad-tree-base guard (the real demo-crash fix): one demo object (returned by func_1B16F0,
        // walked via sub_001B3440->sub_001B1708) has a stale tree field pointing at the intro ctx
        // (~0x103fa40) instead of a real scene tree (~0x14-0x15MB). Walking that garbage recurses
        // forever -> guest-stack overflow -> corrupt-$ra crash. Skip the walk at the TOP entry,
        // BEFORE any recursion, so no state is corrupted and the demo proceeds to the next object.
        // Valid scene trees live in the demo scene heap (>= 0x1200000); the stale intro-ctx pointer
        // is far below it. This is the object-level skip; the deep $sp-cap in FUN_002316d0 remains a
        // backstop for any other cyclic path.
        {
            const uint32_t base = getRegU32(ctx, 4);
            if (base < 0x1200000u || base >= 0x2000000u)
            {
                static std::atomic<uint32_t> s_sk{0};
                if (s_sk.fetch_add(1) < 8)
                    std::cerr << "[demoskip] skipping walk of invalid scene-tree base=0x" << std::hex
                              << base << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
                ctx->pc = getRegU32(ctx, 31); // jr $ra: skip this object's walk entirely
                return;
            }
        }
        static const int s_maxDepth = [](){ const char *v = std::getenv("PS2X_DEMO_MAXDEPTH"); int d = v && v[0] ? std::atoi(v) : 256; return d > 0 ? d : 256; }();
        static thread_local int s_depth = 0;
        if (s_depth >= s_maxDepth)
        {
            static std::atomic<uint32_t> s_b{0};
            if (s_b.fetch_add(1) < 6)
                std::cerr << "[demoguard] recursion depth >= " << s_maxDepth << " -> bail (cyclic tree) ra=0x" << std::hex << getRegU32(ctx, 31) << std::dec << std::endl;
            ctx->pc = getRegU32(ctx, 31); // jr $ra: return to caller without recursing further
            return;
        }
        static const bool s_probe = [](){ const char *v = std::getenv("PS2X_DEMOPROBE"); return v && v[0] && v[0] != '0'; }();
        if (s_probe)
        {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t n = s_n.fetch_add(1);
            if (n < 16)
            {
                const uint32_t a0 = getRegU32(ctx, 4), s0 = getRegU32(ctx, 16);
                const uint32_t ra = getRegU32(ctx, 31), gp = getRegU32(ctx, 28);
                auto ru=[&](uint32_t a)->uint32_t{ const uint8_t*p=getMemPtr(rdram,a&0x1FFFFFFFu); uint32_t u=0; if(p)std::memcpy(&u,p,4); return u; };
                std::fprintf(stderr, "[demowalk] #%u depth=%d node=0x%x s0=0x%x ra=0x%x callback=0x%x | node[0..3]: %08x %08x %08x %08x\n",
                             n, s_depth, a0, s0, ra, ru(gp - 0x56CCu), ru(a0), ru(a0+4), ru(a0+8), ru(a0+0xC));
            }
        }
        ++s_depth;
        if (g_orig2316d0) g_orig2316d0(rdram, ctx, runtime);
        --s_depth;
    }

    // Probe sub_001B1708(object=$a1): logs the object pointer + its tree field [obj+4] for each
    // call. If on the crash frame the object POINTER ($a1) is a new/wrong value, the object LIST
    // upstream is corrupt; if $a1 is stable but [obj+4] flips to 0x103fa3c, the FIELD is being
    // clobbered (by DMA/memset, which the value-watch can't see). Pins which of the two it is.
    PS2Runtime::RecompiledFunction g_orig1b1708 = nullptr;
    void bt3ObjProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_001B1708
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t obj = getRegU32(ctx, 5); // $a1 = object
        const uint32_t gp  = getRegU32(ctx, 28);
        auto ru = [&](uint32_t a) -> uint32_t {
            if (a < 0x100008u || a >= 0x2000000u) return 0xDEADu;
            return *reinterpret_cast<uint32_t *>(rdram + (a & 0x1FFFFFFu));
        };
        const uint32_t tree = ru(obj + 4u);
        const bool bad = (tree < 0x1400000u) || (tree >= 0x2000000u);
        if (bad || s_n.load() < 14)
        {
            if (s_n.fetch_add(1) < 60)
                std::fprintf(stderr, "[objprobe] obj=0x%x ra=0x%x flag[gp-0x50B8]=0x%x | +0=%08x +4=%08x(tree) +8=%08x +c=%08x +10=%08x +14=%08x +18=%08x +1c=%08x%s\n",
                             obj, getRegU32(ctx, 31), ru(gp - 0x50B8u),
                             ru(obj+0), ru(obj+4), ru(obj+8), ru(obj+0xC), ru(obj+0x10), ru(obj+0x14), ru(obj+0x18), ru(obj+0x1C),
                             bad ? "  <== BAD TREE" : "");
        }
        if (g_orig1b1708) g_orig1b1708(rdram, ctx, runtime);
    }

    // The demo scene-tree recursion FUN_002316d0 <-> FUN_00231590 <-> sub_00231148 is CYCLIC
    // (freezes or overflows the stack -> corrupt $ra crash). FUN_002316d0 is re-entered at
    // interior addresses so an entry-hook there misses it, but FUN_00231590 IS entered at its
    // real entry (0x231590) each recursion level -> cap the depth here to break the cycle.
    PS2Runtime::RecompiledFunction g_orig231590 = nullptr;
    void bt3DemoRecursionGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00231590
    {
        static const int s_max = [](){ const char *v = std::getenv("PS2X_DEMO_MAXDEPTH"); int d = v && v[0] ? std::atoi(v) : 200; return d > 0 ? d : 200; }();
        static thread_local int s_depth = 0;
        if (s_depth >= s_max)
        {
            static std::atomic<uint32_t> s_b{0};
            if (s_b.fetch_add(1) < 6)
                std::cerr << "[demoguard590] recursion depth >= " << s_max << " -> bail (cyclic tree) ra=0x" << std::hex << getRegU32(ctx, 31) << std::dec << std::endl;
            ctx->pc = getRegU32(ctx, 31); // jr $ra
            return;
        }
        ++s_depth;
        if (g_orig231590) g_orig231590(rdram, ctx, runtime);
        --s_depth;
    }

    PS2Runtime::RecompiledFunction g_orig1202a0 = nullptr;
    void bt3CamMatrixProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_001202a0
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
        const uint32_t ra = getRegU32(ctx, 31); // caller PC (return addr) = the camera-setup fn
        auto rf=[&](uint32_t p,int i)->float{ const uint8_t*q=getMemPtr(rdram,(p+ (uint32_t)i*4)&0x1FFFFFFFu); float f=0; if(q)std::memcpy(&f,q,4); return f; };
        // Classify the INPUT rotation block (rows 0,1,2 * cols x,y,z = a1[0,1,2, 4,5,6, 8,9,10]).
        bool rotZero = true;
        for (int idx : {0,1,2, 4,5,6, 8,9,10}) if (rf(a1, idx) != 0.0f) { rotZero = false; break; }
        if (g_orig1202a0) g_orig1202a0(rdram, ctx, runtime);
        // Log the distinct callers separately for zero-rotation vs valid-rotation inputs, so ONE
        // run reveals: who builds this camera matrix, and whether it's EVER given a valid rotation.
        static std::mutex s_m; static std::map<uint32_t,uint32_t> s_zeroCallers, s_okCallers;
        static std::atomic<uint32_t> s_n{0};
        {
            std::lock_guard<std::mutex> lk(s_m);
            (rotZero ? s_zeroCallers : s_okCallers)[ra]++;
        }
        const uint32_t n = s_n.fetch_add(1);
        if ((n % 120u) == 1u)
        {
            std::lock_guard<std::mutex> lk(s_m);
            std::cerr << "[cammtx] call#"<<n<<" ra=0x"<<std::hex<<ra<<" a1(in)=0x"<<a1<<std::dec
                      << " rot="<<(rotZero?"ZERO":"ok");
            std::cerr << " IN:"; for(int i=0;i<16;i++) std::cerr<<(i%4?",":" ")<<rf(a1,i);
            std::cerr << std::endl;
            std::cerr << "  [cammtx-callers] ZERO-rot from:"; for (auto &kv : s_zeroCallers) std::cerr<<" 0x"<<std::hex<<kv.first<<"(x"<<std::dec<<kv.second<<")";
            std::cerr << " | OK-rot from:"; for (auto &kv : s_okCallers) std::cerr<<" 0x"<<std::hex<<kv.first<<"(x"<<std::dec<<kv.second<<")";
            std::cerr << std::endl;
        }
    }

    // PS2X_CAMPROBE: dump the VU0 base-rotation registers vf1,vf2,vf3 at func_120A98 entry. That fn
    // copies vf3->vf16, vf2->vf17, vf1->vf18 (the rotation matrix rows) which then get stored to the
    // object's +0x960 orientation matrix. If vf1-3 are ZERO here, the CALLER passed a zero base
    // rotation -> the whole fight collapses. Reveals whether the root is the VU0 input (caller) vs math.
    PS2Runtime::RecompiledFunction g_orig120a98 = nullptr;
    void bt3RotBaseProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // func_120A98
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1);
        if ((n % 200u) == 1u)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            auto vf=[&](int r,int c)->float{ alignas(16) float f[4]; _mm_store_ps(f, ctx->vu0_vf[r]); return f[c]; };
            std::fprintf(stderr, "[rotbase] call#%u ra=0x%x | vf0=(%.3f,%.3f,%.3f,%.3f) vf1=(%.3f,%.3f,%.3f,%.3f) vf2=(%.3f,%.3f,%.3f,%.3f) vf3=(%.3f,%.3f,%.3f,%.3f)\n",
                         n, ra, vf(0,0),vf(0,1),vf(0,2),vf(0,3), vf(1,0),vf(1,1),vf(1,2),vf(1,3), vf(2,0),vf(2,1),vf(2,2),vf(2,3), vf(3,0),vf(3,1),vf(3,2),vf(3,3));
        }
        if (g_orig120a98) g_orig120a98(rdram, ctx, runtime);
    }

    // PS2X_CAMPROBE: dump the OBJECT struct passed to sub_0024E2B0 ($a0). Shows which regions are
    // populated (position/angle) vs zero (the local rotation matrix that should feed vf1-3). Reveals
    // whether the fighter's orientation is uninitialized (never set to identity) = the true root.
    PS2Runtime::RecompiledFunction g_orig24e2b0 = nullptr;
    void bt3E2B0Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_0024E2B0
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1);
        if ((n % 300u) == 1u)
        {
            const uint32_t a0 = getRegU32(ctx, 4);
            auto rf=[&](uint32_t off,int i)->float{ const uint8_t*q=getMemPtr(rdram,(a0+off+(uint32_t)i*4)&0x1FFFFFFFu); float f=0; if(q)std::memcpy(&f,q,4); return f; };
            std::fprintf(stderr, "[objdump] a0=0x%x nonzero 16B rows in [0..0xA80]:\n", a0);
            for (uint32_t off=0; off<0xA80; off+=16) {
                float v0=rf(off,0),v1=rf(off,1),v2=rf(off,2),v3=rf(off,3);
                if (v0||v1||v2||v3) std::fprintf(stderr, "  +0x%03x: %10.3f %10.3f %10.3f %10.3f\n", off, v0,v1,v2,v3);
            }
        }
        if (g_orig24e2b0) g_orig24e2b0(rdram, ctx, runtime);
    }

    // PS2X_HUDCALLER: hook the 2D sprite packet builder FUN_00109508. HUD sprites collapse to
    // screen (0,0) = zero-extent; their corner coords arrive zero. Log the CALLER ($ra) + args +
    // the sprite descriptor ($a1 points at it) so we can find the HUD layout code passing zeros.
    PS2Runtime::RecompiledFunction g_orig109508 = nullptr; // now points at FUN_00218848 (HUD vtable dispatcher)
    void bt3SpriteProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00218848: obj=$a0, calls [obj+0x28]
    {
        const uint32_t a0 = getRegU32(ctx, 4);
        auto rd = [&](uint32_t addr) -> uint32_t { const uint8_t *q = getMemPtr(rdram, addr & 0x1FFFFFFFu); uint32_t v=0; if(q) std::memcpy(&v,q,4); return v; };
        const uint32_t m30 = rd((a0 + 0x30u) & 0x1FFFFFFFu);
        const uint32_t method = m30 ? m30 : rd((a0 + 0x28u) & 0x1FFFFFFFu); // dispatcher uses +0x30 else +0x28
        // Log the object list: highlight health-bar objects (method == FUN_00227468 = 0x227468).
        const bool isHB = (method == 0x227468u);
        static std::atomic<uint32_t> s_d{0};
        const uint32_t d = s_d.fetch_add(1);
        if (isHB || (d % 4096u) == 1u) {
            std::fprintf(stderr, "[objdisp]%s #%u ra=0x%x obj=0x%x method=0x%x | obj[0]=0x%x [4]=0x%x [8]=0x%x [0x18]=0x%x [0x24]=0x%x\n",
                         isHB ? " <HEALTHBAR>" : "", d, getRegU32(ctx,31), a0, method,
                         rd(a0), rd(a0+4), rd(a0+8), rd(a0+0x18), rd(a0+0x24));
        }
        if (g_orig109508) g_orig109508(rdram, ctx, runtime);
        return;
    }
    void bt3SpriteProbe_unused(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00227468
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1);
        const uint32_t a0 = getRegU32(ctx, 4);
        const uint32_t ra = getRegU32(ctx, 31);
        const uint32_t gp = getRegU32(ctx, 28);
        auto rd = [&](uint32_t addr) -> uint32_t { const uint8_t *q = getMemPtr(rdram, addr & 0x1FFFFFFFu); uint32_t v=0; if(q) std::memcpy(&v,q,4); return v; };
        const uint32_t ctxp = rd(gp - 0x5710u);
        const uint32_t drawbase = rd((ctxp + 4u) & 0x1FFFFFFFu);
        if (g_orig109508) g_orig109508(rdram, ctx, runtime); // run the fill first (builds buffer A = a0)
        // TEST (PS2X_HBDUP): the fill builds buffer A (a0) but the drawer reads buffer B (drawbase).
        // Re-run the fill with a0 = drawbase so buffer B gets the full pointer-linked structure built
        // properly. If the bar appears, the fix is to make the fill target the display buffer.
        if (drawbase && a0 != drawbase && std::getenv("PS2X_HBDUP")) {
            ctx->r[4] = _mm_cvtsi32_si128((int)drawbase);   // a0 = buffer B
            if (g_orig109508) g_orig109508(rdram, ctx, runtime);
            ctx->r[4] = _mm_cvtsi32_si128((int)a0);          // restore
        }
        auto rh = [&](uint32_t off) -> int { return (int)(int16_t)(uint16_t)rd((a0+off)&0x1FFFFFFFu); };
        // Compare the fill's health-bar descriptor (a0+0x188) vs a working FRAME descriptor (a0+0x118).
        if (n < 8 || (n % 512u) == 1u)
            std::fprintf(stderr, "[hbfill] #%u a0=0x%x | HBdesc@+0x188 halfwords(0x8..0xe)=%d,%d,%d,%d  full[0..0x1c]=%08x %08x %08x %08x %08x %08x %08x | FRAMEdesc@+0x118(0x8..0xe)=%d,%d,%d,%d\n",
                         n, a0, rh(0x190),rh(0x192),rh(0x194),rh(0x196),
                         rd(a0+0x188),rd(a0+0x18c),rd(a0+0x190),rd(a0+0x194),rd(a0+0x198),rd(a0+0x19c),rd(a0+0x1a0),
                         rh(0x120),rh(0x122),rh(0x124),rh(0x126));
    }

    // Camera matrix-multiply probe (PS2X_CAMPROBE). sub_001201B8 concatenates $a0 = A($a1) x B($a2).
    // The gameplay-camera update (FUN_0023d510) calls it at ra=0x23d9bc to build the camera WORLD
    // matrix = localRot(BASE+0x260) x parent(BASE+0x40). Dump A and B ONLY for that caller so we
    // learn which input is zero (local rotation vs parent transform) = the true upstream root.
    PS2Runtime::RecompiledFunction g_orig1201b8 = nullptr;
    void bt3CamMulProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_001201B8
    {
        const uint32_t ra = getRegU32(ctx, 31);
        if (ra == 0x23d9bcu) // the gameplay-camera world-matrix concat
        {
            const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6);
            // Arm the write-watch on the camera-target range [BASE+0x210, BASE+0x268) once,
            // so we catch whoever writes the target vector (0x220/0x260). BASE = a2 - 0x40.
            static const bool s_cw = [](){ const char *v = std::getenv("PS2X_CAMWATCH"); return v && v[0] && v[0] != '0'; }();   // [watchgate]
            if (s_cw && g_ps2WatchLo.load(std::memory_order_relaxed) == 0u)
            {
                const uint32_t base = (a2 - 0x40u) & 0x1FFFFFFFu;
                g_bt3CamBase.store(base, std::memory_order_relaxed);
                g_ps2WatchHi.store(base + 0x310u, std::memory_order_relaxed);
                g_ps2WatchLo.store(base + 0x2f0u, std::memory_order_relaxed);
                std::cerr << "[camwatch] armed on 0x"<<std::hex<<(base+0x2f0u)<<"..0x"<<(base+0x310u)<<" (flags 0x300/0x304 + orientation)"<<std::dec<<std::endl;
            }
            auto rf=[&](uint32_t p,int i)->float{ const uint8_t*q=getMemPtr(rdram,(p+ (uint32_t)i*4)&0x1FFFFFFFu); float f=0; if(q)std::memcpy(&f,q,4); return f; };
            static std::atomic<uint32_t> s_n{0};
            if ((s_n.fetch_add(1) % 120u) == 1u)
            {
                const uint32_t base = a2 - 0x40u; // B = BASE+0x40 => BASE
                std::cerr << "[camstruct] BASE=0x"<<std::hex<<base<<std::dec<<" (nonzero rows of 0x340):\n";
                for (uint32_t off = 0; off < 0x340u; off += 16)
                {
                    float v0=rf(base+off,0),v1=rf(base+off,1),v2=rf(base+off,2),v3=rf(base+off,3);
                    if (v0!=0.0f||v1!=0.0f||v2!=0.0f||v3!=0.0f)
                        std::fprintf(stderr, "  +0x%03x: %12.4g %12.4g %12.4g %12.4g\n", off, v0,v1,v2,v3);
                }
                // Dump the attached target object (0x1611080) to see if it's a valid fighter.
                const uint32_t tgt = g_bt3CamTarget.load(std::memory_order_relaxed);
                if (tgt)
                {
                    auto ru=[&](uint32_t p)->uint32_t{ const uint8_t*q=getMemPtr(rdram,p&0x1FFFFFFFu); uint32_t u=0; if(q)std::memcpy(&u,q,4); return u; };
                    std::fprintf(stderr, "[camtgt] obj=0x%x  hdr:", tgt);
                    for (uint32_t o=0;o<0x40;o+=4) std::fprintf(stderr, " %08x", ru(tgt+o));
                    std::fprintf(stderr, "\n  +0x10=0x%x  +0x91C=0x%x  as-floats +0x0:", ru(tgt+0x10), ru(tgt+0x91C));
                    for (int i=0;i<8;i++) std::fprintf(stderr, " %.4g", rf(tgt, i));
                    std::fprintf(stderr, "\n");
                }
            }
        }
        if (g_orig1201b8) g_orig1201b8(rdram, ctx, runtime);
    }

    void bt3FrameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00100ab8
    {
        // Keep the SE stream fed from the active voices. Effects are produced incrementally so
        // a stop-by-serial can cut a voice's tail; without a per-frame top-up a long effect
        // would only advance when the next SE command happened to arrive.
        seServiceVoices(runtime);
        g_bt3FrameCount.fetch_add(1, std::memory_order_relaxed);
        {   // [framegate] PS2X_FRAMEGATE (default ON when async is on, =0 disables): require two
            // vsync ticks between render kicks.
            //
            // BT3's 30 fps is partly EMERGENT, not declared. Measured 2026-09-06, split screen:
            //   sync : frame 33.3 ms = 21.62 CPU + 11.7 wait  -> guest CPU EXCEEDS one 16.7 ms
            //          vsync, so the frame always misses the next one and lands on the second.
            //   async: frame 21.3 ms =  3.62 CPU + 17.7 wait  -> CPU now FITS inside one vsync,
            //          so it starts catching vsyncs it used to miss: 47 fps, i.e. fast-forward.
            // 1P was never affected (sync CPU 13.9 ms already fits) which is why async holds a
            // clean 30 there and only split screen ran away. So the render DURATION was the brake,
            // and moving it off-thread removed it -- no status bit could have fixed that, and
            // indeed [kickq] shows depth=0 in both modes, i.e. the worker is never backed up and
            // the CHCR.STR gate is always already clear when the guest polls.
            //
            // Two ticks is what the console effectively enforced. It costs nothing whenever the
            // guest is already slower than that (every machine that cannot hit 30 -- the ones we
            // actually care about), and it stops the overshoot on machines that can.
            static const bool s_gate = [](){ const char *v = std::getenv("PS2X_FRAMEGATE");
                                             return !(v && v[0] == '0'); }();
            // CONDITIONAL, and it must be: the first version gated unconditionally and halved the
            // MENUS, which run at 60 on hardware (fights are the 30-locked ones). Sync mode's brake
            // was the render EXCEEDING ONE VSYNC, so only reproduce it when the render actually
            // does. Menus leave the worker near-idle -> ungated -> 60 preserved. A split-screen
            // fight loads it to ~18 ms -> gated -> 30, which is what sync produced anyway.
            static const uint64_t s_vsyncNs = [](){ const char *v = std::getenv("PS2X_VBLANK_US");
                                                    const long us = (v && v[0]) ? std::atol(v) : 0L;
                                                    return (uint64_t)(us > 1000 ? us : 16667L) * 1000ull; }();
            const bool heavy = g_workerFrameNs.load(std::memory_order_relaxed) > s_vsyncNs;
            if (s_gate && heavy && PS2Memory::asyncKickEnabled())
            {
                static uint64_t s_lastTick = 0;
                const uint64_t want = s_lastTick + 2u;
                // Bounded: never wait more than ~50 ms, so a stalled vblank worker cannot hang
                // the guest (the failure mode I wrongly suspected of [asyncpace] earlier tonight).
                for (int i = 0; i < 50; ++i)
                {
                    const uint64_t now = ps2_syscalls::GetCurrentVSyncTick();
                    if (now >= want || now < s_lastTick) break;   // reached it, or counter reset
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                s_lastTick = ps2_syscalls::GetCurrentVSyncTick();
            }
        }
        {   // [guestbusy-tid] Publish THIS thread's CPU time for the [guestbusy] meter in
            // GsGpuRenderer::swapFrame(). bt3FrameKick is the game's own per-frame render kick, so
            // it always runs on the guest thread -- unlike swapFrame, which moves to the kick worker
            // under PS2X_ASYNC_KICK and made guest_ms measure the wrong thread there.
#if defined(_WIN32)
            g_guestThreadCpuNs.store((uint64_t)ps2xWinThreadCpuNs(), std::memory_order_relaxed);
#else
            struct timespec tg; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tg);
            g_guestThreadCpuNs.store((uint64_t)tg.tv_sec * 1000000000ull + (uint64_t)tg.tv_nsec,
                                     std::memory_order_relaxed);
#endif
        }
        {   // [init114] lifecycle: periodic read of the resident texture table rec0/rec14 ptrs
            static const bool s_i14 = [](){ const char *v = std::getenv("PS2X_INIT114"); return v && v[0] && v[0] != '0'; }();
            if (s_i14)
            {
                const uint64_t fr_ = g_bt3FrameCount.load(std::memory_order_relaxed);
                static std::atomic<uint32_t> s_pn{0};
                if ((fr_ % 300u) == 0u && s_pn.fetch_add(1) < 40u)
                {
                    auto r32 = [&](uint32_t a) -> uint32_t { const uint8_t *pp = getMemPtr(rdram, a & 0x1FFFFFFFu); uint32_t v = 0; if (pp) std::memcpy(&v, pp, 4); return v; };
                    std::fprintf(stderr, "[tbl-life] fr=%llu rec0=0x%x rec14=0x%x rec15=0x%x\n",
                                 (unsigned long long)fr_, r32(0x135a658u), r32(0x135a658u + 14u*64u), r32(0x135a658u + 15u*64u));
                }
            }
        }
        // [valscan] PS2X_VALSCAN=<hexval>: one-shot full-RAM scans for a 32-bit value at
        // fr>=4400 and fr>=4460 -- finds the constants source struct + DL copies without store tracing.
        {
            static const uint32_t s_vsv = [](){ const char *v = std::getenv("PS2X_VALSCAN"); return v && v[0] ? (uint32_t)std::strtoul(v, nullptr, 16) : 0u; }();
            static int s_vsn = 0;
            const uint64_t vfr_ = g_bt3FrameCount.load(std::memory_order_relaxed);
            if (s_vsv && ((s_vsn == 0 && vfr_ >= 4400u) || (s_vsn == 1 && vfr_ >= 4460u)))
            {
                ++s_vsn;
                const uint32_t *w = reinterpret_cast<const uint32_t *>(rdram);
                uint32_t hits = 0;
                for (uint32_t i = 0; i < (32u * 1024u * 1024u) / 4u; ++i)
                    if (w[i] == s_vsv)
                    {
                        if (hits < 40u) std::fprintf(stderr, "[valscan] fr=%llu addr=0x%08x\n", (unsigned long long)vfr_, i * 4u);
                        ++hits;
                    }
                std::fprintf(stderr, "[valscan] fr=%llu total=%u\n", (unsigned long long)vfr_, hits);
            }
        }
        // [ramdump] PS2X_RAMDUMP=<hexaddr>,<hexbytes>,<frame>: one-shot guest RAM dump to work/ramdump.bin
        {
            static const std::string s_rd = [](){ const char *v = std::getenv("PS2X_RAMDUMP"); return std::string(v ? v : ""); }();
            static uint32_t ra_=0, rb_=0, rf_=0;
            static const bool s_rok = !s_rd.empty() && std::sscanf(s_rd.c_str(), "%x,%x,%x", &ra_, &rb_, &rf_) == 3;
            static bool s_rdone = false;
            { static bool s_said = false;
              if (!s_said && !s_rd.empty()) { s_said = true;
                std::fprintf(stderr, "[ramdump] cfg='%s' ok=%d addr=0x%x bytes=0x%x frGate=%u\n",
                             s_rd.c_str(), (int)s_rok, ra_, rb_, rf_); } }
            if (s_rok && !s_rdone && g_bt3FrameCount.load(std::memory_order_relaxed) >= rf_)
            {
                s_rdone = true;
                if (const uint8_t *pr = getMemPtr(rdram, ra_))
                    if (FILE *f = std::fopen("/home/z3/Desktop/bt3/work/ramdump.bin", "wb"))
                    { std::fwrite(pr, 1, rb_, f); std::fclose(f);
                      std::fprintf(stderr, "[ramdump] 0x%x +0x%x written at fr=%u\n", ra_, rb_, rf_); }
            }
        }
        // [sheetwatch] PS2X_SHEETWATCH=1: per-frame content watch on the terrain band sheet
        // buffer (0x53d3a0) — prints the frame whenever the first 64 bytes change.
        {
            static const bool s_sw2 = [](){ const char *v = std::getenv("PS2X_SHEETWATCH"); return v && v[0] && v[0] != '0'; }();
            if (s_sw2)
            {
                static uint8_t s_last[64]; static bool s_have = false; static int s_prints = 0;
                if (const uint8_t *ps = getMemPtr(rdram, 0x53d3a0u))
                {
                    if (!s_have || std::memcmp(s_last, ps, 64) != 0)
                    {
                        std::memcpy(s_last, ps, 64); 
                        if (s_prints < 40)
                        {
                            ++s_prints;
                            char hx[40]; for (int i = 0; i < 16; ++i) std::snprintf(hx + i*2, 4, "%02x", ps[i]);
                            std::fprintf(stderr, "[sheetwatch] fr=%llu changed%s first16=%s\n",
                                         (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed),
                                         s_have ? "" : " (first)", hx);
                        }
                        s_have = true;
                    }
                }
            }
        }
        // ***** PER-FRAME CD FILE-SERVER PUMP (PS2X_CDPUMP, default ON) *****
        // The in-fight STAGE-CHUNK streaming (near-LOD terrain, collision) polls its
        // completion through paths that never tick the CRI CD file server FUN_0028a3b0 —
        // unlike the boot loaders, whose polls we hook to pump inline (bt3CdReadStatePoll /
        // bt3AfsStatusPoll). Result: terrain chunk reads (archive ids 5/6) complete only
        // by accident (~2x per fight) and the ensure-resident loop re-requests forever =
        // the missing-ground/collision livelock. On real HW the server runs continuously
        // on the IOP; pump it once per game frame here — same established tick pattern.
        {
            static const bool s_pump = [](){ const char *v = std::getenv("PS2X_CDPUMP"); return !(v && v[0] == '0'); }();
            Bt3CdTickGuard tickGuard;
            if (s_pump && tickGuard.engaged && runtime->hasFunction(0x0028a3b0u))
            {
                // PS2X_CDPUMP_N: tick the server N times per frame (default 1). The server is a
                // state machine advancing ~one stage per tick; 1/frame starves multi-stage chunk
                // requests (terrain texture slots stay 0xFE fill — the pale-terrain root).
                static const uint32_t s_pumpN = [](){ const char *v = std::getenv("PS2X_CDPUMP_N"); return v && v[0] ? (uint32_t)std::strtoul(v, nullptr, 0) : 1u; }();
                for (uint32_t pn = 0; pn < s_pumpN; ++pn)
                {
                    R5900Context tctx = *ctx;           // inherit gp/sp
                    tctx.r[31] = _mm_setzero_si128();   // ra = 0 => run until return
                    tctx.pc = 0x0028a3b0u;              // CD file-server tick
                    uint32_t steps = 0u;
                    while (tctx.pc != 0u && steps++ < 2000000u)
                    {
                        PS2Runtime::RecompiledFunction step = runtime->lookupFunction(tctx.pc);
                        if (!step) break;
                        step(rdram, &tctx, runtime);
                    }
                }
                s_bt3CdTicking = false;
            }
        }
        // PS2X_CAMDUMP: dump the camera/view struct at EE 0x1001b40 (found via PCSX2 -- the
        // view matrix rot+translate lives here). If zero in our run, the camera is never
        // computed = the root of the zero MVP.
        {
            static const bool s_cd = [](){ const char *v=std::getenv("PS2X_CAMDUMP"); return v&&v[0]&&v[0]!='0'; }();
            if (s_cd)
            {
                static std::atomic<uint32_t> s_n{0};
                if ((s_n.fetch_add(1) % 120u) == 1u)
                {
                    auto rf=[&](uint32_t a)->float{ const uint8_t*p=getMemPtr(rdram,a&0x1FFFFFFFu); float f=0; if(p) std::memcpy(&f,p,4); return f; };
                    std::cerr << "[cam] 0x1001b40:";
                    for (uint32_t q=0; q<8; ++q)
                        std::cerr << " ["<<q<<"]"<<rf(0x1001b40+q*16)<<","<<rf(0x1001b44+q*16)<<","<<rf(0x1001b48+q*16)<<","<<rf(0x1001b4c+q*16);
                    std::cerr << std::endl;
                }
            }
        }
        // Cadence probe (PS2X_CADENCE): vsync ticks elapsed since the last render kick.
        // 1 => the game renders every vblank (60fps); 3 => every 3rd (20fps). Histogram
        // reveals whether the menu 20-vs-60 is a clean N-vsync wait or jittery.
        {
            static const bool s_cad = [](){ const char *v = std::getenv("PS2X_CADENCE"); return v && v[0] && v[0] != '0'; }();
            if (s_cad)
            {
                static thread_local uint64_t s_lastV = 0;
                const uint64_t v = ps2_syscalls::GetCurrentVSyncTick();
                const uint64_t d = v - s_lastV; s_lastV = v;
                static std::atomic<uint32_t> s_h[8]{}; static std::atomic<uint32_t> s_n{0};
                s_h[d < 7 ? d : 7].fetch_add(1, std::memory_order_relaxed);
                if ((s_n.fetch_add(1) % 120u) == 119u)
                {
                    std::fprintf(stderr, "[cadence] vsync/frame: 0=%u 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7+=%u\n",
                        s_h[0].load(),s_h[1].load(),s_h[2].load(),s_h[3].load(),s_h[4].load(),s_h[5].load(),s_h[6].load(),s_h[7].load());
                    for (auto &x : s_h) x.store(0);
                }
            }
        }
        // PS2X_DISPFB_PUBLISH: publish on the real DISPFB1 flip instead of here (the render-kick),
        // so a published frame contains the WHOLE frame in order (render targets THEN the draws
        // that sample them) -> the HUD/composite can resolve their render-target sources. Opt-in
        // because per-flip publishing risks partial/extra frames + cadence jitter on the menus.
        static const bool s_dfPub = [](){ const char *v = std::getenv("PS2X_DISPFB_PUBLISH"); return v && v[0] && v[0] != '0'; }();
        if (GsGpuRenderer::enabled() && !s_dfPub)
        {
            // Async kick mode: the frame's draws are still in the kick-worker queue, so the
            // publish must be enqueued after them (stream order), not executed here.
            if (PS2Memory::asyncKickEnabled())
                runtime->memory().enqueueGpuSwapMarker();
            else
                ps2GpuRenderer().swapFrame(); // publish this frame's GPU command list (render-kick, default)
        }
        if (g_orig100ab8)
            g_orig100ab8(rdram, ctx, runtime);
    }

    PS2Runtime::RecompiledFunction g_orig265298 = nullptr;
    void bt3FileLoadPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00265298
    {
        static thread_local bool s_inTick = false;
        // Pump the ADX tick FUN_0028a530 at most ONCE PER VSYNC. The post-boot
        // FUN_00263198 loop calls this thousands of times/frame; pumping the ADX tick
        // every call over-advances and corrupts the ADX state (stuck early / pink).
        // Rate-limiting to once/vsync matches real hardware (CD-paced) and is stable.
        static thread_local uint64_t s_lastVsync = ~0ull;
        const uint64_t vsync = ps2_syscalls::GetCurrentVSyncTick();
        const bool vsyncElapsed = (vsync != s_lastVsync);
        if (!s_inTick && vsyncElapsed && runtime->hasFunction(0x0028a530u))
        {
            s_lastVsync = vsync;
            s_inTick = true;
            R5900Context tctx = *ctx;
            tctx.r[31] = _mm_setzero_si128();
            tctx.pc = 0x0028a530u;
            uint32_t steps = 0u;
            while (tctx.pc != 0u && steps++ < 2000000u)
            {
                PS2Runtime::RecompiledFunction step = runtime->lookupFunction(tctx.pc);
                if (!step)
                {
                    break;
                }
                step(rdram, &tctx, runtime);
            }
            s_inTick = false;
        }
        {
            static const bool s_lp = [](){ const char *v=std::getenv("PS2X_LOADPROBE"); return v&&v[0]&&v[0]!='0'; }();
            if (s_lp)
            {
                static std::atomic<uint32_t> s_n{0};
                uint32_t n = s_n.fetch_add(1);
                (void)n;
            }
        }
        if (g_orig265298)
        {
            g_orig265298(rdram, ctx, runtime);
        }
        else
        {
            setReturnU32(ctx, 1u);
            ctx->pc = getRegU32(ctx, 31);
        }
        // Internal-state probe (PS2X_LOADPROBE): func_265298's state struct is at
        // 0x31E760 (+0=state 0..4, +4=fd/handle). Dump it + this call's return so we
        // see exactly which internal read-state is frozen when the fight won't load.
        {
            static const bool s_lp = [](){ const char *v=std::getenv("PS2X_LOADPROBE"); return v&&v[0]&&v[0]!='0'; }();
            if (s_lp)
            {
                auto rd = [&](uint32_t a)->uint32_t{ const uint8_t*p=getMemPtr(rdram,a&0x1FFFFFFFu); return p?*reinterpret_cast<const uint32_t*>(p):0u; };
                auto rb = [&](uint32_t a)->int{ const uint8_t*p=getMemPtr(rdram,a&0x1FFFFFFFu); return p?(int)(int8_t)*p:-99; };
                static std::atomic<uint32_t> s_n{0};
                if ((s_n.fetch_add(1) % 240u) == 1u)
                {
                    const uint32_t fd = rd(0x31E764u);
                    std::cerr << "[fileload] ret=" << getRegU32(ctx,2)
                              << " state@0x31E760=" << rd(0x31E760u)
                              << " fd=0x" << std::hex << fd << std::dec
                              << " adxState@fd+1=" << (fd?rb(fd+1u):-1)
                              << " fd[0]=" << (fd?rb(fd):-1)
                              << " fd+4=0x" << std::hex << (fd?rd(fd+4u):0) << std::dec << std::endl;
                }
            }
        }
    }

    // Opening-movie (and any post-boot AFS/PSS) load. The intro-movie sequencer
    // FUN_0035de58 spins `while (FUN_00264af0() != true)` where FUN_00264af0 =
    // (adxf_GetPtStat == 3). The ADX file-read driver FUN_0028a530 that advances
    // that partition state is normally ticked by the game's BOOT loop FUN_00264b18
    // -- which is no longer running by the movie phase, so the AFS read completes 8
    // sectors then stalls (state stuck at 2) => infinite "loading" screen. The
    // central pump deliberately skips FUN_0028a530 (per-frame double-tick during boot
    // corrupts ADX). Fix, mirroring bt3FileLoadPoll: on the movie-load poll, tick the
    // ADX driver ONCE PER VSYNC (hardware rate), then run the original state check.
    // FUN_00264af0 is AFS-load specific (not called during boot) so boot is untouched.
    PS2Runtime::RecompiledFunction g_orig264af0 = nullptr;
    void bt3MovieLoadPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00264af0
    {
        static thread_local bool s_inTick = false;
        static thread_local uint64_t s_lastVsync = ~0ull;
        const uint64_t vsync = ps2_syscalls::GetCurrentVSyncTick();
        // Only tick when the CRI ADXF partition actually exists. FUN_00264af0 is also
        // called early (adxf=NULL) before the AFS is opened; ticking FUN_0028a530 on a
        // null partition writes garbage and derails execution (0x3376b8 crash). Gating
        // on a valid partition confines the tick to the real movie-load spin.
        uint32_t adxf = 0u;
        if (const uint8_t *h = getMemPtr(rdram, 0x2e6370u))
            adxf = *reinterpret_cast<const uint32_t *>(h);
        {
            static const bool s_lg = std::getenv("PS2X_OVLOG") != nullptr;
            if (s_lg && vsync != s_lastVsync)
            {
                int st = -1, already = -1, total = -1;
                if (adxf)
                {
                    if (const uint8_t *p = getMemPtr(rdram, adxf + 1u)) st = *p;
                    if (const uint8_t *p = getMemPtr(rdram, adxf + 0x18u)) already = *reinterpret_cast<const int *>(p);
                    if (const uint8_t *p = getMemPtr(rdram, adxf + 0xcu)) total = *reinterpret_cast<const int *>(p);
                }
                std::cerr << "[movpoll] vsync=" << vsync << " adxf=0x" << std::hex << adxf
                          << std::dec << " state=" << st << " already=" << already
                          << " total=" << total << std::endl;
            }
        }
        // The ADX tick is gated behind PS2X_MOVIEPUMP (default OFF): registering this hook
        // as a logging-only passthrough (PS2X_OVLOG) is safe and lets us confirm whether the
        // demo-load spins here; enabling the pump is the actual (previously-unstable) fix.
        static const bool s_moviePump = [](){ const char *v=std::getenv("PS2X_MOVIEPUMP"); return v&&v[0]&&v[0]!='0'; }();
        if (s_moviePump && adxf != 0u && !s_inTick && vsync != s_lastVsync && runtime->hasFunction(0x0028a530u))
        {
            s_lastVsync = vsync;
            s_inTick = true;
            R5900Context tctx = *ctx;
            tctx.r[31] = _mm_setzero_si128();
            tctx.pc = 0x0028a530u;
            uint32_t steps = 0u;
            while (tctx.pc != 0u && steps++ < 2000000u)
            {
                PS2Runtime::RecompiledFunction step = runtime->lookupFunction(tctx.pc);
                if (!step)
                {
                    break;
                }
                step(rdram, &tctx, runtime);
            }
            s_inTick = false;
        }
        if (g_orig264af0)
        {
            g_orig264af0(rdram, ctx, runtime);
        }
        else
        {
            setReturnU32(ctx, 0u);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    // SPEED (env PS2X_FASTTIMER): FUN_002baae8 writes EE Timer2 COMP (0xB0001020)
    // through FUN_002baa58's heavy COP0 interrupt-disable + eret critical-section dance.
    // The HLE fires the Timer2 IRQ every vblank regardless of the COMP value, so this
    // write is inert -- yet the game's Timer2 handler (FUN_002bae48) calls it constantly,
    // making it ~85% of frame time and pinning the title/menu at ~2 fps. Skip it: the
    // guest never reads COMP back and the emulated IRQ ignores it. (FUN_002baad8 = the
    // Timer2 MODE / interrupt-flag write is left intact so flags still clear.)
    void bt3FastTimerCompWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_002baae8
    {
        (void)rdram; (void)runtime;
        ctx->pc = getRegU32(ctx, 31); // return, doing nothing
    }

    // SPEED: FUN_00263278 is an LZ decompressor. The menu/popup flash system re-runs it
    // on identical assets every frame -> CPU-bound ~2fps. Decompression is deterministic
    // (input -> output), so cache the output keyed on (src ptr, out size, count, a hash of
    // the compressed header) and, on a repeat with a caller-supplied output buffer, memcpy
    // the cached bytes instead of decompressing. On the FIRST call (miss) we run the real
    // function and record its output. Env-gated (PS2X_DECOMPCACHE).
    PS2Runtime::RecompiledFunction g_orig263278 = nullptr;
    void bt3DecompressCached(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00263278
    {
        const uint32_t a0 = getRegU32(ctx, 4); // compressed src
        const uint32_t a1 = getRegU32(ctx, 5); // output buffer (0 => callee allocates)
        const uint32_t a2 = getRegU32(ctx, 6); // out size ptr (or 0)
        const uint8_t *inp = (a0 != 0u) ? getMemPtr(rdram, a0) : nullptr;
        // Only cache the common fast case: a real src + a caller-supplied output buffer.
        // (a1==0 means the callee allocates a guest buffer, which we can't replicate here.)
        if (!inp || a1 == 0u || !getMemPtr(rdram, a0 + 64u))
        {
            g_orig263278(rdram, ctx, runtime);
            return;
        }
        const uint32_t outSize = *reinterpret_cast<const uint32_t *>(inp);
        const uint32_t count = *reinterpret_cast<const uint32_t *>(inp + 4);
        if (outSize == 0u || outSize > 0x400000u || count == 0u)
        {
            g_orig263278(rdram, ctx, runtime);
            return;
        }
        uint64_t key = 1469598103934665603ull;
        auto mix = [&key](uint32_t v) { key = (key ^ v) * 1099511628211ull; };
        mix(a0); mix(outSize); mix(count);
        for (uint32_t i = 8u; i < 64u; ++i) mix(inp[i]); // hash the compressed header (bounds-checked above)

        static std::mutex s_m;
        static std::unordered_map<uint64_t, std::vector<uint8_t>> s_cache;
        static const bool s_lg = std::getenv("PS2X_OVLOG") != nullptr;
        static std::atomic<uint32_t> s_hit{0}, s_miss{0};
        {
            std::lock_guard<std::mutex> lk(s_m);
            auto it = s_cache.find(key);
            if (it != s_cache.end() && it->second.size() == outSize)
            {
                if (s_lg && (s_hit.fetch_add(1) % 512u) == 0u)
                    std::cerr << "[decomp] HIT hits=" << s_hit.load() << " miss=" << s_miss.load()
                              << " cacheN=" << s_cache.size() << " outSize=" << outSize << std::endl;
                if (uint8_t *out = getMemPtr(rdram, a1))
                    std::memcpy(out, it->second.data(), outSize);
                if (a2 != 0u)
                    if (uint8_t *sp = getMemPtr(rdram, a2)) *reinterpret_cast<uint32_t *>(sp) = outSize;
                setReturnU32(ctx, a1);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        // Miss: run the real decompressor, then record its output.
        if (s_lg && (s_miss.fetch_add(1) % 512u) == 0u)
            std::cerr << "[decomp] MISS hits=" << s_hit.load() << " miss=" << s_miss.load()
                      << " cacheN=" << s_cache.size() << " outSize=" << outSize << " src=0x" << std::hex << a0 << std::dec << std::endl;
        g_orig263278(rdram, ctx, runtime);
        const uint32_t outBuf = getRegU32(ctx, 2); // v0 = output buffer
        if (const uint8_t *op = getMemPtr(rdram, outBuf))
        {
            if (getMemPtr(rdram, outBuf + outSize))
            {
                std::lock_guard<std::mutex> lk(s_m);
                s_cache[key].assign(op, op + outSize);
            }
        }
    }

    // DIAGNOSTIC: stream/queue processor FUN_0027f518 is the current spin point
    // (in func_239ff0's wait loop). Log the object's state fields on change so we
    // can see exactly what completion it is waiting for. Trampolines to original.
    PS2Runtime::RecompiledFunction g_orig27f518 = nullptr;
    void bt3StreamProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // hooked fn
    {
        {
            const uint32_t raOuter = getRegU32(ctx, 31);
            static uint32_t s_lastRa = 0xdeadbeefu;
            static uint32_t s_raCount = 0u;
            if (raOuter != s_lastRa && s_raCount < 60u)
            {
                s_lastRa = raOuter;
                ++s_raCount;
                std::cerr << "[probe] enter hooked fn, ra=0x" << std::hex << raOuter << std::dec << std::endl;
            }
        }
        const uint32_t obj = getRegU32(ctx, 4);
        if (const uint8_t *base = getMemPtr(rdram, obj))
        {
            auto rd32 = [&](uint32_t off) -> uint32_t { return *reinterpret_cast<const uint32_t *>(base + off); };
            const uint8_t st4 = base[4];
            const uint8_t st1 = base[1];
            const uint32_t idx = rd32(0x20);
            const uint32_t cnt = rd32(0x24);
            const uint32_t handle = rd32(0x28);
            uint32_t field = 0xffffffffu;
            const uint32_t fieldOff = 0x50u + idx * 0x20u;
            if (fieldOff + 4u <= 0x4000u)
            {
                field = rd32(fieldOff);
            }
            const uint32_t ra = getRegU32(ctx, 31);
            static uint32_t s_lastSig = 0xdeadbeefu;
            const uint32_t sig = (uint32_t)st4 | ((uint32_t)st1 << 8) | ((idx & 0xff) << 16) | ((field & 0xff) << 24) | (ra << 12);
            static uint32_t s_count = 0u;
            if (sig != s_lastSig && s_count < 200u)
            {
                s_lastSig = sig;
                ++s_count;
                std::cerr << "[27f518] obj=0x" << std::hex << obj
                          << " st4=" << (int)st4 << " st1=" << (int)st1
                          << " idx=" << idx << " cnt=" << cnt
                          << " handle=0x" << handle << " field=0x" << field
                          << " ra=0x" << ra
                          << std::dec << std::endl;
            }
        }
        if (g_orig27f518)
        {
            g_orig27f518(rdram, ctx, runtime);
        }
    }

    void applyBt3SoundInitBypass(PS2Runtime &runtime)
    {
        std::cerr << "[game_overrides] BT3: sound init bypass + lock-callback stub" << std::endl;
        if (std::getenv("PS2X_PROBE_STREAM"))
        {
            g_orig27f518 = runtime.lookupFunction(0x0027e938u);
            runtime.replaceFunction(0x0027e938u, &bt3StreamProbe);
        }
        // CD/file read completion is now driven by the central interrupt-tick pump
        // in PS2Runtime::dispatchLoop (FUN_0028a3b0 + FUN_0028a530). That lets the
        // ORIGINAL async driver functions (func_270dd0 -> func_270E08, FUN_00265298)
        // read the real, tick-advanced state instead of a hand-faked completion.
        // FUN_00265298 (post-boot file-load state machine spun on by FUN_00263198)
        // needs the ADX tick pumped, but ONCE PER VSYNC not per poll (per-poll pump
        // over-ticks -> corrupt/pink). bt3FileLoadPoll is now vsync-gated -> stable.
        g_orig265298 = runtime.lookupFunction(0x00265298u);
        runtime.replaceFunction(0x00265298u, &bt3FileLoadPoll);
        // Frame counter hook (harmless passthrough) for an honest fps readout.
        g_orig100ab8 = runtime.lookupFunction(0x00100ab8u);
        if (g_orig100ab8)
            runtime.replaceFunction(0x00100ab8u, &bt3FrameKick);
        if (const char *v = std::getenv("PS2X_STAGEGATE"); v && v[0] && v[0] != '0')
            bt3StageGateArm(runtime);
        if (const char *v = std::getenv("PS2X_FORCE14"); v && v[0] && v[0] != '0')
        {
            g_f14Orig = runtime.lookupFunction(0x0010a218u);
            if (g_f14Orig) runtime.replaceFunction(0x0010a218u, &bt3Force14Probe);
            std::fprintf(stderr, "[force14] hook %s\n", g_f14Orig ? "ok" : "MISSING");
        }
        if (const char *v = std::getenv("PS2X_INIT114"); v && v[0] && v[0] != '0')
        {
            g_i114Orig = runtime.lookupFunction(0x00114c60u);
            if (g_i114Orig) runtime.replaceFunction(0x00114c60u, &bt3Init114Probe);
            std::fprintf(stderr, "[init114] hook %s\n", g_i114Orig ? "ok" : "MISSING");
        }
        if (const char *v = std::getenv("PS2X_INST337"); v && v[0] && v[0] != '0')
        {
            const uint32_t idx337 = (0x337090u - 0x334C00u) / 4u;
            g_i337Orig = g_ps2OverlayFunctionTable[idx337];
            g_ps2OverlayFunctionTable[idx337] = &bt3Inst337Probe;
            std::fprintf(stderr, "[inst337] overlay hook %s\n", g_i337Orig ? "ok" : "MISSING");
        }
        if (const char *v = std::getenv("PS2X_TBLCEN"); v && v[0] && v[0] != '0')
        {
            g_tcOrig = runtime.lookupFunction(0x0010c520u);
            if (g_tcOrig) runtime.replaceFunction(0x0010c520u, &bt3TblCenProbe);
            std::fprintf(stderr, "[tblcen] hook %s\n", g_tcOrig ? "ok" : "MISSING");
        }
        if (const char *v = std::getenv("PS2X_CAROUSEL"); v && v[0] && v[0] != '0')
        {
            g_caOrig = runtime.lookupFunction(0x00100738u);
            if (g_caOrig) runtime.replaceFunction(0x00100738u, &bt3CarouselProbe);
            std::fprintf(stderr, "[carousel] hook %s\n", g_caOrig ? "ok" : "MISSING");
        }
        if (const char *v = std::getenv("PS2X_ROLLBACK"); v && v[0] && v[0] != '0')
        {
            g_rbOrig = runtime.lookupFunction(0x00100890u);
            if (g_rbOrig) runtime.replaceFunction(0x00100890u, &bt3RollbackProbe);
            std::fprintf(stderr, "[rollback] hook %s\n", g_rbOrig ? "ok" : "MISSING");
        }
        // Resource-ready probe hook (only logs under PS2X_LOADPROBE; passthrough otherwise).
        g_orig252d78 = runtime.lookupFunction(0x00252d78u);
        if (g_orig252d78)
            runtime.replaceFunction(0x00252d78u, &bt3ResReadyProbe);
        // NOTE: FUN_00296160 is the PAD STATUS function (bt3PadStatus, returns 1 =
        // controller ready) -- NOT a load gate. The old bt3LoadStatusDone hook here was
        // a wrong-premise dead-end and is removed; bt3PadStatus owns 0x296160 (below).
        // The fight-load gate is FUN_00263508's task queue -- see bt3TaskQueueProbe.
        (void)&bt3LoadStatusDone; (void)g_orig296160;
        // Fight-load task-queue probe (PS2X_TASKPROBE): the fight loader FUN_002635c8
        // loops while FUN_00263508() != 0, which is non-zero while its work-item queue
        // at *(0x2FF120) is non-empty. Dump the stuck task object + its callback ptr so
        // we can name the exact subsystem whose "done" never fires. Passthrough otherwise.
        if (std::getenv("PS2X_TASKPROBE"))
        {
            g_orig263508 = runtime.lookupFunction(0x00263508u);
            if (g_orig263508)
                runtime.replaceFunction(0x00263508u, &bt3TaskQueueProbe);
        }
        // Fight-load DVCI slot-completion signal (see bt3DvciSlotComplete). Default ON
        // (it is the correct synchronous-completion model); set PS2X_NO_DVCI_COMPLETE=1
        // to disable for A/B testing.
        if (!std::getenv("PS2X_NO_DVCI_COMPLETE"))
            runtime.replaceFunction(0x00124548u, &bt3DvciSlotComplete);
        // Fight-load AFS-stream completion: pump the CD file-server tick on the AFS status
        // poll so the partition read advances 2->3 (see bt3AfsStatusPoll). Default ON;
        // PS2X_NO_AFS_TICK=1 disables for A/B testing.
        if (!std::getenv("PS2X_NO_AFS_TICK"))
        {
            g_orig26b900 = runtime.lookupFunction(0x0026b900u);
            runtime.replaceFunction(0x0026b900u, &bt3AfsStatusPoll);
        }
        // HLE acosf (0x28f710 = the game's acosf entry): the 957-line recompiled polynomial
        // intermittently goes wrong (source of garbage hair-bend angles). Replace with host
        // acosf, input clamped to the domain like the game does anyway. Default ON;
        // PS2X_HLE_ACOS=0 restores the recompiled original.
        {
            const char *v = std::getenv("PS2X_HLE_ACOS");
            if (!(v && v[0] == '0'))
                runtime.replaceFunction(0x0028f710u, &bt3Acosf);
        }
        // Camera view-matrix builder probe (PS2X_CAMPROBE).
        // Demo scene-tree recursion-depth guard: default ON (prevents the cyclic-tree stack
        // overflow crash). Disable with PS2X_NO_DEMO_GUARD. The PS2X_DEMOPROBE dump rides on it.
        if (std::getenv("PS2X_VSTEP"))
        {   // [vstep] [logicrate]
            g_orig102060 = runtime.lookupFunction(0x00102060u);
            if (g_orig102060) runtime.replaceFunction(0x00102060u, &bt3VStep);
            g_orig115950 = runtime.lookupFunction(0x00115950u);
            g_orig264d98 = runtime.lookupFunction(0x00264d98u);
            if (g_orig264d98) runtime.replaceFunction(0x00264d98u, &bt3WaitProbe);   // [vstepprobe]
            if (g_orig115950) runtime.replaceFunction(0x00115950u, &bt3LogicRate);
            std::fprintf(stderr, "[vstep] step override armed (0x102060 %s, 0x115950 %s)\n", g_orig102060 ? "ok" : "MISSING", g_orig115950 ? "ok" : "MISSING");
        }
        {   // [shadowprobe]
            const char *sp = std::getenv("PS2X_SHADOWPROBE");
            if (sp && sp[0] == '1') bt3ShadowProbeArm(runtime);
        }
        {   // [fixupprobe] always on (a few lines per load)
            g_orig10a028 = runtime.lookupFunction(0x0010a028u);
            if (g_orig10a028) runtime.replaceFunction(0x0010a028u, &bt3FixupProbe);
        }
        {   // [thunkwatch]
            const char *tw = std::getenv("PS2X_THUNKWATCH");
            if (!(tw && tw[0] == '0'))
            {
                g_orig2722c0 = runtime.lookupFunction(0x002722c0u);
                if (g_orig2722c0) runtime.replaceFunction(0x002722c0u, &bt3ThunkStackWatch);
                g_orig2188b8 = runtime.lookupFunction(0x002188b8u);   // [terrround]
                if (g_orig2188b8) runtime.replaceFunction(0x002188b8u, &bt3TerrRoundScope);
                if (std::getenv("PS2X_UPB"))
                {
                    g_orig13c300 = runtime.lookupFunction(0x0013c300u);
                    if (g_orig13c300) runtime.replaceFunction(0x0013c300u, &bt3Upb13c300);
                    g_orig13c638 = runtime.lookupFunction(0x0013c638u);
                    if (g_orig13c638) runtime.replaceFunction(0x0013c638u, &bt3Upb13c638);
                    g_orig13ca80 = runtime.lookupFunction(0x0013ca80u);
                    if (g_orig13ca80) runtime.replaceFunction(0x0013ca80u, &bt3Upb13ca80);
                }
                if (std::getenv("PS2X_WLK"))
                {
                    const uint32_t slot = (0x399b18u - g_ps2OverlayFunctionTableBase) / 4u;
                    if (slot < g_ps2OverlayFunctionTableSlotCount && g_ps2OverlayFunctionTable[slot])
                    {
                        g_origWalker = g_ps2OverlayFunctionTable[slot];
                        g_ps2OverlayFunctionTable[slot] = &bt3WalkerProbe;
                        std::fprintf(stderr, "[wlk] walker hook installed (slot %u)\n", slot);
                    }
                    else std::fprintf(stderr, "[wlk] hook FAILED (slot %u base 0x%x)\n", slot, g_ps2OverlayFunctionTableBase);
                }
                if (std::getenv("PS2X_SPRQ"))
                {
                    g_orig2bb098 = runtime.lookupFunction(0x002bb098u);   // [sprq]
                    if (g_orig2bb098) runtime.replaceFunction(0x002bb098u, &bt3SprQProbe);
                }
                if (std::getenv("PS2X_PAKCPY"))
                {
                    g_orig2a9a1c = runtime.lookupFunction(0x002a9a1cu);   // [pakcpy]
                    if (g_orig2a9a1c) runtime.replaceFunction(0x002a9a1cu, &bt3PakCpyProbe);
                }
                if (std::getenv("PS2X_SLOTPROBE"))
                {
                    g_orig24f860 = runtime.lookupFunction(0x0024f860u);   // [slotprobe]
                    if (g_orig24f860) runtime.replaceFunction(0x0024f860u, &bt3SlotProbe);
                }
                if (std::getenv("PS2X_VF3PROBE"))
                {
                    g_orig111358 = runtime.lookupFunction(0x00111358u);   // [vf3probe]
                    if (g_orig111358) runtime.replaceFunction(0x00111358u, &bt3Vf3Probe);
                }
            }
        }
        if (!std::getenv("PS2X_NO_DEMO_GUARD"))
        {
            g_orig2316d0 = runtime.lookupFunction(0x002316d0u);
            if (g_orig2316d0) runtime.replaceFunction(0x002316d0u, &bt3DemoWalkGuard);
            g_orig231590 = runtime.lookupFunction(0x00231590u);
            if (g_orig231590) runtime.replaceFunction(0x00231590u, &bt3DemoRecursionGuard);
            if (std::getenv("PS2X_OBJPROBE"))
            {
                g_orig1b1708 = runtime.lookupFunction(0x001b1708u);
                if (g_orig1b1708) runtime.replaceFunction(0x001b1708u, &bt3ObjProbe);
            }
        }
        // Demo garbage-callback guard: default ON, disable with PS2X_NO_DEMO_FIX.
        if (!std::getenv("PS2X_NO_DEMO_FIX"))
        {
            g_orig231768 = runtime.lookupFunction(0x00231768u);
            if (g_orig231768) runtime.replaceFunction(0x00231768u, &bt3DemoCallbackFix);
        }
        if (std::getenv("PS2X_CAMPROBE"))
        {
            g_orig1202a0 = runtime.lookupFunction(0x001202a0u);
            if (g_orig1202a0) runtime.replaceFunction(0x001202a0u, &bt3CamMatrixProbe);
            g_orig120a98 = runtime.lookupFunction(0x00120a98u);
            if (g_orig120a98) runtime.replaceFunction(0x00120a98u, &bt3RotBaseProbe);
            g_orig24e2b0 = runtime.lookupFunction(0x0024e2b0u);
            if (g_orig24e2b0) runtime.replaceFunction(0x0024e2b0u, &bt3E2B0Probe);
            g_orig1201b8 = runtime.lookupFunction(0x001201b8u);
            if (g_orig1201b8) runtime.replaceFunction(0x001201b8u, &bt3CamMulProbe);
            g_orig2499b0 = runtime.lookupFunction(0x002499b0u);
            if (g_orig2499b0) runtime.replaceFunction(0x002499b0u, &bt3PlayerTableProbe);
        }
        if (std::getenv("PS2X_HUDCALLER"))
        {
            g_orig109508 = runtime.lookupFunction(0x00218848u);
            if (g_orig109508) runtime.replaceFunction(0x00218848u, &bt3SpriteProbe);
        }
        if (std::getenv("PS2X_CAMPROBE"))
        {
            for (auto &h : g_camSetters)
            {
                h.orig = runtime.lookupFunction(h.addr);
                if (h.orig) runtime.replaceFunction(h.addr, &bt3CamSetterProbe);
            }
            if (std::getenv("PS2X_CAMFORCE") || std::getenv("PS2X_CAMROUND"))   // [camround] shares the hook
            {
                g_orig23d510 = runtime.lookupFunction(0x0023d510u);
                if (g_orig23d510) runtime.replaceFunction(0x0023d510u, &bt3CamForce);
            }
            if (std::getenv("PS2X_CAMENABLE"))
            {
                g_orig1dac78 = runtime.lookupFunction(0x001dac78u);
                if (g_orig1dac78) runtime.replaceFunction(0x001dac78u, &bt3CamEnableForce);
            }
        }
        // The IOP-side ring consumer LIVES in the stream tick, so the hook has to be installed
        // whenever audio is on -- it is no longer just the [sndstream] diagnostic it started as.
        // DEFAULT ON. Audio was opt-in while it was being built; now that the ring consumer and
        // the SE path are working, a fresh clone should make sound without having to know a flag
        // name. PS2X_SNDPLAY=0 opts out, matching how PS2X_SNDIOP already reads its value. The
        // other three are legacy switches: setting any of them still forces audio on.
        const bool sndAudioOn = []() {
            const char *v = std::getenv("PS2X_SNDPLAY");
            if (v && v[0] == '0')
                return false;
            return true;
        }() || std::getenv("PS2X_SNDSTREAM") || std::getenv("PS2X_SNDPUMP") ||
                                std::getenv("PS2X_SNDIOP");
        if (sndAudioOn)
        {
            g_orig28ae60 = runtime.lookupFunction(0x0028ae60u);
            if (g_orig28ae60) runtime.replaceFunction(0x0028ae60u, &bt3SndStreamTick);
            else std::cerr << "[sndstream] 0x28ae60 not registered" << std::endl;
        }
        // [sndapi] game-facing sound API call counters.
        if (std::getenv("PS2X_SNDAPI"))
        {
            const PS2Runtime::RecompiledFunction probes[kSndApiCount] = {
                &bt3SndApiProbe<0>, &bt3SndApiProbe<1>, &bt3SndApiProbe<2>, &bt3SndApiProbe<3>,
                &bt3SndApiProbe<4>, &bt3SndApiProbe<5>, &bt3SndApiProbe<6>, &bt3SndApiProbe<7>,
                &bt3SndApiProbe<8>, &bt3SndApiProbe<9>, &bt3SndApiProbe<10>, &bt3SndApiProbe<11>,
                &bt3SndApiProbe<12>, &bt3SndApiProbe<13>, &bt3SndApiProbe<14>, &bt3SndApiProbe<15>,
                &bt3SndApiProbe<16>, &bt3SndApiProbe<17>, &bt3SndApiProbe<18>, &bt3SndApiProbe<19>,
                &bt3SndApiProbe<20>, &bt3SndApiProbe<21>, &bt3SndApiProbe<22>, &bt3SndApiProbe<23>,
                &bt3SndApiProbe<24>, &bt3SndApiProbe<25>, &bt3SndApiProbe<26>, &bt3SndApiProbe<27>};
            std::string got;
            for (int i = 0; i < kSndApiCount; ++i)
            {
                g_origSndApi[i] = runtime.lookupFunction(kSndApiAddr[i]);
                if (g_origSndApi[i]) runtime.replaceFunction(kSndApiAddr[i], probes[i]);
                got += (g_origSndApi[i] ? '1' : '0');
            }
            std::fprintf(stderr, "[sndapi] hooks %s\n", got.c_str());
        }
        // STREAM START (0x28b428): resets the sink ring so a restart finds it whole. Registered
        // AFTER the api probes so it overrides the plain counter probe on the same address.
        if (sndAudioOn)
        {
            g_orig28b428 = runtime.lookupFunction(0x0028b428u);
            if (g_orig28b428) runtime.replaceFunction(0x0028b428u, &bt3StreamStartNote);
            else std::cerr << "[sndiop] 0x28b428 (stream START) not registered" << std::endl;
            // 0x281bb0 asserts the ring is whole BEFORE calling 0x28b428, so the reset has to
            // happen here or a restart hangs in the 0x281cf0 error loop.
            g_orig281bb0 = runtime.lookupFunction(0x00281bb0u);
            if (g_orig281bb0) runtime.replaceFunction(0x00281bb0u, &bt3StreamGroupStart);
            else std::cerr << "[sndiop] 0x281bb0 (group start) not registered" << std::endl;
        }
        if (const char *rh = std::getenv("PS2X_RAYHOOK"); rh && rh[0] && rh[0] != '0')
        {
            g_orig132b60 = runtime.lookupFunction(0x00132b60u);
            if (g_orig132b60) runtime.replaceFunction(0x00132b60u, &bt3RayHook);
            g_orig131478 = runtime.lookupFunction(0x00131478u);
            if (g_orig131478) runtime.replaceFunction(0x00131478u, &bt3ClipInHook);
            std::fprintf(stderr, "[rayhook] installed %d\n", g_orig132b60 ? 1 : 0);
        }
        if (const char *cd = std::getenv("PS2X_CADENCE"); cd && cd[0] && cd[0] != '0')
        {   // [cadence] see bt3CadenceTick
            g_orig1c2218 = runtime.lookupFunction(0x001c2218u); if (g_orig1c2218) runtime.replaceFunction(0x001c2218u, &bt3Cad1c2218);
            g_orig1d2d30 = runtime.lookupFunction(0x001d2d30u); if (g_orig1d2d30) runtime.replaceFunction(0x001d2d30u, &bt3Cad1d2d30);
            g_orig1d0508 = runtime.lookupFunction(0x001d0508u); if (g_orig1d0508) runtime.replaceFunction(0x001d0508u, &bt3Cad1d0508);
            g_orig1cf678c = runtime.lookupFunction(0x001cf678u); if (g_orig1cf678c) runtime.replaceFunction(0x001cf678u, &bt3Cad1cf678);
            std::fprintf(stderr, "[cadence] installed %d%d%d%d\n", g_orig1c2218 ? 1 : 0, g_orig1d2d30 ? 1 : 0, g_orig1d0508 ? 1 : 0, g_orig1cf678c ? 1 : 0);
        }
        if (const char *wh = std::getenv("PS2X_WISPHOOK"); wh && wh[0] && wh[0] != '0')
        {   // [wisphook] see bt3QuadEmitHook
            g_orig131a20 = runtime.lookupFunction(0x00131a20u);
            if (g_orig131a20) runtime.replaceFunction(0x00131a20u, &bt3QuadEmitHook);
            std::fprintf(stderr, "[wisphook] installed=%d\n", g_orig131a20 ? 1 : 0);
        }
        // [se] sound effects: service the RPC the IOP would have handled. DEFAULT ON alongside
        // the rest of audio; PS2X_SEPLAY=0 opts out. Gated on sndAudioOn too, so silencing audio
        // silences effects with it rather than leaving them playing on their own.
        const bool sePlayOn = sndAudioOn && []() {
            const char *v = std::getenv("PS2X_SEPLAY");
            return !(v && v[0] == '0');
        }();
        if (sePlayOn)
        {
            g_orig2b48f0 = runtime.lookupFunction(0x002b48f0u);
            if (g_orig2b48f0) runtime.replaceFunction(0x002b48f0u, &bt3SeRpcSend);
            std::fprintf(stderr, "[se] system-SE playback %s\n",
                         g_orig2b48f0 ? "ARMED" : "FAILED (0x2b48f0 not registered)");
        }
        // [sndse] sound-engine lifecycle probe: what happens when a punch should sound?
        if (std::getenv("PS2X_SNDSE"))
        {
            const PS2Runtime::RecompiledFunction probes[kSndSeCount] = {
                &bt3SndSeProbe<0>, &bt3SndSeProbe<1>, &bt3SndSeProbe<2>,
                &bt3SndSeProbe<3>, &bt3SndSeProbe<4>, &bt3SndSeProbe<5>};
            std::string got;
            for (int i = 0; i < kSndSeCount; ++i)
            {
                g_origSndSe[i] = runtime.lookupFunction(kSndSeAddr[i]);
                if (g_origSndSe[i]) runtime.replaceFunction(kSndSeAddr[i], probes[i]);
                got += (g_origSndSe[i] ? '1' : '0');
            }
            std::fprintf(stderr, "[sndse] hooks %s (%s)\n", got.c_str(),
                         "playerCreate/START/STOP,streamAlloc,loadById,waitSlots");
        }
        // [sndnostop] RETIRED. Only hooked when explicitly asked for, as a rollback path.
        if (std::getenv("PS2X_SNDNOSTOP"))
        {
            g_orig28b438 = runtime.lookupFunction(0x0028b438u);
            if (g_orig28b438) runtime.replaceFunction(0x0028b438u, &bt3StreamStopSuppress);
            std::fprintf(stderr, "[sndnostop] stream STOP suppression %s\n",
                         g_orig28b438 ? "ARMED" : "FAILED (0x28b438 not registered)");
        }
        // [sndcnt] sound-ready refcount stage counters.
        if (std::getenv("PS2X_SNDCNT"))
        {
            g_orig26e290 = runtime.lookupFunction(0x0026e290u);
            if (g_orig26e290) runtime.replaceFunction(0x0026e290u, &bt3SndSvc);
            g_orig26d810 = runtime.lookupFunction(0x0026d810u);
            if (g_orig26d810) runtime.replaceFunction(0x0026d810u, &bt3SndEnq);
            g_orig26d9f0 = runtime.lookupFunction(0x0026d9f0u);
            if (g_orig26d9f0) runtime.replaceFunction(0x0026d9f0u, &bt3SndDec);
            std::fprintf(stderr, "[sndcnt] hooks svc=%d enq=%d dec=%d\n",
                         g_orig26e290 != nullptr, g_orig26d810 != nullptr, g_orig26d9f0 != nullptr);
        }
        // [sndwake] sound-thread wake-guard probe.
        if (std::getenv("PS2X_SNDWAKE"))
        {
            g_orig26d338 = runtime.lookupFunction(0x0026d338u);
            if (g_orig26d338) runtime.replaceFunction(0x0026d338u, &bt3SndResumeIfSusp);
            else std::cerr << "[sndwake] 0x26d338 not registered" << std::endl;
            g_orig26e160 = runtime.lookupFunction(0x0026e160u);
            if (g_orig26e160) runtime.replaceFunction(0x0026e160u, &bt3SndKickProbe);
            else std::cerr << "[sndwake] 0x26e160 not registered" << std::endl;
        }
        // Battle-ready wait probe/force (PS2X_BATTLEPROBE or PS2X_FORCEBATTLE).
        if (std::getenv("PS2X_BATTLEPROBE") || std::getenv("PS2X_FORCEBATTLE"))
        {
            g_orig12ab10 = runtime.lookupFunction(0x0012ab10u);
            if (g_orig12ab10) runtime.replaceFunction(0x0012ab10u, &bt3BattleWaitProbe);
        }
        // Sound-ready probe / fight-load-only gate hooks (PS2X_SNDPROBE or
        // PS2X_FIGHTSNDGATE; passthrough otherwise).
        // Battle-ready wait probe/force (PS2X_BATTLEPROBE or PS2X_FORCEBATTLE).
        if (std::getenv("PS2X_BATTLEPROBE") || std::getenv("PS2X_FORCEBATTLE"))
        {
            g_orig12ab10 = runtime.lookupFunction(0x0012ab10u);
            if (g_orig12ab10) runtime.replaceFunction(0x0012ab10u, &bt3BattleWaitProbe);
        }
        // Sound-ready probe / fight-load-only gate hooks (PS2X_SNDPROBE or
        // PS2X_FIGHTSNDGATE; passthrough otherwise).
        if (std::getenv("PS2X_SNDPROBE") || std::getenv("PS2X_FIGHTSNDGATE"))
        {
            g_orig26d9a0 = runtime.lookupFunction(0x0026d9a0u);
            if (g_orig26d9a0) runtime.replaceFunction(0x0026d9a0u, &bt3SoundReadySet);
            g_orig26cd70 = runtime.lookupFunction(0x0026cd70u);
            if (g_orig26cd70) runtime.replaceFunction(0x0026cd70u, &bt3SoundSpinCounter);
        }
        (void)&bt3CdReadStatePoll;
        // Timer2 COMP-write fast path (env PS2X_FASTTIMER) -- kills the ~2fps menu stall.
        if (std::getenv("PS2X_FASTTIMER"))
            runtime.replaceFunction(0x002baae8u, &bt3FastTimerCompWrite);
        // LZ-decompression cache (env PS2X_DECOMPCACHE) -- avoids re-decompressing the
        // menu/popup flash assets every frame (the real ~2fps bottleneck, FUN_00263278).
        if (std::getenv("PS2X_DECOMPCACHE"))
        {
            g_orig263278 = runtime.lookupFunction(0x00263278u);
            if (g_orig263278) runtime.replaceFunction(0x00263278u, &bt3DecompressCached);
        }
        // NOTE: bt3MovieLoadPoll (hook on FUN_00264af0) REVERTED again -- even guarded to
        // only tick with a valid adxf partition, installing it destabilizes the post-logos
        // path into the 0x3376b8 unregistered-PC crash (game never even reaches the movie-
        // load spin; the hook's tick never fires). The opening-movie AFS-load fix must not
        // go through FUN_00264af0. Left defined for reference.
        // Install the AFS/movie-load poll hook (PS2X_MOVIEHOOK). As a passthrough+logging it
        // confirms whether the demo-load streams via this path; with PS2X_MOVIEPUMP it drives
        // the ADX read to complete. Default off (the pump historically destabilized boot).
        if (std::getenv("PS2X_MOVIEHOOK"))
        {
            g_orig264af0 = runtime.lookupFunction(0x00264af0u);
            runtime.replaceFunction(0x00264af0u, &bt3MovieLoadPoll);
        }
        (void)&bt3MovieLoadPoll;
        (void)g_orig264af0;
        // SJX_Init IOP heap/DTX creation (stubbed to succeed so sound init proceeds).
        ps2_game_overrides::bindAddressHandler(runtime, 0x002B8CE0u, "ret1");
        ps2_game_overrides::bindAddressHandler(runtime, 0x0027B4A0u, "ret1");
        // Sound-driver lock/unlock callbacks corrupt the caller's stack; stub them.
        // (boundary fixed) 0x0026CB40
        // (boundary fixed) 0x0026CBC8
        // Virtual controller: connected + ready + per-player input, consistently
        // across all sceDbc pad accessors (see notes above).
        ps2_stubs::padConfigInit(runtime.getIoPaths().elfDirectory.string());
        runtime.replaceFunction(0x00295160u, &bt3PadConnect);
        runtime.replaceFunction(0x00296160u, &bt3PadStatus);
        runtime.replaceFunction(0x00296090u, &bt3PadRead);
        runtime.replaceFunction(0x00295fb8u, &bt3PadGetState);
        runtime.replaceFunction(0x00295e58u, &bt3PadCreateSocket);
    }

    // Dragon Ball Z: Budokai Tenkaichi 3 (SLUS_216.78): the PS2RNA sound engine
    // is a DTX/SJX URPC client (same middleware family as RECVX). It binds the
    // IOP sound RPC service sid=0x90000200 and drives it with URPC commands
    // (rpcNum 0x400..0x4FF, plus DTX create/destroy) then polls for completion.
    // With no real IOP, configure the runtime's DTX compat layer so its built-in
    // URPC/DTX emulation services those calls and PS2RNA_Init can finish, letting
    // the boot advance past the loading screen. urpcFnTableBase/urpcObjTableBase/
    // dispatcherFuncAddr are left 0 so the generic fallback emulation handles the
    // commands (no game-side dispatcher needed).
    void applyBt3DtxCompat(PS2Runtime &runtime)
    {
        (void)runtime;
        std::cerr << "[game_overrides] BT3: DTX/SJX sound URPC compat (sid=0x90000200)" << std::endl;
        PS2DtxCompatLayout layout{};
        layout.rpcSid = 0x90000200u;
        layout.urpcObjStride = 0x20u;
        ps2_syscalls::setDtxCompatLayout(layout);
    }

    // [mpegcb] The config lists every sceMpeg entry point as a stub, but only the ones Ghidra
    // recognised as functions got a generated thunk -- sceMpegAddCallback (0x297DD0) and
    // sceMpegAddStrCallback (0x29C250) did not. A `jal` to an unregistered address falls into
    // PS2Runtime's interpreter fallback, which happily runs the game's OWN libmpeg code, so the
    // movie player's callbacks were filed inside a library that never executes. Everything else
    // in the path (Init/DemuxPssRing/GetPicture/Reset/Flush) IS our stub, so the callbacks were
    // registered in one world and needed in the other, and the opening FMV froze about two
    // seconds in with a full ring nobody drained. Register the two registration entry points so
    // the callbacks land in the stub registry that sceMpegGetPicture actually dispatches from.
    // Deliberately narrow: sceMpegCreate and friends stay interpreted, because the guest library
    // builds the mpeg object our GetPicture stub writes through.
    // PS2X_MPEGCB=0 disables the dispatch side and makes this registration inert.
    template <void (*Stub)(uint8_t *, R5900Context *, PS2Runtime *)>
    void bt3MpegStubThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entryPc = ctx->pc;
        Stub(rdram, ctx, runtime);
        if (ctx->pc == entryPc)
            ctx->pc = getRegU32(ctx, 31);
    }

    void applyBt3MpegCallbackStubs(PS2Runtime &runtime)
    {
        struct MpegStubEntry
        {
            uint32_t address;
            PS2Runtime::RecompiledFunction fn;
            const char *name;
        };
        static const MpegStubEntry kEntries[] = {
            {0x00297DD0u, &bt3MpegStubThunk<&ps2_stubs::sceMpegAddCallback>, "sceMpegAddCallback"},
            {0x0029C250u, &bt3MpegStubThunk<&ps2_stubs::sceMpegAddStrCallback>, "sceMpegAddStrCallback"},
        };
        for (const MpegStubEntry &entry : kEntries)
        {
            if (runtime.hasFunction(entry.address))
                continue; // already generated -- leave it alone
            if (runtime.replaceFunction(entry.address, entry.fn))
                std::cerr << "[game_overrides] BT3: registered " << entry.name << " stub at 0x"
                          << std::hex << entry.address << std::dec << std::endl;
        }
    }

    // THE FIX -- see bt3CdStateEdge. PS2X_CDEDGE=0 keeps the hook installed but stops it
    // substituting the value, which is the A/B that attributes the fix to the substitution
    // rather than to the hook's timing.
    void applyBt3CdStateEdge(PS2Runtime &runtime)
    {
        g_orig270dd0 = runtime.lookupFunction(0x00270dd0u);
        if (g_orig270dd0 && runtime.replaceFunction(0x00270dd0u, &bt3CdStateEdge))
            std::cerr << "[game_overrides] BT3: CD read-state completion edge guard on func_270DD0"
                      << std::endl;
    }

    PS2_REGISTER_GAME_OVERRIDE("RECVX sound-driver compat", "slus_201.84", 0u, 0u, &applyRecvxSoundDriverCompat);
    PS2_REGISTER_GAME_OVERRIDE("RECVX DTX compat", "slus_201.84", 0u, 0u, &applyRecvxDtxCompat);
    PS2_REGISTER_GAME_OVERRIDE("LotR sound RPC compat", "SLUS_205.78", 0u, 0u, &applyLotrSoundRpcCompat);
    PS2_REGISTER_GAME_OVERRIDE("BT3 sound init bypass", "SLUS_216.78", 0u, 0u, &applyBt3SoundInitBypass);
    PS2_REGISTER_GAME_OVERRIDE("BT3 DTX sound URPC compat", "SLUS_216.78", 0u, 0u, &applyBt3DtxCompat);
    PS2_REGISTER_GAME_OVERRIDE("BT3 sceMpeg callback stubs", "SLUS_216.78", 0u, 0u, &applyBt3MpegCallbackStubs);
    // [nullpkt] The infinite-loading freeze: func_114860 (texture-packet address patcher) is
    // called with a NULL packet list (a1 = [obj+0x2C] not populated yet) and walks it as a
    // linked list from address 0 -- on hardware address 0 aliases the kernel's exception-vector
    // code, so the walk stumbles through non-zero garbage and exits; our RAM there is zeros, so
    // next-offset 0 loops forever ([stallprobe]: pc 0x1149a0, t6=0, all reads 0). Returning
    // immediately is the hardware outcome (nothing patched). PS2X_NULLPKT=0 disables.
    PS2Runtime::RecompiledFunction g_orig114860 = nullptr;
    PS2Runtime::RecompiledFunction g_orig113478 = nullptr;
    extern "C" void *ps2xGuestWaitBegin();
    extern "C" void ps2xGuestWaitEnd(void *);
    // Wait (yielding the guest execution token so the loader threads can run) until the 32-bit
    // field at `addr` becomes non-zero. Returns the value (0 after the cap).
    static uint32_t bt3WaitFieldNonZero(uint8_t *rdram, uint32_t addr, const char *what, uint32_t pc, R5900Context *ctx = nullptr, PS2Runtime *runtime = nullptr)
    {
        static const int s_capMs = [](){ const char *v = std::getenv("PS2X_NULLPKT_WAITMS"); return v ? std::atoi(v) : 1000; }();
        auto rd = [&]() -> uint32_t { uint32_t v = 0; if (const uint8_t *q = getConstMemPtr(rdram, addr)) std::memcpy(&v, q, 4); return v; };
        uint32_t v = rd();
        if (v != 0u) return v;
        static unsigned long n = 0; const unsigned long id = ++n;
        const auto t0 = std::chrono::steady_clock::now();
        int waited = 0;
        while (v == 0u && waited < s_capMs)
        {
            // [nullpkt-tick] the list is filled by the loader, whose CD reads only complete when the CD server
            // is ticked (see [spinpump]); the two 2026-08-28 loading hangs sat in this loop for the full cap with
            // the loader parked. Tick it every iteration, then yield the scheduler.
            if (ctx && runtime && runtime->hasFunction(0x0028a3b0u))
            {
                Bt3CdTickGuard tickGuard;
                if (tickGuard.engaged) { bt3RunCdTickInline(rdram, ctx, runtime); s_bt3CdTicking = false; }
            }
            void *scope = ps2xGuestWaitBegin();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ps2xGuestWaitEnd(scope);
            waited += 2;
            v = rd();
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (id <= 12) std::cerr << "[nullpkt] " << what << " at pc 0x" << std::hex << pc << " was NULL; waited " << std::dec << (int)ms
                                << " ms -> " << (v ? "populated, continuing" : "STILL NULL, skipping") << " (x" << id << ")" << std::endl;
        return v;
    }
    void bt3NullPacketGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_NULLPKT"); return !(v && v[0] == '0'); }();
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
        if (s_on && a0 == 0u && a1 == 0u)
        {
            // The known caller (0x1133ac) loads a1 from [s0+0x2C]; s0 is still live in the context.
            uint32_t v = 0u;
            if (ra == 0x1133b4u) v = bt3WaitFieldNonZero(rdram, getRegU32(ctx, 16) + 0x2Cu, "func_114860 packet list [s0+0x2C]", 0x114860u, ctx, runtime);
            if (v == 0u) { ctx->pc = ra; return; }
            ctx->r[5] = _mm_set_epi64x(0, (int64_t)(int32_t)v);   // low 64 bits = sign-extended 32-bit value, upper zero
        }
        if (g_orig114860) g_orig114860(rdram, ctx, runtime);
    }
    void bt3NullListGuard113478(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_NULLPKT"); return !(v && v[0] == '0'); }();
        const uint32_t a0 = getRegU32(ctx, 4);
        if (s_on && a0 != 0u)
        {
            const uint32_t v = bt3WaitFieldNonZero(rdram, a0 + 0x44u, "sub_113478 list [a0+0x44]", 0x113478u, ctx, runtime);
            if (v == 0u) { ctx->pc = getRegU32(ctx, 31); return; }
        }
        if (g_orig113478) g_orig113478(rdram, ctx, runtime);
    }
    void applyBt3NullPacketGuard(PS2Runtime &runtime)
    {
        g_orig114860 = runtime.lookupFunction(0x00114860u);
        if (g_orig114860 && runtime.replaceFunction(0x00114860u, &bt3NullPacketGuard))
            std::cerr << "[game_overrides] BT3: NULL packet-list guard on func_114860 (waits for the loader)" << std::endl;
        g_orig113478 = runtime.lookupFunction(0x00113478u);
        if (g_orig113478 && runtime.replaceFunction(0x00113478u, &bt3NullListGuard113478))
            std::cerr << "[game_overrides] BT3: NULL list guard on sub_113478 (waits for the loader)" << std::endl;
    }
    PS2_REGISTER_GAME_OVERRIDE("BT3 NULL packet-list guard", "SLUS_216.78", 0u, 0u, &applyBt3NullPacketGuard);
    PS2_REGISTER_GAME_OVERRIDE("BT3 CD read-state edge guard", "SLUS_216.78", 0u, 0u, &applyBt3CdStateEdge);
}
