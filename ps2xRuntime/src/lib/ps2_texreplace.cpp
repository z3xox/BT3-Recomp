#include "runtime/ps2_texreplace.h"
#include "runtime/ps2_gs_psmt8.h"
#include "runtime/ps2_gs_psmt4.h"

#define XXH_INLINE_ALL
#include "thirdparty/xxhash.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include "raylib.h"

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
              uint8_t ta0, bool aem, uint8_t ta1, TexIdent &out)
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
    std::once_flag g_once;
    bool g_on = false;

    inline uint64_t pairKey(uint64_t a, uint64_t b)
    {
        return a ^ (b << 1) ^ (b >> 63);
    }

    void buildIndex()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        // Default to ./textures, CREATED IF ABSENT so it is discoverable without documentation --
        // a user should be able to see the folder, drop a pack in and flip the switch. Settings
        // (bt3_settings.ini) already resolve CWD-relative, so this sits beside them.
        // PS2X_TEXREPLACE overrides for anyone keeping packs elsewhere.
        const char *env = std::getenv("PS2X_TEXREPLACE");
        const std::string root = (env && env[0]) ? std::string(env) : std::string("textures");
        if (!(env && env[0]))
        {
            fs::create_directories(root, ec);   // harmless if it already exists
            ec.clear();
        }
        const char *dir = root.c_str();
        if (!fs::is_directory(dir, ec)) { std::fprintf(stderr, "[texreplace] not a directory: %s\n", dir); return; }
        size_t n = 0, skipped = 0;
        // RECURSIVE on purpose: PCSX2 searches replacements/ recursively, and real packs ship with
        // their own nested textures/<SERIAL>/replacements/ path inside the archive.
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            const std::string stem = it->path().stem().string();
            const std::string ext = it->path().extension().string();
            if (ext != ".png" && ext != ".dds" && ext != ".DDS") continue;
            // "<tex0hash>-<cluthash>-<bits>" or "<tex0hash>-<bits>" (no palette).
            unsigned long long a = 0, b = 0; unsigned bits = 0;
            const char *cs = stem.c_str();
            if (std::sscanf(cs, "%llx-%llx-%8x", &a, &b, &bits) == 3)
                g_index.emplace(pairKey(a, b), it->path().string());
            else if (std::sscanf(cs, "%llx-%8x", &a, &bits) == 2)
                g_index.emplace(pairKey(a, 0), it->path().string());
            else { ++skipped; continue; }
            ++n;
        }
        g_on = n > 0;
        if (n)
            std::fprintf(stderr, "[texreplace] indexed %zu replacements from %s (%zu unparsed)\n", n, dir, skipped);
        else
            std::fprintf(stderr, "[texreplace] no replacements in ./%s -- drop a PCSX2 texture pack "
                                 "in there (any nesting; the folder is searched recursively) and "
                                 "enable Texture Replacement in the overlay\n", dir);
    }
}

bool replacementsEnabled()
{
    std::call_once(g_once, buildIndex);
    return g_on;
}

bool loadReplacement(const TexIdent &id, std::vector<uint8_t> &rgba, int &w, int &h, int &fmt)
{
    if (!replacementsEnabled()) return false;
    auto it = g_index.find(pairKey(id.tex0Hash, id.hasClut ? id.clutHash : 0ull));
    if (it == g_index.end()) return false;

    // raylib's LoadImage is pure CPU (stb_image) -- safe off the GL thread, which matters because
    // decoding happens on the guest thread.
    Image img = LoadImage(it->second.c_str());
    if (img.data == nullptr || img.width <= 0 || img.height <= 0) { UnloadImage(img); return false; }

    // KEEP a compressed DDS compressed. ImageFormat() silently REFUSES to convert compressed
    // input (rtextures.c only converts when both formats are < PIXELFORMAT_COMPRESSED_DXT1_RGB),
    // so calling it on BC data would no-op and we would then copy compressed bytes as if they
    // were RGBA8 -- garbage textures with no error. Pass the format through instead and let
    // rlLoadTexture route it to glCompressedTexImage2D.
    const bool isCompressed = (img.format >= PIXELFORMAT_COMPRESSED_DXT1_RGB);
    if (!isCompressed) ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    w = img.width; h = img.height; fmt = img.format;
    const int bytes = GetPixelDataSize(w, h, img.format);
    if (bytes <= 0) { UnloadImage(img); return false; }
    rgba.assign((const uint8_t *)img.data, (const uint8_t *)img.data + (size_t)bytes);
    UnloadImage(img);
    return true;
}
}
