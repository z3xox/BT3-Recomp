#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    std::vector<uint8_t> data;
};

class GifArbiter
{
public:
    using ProcessPacketFn = std::function<void(const uint8_t *, uint32_t)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);

    void setProcessPacketFn(ProcessPacketFn fn) { m_processFn = std::move(fn); }

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false);

    void drain();
    void takeQueue(std::vector<GifArbiterPacket> &out);   // [vu1pipe] hand the queued packets (ordered as drain would) to another thread
    void process(const GifArbiterPacket &pkt);              // [vu1pipe] run the process function on one packet (the thread that owns the GS)

private:
    ProcessPacketFn m_processFn;
    // [giflock] m_queue is written by the EE guest thread (GS/font stubs -> processPendingTransfers ->
    // submitGifPacket) AND by the kick worker (VIF1/path3), and swapped out by the stage-2 thread when
    // [vu1pipe] is on. Unsynchronised, that corrupts the host heap: two crashes on 2026-09-10 aborted in
    // malloc/free (cores 781549, 787693). The lock covers ONLY the queue operations; packet processing,
    // which is where the time goes, stays outside it.
    std::mutex m_qMtx;
    std::atomic<uint64_t> m_lockWaits{0};    // times a thread had to wait
    std::atomic<uint64_t> m_lockWaitNs{0};   // total wait, nanoseconds
    std::vector<GifArbiterPacket> m_queue;

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
