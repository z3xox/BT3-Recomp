#include "Common.h"
#include "SIF.h"
#include "../Syscalls/RPC.h"

#include <map>

void bt3NoteSeBankBlob(const uint8_t *data, uint32_t size);
void bt3NoteSeBankHeader(uint32_t dst, const uint8_t *data, uint32_t size);

namespace ps2_stubs
{
    // [adxrate] Set in CD.cpp from the ADX file header; 0 until a stream declares its rate.
    extern std::atomic<uint32_t> g_detectedAdxRate;
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = readStackU32(rdram, ctx, 16);
        const uint32_t size = readStackU32(rdram, ctx, 20);
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                uint8_t *dst = getMemPtr(rdram, dstAddr + i);
                if (!src || !dst)
                {
                    break;
                }
                *dst = *src;
            }
        }

        setReturnS32(ctx, 1);
    }

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::mutex g_sifHeapMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        std::unordered_map<uint32_t, uint32_t> g_sifCmdHandlers;
        std::map<uint32_t, uint32_t> g_sifHeapAllocations;
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        constexpr uint32_t kSifRegBootStatus = 0x4u;
        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;
        constexpr uint32_t kSifBootReadyMask = 0x00020000u;

        void seedDefaultSifRegsLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;

            g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x2u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                seedDefaultSifRegsLocked();
            }
        } g_sifStateInitializer;

        uint32_t allocateSifDmaTransferId()
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            return id;
        }

        uint32_t alignIopHeapSize(uint32_t size)
        {
            return (size + (kIopHeapAlign - 1u)) & ~(kIopHeapAlign - 1u);
        }

        uint32_t allocateSifHeapBlock(uint32_t requestSize)
        {
            const uint32_t alignedSize = alignIopHeapSize(requestSize);
            if (alignedSize == 0u)
            {
                return 0u;
            }

            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            uint32_t candidate = kIopHeapBase;
            for (const auto &[addr, size] : g_sifHeapAllocations)
            {
                if (candidate + alignedSize <= addr)
                {
                    break;
                }

                const uint32_t blockEnd = alignIopHeapSize(addr + size);
                if (blockEnd > candidate)
                {
                    candidate = blockEnd;
                }
            }

            if (candidate < kIopHeapBase || candidate + alignedSize > kIopHeapLimit)
            {
                return 0u;
            }

            g_sifHeapAllocations[candidate] = alignedSize;
            g_iopHeapNext = candidate + alignedSize;
            return candidate;
        }

        bool freeSifHeapBlock(uint32_t addr)
        {
            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            const auto it = g_sifHeapAllocations.find(addr);
            if (it == g_sifHeapAllocations.end())
            {
                return false;
            }

            g_sifHeapAllocations.erase(it);
            if (g_sifHeapAllocations.empty())
            {
                g_iopHeapNext = kIopHeapBase;
            }
            return true;
        }

        void resetSifHeapState()
        {
            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            g_sifHeapAllocations.clear();
            g_iopHeapNext = kIopHeapBase;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < 0x20000000u)
            {
                return true;
            }

            if (addr >= 0x20000000u && addr < 0x40000000u)
            {
                return true;
            }

            if (addr >= 0x80000000u && addr < 0xC0000000u)
            {
                return true;
            }

            return false;
        }

        bool canCopyGuestByteRange(const uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }

            if (sizeBytes == 0u)
            {
                return true;
            }

            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t srcByteAddr = srcAddr + i;
                const uint32_t dstByteAddr = dstAddr + i;

                if (!isCopyableGuestAddress(srcByteAddr) || !isCopyableGuestAddress(dstByteAddr))
                {
                    return false;
                }

                const uint8_t *src = getConstMemPtr(rdram, srcByteAddr);
                const uint8_t *dst = getConstMemPtr(rdram, dstByteAddr);
                if (!src || !dst)
                {
                    return false;
                }
            }

            return true;
        }

        // EE -> IOP transfer. sceSifSetDma's destination is an IOP address, which lives in its own
        // 2 MB space (PS2Memory::getIOPRAM()) and overlaps EE RAM numerically -- so it must never
        // be written through `rdram`. The role of the parameter, not its value, is what identifies
        // it: ::src is always EE, ::dest is always IOP.
        bool copyEeToIop(uint8_t *rdram, uint8_t *iopRam, uint32_t dstIop, uint32_t srcEe, uint32_t sizeBytes)
        {
            if (sizeBytes == 0u)
            {
                return true;
            }
            if (!rdram || !iopRam)
            {
                return false;
            }

            const uint32_t dst = dstIop & 0x1FFFFFFFu;
            if (dst >= kIopRamSize || sizeBytes > (kIopRamSize - dst))
            {
                return false; // outside IOP RAM: a bad descriptor, not something to write anywhere
            }

            const uint32_t src = srcEe & 0x1FFFFFFFu;
            if (src < PS2_RAM_SIZE && sizeBytes <= (PS2_RAM_SIZE - src))
            {
                std::memcpy(iopRam + dst, rdram + src, sizeBytes); // main RAM: contiguous
                return true;
            }

            // Scratchpad or any other mapped source: fall back to the per-byte accessor.
            for (uint32_t i = 0; i < sizeBytes; ++i)
            {
                const uint8_t *q = getConstMemPtr(rdram, srcEe + i);
                if (!q)
                {
                    return false;
                }
                iopRam[dst + i] = *q;
            }
            return true;
        }

        bool copyGuestByteRange(uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t sizeBytes)
        {
            if (!canCopyGuestByteRange(rdram, dstAddr, srcAddr, sizeBytes))
            {
                return false;
            }

            if (sizeBytes == 0u)
            {
                return true;
            }

            const uint64_t srcBegin = srcAddr;
            const uint64_t srcEnd = srcBegin + static_cast<uint64_t>(sizeBytes);
            const uint64_t dstBegin = dstAddr;
            const bool copyBackward = (dstBegin > srcBegin) && (dstBegin < srcEnd);

            if (copyBackward)
            {
                for (uint32_t i = sizeBytes; i > 0u; --i)
                {
                    const uint32_t index = i - 1u;
                    const uint8_t *src = getConstMemPtr(rdram, srcAddr + index);
                    uint8_t *dst = getMemPtr(rdram, dstAddr + index);
                    if (!src || !dst)
                    {
                        return false;
                    }
                    *dst = *src;
                }
                return true;
            }

            for (uint32_t i = 0; i < sizeBytes; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                uint8_t *dst = getMemPtr(rdram, dstAddr + i);
                if (!src || !dst)
                {
                    return false;
                }
                *dst = *src;
            }
            return true;
        }
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        resetSifHeapState();
    }

    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = handler;
        setReturnS32(ctx, 0);
    }

    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t reqSize = getRegU32(ctx, 4);
        setReturnU32(ctx, allocateSifHeapBlock(reqSize));
    }

    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t size = getRegU32(ctx, 5);
        setReturnU32(ctx, allocateSifHeapBlock(size));
    }

    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifCheckStatRpc(rdram, ctx, runtime);
    }

    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        (void)getRegU32(ctx, 4); // trid

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        setReturnS32(ctx, -1);
    }

    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        setReturnS32(ctx, 0);
    }

    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, freeSifHeapBlock(addr) ? 0 : -1);
    }

    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, freeSifHeapBlock(addr) ? 0 : -1);
    }

    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;

        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        if (sizeSigned <= 0)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        ps2_syscalls::prepareSoundDriverStatusTransfer(rdram, srcAddr, size);

        if (!copyGuestByteRange(rdram, dstAddr, srcAddr, size))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
        }

        ps2_syscalls::finalizeSoundDriverStatusTransfer(rdram, srcAddr, dstAddr, size);

        setReturnS32(ctx, 0);
    }

    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, value);
    }

    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resetSifHeapState();
        setReturnS32(ctx, 0);
    }

    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifInitRpc(rdram, ctx, runtime);
    }

    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElf(rdram, ctx, runtime);
    }

    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime);
    }

    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRegisterRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpcQueue(rdram, ctx, runtime);
    }

    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDChain(rdram, ctx, runtime);
    }

    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDma(rdram, ctx, runtime);
    }

    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;

        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        const uint32_t listAddr = getRegU32(ctx, 4);
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " list=0x" << listAddr
                      << " count=" << std::dec << count
                      << std::endl;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                const uint32_t src = READ32(desc + 0);
                const uint32_t dst = READ32(desc + 4);
                const uint32_t size = READ32(desc + 8);
                const uint32_t attr = READ32(desc + 12);

                std::cerr << "[sceSifSetDma:DESC] i=" << i
                          << " src=0x" << std::hex << src
                          << " dst=0x" << dst
                          << " size=0x" << size
                          << " attr=0x" << attr
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            }
        });

        if (!dmatAddr || count == 0u || count > 32u)
        {
            setReturnS32(ctx, 0);
            return;
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                break;
            }
            if (!canCopyGuestByteRange(rdram, xfer.dest, xfer.src, sizeBytes))
            {
                ok = false;
                break;
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                // EE -> IOP. The destination lives in IOP RAM, a separate 2 MB space; writing it
                // through `rdram` (as this did) lands on unrelated EE memory, which is how the
                // game's sound-bank uploads used to overwrite its own live heap objects.
                if (!copyEeToIop(rdram,
                                 runtime ? runtime->memory().getIOPRAM() : nullptr,
                                 xfer.dest,
                                 xfer.src,
                                 static_cast<uint32_t>(xfer.size)))
                {
                    static std::atomic<uint32_t> bad{0};
                    if (bad.fetch_add(1) < 8u)
                        std::fprintf(stderr,
                                     "[sifdma] transfer rejected: dest=0x%x (IOP) src=0x%x size=%d\n",
                                     xfer.dest, xfer.src, (int)xfer.size);
                    ok = false;
                    break;
                }

                ps2_syscalls::noteDtxSifDmaTransfer(
                    rdram,
                    xfer.src,
                    xfer.dest,
                    static_cast<uint32_t>(xfer.size));

                // PS2X_SNDPLAY=1: feed the streamed PCM to the host audio device. BT3 sends
                // already-decoded 16-bit mono PCM to the IOP in double-buffered slots, so there
                // is no libsd voice command to intercept -- this is the only place the audio
                // actually passes through. streamId = dst >> 14 cleanly separates the two stereo
                // banks (0x140..0x3a40 -> 0, 0x4240..0x7b40 -> 1) and keeps unrelated streams
                // (0x20940+) apart. Requires the PS2X_SNDPUMP buffer return, or the game only
                // ever sends one buffer per slot.
                // NOTE: this is a FEATURE, so it must sit OUTSIDE the PS2X_SIFDMA debug gate --
                // nesting it there (as PS2X_SNDDUMP is) silently disables audio unless the
                // diagnostic flag also happens to be set.
                {
                    // Default ON; PS2X_SNDPLAY=0 opts out. Must agree with the registration gate
                    // in game_overrides.cpp -- if this one stayed opt-in the hooks would install
                    // and no PCM would ever reach the device.
                    static const bool s_play = []() {
                        const char *v = std::getenv("PS2X_SNDPLAY");
                        return !(v && v[0] == '0');
                    }();
                    // Streaming PCM goes to the fixed low ring area; anything from the IOP HEAP is a
                    // bank/sequence upload, not audio. This used to test `dest < 0x100000`, which
                    // only worked because the heap sat at 0x1a00000 -- once the heap moved to real
                    // IOP addresses the bank uploads fell inside that window and got played as raw
                    // PCM (35 KB of ADPCM straight to the device). Gate on the heap boundary so the
                    // split stays correct wherever the heap lives.
                    {   // [sndflow] Where does streamed PCM stop? Rings get registered on a bad
                        // boot but no stream ever appears, so the transfer either never arrives,
                        // is routed away by the heap gate, or is dropped for being too small.
                        static const bool s_fl = [](){ const char *v = std::getenv("PS2X_SNDFLOW"); return v && v[0] && v[0] != '0'; }();
                        if (s_fl)
                        {
                            static unsigned long nTot = 0, nBelow = 0, nSmall = 0;
                            static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                            ++nTot;
                            if (xfer.dest < kIopHeapBase) { ++nBelow; if (xfer.size < 256) ++nSmall; }
                            const auto now = std::chrono::steady_clock::now();
                            if (std::chrono::duration<double>(now - t0).count() >= 5.0)
                            {
                                std::fprintf(stderr, "[sndflow] 5s: %lu transfers, %lu below heap (audio), %lu of those too small; last dest=0x%x size=%u heapBase=0x%x play=%d\n",
                                             nTot, nBelow, nSmall, (unsigned)xfer.dest, (unsigned)xfer.size,
                                             (unsigned)kIopHeapBase, s_play ? 1 : 0);
                                nTot = nBelow = nSmall = 0; t0 = now;
                            }
                        }
                    }
                    {   // [sndflow] Where does streamed PCM stop? On a bad boot the rings get
                        // registered but no stream ever appears, so the transfer either never
                        // arrives, is routed away by the heap gate, or is dropped for being small.
                        static const bool s_fl = [](){ const char *v = std::getenv("PS2X_SNDFLOW"); return v && v[0] && v[0] != '0'; }();
                        if (s_fl)
                        {
                            static unsigned long nTot = 0, nBelow = 0, nSmall = 0;
                            static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                            ++nTot;
                            if (xfer.dest < kIopHeapBase) { ++nBelow; if (xfer.size < 256) ++nSmall; }
                            const auto now = std::chrono::steady_clock::now();
                            if (std::chrono::duration<double>(now - t0).count() >= 5.0)
                            {
                                std::fprintf(stderr, "[sndflow] 5s: %lu transfers, %lu below heap (audio), %lu too small; last dest=0x%x size=%u heapBase=0x%x play=%d\n",
                                             nTot, nBelow, nSmall, (unsigned)xfer.dest, (unsigned)xfer.size,
                                             (unsigned)kIopHeapBase, s_play ? 1 : 0);
                                nTot = nBelow = nSmall = 0; t0 = now;
                            }
                        }
                    }
                    if (s_play && runtime && xfer.dest < kIopHeapBase)
                    {
                        // [adxrate] Prefer what the stream's own ADX header declared (see
                        // noteAdxHeaderIfPresent in CD.cpp). The old fixed 24000 was half the
                        // opening movie's real 48000, so its song played an octave low at half
                        // speed and dragged the FMV video down with it. Falls back to 24000 for
                        // any stream we never saw a header for, so nothing that already sounded
                        // right changes. PS2X_SNDRATE still forces a value for A/B testing.
                        static const uint32_t s_rateOverride = []() -> uint32_t {
                            if (const char *v = std::getenv("PS2X_SNDRATE"))
                            {
                                const long n = std::strtol(v, nullptr, 10);
                                if (n > 0) return static_cast<uint32_t>(n);
                            }
                            return 0u;
                        }();
                        uint32_t s_rate = s_rateOverride;
                        if (s_rate == 0u)
                            s_rate = g_detectedAdxRate.load(std::memory_order_relaxed);
                        if (s_rate == 0u)
                            s_rate = 24000u;
                        // NOT `dest >> 14`: the rings are 16KB but 0x100-staggered, so each
                        // one's tail falls in the next bucket and its audio would be spliced
                        // into the neighbouring stream. Resolve against the registered spans.
                        const uint32_t streamId = runtime->audioBackend().streamIdForAddress(
                            static_cast<uint32_t>(xfer.dest));
                        const uint8_t *p = (xfer.size >= 256)
                                               ? getConstMemPtr(rdram, xfer.src)
                                               : nullptr;
                        if (p)
                        {
                            runtime->audioBackend().onStreamPcm(
                                streamId,
                                reinterpret_cast<const int16_t *>(p),
                                static_cast<uint32_t>(xfer.size) / 2u,
                                s_rate);
                        }
                        else
                        {
                            // Skipped payload still occupied a slot in the guest's ring. The
                            // IOP-side consumer hands ring space back at the rate the DEVICE
                            // plays it, so bytes that never reach the device would never be
                            // returned and the ring would clog. Book them as consumed instead.
                            runtime->audioBackend().noteStreamGap(
                                streamId, static_cast<uint32_t>(xfer.size));
                        }
                    }
                }

                // [sndbank] PS2X_SNDBANK=1. Capture the system-SE sample banks so their format
                // can be decoded offline. Menu blips and hit sounds are played by the IOP out of
                // these, driven by the RPC command (rpcNum 0xD) the EE sends per press -- that
                // whole path is confirmed working; only the IOP side is missing. The banks arrive
                // here as ordinary SIF DMA, so they can be grabbed without touching the 1.4 GB
                // AFS: headers land at 0x1aa4800+ with byte-swapped "SCEI"/"Vers" magic, the
                // sample data at 0x1a00000. Files are written next to the runner.
                {
                    static const bool s_bank = []() {
                        const char *v = std::getenv("PS2X_SNDBANK");
                        return v && v[0] && v[0] != '0';
                    }();
                    const uint32_t dst = static_cast<uint32_t>(xfer.dest);
                    // Snapshot the SE sample blobs regardless of the capture flag: the staging
                    // address is reused per bank, so reading them back later sees only the last
                    // upload. Defined in game_overrides.cpp.
                    if (dst == kIopHeapBase)
                    {
                        if (const uint8_t *p = getConstMemPtr(rdram, xfer.src))
                            bt3NoteSeBankBlob(p, static_cast<uint32_t>(xfer.size));
                    }
                    else if (dst > kIopHeapBase && dst < kIopHeapBase + 0x100000u)
                    {
                        // Bank headers land above the sample staging area, one per bank ahead of
                        // its blob, so recording them keeps bank slot N paired with blob N.
                        if (const uint8_t *p = getConstMemPtr(rdram, xfer.src))
                            bt3NoteSeBankHeader(dst, p, static_cast<uint32_t>(xfer.size));
                    }
                    // [se] PS2X_SELOG=1 -- log EVERY transfer into the IOP sound region, not
                    // just the one staging address we snapshot. Only two banks load at boot, so
                    // sePlay maps bank 1/2 onto those two blobs; if fight effects live in banks
                    // loaded later (or at a different staging address) this is what shows it.
                    // Also flags whether a transfer looks like a SCEI bank header.
                    {
                        static const bool s_selog = []() {
                            const char *v = std::getenv("PS2X_SELOG");
                            return v && v[0] && v[0] != '0';
                        }();
                        if (s_selog && dst >= kIopHeapBase && dst < kIopHeapBase + 0x100000u)
                        {
                            const uint8_t *p = getConstMemPtr(rdram, xfer.src);
                            const bool scei = p && xfer.size >= 4u && p[0] == 'I' && p[1] == 'E' &&
                                              p[2] == 'C' && p[3] == 'S';
                            std::fprintf(stderr, "[se] sound-region upload dst=0x%x size=%u%s\n",
                                         dst, static_cast<uint32_t>(xfer.size),
                                         scei ? "  <- SCEI header" : "");
                        }
                    }
                    if (s_bank && dst >= kIopHeapBase && dst < kIopHeapBase + 0x100000u)
                    {
                        char path[64];
                        std::snprintf(path, sizeof(path), "sndbank_%08x.bin", dst);
                        if (const uint8_t *p = getConstMemPtr(rdram, xfer.src))
                        {
                            // Append: the same destination can be written more than once.
                            if (std::FILE *f = std::fopen(path, "ab"))
                            {
                                std::fwrite(p, 1, static_cast<size_t>(xfer.size), f);
                                std::fclose(f);
                            }
                            std::fprintf(stderr, "[sndbank] captured dst=0x%x size=%u -> %s\n",
                                         dst, static_cast<uint32_t>(xfer.size), path);
                        }
                    }
                }

                // [sifother] PS2X_SIFOTHER=1. Combat SFX have been measured NOT to be streamed
                // PCM (rings 4/10 get one burst at fight load and nothing per hit) and NOT to
                // touch the streamed-sound player (zero lifecycle events during a punch burst).
                // So either a hit emits a command on some other channel, or nothing leaves the
                // EE at all -- and those two answers need completely different work. The ring
                // transfers are the bulk of SIF traffic and are already understood, so log
                // everything EXCEPT them: whatever accompanies a punch will stand out.
                {
                    static const bool s_other = []() {
                        const char *v = std::getenv("PS2X_SIFOTHER");
                        return v && v[0] && v[0] != '0';
                    }();
                    if (s_other && runtime)
                    {
                        const uint32_t dst = static_cast<uint32_t>(xfer.dest);
                        // A ring transfer is one whose destination resolves to a registered
                        // ring; streamIdForAddress falls back to the shift split, so also
                        // require the low-memory range the rings live in.
                        const bool isRing = dst < 0x40000u;
                        if (!isRing)
                        {
                            static std::atomic<uint32_t> s_o{0};
                            const uint32_t k = s_o.fetch_add(1);
                            if (k < 400u)
                            {
                                char sig[52] = {0};
                                if (const uint8_t *p = getConstMemPtr(rdram, xfer.src))
                                    for (int i = 0; i < 16; ++i)
                                        std::snprintf(sig + i * 3, 4, "%02x ", p[i]);
                                std::fprintf(stderr, "[sifother] #%u dst=0x%x size=%u src=0x%x [%s]\n",
                                             k + 1u, dst, static_cast<uint32_t>(xfer.size),
                                             xfer.src, sig);
                            }
                        }
                    }
                }

                // [sifdma] PS2X_SIFDMA=1. BT3's EE sound engine runs play requests to
                // completion but emits no URPC after init; DTX's other IOP channel is SIF
                // DMA, so log the traffic to see whether audio commands/data go out this
                // way instead. Cheap counter + first-N detail.
                static const bool s_dmaLog = []() {
                    const char *v = std::getenv("PS2X_SIFDMA");
                    return v && v[0] && v[0] != '0';
                }();
                if (s_dmaLog)
                {
                    static std::atomic<uint32_t> s_n{0};
                    const uint32_t k = s_n.fetch_add(1);
                    if (k < 24u || (k % 500u) == 0u)
                    {
                        // Transfers to a small destination are SPU2-side voice memory. Sample
                        // the payload and test it for PS2 ADPCM (VAG) structure: 16-byte
                        // blocks where byte1 is a loop-flag in 0..7. If that holds, this is
                        // the audio the game is streaming, and rendering it is what silence
                        // actually needs.
                        char sig[80] = {0};
                        int adpcmBlocks = 0, testedBlocks = 0;
                        if (const uint8_t *p = getConstMemPtr(rdram, xfer.src))
                        {
                            const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(xfer.size), 512u);
                            for (uint32_t b = 0; b + 16u <= n; b += 16u)
                            {
                                ++testedBlocks;
                                if (p[b + 1u] <= 7u) ++adpcmBlocks;
                            }
                            for (int i = 0; i < 16 && static_cast<uint32_t>(i) < n; ++i)
                                std::snprintf(sig + i * 3, 4, "%02x ", p[i]);
                        }
                        std::fprintf(stderr, "[sifdma] #%u src=0x%x dst=0x%x size=%u adpcm=%d/%d ra=0x%x [%s]\n",
                                     k + 1u, xfer.src, xfer.dest,
                                     static_cast<uint32_t>(xfer.size), adpcmBlocks, testedBlocks,
                                     getRegU32(ctx, 31), sig);
                    }

                    // PS2X_SNDDUMP=1: append the payload of each streaming transfer (small
                    // IOP destination = the double-buffered audio slots) to one raw file per
                    // destination, so the captured stream can be auditioned as PCM. Proves
                    // whether the audio content is intact before building live playback.
                    static const bool s_dump = []() {
                        const char *v = std::getenv("PS2X_SNDDUMP");
                        return v && v[0] && v[0] != '0';
                    }();
                    if (s_dump && xfer.dest < 0x100000u && xfer.size >= 256)
                    {
                        if (const uint8_t *p = getConstMemPtr(rdram, xfer.src))
                        {
                            char path[256];
                            std::snprintf(path, sizeof(path),
                                          "/home/z3/Desktop/bt3/work/snddump_%06x.raw",
                                          xfer.dest);
                            if (FILE *f = std::fopen(path, "ab"))
                            {
                                std::fwrite(p, 1, static_cast<size_t>(xfer.size), f);
                                std::fclose(f);
                            }
                        }
                    }
                }
            }
        }

        if (!ok)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifSetDma failed dmat=0x" << std::hex << dmatAddr
                              << " count=0x" << count
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, 0);
            return;
        }

        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);

        setReturnS32(ctx, static_cast<int32_t>(allocateSifDmaTransferId()));
    }

    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifSetRpcQueue(rdram, ctx, runtime);
    }

    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }
}
