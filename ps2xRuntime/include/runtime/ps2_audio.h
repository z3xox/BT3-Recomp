#ifndef PS2_AUDIO_H
#define PS2_AUDIO_H

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class PS2AudioBackend
{
public:
    PS2AudioBackend();
    ~PS2AudioBackend();

    void onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes);
    void onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr);
    void onSoundCommand(uint32_t sid, uint32_t rpcNum,
                        const uint8_t *sendBuf, uint32_t sendSize,
                        uint8_t *recvBuf, uint32_t recvSize);

    // Streaming PCM path (BT3 and other DTX/SJX titles). The game DMAs already-decoded
    // 16-bit mono PCM to the IOP in double-buffered slots; there is no libsd voice command
    // to hook, so the SIF DMA path feeds it here directly. `streamId` separates concurrent
    // streams (BT3 uses one per stereo side). Data is buffered and drained into a raylib
    // AudioStream by serviceStreams(), which must be called from the render thread.
    void onStreamPcm(uint32_t streamId, const int16_t *samples, uint32_t sampleCount,
                     uint32_t sampleRate);
    // One-shot SFX variant: MIXES into what is already queued instead of appending after it.
    // Sound effects are not a stream -- on the real SPU each one takes its own voice and they
    // overlap. Appending makes rapid effects (menu cursor spam) queue up and play one after the
    // other, drifting further behind the action with every press. Mixing from the head of the
    // buffer starts each effect at the next device chunk, so they overlap and the backlog can
    // never grow past the longest single effect.
    void onStreamPcmMix(uint32_t streamId, const int16_t *samples, uint32_t sampleCount,
                        uint32_t sampleRate);
    void serviceStreams();
    // Backlog awaiting the device for ONE stream, in samples. The buffer-return pump uses this
    // to self-clock: hand a buffer back only when that stream has drained enough, so the guest
    // produces at exactly the rate audio is consumed instead of on a drifting wall clock.
    // Deliberately PER-STREAM: a max across all streams couples them, so a single stream that
    // is not draining pins the value above target and starves every other stream (this is what
    // made the title music cut out a couple of seconds in).
    size_t streamBacklog(uint32_t streamId);

    // Honest playback clock, used to emulate the IOP handing streaming buffers back.
    // `consumedSamples` only advances when the device frees a sub-buffer, so it ticks at
    // exactly the real playback rate -- that is the ONE signal the guest can use to pace
    // its own streaming and to know a sound is not finished until it has actually drained.
    struct StreamProgress
    {
        bool known = false;             // the guest has sent PCM for this id
        bool started = false;           // the device is playing it
        uint64_t consumedSamples = 0;   // cumulative samples accepted by the device
        uint64_t gapSamples = 0;        // guest samples that never reached the device
        size_t pending = 0;             // samples still queued ahead of the device
    };
    StreamProgress streamProgress(uint32_t streamId);
    // "The guest cannot queue any more -- start with what you have." The start cushion is a
    // host-underrun guard, but it must never outlast the guest's ability to feed it: once the
    // IOP ring is full the cushion can only shrink from here, so waiting longer would deadlock.
    void requestStreamStart(uint32_t streamId);
    // Guest PCM that the transfer path chose not to render (too small to be worth a feed, or
    // unreadable). It still occupied the guest's ring, so it has to be accounted as consumed --
    // otherwise those bytes are never "played", never handed back, and the ring slowly clogs.
    // Only counted for streams that are already rendering, so it can never invent one.
    void noteStreamGap(uint32_t streamId, uint32_t bytes);

    // IOP ring geometry. Splitting streams by `iopAddr >> 14` is WRONG at the ring ends: BT3's
    // rings are 16KB but start at 0x140 + i*0x4100, so every ring's tail crosses into the next
    // 16KB bucket and its samples are credited to the NEXT stream. For the BGM pair that fed
    // ~160 LEFT-channel samples into the RIGHT channel on every ring wrap (~469 samples/sec) --
    // which is exactly the permanent L/R offset that no amount of pump balancing could fix.
    // Register each ring as it is discovered and resolve addresses against the real spans.
    void registerStreamRing(uint32_t base, uint32_t size);
    uint32_t streamIdForAddress(uint32_t iopAddr);
    // Discard whatever is still queued for a stream (the IOP drops its ring when a stream
    // is restarted). Returns the number of samples dropped.
    size_t dropStreamPending(uint32_t streamId);

    void play(uint32_t sampleAddr, float pitch = 1.0f, float volume = 1.0f,
              uint32_t voiceIndex = 0xFFFFFFFFu);
    void stop(uint32_t voiceId);
    void stopAll();
    void setAudioReady(bool ready) { m_audioReady = ready; }

    static void setMasterVolume(float v) { s_masterVolume = v; }
    static float masterVolume() { return s_masterVolume; }

    // Per-category volume (multiplied on top of master). Music = the BGM stereo pair
    // (streams 0+1); SFX = mono streams (voices, effects) and VAG one-shots.
    static void setMusicVolume(float v) { s_musicVolume = v; }
    static float musicVolume() { return s_musicVolume; }
    static void setSfxVolume(float v) { s_sfxVolume = v; }
    static float sfxVolume() { return s_sfxVolume; }

private:
    static float s_masterVolume;
    static float s_musicVolume;
    static float s_sfxVolume;
    struct DecodedSample
    {
        std::vector<int16_t> pcm;
        uint32_t sampleRate = 44100;
    };

    struct StreamState
    {
        std::vector<int16_t> ring; // pending PCM awaiting the device
        uint32_t sampleRate = 48000;
        bool started = false;
        bool opened = false;
        bool forceStart = false; // guest ring is full: start without waiting for the cushion
        uint64_t fed = 0;
        uint64_t dropped = 0;
        uint64_t gap = 0; // samples the guest queued that never reached the device
        // [bgmstall] startup deadlock detection: the guest is blocked waiting for playback
        // credit while we wait for a cushion it will never send. Track whether the ring is
        // still growing; if it has stopped below the cushion, the guest is waiting on us.
        size_t stallFrames = 0;
        std::chrono::steady_clock::time_point stallSince{};
    };
    std::unordered_map<uint32_t, StreamState> m_streams;
    // base -> {size, streamId}, ordered so an address lookup is one upper_bound.
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> m_streamRings;
    std::mutex m_streamMutex;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_audioReady = false;
    uint32_t m_mostRecentSampleKey = 0;
    std::vector<DecodedSample> m_loadOrderSamples;
    std::vector<uint32_t> m_loadOrderSampleKeys;
    std::unordered_map<uint32_t, DecodedSample> m_sampleBank;
    std::mutex m_mutex;

    void playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                          bool isBgm = false);
    void pruneFinishedSounds();
};

#endif
