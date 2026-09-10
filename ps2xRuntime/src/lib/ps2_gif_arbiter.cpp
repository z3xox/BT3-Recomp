#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_gs_pgs.h"   // [pgs]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// [giflock] lock the queue, counting only the acquisitions that actually had to wait, so the cost of the
// lock is measured instead of guessed ([giflock] line every 5 s; PS2X_GIFLOCKSTAT=0 silences it).
struct CountedLock
{
    std::mutex &m;
    CountedLock(std::mutex &mm, std::atomic<uint64_t> &waits, std::atomic<uint64_t> &ns) : m(mm)
    {
        if (m.try_lock()) return;
        const auto t0 = std::chrono::steady_clock::now();
        m.lock();
        ns.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
        waits.fetch_add(1, std::memory_order_relaxed);
    }
    ~CountedLock() { m.unlock(); }
};
}

// Diagnostic: pathId of the packet currently being dispatched to the GS (1=XGKICK, 2=DIRECT,
// 3=path3 DMA, 0=idle). Consumed by the runtime's process callback to tag GS::m_curSrcPath.
uint8_t g_gifArbCurPath = 0;

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);   // the copy stays OUTSIDE the lock
    CountedLock lk(m_qMtx, m_lockWaits, m_lockWaitNs);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::takeQueue(std::vector<GifArbiterPacket> &out)
{   // [vu1pipe] the ordering drain() applies, without processing
    static const bool s_sort = [](){ const char *v = std::getenv("PS2X_GIF_SORT"); return v && v[0] && v[0] != '0'; }();
    static const bool s_stat = [](){ const char *v = std::getenv("PS2X_GIFLOCKSTAT"); return !(v && v[0] == '0'); }();
    CountedLock lk(m_qMtx, m_lockWaits, m_lockWaitNs);
    if (s_sort)
        std::stable_sort(m_queue.begin(), m_queue.end(),
                         [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                         {
                             if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                             {
                                 if (a.path3Image && b.path2DirectHl)
                                     return true;
                                 if (a.path2DirectHl && b.path3Image)
                                     return false;
                             }
                             return pathPriority(a.pathId) < pathPriority(b.pathId);
                         });
    out.swap(m_queue);
    m_queue.clear();
    if (s_stat)
    {
        static auto s_t0 = std::chrono::steady_clock::now(); static uint64_t s_w0 = 0, s_n0 = 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - s_t0 >= std::chrono::seconds(5))
        {
            const uint64_t w = m_lockWaits.load(std::memory_order_relaxed), n = m_lockWaitNs.load(std::memory_order_relaxed);
            const double secs = std::chrono::duration<double>(now - s_t0).count();
            std::fprintf(stderr, "[giflock] contended %.0f/s, waiting %.3f ms/s\n", (double)(w - s_w0) / secs, (double)(n - s_n0) / 1e6 / secs);
            s_t0 = now; s_w0 = w; s_n0 = n;
        }
    }
}
void GifArbiter::process(const GifArbiterPacket &pkt)
{
    if (!m_processFn || pkt.data.empty()) return;
    if (ps2x_pgs::enabled())
    {   // [pgs] the paraLLEl-GS backend consumes the same packet, on its own path index. Pack mode: OUR parse first, so the
        // VRAM and palettes its replacement hook hashes already include this packet's uploads.
        if (ps2x_pgs::packMode())
        {
            g_gifArbCurPath = static_cast<uint8_t>(pkt.pathId);
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            g_gifArbCurPath = 0;
            ps2x_pgs::gifTransfer(static_cast<uint8_t>(pkt.pathId), pkt.data.data(), pkt.data.size());
            return;
        }
        const bool consumed = ps2x_pgs::gifTransfer(static_cast<uint8_t>(pkt.pathId), pkt.data.data(), pkt.data.size());
        if (consumed && ps2x_pgs::exclusive()) return;   // not consumed (backend unavailable): our parse takes it
    }
    g_gifArbCurPath = static_cast<uint8_t>(pkt.pathId);
    m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
    g_gifArbCurPath = 0;
}
void GifArbiter::drain()
{
    if (!m_processFn)
        return;
    std::vector<GifArbiterPacket> q;
    takeQueue(q);
    for (const auto &pkt : q) process(pkt);
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
