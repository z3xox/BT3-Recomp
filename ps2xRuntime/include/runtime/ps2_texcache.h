#pragma once
// [texcache] Persistent, write-back texture cache.
//
// Keyed by the renderer's texture-cache key (texKey), it stores the FINAL, GPU-ready
// payload for each texture the runtime has already resolved (decoded PSMT + pack
// replacement applied). A hit means "already resolved": the runtime uploads the cached
// bytes and skips the PSMT decode, the pack file scan/lookup and the file decode.
//
// The file is filled progressively (new entries on each miss) and persisted across
// runs. On startup it is loaded once; entries are staged in RAM and flushed in one
// atomic rewrite (header + index + payload blob). Invalidation is by the header's
// schema/pack/data/variant hashes.

#include <cstdint>
#include <cstddef>

namespace ps2texcache
{
    struct Meta
    {
        uint64_t tex0 = 0;
        uint64_t clut = 0;
        uint32_t bits = 0;
    };

    // Configure before load(). packHash/dataHash identify the current pack + game data;
    // a mismatch with the file header discards the file. path can be overridden by
    // PS2X_TEXCACHE.
    void setConfig(bool enabled, uint64_t packHash, uint64_t dataHash, const char *path);

    bool enabled();

    // Load the file (no-op if disabled or absent). Returns true when an index is live.
    bool load();

    // Lookup by texKey. On hit, `data` points into the loaded blob (valid until flush)
    // and w/h/fmt/scale/alpha are filled. `fmt` uses the renderer convention (0 = RGBA8,
    // non-zero = compressed DDS format).
    bool get(uint64_t texKey, const uint8_t *&data, size_t &len,
             int &w, int &h, int &fmt, int &scale, float &alpha, Meta *meta = nullptr);

    // Stage a new/updated entry (copies the bytes). Bounded by PS2X_TEXCACHE_MAX_MB.
    void add(uint64_t texKey, const uint8_t *data, size_t len,
             int w, int h, int fmt, int scale, float alpha, const Meta &meta);

    // Number of staged entries not yet flushed.
    size_t pending();

    // Atomic rewrite of the whole file (header + index + payloads). Safe to call often.
    bool flush();
}
