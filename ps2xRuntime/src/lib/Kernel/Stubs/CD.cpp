#include <cstring>
#include "ps2_compat.h"
#include <atomic>
#include <cstdio>
#include "Common.h"
#include "CD.h"
#include "MPEG.h"

namespace ps2_stubs
{

    namespace
    {
        uint32_t g_cdStReadTraceCount = 0u;
    }


    CdDebugSnapshot getCdDebugSnapshot()
    {
        CdDebugSnapshot snapshot{};
        snapshot.initialized = g_cdInitialized;
        snapshot.lastError = g_lastCdError;
        snapshot.mode = g_cdMode;
        snapshot.streamingLbn = g_cdStreamingLbn;
        snapshot.streamingEndLbn = g_cdStreamingEndLbn;
        snapshot.nextPseudoLbn = g_nextPseudoLbn;
        snapshot.imageSizeBytes = g_cdImageSizeBytes;
        snapshot.imageSizeValid = g_cdImageSizeValid;
        snapshot.cdRoot = getCdRootPath();
        snapshot.cdImage = getCdImagePath();
        snapshot.imageSizePath = g_cdImageSizePath;
        snapshot.leafIndexRoot = g_cdLeafIndexRoot;
        snapshot.leafIndexBuilt = g_cdLeafIndexBuilt;
        snapshot.leafIndexCount = g_cdLeafIndex.size();
        snapshot.loosePathIndexCount = g_cdLoosePathIndex.size();

        snapshot.files.reserve(g_cdFilesByKey.size());
        for (const auto &[key, entry] : g_cdFilesByKey)
        {
            CdDebugFileEntry row{};
            row.key = key;
            row.hostPath = entry.hostPath;
            row.sizeBytes = entry.sizeBytes;
            row.baseLbn = entry.baseLbn;
            row.sectors = entry.sectors;
            snapshot.files.push_back(std::move(row));
        }
        std::sort(snapshot.files.begin(), snapshot.files.end(), [](const CdDebugFileEntry &a, const CdDebugFileEntry &b)
        {
            return a.baseLbn < b.baseLbn;
        });
        return snapshot;
    }

    // [adxrate] The streaming-PCM rate used to be one hardcoded 24000 "confirmed by ear", which is
    // exactly half of what the opening movie's ZS3USOP.ADX declares -- so its song played an octave
    // low at half speed, and because the movie sequencer paces the video to the audio clock, the
    // FMV ran slow with it. ADX is self-describing, so read the rate off the file instead of
    // guessing: magic 0x8000, then sample rate as a big-endian u32 at +0x08 (channels at +0x07).
    // A sector read whose destination begins with a valid ADX header is the start of a stream;
    // remember what it said and let the audio path use it. Anything we never see a header for
    // keeps the previous default, so this cannot regress streams that were already correct.
    std::atomic<uint32_t> g_detectedAdxRate{0u};
    std::atomic<uint32_t> g_detectedAdxChannels{0u};

    void noteAdxHeaderIfPresent(const uint8_t *rdram, uint32_t dest, uint32_t sectors)
    {
        if (sectors == 0u)
            return;
        const uint32_t offset = dest & PS2_RAM_MASK;
        if (static_cast<uint64_t>(offset) + 16u > PS2_RAM_SIZE)
            return;
        const uint8_t *p = rdram + offset;
        if (p[0] != 0x80u || p[1] != 0x00u) // ADX magic
            return;

        const uint8_t channels = p[7];
        const uint32_t rate = (static_cast<uint32_t>(p[8]) << 24) | (static_cast<uint32_t>(p[9]) << 16) |
                              (static_cast<uint32_t>(p[10]) << 8) | static_cast<uint32_t>(p[11]);
        // Guard against a random block that merely starts 0x8000.
        if (channels == 0u || channels > 2u || rate < 8000u || rate > 48000u)
            return;

        const uint32_t prev = g_detectedAdxRate.exchange(rate);
        g_detectedAdxChannels.store(channels);
        if (prev != rate)
            std::cerr << "[adxrate] ADX header: " << rate << " Hz, " << static_cast<int>(channels)
                      << " ch (was " << (prev ? prev : 0u) << ")" << std::endl;
    }

    void sceCdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4); // usually lbn
        const uint32_t a1 = getRegU32(ctx, 5); // usually sector count
        const uint32_t a2 = getRegU32(ctx, 6); // usually destination buffer
        {
            static std::atomic<uint32_t> s_cdrd{0};
            if (s_cdrd.fetch_add(1) < 40u)
                std::cerr << "[cdRead] lbn=0x" << std::hex << a0 << " sectors=0x" << a1 << " dst=0x" << a2 << std::dec << std::endl;
        }

        // [menuhex] PS2X_MENUHEX=1: hex-dump first 128 bytes of every sceCdRead into guest RAM,
        // plus ra (caller PC). No cap. Useful to trace PAK/AFS data loading.
        {
            static const bool s_menuHex = [](){
                const char *v = std::getenv("PS2X_MENUHEX");
                return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
            }();
            if (s_menuHex)
            {
                const uint32_t ra = getRegU32(ctx, 31);
                const uint32_t pc = ctx->pc;
                const uint32_t dstOff = a2 & PS2_RAM_MASK;
                const uint32_t readBytes = a1 * kCdSectorSize;
                std::cerr << "[menuhex] sceCdRead pc=0x" << std::hex << pc
                          << " ra=0x" << ra
                          << " lbn=0x" << a0
                          << " sec=0x" << a1
                          << " dst=0x" << a2
                          << " bytes=0x" << readBytes
                          << std::dec << std::endl;
                // We'll hex-dump AFTER the read succeeds (below tryRead).
            }
        }

        struct CdReadArgs
        {
            uint32_t lbn = 0;
            uint32_t sectors = 0;
            uint32_t buf = 0;
            const char *tag = "";
        };

        auto clampReadBytes = [](uint32_t sectors, uint32_t offset) -> size_t
        {
            const uint64_t requested = static_cast<uint64_t>(sectors) * static_cast<uint64_t>(kCdSectorSize);
            if (requested == 0)
            {
                return 0;
            }

            const uint64_t maxBytes = static_cast<uint64_t>(PS2_RAM_SIZE - offset);
            const uint64_t clamped = std::min<uint64_t>(requested, maxBytes);
            return static_cast<size_t>(clamped);
        };

        auto tryRead = [&](const CdReadArgs &args) -> bool
        {
            const uint32_t offset = args.buf & PS2_RAM_MASK;
            const size_t bytes = clampReadBytes(args.sectors, offset);
            if (bytes == 0)
            {
                return true;
            }
            uint32_t lbn = args.lbn;
            {   // [afsremap] PS2X_AFSREMAP=<lbn1>:<lbn2>:<nsectors> (hex): pure sector redirect, the AFS index is untouched
                // (editing the index -- even the size alone -- corrupts the boot). Reads inside [lbn1, lbn1+n) fetch
                // lbn2 + (lbn - lbn1). Use a target entry no larger than the source so its header table stays inside.
                static const std::string s_rm = [](){ const char *v = std::getenv("PS2X_AFSREMAP"); return std::string(v ? v : ""); }();
                static uint32_t r1 = 0, r2 = 0, rn = 0;
                static const bool s_rok = !s_rm.empty() && std::sscanf(s_rm.c_str(), "%x:%x:%x", &r1, &r2, &rn) == 3;
                if (s_rok && lbn >= r1 && lbn < r1 + rn)
                {
                    static uint32_t s_rlog = 0;
                    if (s_rlog++ < 12u) std::fprintf(stderr, "[afsremap] read lbn 0x%x -> 0x%x (%u sectors)\n", lbn, r2 + (lbn - r1), args.sectors);
                    lbn = r2 + (lbn - r1);
                }
            }
            {   // [afsswap] sector remap: reads inside entry 1's (enlarged) range fetch entry 2's sectors instead
                static const std::string s_sw = [](){ const char *v = std::getenv("PS2X_AFSSWAP"); return std::string(v ? v : ""); }();
                static uint32_t o1 = 0, z1 = 0, o2 = 0, z2 = 0;
                static const bool s_ok = !s_sw.empty() && std::sscanf(s_sw.c_str(), "%x:%x:%x:%x", &o1, &z1, &o2, &z2) == 4;
                if (s_ok)
                {
                    const uint32_t l1 = 0x7c6u + (o1 >> 11), l2 = 0x7c6u + (o2 >> 11), n = (z2 + 2047u) >> 11;
                    if (lbn >= l1 && lbn < l1 + n)
                    {
                        lbn = l2 + (lbn - l1);
                        static uint32_t s_log = 0;
                        if (s_log++ < 12u) std::fprintf(stderr, "[afsswap] remap read lbn 0x%x -> 0x%x (%u sectors)\n", args.lbn, lbn, args.sectors);
                    }
                }
            }
            {
                const bool ok_ = readCdSectors(lbn, args.sectors, rdram + offset, bytes);
                if (ok_) ps2TraceGuestRangeWrite(rdram, offset, (uint32_t)bytes, "cdread", ctx);
                return ok_;
            }
        };

        CdReadArgs selected{a0, a1, a2, "a0/a1/a2"};
        bool ok = tryRead(selected);
        // [cddst] PS2X_CDDST=<lo>:<hi> (hex): UNCAPPED log of reads whose dst intersects the
        // range — the general [cdread] log's 6000-line budget hides in-fight deliveries.
        {
            static const std::string s_cw = [](){ const char *v = std::getenv("PS2X_CDDST"); return std::string(v ? v : ""); }();
            static uint32_t w_lo = 0, w_hi = 0;
            static const bool s_wok = !s_cw.empty() && std::sscanf(s_cw.c_str(), "%x:%x", &w_lo, &w_hi) == 2;
            if (ok && s_wok)
            {
                const uint32_t d0 = selected.buf & 0x1FFFFFFFu;
                const uint32_t d1 = d0 + selected.sectors * 2048u;
                if (d0 < w_hi && d1 > w_lo)
                    std::fprintf(stderr, "[cddst] lbn=0x%x sectors=%u dst=0x%x..0x%x ra=0x%x\n",
                                 selected.lbn, selected.sectors, d0, d1, getRegU32(ctx, 31));
            }
        }
        {   // [cdreadlog] every read that landed: destination range + which argument layout, so a late/misplaced read
            // can be matched against a corrupted object (loader wild-jump hunt). First 6000 only.
            static std::atomic<uint32_t> s_cl{0};
            if (ok && s_cl.fetch_add(1u) < 6000u)
                std::fprintf(stderr, "[cdread] lbn=0x%x sectors=%u dst=0x%x..0x%x\n", selected.lbn, selected.sectors, selected.buf, selected.buf + selected.sectors * 2048u);
        }

        if (!ok)
        {
            // Some game-side wrappers use a nonstandard register layout.
            // If primary decode does not resolve to a known LBN, try safe alternatives.
            constexpr uint32_t kMaxReasonableSectors = PS2_RAM_SIZE / kCdSectorSize;
            if (!isResolvableCdLbn(selected.lbn))
            {
                const std::array<CdReadArgs, 5> alternatives = {
                    CdReadArgs{a2, a1, a0, "a2/a1/a0"},
                    CdReadArgs{a0, a2, a1, "a0/a2/a1"},
                    CdReadArgs{a1, a0, a2, "a1/a0/a2"},
                    CdReadArgs{a1, a2, a0, "a1/a2/a0"},
                    CdReadArgs{a2, a0, a1, "a2/a0/a1"}};

                for (const CdReadArgs &candidate : alternatives)
                {
                    if (candidate.sectors > kMaxReasonableSectors)
                    {
                        continue;
                    }
                    if (!isResolvableCdLbn(candidate.lbn))
                    {
                        continue;
                    }

                    if (tryRead(candidate))
                    {
                        std::fprintf(stderr, "[cdread] ALT %s lbn=0x%x sectors=%u dst=0x%x..0x%x\n", candidate.tag, candidate.lbn, candidate.sectors, candidate.buf, candidate.buf + candidate.sectors * 2048u);   // [cdreadlog]
                        static uint32_t recoverLogCount = 0;
                        if (recoverLogCount < 16)
                        {
                            RUNTIME_LOG("[sceCdRead] recovered with alternate args " << candidate.tag
                                                                                     << " (pc=0x" << std::hex << ctx->pc
                                                                                     << " ra=0x" << getRegU32(ctx, 31)
                                                                                     << " a0=0x" << a0
                                                                                     << " a1=0x" << a1
                                                                                     << " a2=0x" << a2 << std::dec << ")" << std::endl);
                            ++recoverLogCount;
                        }
                        selected = candidate;
                        ok = true;
                        break;
                    }
                }
            }

            if (!ok)
            {
                const uint32_t offset = a2 & PS2_RAM_MASK;
                const size_t bytes = clampReadBytes(a1, offset);
                if (bytes > 0)
                {
                    std::memset(rdram + offset, 0, bytes);
                }

                static uint32_t unresolvedLogCount = 0;
                if (unresolvedLogCount < 32)
                {
                    std::cerr << "[sceCdRead] unresolved request pc=0x" << std::hex << ctx->pc
                              << " ra=0x" << getRegU32(ctx, 31)
                              << " a0=0x" << a0
                              << " a1=0x" << a1
                              << " a2=0x" << a2 << std::dec << std::endl;
                    ++unresolvedLogCount;
                }
            }
        }

        if (ok)
        {
            // [afsswap] PS2X_AFSSWAP=<off1>:<size1>:<off2>:<size2> (hex): the game reads the PZS3US1.AFS index at boot
            // (lbn 0x7c6.. one sector at a time into a staging buffer, parsed right after each read). Patch the raw
            // (off,size) pair in the buffer we just filled, and -- in case the parsed table keeps (lbn,size) -- scan RAM
            // for that derived form too. Used to load another stage's Map_XX_PS on the scripted fight.
            static const std::string s_sw = [](){ const char *v = std::getenv("PS2X_AFSSWAP"); return std::string(v ? v : ""); }();
            static uint32_t s_calls = 0, s_hits = 0;
            if (!s_sw.empty() && s_calls++ < 3000u)
            {
                uint32_t o1 = 0, z1 = 0, o2 = 0, z2 = 0;
                if (std::sscanf(s_sw.c_str(), "%x:%x:%x:%x", &o1, &z1, &o2, &z2) == 4)
                {
                    const uint32_t l1 = 0x7c6u + (o1 >> 11), l2 = 0x7c6u + (o2 >> 11);
                    auto scan = [&](const uint8_t *base, size_t len, uint32_t k0, uint32_t k1, uint32_t r0, uint32_t r1, const char *what)
                    {
                        uint8_t key[8]; std::memcpy(key, &k0, 4); std::memcpy(key + 4, &k1, 4);
                        const uint8_t *cur = base; size_t left = len;
                        while (left >= 8)
                        {
                            const uint8_t *hit = static_cast<const uint8_t *>(memmem(cur, left, key, 8));
                            if (!hit) break;
                            const uint32_t at = static_cast<uint32_t>(hit - rdram);
                            std::memcpy(rdram + at, &r0, 4); std::memcpy(rdram + at + 4, &r1, 4);
                            std::fprintf(stderr, "[afsswap] %s (0x%x,0x%x) at guest 0x%x -> (0x%x,0x%x) (cd call %u lbn=0x%x, hit %u)\n", what, k0, k1, at, r0, r1, s_calls, selected.lbn, ++s_hits);
                            cur = hit + 8; left = len - (cur - base);
                        }
                    };
                    // size only: the index must stay offset-sorted (swapping the offset crashed the boot at 0x26e104);
                    // the sector remap in tryRead() fetches the other entry's data for the whole (larger) size.
                    (void)l1; (void)l2; (void)o2;
                    const uint32_t off = selected.buf & PS2_RAM_MASK; const size_t bytes = clampReadBytes(selected.sectors, off);
                    scan(rdram + off, bytes, o1, z1, o1, z2, "raw pair in read buffer (size only)");
                }
            }
            g_cdStreamingLbn = selected.lbn + selected.sectors;
            noteAdxHeaderIfPresent(rdram, a2, a1);

            // [menuhex] hex-dump first 128 bytes of data just read into guest RAM
            {
                static const bool s_menuHex = [](){
                    const char *v = std::getenv("PS2X_MENUHEX");
                    return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
                }();
                if (s_menuHex)
                {
                    const uint32_t off = selected.buf & PS2_RAM_MASK;
                    const size_t bytes = clampReadBytes(selected.sectors, off);
                    const size_t dumpLen = bytes < 128u ? bytes : 128u;
                    std::cerr << "[menuhex] DATA lbn=0x" << std::hex << selected.lbn
                              << " sec=" << selected.sectors
                              << " dst=0x" << selected.buf
                              << " bytes=0x" << bytes
                              << " hex:" << std::dec << std::endl;
                    for (size_t i = 0; i < dumpLen; i += 16)
                    {
                        std::cerr << "  " << std::hex << std::setw(4) << std::setfill('0') << i << ": ";
                        std::cerr.unsetf(std::ios::hex);
                        for (size_t j = 0; j < 16 && (i + j) < dumpLen; ++j)
                        {
                            std::cerr << std::hex << std::setw(2) << std::setfill('0')
                                      << static_cast<uint32_t>(rdram[off + i + j]) << " ";
                        }
                        std::cerr << std::dec;
                        std::cerr << " |";
                        for (size_t j = 0; j < 16 && (i + j) < dumpLen; ++j)
                        {
                            char c = static_cast<char>(rdram[off + i + j]);
                            std::cerr << (c >= 32 && c < 127 ? c : '.');
                        }
                        std::cerr << "|" << std::endl;
                    }
                }
            }

            setReturnS32(ctx, 1); // command accepted/success
            return;
        }

        setReturnS32(ctx, 0);
    }

    void sceCdSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0); // 0 = completed/not busy
    }

    void sceCdGetError(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_lastCdError);
    }

    void sceCdRI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceCdRI", rdram, ctx, runtime);
    }

    void sceCdRM(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceCdRM", rdram, ctx, runtime);
    }

    void sceCdApplyNCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdBreak(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceCdChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdDelayThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceCdDiskReady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 2);
    }

    void sceCdGetDiskType(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // SCECdPS2DVD
        setReturnS32(ctx, 0x14);
    }

    void sceCdGetReadPos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, g_cdStreamingLbn);
    }

    void sceCdGetToc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t tocAddr = getRegU32(ctx, 4);
        if (uint8_t *toc = getMemPtr(rdram, tocAddr))
        {
            std::memset(toc, 0, 1024);
        }
        setReturnS32(ctx, 1);
    }

    void sceCdInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdInitialized = true;
        g_lastCdError = 0;
        setReturnS32(ctx, 1);
    }

    void sceCdInitEeCB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdIntToPos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t lsn = getRegU32(ctx, 4);
        uint32_t posAddr = getRegU32(ctx, 5);
        uint8_t *pos = getMemPtr(rdram, posAddr);
        if (!pos)
        {
            setReturnS32(ctx, 0);
            return;
        }

        uint32_t adjusted = lsn + 150;
        const uint32_t minutes = adjusted / (60 * 75);
        adjusted %= (60 * 75);
        const uint32_t seconds = adjusted / 75;
        const uint32_t sectors = adjusted % 75;

        pos[0] = toBcd(minutes);
        pos[1] = toBcd(seconds);
        pos[2] = toBcd(sectors);
        pos[3] = 0;
        setReturnS32(ctx, 1);
    }

    void sceCdMmode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdMode = getRegU32(ctx, 4);
        setReturnS32(ctx, 1);
    }

    void sceCdNcmdDiskReady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 2);
    }

    void sceCdPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdPosToInt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t posAddr = getRegU32(ctx, 4);
        const uint8_t *pos = getConstMemPtr(rdram, posAddr);
        if (!pos)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t minutes = fromBcd(pos[0]);
        const uint32_t seconds = fromBcd(pos[1]);
        const uint32_t sectors = fromBcd(pos[2]);
        const uint32_t absolute = (minutes * 60 * 75) + (seconds * 75) + sectors;
        const int32_t lsn = static_cast<int32_t>(absolute) - 150;
        setReturnS32(ctx, lsn);
    }

    void sceCdReadChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t chainAddr = getRegU32(ctx, 4);
        bool ok = true;

        for (int i = 0; i < 64; ++i)
        {
            uint32_t *entry = reinterpret_cast<uint32_t *>(getMemPtr(rdram, chainAddr + (i * 16)));
            if (!entry)
            {
                ok = false;
                break;
            }

            const uint32_t lbn = entry[0];
            const uint32_t sectors = entry[1];
            const uint32_t buf = entry[2];
            if (lbn == 0xFFFFFFFFu || sectors == 0)
            {
                break;
            }

            uint32_t offset = buf & PS2_RAM_MASK;
            size_t bytes = static_cast<size_t>(sectors) * kCdSectorSize;
            const size_t maxBytes = PS2_RAM_SIZE - offset;
            if (bytes > maxBytes)
            {
                bytes = maxBytes;
            }

            if (readCdSectors(lbn, sectors, rdram + offset, bytes))
                ps2TraceGuestRangeWrite(rdram, offset, (uint32_t)bytes, "cdchain", ctx);
            else if (true)
            {
                ok = false;
                break;
            }

            g_cdStreamingLbn = lbn + sectors;
        }

        setReturnS32(ctx, ok ? 1 : 0);
    }

    void sceCdReadClock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clockAddr = getRegU32(ctx, 4);
        uint8_t *clockData = getMemPtr(rdram, clockAddr);
        if (!clockData)
        {
            setReturnS32(ctx, 0);
            return;
        }

        std::time_t now = std::time(nullptr);
        std::tm localTm{};
#ifdef _WIN32
        localtime_s(&localTm, &now);
#else
        localtime_r(&now, &localTm);
#endif

        // sceCdCLOCK format (BCD fields).
        clockData[0] = 0;
        clockData[1] = toBcd(static_cast<uint32_t>(localTm.tm_sec));
        clockData[2] = toBcd(static_cast<uint32_t>(localTm.tm_min));
        clockData[3] = toBcd(static_cast<uint32_t>(localTm.tm_hour));
        clockData[4] = 0;
        clockData[5] = toBcd(static_cast<uint32_t>(localTm.tm_mday));
        clockData[6] = toBcd(static_cast<uint32_t>(localTm.tm_mon + 1));
        clockData[7] = toBcd(static_cast<uint32_t>((localTm.tm_year + 1900) % 100));
        setReturnS32(ctx, 1);
    }

    void sceCdReadIOPm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceCdRead(rdram, ctx, runtime);
    }

    void sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t fileAddr = getRegU32(ctx, 4);
        uint32_t pathAddr = getRegU32(ctx, 5);
        const std::string path = readPs2CStringBounded(rdram, pathAddr, 260);
        const std::string normalizedPath = normalizeCdPathNoPrefix(path);
        {
            static std::atomic<uint32_t> s_sf{0};
            if (s_sf.fetch_add(1) < 40u)
                std::cerr << "[cdSearch] path=\"" << path << "\"" << std::endl;
        }
        static uint32_t traceCount = 0;
        const uint32_t callerRa = getRegU32(ctx, 31);
        const bool shouldTrace = (traceCount < 128u) || ((traceCount % 512u) == 0u);
        if (shouldTrace)
        {
            RUNTIME_LOG("[sceCdSearchFile] pc=0x" << std::hex << ctx->pc
                                                   << " ra=0x" << callerRa
                                                   << " file=0x" << fileAddr
                                                   << " pathAddr=0x" << pathAddr
                                                   << " path=\"" << sanitizeForLog(path) << "\""
                                                   << std::dec << std::endl);
        }
        ++traceCount;

        // [menuhex] log ALL sceCdSearchFile calls for menu-relevant paths
        {
            static const bool s_menuHex = [](){
                const char *v = std::getenv("PS2X_MENUHEX");
                return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
            }();
            if (s_menuHex && !path.empty())
            {
                std::string lower = normalizedPath;
                for (auto &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower.find("main_") != std::string::npos ||
                    lower.find("battle_") != std::string::npos ||
                    lower.find("pzs3us") != std::string::npos ||
                    lower.find("pak") != std::string::npos ||
                    lower.find("cpak") != std::string::npos ||
                    lower.find("textpack") != std::string::npos ||
                    lower.find("menu") != std::string::npos ||
                    lower.find("plate") != std::string::npos ||
                    lower.find("afs") != std::string::npos)
                {
                    std::cerr << "[menuhex-search] pc=0x" << std::hex << ctx->pc
                              << " ra=0x" << callerRa
                              << " path=\"" << path << "\""
                              << " norm=\"" << normalizedPath << "\""
                              << std::dec << std::endl;
                }
            }
        }

        if (path.empty())
        {
            static uint32_t emptyPathCount = 0;
            if (emptyPathCount < 64 || (emptyPathCount % 512u) == 0u)
            {
                std::ostringstream preview;
                preview << std::hex;
                for (uint32_t i = 0; i < 16; ++i)
                {
                    const uint8_t byte = *getConstMemPtr(rdram, pathAddr + i);
                    preview << (i == 0 ? "" : " ") << static_cast<uint32_t>(byte);
                }
                std::cerr << "[sceCdSearchFile] empty path at 0x" << std::hex << pathAddr
                          << " preview=" << preview.str()
                          << " ra=0x" << callerRa << std::dec << std::endl;
            }
            ++emptyPathCount;
            g_lastCdError = -1;
            setReturnS32(ctx, 0);
            return;
        }

        if (normalizedPath.empty())
        {
            static uint32_t emptyNormalizedCount = 0;
            if (emptyNormalizedCount < 64u || (emptyNormalizedCount % 512u) == 0u)
            {
                std::cerr << "sceCdSearchFile failed: " << sanitizeForLog(path)
                          << " (normalized path is empty, root: " << getCdRootPath().string() << ")"
                          << std::endl;
            }
            ++emptyNormalizedCount;
            g_lastCdError = -1;
            setReturnS32(ctx, 0);
            return;
        }

        CdFileEntry entry;
        bool found = registerCdFile(path, entry);
        CdFileEntry resolvedEntry = entry;
        std::string resolvedPath;

        if (!found)
        {
            static std::string lastFailedPath;
            static uint32_t samePathFailCount = 0;
            if (path == lastFailedPath)
            {
                ++samePathFailCount;
            }
            else
            {
                lastFailedPath = path;
                samePathFailCount = 1;
            }

            if (samePathFailCount <= 16u || (samePathFailCount % 512u) == 0u)
            {
                std::cerr << "sceCdSearchFile failed: " << sanitizeForLog(path)
                          << " (root: " << getCdRootPath().string()
                          << ", repeat=" << samePathFailCount << ")" << std::endl;
            }
            setReturnS32(ctx, 0);
            return;
        }

        if (!writeCdSearchResult(rdram, fileAddr, path, resolvedEntry))
        {
            g_lastCdError = -1;
            setReturnS32(ctx, 0);
            return;
        }

        g_cdStreamingLbn = resolvedEntry.baseLbn;
        g_cdStreamingEndLbn = resolvedEntry.baseLbn + resolvedEntry.sectors;
        if (shouldTrace)
        {
            RUNTIME_LOG("[sceCdSearchFile:ok] path=\"" << sanitizeForLog(path)
                                                       << "\" lsn=0x" << std::hex << resolvedEntry.baseLbn
                                                       << " size=0x" << resolvedEntry.sizeBytes
                                                       << " sectors=0x" << resolvedEntry.sectors
                                                       << std::dec << std::endl);
        }
        setReturnS32(ctx, 1);
    }

    void sceCdSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdStreamingLbn = getRegU32(ctx, 4);
        g_cdStreamingEndLbn = cdStreamingEndLbnForStart(g_cdStreamingLbn);
        setReturnS32(ctx, 1);
    }

    void sceCdStandby(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_cdInitialized ? 6 : 0);
    }

    void sceCdStInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdStPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdStRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t requestedSectors = getRegU32(ctx, 4);
        uint32_t sectors = requestedSectors;
        uint32_t buf = getRegU32(ctx, 5);
        uint32_t errAddr = getRegU32(ctx, 7);

        uint32_t offset = buf & PS2_RAM_MASK;
        size_t requestedBytes = static_cast<size_t>(requestedSectors) * kCdSectorSize;
        const size_t maxBytes = PS2_RAM_SIZE - offset;
        if (requestedBytes > maxBytes)
        {
            requestedBytes = maxBytes;
        }

        bool hitStreamEnd = false;
        if (g_cdStreamingEndLbn != 0xFFFFFFFFu)
        {
            if (g_cdStreamingLbn >= g_cdStreamingEndLbn)
            {
                sectors = 0u;
                hitStreamEnd = true;
            }
            else
            {
                const uint32_t remaining = g_cdStreamingEndLbn - g_cdStreamingLbn;
                if (sectors > remaining)
                {
                    sectors = remaining;
                    hitStreamEnd = true;
                }
            }
        }

        size_t bytes = static_cast<size_t>(sectors) * kCdSectorSize;
        if (bytes > maxBytes)
        {
            bytes = maxBytes;
        }

        const uint32_t readLbn = g_cdStreamingLbn;
        const bool ok = (sectors > 0u) && readCdSectors(readLbn, sectors, rdram + offset, bytes);
        if (ok)
        {
            ps2TraceGuestRangeWrite(rdram, offset, (uint32_t)bytes, "cdstream", ctx);
            g_cdStreamingLbn += sectors;
            if (requestedBytes > bytes)
            {
                std::memset(rdram + offset + bytes, 0, requestedBytes - bytes);
            }
            if (hitStreamEnd || g_cdStreamingLbn == g_cdStreamingEndLbn)
            {
                notifyMpegCdStreamEof();
            }
        }
        else
        {
            if (requestedBytes > 0u)
            {
                std::memset(rdram + offset, 0, requestedBytes);
            }
            notifyMpegCdStreamEof();
        }

        if (int32_t *err = reinterpret_cast<int32_t *>(getMemPtr(rdram, errAddr)); err)
        {
            *err = ok ? 0 : g_lastCdError;
        }

        if (g_cdStReadTraceCount < 32u)
        {
            std::cerr << "[sceCdStRead] sectors=" << requestedSectors
                      << " read=" << sectors
                      << " buf=0x" << std::hex << buf
                      << " lbn=0x" << readLbn
                      << " end=0x" << g_cdStreamingEndLbn
                      << std::dec << " ok=" << ok
                      << " bytes=" << bytes << std::endl;
            ++g_cdStReadTraceCount;
        }

        setReturnS32(ctx, ok ? static_cast<int32_t>(sectors) : 0);
    }

    void sceCdStream(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdStResume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceCdStSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdStreamingLbn = getRegU32(ctx, 4);
        g_cdStreamingEndLbn = cdStreamingEndLbnForStart(g_cdStreamingLbn);
        setReturnS32(ctx, 1);
    }

    void sceCdStSeekF(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdStreamingLbn = getRegU32(ctx, 4);
        g_cdStreamingEndLbn = cdStreamingEndLbnForStart(g_cdStreamingLbn);
        setReturnS32(ctx, 1);
    }

    void sceCdStStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cdStreamingLbn = getRegU32(ctx, 4);
        g_cdStreamingEndLbn = cdStreamingEndLbnForStart(g_cdStreamingLbn);
        g_cdStReadTraceCount = 0u;

        notifyMpegCdStreamStart();

        std::cerr << "[sceCdStStart] lbn=0x" << std::hex << g_cdStreamingLbn
                  << " endLbn=0x" << g_cdStreamingEndLbn << std::dec << std::endl;
        setReturnS32(ctx, 1);
    }

    void sceCdStStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceCdStStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        notifyMpegCdStreamEof();
        setReturnS32(ctx, 1);
    }

    void sceCdSyncS(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceCdTrayReq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t statusPtr = getRegU32(ctx, 5);
        if (uint32_t *status = reinterpret_cast<uint32_t *>(getMemPtr(rdram, statusPtr)); status)
        {
            *status = 0;
        }
        setReturnS32(ctx, 1);
    }

    bool dvciFindExtractedFile(const char *ps2Path, uint32_t &lbnOut, uint32_t &sizeOut)
    {
        CdFileEntry entry;
        if (registerCdFile(ps2Path, entry))
        {
            lbnOut = entry.baseLbn;
            sizeOut = entry.sizeBytes;
            return true;
        }
        return false;
    }
}
