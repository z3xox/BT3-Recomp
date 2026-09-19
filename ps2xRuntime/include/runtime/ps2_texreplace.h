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
    // cbp/csa/csm/cpsm are only used by the [texraw] diagnostic (CLUT layout provenance).
    bool identify(const uint8_t *vram, uint32_t tbp0, uint32_t tbw, uint8_t psm,
                  uint8_t tw, uint8_t th, const uint32_t *clut,
                  uint8_t ta0, bool aem, uint8_t ta1, TexIdent &out,
                  uint32_t cbp = 0, uint32_t csa = 0, uint32_t csm = 0, uint32_t cpsm = 0);

    // True once a replacement directory has been indexed (PS2X_TEXREPLACE=<dir>).
    bool replacementsEnabled();

    // [texraw] Diagnostic (PS2X_TEXRAWD=<name|*>, PS2X_TEXRAWD_DIR): dump the RESOLVED decoded RGBA
    // (the texture as handed to the runner) so the source container layout can be derived offline.
    void maybeDumpResolved(const TexIdent &id, const uint8_t *rgba, int w, int h);

    // [texui] Pack status for the launcher/overlay "Texture Replacement" popup.
    size_t replacementsCount();       // files actually indexed (0 = no pack)
    const char *replacementsRoot();   // indexed root directory ("" if none)
    bool replacementsHave3D();        // true = full pack (Characters/Body present); false = 2D-only Lite

    // [texrepdiag] A pack entry that has this TEX0 hash, whatever its CLUT: a lookup miss whose hash
    // pair differs ONLY in the palette comes back here. Returns the file stem (name without extension)
    // or nullptr. Used to tell "the pack does not have this texture" from "our CLUT differs".
    const char *findByTex0Hash(uint64_t tex0Hash);

    // [texrepdiag] Is there a pack entry for this exact hash PAIR (what the lookup keys on)? A miss
    // with hasPair=true is not "absent from the pack": it is queued for decode or failed to decode.
    bool hasPair(uint64_t tex0Hash, uint64_t clutHash);



    // Look up a replacement for `id` and decode it to RGBA8. Matches on the HASH PAIR only --
    // the bits field is advisory (a real pack was built by an older PCSX2 whose TEXA convention
    // differs, so an exact-name match would silently find nothing). Returns false if absent.
    // `fmt` returns the raylib PixelFormat: RGBA8 for PNG (and uncompressed DDS), or a
    // PIXELFORMAT_COMPRESSED_* for a BC-compressed DDS, which the GL upload hands to
    // glCompressedTexImage2D unchanged. Keeping it compressed is the whole point: a 4x PNG
    // replacement costs 16x the original's VRAM, where BC1 is 4:1 on top of that.
    bool loadReplacement(const TexIdent &id, std::vector<uint8_t> &rgba, int &w, int &h, int &fmt);

    // [texpackasync] Non-blocking variant for the record path. A replacement file is decoded on a
    // background worker; until it is ready this returns false (the caller uploads the original
    // texture as usual) and the file is queued once. When the worker finishes, `texKey` is marked
    // so the texture-cache HIT path can force one re-resolve (takeReadySwap), which then gets the
    // decoded blob from here. PS2X_TEXPACK_ASYNC=0 restores the synchronous load.
    bool loadReplacement(const TexIdent &id, uint64_t texKey, std::vector<uint8_t> &rgba, int &w, int &h, int &fmt);
    bool takeReadySwap(uint64_t texKey);

    // [texcache] True while the async replacement for `id` is queued/decoding (its first decode
    // uploads the ORIGINAL). The texcache must NOT store the result then, or it would bake the
    // original and the read hook would cancel the later swap re-decode.
    bool replacementPending(const TexIdent &id);

    // [texmega] One-shot diagnostic dump (enable PS2X_TEXMEGA=1, arm with F9). While armed it
    // writes to <dir>: texlookup.tsv (every lookup, HIT/MISS + full identity), the original
    // decode and the replacement as PNG pairs (<name>_orig / <name>_new), the whole pack index
    // (texpack_index.tsv) and a summary. Bounded, so a single capture is enough to see why a
    // texture is not replaced.
    void megaArm(const char *dir, double seconds);
    bool megaActive();
    void megaDumpIndex();
    void megaLookup(const TexIdent &id, uint64_t texKey, uint32_t tbp0, uint32_t tbw,
                    uint8_t psm, uint8_t tw, uint8_t th, bool hit,
                    const uint8_t *origRgba, int ow, int oh,
                    const uint8_t *repRgba, int rw, int rh, int rfmt);
}
#endif
