// Based on Blackline Interactive implementation
#include "runtime/ps2_guestprof.h"
#include <cstdio>
#include "runtime/ps2_memory.h"
#include <cstring>
#include <cmath>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <iostream>

// UNPACK time profiler (env PS2X_DMAPROF): confirm whether VIF UNPACK vertex
// expansion is the fps bottleneck inside processVIF1Data.
namespace { std::atomic<uint32_t> g_boneScanTarget{0}; }
#include <array>
extern std::vector<std::array<uint32_t, 3>> g_kickSrcMap; // see ps2_memory.cpp
extern bool g_kickSrcMapEnabled();
extern uint32_t g_vif1QwcSrcGuest; // qwc (non-chain) transfer source base
// [shadowpass] PS2X_SHADOWPASS=1: the game sends the Pass-1 shadow-silhouette GS context every frame
// (DIRECT A+D FRAME_1=0x40150 = fbp336 fbw4, then SCISSOR_1 (1,1)-(254,254)) but no vertices ever land
// in fbp336 under our runtime (pcsx2dump draw 1849 = 9348-vertex grey mesh). Count what VIF1/VU1 do
// while that context is current: MSCALs (entry pc), XGKICKs and their GIF NLOOPs.
bool g_spInShadow = false; bool g_spFrame336 = false; bool g_spScissor254 = false;
uint32_t g_spSets = 0, g_spMscal = 0, g_spLastPC = 0, g_spKicks = 0, g_spLoops = 0, g_spUnpackQw = 0;
extern bool g_vif1QwcActive;
// PS2X_KICKHIST: rolling history of recent VIF1 unpacks (dest qw, count, EE source, frame),
// dumped by the XGKICK spike probe to show exactly which writes fed a popup kick's buffer.
// VIF parse and the resulting VU1 run share a thread in both sync and async modes, so
// thread_local is safe.
struct Vif1UnpackRec { uint32_t destQw, cnt, srcGuest, spr; uint64_t frame; };
thread_local Vif1UnpackRec g_unpackRing[32] = {};
thread_local uint32_t g_unpackRingPos = 0;
bool g_unpackRingEnabled()
{
    static const bool s_on = [](){ const char *v = std::getenv("PS2X_KICKHIST"); return v && v[0] && v[0] != '0'; }();
    return s_on;
}
// PS2X_SKIP_DEGEN: when the constants block (MVP qw0-3) that just arrived is degenerate
// (non-float garbage), suppress the following XGKICKs until a sane block arrives. Kills
// the garbage-matrix object's screen-covering triangles while the real writer is hunted.
std::atomic<bool> g_degenSuppress{false};
// [mvpdisp] correlation: set by the 13qw@addr0 unpack check (1 healthy / 2 degenerate),
// consumed by the next MSCAL log. Same thread: VIF parsing is sequential per kick.
thread_local uint32_t g_last13at0 = 0;
namespace { std::atomic<uint64_t> g_vifUnpackNs{0}; std::atomic<uint64_t> g_vifUnpackVecs{0};
    std::atomic<uint64_t> g_vifMscalNs{0}; std::atomic<uint64_t> g_vifMscalN{0};
    std::atomic<uint64_t> g_vifMscntNs{0}; std::atomic<uint64_t> g_vifMscntN{0};
    std::atomic<uint64_t> g_vifGifNs{0}; std::atomic<uint64_t> g_vifGifN{0};
    const bool g_vifTimeProf = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_DMAPROF"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
    // PS2X_MTXSEQ: armed event-sequence tracer. Arms on the first VALUED matrix upload
    // (UNPACK vuAddr=0 cnt>=12, non-zero qw0) — i.e. fight time, skipping boot noise — then
    // logs the next ~150 UNPACK/MSCAL events to show exactly what overwrites VU1 qw0.
    const bool g_mtxSeq = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_MTXSEQ"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
    std::atomic<int> g_mtxSeqN{-1}; // -1 = not armed; >=0 = events logged
}

thread_local bool g_upsrcArmSheet = false;   // [upsrc2]
enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t gifImageQwcFromTag(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        if (flg != kGifFmtImage)
            return 0u;

        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    if ([](){ static const char *s_env = std::getenv("PS2X_GIFSRC"); return s_env; }()) {
        static int vs_n = 0;
        for (uint32_t o = 0; o + 4 <= sizeBytes && vs_n < 40; o += 4) {
            uint32_t w; std::memcpy(&w, m_rdram + srcPhys + o, 4);
            uint32_t tbp0 = w & 0x3FFFu, psm = (w >> 20) & 0x3Fu;
            if ((psm == 19u || psm == 20u) && tbp0 >= 10700u && tbp0 <= 11300u) {
                vs_n++;
                fprintf(stderr, "[vif1src] HUD tbp0=%u psm=%u at EEsrc=0x%08x (+%u, size=%u)\n",
                    tbp0, psm, srcPhys, o, sizeBytes);
                break;
            }
        }
    }

    {   // [wsdma] The widescreen projection floats (@0x2fe4cc / @0x2fe58c / @0x2fe594) have NO
        // EE reader -- gp-relative and lui/addiu searches over every generated function come up
        // empty -- so they must reach VU1 as DMA'd constant data. PS2X_WSDMA=1 reports each VIF1
        // transfer whose SOURCE range covers them, which is the only place a per-pass fix could
        // live: the pause menu needs the unscaled value while the clipper needs the scaled one.
        static const bool s_wsdma = [](){ const char *v = std::getenv("PS2X_WSDMA");
                                          return v && v[0] && v[0] != '0'; }();
        if (s_wsdma)
        {
            static const uint32_t kAddrs[3] = {0x2fe4ccu, 0x2fe58cu, 0x2fe594u};
            static unsigned long s_hits[3] = {0, 0, 0};
            static unsigned long s_frames = 0;
            for (int i = 0; i < 3; ++i)
                if (srcPhys <= kAddrs[i] && kAddrs[i] < srcPhys + sizeBytes)
                {
                    ++s_hits[i];
                    if (s_hits[i] <= 6 || (s_hits[i] % 600) == 0)
                    {
                        float v; std::memcpy(&v, m_rdram + kAddrs[i], 4);
                        std::fprintf(stderr, "[wsdma] 0x%06x carried by VIF1 src=0x%08x size=%u "
                                     "(+%u) value=%.4f hit#%lu\n",
                                     kAddrs[i], srcPhys, sizeBytes, kAddrs[i] - srcPhys, v, s_hits[i]);
                    }
                }
            if (++s_frames % 20000 == 0)
                std::fprintf(stderr, "[wsdma] totals: k1=%lu half=%lu k2=%lu over %lu transfers\n",
                             s_hits[0], s_hits[1], s_hits[2], s_frames);
        }
    }
    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;
    gprof::Scope gpScope(gprof::VIF);   // [guestprof]

    if ([](){ static const char *s_env = std::getenv("PS2X_GIFSRC"); return s_env; }()) {
        static int vs_n = 0;
        for (uint32_t o = 0; o + 4 <= sizeBytes && vs_n < 40; o += 4) {
            uint32_t w; std::memcpy(&w, data + o, 4);
            uint32_t tbp0 = w & 0x3FFFu, psm = (w >> 20) & 0x3Fu;
            if ((psm == 19u || psm == 20u) && tbp0 >= 10700u && tbp0 <= 11300u) {
                vs_n++;
                const char *where; long off = -1;
                if (data >= m_rdram && data < m_rdram + PS2_RAM_SIZE) { where = "rdram"; off = (long)(data - m_rdram); }
                else if (data >= m_scratchpad && data < m_scratchpad + PS2_SCRATCHPAD_SIZE) { where = "scratchpad"; off = (long)(data - m_scratchpad); }
                else where = "chainbuf";
                fprintf(stderr, "[vif1src] HUD tbp0=%u psm=%u in %s off=0x%lx (+%u in pkt, size=%u)\n",
                    tbp0, psm, where, off, o, sizeBytes);
                break;
            }
        }
    }

    static const bool s_vifProbe = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_VIF_PROBE"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
    if (s_vifProbe)
    {
        static std::atomic<uint32_t> s_calls{0};
        static std::atomic<uint64_t> s_bytes{0};
        uint32_t c = s_calls.fetch_add(1) + 1u;
        s_bytes.fetch_add(sizeBytes);
        if ((c % 128u) == 1u)
            std::cerr << "[vif1] call#" << c << " bytes=" << sizeBytes
                      << " totBytes=" << s_bytes.load()
                      << " vu1Code=" << (m_vu1Code ? 1 : 0)
                      << " mscalCb=" << (m_vu1MscalCallback ? 1 : 0) << std::endl;
    }

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
            const uint64_t imageTag =
                static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
            submitGifPacket(GifPathId::Path2,
                            imagePacket.data(),
                            static_cast<uint32_t>(imagePacket.size()),
                            true,
                            m_vif1PendingPath2DirectHl);

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        if (s_vifProbe)
        {
            static std::atomic<uint64_t> s_op[128]{};
            static std::atomic<uint32_t> s_tot{0};
            s_op[opcode & 0x7f].fetch_add(1);
            uint32_t t = s_tot.fetch_add(1) + 1u;
            if ((t % 20000u) == 1u)
            {
                std::cerr << "[vif1:ops]";
                for (int i = 0; i < 128; ++i) { uint64_t v = s_op[i].load(); if (v) std::cerr << " 0x" << std::hex << i << std::dec << "=" << v; }
                std::cerr << std::endl;
            }
        }

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;
            if (g_spInShadow) { ++g_spMscal; g_spLastPC = startPC; }   // [shadowpass]
            // [mvpdisp]: which microprogram consumes the last 13qw@addr0 unpack (healthy vs
            // degenerate)? Same pc for both = one program, garbage input; different pc =
            // packet families and the degenerate ones are misrouted.
            {
                extern thread_local uint32_t g_last13at0;
                if (g_last13at0)
                {
                    static std::atomic<uint32_t> s_md{0};
                    const bool deg = g_last13at0 == 2u;
                    if (deg || s_md.fetch_add(1) < 10u)
                        std::fprintf(stderr, "[mvpdisp] MSCAL pc=%u deg=%u top=%u\n",
                                     startPC, deg ? 1u : 0u, vif1_regs.tops & 0x3FFu);
                    g_last13at0 = 0u;
                }
            }

            // Values visible to the VU program for this MSCAL.
            // DobieStation semantics: ITOP = ITOPS; TOP = current TOPS;
            // then TOPS/DBF are prepared for the next buffer.
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if ([](){ static const char *s_env = std::getenv("PS2X_SEQ"); return s_env; }()) {
                static std::atomic<int> s_sm{0};
                if (s_sm.fetch_add(1) < 80)
                    std::fprintf(stderr, "[seq] MSCAL pc=%u top=%u itop=%u  <-- microprogram runs here\n", startPC, runTop, runItop);
            }
            if (g_mtxSeq && g_mtxSeqN.load() >= 0 && g_mtxSeqN.fetch_add(1) < 150)
                std::fprintf(stderr, "[mtxseq] MSCAL pc=%u top=%u\n", startPC, runTop);
            if (s_vifProbe)
            {
                static std::atomic<uint32_t> s_mscal{0};
                uint32_t m = s_mscal.fetch_add(1) + 1u;
                if ((m % 64u) == 1u)
                    std::cerr << "[vif1:MSCAL] #" << m << " startPC=" << startPC
                              << " top=" << runTop << " itop=" << runItop
                              << " cb=" << (m_vu1MscalCallback ? 1 : 0)
                              << " gif=" << m_gifCopyCount.load() << " gsw=" << m_gsWriteCount.load() << std::endl;
            }
            if (m_vu1MscalCallback)
            {
                const auto _m0 = g_vifTimeProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                m_vu1MscalCallback(startPC, runTop, runItop);
                if (g_vifTimeProf)
                {
                    g_vifMscalNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _m0).count(), std::memory_order_relaxed);
                    g_vifMscalN.fetch_add(1, std::memory_order_relaxed);
                }
            }
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            {
                extern thread_local uint32_t g_last13at0;
                if (g_last13at0)
                {
                    static std::atomic<uint32_t> s_mdc{0};
                    const bool deg = g_last13at0 == 2u;
                    if (deg || s_mdc.fetch_add(1) < 10u)
                        std::fprintf(stderr, "[mvpdisp] MSCNT top=%u deg=%u\n", runTop, deg ? 1u : 0u);
                    g_last13at0 = 0u;
                }
            }
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
            {
                const auto _m0 = g_vifTimeProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                m_vu1MscntCallback(runTop, runItop);
                if (g_vifTimeProf)
                {
                    g_vifMscntNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _m0).count(), std::memory_order_relaxed);
                    g_vifMscntN.fetch_add(1, std::memory_order_relaxed);
                }
            }
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            // VIF MPG semantics: NUM==0 means 256 instructions (2048 bytes).
            // MPG payload is instruction-packed and should not be QW-aligned.
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    { extern std::atomic<uint32_t> g_vu1CodeGen; g_vu1CodeGen.fetch_add(1u, std::memory_order_relaxed); }   // [vucache16] invalidate the decode cache
                {   // [vu1jit-missdump] program extent hi-water: an upload at 0 starts a program, later chunks extend it
                    extern std::atomic<uint32_t> g_vu1MpgHi;
                    const uint32_t end = destAddr + copyBytes;
                    if (destAddr == 0u || end > g_vu1MpgHi.load(std::memory_order_relaxed)) g_vu1MpgHi.store(end, std::memory_order_relaxed);
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                // [upsrc2] armed by a BITBLTBUF dbp=10752 header DIRECT: sample THIS payload
                // (the sheet image data) and find its guest RAM home by memmem.
                if (g_upsrcArmSheet && qwCount >= 1024u)
                {
                    g_upsrcArmSheet = false;
                    static std::atomic<int> s_sc{0};
                    {   // [upsrc3 2026-09-01] dump the first SIX armed payloads + multi-window RAM base vote
                        static std::atomic<int> s_dumpN{0};
                        const int dn = s_dumpN.fetch_add(1);
                        if (dn < 6)
                        {
                            const uint32_t lim3 = std::min(sizeBytes - pos, qwCount * 16u);
                            char pth3[96];
                            std::snprintf(pth3, sizeof pth3, "/home/z3/Desktop/bt3/work/upload_payload_%d.bin", dn);
                            if (FILE *f = std::fopen(pth3,"wb"))
                            { std::fwrite(data + pos, 1, lim3, f); std::fclose(f);
                              std::fprintf(stderr, "[upsrc3] payload %d dumped (%u bytes)\n", dn, lim3); }
                            // vote: 10 entropy-checked windows spread across the payload; base = home - windowOff
                            int votes = 0; uint32_t base0 = 0; int agree = 0;
                            for (int wnd = 0; wnd < 10; ++wnd)
                            {
                                const uint32_t wo = 256u + (uint32_t)wnd * (lim3 > 4096u ? (lim3 - 512u) / 10u : 64u);
                                if (wo + 64u > lim3) break;
                                bool seen[256]={}; int di=0;
                                for (int i=0;i<64;++i){ const uint8_t b=data[pos+wo+i]; if(!seen[b]){seen[b]=true;++di;} }
                                if (di < 16) continue;
                                for (uint32_t a = 0; a + 64u <= PS2_RAM_SIZE; a += 16u)
                                    if (std::memcmp(m_rdram + a, data + pos + wo, 64) == 0)
                                    {
                                        const uint32_t b2 = a - (wo & ~15u);
                                        std::fprintf(stderr, "[upsrc3] wnd+0x%x home=0x%08x base=0x%08x\n", wo, a, b2);
                                        ++votes; if (!base0) { base0 = b2; agree = 1; } else if (b2 == base0) ++agree;
                                        break;
                                    }
                            }
                            std::fprintf(stderr, "[upsrc3] votes=%d agree=%d base0=0x%08x\n", votes, agree, base0);
                        }
                    }
                    if (s_sc.fetch_add(1) < 3 && pos + 80u + 64u <= sizeBytes)
                    {
                        // [upsrc2-fix 2026-09-01] STRONG needle: the old fixed payload+80 window could be
                        // all-zero, matching the first zero block in RAM (0x53d3a0 mirage — 3 runs, byte
                        // watch showed pure zeros there). Skip forward to a 64B window with >=16 distinct
                        // byte values before searching; also search SPR (scratchpad DMA sources never
                        // touch main RAM and their stores take the traceless special path).
                        const uint8_t *needle = data + pos + 80u;
                        {
                            const uint32_t lim2 = std::min(sizeBytes - pos, qwCount * 16u);
                            for (uint32_t w = 80u; w + 64u <= lim2 && w < 4096u; w += 16u)
                            {
                                bool seen[256] = {}; int distinct = 0;
                                for (int i = 0; i < 64; ++i) { const uint8_t b = data[pos + w + i]; if (!seen[b]) { seen[b] = true; ++distinct; } }
                                if (distinct >= 16) { needle = data + pos + w; break; }
                            }
                        }
                        {   // scratchpad scan (16KB) — inline accessor from runtime/ps2_memory.h
                            if (uint8_t *sp = ps2GetScratchpadHostPtr())
                                for (uint32_t a = 0; a + 64u <= 16384u; a += 16u)
                                    if (std::memcmp(sp + a, needle, 64) == 0)
                                    { std::fprintf(stderr, "[upsrc2] SHEET payload found in SPR at 0x%04x (qwc=%u)\n", a, qwCount); break; }
                        }
                        int hits = 0;
                        for (uint32_t a = 0; a + 64u <= PS2_RAM_SIZE && hits < 3; a += 16u)
                            if (std::memcmp(m_rdram + a, needle, 64) == 0)
                            {
                                ++hits;
                                std::fprintf(stderr, "[upsrc2] SHEET payload found in RAM at 0x%08x (qwc=%u)\n", a, qwCount);
                                // [sheetwriters] aim the global store-watch at THIS run's staging buffer:
                                // next frame's rebuild reports its writer pcs as [camwrite].
                                extern std::atomic<uint32_t> g_ps2WatchLo, g_ps2WatchHi;
                                g_ps2WatchLo.store(a, std::memory_order_relaxed);
                                g_ps2WatchHi.store(a + 0x80u, std::memory_order_relaxed);
                                std::fprintf(stderr, "[upsrc2] store-watch re-aimed to 0x%08x..0x%08x\n", a, a + 0x80u);
                            }
                        if (!hits) std::fprintf(stderr, "[upsrc2] SHEET sample not in RAM (qwc=%u; staged/SPR?)\n", qwCount);
                    }
                }
                // PS2X_VIFTEX: the terrain draw DIRECTs (TEX0 tbp0 10816/10880/10944/10992)
                // exist in EE RAM, the chain walker visits their tags, offline sim parses them
                // — yet the GS never sees those TEX0s. Log when a DIRECT containing one passes
                // through here: present => loss is downstream (arbiter/GS); absent => the live
                // VIF stream desyncs before them.
                static const bool s_vt = [](){ const char *v = [](){ static const char *s_env = std::getenv("PS2X_VIFTEX"); return s_env; }(); return v && v[0] && v[0] != '0'; }();
                if (s_vt)
                {
                    static std::atomic<uint32_t> s_vtn{0};
                    uint32_t o = 0;
                    const uint32_t lim = qwCount * 16u;
                    while (o + 16u <= lim && o < 4096u)
                    {
                        uint64_t tlo, thi;
                        std::memcpy(&tlo, data + pos + o, 8); std::memcpy(&thi, data + pos + o + 8, 8);
                        o += 16u;
                        const uint32_t nloop = (uint32_t)(tlo & 0x7FFF);
                        const uint32_t flg = (uint32_t)((tlo >> 58) & 3u);
                        uint32_t nreg = (uint32_t)((tlo >> 60) & 0xFu); if (!nreg) nreg = 16u;
                        auto hit = [&](uint64_t v){
                            const uint32_t t = (uint32_t)(v & 0x3FFFu);
                            if ((t == 10816u || t == 10880u || t == 10944u || t == 10992u) && s_vtn.fetch_add(1) < 60u)
                                std::fprintf(stderr, "[viftex] DIRECT delivers TEX0 tbp0=%u (qwc=%u)\n", t, qwCount);
                        };
                        if (flg == 0u)
                        {
                            for (uint32_t l = 0; l < nloop && o + 16u <= lim; ++l)
                                for (uint32_t r = 0; r < nreg && o + 16u <= lim; ++r, o += 16u)
                                {
                                    const uint32_t d = (uint32_t)((thi >> (r * 4u)) & 0xFu);
                                    uint64_t plo, phi;
                                    std::memcpy(&plo, data + pos + o, 8); std::memcpy(&phi, data + pos + o + 8, 8);
                                    if (d == 6u || d == 7u || (d == 14u && ((phi & 0xFFu) == 6u || (phi & 0xFFu) == 7u)))
                                        hit(plo);
                                }
                        }
                        else if (flg == 1u)
                        {
                            const uint32_t total = nloop * nreg;
                            for (uint32_t i = 0; i < total && o + 8u <= lim; ++i, o += 8u)
                            {
                                const uint32_t d = (uint32_t)((thi >> ((i % nreg) * 4u)) & 0xFu);
                                if (d == 6u || d == 7u)
                                {
                                    uint64_t v; std::memcpy(&v, data + pos + o, 8);
                                    hit(v);
                                }
                            }
                            if (total & 1u) o += 8u;
                        }
                        else
                            o += nloop * 16u;
                    }
                }
                {   // [upsrc2] PS2X_UPSRC=1: guest source of the band-sheet upload — scan DIRECT
                    // payloads for A+D BITBLTBUF with DBP==10752 and report the guest src address.
                    static const bool s_u2 = [](){ const char *v = std::getenv("PS2X_UPSRC"); return v && v[0] && v[0] != '0'; }();
                    if (s_u2)
                    {
                        static std::atomic<int> s_un2{0};
                        const uint32_t lim = qwCount * 16u;
                        for (uint32_t o = 0; o + 16u <= lim && s_un2.load(std::memory_order_relaxed) < 24; o += 16u)
                        {
                            uint64_t plo, phi;
                            std::memcpy(&plo, data + pos + o, 8); std::memcpy(&phi, data + pos + o + 8, 8);
                            const uint32_t u2dbp = (uint32_t)((plo >> 32) & 0x3FFFu);
                            if ((phi & 0xFFu) == 0x50u && u2dbp == 10752u)
                            {
                                uint32_t srcG = 0u;
                                if (g_vif1QwcActive) srcG = g_vif1QwcSrcGuest + pos + o;
                                else if (data >= m_rdram && data < m_rdram + PS2_RAM_SIZE) srcG = (uint32_t)(data - m_rdram) + pos + o;
                                if (s_un2.fetch_add(1) < 24)
                                    std::fprintf(stderr, "[upsrc2] BITBLTBUF dbp=%u sbp=%u spsm=%u srcG=0x%08x qwc=%u\n",
                                                 u2dbp, (uint32_t)(plo & 0x3FFFu), (uint32_t)((plo >> 24) & 0x3Fu), srcG, qwCount);
                                // capture the CLUT payload + its guest address once: the IMAGE data follows
                                // in the VIF stream — dump the next 2KB of stream bytes with their srcG base.
                                g_upsrcArmSheet = true;   // payload arrives in the NEXT big DIRECT
                            }
                        }
                    }
                }
                {   // [shadowpass] detect the Pass-1 context in DIRECT A+D packets
                    static const bool s_sp = [](){ const char *v = std::getenv("PS2X_SHADOWPASS"); return v && v[0] && v[0] != '0'; }();
                    if (s_sp)
                    {
                        uint32_t o = 0; const uint32_t lim = qwCount * 16u;
                        while (o + 16u <= lim && o < 4096u)
                        {
                            uint64_t tlo, thi; std::memcpy(&tlo, data + pos + o, 8); std::memcpy(&thi, data + pos + o + 8, 8); o += 16u;
                            const uint32_t nloop = (uint32_t)(tlo & 0x7FFF); const uint32_t flg = (uint32_t)((tlo >> 58) & 3u);
                            uint32_t nreg = (uint32_t)((tlo >> 60) & 0xFu); if (!nreg) nreg = 16u;
                            if (flg == 0u)
                            {
                                for (uint32_t l = 0; l < nloop && o + 16u <= lim; ++l)
                                    for (uint32_t r = 0; r < nreg && o + 16u <= lim; ++r, o += 16u)
                                    {
                                        if (((thi >> (r * 4u)) & 0xFu) != 14u) continue;
                                        uint64_t plo, phi; std::memcpy(&plo, data + pos + o, 8); std::memcpy(&phi, data + pos + o + 8, 8);
                                        const uint32_t addr = (uint32_t)(phi & 0xFFu);
                                        if (addr == 0x4Cu) { g_spFrame336 = ((plo & 0xFFFFFFFFu) == 0x40150u); if (!g_spFrame336) g_spInShadow = false; }
                                        else if (addr == 0x40u && g_spFrame336 && (plo & 0xFFFFFFFFFFFFull) == 0x00fe000100fe0001ull)
                                        {
                                            g_spInShadow = true; const uint32_t n = ++g_spSets;
                                            if (n <= 4u || (n % 240u) == 0u)
                                            {
                                                std::fprintf(stderr, "[shadowpass] ctx set #%u (DIRECT qwc=%u) | since previous set: mscal=%u lastPC=0x%x unpackQw=%u kicks=%u nloopSum=%u\n",
                                                             n, qwCount, g_spMscal, g_spLastPC, g_spUnpackQw, g_spKicks, g_spLoops);
                                            }
                                            g_spMscal = 0; g_spKicks = 0; g_spLoops = 0; g_spUnpackQw = 0;
                                        }
                                    }
                            }
                            else if (flg == 1u) o += ((nloop * nreg + 1u) / 2u) * 16u;
                            else o += nloop * 16u;
                        }
                    }
                }
                const bool directHl = (opcode == VIF_DIRECTHL);
                const auto _g0 = g_vifTimeProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);
                if (g_vifTimeProf) { g_vifGifNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _g0).count(), std::memory_order_relaxed); g_vifGifN.fetch_add(1, std::memory_order_relaxed); }

                const uint32_t imageQw = gifImageQwcFromTag(data + pos, qwCount * 16u);
                if (imageQw != 0u)
                {
                    const uint32_t inlineImageQw = (qwCount > 0u) ? (qwCount - 1u) : 0u;
                    if (imageQw > inlineImageQw)
                    {
                        m_vif1PendingPath2ImageQwc = imageQw - inlineImageQw;
                        m_vif1PendingPath2DirectHl = directHl;
                    }
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            const auto _u0 = g_vifTimeProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            {
                static const bool s_ud = [](){ static const char *s_env = std::getenv("PS2X_UNPACK_DUMP"); return s_env; }() != nullptr;
                // PS2X_SEQ: compact log of EVERY UNPACK (vuAddr, cnt, dbf, q0.x) to reveal the
                // memory layout — where the matrix goes vs where the vertices land.
                if ([](){ static const char *s_env = std::getenv("PS2X_SEQ"); return s_env; }()) {
                    static std::atomic<int> s_sq{0};
                    if (s_sq.fetch_add(1) < 80) {
                        float q0x=0; if (pos+4<=sizeBytes) std::memcpy(&q0x, data+pos, 4);
                        std::fprintf(stderr, "[seq] UNPACK vuAddr=%u cnt=%u dbf=%d tops=%u q0.x=%.1f\n",
                            vuAddr, writeVectorCount, (imm&0x8000u)?1:0, vif1_regs.tops & 0x3FFu, q0x);
                    }
                }
                if (g_mtxSeq) {
                    bool z64 = true;
                    for (uint32_t b = 0; b < 64u && pos + b < sizeBytes; ++b) if (data[pos + b] != 0) { z64 = false; break; }
                    int n = g_mtxSeqN.load();
                    if (n < 0 && vuAddr == 0u && writeVectorCount >= 12u && !z64) {
                        g_mtxSeqN.store(0); n = 0;
                        std::fprintf(stderr, "[mtxseq] === ARMED on valued matrix upload ===\n");
                    }
                    if (n >= 0 && g_mtxSeqN.fetch_add(1) < 150) {
                        float q0[4] = {0,0,0,0};
                        if (pos + 16 <= sizeBytes) std::memcpy(q0, data + pos, 16);
                        std::fprintf(stderr, "[mtxseq] UNPACK addr=%u cnt=%u flg=%d tops=%u zero64=%d q0=(%.2f %.2f %.2f %.2f)\n",
                            vuAddr, writeVectorCount, (imm & 0x8000u) ? 1 : 0, vif1_regs.tops & 0x3FFu, z64 ? 1 : 0, q0[0], q0[1], q0[2], q0[3]);
                    }
                }
                // PS2X_MTXUP: is the scene's zero MVP actually UPLOADED as zero (EE computed zero)
                // or never uploaded (stays zero)? Count matrix-region uploads (vuAddr=0, cnt>=12),
                // split by whether the uploaded 64 bytes (qw0-3) are all-zero vs valued.
                if ([](){ static const char *s_env = std::getenv("PS2X_MTXUP"); return s_env; }() && vuAddr == 0u && writeVectorCount >= 12u) {
                    bool allZero = true;
                    for (uint32_t b = 0; b < 64u && pos + b < sizeBytes; ++b) if (data[pos + b] != 0) { allZero = false; break; }
                    static std::atomic<uint32_t> zc{0}, nzc{0};
                    if (allZero) zc.fetch_add(1); else nzc.fetch_add(1);
                    if (((zc.load() + nzc.load()) % 400u) == 1u)
                        std::fprintf(stderr, "[mtxup] qw0 matrix uploads: ZERO=%u VALUED=%u (cnt=%u)\n", zc.load(), nzc.load(), writeVectorCount);
                    // Where does the ZERO matrix data actually live? (rdram / scratchpad / elsewhere)
                    if (allZero) {
                        static std::atomic<uint32_t> zlog{0};
                        if ((zlog.fetch_add(1) % 3000u) < 5u) {
                            const long off = (long)(data + pos - m_rdram);
                            const int inRd = (data >= m_rdram && data < m_rdram + PS2_RAM_SIZE) ? 1 : 0;
                            std::fprintf(stderr, "[mtxup-zero] data-rdram off=0x%lx inRdram=%d ramSize=0x%x cnt=%u\n",
                                         off, inRd, (unsigned)PS2_RAM_SIZE, writeVectorCount);
                        }
                    }
                }
                // Focus on the constant-block UNPACK (vuAddr=0, cnt>=12) that carries the
                // MVP(qw0-3)+viewport(qw8-11). Log its SOURCE EE ADDRESS so we can find the
                // EE code that builds the packet (and why qw0-3 is zero).
                if (s_ud && vuAddr <= 4u)   // any UNPACK targeting the world-matrix region qw0-4
                {
                    static std::atomic<uint32_t> s_un{0};
                    if ((s_un.fetch_add(1) % 2000u) < 6u)
                    {
                        std::fprintf(stderr, "[mvpkt] vuAddr=%u cnt=%u vn=%d vl=%d | dbf=%d tops=%u base=%u ofst=%u | FULL SOURCE floats/qw:",
                            vuAddr, writeVectorCount, (int)vn, (int)vl,
                            (imm & 0x8000u) ? 1 : 0, vif1_regs.tops & 0x3FFu, vif1_regs.base & 0x3FFu, vif1_regs.ofst & 0x3FFu);
                        for (uint32_t q = 0; q < writeVectorCount && q < 12u; ++q) {
                            float f[4] = {0,0,0,0};
                            for (int k=0;k<4;k++) if (pos + q*16u + (uint32_t)(k*4+4) <= sizeBytes) std::memcpy(&f[k], data+pos+q*16u+k*4, 4);
                            std::fprintf(stderr, " q%u(%.1f,%.1f,%.1f,%.1f)", q, f[0],f[1],f[2],f[3]);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }

            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));

                    // PS2X_BONECHK: flag NaN/huge floats arriving in V4-32 unpacks (bone matrices
                    // and vertices are all |v| < ~1e5). Garbage here = the EE side computed it;
                    // clean here = the corruption happens later (VU1 microprogram side).
                    {
                        static const bool s_bc = [](){ const char *v = std::getenv("PS2X_BONECHK"); return v && v[0] && v[0] != '0'; }();
                        if (s_bc && vn == 3u && vl == 0u)
                        {
                            for (int bf = 0; bf < 4; ++bf)
                            {
                                float fv;
                                std::memcpy(&fv, &lanes[bf], 4);
                                const bool bad = std::isnan(fv) || std::fabs(fv) > 1.0e6f;
                                if (bad)
                                {
                                    static std::atomic<uint64_t> s_bn{0};
                                    const uint64_t n = s_bn.fetch_add(1) + 1;
                                    if (n <= 30 || (n % 4096u) == 0u)
                                    {
                                        // Attribute the poison word to its guest source address via the
                                        // tag-walk source map (sync-kick mode; empty map = qwc transfer).
                                        uint32_t srcGuest = 0u, srcSpr = 0u;
                                        bool srcKnown = false;
                                        if (srcVec)
                                        {
                                            const uint32_t srcOff = static_cast<uint32_t>(srcVec - data) + static_cast<uint32_t>(bf) * 4u;
                                            for (size_t mi = g_kickSrcMap.size(); mi > 0; --mi)
                                            {
                                                const auto &e = g_kickSrcMap[mi - 1];
                                                if (e[0] <= srcOff)
                                                {
                                                    srcGuest = e[1] + (srcOff - e[0]);
                                                    srcSpr = e[2];
                                                    srcKnown = true;
                                                    break;
                                                }
                                            }
                                        }
                                        std::fprintf(stderr, "[bonechk] #%llu BAD float %.3g bits=%08x at VU1 qw%u lane%d (unpack vuAddr=%u cnt=%u) src=%s0x%08x%s\n",
                                                     (unsigned long long)n, fv, lanes[bf], destVec, bf, vuAddr, writeVectorCount,
                                                     srcKnown ? (srcSpr ? "SPR:" : "EE:") : "?", srcKnown ? srcGuest : 0u,
                                                     srcKnown ? "" : " (unmapped)");
                                    }
                                    // Lock the RAM scanner onto the first exactly-repeating poison
                                    // value (|f|>1e15 => not a projection constant) so [scanbits]
                                    // reports which EE addresses hold this bit pattern.
                                    if (std::fabs(fv) > 1.0e15f || std::isnan(fv))
                                    {
                                        uint32_t expected = 0u;
                                        if (g_boneScanTarget.compare_exchange_strong(expected, lanes[bf]))
                                        {
                                            // First poison value seen: scan EE RAM for the pattern on a
                                            // background thread so we learn the writer's target address.
                                            uint8_t *ram = m_rdram;
                                            std::thread([ram]() {
                                                for (int sweep = 0; sweep < 6; ++sweep)
                                                {
                                                    std::this_thread::sleep_for(std::chrono::seconds(2));
                                                    const uint32_t tgt = g_boneScanTarget.load();
                                                    int hits = 0;
                                                    for (uint32_t off = 0; off + 4u <= PS2_RAM_SIZE && hits < 8; off += 4u)
                                                    {
                                                        uint32_t w;
                                                        std::memcpy(&w, ram + off, 4);
                                                        if (w == tgt)
                                                        {
                                                            ++hits;
                                                            std::fprintf(stderr, "[scanbits] pattern %08x at EE 0x%08x\n", tgt, off);
                                                        }
                                                    }
                                                    std::fprintf(stderr, "[scanbits] sweep %d: %d hit(s) for %08x\n", sweep, hits, tgt);
                                                }
                                            }).detach();
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // [cycletrace] PS2X_CYCLETRACE=<minframe>: log S-format and MASKED unpacks with
            // their full cycle state (cmd/CL/WL/mode/mask) — the terrain per-cell param
            // streams; decides whether fill-mode (WL>CL) semantics are in play there.
            {
                static const long s_ct = [](){ const char *v = std::getenv("PS2X_CYCLETRACE"); return v && v[0] ? std::atol(v) : -1; }();
                if (s_ct >= 0 && (vn == 0u || maskEnable))
                {
                    extern std::atomic<uint64_t> g_bt3FrameCount;
                    const long fr = (long)g_bt3FrameCount.load(std::memory_order_relaxed);
                    if (fr >= s_ct)
                    {
                        static std::atomic<int> s_cn{0};
                        if (s_cn.fetch_add(1) < 100)
                            std::fprintf(stderr, "[cycletrace] fr=%ld cmd=%02x vn=%d vl=%d m=%d vuAddr=%u cnt=%u cl=%u wl=%u mode=%u mask=%08x tops=%u\n",
                                         fr, (unsigned)((imm >> 24) & 0xFFu) | 0x60u, (int)vn, (int)vl, maskEnable ? 1 : 0,
                                         vuAddr, (unsigned)writeVectorCount, cl, wl, vif1_regs.mode & 3u, vif1_regs.mask, vif1_regs.tops & 0x3FFu);
                    }
                }
            }
            // [rowtrace] PS2X_ROWTRACE=<minframe>: log every unpack whose EFFECTIVE target is
            // VU rows 0-15 (the terrain micro's uniform block; entry 0 latches vf1-8/vf13-16
            // from there) with its guest SOURCE address -- raw chain-byte scans missed the
            // carrier, so observe at the only place effective addresses exist.
            {
                static const long s_rt = [](){ const char *v = std::getenv("PS2X_ROWTRACE"); return v && v[0] ? std::atol(v) : -1; }();
                if (s_rt >= 0 && vuAddr < 16u)
                {
                    extern std::atomic<uint64_t> g_bt3FrameCount;
                    const long fr = (long)g_bt3FrameCount.load(std::memory_order_relaxed);
                    if (fr >= s_rt)
                    {
                        uint32_t srcG = 0u;
                        if (g_vif1QwcActive) srcG = g_vif1QwcSrcGuest + pos;
                        else
                            for (size_t mi = g_kickSrcMap.size(); mi > 0; --mi)
                                if (g_kickSrcMap[mi - 1][0] <= pos)
                                { srcG = g_kickSrcMap[mi - 1][1] + (pos - g_kickSrcMap[mi - 1][0]); break; }
                        // map empty on this feed path: when `data` points into guest RAM the
                        // source is directly recoverable, no map needed.
                        if (srcG == 0u && data >= m_rdram && data < m_rdram + PS2_RAM_SIZE)
                            srcG = (uint32_t)(data - m_rdram) + pos;
                        // PS2X_ROWTRACE_CNT=<n>: log only unpacks of exactly n vectors (12 = the
                        // per-frame view-matrix upload) — big budget for a per-frame value series.
                        static const long s_rtCnt = [](){ const char *v = std::getenv("PS2X_ROWTRACE_CNT"); return v && v[0] ? std::atol(v) : -1; }();
                        if (s_rtCnt >= 0 && (long)writeVectorCount != s_rtCnt) goto rowtrace_done;
                        static std::atomic<int> s_n{0};
                        if (s_n.fetch_add(1) < (s_rtCnt >= 0 ? 4000 : 240))
                        {
                            float q0[4] = {0, 0, 0, 0};
                            if (pos + 16u <= sizeBytes) std::memcpy(q0, data + pos, 16);
                            std::fprintf(stderr, "[rowtrace] fr=%ld vuAddr=%u cnt=%u src=0x%08x q0=(%.3f %.3f %.3f %.3f)\n",
                                         fr, vuAddr, (unsigned)writeVectorCount, srcG, q0[0], q0[1], q0[2], q0[3]);
                        }
                        rowtrace_done:;
                    }
                }
            }
            {   // [row12w] PS2X_ROW12LOG=1: log every V4-32 unpack whose dest range covers row 12
                static const bool s_r12w = [](){ const char *v = std::getenv("PS2X_ROW12LOG"); return v && v[0] && v[0] != '0'; }();
                if (s_r12w && vn == 3u && vl == 0u && vuAddr <= 12u && vuAddr + writeVectorCount > 12u)
                {
                    extern std::atomic<uint64_t> g_bt3FrameCount;
                    const uint64_t fr_ = g_bt3FrameCount.load(std::memory_order_relaxed);
                    if ((fr_ % 600u) < 2u)
                    {
                        static std::atomic<uint32_t> s_n{0};
                        if (s_n.fetch_add(1) < 4000u)
                        {
                            uint32_t r12v = 0;
                            const size_t off_ = pos + (size_t)(12u - vuAddr) * 16u;
                            if (off_ + 4u <= sizeBytes) std::memcpy(&r12v, data + off_, 4);
                            std::fprintf(stderr, "[row12w] fr=%llu dest=%u cnt=%u m=%d row12src=%08x\n",
                                         (unsigned long long)fr_, vuAddr, (unsigned)writeVectorCount, maskEnable ? 1 : 0, r12v);
                        }
                    }
                }
            }
            // PS2X_KICKHIST: record this unpack in the rolling ring for spike-kick forensics.
            if (g_unpackRingEnabled() && vn == 3u && vl == 0u)
            {
                extern std::atomic<uint64_t> g_bt3FrameCount;
                uint32_t srcG = 0u;
                if (g_vif1QwcActive)
                    srcG = g_vif1QwcSrcGuest + pos;
                else
                    for (size_t mi = g_kickSrcMap.size(); mi > 0; --mi)
                        if (g_kickSrcMap[mi - 1][0] <= pos)
                        { srcG = g_kickSrcMap[mi - 1][1] + (pos - g_kickSrcMap[mi - 1][0]); break; }
                Vif1UnpackRec &r = g_unpackRing[g_unpackRingPos++ & 31u];
                r.destQw = vuAddr;
                r.cnt = writeVectorCount;
                r.srcGuest = srcG;
                r.spr = g_vif1QwcActive ? 0u : 2u; // 2 = chain (map-attributed)
                r.frame = g_bt3FrameCount.load(std::memory_order_relaxed);
            }

            // PS2X_VTXCHK: for the wedge-kick vertex buffer (vuAddr==577), dump the raw SOURCE
            // qwords from the packet vs the qwords that LANDED in VU memory, with the cycle
            // registers. Decides whether the duplicated vertices (q3==q9 in the snapshot) come
            // from the game's RAM (EE builder) or from our unpack write placement.
            {
                static const bool s_vc = [](){ const char *v = std::getenv("PS2X_VTXCHK"); return v && v[0] && v[0] != '0'; }();
                if (s_vc && vuAddr >= 577u && vuAddr <= 600u && m_vu1Data)
                {
                    static std::atomic<uint32_t> s_vn2{0};
                    if (s_vn2.fetch_add(1) < 20u)
                    {
                        std::fprintf(stderr, "[vtxchk] vuAddr=%u num=%u srcVecs=%u cl=%u wl=%u mode=%u maskEn=%d bytes/vec=%u\n",
                                     vuAddr, writeVectorCount, sourceVectorCount, cl, wl,
                                     (unsigned)vif1_regs.mode, maskEnable ? 1 : 0, bytesPerVector);
                        for (uint32_t q = 0; q < 14u && q < sourceVectorCount; ++q)
                        {
                            float f[4] = {0, 0, 0, 0};
                            const uint32_t sb = q * bytesPerVector;
                            if (pos + sb + bytesPerVector <= sizeBytes) std::memcpy(f, data + pos + sb, bytesPerVector > 16u ? 16u : bytesPerVector);
                            float l[4];
                            std::memcpy(l, m_vu1Data + ((vuAddr + q) & 0x3FFu) * 16u, 16);
                            std::fprintf(stderr, "  q%-2u src=(%.4g %.4g %.4g %.4g) landed=(%.4g %.4g %.4g %.4g)\n",
                                         q, f[0], f[1], f[2], f[3], l[0], l[1], l[2], l[3]);
                        }
                    }
                }
            }
            // PS2X_MVPCHK: validate what actually LANDED in VU1 memory (post-mask/mode) for the
            // two packet families that drive characters: the constants block (vuAddr=0, >=12qw:
            // MVP at qw0-3) and the bone-matrix block (34 vectors). Logs frame + EE source, so
            // blink frames can be correlated with degenerate constants and hair with bad bones.
            {
                static const bool s_mc = [](){ const char *v = std::getenv("PS2X_MVPCHK"); if (v && v[0] && v[0] != '0') return true;
                                               const char *s = std::getenv("PS2X_SKIP_DEGEN"); return s && s[0] && s[0] != '0'; }();
                if (s_mc && vn == 3u && vl == 0u && m_vu1Data)
                {
                    extern std::atomic<uint64_t> g_bt3FrameCount;
                    const uint64_t frame = g_bt3FrameCount.load(std::memory_order_relaxed);
                    auto srcOf = [&](uint32_t payloadByteOff) -> uint32_t {
                        const uint32_t srcOff = pos + payloadByteOff;
                        if (g_vif1QwcActive) // non-chain transfer: data IS guest memory at the qwc base
                            return g_vif1QwcSrcGuest + srcOff;
                        for (size_t mi = g_kickSrcMap.size(); mi > 0; --mi)
                            if (g_kickSrcMap[mi - 1][0] <= srcOff)
                                return g_kickSrcMap[mi - 1][1] + (srcOff - g_kickSrcMap[mi - 1][0]);
                        return 0u;
                    };
                    if (vuAddr == 0u && writeVectorCount >= 12u)
                    {
                        extern thread_local uint32_t g_last13at0; // 1=healthy 2=degenerate
                        float m[16];
                        std::memcpy(m, m_vu1Data, 64);
                        float amax = 0.0f; bool bad = false;
                        for (int i = 0; i < 16; ++i)
                        {
                            if (std::isnan(m[i])) bad = true;
                            const float a = std::fabs(m[i]);
                            if (a > amax) amax = a;
                        }
                        const bool degen = bad || amax < 1.0e-4f || amax > 1.0e7f;
                        if (degen)
                        {
                            static std::atomic<uint32_t> s_mn{0};
                            const uint32_t n = s_mn.fetch_add(1) + 1u;
                            if (n <= 40 || (n % 512u) == 0u)
                                std::fprintf(stderr, "[mvpchk] #%u frame=%llu DEGENERATE mvp amax=%.3g nan=%d cnt=%u src=EE:0x%08x | row0: %.3g %.3g %.3g %.3g\n",
                                             n, (unsigned long long)frame, amax, bad ? 1 : 0, writeVectorCount, srcOf(0), m[0], m[1], m[2], m[3]);
                            // First occurrences: dump the raw VIF stream around the unpack so we can
                            // SEE whether the parser is in sync (sane codes before `|`) or desynced.
                            if (n <= 6)
                            {
                                const uint32_t back = (pos >= 96u) ? 96u : pos;
                                const uint32_t w0 = (pos - back) / 4u;
                                const uint32_t total = sizeBytes / 4u;
                                const uint32_t *ws = reinterpret_cast<const uint32_t *>(data);
                                std::fprintf(stderr, "[mvpstream] pos=%u words:", pos);
                                for (uint32_t i = w0; i < w0 + 40u && i < total; ++i)
                                    std::fprintf(stderr, "%s%08x", (i * 4u == pos) ? " | " : " ", ws[i]);
                                std::fprintf(stderr, "\n");
                            }
                        }
                        static const bool s_skip = [](){ const char *v = std::getenv("PS2X_SKIP_DEGEN"); return v && v[0] && v[0] != '0'; }();
                        if (s_skip)
                            g_degenSuppress.store(degen, std::memory_order_relaxed);
                        g_last13at0 = degen ? 2u : 1u;
                    }
                    else if (writeVectorCount == 34u)
                    {
                        // Bone block: 8 matrices in qw2..33 (2 header qw). Rotation rows' xyz
                        // should stay modest; |v|>100 or NaN = corrupt bone.
                        for (uint32_t g2 = 0; g2 < 8u; ++g2)
                        {
                            const uint32_t baseQw = (vuAddr + 2u + g2 * 4u) & 0x3FFu;
                            float mm[12];
                            std::memcpy(mm, m_vu1Data + baseQw * 16u, 48); // rows 0-2
                            bool badB = false;
                            for (int i = 0; i < 12 && !badB; ++i)
                                if (std::isnan(mm[i]) || (((i & 3) != 3) && std::fabs(mm[i]) > 100.0f))
                                    badB = true;
                            if (badB)
                            {
                                static std::atomic<uint32_t> s_bb{0};
                                const uint32_t n = s_bb.fetch_add(1) + 1u;
                                if (n <= 40 || (n % 1024u) == 0u)
                                    std::fprintf(stderr, "[bonemtx] #%u frame=%llu BAD bone m%u at vu qw%u src=EE:0x%08x | r0: %.3g %.3g %.3g r1: %.3g %.3g %.3g\n",
                                                 n, (unsigned long long)frame, g2, baseQw, srcOf((2u + g2 * 4u) * 16u),
                                                 mm[0], mm[1], mm[2], mm[4], mm[5], mm[6]);
                                break;
                            }
                        }
                    }
                }
            }
            pos += totalBytes;

            if (g_vifTimeProf)
            {
                g_vifUnpackNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _u0).count(), std::memory_order_relaxed);
                g_vifUnpackVecs.fetch_add(writeVectorCount, std::memory_order_relaxed);
                static std::atomic<uint64_t> s_lastNs{0};
                static std::chrono::steady_clock::time_point s_last = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - s_last).count();
                if (dt >= 1.0)
                {
                    std::cerr << "[vifunpack] " << (g_vifUnpackNs.load() / 1e6 / dt) << "ms/s vecs/s="
                              << (uint64_t)(g_vifUnpackVecs.load() / dt) << std::endl;
                    g_vifUnpackNs = 0; g_vifUnpackVecs = 0; s_last = now;
                }
            }

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }

    if (g_vifTimeProf)
    {
        static std::mutex s_bm; static std::chrono::steady_clock::time_point s_bl = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(s_bm);
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_bl).count();
        if (dt >= 1.0)
        {
            std::cerr << "[vifbreak] mscal(VU1)=" << (g_vifMscalNs.load()/1e6/dt) << "ms/s n=" << (uint64_t)(g_vifMscalN.load()/dt)
                      << " | mscnt(VU1)=" << (g_vifMscntNs.load()/1e6/dt) << "ms/s n=" << (uint64_t)(g_vifMscntN.load()/dt)
                      << " | gif=" << (g_vifGifNs.load()/1e6/dt) << "ms/s n=" << (uint64_t)(g_vifGifN.load()/dt)
                      << " | unpack=" << (g_vifUnpackNs.load()/1e6/dt) << "ms/s vecs/s=" << (uint64_t)(g_vifUnpackVecs.load()/dt) << "\n";
            g_vifMscalNs=0; g_vifMscalN=0; g_vifMscntNs=0; g_vifMscntN=0; g_vifGifNs=0; g_vifGifN=0; g_vifUnpackNs=0; g_vifUnpackVecs=0; s_bl=std::chrono::steady_clock::now();
        }
    }
}
