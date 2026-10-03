#include "ps2_waitprof.h"   // [waitprof]
#include "runtime/ps2_statesync.h"   // [statesync]
extern "C" bool ps2xAudioFeedOn();   // [rollback] ps2_runtime.cpp: false while re-simulating / fast-forwarding (no device feed)
#include "ps2_runtime_macros.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2x_mods.h"   // [mods] loadable mods: install, frame hooks

// [guestbusy-tid] Defined in ps2_gs_gpu_renderer.cpp; declared HERE at file scope on purpose.
// Declaring it inside bt3FrameKick put it in this file's anonymous namespace, which silently
// created a SECOND internal-linkage symbol and failed at link with
// "undefined reference to (anonymous namespace)::g_guestThreadCpuNs".
extern std::atomic<uint64_t> g_guestThreadCpuNs;

// [skipforce] FMV-active latch (defined in ps2_gs_gpu.cpp); gates the forced skip button.
extern std::atomic<uint32_t> g_ps2FmvActive;
extern std::atomic<uint32_t> g_ps2ForceSkipFrames;   // [skipforce] countdown poked by the FMV override

// [framegate] vsync tick source, declared at file scope for the same reason as the above.
namespace ps2_syscalls { uint64_t GetCurrentVSyncTick(); }
extern std::chrono::steady_clock::time_point g_ps2xBootT0;
// [fightgate] FILE SCOPE (a block-scope extern inside this file's anonymous namespace would declare a different symbol).
bool ps2HalfStepFightActive();
void ps2HalfStepNoteLogic(uint64_t frame);
void ps2AddrWatchEnable(const char *hex);
void ps2StoreTraceEnable(const char *spec);
extern std::atomic<uint64_t> g_workerFrameNs;   // [framegate] kick worker busy ns, last frame
extern std::atomic<uint64_t> g_cdLoadReads, g_cdLoadBytes;
extern std::atomic<uint64_t> g_cdReadSerial;   // [cdedge3] CD.cpp
bool ps2xCdReadSince(uint32_t dst, uint64_t serialAfter);   // [cdedge3] CD.cpp   // [cdload] CD.cpp (file scope: a block-scope extern inside the namespace mangles into it)
extern std::atomic<uint32_t> g_bt3StateLive;    // [fightgate] BT3's top-level state, as seen by the status probe (ps2_runtime.cpp)
extern std::atomic<uint64_t> g_vu1PairCount;    // [fightgate] VU1 instruction pairs run by the fight's programs (ps2_vu1.cpp)
extern std::atomic<uint64_t> g_seamHostChunks;  // [fightgate] the seam's host mesh chunks (ps2_seammesh.cpp): the fight's render work when its VU1 programs are skipped
// [syncrelax] true while the frame gate is engaged (async kick on, gate on, worker frame > one vblank): the gate
// then owns the frame rate, so the busy-bit pacing and the sceGsSyncPath drain can let the guest run ahead.
std::atomic<bool> g_ps2xFrameGateHeavy{false};
extern "C" bool ps2xFrameGateHeavy() { return g_ps2xFrameGateHeavy.load(std::memory_order_relaxed); }

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
#include "Kernel/Stubs/MemoryCard.h"   // [savestate] getMemoryCardDebugSnapshot (deferred quickload)
#include "runtime/ps2x_dueldump.h"
#include "runtime/ps2_netplay.h"   // [netplay]
#include "runtime/ps2_seamprobe.h"  // [seamprobe]
#include "runtime/ps2x_achieve.h"  // [ach]
#include "runtime/ps2x_notify.h"   // [notify] the drop box

// [notify] Where the drop box lives: <exeDir>/savedata, the same directory settings.toml is in.
// The CWD is tried first because the front-end runs with the deploy as its working directory and a
// dev build run from the build tree should find the tree's savedata; the exe dir is the fallback for
// a launch from anywhere else. The current_path() branch is only taken if savedata actually exists
// there, so a stray savedata in an unrelated working directory cannot shadow the real one.
extern "C" const char *ps2xDropBoxDir()
{
    static const std::string s_dir = []() {
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path() / "savedata";
        if (std::filesystem::is_directory(cwd, ec) && !ec)
            return cwd.string();
        extern const char *ps2xExeDirC();   // main.cpp: <exeDir> (honors PS2X_EXEDIR)
        const char *xd = ps2xExeDirC();
        if (xd && xd[0])
            return (std::filesystem::path(xd) / "savedata").string();
        return cwd.string();
    }();
    return s_dir.c_str();
}

// [netjump] Frames of display HOLD remaining. While non-zero, GsGpuRenderer::swapFrame() returns
// immediately, so the screen keeps showing the last presented frame. The menu transition needs a
// few frames of real menu (the duel module's object only exists while the versus menu is up, and
// func_356090 loads character select's assets on the confirm) -- this hides those frames instead
// of pretending they are not needed. Counts DOWN in swapFrame so a failed transition cannot
// freeze the picture forever.
std::atomic<int> g_netJumpHold{0};

// [netjump] The overlay curtain's TARGET, not its level: 1 = cover the screen with black and show
// "Loading..." while the transition runs, 0 = take it away again. The level lives with whoever
// draws (ps2_settings_overlay.cpp), because a fade needs a frame clock and this file is the
// guest's, not the renderer's -- and because the reverse has to be smooth on the way out too,
// which is the give-up path, and that one lives here.
//
// This is a black RECTANGLE over the game, not the hold above. The hold stops the frame being
// published, which freezes the picture on the main menu; this covers whatever is there with
// something that says what is happening. Both are wanted: the hold is what keeps the versus menu
// from being seen operated, the curtain is what the player looks at while it happens.
std::atomic<int> g_netCurtainWant{0};


#define XXH_INLINE_ALL
#include "thirdparty/xxhash.h"   // [dethash]
#include "runtime/ps2_gs_gpu_renderer.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include "runtime/ps2_gs_pgs.h"   // [steporacle]
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
#include <cstdlib>   // [skipforce] std::strtoul

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
    uint32_t ps2RandCallCount();   // [dethash]
    uint64_t ps2RandState();      // [dethash]
    void     ps2RandRestore(uint64_t state, uint32_t calls);   // [savestate]
}

// External-linkage game-frame counter (read by the [fps] line in ps2_runtime.cpp).
std::atomic<uint64_t> g_bt3FrameCount{0};
uint64_t ps2FightTicks();                                       // [fighttick] ps2_stepcensus.cpp
extern "C" void ps2xSeamTickMark(unsigned long long tick);      // [fighttick] ps2_seamgs.cpp

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

// [rumble] the launcher setting as PS2SettingsOverlay::applySettings pushes it; read by bt3PadSendRumble every frame
std::atomic<bool> g_ps2xRumbleOn{true};
std::atomic<int> g_ps2xRumbleStrength{100};

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
    // [inrec] 12-byte records: frame, player, buttons, rx, ry, lx, ly
    #pragma pack(push, 1)
    struct Ps2xPadSample { uint32_t frame; uint8_t player, pad0; uint16_t buttons; uint8_t rx, ry, lx, ly; };
    #pragma pack(pop)
    static_assert(sizeof(Ps2xPadSample) == 12, "pad sample must stay 12 bytes");
    static std::mutex g_inRecMtx;
    static void ps2xInRecWrite(uint32_t frame, uint32_t player, uint16_t buttons, uint8_t rx, uint8_t ry, uint8_t lx, uint8_t ly)
    {
        static std::FILE *f = [](){ const char *v = std::getenv("PS2X_INREC");
            if (!v || !v[0]) { std::fprintf(stderr, "[inrec] PS2X_INREC not set -- not recording\n"); return (std::FILE *)nullptr; }
            std::FILE *h = std::fopen(v, "wb");
            std::fprintf(stderr, h ? "[inrec] recording pad input to %s\n" : "[inrec] FAILED to open %s\n", v);
            return h; }();
        if (!f) return;
        const Ps2xPadSample e{frame, static_cast<uint8_t>(player), 0u, buttons, rx, ry, lx, ly};
        std::lock_guard<std::mutex> lk(g_inRecMtx);
        std::fwrite(&e, sizeof e, 1, f);
        std::fflush(f);
    }
    static const std::map<uint64_t, Ps2xPadSample> &ps2xInPlayMap()
    {
        static const std::map<uint64_t, Ps2xPadSample> m = [](){
            std::map<uint64_t, Ps2xPadSample> out;
            const char *v = std::getenv("PS2X_INPLAY");
            if (!v || !v[0]) return out;
            std::FILE *h = std::fopen(v, "rb");
            if (!h) { std::fprintf(stderr, "[inrec] FAILED to open %s for replay\n", v); return out; }
            Ps2xPadSample e{};
            while (std::fread(&e, sizeof e, 1, h) == 1) out[(uint64_t(e.frame) << 8) | e.player] = e;
            std::fclose(h);
            std::fprintf(stderr, "[inrec] replaying %zu pad samples from %s\n", out.size(), v);
            return out; }();
        return m;
    }
    static bool ps2xInPlayActive() { static const bool on = !ps2xInPlayMap().empty(); return on; }
    static bool ps2xInPlayLookup(uint32_t frame, uint32_t player, Ps2xPadSample &out)
    {
        const auto &m = ps2xInPlayMap();
        const auto it = m.find((uint64_t(frame) << 8) | uint8_t(player));
        if (it == m.end()) return false;
        out = it->second; return true;
    }

    // [netjump] frames of synthetic CROSS remaining, consumed by writeNeutralPadPacket.
    // Everything about the destination is set directly (screen state, versus mode, battle type);
    // this is only the CONFIRM, because func_356090 loads character select's assets and the duel
    // module polls it -- it completes on a press, and no amount of variable writing substitutes
    // for that loading. One press at a state we chose and can verify, not menu navigation.
    std::atomic<int> g_netJumpPressCross{0};

    // [netjump] The player asking to abort, from CIRCLE. The ONLY new pad state this work needs:
    // the freeze-with-an-exception and the two synthetic presses are the ones that already existed
    // for the retired custom page (g_netMenuGate / g_netMenuPressFrames, kept for exactly this),
    // and reusing them is the difference between one pad seam and two that can disagree.
    //
    // It has to be a flag because the jump runs on the guest thread with no view of the pad, and the
    // seam is the only place that sees one.
    std::atomic<int> g_netJumpCancel{0};
// [netmenu] frames of synthetic CROSS remaining for the custom page's direct-subtype start.
// Same seam as the netjump's press, but NOT gated by netplay: BT3 never calls libpad, so the
// only pad the game sees is built in writeNeutralPadPacket below.
// [netmenu] synthetic button frames for the retired custom page. Same seam as the netjump's press,
// but NOT gated by netplay: BT3 never calls libpad, so the only pad the game sees is built in
// writeNeutralPadPacket below. Nothing drives these any more (the page that used them is gone), so
// the mask stays 0 and this is inert; kept because it is the pad seam an entry test would need.
std::atomic<uint32_t> g_netMenuPressMask{0};   // PS2 button mask (active low: clear the bit)
std::atomic<int> g_netMenuPressFrames{0};
extern "C" void ps2xNetMenuPress(int mask, int frames)
{
    g_netMenuPressMask.store((uint32_t)mask, std::memory_order_relaxed);
    g_netMenuPressFrames.store(frames > 0 ? frames : 0, std::memory_order_relaxed);
}
// [netmenu] while a custom page owns the screen the guest must see NO input at all (the libpad
// override is invisible to BT3, so this is the only place that can freeze it). The Cross pulse
// above is applied AFTER the freeze, so the start sequence can still confirm. Unused for now --
// see g_netMenuPressMask. The freeze releases buttons, both sticks and every socket, not just
// player 1: a freeze that only cleared buttons leaked the sticks, because the only thing that used
// to neutralise them (the gate below) is player 1 only.
std::atomic<int> g_netMenuFreeze{0};
extern "C" void ps2xNetMenuFreeze(int on)
{ g_netMenuFreeze.store(on ? 1 : 0, std::memory_order_relaxed); }
// [netmenu] Selective gate: while armed, ONLY the buttons in the allow mask pass through to the
// guest (player 1 / socket 0); everything else is forced neutral. Unused for now -- see
// g_netMenuPressMask.
std::atomic<int> g_netMenuGate{0};
std::atomic<uint32_t> g_netMenuAllowMask{0};
extern "C" void ps2xNetMenuGate(int on, int allowMask)
{
    g_netMenuAllowMask.store((uint32_t)allowMask, std::memory_order_relaxed);
    g_netMenuGate.store(on ? 1 : 0, std::memory_order_relaxed);
}

// [netmenu] Conditional AFS serve. While the net entry is active ONE chosen AFS slot is served
// from an in-memory image instead of the folder file, so the SAME game code (the voice/BGM/loader
// that already knows this container) runs on our converted Wii data -- and a native entry into the
// same screen keeps the original bytes untouched. Zero-fills past the image end.
// [netmenu] True only while the NET entry owns the screen. The [slot-read] trace keys off it, so
// the log stays clean: it shows the reads that happen because the net entry was pressed.
std::atomic<int> g_netEntryActive{0};
extern "C" void ps2xNetEntrySetActive(int on)
{ g_netEntryActive.store(on ? 1 : 0, std::memory_order_relaxed); }
extern "C" int ps2xNetEntryActive()
{ return g_netEntryActive.load(std::memory_order_relaxed); }

// [netmenu] Drive the game's OWN SE playback from the host menu (no decoded WAVs).
// The guest-side sePlay() needs rdram + runtime; bt3FrameKick() stashes them every frame.
uint8_t *g_ps2xMenuRdram = nullptr;
PS2Runtime *g_ps2xMenuRuntime = nullptr;
// While the net entry freezes the game audio (sfx volume 0), keep the reserved SE stream audible
// so the menu's effects play through the game's own mixer. Read by PS2AudioBackend.
std::atomic<int> g_ps2xSeMenuBypass{0};
extern "C" void ps2xSeMenuBypass(int on) { g_ps2xSeMenuBypass.store(on ? 1 : 0, std::memory_order_relaxed); }
extern "C" int ps2xSeMenuBypassGet() { return g_ps2xSeMenuBypass.load(std::memory_order_relaxed); }

std::atomic<int> g_netServeSwapActive{0};
std::atomic<uint64_t> g_netServeSwapSlot{0};
std::shared_ptr<std::vector<uint8_t>> g_netServeSwapData;
extern "C" void ps2xNetServeSwapLoad(const char *path, unsigned long long slot)
{
    auto image = std::make_shared<std::vector<uint8_t>>();
    if (path && path[0])
    {
        std::ifstream f(path, std::ios::binary);
        if (f.is_open())
        {
            f.seekg(0, std::ios::end);
            const std::streamoff n = f.tellg();
            f.seekg(0, std::ios::beg);
            if (n > 0)
            {
                image->resize(static_cast<size_t>(n));
                f.read(reinterpret_cast<char *>(image->data()), n);
            }
        }
    }
    g_netServeSwapData = std::move(image);
    g_netServeSwapSlot.store(slot, std::memory_order_relaxed);
    g_netServeSwapActive.store(g_netServeSwapData->empty() ? 0 : 1, std::memory_order_relaxed);
    std::fprintf(stderr, "[netmenu] serve-swap: slot %llu <- %s (%zu bytes)%s\n",
                 slot, path ? path : "(none)", g_netServeSwapData->size(),
                 g_netServeSwapData->empty() ? " [INACTIVE: empty]" : "");
}
extern "C" void ps2xNetServeSwapOff()
{
    g_netServeSwapActive.store(0, std::memory_order_relaxed);
    std::fprintf(stderr, "[netmenu] serve-swap: off (original bytes)\n");
}
// [netmenu] Extra AFS slots silenced while the net entry is active: every OTHER BGM stream the game
// keeps playing is served as zeros, so only the requested (swapped) one is heard.
uint64_t g_netServeMuteSlots[16] = {};
std::atomic<int> g_netServeMuteN{0};
extern "C" void ps2xNetServeMuteSlots(const char *csv)
{
    int n = 0;
    if (csv && csv[0])
    {
        const char *p = csv;
        while (*p && n < 16)
        {
            char *end = nullptr;
            const unsigned long long v = std::strtoull(p, &end, 0);
            if (end == p) break;
            g_netServeMuteSlots[n++] = (uint64_t)v;
            p = (*end == ',') ? end + 1 : end;
            if (*end == '\0') break;
        }
    }
    g_netServeMuteN.store(n, std::memory_order_relaxed);
    std::fprintf(stderr, "[netmenu] serve-swap: muting %d other BGM slot(s): %s\n", n, csv ? csv : "");
}
extern "C" int ps2xNetServeSwapRead(unsigned long long slotId, unsigned long long off,
                                    unsigned char *dst, unsigned long long n)
{
    if (g_netServeSwapActive.load(std::memory_order_relaxed) == 0 || !dst)
        return 0;
    // [netmenu] The swap slot AND every "muted" slot are served the SAME image -- a valid silent
    // stream. Serving raw zeros here produced decoder NOISE, not silence.
    bool useImage = (slotId == g_netServeSwapSlot.load(std::memory_order_relaxed));
    if (!useImage)
    {
        const int mn = g_netServeMuteN.load(std::memory_order_relaxed);
        for (int i = 0; i < mn; ++i)
            if (slotId == g_netServeMuteSlots[i]) { useImage = true; break; }
    }
    if (!useImage)
        return 0;
    static std::atomic<uint64_t> s_hits{0};
    const uint64_t hit = s_hits.fetch_add(1, std::memory_order_relaxed);
    if (hit < 12)
        std::fprintf(stderr, "[netmenu] serve-swap HIT #%llu slot=%llu off=%llu n=%llu\n",
                     hit, slotId, off, n);
    const std::vector<uint8_t> &img = *g_netServeSwapData;
    if (off >= img.size())
        std::memset(dst, 0, static_cast<size_t>(n));
    else
    {
        const size_t avail = static_cast<size_t>(std::min<unsigned long long>(img.size() - off, n));
        std::memcpy(dst, img.data() + static_cast<size_t>(off), avail);
        if (avail < n) std::memset(dst + avail, 0, static_cast<size_t>(n - avail));
    }
    return 1;
}
    // [statesync] 0 = no jump this session, 1 = jumping, 2 = settled / gave up. The state sync waits for
    // 2 on both sides: the host publishes AFTER its jump (so the joiner adopts character select), the
    // joiner adopts only once its own jump has it in the same screen (comparable call chains).
    std::atomic<int> g_netJumpState{0};
    std::atomic<uint32_t> g_netJumpSession{0};   // the netplay session the state belongs to
    extern "C" int ps2xNetJumpState() { return g_netJumpState.load(std::memory_order_relaxed); }
    // True once THIS session's jump has settled (or given up). A session the jump has not looked at yet
    // (it runs from the frame hook, after the boundary that first sees the peer) counts as not settled.
    extern "C" int ps2xNetJumpSettledFor(uint32_t session)
    { return g_netJumpSession.load(std::memory_order_relaxed) == session && g_netJumpState.load(std::memory_order_relaxed) == 2; }

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
        // [netplay] The LOCAL player's buttons always come from this machine's PRIMARY device (player-1
        // config): a joiner is player 2, whose socket would otherwise poll the second-gamepad slot and
        // read nothing while the only controller sits on slot 0.
        const int liveSlot = (ps2NetActive() && static_cast<int>(socket & 3u) + 1 == ps2NetLocalPlayer()) ? 0 : static_cast<int>(socket & 3u);
        const uint16_t buttons = ps2_stubs::ps2xLivePadButtons(liveSlot, lx, ly, rx, ry);
        uint8_t b0 = static_cast<uint8_t>(buttons & 0xffu);
        uint8_t b1 = static_cast<uint8_t>((buttons >> 8) & 0xffu);
        // TEST (env PS2X_AUTOSTART): also tap START+CROSS periodically to auto-advance.
        static const bool s_autostart = [](){ const char *v = std::getenv("PS2X_AUTOSTART"); return v && v[0] && v[0] != '0'; }();
        if (s_autostart)
        {
            // In a fight START would only pause it: tap attacks and directions in bursts instead, so the fight stays live and busy (effects, blur,
            // glow) for unattended captures and the packet oracle.
            // The fight is on when the game's state object reads 0x8 (g_bt3StateLive, ps2_runtime.cpp status line); every
            // other state (menus 0x7, fight-load 0x27, ...) still gets the START+CROSS taps that drive the flow.
            static std::atomic<uint32_t> s_n{0};
            const uint32_t n = s_n.fetch_add(1);
            if (g_bt3StateLive.load(std::memory_order_relaxed) != 0x8u)
            {
                if ((n % 180u) < 12u)
                {
                    b0 = static_cast<uint8_t>(b0 & ~0x08u); // START
                    b1 = static_cast<uint8_t>(b1 & ~0x40u); // CROSS
                }
            }
            else
            {
                const uint32_t phase = (n / 30u) % 8u;   // half-second bursts
                if (phase == 0u || phase == 4u) b1 = static_cast<uint8_t>(b1 & ~0x80u);   // SQUARE (attack)
                if (phase == 1u) b1 = static_cast<uint8_t>(b1 & ~0x10u);                  // TRIANGLE
                if (phase == 2u) b0 = static_cast<uint8_t>(b0 & ~0x20u);                  // LEFT
                if (phase == 5u) b0 = static_cast<uint8_t>(b0 & ~0x80u);                  // RIGHT
                if (phase == 6u) { b1 = static_cast<uint8_t>(b1 & ~0x40u); b1 = static_cast<uint8_t>(b1 & ~0x80u); }   // CROSS+SQUARE (ki blast / rush)
            }
        }
        // [skipforce] Force the skip button. Either PS2X_FORCE_SKIP=<n> (holds while the FMV is
        // live for the first <n> pad reads) or the g_ps2ForceSkipFrames countdown the FMV override
        // pokes to end the native movie. PS2X_FORCE_SKIP_BTNS=<hex> overrides the cleared bits.
        bool forceSkip = false;
        static const bool s_skipEnv = std::getenv("PS2X_FORCE_SKIP") != nullptr;
        if (s_skipEnv && g_ps2FmvActive.load(std::memory_order_relaxed) != 0u)
        {
            static const uint32_t s_skipFrames = [](){
                const char *v = std::getenv("PS2X_FORCE_SKIP");
                return v ? (uint32_t)std::strtoul(v, nullptr, 10) : 0u; }();
            static std::atomic<uint32_t> s_done{0};
            if (s_done.fetch_add(1) < s_skipFrames) forceSkip = true;
        }
        else if (g_ps2ForceSkipFrames.load(std::memory_order_relaxed) > 0u)
        {
            g_ps2ForceSkipFrames.fetch_sub(1u, std::memory_order_relaxed);
            forceSkip = true;
        }
        if (forceSkip)
        {
            static const uint32_t s_skipMask = [](){
                const char *v = std::getenv("PS2X_FORCE_SKIP_BTNS");
                return v ? (uint32_t)std::strtoul(v, nullptr, 16) : 0x4008u; }();
            b0 = static_cast<uint8_t>(b0 & ~(uint8_t)(s_skipMask & 0xffu));
            b1 = static_cast<uint8_t>(b1 & ~(uint8_t)((s_skipMask >> 8) & 0xffu));
        }
        // [inrec] Deterministic input record/replay -- THE NETPLAY SEAM.
        // NOTE: BT3 does NOT use libpad. scePadRead/readPadPortData in Kernel/Stubs/Pad.cpp are
        // never called (verified: the trace printed no entry at all); the game reads pads through
        // its own FUN_00296090 / FUN_00295fb8, which land here. A hook in Pad.cpp therefore
        // records nothing -- that cost three user runs before the startup banner made it obvious.
        //   PS2X_INREC=<file>   capture per frame       PS2X_INPLAY=<file>   feed it back
        // Later the remote player's buttons arrive here off a socket instead of out of a file.
        // [netplay] When a peer is connected, the LOCAL player's buttons come from this machine's
        // pad and are sent to the peer; the REMOTE player's arrive over UDP. The game reads two
        // pads and cannot tell the difference. Input sampled now is applied delay frames later,
        // so the packet has that long to cross the network.
        // [netmenu] the custom page owns input: release everything for player 1 before the pulse below
    // (and before the netplay block, so it holds offline too).
    // [netmenu] the custom page owns input: release everything, every socket and both sticks, not
    // just player 1's buttons -- the gate below is player 1 only, so a freeze that only cleared
    // b0/b1 leaked the sticks. (And before the netplay block, so it holds offline too.)
    if (g_netMenuFreeze.load(std::memory_order_relaxed) > 0)
    {
        b0 = 0xFFu;
        b1 = 0xFFu;
        rx = 0x80u; ry = 0x80u; lx = 0x80u; ly = 0x80u;
    }
    // [netmenu] Selective gate (see ps2xNetMenuGate): deny everything except the allowed buttons,
    // and force the sticks neutral so the hidden state's menu cannot be navigated.
    if ((socket & 3u) == 0u && g_netMenuGate.load(std::memory_order_relaxed) > 0)
    {
        const uint32_t allow = g_netMenuAllowMask.load(std::memory_order_relaxed);
        const uint8_t a0 = static_cast<uint8_t>(allow & 0xFFu);
        const uint8_t a1 = static_cast<uint8_t>((allow >> 8) & 0xFFu);
        b0 = static_cast<uint8_t>(static_cast<uint8_t>(~a0) | (b0 & a0));
        b1 = static_cast<uint8_t>(static_cast<uint8_t>(~a1) | (b1 & a1));
        rx = 0x80u; ry = 0x80u; lx = 0x80u; ly = 0x80u;
    }
    // [netjump] CIRCLE is the cancel. It is SWALLOWED here and nothing is sent to the guest: the
    // button the game needs is triangle, and step 4 already pulses triangle until the game reaches
    // 0x04. Injecting a press here as well would give two independent triangle sources for one
    // cancel, and the second one to arrive would be a stray press on whatever screen the first one
    // landed on.
    //
    // Letting CIRCLE through instead is not an option: it means "abort the netplay transition",
    // which is not a thing the versus menu has an opinion about, and the game would act on a
    // button the player never pressed on this screen.
    //
    // A COUNTER, and it counts every edge. It was a flag guarded by `== 0` here, which meant the
    // first press set it and this block then stopped looking -- so a second press was invisible and
    // the two-press escape could not exist. The seam's only job is to notice presses; deciding what
    // each one means belongs to whichever step of the jump is running.
    //
    // Only the edge fires, so holding circle does not re-send. Once per frame, on socket 0: the seam
    // runs per socket and a per-socket read would fire three times over on a three-pad game.
    if ((socket & 3u) == 0u)
    {
        static uint16_t s_prevHigh = 0xFFu;
        // Active low: a press is the bit going from 1 to 0. Bit 13 is CIRCLE, bit 5 of the high byte.
        const bool circleDown = (s_prevHigh & 0x20u) != 0u && (b1 & 0x20u) == 0u;
        s_prevHigh = b1;
        if (circleDown)
        {
            g_netJumpCancel.fetch_add(1, std::memory_order_relaxed);
            b1 = static_cast<uint8_t>(b1 | 0x20u);   // keep CIRCLE released: the guest must not see it
        }
    }

    // [netmenu] synthetic button (player 1 / socket 0). Applied here, NOT inside the netplay
    // block, so it works offline too. Runs AFTER the freeze above, so a press still lands while
    // the guest input is held.
    if ((socket & 3u) == 0u && g_netMenuPressFrames.load(std::memory_order_relaxed) > 0)
    {
        g_netMenuPressFrames.fetch_sub(1, std::memory_order_relaxed);
        const uint32_t m = g_netMenuPressMask.load(std::memory_order_relaxed);
        b0 = static_cast<uint8_t>(b0 & ~(uint8_t)(m & 0xFFu));
        b1 = static_cast<uint8_t>(b1 & ~(uint8_t)((m >> 8) & 0xFFu));
    }
    if (ps2NetActive())
        {
            const uint32_t frame = static_cast<uint32_t>(g_bt3FrameCount.load(std::memory_order_relaxed));
            const int pl = static_cast<int>(socket & 3u) + 1;          // socket 0/1 -> player 1/2
            // [netjump] The jump's confirm press is a P1 press (the versus menu listens to player 1 only).
            // In lockstep the host's P1 press crosses the wire, so injecting on the LOCAL player was right;
            // while a state sync is pending nothing crosses and each side drives its own menus, so it must
            // be injected on socket 0 whoever we are.
            const bool injectHere = ps2NetSyncPending() ? (pl == 1) : (pl == ps2NetLocalPlayer());
            if (injectHere && g_netJumpPressCross.load(std::memory_order_relaxed) > 0)
            {
                g_netJumpPressCross.fetch_sub(1, std::memory_order_relaxed);
                b1 = static_cast<uint8_t>(b1 & ~0x40u);   // CROSS (active low), bit 14
            }
            if (pl == ps2NetLocalPlayer())
            {
                Ps2xNetInput live{static_cast<uint16_t>(b0 | (uint16_t(b1) << 8)), rx, ry, lx, ly};
                Ps2xNetInput canned{};
                if (ps2NetAutoInput(canned)) live = canned;   // [netplay] host-driven auto-start
                ps2NetSubmitLocal(frame, live);
            }
            Ps2xNetInput use{};
            if (ps2NetGetInput(frame, pl, use))
            {
                b0 = static_cast<uint8_t>(use.buttons & 0xFFu);
                b1 = static_cast<uint8_t>((use.buttons >> 8) & 0xFFu);
                rx = use.rx; ry = use.ry; lx = use.lx; ly = use.ly;
            }
            p[0] = b0; p[1] = b1; p[2] = rx; p[3] = ry; p[4] = lx; p[5] = ly;
            return;
        }
        {
            const uint32_t plyr = socket & 3u;
            uint16_t recB = static_cast<uint16_t>(b0 | (uint16_t(b1) << 8));
            if (ps2xInPlayActive())
            {
                Ps2xPadSample sm{};
                if (ps2xInPlayLookup(static_cast<uint32_t>(g_bt3FrameCount.load(std::memory_order_relaxed)), plyr, sm))
                { recB = sm.buttons; rx = sm.rx; ry = sm.ry; lx = sm.lx; ly = sm.ly; }
                else
                { recB = 0xFFFFu; rx = ry = lx = ly = 0x80u; }   // neutral, never a live pad
                b0 = static_cast<uint8_t>(recB & 0xFFu);
                b1 = static_cast<uint8_t>((recB >> 8) & 0xFFu);
            }
            else
                ps2xInRecWrite(static_cast<uint32_t>(g_bt3FrameCount.load(std::memory_order_relaxed)), plyr, recB, rx, ry, lx, ly);
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

    // [rumble] FUN_00122f10(player): the game's per-frame pad SEND. The fight code accumulates the big motor's
    // strength as a float at rec+0x160 (FUN_00122e88 adds each hit's pulse, clamped to 1.0) and the small motor's
    // on/off at rec+0x164 (rec = 0x333800 + player*0x1C0); this function folds them into (big*127)<<1 | small,
    // hands the byte pair to libdbc's per-socket "send data" (code 0x0103400b) for the IOP's DualShock poll, and
    // zeroes both. The IOP side goes nowhere here (no SIO2), so the motors never moved. Replaced whole: the same
    // fields go to the host pad's motors instead and the RPC is skipped (it was two IOP calls per frame doing
    // nothing). The game's own Vibration option still gates it: sub_001C02C8 sets fighter+0x15D0 from the options
    // word bit (player+1), and sub_001DC5E0 only calls the accumulator when that flag is set.
    //   PS2X_RUMBLE=0 disables, PS2X_RUMBLE_SCALE=<f> scales both motors (default 1), PS2X_RUMBLE_LOG=1 prints requests.
    // [tagteam] The Tag Team port (stages 1-30) lives in ps2xRuntime/mods/tagteam/tagteam.cpp, a loadable mod (ps2x_mod_api.h).
    // [rumble] PS2X_RUMBLE_FORCE=1 (test knob): sub_001DC5E0(fighter) is the per-fighter vibration driver; it bails
    // unless fighter+0x15D0 (set at fighter init from the game's Vibration option bits) is nonzero. Forcing the flag
    // on entry exercises the whole chain on a rig that has no memory card with the option turned on.
    PS2Runtime::RecompiledFunction g_orig1dc520 = nullptr, g_orig1dc578 = nullptr;
    void bt3VibStartBigTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_001DC520(fighter, f12 strength, f13 seconds)
    {
        static std::atomic<uint32_t> s_n{0}; const uint32_t n = s_n.fetch_add(1u);
        if (n < 40u) std::fprintf(stderr, "[rumble-start] big fighter 0x%x strength %.2f seconds %.2f\n", getRegU32(ctx, 4), ctx->f[12], ctx->f[13]);
        if (g_orig1dc520) g_orig1dc520(rdram, ctx, runtime); else ctx->pc = getRegU32(ctx, 31);
    }
    void bt3VibStartSmallTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_001dc578(fighter, f12 seconds)
    {
        static std::atomic<uint32_t> s_n{0}; const uint32_t n = s_n.fetch_add(1u);
        if (n < 40u) std::fprintf(stderr, "[rumble-start] small fighter 0x%x seconds %.2f\n", getRegU32(ctx, 4), ctx->f[12]);
        if (g_orig1dc578) g_orig1dc578(rdram, ctx, runtime); else ctx->pc = getRegU32(ctx, 31);
    }
    PS2Runtime::RecompiledFunction g_orig1dc5e0 = nullptr;
    void bt3VibrationDriverForced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_001DC5E0
    {
        if (uint8_t *p = getMemPtr(rdram, getRegU32(ctx, 4) + 0x15D0u)) { const uint32_t one = 1u; std::memcpy(p, &one, 4); }
        {   // trace: the other gates of the driver, every 120th call per fighter
            static std::atomic<uint32_t> s_n{0}; const uint32_t n = s_n.fetch_add(1u);
            if ((n % 120u) == 0u)
            {
                const uint32_t f = getRegU32(ctx, 4);
                auto r32 = [&](uint32_t a) -> uint32_t { const uint8_t *p = getMemPtr(rdram, a); uint32_t v = 0; if (p) std::memcpy(&v, p, 4); return v; };
                std::fprintf(stderr, "[rumble-drv] call %u fighter 0x%x player %u +1278=%u timers big %u small %u base %08x pauseflag[0x31be04]=%u\n", n, f, r32(f + 4u), r32(f + 0x1278u), r32(f + 0x15D8u), r32(f + 0x15DCu), r32(f + 0x15D4u), r32(0x31be04u));
            }
        }
        if (g_orig1dc5e0) g_orig1dc5e0(rdram, ctx, runtime);
        else ctx->pc = getRegU32(ctx, 31);
    }
    void bt3PadSendRumble(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00122f10
    {
        (void)runtime;
        // the launcher's Pads page (settings.toml [controllers] rumble / rumble_strength) unless the env says otherwise
        static const int s_envOn = [](){ const char *v = std::getenv("PS2X_RUMBLE"); return v && v[0] ? (v[0] == '0' ? 0 : 1) : -1; }();
        static const float s_envScale = [](){ const char *v = std::getenv("PS2X_RUMBLE_SCALE"); return v && v[0] ? (float)std::atof(v) : -1.0f; }();
        const bool s_on = s_envOn >= 0 ? s_envOn == 1 : ::g_ps2xRumbleOn.load(std::memory_order_relaxed);
        float s_scale = s_envScale >= 0.0f ? s_envScale : ::g_ps2xRumbleStrength.load(std::memory_order_relaxed) / 100.0f;
        if (s_scale > 4.0f) s_scale = 4.0f;
        static const bool s_log = [](){ const char *v = std::getenv("PS2X_RUMBLE_LOG"); return v && v[0] == '1'; }();
        const uint32_t player = getRegU32(ctx, 4) & 3u;
        const uint32_t rec = 0x333800u + player * 0x1C0u;
        float big = 0.0f; uint32_t small = 0u;
        if (const uint8_t *p = getMemPtr(rdram, rec + 0x160u)) std::memcpy(&big, p, 4);
        if (const uint8_t *p = getMemPtr(rdram, rec + 0x164u)) std::memcpy(&small, p, 4);
        if (!(big >= 0.0f)) big = 0.0f;   // NaN guard
        if (big > 1.0f) big = 1.0f;
        static uint16_t s_lastLow[4] = {}, s_lastHigh[4] = {};
        // The game drives the small motor the DualShock way: on one frame, off the next, for the pulse's duration
        // (sub_001DC5E0 toggles it from a frame counter). A DualShock's small motor cannot stop in 16 ms so that
        // reads as "on"; a modern pad's high-frequency motor can, and it would buzz at 30 Hz. Hold it on for 100 ms
        // past the last "on" frame instead.
        static std::chrono::steady_clock::time_point s_smallUntil[4] = {};
        const auto now = std::chrono::steady_clock::now();
        if (small) s_smallUntil[player] = now + std::chrono::milliseconds(100);
        const bool smallOn = now < s_smallUntil[player];
        const float sb = big * s_scale, ss = (smallOn ? 1.0f : 0.0f) * s_scale;
        const uint16_t low = (uint16_t)(sb >= 1.0f ? 65535u : (uint32_t)(sb * 65535.0f));
        const uint16_t high = (uint16_t)(ss >= 1.0f ? 65535u : (uint32_t)(ss * 65535.0f));
        if (s_on && (low || high || s_lastLow[player] || s_lastHigh[player]))
        {   // refreshed every frame while active; 60 ms outlives a dropped frame and stops on its own if the game stops
            ps2_stubs::padRumblePlayer((int)player, low, high, (low || high) ? 60u : 0u);
            if (s_log && (low != s_lastLow[player] || high != s_lastHigh[player]))
                std::fprintf(stderr, "[rumble] player %u big %.2f small %u -> low %u high %u\n", player, big, small, low, high);
        }
        s_lastLow[player] = low; s_lastHigh[player] = high;
        // what the original did with the fields: fold into the halfword and clear
        uint32_t v = (uint32_t)(big * 127.0f); if (v > 127u) v = 127u;
        const uint16_t half = (uint16_t)((v << 1) | (small ? 1u : 0u));
        if (uint8_t *p = getMemPtr(rdram, rec + 0x12Cu)) std::memcpy(p, &half, 2);
        if (uint8_t *p = getMemPtr(rdram, rec + 0x160u)) std::memset(p, 0, 4);
        if (uint8_t *p = getMemPtr(rdram, rec + 0x164u)) std::memset(p, 0, 4);
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
    std::mutex g_sndRateM;                                                            // [rollback] the consumer's per-sink feed
    std::map<uint32_t, std::chrono::steady_clock::time_point> g_sndRateLast;          //   rate limiter, snapshotted with the rest
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
        // [sndwatch] PS2X_SNDWATCH=<hex addr>: name the HOST-side writer of one sound-block slot.
        // The guest-store watch (PS2X_ADDRWATCH) is silent for these because the runtime writes
        // them directly into guest RAM, bypassing the recompiled store path entirely.
        {
            static const uint32_t s_w = [](){ const char *v = std::getenv("PS2X_SNDWATCH");
                                              return (v && v[0]) ? (uint32_t)std::strtoul(v, nullptr, 16) : 0u; }();
            if (s_w && (addr & 0x1FFFFFFFu) == (s_w & 0x1FFFFFFFu))
            {
                static std::atomic<uint32_t> s_n{0};
                if (s_n.fetch_add(1u) < 25u)
                {
                    uint32_t old32 = 0; std::memcpy(&old32, rdram + (addr & 0x1FFFFFFFu), 4);
                    std::fprintf(stderr, "[sndwatch] frame %llu addr 0x%x  %08x -> %08x\n",
                                 (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), addr, old32, val);
                }
            }
        }
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
        // [detsound] deterministic pacing: same idea as wallBase/wallBaseBytes but counted in
        // GUEST FRAMES instead of host milliseconds, so two machines credit the stream
        // identically. Wall-clock pacing is the measured root of boot non-determinism
        // (12 bytes differ at frame 1, all of them stream-position counters).
        bool  frameClock = false;
        uint64_t frameBase = 0u;
        uint64_t frameBaseBytes = 0u;
    };
    // [detsound] PS2X_DETSOUND=<fps> (1 => 60). When set, stream credit advances on the guest
    // frame counter rather than the host clock, making the sound engine's consumption identical
    // on two machines. This is what lets two independently-booted clients stay in step without a
    // savestate transfer. Off by default: it decouples credit from real playback, so audio can
    // drift if the guest frame rate is not what is declared here.
    static uint32_t detSoundFps()
    {
        static const uint32_t s_fps = [](){ const char *v = std::getenv("PS2X_DETSOUND");
            if (!v || !v[0] || v[0] == '0') return 0u;
            const long n = std::atol(v);
            const uint32_t f = (n <= 1) ? 60u : (uint32_t)n;
            std::fprintf(stderr, "[detsound] stream credit paced by the GUEST FRAME COUNTER at %u fps (deterministic)\n", f);
            return f; }();
        return s_fps;
    }
    std::mutex g_iopSinkM;
    std::map<uint32_t, IopSink> g_iopSinks;
    extern "C" bool ps2xFrameStepOn();   // [rollback] ps2_runtime.cpp
    extern "C" bool ps2xVirtualClockOn();      // [rollback] ps2_memory.cpp
    extern "C" uint64_t ps2xVirtualClockGet();
    // [rollback] The sound/CD HLE's notion of "now": the virtual clock in frame-stepped mode (it advances
    // 1/60 s per delivered vblank and is part of the snapshot), the wall clock otherwise. Every timing
    // decision the guest can observe through this HLE routes through here, so a rolled-back re-run
    // makes the same decisions.
    static std::chrono::steady_clock::time_point ps2xNowSteady()
    {
        if (ps2xVirtualClockOn())
            return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ps2xVirtualClockGet()));
        return std::chrono::steady_clock::now();
    }
    // Deterministic pacing is on under PS2X_DETSOUND or in frame-stepped mode.
    static bool ps2xDetPacing() { return detSoundFps() != 0u || ps2xFrameStepOn(); }

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
        s.frameClock = false;   // [rollback] a new stream re-bases its frame/tick clock
        if (runtime && !ps2xFrameStepOn())   // [rollback] stepped mode never consults the device's progress
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
        const auto prog = runtime->audioBackend().streamProgress(s.streamId);   // (stepped mode: diagnostics only)
        if (ps2xFrameStepOn())
        {   // [rollback] Frame-stepped mode NEVER consults the device: whether the host stream has
            // started, how much it has played and whether the ring looks full to it are real-time
            // facts that differ between a run and its re-simulation (and between two machines). The
            // early return below for "device not started yet" withheld the credit in one run and not
            // the other, so the stream thread issued one SIF DMA more -- the last sound-block
            // divergence. Credit by vblank ticks only; the device starts on its own once fed.
            s.wallClock = false; s.ringFullIdle = false;
            // Nothing queued = nothing playing: no credit, and the clock re-bases when data next arrives
            // (a stream that has just started). Measured without this: the clock ran from boot on an
            // empty sink, so the first BGM was credited 35 s of "played" the moment it started, the game
            // refilled at full speed and the device sat 4 s behind, trimming forever.
            if (queued == 0u) { s.frameClock = false; return; }
            const uint64_t fr = ps2_syscalls::GetCurrentVSyncTick();
            if (!s.frameClock) { s.frameClock = true; s.frameBase = fr; s.frameBaseBytes = s.returnedBytes; }
            // At the rate the stream was DECLARED at (the game's ADX header, as the DMA path told the
            // backend) -- not the 24 kHz default: the title music is 48 kHz, and crediting it at half
            // rate fed the device half of what it played (measured: 24 002 vs 48 169 samples/s).
            const uint32_t rate = (prog.known && prog.sampleRate) ? prog.sampleRate : sndDeclaredRate();
            playedBytes = s.frameBaseBytes + ((fr - s.frameBase) * (uint64_t)rate * 2ull) / 60u;
            const uint64_t fedTotal = s.returnedBytes + queued;   // the device cannot have played what was never fed
            if (playedBytes > fedTotal) playedBytes = fedTotal;
        }
        else
        {
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
                const auto now = ps2xNowSteady();
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
            if (const uint32_t fps = ps2xFrameStepOn() ? 60u : detSoundFps())
            {   // [detsound] ignore the device's real progress; credit by frames instead.
                // [rollback] In frame-stepped mode the clock is the VSYNC TICK: 60 Hz by construction
                // (the controller delivers it), independent of the game's 30/60 fps, and part of the
                // snapshot -- so a rolled-back re-run credits the stream identically.
                const uint64_t fr = ps2xFrameStepOn() ? ps2_syscalls::GetCurrentVSyncTick() : g_bt3FrameCount.load(std::memory_order_relaxed);
                if (!s.frameClock) { s.frameClock = true; s.frameBase = fr; s.frameBaseBytes = s.returnedBytes; }
                playedBytes = s.frameBaseBytes + ((fr - s.frameBase) * sndDeclaredRate() * 2ull) / fps;
            }
        }
        else
        {
            // Nothing is rendering this stream (PS2X_SNDPLAY off, or an id the DMA path never
            // feeds). Advance on a wall clock at the declared rate so the guest's sound engine
            // still runs instead of wedging on a ring that never drains.
            const auto now = ps2xNowSteady();
            if (!s.wallClock)
            {
                s.wallClock = true;
                s.wallBase = now;
                s.wallBaseBytes = s.returnedBytes;
            }
            const uint64_t ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - s.wallBase).count());
            playedBytes = s.wallBaseBytes + (ms * sndDeclaredRate() * 2ull) / 1000ull;
            if (const uint32_t fps = ps2xFrameStepOn() ? 60u : detSoundFps())
            {
                const uint64_t fr = ps2xFrameStepOn() ? ps2_syscalls::GetCurrentVSyncTick() : g_bt3FrameCount.load(std::memory_order_relaxed);
                if (!s.frameClock) { s.frameClock = true; s.frameBase = fr; s.frameBaseBytes = s.returnedBytes; }
                playedBytes = s.frameBaseBytes + ((fr - s.frameBase) * sndDeclaredRate() * 2ull) / fps;
            }
        }
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
                const bool audible = ps2xFrameStepOn() ? (sndListBytes(rdram, sink, kSinkList1) != 0u)   // [rollback] guest queue, not device
                                                       : (runtime->audioBackend().streamProgress(it->second.streamId).pending > 0u);
                if (audible)
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
            s.returnedBytes = (prog.known && !ps2xFrameStepOn())
                                  ? (prog.consumedSamples + prog.gapSamples + prog.pending) * 2ull
                                  : 0u;
            s.frameClock = false;   // [rollback] re-base the tick clock after a flush
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

    // [sereload] The bank table is a FIXED SIX SLOTS that get OVERWRITTEN, not a growing list.
    //
    // Every fight re-uploads its banks, and the old code appended them, so the second fight's
    // per-character banks became slots 6 and 7 while the game went on asking for bank bits 16
    // and 32 (slots 4 and 5) -- and got fight one's characters. That is the "voices stay with
    // the previous character after switching" bug. Measured across a six-fight session:
    //
    //     boot     704 -> slot 0   (menu/system, uploaded ONCE and never reloaded)
    //     fight N 5120 -> slot 1   5440 -> slot 2   832 -> slot 3
    //             3712 -> slot 4   3712 -> slot 5   (the two per-character banks)
    //
    // so a reload group is exactly FIVE headers, always slots 1..5 in that order. The upload
    // ADDRESS cannot identify a bank: it marches upward every reload (0x124c40, 0x12c880,
    // 0x1344c0, 0x13c100, 0x143d40, 0x14b980 ...), which is why this keys on group position.
    // Per-character header size stays 3712 while the blobs differ each fight, so size alone
    // cannot separate slots 4 and 5 either -- only order does.
    //
    // Bounded by construction now: six slots instead of a list that used to run to its 32-entry
    // cap and then silently stop capturing (~20 MB of stale banks held at the same time).
    constexpr uint32_t kSeSlots = 6u;  // bank bitmask bits 1, 2, 4, 8, 16, 32
    struct SeSlot
    {
        uint32_t addr = 0u;
        std::vector<uint8_t> hdr;
        std::vector<uint8_t> blob;
    };
    SeSlot g_seSlot[kSeSlots];
    // Slots whose header has arrived but whose sample blob has not. Blobs carry no identity of
    // their own, so they are matched to headers FIFO -- which is exactly the observed order:
    // [hdr 1][blob 1] then [hdr 2..5][blob 2..5].
    std::vector<uint32_t> g_sePendingBlob;
    // Which per-character slot the next unrecognised bank takes: reset to 4 by any identified
    // bank, so each load group fills 4 then 5.
    uint32_t g_sePerChar = 4u;
    uint32_t g_seSlotCount[kSeSlots] = {};   // Vagi sample count currently in each slot

    // Read a byte out of slot `idx`'s sample blob; returns false past the end.
    bool seBlobByte(uint32_t idx, uint32_t off, uint8_t &out)
    {
        std::lock_guard<std::mutex> lk(g_seBlobM);
        if (idx >= kSeSlots || off >= g_seSlot[idx].blob.size())
            return false;
        out = g_seSlot[idx].blob[off];
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
    if (g_sePendingBlob.empty())
    {
        // A blob with no header waiting for it. Never seen in any captured session; log it
        // rather than guess a slot, because guessing is how the wrong character ends up talking.
        std::fprintf(stderr, "[se] bank blob (%u bytes) with no header awaiting it -- DROPPED\n",
                     size);
        return;
    }
    const uint32_t slot = g_sePendingBlob.front();
    g_sePendingBlob.erase(g_sePendingBlob.begin());
    g_seSlot[slot].blob.assign(data, data + size);
    std::fprintf(stderr, "[se] bank blob -> slot %u (%u bytes)\n", slot, size);
}

// Called from SIF.cpp for every DMA into the IOP sound region. A bank's header is uploaded
// before its sample blob, so recording the Vagi-bearing ones in order keeps header slot N
// paired with blob N. Sequence (Sequ/Sesq) uploads share the SCEI container but carry no Vagi
// chunk, and must not be counted or every slot after the first would be off by one.
void bt3NoteSeBankHeader(uint32_t dst, const uint8_t *data, uint32_t size)
{
    if (!data || size < 24u)
        return;
    // Find the Vagi chunk and read its SAMPLE COUNT -- that count is the bank's identity.
    bool hasVagi = false;
    uint32_t vagiCount = 0u;
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
            if (o + 16u <= size)
                std::memcpy(&vagiCount, data + o + 12u, 4);
            break;
        }
        o += len;
    }
    if (!hasVagi)
        return;
    std::lock_guard<std::mutex> lk(g_seBlobM);

    // [sebankid] The slot comes from WHAT THE BANK IS, not from its position in the upload group.
    //
    // Position was the first attempt and it is wrong: a group is NOT always five banks. Observed
    // two shapes, both real --
    //     boot + fight:   79, 84, 10, 56, 56   (five)
    //     another fight:  79,     10, 56, 56   (four -- no 84-sample common-SFX bank)
    // -- so counting arrivals slid the two per-character banks down into slots 3 and 4, and the
    // game asking for bank bit 16/32 got the wrong bank entirely. Reported as a character voice
    // looping (the fly-up call-out repeating), because a sample index resolved inside a bank that
    // was never meant to answer it.
    //
    // The Vagi sample count identifies each fixed bank uniquely, and the per-character banks are
    // the only pair that collides -- they are told apart by group order, which is the one thing
    // position IS reliable for.
    uint32_t slot;
    const char *why;
    switch (vagiCount)
    {
        case 8u:  slot = 0u; g_sePerChar = 4u; why = "menu/system, 8"; break;
        case 79u: slot = 1u; g_sePerChar = 4u; why = "system SE, 79"; break;
        case 84u: slot = 2u; g_sePerChar = 4u; why = "common fight SFX, 84"; break;
        case 10u: slot = 3u; g_sePerChar = 4u; why = "10-sample"; break;
        default:
            // Anything else is a per-character bank (56 in every bank seen, but a character with
            // a different count must still land in 4 then 5 rather than be dropped).
            why = "per-character";
            // [sebankaddr] A TRANSFORMATION re-uploads the fighter's voice bank mid-fight to the SAME
            // header address its original bank used (SSJ Goku's bank at 0x129880 = base Goku's slot 4;
            // seen 2026-09-17 in every replay). The group counter cannot know that and filed it as a
            // third per-character bank -> slot 5 -> the OPPONENT spoke with P1's transformed voice.
            // Within a fight the header address IS the identity of a per-character bank: reuse that slot.
            slot = kSeSlots;
            for (uint32_t k = 4u; k < kSeSlots; ++k)
                if (!g_seSlot[k].hdr.empty() && g_seSlot[k].addr == dst) { slot = k; why = "per-character, same address = same fighter"; break; }
            if (slot < kSeSlots)
            {
            }
            else if (g_sePerChar < kSeSlots)
            {
                slot = g_sePerChar++;   // 4, then 5
            }
            else
            {
                // A THIRD per-character bank in one group. The cursor is one PAST slot 5 by then,
                // so this is the real over-run -- comparing against kSeSlots-1 instead made the
                // legitimate SECOND bank warn on every single fight.
                slot = kSeSlots - 1u;
                std::fprintf(stderr, "[se] WARNING a third per-character bank (%u samples) in one "
                                     "group -- slot 5 overwritten, mapping may be off\n", vagiCount);
            }
            break;
    }
    const bool reload = !g_seSlot[slot].hdr.empty();
    // Same-slot reloads should keep the same sample count. If one changes, the identity table
    // above no longer matches this build of the game -- say so loudly rather than quietly playing
    // the wrong sound, which is the failure this whole path exists to prevent.
    if (reload && g_seSlotCount[slot] != vagiCount)
        std::fprintf(stderr, "[se] WARNING slot %u sample count changed %u -> %u\n",
                     slot, g_seSlotCount[slot], vagiCount);
    g_seSlot[slot].addr = dst;
    g_seSlot[slot].hdr.assign(data, data + size);
    g_seSlot[slot].blob.clear();   // the old blob belongs to the bank being replaced
    g_seSlotCount[slot] = vagiCount;
    g_sePendingBlob.push_back(slot);
    std::fprintf(stderr, "[se] bank header -> slot %u at 0x%x (%u bytes, %u samples: %s)%s\n",
                 slot, dst, size, vagiCount, why, reload ? " [reload]" : "");
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
    // [seloop] Sony ADPCM block flags: bit 0 = last block, bit 1 = loop region (an end block with it set jumps back to
    // the loop start instead of stopping), bit 2 = loop start. loopStartOut = the sample index the voice returns to,
    // loopsOut = whether it returns at all. The ki-charge hum is such a sample: without the loop it played once, so
    // only the charge's initial burst was heard (user, 2026-09-29).
    void seDecodeAdpcm(uint32_t blob, uint32_t addr, std::vector<int16_t> &out, uint32_t maxBlocks, size_t *loopStartOut = nullptr, bool *loopsOut = nullptr)
    {
        static const int kF0[5] = {0, 60, 115, 98, 122};
        static const int kF1[5] = {0, 0, -52, -55, -60};
        int32_t s1 = 0, s2 = 0;
        uint8_t blk[16];
        if (loopStartOut) *loopStartOut = 0u;
        if (loopsOut) *loopsOut = false;
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
            if ((flags & 4u) && loopStartOut) *loopStartOut = out.size();   // [seloop] loop start = this block's first sample
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
            if (flags & 1u) // end of this sample; with bit 1 the voice loops back to the loop start
            {
                if (loopsOut) *loopsOut = (flags & 2u) != 0u;
                return;
            }
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
        size_t loopStart = 0;          // [seloop] where the voice returns to when it loops
        bool loops = false;            // [seloop] sustained until the game's stop-by-handle (or the safety cap)
        size_t looped = 0;             // samples produced past the first pass (for the cap)
    };
    constexpr size_t kSeLoopCap = 30u * kSeMixRate;   // [seloop] a looped voice the game never stops dies after 30 s
    std::mutex g_seVoiceM;
    std::vector<SeVoice> g_seVoices;
    // [rollback] Stepped-mode SE pacing: samples are generated per vsync tick (22050/60 each) instead
    // of per the device's pending level, so voice positions and completions follow guest progress.
    // Both are part of the snapshot.
    uint64_t g_seTickBase = 0, g_seTickCarry = 0;

    void seAddVoice(uint32_t serial, std::vector<int16_t> &&pcm, size_t loopStart = 0, bool loops = false)
    {
        if (pcm.empty())
            return;
        std::lock_guard<std::mutex> lk(g_seVoiceM);
        if (g_seVoices.size() >= 32u) // SPU2 has 24 voices; a cap keeps a runaway bounded
            g_seVoices.erase(g_seVoices.begin());
        SeVoice v;
        v.serial = serial;
        v.pcm = std::move(pcm);
        v.loops = loops && loopStart < v.pcm.size();
        v.loopStart = v.loops ? loopStart : 0u;
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
        // [rollback] stepped mode: a fixed budget of samples per vsync tick, whatever the device holds
        const bool stepped = ps2xFrameStepOn();
        uint64_t budget = 0;   // samples this call may mix (stepped mode)
        if (stepped)
        {
            // The budget is kept in 1/60-sample units (g_seTickCarry) so nothing is ever lost: one
            // vblank is 367.5 samples and a chunk is 512, so at 60 fps a budget that was thrown away
            // whenever it fell short of a chunk never mixed anything (the logo chime, every menu
            // sound), and at 30 fps it mixed one chunk per two ticks and dropped the rest (effects
            // at ~70 % rate: crackly voices). Unspent whole samples go back into the carry below.
            const uint64_t tick = ps2_syscalls::GetCurrentVSyncTick();
            if (g_seTickBase == 0 || tick < g_seTickBase) g_seTickBase = tick;
            const uint64_t ticks = tick - g_seTickBase;
            g_seTickBase = tick;
            const uint64_t acc60 = g_seTickCarry + ticks * kSeMixRate;
            budget = acc60 / 60u; g_seTickCarry = acc60 % 60u;
        }
        for (int guard = 0; guard < 64; ++guard)
        {
            {
                std::lock_guard<std::mutex> lk(g_seVoiceM);
                if (g_seVoices.empty())
                {
                    if (stepped) g_seTickCarry = 0u;   // nothing to play: do not bank time for a later burst
                    return;
                }
            }
            if (stepped)
            {
                if (budget < kSeChunk) { g_seTickCarry += budget * 60u; return; }   // keep the remainder for the next call
                budget -= kSeChunk;
            }
            else
            {
                const auto prog = runtime->audioBackend().streamProgress(kSeStreamId);
                if (prog.pending >= kSeTargetPending)
                {
                    // The target IS this stream's whole cushion -- nothing more is produced until
                    // the device drains some -- so tell the backend to start with it. Without this
                    // it waited for the 100 ms "one-shot idle" rule to fire, which also padded the
                    // partial chunk with silence: the click on the memory-card prompt sound.
                    if (prog.known && !prog.started)
                        runtime->audioBackend().requestStreamStart(kSeStreamId);
                    return;
                }
            }
            int32_t acc[kSeChunk];
            std::memset(acc, 0, sizeof(acc));
            size_t used = 0;
            {
                std::lock_guard<std::mutex> lk(g_seVoiceM);
                for (auto it = g_seVoices.begin(); it != g_seVoices.end();)
                {
                    size_t n = 0;
                    if (it->loops)
                    {   // [seloop] wrap inside the chunk; the voice ends only by stop-by-handle or the cap
                        for (; n < kSeChunk; ++n)
                        {
                            if (it->pos >= it->pcm.size())
                            {
                                if (it->looped >= kSeLoopCap) break;
                                it->pos = it->loopStart;
                            }
                            acc[n] += it->pcm[it->pos++];
                            if (it->pos > it->loopStart || it->looped) ++it->looped;
                        }
                    }
                    else
                    {
                        const size_t avail = it->pcm.size() - it->pos;
                        n = avail < kSeChunk ? avail : kSeChunk;
                        for (size_t i = 0; i < n; ++i)
                            acc[i] += it->pcm[it->pos + i];
                        it->pos += n;
                    }
                    if (n > used) used = n;
                    if (it->pos >= it->pcm.size() && (!it->loops || it->looped >= kSeLoopCap))
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
            // [rollback] Stepped mode mixes by ticks (the deterministic part: voice positions), but the
            // device is fed by its own queue level, like the wall-clock path: a chunk the device has no
            // room for is dropped rather than queued behind everything else, so a burst of ticks can
            // never turn into lasting latency. Nothing is fed during a re-simulation or catch-up.
            if (stepped)
            {
                if (!ps2xAudioFeedOn()) continue;
                const auto prog = runtime->audioBackend().streamProgress(kSeStreamId);
                if (prog.known && prog.pending >= kSeTargetPending)
                {
                    if (!prog.started)
                        runtime->audioBackend().requestStreamStart(kSeStreamId);   // same as the wall-clock path
                    continue;
                }
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
        {   // PS2X_SEMUTEBANK=<bank id>: drop every effect of that bank (diagnostic: which path a doubled sound comes from)
            static const int s_mute = [](){ const char *v = std::getenv("PS2X_SEMUTEBANK"); return v && v[0] ? std::atoi(v) : -1; }();
            if (s_mute >= 0 && (int)bank == s_mute) { std::fprintf(stderr, "[se] muted bank%u id%u (PS2X_SEMUTEBANK)\n", bank, idx); return; }
        }
        // Header <-> snapshot pairing follows UPLOAD ORDER: bank A's header (8 samples) arrives
        // with the first blob, bank B's (79) with the second. Bank A is the small system set --
        // menu cursor/confirm/cancel -- so try it first.
        // Command entry +4 selects the BANK (1 = bank A / 8 samples, 2 = bank B / 79) and +5 is
        // the sample index within it. Reading +4 as the sound id is why every menu action played
        // the same sample: +4 barely varies, +5 is the real selector.
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
            bool haveBlob = false;
            {
                std::lock_guard<std::mutex> lk(g_seBlobM);
                if (slot >= kSeSlots || g_seSlot[slot].hdr.empty())
                {
                    seDrop(bank, idx, "no header captured for this slot");
                    return;
                }
                hdrSnap = g_seSlot[slot].hdr;
                hdrAddr = g_seSlot[slot].addr;
                haveBlob = !g_seSlot[slot].blob.empty();
            }
            const struct { uint32_t hdr; uint32_t blob; } bk{hdrAddr, slot};
            uint32_t pay = 0u, cnt = 0u;
            if (!haveBlob)
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
            size_t loopStart = 0; bool loops = false;
            seDecodeAdpcm(bk.blob, dataOff, pcm, 8192u, &loopStart, &loops);   // [seloop] 8192 blocks: a looped hum must be whole (1024 cut samples past 1.3 s)
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
                loopStart = static_cast<size_t>((static_cast<uint64_t>(loopStart) * kSeMixRate) / srcRate);   // [seloop]
            }
            // Hand it to a voice; seServiceVoices() mixes the active voices incrementally so
            // a later stop-by-serial can cut the tail. Overlap still works -- voices sum.
            const size_t pcmN = pcm.size();
            {   // PS2X_SEDUMP=<dir>: every decoded effect as raw s16 mono at kSeMixRate (se_<serial>_bank<b>_id<i>.raw)
                static const char *s_dir = std::getenv("PS2X_SEDUMP");
                if (s_dir && s_dir[0])
                {
                    char path[512]; std::snprintf(path, sizeof path, "%s/se_%u_bank%u_id%u.raw", s_dir, serial, bank, idx);
                    if (FILE *f = std::fopen(path, "wb")) { std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f); std::fclose(f); }
                }
            }
            seAddVoice(serial, std::move(pcm), loopStart, loops);
            seServiceVoices(runtime);
            static std::atomic<uint32_t> n{0};
            const uint32_t k = n.fetch_add(1);
            if (seLogEnabled() || k < 12u)
            {
                const uint32_t r = kSeMixRate; // post-resample: pcm.size() is in THIS rate
                std::fprintf(stderr, "[se] #%-4u ser=%-5u slot%u(bank%-2u) id%-3u%-12s %-4s %5zu smp "
                                     "%4ums @%5uHz vol=%-3u pan=%-3u dataOff=0x%x%s%zu\n",
                             k, serial, slot, bank, idx, seName(bank, idx), head ? "head" : "vagi",
                             pcmN, static_cast<uint32_t>(pcmN * 1000u / r), r,
                             vol, pan, dataOff, loops ? " LOOP@" : " oneshot ", loops ? loopStart : (size_t)0);
            }
            return;
        }
        static std::atomic<uint32_t> miss{0};
        if (miss.fetch_add(1) < 8u)
            std::fprintf(stderr, "[se] bank%u sample%u NOT FOUND (slot %u)\n", bank, idx, slot);
    }

    // [cliprectlog] PS2X_CLIPRECTLOG=1: every call of the UI clip-rect setter (0x224be8: sh a1/a2/a3/t0 -> +0x10..+0x16
    // of the widget) with its arguments, caller and frame -- to find who hands the Evolution Z panel a 256-wide rect.
    PS2Runtime::RecompiledFunction g_orig224be8 = nullptr;
    void bt3ClipRectLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1) < 4000u)
            std::fprintf(stderr, "[cliprect] fr=%llu widget=0x%x rect=(%d,%d,%d,%d) ra=0x%x\n",
                         (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), getRegU32(ctx, 4),
                         (int16_t)getRegU32(ctx, 5), (int16_t)getRegU32(ctx, 6), (int16_t)getRegU32(ctx, 7), (int16_t)getRegU32(ctx, 8), getRegU32(ctx, 31));
        if (g_orig224be8) g_orig224be8(rdram, ctx, runtime);
    }
    // [cliprectlog] =2: the GS packet emitter 0x101400 (called through a function pointer from the flush at
    // 0x10c4c4/0x10c4fc with a0 = the register shadow it copies out): print its arguments and the shadow's words
    PS2Runtime::RecompiledFunction g_orig101400 = nullptr;
    void bt3EmitLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> n{0};
        const uint32_t ra = getRegU32(ctx, 31);
        if ((ra == 0x10c4ccu || ra == 0x10c504u) && n.fetch_add(1) < 600u)
        {
            const uint32_t a0 = getRegU32(ctx, 4) & 0x1FFFFFFFu; uint32_t w[8] = {};
            if (a0 + 32u < 32u * 1024u * 1024u) std::memcpy(w, rdram + a0, 32);
            std::fprintf(stderr, "[emit] fr=%llu a0=0x%x a1=0x%x a2=0x%x a3=0x%x ra=0x%x | %08x %08x %08x %08x %08x %08x %08x %08x\n",
                         (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), a0, getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), ra,
                         w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
        if (g_orig101400) g_orig101400(rdram, ctx, runtime);
    }

    // [cliprectlog] =3: the scissor callback FUN_00100648 (rect = [a0+0x200..0x20C] as u32 x0,x1,y0,y1, tail-jumps into
    // the emitter): print the context pointer and the rect so its writer can be range-watched
    PS2Runtime::RecompiledFunction g_orig100648 = nullptr;
    void bt3ScissorCbLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1) < 600u)
        {
            const uint32_t a0 = getRegU32(ctx, 4) & 0x1FFFFFFFu; uint32_t w[4] = {};
            if (a0 + 0x210u < 32u * 1024u * 1024u) std::memcpy(w, rdram + a0 + 0x200u, 16);
            std::fprintf(stderr, "[sciscb] fr=%llu ctx=0x%x rect x%u..%u y%u..%u ra=0x%x\n",
                         (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), a0, w[0], w[1], w[2], w[3], getRegU32(ctx, 31));
        }
        if (g_orig100648) g_orig100648(rdram, ctx, runtime);
    }

    // [cliprectlog] =4: FUN_00126b10 (x0,x1,y0,y1 in, clamps through func_126620, tail-jumps into the emitter)
    PS2Runtime::RecompiledFunction g_orig126b10 = nullptr;
    void bt3ScissorWrapLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1) < 600u)
        {
            const uint32_t s0 = getRegU32(ctx, 16) & 0x1FFFFFFFu; uint32_t f[8] = {};   // the flush's context object (callee-saved, still live)
            if (s0 + 0x100u < 32u * 1024u * 1024u) { std::memcpy(&f[0], rdram + s0 + 0x88u, 4); std::memcpy(&f[1], rdram + s0 + 0x90u, 4); std::memcpy(&f[2], rdram + s0 + 0x98u, 4); std::memcpy(&f[3], rdram + s0 + 0xB4u, 4); std::memcpy(&f[4], rdram + s0 + 0xB8u, 4); std::memcpy(&f[5], rdram + s0 + 0xBCu, 4); std::memcpy(&f[6], rdram + s0 + 0xC0u, 4); std::memcpy(&f[7], rdram + s0 + 0x48u, 4); }
            std::fprintf(stderr, "[sciswrap] fr=%llu a0=%d a1=%d a2=%d a3=%d ra=0x%x s0=0x%x [+88]=%u [+90]=%u [+98]=%u [+b4]=0x%x [+b8]=0x%x [+bc]=%u [+c0]=%u [+48]=0x%x\n",
                         (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), (int)getRegU32(ctx, 4), (int)getRegU32(ctx, 5), (int)getRegU32(ctx, 6), (int)getRegU32(ctx, 7), getRegU32(ctx, 31),
                         s0, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
        if (g_orig126b10) g_orig126b10(rdram, ctx, runtime);
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
                    if (runtime && bp && !ps2xFrameStepOn())   // [rollback] stepped mode: the rate limiter below (virtual clock), never the device
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
                        const auto now = ps2xNowSteady();
                        std::lock_guard<std::mutex> lk(g_sndRateM);
                        auto it = g_sndRateLast.find(sink);
                        if (it == g_sndRateLast.end() ||
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() >= s_ms)
                        {
                            g_sndRateLast[sink] = now;
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
    // ===================== [statelog] what screen is this, and what is playing =====================
    //
    // ONE table, because there used to be two and they disagreed. ps2_runtime.cpp's [hstate] switch
    // labels 0x27 "FIGHT"; docs/MAIN-MENU.md labels it CHARACTER_SELECT and puts the fight at 0x2D.
    // Both are named tables of the same field -- the top-level state at [[0x2ff10c]+0x18] -- and
    // having to pick one to read a log is how a wrong name survives for months. This is the one
    // now; the [hstate] switch is the thing to fix next, against these values.
    //
    // The names come from the jump table the main menu actually uses (0x3364f4..0x336534) plus the
    // duel's own module states. "PREFIGHT_SETUP" for 0x28/0x29 is the game's own naming as recorded
    // in MAIN-MENU.md; whether those two are one screen or two is exactly what [statelog] is for.
    static const char *bt3StateName(uint32_t s)
    {
        switch (s)
        {
        case 0x01u: return "BOOT";
        case 0x06u: return "LOADING";
        case 0x0Bu: return "?0x0B";
        case 0x0Du: return "ULTIMATE_BATTLE";
        case 0x0Eu: return "?0x0E";
        case 0x10u: return "?0x10";
        case 0x11u: return "?0x11";
        case 0x1Au: return "?0x1A";
        case 0x1Du: return "?0x1D";
        case 0x20u: return "?0x20";
        case 0x21u: return "DRAGON_WORLD_TOUR";
        case 0x26u: return "DUEL_MENU";          // the versus screen: mode, then battle type
        case 0x27u: return "CHARACTER_SELECT";   // NOT the fight -- that is 0x2D
        case 0x28u: return "PREFIGHT_SETUP";
        case 0x29u: return "PREFIGHT_SETUP_2";  // same name as 0x28 upstream; may be the same screen
        case 0x2Cu: return "ULTIMATE_TRAINING";
        case 0x2Du: return "IN_FIGHT";
        case 0x30u: return "EVOLUTION_Z";
        case 0x35u: return "DATA_CENTER";
        case 0x38u: return "POST_FIGHT";
        case 0x3Cu: return "CHARACTER_REFERENCE";
        case 0x3Eu: return "OPTIONS";
        case 0x46u: return "EXTRA";
        default:   return "?UNKNOWN";
        }
    }

    // PS2X_STATELOG=1. Off by default: it is a diagnostic, and a per-frame probe of guest RAM that
    // nobody asked for is not something to leave running in a shipping tree.
    static bool bt3StateLogOn()
    {
        static const bool on = [](){ const char *v = std::getenv("PS2X_STATELOG");
                                     return v && v[0] && v[0] != '0'; }();
        return on;
    }

    // The current top-level state, or 0 when the state object does not exist yet (still booting).
    // Resolved through the pointer at 0x2ff10c on purpose: it survives whatever the allocator does,
    // whereas the [ach] tracker reads the state object's fields as flat addresses, which is fine
    // because the layout is deterministic but would break the moment it is not.
    static uint32_t bt3CurState(uint8_t *rdram)
    {
        const uint32_t so = sndRd32(rdram, 0x2ff10cu) & 0x1FFFFFFFu;
        return so ? sndRd32(rdram, so + 0x18u) : 0u;
    }

    // The frame counter, for the music correlation. Read through the same atomic the rest of the
    // netjump code uses, so a [statelog] line and a [netjump] line can be lined up by frame.
    std::atomic<uint64_t> g_statelogLastStart{0};

    std::mutex g_streamStartM;
    std::map<uint32_t, std::chrono::steady_clock::time_point> g_streamStart;
    PS2Runtime::RecompiledFunction g_orig28b428 = nullptr;
    void bt3StreamStartNote(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x28b428
    {
        const uint32_t obj = getRegU32(ctx, 4);
        // [statelog] Every stream START, tagged with the screen it happened on.
        //
        // Deliberately NOT a track id: the stream object and its sink are runtime addresses that
        // land differently every run, so a "track id" built from them would not survive to be
        // compared. What IS worth having is the TIMING -- whether a START lands on a state
        // transition is the whole question, because the code's own note is that after the title the
        // BGM goes to state 0 "and never returns to 1". If these lines cluster on transitions, each
        // screen has its own track. If they stop after the title, the music does not discriminate
        // and the pad reads in [fightprobe] are what has to answer it.
        if (bt3StateLogOn())
        {
            const uint32_t st = bt3CurState(rdram);
            std::fprintf(stderr, "[statelog]   bgm START  state=0x%02x %-17s obj=0x%x sink=0x%x fr=%llu\n",
                         st, bt3StateName(st), obj, sndRd32(rdram, obj + 8u),
                         (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed));
        }
        {
            std::lock_guard<std::mutex> lk(g_streamStartM);
            g_streamStart[obj] = ps2xNowSteady();
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
            if (bufPtr && ps2xFrameStepOn())
            {   // [rollback] stepped mode: the device's backlog is host-paced; the guest's own queue is not
                stillPlaying = sink && sndListBytes(rdram, sink, kSinkList1) != 0u;
            }
            else if (bufPtr)
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
                                         ps2xNowSteady() - it->second).count();
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
                        std::atomic<uint32_t> activeReq{0u};   // [cdedge2] pending request ([stream+8]) the device was seen busy/done for
                        std::atomic<uint32_t> pendSeen{0u}; std::atomic<uint64_t> pendSerial{0u}; std::atomic<uint32_t> busyPolls{0u}; };   // [cdedge3] the pending request last registered, the read serial when it appeared, polls answered BUSY for it
    static Bt3DevDone s_bt3DevSlots[8];
    Bt3DevDone *bt3DevSlotAt(int i) { return &s_bt3DevSlots[(i < 0 || i >= 8) ? 0 : i]; }   // [statesync]
    inline Bt3DevDone *bt3DevSlot(uint32_t dev)
    {
        Bt3DevDone (&s_slots)[8] = s_bt3DevSlots;
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

    // [statesync] The slots are host-side latches that decide which device read-state the game sees
    // (bt3CdStateEdge, the tick pump): they must travel with a snapshot, or a peer that adopts our RAM
    // mid-read continues on its own latches and its loader takes a different branch ~30 frames later.
    struct Bt3DevDoneSer { uint32_t dev, reported, stream, idleWait, activeReq; };
    static void bt3DevSlotsCapture(Bt3DevDoneSer out[8])
    {
        for (int i = 0; i < 8; ++i)
        {
            Bt3DevDone *c = bt3DevSlotAt(i);
            out[i] = Bt3DevDoneSer{ c->dev.load(), c->reported.load(), c->stream.load(), c->idleWait.load(), c->activeReq.load() };
        }
    }
    static void bt3DevSlotsRestore(const Bt3DevDoneSer in[8])
    {
        for (int i = 0; i < 8; ++i)
        {
            Bt3DevDone *c = bt3DevSlotAt(i);
            c->dev.store(in[i].dev); c->reported.store(in[i].reported); c->stream.store(in[i].stream); c->idleWait.store(in[i].idleWait); c->activeReq.store(in[i].activeReq);
        }
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
                // [cdedge3] Reporting 3 on the FIRST idle poll is a false completion when the request has only been
                // posted, not read yet: the opening movie's PSS ring hit exactly that (the guard fired once, the game
                // skipped the read of ring slot 0 and demuxed the stale chunk -> the first two seconds played twice).
                // A request counts as started once a sceCdRead was issued AFTER it appeared (g_cdReadSerial moved);
                // until then answer BUSY (2 keeps the guest's poll gate open, the tick issues the read, the next poll
                // sees it). Bounded (240 polls) so a request whose read predates its registration cannot wedge.
                static const bool s_edge3 = [](){ const char *v = std::getenv("PS2X_CDEDGE3"); return !(v && v[0] == '0'); }();
                const uint64_t serialNow = g_cdReadSerial.load(std::memory_order_relaxed);
                // The request node is REUSED (one pointer for every request): a request is "new" when [stream+8] comes back from 0.
                if (pendNow == 0u) ds->pendSeen.store(0u);
                else if (pendNow != ds->pendSeen.load(std::memory_order_relaxed)) { ds->pendSeen.store(pendNow); ds->pendSerial.store(serialNow); ds->busyPolls.store(0u); }
                uint32_t reqDst = 0u; if (pendNow) if (const uint8_t *q = getConstMemPtr(rdram, pendNow + 0x1cu)) std::memcpy(&reqDst, q, 4);   // node +0x1c = destination buffer (dump 2026-09-28)
                const bool started = pendNow == 0u || (reqDst ? ps2xCdReadSince(reqDst & PS2_RAM_MASK, ds->pendSerial.load(std::memory_order_relaxed)) : serialNow > ds->pendSerial.load(std::memory_order_relaxed));
                {   // PS2X_CDEDGE_DUMP=1 (dev): the pending request node and the stream object, to find the request's buffer/LBN fields
                    static const bool s_dump = [](){ const char *v = std::getenv("PS2X_CDEDGE_DUMP"); return v && v[0] && v[0] != '0'; }();
                    static std::atomic<uint32_t> s_dn{0};
                    if (s_dump && waiting && s_dn.fetch_add(1u) < 6u)
                    {
                        char line[600]; int n = std::snprintf(line, sizeof line, "[cdedge-dump] pend 0x%x serial %llu/%llu started %d req:", pendNow, (unsigned long long)serialNow, (unsigned long long)ds->pendSerial.load(), (int)started);
                        for (uint32_t i = 0; i < 24u && n < 560; ++i) { uint32_t w = 0; if (const uint8_t *q = getConstMemPtr(rdram, pendNow + i * 4u)) std::memcpy(&w, q, 4); n += std::snprintf(line + n, sizeof line - n, " %x", w); }
                        n += std::snprintf(line + n, sizeof line - n, " | stream:");
                        for (uint32_t i = 0; i < 12u && n < 590; ++i) { uint32_t w = 0; if (const uint8_t *q = getConstMemPtr(rdram, stream + i * 4u)) std::memcpy(&w, q, 4); n += std::snprintf(line + n, sizeof line - n, " %x", w); }
                        std::fprintf(stderr, "%s\n", line);
                    }
                }
                if (waiting && s_edgeFix && s_edge3 && !started && ds->busyPolls.fetch_add(1u) < 240u)
                {
                    state = 2u;
                    static std::atomic<uint32_t> s_b3{0};
                    const uint32_t k = s_b3.fetch_add(1u);
                    if (k < 8u || (k % 200u) == 0u)
                        std::fprintf(stderr, "[cdstate] #%u dev=0x%x idle before any read was issued for request 0x%x (stream 0x%x); reporting BUSY\n", k, handle, pendNow, stream);
                }
                else if (waiting && s_edgeFix && s_edge2 && ds->activeReq.load(std::memory_order_relaxed) != pendNow)
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
    static std::atomic<uint64_t> g_lastCdTickFrame{~0ull};   // [detsound]
    extern "C" bool ps2xCdTickStale(unsigned ms)
    {
        // [detsound] Deterministic pacing. Gating the disc pump on ELAPSED MILLISECONDS makes the
        // number of CD ticks per frame depend on how fast this machine is, so disc data lands at
        // different rates on two machines -- measured as the 12 bytes that differ at boot frame 1,
        // all of them stream-position counters. Under PS2X_DETSOUND the pump fires exactly once
        // per guest frame instead, which is identical everywhere.
        if (ps2xDetPacing())
        {
            const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
            return g_lastCdTickFrame.load(std::memory_order_relaxed) != fr;
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t last = g_lastCdTickNs.load(std::memory_order_relaxed);
        return last == 0 || (now - last) > (int64_t)ms * 1000000LL;
    }
    extern "C" int ps2xSchedTraceOn();
    extern "C" int ps2xSchedTid();
    extern "C" int ps2xSchedTraceOn();
    extern "C" int ps2xSchedTid();
    static void bt3RunCdTickInline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ps2xSchedTraceOn()) std::fprintf(stderr, "[schedtrace] CDTICK tid=%d pc=0x%x\n", ps2xSchedTid(), ctx ? ctx->pc : 0u);
        if (ps2xSchedTraceOn()) std::fprintf(stderr, "[schedtrace] CDTICK tid=%d pc=0x%x\n", ps2xSchedTid(), ctx ? ctx->pc : 0u);
        g_lastCdTickNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);   // [dispatchpump]
        g_lastCdTickFrame.store(g_bt3FrameCount.load(std::memory_order_relaxed), std::memory_order_relaxed);   // [detsound]
        // [cdload] PS2X_CDLOAD=1: what a load is made of, per second -- CD file-server ticks (count + guest time inside
        // them), sceCdRead requests (count + MB, counted in CD.cpp) and the current bt3state. The datum for "would
        // dropping the CD model speed loads up": the tick is the game's own state machine, run inline per poll.
        static const bool s_cdload = [](){ const char *v = std::getenv("PS2X_CDLOAD"); return v && v[0] && v[0] != '0'; }();
        const auto _t0 = s_cdload ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
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
        if (s_cdload)
        {
            static std::atomic<uint64_t> s_ticks{0}, s_tickNs{0};
            static std::atomic<int64_t> s_last{0};
            const auto now = std::chrono::steady_clock::now();
            s_ticks.fetch_add(1u, std::memory_order_relaxed);
            s_tickNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - _t0).count(), std::memory_order_relaxed);
            const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            int64_t last = s_last.load(std::memory_order_relaxed);
            if (last == 0) s_last.store(nowNs, std::memory_order_relaxed);
            else if (nowNs - last >= 1000000000LL && s_last.compare_exchange_strong(last, nowNs, std::memory_order_relaxed))
            {
                const double dt = (nowNs - last) / 1e9;
                const uint64_t t = s_ticks.exchange(0), tn = s_tickNs.exchange(0), rd = g_cdLoadReads.exchange(0), by = g_cdLoadBytes.exchange(0);
                std::fprintf(stderr, "[cdload] state=0x%x: ticks %.0f/s (%.1f ms/s inside), reads %.0f/s (%.2f MB/s), %.1f ticks/read\n",
                             g_bt3StateLive.load(std::memory_order_relaxed), t / dt, tn / 1e6 / dt, rd / dt, by / 1048576.0 / dt, rd ? (double)t / rd : 0.0);
            }
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
    extern "C" void ps2xGuestSleepMs(unsigned ms);   // [fibers] parks the guest fiber, not the host thread
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
        ps2xGuestSleepMs(1u);   // [fibers] a host sleep here would stop the very threads the CD tick serves
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
        { if (const uint8_t *pdev = getConstMemPtr(rdram, handle + 4u)) std::memcpy(&cdDev, pdev, sizeof(cdDev));
          if (const uint8_t *pp = getConstMemPtr(rdram, handle + 8u)) std::memcpy(&pendBefore, pp, sizeof(pendBefore)); }
        if (cdDev && pendBefore)
        {   // [cdedge3] register the pending request BEFORE the tick that may start its read, with the read serial of that moment
            if (Bt3DevDone *slot = bt3DevSlot(cdDev))
                if (pendBefore != slot->pendSeen.load(std::memory_order_relaxed)) { slot->pendSeen.store(pendBefore); slot->pendSerial.store(g_cdReadSerial.load(std::memory_order_relaxed)); slot->busyPolls.store(0u); }
        }
        else if (cdDev) { if (Bt3DevDone *slot = bt3DevSlot(cdDev)) slot->pendSeen.store(0u); }   // [cdedge3] no request pending: the next one is new
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
            if (s_guard && (cnt > 1024u || entBase < a1m || entBase - a1m > 0x200000u || (a1 & 0x1FFFFFFFu) >= PS2_RAM_SIZE))   // [tagteam] packs of extra fighters live in heap1 above 32 MB
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
    // [clipguard] armed with PS2X_VSTEP. sub_00139D78 clips an effect polygon against 5 planes (func_121A10) into a
    // stack polygon, then func_121D48(out0=sp, out1=sp+0x90, src, count) transforms `count` vertices into two
    // 9-entry stack buffers. A polygon with blown-up/NaN coordinates makes the clipper return a garbage count, the
    // transform overruns the buffers and the saved $ra becomes vertex data -> the 0x10000000 wild jumps of
    // half3/half5/half6. func_11F548 (wrap angle into [-r, r] by repeated +-2r) never terminates on a huge angle
    // (half4's hang). Both get a guard + a probe naming the caller and the data.
    PS2Runtime::RecompiledFunction g_orig121d48 = nullptr, g_orig11f548 = nullptr;
    PS2Runtime::RecompiledFunction g_orig23e770 = nullptr;   // [netview]
    // func_121A10(poly, plane, count) -> new count: one clip pass. With NaN vertices every edge "crosses" and the
    // count can double per pass (5 passes), overflowing the caller's stack polygon before the transform ever runs.
    PS2Runtime::RecompiledFunction g_orig121a10 = nullptr;
    // [vstepenv] read PS2X_VSTEP once. With 60 fps off (the default) the three guards below asked getenv on every
    // call, and the UCRT getenv compares the name with each environment variable (_strnicoll) every time.
    // 2026-10-02, COM vs COM fight at 30 fps on Windows: ~4.5% of the game thread's samples, 208 -> 171 ms/s CPU.
    static bool ps2xVStepEnvSet() { static const bool s = std::getenv("PS2X_VSTEP") != nullptr; return s; }
    void bt3ClipPassGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // func_121A10
    {
        // [fps60] inert at 30 fps: these guards exist for step-1 pathologies, so stock play stays byte-identical.
        if (!ps2VStepActive() && !ps2xVStepEnvSet()) { if (g_orig121a10) g_orig121a10(rdram, ctx, runtime); return; }
        const uint32_t nin = getRegU32(ctx, 6); const uint32_t poly = getRegU32(ctx, 4);
        // LOG ONLY: guard3 showed the recompiled clipper re-entering itself through the function table (ra inside
        // 0x121a10..0x121d48) with registers that are not this function's arguments -- clamping there would corrupt it.
        if (g_orig121a10) g_orig121a10(rdram, ctx, runtime);
        const uint32_t nout = getRegU32(ctx, 2); const uint32_t ra = getRegU32(ctx, 31);
        if (nout > 9u && (ra < 0x121a10u || ra >= 0x121d48u))
        {
            static std::atomic<uint32_t> s_m{0}; if (s_m.fetch_add(1u) < 40u)
            {
                float v[4] = {}; if (const uint8_t *q = getMemPtr(rdram, poly & 0x1FFFFFFFu)) std::memcpy(v, q, 16);
                std::fprintf(stderr, "[clipguard] func_121A10 in=%u out=%u (>9) ra=0x%x poly=0x%x v0=(%g %g %g %g) frame=%llu (log only)\n", nin, nout, ra, poly, v[0], v[1], v[2], v[3], (unsigned long long)g_bt3FrameCount.load());
            }
        }
    }
    // [netview] FUN_0023e770(viewObject, mode) is BT3's viewport configurator. $a1 selects:
    //     0 -> FULL SCREEN   : scissor (0,511,0,447),   viewport centre X 2048.0
    //     1 -> left half     : scissor (0,254,0,447),   centre X 1920.0  (2048 - 128)
    //     2 -> right half    : scissor (257,511,0,447), centre X 2176.0  (2048 + 128)
    //     >=2 other values return without doing anything.
    // It does NOT just set the scissor -- it calls func_121E28 (projection setup) with the
    // mode's parameters and stores the bounds into the view object at +0x208/+0x20C. That is
    // why splitscreen loses scenery: the narrower frustum culls props, so outside each half
    // only the terrain sheet (tbp0 10752) survives -- measured 14-15 distinct textures per
    // column inside a viewport versus exactly ONE outside it.
    // For "online, each player full-screen" we therefore do not widen the scissor by hand (that
    // gives a wider terrain vista with no props). We ask the GAME for its full-screen setup and
    // let its own culling follow. PS2X_NETVIEW=1|2 says which player this client is; pair it
    // with PS2X_VPKEEP so the other player's (still half-width) draws are dropped.
    void bt3NetViewSelect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_0023e770
    {
        // Follow the netplay player automatically when connected -- hosting makes you player 1,
        // joining player 2, so the full-screen viewport should not need its own env var. The env
        // var stays as an override for offline testing.
        static const int s_env = [](){ const char *v = ::getenv("PS2X_NETVIEW");
                                       return (v && v[0]) ? std::atoi(v) : 0; }();
        const int s_player = s_env ? s_env : (ps2NetActive() ? ps2NetLocalPlayer() : 0);
        if (s_player == 1 || s_player == 2)
        {
            const uint32_t mode = getRegU32(ctx, 5);
            // [statesync] SYMMETRIC (default under netplay; PS2X_NETVIEW_SYM=0 restores the old way): BOTH
            // players' views become full screen on BOTH machines, so the fight's guest state is identical
            // on the two sides (verified: 3 bytes differ instead of 12 KB, all in the sound block). The
            // local player's view is then chosen at the renderer, which tells the two identical views
            // apart by a mark the guest carries into its own SCISSOR: the view object's +0x208 is the
            // scissor's y0 (FUN_001027c8/FUN_00112548 pack it as y0 << 32), so player 1's view starts at
            // y = 1 and player 2's at y = 2. The HUD keeps y0 = 0 and is never dropped. One or two
            // pixel rows at the top of the view, in the overscan, are the whole visual cost.
            static const bool s_sym = [](){ const char *v = ::getenv("PS2X_NETVIEW_SYM"); return !(v && v[0] == '0'); }();
            if (s_sym && (mode == 1u || mode == 2u))
            {
                const uint32_t view = getRegU32(ctx, 4);
                static std::atomic<uint32_t> s_said{0};
                if (s_said.fetch_add(1u) < 2u)
                    std::fprintf(stderr, "[netview] player %d: viewport mode %u -> 0 (full screen, marked y0=%u)\n", s_player, mode, mode);
                ctx->r[5] = _mm_set_epi64x(0, 0);
                if (g_orig23e770) g_orig23e770(rdram, ctx, runtime);
                auto put32 = [&](uint32_t addr, uint32_t v) { if (uint8_t *q = getMemPtr(rdram, addr & 0x1FFFFFFFu)) std::memcpy(q, &v, sizeof v); };
                put32(view + 0x208u, mode);   // scissor y0 = 1 (P1) / 2 (P2)
                return;
            }
            // Leave the OTHER player's call untouched: it keeps its half-width viewport, and
            // PS2X_VPKEEP drops its draws at the rasteriser. Skipping this call instead would
            // leave that view object unconfigured and it would render from stale bounds.
            if (mode == static_cast<uint32_t>(s_player))
            {
                static std::atomic<uint32_t> s_said{0};
                if (s_said.fetch_add(1u) == 0u)
                    std::fprintf(stderr, "[netview] player %d: viewport mode %u -> 0 (full screen)\n", s_player, mode);
                ctx->r[5] = _mm_set_epi64x(0, 0);
            }
        }
        if (g_orig23e770) g_orig23e770(rdram, ctx, runtime);
    }
    void bt3ClipXformGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // func_121D48
    {
        // [fps60] inert at 30 fps: these guards exist for step-1 pathologies, so stock play stays byte-identical.
        if (!ps2VStepActive() && !ps2xVStepEnvSet()) { if (g_orig121d48) g_orig121d48(rdram, ctx, runtime); return; }
        const uint32_t n = getRegU32(ctx, 7);
        if (n > 9u)
        {
            static std::atomic<uint32_t> s_n{0}; const uint32_t k = s_n.fetch_add(1u);
            if (k < 40u)
            {
                const uint32_t src = getRegU32(ctx, 6);
                float v[8] = {};
                if (const uint8_t *q = getMemPtr(rdram, src & 0x1FFFFFFFu)) { std::memcpy(v, q, 16); std::memcpy(v + 4, q + 0x30, 16); }
                std::fprintf(stderr, "[clipguard] func_121D48 count=%u (>9: stack overrun) ra=0x%x src=0x%x v0=(%g %g %g %g) v1=(%g %g %g %g) frame=%llu -> clamped to 9\n",
                             n, getRegU32(ctx, 31), src, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], (unsigned long long)g_bt3FrameCount.load());
            }
            ctx->r[7] = _mm_set_epi64x(0, 9);
        }
        if (g_orig121d48) g_orig121d48(rdram, ctx, runtime);
    }
    void bt3AngleWrapGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // func_11F548(f12 angle, f13 half-range) -> f0
    {
        // [fps60] inert at 30 fps: these guards exist for step-1 pathologies, so stock play stays byte-identical.
        if (!ps2VStepActive() && !ps2xVStepEnvSet()) { if (g_orig11f548) g_orig11f548(rdram, ctx, runtime); return; }
        const float a = ctx->f[12], r = ctx->f[13];
        if (!(std::fabs(a) < 1.0e6f) || !(r > 1.0e-6f))
        {
            static std::atomic<uint32_t> s_n{0}; const uint32_t k = s_n.fetch_add(1u);
            if (k < 40u)
            {   // the usual caller is the wrapper at 0x11f588, which saved the OUTER return address at 0($sp)
                uint64_t outer = 0; if (const uint8_t *q = getMemPtr(rdram, getRegU32(ctx, 29) & 0x1FFFFFFFu)) std::memcpy(&outer, q, 8);
                std::fprintf(stderr, "[wrapguard] func_11F548 angle=%g range=%g ra=0x%x outer_ra=0x%llx frame=%llu -> returning 0 (would spin)\n",
                             a, r, getRegU32(ctx, 31), (unsigned long long)outer, (unsigned long long)g_bt3FrameCount.load());
            }
            ctx->f[0] = 0.0f; ctx->f[12] = 0.0f;
            return;
        }
        if (g_orig11f548) g_orig11f548(rdram, ctx, runtime);
    }
    // [fightgate] the step override applies only while the fight is underway (see ps2_stepcensus.cpp): the intro at
    // step 1 corrupted memory in four runs out of five. PS2X_VSTEP_ALWAYS=1 restores the ungated behaviour.
    void bt3VStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const int s_envStep = [](){ const char *v = std::getenv("PS2X_VSTEP"); return v && v[0] ? std::atoi(v) : 0; }();
        const int s_step = ps2VStepActive() ? 1 : s_envStep;   // [fps60] the overlay's toggle, else the env override
        static const bool s_always = [](){ const char *v = std::getenv("PS2X_VSTEP_ALWAYS"); return v && v[0] && v[0] != '0'; }();
        if (s_step > 0 && getRegU32(ctx, 4) == 2u && (s_always || ps2HalfStepFightActive())) ctx->r[4] = _mm_set_epi64x(0, (int64_t)s_step);   // $a0 = step
        if (g_orig102060) g_orig102060(rdram, ctx, runtime);
    }
    // [fps60 predict] FUN_001de8a8(obj, out, ..., f12 = own speed/frame, f13, f14 = reach frames, f15 = N frames):
    // the slam-dive setup (state 0x30, FUN_001f3668) aims at "opponent position + opponent velocity/frame * N".
    // At 60 fps the per-frame velocity is halved by the pacing table while N is a frame count the game never
    // scales, so the aim point fell short (the teleport slam missed a sliding victim). Double N in 60 fps mode.
    PS2Runtime::RecompiledFunction g_orig1de8a8 = nullptr;
    void bt3PredictAhead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> s_n{0}; const uint32_t n = s_n.fetch_add(1u);
        const bool on = ps2VStepActive() && ps2HalfStepFightActive();
        if (n < 8u)
            std::fprintf(stderr, "[fps60] predict f12=%g f13=%g f14=%g f15=%g %s frame=%llu\n", ctx->f[12], ctx->f[13], ctx->f[14], ctx->f[15],
                         on ? "(N doubled)" : "", (unsigned long long)g_bt3FrameCount.load());
        if (on) ctx->f[15] *= 2.0f;
        if (g_orig1de8a8) g_orig1de8a8(rdram, ctx, runtime);
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
        ps2HalfStepNoteLogic(g_bt3FrameCount.load(std::memory_order_relaxed));   // [fightgate]
        {   // [fighttick] a stream marker for this fight update: the native renderer stamps the frame lists with it, in
            // stream order, so "fight tick N" names the same list in every run of a replay (the vblank flip's own stamp
            // jitters by a frame: the handler preempts the game thread wherever it happens to be)
            const unsigned long long tick = ps2FightTicks();
            if (PS2Memory::asyncKickEnabled()) { PS2Memory::KickJob j; j.kind = PS2Memory::KickJob::GsApply; j.fn = [tick]() { ps2xSeamTickMark(tick); }; runtime->memory().enqueueKickJob(std::move(j)); }
            else ps2xSeamTickMark(tick);
        }
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

    // [seamprobe] PS2X_SEAMPROBE=1 -- Phase 0 of docs/NATIVE-RENDER-SEAM.md. Taps, never edits:
    //   sub_00111358  the per-model draw loop (actor, entity, mode, TEX0 in a3)
    //   sub_00123278 and siblings  the "begin batch" builders; v0 = the header packet
    //   sub_00100798  end of display list; every pending block is filled by now
    // The probe hashes each finished constant block and matches it to the VIF unpack that later
    // delivers it, then charges the MSCAL/MSCNT that follow to that batch (ps2_seamprobe.cpp).
    PS2Runtime::RecompiledFunction g_origSeamDraw = nullptr;
    void bt3SeamDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_00111358
    {
        const uint64_t a3 = GPR_U64(ctx, 7);
        seamprobe::noteDrawEnter(getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                                 (uint32_t)a3, (uint32_t)(a3 >> 32), getRegU32(ctx, 31));
        if (g_origSeamDraw) g_origSeamDraw(rdram, ctx, runtime);
        seamprobe::noteDrawExit();
    }
    struct SeamBuilderHook { uint32_t addr; PS2Runtime::RecompiledFunction orig; };
    SeamBuilderHook g_seamBuilders[] = {
        {0x00123278u, nullptr}, {0x00123370u, nullptr}, {0x00123468u, nullptr}, {0x00123130u, nullptr},
        {0x001236b0u, nullptr}, {0x00123cd0u, nullptr}, {0x00123588u, nullptr}, {0x00123dc8u, nullptr},
    };
    template <int I>
    void bt3SeamBuilderHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        if (g_seamBuilders[I].orig) g_seamBuilders[I].orig(rdram, ctx, runtime);
        seamprobe::noteBuilder(g_seamBuilders[I].addr, getRegU32(ctx, 2), a0, ra);
    }
    PS2Runtime::RecompiledFunction g_origSeamListEnd = nullptr;
    void bt3SeamListEndHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_00100798
    {
        seamprobe::finalizeList(rdram);
        if (g_origSeamListEnd) g_origSeamListEnd(rdram, ctx, runtime);
    }

    // [kickprobe] the VIF1 DMA-send helpers: a0 = chain address; ra = the code that sent it
    struct KickHook { uint32_t addr; PS2Runtime::RecompiledFunction orig; };
    KickHook g_kickHooks[] = { {0x00100cc0u, nullptr}, {0x00100b98u, nullptr}, {0x00100d88u, nullptr} };
    template <int I>
    void bt3KickHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        seamprobe::noteKick(g_kickHooks[I].addr, getRegU32(ctx, 4), getRegU32(ctx, 31));
        if (g_kickHooks[I].orig) g_kickHooks[I].orig(rdram, ctx, runtime);
    }

    // [kickprobe] the three helpers that advance the display-list bump pointer (gp-0x59d8 = 0x2fe898): the batch
    // allocator 0x100850 and the packet open/close pair 0x1006e8 / 0x100738. Every advance [before, after) belongs
    // to the caller (ra); so do the bytes the caller wrote inline since the previous advance.
    struct AllocHook { uint32_t addr; PS2Runtime::RecompiledFunction orig; };
    AllocHook g_allocHooks[] = { {0x00100850u, nullptr}, {0x001006e8u, nullptr}, {0x00100738u, nullptr} };
    PS2Runtime::RecompiledFunction g_origAlloc = nullptr;   // 0x100850, kept for the install message
    template <int I>
    void bt3AllocHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31), gp = getRegU32(ctx, 28), ptrAddr = (gp - 0x59d8u) & 0x1FFFFFFu;
        uint32_t before = 0; std::memcpy(&before, rdram + ptrAddr, 4);
        if (g_allocHooks[I].orig) g_allocHooks[I].orig(rdram, ctx, runtime);
        uint32_t after = 0; std::memcpy(&after, rdram + ptrAddr, 4);
        seamprobe::noteAdvance(before, after, ra);
    }

    // [kickprobe] the generic packet emitters (88..300-byte helpers that build one sprite / quad / mask write): the
    // effect logic is their caller, so while one runs the list advances are charged to ITS ra instead.
    struct EmitHook { uint32_t addr; PS2Runtime::RecompiledFunction orig; };
    EmitHook g_emitHooks[] = {
        {0x00101298u, nullptr}, {0x00101548u, nullptr}, {0x001051c8u, nullptr}, {0x00105250u, nullptr}, {0x00105bd8u, nullptr}, {0x00108750u, nullptr},
        {0x0010a0a8u, nullptr}, {0x0010a110u, nullptr}, {0x00101210u, nullptr}, {0x00101400u, nullptr}, {0x00102b70u, nullptr}, {0x00116770u, nullptr},
        {0x001234e8u, nullptr}, {0x00101644u, nullptr}, {0x00101d40u, nullptr}, {0x00105c4cu, nullptr},
    };
    template <int I>
    void bt3EmitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        seamprobe::pushEmitter(g_emitHooks[I].addr, getRegU32(ctx, 31));
        if (g_emitHooks[I].orig) g_emitHooks[I].orig(rdram, ctx, runtime);
        seamprobe::popEmitter();
    }

    // [postskip] PS2X_POSTSKIP=1: the three post-processing orchestrators of the fight frame do nothing
    // (sub_00247578: depth mask / ink / glow composite; FUN_0010ff40: downscales, DoF masks, Z top-byte plane;
    // FUN_00247660). Step one of replacing them natively (docs/SEAM-POSTCHAIN.md).
    // [steporacle] PS2X_STEPORACLE=<hex addr>[:<nth call>] (default 200th): around that call of the step, with the GS stream
    // drained, dump the backend's VRAM and registers before and after into PS2X_STEPORACLE_DIR (default /tmp) as
    // oracle_before.bin/.txt and oracle_after.bin/.txt. The step itself runs as the game wrote it. Pins a pass's semantics.
    PS2Runtime::RecompiledFunction g_origOracleStep = nullptr; uint32_t g_oracleAddr = 0, g_oracleNth = 200;
    void bt3StepOracleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0; static bool s_done = false;
        const bool fire = !s_done && ++s_calls == g_oracleNth;
        static const char *s_dir = [](){ const char *v = std::getenv("PS2X_STEPORACLE_DIR"); return v && v[0] ? v : "/tmp"; }();
        char b[512], t[512];
        if (fire)
        {
            runtime->memory().drainKickQueue(false);
            std::snprintf(b, sizeof(b), "%s/oracle_before.bin", s_dir); std::snprintf(t, sizeof(t), "%s/oracle_before.txt", s_dir);
            const bool ok = ps2x_pgs::dumpVramRaw(b, t);
            std::fprintf(stderr, "[steporacle] step 0x%x call %u frame %llu: before dump %s\n", g_oracleAddr, s_calls, (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), ok ? "ok" : "FAILED");
        }
        if (g_origOracleStep) g_origOracleStep(rdram, ctx, runtime);
        if (fire)
        {
            runtime->memory().drainKickQueue(false);
            std::snprintf(b, sizeof(b), "%s/oracle_after.bin", s_dir); std::snprintf(t, sizeof(t), "%s/oracle_after.txt", s_dir);
            const bool ok = ps2x_pgs::dumpVramRaw(b, t);
            std::fprintf(stderr, "[steporacle] after dump %s\n", ok ? "ok" : "FAILED");
            s_done = true;
        }
    }
    // A replaced function must end like `jr $ra`: some of these are reached by a tail jump (FUN_0010ff40 -> func_111E50), and
    // a hook that leaves ctx->pc at its own entry makes the dispatcher unwind the caller chain with $sp out of step.
    void bt3PostSkipHook(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = getRegU32(ctx, 31); }
    // [postskip] sub_0010A218(ctxTable, slot, id, flag): the game's frame-context switch (FRAME/ZBUF/... packet from the
    // context table). Census of (id, caller) per 300 frames, to learn which targets each pass draws into.
    PS2Runtime::RecompiledFunction g_origCtxSwitch = nullptr;
    void bt3CtxSwitchHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::mutex mtx; static std::map<std::pair<uint32_t, uint32_t>, uint32_t> hist; static uint32_t lastPrint = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            ++hist[{getRegU32(ctx, 6), getRegU32(ctx, 31)}];
            const uint32_t fc = (uint32_t)g_bt3FrameCount.load(std::memory_order_relaxed);
            if (fc >= lastPrint + 300u && !hist.empty())
            {
                lastPrint = fc;
                std::fprintf(stderr, "[ctxswitch] frame %u: id(a2) x caller(ra) -> calls since last print\n", fc);
                for (const auto &kv : hist) std::fprintf(stderr, "[ctxswitch]   id 0x%x ra 0x%x : %u\n", kv.first.first, kv.first.second, kv.second);
                hist.clear();
            }
        }
        if (g_origCtxSwitch) g_origCtxSwitch(rdram, ctx, runtime);
    }
    // FUN_0010ff40 also renders the characters: replay only that part of it.
    //   s0 = func_248FC0()->[8]; if (func_2490F8(2)) { 102120; 2493A0; func_10FB80(s0); 24B118; if (func_2490F8(8)) func_10FC50(s0); 10FD98; 111E50 }
    void bt3PostBCharsOnlyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto f248fc0 = runtime->lookupFunction(0x00248fc0u), f2490f8 = runtime->lookupFunction(0x002490f8u);
        auto f10fb80 = runtime->lookupFunction(0x0010fb80u), f10fc50 = runtime->lookupFunction(0x0010fc50u);
        const uint32_t ra = getRegU32(ctx, 31);
        struct Ret { R5900Context *c; uint32_t ra; ~Ret() { c->pc = ra; SET_GPR_U32(c, 31, ra); } } ret{ctx, ra};
        if (!f248fc0 || !f2490f8 || !f10fb80 || !f10fc50) return;
        f248fc0(rdram, ctx, runtime);
        uint32_t v0 = getRegU32(ctx, 2), s0 = 0; std::memcpy(&s0, rdram + ((v0 + 8u) & 0x1FFFFFFu), 4);
        SET_GPR_U32(ctx, 4, 2u); f2490f8(rdram, ctx, runtime);
        if (!getRegU32(ctx, 2)) return;
        SET_GPR_U32(ctx, 4, s0); f10fb80(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 4, 8u); f2490f8(rdram, ctx, runtime);
        if (getRegU32(ctx, 2)) { SET_GPR_U32(ctx, 4, s0); f10fc50(rdram, ctx, runtime); }
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

    // [dethash] PS2X_DETHASH=N -- hash the guest's simulation state every N frames and print it.
    // This is the determinism gate for online play: record a fight's inputs once (PS2X_INREC),
    // replay it twice (PS2X_INPLAY) and diff the two hash logs. The FIRST differing frame names
    // the cause. Same-machine divergence must be fixed before cross-machine is even worth testing;
    // the two standing suspects are [asyncpace] (the guest polls CHCR busy, which tracks how fast
    // the host worker drains) and [framegate] (vsync cadence decided from measured g_workerFrameNs).
    // EE RAM only, deliberately: guest state lives there, while the CPU context is transient
    // between frames and the R5900Context holds host pointers that would hash differently by
    // construction. ~2-3 ms/frame for 32 MB, which is fine for a diagnostic.
    static void ps2DetHashFrame(const uint8_t *rdram, __m128 ctxR)
    {
        static const int s_every = [](){ const char *v = std::getenv("PS2X_DETHASH");
                                         const int n = (v && v[0]) ? std::atoi(v) : 0;
                                         if (n > 0) std::fprintf(stderr, "[dethash] hashing EE RAM every %d frame(s)\n", n);
                                         return n; }();
        if (s_every <= 0 || !rdram) return;
        const unsigned long long f = g_bt3FrameCount.load(std::memory_order_relaxed);
        if (f % (unsigned long long)s_every) return;
        const uint64_t h = XXH3_64bits(rdram, PS2_RAM_SIZE);
        // Second hash EXCLUDING the sound-stream / disc-stream window. Measured 2026-09-14: two
        // identical-input runs differ by only 12 bytes at frame 1 and 432 at frame 600, ALL of it
        // inside the sound stream control block at 0x2c9350 and its neighbours -- stream position
        // counters (0x25b vs 0x25c, byte offsets 0x46050 vs 0x46438) that advance on WALL CLOCK
        // because disc/ADX delivery is host-paced. Whether GAMEPLAY state is deterministic is the
        // question netplay actually depends on, so hash it separately.
        // PS2X_DETSKIP=0 reports only the full hash.
        static const bool s_skip = [](){ const char *v = std::getenv("PS2X_DETSKIP");
                                         return !(v && v[0] == '0'); }();
        if (s_skip)
        {
            constexpr uint32_t kStreamLo = 0x002c0000u, kStreamHi = 0x00300000u;   // sndblk dumps reach 0x2f72a0
            XXH3_state_t *st = XXH3_createState();
            XXH3_64bits_reset(st);
            XXH3_64bits_update(st, rdram, kStreamLo);
            XXH3_64bits_update(st, rdram + kStreamHi, PS2_RAM_SIZE - kStreamHi);
            const uint64_t g = XXH3_64bits_digest(st);
            XXH3_freeState(st);
            // [dethash] also report the RNG STREAM. The user's concern is the classic desync
            // cause: if particles/effects draw from the same rand() sequence as gameplay and the
            // two machines consume it at different rates, every later gameplay roll drifts.
            // calls = how many rand() calls have happened; state = the 64-bit LCG. Both matching
            // every frame means the stream is in lockstep, which is the thing that actually
            // matters -- BT3 particles are known rand() consumers (aura wisps) and there is a
            // SECOND generator, the VU0 R register, used by other effects.
            // [dethash] plus the VU0 R register -- BT3's SECOND random source, used by effects
            // (see [[bt3-kaioken-white]]: the recompiler's VU0 R ops were once invented outright).
            // NOTE VU1's R is a different story: RNEXT/RGET/RINIT/RXOR in ps2_vu1.cpp are all
            // no-ops, so VU1 has no random source at all -- trivially deterministic, but a real
            // rendering gap in its own right.
            ps2NetSetChecksum((uint32_t)f, g);   // [netplay] desync detector (needs PS2X_DETHASH=1)
            uint32_t r4[4]; std::memcpy(r4, &ctxR, sizeof r4);
            std::fprintf(stderr, "[dethash] frame %llu ee=%016llx gp=%016llx rng=%u:%016llx vu0r=%08x%08x%08x%08x\n",
                         f, (unsigned long long)h, (unsigned long long)g,
                         ps2_stubs::ps2RandCallCount(), (unsigned long long)ps2_stubs::ps2RandState(),
                         r4[0], r4[1], r4[2], r4[3]);
        }
        else
            std::fprintf(stderr, "[dethash] frame %llu ee=%016llx\n", f, (unsigned long long)h);
        {   // PS2X_DETDUMP=<frame> + PS2X_DETDUMPFILE=<path>: write EE RAM once, so two runs can be
            // diffed byte-for-byte to find WHICH addresses diverge rather than just that they do.
            // PS2X_DETDUMP takes a COMMA LIST of frames; PS2X_DETDUMPFILE is a prefix and each
            // frame lands in "<prefix>.<frame>.bin". Several dumps from ONE run is what finds a
            // screen/sub-state variable: a word that is constant while a menu screen is up and
            // changes only at transitions stands out across a handful of samples, with no need
            // to see the screen at all.
            static const std::vector<unsigned long long> s_at = [](){
                std::vector<unsigned long long> v;
                const char *e = std::getenv("PS2X_DETDUMP");
                if (e && e[0]) { const char *q = e; while (*q) { v.push_back(std::strtoull(q, nullptr, 10));
                                 const char *c = std::strchr(q, ','); if (!c) break; q = c + 1; } }
                return v; }();
            if (!s_at.empty() && std::find(s_at.begin(), s_at.end(), f) != s_at.end())
            {
                const char *fp = std::getenv("PS2X_DETDUMPFILE");
                if (fp && fp[0])
                {
                    char path[512];
                    if (s_at.size() > 1) std::snprintf(path, sizeof path, "%s.%llu.bin", fp, f);
                    else                 std::snprintf(path, sizeof path, "%s", fp);
                    if (std::FILE *o = std::fopen(path, "wb"))
                    {
                        std::fwrite(rdram, 1, PS2_RAM_SIZE, o);
                        std::fclose(o);
                        std::fprintf(stderr, "[dethash] dumped EE RAM at frame %llu -> %s\n", f, path);
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // [savestate] Snapshot / restore of the guest SIMULATION, for netplay's state sync at connect
    // (and, later, for rollback and for a fight-determinism test that does not inherit boot drift).
    //
    // WHY IT IS SAVED AND LOADED AT THE FRAME HOOK, and nowhere else:
    // the host C++ stack MIRRORS the guest call chain -- dispatchLoop's own comment notes that
    // "BT3's nested CDVD wait loops never unwind back to this top-level loop". So state cannot be
    // restored at an arbitrary point: the host stack would describe a call chain that no longer
    // matches guest memory. FUN_00100ab8 is reached the same way every frame, so two instances
    // sitting in this hook have STRUCTURALLY IDENTICAL host stacks, and swapping guest memory and
    // registers underneath them is safe. Save and load must therefore use this one site.
    //
    //   PS2X_SAVESTATE=<frame>:<path>   write a snapshot once, at that frame
    //   PS2X_LOADSTATE=<path>           restore it at the next frame hook, once
    //
    // v1 covers EE RAM, the scratchpad, the calling context and the RNG. NOT covered: IOP RAM
    // (sound), GS VRAM (picture only), VU memory, and the other guest threads' host stacks --
    // those threads are parked in their own nesting, so this is sound only while the cooperative
    // scheduler (PS2X_SCHED=1) keeps them out of the way. Widen it once v1 is proven.
    struct SaveHdr { char magic[8]; uint32_t version, ramSize, spSize, ctxSize; uint64_t frame, rand64;
                     uint32_t randCalls, iopSize, vu0Size, vu1Size, vramSize;
                     uint32_t vu0CodeSize, vu1CodeSize, vuStateSize, sinkCount, gsRegCount; };
    // GS PRIVILEGED registers (PMODE / DISPFB1,2 / DISPLAY1,2 / ...). I first left these out as
    // "picture only, cannot cause a desync" -- true about desync, wrong about being optional: they
    // are what SELECT the framebuffer being shown, so a restored instance displayed whatever its
    // own boot had left configured and came up BLACK. The guest does not re-emit them, because it
    // resumes mid-execution long after it set the display up.
    // GSRegisters is 19 uint64s (there is a static_assert on that) but holds `csr` as an atomic,
    // so it is packed field by field rather than copied.
    struct GsRegSer { uint64_t v[19]; };
    static void gsRegPack(const GSRegisters &g, GsRegSer &o)
    {
        o.v[0]=g.pmode;   o.v[1]=g.smode1;  o.v[2]=g.smode2;   o.v[3]=g.srfsh;
        o.v[4]=g.synch1;  o.v[5]=g.synch2;  o.v[6]=g.syncv;
        o.v[7]=g.dispfb1; o.v[8]=g.display1; o.v[9]=g.dispfb2; o.v[10]=g.display2;
        o.v[11]=g.extbuf; o.v[12]=g.extdata; o.v[13]=g.extwrite; o.v[14]=g.bgcolor;
        o.v[15]=g.csr.load(std::memory_order_relaxed);
        o.v[16]=g.imr;    o.v[17]=g.busdir; o.v[18]=g.siglblid;
    }
    static void gsRegUnpack(const GsRegSer &o, GSRegisters &g)
    {
        g.pmode=o.v[0];   g.smode1=o.v[1];  g.smode2=o.v[2];   g.srfsh=o.v[3];
        g.synch1=o.v[4];  g.synch2=o.v[5];  g.syncv=o.v[6];
        g.dispfb1=o.v[7]; g.display1=o.v[8]; g.dispfb2=o.v[9]; g.display2=o.v[10];
        g.extbuf=o.v[11]; g.extdata=o.v[12]; g.extwrite=o.v[13]; g.bgcolor=o.v[14];
        // csr (o.v[15]) is deliberately NOT restored: the vsync worker toggles its FIELD bit and
        // the GIF sets SIGNAL/FINISH from other threads, so a stale value would either clobber the
        // live field parity or re-raise an interrupt flag that has already been serviced. It is
        // saved for diagnostics only.
        g.imr=o.v[16];    g.busdir=o.v[17]; g.siglblid=o.v[18];
    }
    // Peek just the frame number a snapshot was taken at, without reading the 38 MB body.
    static uint64_t bt3PeekStateFrame(const char *path)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f) return 0u;
        SaveHdr h{};
        const bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, "BT3STATE", 8) == 0;
        std::fclose(f);
        return ok ? h.frame : 0u;
    }
    // v3 adds the state that lives on the HOST side of the emulation rather than in guest memory,
    // which is what v2 still inherited from the loading instance's own boot:
    //   * VU0/VU1 MICRO memory -- the uploaded microprograms. Guest RAM holds the source, but the
    //     copy VU1 actually executes is in ps2xRuntime's own buffer.
    //   * The VU interpreters' registers (vf/vi/ACC/Q and the Q pipeline). m_vu0/m_vu1 are
    //     PS2Runtime members, so these persist between kicks and are genuinely live state.
    //   * The IOP sound sinks' stream bookkeeping -- measured as the ONLY bytes that differ
    //     between two instances at frame 1 ([[bt3-determinism]]).
    // NOT yet covered: GS register state (VRAM is saved, so this is picture-only), and the other
    // guest threads' host stacks -- sound only while PS2X_SCHED=1 parks them.
    //
    // The sinks cannot be memcpy'd: IopSink holds steady_clock::time_points, whose epoch is
    // per-process, so a snapshot moved to another machine (or reloaded in a later run) would carry
    // a meaningless origin. Serialise the byte counters and RE-ANCHOR the clocks to "now" on load,
    // which is what the pacing code would do for a stream that has just started.
    struct SinkSer { uint32_t key, streamId; uint64_t returnedBytes, heldBytes, wallBaseBytes,
                     frameBase, frameBaseBytes; uint8_t wallClock, ringFullIdle, frameClock, pad; };
    static bool bt3SaveState(const char *path, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::FILE *f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "[savestate] cannot write %s\n", path); return false; }
        uint8_t *sp = ps2GetScratchpadHostPtr();
        SaveHdr h{}; std::memcpy(h.magic, "BT3STATE", 8);
        // v2 adds every other guest-visible memory region. v1 restored only EE RAM + scratchpad
        // and diverged one frame after the load, because the loading instance kept its OWN sound
        // (IOP), VU and VRAM state from its own boot and those write back into EE RAM.
        PS2Memory &mem = runtime->memory();
        // Collect the sinks BEFORE the header goes out: the count belongs in it, and the lock
        // should not be held across file writes.
        std::vector<SinkSer> sinks;
        {
            std::lock_guard<std::mutex> lk(g_iopSinkM);
            sinks.reserve(g_iopSinks.size());
            for (const auto &kv : g_iopSinks)
            {
                const IopSink &v = kv.second;
                sinks.push_back(SinkSer{ kv.first, v.streamId, v.returnedBytes, v.heldBytes,
                                         v.wallBaseBytes, v.frameBase, v.frameBaseBytes,
                                         (uint8_t)v.wallClock, (uint8_t)v.ringFullIdle,
                                         (uint8_t)v.frameClock, 0u });
            }
        }
        h.version = 3u; h.ramSize = PS2_RAM_SIZE; h.spSize = sp ? PS2_SCRATCHPAD_SIZE : 0u;
        h.ctxSize = (uint32_t)sizeof(R5900Context);
        h.iopSize = 2u * 1024u * 1024u; h.vu0Size = PS2_VU0_DATA_SIZE;
        h.vu1Size = PS2_VU1_DATA_SIZE;   h.vramSize = (uint32_t)PS2_GS_VRAM_SIZE;
        h.vu0CodeSize = PS2_VU0_CODE_SIZE; h.vu1CodeSize = PS2_VU1_CODE_SIZE;
        h.vuStateSize = (uint32_t)sizeof(VU1State); h.sinkCount = (uint32_t)sinks.size();
        h.gsRegCount = 19u;
        h.frame = g_bt3FrameCount.load(std::memory_order_relaxed);
        h.rand64 = ps2_stubs::ps2RandState(); h.randCalls = ps2_stubs::ps2RandCallCount();
        std::fwrite(&h, sizeof h, 1, f);
        std::fwrite(rdram, 1, PS2_RAM_SIZE, f);
        if (sp) std::fwrite(sp, 1, PS2_SCRATCHPAD_SIZE, f);
        std::fwrite(ctx, sizeof(R5900Context), 1, f);
        std::fwrite(mem.getIOPRAM(),  1, h.iopSize,  f);
        std::fwrite(mem.getVU0Data(), 1, h.vu0Size,  f);
        std::fwrite(mem.getVU1Data(), 1, h.vu1Size,  f);
        std::fwrite(mem.getGSVRAM(),  1, h.vramSize, f);
        std::fwrite(mem.getVU0Code(), 1, h.vu0CodeSize, f);
        std::fwrite(mem.getVU1Code(), 1, h.vu1CodeSize, f);
        { const VU1State v0 = runtime->vu0().state(); std::fwrite(&v0, sizeof v0, 1, f); }
        { const VU1State v1 = runtime->vu1().state(); std::fwrite(&v1, sizeof v1, 1, f); }
        if (!sinks.empty()) std::fwrite(sinks.data(), sizeof(SinkSer), sinks.size(), f);
        { GsRegSer gr{}; gsRegPack(mem.gs(), gr); std::fwrite(&gr, sizeof gr, 1, f); }
        std::fclose(f);
        std::fprintf(stderr, "[savestate] saved frame %llu -> %s (%.1f MB: ee+sp+ctx+iop+vu+vram"
                     "+vucode+vustate+%u sinks)\n",
                     (unsigned long long)h.frame, path,
                     (PS2_RAM_SIZE + h.spSize + h.ctxSize + h.iopSize + h.vu0Size + h.vu1Size
                      + h.vramSize + h.vu0CodeSize + h.vu1CodeSize) / 1048576.0, h.sinkCount);
        return true;
    }
    static bool bt3LoadState(const char *path, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f) { std::fprintf(stderr, "[savestate] cannot read %s\n", path); return false; }
        SaveHdr h{};
        if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(h.magic, "BT3STATE", 8) != 0 ||
            h.version != 3u || h.ramSize != PS2_RAM_SIZE || h.ctxSize != sizeof(R5900Context))
        { std::fprintf(stderr, "[savestate] %s is not a matching snapshot\n", path); std::fclose(f); return false; }
        if (std::fread(rdram, 1, PS2_RAM_SIZE, f) != PS2_RAM_SIZE) { std::fclose(f); return false; }
        if (h.spSize)
        {
            uint8_t *sp = ps2GetScratchpadHostPtr();
            if (sp && h.spSize == PS2_SCRATCHPAD_SIZE) std::fread(sp, 1, h.spSize, f);
            else std::fseek(f, (long)h.spSize, SEEK_CUR);
        }
        // The context is restored EXCEPT pc: we are inside the frame hook, and the host stack
        // expects to return through it normally. Guest pc/ra are re-established by that return.
        R5900Context tmp{};
        if (std::fread(&tmp, sizeof tmp, 1, f) != 1) { std::fclose(f); return false; }
        const uint32_t keepPc = ctx->pc;
        *ctx = tmp; ctx->pc = keepPc;
        {
            PS2Memory &mem = runtime->memory();
            if (h.iopSize  == 2u * 1024u * 1024u)   std::fread(mem.getIOPRAM(),  1, h.iopSize,  f);
            if (h.vu0Size  == PS2_VU0_DATA_SIZE)    std::fread(mem.getVU0Data(), 1, h.vu0Size,  f);
            if (h.vu1Size  == PS2_VU1_DATA_SIZE)    std::fread(mem.getVU1Data(), 1, h.vu1Size,  f);
            if (h.vramSize == PS2_GS_VRAM_SIZE)     std::fread(mem.getGSVRAM(),  1, h.vramSize, f);
            if (h.vu0CodeSize == PS2_VU0_CODE_SIZE) std::fread(mem.getVU0Code(), 1, h.vu0CodeSize, f);
            if (h.vu1CodeSize == PS2_VU1_CODE_SIZE) std::fread(mem.getVU1Code(), 1, h.vu1CodeSize, f);
            if (h.vuStateSize == sizeof(VU1State))
            {
                VU1State v{};
                if (std::fread(&v, sizeof v, 1, f) == 1) runtime->vu0().state() = v;
                if (std::fread(&v, sizeof v, 1, f) == 1) runtime->vu1().state() = v;
            }
            else std::fseek(f, (long)(2u * h.vuStateSize), SEEK_CUR);
        }
        if (h.sinkCount)
        {
            std::vector<SinkSer> sinks(h.sinkCount);
            if (std::fread(sinks.data(), sizeof(SinkSer), h.sinkCount, f) == h.sinkCount)
            {
                // Re-anchor the clocks: the saved epoch is meaningless in this process, and a
                // stream resuming from restored byte counts is exactly a stream that has just
                // started. frameBase is a GUEST frame number, so it transfers as-is.
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lk(g_iopSinkM);
                for (const SinkSer &ss : sinks)
                {
                    IopSink &d = g_iopSinks[ss.key];
                    d.streamId = ss.streamId; d.returnedBytes = ss.returnedBytes;
                    d.heldBytes = ss.heldBytes; d.wallBaseBytes = ss.wallBaseBytes;
                    d.frameBase = ss.frameBase; d.frameBaseBytes = ss.frameBaseBytes;
                    d.wallClock = ss.wallClock != 0; d.ringFullIdle = ss.ringFullIdle != 0;
                    d.frameClock = ss.frameClock != 0;
                    d.wallBase = now; d.ringFullSince = now;
                }
            }
        }
        if (h.gsRegCount == 19u)
        {
            GsRegSer gr{};
            if (std::fread(&gr, sizeof gr, 1, f) == 1) gsRegUnpack(gr, runtime->memory().gs());
        }
        std::fclose(f);
        g_bt3FrameCount.store(h.frame, std::memory_order_relaxed);
        ps2_stubs::ps2RandRestore(h.rand64, h.randCalls);
        std::fprintf(stderr, "[savestate] restored frame %llu from %s (v%u, %u sinks)\n",
                     (unsigned long long)h.frame, path, h.version, h.sinkCount);
        return true;
    }

    // [rollback] In-memory snapshot of the guest simulation for rollback: the same regions and
    // device state savestate v3 writes to disk, kept as one heap object so a restore is a handful
    // of memcpys (a 32 MB copy is ~0.5 ms). The fiber stacks and scheduler state are the runtime's
    // half (Ps2xRollback in ps2_runtime.cpp); the two are captured together at a frame gate.
    struct SimSnap
    {
        uint64_t frame = 0, rand64 = 0; uint32_t randCalls = 0;
        std::vector<uint8_t> ram, sp, iop, vu0d, vu1d, vram, vu0c, vu1c;
        VU1State v0{}, v1{};
        std::vector<SinkSer> sinks;
        GsRegSer gs{};
        std::vector<SeVoice> seVoices;   // HLE sound-effect voices (host side of the SE stream)
        // [rollback] The sound HLE's own host bookkeeping, copied whole (time points are on the
        // virtual clock in stepped mode, so they transfer exactly): the sinks with their clocks,
        // the consumer's ring cursors, the stereo-pair balance, stream-start times, feed limiter.
        std::map<uint32_t, IopSink> sinksFull;
        decltype(g_sinkRings) rings;
        uint32_t pairSink[2] = {0u, 0u}; uint64_t pairReturns[2] = {0u, 0u};
        decltype(g_streamStart) streamStart;
        decltype(g_sndRateLast) rateLast;
        uint64_t seTickBase = 0, seTickCarry = 0;
        Bt3DevDoneSer devSlots[8] = {};   // [statesync] the CD device-done latches (bt3CdStateEdge)
    };
    static void snapCopy(std::vector<uint8_t> &dst, const uint8_t *src, size_t n) { dst.resize(n); if (n) std::memcpy(dst.data(), src, n); }
    extern "C" void *ps2xSimSnapCapture(PS2Runtime *runtime, uint8_t *rdram)
    {
        SimSnap *s = new SimSnap();
        PS2Memory &mem = runtime->memory();
        s->frame = g_bt3FrameCount.load(std::memory_order_relaxed);
        s->rand64 = ps2_stubs::ps2RandState(); s->randCalls = ps2_stubs::ps2RandCallCount();
        snapCopy(s->ram, rdram, PS2_RAM_SIZE);
        if (uint8_t *sp = ps2GetScratchpadHostPtr()) snapCopy(s->sp, sp, PS2_SCRATCHPAD_SIZE);
        snapCopy(s->iop,  mem.getIOPRAM(),  2u * 1024u * 1024u);
        snapCopy(s->vu0d, mem.getVU0Data(), PS2_VU0_DATA_SIZE);
        snapCopy(s->vu1d, mem.getVU1Data(), PS2_VU1_DATA_SIZE);
        snapCopy(s->vram, mem.getGSVRAM(),  PS2_GS_VRAM_SIZE);
        snapCopy(s->vu0c, mem.getVU0Code(), PS2_VU0_CODE_SIZE);
        snapCopy(s->vu1c, mem.getVU1Code(), PS2_VU1_CODE_SIZE);
        s->v0 = runtime->vu0().state(); s->v1 = runtime->vu1().state();
        {
            std::lock_guard<std::mutex> lk(g_iopSinkM);
            for (const auto &kv : g_iopSinks)
            {
                const IopSink &v = kv.second;
                s->sinks.push_back(SinkSer{ kv.first, v.streamId, v.returnedBytes, v.heldBytes, v.wallBaseBytes,
                                            v.frameBase, v.frameBaseBytes, (uint8_t)v.wallClock, (uint8_t)v.ringFullIdle,
                                            (uint8_t)v.frameClock, 0u });
            }
        }
        gsRegPack(mem.gs(), s->gs);
        { std::lock_guard<std::mutex> lk(g_seVoiceM); s->seVoices = g_seVoices; }
        { std::lock_guard<std::mutex> lk(g_iopSinkM); s->sinksFull = g_iopSinks; }
        { std::lock_guard<std::mutex> lk(g_sinkRingM); s->rings = g_sinkRings; s->pairSink[0] = g_pairSink[0]; s->pairSink[1] = g_pairSink[1];
          s->pairReturns[0] = g_pairReturns[0]; s->pairReturns[1] = g_pairReturns[1]; }
        { std::lock_guard<std::mutex> lk(g_streamStartM); s->streamStart = g_streamStart; }
        { std::lock_guard<std::mutex> lk(g_sndRateM); s->rateLast = g_sndRateLast; }
        s->seTickBase = g_seTickBase; s->seTickCarry = g_seTickCarry;
        bt3DevSlotsCapture(s->devSlots);
        return s;
    }
    extern "C" bool ps2xSimSnapRestore(void *h, PS2Runtime *runtime, uint8_t *rdram)
    {
        const SimSnap *s = static_cast<const SimSnap *>(h);
        if (!s || s->ram.size() != PS2_RAM_SIZE) return false;
        PS2Memory &mem = runtime->memory();
        std::memcpy(rdram, s->ram.data(), PS2_RAM_SIZE);
        if (uint8_t *sp = ps2GetScratchpadHostPtr()) if (s->sp.size() == PS2_SCRATCHPAD_SIZE) std::memcpy(sp, s->sp.data(), PS2_SCRATCHPAD_SIZE);
        std::memcpy(mem.getIOPRAM(),  s->iop.data(),  s->iop.size());
        std::memcpy(mem.getVU0Data(), s->vu0d.data(), s->vu0d.size());
        std::memcpy(mem.getVU1Data(), s->vu1d.data(), s->vu1d.size());
        std::memcpy(mem.getGSVRAM(),  s->vram.data(), s->vram.size());
        std::memcpy(mem.getVU0Code(), s->vu0c.data(), s->vu0c.size());
        std::memcpy(mem.getVU1Code(), s->vu1c.data(), s->vu1c.size());
        runtime->vu0().state() = s->v0; runtime->vu1().state() = s->v1;
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(g_iopSinkM);
            for (const SinkSer &ss : s->sinks)
            {
                IopSink &d = g_iopSinks[ss.key];
                d.streamId = ss.streamId; d.returnedBytes = ss.returnedBytes; d.heldBytes = ss.heldBytes;
                d.wallBaseBytes = ss.wallBaseBytes; d.frameBase = ss.frameBase; d.frameBaseBytes = ss.frameBaseBytes;
                d.wallClock = ss.wallClock != 0; d.ringFullIdle = ss.ringFullIdle != 0; d.frameClock = ss.frameClock != 0;
                d.wallBase = now; d.ringFullSince = now;
            }
        }
        gsRegUnpack(s->gs, mem.gs());
        { std::lock_guard<std::mutex> lk(g_seVoiceM); g_seVoices = s->seVoices; }
        { std::lock_guard<std::mutex> lk(g_iopSinkM); g_iopSinks = s->sinksFull; }   // exact clocks, no re-anchoring
        { std::lock_guard<std::mutex> lk(g_sinkRingM); g_sinkRings = s->rings; g_pairSink[0] = s->pairSink[0]; g_pairSink[1] = s->pairSink[1];
          g_pairReturns[0] = s->pairReturns[0]; g_pairReturns[1] = s->pairReturns[1]; }
        { std::lock_guard<std::mutex> lk(g_streamStartM); g_streamStart = s->streamStart; }
        { std::lock_guard<std::mutex> lk(g_sndRateM); g_sndRateLast = s->rateLast; }
        g_seTickBase = s->seTickBase; g_seTickCarry = s->seTickCarry;
        bt3DevSlotsRestore(s->devSlots);
        g_bt3FrameCount.store(s->frame, std::memory_order_relaxed);
        ps2_stubs::ps2RandRestore(s->rand64, s->randCalls);
        return true;
    }
    extern "C" void ps2xSimSnapFree(void *h) { delete static_cast<SimSnap *>(h); }
    // [statesync] Portable form of the simulation snapshot (same binary on both ends: PODs go raw;
    // the sound HLE's time points are on the virtual clock in stepped mode, so they travel as ns).
    extern "C" bool ps2xSimSnapSerialize(const void *h, std::vector<uint8_t> &out)
    {
        const SimSnap *s = static_cast<const SimSnap *>(h);
        if (!s) return false;
        Ps2xByteW w(out);
        w.u32(0x53494d31u);   // 'SIM1'
        w.u64(s->frame); w.u64(s->rand64); w.u32(s->randCalls);
        w.bytes(s->ram); w.bytes(s->sp); w.bytes(s->iop); w.bytes(s->vu0d); w.bytes(s->vu1d); w.bytes(s->vram); w.bytes(s->vu0c); w.bytes(s->vu1c);
        w.pod(s->v0); w.pod(s->v1);
        w.podVec(s->sinks);
        w.pod(s->gs);
        w.u64(s->seVoices.size());
        for (const SeVoice &v : s->seVoices) { w.u32(v.serial); w.podVec(v.pcm); w.u64(v.pos); }
        w.u64(s->sinksFull.size());
        for (const auto &kv : s->sinksFull)
        {
            const IopSink &v = kv.second;
            w.u32(kv.first); w.u32(v.streamId); w.u64(v.returnedBytes); w.u64(v.heldBytes); w.u8(v.wallClock); w.tp(v.wallBase);
            w.u64(v.wallBaseBytes); w.u8(v.ringFullIdle); w.tp(v.ringFullSince); w.u8(v.frameClock); w.u64(v.frameBase); w.u64(v.frameBaseBytes);
        }
        w.u64(s->rings.size());
        for (const auto &kv : s->rings) { w.u32(kv.first); w.podVec(kv.second.bufs); w.u64(kv.second.next); }
        w.u32(s->pairSink[0]); w.u32(s->pairSink[1]); w.u64(s->pairReturns[0]); w.u64(s->pairReturns[1]);
        w.u64(s->streamStart.size()); for (const auto &kv : s->streamStart) { w.u32(kv.first); w.tp(kv.second); }
        w.u64(s->rateLast.size());    for (const auto &kv : s->rateLast)    { w.u32(kv.first); w.tp(kv.second); }
        w.u64(s->seTickBase); w.u64(s->seTickCarry);
        w.raw(s->devSlots, sizeof s->devSlots);
        w.u32(0x53494d45u);   // 'SIME'
        return true;
    }
    extern "C" void *ps2xSimSnapDeserialize(const uint8_t *data, size_t n, size_t *used)
    {
        Ps2xByteR r(data, n);
        if (r.u32() != 0x53494d31u) return nullptr;
        SimSnap *s = new SimSnap();
        s->frame = r.u64(); s->rand64 = r.u64(); s->randCalls = r.u32();
        r.bytes(s->ram); r.bytes(s->sp); r.bytes(s->iop); r.bytes(s->vu0d); r.bytes(s->vu1d); r.bytes(s->vram); r.bytes(s->vu0c); r.bytes(s->vu1c);
        s->v0 = r.pod<VU1State>(); s->v1 = r.pod<VU1State>();
        r.podVec(s->sinks);
        s->gs = r.pod<GsRegSer>();
        { const size_t k = r.count(4); s->seVoices.resize(r.ok ? k : 0);
          for (SeVoice &v : s->seVoices) { v.serial = r.u32(); r.podVec(v.pcm); v.pos = (size_t)r.u64(); } }
        { const size_t k = r.count(8);
          for (size_t i = 0; i < k && r.ok; ++i)
          {
              const uint32_t key = r.u32(); IopSink &v = s->sinksFull[key];
              v.streamId = r.u32(); v.returnedBytes = r.u64(); v.heldBytes = r.u64(); v.wallClock = r.u8() != 0; v.wallBase = r.tp();
              v.wallBaseBytes = r.u64(); v.ringFullIdle = r.u8() != 0; v.ringFullSince = r.tp(); v.frameClock = r.u8() != 0; v.frameBase = r.u64(); v.frameBaseBytes = r.u64();
          } }
        { const size_t k = r.count(8);
          for (size_t i = 0; i < k && r.ok; ++i) { const uint32_t key = r.u32(); SinkRing &ring = s->rings[key]; r.podVec(ring.bufs); ring.next = (size_t)r.u64(); } }
        s->pairSink[0] = r.u32(); s->pairSink[1] = r.u32(); s->pairReturns[0] = r.u64(); s->pairReturns[1] = r.u64();
        { const size_t k = r.count(12); for (size_t i = 0; i < k && r.ok; ++i) { const uint32_t key = r.u32(); s->streamStart[key] = r.tp(); } }
        { const size_t k = r.count(12); for (size_t i = 0; i < k && r.ok; ++i) { const uint32_t key = r.u32(); s->rateLast[key] = r.tp(); } }
        s->seTickBase = r.u64(); s->seTickCarry = r.u64();
        r.raw(s->devSlots, sizeof s->devSlots);
        if (r.u32() != 0x53494d45u || !r.ok || s->ram.size() != PS2_RAM_SIZE) { delete s; return nullptr; }
        if (used) *used = (size_t)(r.p - data);
        return s;
    }
    extern "C" const uint8_t *ps2xSimSnapRam(const void *h) { const SimSnap *s = static_cast<const SimSnap *>(h); return s && s->ram.size() == PS2_RAM_SIZE ? s->ram.data() : nullptr; }
    extern "C" uint64_t ps2xSimSnapFrame(const void *h) { const SimSnap *s = static_cast<const SimSnap *>(h); return s ? s->frame : 0u; }
    extern "C" uint64_t ps2xRamHash(const uint8_t *rdram, uint32_t skipLo, uint32_t skipHi)
    {   // 64-bit FNV-1a over 8-byte words (~10 ms for 32 MB), optionally skipping [skipLo, skipHi)
        uint64_t h = 1469598103934665603ull;
        const uint64_t *w = reinterpret_cast<const uint64_t *>(rdram);
        const size_t lo = skipLo / 8u, hi = skipHi / 8u;
        for (size_t i = 0; i < PS2_RAM_SIZE / 8u; ++i) { if (i >= lo && i < hi) continue; h ^= w[i]; h *= 1099511628211ull; }
        return h;
    }

    // [memwatch] PS2X_MEMWATCH=<hex>[,<hex>...] -- print these EE words whenever any of them
    // changes. Built to verify the RetroAchievements-documented menu variables against this
    // build, since a static dump cannot: menu variables are typically only meaningful WHILE their
    // screen is up, so they must be watched live while the cursor moves.
    //   RA notes: 0x6af7a0 versus mode (0 = 1vCPU, 1 = 1v2, 2 = CPUvCPU)
    //             0x6af7a4 battle type (0 = Single, 1 = Team)
    //             0x6af1ac current mode
    static void bt3MemWatch(uint8_t *rdram)
    {
        static std::vector<uint32_t> s_addrs = [](){
            std::vector<uint32_t> v; const char *e = std::getenv("PS2X_MEMWATCH");
            if (e && e[0]) { const char *q = e;
                while (*q) { v.push_back((uint32_t)std::strtoul(q, nullptr, 16) & 0x1FFFFFFFu);
                             const char *c = std::strchr(q, ','); if (!c) break; q = c + 1; } }
            if (!v.empty()) { std::fprintf(stderr, "[memwatch] watching %zu slot(s)\n", v.size()); }
            return v; }();
        {   // [ramscan] PS2X_RAMSCAN=<hex bytes>: every 20 frames, list where that byte pattern sits in EE RAM (first 8 hits,
            // printed when the set changes) -- finds the struct that holds a value seen in a GS packet
            static std::vector<uint8_t> s_pat = [](){ std::vector<uint8_t> v; const char *e = std::getenv("PS2X_RAMSCAN");
                if (e) for (size_t i = 0; e[i] && e[i + 1]; i += 2) v.push_back((uint8_t)std::strtoul(std::string(e + i, 2).c_str(), nullptr, 16)); return v; }();
            if (!s_pat.empty() && rdram)
            {
                static uint64_t s_last = 0; const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
                if (fr >= s_last + 20u)
                {
                    s_last = fr; std::vector<uint32_t> hits; const size_t n = 32u * 1024u * 1024u;
                    for (const uint8_t *q = rdram, *end = rdram + n; hits.size() < 48u; )
                    { q = std::search(q, end, s_pat.begin(), s_pat.end()); if (q == end) break; hits.push_back((uint32_t)(q - rdram)); ++q; }   // std::search, not memmem: no memmem on Windows
                    static std::vector<uint32_t> s_prevHits;
                    if (hits != s_prevHits) { s_prevHits = hits; std::fprintf(stderr, "[ramscan] frame %llu:", (unsigned long long)fr); for (uint32_t a : hits) std::fprintf(stderr, " 0x%x", a); std::fprintf(stderr, "%s\n", hits.empty() ? " (none)" : ""); }
                }
            }
        }
        {   // [ramdump] PS2X_RAMDUMP=<frame>[,<path>]: write the 32 MB of EE RAM once at that game frame (default work/ramdump.bin)
            static const uint64_t s_at = [](){ const char *e = std::getenv("PS2X_RAMDUMP"); return e && e[0] ? (uint64_t)std::strtoull(e, nullptr, 0) : 0ull; }();
            static bool s_done = false;
            if (s_at && !s_done && rdram && g_bt3FrameCount.load(std::memory_order_relaxed) >= s_at)
            {
                s_done = true; const char *e = std::getenv("PS2X_RAMDUMP"); const char *c = std::strchr(e, ',');
                const char *path = c ? c + 1 : "ramdump.bin";
                if (FILE *f = std::fopen(path, "wb")) { std::fwrite(rdram, 1, 32u * 1024u * 1024u, f); if (const uint8_t *spr = ps2GetScratchpadHostPtr()) std::fwrite(spr, 1, 16384, f); std::fclose(f); std::fprintf(stderr, "[ramdump] frame %llu -> %s (+16 KB scratchpad)\n", (unsigned long long)g_bt3FrameCount.load(), path); }
            }
        }
        if (s_addrs.empty() || !rdram) return;
        static std::vector<uint32_t> s_prev(s_addrs.size(), 0xdeadbeefu);
        bool changed = false;
        std::vector<uint32_t> cur(s_addrs.size());
        for (size_t i = 0; i < s_addrs.size(); ++i)
        {
            std::memcpy(&cur[i], rdram + (s_addrs[i] & PS2_RAM_MASK), 4);
            if (cur[i] != s_prev[i]) changed = true;
        }
        if (!changed) return;
        std::fprintf(stderr, "[memwatch] frame %llu |", (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed));
        for (size_t i = 0; i < s_addrs.size(); ++i)
            std::fprintf(stderr, " 0x%06x=%-6u%s", s_addrs[i], cur[i], cur[i] != s_prev[i] ? "*" : " ");
        std::fprintf(stderr, "\n");
        s_prev = cur;
    }

    static uint32_t rd32(const uint8_t *r, uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & PS2_RAM_MASK), 4); return v; }
    static void     wr32(uint8_t *r, uint32_t a, uint32_t v) { std::memcpy(r + (a & PS2_RAM_MASK), &v, 4); }

    // [netjump] func_356090 IS the versus menu: it loops internally and its RETURN VALUE decides
    // the transition --  0x352d88 jal func_356090 / beq $v0,$zero,stay / sw state=0x27.
    // So the seamless 0x26 -> 0x27 is simply "make it return non-zero", with the two side effects
    // the real function performs before returning (0x356234: stateObj+0x620/+0x624 = the duel
    // object's 0x110/0x114). No synthetic button presses, no walking menus.
    // Team Battle and DP Battle do NOT use character select 0x27. The duel dispatcher picks the
    // screen from the battle type on the way out of the versus menu (0x352da8..0x352db4):
    //     lw $a0, 0x624($v0)      ; battle type
    //     daddu $v1, $s5, $zero   ; $s5 = 0x28   (set at 0x352d28)
    //     movz $v1, $s3, $a0      ; $s3 = 0x27   (set at 0x352d1c) -- taken only when type == 0
    //     sw $v1, 0x18($v0)
    // So Single -> 0x27, Team and DP -> 0x28 (the multi-character roster screen). Everything below
    // used to compare against a literal 0x27, so for Team/DP the driver never recognised that it
    // had arrived: it sat in the pulse loop for its full 20 s timeout with the display frozen, and
    // the step-3 re-assert -- which is what holds 1P VS 2P against the screen's own entry code --
    // returned on its first line every frame. That is why DP came up as 1P vs COM.
    // The fight does NOT read duelObj+0x13c. That is only Battle Settings' working copy -- it
    // reads 0 until the menu is opened, while the game's default is 240 s, which is why writing it
    // alone changed nothing. Leaving Battle Settings commits it into a PER-SLOT table
    // (0x355c3c..0x355c58):
    //     base = [0x2ff28c] ; slot = duelObj->0x134 ; base[slot*4 + 0xc34] = duelObj->0x13c
    // Confirmed against four full-RAM dumps: the only word in 32 MB that held 0, then 4, then 2
    // across three time-limit settings was 0x6be254, and [0x2ff28c] + 0xc34 lands exactly there.
    // (The neighbouring 0x140 -> +0xc38 commit is a different Battle Settings option we do not
    // expose; leave it alone so the game's stored default survives.)
    static void bt3NetCommitTimeLimit(uint8_t *rdram, uint32_t duelObj,
                                      uint32_t (*rd)(const uint8_t *, uint32_t),
                                      void (*wr)(uint8_t *, uint32_t, uint32_t))
    {
        const uint32_t base = rd(rdram, 0x2ff28cu) & 0x1FFFFFFFu;
        if (!base || !duelObj) return;
        const uint32_t slot = rd(rdram, duelObj + 0x134u);
        if (slot >= 64u) return;          // it indexes a table: do not scribble on a wild value
        wr(rdram, base + slot * 4u + 0xc34u, (uint32_t)ps2NetTimeLimit());
    }

    static uint32_t bt3NetTargetState() { return ps2NetBattleType() == 0 ? 0x27u : 0x28u; }

    std::atomic<bool> g_netJumpWantConfirm{false};
    PS2Runtime::RecompiledFunction g_orig356090 = nullptr;
    void bt3VersusMenuGate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // func_356090
    {
        if (!g_netJumpWantConfirm.load(std::memory_order_relaxed))
        {
            if (g_orig356090) g_orig356090(rdram, ctx, runtime);
            return;
        }
        const uint32_t duelObj  = rd32(rdram, 0x3b38e8u) & 0x1FFFFFFFu;
        const uint32_t stateObj = rd32(rdram, 0x2ff10cu) & 0x1FFFFFFFu;
        if (!duelObj || !stateObj)
        {
            // The duel module has not allocated yet (or has been torn down). Confirming now would
            // commit NOTHING and still return non-zero, so the caller would transition on whatever
            // stale setup the state object still holds -- which is how a DP match came up with the
            // default 10 DP budget. Stay armed, run the real menu, and try again next frame.
            if (g_orig356090) g_orig356090(rdram, ctx, runtime);
            return;
        }
        g_netJumpWantConfirm.store(false, std::memory_order_relaxed);
        {
            wr32(rdram, duelObj + 0x110u, 1u);                                  // 1P VS 2P
            // 0x114 is the battle type: 0 Single, 1 Team, 2 DP. Taken from the netplay session
            // (the host's choice, which it stamps into every packet) so both machines build the
            // SAME match -- they each run this hook independently, so disagreeing here would set
            // up two different fights.
            wr32(rdram, duelObj + 0x114u, (uint32_t)ps2NetBattleType());
            // +0x118 is DP Battle's point budget (0 = 10 DP, 1 = 15, 2 = 20), the row of the
            // versus menu we never visit. Measured with [matchwatch]: picking 20 DP moved it to 2
            // and confirm committed it to stateObj+0x630, which is RetroAchievements' 0x6af7b0.
            // Leaving it at the default is why DP came up playing like Team -- the screen had a
            // DP type with no budget behind it.
            wr32(rdram, duelObj + 0x118u, (uint32_t)ps2NetDpLimit());
            bt3NetCommitTimeLimit(rdram, duelObj, &rd32, &wr32);
            // +0x13c is Battle Settings' working copy. Keep writing it so the menu agrees with the
            // table if it is ever displayed; the commit above is what the fight actually reads., found by dumping RAM at four settings and
            // keeping the only pointer-reachable value that tracked 3 -> 2 -> 1 -> 0 in order.
            wr32(rdram, duelObj + 0x13cu, (uint32_t)ps2NetTimeLimit());
            wr32(rdram, stateObj + 0x620u, rd32(rdram, duelObj + 0x110u));      // what 0x356234 does
            wr32(rdram, stateObj + 0x624u, rd32(rdram, duelObj + 0x114u));
            // The real commit copies THREE fields, not two (0x35622c..0x35625c). We were dropping
            // the last one. stateObj+0x630 is read by the duel module at 0x34b780 and 0x353f94,
            // so leaving it stale is a real difference -- mirror it exactly as the game does.
            wr32(rdram, stateObj + 0x630u, rd32(rdram, duelObj + 0x118u));
        }
        std::fprintf(stderr, "[netjump] versus-menu gate -> confirm (duelObj=0x%x)\n", duelObj);
        setReturnS32(ctx, 1);   // non-zero: the caller now performs its own 0x26 -> 0x27
    }

    // [netjump] On peer connect, take BOTH machines straight to character select instead of
    // making each player walk the menus.
    //
    // The top-level state IS the screen selector -- I originally mislabelled its values from the
    // [hstate] probe's guesswork. Measured by dumping RAM on known screens (PS2X_DUMPKEY):
    //     0x04 = main menu      0x26 = versus/duel menu      0x27 = CHARACTER SELECT
    // It lives at [[0x2ff10c] + 0x18] -- resolved through the pointer, so it survives whatever
    // the allocator does (RetroAchievements' fixed addresses do NOT transfer to this build: that
    // region is heap and our allocator places it differently than PCSX2).
    //
    // Writing the state is deliberately the ONLY thing done here. The per-player "is CPU"
    // flags found at 0x0f88208 / 0x0f883d0 are heap addresses with no pointer to resolve them
    // from, so hard-coding them would be exactly the mistake the RA addresses already were.
    // If the jump lands on character select but with a CPU opponent, that is the next thing to
    // chase -- through a pointer, not a literal.
    // Perform BT3's own "the player chose Duel" transition, rather than poking a state value.
    // Reverse-engineered from the main-menu overlay module at 0x3364f4..0x336534, which is a jump
    // table of menu rows (each loads its target state into $t1: 0x06, 0x0d, 0x21, 0x26=Duel) that
    // falls into this common tail:
    //     menuObj = [0x3b0e80]
    //     menuObj->0x108 |= 1 ;  menuObj->0x108 |= 2 ;  menuObj->0x110 = 0x0f
    //     stateObj->0x18 = <target state>          (stateObj = [0x2ff10c])
    //     func_10D878(menuObj + 0x10)              <-- the call that actually drives it
    // Writing the state alone does nothing from the main menu: the duel module is not being
    // ticked yet, so nothing reads the value. This call is what activates it.

    static bool bt3MenuGoto(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t targetState)
    {
        const uint32_t menuObj = rd32(rdram, 0x3b0e80u) & 0x1FFFFFFFu;
        const uint32_t stateObj = rd32(rdram, 0x2ff10cu) & 0x1FFFFFFFu;
        if (!menuObj || !stateObj) return false;
        wr32(rdram, menuObj + 0x108u, rd32(rdram, menuObj + 0x108u) | 1u);
        wr32(rdram, menuObj + 0x108u, rd32(rdram, menuObj + 0x108u) | 2u);
        wr32(rdram, menuObj + 0x110u, 0x0fu);
        wr32(rdram, stateObj + 0x18u, targetState);
        // run func_10D878(menuObj + 0x10) on a private context, ra = 0 so it returns to us
        R5900Context t = *ctx;
        t.r[4] = _mm_set_epi64x(0, (int64_t)(menuObj + 0x10u));
        t.r[31] = _mm_setzero_si128();
        t.pc = 0x0010d878u;
        uint32_t steps = 0u;
        while (t.pc != 0u && steps++ < 2000000u)
        {
            PS2Runtime::RecompiledFunction f = runtime->lookupFunction(t.pc);
            if (!f) break;
            f(rdram, &t, runtime);
        }
        std::fprintf(stderr, "[netjump] menuGoto(0x%02x): menuObj=0x%x stateObj=0x%x, func_10D878 ran %u steps\n",
                     targetState, menuObj, stateObj, steps);
        return true;
    }

    // [menujump] PS2X_MENU_JUMP=<state>: hold the host combo (keyboard P+L, or both mouse buttons held)
    // to "detonate" a screen transition by code instead of navigating. One-shot per press.
    // The env value is the target top-level state, decimal (4 = main menu, 38 = versus/duel,
    // 39 = character select, ...). Forcing it while the game is still booting bypasses the intro
    // FMV / title / splash logos: the state is written and the game's own transition (func_10D878
    // through bt3MenuGoto) is re-run as soon as the menu object exists.
    extern "C" bool IsKeyDown(int key);             // raylib; KEY_P == 80, KEY_L == 76
    extern "C" bool IsMouseButtonDown(int button);  // raylib; MOUSE_BUTTON_LEFT == 0, RIGHT == 1
    static void bt3MenuJumpFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const int s_target = [](){
            const char *v = std::getenv("PS2X_MENU_JUMP");
            return (v && v[0]) ? std::atoi(v) : 0;      // target top-level state (4 = main menu)
        }();
        static const bool s_auto = [](){
            const char *v = std::getenv("PS2X_MENU_AUTO");
            return v && v[0] && v[0] != '0';            // PS2X_MENU_AUTO=1: fire automatically
        }();
        if (s_target <= 0 || !rdram || !ctx || !runtime) return;

        const uint32_t stateObj = rd32(rdram, 0x2ff10cu) & 0x1FFFFFFFu;
        if (!stateObj) return;
        const uint32_t cur     = rd32(rdram, stateObj + 0x18u);
        const uint32_t menuObj = rd32(rdram, 0x3b0e80u) & 0x1FFFFFFFu;
        const uint64_t fr      = g_bt3FrameCount.load(std::memory_order_relaxed);
        // Boot markers: intro timer (*(*(0x3b0eb8)+0xc4) counts to 0x708) and the splash gate.
        uint32_t introT = 0u;
        if (const uint32_t ivp = rd32(rdram, 0x3b0eb8u) & 0x1FFFFFFFu)
            introT = rd32(rdram, (ivp + 0xc4u) & 0x1FFFFFFFu);

        // [menujump-diag] heartbeat: shows the boot flow (top-level state + whether the main-menu
        // object exists yet) so a long boot is visibly alive and we can see the exact moment the
        // menu comes up. Every 2 s.
        static uint64_t s_lastLog = ~0ull;
        if (fr != s_lastLog && (fr % 120u) == 0u)
        {
            s_lastLog = fr;
            std::fprintf(stderr, "[menujump] frame=%llu cur=0x%02x menuObj=0x%x introT=%u/1800 target=0x%02x\n",
                         (unsigned long long)fr, cur, menuObj, introT, s_target);
        }

        const char *why = nullptr;
        if (s_auto)
        {
            // The main-menu object [0x3b0e80] exists ONLY while the main menu is displayed: the
            // game frees it when leaving 0x04 (see the netjump notes below). So the primitive can
            // only fire from 0x04 with the object alive -- exactly the state PS2X_NET_JUMP uses.
            // Forcing a state before that (e.g. during boot) writes a value nobody reads and the
            // menu is never built (measured: 0x01 -> 0x04 flips the state, menuObj stays 0).
            static bool s_fired = false;
            if (!s_fired && cur == 0x04u && menuObj)
            {
                s_fired = true;
                std::fprintf(stderr, "[menujump] AUTO-VALIDATE: main menu up (menuObj=0x%x introT=%u fr=%llu) -> 0x%02x\n",
                             menuObj, introT, (unsigned long long)fr, s_target);
                why = "AUTO-VALIDATE(0x04)";
            }
        }
        else
        {
            const bool kb = IsKeyDown(80) && IsKeyDown(76);
            const bool ms = IsMouseButtonDown(0) && IsMouseButtonDown(1);
            static bool s_armed = false;
            if (!(kb || ms)) { s_armed = false; }
            else if (!s_armed) { s_armed = true; why = "COMBO"; }
        }
        if (!why) return;

        std::fprintf(stderr, "[menujump] %s: target=0x%02x cur=0x%02x menuObj=0x%x introT=%u fr=%llu\n",
                     why, s_target, cur, menuObj, introT, (unsigned long long)fr);
        if (!menuObj)
        {   // no menu object: nothing to drive (forcing the state would be a no-op)
            std::fprintf(stderr, "[menujump] no menuObj (cur=0x%02x) -- primitive needs the menu up\n", cur);
            return;
        }
        bt3MenuGoto(rdram, ctx, runtime, (uint32_t)s_target);
    }

    // [statelog] One line per screen change, with everything that is cheap to read and hard to
    // guess. The point is not the pretty name -- it is that each line carries the fields that
    // DIFFER between screens, so a pasted log answers "which of these is character select" without
    // anybody having to remember which offset means what.
    //
    //   duelObj   [0x3b38e8]  the duel module's object. Non-null only while the duel module is up,
    //                          which is DUEL_MENU and the states it owns. Measured non-null -> null
    //                          across 0x26 -> 0x27, so it separates "in the duel module" from "past
    //                          it" -- which is the question character select turns on.
    //   mode/type/dp  stateObj+0x620 / +0x624 / +0x630. The match setup. 0x620 is committed by
    //                          the versus menu's confirm gate at 0x35622c; 0x624 is the one
    //                          character select READS, at 0x352da8.
    //   plates     [0x3b0e80]+0x144, the main menu's own build counter. Non-zero only on 0x04, so
    //                          it is how a main-menu screen tells itself apart from everything else.
    //   tlimit     stateObj+0x62c, next to the others, in case the time limit has a life of its own.
    static void bt3StateLogFrame(uint8_t *rdram)
    {
        static uint32_t s_prev = ~0u;
        const uint32_t st = bt3CurState(rdram);
        if (st == s_prev)
            return;
        const uint32_t from = s_prev;
        s_prev = st;

        const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
        const uint32_t so     = rd32(rdram, 0x2ff10cu) & 0x1FFFFFFFu;
        const uint32_t duel   = rd32(rdram, 0x3b38e8u) & 0x1FFFFFFFu;
        const uint32_t menu   = rd32(rdram, 0x3b0e80u) & 0x1FFFFFFFu;
        const uint32_t plates = menu ? rd32(rdram, menu + 0x144u) : 0u;

        std::fprintf(stderr,
                     "[statelog] fr=%llu  0x%02x %-17s -> 0x%02x %-17s | stateObj=0x%x duelObj=0x%x"
                     " menuObj=0x%x plates=%u | mode=%u type=%u dp=%u tlimit=%u\n",
                     (unsigned long long)fr,
                     from == ~0u ? 0u : from, from == ~0u ? "(start)" : bt3StateName(from),
                     st, bt3StateName(st),
                     so, duel, menu, plates,
                     so ? rd32(rdram, so + 0x620u) : 0u,
                     so ? rd32(rdram, so + 0x624u) : 0u,
                     so ? rd32(rdram, so + 0x630u) : 0u,
                     so ? rd32(rdram, so + 0x62cu) : 0u);
    }

    static void bt3NetJumpCharSelect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // PS2X_NET_JUMP=1  go via the versus menu (0x04 -> 0x26 -> 0x27), the path the game
        //                   itself takes, so whatever the versus menu establishes gets set.
        // PS2X_NET_JUMP=2  jump straight to character select (0x04 -> 0x27). The main menu's
        //                   jump table uses ONE common tail for every row and only varies the
        //                   target state, so func_10D878 is a generic "go to screen N" -- the
        //                   duel module handles 0x26..0x29, so 0x27 should activate it directly.
        static const int s_env = [](){ const char *v = std::getenv("PS2X_NET_JUMP");
                                       return (v && v[0]) ? std::atoi(v) : 0; }();
        // The overlay's "jump on connect" checkbox drives this now; PS2X_NET_JUMP stays as an
        // override for headless runs. Mode 2 (straight to 0x27) is env-only -- it cannot set the
        // versus mode, because the duel object that holds it is freed before character select.
        const int s_mode = s_env > 0 ? s_env : (ps2NetAutoJump() ? 1 : 0);
        // The whole feature is behind NET_OVERLAY, transition included. The curtain lives in the
        // overlay, so a jump without it would drive the menus while the player watches an unexplained
        // sequence of screens -- the transition is not useful on its own, it is useful next to the
        // thing that says what is happening.
        if (!ps2xNetOverlayEnabled())
            return;
        const bool live = ps2NetActive() && ps2NetPeerConnected();
        // [netjump] `live` is NOT an entry gate any more, and that is the fix for the curtain that
        // never came down. It used to be one, and every path that CLEARS the curtain sits below
        // here -- so the moment the session died (Disconnect, the peer leaving, the fake switched
        // off) this function returned on its first line and nothing could ever clear it again. A
        // black screen until the process restarted.
        //
        // It is a condition for ADVANCING instead, which is what it always meant: no session, no
        // new jumps. The return trip and the curtain teardown keep running without one, because
        // they are how a dead session gets cleaned up.
        if (s_mode <= 0 || !rdram) return;
        // Reset per connection, so disconnecting and reconnecting jumps again instead of
        // remembering that it already ran once this process.
        static uint32_t s_session = 0;
        static int s_step = 0; static uint64_t s_waitUntil = 0;
        // [netjump] "the duel module has been up at least once since the curtain went up". Needed
        // because duelObj == 0 is ALSO true on the main menu, so on its own it cannot tell "we
        // arrived at character select" from "we never left". Three states, not two: not started
        // (never seen it), transitioning (seen it, still there), arrived (seen it, now gone).
        static bool s_sawDuel = false;
        // [netjump] The desync frame this transition STARTED with, so "a desync happened" can mean
        // "a desync happened NOW". ps2NetDesyncFrame() latches the first mismatch and never clears
        // it -- not even on a new connect -- so a bare `!= 0` would abort on a desync from the
        // previous session, minutes or hours later. Comparing against this baseline is the only
        // version of the test that means what it says.
        static uint32_t s_desyncBase = 0;
        // [netjump] When the return trip started, for its watchdog. 0 = not returning.
        static uint64_t s_returnStart = 0;
        // [netjump] So the "heading for 0x04" line prints once per trip, not once per process.
        static bool s_saidEntry = false;
        // s_pulseStart lives HERE, not inside step 2, because a static in there survives the
        // connection: on a second connect it still held the first one's frame, so the 600-frame
        // timeout had already expired and step 2 gave up on its very first tick. Every piece of
        // this state machine has to be reset per session, the armed gate flag included -- a gate
        // left armed from a failed attempt fires on the NEXT connect before the duel module is up.
        static uint64_t s_pulseStart = 0;
        if (s_session != ps2NetSession())
        { s_session = ps2NetSession(); s_step = 0; s_waitUntil = 0; s_pulseStart = 0; s_sawDuel = false;
          s_desyncBase = ps2NetDesyncFrame();
          s_returnStart = 0;
          s_saidEntry = false;
          g_netJumpState.store(1, std::memory_order_relaxed);
          g_netJumpSession.store(s_session, std::memory_order_relaxed);
          g_netJumpWantConfirm.store(false, std::memory_order_relaxed);
          g_netJumpHold.store(0, std::memory_order_relaxed);
          // The curtain and the return flag are per-connection state too. Left at 1 from a failed
          // attempt they would cover the main menu forever: the forward machine only ever clears
          // them on the paths that RUN, and a reset skips all of them.
          g_netCurtainWant.store(0, std::memory_order_relaxed);
          // The pad too. The gate outlives the session by design (it is a pad state, not a netplay
          // one), so a reset that forgot it would leave the player on a dead controller with nothing
          // on screen to explain it.
          ps2xNetMenuGate(0, 0);
          g_netJumpCancel.store(0, std::memory_order_relaxed);
          g_netJumpPressCross.store(0, std::memory_order_relaxed); }
        // == 3, NOT >= 3. Step 4 is the reverse trip and it lives BELOW this block, so a ">=" here
        // swallowed it: the give-up set s_step = 4, the next tick fell into the re-assert arm and
        // returned, and the trip back to the main menu was code that could never run. The log showed
        // the give-up firing and then nothing at all, which is what dead code looks like from outside.
        if (s_step == 3)
        {
            // [netjump] Only while there is a session to set it up for. Without this the re-assert
            // kept writing stateObj+0x620 into a local game forever, long after Disconnect.
            if (!live) return;
            // HOLD the match setup. Writing it once is not enough: the mode is normally committed
            // inside func_356090 (the confirm gate) at 0x35622c, 25 frames BEFORE the 0x26 -> 0x27
            // transition. Jumping straight to character select skips that, and the screen's own
            // entry code then puts the default (1P vs CPU) back. So re-assert it every frame while
            // character select is up, and stop as soon as the screen changes.
            const uint32_t so = rd32(rdram, 0x2ff10cu) & 0x1FFFFFFFu;
            if (!so) return;
            if (rd32(rdram, so + 0x18u) != bt3NetTargetState()) return;   // left the screen: done
            // Check ALL THREE, not just the mode. The guard used to be `if (mode != 1)`, so once the
            // versus menu had committed 1P VS 2P the whole block was skipped -- and if the screen's
            // own entry code then put the DEFAULT battle type and DP budget back, nothing noticed
            // and nothing repaired them. That is invisible for the defaults (Single is type 0 and
            // 10 DP is dp 0, so a reset lands on the right value by accident) and only shows up
            // when a NON-default is chosen, which is exactly the DP 15/20 case.
            const uint32_t wantType = (uint32_t)ps2NetBattleType();
            const uint32_t wantDp   = (uint32_t)ps2NetDpLimit();
            const uint32_t haveMode = rd32(rdram, so + 0x620u);
            const uint32_t haveType = rd32(rdram, so + 0x624u);
            const uint32_t haveDp   = rd32(rdram, so + 0x630u);
            if (haveMode != 1u || haveType != wantType || haveDp != wantDp)
            {
                wr32(rdram, so + 0x620u, 1u);                  // 1P VS 2P
                wr32(rdram, so + 0x624u, wantType);
                wr32(rdram, so + 0x630u, wantDp);
                static std::atomic<uint32_t> s_n{0};
                if (s_n.fetch_add(1u) < 8u)
                    std::fprintf(stderr, "[netjump] re-asserted: mode %u->1 type %u->%u dp %u->%u\n",
                                 haveMode, haveType, wantType, haveDp, wantDp);
            }
            return;
        }
        const uint64_t now = g_bt3FrameCount.load(std::memory_order_relaxed);
        if (now < s_waitUntil) return;
        const uint32_t stateObj = rd32(rdram, 0x2ff10cu) & 0x1FFFFFFFu;
        if (!stateObj) return;
        const uint32_t cur = rd32(rdram, stateObj + 0x18u);
        if (s_step == 0)
        {
            // [netjump] Needs a session, like step 1. Removing `live` as an entry gate is what let a
            // DEAD session start a transition: unchecking the fake box drops ps2NetSession() back to
            // 0, the per-session reset fires and puts s_step back to 0, and step 0 -- the only step
            // that had no live check -- went ahead and navigated to 0x26 with nobody connected. It
            // raised the curtain, armed the pad gate, and had no way out, because F10 only exists in
            // step 2. That is the "giving it test again does nothing": the second attempt was
            // waiting on a main menu that the first one had already navigated away from.
            if (!live) return;
            if (cur == bt3NetTargetState()) { s_step = 3; g_netJumpState.store(2, std::memory_order_relaxed); std::fprintf(stderr, "[netjump] already at character select\n"); return; }
            if (cur != 0x04u) return;                       // wait until the main menu is up
            if (s_mode >= 2)
            {   // straight to character select
                if (!bt3MenuGoto(rdram, ctx, runtime, bt3NetTargetState())) return;
                std::fprintf(stderr, "[netjump] 0x04 -> 0x%02x direct (character select)\n", bt3NetTargetState());
                s_step = 2; s_waitUntil = now + 90u; return;
            }
            if (!bt3MenuGoto(rdram, ctx, runtime, 0x26u)) return;
            g_netJumpHold.store(600, std::memory_order_relaxed);   // hide the menus (~20 s cap)
            g_netCurtainWant.store(1, std::memory_order_relaxed);  // [netjump] black + "Loading..."
            // [netjump] The transition owns the pad from here to the curtain coming down. The gate
            // is the project's own selective freeze, kept since the custom page was retired, and
            // the allow mask is CIRCLE alone -- so every button and both sticks are released and
            // the one key the player is allowed is the one that aborts. Reusing it matters: a
            // second freeze beside it is a second thing to keep in agreement with the seam.
            // CIRCLE is bit 13 of the active-low pad word.
            ps2xNetMenuGate(1, 1u << 13);
            s_sawDuel = false;
            s_step = 1; return;   // no fixed wait: step 1 polls for the module itself
        }
        if (s_step == 1)
        {
            if (!live) return;          // no session: stop advancing. The curtain and the gate come
                                        // down on the session reset or the give-up, not here.
            if (cur != 0x26u) return;   // still switching modules: poll, do not give up
            // Choose 1P VS 2P *here*, on the versus menu, because this is the only place it can
            // be chosen: the duel module's object [0x3b38e8] holds the real setting and is FREED
            // by the time character select is up.
            //     duelObj->0x110  versus mode (0 = 1P vs CPU, 1 = 1P vs 2P, 2 = CPU vs CPU)
            //     duelObj->0x114  battle type (0 = Single Battle)
            // It tracks the menu cursor live, and func_356090 later COPIES it to stateObj+0x620
            // (see 0x356234..0x356238). Writing stateObj+0x620 directly, which is what the last
            // three attempts did, only edits that copy -- the screen goes on reading the source.
            // This is also why a direct 0x04 -> 0x27 jump can never set the mode.
            const uint32_t duelObj = rd32(rdram, 0x3b38e8u) & 0x1FFFFFFFu;
            if (!duelObj) return;                       // module still coming up: wait
            wr32(rdram, duelObj + 0x110u, 1u);          // 1P VS 2P
            // Take the battle type from the netplay session (the host stamps its choice into
            // every packet) instead of forcing Single -- this write is what the versus menu would
            // have made had the player navigated it, and the gate below reads it back out.
            wr32(rdram, duelObj + 0x114u, (uint32_t)ps2NetBattleType());
            wr32(rdram, duelObj + 0x118u, (uint32_t)ps2NetDpLimit());
            wr32(rdram, duelObj + 0x13cu, (uint32_t)ps2NetTimeLimit());
            bt3NetCommitTimeLimit(rdram, duelObj, &rd32, &wr32);
            std::fprintf(stderr, "[netjump] duelObj=0x%x: mode -> 1 (1P VS 2P), type -> %u, dp -> %u\n",
                         duelObj, (unsigned)ps2NetBattleType(), (unsigned)ps2NetDpLimit());
            // Advance with a PLAIN WRITE, not bt3MenuGoto: that helper needs the MAIN-MENU object
            // [0x3b0e80], which is freed the moment we leave the main menu, so it returned false
            // every frame here and the sequence span forever re-writing the mode.
            // A plain write is enough now for the reason it was not before: the duel module is
            // genuinely ACTIVE (menuGoto(0x26) loaded it), and its dispatcher at 0x352d30
            // branches on this very slot -- 0x27 goes to the character-select handler at 0x352dd8.
            // Do NOT write the state here: the module must run its own 0x26 -> 0x27 path so
            // func_356090 loads the screen. Press confirm and let the game do it.
            g_netJumpWantConfirm.store(true, std::memory_order_relaxed);
            std::fprintf(stderr, "[netjump] arming the versus-menu gate on frame %llu\n", (unsigned long long)now);
            s_step = 2; return;
        }
        if (s_step == 2)
        {
            // [netjump] ARRIVAL, measured by the duel module and NOT by the state.
            //
            // duelObj ([0x3b38e8]) is non-null exactly while the versus menu is up and goes null the
            // moment the game leaves it: 0xc4a340 across 0x26, 0 on 0x27 and on 0x28, for the whole
            // run. The game writes it; we never do.
            //
            // The state cannot answer this question, because WE wrote it on the way in with
            // bt3MenuGoto(0x26) -- reading it back only confirms our own write, which is precisely
            // why the first version could sit at 0x26 for 600 frames with a gate armed and a
            // curtain up and never notice anything was wrong.
            const uint32_t duel = rd32(rdram, 0x3b38e8u) & 0x1FFFFFFFu;
            if (duel) s_sawDuel = true;
            if (duel || !s_sawDuel)
            {
                // Still in the versus menu, or never got there. Keep the picture held: the count
                // re-arms every frame, and the 600 below is the backstop for when this state
                // machine stops running at all.
                g_netJumpHold.store(600, std::memory_order_relaxed);
                // PULSE the confirm: there are TWO menus to get through (versus mode, then battle
                // type), and a held button is ONE press -- a second menu needs a release in between.
                // 3 frames down, 5 up. The previous version pressed once and waited 240 frames
                // before retrying, which is the 8-second "slow clicking" -- that was my retry timer,
                // not the game being slow.
                if (!s_pulseStart) s_pulseStart = now;
                // [netjump] Three ways out of here, all running the SAME teardown: the timeout, the
                // test hook, and the player's circle. A cancel is not a fourth path, it is this one
                // with a different reason -- which is why the curtain comes down the same way and
                // there is only one reverse trip to get right.
                // [netjump] the forced failure lands here, so it runs the SAME branch as the real
                // timeout below rather than a copy of it -- a copy is a second thing to keep right.
                const bool cancelled = g_netJumpCancel.exchange(0, std::memory_order_relaxed) != 0;
                // [netjump] Losing the session is a fourth way out, and it has to be HERE. Steps 0
                // and 1 now refuse to advance without one, so a session that dies mid-transition
                // reaches this step with the curtain up and no way to lower it -- unless this is
                // also a give-up. With it, every way of losing a session funnels into the same
                // teardown and the same reverse trip, which is the only reason the curtain can be
                // trusted to come down.
                const bool lost = !live;
                // [netjump] A desync during the transition is a give-up, same as a cancel. The two
                // sides are supposed to be walking the same menus from the same frame; once the
                // confirmed-state hashes differ, whatever is on screen is no longer a shared
                // screen and letting the transition keep driving it is how you end up with two
                // machines in different places and a curtain that never lifts.
                //
                // Only while the curtain is up, which is exactly this step -- that is where
                // g_netCurtainWant is 1. Outside a transition a desync is a different problem with
                // a different fix, and pretending otherwise here would abort a match that is
                // playing perfectly well apart from one bad checksum.
                const uint32_t desyncNow = ps2NetDesyncFrame();
                const bool desync = desyncNow != 0u && desyncNow != s_desyncBase;
                if (cancelled || lost || desync ||
                    now - s_pulseStart > 600u)     // ~20 s: something is wrong, stop hiding it
                {
                    // One reason, one line. The desync case carries its frame because the frame
                    // number is the only thing that says WHICH desync, and there is one per session.
                    char why[64];
                    if (desync)
                        std::snprintf(why, sizeof why, "desync at frame %u", desyncNow);
                    else if (cancelled)
                        std::snprintf(why, sizeof why, "cancelled by the player");
                    else if (lost)
                        std::snprintf(why, sizeof why, "session lost");
                    else
                        std::snprintf(why, sizeof why, "no progress");
                    std::fprintf(stderr, "[netjump] giving up (%s) at state 0x%02x, duelObj=0x%x\n",
                                 why, cur, duel);
                    // Same thing in reverse, back to the main menu. The curtain comes down so the
                    // player watches the way out instead of being cut to the menu, and the hold is
                    // re-armed for the trip so the versus menu is not seen on the way either.
                    // s_step = 4 keeps this machine out of the way while it runs; a new connect
                    // bumps the session and resets it (see the reset above).
                    // The curtain STAYS UP. The transition is one journey with two ends -- character
                    // select, or the main menu -- and the curtain covers both; it comes down when
                    // the game arrives at one of them, not when the machine decides to stop trying.
                    // Dropping it here would cut from black straight to a versus menu the player was
                    // just told they had left, which is the worst of both.
                    //
                    // So step 4 pulses triangle until 0x04, and the ONLY thing that can end this
                    // short of arriving is the watchdog below.
                    g_netJumpHold.store(600, std::memory_order_relaxed);
                    s_returnStart = now;
                    s_saidEntry = false;   // so the next trip announces itself too
                    s_step = 4;
                    g_netJumpState.store(3, std::memory_order_relaxed);   // 3 = returning: "Aborting..."
                    return;
                }
                if (duel && ((now - s_pulseStart) % 8u) == 0u)
                {
                    // The project's own synthetic press, not g_netJumpPressCross: the retired page's
                    // seam applies OUTSIDE the netplay block, so it works with no session at all --
                    // which is what makes a faked test possible. CROSS is bit 14, active low.
                    ps2xNetMenuPress(1 << 14, 3);
                    g_netJumpPressCross.store(3, std::memory_order_relaxed);
                }
                return;
            }
            // ARRIVED. duelObj is gone, so the game has left the versus menu for character select
            // (0x27 for Single) or the pre-fight setup (0x28 for Team and DP).
            std::fprintf(stderr, "[netjump] arrived: duelObj released, state=0x%02x (%s)\n",
                         cur, cur == 0x27u ? "character select" : "pre-fight setup");
            // Set the match up as 1P VS 2P, Single Battle.
            // These live INSIDE the state object, so they are reached through the pointer at
            // 0x2ff10c like the state itself -- no heap literal:
            //     stateObj + 0x620  versus mode  (0 = 1P vs CPU, 1 = 1P vs 2P, 2 = CPU vs CPU)
            //     stateObj + 0x624  battle type  (0 = Single Battle, 1 = Team Battle)
            // The duel module reads +0x624 at 0x352da8 on the way to character select.
            // Found via the RetroAchievements map: every RA address is OURS MINUS 0x4000 (their
            // 0x6af198 screen id is our 0x6b3198). They did not transfer directly because that
            // 16 KB shift makes each one land in unrelated data -- which is what made the earlier
            // 0x6af7a0 reading look like a table of positions and score thresholds.
            // The mode only COMMITS on confirm, which is why diffing dumps taken with the rows
            // merely highlighted showed no difference and sent me after the per-player "is CPU"
            // heap flags instead.
            wr32(rdram, stateObj + 0x620u, 1u);   // 1P VS 2P
            wr32(rdram, stateObj + 0x624u, (uint32_t)ps2NetBattleType());
            wr32(rdram, stateObj + 0x630u, (uint32_t)ps2NetDpLimit());
            g_netJumpHold.store(0, std::memory_order_relaxed);   // character select is up: show it
            g_netCurtainWant.store(0, std::memory_order_relaxed);  // [netjump] and take the curtain down
            // The pad goes back with the curtain. Not before: a black screen with the pad already
            // live lets the player drive menus they cannot see, which is worse than the two frames
            // of the other way round.
            ps2xNetMenuGate(0, 0);
            g_netJumpCancel.store(0, std::memory_order_relaxed);
            static const char *kType[] = { "Single", "Team", "DP" };
            const unsigned bt = (unsigned)ps2NetBattleType();
            static const char *kDp[] = { "10 DP", "15 DP", "20 DP" };
            const unsigned dp = (unsigned)ps2NetDpLimit();
            std::fprintf(stderr, "[netjump] settled at state 0x%02x | mode=%u type=%u dp=%u (1P VS 2P, %s Battle%s%s)\n",
                         cur, rd32(rdram, stateObj + 0x620u), rd32(rdram, stateObj + 0x624u),
                         rd32(rdram, stateObj + 0x630u), bt < 3 ? kType[bt] : "?",
                         bt == 2 ? ", " : "", (bt == 2 && dp < 3) ? kDp[dp] : "");
            s_step = 3; g_netJumpState.store(2, std::memory_order_relaxed);
        }

    if (s_step == 4)
    {
        // [netjump] Back to the main menu, because a transition that gave up leaves the game sitting
        // in the versus menu with nobody pressing anything, and that is worse to look at than being
        // where you started. The curtain is already on its way down, so this runs under black and
        // the player never sees the versus menu on the way out.
        // [netjump] Step 4 was completely silent, which is why a failing return trip looked
        // identical to a working one: the log just stopped after the give-up. Every branch below
        // now says something, including a heartbeat, so "it is still trying" and "it is not
        // running" are different things in the log instead of the same absence.
        {
            if (!s_saidEntry)
            {
                s_saidEntry = true;
                std::fprintf(stderr, "[netjump] return trip: heading for 0x04, curtain stays up "
                                     "(circle again to take the game back now)\n");
            }
        }
        // [netjump] Nothing gates entry here any more. There used to be a one-shot
        // g_netJumpReturn flag, consumed here with exchange(0), and it was the whole reason the
        // return trip did nothing: the first tick took the 1 and carried on, the SECOND tick found
        // 0 and did `s_step = 3; return`. One triangle pulse, then silence. The watchdog lives in
        // this same block, so it never ran either -- the curtain went up and nothing was ever going
        // to bring it down. `s_step == 4` already IS the state; a second signal saying the same
        // thing, with the wrong lifetime, was the bug.
        // [netjump] The second CIRCLE is the escape hatch. The first one asked for the trip back and
        // the curtain is deliberately still up, waiting for the game to reach 0x04 -- so if the duel
        // module will not release, the player is looking at a black screen that only a watchdog can
        // end, twenty seconds later. They should not have to wait for a watchdog: pressing the
        // cancel button again says "stop, give me my game back", and that is the one thing this
        // whole arrangement owes them.
        //
        // Everything comes back at once, in the order that matters -- pad, then picture, then the
        // curtain, which is also what carries the audio. No navigation, no attempt to reach 0x04:
        // the player ends up wherever the game actually is, which is a real screen.
        if (g_netJumpCancel.exchange(0, std::memory_order_relaxed) > 0)
        {
            std::fprintf(stderr, "[netjump] second circle at state 0x%02x -- handing the game back "
                                 "where it is\n", cur);
            ps2xNetMenuGate(0, 0);
            g_netJumpHold.store(0, std::memory_order_relaxed);
            g_netCurtainWant.store(0, std::memory_order_relaxed);
            s_returnStart = 0;
            s_step = 3;
            g_netJumpState.store(0, std::memory_order_relaxed);
            return;
        }
        // [netjump] The watchdog. The curtain is deliberately held until the game reaches 0x04, and
        // step 4 is driving the menus to get it there -- so if the duel module will not release, the
        // curtain would never come down on its own. That is the failure this whole arrangement
        // risks, and it is the one that left a black screen and a dead controller before, so it gets
        // its own exit: after 20 s of trying, hand everything back and stop pretending. The player
        // ends up wherever the game is, which is a real screen, instead of on black.
        if (s_returnStart && now - s_returnStart > 600u)
        {
            std::fprintf(stderr, "[netjump] the return trip timed out at state 0x%02x -- "
                                 "releasing the player where the game is\n", cur);
            g_netJumpHold.store(0, std::memory_order_relaxed);
            g_netCurtainWant.store(0, std::memory_order_relaxed);
            ps2xNetMenuGate(0, 0);
            g_netJumpCancel.store(0, std::memory_order_relaxed);
            s_returnStart = 0;
            s_step = 3;
            g_netJumpState.store(0, std::memory_order_relaxed);
            return;
        }
        if (rd32(rdram, stateObj + 0x18u) == 0x04u)
        {
            g_netJumpHold.store(0, std::memory_order_relaxed);   // arrived: show the main menu
            g_netCurtainWant.store(0, std::memory_order_relaxed);
            ps2xNetMenuGate(0, 0);        // the pad comes back with the curtain, same as the
            g_netJumpCancel.store(0, std::memory_order_relaxed);   // forward trip
            s_step = 3;
            g_netJumpState.store(0, std::memory_order_relaxed);
            std::fprintf(stderr, "[netjump] back at the main menu -- curtain down, pad and audio back\n");
            return;
        }
        if (now < s_waitUntil) return;
        // The duel module owns the screen until it lets go, so confirm first and only then ask for
        // the main menu -- the same order the forward path uses, in reverse. bt3MenuGoto needs the
        // MAIN-MENU object, which only exists again once we are back, hence the second step below.
        if (rd32(rdram, stateObj + 0x18u) == 0x26u)
        {
            // [netjump] TRIANGLE, and this is the bug the user's own test found: pressing cross here
            // pushed the screen the WRONG way, forward into character select instead of back to the
            // main menu. Walking 0x26 -> 0x04 by hand takes triangle. The two are separate bits --
            // cross 14, triangle 12 -- so they need separate masks.
            ps2xNetMenuPress(1 << 12, 3);
            {   // [netjump] Every 30th pulse, so the log shows the trip is alive without becoming
                // the log. A silent retry loop is indistinguishable from a dead one.
                static uint32_t s_pulses = 0;
                if ((++s_pulses % 30u) == 1u)
                    std::fprintf(stderr, "[netjump] return trip: triangle pulse #%u at state 0x%02x\n",
                                 s_pulses, cur);
            }
            s_waitUntil = now + 24u;
            return;
        }
        if (!bt3MenuGoto(rdram, ctx, runtime, 0x04u)) { s_waitUntil = now + 30u; return; }
        std::fprintf(stderr, "[netjump] 0x%02x -> 0x04 (back to the main menu)\n",
                     rd32(rdram, stateObj + 0x18u));
        s_waitUntil = now + 90u;
        return;
    }
    }

    // [dumpkey] PS2X_DUMPKEY=<prefix>: press F9 to write EE RAM to "<prefix>.<n>.bin".
    // Needed to find the RetroAchievements addresses that are NOT the state object. The state
    // object's are already translated: the achievements were written against PCSX2, where it sat at
    // 0x6af180, and in this build it is at 0x6b3180 -- a flat +0x4000, confirmed from two
    // directions because 0x006af7a0/0x006af7a4 land on stateObj+0x620/+0x624, which is where mode
    // and battle type were already known to live. See scripts/gen_ach_patch.py, whose VERIFIED list
    // is where that translation is applied and where the rest of the set is held back.
    //
    // The remaining regions are still HEAP (the ELF's loaded segments end at 0x334bf8) and still
    // land at different addresses than under PCSX2, and our layout IS stable run to run, so the
    // equivalents can be found by dumping at KNOWN screens and diffing -- which means letting the
    // player mark the moment. PS2X_ACH_TRACE=1 logs every address the tracker reads, which is the
    // other half of the same job.
    extern "C" bool IsKeyPressed(int key);   // raylib; KEY_F9 == 298
    static void bt3DumpKey(uint8_t *rdram)
    {
        static const char *s_prefix = std::getenv("PS2X_DUMPKEY");
        if (!s_prefix || !s_prefix[0] || !rdram) return;
        static bool s_said = false;
        if (!s_said) { s_said = true; std::fprintf(stderr, "[dumpkey] press F9 to dump EE RAM to %s.<n>.bin\n", s_prefix); }
        if (!IsKeyPressed(298)) return;
        static int s_n = 0;
        char path[512]; std::snprintf(path, sizeof path, "%s.%d.bin", s_prefix, s_n);
        if (std::FILE *o = std::fopen(path, "wb"))
        {
            std::fwrite(rdram, 1, PS2_RAM_SIZE, o); std::fclose(o);
            std::fprintf(stderr, "[dumpkey] dump #%d at frame %llu -> %s\n", s_n, 
                         (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), path);
            ++s_n;
        }
    }

    // [memblock] PS2X_MEMBLOCK=<hex addr>:<word count> -- print the block whenever any word in it
    // changes, marking the changed ones with '*'. Used to find the menu SCREEN selector now that
    // the RetroAchievements mode variables are confirmed for this build:
    //     0x6af7a0 versus mode (0 = 1vCPU, 1 = 1v2, 2 = CPUvCPU)   CONFIRMED
    //     0x6af7a4 battle type (0 = Single, 1 = Team)              CONFIRMED
    //     0x6af1ac "current mode"                                  WRONG for this build
    // The screen selector is most likely a neighbour of the two that are right.
    static void bt3MemBlock(uint8_t *rdram)
    {
        static uint32_t s_base = 0u; static int s_words = 0; static bool s_init = false;
        if (!s_init)
        {
            s_init = true;
            if (const char *e = std::getenv("PS2X_MEMBLOCK"))
            {
                char *end = nullptr;
                s_base = (uint32_t)std::strtoul(e, &end, 16) & 0x1FFFFFFFu;
                s_words = (end && *end == ':') ? std::atoi(end + 1) : 32;
                if (s_words < 1 || s_words > 256) s_words = 32;
                std::fprintf(stderr, "[memblock] watching 0x%06x for %d words\n", s_base, s_words);
            }
        }
        if (!s_words || !rdram) return;
        static std::vector<uint32_t> prev;
        std::vector<uint32_t> cur((size_t)s_words);
        std::memcpy(cur.data(), rdram + (s_base & PS2_RAM_MASK), (size_t)s_words * 4u);
        if (prev.size() == cur.size() && std::memcmp(prev.data(), cur.data(), cur.size() * 4u) == 0) return;
        std::fprintf(stderr, "[memblock] frame %llu\n", (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed));
        for (int i = 0; i < s_words; i += 8)
        {
            std::fprintf(stderr, "   0x%06x ", s_base + (uint32_t)i * 4u);
            for (int k = i; k < i + 8 && k < s_words; ++k)
                std::fprintf(stderr, " %c%08x", (prev.size() == cur.size() && cur[k] != prev[k]) ? '*' : ' ', cur[k]);
            std::fprintf(stderr, "\n");
        }
        prev = cur;
    }

    // [statewatch] PS2X_STATEWATCH=1: log every change of BT3's top-level state machine.
    // The state lives at [[0x2ff10c] + 0x18] -- a pointer to the state object, state value at
    // +0x18 (the same slot the status probe reads as "bt3state"). Known values so far:
    //   0x01 BOOT   0x04 MENU   0x26 PREFIGHT_SETUP   0x27 FIGHT-LOAD   0x2d IN-FIGHT
    // Walking the menus with this on names the CHARACTER-SELECT state, which is what a direct
    // "jump both players to character select" needs instead of replaying canned button presses.
    // Also logs the guest pc/ra at the moment of the change, to point at the code that sets it.
    // [matchwatch] PS2X_MATCHWATCH=1 -- print the whole match-setup tuple whenever any part of it
    // changes. Team Battle and DP Battle share character-select screen 0x28, and stateObj+0x624
    // (the battle type) has exactly ONE writer in the overlay -- 0x356248, inside the confirm
    // function we replace -- so setting it to 2 cannot be what is missing when DP comes up playing
    // like Team. The remaining candidate is the third field the real confirm commits,
    // duelObj+0x118 -> stateObj+0x630, which the versus menu would have filled in from a sub-row we
    // never visit. Rather than guess its value: walk to DP Battle by hand once with this on, and
    // the line printed on confirm IS the answer.
    //     stateObj = [0x2ff10c]   +0x18 screen  +0x620 mode  +0x624 type  +0x628 ?  +0x630 ?
    //     duelObj  = [0x3b38e8]   +0x110 mode   +0x114 type  +0x118 ?     +0x13c time limit
    //     cfgObj   = [0x3b38d8]   +0x3c38/+0x3c3c/+0x3c40  <- where char-select copies the trio
    //                                                          (0x34b760..0x34b784)
    static void bt3MatchWatch(uint8_t *rdram)
    {
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_MATCHWATCH");
                                       return v && v[0] && v[0] != '0'; }();
        if (!s_on || !rdram) return;
        auto ld = [&](uint32_t a) -> uint32_t { uint32_t v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const uint32_t so = ld(0x2ff10cu) & 0x1FFFFFFFu;
        const uint32_t du = ld(0x3b38e8u) & 0x1FFFFFFFu;
        const uint32_t cf = ld(0x3b38d8u) & 0x1FFFFFFFu;
        uint32_t cur[14] = {0};
        if (so) { cur[0] = ld(so + 0x18u);  cur[1] = ld(so + 0x620u); cur[2] = ld(so + 0x624u);
                  cur[3] = ld(so + 0x628u); cur[4] = ld(so + 0x630u); }
        if (du) { cur[5] = ld(du + 0x110u); cur[6] = ld(du + 0x114u); cur[7] = ld(du + 0x118u);
                  cur[8] = ld(du + 0x13cu); }
        if (cf) { cur[9] = ld(cf + 0x3c38u); cur[10] = ld(cf + 0x3c3cu); cur[11] = ld(cf + 0x3c40u); }
        // The COMMITTED time limit, reached the way the game reaches it (0x355c3c..0x355c58)
        // rather than as a heap literal: base = [0x2ff28c], slot = duelObj->0x134.
        const uint32_t tlb = ld(0x2ff28cu) & 0x1FFFFFFFu;
        const uint32_t slot = du ? ld(du + 0x134u) : 0u;
        cur[12] = slot;
        cur[13] = (tlb && slot < 64u) ? ld(tlb + slot * 4u + 0xc34u) : 0u;
        static uint32_t s_prev[14]; static bool s_have = false;
        if (s_have && std::memcmp(cur, s_prev, sizeof cur) == 0) return;
        static const char *kName[14] = { "screen", "st.mode", "st.type", "st.628", "st.630",
                                         "du.110", "du.114", "du.118", "du.13c",
                                         "cf.3c38", "cf.3c3c", "cf.3c40", "tl.slot", "tl.value" };
        std::fprintf(stderr, "[matchwatch] frame %llu |", (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed));
        for (int i = 0; i < 14; ++i)
            std::fprintf(stderr, " %s=%u%s", kName[i], cur[i], (s_have && cur[i] != s_prev[i]) ? "*" : "");
        std::fprintf(stderr, "%s%s\n", du ? "" : "  (no duelObj)", so ? "" : "  (no stateObj)");
        std::memcpy(s_prev, cur, sizeof cur); s_have = true;
    }

    // [matchwatch] PS2X_MATCHWATCH=2 also reports every word of the duel object's first 0x400
    // bytes that changes. duelObj+0x13c was picked by diffing whole-RAM dumps at four time-limit
    // settings, and it reads 0 when the game's default is 3 -- so it is probably the wrong field.
    // Walking Battle Settings with this on names the right one directly.
    static void bt3MatchScan(uint8_t *rdram)
    {
        static const int s_lvl = [](){ const char *v = std::getenv("PS2X_MATCHWATCH");
                                       return (v && v[0]) ? std::atoi(v) : 0; }();
        if (s_lvl < 2 || !rdram) return;
        uint32_t du = 0; std::memcpy(&du, rdram + (0x3b38e8u & PS2_RAM_MASK), 4);
        du &= 0x1FFFFFFFu;
        static uint32_t s_base = 0; static uint32_t s_prev[0x100]; static bool s_have = false;
        if (!du) { s_have = false; return; }
        if (du != s_base) { s_base = du; s_have = false; }
        uint32_t cur[0x100];
        std::memcpy(cur, rdram + (du & PS2_RAM_MASK), sizeof cur);
        if (!s_have) { std::memcpy(s_prev, cur, sizeof cur); s_have = true; return; }
        if (std::memcmp(cur, s_prev, sizeof cur) == 0) return;
        std::fprintf(stderr, "[matchscan] frame %llu duelObj=0x%x |",
                     (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), du);
        for (int i = 0; i < 0x100; ++i)
            if (cur[i] != s_prev[i]) std::fprintf(stderr, " +0x%03x: %u -> %u", i * 4, s_prev[i], cur[i]);
        std::fprintf(stderr, "\n");
        std::memcpy(s_prev, cur, sizeof cur); s_have = true;
    }

    static void bt3StateWatch(uint8_t *rdram, R5900Context *ctx)
    {
        static const bool s_on = [](){ const char *v = std::getenv("PS2X_STATEWATCH");
                                       return v && v[0] && v[0] != '0'; }();
        if (!s_on || !rdram) return;
        uint32_t p = 0u, st = 0xffffffffu;
        std::memcpy(&p, rdram + (0x2ff10cu & PS2_RAM_MASK), 4);
        if (!p) return;
        const uint32_t stateAddr = ((p & 0x1FFFFFFFu) + 0x18u) & PS2_RAM_MASK;
        std::memcpy(&st, rdram + stateAddr, 4);
        // [statewatch] PS2X_STATEWATCH=2 also dumps the state OBJECT's first 0x40 bytes whenever
        // any of them changes. The top-level state is coarse -- the whole menu flow (main menu,
        // Duel, 1P VS 2P, character select, stage select) is ONE value, 0x04 -- so the
        // character-select screen is a SUB-STATE held elsewhere. The neighbouring fields of the
        // same object are the cheapest place to look for it.
        static const int s_deep = [](){ const char *v = std::getenv("PS2X_STATEWATCH");
                                        return (v && v[0]) ? std::atoi(v) : 0; }();
        if (s_deep >= 2)
        {
            static uint32_t s_prev[16] = {0}; static bool s_have = false;
            uint32_t cur[16];
            std::memcpy(cur, rdram + ((p & 0x1FFFFFFFu) & PS2_RAM_MASK), sizeof cur);
            if (!s_have || std::memcmp(cur, s_prev, sizeof cur) != 0)
            {
                std::fprintf(stderr, "[statewatch2] frame %llu st=0x%02x |", 
                             (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed), st);
                for (int i = 0; i < 16; ++i)
                    std::fprintf(stderr, " %c%08x", (s_have && cur[i] != s_prev[i]) ? '*' : ' ', cur[i]);
                std::fprintf(stderr, "\n");
                std::memcpy(s_prev, cur, sizeof cur); s_have = true;
            }
        }
        static uint32_t s_last = 0xdeadbeefu;
        if (st == s_last) return;
        std::fprintf(stderr, "[statewatch] frame %llu  state 0x%02x -> 0x%02x   (obj 0x%x, slot 0x%x)  pc=0x%x ra=0x%x\n",
                     (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed),
                     s_last == 0xdeadbeefu ? 0u : s_last, st, p, stateAddr + 0u,
                     ctx ? ctx->pc : 0u, ctx ? getRegU32(ctx, 31) : 0u);
        s_last = st;
    }

    extern "C" void ps2xFrameGateWait(uint64_t frame, uint8_t *rdram, R5900Context *ctx);   // [rollback] ps2_runtime.cpp
    extern "C" bool ps2xFrameStepOn();                                                        // [rollback] ps2_runtime.cpp
    extern "C" bool ps2xRenderSkipOn();                                                       // [rollback] ps2_memory.cpp
    void bt3FrameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00100ab8
    {
        // [netmenu] stash the pointers so the host menu can call sePlay() (the game's own SE).
        g_ps2xMenuRdram = rdram;
        g_ps2xMenuRuntime = runtime;
        // Keep the SE stream fed from the active voices. Effects are produced incrementally so
        // a stop-by-serial can cut a voice's tail; without a per-frame top-up a long effect
        // would only advance when the next SE command happened to arrive.
        seServiceVoices(runtime);
        g_bt3FrameCount.fetch_add(1, std::memory_order_relaxed);
        ps2xModsFrame(rdram, ctx, runtime);   // [mods] the mods' frame hooks (the Tag Team mod drives its loads here)
        // [rollback] the frame gate: in frame-stepped mode tid 1 parks here until the host controller
        // has had the boundary (snapshot / rollback) and opened the gate. No-op otherwise.
        ps2xFrameGateWait(g_bt3FrameCount.load(std::memory_order_relaxed), rdram, ctx);
        {   // [savestate] one save, one load, both at this hook -- see the note above
            static const char *s_save = std::getenv("PS2X_SAVESTATE");
            static const char *s_load = std::getenv("PS2X_LOADSTATE");
            static bool s_loaded = false, s_saved = false;
            // Do NOT load at the first hook we happen to reach. The host C++ stack MIRRORS the
            // guest call chain (see the note on bt3SaveState), so a snapshot taken deep in the
            // title loop must be restored at a structurally comparable point -- dropping frame 900
            // into a process still nested in boot leaves the stack describing a call chain that no
            // longer matches guest memory. Default: wait until THIS instance's own frame counter
            // reaches the frame the snapshot was taken at, which both instances arrive at by the
            // same boot path. PS2X_LOADSTATE_AT=<frame> overrides (0 = the old load-immediately).
            if (s_load && s_load[0] && !s_loaded)
            {
                static const long s_at = [](){ const char *v = std::getenv("PS2X_LOADSTATE_AT");
                                               return (v && v[0]) ? std::atol(v) : -1L; }();
                static const uint64_t s_want = (s_at >= 0) ? (uint64_t)s_at : bt3PeekStateFrame(s_load);
                if (g_bt3FrameCount.load(std::memory_order_relaxed) >= s_want)
                { s_loaded = true; bt3LoadState(s_load, rdram, ctx, runtime); }
            }
            if (s_save && s_save[0] && !s_saved)
            {
                const char *c = std::strchr(s_save, ':');
                const unsigned long long at = std::strtoull(s_save, nullptr, 10);
                    if (c && g_bt3FrameCount.load(std::memory_order_relaxed) >= at) { s_saved = true; bt3SaveState(c + 1, rdram, ctx, runtime); }
                }
                // [savestate] hotkeys: F6 = quicksave, F7 = quickload. Edge-triggered (one action
                // per press), same file for both. PS2X_SAVESTATE_PATH overrides it; default is the
                // deploy's savedata folder. This is what lets the intro be skipped: boot, F6 once
                // the menu is up, then on a later run press F7 (or leave PS2X_LOADSTATE set) and
                // the snapshot's frame counter jumps the host straight past the FMV/title.
                {
                    // [savestate] quick keys (F6 quicksave / F7 quickload / PS2X_QUICK*_AT) are OFF
                    // by default now; PS2X_QUICKSAVE=1 turns them back on. The env-driven
                    // PS2X_SAVESTATE / PS2X_LOADSTATE path above is untouched.
                    static const bool s_quickKeys = [](){
                        const char *v = std::getenv("PS2X_QUICKSAVE");
                        return v && v[0] && v[0] != '0';
                    }();
                    if (s_quickKeys)
                    {
                    static const char *s_slot = [](){
                        const char *v = std::getenv("PS2X_SAVESTATE_PATH");
                        return (v && v[0]) ? v : "savedata/bt3-quicksave.sst";
                    }();
                    // [savestate] deferred quickload: never load while the game is still opening
                    // memory cards / bringing the IOP up. A frame-30 load restored the whole
                    // IOP/EE/VU image into a process whose own init had not run yet: the sound
                    // system came up silent (measured: "no BGM after 12s"). A quickload requested
                    // before the memcard boot phase finishes is therefore DEFERRED until it does.
                    static bool s_qlPending = false;
                    const auto mcReady = []() -> bool {
                        const ps2_stubs::MemoryCardDebugSnapshot mc = ps2_stubs::getMemoryCardDebugSnapshot();
                        return mc.lastCmd != 0 && mc.openFiles.empty();
                    };
                    const auto doLoad = [&](const char *why){
                        const uint64_t now = g_bt3FrameCount.load(std::memory_order_relaxed);
                        std::fprintf(stderr, "[savestate] %s load <- %s (frame %llu, snapshot frame %llu)\n",
                                     why, s_slot, (unsigned long long)now,
                                     (unsigned long long)bt3PeekStateFrame(s_slot));
                        bt3LoadState(s_slot, rdram, ctx, runtime);
                    };
                    const auto requestLoad = [&](const char *why){
                        if (mcReady()) doLoad(why);
                        else if (!s_qlPending)
                        {
                            s_qlPending = true;
                            std::fprintf(stderr, "[savestate] %s load deferred until the memcard boot phase is done\n", why);
                        }
                    };
                    if (s_qlPending && mcReady()) { s_qlPending = false; doLoad("deferred"); }
                    static bool s_f6 = false, s_f7 = false;
                    const bool f6 = IsKeyDown(295), f7 = IsKeyDown(296);
                    if (f6 && !s_f6)
                    {
                        std::fprintf(stderr, "[savestate] F6 quicksave -> %s (frame %llu)\n",
                                     s_slot, (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed));
                        bt3SaveState(s_slot, rdram, ctx, runtime);
                    }
                    s_f6 = f6;
                    if (f7 && !s_f7) requestLoad("F7");
                    s_f7 = f7;
                    // PS2X_QUICKLOAD_AT=<frame> / PS2X_QUICKSAVE_AT=<frame>: fire the same action
                    // once, automatically, at that frame. This is the reproducible way to "force F7"
                    // (e.g. PS2X_QUICKLOAD_AT=30 to slam a menu snapshot into a fresh boot and see
                    // whether the intro is skipped or the mirrored host stack breaks).
                    static const long s_qlAt = [](){ const char *v = std::getenv("PS2X_QUICKLOAD_AT");
                        return (v && v[0]) ? std::atol(v) : -1L; }();
                    static const long s_qsAt = [](){ const char *v = std::getenv("PS2X_QUICKSAVE_AT");
                        return (v && v[0]) ? std::atol(v) : -1L; }();
                    static bool s_qlDone = false, s_qsDone = false;
                    const uint64_t frNow = g_bt3FrameCount.load(std::memory_order_relaxed);
                    if (!s_qsDone && s_qsAt >= 0 && frNow >= (uint64_t)s_qsAt)
                    {
                        s_qsDone = true;
                        std::fprintf(stderr, "[savestate] QUICKSAVE_AT frame %llu -> %s\n",
                                     (unsigned long long)frNow, s_slot);
                        bt3SaveState(s_slot, rdram, ctx, runtime);
                    }
                    if (!s_qlDone && s_qlAt >= 0 && frNow >= (uint64_t)s_qlAt)
                    {
                        s_qlDone = true;
                        std::fprintf(stderr, "[savestate] QUICKLOAD_AT frame %llu <- %s (snapshot frame %llu)\n",
                                     (unsigned long long)frNow, s_slot,
                                     (unsigned long long)bt3PeekStateFrame(s_slot));
                        requestLoad("QUICKLOAD_AT");
                    }
                    }   // [savestate] s_quickKeys
                }
        }
        bt3StateWatch(rdram, ctx);   // [statewatch]
        bt3MatchWatch(rdram);        // [matchwatch]
        bt3MatchScan(rdram);         // [matchwatch] level 2
        bt3MemWatch(rdram);          // [memwatch]
        bt3MemBlock(rdram);          // [memblock]
        bt3DumpKey(rdram);           // [dumpkey]
        if (bt3StateLogOn()) bt3StateLogFrame(rdram);   // [statelog] one line per screen change
        bt3NetJumpCharSelect(rdram, ctx, runtime); // [netjump]
        bt3MenuJumpFrame(rdram, ctx, runtime);     // [menujump] PS2X_MENU_JUMP + P+L / LMB+RMB combo
        ps2x_dueldump::tick(rdram, runtime);   // [dueldump] PS2X_DUELDUMP=1
        ps2x_dueldump::tickSettings(rdram);    // [duelsettings] PS2X_DUELSETTINGS=1
        ps2x_dueldump::tickTime(rdram);        // [dueltime] PS2X_DUELTIME=1
        ps2NetInit();   // [netplay] no-op unless PS2X_NET / PS2X_NET_LISTEN is set
        ps2NetFrame(static_cast<uint32_t>(g_bt3FrameCount.load(std::memory_order_relaxed)));
        // [ach] One condition evaluation per presented frame, next to the netplay tick because it
        // is the same kind of guest-RAM observer. It is a no-op unless the tracker is on, and it
        // runs on this thread rather than from swapFrame() so that the [netjump] freeze, which
        // returns early in swapFrame() before reaching the renderer, cannot skip a frame of
        // evaluation while the screen is held.
        ps2AchFrame(rdram);
        // [notify] The drop box, polled from here rather than from the overlay's draw(). The tick
        // runs every frame whatever the UI is doing, and it is the only place with the guest RAM in
        // hand -- which the box's `dump` verb needs. The module throttles the actual stat().
        ps2xNotifyPollDropBox(ps2xDropBoxDir(), rdram);
        ps2DetHashFrame(rdram, ctx->vu0_r);   // [dethash]
        if (g_ps2StepCensus.load(std::memory_order_relaxed)) ps2StepCensusFrame(ctx);   // [stepcensus]
        ps2HalfStepFrame(ctx);        // [halfstep] (no-op unless configured; raises the macro switch on fight frames only)
        {   // [findclock] PS2X_FINDCLOCK=<start value>: when the fight gate opens, remember every 32-bit slot holding a value
            // within 12 of the start value (int, or float), and 300 render frames (5 s at 60) later print the ones that
            // moved by 2..14 -- a countdown in seconds shows up as the slot that lost ~5 (or ~10 if it runs at 2x).
            static const int s_fc = [](){ const char *v = std::getenv("PS2X_FINDCLOCK"); return v && v[0] ? std::atoi(v) : 0; }();
            if (s_fc && ps2HalfStepFightActive())
            {
                static uint64_t s_t0 = 0; static std::vector<std::pair<uint32_t, float>> s_cand; static bool s_done = false;
                const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
                if (!s_t0)
                {
                    s_t0 = fr;
                    for (uint32_t a = 0x100000u; a < 32u * 1024u * 1024u; a += 4u)
                    {
                        uint32_t bits; std::memcpy(&bits, rdram + a, 4);
                        const int32_t iv = (int32_t)bits; float fv; std::memcpy(&fv, &bits, 4);
                        if (iv >= s_fc - 12 && iv <= s_fc + 12 && iv != 0) s_cand.push_back({a, (float)iv});
                        else if (std::isfinite(fv) && fv >= s_fc - 12.f && fv <= s_fc + 12.f) s_cand.push_back({a | 0x80000000u, fv});
                    }
                    std::fprintf(stderr, "[findclock] armed at frame %llu: %zu slots near %d\n", (unsigned long long)fr, s_cand.size(), s_fc);
                }
                else if (!s_done && fr - s_t0 >= 300u)
                {
                    s_done = true; unsigned n = 0;
                    for (const auto &c : s_cand)
                    {
                        const uint32_t a = c.first & 0x1FFFFFFFu; uint32_t bits; std::memcpy(&bits, rdram + a, 4);
                        float now; if (c.first & 0x80000000u) std::memcpy(&now, &bits, 4); else now = (float)(int32_t)bits;
                        const float drop = c.second - now;
                        if (drop >= 2.f && drop <= 14.f && n++ < 40)
                            std::fprintf(stderr, "[findclock] %s0x%x: %g -> %g (dropped %g in 300 frames)\n", (c.first & 0x80000000u) ? "float " : "int ", a, c.second, now, drop);
                    }
                    std::fprintf(stderr, "[findclock] done: %u candidates dropped 2..14\n", n);
                }
            }
        }
        {   // [animprobe] PS2X_ANIMPROBE=<hex addr>[,<hex addr>...]: once the fight gate is open, print those floats every
            // 10 render frames for 600 frames -- animation frame counters, position components -- to MEASURE the pace
            // an "i:" prefix prints the slot as a 32-bit integer (counters, the match clock); plain = float
            static const std::vector<uint32_t> s_probes = [](){ std::vector<uint32_t> v; if (const char *e = std::getenv("PS2X_ANIMPROBE")) { std::string t(e); size_t p0 = 0; while (p0 < t.size()) { size_t p1 = t.find(',', p0); if (p1 == std::string::npos) p1 = t.size(); if (p1 > p0) { std::string tok = t.substr(p0, p1 - p0); const bool isInt = tok.rfind("i:", 0) == 0; if (isInt) tok = tok.substr(2); v.push_back((uint32_t)std::strtoul(tok.c_str(), nullptr, 16) | (isInt ? 0x80000000u : 0u)); } p0 = p1 + 1; } } return v; }();
            if (!s_probes.empty() && ps2HalfStepFightActive())
            {
                static uint32_t s_n = 0; static std::vector<float> s_prev(s_probes.size(), 0.f);
                static const uint32_t s_every = [](){ const char *v = std::getenv("PS2X_ANIMPROBE_EVERY"); const int n = v && v[0] ? std::atoi(v) : 10; return (uint32_t)(n < 1 ? 1 : n); }();
                static const uint32_t s_count = [](){ const char *v = std::getenv("PS2X_ANIMPROBE_N"); const int n = v && v[0] ? std::atoi(v) : 60; return (uint32_t)(n < 1 ? 1 : n); }();
                if (s_n < s_count * s_every)
                {
                    if ((s_n % s_every) == 0u)
                    {
                        std::string line;
                        for (size_t k = 0; k < s_probes.size(); ++k)
                        {
                            float f = 0.f; if (const uint8_t *q = getMemPtr(rdram, s_probes[k] & 0x1FFFFFFFu)) { if (s_probes[k] & 0x80000000u) { int32_t i32; std::memcpy(&i32, q, 4); f = (float)i32; } else std::memcpy(&f, q, 4); }
                            char b[96]; std::snprintf(b, sizeof b, " [%s0x%x]=%g (d10=%g)", (s_probes[k] & 0x80000000u) ? "i:" : "", s_probes[k] & 0x1FFFFFFFu, f, f - s_prev[k]); line += b; s_prev[k] = f;
                        }
                        std::fprintf(stderr, "[animprobe] frame %llu:%s\n", (unsigned long long)g_bt3FrameCount.load(), line.c_str());
                    }
                    ++s_n;
                }
            }
        }
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
            // PS2X_FRAMEGATE_FORCEHEAVY=1 (dev): treat every frame as heavy on a fast box, to exercise the gated +
            // relaxed-pacing path that slow machines take (the vblank period itself stays real).
            static const bool s_forceHeavy = [](){ const char *v = std::getenv("PS2X_FRAMEGATE_FORCEHEAVY"); return v && v[0] && v[0] != '0'; }();
            // PS2X_FRAMEGATE_FORCELIGHT=1 (dev): the opposite -- treat every frame as light, to reproduce on a
            // slow box what a fast one does when the render fits in a vsync.
            static const bool s_forceLight = [](){ const char *v = std::getenv("PS2X_FRAMEGATE_FORCELIGHT"); return v && v[0] && v[0] != '0'; }();
            const bool heavy = s_forceHeavy || (!s_forceLight && g_workerFrameNs.load(std::memory_order_relaxed) > s_vsyncNs);
            // [fightgate] The heavy test alone ties GAME SPEED to host performance: a machine whose render
            // fits in one vsync (a 3070 Ti, 2026-09-17, right after the native VU1 kernels lightened the
            // worker) is never "heavy", so the fight loop runs every vblank -- 40-60 fps of game logic,
            // i.e. fast-forward, exactly the async failure described above, now on the fast machines. The
            // console's 30 in fights is a property of the FIGHT, not of the load, so gate on the state:
            // 0x27 (fight), 0x28 (team/DP battle), 0x2d (in-fight). Menus (0x04) stay ungated at 60.
            // PS2X_FRAMEGATE_FIGHT=0 restores the load-only rule.
            static const bool s_fightGate = [](){ const char *v = std::getenv("PS2X_FRAMEGATE_FIGHT"); return !(v && v[0] == '0'); }();
            const uint32_t stLive = g_bt3StateLive.load(std::memory_order_relaxed);
            const bool fightState = (stLive == 0x27u || stLive == 0x2du);
            // The FIGHT-LOAD shares state 0x27 with the fight, and gating it halved the loader (its CD
            // pump is per frame): 13 s at 29 fps instead of 6 s at 60 (2026-09-17). What separates them
            // is the guest's own render work: a loading/minigame frame runs none of the fight's VU1
            // programs (vu1pairs = 0), a fight frame runs ~3M pairs. Latch "the fight is rendering" on
            // the first frame with real VU1 work and hold it for the rest of the fight state.
            static uint64_t s_lastPairs = 0; static bool s_fightRendering = false;
            {
                const uint64_t pairs = g_vu1PairCount.load(std::memory_order_relaxed);
                const uint64_t delta = pairs - s_lastPairs; s_lastPairs = pairs;
                static uint64_t s_lastChunks = 0;
                const uint64_t chunks = g_seamHostChunks.load(std::memory_order_relaxed);
                const uint64_t dChunks = chunks - s_lastChunks; s_lastChunks = chunks;
                static const bool s_seamLatch = [](){ const char *v = std::getenv("PS2X_FRAMEGATE_SEAMLATCH"); return v && v[0] == '1'; }();   // off: with it the seam path dropped to 5 fps (the gate's pacing branch misbehaves there; open item); fights in 30 fps mode may run ungated on the native path
                if (!fightState) s_fightRendering = false;
                else if (delta > 200000ull || (s_seamLatch && dChunks > 50ull)) s_fightRendering = true;   // seam path: the meshes it emits are the fight's VU1 work
            }
            const bool inFight = s_fightGate && fightState && s_fightRendering;
            g_ps2xFrameGateHeavy.store(s_gate && heavy && PS2Memory::asyncKickEnabled(), std::memory_order_relaxed);   // [syncrelax]
            // [rollback] in frame-stepped mode the controller paces vblanks; a host sleep here would only starve them
            if (s_gate && (heavy || inFight) && PS2Memory::asyncKickEnabled() && !ps2xFrameStepOn())
            {
                static uint64_t s_lastTick = 0;
                // [fps60gate] The 2-tick target IS a 30 fps lock: two vsyncs at 60 Hz = 33.3 ms. That is
                // right for the game's native 30 fps fight loop, and it is what sync mode produced on
                // console. But it was hardcoded, so it also fired with the 60 fps mode ON -- the mode
                // whose whole purpose is to run the fight loop every vblank. The gate then paced 60 fps
                // logic at 30, and no amount of making the renderer faster could show up, because the
                // brake is applied per frame regardless of how quickly the frame was produced.
                // Match the target to the frame step the game is actually running: 1 tick at 60, 2 at 30.
                const uint64_t ticks = ps2VStepActive() ? 1u : 2u;
                const uint64_t want = s_lastTick + ticks;
                // Bounded: never wait more than ~50 ms, so a stalled vblank worker cannot hang
                // the guest (the failure mode I wrongly suspected of [asyncpace] earlier tonight).
                Ps2xWaitScope wgate(WP_FRAMEGATE);   // [waitprof]
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
        if (!ps2xRenderSkipOn())   // [rollback] a re-simulated frame has nothing to publish
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
            static const bool s_lg = std::getenv("PS2X_OVLOG") != nullptr || std::getenv("PS2X_MOVIEPROBE") != nullptr;   // [movprobe]
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

    // [movprobe] PS2X_MOVIEPROBE=1: opening-movie flow points. The movie state block is the
    // guest u32 at 0x00301048. START = sub_00126D40 (state := 1), STOP = sub_00126DD8
    // (state := 2), END/SKIP predicate = FUN_00126E88 = (state >> 3) & 1, sequencer =
    // f_35de58 (overlay). All trampoline to the originals.
    PS2Runtime::RecompiledFunction g_orig126D40 = nullptr;
    PS2Runtime::RecompiledFunction g_orig126DD8 = nullptr;
    PS2Runtime::RecompiledFunction g_orig126E88 = nullptr;
    PS2Runtime::RecompiledFunction g_orig35DE58 = nullptr;

    static uint32_t movprobeU32(uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0xffffffffu;
        if (const uint8_t *p = getMemPtr(rdram, addr)) std::memcpy(&v, p, 4);
        return v;
    }

    void bt3MovieStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_00126D40
    {
        std::cerr << "[movprobe] START sub_126D40 a0=0x" << std::hex << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6)
                  << " a3=0x" << getRegU32(ctx, 7)
                  << " state_before=0x" << movprobeU32(rdram, 0x301048u)
                  << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
        if (g_orig126D40) g_orig126D40(rdram, ctx, runtime);
        std::cerr << "[movprobe] START done state=0x" << std::hex
                  << movprobeU32(rdram, 0x301048u) << std::dec << std::endl;
    }

    void bt3MovieStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // sub_00126DD8
    {
        std::cerr << "[movprobe] STOP sub_126DD8 a0=0x" << std::hex << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6)
                  << " a3=0x" << getRegU32(ctx, 7)
                  << " state_before=0x" << movprobeU32(rdram, 0x301048u)
                  << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
        if (g_orig126DD8) g_orig126DD8(rdram, ctx, runtime);
        std::cerr << "[movprobe] STOP done state=0x" << std::hex
                  << movprobeU32(rdram, 0x301048u) << std::dec << std::endl;
    }

    void bt3MovieEndPred(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // FUN_00126E88
    {
        if (g_orig126E88) g_orig126E88(rdram, ctx, runtime);
        const uint32_t v0 = getRegU32(ctx, 2);
        static uint32_t s_last = 0xffffffffu;
        if (v0 != s_last)
        {
            s_last = v0;
            std::cerr << "[movprobe] ENDPRED FUN_126E88 v0=" << v0
                      << " state=0x" << std::hex << movprobeU32(rdram, 0x301048u)
                      << " level=" << std::dec << movprobeU32(rdram, 0x301050u)
                      << " ra=0x" << std::hex << getRegU32(ctx, 31) << std::dec << std::endl;
        }
    }

    void bt3MovieSeq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // f_35de58 (overlay)
    {
        static uint32_t s_entries = 0u;
        if (s_entries < 12u)
        {
            ++s_entries;
            std::cerr << "[movprobe] SEQ f_35de58 enter a0=0x" << std::hex << getRegU32(ctx, 4)
                      << " state=0x" << movprobeU32(rdram, 0x301048u)
                      << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
        }
        if (g_orig35DE58) g_orig35DE58(rdram, ctx, runtime);
    }

    // [ovmain] The overlay ENTRY POINT f_334c00 (DBZP.BIN e_entry = 0x334c00) hangs with a REAL
    // memory-card save loaded. overlay_register maps only two slots for it:
    //     [0]  = 0x334c00 (entry)   [20] = 0x334c50 (the one real label)
    // and leaves 1..19 NULL. The host re-dispatches the function with ctx->pc = 0x334c40 (mid-body,
    // right before the first `jal`); the dispatcher resolves slot (0x334c40-0x334c00)/4 = 16, finds
    // it NULL, and falls into the gap handler which re-dispatches the SAME pc -> infinite
    // host-side re-entry. The profiler pegs 100% at 0x334c40 (that pc, not a guest loop). The
    // empty save never reached the overlay, which is why this only appears with a populated card.
    //
    // Fix: point slot 16 at a stub that jumps to the return path (0x334c50, slot 20) so the
    // pending call sequence completes instead of spinning. Same for the other mid-body slots
    // (1..19) so any of them can re-enter without the same fate. PS2X_OVMAIN=0 disables.
    void bt3OverlayMidReentry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s_n = 0;
        if (s_n < 20)
        {
            ++s_n;
            std::fprintf(stderr, "[ovmain] mid-body re-entry at 0x%X -> jump 0x334c50\n", ctx->pc);
        }
        ctx->pc = 0x334c50u;   // the real label: the two `jal`s have already been dispatched
    }

    void applyBt3SoundInitBypass(PS2Runtime &runtime)
    {
        std::cerr << "[game_overrides] BT3: sound init bypass + lock-callback stub" << std::endl;
        if (const char *mv = std::getenv("PS2X_MOVIEPROBE"); mv && mv[0] && mv[0] != '0')
        {
            g_orig126D40 = runtime.lookupFunction(0x00126D40u);
            if (g_orig126D40) runtime.replaceFunction(0x00126D40u, &bt3MovieStart);
            g_orig126DD8 = runtime.lookupFunction(0x00126DD8u);
            if (g_orig126DD8) runtime.replaceFunction(0x00126DD8u, &bt3MovieStop);
            g_orig126E88 = runtime.lookupFunction(0x00126E88u);
            if (g_orig126E88) runtime.replaceFunction(0x00126E88u, &bt3MovieEndPred);
            const uint32_t slot35 = (0x0035DE58u - g_ps2OverlayFunctionTableBase) / 4u;
            if (slot35 < g_ps2OverlayFunctionTableSlotCount && g_ps2OverlayFunctionTable[slot35])
            {
                g_orig35DE58 = g_ps2OverlayFunctionTable[slot35];
                g_ps2OverlayFunctionTable[slot35] = &bt3MovieSeq;
            }
            std::fprintf(stderr, "[movprobe] hooks start=%d stop=%d endpred=%d seq=%d\n",
                         g_orig126D40 ? 1 : 0, g_orig126DD8 ? 1 : 0,
                         g_orig126E88 ? 1 : 0, g_orig35DE58 ? 1 : 0);
        }
        {   // [ovmain] The overlay table is populated by a static initializer (overlay_register.cpp),
            // so it is already filled when this runs -- this does NOT belong inside the
            // PS2X_MOVIEPROBE gate above. Fill the NULL mid-body slots of the overlay entry
            // f_334c00 (1..19, addresses 0x334c04..0x334c4c) with the re-entry stub so a host
            // re-dispatch there resolves instead of falling into the gap handler and spinning.
            static const bool s_ov = [](){ const char *v = std::getenv("PS2X_OVMAIN"); return !(v && v[0] == (char)48); }();
            if (s_ov)
            {
                int n_filled = 0;
                for (uint32_t slot = 1u; slot < 20u; ++slot)
                {
                    if (slot < g_ps2OverlayFunctionTableSlotCount && g_ps2OverlayFunctionTable[slot] == nullptr)
                    {
                        g_ps2OverlayFunctionTable[slot] = &bt3OverlayMidReentry;
                        ++n_filled;
                    }
                }
                std::fprintf(stderr, "[ovmain] filled %d mid-body re-entry slots (1..19) of f_334c00\n", n_filled);
            }
        }
        {   // [ovlazy] Report NULL runs in the overlay dispatch table. The table gets one entry
            // per LABEL the generator detected, so a function body with a single label leaves
            // every intermediate slot NULL. A host re-dispatch at one of those slots falls into
            // the gap handler, which re-dispatches the same PC forever -- that is what froze the
            // game on a populated save (see docs/BUGFIXES-TODO.md). A NULL slot is not itself a
            // bug: most of the body is straight-line code the host never re-enters. But a NULL
            // run inside a function that HAS been re-dispatched is the signature, and a
            // re-dispatch counter makes that visible without waiting for a hang.
            // PS2X_OVLAZY=0 disables; PS2X_OVLAZYFULL=1 lists every run instead of the summary.
            static const bool s_lazy = [](){ const char *v = std::getenv("PS2X_OVLAZY"); return !(v && v[0] == (char)48); }();
            if (s_lazy)
            {
                const bool full = std::getenv("PS2X_OVLAZYFULL") != nullptr;
                uint32_t n_null_total = 0u, n_runs = 0u, run_start = 0u;
                bool in_run = false;
                for (uint32_t slot = 0u; slot < g_ps2OverlayFunctionTableSlotCount; ++slot)
                {
                    const bool is_null = (g_ps2OverlayFunctionTable[slot] == nullptr);
                    if (is_null)
                    {
                        ++n_null_total;
                        if (!in_run) { in_run = true; run_start = slot; }
                    }
                    else if (in_run)
                    {
                        in_run = false;
                        ++n_runs;
                        if (full)
                        {
                            const uint32_t len = slot - run_start;
                            std::fprintf(stderr, "[ovlazy] NULL run: slots %u..%u (0x%X..0x%X, %u)\n",
                                         run_start, slot - 1u,
                                         g_ps2OverlayFunctionTableBase + run_start * 4u,
                                         g_ps2OverlayFunctionTableBase + slot * 4u - 4u, len);
                        }
                    }
                }
                if (in_run) ++n_runs;   // table ran out mid-run
                std::fprintf(stderr, "[ovlazy] overlay table: %u slots, %u NULL in %u runs%s\n",
                             g_ps2OverlayFunctionTableSlotCount, n_null_total, n_runs,
                             full ? "" : "  (PS2X_OVLAZYFULL=1 to list them)");
            }
        }
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
        if (const char *sc = std::getenv("PS2X_STEPCENSUS"); sc && sc[0]) ps2StepCensusEnable(sc);   // [stepcensus]
        if (const char *aw = std::getenv("PS2X_ADDRWATCH"); aw && aw[0]) ps2AddrWatchEnable(aw);   // [addrwatch]
        if (const char *st = std::getenv("PS2X_STORETRACE"); st && st[0]) ps2StoreTraceEnable(st);   // [storetrace]
        if (const char *hs = std::getenv("PS2X_HALFSTEP"); hs && hs[0]) ps2HalfStepEnable(hs);        // [halfstep]
        if (true)
        {   // [vstep] [logicrate] [fps60] hooks are always installed so the overlay toggle needs no restart
            g_orig102060 = runtime.lookupFunction(0x00102060u);
            if (g_orig102060) runtime.replaceFunction(0x00102060u, &bt3VStep);
            g_orig115950 = runtime.lookupFunction(0x00115950u);
            g_orig23e770 = runtime.lookupFunction(0x0023e770u);   // [netview]
            if (g_orig23e770) runtime.replaceFunction(0x0023e770u, &bt3NetViewSelect);
            {   // [netjump] the versus-menu loop, in the OVERLAY (base 0x334c00)
                g_orig356090 = runtime.lookupFunction(0x00356090u);
                if (g_orig356090 && runtime.replaceFunction(0x00356090u, &bt3VersusMenuGate))
                    std::fprintf(stderr, "[netjump] versus-menu gate hooked at 0x356090\n");
                else
                    std::fprintf(stderr, "[netjump] could NOT hook 0x356090 (overlay not resident yet?)\n");
            }
            g_orig121d48 = runtime.lookupFunction(0x00121d48u);   // [clipguard]
            if (g_orig121d48) runtime.replaceFunction(0x00121d48u, &bt3ClipXformGuard);
            g_orig11f548 = runtime.lookupFunction(0x0011f548u);
            g_orig121a10 = runtime.lookupFunction(0x00121a10u);   // [clipguard] clip pass
            if (g_orig121a10) runtime.replaceFunction(0x00121a10u, &bt3ClipPassGuard);
            if (g_orig11f548) runtime.replaceFunction(0x0011f548u, &bt3AngleWrapGuard);
            std::fprintf(stderr, "[clipguard] armed (0x121d48 %s, 0x11f548 %s)\n", g_orig121d48 ? "ok" : "MISSING", g_orig11f548 ? "ok" : "MISSING");
            g_orig264d98 = runtime.lookupFunction(0x00264d98u);
            if (g_orig264d98) runtime.replaceFunction(0x00264d98u, &bt3WaitProbe);   // [vstepprobe]
            g_orig1de8a8 = runtime.lookupFunction(0x001de8a8u);   // [fps60 predict]
            if (g_orig1de8a8) runtime.replaceFunction(0x001de8a8u, &bt3PredictAhead);
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
        {   // [postskip]
            {   // [steporacle] installed on its own, with or without the skips (the oracle address is never skipped)
                if (const char *v = std::getenv("PS2X_STEPORACLE"); v && v[0])
                {
                    g_oracleAddr = (uint32_t)std::strtoul(v, nullptr, 16);
                    if (const char *c = std::strchr(v, ':')) g_oracleNth = (uint32_t)std::atoi(c + 1);
                    g_origOracleStep = runtime.lookupFunction(g_oracleAddr);
                    if (g_origOracleStep) { runtime.replaceFunction(g_oracleAddr, &bt3StepOracleHook); std::fprintf(stderr, "[steporacle] step 0x%x, call %u\n", g_oracleAddr, g_oracleNth); }
                }
            }
            static const bool s_postSkip = [](){ const char *v = std::getenv("PS2X_POSTSKIP"); return v && v[0] && v[0] != '0'; }();
            if (s_postSkip)
            {
                int n = 0;
                // The STEP functions, not the orchestrators: FUN_0010ff40 also renders the characters (0x10fb80 / 0x10fc50
                // call the per-model draw loop). Steps: depth mask, ink, glow composite, downscale + material tints,
                // Z top-byte plane + tint, 16-bit mask work, Z-plane writer, mask clear, blur.
                // PS2X_POSTSKIP=1: the nine steps; =2: + func_111E50 and the context census; =3: + sub_002493A0 (see below)
                const int lvl = std::atoi(std::getenv("PS2X_POSTSKIP"));
                for (uint32_t a : { 0x00109848u, 0x00245a50u, 0x00103070u, 0x00102120u, 0x0024b118u, 0x00105cd8u, 0x00108750u, 0x00106ba8u, 0x00111e68u })
                    if (a != g_oracleAddr && runtime.lookupFunction(a)) { runtime.replaceFunction(a, &bt3PostSkipHook); ++n; }
                if (lvl >= 2)
                {
                    if (runtime.lookupFunction(0x00111e50u)) { runtime.replaceFunction(0x00111e50u, &bt3PostSkipHook); ++n; }
                    g_origCtxSwitch = runtime.lookupFunction(0x0010a218u);
                    if (g_origCtxSwitch) runtime.replaceFunction(0x0010a218u, &bt3CtxSwitchHook);
                }
                // level 3: sub_002493A0 (post B's frame-context switch before its character passes) does nothing, so those
                // passes draw the characters into the frame in force instead of the post chain's own target.
                if (lvl >= 3 && runtime.lookupFunction(0x002493a0u)) { runtime.replaceFunction(0x002493a0u, &bt3PostSkipHook); ++n; }
                std::fprintf(stderr, "[postskip] level %d: %d post-chain steps skipped\n", lvl, n);
            }
        }
        if (seamprobe::kickProbeOn())
        {   // [kickprobe]
            PS2Runtime::RecompiledFunction fns[] = { &bt3KickHook<0>, &bt3KickHook<1>, &bt3KickHook<2> };
            int nk = 0;
            for (int i = 0; i < 3; ++i) { g_kickHooks[i].orig = runtime.lookupFunction(g_kickHooks[i].addr); if (g_kickHooks[i].orig) { runtime.replaceFunction(g_kickHooks[i].addr, fns[i]); ++nk; } }
            PS2Runtime::RecompiledFunction afns[] = { &bt3AllocHook<0>, &bt3AllocHook<1>, &bt3AllocHook<2> };
            int na = 0;
            for (int i = 0; i < 3; ++i) { g_allocHooks[i].orig = runtime.lookupFunction(g_allocHooks[i].addr); if (g_allocHooks[i].orig) { runtime.replaceFunction(g_allocHooks[i].addr, afns[i]); ++na; } }
            g_origAlloc = g_allocHooks[0].orig;
            PS2Runtime::RecompiledFunction efns[] = { &bt3EmitHook<0>, &bt3EmitHook<1>, &bt3EmitHook<2>, &bt3EmitHook<3>, &bt3EmitHook<4>, &bt3EmitHook<5>, &bt3EmitHook<6>, &bt3EmitHook<7>,
                                                     &bt3EmitHook<8>, &bt3EmitHook<9>, &bt3EmitHook<10>, &bt3EmitHook<11>, &bt3EmitHook<12>, &bt3EmitHook<13>, &bt3EmitHook<14>, &bt3EmitHook<15> };
            int ne = 0;
            for (int i = 0; i < 16; ++i) { g_emitHooks[i].orig = runtime.lookupFunction(g_emitHooks[i].addr); if (g_emitHooks[i].orig) { runtime.replaceFunction(g_emitHooks[i].addr, efns[i]); ++ne; } }
            std::fprintf(stderr, "[kickprobe] installed %d/3 DMA-send hooks, %d/3 list-pointer hooks, %d/16 emitter hooks\n", nk, na, ne);
        }
        if (seamprobe::on())
        {   // [seamprobe] see bt3SeamDrawHook
            g_origSeamDraw = runtime.lookupFunction(0x00111358u);
            if (g_origSeamDraw) runtime.replaceFunction(0x00111358u, &bt3SeamDrawHook);
            g_origSeamListEnd = runtime.lookupFunction(0x00100798u);
            if (g_origSeamListEnd) runtime.replaceFunction(0x00100798u, &bt3SeamListEndHook);
            PS2Runtime::RecompiledFunction fns[] = {
                &bt3SeamBuilderHook<0>, &bt3SeamBuilderHook<1>, &bt3SeamBuilderHook<2>, &bt3SeamBuilderHook<3>,
                &bt3SeamBuilderHook<4>, &bt3SeamBuilderHook<5>, &bt3SeamBuilderHook<6>, &bt3SeamBuilderHook<7>};
            int nb = 0;
            for (int i = 0; i < 8; ++i)
            {
                g_seamBuilders[i].orig = runtime.lookupFunction(g_seamBuilders[i].addr);
                if (g_seamBuilders[i].orig) { runtime.replaceFunction(g_seamBuilders[i].addr, fns[i]); ++nb; }
            }
            std::fprintf(stderr, "[seamprobe] installed draw=%d listend=%d builders=%d/8\n",
                         g_origSeamDraw ? 1 : 0, g_origSeamListEnd ? 1 : 0, nb);
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
            if (const char *v = std::getenv("PS2X_CLIPRECTLOG"); v && v[0] && v[0] != '0')
            {   // [cliprectlog]
                g_orig224be8 = runtime.lookupFunction(0x00224be8u);
                if (g_orig224be8) runtime.replaceFunction(0x00224be8u, &bt3ClipRectLog);
                std::fprintf(stderr, "[cliprect] hook %s\n", g_orig224be8 ? "ARMED" : "FAILED");
                if (v[0] == '4')
                {
                    g_orig126b10 = runtime.lookupFunction(0x00126b10u);
                    if (g_orig126b10) runtime.replaceFunction(0x00126b10u, &bt3ScissorWrapLog);
                    std::fprintf(stderr, "[sciswrap] hook %s\n", g_orig126b10 ? "ARMED" : "FAILED");
                }
                if (v[0] == '3')
                {
                    g_orig100648 = runtime.lookupFunction(0x00100648u);
                    if (g_orig100648) runtime.replaceFunction(0x00100648u, &bt3ScissorCbLog);
                    std::fprintf(stderr, "[sciscb] hook %s\n", g_orig100648 ? "ARMED" : "FAILED");
                }
                if (v[0] == '2')
                {
                    g_orig101400 = runtime.lookupFunction(0x00101400u);
                    if (g_orig101400) runtime.replaceFunction(0x00101400u, &bt3EmitLog);
                    std::fprintf(stderr, "[emit] hook %s\n", g_orig101400 ? "ARMED" : "FAILED");
                }
            }
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
        // [deploy] Pads live in <deploy>/savedata/pad_pN.conf while the ELF sits
        // in <deploy>/data, so anchor to the deploy root (same convention as
        // mcRoot below in configureIoPathsFromElf) or the launcher's saves
        // would never be found.
        ps2_stubs::padConfigInit(runtime.getIoPaths().elfDirectory.parent_path().string());
        runtime.replaceFunction(0x00295160u, &bt3PadConnect);
        runtime.replaceFunction(0x00296160u, &bt3PadStatus);
        runtime.replaceFunction(0x00296090u, &bt3PadRead);
        runtime.replaceFunction(0x00295fb8u, &bt3PadGetState);
        runtime.replaceFunction(0x00295e58u, &bt3PadCreateSocket);
        runtime.replaceFunction(0x00122f10u, &bt3PadSendRumble);   // [rumble]
        ps2xModsInstall(runtime);   // [mods] loadable mods in mods/ (the Tag Team mod lives there now, ps2x_mod_api.h)
        if (const char *rf = std::getenv("PS2X_RUMBLE_FORCE"); rf && rf[0] == '1')
        {   // [rumble] test knob, see bt3VibrationDriverForced
            g_orig1dc5e0 = runtime.lookupFunction(0x001dc5e0u);
            runtime.replaceFunction(0x001dc5e0u, &bt3VibrationDriverForced);
            g_orig1dc520 = runtime.lookupFunction(0x001dc520u); runtime.replaceFunction(0x001dc520u, &bt3VibStartBigTrace);
            g_orig1dc578 = runtime.lookupFunction(0x001dc578u); runtime.replaceFunction(0x001dc578u, &bt3VibStartSmallTrace);
        }
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
    // [modules] The same five, registered for the install's renamed boot ELF. The matcher keys on the
    // ELF's BASENAME and every BT3 descriptor carries crc32 = 0, so the name is the only criterion --
    // an install that ships the ELF as data/Modules/BOOT would otherwise lose all of them with no
    // error at all. One descriptor matches either name (the other `continue`s), so nothing applies twice.
    PS2_REGISTER_GAME_OVERRIDE("BT3 sound init bypass", "BOOT", 0u, 0u, &applyBt3SoundInitBypass);
    PS2_REGISTER_GAME_OVERRIDE("BT3 DTX sound URPC compat", "BOOT", 0u, 0u, &applyBt3DtxCompat);
    PS2_REGISTER_GAME_OVERRIDE("BT3 sceMpeg callback stubs", "BOOT", 0u, 0u, &applyBt3MpegCallbackStubs);
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
    extern "C" void ps2xGuestSleepMs(unsigned ms);   // [fibers] parks the guest fiber, not the host thread
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
            ps2xGuestSleepMs(2u);   // [fibers] the loader is a fiber on this host thread: park, do not sleep
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
    // [modules] ...and for the renamed boot ELF, same reasoning as the three above. These two are the
    // ones whose absence is a real gameplay bug rather than a cosmetic one: the NULL packet-list guard
    // is the infinite-loading freeze, and the CD read-state guard is a load-time edge case.
    PS2_REGISTER_GAME_OVERRIDE("BT3 NULL packet-list guard", "BOOT", 0u, 0u, &applyBt3NullPacketGuard);
    PS2_REGISTER_GAME_OVERRIDE("BT3 CD read-state edge guard", "BOOT", 0u, 0u, &applyBt3CdStateEdge);
}

// [netmenu] Play one of the game's OWN SEs (system bank A) from the host menu -- no decoded WAVs.
// bank: a single-slot bitmask (1 = bank A, the 8-sample system set with cursor/confirm/popup).
// Returns 1 if the bank was captured and a voice was queued, 0 otherwise (so the caller can warn).
extern "C" int ps2xMenuSePlay(int bank, int idx)
{
    if (!g_ps2xMenuRdram || !g_ps2xMenuRuntime) return 0;
    const uint32_t b = (bank > 0) ? (uint32_t)bank : 1u;
    if (b == 0u || (b & (b - 1u)) != 0u) return 0;
    const uint32_t slot = (uint32_t)__builtin_ctz(b);
    {
        std::lock_guard<std::mutex> lk(g_seBlobM);
        if (slot >= kSeSlots || g_seSlot[slot].hdr.empty() || g_seSlot[slot].blob.empty())
            return 0;
    }
    static std::atomic<uint32_t> s_serial{0x9000u};
    // vol 100/127, pan centre: the exact path the guest uses (sePlay -> a voice into the SE stream).
    sePlay(g_ps2xMenuRdram, g_ps2xMenuRuntime, b, (uint32_t)(idx < 0 ? 0 : idx), 100u, 64u,
           s_serial.fetch_add(1u, std::memory_order_relaxed));
    return 1;
}


