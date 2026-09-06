#include "runtime/ps2_iop.h"
#include "runtime/ps2_iop_audio.h"
#include "runtime/ps2_iop_cl.h"
#include "runtime/ps2_iop_dbcman.h"
#include "runtime/ps2_iop_sdrdrv.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "Kernel/Syscalls/RPC.h"
#include "Kernel/Stubs/CD.h"
#include <fstream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <iostream>

namespace
{
    // Minimal ISO9660 path -> (LBN, size-bytes) resolver for the mounted CD image.
    // Handles the game's DVCI file-resolver RPC (sid 0x80000597, cmd 0), which sends
    // a path like "\DATA\PZS3US.DIR;1" and expects the file's absolute sector (LBN).
    bool iso9660Resolve(const std::string &imagePath, const std::string &guestPath,
                        uint32_t &lbnOut, uint32_t &sizeOut)
    {
        std::ifstream f(imagePath, std::ios::binary);
        if (!f.is_open()) return false;
        auto readSector = [&](uint32_t lbn, std::vector<uint8_t> &buf) -> bool {
            buf.resize(2048);
            f.seekg(static_cast<std::streamoff>(lbn) * 2048);
            f.read(reinterpret_cast<char *>(buf.data()), 2048);
            return f.good() || f.gcount() > 0;
        };
        std::vector<uint8_t> sec;
        if (!readSector(16, sec)) return false; // Primary Volume Descriptor
        // Root directory record lives at PVD offset 156, 34 bytes.
        const uint8_t *root = sec.data() + 156;
        uint32_t dirLbn = *reinterpret_cast<const uint32_t *>(root + 2);
        uint32_t dirSize = *reinterpret_cast<const uint32_t *>(root + 10);

        // Split the guest path into components, dropping device/prefix, ";1" and case.
        std::string p = guestPath;
        for (char &c : p) { if (c == '\\') c = '/'; }
        std::vector<std::string> comps;
        size_t i = 0;
        while (i < p.size())
        {
            while (i < p.size() && p[i] == '/') ++i;
            size_t j = i;
            while (j < p.size() && p[j] != '/') ++j;
            if (j > i)
            {
                std::string c = p.substr(i, j - i);
                size_t semi = c.find(';');
                if (semi != std::string::npos) c = c.substr(0, semi);
                std::transform(c.begin(), c.end(), c.begin(), ::toupper);
                if (!c.empty() && c != "." && c != "HST:") comps.push_back(c);
            }
            i = j;
        }
        if (comps.empty()) return false;

        for (size_t ci = 0; ci < comps.size(); ++ci)
        {
            const std::string &want = comps[ci];
            const bool wantDir = (ci + 1 < comps.size());
            bool found = false;
            uint32_t remaining = dirSize;
            uint32_t curLbn = dirLbn;
            std::vector<uint8_t> dbuf;
            while (remaining > 0 && !found)
            {
                if (!readSector(curLbn, dbuf)) return false;
                uint32_t off = 0;
                while (off < 2048)
                {
                    uint8_t len = dbuf[off];
                    if (len == 0) break; // rest of sector is padding
                    const uint8_t *rec = dbuf.data() + off;
                    uint32_t eLbn = *reinterpret_cast<const uint32_t *>(rec + 2);
                    uint32_t eSize = *reinterpret_cast<const uint32_t *>(rec + 10);
                    uint8_t flags = rec[25];
                    uint8_t nameLen = rec[32];
                    std::string name(reinterpret_cast<const char *>(rec + 33), nameLen);
                    size_t semi = name.find(';');
                    if (semi != std::string::npos) name = name.substr(0, semi);
                    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
                    if (name == want && (((flags & 2) != 0) == wantDir))
                    {
                        dirLbn = eLbn; dirSize = eSize;
                        lbnOut = eLbn; sizeOut = eSize;
                        found = true;
                        break;
                    }
                    off += len;
                }
                if (remaining >= 2048) { remaining -= 2048; ++curLbn; }
                else remaining = 0;
            }
            if (!found) return false;
        }
        return true;
    }
}

ps2_iop::ps2_iop()
{
    reset();
}

void ps2_iop::init(uint8_t *rdram)
{
    m_rdram = rdram;
}

void ps2_iop::reset()
{
    ps2_iop_cl::reset();
    ps2_iop_dbcman::reset();
    ps2_iop_sdrdrv::reset();
}

bool ps2_iop::handleRPC(PS2Runtime *runtime,
                        uint32_t sid,
                        uint32_t rpcNum,
                        uint32_t sendBufAddr,
                        uint32_t sendSize,
                        uint32_t recvBufAddr,
                        uint32_t recvSize,
                        uint32_t &resultPtr,
                        bool &signalNowaitCompletion)
{
    resultPtr = 0u;
    signalNowaitCompletion = false;

    // Dragon Ball Z: Budokai Tenkaichi 3 (SLUS_216.78) DVCI disc file-resolver.
    // sid 0x80000597, cmd 0 = "find file": the guest sends a path string at
    // sendBuf+0x24 (e.g. "\DATA\PZS3US.DIR;1") in a 300-byte buffer and expects
    // the file's absolute sector (LBN) written back to sendBuf[0] and a non-zero
    // success code in the 4-byte recv buffer. The subsequent sceCdRead(LBN,...)
    // then reads the file from the mounted CD image.
    if (sid == 0x80000597u && rpcNum == 0u && sendBufAddr && sendSize >= 0x28u)
    {
        uint8_t *sb = getMemPtr(m_rdram, sendBufAddr);
        if (sb)
        {
            char path[260];
            uint32_t n = 0;
            for (; n < 259u && (0x24u + n) < sendSize; ++n)
            {
                char c = static_cast<char>(sb[0x24u + n]);
                if (c == '\0') break;
                path[n] = c;
            }
            path[n] = '\0';

            const std::string image = PS2Runtime::getIoPaths().cdImage.string();
            uint32_t lbn = 0, size = 0;
            bool ok = !image.empty() && iso9660Resolve(image, path, lbn, size);

            // Fallback: if the ISO is missing or the path isn't in the
            // ISO9660 tree, resolve from extracted files on disk.
            if (!ok)
            {
                ok = ps2_stubs::dvciFindExtractedFile(path, lbn, size);
            }

            static std::atomic<uint32_t> s_dvci{0};
            if (s_dvci.fetch_add(1) < 40u)
                std::cerr << "[dvci-find] path=\"" << path << "\" -> "
                          << (ok ? "lbn=" : "NOT FOUND lbn=") << lbn << " size=" << size << std::endl;

            // [menuhex] log ALL DVCI lookups that involve menu-relevant files
            {
                static const bool s_menuHex = [](){
                    const char *v = std::getenv("PS2X_MENUHEX");
                    return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
                }();
                if (s_menuHex && ok)
                {
                    std::string lower = path;
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
                        std::cerr << "[menuhex-dvci] RESOLVE path=\"" << path
                                  << "\" lbn=0x" << std::hex << lbn
                                  << " size=0x" << size
                                  << std::dec << std::endl;
                    }
                }
            }

            // Write the file info back into the shared send buffer.
            std::memset(sb, 0, 0x24u);
            std::memcpy(sb + 0, &lbn, 4);   // sceCdlFILE.lsn
            std::memcpy(sb + 4, &size, 4);  // sceCdlFILE.size
            if (recvBufAddr)
            {
                uint8_t *rb = getMemPtr(m_rdram, recvBufAddr);
                if (rb)
                {
                    std::memset(rb, 0, recvSize);
                    uint32_t rc = ok ? 1u : 0u;
                    std::memcpy(rb, &rc, sizeof(rc) <= recvSize ? sizeof(rc) : recvSize);
                }
                resultPtr = recvBufAddr;
            }
        }
        signalNowaitCompletion = true;
        return true;
    }

    // DIAGNOSTIC: dump DVCI RPC (sid 0x2000004) commands + sendBuf so we can see
    // the movie read/stream requests and their params.
    if (sid == 0x2000004u && std::getenv("PS2X_DVCI_PROBE"))
    {
        static std::atomic<uint32_t> s_d{0};
        uint32_t n = s_d.fetch_add(1);
        if (n < 120u)
        {
            std::cerr << "[dvci-rpc] #" << n << " rpcNum=" << rpcNum << " sendBuf=0x" << std::hex << sendBufAddr
                      << " size=" << std::dec << sendSize << " |";
            if (const uint8_t *sb = getMemPtr(m_rdram, sendBufAddr))
                for (uint32_t i = 0; i < 32u && i < sendSize; i += 4)
                    std::cerr << " " << std::hex << *reinterpret_cast<const uint32_t *>(sb + i) << std::dec;
            std::cerr << std::endl;
        }
    }

    // EMPIRICAL: cmd 13 is the batch-dispatch. sendBuf 0x300ec0 holds the whole
    // DVCI command queue that the game just built: word[0] = count, then `count`
    // entries of 12 bytes each starting at sendBuf+4 (see FUN_001242f0/00124150).
    // Parse and log every queued command so we can learn the per-type semantics
    // before executing them. Gated on PS2X_DVCI_Q.
    if (sid == 0x2000004u && rpcNum == 13u && std::getenv("PS2X_DVCI_Q"))
    {
        static std::atomic<uint32_t> s_q{0};
        uint32_t n = s_q.fetch_add(1);
        if (n < 200u)
        {
            if (const uint8_t *sb = getMemPtr(m_rdram, sendBufAddr))
            {
                uint32_t count = *reinterpret_cast<const uint32_t *>(sb);
                std::cerr << "[dvci-q] #" << n << " count=" << count << std::endl;
                uint32_t cap = count > 32u ? 32u : count;
                for (uint32_t e = 0; e < cap; ++e)
                {
                    const uint8_t *ent = sb + 4u + e * 12u;
                    std::cerr << "  e" << e << " type=" << (int)ent[0]
                              << " b1=" << std::hex << (int)ent[1]
                              << " seq=" << *reinterpret_cast<const uint16_t *>(ent + 2)
                              << " b4=" << (int)ent[4] << " b5=" << (int)ent[5]
                              << " b6=" << (int)ent[6] << " b7=" << (int)ent[7]
                              << " w8=0x" << *reinterpret_cast<const uint32_t *>(ent + 8)
                              << std::dec << std::endl;
                }
            }
        }
    }

    // Locate the game's DVCI file table by scanning RAM for the distinctive AFS2 LBA
    // (715866=0xAEC5A) so we can decode the 76-byte entry layout (lbn / position / size).
    if (sid == 0x2000004u && (rpcNum == 3u || rpcNum == 13u) && std::getenv("PS2X_DVCI_TABLE"))
    {
        static std::atomic<uint32_t> s_scan{0};
        if (s_scan.fetch_add(1) < 3u)
        {
            const uint32_t targets[] = { 715866u, 715866u*2048u, 1990u, 1990u*2048u, 1220337664u };
            for (uint32_t ti = 0; ti < 5u; ++ti)
            {
              const uint32_t target = targets[ti];
              for (uint32_t a = 0x100000u; a < 0x1F00000u; a += 4u)
              {
                uint32_t v;
                std::memcpy(&v, m_rdram + a, 4);
                if (v == target)
                {
                    std::cerr << "[dvci-table] AFS2 lbn found @0x" << std::hex << a << " -> 76B entry dump:";
                    // dump 24 words starting a bit before, to see the whole entry
                    for (int w = -4; w < 20; ++w)
                    {
                        uint32_t wv; std::memcpy(&wv, m_rdram + (a + w*4), 4);
                        std::cerr << " [" << std::dec << w*4 << "]=0x" << std::hex << wv;
                    }
                    std::cerr << std::dec << std::endl;
                }
              }
            }
        }
    }

    // Other BT3 SVM/DVCI control RPCs: acknowledge so init proceeds.
    if (sid == 0x80000597u || sid == 0x2000004u)
    {
        if (recvBufAddr)
        {
            uint8_t *rb = getMemPtr(m_rdram, recvBufAddr);
            if (rb)
            {
                std::memset(rb, 0, recvSize);
                // DVCI poll (rpcNum 9) asks "how many async reads still pending?".
                // The data is already delivered (SIF DMA), so report 0 pending so
                // the game stops waiting and advances. Env PS2X_DVCI_DONE gates the
                // experiment; other cmds keep returning handle=1.
                static const bool s_done = [](){ const char *v = std::getenv("PS2X_DVCI_DONE"); return v && v[0] && v[0] != '0'; }();
                uint32_t val = (s_done && sid == 0x2000004u && rpcNum == 9u) ? 0u : 1u;
                std::memcpy(rb, &val, sizeof(val) <= recvSize ? sizeof(val) : recvSize);
            }
            resultPtr = recvBufAddr;
        }
        signalNowaitCompletion = true;
        return true;
    }

    if (ps2_syscalls::handleSoundDriverRpcService(m_rdram,
                                                  runtime,
                                                  sid,
                                                  rpcNum,
                                                  sendBufAddr,
                                                  sendSize,
                                                  recvBufAddr,
                                                  recvSize,
                                                  resultPtr,
                                                  signalNowaitCompletion))
    {
        return true;
    }

    if (ps2_iop_dbcman::handleDbcManRpc(m_rdram,
                                        sid,
                                        rpcNum,
                                        sendBufAddr,
                                        sendSize,
                                        recvBufAddr,
                                        recvSize,
                                        resultPtr))
    {
        return true;
    }

    if (ps2_iop_audio::handleLibSdRpc(m_rdram,
                                      runtime,
                                      sid,
                                      rpcNum,
                                      sendBufAddr,
                                      sendSize,
                                      recvBufAddr,
                                      recvSize,
                                      resultPtr))
    {
        return true;
    }

    if (ps2_iop_cl::handleSoundRpc(m_rdram, sid,
                                   rpcNum, sendBufAddr,
                                   sendSize, recvBufAddr,
                                   recvSize, resultPtr,
                                   signalNowaitCompletion))
    {
        return true;
    }

    if (ps2_iop_cl::handleClFileRpc(m_rdram, sid,
                                    rpcNum, sendBufAddr,
                                    sendSize, recvBufAddr,
                                    recvSize, resultPtr))
    {
        return true;
    }

    if (ps2_iop_sdrdrv::handleSdrdrvRpc(m_rdram, sid,
                                        rpcNum, sendBufAddr,
                                        sendSize, recvBufAddr,
                                        recvSize, resultPtr))
    {
        return true;
    }

    return false;
}
