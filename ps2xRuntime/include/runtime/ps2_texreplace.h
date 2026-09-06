#ifndef PS2_TEXREPLACE_H
#define PS2_TEXREPLACE_H
#include <cstdint>
#include <string>
#include <vector>

// [texreplace] PCSX2-COMPATIBLE texture identity.
//
// Goal: name textures exactly as PCSX2 does so its existing replacement packs load unchanged.
// Format (GS/Renderers/HW/GSTextureReplacements.cpp):
//     "%" PRIx64 "-%" PRIx64 "-%08x"   ==  TEX0Hash-CLUTHash-bits      (no CLUT: drop field 2)
//     bits = PSM(6) | TW(4)<<6 | TH(4)<<10 | TEXA_TA0(8)<<14 | AEM(1)<<22 | TA1(8)<<23
// Hashes are XXH3-64. CLUTHash is over 16 or 256 u32 entries. TEX0Hash is over the RAW SWIZZLED
// VRAM in GS BLOCK ORDER, 256 bytes per block (GSTextureCache::HashTextureLevel fast path).
//
// ⚠ The hashes are the IDENTITY; the bits field is ADVISORY. Verified on this machine: PCSX2
// DUMPS this game with TA0=0 yet LOADS replacements named with TA0=1 -- 12 textures shared
// between a live dump session and a real pack had identical TEX0Hash+CLUTHash and bits differing
// by exactly 0x4000, and they render. So a loader must match on the hash pair and tolerate a
// differing third field, or a working pack silently loads nothing.
namespace ps2tex
{
    struct TexIdent
    {
        uint64_t tex0Hash = 0;
        uint64_t clutHash = 0;
        uint32_t bits = 0;
        bool hasClut = false;
        std::string name() const;      // the PCSX2 filename stem (no extension)
    };

    // vram: the 4 MB GS VRAM base. Returns false for formats we do not hash yet.
    bool identify(const uint8_t *vram, uint32_t tbp0, uint32_t tbw, uint8_t psm,
                  uint8_t tw, uint8_t th, const uint32_t *clut,
                  uint8_t ta0, bool aem, uint8_t ta1, TexIdent &out);

    // True once a replacement directory has been indexed (PS2X_TEXREPLACE=<dir>).
    bool replacementsEnabled();

    // Look up a replacement for `id` and decode it to RGBA8. Matches on the HASH PAIR only --
    // the bits field is advisory (a real pack was built by an older PCSX2 whose TEXA convention
    // differs, so an exact-name match would silently find nothing). Returns false if absent.
    bool loadReplacement(const TexIdent &id, std::vector<uint8_t> &rgba, int &w, int &h);
}
#endif
