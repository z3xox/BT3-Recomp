#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

float PS2AudioBackend::s_masterVolume = 1.0f;
float PS2AudioBackend::s_musicVolume = 1.0f;
float PS2AudioBackend::s_sfxVolume = 1.0f;

namespace
{
    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

// Frames per raylib AudioStream sub-buffer. Feeds are aligned to this exactly (see
// serviceStreams): a partial feed leaves the rest of the sub-buffer unfilled and is heard as
// rapid pause/unpause stutter. At 24kHz this is ~85ms; raylib double-buffers, so it gives ~170ms
// of slack -- enough to ride out a dropped frame without underrunning (the reported "stereo goes
// out of sync when I lose fps").
static constexpr size_t kStreamChunkFrames = 1024;

struct PS2AudioBackend::Impl
{
    struct DeviceProgress
    {
        uint64_t played = 0;
        size_t queued = 0;
        uint32_t primingSlots = 2;
    };
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;
    // Open raylib AudioStreams for the streaming-PCM path, keyed by streamId.
    std::unordered_map<uint32_t, AudioStream> streams;
    // PCM that is still inside raylib/Core Audio rather than in StreamState::ring.
    std::unordered_map<uint32_t, DeviceProgress> deviceProgress;
};

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
        stopAll();
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

namespace
{
    // Kept out of StreamState (and therefore out of the header) on purpose: both are pure
    // instrumentation/heuristics for the streaming path, and touching ps2_audio.h forces a
    // near-full rebuild of the runner. Always called with m_streamMutex held.
    std::map<uint32_t, std::chrono::steady_clock::time_point> g_streamGrew;

    void ps2xStreamGrew(uint32_t streamId)
    {
        g_streamGrew[streamId] = std::chrono::steady_clock::now();
    }

    // Milliseconds since this stream last received data. A stream that has stopped growing is
    // never going to reach a start cushion, whatever the cushion is set to.
    long ps2xStreamIdleMs(uint32_t streamId)
    {
        auto it = g_streamGrew.find(streamId);
        if (it == g_streamGrew.end())
            return 0;
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - it->second).count());
    }

    // [sndlevel] PS2X_SNDLEVEL=1. Per-stream RMS and peak of the PCM the guest actually sends.
    // The PS2 mixes all 12 IOP rings through SPU2 with per-voice volume; we play each ring as
    // its own host stream at unity gain. If one ring comes out too loud, this says whether its
    // payload is genuinely hot (so the missing piece is an attenuation the IOP would apply) or
    // level-matched to the others (so the fault is on our side).
    struct StreamLevel { double sumSq = 0.0; uint64_t n = 0; int32_t peak = 0; };
    std::map<uint32_t, StreamLevel> g_streamLevel;

    void ps2xStreamLevel(uint32_t streamId, const int16_t *samples, uint32_t count)
    {
        static const bool s_on = []() {
            const char *v = std::getenv("PS2X_SNDLEVEL");
            return v && v[0] && v[0] != '0';
        }();
        if (!s_on)
            return;
        StreamLevel &lv = g_streamLevel[streamId];
        for (uint32_t i = 0; i < count; ++i)
        {
            const int32_t s = samples[i];
            lv.sumSq += static_cast<double>(s) * static_cast<double>(s);
            if (std::abs(s) > lv.peak)
                lv.peak = std::abs(s);
        }
        lv.n += count;
        static std::atomic<uint32_t> k{0};
        if ((k.fetch_add(1) % 400u) == 0u)
        {
            std::fprintf(stderr, "[sndlevel]");
            for (const auto &e : g_streamLevel)
                std::fprintf(stderr, " s%u:rms=%.0f/peak=%d", e.first,
                             e.second.n ? std::sqrt(e.second.sumSq / static_cast<double>(e.second.n)) : 0.0,
                             e.second.peak);
            std::fprintf(stderr, "\n");
        }
    }
} // namespace

void PS2AudioBackend::onStreamPcmMix(uint32_t streamId, const int16_t *samples,
                                     uint32_t sampleCount, uint32_t sampleRate)
{
    if (!samples || sampleCount == 0u)
        return;

    std::lock_guard<std::mutex> lock(m_streamMutex);
    StreamState &st = m_streams[streamId];
    if (sampleRate)
        st.sampleRate = sampleRate;

    // ring[0] is the next sample the device will take, so mixing from index 0 starts this
    // effect immediately rather than behind whatever is still queued.
    if (st.ring.size() < sampleCount)
        st.ring.resize(sampleCount, 0);
    for (uint32_t i = 0; i < sampleCount; ++i)
    {
        int32_t v = static_cast<int32_t>(st.ring[i]) + static_cast<int32_t>(samples[i]);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        st.ring[i] = static_cast<int16_t>(v);
    }
    ps2xStreamGrew(streamId);
    ps2xStreamLevel(streamId, samples, sampleCount);
}

void PS2AudioBackend::onStreamPcm(uint32_t streamId, const int16_t *samples, uint32_t sampleCount,
                                  uint32_t sampleRate)
{
    if (!samples || sampleCount == 0u)
        return;

    std::lock_guard<std::mutex> lock(m_streamMutex);
    StreamState &st = m_streams[streamId];
    if (sampleRate)
        st.sampleRate = sampleRate;

    {
        static std::atomic<uint32_t> n{0};
        const uint32_t k = n.fetch_add(1);
        if (k < 6u || (k % 100u) == 0u)
            std::fprintf(stderr, "[sndplay] recv #%u stream=%u samples=%u rate=%u backlog=%zu\n",
                         k + 1u, streamId, sampleCount, st.sampleRate, st.ring.size());
    }

    // Accept everything here. Trimming is done in serviceStreams instead, because dropping
    // per-stream independently breaks stereo alignment: if L overflows and R does not, the
    // two sides lose sync permanently. Dropping the OLDEST samples is also heard directly as
    // audio "jumping forward", so the cap must be generous and any trim symmetric.
    st.ring.insert(st.ring.end(), samples, samples + sampleCount);
    ps2xStreamGrew(streamId);
    ps2xStreamLevel(streamId, samples, sampleCount);
}

size_t PS2AudioBackend::streamBacklog(uint32_t streamId)
{
    std::lock_guard<std::mutex> lock(m_streamMutex);
    auto it = m_streams.find(streamId);
    return (it == m_streams.end()) ? 0u : it->second.ring.size();
}

PS2AudioBackend::StreamProgress PS2AudioBackend::streamProgress(uint32_t streamId)
{
    StreamProgress p;
    std::lock_guard<std::mutex> lock(m_streamMutex);
    auto it = m_streams.find(streamId);
    if (it == m_streams.end())
        return p;
    p.known = true;
    p.started = it->second.started;
    // `fed` only means that PCM was copied into raylib's double buffer.  Treating it as
    // played returns the IOP ring immediately, so BT3 sends STOP while the first character
    // call-out is still sitting in Core Audio.  `played` moves only when raylib reports a
    // previously-filled sub-buffer free again.
    auto dev = m_impl->deviceProgress.find(streamId);
    p.consumedSamples = (dev != m_impl->deviceProgress.end()) ? dev->second.played : 0u;
    p.gapSamples = it->second.gap;
    p.pending = it->second.ring.size();
    return p;
}

void PS2AudioBackend::registerStreamRing(uint32_t base, uint32_t size)
{
    if (!base || !size)
        return;
    std::lock_guard<std::mutex> lock(m_streamMutex);
    // Reject anything that lies INSIDE a ring we already know. The caller offers the sink's
    // free-list head whenever the list is a single node with nothing queued, and that is only
    // the whole ring while the stream is idle -- mid-playback it can legitimately be a partial
    // node (e.g. 0x1440..0x4140 out of ring 0's 0x140..0x4140). Registering those would shadow
    // the real spans, and a partial node starting in a ring's TAIL would resolve to the next
    // stream id and misroute its audio -- the exact bug the ring map exists to prevent.
    {
        auto hit = m_streamRings.upper_bound(base);
        if (hit != m_streamRings.begin())
        {
            --hit;
            if (base - hit->first < hit->second.first)
                return; // inside a known ring
        }
    }
    auto it = m_streamRings.find(base);
    if (it != m_streamRings.end() && it->second.first >= size)
        return;
    // The id stays `base >> 14` so it matches every id already in use; only the ADDRESS
    // resolution changes, and only for the tail bytes that used to land in the next bucket.
    m_streamRings[base] = {size, base >> 14};
    std::fprintf(stderr, "[sndplay] ring stream=%u 0x%x..0x%x (%u bytes)\n",
                 base >> 14, base, base + size, size);
}

uint32_t PS2AudioBackend::streamIdForAddress(uint32_t iopAddr)
{
    std::lock_guard<std::mutex> lock(m_streamMutex);
    auto it = m_streamRings.upper_bound(iopAddr);
    if (it != m_streamRings.begin())
    {
        --it;
        if (iopAddr - it->first < it->second.first)
            return it->second.second;
    }
    return iopAddr >> 14; // no ring registered yet: the old split is the best guess
}

void PS2AudioBackend::noteStreamGap(uint32_t streamId, uint32_t bytes)
{
    std::lock_guard<std::mutex> lock(m_streamMutex);
    auto it = m_streams.find(streamId);
    if (it != m_streams.end())
        it->second.gap += bytes / 2u; // 16-bit samples
}

void PS2AudioBackend::requestStreamStart(uint32_t streamId)
{
    std::lock_guard<std::mutex> lock(m_streamMutex);
    auto it = m_streams.find(streamId);
    if (it != m_streams.end())
        it->second.forceStart = true;
}

size_t PS2AudioBackend::dropStreamPending(uint32_t streamId)
{
    std::lock_guard<std::mutex> lock(m_streamMutex);
    auto it = m_streams.find(streamId);
    if (it == m_streams.end())
        return 0u;
    const size_t n = it->second.ring.size();
    it->second.ring.clear();
    it->second.dropped += n;
    return n;
}

void PS2AudioBackend::serviceStreams()
{
#if defined(PLATFORM_VITA)
    return;
#else
    {
        static std::atomic<uint32_t> svc{0};
        const uint32_t s = svc.fetch_add(1);
        if (s == 0u)
            std::fprintf(stderr, "[sndplay] serviceStreams live, audioReady=%d\n", m_audioReady ? 1 : 0);
    }
    if (!m_audioReady)
        return;

    std::lock_guard<std::mutex> lock(m_streamMutex);

    // ---- STEREO PAIRING -------------------------------------------------------------
    // Streams 0 and 1 are the LEFT and RIGHT halves of one stereo source. Playing them as
    // two independent mono AudioStreams lets each drift on its own clock; two correlated
    // copies offset by tens of ms is heard as the music playing "twice over each other"
    // with a hollow, low-quality (comb-filtered) tone. Interleaving them into ONE 2-channel
    // stream keeps them sample-locked by construction. Other ids (SE banks) stay mono.
    // PS2X_SNDNOPAIR=1 restores the old independent-mono behaviour.
    constexpr uint32_t kLeftId = 0u, kRightId = 1u, kPairKey = 0xFFFF0000u;
    static const bool s_noPair = []() {
        const char *v = std::getenv("PS2X_SNDNOPAIR");
        return v && v[0] && v[0] != '0';
    }();
    auto itL = m_streams.find(kLeftId);
    auto itR = m_streams.find(kRightId);
    const bool paired = !s_noPair && itL != m_streams.end() && itR != m_streams.end();
    if (paired)
    {
        StreamState &L = itL->second;
        StreamState &R = itR->second;
        // Key the "already opened?" test on the PAIR stream, not on L.opened. They are not the
        // same question: if the left side was ever seen alone it would have been opened as a
        // mono stream, setting L.opened, and this block would then be skipped forever -- taking
        // a reference to a default-constructed AudioStream below and playing into nothing.
        // The device is opened at whatever rate the first PCM declared and used to keep that
        // rate for the rest of the run. Streams do not all share one rate -- the opening movie's
        // ADX is 48000 while in-game BGM is 24000 -- so a stuck device plays every later stream
        // at the wrong speed (BGM at double speed after the 48 kHz movie). Reopen when the rate
        // actually changes; st.sampleRate is already tracked per stream, only the device was
        // sticky.
        static uint32_t s_pairOpenRate = 0u;
        auto pairIt = m_impl->streams.find(kPairKey);
        if (pairIt != m_impl->streams.end() && L.sampleRate != 0u && L.sampleRate != s_pairOpenRate)
        {
            std::fprintf(stderr, "[sndplay] pair rate %u -> %u, reopening device\n",
                         s_pairOpenRate, L.sampleRate);
            StopAudioStream(pairIt->second);
            UnloadAudioStream(pairIt->second);
            m_impl->streams.erase(pairIt);
            L.started = R.started = false;
            L.opened = R.opened = false;
        }
        if (m_impl->streams.find(kPairKey) == m_impl->streams.end())
        {
            SetAudioStreamBufferSizeDefault(static_cast<int>(kStreamChunkFrames));
            m_impl->streams[kPairKey] = LoadAudioStream(L.sampleRate, 16, 2);
            L.opened = R.opened = true;
            m_impl->deviceProgress[kLeftId] = {};
            m_impl->deviceProgress[kRightId] = {};
            s_pairOpenRate = L.sampleRate;
            std::fprintf(stderr, "[sndplay] opened STEREO PAIR (streams 0+1) rate=%u\n", L.sampleRate);
        }
        AudioStream &s = m_impl->streams[kPairKey];

        // Both sides must have a full sub-buffer; the pair advances in lockstep or not at all.
        if (!L.started)
        {
            // forceStart: the guest has filled its ring and cannot queue more, so this is the
            // largest cushion this stream will ever have. Waiting past that point deadlocks --
            // no playback means no buffer returns means no further data.
            const size_t have = std::min(L.ring.size(), R.ring.size());
            const bool forced = (L.forceStart || R.forceStart) && have >= kStreamChunkFrames;
            if (have >= kStreamChunkFrames * 4u || forced)
            {
                PlayAudioStream(s);
                L.started = R.started = true;
                if (forced)
                    std::fprintf(stderr, "[sndplay] pair force-start with %zu frames cushion\n", have);
            }
        }
        // REPAIR L/R IMBALANCE AT THE FEED (safe place -- gating the pump on the partner's
        // state deadlocked it twice). Measured: the pump can hand one sink a buffer the other
        // never gets, so that channel gains exactly one buffer (~2432 samples) and every later
        // interleave pairs samples ~100ms apart -- "one ear suddenly lags, then stays behind".
        // Both rings are consumed from the FRONT, so the fronts remain aligned; the unmatched
        // audio is the TAIL of the longer ring. Drop that tail: one channel loses a slice of
        // its newest audio (a brief blemish) instead of the pair being offset permanently.
        // MEASURED: the imbalance is CONTINUOUS, not a one-off -- rebalancing every frame
        // discarded 2000-4000 samples at a time, which sounds worse than the drift it fixes.
        // The two sinks genuinely receive different amounts of data over time, so this is the
        // wrong layer to correct it. Left in, default OFF, as a diagnostic: PS2X_SNDREBAL=1.
        static const bool s_rebal = []() {
            const char *v = std::getenv("PS2X_SNDREBAL");
            return v && v[0] && v[0] != '0';
        }();
        if (s_rebal && L.ring.size() != R.ring.size())
        {
            const size_t lo = std::min(L.ring.size(), R.ring.size());
            const size_t excess = std::max(L.ring.size(), R.ring.size()) - lo;
            if (L.ring.size() > lo) L.ring.resize(lo);
            else                    R.ring.resize(lo);
            static std::atomic<uint32_t> rb{0};
            const uint32_t k = rb.fetch_add(1);
            if (k < 8u || (k % 100u) == 0u)
                std::fprintf(stderr, "[sndplay] pair rebalance #%u dropped %zu tail samples (now %zu/%zu)\n",
                             k + 1u, excess, L.ring.size(), R.ring.size());
        }

        if (L.started)
        {
            const float musicVol = s_masterVolume * s_musicVolume;
            std::vector<int16_t> inter(kStreamChunkFrames * 2u);
            while (IsAudioStreamProcessed(s) &&
                   std::min(L.ring.size(), R.ring.size()) >= kStreamChunkFrames)
            {
                // A new AudioStream starts with two empty sub-buffers.  The first two
                // processed notifications merely let us prime them; every later one means a
                // complete stereo chunk was actually heard.
                auto &lDev = m_impl->deviceProgress[kLeftId];
                auto &rDev = m_impl->deviceProgress[kRightId];
                if (lDev.primingSlots != 0u)
                {
                    --lDev.primingSlots;
                    --rDev.primingSlots;
                }
                else if (lDev.queued >= kStreamChunkFrames &&
                         rDev.queued >= kStreamChunkFrames)
                {
                    lDev.queued -= kStreamChunkFrames;
                    rDev.queued -= kStreamChunkFrames;
                    lDev.played += kStreamChunkFrames;
                    rDev.played += kStreamChunkFrames;
                }
                for (size_t i = 0; i < kStreamChunkFrames; ++i)
                {
                    inter[i * 2u]     = static_cast<int16_t>(L.ring[i] * musicVol);
                    inter[i * 2u + 1u] = static_cast<int16_t>(R.ring[i] * musicVol);
                }
                UpdateAudioStream(s, inter.data(), static_cast<int>(kStreamChunkFrames));
                L.ring.erase(L.ring.begin(), L.ring.begin() + static_cast<long>(kStreamChunkFrames));
                R.ring.erase(R.ring.begin(), R.ring.begin() + static_cast<long>(kStreamChunkFrames));
                L.fed += kStreamChunkFrames;
                R.fed += kStreamChunkFrames;
                lDev.queued += kStreamChunkFrames;
                rDev.queued += kStreamChunkFrames;
                static std::atomic<uint32_t> pf{0};
                const uint32_t k = pf.fetch_add(1);
                if (k < 4u || (k % 300u) == 0u)
                    std::fprintf(stderr, "[sndplay] pair fed #%u frames=%zu Lback=%zu Rback=%zu\n",
                                 k + 1u, kStreamChunkFrames, L.ring.size(), R.ring.size());
            }

            // SYMMETRIC trim. A frame-rate dip means serviceStreams runs less often and the
            // rings grow; if they are ever trimmed by different amounts the stereo image
            // desyncs for good. Always drop the SAME count from both sides, and only once the
            // backlog is genuinely excessive, since dropping is audible as a forward jump.
            constexpr size_t kMaxBacklog = 192000; // ~8s at 24kHz -- generous on purpose
            const size_t worst = std::max(L.ring.size(), R.ring.size());
            if (worst > kMaxBacklog)
            {
                const size_t excess = worst - kMaxBacklog;
                const size_t trim = std::min(excess, std::min(L.ring.size(), R.ring.size()));
                if (trim)
                {
                    L.ring.erase(L.ring.begin(), L.ring.begin() + static_cast<long>(trim));
                    R.ring.erase(R.ring.begin(), R.ring.begin() + static_cast<long>(trim));
                    L.dropped += trim;
                    R.dropped += trim;
                    std::fprintf(stderr, "[sndplay] pair trim %zu samples (backlog was %zu)\n",
                                 trim, worst);
                }
            }
        }
    }

    for (auto &entry : m_streams)
    {
        const uint32_t id = entry.first;
        StreamState &st = entry.second;
        // Reserve the BGM pair for the pair path UNCONDITIONALLY, not just once both sides have
        // arrived. Their first DMAs land microseconds apart, so a serviceStreams call landing
        // between them would otherwise open the left channel as a lone MONO stream -- after
        // which the pair can never form and the BGM is silent for the rest of the run. That is
        // a pure startup race: it depends only on which side's first transfer wins.
        if (!s_noPair && (id == kLeftId || id == kRightId))
            continue; // handled above as one stereo stream

        // PS2X_SNDCHANNELS: how to interpret the payload. If the game is actually sending
        // STEREO INTERLEAVED frames and we play them as mono, playback runs exactly 2x fast
        // -- one octave up, the classic "chipmunk" symptom. Switching to 2 fixes both the
        // pitch and the stereo image, so this is the first thing to try when pitch is wrong.
        static const uint32_t s_channels = []() -> uint32_t {
            if (const char *v = std::getenv("PS2X_SNDCHANNELS"))
            {
                const long n = std::strtol(v, nullptr, 10);
                if (n == 1 || n == 2) return static_cast<uint32_t>(n);
            }
            return 1u;
        }();

        if (!st.opened)
        {
            // raylib refills an AudioStream in fixed-size sub-buffers. Feeding fewer frames
            // than a whole sub-buffer leaves the remainder unfilled, which is heard as rapid
            // dropouts (~10/sec) that sound like pause/unpause stutter. Pin the sub-buffer
            // size and always hand over exactly that many frames.
            SetAudioStreamBufferSizeDefault(static_cast<int>(kStreamChunkFrames));
            AudioStream s = LoadAudioStream(st.sampleRate, 16, s_channels);
            m_impl->streams[id] = s;
            st.opened = true;
            m_impl->deviceProgress[id] = {};
            std::fprintf(stderr, "[sndplay] opened stream=%u rate=%u channels=%u\n",
                         id, st.sampleRate, s_channels);
        }
        AudioStream &s = m_impl->streams[id];

        // Wait for a little cushion before starting so the first buffers do not underrun.
        // ---- ONE-SHOT SOUNDS (punches, explosions, menu blips) --------------------------
        // These arrive as a single short burst -- a punch is well under the 256ms start
        // cushion -- so the stream would sit below the threshold FOREVER and never play a
        // sample. Measured directly: streams 4 and 10 were opened during a fight and fed
        // exactly nothing. Once a stream has stopped receiving data it is not going to reach
        // any cushion, so treat "gone quiet" as "this sound is complete": pad it up to a whole
        // sub-buffer with silence and let it play.
        //
        // Padding to a WHOLE sub-buffer matters -- raylib leaves the remainder of a partly
        // filled one unplayed, which is the ~10/sec pause-unpause stutter. It is also what
        // rescues the tail of every sound, since the last <1024 samples could never be fed.
        //
        // Continuously-fed streams (BGM, voice) never go idle, so they keep the normal cushion.
        static const long s_idleMs = []() -> long {
            if (const char *v = std::getenv("PS2X_SNDIDLE_MS"))
            {
                const long n = std::strtol(v, nullptr, 10);
                if (n > 0) return n;
            }
            return 100;
        }();
        if (!st.ring.empty() && (st.ring.size() % (kStreamChunkFrames * s_channels)) != 0u &&
            ps2xStreamIdleMs(id) >= s_idleMs)
        {
            const size_t chunk = kStreamChunkFrames * s_channels;
            const size_t padded = ((st.ring.size() + chunk - 1u) / chunk) * chunk;
            static std::atomic<uint32_t> p{0};
            const uint32_t k = p.fetch_add(1);
            if (k < 8u || (k % 100u) == 0u)
                std::fprintf(stderr, "[sndplay] stream=%u one-shot complete: %zu -> %zu samples\n",
                             id, st.ring.size(), padded);
            st.ring.resize(padded, 0);
        }

        if (!st.started)
        {
            // Build several whole sub-buffers before starting, so the first refills never run
            // dry. A voice line arrives as a burst, so the IOP needs this head start before
            // the real device clock begins returning ring capacity.
            // The cushion MUST stay BELOW the pump's backlog target (PS2X_SNDBACKLOG, default
            // 8192) or the two deadlock: the pump stops handing buffers back once it reaches
            // the target, a larger start threshold is then never reached, and the stream never
            // plays at all. 6 x 1024 = 6144 < 8192.
            static const size_t s_cushion = []() -> size_t {
                if (const char *v = std::getenv("PS2X_SNDCUSHION"))
                {
                    const long n = std::strtol(v, nullptr, 10);
                    if (n > 0) return static_cast<size_t>(n);
                }
                return kStreamChunkFrames * 6u;
            }();
            // forceStart: see the pair path -- once the guest's ring is full the cushion can
            // only shrink, so holding out for a bigger one would stall the stream for good.
            // The idle case is the one-shot above: the sound is already complete, so the
            // cushion has no underrun to protect against and waiting for it means silence.
            const bool complete = ps2xStreamIdleMs(id) >= s_idleMs;
            const size_t need = (st.forceStart || complete) ? kStreamChunkFrames * s_channels
                                                            : s_cushion * s_channels;
            if (st.ring.size() < need)
                continue;
            PlayAudioStream(s);
            st.started = true;
            if (st.forceStart)
                std::fprintf(stderr, "[sndplay] stream=%u force-start with %zu samples cushion\n",
                             id, st.ring.size());
        }

        // raylib refills in fixed-size chunks; feed while it has room and we have data.
        while (IsAudioStreamProcessed(s) && !st.ring.empty())
        {
            // Only ever hand over a COMPLETE sub-buffer; a short feed is what causes the
            // stutter. If we do not have a full one yet, leave it for the next frame.
            const size_t chunk = kStreamChunkFrames * s_channels;
            if (st.ring.size() < chunk)
                break;
            auto &dev = m_impl->deviceProgress[id];
            // See the paired-stream path above: do not credit the IOP until a buffer that
            // contained audio has become free.  This holds the guest's STOP/cleanup path off
            // for the real device latency without changing the game's stream state machine.
            if (dev.primingSlots != 0u)
            {
                --dev.primingSlots;
            }
            else if (dev.queued >= chunk)
            {
                dev.queued -= chunk;
                dev.played += chunk;
            }
            // Apply SFX volume (master * sfx) to this chunk. Mono streams carry voices and
            // effects; scaling here is cheaper than a second ring pass.
            const float sfxVol = s_masterVolume * s_sfxVolume;
            if (sfxVol < 0.999f || sfxVol > 1.001f)
            {
                std::vector<int16_t> scaled(st.ring.begin(), st.ring.begin() + static_cast<long>(chunk));
                for (auto &v : scaled) v = static_cast<int16_t>(v * sfxVol);
                // UpdateAudioStream counts FRAMES, not samples.
                UpdateAudioStream(s, scaled.data(), static_cast<int>(kStreamChunkFrames));
            }
            else
            {
                // UpdateAudioStream counts FRAMES, not samples.
                UpdateAudioStream(s, st.ring.data(), static_cast<int>(kStreamChunkFrames));
            }
            st.ring.erase(st.ring.begin(), st.ring.begin() + static_cast<long>(chunk));
            st.fed += chunk;
            dev.queued += chunk;
            static std::atomic<uint32_t> f{0};
            const uint32_t k = f.fetch_add(1);
            if (k < 6u || (k % 200u) == 0u)
                std::fprintf(stderr, "[sndplay] fed #%u stream=%u chunk=%zu total=%llu backlog=%zu dropped=%llu\n",
                             k + 1u, id, chunk, (unsigned long long)st.fed, st.ring.size(),
                             (unsigned long long)st.dropped);
        }

        // Same generous cap for mono streams (the voice lines). Dropping the oldest samples
        // is heard as the voice skipping/jumping forward, which is why the previous 96000-
        // sample cap in onStreamPcm was audible -- only trim when truly excessive.
        constexpr size_t kMaxBacklogMono = 192000;
        if (st.ring.size() > kMaxBacklogMono)
        {
            const size_t excess = st.ring.size() - kMaxBacklogMono;
            st.ring.erase(st.ring.begin(), st.ring.begin() + static_cast<long>(excess));
            st.dropped += excess;
            std::fprintf(stderr, "[sndplay] stream=%u trim %zu samples\n", id, excess);
        }
    }
#endif
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume * s_masterVolume * s_sfxVolume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}
