#ifndef PS2_GS_GPU_RENDERER_H
#define PS2_GS_GPU_RENDERER_H

// Optional hardware (OpenGL via raylib) renderer for the GS, enabled with PS2X_GPU=1.
// The software rasterizer stays the default + correctness reference.
//
// Threading: GS primitives arrive on the GIF/DMA thread, but GL is single-threaded
// (present thread). So the rasterizer RECORDS compact draw commands here (no GL); the
// present thread REPLAYS them into an FBO and presents it. Textures are detextured
// (swizzle->linear + CLUT expand) by the rasterizer (which owns the sampling code)
// into CPU RGBA buffers handed to putTexture(); the present thread uploads them to GL
// lazily.

#include <cstdint>
#include <memory>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

struct TexDecodeReq;   // [deferdec]
struct LinImg;   // [linvram] ps2_gs_gpu_renderer.cpp
class GsGpuRenderer
{
public:
    static bool enabled(); // PS2X_GPU=1 (cached)
    // [uitoggles] Live-toggleable knobs for the settings overlay. Getters lazy-init from
    // the env on first call (which is after main()'s baked-defaults block), so PS2X_*
    // defaults and overrides keep working; setters flip them at runtime.
    static void setEnabled(bool v);
    static bool glowEnabled();      static void setGlow(bool v);
    static bool postfxEnabled();    static void setPostfx(bool v);
    static bool glowFixEnabled();   static void setGlowFix(bool v);
    // [inkstrength] cel-outline darkener strength, in percent of Cs (100 = the old
    // half-strength line, 199 = the hardware-exact 255/128 -- see PS2X_ADGS).
    static int  inkStrengthPct();   static void setInkStrengthPct(int pct);
    static bool bilinearEnabled();  static void setBilinear(bool v);
    static bool halfTexelEnabled(); static void setHalfTexel(bool v);
    static bool skipPostEnabled();  static void setSkipPost(bool v);
    static bool skipStaleVramEnabled(); static void setSkipStaleVram(bool v);
    // Stored/persisted only for the overlay's config; the scaling machinery itself is
    // NOT ported (its per-draw copy cost regressed the fight loop) -- always renders 1x.
    static int renderScale();       static void setRenderScale(int s);
    // Cel outline (ink rim + darkener) and character shadow decals, live-toggleable.
    static bool outlineEnabled();   static void setOutline(bool v);
    static bool shadowsEnabled();   static void setShadows(bool v);
    // Depth-of-field blur (the graded far-field mask stamp) + its reach (rawZ at which
    // blur weight reaches 0; larger = blur starts further away). Live-toggleable.
    static bool dofBlurEnabled();   static void setDofBlur(bool v);
    static int  dofZFar();          static void setDofZFar(int z);

    // A draw command is either an axis-aligned SPRITE quad (rendered with the proven
    // DrawTexturePro path) or a TRIANGLE (rendered with rlgl). One ordered list keeps
    // blend order correct. texKey 0 = untextured (flat).
    struct Vtx
    {
        float x, y;        // screen space (after XYOFFSET)
        float u, v;        // normalized UV (triangles)
        uint8_t r, g, b, a;
        // Normalized GL window depth in [0,1] (larger = NEARER, matching GS Z where a
        // larger integer is nearer). Only populated when PS2X_GPU_DEPTH is on; stays 0
        // otherwise so the 2D replay path is byte-for-byte unchanged.
        float z = 0.0f;
        // GS TEX0.FST=0 (STQ) sources: the RAW q, so the shader can divide PER PIXEL. The GS
        // interpolates S, T and Q separately and divides per pixel; dividing per VERTEX and
        // letting GL interpolate the quotient linearly smears a lighting ramp (BT3's cel/
        // outline pass) from a thin silhouette band into a broad one. 1.0 = already divided.
        float q = 1.0f;
    };
    struct DrawCmd
    {
        uint64_t texKey;
        bool isTriangle;
        // GS local-to-local VRAM transfer, replayed as an FBO->FBO blit (render targets
        // the game fills by copying VRAM, e.g. the staged logo). When true, the fields
        // below (x*) are used and the draw fields are ignored.
        bool isTransfer = false;
        // VRAM -> this page's FBO. Emitted after a run of draws that the SOFTWARE rasterizer
        // handled (it works in VRAM), so their result re-enters the GPU pipeline AT THE RIGHT
        // POSITION in the command order. Doing the upload at record time instead loses it: the
        // end-of-frame render can replay the whole list into the FBO and overwrite anything
        // that is not itself a command.
        bool isVramBlit = false;
        // Colour is being produced by the SOFTWARE rasterizer for this triangle, but the GS
        // draw also writes Z (the character cel passes all have ZMSK=0). Recorded as a GPU
        // companion with the colour mask off so the depth buffer still gets the character;
        // without it later GPU draws depth-test against a buffer with no character in it and
        // paint bright quads on the ground.
        bool depthOnly = false;
        bool wsHudApplied = false;              // [wshud] aspect squeeze already applied (lists can be replayed)
        bool isAliasPass = false;               // [gpualias] a CT16-view pass (rebuild/edge/clear) recorded for GPU execution
        uint8_t aliasKind = 0;                  // [gpualias] 1 = Z16 rebuild, 2 = untextured clear, 3 = P8H edge
        bool isDecode = false;                  // [deferdec] not a draw: service a deferred texture decode here
        std::shared_ptr<TexDecodeReq> decode;   // [deferdec]
        // The pixels AS THEY WERE when the software run ended. Re-reading VRAM at render time
        // is wrong: by then VRAM holds the whole frame's software output, and the scene copy in
        // it predates every GPU draw recorded after the first bracket -- blitting that wiped
        // the ground shadow decals. Only ~3 brackets happen per frame, so a page snapshot each
        // is cheap.
        std::shared_ptr<std::vector<uint32_t>> vramSnap;
        // [livegl] swOutlineEnd compose deferred to execute time (live mode: the bracket ends
        // on the GUEST thread, where the FBO cannot be read). Dirty mask selecting the
        // software-written pixels; compose happens in the isVramBlit branch on the GL thread.
        std::shared_ptr<std::vector<uint8_t>> swoDirty;
        uint32_t xSrcFbp = 0, xDstFbp = 0;
        int xSX = 0, xSY = 0, xDX = 0, xDY = 0, xW = 0, xH = 0;
        // Destination framebuffer (GS FRAME reg) + source texture base (TEX0.tbp0). Used
        // to route draws to per-fbp FBOs and to sample a render-target framebuffer back
        // as a texture (render-to-texture) instead of stale VRAM.
        uint32_t destFbp = 0, destFbw = 0, srcTbp0 = 0;
        // GS FRAME.PSM of the destination. BT3 re-views one framebuffer address at two bit
        // depths inside a single frame -- fbp0 is both the 32-bit scene and a 512x896 PSMCT16
        // surface -- and one RGBA8 FBO per address cannot express that, so the renderer needs
        // to know which view a draw belongs to.
        uint8_t destPsm = 0;
        // GS TEX1.MMAG (bit 5): magnification filter, 1 = bilinear. BT3 sets it on nearly
        // every draw; sampling those GL_NEAREST is what leaves stretched content blocky.
        bool bilinear = false;
        int srcTexW = 0, srcTexH = 0;   // source texture dims (for render-target FBO size)
        int sx, sy, sw, sh;         // GS scissor rect (top-left origin, framebuffer px)
        // sprite quad (isTriangle == false):
        float dx0, dy0, dx1, dy1;   // dest rect
        float su0, sv0, su1, sv1;   // src rect in TEXELS (DrawTexturePro normalizes)
        uint8_t r, g, b, a;         // single modulate color
        float z = 0.0f;             // sprite depth (normalized GL window depth, larger=nearer)
        // triangle (isTriangle == true): normalized UV + per-vertex color
        Vtx tri[3];
        // [drawbatch] PLAIN-CLASS BATCHING. Ordinary scene triangles arrive in long runs that
        // share every field above -- one command per triangle then costs a ~330 B fill + copy +
        // move each (measured ~240 ns/prim of record path at 37k prims/frame). A batched command
        // carries triangle 0 in tri[] and triangles 1..triCount-1 in triMore (3 Vtx each); every
        // other field describes all of them. triCount == 1 (and triMore == nullptr) is the
        // ordinary one-triangle command, so nothing that walks a list of these changes shape.
        // Only the classes batchEligible() admits are ever batched -- see it for what is excluded
        // and why (HUD splits, alias passes, decals, self-feedback reads, the outline darkener).
        uint32_t triCount = 1;
        std::shared_ptr<std::vector<Vtx>> triMore;
        // GS depth (Z) test state, captured only when PS2X_GPU_DEPTH is on. When off,
        // depthTest stays false and replay behaves exactly as today (no depth test).
        //   depthFunc mirrors GS TEST.ZTST: 0=NEVER, 1=ALWAYS, 2=GEQUAL, 3=GREATER.
        bool depthTest = false;     // GS TEST.ZTE != 0
        uint8_t depthFunc = 1;      // GS TEST.ZTST (default ALWAYS)
        bool depthWrite = false;    // GS ZBUF.ZMSK == 0 (z-write enabled)
        // [zbufbp] GS ZBUF.ZBP (in PAGES) for this draw. Without it the renderer knows depth is
        // written but not WHERE, so the Z page could never be marked rendered and a colour-format
        // read of it could not be recognised. g_zwbBp only ever gets set by the software
        // rasterizer, so in GPU mode it stays 0 and every check against it is inert.
        uint32_t zbufBp = 0;
        // GS TEST alpha-test state (ATE/ATST/AREF/AFAIL). The SW rasterizer honors this;
        // the GPU replay must too — BT3's stage detail overlays, foliage cutouts and HUD
        // effects are alpha-keyed, and drawing their fail texels darkens/noises the frame.
        bool alphaTest = false;     // GS TEST.ATE != 0
        uint8_t alphaFunc = 1;      // GS TEST.ATST (1 = ALWAYS)
        uint8_t alphaRef = 0;       // GS TEST.AREF (0-255, GS alpha units: 0x80 = 1.0)
        uint8_t alphaFail = 0;      // GS TEST.AFAIL (0 = KEEP: discard the fragment)
        // GS TEST destination-alpha test (DATE bit14, DATM bit15): the fragment passes only
        // where the FRAMEBUFFER alpha's bit7 == DATM. BT3's HUD bars: an alpha-only mask is
        // written over the fill fraction, then the empty-look overlay is drawn DATE-gated —
        // the mechanism behind partial health/blast-stock fills. GL has no dest-alpha test;
        // the replay approximates it with dest-alpha lerp blend factors (exact for the 0/0x80
        // binary masks these draws use).
        bool dateEnable = false;    // GS TEST.DATE != 0
        uint8_t dateMode = 0;       // GS TEST.DATM (pass where destA bit7 == DATM)
        uint8_t fst = 0;            // GS PRIM.FST (1 = direct UV coords: 2D/HUD-class draw)
        // GS PRIM.ABE: alpha-blend enable. When false the primitive is opaque and must NOT
        // be alpha-blended (blending an opaque prim over a dark buffer darkens the scene).
        bool abe = false;
        // GS ALPHA register (context): blend equation (A|B<<2|C<<4|D<<6) + FIX constant.
        // BT3 fight uses three modes: 0x44 (Cs-Cd)*As+Cd standard; 0x64 (Cs-Cd)*FIX+Cd
        // (FIX=0x80 -> OPAQUE, must NOT use texture alpha); 0x62 Cd-Cs*FIX (subtractive
        // shadows). Replay maps these to GL blend modes; texture-alpha blending for 0x64
        // was the "black stage in GPU mode" bug.
        uint8_t blendMode = 0x44;   // ALPHA reg low byte
        uint8_t blendFix = 0x80;    // ALPHA reg FIX (bits 32-39)
        // GS FRAME.FBMSK: per-bit framebuffer WRITE MASK (1 = don't write). The Z-buffer-as-
        // texture passes (8px column strips sampling zbp, depth ALWAYS, opaque) rely on it to
        // write only some channels (e.g. alpha-only destination-alpha tricks). The software
        // rasterizer honors it; the GPU replay ignoring it painted opaque black columns over
        // the scene. Replay maps it to glColorMask at byte granularity.
        uint32_t fbmsk = 0;
        // GS CLAMP register wrap modes (0=REPEAT tiles, 1=CLAMP, 2/3=region variants).
        // GL textures were created CLAMP-only; stage/sky triangles use negative/beyond-1 STQ with
        // REPEAT — clamping collapsed them to texel(0,0) = flat gray/black scene.
        uint8_t wrapU = 1;          // 0=repeat 1=clamp
        uint8_t wrapV = 1;
        // GS CLAMP as issued: wms/wmt 0=REPEAT 1=CLAMP 2=REGION_CLAMP 3=REGION_REPEAT, plus
        // the region window. REGION_REPEAT is NOT repeat -- the texel is (u & MINU) | MAXU --
        // so approximating it by REPEAT lets out-of-range UVs wander across the atlas.
        uint8_t tfx = 0;   // GS TEX0.TFX: 0=MODULATE 1=DECAL 2=HIGHLIGHT 3=HIGHLIGHT2
        uint8_t wms = 0, wmt = 0;
        uint16_t minu = 0, maxu = 0, minv = 0, maxv = 0;
        // GS TEX0.TCC: 0 = texture alpha NOT used (alpha comes from vertex/fragment only;
        // emulated by swizzling the texture's A to ONE for this draw), 1 = use texture alpha.
        uint8_t tcc = 1;
        // Source texture uses an indexed/CLUT format (PSMT8/PSMT4/...). You cannot render to an
        // indexed framebuffer, so such a texture is NEVER a render target -> must be DECODED, never
        // composited from an FBO slot even if its VRAM base aliases a render-target base.
        bool srcIndexed = false;
        // The sampled VRAM base page received an IMAGE upload more recently than any draw
        // rendered into that fbp (set in recordCmd). The game means the uploaded texture,
        // not render-target feedback -> decode it; never substitute the FBO slot / neutral
        // white. Fight stages upload their tiles into fbp-aliasing regions (e.g. tbp0=10752
        // = fbp336) every frame; treating those as RT samples flattened the whole 3D scene
        // to one texture in GPU mode.
        bool srcUploaded = false;
        // Source page was genuinely RENDERED into (not merely never uploaded). Such a
        // texture cannot be re-decoded -- GPU mode never writes rendered pixels to VRAM.
        bool srcRendered = false;
        // GS TEX0.PSM of the source, and TEXA. A PSMCT24 render target has NO stored alpha --
        // hardware derives it from TEXA and gets 0 wherever RGB is zero when AEM=1, which is
        // what makes the character-shadow pass invisible on console. Sampling the FBO's own
        // alpha byte instead (255 after a clear) makes every fragment pass its NOTEQUAL-vs-0
        // alpha test, painting dark parallelogram bands across clean ground.
        uint8_t srcPsm = 0;
        uint8_t texaTa0 = 0x80, texaTa1 = 0x80;
        bool texaAem = false;
        // TEX0.CBP of an indexed source. An indexed texture can have perfectly re-decodable
        // INDEX data while its PALETTE was produced by rendering -- decoding it again then
        // yields right indices through a wrong CLUT. srcRendered only inspects the texture's
        // own page, so that case slips through unprotected.
        uint32_t srcClutTbp = 0;
        // Key into g_clutPalettes (renderer-side) for the DECODED 256-entry palette of an
        // indexed source. Needed to sample an indexed texture that lives in a live FBO: the
        // index comes from the FBO's alpha channel, so the palette has to travel with the draw
        // rather than be re-decoded from VRAM (which is exactly the stale data we are avoiding).
        uint64_t srcClutKey = 0;
        // GS FBA (FBA_1/FBA_2): force bit 7 of the alpha WRITTEN to the framebuffer. Applied
        // after blending and skipped for PSMCT24 (no alpha in memory). BT3 sets it on ~4300
        // terrain kicks per frame, and the mask chain reads frame alpha as a palette INDEX, so
        // dropping it shifts every one of those lookups.
        bool fba = false;
    };

    // ---- GIF/DMA thread (called from the rasterizer) ----
    // Per-VRAM-page invalidation: a cached decode is valid only if none of the pages its
    // texels occupy [pageLo,pageHi] were written since the decode. This avoids the global
    // "any upload invalidates everything" churn that tanked fps on busy screens.
    bool hasTexture(uint64_t key, uint32_t pageLo, uint32_t pageHi);
    // hasTexture + content-hash revalidation: when the texture's VRAM pages were dirtied,
    // hash the underlying span first and keep the cached decode if the bytes are unchanged.
    // Unrelated uploads land in shared 8KB pages constantly (fight effects/CLUT streams),
    // and a hash is ~10x cheaper than the decode+GL-upload a false invalidation costs.
    // Content-versioned key resolution: BT3 STREAMS several materials through one VRAM
    // slot (tbp 10752) between draw packets. A per-(regs+CLUT) key made them all collide
    // on ONE cache entry/GL texture — the replay drew every pass with the LAST-uploaded
    // material (flat dark-green terrain in GPU mode; SW samples VRAM live and was right).
    // This folds the texel-span content hash into the key: each material version gets its
    // own stable entry (decoded once ever, even for per-frame cycling). Returns the
    // versioned key; needDecode=true when the caller must decode+putTexture under it.
    uint64_t pageDrawStamp(uint32_t lo, uint32_t hi) const   // [rectemplate] max FBO draw seq over a page span (0 for an empty span)
    {
        uint64_t m = 0; if (hi >= kVramPages) hi = kVramPages - 1;
        for (uint32_t p = lo; p <= hi; ++p) { const uint64_t v = __atomic_load_n(&m_pageDrawSeq[p], __ATOMIC_ACQUIRE); if (v > m) m = v; }
        return m;
    }
    uint64_t recTplEpoch() const   // [rectemplate] everything resolveTextureVersion / rtServedRead read besides GS state
    {
        uint64_t e = m_writeSeq; e = (e * 0x9E3779B97F4A7C15ull) ^ m_texCacheEpoch.load(std::memory_order_relaxed);
        e = (e * 0x9E3779B97F4A7C15ull) ^ m_renderOnceGen; return e;
    }
    bool fbpRenderedOnce(uint32_t fbp) const { return fbp < kVramPages && m_fbpRenderSeq[fbp] != 0u; }   // [p8twinskip] has GL ever rendered this fbp
    // [rtreadskip] PS2X_RTREADSKIP=<mask>: record-time reads of GL-rendered pages whose replay never touches the VRAM
    // decode -- skip the decode and the barrier request. bit0: PSMT8H mask builds (scene 0/112 -> f224, IDXRT);
    // bit1: Z-format reads of the Z page (dropped by gaZ16DropRead); bit2: the CT32 blur chain 336->368->502<->504;
    // bit3: CT16 views of page 336 (mode-4 gaview). Default 0 until each class passes matched-warm parity.
    static bool rtReadServedClass(uint32_t pg, uint32_t psm, uint32_t destFbp)
    {
        static const int s_mask = [](){ const char *v = std::getenv("PS2X_RTREADSKIP"); return v && v[0] ? std::atoi(v) : 2047; }();   // default: every class below (all parity-clean 2026-09-03)
        if (s_mask == 0) return false;
        if ((s_mask & 1) && psm == 27u && (pg == 0u || pg == 112u) && destFbp == 224u) return true;
        if ((s_mask & 2) && pg == 224u && (psm == 0x30u || psm == 0x31u || psm == 0x32u || psm == 0x3Au)) return true;
        if ((s_mask & 4) && psm == 0u && ((pg == 336u && destFbp == 368u) || (pg == 368u && destFbp == 502u) || (pg == 502u && destFbp == 504u) || (pg == 504u && destFbp == 502u))) return true;
        if ((s_mask & 8) && psm == 2u && pg == 336u && (destFbp == 0u || destFbp == 112u)) return true;
        // bit4: the blur chain's UP direction (504 -> 368 -> 336, CT32); bit5: PSMT8H imports of the mask page into the
        // scene (P8TWIN); bit6: the bloom composite (336 CT32 -> scene); bit7: the scene downsample into the chain
        // (0/112 CT32 -> f336); bit8: scene CT32 reads into the mask page (0/112 -> f224).
        if ((s_mask & 16) && psm == 0u && ((pg == 504u && destFbp == 368u) || (pg == 368u && destFbp == 336u))) return true;
        if ((s_mask & 32) && psm == 27u && pg == 224u && (destFbp == 0u || destFbp == 112u)) return true;
        if ((s_mask & 64) && psm == 0u && pg == 336u && (destFbp == 0u || destFbp == 112u)) return true;
        if ((s_mask & 128) && psm == 0u && (pg == 0u || pg == 112u) && destFbp == 336u) return true;
        if ((s_mask & 256) && psm == 0u && (pg == 0u || pg == 112u) && destFbp == 224u) return true;
        // bit9: PSMT8H mask reads into the ink page (224 -> f336); bit10: CT24 views of the edge map into the scene (336 -> f0/112)
        if ((s_mask & 512) && psm == 27u && pg == 224u && destFbp == 336u) return true;
        if ((s_mask & 1024) && psm == 1u && pg == 336u && (destFbp == 0u || destFbp == 112u)) return true;
        return false;
    }
    // [rtreadskip] page rendered by GL and not re-uploaded by the guest since: the replay samples the FBO, never a VRAM decode
    bool pageRenderedNotUploaded(uint32_t pg) const { return pg < kVramPages && m_fbpRenderSeq[pg] != 0u && !(m_pageSeq[pg] > m_fbpRenderSeq[pg]); }
    uint64_t resolveTextureVersion(uint64_t baseKey, uint32_t pageLo, uint32_t pageHi,
                                   const uint8_t *vram, uint32_t vramSize, bool &needDecode);
    // PS2X_BARRIER: a draw that SAMPLES a page an earlier queued draw WROTE cannot see that
    // write, because commands are decoded at build time and rendered later. recordCmd() raises a
    // request here; the replay loop drains it by publishing + rendering what is built so far and
    // flushing that page to VRAM, so the next decode reads fresh bytes. Census says this fires
    // ~37 times per frame (0.09% of draws), so it is cheap.
    bool takeBarrierRequest(uint32_t &page);
    // [barblock] PS2X_BARBLOCK=1: GL-thread service of guest barriers (see the .cpp).
    bool serviceBlockingBarriers();
    void waitForBlockingBarrierRequest(int micros);
    void abortBlockingBarriers();
    static bool blockingBarriersEnabled();
    // Read one page's FBO back and push it into VRAM in that page's own recorded format.
    void flushPageToVram(uint32_t fbp);
    void flushRecentPagesToVram(int minVsync);   // [slice]

    // Mid-frame barrier render (PS2X_BARRIER). Draws the commands of the IN-PROGRESS list
    // that have not been drawn yet into their FBOs and returns -- no display pick, no
    // present, no publish, no census. Lets a page be flushed to VRAM (flushPageToVram)
    // BEFORE a later draw in the same frame samples it, which is what the GS does natively
    // and an FBO-per-address renderer cannot otherwise express. The commands it draws are
    // remembered (m_segFrom), so the end-of-frame render resumes rather than redrawing them
    // -- redrawing would blend every prior command a second time.
    void renderRange(int fbWidth, int fbHeight);
    bool prerenderChunk();   // [prerender] GL thread: draw already-recorded commands ahead of the next barrier

    // Read-time barrier: if the page behind srcTbp0 has been drawn into since its last flush,
    // render what is built so far and push that page to VRAM, so the decode about to run reads
    // the bytes the earlier draws produced. Call BEFORE decoding, from the GL-owning thread.
    // requireAligned=false is for a CLUT: a palette base need not be page-aligned (BT3's
    // outline CLUTs sit at block 15972 = page 499 + 4 blocks), and a palette can itself be a
    // render target, so the page holding it has to be flushed before it is read.
    // wantsAlphaAsData: the reader is sampling this page as PSMT8H, i.e. it wants the ALPHA
    // BYTE as a palette index. Only then may the flush push the framebuffer's alpha into VRAM;
    // every other flush must protect it, or a later flush with a flat/empty FBO alpha wipes
    // the field the mask build needs (measured: good field written, then zeroed, VRAM ends 0%).
    void barrierBeforeRead(uint32_t srcBlock, bool requireAligned = true,
                           bool wantsAlphaAsData = false, bool *deferOut = nullptr);   // [deferdec] deferOut: report "would wait" instead of waiting
    void postDecode(std::shared_ptr<TexDecodeReq> req);   // [deferdec]
    bool flushPending(uint32_t page);                    // [deferpend] a deferred flush of this page is queued but not yet serviced
    void linRefresh(struct LinImg &img);   // [linvram] bring the linear image up to VRAM for pages the guest wrote
    struct LinImg *linImageFor(uint32_t fbp, uint32_t psm, uint32_t fbw, int w, int h, bool create);   // [linvram]
    void waitPendingFlush(uint32_t page);                // [uploadwait] guest: block (bounded) until that flush has run -- before overwriting the page
    bool serviceNextDecode();                            // [deferdec] GL thread: render up to the next unserved decode command, then service it
    void serviceDecodeReq(TexDecodeReq &q);              // [deferdec] GL thread: flush page(s) -> decode -> putTexture

    // Push the scene FBO's ALPHA into VRAM once, immediately before BT3's alpha-rebuild pass
    // runs in software. The rebuild only writes bits 14-15 of a CT16 view (~25% of the alpha
    // bytes); the rest of console's field is the geometry's own alpha, which our per-flush
    // alpha protection was keeping out of VRAM entirely.
    void seedVramAlpha(uint32_t fbp);
    // Hand the scene ALPHA over from the FBO to VRAM, once per frame, just before BT3's
    // rebuild pass starts writing that byte. See the definition for why the handover has to
    // be phased rather than protected unconditionally.
    void seedSceneAlphaForRebuild(uint32_t fbp);
    // BT3's cel/outline pass (blend 0x62, toon ramp tbp 15680) is drawn as sub-pixel SLIVER
    // triangles -- median ONE covered pixel each -- and GL's top-left fill rule drops most of
    // them, so we cover only 70.7% of the pixels the GS covers and lose 55.8% of the highest-u
    // band, which IS the silhouette rim. The software rasterizer uses an inclusive edge test
    // and reproduces them, but it works in VRAM while the scene lives in an FBO. These bracket
    // the pass: hand the page down to VRAM, let the software path draw, bring it back.
    void swOutlineBegin(uint32_t fbp);
    void swOutlineEnd();
    void blitVramPageToBoundFbo(const DrawCmd &c);
    bool swOutlineActive() const { return m_swOutlineFbp != 0xFFFFFFFFu; }
    uint32_t swOutlinePage() const { return m_swOutlineFbp; }
    void reportFboAlpha(uint32_t fbp, const char *when);

    bool revalidateTexture(uint64_t key, uint32_t pageLo, uint32_t pageHi,
                           const uint8_t *vram, uint32_t vramSize);
    void putTexture(uint64_t key, std::vector<uint8_t> rgba, int w, int h, uint32_t pageLo, uint32_t pageHi);
    void recordCmd(const DrawCmd &cmd);
    // [drawbatch] ---- plain-class triangle batching (guest thread only) ----
    // Batching lives entirely in the guest-owned staging buffer (PS2X_RECSTAGE > 0, the default):
    // the open batch is m_stage[m_openBatchIdx], so appending needs no lock at all and the GL
    // thread can never see a half-built batch. flushStage() closes it before the commands become
    // visible. With RECSTAGE <= 0 (everything under m_mtx) batching is simply off.
    static bool batchingEnabled();                       // PS2X_DRAWBATCH (default on) && RECSTAGE > 0
    static bool batchEligible(const DrawCmd &c);         // is this command a plain scene triangle?
    static int  batchWhy(const DrawCmd &c);              // 0 = eligible, else which rule rejected it
    static void batchWhyCensus(const DrawCmd &c);        // [batchwhy] PS2X_BATCHWHY=1 rejection census
    static bool sameBatchState(const DrawCmd &a, const DrawCmd &b);   // every field except tri[]/triCount/triMore
    bool batchOpen() const { return m_openBatchIdx != (size_t)-1; }   // guest thread only
    // Append one triangle to the open batch. Returns false if there is no open batch, it is
    // full, or the caller's state no longer matches -- the caller then records a full command.
    bool appendBatchTri(const Vtx v[3], bool addDirty = true);
    void closeBatch() { closeOpenBatch(); }
    // Bumped by every recordCmd and every appended triangle. The recorder keeps the last value it
    // saw next to its retained command; a mismatch means SOMETHING ELSE recorded in between (another
    // thread, an internal blit, an alias pass), so the retained state is no longer the batch head's.
    uint64_t batchSeq() const { return m_batchSeq; }
    void swapFrame();      // frame boundary (FUN_00100ab8): publish command list
    void onVramUpload(uint32_t dbpBlock, uint32_t sizeBlocks); // stamp the written VRAM pages
    bool hasPendingFrame();   // [pubbreak] a published list is waiting for its present
    void onVramWriteback(uint32_t dbpBlock, uint32_t sizeBlocks); // content-only stamp
    static constexpr uint32_t kVramPages = 512; // 4MB / 8KB page
    // DISPFB1 -> the scanned-out buffer's fbp AND its display stride (FBW, in 64px units).
    // If the display FBW differs from the draw FBW, the present must re-stride the buffer.
    void setDisplay(uint32_t fbp, uint32_t fbw) { m_hintDisplayFbp = fbp; m_hintDisplayFbw = fbw; }

    // ---- present thread (owns the GL context) ----
    // Replay the published frame into the FBO; returns the rendered GL texture id
    // (0 = nothing yet). fbWidth/fbHeight = the PS2 display size.
    unsigned int renderAndGetTextureId(int fbWidth, int fbHeight);
    bool debugSavePresent(const char *path);   // offline replay harness
    // Display region derived from the command scissors (the software latch that
    // normally reports this doesn't run in GPU mode). Valid after renderAndGetTextureId.
    int displayWidth() const { return m_dispW; }
    int displayHeight() const { return m_dispH; }
    // Actual GL texture size of the presented (display) FBO — the present crop must
    // normalize its source rect against these, not the requested FB size.
    int presentTexWidth() const { return m_presentTexW; }
    int presentTexHeight() const { return m_presentTexH; }
    // Top-left origin (in presentTex pixels) of the region to crop for display. Non-zero only in
    // PS2X_ATLAS mode, where the display buffer occupies a sub-rect of the big atlas texture.
    int presentSrcX() const { return m_presentSrcX; }
    int presentSrcY() const { return m_presentSrcY; }

    uint64_t recordedThisSecond();

private:
    struct CachedTex
    {
        std::vector<uint8_t> rgba;  // linear RGBA8, w*h*4
        int w = 0, h = 0;
        bool needsUpload = false;
        unsigned int glId = 0;      // GL texture id (present thread only)
        uint32_t decodeSeq = 0;     // m_writeSeq at decode time
        uint64_t srcHash = 0;       // hash of the VRAM page span at decode/revalidate time
        bool srcHashValid = false;
    };

    std::mutex m_mtx;
    // Mid-frame segment state (see renderRange). m_segMode is set only for the duration of
    // one renderRange call; m_segActive stays set until the frame's final render consumes
    // the watermark. The depth-cleared set has to survive across segments of a frame or each
    // segment would re-clear the shared Z and the frame would self-destruct.
    bool   m_seedAlphaOnce = false;
    bool   m_flushAlphaNow = false;
    bool   m_segMode   = false;
    // [prerender] chunk mode: cmds is a COPY of m_building[m_chunkBase, m_chunkBase + cmds.size())
    // taken under m_mtx, so the guest keeps recording while the GL thread draws.
    bool   m_chunkMode = false;
    size_t m_chunkBase = 0;
    bool   m_segSwapped = false;          // [segshadow] a segment render holds m_building swapped out; guest appends go to the shadow
    std::vector<DrawCmd> m_buildingShadow;   // [segshadow]
    std::vector<DrawCmd> m_segTail;          // [segtail] commands past m_stopAt, parked during a split segment render
    const std::vector<uint32_t> *m_pageSkipSeq = nullptr; uint32_t m_pageSkipLo = 0;   // [pageskip] while servicing a deferred flush
    std::atomic<uint32_t> m_svcLo{0xFFFFFFFFu}, m_svcN{0}, m_svcLo2{0xFFFFFFFFu}, m_svcN2{0};   // [svcwindow] page ranges a deferred service is touching right now
    const TexDecodeReq *m_zwbOverride = nullptr;   // [zwbsnap] while servicing a deferred flush: use the request's zbuf snapshot
    bool   m_inDecodeService = false;   // [deferdec] reentrancy guard for serviceNextDecode
    size_t m_stopAt = (size_t)-1;   // [deferdec] a render stops BEFORE this index (segment split at a decode command)
    std::vector<DrawCmd> m_chunk;
    bool   m_segActive = false;
    size_t m_segFrom   = 0;
    std::unordered_set<uint32_t> m_segDepthCleared;
    std::vector<DrawCmd> m_building;
    std::vector<DrawCmd> m_stage;   // [recstage] guest-owned staging, moved into m_building under one lock
    void flushStage();
    // [drawbatch] index into m_stage of the batch currently accepting triangles ((size_t)-1 = none).
    size_t m_openBatchIdx = (size_t)-1;
    // g_barDirtyGen as it stood when the batch head did its barrier bookkeeping; an append is only
    // valid while nothing has REMOVED a dirty entry since (see appendBatchTri).
    uint32_t m_openBatchDirtyGen = 0;
    // [drawbatch] One dirty-rect union for the whole run instead of one per triangle: the union is
    // monotone and the staged commands are invisible to the GL thread until flushStage, so nothing
    // can read the page back between the appends and closeOpenBatch(). Saves a g_barMx lock per
    // primitive -- ~1M of them per second in a fight.
    float m_batchRX0 = 0.f, m_batchRY0 = 0.f, m_batchRX1 = 0.f, m_batchRY1 = 0.f;
    uint32_t m_batchRFbp = 0;
    bool m_batchRPend = false;
    void closeOpenBatch();
    // Recycled vertex stores for batched commands. A store is handed to a DrawCmd by shared_ptr and
    // travels with it through m_building -> m_pending -> the replay; use_count()==1 means the last
    // holder is this ring, so it can be refilled without a fresh allocation. Batches of one triangle
    // never take a store at all (tri[] holds it), so this only churns for runs that really merged.
    std::vector<std::shared_ptr<std::vector<Vtx>>> m_vtxRing;
    size_t m_vtxRingPos = 0;
    uint64_t m_batchSeq = 0;
    std::shared_ptr<std::vector<Vtx>> takeVtxStore();
    std::vector<DrawCmd> m_ready;
    // Published-but-not-yet-replayed lists, oldest first. FBOs are persistent (content only
    // changes by replaying draws), so every published list MUST be replayed exactly once, in
    // order — the old replace-on-publish (m_ready.swap) silently dropped every list the
    // present thread didn't get to, leaving buffers frozen mid-frame (flat-gray scene at
    // downsample time, strobing presents, empty light/shadow maps).
    std::vector<std::vector<DrawCmd>> m_pending;
    std::vector<uint32_t> m_pendingGen;   // [groundshadow] publish generation of each pending list (parallel to m_pending)
    std::vector<std::vector<DrawCmd>> m_vecPool;
    std::vector<DrawCmd> m_cmdsScratch;   // [vecpool] renderAndGetTextureId command scratch   // [vecpool] emptied frame lists handed back by the GL thread so the guest reuses their capacity (a moved-out m_building regrew from 0 every frame: ~5% of the guest thread in DrawCmd moves)
    std::unordered_map<uint64_t, CachedTex> m_texCache;
    std::vector<uint64_t> m_upQueue;   // [upqueue] keys with needsUpload set since the last drain (guarded by m_mtx)
    // [verfast] lock-free clean path for resolveTextureVersion: the guest keeps a mirror of "this verKey is decoded and
    // present"; the GL thread bumps m_texCacheEpoch whenever it removes cache entries, which drops the mirror.
    std::atomic<uint32_t> m_texCacheEpoch{0};
    std::unordered_map<uint64_t, uint8_t> m_verPresent;
    uint32_t m_verPresentEpoch = 0;
    struct TexVersion
    {
        uint32_t seqChecked = 0; // m_writeSeq when the span was last hashed/validated
        uint64_t drawChecked = 0;// m_drawSeq when the span was last hashed/validated
        uint64_t hash = 0;       // content hash of the texel span at that time
        uint64_t verKey = 0;     // baseKey ^ mixed(hash); 0 = never resolved
    };
    std::unordered_map<uint64_t, TexVersion> m_texVersion; // baseKey -> current version
    uint32_t m_swOutlineFbp = 0xFFFFFFFFu;   // page currently handed to the software rasterizer
    uint32_t m_alphaSeedGen = 0xFFFFFFFFu;   // publish gen whose scene alpha we already seeded
    // m_writeSeq only advances on VRAM UPLOADS, so neither it nor m_fbpRenderSeq can tell that
    // we have drawn into a page since a texture sourced from it was last decoded. BT3 samples
    // the scene page as PSMT8H twice a frame -- before and after the characters are drawn --
    // with identical TEX0, so the second read hit the cache and reused the FIRST decode. That
    // is why the mask (and the outline built from it) held terrain only.
    uint64_t m_drawSeq = 0;                  // monotonically bumped per RECORDED draw
    uint64_t m_pageDrawSeq[kVramPages] = {}; // m_drawSeq of the last draw INTO each page
    uint32_t m_writeSeq = 0;                 // monotonically bumped per VRAM upload
    uint32_t m_renderOnceGen = 0;            // [rectemplate] bumped when a page is rendered for the first time (fbpRenderedOnce flips)
    uint32_t m_pageSeq[kVramPages] = {};     // last GUEST-upload seq per VRAM page (ROUTING)
    // Content version, bumped by guest uploads AND by our own FBO->VRAM writeback. The cache
    // must re-decode when either changes, but only a GUEST upload may influence srcUploaded --
    // stamping m_pageSeq from a writeback flips the FBO-vs-decode routing and makes the blur
    // chain flap between paths on alternate frames.
    uint32_t m_contentSeq[kVramPages] = {};
    uint32_t m_uploadSeq[kVramPages] = {};   // [pageskip] bumped by GUEST uploads/copies only (m_contentSeq also counts GL writebacks)
    // Last write seq at which a draw/transfer RENDERED INTO each fbp. Indexed by fbp,
    // which equals the VRAM page index (tbp0/32), same unit as m_pageSeq. Compared
    // against m_pageSeq to decide upload-vs-RT precedence (DrawCmd::srcUploaded).
    uint32_t m_fbpRenderSeq[kVramPages] = {};
    uint64_t m_recordCount = 0, m_recordSnapshot = 0;

    // GL (present thread only)
    unsigned int m_fbo = 0, m_fboTex = 0;
    int m_fboW = 0, m_fboH = 0;
    int m_dispW = 0, m_dispH = 0;
    int m_presentTexW = 0, m_presentTexH = 0;
    int m_presentSrcX = 0, m_presentSrcY = 0;
    uint32_t m_hintDisplayFbp = 0xFFFFFFFFu; // DISPFB1 fbp (the buffer the CRT scans out)
    uint32_t m_hintDisplayFbw = 0u;          // DISPFB1 fbw (display stride, 64px units)
    bool m_glInit = false;
    void ensureGl(int w, int h);
};

GsGpuRenderer &ps2GpuRenderer();

#endif
