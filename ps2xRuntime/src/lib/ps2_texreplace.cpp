#include "runtime/ps2_texreplace.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_gs_psmt4.h"

#define XXH_INLINE_ALL
#include "thirdparty/xxhash.h"
#include "gfx/image_io.h"

#include "runtime/ps2_toml.h"   // [texui] button_layout from savedata/settings.toml

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <string>
#include <vector>
#include <chrono>
#include <map>
#include <algorithm>
#include <array>
#include <cstring>
#include "gfx/bt3gl_api.h"   // [B] bt3* API bridge
extern "C" const char *ps2xExeDirC();   // [mergefix] main.cpp

namespace ps2tex
{
namespace
{
    constexpr uint32_t kBlockBytes = 256;          // GS_BLOCK_SIZE
    constexpr uint32_t kVramMask   = 0x3FFFFFu;    // 4 MB

    // GS PSM codes we handle. BT3's texture set is overwhelmingly PSMT8 (16,858 of 17,398 in a
    // PCSX2 dump of this game) with PSMT4 second (527); everything else is rare or is a render
    // target, which must NOT be replaceable anyway.
    enum : uint8_t { PSM_T8 = 19, PSM_T4 = 20 };

    // Byte address of the 256-byte block containing texel (bx*bw, by*bh), derived from the same
    // arithmetic as addrPSMT8/addrPSMT4 with the intra-block term dropped. Keeping it in terms of
    // those functions is deliberate: if the swizzle is ever corrected, this follows automatically.
    inline uint32_t blockAddr8(uint32_t tbp0, uint32_t tbw, uint32_t bx, uint32_t by)
    {
        const uint32_t pagesPerRow = (tbw >> 1u) ? (tbw >> 1u) : 1u;
        const uint32_t page    = (tbp0 >> 5u) + (by >> 2u) * pagesPerRow + (bx >> 3u);
        const uint32_t blockId = (tbp0 & 0x1Fu) + GSPSMT8::blockTable8[by & 3u][bx & 7u];
        return (page << 13u) + ((blockId >> 5u) << 13u) + (blockId & 0x1Fu) * kBlockBytes;
    }
    // PSMT4 differs from T8 in three ways and all three matter: its block is 32x16 texels, its
    // addresses are in NIBBLES (page shift 14, localBlock*512), and its block table is [8][4]
    // indexed [(y>>4)&7][(x>>5)&3]. Mirrors addrPSMT4 with the intra-block term dropped, then
    // converts nibbles -> bytes. 512 nibbles == the same 256-byte block PCSX2 hashes.
    inline uint32_t blockAddr4(uint32_t tbp0, uint32_t tbw, uint32_t bx, uint32_t by)
    {
        const uint32_t pagesPerRow = (tbw >> 1u) ? (tbw >> 1u) : 1u;
        const uint32_t page    = (tbp0 >> 5u) + (by >> 3u) * pagesPerRow + (bx >> 2u);
        const uint32_t blockId = (tbp0 & 0x1Fu) + GSPSMT4::blockTable4[by & 7u][bx & 3u];
        const uint32_t nib = (page << 14u) + ((blockId >> 5u) << 14u) + (blockId & 0x1Fu) * 512u;
        return nib >> 1u;   // nibble address -> byte address
    }
}

std::string TexIdent::name() const
{
    char buf[64];
    if (hasClut)
        std::snprintf(buf, sizeof buf, "%llx-%llx-%08x",
                      (unsigned long long)tex0Hash, (unsigned long long)clutHash, bits);
    else
        std::snprintf(buf, sizeof buf, "%llx-%08x", (unsigned long long)tex0Hash, bits);
    return std::string(buf);
}

bool identify(const uint8_t *vram, uint32_t tbp0, uint32_t tbw, uint8_t psm,
              uint8_t tw, uint8_t th, const uint32_t *clut,
              uint8_t ta0, bool aem, uint8_t ta1, TexIdent &out,
              uint32_t cbp, uint32_t csa, uint32_t csm, uint32_t cpsm)
{
    if (!vram) return false;
    // Block dimensions in TEXELS: PSMT8 is 16x16, PSMT4 is 32x16.
    uint32_t bw, bh;
    if (psm == PSM_T8)      { bw = 16; bh = 16; }
    else if (psm == PSM_T4) { bw = 32; bh = 16; }
    else return false;                       // other formats: not yet, and RTs must stay excluded

    const uint32_t texW = 1u << tw, texH = 1u << th;
    // PCSX2 hashes the BLOCK-ALIGNED rect (ralign<Align_Outside>), and takes the fast path only
    // when the texture is at least one block and the format covers all bits -- true for T8/T4 here.
    const uint32_t bx1 = (texW + bw - 1) / bw, by1 = (texH + bh - 1) / bh;

    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    for (uint32_t by = 0; by < by1; ++by)
        for (uint32_t bx = 0; bx < bx1; ++bx)
        {
            const uint32_t a = (psm == PSM_T8 ? blockAddr8(tbp0, tbw, bx, by)
                                              : blockAddr4(tbp0, tbw, bx, by)) & kVramMask;
            if (a + kBlockBytes > 0x400000u) return false;   // never read outside VRAM
            XXH3_64bits_update(&st, vram + a, kBlockBytes);
        }
    out.tex0Hash = XXH3_64bits_digest(&st);

    out.hasClut = (clut != nullptr);
    if (out.hasClut)
        out.clutHash = XXH3_64bits(clut, sizeof(uint32_t) * (psm == PSM_T4 ? 16u : 256u));

    // TEXA is EXCLUDED for paletted formats, matching current PCSX2: TEXA only expands alpha for
    // 16/24-bit formats, so for T4/T8 (alpha comes from the CLUT) it cannot affect the output and
    // PCSX2 zeroes it in the key. Verified empirically: across 200 textures where our TEX0Hash AND
    // CLUTHash both matched a real PCSX2 dump, PSM/TW/TH agreed every time and the ONLY difference
    // was these bits -- we emitted TA1=128 (0x40000000), PCSX2 emitted 0.
    // NOTE the shipped upscale pack was built by an OLDER PCSX2 that DID include TEXA (its names
    // carry TA0=1), which is why a loader must treat the third field as advisory and match on the
    // hash pair -- see the header. This only makes our DUMPS byte-identical to current PCSX2.
    const bool paletted = (psm == PSM_T8 || psm == PSM_T4);
    const uint8_t eTa0 = paletted ? 0u : ta0;
    const bool    eAem = paletted ? false : aem;
    const uint8_t eTa1 = paletted ? 0u : ta1;
    out.bits = (uint32_t)(psm & 0x3F) | ((uint32_t)(tw & 0xF) << 6) | ((uint32_t)(th & 0xF) << 10)
             | ((uint32_t)eTa0 << 14) | ((uint32_t)(eAem ? 1u : 0u) << 22) | ((uint32_t)eTa1 << 23);

    // [texraw] Diagnostic: PS2X_TEXRAWD=<name|tex0Hash-prefix|*>: dump the exact bytes the hash reads
    // (VRAM blocks in hash order + the CLUT) so the source container layout can be derived offline.
    // Bounded by PS2X_TEXRAWD_MAX (default 4000). One file per identity in PS2X_TEXRAWD_DIR (or ".").
    if (const char *want = std::getenv("PS2X_TEXRAWD"); want && want[0])
    {
        const std::string nm = out.name();
        const bool all = (want[0] == '*' && want[1] == '\0');
        if (all || nm.rfind(want, 0) == 0)
        {
            static std::mutex s_mx;
            static std::unordered_set<std::string> s_done;
            bool first = false;
            {
                std::lock_guard<std::mutex> lk(s_mx);
                static size_t s_max = []() {
                    const char *v = std::getenv("PS2X_TEXRAWD_MAX");
                    return (size_t)((v && v[0]) ? std::atol(v) : 4000L);
                }();
                if (s_done.size() < s_max) first = s_done.insert(nm).second;
            }
            if (first)
            {
                const char *dir = std::getenv("PS2X_TEXRAWD_DIR");
                const std::string path = std::string((dir && dir[0]) ? dir : ".") + "/texraw_" + nm + ".bin";
                if (std::FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    const int nclut = (psm == PSM_T4) ? 16 : 256;
                    std::fprintf(f, "tbp0 %u tbw %u psm %u tw %u th %u clutN %d bits %08x tex0 %016llx clut %016llx cbp %u csa %u csm %u cpsm %u\n",
                                 tbp0, tbw, psm, tw, th, nclut, out.bits,
                                 (unsigned long long)out.tex0Hash, (unsigned long long)out.clutHash,
                                 cbp, csa, csm, cpsm);
                    for (uint32_t by = 0; by < by1; ++by)
                        for (uint32_t bx = 0; bx < bx1; ++bx)
                        {
                            const uint32_t a = (psm == PSM_T8 ? blockAddr8(tbp0, tbw, bx, by)
                                                              : blockAddr4(tbp0, tbw, bx, by)) & kVramMask;
                            std::fwrite(vram + a, 1, kBlockBytes, f);
                        }
                    if (clut) std::fwrite(clut, sizeof(uint32_t), (size_t)nclut, f);
                    std::fclose(f);
                }
            }
        }
    }
    return true;
}
}

namespace ps2tex
{
namespace
{
    // Index of available replacements, keyed by the HASH PAIR ONLY.
    //
    // The third filename field is deliberately NOT part of the key. Verified on a real pack for
    // this game: PCSX2 dumps it with TEXA zeroed but the pack was built by an older PCSX2 that
    // included TEXA, so the same texture appears as ...-00001dd3 in a fresh dump and ...-00005dd3
    // in the pack -- identical hashes, bits differing by 0x4000. Keying on the full name would
    // load NOTHING from a working pack and look like the feature is broken.
    std::unordered_map<uint64_t, std::string> g_index;   // (tex0Hash ^ rotl(clutHash)) -> path
    // [texrepdiag] tex0Hash -> a pack entry's stem, for "same art, different palette" misses.
    std::unordered_map<uint64_t, std::string> g_byTex0;
    std::once_flag g_once;
    bool g_on = false;
    std::string g_root;   // [texui] the indexed pack root, for the launcher/overlay status

    inline uint64_t pairKey(uint64_t a, uint64_t b)
    {
        return a ^ (b << 1) ^ (b >> 63);
    }

    // [texui] Button layout preference: 0 = PS2 (Original Buttons), 1 = Xbox (Xbox Layout). Read from
    // savedata/settings.toml [video] button_layout (default Xbox, matching the packs' replacements/
    // Buttons). PS2X_BUTTONS=ps2|xbox overrides for A/B.
    int packButtonLayout()
    {
        if (const char *v = std::getenv("PS2X_BUTTONS"); v && v[0])
            return (v[0] == '0' || v[0] == 'p' || v[0] == 'P') ? 0 : 1;
        const char *xd = ps2xExeDirC();
        std::ifstream f(std::string((xd && xd[0]) ? xd : ".") + "/savedata/settings.toml");
        if (!f.is_open()) return 1;
        ps2x_toml::Document doc;
        doc.parse(f);
        return doc.getI("video.button_layout", 1);
    }

    void buildIndex()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        // [texloc] The pack lives where the game data was installed: <exeDir>/data/Textures
        // (the deploy's data/ dir, next to the extracted ISO tree). Created if absent so it is
        // discoverable without documentation. PS2X_TEXREPLACE overrides for packs kept elsewhere.
        // NOTE: this runs at startup BEFORE loadELF, so anchor on the executable dir (ps2xExeDirC
        // honors PS2X_EXEDIR, i.e. the deploy root), not on the boot ELF's directory.
        const char *env = std::getenv("PS2X_TEXREPLACE");
        std::string root;
        if (env && env[0])
        {
            root = env;
        }
        else
        {
            const char *xd = ps2xExeDirC();
            // .string() is REQUIRED for Windows: fs::path::string_type is std::string on POSIX
            // but std::wstring on Windows, so the implicit conversion yields a wstring and there
            // is no matching operator= for std::string. Linux/clang accepts this silently, so
            // this class of break can only be caught by an actual Windows build.
            root = (((xd && xd[0]) ? fs::path(xd) : fs::path(".")) / "data" / "Textures").string();
            fs::create_directories(root, ec);   // harmless if it already exists
            ec.clear();
        }
        g_root = root;   // [texui] expose the indexed root for status reporting
        const char *dir = root.c_str();
        if (!fs::is_directory(dir, ec)) { std::fprintf(stderr, "[texreplace] not a directory: %s\n", dir); return; }
        size_t n = 0, skipped = 0;
        std::unordered_set<std::string> seenPaths;   // the preferred pass may overlap the full scan
        auto addFile = [&](const fs::path &path)
        {
            if (!seenPaths.insert(path.string()).second) return;
            const std::string stem = path.stem().string();
            const std::string ext = path.extension().string();
            if (ext != ".png" && ext != ".dds" && ext != ".DDS") return;
            // "<tex0hash>-<cluthash>-<bits>" or "<tex0hash>-<bits>" (no palette).
            unsigned long long a = 0, b = 0; unsigned bits = 0;
            const char *cs = stem.c_str();
            if (std::sscanf(cs, "%llx-%llx-%8x", &a, &b, &bits) == 3)
            {
                g_index.emplace(pairKey(a, b), path.string());
                g_byTex0.emplace(a, stem);   // [texrepdiag]
            }
            else if (std::sscanf(cs, "%llx-%8x", &a, &bits) == 2)
            {
                g_index.emplace(pairKey(a, 0), path.string());
                g_byTex0.emplace(a, stem);   // [texrepdiag]
            }
            else { ++skipped; return; }
            ++n;
        };
        // RECURSIVE on purpose: PCSX2 searches replacements/ recursively, and real packs ship with
        // their own nested textures/<SERIAL>/replacements/ path inside the archive.
        auto scan = [&](const std::string &base)
        {
            std::error_code e2;
            for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, e2), end;
                 it != end && !e2; it.increment(e2))
                if (it->is_regular_file(e2)) addFile(it->path());
        };
        // [texui] Button layout: scan the chosen variant folder FIRST so its identically-keyed
        // entries win over the other variant (g_index.emplace keeps the first). Both variants live
        // in the pack side by side: "Original Buttons" (PS2) and "Xbox Layout" (Xbox).
        {
            const std::string pref = (packButtonLayout() == 0) ? "/Original Buttons" : "/Xbox Layout";
            std::error_code e3;
            if (fs::is_directory(root + pref, e3)) scan(root + pref);
        }
        scan(root);
        g_on = n > 0;
        if (n)
            std::fprintf(stderr, "[texreplace] indexed %zu replacements from %s (%zu unparsed)\n", n, dir, skipped);
        else
            std::fprintf(stderr, "[texreplace] no replacements in %s -- drop a PCSX2 texture pack "
                                 "in there (any nesting; the folder is searched recursively) and "
                                 "enable Texture Replacement in the overlay\n", dir);
    }
}

bool replacementsEnabled()
{
    std::call_once(g_once, buildIndex);
    return g_on;
}

// [texraw] Dump the resolved decoded RGBA (the texture as handed to the runner) for the identity
// requested by PS2X_TEXRAWD (exact name prefix or "*"). Bounded to one file per identity.
void maybeDumpResolved(const TexIdent &id, const uint8_t *rgba, int w, int h)
{
    const char *want = std::getenv("PS2X_TEXRAWD");
    if (!rgba || !want || !want[0] || w <= 0 || h <= 0) return;
    const std::string nm = id.name();
    const bool all = (want[0] == '*' && want[1] == '\0');
    if (!all && nm.rfind(want, 0) != 0) return;
    static std::mutex s_mx;
    static std::unordered_set<std::string> s_done;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (!s_done.insert(nm).second) return;
    }
    const char *dir = std::getenv("PS2X_TEXRAWD_DIR");
    const std::string base = std::string((dir && dir[0]) ? dir : ".") + "/texrgba_" + nm;
    if (std::FILE *f = std::fopen((base + ".bin").c_str(), "wb"))
    {
        std::fprintf(f, "w %d h %d\n", w, h);
        std::fwrite(rgba, 1, (size_t)w * (size_t)h * 4u, f);
        std::fclose(f);
    }
    bt3Image img(const_cast<void *>(static_cast<const void *>(rgba)), w, h, 1,
                 BT3_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    bt3ExportImage(img, (base + ".png").c_str());
}

// [texui] Pack status for the launcher/overlay popup.
size_t replacementsCount()
{
    std::call_once(g_once, buildIndex);
    return g_index.size();
}

const char *replacementsRoot()
{
    std::call_once(g_once, buildIndex);
    return g_root.c_str();
}

bool replacementsHave3D()
{
    std::call_once(g_once, buildIndex);
    if (g_root.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(g_root + "/replacements/Characters/Body", ec);
}

const char *findByTex0Hash(uint64_t tex0Hash)
{
    std::call_once(g_once, buildIndex);
    auto it = g_byTex0.find(tex0Hash);
    return it == g_byTex0.end() ? nullptr : it->second.c_str();
}

bool hasPair(uint64_t tex0Hash, uint64_t clutHash)
{
    std::call_once(g_once, buildIndex);
    return g_index.find(pairKey(tex0Hash, clutHash)) != g_index.end();
}

namespace
{
    // Decode one replacement file to an upload-ready blob. raylib's bt3LoadImage is pure CPU
    // (stb_image) -- safe on any thread, which is what the async worker relies on.
    bool decodeFile(const std::string &path, std::vector<uint8_t> &rgba, int &w, int &h, int &fmt)
    {
        // [A4.2] PNG/JPG/BMP/... decode with our own stb loader (thread-safe, no raylib state).
        // DDS stays on raylib on purpose: raylib keeps BC data COMPRESSED (glCompressedTexImage2D),
        // and stb cannot hand us compressed bytes -- decompressing the pack's 18,700 DXT5 files to
        // RGBA8 would multiply its VRAM footprint. Fall back to raylib for anything stb cannot read.
        const bool isDds = path.size() >= 4 &&
            (path.compare(path.size() - 4, 4, ".dds") == 0 || path.compare(path.size() - 4, 4, ".DDS") == 0);
        if (!isDds)
        {
            int lw = 0, lh = 0;
            if (ps2x::gfx::GsDecodeImageRGBA8(path.c_str(), rgba, lw, lh) && lw > 0 && lh > 0)
            { w = lw; h = lh; fmt = BT3_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8; return true; }
        }
        bt3Image img = bt3LoadImage(path.c_str());
        if (img.data == nullptr || img.width <= 0 || img.height <= 0) { bt3UnloadImage(img); return false; }

        // KEEP a compressed DDS compressed. bt3ImageFormat() silently REFUSES to convert compressed
        // input (rtextures.c only converts when both formats are < BT3_PIXELFORMAT_COMPRESSED_DXT1_RGB),
        // so calling it on BC data would no-op and we would then copy compressed bytes as if they
        // were RGBA8 -- garbage textures with no error. Pass the format through instead and let
        // rlLoadTexture route it to glCompressedTexImage2D.
        const bool isCompressed = (img.format >= BT3_PIXELFORMAT_COMPRESSED_DXT1_RGB);
        if (!isCompressed) bt3ImageFormat(&img, BT3_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        w = img.width; h = img.height; fmt = img.format;
        const int bytes = bt3GetPixelDataSize(w, h, img.format);
        if (bytes <= 0) { bt3UnloadImage(img); return false; }
        rgba.assign((const uint8_t *)img.data, (const uint8_t *)img.data + (size_t)bytes);
        bt3UnloadImage(img);
        return true;
    }

    // [texpackasync] Background decode. A 5600G+3050 Windows log (2026-09-07) spent a 90-second
    // stretch at ~20 fps with 73 replacement PNG loads per second decoded INLINE on the record
    // thread; with the pack indexed the loads are the whole difference between its good and bad
    // seconds. The record path must never wait on a file: queue it, draw the original, swap the
    // replacement in when the worker is done.
    struct Blob { std::vector<uint8_t> rgba; int w = 0, h = 0, fmt = 0; };
    struct Job { uint64_t key; std::string path; uint64_t texKey; };
    struct AsyncState
    {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<Job> queue;
        std::unordered_map<uint64_t, Blob> ready;        // pairKey -> decoded, not yet consumed
        std::deque<uint64_t> readyOrder;                  // FIFO for the byte cap
        size_t readyBytes = 0;
        std::unordered_set<uint64_t> pending;             // pairKey queued or decoding
        std::unordered_set<uint64_t> failed;              // pairKey that would not decode: never retry
        std::unordered_set<uint64_t> swap;                // texKeys whose replacement is ready
        std::vector<std::thread> workers;
        unsigned long decoded = 0, dropped = 0, consumed = 0;
    };
    // Heap-allocated and never destroyed on purpose: the workers are detached and may still be
    // decoding when static destructors run at exit.
    AsyncState *g_async = nullptr;
    std::once_flag g_asyncOnce;

    bool asyncEnabled()
    {
        static const bool s = [](){ const char *v = std::getenv("PS2X_TEXPACK_ASYNC"); return !(v && v[0] == '0'); }();
        return s;
    }

    void workerLoop(AsyncState *st, int idx)
    {
        (void)idx;
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lk(st->mtx);
                st->cv.wait(lk, [&]{ return !st->queue.empty(); });
                job = std::move(st->queue.front()); st->queue.pop_front();
            }
            Blob b;
            const bool ok = decodeFile(job.path, b.rgba, b.w, b.h, b.fmt);
            std::lock_guard<std::mutex> lk(st->mtx);
            st->pending.erase(job.key);
            if (!ok)
            {
                // [texrepdiag] A pack entry that EXISTS but will not decode: the texture keeps drawing
                // the original even though the pack has art for it.
                static const bool s_df = [](){ const char *v = std::getenv("PS2X_TEXREPDIAG"); return !(v && v[0] == '0'); }();
                if (s_df)
                {
                    static int s_n = 0;
                    if (s_n++ < 40)
                        std::fprintf(stderr, "[texrepdiag] DECODEFAIL %s\n", job.path.c_str());
                }
                st->failed.insert(job.key);
                continue;
            }
            // Byte cap: a texture that is never resolved again would otherwise pin its blob
            // forever. Oldest-first eviction; 384 MB default, PS2X_TEXPACK_CACHE_MB overrides.
            static const size_t s_capBytes = [](){ const char *v = std::getenv("PS2X_TEXPACK_CACHE_MB");
                                                    return (size_t)((v && v[0]) ? std::atol(v) : 384L) * 1024u * 1024u; }();
            st->readyBytes += b.rgba.size();
            st->readyOrder.push_back(job.key);
            st->ready[job.key] = std::move(b);
            ++st->decoded;
            while (st->readyBytes > s_capBytes && !st->readyOrder.empty())
            {
                const uint64_t k = st->readyOrder.front(); st->readyOrder.pop_front();
                auto it = st->ready.find(k);
                if (it == st->ready.end()) continue;
                st->readyBytes -= it->second.rgba.size();
                st->ready.erase(it);
                ++st->dropped;
            }
            st->swap.insert(job.texKey);
        }
    }

    void startAsync()
    {
        g_async = new AsyncState();
        const char *v = std::getenv("PS2X_TEXPACK_THREADS");
        int n = (v && v[0]) ? std::atoi(v) : 2;
        if (n < 1) n = 1;
        if (n > 8) n = 8;
        for (int i = 0; i < n; ++i) { std::thread t(workerLoop, g_async, i); t.detach(); }
        std::fprintf(stderr, "[texpackasync] replacement files decode on %d background thread(s); "
                             "originals draw until each is ready (PS2X_TEXPACK_ASYNC=0 = inline loads)\n", n);
    }
}

bool loadReplacement(const TexIdent &id, std::vector<uint8_t> &rgba, int &w, int &h, int &fmt)
{
    if (!replacementsEnabled()) return false;
    auto it = g_index.find(pairKey(id.tex0Hash, id.hasClut ? id.clutHash : 0ull));
    if (it == g_index.end()) return false;
    return decodeFile(it->second, rgba, w, h, fmt);
}

bool loadReplacement(const TexIdent &id, uint64_t texKey, std::vector<uint8_t> &rgba, int &w, int &h, int &fmt)
{
    if (!replacementsEnabled()) return false;
    if (!asyncEnabled()) return loadReplacement(id, rgba, w, h, fmt);
    const uint64_t key = pairKey(id.tex0Hash, id.hasClut ? id.clutHash : 0ull);
    auto it = g_index.find(key);
    if (it == g_index.end()) return false;
    std::call_once(g_asyncOnce, startAsync);
    AsyncState *st = g_async;
    std::lock_guard<std::mutex> lk(st->mtx);
    auto rd = st->ready.find(key);
    if (rd != st->ready.end())
    {   // [texreplace-persist] COPY the blob out instead of consuming it. The old one-shot
        // move+erase made every re-decode of the same texture MISS: the original drew again
        // until the worker re-decoded it, so the 2D UI visibly cycled original -> new -> original
        // every few seconds. Keep the decoded blob cached (bounded by PS2X_TEXPACK_CACHE_MB,
        // oldest-first eviction) so any re-decode is a stable HIT. PS2X_TEXPACK_ONESHOT=1
        // restores the old consume-on-use behaviour.
        static const bool s_oneShot = [](){ const char *v = std::getenv("PS2X_TEXPACK_ONESHOT"); return v && v[0] && v[0] != '0'; }();
        Blob &b = rd->second;
        rgba = b.rgba; w = b.w; h = b.h; fmt = b.fmt;
        st->swap.erase(texKey);
        ++st->consumed;
        if (s_oneShot) { st->readyBytes -= rgba.size(); st->ready.erase(rd); }
        {   static unsigned long s_n = 0;
            if (++s_n <= 3 || (s_n % 500) == 0)
                std::fprintf(stderr, "[texpackasync] swapped in #%lu (decoded %lu, dropped %lu, pending %zu)%s\n",
                             s_n, st->decoded, st->dropped, st->pending.size(), s_oneShot ? "" : " [persistent]"); }
        return true;
    }
    if (st->failed.count(key) || st->pending.count(key)) return false;
    st->pending.insert(key);
    st->queue.push_back(Job{key, it->second, texKey});
    st->cv.notify_one();
    return false;
}

bool takeReadySwap(uint64_t texKey)
{
    if (!g_async) return false;
    std::lock_guard<std::mutex> lk(g_async->mtx);
    auto it = g_async->swap.find(texKey);
    if (it == g_async->swap.end()) return false;
    g_async->swap.erase(it);
    return true;
}

bool replacementPending(const TexIdent &id)
{
    if (!replacementsEnabled() || !g_async) return false;
    const uint64_t key = pairKey(id.tex0Hash, id.hasClut ? id.clutHash : 0ull);
    std::lock_guard<std::mutex> lk(g_async->mtx);
    return g_async->pending.count(key) != 0;
}

// ---------------------------------------------------------------- [texmega]
namespace
{
    std::string s_megaDir;
    FILE *s_megaLookup = nullptr;
    std::mutex s_megaMx;
    std::atomic<long long> s_megaUntilMs{0};
    std::unordered_set<uint64_t> s_megaPngDone;
    unsigned long s_megaN = 0, s_megaHit = 0, s_megaMiss = 0, s_megaPng = 0;
    std::map<uint32_t, unsigned long> s_megaMissByPsm;
    std::map<std::string, unsigned long> s_megaMissByName;
    bool s_megaFinalized = true;

    long long megaNowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // Write an RGBA8 buffer as a PNG next to the dump. Bounded; best-effort.
    void megaPng(const std::string &name, const uint8_t *rgba, int w, int h)
    {
        if (!rgba || w <= 0 || h <= 0 || s_megaPng >= 800u) return;
        bt3Image img(static_cast<void *>(const_cast<uint8_t *>(rgba)), w, h, 1, BT3_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        const std::string p = s_megaDir + "/" + name + ".png";
        if (bt3ExportImage(img, p.c_str())) ++s_megaPng;
    }

    void megaFinalizeLocked()
    {
        if (s_megaLookup)
        {
            FILE *f = std::fopen((s_megaDir + "/summary.txt").c_str(), "w");
            if (f)
            {
                std::fprintf(f, "[texmega] lookups=%lu hit=%lu miss=%lu pngs=%lu\n",
                             s_megaN, s_megaHit, s_megaMiss, s_megaPng);
                std::fprintf(f, "miss by psm:\n");
                for (auto &kv : s_megaMissByPsm) std::fprintf(f, "  psm=%u : %lu\n", kv.first, kv.second);
                std::fprintf(f, "top misses:\n");
                std::vector<std::pair<unsigned long, std::string>> v;
                for (auto &kv : s_megaMissByName) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                for (size_t i = 0; i < v.size() && i < 60; ++i)
                    std::fprintf(f, "  %lu  %s\n", v[i].first, v[i].second.c_str());
                std::fclose(f);
            }
            std::fclose(s_megaLookup);
            s_megaLookup = nullptr;
        }
        s_megaFinalized = true;
    }

    // Called under s_megaMx at entry.
    bool megaEnsureOpen()
    {
        if (!s_megaFinalized && megaNowMs() > s_megaUntilMs.load(std::memory_order_relaxed))
            megaFinalizeLocked();
        return s_megaLookup != nullptr;
    }
}

void megaArm(const char *dir, double seconds)
{
    std::lock_guard<std::mutex> lk(s_megaMx);
    if (s_megaLookup) megaFinalizeLocked();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    s_megaDir = dir ? dir : ".";
    s_megaLookup = std::fopen((s_megaDir + "/texlookup.tsv").c_str(), "w");
    if (s_megaLookup)
        std::fprintf(s_megaLookup, "result\ttex0Hash\tclutHash\tbits\tname\ttexKey\ttbp0\ttbw\tpsm\ttw\tth\thorig\trep_fmt\trep_wh\n");
    s_megaPngDone.clear();
    s_megaN = s_megaHit = s_megaMiss = s_megaPng = 0;
    s_megaMissByPsm.clear(); s_megaMissByName.clear();
    s_megaUntilMs.store(megaNowMs() + (long long)(seconds * 1000.0), std::memory_order_relaxed);
    s_megaFinalized = false;
}

bool megaActive()
{
    std::lock_guard<std::mutex> lk(s_megaMx);
    return megaEnsureOpen();
}

void megaDumpIndex()
{
    std::call_once(g_once, buildIndex);
    std::lock_guard<std::mutex> lk(s_megaMx);
    if (s_megaDir.empty()) return;
    FILE *f = std::fopen((s_megaDir + "/texpack_index.tsv").c_str(), "w");
    if (!f) return;
    std::fprintf(f, "pairKey\tpath\n");
    for (auto &kv : g_index) std::fprintf(f, "%016llx\t%s\n", (unsigned long long)kv.first, kv.second.c_str());
    std::fclose(f);
}

void megaLookup(const TexIdent &id, uint64_t texKey, uint32_t tbp0, uint32_t tbw,
                uint8_t psm, uint8_t tw, uint8_t th, bool hit,
                const uint8_t *origRgba, int ow, int oh,
                const uint8_t *repRgba, int rw, int rh, int rfmt)
{
    std::lock_guard<std::mutex> lk(s_megaMx);
    if (!megaEnsureOpen()) return;
    const std::string nm = id.name();
    std::fprintf(s_megaLookup, "%s\t%016llx\t%016llx\t%08x\t%s\t%llx\t%u\t%u\t%u\t%u\t%u\t%dx%d\t%d\t%dx%d\n",
                 hit ? "HIT" : "MISS", (unsigned long long)id.tex0Hash, (unsigned long long)id.clutHash,
                 id.bits, nm.c_str(), (unsigned long long)texKey, tbp0, tbw, psm, tw, th, ow, oh, rfmt, rw, rh);
    ++s_megaN;
    if (hit) ++s_megaHit; else { ++s_megaMiss; ++s_megaMissByPsm[psm]; ++s_megaMissByName[nm]; }
    // Dump the ORIGINAL decode and, when present, the replacement -- one PNG pair per identity.
    const uint64_t dn = id.tex0Hash ^ (id.clutHash << 1);
    if (s_megaPngDone.insert(dn).second)
    {
        megaPng(nm + "_orig", origRgba, ow, oh);
        if (repRgba && rfmt == 0) megaPng(nm + "_new", repRgba, rw, rh);
    }
}
}
