#include "runtime/ps2_texcache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2texcache
{
namespace
{
    constexpr char kMagic[8] = {'B', 'T', '3', 'T', 'E', 'X', 'C', '\0'};
    constexpr uint32_t kVersion = 2u;   // v2: update-on-rewrite; invalidates caches built with originals

    struct Entry
    {
        uint64_t texKey = 0;
        uint64_t off = 0;
        uint32_t len = 0;
        uint16_t w = 0, h = 0;
        uint8_t fmt = 0;
        uint8_t scale = 1;
        float alpha = 1.0f;
        Meta meta;
    };

    std::mutex g_mx;
    bool g_enabled = false;
    std::string g_path;
    uint64_t g_packHash = 0, g_dataHash = 0;
    bool g_loaded = false;
    std::vector<uint8_t> g_blob;
    std::unordered_map<uint64_t, Entry> g_map;
    size_t g_added = 0;
    size_t g_maxBytes = 0;

    size_t maxBytes()
    {
        if (g_maxBytes) return g_maxBytes;
        const char *v = std::getenv("PS2X_TEXCACHE_MAX_MB");
        const long mb = (v && v[0]) ? std::atol(v) : 2048L;
        g_maxBytes = (size_t)(mb > 0 ? mb : 2048L) * 1024u * 1024u;
        return g_maxBytes;
    }

    // --- little-endian (de)serialization helpers ---------------------------------
    void putU8(std::vector<uint8_t> &b, uint8_t v) { b.push_back(v); }
    void putU16(std::vector<uint8_t> &b, uint16_t v)
    { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
    void putU32(std::vector<uint8_t> &b, uint32_t v)
    { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void putU64(std::vector<uint8_t> &b, uint64_t v)
    { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void putF32(std::vector<uint8_t> &b, float f) { uint32_t v; std::memcpy(&v, &f, 4); putU32(b, v); }

    uint8_t rdU8(const uint8_t *p) { return p[0]; }
    uint16_t rdU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    uint32_t rdU32(const uint8_t *p)
    { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    uint64_t rdU64(const uint8_t *p)
    { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i); return v; }
    float rdF32(const uint8_t *p) { uint32_t v = rdU32(p); float f; std::memcpy(&f, &v, 4); return f; }

    constexpr size_t kHeaderSize = 8 + 4 + 4 + 8 + 8 + 4 + 4 + 8;   // 48
    constexpr size_t kEntrySize = 8 + 8 + 4 + 2 + 2 + 1 + 1 + 4 + 8 + 8 + 4;   // 50

    void putEntry(std::vector<uint8_t> &b, const Entry &e)
    {
        putU64(b, e.texKey); putU64(b, e.off); putU32(b, e.len);
        putU16(b, e.w); putU16(b, e.h); putU8(b, e.fmt); putU8(b, e.scale);
        putF32(b, e.alpha); putU64(b, e.meta.tex0); putU64(b, e.meta.clut); putU32(b, e.meta.bits);
    }

    bool rdEntry(const uint8_t *p, size_t avail, Entry &e)
    {
        if (avail < kEntrySize) return false;
        size_t o = 0;
        e.texKey = rdU64(p + o); o += 8;
        e.off = rdU64(p + o); o += 8;
        e.len = rdU32(p + o); o += 4;
        e.w = rdU16(p + o); o += 2;
        e.h = rdU16(p + o); o += 2;
        e.fmt = rdU8(p + o); o += 1;
        e.scale = rdU8(p + o); o += 1;
        e.alpha = rdF32(p + o); o += 4;
        e.meta.tex0 = rdU64(p + o); o += 8;
        e.meta.clut = rdU64(p + o); o += 8;
        e.meta.bits = rdU32(p + o); o += 4;
        return true;
    }
}

// Defined below; the write path flushes on a threshold so the file survives a kill.
bool flushLocked();

void setConfig(bool enabled, uint64_t packHash, uint64_t dataHash, const char *path)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_enabled = enabled;
    g_packHash = packHash;
    g_dataHash = dataHash;
    const char *env = std::getenv("PS2X_TEXCACHE");
    g_path = (env && env[0]) ? env : (path ? path : "");
    // Persist on normal exit (atexit handlers run before the namespace statics are destroyed).
    static bool s_atexit = false;
    if (!s_atexit) { s_atexit = true; std::atexit([]{ ps2texcache::flush(); }); }
}

bool enabled()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_enabled && !g_path.empty();
}

bool load()
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_loaded = false;
    g_blob.clear();
    g_map.clear();
    g_added = 0;
    if (!g_enabled || g_path.empty()) return false;
    // [texcache] Force a rebuild: ignore any existing file (PS2X_TEXCACHE_REGEN=1). Without this
    // the previously sampled entries are reused even after the pack/settings changed.
    if (const char *rg = std::getenv("PS2X_TEXCACHE_REGEN"); rg && rg[0] && rg[0] != '0')
    {
        std::fprintf(stderr, "[texcache] regen forced: ignoring %s\n", g_path.c_str());
        return false;
    }

    std::ifstream f(g_path, std::ios::binary);
    if (!f.is_open()) return false;   // no cache yet: a fresh one will be written on flush
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    if (data.size() < kHeaderSize) return false;
    if (std::memcmp(data.data(), kMagic, 8) != 0) return false;
    if (rdU32(data.data() + 8) != kVersion) return false;
    if (rdU64(data.data() + 16) != g_packHash || rdU64(data.data() + 24) != g_dataHash)
    {
        std::fprintf(stderr, "[texcache] stale (pack/data changed); rebuilding %s\n", g_path.c_str());
        return false;
    }
    const uint32_t count = rdU32(data.data() + 32);
    const uint64_t blobOff = rdU64(data.data() + 40);
    const size_t indexOff = kHeaderSize;
    if (indexOff + (size_t)count * kEntrySize > data.size()) return false;
    if (blobOff > data.size()) return false;

    // Copy the payload blob out so subsequent adds can append to it safely.
    g_blob.assign(data.begin() + (std::ptrdiff_t)blobOff, data.end());
    for (uint32_t i = 0; i < count; ++i)
    {
        Entry e;
        if (!rdEntry(data.data() + indexOff + (size_t)i * kEntrySize, data.size() - indexOff - (size_t)i * kEntrySize, e))
            continue;
        if (e.off + e.len > g_blob.size()) continue;
        g_map[e.texKey] = e;
    }
    g_loaded = true;
    std::fprintf(stderr, "[texcache] loaded %zu entries (%zu bytes payload) from %s\n",
                 g_map.size(), g_blob.size(), g_path.c_str());
    return true;
}

bool get(uint64_t texKey, const uint8_t *&data, size_t &len,
         int &w, int &h, int &fmt, int &scale, float &alpha, Meta *meta)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_map.find(texKey);
    if (it == g_map.end()) return false;
    const Entry &e = it->second;
    if (e.off + e.len > g_blob.size()) return false;
    data = g_blob.data() + e.off;
    len = e.len; w = e.w; h = e.h; fmt = e.fmt; scale = e.scale; alpha = e.alpha;
    if (meta) *meta = e.meta;
    return true;
}

void add(uint64_t texKey, const uint8_t *data, size_t len,
         int w, int h, int fmt, int scale, float alpha, const Meta &meta)
{
    if (!data || !len) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_enabled || g_path.empty()) return;
    // Insert OR update: the async pack replacement means the first decode caches the ORIGINAL and a
    // later re-decode (with the replacement applied) must be able to overwrite it.
    if (g_blob.size() + len > maxBytes())
    {
        static bool warned = false;
        if (!warned) { warned = true; std::fprintf(stderr, "[texcache] size cap reached; new entries skipped\n"); }
        return;
    }
    Entry e;
    e.texKey = texKey; e.off = g_blob.size(); e.len = (uint32_t)len;
    e.w = (uint16_t)w; e.h = (uint16_t)h; e.fmt = (uint8_t)fmt; e.scale = (uint8_t)scale;
    e.alpha = alpha; e.meta = meta;
    g_blob.insert(g_blob.end(), data, data + len);
    g_map[texKey] = e;
    ++g_added;
    static const size_t s_threshold = []() {
        const char *v = std::getenv("PS2X_TEXCACHE_FLUSH");
        return (size_t)((v && v[0]) ? std::atol(v) : 512L);
    }();
    if (s_threshold && g_added >= s_threshold) flushLocked();
}

size_t pending()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_added;
}

bool flushLocked()
{
    if (!g_enabled || g_path.empty() || g_added == 0) return g_added == 0;

    const std::string tmp = g_path + ".tmp";
    std::vector<uint8_t> header;
    header.reserve(kHeaderSize + g_map.size() * kEntrySize);
    header.insert(header.end(), kMagic, kMagic + 8);
    putU32(header, kVersion);
    putU32(header, 0);   // flags
    putU64(header, g_packHash);
    putU64(header, g_dataHash);
    putU32(header, (uint32_t)g_map.size());
    putU32(header, 0);
    putU64(header, kHeaderSize + (uint64_t)g_map.size() * kEntrySize);   // blobOff
    for (auto &kv : g_map) putEntry(header, kv.second);

    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o.is_open()) { std::fprintf(stderr, "[texcache] cannot write %s\n", tmp.c_str()); return false; }
        o.write((const char *)header.data(), (std::streamsize)header.size());
        o.write((const char *)g_blob.data(), (std::streamsize)g_blob.size());
        o.flush();
        if (!o.good()) { std::fprintf(stderr, "[texcache] write failed for %s\n", tmp.c_str()); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_path, ec);
    if (ec) { std::fprintf(stderr, "[texcache] rename failed: %s\n", ec.message().c_str()); return false; }
    std::fprintf(stderr, "[texcache] flushed %zu entries (%zu bytes), +%zu new\n",
                 g_map.size(), g_blob.size(), g_added);
    g_added = 0;
    return true;
}

bool flush()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return flushLocked();
}
}
