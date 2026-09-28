#ifndef PS2_GS_RASTERIZER_H
#define PS2_GS_RASTERIZER_H

#include <vector>
#include <cstddef>
#include <cstdint>
#include <cstdint>

class GS;
struct GSTex0Reg; struct GSTexaReg; struct GSTexClutReg; struct TexDecodeReq;   // [deferdec]
struct DecPoolJob;   // [decpool]
struct GSVertex; struct GSPrimReg; struct GSContext;   // [recinput]
// [deferdec] what decodeTexRGBA's fast paths read. In the synchronous case these point into the
// live GS; in the deferred case into a TexDecodeReq snapshot + a thread-local CLUT.
struct TexDecodeSrc
{
    const GSTex0Reg *tex0 = nullptr;
    uint64_t clamp = 0;
    uint8_t *vram = nullptr;
    size_t vramSize = 0;
    const uint32_t *clut = nullptr;
    uint64_t clutKey = ~0ull;
    const GSTexaReg *texa = nullptr;
    const GSTexClutReg *texclut = nullptr;
    int subDxW = 0, subDx0 = 0;
};

class GSRasterizer
{
public:
    // [pgs-texreplace] the paraLLEl-GS backend's replacement hook builds a palette from OUR VRAM for hashing
    static int fillClutForBackend(uint32_t *out, uint8_t *vram, const GSTexaReg &texa, const GSTexClutReg &texclut, const GSTex0Reg &tex0) { return fillClutFrom(out, vram, texa, texclut, tex0); }
    void decodeDeferred(const TexDecodeReq &req, uint8_t *vram, size_t vramSize, int &subW, std::vector<uint8_t> &rgba);   // [deferdec] GL thread
    void decodeSnapshot(const DecPoolJob &job, uint8_t *scratch, size_t vramSize, int &subW, std::vector<uint8_t> &rgba);   // [decpool] pool thread
    // [texreplace] the pack-replacement swap, shared by the inline decode and the decode pool: on a hit rgba/upW/upH/upFmt/upScale/upAlpha become the replacement's
    static void applyTexReplacement(const uint8_t *vram, const GSTex0Reg &tex0, const uint32_t *clut, uint64_t clutKey, const GSTexaReg &texa, uint64_t texKey, int subW, int texH, bool allowed,
                                    std::vector<uint8_t> &rgba, int &upW, int &upH, int &upFmt, int &upScale, float &upAlpha, float &upSnap);
    static bool decodeIsDeferrable(uint32_t psm);   // [deferdec] only the fast paths can run without the GS
    void drawPrimitive(GS *gs);
    // [recinput] everything recordSpriteGPU reads. Stage 1 of chunked recording: the wrapper below fills it from the
    // live GS (pointers into it, scalars by value); a later stage copies it into a snapshot a record thread can consume.
    // `gs` is the worker-only handle for the few callees that must see the live GS (palette-cache build, inline decode);
    // it is null on a record thread.
    struct RecOut;   // [recpool] a record thread's output (defined in the .cpp)
    struct RecInput
    {
        GS *gs = nullptr;
        GSVertex *vtx = nullptr;             // the primitive's vertex queue (mutable: [fadefull] rewrites alpha)
        const GSPrimReg *prim = nullptr;
        const GSContext *ctx = nullptr;
        const GSTexaReg *texa = nullptr;
        const GSTexClutReg *texclut = nullptr;
        const uint32_t *clut = nullptr;      // the built palette cache
        uint64_t clutKey = ~0ull, clutHash256 = 0, clutHash16 = 0;
        uint8_t *vram = nullptr; size_t vramSize = 0;
        uint32_t stateGen = 0, texUploadGen = 0;
        void refreshClut();                  // re-read the palette-cache scalars after ensureClutCache (worker only)
        // [recpool] Stage 3
        bool resolveOnly = false;            // worker: run pre + the resolve in stream order, fill `resolved`, return before the build
        bool preResolved = false;            // record thread: skip the resolve and take `resolved`
        struct Resolved { uint64_t texKey = 0; int texW = 1, texH = 1; bool tplHit = false; int subDxW = 0, subDx0 = 0; } resolved;
        bool batchOk = true;                 // worker-captured: !g_recordDepthOnly && g_recordAliasKind == 0
        RecOut *out = nullptr;               // record thread: DrawCmds go here; the worker merges them in order
    };
    bool recordSpriteGPU(RecInput &in);
    bool recordSpriteGPUSeq(GS *gs);   // [recpool] the stream-ordered path (Stage 1/2 wrapper), also the inline fallback
    void writePixel(GS *gs, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool widened = false);
    uint32_t sampleTexture(GS *gs, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t lookupCLUT(GS *gs, uint8_t index, uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

private:
    void drawSprite(GS *gs);
    void drawTriangle(GS *gs);
    void drawLine(GS *gs);
    // Rebuild the per-primitive CLUT cache if the palette state changed. Safe to
    // call before any sampleTexture() for the primitive; no-op for non-paletted.
    void ensureClutCache(GS *gs);
    void decodeTexRGBA(GS *gs, TexDecodeSrc src, int texW, int texH, bool rawAlphaDec, uint64_t texKey, int &subW, std::vector<uint8_t> &rgba);   // [decodefn]
    static uint32_t lookupCLUTFrom(uint8_t *vram, const GSTexaReg &texa, const GSTexClutReg &texclut, uint8_t index, uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);   // [deferdec]
    static int fillClutFrom(uint32_t *out, uint8_t *vram, const GSTexaReg &texa, const GSTexClutReg &texclut, const GSTex0Reg &tex0);   // [deferdec] entries filled (0 = not paletted)
    // GPU mode (PS2X_GPU): record a SPRITE as a GPU draw command (detexturing the
    // texture via sampleTexture into the renderer cache). Returns false if unsupported.
    bool recordSpriteGPU(GS *gs);
};

#endif
