#include "runtime/ps2_memory.h"
#include "runtime/ps2_gs_gpu_renderer.h"
#include "ps2_log.h"
#include <array>

// PS2X_BONECHK diagnostic (shared with ps2_vif1_interpreter.cpp): per-kick map of
// {chainBuf byte offset, guest source address, isScratchpad} recorded during the tag
// walk, so poison floats found at UNPACK time can be attributed to the EE/SPR memory
// they came from. Valid only in sync-kick mode (async defers processing past the walk).
std::vector<std::array<uint32_t, 3>> g_kickSrcMap;
// Non-chain (plain qwc) VIF1 transfers bypass the chain map; the guest source base of the
// chunk currently being processed lives here instead (sync mode only).
uint32_t g_vif1QwcSrcGuest = 0u;
bool g_vif1QwcActive = false;
bool g_kickSrcMapEnabled()
{
    static const bool s_on = [](){ const char *v = std::getenv("PS2X_BONECHK"); if (v && v[0] && v[0] != '0') return true;
                                   const char *m = std::getenv("PS2X_MVPCHK"); if (m && m[0] && m[0] != '0') return true;
                                   const char *k = std::getenv("PS2X_KICKHIST"); return k && k[0] && k[0] != '0'; }();
    return s_on;
}
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include "ps2_compat.h"
#include <string>
#include <vector>
#include <map>
#if !defined(_WIN32)
#include <execinfo.h> // glibc backtrace for the PS2X_WATCH debug probe
#endif
#include <cstdio>
#include <cstdlib>

// PS2X_WATCH: when the projection value 0x44db523d (1754.57) is written into a transform packet,
// print a backtrace to reveal the recompiled EE function that builds the packet (and therefore
// where the zero world matrix should have been written). Robust to the packet's address changing.
static inline void ps2xWatchStore(uint32_t address, const void *bytes, uint32_t n)
{
    static const bool s_w = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_WATCH"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
    if (!s_w) return;
    const uint32_t va = address & 0x1FFFFFFFu; // physical
    // Watch region: PS2X_WATCH_LO/PS2X_WATCH_HI (hex guest addrs) override the legacy
    // defaults (transform-packet region + the projection-constant value trigger).
    static const uint32_t s_lo = [](){ const char *v = std::getenv("PS2X_WATCH_LO"); return v ? (uint32_t)std::strtoul(v, nullptr, 16) : 0x007c1900u; }();
    static const uint32_t s_hi = [](){ const char *v = std::getenv("PS2X_WATCH_HI"); return v ? (uint32_t)std::strtoul(v, nullptr, 16) : 0x007c1a00u; }();
    static const bool s_custom = std::getenv("PS2X_WATCH_LO") != nullptr;
    bool hit = (va >= s_lo && va < s_hi);
    if (!hit && !s_custom) { const uint32_t *w = reinterpret_cast<const uint32_t *>(bytes);
        for (uint32_t i = 0; i < n / 4u; ++i) if (w[i] == 0x44db523du) { hit = true; break; } }
    if (!hit) return;
    static const uint32_t s_frMin = [](){ const char *v = std::getenv("PS2X_WATCH_FRMIN"); return v ? (uint32_t)std::strtoul(v, nullptr, 0) : 0u; }();
    if (s_frMin) { extern std::atomic<uint64_t> g_bt3FrameCount; if (g_bt3FrameCount.load(std::memory_order_relaxed) < s_frMin) return; }
    static int s_n = 0; if (s_n++ >= 120) return;
    const uint32_t *w = reinterpret_cast<const uint32_t *>(bytes);
    std::fprintf(stderr, "[watch] store EEva=0x%08x n=%u val0=0x%08x -- backtrace:\n", address, n, w[0]);
#if !defined(_WIN32)
    void *bt[24]; int m = backtrace(bt, 24);
    char **sy = backtrace_symbols(bt, m);
    if (sy) { for (int i = 0; i < m && i < 10; ++i) std::fprintf(stderr, "   %s\n", sy[i]); std::free(sy); }
#endif
}

// Guest write-watch controls (defined in ps2_runtime.cpp; hooked into ps2TraceGuestWrite).
// The PS2X_ZEROTAG probe below arms them at DMA time on a zero-matrix packet's address.
extern std::atomic<uint32_t> g_ps2WatchLo;
extern std::atomic<uint32_t> g_ps2WatchHi;
extern std::atomic<uint32_t> g_ps2WatchAll;

namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return addr >= PS2_GS_PRIV_REG_BASE && addr < PS2_GS_PRIV_REG_BASE + PS2_GS_PRIV_REG_SIZE;
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x3u;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x3ull;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr uint32_t kEeTimer0Count = 0x10000000u;
    constexpr uint32_t kEeTimer0Mode = 0x10000010u;
    constexpr uint32_t kEeTimer0Compare = 0x10000020u;
    constexpr uint32_t kEeTimer0Hold = 0x10000030u;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint64_t kEeTimer0TicksPerSecond = 15720ull;
    constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;

    // Proper stateful EE Timer1/2/3 emulation (Timer0 has its own path above). The count
    // advances at the MODE-selected clock and — crucially — RESETS when the guest writes
    // COUNT/MODE, so "reset COUNT; wait until COUNT>=N" timer-delay loops (BT3's ~3fps
    // Timer2 spin, sub_002BAAF8) complete at the right time instead of free-running.
    // rates[mode&3]: BUSCLK, BUSCLK/16, BUSCLK/256, HBLANK.
    constexpr uint64_t kEeTimerClkRates[4] = {147456000ull, 9216000ull, 576000ull, 15734ull};
    uint64_t g_eeTimerLastNs[4] = {0, 0, 0, 0};
    uint64_t g_eeTimerFracNs[4] = {0, 0, 0, 0};
    inline uint32_t eeTimerIndexForBase(uint32_t base) // 0x10000000+idx*0x800 -> idx
    {
        return (base - 0x10000000u) / 0x800u;
    }

    inline bool isEeTimer0Register(uint32_t address)
    {
        return address == kEeTimer0Count ||
               address == kEeTimer0Mode ||
               address == kEeTimer0Compare ||
               address == kEeTimer0Hold;
    }

    inline uint64_t steadyClockNs()
    {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    {
        std::unique_lock<std::mutex> lk(m_kickMtx);
        if (m_kickThreadStarted)
        {
            m_kickStop = true;
            m_kickCv.notify_all();
        }
    }
    if (m_kickThread.joinable())
        m_kickThread.join();

    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    m_timer0LastHostNs = 0;
    m_timer0FractionNs = 0;

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);
        ps2SetRdramHostPtr(m_rdram);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::raiseEeTimerInterruptFlag(uint32_t intcCause)
{
    if (intcCause < 9u || intcCause > 12u)
    {
        return;
    }
    // EE timer MODE registers: T0=0x10000010, T1=0x10000810, T2=0x10001010, T3=0x10001810.
    const uint32_t modeAddr = 0x10000010u + (intcCause - 9u) * 0x800u;
    const bool configured = m_ioRegisters.count(modeAddr) != 0u;
    // Set CMPE (bit 10) and OVFF (bit 11) interrupt-status flags.
    m_ioRegisters[modeAddr] |= (1u << 10) | (1u << 11);
    if (intcCause == 11u)
    {
        static std::atomic<uint32_t> s_dbg{0};
        if (s_dbg.fetch_add(1) < 5u)
            std::cerr << "[timer2-flag] modeAddr=0x" << std::hex << modeAddr
                      << " wasConfigured=" << std::dec << (configured ? 1 : 0)
                      << " modeVal=0x" << std::hex << m_ioRegisters[modeAddr] << std::dec << std::endl;
    }
}

void PS2Memory::updateEeTimer0Counter()
{
    const uint64_t nowNs = steadyClockNs();
    if (m_timer0LastHostNs == 0u)
    {
        m_timer0LastHostNs = nowNs;
        return;
    }

    const uint32_t mode = m_ioRegisters.count(kEeTimer0Mode) ? m_ioRegisters[kEeTimer0Mode] : 0u;
    if ((mode & kEeTimerModeCue) == 0u)
    {
        m_timer0LastHostNs = nowNs;
        m_timer0FractionNs = 0u;
        return;
    }

    const uint64_t elapsedNs = nowNs - m_timer0LastHostNs;
    m_timer0LastHostNs = nowNs;
    if (elapsedNs == 0u)
    {
        return;
    }

    const uint64_t scaled = elapsedNs * kEeTimer0TicksPerSecond + m_timer0FractionNs;
    const uint64_t ticks = scaled / kNanosecondsPerSecond;
    m_timer0FractionNs = scaled % kNanosecondsPerSecond;
    if (ticks != 0u)
    {
        m_ioRegisters[kEeTimer0Count] = m_ioRegisters[kEeTimer0Count] + static_cast<uint32_t>(ticks);
    }
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    // Diagnostic (PS2X_VUWRITE_DUMP): this non-const overload is the WRITE path. Log EE
    // writes into VU1 DATA memory (0x1100C000..0x1100FFFF) so we can see whether the game
    // ever direct-stores the model-view matrix into VU1 mem (esp. qw0-3 = offset 0..63).
    {
        static const bool s_vw = [](){ static const char *s_env = std::getenv("PS2X_VUWRITE_DUMP"); return s_env; }() != nullptr;
        if (s_vw && physAddr >= PS2_VU1_DATA_BASE && physAddr < (PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE))
        {
            static std::atomic<uint32_t> s_n{0};
            uint32_t off = physAddr - PS2_VU1_DATA_BASE;
            if ((s_n.fetch_add(1) % 2000u) < 8u)
                std::cerr << "[vuwrite] off=" << off << " qw=" << (off / 16u) << " size=" << size << std::endl;
        }
    }
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (virtualAddress >= 0x20000000u && virtualAddress < 0x40000000u)
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (virtualAddress >= 0x80000000 && virtualAddress < 0xC0000000)
    {
        return virtualAddress & 0x1FFFFFFF;
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (virtualAddress >= 0xC0000000)
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = gs_regs.csr.load();
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return gs_regs.csr.load();
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (address >= PS2_IO_BASE && address < (PS2_IO_BASE + PS2_IO_SIZE))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            return;
        }
    }
    if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            return;
        }
    }
    if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            const bool changed = (newVal != *reg);
            *reg = newVal;
            // DISPFB1 (offset 0x0070) changed => the game swapped display buffers,
            // i.e. the just-rendered frame is complete. Snapshot it now.
            if (regOff == 0x0070u && changed && m_displaySwapCallback)
            {
                m_displaySwapCallback();
            }
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
        ps2xWatchStore(address, &value, 4);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            return;
        }
    }
    if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
        ps2xWatchStore(address, &value, 8);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            return;
        }
    }
    if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
        { alignas(16) uint8_t vb[16]; _mm_store_si128(reinterpret_cast<__m128i *>(vb), value); ps2xWatchStore(address, vb, 16); }
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            return;
        }
    }
    if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    if (isEeTimer0Register(address))
    {
        if (address == kEeTimer0Count)
        {
            m_ioRegisters[address] = value;
            m_timer0LastHostNs = steadyClockNs();
            m_timer0FractionNs = 0u;
            return true;
        }

        updateEeTimer0Counter();
        m_ioRegisters[address] = value;
        m_timer0LastHostNs = steadyClockNs();
        if (address == kEeTimer0Mode)
        {
            m_timer0FractionNs = 0u;
        }
        return true;
    }

    // EE Timer1/2/3 COUNT (base) or MODE (base+0x10) write: store it and reset the
    // stateful counter base so "reset COUNT; wait until COUNT>=N" delay loops count from
    // the write instead of free-running (BT3's Timer2 ~3fps spin).
    if (address == 0x10000800u || address == 0x10001000u || address == 0x10001800u ||
        address == 0x10000810u || address == 0x10001010u || address == 0x10001810u)
    {
        // T_MODE (base+0x10): bits 10 (EQUF) and 11 (OVFF) are write-1-to-clear status
        // flags set by hardware. Storing the raw value would wrongly set/keep them; instead
        // clear only the flags the game writes 1 to and preserve the rest.
        if ((address & 0x1Fu) == 0x10u)
        {
            const uint32_t old = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
            const uint32_t keptFlags = (old & 0xC00u) & ~(value & 0xC00u);
            value = (value & ~0xC00u) | keptFlags;
        }
        m_ioRegisters[address] = value;
        const uint32_t idx = eeTimerIndexForBase(address & ~0x1Fu);
        g_eeTimerLastNs[idx] = steadyClockNs();
        g_eeTimerFracNs[idx] = 0u;
        return true;
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);

            // DIAGNOSTIC: per-channel DMA-start histogram + gsVRAM gate state, so
            // we can see whether the game ever submits to VIF1/GIF (graphics) and
            // whether m_gsVRAM is dropping those transfers.
            static const bool s_dmaProbe = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_DMA_PROBE"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
            if (s_dmaProbe)
            {
                static std::atomic<uint32_t> s_ch[8]{};
                const uint32_t idx = (channelBase >= 0x10008000u && channelBase < 0x10010000u) ? ((channelBase >> 12) & 7u) : 0u;
                uint32_t n = s_ch[idx].fetch_add(1) + 1u;
                if ((n % 64u) == 1u)
                {
                    std::cerr << "[dma] ch=0x" << std::hex << channelBase << std::dec
                              << " n=" << n << " qwc=" << qwc
                              << " mode=" << ((value >> 2) & 3u)
                              << " chcr=0x" << std::hex << value << std::dec
                              << " tagAddr=0x" << std::hex << m_ioRegisters[channelBase + 0x30] << std::dec
                              << " gsVRAM=" << (m_gsVRAM ? 1 : 0)
                              << " gif=" << m_gifCopyCount.load() << " gsw=" << m_gsWriteCount.load() << std::endl;
                }
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) &&
                (m_gsVRAM || channelBase == 0x10008000u))
            {
                static const bool s_tw = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_DMAPROF"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                const auto _tw0 = s_tw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                uint32_t lastKickTags = 0;
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    // CHCR.TTE (bit 6): transfer the tag's upper 8 bytes to the peripheral as
                    // two VIF codes. We used to do this UNCONDITIONALLY for inline-payload tags;
                    // for TTE=0 chains whose tag uppers carry game bookkeeping (the fight's
                    // terrain-chunk packet builder at 0x239e00 stores heap pointers there), the
                    // injected junk shifted the VIF stream and the unpacker consumed DMA tags as
                    // matrix rows -> saturated SPS wedges/slashes over the arena.
                    const bool tteEnabled = (chcr & (1u << 6)) != 0u;
                    // [chaincap] Runaway guard on how many DMA chain tags we follow. It was
                    // 4096 and it exited SILENTLY, discarding the rest of the frame. BT3's
                    // splitscreen fight exceeds 4096 tags once the fighters separate (~4600
                    // truncations in one retreat) and the game draws its HUD as the LAST pass of
                    // the frame (measured: kicks 99.7%-99.9% through), so the dropped tail took
                    // the HUD, its CLUT uploads and the ink with it -- 0 HUD draws in 27 frames
                    // where console emits them in 273/273 frames of the same retreat. Raised to
                    // 65536 (still far above any legitimate chain) and a truncation now logs.
                    // PS2X_MAXCHAINTAGS=<n> overrides, e.g. =4096 to reproduce the old behaviour.
                    static const int kMaxChainTags = [](){ const char *v = std::getenv("PS2X_MAXCHAINTAGS");
                                                           const int n = v && v[0] ? std::atoi(v) : 0;
                                                           return (n > 0) ? n : 65536; }();
                    // Pre-size to the recent high-water mark: the chain assembly appends ~430
                    // tag payloads/kick and, starting from an empty vector, reallocated ~20x
                    // per kick (each realloc copies the whole growing buffer + malloc/free) --
                    // ~357ms/s for only 32MB/s of data (~50x memcpy). Reserving once collapses
                    // that to a single right-sized allocation and the raw copies.
                    static std::atomic<uint32_t> s_chainHint{0};
                    std::vector<uint8_t> chainBuf;
                    // PS2X_BONECHK source map: chainBuf offset -> guest source address, so the
                    // VIF interpreter can attribute poison floats to the EE/SPR memory they were
                    // copied from. Sync-kick mode only (async processes the copy later).
                    if (g_kickSrcMapEnabled())
                        g_kickSrcMap.clear();
                    {
                        uint32_t hint = s_chainHint.load(std::memory_order_relaxed);
                        if (hint) chainBuf.reserve(hint);
                    }

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = translateAddress(srcAddr);
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        {   // [raysrc] PS2X_RAYSRC=<tbp> (2026-09-03, Kaioken white): CHAIN-path segments
                            // carrying the flash-RAY triangles -- REGLIST tag nreg=12 {PRIM,TEX0,3x[RGBAQ,ST,XYZF2],NOP}
                            // (7 qwords / triangle). Decode the three screen positions (12.4 fixed minus the
                            // XYOFFSET PS2X_RAYOFX/RAYOFY, default 1792/1824) and report them. Console rays are
                            // 10s..100s of px; ours contain triangles clipped to the guard-band CORNERS
                            // (-30,-28)..(542,474) = enormous source triangles -> full-screen additive fills = white.
                            // PS2X_RAYSRC_ARM=1 arms the guest write-watch on the XYZF2 qword of the first CLAMPED
                            // vertex so the EE writer of the next packet backtraces ([camwrite] pc/ra/stk).
                            // PS2X_RAYSRC_SCAN=1 also memmem's RAM for another copy of that vertex block (the
                            // build buffer, stable across frames) and arms THERE instead of the moving arena slot.
                            static const uint32_t s_rt = [](){ const char *v = std::getenv("PS2X_RAYSRC");
                                                               return v && v[0] ? (uint32_t)std::atoi(v) : 0u; }();
                            static const bool s_rarm = [](){ const char *v = std::getenv("PS2X_RAYSRC_ARM");
                                                             return v && v[0] && v[0] != '0'; }();
                            static const bool s_rscan = [](){ const char *v = std::getenv("PS2X_RAYSRC_SCAN");
                                                              return v && v[0] && v[0] != '0'; }();
                            static const float s_ofx = [](){ const char *v = std::getenv("PS2X_RAYOFX"); return v && v[0] ? (float)std::atof(v) : 1792.0f; }();
                            static const float s_ofy = [](){ const char *v = std::getenv("PS2X_RAYOFY"); return v && v[0] ? (float)std::atof(v) : 1824.0f; }();
                            static int s_rn = 0, s_rarmed = 0;
                            if (s_rt && src < maxSz2 && bytes <= maxSz2 - src)
                            {
                                const uint8_t *pw = base2 + src;
                                for (uint32_t off = 0; off + 16 <= bytes; off += 16)
                                {
                                    uint64_t h2[2]; std::memcpy(h2, pw + off, 16);
                                    int hit = -1;
                                    for (int k = 0; k < 2; ++k)
                                        if ((h2[k] & 0x3FFFull) == s_rt && ((h2[k] >> 20) & 0x3Full) == 19ull) hit = k;
                                    if (hit < 0) continue;
                                    const uint32_t v0 = off + 8u * (uint32_t)(hit + 1);   // first RGBAQ slot
                                    if (v0 + 72u > bytes) break;
                                    float xs[3], ys[3]; uint32_t zs[3], as[3], xyzOff[3]; float ss[3], ts[3], qs[3];
                                    bool clamped = false; int firstClamped = -1;
                                    for (int v = 0; v < 3; ++v)
                                    {
                                        uint64_t rg, st, xyz;
                                        std::memcpy(&rg, pw + v0 + v * 24u, 8); std::memcpy(&st, pw + v0 + v * 24u + 8u, 8); std::memcpy(&xyz, pw + v0 + v * 24u + 16u, 8);
                                        xyzOff[v] = v0 + v * 24u + 16u;
                                        xs[v] = (float)(xyz & 0xFFFFull) / 16.0f - s_ofx; ys[v] = (float)((xyz >> 16) & 0xFFFFull) / 16.0f - s_ofy; zs[v] = (uint32_t)((xyz >> 32) & 0xFFFFFFull);
                                        as[v] = (uint32_t)((rg >> 24) & 0xFFull);
                                        uint32_t sb = (uint32_t)(st & 0xFFFFFFFFull), tb = (uint32_t)(st >> 32), qb = (uint32_t)(rg >> 32);
                                        std::memcpy(&ss[v], &sb, 4); std::memcpy(&ts[v], &tb, 4); std::memcpy(&qs[v], &qb, 4);
                                        const bool c = xs[v] < -25.0f || xs[v] > 535.0f || ys[v] < -22.0f || ys[v] > 470.0f;
                                        if (c && firstClamped < 0) firstClamped = v;
                                        clamped = clamped || c;
                                    }
                                    // a REAL ray: all three depths non-trivial, alpha > 0, finite positive Q (stale slots
                                    // reused by other packet builders show z0/a0/NaN -- KAIO1 spent every arm on those)
                                    const bool real = zs[0] > 1000u && zs[1] > 1000u && zs[2] > 1000u && (as[0] | as[1] | as[2]) != 0u
                                                      && std::isfinite(qs[0]) && qs[0] > 0.0f && std::isfinite(ss[0]);
                                    static int s_rreal = 0;
                                    if (real) ++s_rreal;
                                    const bool show = (real && (s_rreal < 120 || (s_rreal % 40) == 0)) || (!real && s_rn < 6);
                                    if (show)
                                        std::fprintf(stderr, "[raysrc] #%d%s tri @guest 0x%08x (seg 0x%08x+%u len %u sp=%d)%s v0=(%.1f,%.1f,z%u a%u s%.4f t%.4f q%.4f) v1=(%.1f,%.1f,z%u a%u s%.4f t%.4f) v2=(%.1f,%.1f,z%u a%u s%.4f t%.4f)\n",
                                                     s_rn, real ? " REAL" : " stale", srcAddr + off, srcAddr, off, bytes, scratch ? 1 : 0, clamped ? " CLAMPED" : "",
                                                     xs[0], ys[0], zs[0], as[0], ss[0], ts[0], qs[0], xs[1], ys[1], zs[1], as[1], ss[1], ts[1], xs[2], ys[2], zs[2], as[2], ss[2], ts[2]);
                                    if (real && clamped)
                                    {   // arm the FUN_00132b60 arg logger ([rayhook]) for the next 400 quad emits
                                        extern std::atomic<int> g_rayHookArm;
                                        static bool s_hookArmed = false;
                                        if (!s_hookArmed) { s_hookArmed = true; g_rayHookArm.store(400, std::memory_order_relaxed); }
                                    }
                                    if (s_rarm && clamped && real && !scratch && s_rarmed < 6)
                                    {
                                        uint32_t armAt = srcAddr + xyzOff[firstClamped];
                                        if (s_rscan)
                                        {   // another copy of this vertex block (RGBAQ..XYZF2 of the clamped vertex = 24 B, plus the next 8 B)
                                            const uint8_t *needle = pw + v0 + firstClamped * 24u; const size_t nl = 32;
                                            const uint8_t *cur = m_rdram; const uint8_t *end = m_rdram + PS2_RAM_SIZE; bool found = false;
                                            while (cur < end)
                                            {
                                                const uint8_t *f = (const uint8_t *)memmem(cur, (size_t)(end - cur), needle, nl);
                                                if (!f) break;
                                                const uint32_t ga = (uint32_t)(f - m_rdram);
                                                if (ga < (srcAddr & 0x1FFFFFFFu) || ga >= (srcAddr & 0x1FFFFFFFu) + bytes)
                                                { std::fprintf(stderr, "[raysrc] SCAN copy of clamped vertex found at guest 0x%08x (arena slot 0x%08x)\n", ga, srcAddr + v0 + firstClamped * 24u); armAt = ga + 16u; found = true; break; }
                                                cur = f + 1;
                                            }
                                            if (!found) std::fprintf(stderr, "[raysrc] SCAN no other copy of the vertex block in RAM\n");
                                        }
                                        const uint32_t a0 = (armAt & 0x1FFFFFFFu) & ~0xFu;
                                        g_ps2WatchAll.store(1u, std::memory_order_relaxed);
                                        g_ps2WatchHi.store(a0 + 16u, std::memory_order_relaxed);
                                        g_ps2WatchLo.store(a0, std::memory_order_relaxed);
                                        ++s_rarmed;
                                        std::fprintf(stderr, "[raysrc] write-watch ARMED #%d on qword 0x%08x..0x%08x (XYZF2 of v%d at 0x%08x)\n", s_rarmed, a0, a0 + 16u, firstClamped, armAt);
                                    }
                                    ++s_rn;
                                    off += 16u * 5u;   // skip the rest of this triangle's data (6 data qwords total)
                                }
                            }
                        }
                        {   // [wispsrc] PS2X_WISPSRC=<tbp>: CHAIN-path segments that bind a PSMT8 64x64
                            // texture at this TBP0 (the aura wisps, REGLIST packets: the TEX0 sits in
                            // either 64-bit half). Then the first REGLIST RGBAQ slot whose colour bytes
                            // equal PS2X_WISPRGB (default 0xA95A92) -- its alpha byte (+3) is the value
                            // that reaches the GS as 0 (2026-09-03). PS2X_WISPSRC_ARM=1 arms the guest
                            // write-watch on that byte so the EE writer of the next frame backtraces.
                            static const uint32_t s_wt = [](){ const char *v = std::getenv("PS2X_WISPSRC");
                                                               return v && v[0] ? (uint32_t)std::atoi(v) : 0u; }();
                            static const uint32_t s_wrgb = [](){ const char *v = std::getenv("PS2X_WISPRGB");
                                                                 return v && v[0] ? (uint32_t)std::strtoul(v, nullptr, 0) : 0xA95A92u; }();
                            static const bool s_warm = [](){ const char *v = std::getenv("PS2X_WISPSRC_ARM");
                                                             return v && v[0] && v[0] != '0'; }();
                            static int s_wn = 0;
                            if (s_wt && src < maxSz2 && bytes <= maxSz2 - src)
                            {
                                const uint8_t *pw = base2 + src;
                                // REGLIST RGBAQ slot: R bits 0-7, G 8-15, B 16-23, A 24-31 -- the low 24 bits
                                // read as an integer are exactly gsparse's rgbaq&0xFFFFFF (0xA95A92 here).
                                const uint32_t rgbLe = s_wrgb;
                                for (uint32_t off = 0; off + 16 <= bytes; off += 16)
                                {
                                    uint64_t h2[2]; std::memcpy(h2, pw + off, 16);
                                    int hit = -1;
                                    for (int k = 0; k < 2; ++k)
                                        if ((h2[k] & 0x3FFFull) == s_wt && ((h2[k] >> 20) & 0x3Full) == 19ull && ((h2[k] >> 26) & 0xFull) == 6ull) hit = k;
                                    if (hit < 0) continue;
                                    uint32_t aAddr = 0u, aVal = 0u;
                                    for (uint32_t o2 = off; o2 + 16 <= bytes && o2 < off + 16u * 64u && !aAddr; o2 += 16)
                                    {
                                        uint64_t v2[2]; std::memcpy(v2, pw + o2, 16);
                                        for (int k = 0; k < 2; ++k)
                                            if ((uint32_t)(v2[k] & 0xFFFFFFull) == rgbLe) { aAddr = srcAddr + o2 + (uint32_t)k * 8u + 3u; aVal = (uint32_t)((v2[k] >> 24) & 0xFFull); break; }
                                    }
                                    const bool show = s_wn < 20 || (s_wn % 30) == 0;   // unbounded detections, rate-limited prints
                                    if (show)
                                        std::fprintf(stderr, "[wispsrc] #%d TEX0 tbp=%u (%s half) at guest 0x%08x (seg 0x%08x+%u len %u scratch=%d) alphaByte@0x%08x=%u\n",
                                                     s_wn, s_wt, hit ? "hi" : "lo", srcAddr + off, srcAddr, off, bytes, scratch ? 1 : 0, aAddr, aVal);
                                    // [wispsrc] BUILD-BUFFER SCAN (PS2X_WISPSRC_SCAN=1): the arena copy is
                                    // memcpy'd from wherever the EE builds the quad, so search all of guest
                                    // RAM for another copy of this quad's first 64 bytes (tag + PRIM/TEX0 +
                                    // 2 register pairs, unique per frame) and arm the watch THERE instead --
                                    // that is where the vertex alpha is written.
                                    static const bool s_wscan = [](){ const char *v = std::getenv("PS2X_WISPSRC_SCAN");
                                                                      return v && v[0] && v[0] != '0'; }();
                                    static int s_scanN = 0;
                                    uint32_t armAt = aAddr;
                                    if (s_wscan && s_scanN < 6 && off >= 16u && off + 64u <= bytes && !scratch)
                                    {
                                        const uint8_t *needle = pw + off - 16u;
                                        const uint8_t *hay = base2; const size_t hayN = maxSz2;
                                        int found = 0;
                                        for (size_t q = 0; q + 64u <= hayN; q += 16)
                                        {
                                            if (hay + q == needle) continue;
                                            if (std::memcmp(hay + q, needle, 64) != 0) continue;
                                            ++found;
                                            const uint32_t addr = (uint32_t)q;
                                            std::fprintf(stderr, "[wispsrc] SCAN copy of quad @0x%08x found at guest 0x%08x (RGBAQ slot 0x%08x)\n",
                                                         srcAddr + off - 16u, addr, addr + 48u);
                                            if (found == 1 && addr + 48u + 3u == (uint32_t)(addr + 51u)) armAt = addr + 48u + 3u;
                                            if (found >= 4) break;
                                        }
                                        if (!found) std::fprintf(stderr, "[wispsrc] SCAN no other copy of the quad in RAM (built in place or in a dead stack frame)\n");
                                        ++s_scanN;
                                    }
                                    // RE-ARM on every detection: the DMA arena is a ring, so the segment
                                    // moves every frame; the watch must follow the CURRENT slot (or the
                                    // build buffer found by the scan, which is stable).
                                    if (s_warm && armAt && !scratch && !(s_wscan && armAt == aAddr && s_scanN > 1 && g_ps2WatchLo.load(std::memory_order_relaxed) != 0u))
                                    {
                                        aAddr = armAt;
                                        // The per-store check keys on the store's START address, so a
                                        // 64/128-bit store of the whole RGBAQ slot/qword would miss a
                                        // 1-byte window: watch the containing 16-byte qword instead.
                                        const uint32_t a0 = (aAddr & 0x1FFFFFFFu) & ~0xFu;
                                        g_ps2WatchAll.store(1u, std::memory_order_relaxed);   // keep ZERO-valued writes (the alpha IS 0)
                                        g_ps2WatchHi.store(a0 + 16u, std::memory_order_relaxed);
                                        g_ps2WatchLo.store(a0, std::memory_order_relaxed);
                                        if (show) std::fprintf(stderr, "[wispsrc] write-watch ARMED on qword 0x%08x..0x%08x (alpha byte 0x%08x)\n", a0, a0 + 16u, aAddr);
                                    }
                                    ++s_wn; break;   // one arm per segment (its first quad)
                                }
                            }
                        }
                        {   // [palsrc] PS2X_PALSRC=<dbp>: the CHAIN path (the one live BT3 uses).
                            // Scan this tag segment for a BITBLTBUF write to the terrain-palette
                            // block, log its guest address, and (PS2X_PALSRC_ARM=1) arm the guest
                            // write-watch on the segment so next frame's rebuild backtraces the
                            // EE builder of the unstable sun-lighting groups.
                            static const uint32_t s_pd2 = [](){ const char *v = std::getenv("PS2X_PALSRC");
                                                                return v && v[0] ? (uint32_t)std::atoi(v) : 0u; }();
                            static int s_pn2 = 0;   // raised cap: slot-address logging needs many frames
                            static int s_expectPayload = 0;   // [palsrc] header seen; the NEXT big segment is the palette bytes
                            // PS2X_PALSRC_GEO=1: skip the palette payload and arm on the NEXT
                            // BIG segment instead -- the group's GEOMETRY batch, rebuilt per frame
                            // by the membership builder we want to backtrace.
                            static const bool s_geo = [](){ const char *v = std::getenv("PS2X_PALSRC_GEO");
                                                            return v && v[0] && v[0] != '0'; }();
                            // PS2X_PALSRC_GEO=2: the RAM batches proved static (PALG3: zero writes,
                            // full store tracing). The per-frame membership data is suspected SPR-resident
                            // (chain reads scratchpad directly via SPR-flagged tags; StoreN traces SPR
                            // writes as 0x10000000+off). Log EVERY segment after a terrain header with
                            // its scratch flag, and latch+re-assert the watch on the FIRST SPR segment.
                            static const bool s_geo2 = [](){ const char *v = std::getenv("PS2X_PALSRC_GEO");
                                                             return v && (v[0] == '2' || v[0] == '3' || v[0] == '5'); }();
                            static const bool s_geo3 = [](){ const char *v = std::getenv("PS2X_PALSRC_GEO");
                                                             return v && v[0] == '3'; }();
                            if (s_pd2 && s_geo2 && s_expectPayload > 0)
                            {
                                static int s_sn = 0;
                                if (s_sn < 400)
                                { std::fprintf(stderr, "[palsrc] SEG scratch=%d addr=0x%08x len %u\n", scratch?1:0, srcAddr, bytes); ++s_sn; }
                                --s_expectPayload;
                                static const bool s_armS = [](){ const char *v = std::getenv("PS2X_PALSRC_ARM");
                                                                 return v && v[0] && v[0] != '0'; }();
                                static uint32_t s_sprLo = 0, s_sprHi = 0;
                                // mode 5: after modes 3/4 proved chunk lists AND DL headers static,
                                // the last unwatched terrain-chain class = the small 512..1023B
                                // segments (0x0104f290 len 992, 0x01050ad0 len 704) — the natural
                                // per-frame UNIFORM blocks for the terrain VU1 micro. Arm there.
                                static const bool s_geo5 = [](){ const char *v = std::getenv("PS2X_PALSRC_GEO");
                                                                 return v && v[0] == '5'; }();
                                if (s_geo5 && s_armS && !scratch && bytes >= 512u && bytes < 1024u && src < maxSz2)
                                {
                                    static uint32_t s_uLo = 0, s_uHi = 0;
                                    if (s_uLo == 0u)
                                    {
                                        s_uLo = src & 0x1FFFFFFFu;
                                        s_uHi = (src + bytes) & 0x1FFFFFFFu;
                                        std::fprintf(stderr, "[palsrc] write-watch ARMED on UNIFORM seg 0x%08x..0x%08x len %u\n", s_uLo, s_uHi, bytes);
                                    }
                                    g_ps2WatchHi.store(s_uHi, std::memory_order_relaxed);
                                    g_ps2WatchLo.store(s_uLo, std::memory_order_relaxed);
                                }
                                // mode 3: PALG4 census showed ZERO scratch segments; the unwatched
                                // size class = the ~22 per-group MID-SIZE chunk-list segments
                                // (1K..24K RAM). Arm the latched watch on the first 2K..32K one.
                                if (s_geo3 && s_armS && !scratch && bytes >= 2048u && bytes < 32768u && src < maxSz2)
                                {
                                    static uint32_t s_midLo = 0, s_midHi = 0;
                                    if (s_midLo == 0u)
                                    {
                                        const uint32_t alen = bytes < 1024u ? bytes : 1024u;
                                        s_midLo = src & 0x1FFFFFFFu;
                                        s_midHi = (src + alen) & 0x1FFFFFFFu;
                                        std::fprintf(stderr, "[palsrc] write-watch ARMED on MIDSEG 0x%08x..0x%08x len %u\n", s_midLo, s_midHi, bytes);
                                    }
                                    g_ps2WatchHi.store(s_midHi, std::memory_order_relaxed);
                                    g_ps2WatchLo.store(s_midLo, std::memory_order_relaxed);
                                }
                                if (s_armS && scratch && src < maxSz2)
                                {
                                    if (s_sprLo == 0u)
                                    {
                                        const uint32_t alen = bytes < 1024u ? bytes : 1024u;
                                        s_sprLo = 0x10000000u + src;
                                        s_sprHi = 0x10000000u + src + alen;
                                        std::fprintf(stderr, "[palsrc] write-watch ARMED on SPR seg 0x%08x..0x%08x (spr off 0x%x)\n", s_sprLo, s_sprHi, src);
                                    }
                                    g_ps2WatchHi.store(s_sprHi, std::memory_order_relaxed);
                                    g_ps2WatchLo.store(s_sprLo, std::memory_order_relaxed);
                                }
                            }
                            // mode 6: the cull test's per-frame UNIFORMS (camera matrix/offset,
                            // VU mem rows 0..15) arrive via an UNPACK V4-32 in a chain segment
                            // never covered by modes 2-5. Scan payloads near the terrain section
                            // for VIF `UNPACK V4-32 addr<16` codes, log them, and latch the watch
                            // on the first such payload -> the EE uniform builder's pc/ra.
                            static const bool s_geo6 = [](){ const char *v = std::getenv("PS2X_PALSRC_GEO");
                                                             return v && v[0] == '6'; }();
                            if (s_pd2 && s_geo6 && s_pn2 > 0 && !scratch && src < maxSz2 && bytes >= 8u && bytes <= maxSz2 - src)
                            {
                                const uint8_t *pp = base2 + src;
                                for (uint32_t off = 0; off + 8 <= bytes && off < 4096u; off += 4)
                                {
                                    uint32_t w; std::memcpy(&w, pp + off, 4);
                                    // 6b: num>=4 — the ubiquitous num=3 cell-bbox headers crowded out
                                    // the log AND stole the arm; the matrix/uniform block is num 4..12.
                                    // 6d: ANY unpack format (cmd 0x60-0x7F), addr<16, num>=4 — V4-32
                                    // never carries the uniform rows; the camera block must ride
                                    // V3-32/V4-16/etc. Log format byte too.
                                    if ((w & 0xE0000000u) == 0x60000000u && (w & 0x3FFu) < 16u && ((w >> 16) & 0xFFu) >= 4u)
                                    {
                                        static int s_un = 0;
                                        const uint32_t num = (w >> 16) & 0xFFu, ad = w & 0x3FFu;
                                        if (s_un < 80)
                                        { std::fprintf(stderr, "[palsrc] UNPACK cmd=%02x num=%u addr=%u flg=%u at guest 0x%08x (payload 0x%08x)\n", (w >> 24) & 0xFFu, num, ad, (w >> 15) & 1u, srcAddr + off, srcAddr + off + 4); ++s_un; }
                                        static const bool s_armU = [](){ const char *v = std::getenv("PS2X_PALSRC_ARM");
                                                                         return v && v[0] && v[0] != '0'; }();
                                        static uint32_t s_u6Lo = 0, s_u6Hi = 0;
                                        if (s_armU && ad < 16u && (w & 0xFF000000u) != 0x6C000000u && (w & 0xFF000000u) != 0x7C000000u)
                                        {
                                            if (s_u6Lo == 0u)
                                            {
                                                s_u6Lo = (src + off + 4u) & 0x1FFFFFFFu;
                                                s_u6Hi = (src + off + 4u + num * 16u) & 0x1FFFFFFFu;
                                                std::fprintf(stderr, "[palsrc] write-watch ARMED on UNPACK payload 0x%08x..0x%08x\n", s_u6Lo, s_u6Hi);
                                            }
                                            g_ps2WatchHi.store(s_u6Hi, std::memory_order_relaxed);
                                            g_ps2WatchLo.store(s_u6Lo, std::memory_order_relaxed);
                                        }
                                        break;
                                    }
                                }
                            }
                            if (s_pd2 && s_geo && s_expectPayload > 0 && !scratch && bytes >= 32768u && src < maxSz2)
                            {
                                s_expectPayload = 0;
                                static int s_gn = 0;
                                if (s_gn < 40) { std::fprintf(stderr, "[palsrc] GEOMETRY seg at guest 0x%08x len %u\n", srcAddr, bytes); ++s_gn; }
                                static const bool s_armG = [](){ const char *v = std::getenv("PS2X_PALSRC_ARM");
                                                                 return v && v[0] && v[0] != '0'; }();
                                // Latch the FIRST batch window and RE-ASSERT it on every terrain header:
                                // scoped probes (thunk/camera watches) and the AWATCH init junk-arm can
                                // hold/steal the global watch; unconditional re-assert keeps ours live.
                                static uint32_t s_geoLo = 0, s_geoHi = 0;
                                if (s_armG)
                                {
                                    if (s_geoLo == 0u)
                                    {
                                        const uint32_t alen = bytes < 4096u ? bytes : 4096u;
                                        s_geoLo = src & 0x1FFFFFFFu;
                                        s_geoHi = (src + alen) & 0x1FFFFFFFu;
                                        std::fprintf(stderr, "[palsrc] write-watch ARMED on GEOMETRY 0x%08x..0x%08x\n", src, src + alen);
                                    }
                                    g_ps2WatchHi.store(s_geoHi, std::memory_order_relaxed);
                                    g_ps2WatchLo.store(s_geoLo, std::memory_order_relaxed);
                                }
                            }
                            if (s_pd2 && !s_geo && s_expectPayload > 0 && !scratch && bytes >= 1024u && src < maxSz2)
                            {
                                s_expectPayload = 0;
                                // Slot-indexed payload-address log: if the ADDRESS a slot references
                                // changes frame-to-frame, the game picks between STATIC light tables
                                // and the chooser writes the display list, not the palette bytes.
                                static int s_slot = 0; static uint32_t s_lastFirst = 0; static int s_pl = 0;
                                if (s_pl < 600)
                                {
                                    if (s_lastFirst == 0) s_lastFirst = srcAddr;
                                    // heuristic frame boundary: slot counter resets when we see the FIRST slot's addr class again
                                    std::fprintf(stderr, "[palslot] %d 0x%08x len %u\n", s_slot++, srcAddr, bytes);
                                    ++s_pl;
                                }
                                std::fprintf(stderr, "[palsrc] PAYLOAD at guest 0x%08x len %u\n", srcAddr, bytes);
                                static const bool s_armP = [](){ const char *v = std::getenv("PS2X_PALSRC_ARM");
                                                                 return v && v[0] && v[0] != '0'; }();
                                if (s_armP && g_ps2WatchLo.load(std::memory_order_relaxed) == 0u)
                                {
                                    const uint32_t alen = bytes < 1024u ? bytes : 1024u;
                                    g_ps2WatchHi.store((src + alen) & 0x1FFFFFFFu, std::memory_order_relaxed);
                                    g_ps2WatchLo.store(src & 0x1FFFFFFFu, std::memory_order_relaxed);
                                    std::fprintf(stderr, "[palsrc] write-watch ARMED on PAYLOAD phys 0x%08x..0x%08x\n", src, src + alen);
                                }
                            }
                            if (s_pd2 && s_pn2 < 700 && !scratch && src < maxSz2 && bytes <= maxSz2 - src)
                            {
                                const uint8_t *p2 = base2 + src;
                                for (uint32_t off = 0; off + 16 <= bytes; off += 16)
                                {
                                    uint64_t lo, hi;
                                    std::memcpy(&lo, p2 + off, 8); std::memcpy(&hi, p2 + off + 8, 8);
                                    if ((hi & 0xFFull) == 0x50ull && ((lo >> 32) & 0x3FFFull) == s_pd2)
                                    {
                                        if (s_pn2 < 40)
                                            std::fprintf(stderr, "[palsrc] #%d BITBLTBUF dbp=%u at guest 0x%08x (seg 0x%08x+%u len %u ch=%08x)\n",
                                                         s_pn2, s_pd2, srcAddr + off, srcAddr, off, bytes, channelBase);
                                        s_expectPayload = 8;   // mode2 logs several following segments; modes 0/1 consume it on the first match
                                        // mode 4: watch the terrain DL HEADER SLOT REGION itself --
                                        // every payload layer (palettes/tables/meshes/chunk lists)
                                        // proved static; if membership lives in the DL ref-tags,
                                        // their per-frame rewrite hits here. Armed at fight time
                                        // (first terrain header), 1KB window, latched+re-asserted.
                                        static const bool s_geo4 = [](){ const char *v = std::getenv("PS2X_PALSRC_GEO");
                                                                         return v && v[0] == '4'; }();
                                        static const bool s_armH = [](){ const char *v = std::getenv("PS2X_PALSRC_ARM");
                                                                         return v && v[0] && v[0] != '0'; }();
                                        if (s_geo4 && s_armH)
                                        {
                                            static uint32_t s_hdrLo = 0, s_hdrHi = 0;
                                            if (s_hdrLo == 0u)
                                            {
                                                s_hdrLo = srcAddr & 0x1FFFFFFFu;
                                                s_hdrHi = (srcAddr + 0x400u) & 0x1FFFFFFFu;
                                                std::fprintf(stderr, "[palsrc] write-watch ARMED on DL HEADER region 0x%08x..0x%08x\n", s_hdrLo, s_hdrHi);
                                            }
                                            g_ps2WatchHi.store(s_hdrHi, std::memory_order_relaxed);
                                            g_ps2WatchLo.store(s_hdrLo, std::memory_order_relaxed);
                                        }
                                        if (++s_pn2 >= 40) break;
                                    }
                                }
                            }
                        }

                        // PS2X_ZEROSRC: the collapsed characters' bone data arrives at VU1 as ZEROS.
                        // Find the guest source: log VIF1 ref payloads that are entirely zero (first
                        // 128B) with their EE address. Histogram by 64KB region so one PCSX2 write-bp
                        // finds the EE writer that our build is missing.
                        if (channelBase == 0x10009000u && qwCount >= 8u)
                        {
                            static const bool s_zs = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_ZEROSRC"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                            if (s_zs && src < maxSz2 && !scratch)
                            {
                                // Full-payload scan: leading-zero prefix + total zero fraction.
                                // (The old first-128B check called PARTIALLY-filled packets "zero" —
                                // bone matrices stored at +0xE0 while the palette head stays empty.)
                                const uint32_t scan = std::min(bytes, maxSz2 - src);
                                uint32_t lead = 0; while (lead < scan && base2[src + lead] == 0) ++lead;
                                if (lead >= 64u && qwCount >= 30u && qwCount <= 40u)
                                {
                                    uint32_t zb = 0;
                                    for (uint32_t b = 0; b < scan; ++b) if (base2[src + b] == 0) ++zb;
                                    static uint32_t s_n = 0;
                                    if ((s_n++ % 2000u) < 4u)
                                        std::fprintf(stderr, "[zerosrc] srcAddr=0x%08x qwc=%u leadZero=0x%x zeroBytes=0x%x/0x%x\n",
                                                     srcAddr, qwCount, lead, zb, scan);
                                }
                            }
                        }

                        // PS2X_PKTFIND: locate the transform packet in EE RAM by its projection value
                        // 1754.57 (float 0x44db523d = LE bytes 3d 52 db 44). The world matrix is 128
                        // bytes (qw0-7) before it -> EE addr srcAddr+off-128. Anchors the EE builder hunt.
                        {
                            static const bool s_pf = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_PKTFIND"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                            if (s_pf && src < maxSz2) {
                                uint32_t scanBytes = std::min(bytes, maxSz2 - src);
                                const char *reg = scratch ? "SPR" : "rdram";
                                for (uint32_t o = 0; o + 4 <= scanBytes; o += 4) {
                                    float fv; std::memcpy(&fv, base2+src+o, 4);
                                    if (fv > 1754.0f && fv < 1755.0f && o >= 128u) {
                                        static int n=0; if (n++ < 10) {
                                            // dump the 4 world-matrix rows (qw0-3, the 128 bytes before proj) + proj row0
                                            uint32_t w = src + o - 128u; // world qw0
                                            float m[20];
                                            for (int i=0;i<20;i++){ if (w+(uint32_t)(i*4+4) <= maxSz2) std::memcpy(&m[i], base2+w+i*4, 4); else m[i]=0; }
                                            std::fprintf(stderr, "[pktfind] proj=%.2f %s EEva=0x%08x qwc=%u WORLD rows: w0(%.2f,%.2f,%.2f,%.2f) w1(%.2f,%.2f,%.2f,%.2f) w2(%.2f,%.2f,%.2f,%.2f) w3(%.2f,%.2f,%.2f,%.2f)\n",
                                                fv, reg, srcAddr+o-128u, qwCount, m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
                                        }
                                    }
                                }
                            }
                        }
                        if ([](){ static const char *s_env = std::getenv("PS2X_GIFSRC"); return s_env; }()) {
                            static int gh_n = 0;
                            uint32_t scanB = (src < maxSz2) ? std::min(bytes, maxSz2 - src) : 0u;
                            for (uint32_t o = 0; o + 4 <= scanB && gh_n < 40; o += 4) {
                                uint32_t w; std::memcpy(&w, base2 + src + o, 4);
                                // COLLAPSE SIGNATURE: GS XYZ2 vertex at the origin (X=0x700, Y=0x720)
                                if (w == 0x07200700u) {
                                    gh_n++;
                                    fprintf(stderr, "[collapse] VERT@origin EEsrc=0x%08x (+%u in seg, %s) qwc=%u\n",
                                        srcAddr + o, o, scratch ? "SPR" : "rdram", qwCount);
                                    continue;
                                }
                                uint32_t tbp0 = w & 0x3FFFu, psm = (w >> 20) & 0x3Fu;
                                if (psm == 19u && tbp0 == 10752u) { // the WORKING frame, for comparison
                                    gh_n++;
                                    fprintf(stderr, "[framesrc] FRAME tbp0=%u EEsrc=0x%08x (+%u in seg, %s) qwc=%u\n",
                                        tbp0, srcAddr + o, o, scratch ? "SPR" : "rdram", qwCount);
                                }
                            }
                        }
                        static const bool s_cp = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_DMAPROF"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                        const auto _c0 = s_cp ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        uint32_t copiedSoFar = 0u;
                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            if (g_kickSrcMapEnabled())
                                g_kickSrcMap.push_back({static_cast<uint32_t>(chainBuf.size()),
                                                        srcAddr + copiedSoFar,
                                                        scratch ? 1u : 0u});
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                            copiedSoFar += chunk;
                        }
                        if (s_cp)
                        {
                            static std::atomic<uint64_t> s_ns{0}, s_by{0};
                            s_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-_c0).count(), std::memory_order_relaxed);
                            s_by.fetch_add(bytes64, std::memory_order_relaxed);
                            static std::mutex s_m; static std::chrono::steady_clock::time_point s_l = std::chrono::steady_clock::now();
                            std::lock_guard<std::mutex> lk(s_m);
                            double dt = std::chrono::duration<double>(std::chrono::steady_clock::now()-s_l).count();
                            if (dt >= 1.0) { std::fprintf(stderr, "[chaincopy] %.1fms/s %lluKB/s\n", s_ns.load()/1e6/dt, (unsigned long long)(s_by.load()/1024/dt)); s_ns=0; s_by=0; s_l=std::chrono::steady_clock::now(); }
                        }
                    };

                    auto appendCompactVif1TagData = [&](uint32_t localTagAddr, uint32_t qwCount)
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr); 
                        tagPhys = translateAddress(localTagAddr);
                        
                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // VIF packet helpers embed 8 bytes of VIF stream in the DMAtag's upper half.
                        if (g_kickSrcMapEnabled())
                            g_kickSrcMap.push_back({static_cast<uint32_t>(chainBuf.size()),
                                                    localTagAddr + 8u, tagScratch ? 1u : 0u});
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                        appendData(localTagAddr + 16u, qwCount);
                    };

                    int tagsProcessed = 0;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;
                    struct ChainCapWarn {
                        const int &n; const int cap;
                        ~ChainCapWarn() {
                            if (n >= cap) {
                                static unsigned long hits = 0;
                                if ((++hits % 200ul) == 1ul)
                                    std::fprintf(stderr, "[chaincap] DMA chain TRUNCATED at %d tags "
                                                 "(hit #%lu) -- the tail of the frame was dropped\n", cap, hits);
                            }
                        }
                    } _ccw{tagsProcessed, kMaxChainTags};

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
                        // DMAtag bit 63 = SPR flag on the ADDR field: the ref/next/call target is in
                        // SCRATCHPAD (ADDR is an SPR-local offset). We previously masked this bit off
                        // and read main RAM at the bare offset -> zeros (collapsed skinned characters:
                        // BT3 builds bone matrices in SPR and refs them with SPR-flagged tags).
                        // Rewrite to the scratchpad VA so appendData/isScratchpad route to SPR.
                        // PS2X_NOSPRTAG=1: A/B kill-switch — restore the pre-2026-07-13-22:23
                        // behavior (SPR flag ignored, bare offset read from main RAM). The SPR
                        // routing landed exactly when the attract demo / grass tile stopped
                        // rendering; flip this to bisect whether it's the cause.
                        static const bool s_noSprTag = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_NOSPRTAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                        const bool tagAddrSPR = !s_noSprTag && ((tag >> 63) & 0x1ull) != 0ull;
                        if (tagAddrSPR)
                        {
                            addr = PS2_SCRATCHPAD_BASE | (addr & (PS2_SCRATCHPAD_SIZE - 1u));
                            static std::atomic<uint32_t> s_sprTag{0};
                            const uint32_t n = s_sprTag.fetch_add(1);
                            if (n < 8u || ([](){ static const char *s_env = std::getenv("PS2X_DMATAG"); return s_env; }() && (n % 20000u) < 2u))
                                std::fprintf(stderr, "[spr-tag] ch=0x%08x id=%u qwc=%u sprOff=0x%x (#%u)\n",
                                             channelBase, id, (unsigned)tagQwc, addr & (PS2_SCRATCHPAD_SIZE - 1u), n);
                        }
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;

                        // PS2X_DMATAG: log VIF1 chain tag ref addresses. Our display-list pool base is
                        // 0x6c1720; if ref addrs point elsewhere (~0x6bd740 stale), the game/chain
                        // references a wrong base -> zero uploads.
                        static const bool s_zeroTag = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_ZEROTAG"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                        if (channelBase == 0x10009000u && (s_zeroTag || [](){ static const char *s_env = std::getenv("PS2X_DMATAG"); return s_env; }())) {
                            // Detect MATRIX PACKETS anywhere in the chain: cnt tag whose inline data
                            // starts with VIF FLUSH 0x10000000 + UNPACK 0x6c0c0000 (V4-32, 12 qw -> qw0).
                            // These are the sub_00123600 packets (qwc=13 — hidden from the old qwc>=30
                            // filter). Log the matrix qw0 the DMA actually reads, plus zero/valued
                            // counts, and the chain START tag so we know where the walk begins.
                            static int st = 0, mp = 0, mz = 0, mv = 0;
                            if (tagsProcessed == 1 && (st++ % 200) < 4)
                                std::fprintf(stderr, "[dmatag-start] first tag@0x%08x id=%u qwc=%u\n",
                                             currentTagAddr, id, (unsigned)tagQwc);
                            if (id == 1u && !tagInSPR && physTag + 0x40u <= tagMax) {
                                const uint32_t *w = reinterpret_cast<const uint32_t*>(tagBase + physTag);
                                if (w[2] == 0x10000000u && w[3] == 0x6c0c0000u) { // FLUSH + UNPACK V4-32 -> qw0
                                    const float *m = reinterpret_cast<const float*>(tagBase + physTag + 16u);
                                    const bool zero = (m[0]==0.f && m[1]==0.f && m[2]==0.f && m[3]==0.f);
                                    if (zero) ++mz; else ++mv;
                                    if ((mp++ % 500) < 4)
                                        std::fprintf(stderr, "[mtxpkt] tag@0x%08x qwc=%u q0=(%.2f %.2f %.2f %.2f) zero=%d valued=%d\n",
                                                     currentTagAddr, (unsigned)tagQwc, m[0],m[1],m[2],m[3], mz, mv);
                                    // PS2X_ZEROTAG: on the first ZERO matrix packet, arm the guest
                                    // write-watch on its header+matrix slot. Frame layouts are stable,
                                    // so next frame the (broken) builder that emits this packet writes
                                    // the same address -> ps2WatchReport logs its guest pc/ra. Finds
                                    // the packet builder whose matrix copy is missing/misdirected.
                                    if (zero && s_zeroTag && g_ps2WatchLo.load(std::memory_order_relaxed) == 0u)
                                    {
                                        const uint32_t lo = currentTagAddr & 0x1FFFFFFFu;
                                        g_ps2WatchAll.store(1u, std::memory_order_relaxed); // keep zero-value writes
                                        g_ps2WatchHi.store(lo + 0x50u, std::memory_order_relaxed);
                                        g_ps2WatchLo.store(lo, std::memory_order_relaxed);
                                        std::fprintf(stderr, "[zerotag] ARMED write-watch on zero matrix packet tag@0x%08x (+0x50)\n", lo);
                                    }
                                }
                            }
                        }

                        // PS2X_CHAINWATCH: the terrain draw chain (arena ~0x6e0900, verified
                        // well-formed + fully parseable offline) never delivers its draws.
                        // Log VIF1 chain STARTS, any visit to the terrain arena, and IRQ-stops
                        // — shows whether the live walk ever reaches/resumes into the arena.
                        {
                            static const bool s_cw2 = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_CHAINWATCH"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                            if (s_cw2 && channelBase == 0x10009000u)
                            {
                                static std::atomic<uint32_t> s_st{0}, s_ar{0};
                                if (tagsProcessed == 1 && s_st.fetch_add(1) < 200000u)
                                {
                                    extern std::atomic<uint64_t> g_bt3FrameCount;
                                    std::fprintf(stderr, "[chain] START fr=%llu tag@0x%08x id=%u qwc=%u\n",
                                                 (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed),
                                                 currentTagAddr, id, (unsigned)tagQwc);
                                }
                                if (currentTagAddr >= 0x6e0000u && currentTagAddr < 0x6e6000u && s_ar.fetch_add(1) < 400u)
                                    std::fprintf(stderr, "[chain] ARENA-VISIT tag@0x%08x id=%u qwc=%u (tag #%d)\n",
                                                 currentTagAddr, id, (unsigned)tagQwc, tagsProcessed);
                            }
                        }

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        {   // [chaintrace] PS2X_CHAINTRACE=<hexframe>: one-shot FULL tag listing of the
                            // first VIF1 chain walked at/after that frame (find where the walk diverges
                            // from the built sheet-upload segment).
                            static const long s_ctFr = [](){ const char *v = std::getenv("PS2X_CHAINTRACE"); return v && v[0] ? std::strtol(v, nullptr, 16) : -1; }();
                            if (s_ctFr >= 0 && channelBase == 0x10009000u)
                            {
                                extern std::atomic<uint64_t> g_bt3FrameCount;
                                static std::atomic<int> s_ctState{0}; // 0=waiting 1=tracing 2=done
                                static std::atomic<uint32_t> s_ctLines{0};
                                int st0 = s_ctState.load(std::memory_order_relaxed);
                                if (st0 == 0 && (long)g_bt3FrameCount.load(std::memory_order_relaxed) >= s_ctFr && tagsProcessed == 1)
                                    { s_ctState.store(1); s_ctLines.store(0); std::fprintf(stderr, "[chaintrace] BEGIN fr=%llu\n", (unsigned long long)g_bt3FrameCount.load(std::memory_order_relaxed)); }
                                if (s_ctState.load(std::memory_order_relaxed) == 1)
                                {
                                    if (s_ctLines.fetch_add(1) < 3000u)
                                        std::fprintf(stderr, "[chaintrace] #%d tag@0x%08x id=%u qwc=%u next=0x%08x\n",
                                                     tagsProcessed, currentTagAddr, id, (unsigned)tagQwc, tagAddr);
                                    if (endChain || id == 7u)
                                        { s_ctState.store(2); std::fprintf(stderr, "[chaintrace] END after %d tags\n", tagsProcessed); }
                                }
                            }
                        }
                        const bool compactVifLocalTag =
                            (channelBase == 0x10009000u || channelBase == 0x10008000u) &&
                            (id == 1u || id == 2u || id == 5u || id == 6u || id == 7u);
                        if (compactVifLocalTag && tteEnabled)
                            appendCompactVif1TagData(currentTagAddr, 0u);

                        if (hasPayload)
                        {
                            if (compactVifLocalTag)
                                appendData(currentTagAddr + 16u, tagQwc);
                            else
                                appendData(dataAddr, tagQwc);
                            // [sheettag] PS2X_SHEETTAG=1: log VIF1 chain tags whose data lies in the
                            // pak band-sheet region or the entry-4 default block — shows whether the
                            // per-frame upload chain REFs the real sheet (0x139bxxx) or the default.
                            static const bool s_st = [](){ const char *v = std::getenv("PS2X_SHEETTAG"); return v && v[0] && v[0] != '0'; }();
                            if (s_st && (channelBase == 0x10009000u || channelBase == 0x1000A000u))
                            {
                                const uint32_t da = (compactVifLocalTag ? currentTagAddr + 16u : dataAddr) & 0x1FFFFFFFu;
                                if (da >= 0x1398000u && da < 0x13a0000u)
                                {
                                    static std::atomic<uint32_t> s_stn{0};
                                    if (s_stn.fetch_add(1) < 200u)
                                        std::fprintf(stderr, "[sheettag] id=%u data=0x%08x qwc=%u tag@0x%08x\n",
                                                     id, da, (unsigned)tagQwc, currentTagAddr);
                                }
                            }
                        }
                        if (irq && tieEnabled)
                        {
                            static const bool s_cw3 = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_CHAINWATCH"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                            if (s_cw3 && channelBase == 0x10009000u)
                            {
                                static std::atomic<uint32_t> s_iq{0};
                                if (s_iq.fetch_add(1) < 400u)
                                    std::fprintf(stderr, "[chain] IRQ-STOP tag@0x%08x id=%u qwc=%u (tag #%d, nextTadr=0x%08x)\n",
                                                 currentTagAddr, id, (unsigned)tagQwc, tagsProcessed, tagAddr);
                            }
                            // PS2X_CHAIN_IGNORE_IRQ=1 (A/B): don't stop at IRQ+TIE tags — behave
                            // as if the DMA-interrupt handler re-kicked instantly, continuing the
                            // remainder in the same walk. Tests whether the fight floor's missing
                            // tw=8 bulk (real HW: ~11k prims AFTER the first chain segments; ours:
                            // truncated at ~145+34 prims — see [floorseq] vs the GS-dump template)
                            // is dropped at these stops.
                            static const bool s_igi = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_CHAIN_IGNORE_IRQ"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                            if (!s_igi)
                                endChain = true;
                        }
                        if (endChain)
                            break;
                    }

                    lastKickTags = static_cast<uint32_t>(tagsProcessed);
                    // Grow the reserve hint toward this chain's size (with headroom) so future
                    // kicks pre-allocate once instead of reallocating up from empty.
                    {
                        uint32_t sz = static_cast<uint32_t>(chainBuf.size());
                        uint32_t hint = s_chainHint.load(std::memory_order_relaxed);
                        if (sz > hint) s_chainHint.store(sz + sz / 4u, std::memory_order_relaxed);
                    }
                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    if (!chainBuf.empty())
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                if (s_tw)
                {
                    static std::atomic<uint64_t> s_twNs{0}, s_twTags{0}; static std::atomic<uint32_t> s_twN{0}, s_twMax{0};
                    s_twNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-_tw0).count(), std::memory_order_relaxed);
                    s_twN.fetch_add(1, std::memory_order_relaxed);
                    s_twTags.fetch_add(static_cast<uint64_t>(lastKickTags), std::memory_order_relaxed);
                    { uint32_t cur = s_twMax.load(); while (lastKickTags > cur && !s_twMax.compare_exchange_weak(cur, lastKickTags)) {} }
                    static std::mutex s_m; static std::chrono::steady_clock::time_point s_l = std::chrono::steady_clock::now();
                    std::lock_guard<std::mutex> lk(s_m);
                    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now()-s_l).count();
                    if (dt >= 1.0) { uint32_t n=s_twN.load(); std::fprintf(stderr, "[tagwalk] %.1fms/s kicks/s=%u avgTags/kick=%llu maxTags=%u\n", s_twNs.load()/1e6/dt, (unsigned)(n/dt), (unsigned long long)(n?s_twTags.load()/n:0), s_twMax.load()); s_twNs=0; s_twN=0; s_twTags=0; s_twMax=0; s_l=std::chrono::steady_clock::now(); }
                }
                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

bool PS2Memory::asyncKickEnabled()
{
    static const bool s_on = [](){ const char *v = std::getenv("PS2X_ASYNC_KICK"); return v && v[0] && v[0] != '0'; }();
    return s_on;
}

void PS2Memory::ensureKickWorker()
{
    // Caller holds m_kickMtx.
    if (m_kickThreadStarted)
        return;
    m_kickThreadStarted = true;
    m_kickThread = std::thread([this]() { kickWorkerLoop(); });
}

void PS2Memory::enqueueKickJob(KickJob &&job)
{
    std::unique_lock<std::mutex> lk(m_kickMtx);
    ensureKickWorker();
    // Backpressure: cap the pipeline at 2 unconsumed frame boundaries (current + 1 in
    // flight) so a slow worker bounds latency/memory instead of queueing frames forever.
    if (job.kind == KickJob::SwapFrame)
    {
        m_kickDoneCv.wait(lk, [this]() { return m_kickFramesQueued < 2u || m_kickStop; });
        ++m_kickFramesQueued;
    }
    else
    {
        m_kickDoneCv.wait(lk, [this]() { return m_kickQueue.size() < 8192u || m_kickStop; });
    }
    if (m_kickStop)
        return;
    m_kickQueue.push_back(std::move(job));
    m_kickCv.notify_one();
}

void PS2Memory::enqueueGpuSwapMarker()
{
    KickJob j;
    j.kind = KickJob::SwapFrame;
    enqueueKickJob(std::move(j));
}

void PS2Memory::drainKickQueue()
{
    std::unique_lock<std::mutex> lk(m_kickMtx);
    if (!m_kickThreadStarted)
        return;
    m_kickDoneCv.wait(lk, [this]() { return (m_kickQueue.empty() && !m_kickBusy) || m_kickStop; });
}

void PS2Memory::kickWorkerLoop()
{
    for (;;)
    {
        KickJob job;
        {
            std::unique_lock<std::mutex> lk(m_kickMtx);
            m_kickCv.wait(lk, [this]() { return !m_kickQueue.empty() || m_kickStop; });
            if (m_kickStop)
                return;
            job = std::move(m_kickQueue.front());
            m_kickQueue.pop_front();
            m_kickBusy = true;
        }
        switch (job.kind)
        {
        case KickJob::Vif1:
            processVIF1Data(job.data.data(), static_cast<uint32_t>(job.data.size()));
            break;
        case KickJob::GifPath3:
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, job.data.data(), static_cast<uint32_t>(job.data.size()), false);
            break;
        case KickJob::SwapFrame:
            ps2GpuRenderer().swapFrame();
            break;
        }
        // Sync mode ends every kick with a trailing arbiter drain (processPendingTransfers);
        // replicate that here so deferred PATH3 packets don't sit queued across frames.
        if (job.kind != KickJob::SwapFrame && m_gifArbiter)
            m_gifArbiter->drain();
        {
            // Worker-thread-only heartbeat (no locking needed for the statics).
            static uint64_t s_jobs = 0, s_swaps = 0;
            static auto s_last = std::chrono::steady_clock::now();
            ++s_jobs;
            if (job.kind == KickJob::SwapFrame) ++s_swaps;
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - s_last).count();
            if (dt >= 1.0)
            {
                size_t depth; uint32_t frames;
                {
                    std::lock_guard<std::mutex> lk(m_kickMtx);
                    depth = m_kickQueue.size();
                    frames = m_kickFramesQueued;
                }
                std::fprintf(stderr, "[kickq] jobs/s=%llu swaps/s=%llu depth=%zu framesQ=%u\n",
                             (unsigned long long)(s_jobs / dt), (unsigned long long)(s_swaps / dt), depth, frames);
                s_jobs = 0; s_swaps = 0; s_last = now;
            }
        }
        {
            std::unique_lock<std::mutex> lk(m_kickMtx);
            m_kickBusy = false;
            if (job.kind == KickJob::SwapFrame && m_kickFramesQueued > 0u)
                --m_kickFramesQueued;
            m_kickDoneCv.notify_all();
        }
    }
}

void PS2Memory::processPendingTransfers()
{
    const auto _pt0 = std::chrono::steady_clock::now();
    const bool hadGif = !m_pendingGifTransfers.empty();
    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (asyncKickEnabled())
        {
            // Hand the transfer to the kick worker. chainData is already a self-contained
            // copy; qwc-only transfers are copied here (at kick time) because the guest is
            // free to reuse the source RAM once CHCR.STR reads back 0.
            if (!p.chainData.empty())
            {
                KickJob j;
                j.kind = KickJob::GifPath3;
                j.data = std::move(p.chainData);
                enqueueKickJob(std::move(j));
            }
            else if (p.qwc > 0)
            {
                uint32_t srcPhys = 0;
                const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
                uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                try
                {
                    srcPhys = translateAddress(p.srcAddr);
                }
                catch (const std::exception &)
                {
                    continue;
                }
                const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
                const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= limit)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > limit)
                        chunk = limit - srcPhys;
                    if (chunk == 0)
                        break;
                    KickJob j;
                    j.kind = KickJob::GifPath3;
                    j.data.assign(base + srcPhys, base + srcPhys + chunk);
                    enqueueKickJob(std::move(j));
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            continue;
        }
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    // [upsrc] PS2X_UPSRC=1: find the guest RAM source of the terrain band-sheet
                    // upload — scan PATH3 chunks for an A+D BITBLTBUF with DBP==10752, report srcPhys.
                    if ([](){ static const char *s_env = std::getenv("PS2X_UPSRC"); return s_env; }()) {
                        static std::atomic<int> s_us{0};
                        for (uint32_t o = 0; o + 16 <= chunk && s_us.load(std::memory_order_relaxed) < 24; o += 16) {
                            uint64_t d0, d1;
                            std::memcpy(&d0, m_rdram + srcPhys + o, 8);
                            std::memcpy(&d1, m_rdram + srcPhys + o + 8, 8);
                            if ((d1 & 0xFFu) == 0x50u && ((d0 >> 32) & 0x3FFFu) == 10752u) {
                                if (s_us.fetch_add(1) < 24)
                                    fprintf(stderr, "[upsrc] BITBLTBUF dbp=10752 sbp=%u spsm=%u at srcPhys=0x%08x +%u chunk=%u\n",
                                        (uint32_t)(d0 & 0x3FFFu), (uint32_t)((d0 >> 24) & 0x3Fu), srcPhys, o, chunk);
                            }
                        }
                    }
                    if ([](){ static const char *s_env = std::getenv("PS2X_GIFSRC"); return s_env; }()) {
                        // Scan the packet for a HUD-texture TEX0 (tbp0 in [10752,11264], psm=19)
                        // and report the EE source address the sprite engine built it at.
                        static int gs_n = 0;
                        for (uint32_t o = 0; o + 4 <= chunk && gs_n < 40; o += 4) {
                            uint32_t w; std::memcpy(&w, m_rdram + srcPhys + o, 4);
                            uint32_t tbp0 = w & 0x3FFFu, psm = (w >> 20) & 0x3Fu;
                            if (psm == 19u && tbp0 >= 10752u && tbp0 <= 11264u) {
                                gs_n++;
                                fprintf(stderr, "[gifsrc] HUD tex tbp0=%u at EE src=0x%08x (pkt off +%u, chunk=%u)\n",
                                    tbp0, srcPhys, o, chunk);
                                break;
                            }
                        }
                    }
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingGifTransfers.clear();
    const auto _pt1 = std::chrono::steady_clock::now();

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif0Transfers.clear();
    const auto _pt2 = std::chrono::steady_clock::now();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    for (auto &p : m_pendingVif1Transfers)
    {
        if (asyncKickEnabled())
        {
            if (!p.chainData.empty())
            {
                KickJob j;
                j.kind = KickJob::Vif1;
                j.data = std::move(p.chainData);
                enqueueKickJob(std::move(j));
            }
            else if (p.qwc > 0)
            {
                uint32_t srcPhys = 0;
                const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
                uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                try
                {
                    srcPhys = translateAddress(p.srcAddr);
                }
                catch (const std::exception &)
                {
                    continue;
                }
                const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
                const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= limit)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > limit)
                        chunk = limit - srcPhys;
                    if (chunk == 0)
                        break;
                    KickJob j;
                    j.kind = KickJob::Vif1;
                    j.data.assign(base + srcPhys, base + srcPhys + chunk);
                    enqueueKickJob(std::move(j));
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            continue;
        }
        if (!p.chainData.empty())
        {
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    g_vif1QwcSrcGuest = p.srcAddr + (sizeBytes - bytesLeft);
                    g_vif1QwcActive = true;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    g_vif1QwcActive = false;
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    g_vif1QwcSrcGuest = p.srcAddr + (sizeBytes - bytesLeft);
                    g_vif1QwcActive = true;
                    processVIF1Data(srcPhys, chunk);
                    g_vif1QwcActive = false;
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif1Transfers.clear();
    const auto _pt3 = std::chrono::steady_clock::now();

    // DMA-path profiler (env PS2X_DMAPROF): the main thread processes VIF1/GIF DMA
    // synchronously on the VIF1_CHCR write (the confirmed fps gate). Split the cost
    // into GIF(direct rasterize) vs VIF1(VU1 microcode->GIF->rasterize) vs VIF0 so we
    // know which sub-phase to make async/optimize.
    {
        static const bool s_dmaProf = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_DMAPROF"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
        if (s_dmaProf)
        {
            using nsdur = std::chrono::nanoseconds;
            static std::atomic<uint64_t> s_gifNs{0}, s_vif0Ns{0}, s_vif1Ns{0}, s_calls{0};
            s_gifNs.fetch_add(std::chrono::duration_cast<nsdur>(_pt1 - _pt0).count(), std::memory_order_relaxed);
            s_vif0Ns.fetch_add(std::chrono::duration_cast<nsdur>(_pt2 - _pt1).count(), std::memory_order_relaxed);
            s_vif1Ns.fetch_add(std::chrono::duration_cast<nsdur>(_pt3 - _pt2).count(), std::memory_order_relaxed);
            s_calls.fetch_add(1, std::memory_order_relaxed);
            static std::mutex s_pm;
            static std::chrono::steady_clock::time_point s_last = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(s_pm);
            double dt = std::chrono::duration<double>(_pt3 - s_last).count();
            if (dt >= 1.0)
            {
                std::cerr << "[dmaprof] vif1(VU1+GIF)=" << (s_vif1Ns.load() / 1e6 / dt)
                          << "ms/s gif(direct)=" << (s_gifNs.load() / 1e6 / dt)
                          << "ms/s vif0=" << (s_vif0Ns.load() / 1e6 / dt)
                          << "ms/s calls/s=" << (uint64_t)(s_calls.load() / dt) << std::endl;
                s_gifNs = 0; s_vif0Ns = 0; s_vif1Ns = 0; s_calls = 0;
                s_last = _pt3;
            }
        }
    }

    // Async mode: the kick worker owns the (unlocked) GifArbiter — draining it here on
    // the guest thread races the worker's submit/drain and corrupts the packet queue.
    // The worker drains after every job instead.
    if (m_gifArbiter && !asyncKickEnabled())
        m_gifArbiter->drain();

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        queueCompletedDmacCause(1u);
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    // [upsrc3] PS2X_UPSRC=1: all-paths scan for BITBLTBUF selecting the band sheet (10752)
    // or band CLUT (12992) — reports the packet's path + guest source when recoverable.
    {
        static const bool s_u3 = [](){ const char *v = std::getenv("PS2X_UPSRC"); return v && v[0] && v[0] != '0'; }();
        if (s_u3 && data && sizeBytes >= 32u)
        {
            static std::atomic<int> s_un3{0};
            for (uint32_t o = 0; o + 16u <= sizeBytes && o < 16384u && s_un3.load(std::memory_order_relaxed) < 30; o += 16u)
            {
                uint64_t plo, phi;
                std::memcpy(&plo, data + o, 8); std::memcpy(&phi, data + o + 8, 8);
                if ((phi & 0xFFu) == 0x50u)
                {
                    const uint32_t dbp3 = (uint32_t)((plo >> 32) & 0x3FFFu);
                    if (dbp3 == 10752u || dbp3 == 12992u)
                    {
                        uint32_t srcG = 0u;
                        if (data >= m_rdram && data < m_rdram + PS2_RAM_SIZE) srcG = (uint32_t)(data - m_rdram) + o;
                        if (s_un3.fetch_add(1) < 30)
                            std::fprintf(stderr, "[upsrc3] path%d BITBLTBUF dbp=%u sbp=%u srcG=0x%08x size=%u off=%u\n",
                                         (int)pathId, dbp3, (uint32_t)(plo & 0x3FFFu), srcG, sizeBytes, o);
                    }
                }
            }
        }
    }
    if (!data || sizeBytes < 16)
        return;

    if ([](){ static const char *s_env = std::getenv("PS2X_GIFSRC"); return s_env; }()) {
        static int gs_n = 0;
        for (uint32_t o = 0; o + 4 <= sizeBytes && gs_n < 60; o += 4) {
            uint32_t w; std::memcpy(&w, data + o, 4);
            uint32_t tbp0 = w & 0x3FFFu, psm = (w >> 20) & 0x3Fu;
            if ((psm == 19u || psm == 20u || psm == 0u) && tbp0 >= 10000u && tbp0 <= 12000u) {
                gs_n++;
                long src = (data >= m_rdram && data < m_rdram + PS2_RAM_SIZE) ? (long)(data - m_rdram) : -1;
                fprintf(stderr, "[gifsrc] path=%d tbp0=%u psm=%u off=+%u EEsrc=0x%08lx size=%u\n",
                    (int)pathId, tbp0, psm, o, src, sizeBytes);
                break;
            }
        }
    }

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;
            
        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        {   // [palsrc] PS2X_PALSRC=<dbp>: log the GUEST source address of GIF packets that set
            // BITBLTBUF to this destination block -- the EE-side buffer the game builds its
            // terrain palettes in. Feed the address to the write-watch to catch the BUILDER
            // function (the unstable sun-lighting group assignment, 2026-09-01).
            static const uint32_t s_pd = [](){ const char *v = std::getenv("PS2X_PALSRC");
                                               return v && v[0] ? (uint32_t)std::atoi(v) : 0u; }();
            static int s_pn = 0;
            if (s_pd && s_pn < 40)
            {
                const uint8_t *p = m_rdram + srcPhysAddr;
                for (uint32_t off = 0; off + 16 <= chunk; off += 16)
                {
                    uint64_t lo, hi;
                    std::memcpy(&lo, p + off, 8); std::memcpy(&hi, p + off + 8, 8);
                    if ((hi & 0xFFull) == 0x50ull && ((lo >> 32) & 0x3FFFull) == s_pd)
                    {
                        std::fprintf(stderr, "[palsrc] #%d BITBLTBUF dbp=%u at guest %#x (packet %#x+%u len %u)\n",
                                     s_pn, s_pd, srcPhysAddr + off, srcPhysAddr, off, chunk);
                        // PS2X_PALSRC_ARM=1: arm the guest write-watch on this packet region --
                        // next frame's rebuild of the palette buffer backtraces its EE writer.
                        static const bool s_arm = [](){ const char *v = std::getenv("PS2X_PALSRC_ARM");
                                                        return v && v[0] && v[0] != '0'; }();
                        if (s_arm && g_ps2WatchLo.load(std::memory_order_relaxed) == 0u)
                        {
                            g_ps2WatchHi.store((srcPhysAddr + chunk) & 0x1FFFFFFFu, std::memory_order_relaxed);
                            g_ps2WatchLo.store(srcPhysAddr & 0x1FFFFFFFu, std::memory_order_relaxed);
                            std::fprintf(stderr, "[palsrc] write-watch ARMED on %#x..%#x\n",
                                         srcPhysAddr, srcPhysAddr + chunk);
                        }
                        if (++s_pn >= 40) break;
                    }
                }
            }
        }
        {   // [wispsrc] PS2X_WISPSRC=<tbp>: guest address of PATH3 packets that bind a PSMT8
            // 64x64 texture at this TBP0 (the aura wisps: tbp 11172/11196), and of the first
            // vertex RGBAQ after it whose colour bytes equal PS2X_WISPRGB (default 0xA95A92,
            // our wisp colour) -- the vertex ALPHA byte that arrives as 0 (2026-09-03).
            // PS2X_WISPSRC_ARM=1 arms the guest write-watch on that alpha byte so the EE
            // writer of the next frame's packet backtraces.
            static const uint32_t s_wt = [](){ const char *v = std::getenv("PS2X_WISPSRC");
                                               return v && v[0] ? (uint32_t)std::atoi(v) : 0u; }();
            static const uint32_t s_wrgb = [](){ const char *v = std::getenv("PS2X_WISPRGB");
                                                 return v && v[0] ? (uint32_t)std::strtoul(v, nullptr, 0) : 0xA95A92u; }();
            static const bool s_warm = [](){ const char *v = std::getenv("PS2X_WISPSRC_ARM");
                                             return v && v[0] && v[0] != '0'; }();
            static int s_wn = 0;
            if (s_wt && s_wn < 40)
            {
                const uint8_t *p = m_rdram + srcPhysAddr;
                for (uint32_t off = 0; off + 16 <= chunk; off += 16)
                {
                    uint64_t lo, hi;
                    std::memcpy(&lo, p + off, 8); std::memcpy(&hi, p + off + 8, 8);
                    // TEX0 signature: TBP0 == tbp, PSM == PSMT8 (19), TW == 6 (64 texels)
                    if ((lo & 0x3FFFull) != s_wt || ((lo >> 20) & 0x3Full) != 19ull || ((lo >> 26) & 0xFull) != 6ull) continue;
                    uint32_t rgbaOff = 0u, aByte = 0u;
                    for (uint32_t o2 = off + 16; o2 + 16 <= chunk && o2 < off + 16u * 96u; o2 += 16)
                    {
                        uint64_t l2, h2;
                        std::memcpy(&l2, p + o2, 8); std::memcpy(&h2, p + o2 + 8, 8);
                        const uint32_t r = (uint32_t)(l2 & 0xFFull), g = (uint32_t)((l2 >> 32) & 0xFFull), b = (uint32_t)(h2 & 0xFFull);
                        if (((r << 16) | (g << 8) | b) == s_wrgb) { rgbaOff = o2; aByte = (uint32_t)((h2 >> 32) & 0xFFull); break; }
                    }
                    std::fprintf(stderr, "[wispsrc] #%d TEX0 tbp=%u reg=%#llx at guest %#x (packet %#x+%u len %u) firstRGBAQ=%#x A=%u\n",
                                 s_wn, s_wt, (unsigned long long)(hi & 0xFFull), srcPhysAddr + off, srcPhysAddr, off, chunk,
                                 rgbaOff ? srcPhysAddr + rgbaOff : 0u, aByte);
                    if (s_warm && rgbaOff && g_ps2WatchLo.load(std::memory_order_relaxed) == 0u)
                    {
                        const uint32_t a0 = (srcPhysAddr + rgbaOff + 12u) & 0x1FFFFFFFu;
                        g_ps2WatchHi.store(a0 + 4u, std::memory_order_relaxed);
                        g_ps2WatchLo.store(a0, std::memory_order_relaxed);
                        std::fprintf(stderr, "[wispsrc] write-watch ARMED on alpha byte %#x..%#x\n", a0, a0 + 4u);
                    }
                    if (++s_wn >= 40) break;
                }
            }
        }
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);
        
        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    if (isGsPrivReg(address))
    {
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }
    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000000 && address < 0x10000100)
        {
            if (isEeTimer0Register(address))
            {
                if (address == kEeTimer0Count)
                {
                    updateEeTimer0Counter();
                }
                auto timerIt = m_ioRegisters.find(address);
                return timerIt != m_ioRegisters.end() ? timerIt->second : 0u;
            }
        }

        // EE Timers T1/T2/T3 count registers (T0 handled above). The runtime only
        // ticked T0, so T2-based frame-timing waits after the boot banner hang.
        // Env-gated (PS2X_EETIMERS) to keep default behavior unchanged.
        // Match a COUNT (base+0x00) or MODE (base+0x10) read of Timer 1/2/3 so that
        // reading either register advances the counter and refreshes the status flags
        // (games poll the MODE register's EQUF/OVFF bits, not just COUNT).
        if (address == 0x10000800u || address == 0x10001000u || address == 0x10001800u ||
            address == 0x10000810u || address == 0x10001010u || address == 0x10001810u)
        {
            const uint32_t baseAddr = address & ~0x1Fu; // COUNT register of this timer
            static const bool s_eeTimers = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_EETIMERS"); return s_env; }(); return !(v && v[0] == '0'); }();
            static std::atomic<uint32_t> s_t2reads{0};
            if ([](){ static const char *s_env = std::getenv("PS2X_TMRPROBE"); return s_env; }() && (s_t2reads.fetch_add(1) % 2000u) == 0u)
            {
                const uint32_t mode = m_ioRegisters.count(0x10001010u) ? m_ioRegisters[0x10001010u] : 0u;
                const uint32_t cnt = m_ioRegisters.count(0x10001000u) ? m_ioRegisters[0x10001000u] : 0u;
                const uint32_t comp = m_ioRegisters.count(0x10001020u) ? m_ioRegisters[0x10001020u] : 0u;
                std::cerr << "[t2read] #" << s_t2reads.load() << " reg=0x" << std::hex << address << std::dec
                          << " T2 count=" << cnt << " comp=" << comp
                          << " mode=0x" << std::hex << mode << std::dec
                          << " EQUF=" << ((mode & 0x400u) ? 1 : 0) << " OVFF=" << ((mode & 0x800u) ? 1 : 0)
                          << " CUE=" << ((mode & kEeTimerModeCue) ? 1 : 0)
                          << " eetimers=" << (s_eeTimers ? 1 : 0) << std::endl;
            }
            if (s_eeTimers)
            {
                const uint32_t idx = eeTimerIndexForBase(baseAddr); // 1,2,3
                const uint32_t modeAddr = baseAddr + 0x10u;
                const uint32_t compAddr = baseAddr + 0x20u;
                const uint32_t mode = m_ioRegisters.count(modeAddr) ? m_ioRegisters[modeAddr] : 0u;
                const uint64_t now = steadyClockNs();
                if (mode & kEeTimerModeCue) // counting enabled
                {
                    if (g_eeTimerLastNs[idx] == 0u) g_eeTimerLastNs[idx] = now;
                    const uint64_t elapsed = now - g_eeTimerLastNs[idx];
                    g_eeTimerLastNs[idx] = now;
                    const uint64_t scaled = elapsed * kEeTimerClkRates[mode & 3u] + g_eeTimerFracNs[idx];
                    g_eeTimerFracNs[idx] = scaled % kNanosecondsPerSecond;
                    const uint64_t inc = scaled / kNanosecondsPerSecond;
                    const uint32_t oldCount = m_ioRegisters.count(baseAddr) ? m_ioRegisters[baseAddr] : 0u;
                    const uint64_t newFull = static_cast<uint64_t>(oldCount) + inc;
                    m_ioRegisters[baseAddr] = static_cast<uint32_t>(newFull & 0xFFFFu);
                    // Emulate the T_MODE status flags that games poll: EQUF (bit 10) on a
                    // compare-match, OVFF (bit 11) on count overflow. Hardware sets these; they
                    // are write-1-to-clear (handled in writeIORegister). Without this the count
                    // free-runs but overflow/compare waits (BT3 fight frame timing) never fire.
                    uint32_t flagSet = 0u;
                    if (newFull > 0xFFFFu) flagSet |= 0x800u; // OVFF overflow
                    if (inc > 0u)
                    {
                        const uint32_t compare = m_ioRegisters.count(compAddr) ? (m_ioRegisters[compAddr] & 0xFFFFu) : 0u;
                        if (inc >= 0x10000u) flagSet |= 0x400u; // swept the whole range
                        else
                        {
                            const uint32_t start = (oldCount + 1u) & 0xFFFFu;
                            const uint32_t rel = (compare - start) & 0xFFFFu;
                            if (rel < static_cast<uint32_t>(inc)) flagSet |= 0x400u; // EQUF compare-match
                        }
                    }
                    if (flagSet) m_ioRegisters[modeAddr] = mode | flagSet;
                }
                else
                {
                    g_eeTimerLastNs[idx] = now;
                    g_eeTimerFracNs[idx] = 0u;
                }
                return m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
            }
        }

        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
