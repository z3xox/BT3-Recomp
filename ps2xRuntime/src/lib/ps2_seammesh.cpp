// [seam] see include/runtime/ps2_seammesh.h and docs/NATIVE-RENDER-SEAM.md.
#include "runtime/ps2_seammesh.h"
#include "runtime/ps2_seamprobe.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_seamvk.h"   // [seamvk]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h>
#endif
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

extern std::atomic<uint64_t> g_bt3FrameCount;   // game_overrides.cpp
extern uint8_t *g_ps2WatchRdram;                // ps2_runtime.cpp: the bound guest RAM

// ============================================================================================
// Phase 1: importer
// ============================================================================================
std::atomic<uint64_t> g_seamHostChunks{0};   // [fightgate] host mesh chunks emitted by the seam (ps2_seammesh.cpp); the frame gate latches the fight on these when VU1 pairs are gone

namespace seammesh
{
    namespace
    {
        uint64_t fnv(const uint8_t *p, size_t n)
        {
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
            return h;
        }
        struct Cache
        {
            std::mutex mtx;
            std::unordered_map<uint32_t, Mesh> meshes;
            uint64_t hits = 0, misses = 0, rehash = 0, bad = 0;
            int dumped = 0, badPrinted = 0;
        };
        Cache &cache() { static Cache *c = new Cache; return *c; }

        const char *dumpDir()
        {
            static const char *s_d = [](){ const char *v = std::getenv("PS2X_SEAMDUMP"); return (v && v[0]) ? v : nullptr; }();
            return s_d;
        }

        void dumpObj(const Mesh &m)
        {
            const char *dir = dumpDir();
            if (!dir) return;
            std::error_code ec; std::filesystem::create_directories(dir, ec);
            char name[512];
            std::snprintf(name, sizeof(name), "%s/mesh_%s_%08x.obj", dir, m.hasSetup ? "stage" : "char", m.list);
            std::FILE *f = std::fopen(name, "w");
            if (!f) return;
            std::fprintf(f, "# list 0x%08x  %u chunks  %u verts  hash %016llx\n", m.list, (unsigned)m.chunks.size(), m.totalVerts, (unsigned long long)m.hash);
            uint32_t base = 1;
            for (size_t ci = 0; ci < m.chunks.size(); ++ci)
            {
                const Chunk &c = m.chunks[ci];
                std::fprintf(f, "o chunk%u\n", (unsigned)ci);
                for (uint32_t v = 0; v < c.count; ++v)
                {
                    float q[12]; std::memcpy(q, c.verts.data() + v * 48u, 48);
                    if (m.hasSetup)   // P, float RGBA, T
                        std::fprintf(f, "v %g %g %g %g %g %g\n", q[0], q[1], q[2], q[4] / 128.0f, q[5] / 128.0f, q[6] / 128.0f);
                    else              // P (w = weight), N, T
                        std::fprintf(f, "v %g %g %g\n", q[0], q[1], q[2]);
                    std::fprintf(f, "vt %g %g\n", q[8], q[9]);
                }
                for (uint32_t v = 2; v < c.count; ++v)
                {
                    const uint32_t a = base + v - 2, b = base + v - 1, cc = base + v;
                    if (v & 1u) std::fprintf(f, "f %u/%u %u/%u %u/%u\n", b, b, a, a, cc, cc);
                    else        std::fprintf(f, "f %u/%u %u/%u %u/%u\n", a, a, b, b, cc, cc);
                }
                base += c.count;
            }
            std::fclose(f);
            std::fprintf(stderr, "[seammesh] dumped %s (%u verts, %u chunks)\n", name, m.totalVerts, (unsigned)m.chunks.size());
        }

        thread_local const char *t_why = "";
        bool parse(uint8_t *rdram, uint32_t list, Mesh &m)
        {
            m = Mesh{}; t_why = "";
            m.list = list & 0x1FFFFFFFu;
            const uint8_t *tp = getMemPtr(rdram, m.list);
            if (!tp) { t_why = "no mem"; return false; }
            uint32_t w[4]; std::memcpy(w, tp, 16);
            const uint32_t qwc = w[0] & 0xFFFFu, id = (w[0] >> 28) & 7u;
            if (id != 6u || qwc == 0u || qwc > 0x4000u) { t_why = "tag"; return false; }    // only RET lists are known
            m.bytes = 16u + qwc * 16u;
            const uint8_t *data = getMemPtr(rdram, m.list + 16u);
            if (!data || !getMemPtr(rdram, m.list + m.bytes - 1u)) { t_why = "range"; return false; }
            m.hash = fnv(tp, m.bytes);
            { const uint32_t h = m.bytes < 64u ? m.bytes : 64u; std::memcpy(m.head, tp, h); std::memcpy(m.tail, tp + m.bytes - h, h); }
            m.checkedFrame = g_bt3FrameCount.load(std::memory_order_relaxed);
            // VIF stream: tag words 2,3 then the data, as one contiguous buffer (a payload can start
            // in the tag's last word).
            const uint32_t total = 8u + qwc * 16u;
            std::vector<uint8_t> stream(total);
            std::memcpy(stream.data(), &w[2], 8);
            std::memcpy(stream.data() + 8, data, qwc * 16u);
            auto rd = [&](uint32_t off) -> uint32_t { uint32_t v; std::memcpy(&v, stream.data() + off, 4); return v; };
            auto ptr = [&](uint32_t off) -> const uint8_t * { return stream.data() + off; };
            uint32_t pos = 0;
            Chunk cur; bool open = false;
            while (pos + 4u <= total)
            {
                const uint32_t c = rd(pos); pos += 4u;
                const uint32_t cmd = (c >> 24) & 0x7Fu, num = (c >> 16) & 0xFFu, imm = c & 0xFFFFu;
                if (cmd == 0x00u) continue;                                   // NOP
                if (cmd == 0x17u)                                             // MSCNT closes a chunk (or the setup)
                {
                    if (open) { m.totalVerts += cur.count; m.chunks.push_back(std::move(cur)); cur = Chunk{}; open = false; }
                    continue;
                }
                if (cmd != 0x6Cu) { t_why = "cmd"; return false; }                               // only unmasked V4-32
                const uint32_t nvec = num ? num : 256u, bytes = nvec * 16u;
                if (pos + bytes > total || !(imm & 0x8000u)) { t_why = "unpack range/flag"; return false; }
                const uint8_t *p = ptr(pos);
                if (!p) { t_why = "ptr"; return false; }
                const uint32_t addr = imm & 0x3FFu;
                if (addr == 0u)
                {
                    if (nvec == 1u && !open && m.chunks.empty() && !m.hasSetup) { std::memcpy(m.setup, p, 16); m.hasSetup = true; }
                    else if ((nvec == 3u || nvec == 5u) && !open) { cur.hdrQw = nvec; std::memcpy(cur.hdr, p, nvec * 16u); open = true; }
                    else { t_why = "hdr shape"; return false; }
                }
                else
                {
                    if (!open || addr != cur.hdrQw) { t_why = "vert shape"; return false; }
                    cur.nvec = nvec;
                    cur.count = nvec / 3u;
                    cur.verts.assign(p, p + bytes);
                }
                pos += bytes;
            }
            if (open) { m.totalVerts += cur.count; m.chunks.push_back(std::move(cur)); }
            m.ok = !m.chunks.empty();
            return m.ok;
        }
    }

    const Mesh *get(uint8_t *rdram, uint32_t list, bool volatileList)
    {
        Cache &c = cache();
        std::lock_guard<std::mutex> lk(c.mtx);
        const uint32_t key = list & 0x1FFFFFFFu;
        auto it = c.meshes.find(key);
        if (it != c.meshes.end())
        {
            // Re-validate: the loader may have rewritten this memory. Cheap check every use (the
            // tag, the first and the last 64 bytes), full hash once every 64 frames per mesh.
            Mesh &m = it->second;
            const uint8_t *tp = getMemPtr(rdram, key);
            const uint64_t fr = g_bt3FrameCount.load(std::memory_order_relaxed);
            if (tp && m.ok)
            {
                const uint32_t n = m.bytes, h = n < 64u ? n : 64u;
                const bool quick = std::memcmp(tp, m.head, h) == 0 && std::memcmp(tp + n - h, m.tail, h) == 0;
                if (quick && !volatileList && fr - m.checkedFrame < 64u) { ++c.hits; return &m; }
                if (quick && fnv(tp, n) == m.hash) { m.checkedFrame = fr; ++c.hits; return &m; }
            }
            ++c.rehash;
        }
        ++c.misses;
        Mesh m;
        if (!parse(rdram, key, m))
        {
            ++c.bad; c.meshes[key] = Mesh{};
            if (c.badPrinted < 8) { ++c.badPrinted; std::fprintf(stderr, "[seammesh] parse failed list=0x%08x: %s\n", key, t_why); }
            return nullptr;
        }
        if (c.dumped < 12) { ++c.dumped; dumpObj(m); }
        Mesh &slot = c.meshes[key];
        slot = std::move(m);
        return &slot;
    }

    void report()
    {
        Cache &c = cache();
        std::lock_guard<std::mutex> lk(c.mtx);
        std::fprintf(stderr, "[seammesh] cache: %zu meshes, hits=%llu misses=%llu rehash=%llu bad=%llu\n",
                     c.meshes.size(), (unsigned long long)c.hits, (unsigned long long)c.misses, (unsigned long long)c.rehash, (unsigned long long)c.bad);
        c.hits = c.misses = c.rehash = c.bad = 0;
    }
}

// ============================================================================================
// Phase 2: host transform for 1627a6cb (stage / effect tristrips), mirroring the native kernel
// in ps2_vu1_native.cpp instruction for instruction so the bytes can be compared.
// ============================================================================================
namespace seamxform
{
    namespace
    {
        inline __m128 bc(__m128 v, int lane)
        {
            switch (lane)
            {
            case 0: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
            case 1: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
            case 2: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
            default: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
            }
        }
        inline __m128 xformW(const __m128 *r, __m128 v)
        {
            __m128 acc = _mm_mul_ps(r[0], bc(v, 0));
            acc = _mm_add_ps(acc, _mm_mul_ps(r[1], bc(v, 1)));
            acc = _mm_add_ps(acc, _mm_mul_ps(r[2], bc(v, 2)));
            return _mm_add_ps(acc, _mm_mul_ps(r[3], bc(v, 3)));
        }
        inline float vuClampFloat(float f)
        {
            uint32_t u; std::memcpy(&u, &f, 4);
            if ((u & 0x7F800000u) == 0x7F800000u) { u = (u & 0x80000000u) | 0x7F7FFFFFu; std::memcpy(&f, &u, 4); }
            return f;
        }
        inline __m128i ftoi(__m128 f)
        {
            __m128i iv = _mm_cvttps_epi32(f);
            const __m128i bad = _mm_cmpeq_epi32(iv, _mm_set1_epi32((int)0x80000000));
            if (_mm_movemask_epi8(bad))
            {
                const __m128i neg = _mm_srai_epi32(_mm_castps_si128(f), 31);
                const __m128i satv = _mm_or_si128(_mm_and_si128(neg, _mm_set1_epi32((int)0x80000000)),
                                                  _mm_andnot_si128(neg, _mm_set1_epi32(0x7FFFFFFF)));
                iv = _mm_or_si128(_mm_and_si128(bad, satv), _mm_andnot_si128(bad, iv));
            }
            return iv;
        }
        inline __m128i ftoi4(__m128 f) { return ftoi(_mm_mul_ps(f, _mm_set1_ps(16.0f))); }
        inline uint32_t clipFlags(__m128 v)
        {
            alignas(16) float c[4]; _mm_store_ps(c, v);
            const float w = std::fabs(c[3]);
            uint32_t f = 0;
            if (c[0] > w) f |= 0x01; if (c[0] < -w) f |= 0x02;
            if (c[1] > w) f |= 0x04; if (c[1] < -w) f |= 0x08;
            if (c[2] > w) f |= 0x10; if (c[2] < -w) f |= 0x20;
            return f;
        }
        constexpr uint32_t kFcorMasks[6] = { 0xff7df7u, 0xffbefbu, 0xffdf7du, 0xffefbeu, 0xfdf7dfu, 0xfefbefu };
        inline __m128 ld(const uint8_t *p) { return _mm_loadu_ps(reinterpret_cast<const float *>(p)); }
    }

    // ---- 1627a6cb, the stage / effect tristrip program, whole batch on the host ----------------
    // Listing: ps2xRuntime/tools/vudis.py on games/bt3/work/vu1/vumicro_0b7f264e0237b48e.bin.
    // Constants (per-frame 12-qw upload): M1 = qw0..3 model->screen, M2 = qw4..7 model->clip,
    // M3 = qw8..11 clip->screen (used only by the clipper's fan). The 1-qw setup unpack is the
    // fan's GIF tag (TRIFAN, ST/RGBAQ/XYZF2); the VU stores it at qw30 and rewrites its NLOOP.
    //
    // Per chunk (entry 0xa8/0x228): kick A = hdr qw0..1 (a 2-qw NOP packet); then for each vertex
    // v: ST = T*Q, RGBA = FTOI0(colour), XYZ = FTOI4(M1*P*Q); from the third vertex, if any of the
    // last three clip-space positions is outside a plane (vi04 > 0): FCOR against six masks, all
    // three outside one plane -> ADC on v; otherwise the clipper at 0x3c0 runs on the triangle,
    // emits a fan through M3 as [empty tag][fan tag + n verts], pollutes the CLIP history, and v
    // gets ADC too. Finally kick B = hdr qw2 + count * 3 qw.
    struct Kick { uint32_t off, len; };
    namespace
    {
        struct PV { __m128 pos, col, tex; };
        inline float lane(__m128 v, int i) { alignas(16) float t[4]; _mm_store_ps(t, v); return t[i]; }
        // VU DIV as the JIT does it (vu1_jit_ops.inc): b == 0 gives +max when a >= 0 (including -0)
        // and -max otherwise; the result is clamped.
        inline float vuDiv(float a, float b)
        {
            if (b == 0.0f) return (a >= 0.0f) ? FLT_MAX : -FLT_MAX;
            return vuClampFloat(a / b);
        }
        inline PV lerpPV(const PV &p, const PV &n, float t)
        {
            const __m128 tv = _mm_set1_ps(t);
            PV r;
            r.pos = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(n.pos, p.pos), tv), p.pos);
            r.col = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(n.col, p.col), tv), p.col);
            r.tex = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(n.tex, p.tex), tv), p.tex);
            return r;
        }
        // The six stages in program order: (prev-outside bit, next-outside bit, component, sign).
        struct Stage { uint32_t bp, bn; int comp; float sign; };
        constexpr Stage kStages[6] = { {2048, 32, 2, -1.0f}, {1024, 16, 2, 1.0f}, {128, 2, 0, -1.0f},
                                       {64, 1, 0, 1.0f}, {512, 8, 1, -1.0f}, {256, 4, 1, 1.0f} };

        // Runs the clipper for triangle (a, b, c) = (vf22, vf23, vf24) with their float colours and
        // texcoords, appending the two kicks to out. clip is the 24-bit CLIP history, updated as
        // the VU would. fanTag = the qw30 tag words 1..3. Returns false when nothing was emitted.
        bool clipper(const __m128 *M3, const uint32_t *fanTag, PV tri[3], uint32_t &clip,
                     std::vector<uint8_t> &out, std::vector<Kick> &kicks, std::vector<uint32_t> &stMask)
        {
            const __m128 one = _mm_set1_ps(1.0f);
            // Centroid, then the triangle scaled by 1.005 about it (0x3f80a3d7).
            __m128 acc = _mm_add_ps(tri[0].pos, tri[1].pos);
            __m128 cen = _mm_add_ps(acc, _mm_mul_ps(tri[2].pos, one));
            // The VU's immediates, bit for bit: LOI 0x3eaaaaaa (one third) and LOI 0x3f80a3d7 (1.005).
            float third, scale; { uint32_t u = 0x3eaaaaaau; std::memcpy(&third, &u, 4); u = 0x3f80a3d7u; std::memcpy(&scale, &u, 4); }
            cen = _mm_mul_ps(cen, _mm_set1_ps(third));
            const __m128 sc = _mm_set1_ps(scale);
            PV poly[2][12]; int cnt = 3;
            for (int i = 0; i < 3; ++i)
            {
                const __m128 rel = _mm_sub_ps(tri[i].pos, cen);
                poly[0][i].pos = _mm_add_ps(_mm_mul_ps(cen, one), _mm_mul_ps(rel, sc));
                poly[0][i].col = tri[i].col;
                poly[0][i].tex = tri[i].tex;
            }
            poly[0][3] = poly[0][0];
            int cur = 0;
            for (const Stage &st : kStages)
            {
                PV *in = poly[cur], *o = poly[cur ^ 1];
                int n = 0;
                for (int e = 0; e < cnt; ++e)
                {
                    const PV &prev = in[e], &next = in[e + 1];
                    // ADD vf17 = vf31(0) + vf21: the copy normalises -0 to +0.
                    PV pv{ _mm_add_ps(_mm_setzero_ps(), prev.pos), _mm_add_ps(_mm_setzero_ps(), prev.col), _mm_add_ps(_mm_setzero_ps(), prev.tex) };
                    clip = ((clip << 6) | clipFlags(pv.pos)) & 0xFFFFFFu;
                    clip = ((clip << 6) | clipFlags(next.pos)) & 0xFFFFFFu;
                    const uint32_t fl = clip & 0xFFFu;
                    const bool prevOut = (fl & st.bp) != 0u, nextOut = (fl & st.bn) != 0u;
                    auto intersect = [&]() -> PV {
                        // vf25 = prev - (prev.w * sign) in every lane; vf26 likewise for next.
                        const float dp = lane(pv.pos, 3) * st.sign, dn = lane(next.pos, 3) * st.sign;
                        const __m128 a = _mm_sub_ps(pv.pos, _mm_set1_ps(dp)), b = _mm_sub_ps(next.pos, _mm_set1_ps(dn));
                        const float da = lane(a, st.comp), db = lane(b, st.comp);
                        const float d = db - da;
                        const float t = std::fabs(vuDiv(da, d));
                        return lerpPV(pv, next, t);
                    };
                    if (!prevOut)
                    {
                        if (!nextOut) { if (n < 11) o[n++] = pv; }
                        else { const PV I = intersect(); if (n < 10) { o[n++] = pv; o[n++] = I; } }
                    }
                    else if (!nextOut) { const PV I = intersect(); if (n < 11) o[n++] = I; }
                }
                if (n == 0) return false;
                o[n] = o[0];
                cnt = n; cur ^= 1;
            }
            // Emit: [empty tag: NLOOP 0 EOP] then [fan tag: NLOOP cnt EOP] + cnt * (ST*Q, RGBA, XYZ).
            const PV *fp = poly[cur];
            const size_t base = out.size();
            out.resize(base + 16u + 16u + (size_t)cnt * 48u);
            uint32_t *t0 = reinterpret_cast<uint32_t *>(out.data() + base);
            t0[0] = 0x8000u; t0[1] = fanTag[0]; t0[2] = fanTag[1]; t0[3] = fanTag[2];
            uint32_t *t1 = t0 + 4;
            t1[0] = 0x8000u | (uint32_t)cnt; t1[1] = fanTag[0]; t1[2] = fanTag[1]; t1[3] = fanTag[2];
            kicks.push_back(Kick{(uint32_t)base, 16u});
            kicks.push_back(Kick{(uint32_t)base + 16u, 16u + (uint32_t)cnt * 48u});
            uint8_t *vo = out.data() + base + 32u;
            __m128 vf25 = xformW(M3, fp[0].pos);
            float q = vuDiv(1.0f, lane(vf25, 3));
            for (int i = 0; i < cnt; ++i)
            {
                const __m128 col = fp[i].col, T = fp[i].tex;
                const __m128 Pn = fp[i + 1].pos;
                const __m128 qv = _mm_set1_ps(q);
                const __m128i rgba = ftoi(col);
                const __m128 scr = _mm_mul_ps(vf25, qv);
                const __m128 vf25n = xformW(M3, Pn);
                __m128 st = _mm_mul_ps(T, qv);
                st = _mm_and_ps(st, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
                const __m128i xyz = ftoi4(scr);
                _mm_storeu_ps(reinterpret_cast<float *>(vo + i * 48), st);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(vo + i * 48 + 16), rgba);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(vo + i * 48 + 32), xyz);
                stMask.push_back((uint32_t)(base + 32u + (size_t)i * 48u + 12u));
                vf25 = vf25n;
                q = vuDiv(1.0f, lane(vf25, 3));
            }
            return true;
        }
    }

    // One chunk of a stage list. hdr = the 3-qw header, verts = count*48 bytes of [P, colour, T],
    // consts = VU1 qw0..11 as uploaded per frame, fanTag = words 1..3 of the list's setup qw.
    // out receives every kick in order; kicks their offsets/lengths; stMask the byte offsets of
    // ST w lanes (stale VU register in the real output, zero here). Returns false only on a bad count.
    // [hdronly] PS2X_SEAM_HDRONLY=1 (native renderer alone): the front end draws the recorded chunk itself and only needs the
    // packet's register headers and one kick per pass, so the CPU transform (0.9 us/chunk, 3.7 ms/frame) is skipped: each
    // vertex tag is rewritten to 3 loops of zero vertices (a kick that draws nothing of its own).
    static void hdrOnlyTag(std::vector<uint8_t> &out, const uint8_t *tag16)
    {
        uint64_t lo, hi; std::memcpy(&lo, tag16, 8); std::memcpy(&hi, tag16 + 8, 8);
        uint32_t nreg = (uint32_t)((lo >> 60) & 0xFu); if (nreg == 0u) nreg = 16u;
        lo = (lo & ~0x7FFFull) | 3u;
        const size_t o = out.size(); out.resize(o + 16u + size_t(3u) * nreg * 16u, 0u);
        std::memcpy(out.data() + o, &lo, 8); std::memcpy(out.data() + o + 8, &hi, 8);
    }
    bool hdrOnlyOn() { static const bool s = [](){ const char *v = std::getenv("PS2X_SEAM_HDRONLY"); return v && v[0] && v[0] != '0'; }(); return s; }
    bool stageChunk(const uint8_t *consts, const uint32_t *fanTag, const uint8_t *hdr, const uint8_t *verts, uint32_t nvec,
                    std::vector<uint8_t> &out, std::vector<Kick> &kicks, std::vector<uint32_t> &stMask)
    {
        out.clear(); kicks.clear(); stMask.clear();
        uint32_t countRaw; std::memcpy(&countRaw, hdr + 32, 4);
        const uint32_t count = countRaw & 0x7FFFu;
        if (count == 0u || count > 300u || count * 3u > nvec) return false;
        __m128 M1[4], M2[4], M3[4];
        for (int i = 0; i < 4; ++i) { M1[i] = ld(consts + i * 16); M2[i] = ld(consts + 64 + i * 16); M3[i] = ld(consts + 128 + i * 16); }
        // Kick A: the NOP packet.
        out.insert(out.end(), hdr, hdr + 32);
        kicks.push_back(Kick{0u, 32u});
        // The strip is built in a side buffer and appended last, because clipper kicks come first.
        static thread_local std::vector<uint8_t> strip;
        strip.resize(16u + count * 48u);
        std::memcpy(strip.data(), hdr + 32, 16);
        uint8_t *o = strip.data() + 16;

        __m128 P = ld(verts);
        __m128 vf25 = xformW(M1, P);
        float q = vuDiv(1.0f, lane(vf25, 3));
        __m128 vf22 = _mm_setzero_ps(), vf23 = _mm_setzero_ps(), vf24 = _mm_setzero_ps(), vf20 = P;
        int32_t vi04 = -2, vi05 = -2, vi06 = -2;
        uint32_t clip = 0;
        for (uint32_t v = 0; v < count; ++v)
        {
            vf22 = vf23; vf23 = vf24;
            vf24 = xformW(M2, vf20);
            const __m128 T = ld(verts + v * 48u + 32u);
            const __m128 col = ld(verts + v * 48u + 16u);
            const __m128 Pn = (v + 1 < count) ? ld(verts + (v + 1) * 48u) : vf20;
            clip = ((clip << 6) | clipFlags(vf22)) & 0xFFFFFFu;
            clip = ((clip << 6) | clipFlags(vf23)) & 0xFFFFFFu;
            vi04 = vi05; vi05 = vi06;
            const __m128 qv = _mm_set1_ps(q);
            const __m128 scr = _mm_mul_ps(vf25, qv);
            __m128 st = _mm_mul_ps(T, qv);
            st = _mm_and_ps(st, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
            clip = ((clip << 6) | clipFlags(vf24)) & 0xFFFFFFu;
            const __m128 vf25n = xformW(M1, Pn);
            __m128i xyz = ftoi4(scr);
            vi04 += vi05;
            const __m128i rgba = ftoi(col);
            const int32_t vi01r = (clip & 0x3Fu) != 0u ? 1 : 0;
            vi06 = vi01r;
            vi04 += vi06;
            uint8_t *vo = o + v * 48u;
            _mm_storeu_ps(reinterpret_cast<float *>(vo), st);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vo + 16), rgba);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vo + 32), xyz);   // stored before the branch, as the VU does
            if (vi04 > 0)
            {
                bool reject = false;
                for (uint32_t m : kFcorMasks) if (((clip | m) & 0xFFFFFFu) == 0xFFFFFFu) { reject = true; break; }
                if (!reject)
                {
                    // The clipper reads the three float colours back from the strip output (ITOF0 of
                    // the stored ints) and the three texcoords from the vertex data.
                    PV tri[3];
                    const __m128 pos[3] = { vf22, vf23, vf24 };
                    for (int k = 0; k < 3; ++k)
                    {
                        const uint32_t vk = v - 2u + (uint32_t)k;
                        tri[k].pos = pos[k];
                        tri[k].col = _mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i *>(o + vk * 48u + 16u)));
                        tri[k].tex = ld(verts + vk * 48u + 32u);
                    }
                    clipper(M3, fanTag, tri, clip, out, kicks, stMask);
                }
                std::memcpy(vo + 32 + 12, "\x00\x80\x00\x00", 4);   // ADC: the strip drops this triangle
            }
            vf25 = vf25n; vf20 = Pn;
            q = vuDiv(1.0f, lane(vf25, 3));
        }
        // Kick B: the strip.
        const size_t base = out.size();
        out.insert(out.end(), strip.begin(), strip.end());
        kicks.push_back(Kick{(uint32_t)base, (uint32_t)strip.size()});
        for (uint32_t v = 0; v < count; ++v) stMask.push_back((uint32_t)(base + 16u + v * 48u + 12u));
        return true;
    }
}

namespace seamxform
{
    // ---- character programs ------------------------------------------------------------------
    // Both read the batch's constant block (qw0..) plus, for 925edd7c, the per-frame camera at
    // qw11..18. Loops mirror the native kernels; the setup runs are folded into charConsts.
    namespace
    {
        // MULAx ACC = r0*v.x ; MADDAy ; MADDAz ; MADDw out = ACC + r3 * vf00.w (= 1): the form the
        // character loops use for positions, normals and the projections.
        inline __m128 xformAcc1(const __m128 *r, __m128 v)
        {
            __m128 acc = _mm_mul_ps(r[0], bc(v, 0));
            acc = _mm_add_ps(acc, _mm_mul_ps(r[1], bc(v, 1)));
            acc = _mm_add_ps(acc, _mm_mul_ps(r[2], bc(v, 2)));
            return _mm_add_ps(acc, _mm_mul_ps(r[3], _mm_set1_ps(1.0f)));
        }
        inline __m128 xyzOf(__m128 a, __m128 b)
        {
            const __m128 m = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
            return _mm_or_ps(_mm_and_ps(m, b), _mm_andnot_ps(m, a));
        }
        inline float recipQ(__m128 v) { const float w = lane(v, 3); return (w != 0.0f) ? vuClampFloat(1.0f / w) : FLT_MAX; }
    }

    struct CharConsts
    {
        __m128 A[4], B[4], C[4], D[4], E[4], F[4];
        __m128 pivA, pivB;
        __m128i rgbaA, rgbaB;   // 3b5dfe97: FTOI0(qw22), FTOI0(qw23). 925edd7c: rgbaA = FTOI0(qw10)
    };
    // vu = VU1 data memory at MSCALF time (the block just unpacked). twoPass selects the program.
    void charConsts(const uint8_t *vu, bool twoPass, CharConsts &c)
    {
        // The setup runs execute under VU1Interpreter::execute, which rounds toward zero with
        // FTZ/DAZ (VuRoundScope, PS2X_VUROUND); resume (the chunk loops) does not. Match it here.
        struct RzScope
        {
            uint32_t saved; bool active;
            RzScope() { static const bool s_on = [](){ const char *v = std::getenv("PS2X_VUROUND"); return !(v && v[0] == '0'); }();
                        active = s_on; saved = _mm_getcsr(); if (active) _mm_setcsr((saved & ~0xE040u) | 0x6000u | 0x8040u); }
            ~RzScope() { if (active) _mm_setcsr(saved); }
        } rz;
        for (int i = 0; i < 4; ++i) { c.A[i] = ld(vu + i * 16); c.B[i] = ld(vu + 64 + i * 16); }
        c.pivA = ld(vu + 8 * 16); c.pivB = ld(vu + 9 * 16);
        if (twoPass)
        {
            __m128 L[4];
            for (int i = 0; i < 4; ++i) { L[i] = ld(vu + (10 + i) * 16); c.E[i] = ld(vu + (14 + i) * 16); c.F[i] = ld(vu + (18 + i) * 16); }
            // Setup run: C rows 0..2 = L applied to A rows 0..2 (with the row's own w), row 3 = L row 3.
            for (int i = 0; i < 3; ++i) { c.C[i] = xformW(L, c.A[i]); c.D[i] = xformW(L, c.B[i]); }
            c.C[3] = L[3]; c.D[3] = L[3];
            c.rgbaA = ftoi(ld(vu + 22 * 16)); c.rgbaB = ftoi(ld(vu + 23 * 16));
        }
        else
        {
            for (int i = 0; i < 4; ++i) { c.E[i] = ld(vu + (11 + i) * 16); c.F[i] = ld(vu + (15 + i) * 16); }
            c.rgbaA = ftoi(ld(vu + 10 * 16)); c.rgbaB = _mm_setzero_si128();
        }
    }

    // 3b5dfe97: two-bone skinned strip, textured pass then toon pass, one kick of
    // [hdr0, hdr1, hdr3, A verts][hdr0, hdr2, hdr4, B verts]. clip is the CLIP history in/out.
    bool charChunk2(const CharConsts &c, const uint8_t *hdr, const uint8_t *verts, uint32_t nvec,
                    uint32_t &clip, std::vector<uint8_t> &out, std::vector<uint32_t> &stMask)
    {
        out.clear(); stMask.clear();
        uint32_t countRaw; std::memcpy(&countRaw, hdr + 48, 4);
        const uint32_t count = countRaw & 0x7FFFu;
        if (count == 0u || count > 300u || count * 3u > nvec) return false;
        const size_t passBytes = 48u + (size_t)count * 48u;
        out.resize(passBytes * 2u);
        std::memcpy(out.data(), hdr, 32); std::memcpy(out.data() + 32, hdr + 48, 16);
        std::memcpy(out.data() + passBytes, hdr, 16); std::memcpy(out.data() + passBytes + 16, hdr + 32, 16); std::memcpy(out.data() + passBytes + 32, hdr + 64, 16);
        uint8_t *oa = out.data() + 48, *ob = out.data() + passBytes + 48;
        const __m128 half = _mm_set1_ps(0.5f);
        __m128 P = ld(verts);
        __m128 vf28 = xformAcc1(c.A, _mm_sub_ps(P, c.pivA)), vf29 = _mm_sub_ps(P, c.pivB);
        for (uint32_t v = 0; v < count; ++v)
        {
            const __m128 N = ld(verts + v * 48u + 16u), T = ld(verts + v * 48u + 32u);
            vf29 = xformAcc1(c.B, vf29);
            __m128 vf26 = xformAcc1(c.C, N);
            __m128 vf20 = _mm_sub_ps(vf28, vf29);
            const __m128 vf27 = xformAcc1(c.D, N);
            vf20 = _mm_mul_ps(vf20, bc(P, 3));
            vf20 = _mm_add_ps(vf29, vf20);
            vf26 = _mm_sub_ps(vf26, vf27);
            __m128 vf24 = xformAcc1(c.E, vf20);
            vf26 = _mm_mul_ps(vf26, bc(P, 3));
            const __m128 vf25 = xformAcc1(c.F, vf20);
            const float q = recipQ(vf24);
            vf26 = _mm_add_ps(vf27, vf26);
            clip = ((clip << 6) | clipFlags(vf25)) & 0xFFFFFFu;
            const __m128 qv = _mm_set1_ps(q);
            vf26 = xyzOf(vf26, _mm_mul_ps(vf26, half));
            vf24 = _mm_mul_ps(vf24, qv);
            const __m128 vf17 = xyzOf(T, _mm_mul_ps(T, qv));
            vf26 = xyzOf(vf26, _mm_add_ps(vf26, half));
            __m128i xyz = ftoi4(vf24);
            const int vi01 = (clip & 0x3FFFFu) != 0u ? 1 : 0;
            __m128 vf22 = _mm_set_ps(0.0f, 0.0f + 1.0f, 0.0f + 0.0f, 0.0f + lane(vf26, 0));
            vf22 = xyzOf(vf22, _mm_mul_ps(vf22, qv));
            if (vi01) { alignas(16) uint32_t xi[4]; _mm_store_si128(reinterpret_cast<__m128i *>(xi), xyz); xi[3] = 0x8000u; xyz = _mm_load_si128(reinterpret_cast<const __m128i *>(xi)); }
            uint8_t *a = oa + v * 48u, *b = ob + v * 48u;
            _mm_storeu_ps(reinterpret_cast<float *>(a), vf17); _mm_storeu_si128(reinterpret_cast<__m128i *>(a + 16), c.rgbaA); _mm_storeu_si128(reinterpret_cast<__m128i *>(a + 32), xyz);
            _mm_storeu_ps(reinterpret_cast<float *>(b), vf22); _mm_storeu_si128(reinterpret_cast<__m128i *>(b + 16), c.rgbaB); _mm_storeu_si128(reinterpret_cast<__m128i *>(b + 32), xyz);
            stMask.push_back((uint32_t)(passBytes + 48u + v * 48u + 12u));   // vf22.w: never written in the loop
            // Tail: the next vertex's A-transform.
            const __m128 Pn = (v + 1 < count) ? ld(verts + (v + 1) * 48u) : P;
            P = Pn;
            vf28 = xformAcc1(c.A, _mm_sub_ps(P, c.pivA));
            vf29 = _mm_sub_ps(P, c.pivB);
        }
        return true;
    }

    // 925edd7c: single-pass skinned strip, one kick of [hdr0, hdr1, hdr4, verts].
    bool charChunk1(const CharConsts &c, const uint8_t *hdr, const uint8_t *verts, uint32_t nvec,
                    uint32_t &clip, std::vector<uint8_t> &out)
    {
        out.clear();
        uint32_t countRaw; std::memcpy(&countRaw, hdr + 64, 4);
        const uint32_t count = countRaw & 0x7FFFu;
        if (count == 0u || count > 300u || count * 3u > nvec) return false;
        out.resize(48u + (size_t)count * 48u);
        std::memcpy(out.data(), hdr, 32); std::memcpy(out.data() + 32, hdr + 64, 16);
        uint8_t *o = out.data() + 48;
        __m128 P = ld(verts);
        __m128 vf28 = xformAcc1(c.A, _mm_sub_ps(P, c.pivA)), vf29 = _mm_sub_ps(P, c.pivB);
        for (uint32_t v = 0; v < count; ++v)
        {
            const __m128 T = ld(verts + v * 48u + 32u);
            vf29 = xformAcc1(c.B, vf29);
            __m128 vf20 = _mm_sub_ps(vf28, vf29);
            vf20 = _mm_mul_ps(vf20, bc(P, 3));
            vf20 = _mm_add_ps(vf29, vf20);
            __m128 vf24 = xformAcc1(c.E, vf20);
            const __m128 vf25 = xformAcc1(c.F, vf20);
            const float q = recipQ(vf24);
            clip = ((clip << 6) | clipFlags(vf25)) & 0xFFFFFFu;
            const __m128 qv = _mm_set1_ps(q);
            vf24 = _mm_mul_ps(vf24, qv);
            const __m128 vf22 = xyzOf(T, _mm_mul_ps(T, qv));
            (void)((clip & 0x3FFFFu) != 0u);   // FCAND result: this program never marks ADC (the branch skips a NOP)
            const __m128i xyz = ftoi4(vf24);
            uint8_t *a = o + v * 48u;
            _mm_storeu_ps(reinterpret_cast<float *>(a), vf22); _mm_storeu_si128(reinterpret_cast<__m128i *>(a + 16), c.rgbaA); _mm_storeu_si128(reinterpret_cast<__m128i *>(a + 32), xyz);
            const __m128 Pn = (v + 1 < count) ? ld(verts + (v + 1) * 48u) : P;
            P = Pn;
            vf28 = xformAcc1(c.A, _mm_sub_ps(P, c.pivA));
            vf29 = _mm_sub_ps(P, c.pivB);
        }
        return true;
    }
}

namespace seamxform
{
    // ---- 4d070cb1, the effects tristrip program ---------------------------------------------
    // The stage program with: 4 qw per vertex [P (w = int flag), unused, colour, T], texcoords
    // generated per vertex from M4 = qw12..15 scaled by qw16.x (T = ((M4*P).xy*s + 1) * 0.5, z = 1),
    // written back into the vertex's T slot (the clipper reads them from there), a per-vertex flag
    // in P.w that marks ADC when no clip flag did, no NOP-packet kick before the loop, and the
    // same clipper and fan (M3 = qw8..11) with ADC on every fan vertex when the flag is set.
    bool effectsChunk(const uint8_t *consts, const uint32_t *fanTag, const uint8_t *hdr, const uint8_t *verts, uint32_t nvec,
                      uint32_t &clip, std::vector<uint8_t> &out, std::vector<Kick> &kicks, std::vector<uint32_t> &stMask, uint32_t variant)
    {
        const bool vM2w = variant & 1u, vM1w = variant & 2u, vClip0 = variant & 4u, vNoFlagAdc = variant & 8u;
        if (vClip0) clip = 0;
        out.clear(); kicks.clear(); stMask.clear();
        uint32_t countRaw; std::memcpy(&countRaw, hdr + 32, 4);
        const uint32_t count = countRaw & 0x7FFFu;
        if (count == 0u || count > 300u || count * 4u > nvec) return false;
        __m128 M1[4], M2[4], M3[4], M4[4];
        for (int i = 0; i < 4; ++i) { M1[i] = ld(consts + i * 16); M2[i] = ld(consts + 64 + i * 16); M3[i] = ld(consts + 128 + i * 16); M4[i] = ld(consts + 192 + i * 16); }
        const __m128 qw16 = ld(consts + 256);
        static thread_local std::vector<uint8_t> strip;
        static thread_local std::vector<__m128> texs;
        strip.resize(16u + count * 48u); texs.resize(count);
        std::memcpy(strip.data(), hdr + 32, 16);
        uint8_t *o = strip.data() + 16;
        const __m128 half = _mm_set1_ps(0.5f);

        __m128 P = ld(verts);
        __m128 vf25 = vM1w ? xformW(M1, P) : xformAcc1(M1, P);
        float q = vuDiv(1.0f, lane(vf25, 3));
        __m128 vf22 = _mm_setzero_ps(), vf23 = _mm_setzero_ps(), vf24 = _mm_setzero_ps(), vf20 = P;
        int32_t vi04 = -2, vi05 = -2, vi06 = -2;
        auto flagOf = [&](uint32_t v) -> int32_t { int32_t f; std::memcpy(&f, verts + v * 64u + 12u, 4); return (int16_t)f; };
        int32_t vi13 = flagOf(0);
        for (uint32_t v = 0; v < count; ++v)
        {
            vf22 = vf23; vf23 = vf24;
            vf24 = vM2w ? xformW(M2, vf20) : xformAcc1(M2, vf20);
            const __m128 Traw = ld(verts + v * 64u + 48u);
            const __m128 col = ld(verts + v * 64u + 32u);
            clip = ((clip << 6) | clipFlags(vf22)) & 0xFFFFFFu;
            const __m128 qv = _mm_set1_ps(q);
            const __m128 scr = _mm_mul_ps(vf25, qv);
            vi04 = vi05; vi05 = vi06;
            // Generated texcoords.
            __m128 vf09 = xformAcc1(M4, vf20);
            __m128 vf19 = _mm_mul_ps(Traw, _mm_setzero_ps());                       // MULx vf19 = T * vf00.x
            { const __m128 sx = bc(qw16, 0); const __m128 m = _mm_mul_ps(vf09, sx);   // MULx.xy vf09 *= qw16.x
              vf09 = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(_mm_set_epi32(0, 0, -1, -1), _mm_castps_si128(m)), _mm_andnot_si128(_mm_set_epi32(0, 0, -1, -1), _mm_castps_si128(vf09)))); }
            alignas(16) float t19[4], t09[4]; _mm_store_ps(t19, vf19); _mm_store_ps(t09, vf09);
            t19[0] = t09[0] + 0.0f;                                                  // ADDx.x vf19 = vf09.x + vf00.x
            t19[1] = t19[1] + t09[1];                                                // ADDy.y vf19 = vf19.y + vf09.y
            t19[0] += 1.0f; t19[1] += 1.0f; t19[2] += 1.0f;                          // ADDw.xyz vf19 += vf00.w
            t19[0] *= 0.5f; t19[1] *= 0.5f;                                          // MULi.xy
            vf19 = _mm_load_ps(t19);
            texs[v] = vf19;
            __m128 st = _mm_mul_ps(vf19, qv);
            st = xyzOf(qw16, st);                                                    // MULq.xyz vf28 = vf19*Q, w = qw16.w
            clip = ((clip << 6) | clipFlags(vf23)) & 0xFFFFFFu;
            clip = ((clip << 6) | clipFlags(vf24)) & 0xFFFFFFu;
            __m128i xyz = ftoi4(scr);
            const __m128 Pn = (v + 1 < count) ? ld(verts + (v + 1) * 64u) : vf20;
            const __m128 vf25n = vM1w ? xformW(M1, Pn) : xformAcc1(M1, Pn);
            const __m128i rgba = ftoi(col);
            const int32_t vi01r = (clip & 0x3Fu) != 0u ? 1 : 0;
            vi06 = vi01r;
            vi04 += vi05;
            vi04 += vi06;
            uint8_t *vo = o + v * 48u;
            _mm_storeu_ps(reinterpret_cast<float *>(vo), st);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vo + 16), rgba);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vo + 32), xyz);
            bool adc = false;
            if (vi04 > 0)
            {
                bool reject = false;
                for (uint32_t m : kFcorMasks) if (((clip | m) & 0xFFFFFFu) == 0xFFFFFFu) { reject = true; break; }
                if (reject) { vi04 += vi13; adc = true; }   // the reject path also adds the flag; vi04 > 0 already
                else
                {
                    PV tri[3];
                    const __m128 pos[3] = { vf22, vf23, vf24 };
                    for (int k = 0; k < 3; ++k)
                    {
                        const uint32_t vk = v - 2u + (uint32_t)k;
                        tri[k].pos = pos[k];
                        tri[k].col = _mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i *>(o + vk * 48u + 16u)));
                        tri[k].tex = texs[vk];
                    }
                    const size_t before = kicks.size();
                    clipper(M3, fanTag, tri, clip, out, kicks, stMask);
                    if (kicks.size() > before && vi13 != 0)
                    {   // fan vertices get ADC when the current vertex's flag is set
                        const Kick &fk = kicks.back();
                        for (uint32_t off = fk.off + 16u + 32u + 12u; off + 4u <= fk.off + fk.len; off += 48u)
                        { uint32_t w = 0x8000u; std::memcpy(out.data() + off, &w, 4); }
                    }
                    adc = true;
                }
            }
            else
            {
                if (!vNoFlagAdc) vi04 += vi13;
                if (vi04 > 0) adc = true;
            }
            vi13 = (v + 1 < count) ? flagOf(v + 1) : vi13;
            if (adc) std::memcpy(vo + 32 + 12, "\x00\x80\x00\x00", 4);
            vf25 = vf25n; vf20 = Pn;
            q = vuDiv(1.0f, lane(vf25, 3));
        }
        const size_t base = out.size();
        out.insert(out.end(), strip.begin(), strip.end());
        kicks.push_back(Kick{(uint32_t)base, (uint32_t)strip.size()});
        return true;
    }

    // ---- ccb6aa07: two-pass skinned strip with a 2D toon lookup ------------------------------
    struct Char3Consts
    {
        __m128 A[4], B[4], L[4], E[4], F[4];
        __m128 pivA, pivB, tex26;
        __m128i rgbaA, rgbaB;
    };
    void char3Consts(const uint8_t *vu, Char3Consts &c)
    {
        for (int i = 0; i < 4; ++i) { c.A[i] = ld(vu + i * 16); c.B[i] = ld(vu + 64 + i * 16); c.L[i] = ld(vu + (10 + i) * 16); c.E[i] = ld(vu + (14 + i) * 16); c.F[i] = ld(vu + (18 + i) * 16); }
        c.pivA = ld(vu + 8 * 16); c.pivB = ld(vu + 9 * 16); c.tex26 = ld(vu + 26 * 16);
        c.rgbaA = ftoi(ld(vu + 22 * 16)); c.rgbaB = ftoi(ld(vu + 23 * 16));
    }
    // Returns the number of kicks appended (1). Empty chunks (first normal's w bits zero) emit a
    // 16-byte tag whose words 1..3 are whatever the output area held; those are masked.
    bool charChunk3(const Char3Consts &c, const uint8_t *hdr, const uint8_t *verts, uint32_t nvec,
                    uint32_t &clip, std::vector<uint8_t> &out, std::vector<uint32_t> &stMask, std::vector<uint32_t> &dontCare)
    {
        out.clear(); stMask.clear(); dontCare.clear();
        uint32_t n0w; std::memcpy(&n0w, verts + 16 + 12, 4);
        if (n0w == 0u)
        {
            out.resize(16);
            uint32_t *w = reinterpret_cast<uint32_t *>(out.data());
            w[0] = 0x8000u; w[1] = 0x10000000u; w[2] = 0xEu; w[3] = 0u;   // an A+D tag's tail is what sits there in practice
            dontCare.push_back(4); dontCare.push_back(8); dontCare.push_back(12);
            return true;
        }
        uint32_t countRaw; std::memcpy(&countRaw, hdr + 48, 4);
        const uint32_t count = countRaw & 0x7FFFu;
        if (count == 0u || count > 300u || count * 3u > nvec) return false;
        const size_t passBytes = 48u + (size_t)count * 48u;
        out.resize(passBytes * 2u);
        std::memcpy(out.data(), hdr, 32); std::memcpy(out.data() + 32, hdr + 48, 16);
        std::memcpy(out.data() + passBytes, hdr, 16); _mm_storeu_ps(reinterpret_cast<float *>(out.data() + passBytes + 16), c.tex26); std::memcpy(out.data() + passBytes + 32, hdr + 64, 16);
        uint8_t *oa = out.data() + 48, *ob = out.data() + passBytes + 48;
        __m128 P = ld(verts);
        __m128 vf28 = xformAcc1(c.A, _mm_sub_ps(P, c.pivA)), vf29 = _mm_sub_ps(P, c.pivB);
        for (uint32_t v = 0; v < count; ++v)
        {
            const __m128 N = ld(verts + v * 48u + 16u), T = ld(verts + v * 48u + 32u);
            vf29 = xformAcc1(c.B, vf29);
            const __m128 vf26 = xformAcc1(c.L, N);
            __m128 vf20 = _mm_sub_ps(vf28, vf29);
            vf20 = _mm_mul_ps(vf20, bc(P, 3));
            // vf13 = vf18 = (1, 1, 1, .); x += lit.x ; y += lit.y ; xy *= 0.5
            alignas(16) float l[4]; _mm_store_ps(l, vf26);
            float t13x = 1.0f + l[0], t13y = 1.0f + l[1];
            t13x *= 0.5f; t13y *= 0.5f;
            vf20 = _mm_add_ps(vf29, vf20);
            __m128 vf24 = xformAcc1(c.E, vf20);
            const __m128 vf25 = xformAcc1(c.F, vf20);
            const float q = recipQ(vf24);
            clip = ((clip << 6) | clipFlags(vf25)) & 0xFFFFFFu;
            const __m128 qv = _mm_set1_ps(q);
            vf24 = _mm_mul_ps(vf24, qv);
            const __m128 vf17 = xyzOf(T, _mm_mul_ps(T, qv));
            const __m128 vf13 = _mm_set_ps(0.0f, 1.0f, t13y, t13x);
            const __m128 vf22 = _mm_and_ps(_mm_mul_ps(vf13, qv), _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
            __m128i xyz = ftoi4(vf24);
            const int vi01 = (clip & 0x3FFFFu) != 0u ? 1 : 0;
            if (vi01) { alignas(16) uint32_t xi[4]; _mm_store_si128(reinterpret_cast<__m128i *>(xi), xyz); xi[3] = 0x8000u; xyz = _mm_load_si128(reinterpret_cast<const __m128i *>(xi)); }
            uint8_t *a = oa + v * 48u, *b = ob + v * 48u;
            _mm_storeu_ps(reinterpret_cast<float *>(a), vf17); _mm_storeu_si128(reinterpret_cast<__m128i *>(a + 16), c.rgbaA); _mm_storeu_si128(reinterpret_cast<__m128i *>(a + 32), xyz);
            _mm_storeu_ps(reinterpret_cast<float *>(b), vf22); _mm_storeu_si128(reinterpret_cast<__m128i *>(b + 16), c.rgbaB); _mm_storeu_si128(reinterpret_cast<__m128i *>(b + 32), xyz);
            stMask.push_back((uint32_t)(passBytes + 48u + v * 48u + 12u));
            const __m128 Pn = (v + 1 < count) ? ld(verts + (v + 1) * 48u) : P;
            P = Pn;
            vf28 = xformAcc1(c.A, _mm_sub_ps(P, c.pivA));
            vf29 = _mm_sub_ps(P, c.pivB);
        }
        return true;
    }
}

// ============================================================================================
// Phase 3: verify / skip
// ============================================================================================
namespace seam
{
    namespace
    {
        constexpr uint32_t kStageBuilder = 0x00123588u;
        constexpr uint32_t kChar2Builder = 0x00123278u;   // 3b5dfe97
        constexpr uint32_t kChar1Builder = 0x00123468u;   // 925edd7c
        constexpr uint32_t kChar3Builder = 0x00123370u;   // ccb6aa07
        constexpr uint32_t kFxBuilder    = 0x00123dc8u;   // 4d070cb1
        thread_local seamxform::Char3Consts t_c3;
        thread_local uint32_t t_hostClip = 0;             // the CLIP history a program starting now would see
        thread_local seamxform::CharConsts t_cc;
        thread_local uint32_t t_builder = 0;
        std::mutex g_skipMtx;
        std::deque<uint32_t> g_skipFifo;      // lists whose CALL the walker skipped, in stream order
        bool keepCall() { static const bool s = [](){ const char *v = std::getenv("PS2X_SEAMKEEPCALL"); return v && v[0] && v[0] != '0'; }(); return s; }
        bool ownedBuilder(uint32_t b) { return b == kStageBuilder || b == kChar2Builder || b == kChar1Builder || b == kChar3Builder || b == kFxBuilder; }
        thread_local uint8_t t_stageConsts[17 * 16];   // effects: qw0..16 at MSCALF time (per-frame upload)
        thread_local const seammesh::Chunk *t_fxChunk = nullptr;
        thread_local uint8_t t_liveVu[17 * 16];
        thread_local uint32_t t_clipIn = 0;
        extern "C" uint32_t ps2xSeamVu1Clip();
        extern "C" void ps2xSeamVu1Q(float *q, float *pendingQ, uint32_t *qWait);
        thread_local float t_vuQ = 0, t_vuPendingQ = 0; thread_local uint32_t t_vuQWait = 0;

        struct Run
        {
            bool active = false;       // the host produced a packet for this run
            bool owned = false;        // skip mode: VU1 was not run
            std::vector<uint8_t> expect;
            std::vector<seamxform::Kick> kicks;
            std::vector<uint32_t> stMask;
            std::vector<uint8_t> actual;
            uint32_t clipOut = 0; bool hasClipOut = false;
            std::vector<uint32_t> dontCare;   // byte offsets never compared
            std::vector<uint32_t> actualKicks;
        };
        thread_local Run t_run;
        thread_local uint32_t t_chunk = 0;         // chunk index within the current stage batch
        thread_local const seammesh::Mesh *t_mesh = nullptr;

        struct Stats
        {
            uint64_t runs = 0, match = 0, mismatch = 0, clipper = 0, noMesh = 0, chunkOOB = 0, vertMismatch = 0, skipped = 0;
            int printed = 0;
        };
        Stats g_st;
        std::mutex g_mtx;
        std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
    }

    bool verifyOn() { static const bool s = [](){ const char *v = std::getenv("PS2X_SEAMVERIFY"); return v && v[0] && v[0] != '0'; }(); return s; }
    // [seamskipdefault] Under the native renderer the host-mesh path is the default: VU1 runs nothing in a fight, the meshes go
    // to the GPU vertex programs (effects from their exact packet vertices, [fxpkt]) and the front end skips the packets' vertex
    // walk ([hostskip]). PS2X_SEAMSKIP=0 restores the kernel + GIF-parse path for A/B; without the native renderer it stays opt-in.
    bool skipOn()   { static const bool s = [](){ const char *v = std::getenv("PS2X_SEAMSKIP");   return v && v[0] ? v[0] != '0' : seamvk::on(); }(); return s; }
    bool on()       { return verifyOn() || skipOn(); }

    // Emits one chunk of the current batch: fills t_run, and in skip mode submits it.
    static void afterRunOwned();
    // [seamvk] hand the chunk to the native renderer with the program's constants laid out for the shader.
#if defined(PS2X_HAVE_SEAMVK)
    static void recordForVk(const seammesh::Chunk &c, bool isStage, bool isFx, bool isChar2, bool isChar1, const uint8_t *vuData, PS2Memory *mem)
    {
        seamvk::DrawPacket k; std::memset(&k, 0, sizeof(k));
        k.magic = seamvk::kDrawMagic;
        uint32_t stride = 48, count = 0; uint8_t prog = 0; uint32_t tagOff = 32;
        auto row = [](float (*dst)[4], const __m128 *m) { for (int i = 0; i < 4; ++i) _mm_storeu_ps(dst[i], m[i]); };
        auto rowb = [](float (*dst)[4], const uint8_t *m) { std::memcpy(dst, m, 64); };
        if (isStage || isFx)
        {
            const uint8_t *cs = isStage ? vuData : t_stageConsts;
            rowb(k.c.A, cs); rowb(k.c.B, cs + 64);
            if (isFx) { rowb(k.c.C, cs + 192); std::memcpy(&k.c.misc[2], cs + 256, 4); stride = 64; prog = 1; }
            else prog = 0;
            tagOff = 32;
        }
        else if (isChar2 || isChar1)
        {
            row(k.c.A, t_cc.A); row(k.c.B, t_cc.B); row(k.c.E, t_cc.E); row(k.c.F, t_cc.F); row(k.c.C, t_cc.C); row(k.c.D, t_cc.D);
            _mm_storeu_ps(k.c.pivA, t_cc.pivA); _mm_storeu_ps(k.c.pivB, t_cc.pivB);
            _mm_storeu_ps(k.c.colA, _mm_cvtepi32_ps(t_cc.rgbaA)); _mm_storeu_ps(k.c.colB, _mm_cvtepi32_ps(t_cc.rgbaB));
            tagOff = isChar2 ? 48 : 64;
            prog = isChar2 ? 2 : 3;
        }
        else
        {
            row(k.c.A, t_c3.A); row(k.c.B, t_c3.B); row(k.c.E, t_c3.E); row(k.c.F, t_c3.F); row(k.c.D, t_c3.L);
            _mm_storeu_ps(k.c.pivA, t_c3.pivA); _mm_storeu_ps(k.c.pivB, t_c3.pivB);
            _mm_storeu_ps(k.c.colA, _mm_cvtepi32_ps(t_c3.rgbaA)); _mm_storeu_ps(k.c.colB, _mm_cvtepi32_ps(t_c3.rgbaB));
            tagOff = 48;
            prog = 4;
        }
        uint32_t countRaw, tagW1; std::memcpy(&countRaw, c.hdr + tagOff, 4); std::memcpy(&tagW1, c.hdr + tagOff + 4, 4);
        count = countRaw & 0x7FFFu;
        k.prog = prog; k.stride = stride; k.count = count; k.gifTagWord1 = tagW1;
        k.c.misc[0] = float(prog); k.c.misc[1] = float(stride);
        if (count < 3u || count * stride > c.nvec * 16u || !mem) return;
        static thread_local std::vector<uint8_t> buf;
        buf.resize(sizeof(k) + size_t(count) * stride);
        std::memcpy(buf.data(), &k, sizeof(k));
        std::memcpy(buf.data() + sizeof(k), c.verts.data(), size_t(count) * stride);
        mem->submitGifPacket(GifPathId::HostDraw, buf.data(), (uint32_t)buf.size());
    }
#else
    static void recordForVk(const seammesh::Chunk &, bool, bool, bool, bool, const uint8_t *, PS2Memory *) {}
#endif
    static bool emitChunk(uint32_t chunkIdx, uint32_t top, uint8_t *vuData, uint32_t dataSize, void *memory, bool verifyCheck)
    {
        g_seamHostChunks.fetch_add(1u, std::memory_order_relaxed);   // [fightgate] the seam's meshes count as the fight's render work (its VU1 programs are skipped)
        std::lock_guard<std::mutex> lk(g_mtx);
        if (chunkIdx >= t_mesh->chunks.size()) { ++g_st.chunkOOB; return false; }
        const bool isStage = t_builder == kStageBuilder, isChar2 = t_builder == kChar2Builder, isChar1 = t_builder == kChar1Builder, isFx = t_builder == kFxBuilder;
        const bool stageLike = isStage || isFx;
        const seammesh::Chunk &c = t_mesh->chunks[chunkIdx];
        if (stageLike ? (c.hdrQw != 3u || !t_mesh->hasSetup) : (c.hdrQw != 5u)) return false;
        if (verifyCheck && verifyOn() && isChar2 && chunkIdx == 0u)
        {   // The derived C/D against what the VU's setup run wrote at qw26..33.
            static int s_cd = 0;
            for (int i = 0; i < 4 && s_cd < 6; ++i)
            {
                alignas(16) float hc[4], hd[4]; _mm_store_ps(hc, t_cc.C[i]); _mm_store_ps(hd, t_cc.D[i]);
                float vc[4], vd[4]; std::memcpy(vc, vuData + (26 + i) * 16, 16); std::memcpy(vd, vuData + (30 + i) * 16, 16);
                if (std::memcmp(hc, vc, 16) || std::memcmp(hd, vd, 16))
                {
                    ++s_cd;
                    std::fprintf(stderr, "[seamcd] row %d C host %a %a %a %a vu %a %a %a %a | D host %a %a %a %a vu %a %a %a %a\n", i,
                                 hc[0], hc[1], hc[2], hc[3], vc[0], vc[1], vc[2], vc[3], hd[0], hd[1], hd[2], hd[3], vd[0], vd[1], vd[2], vd[3]);
                }
            }
        }
        if (verifyCheck && verifyOn())
        {   // Cross-check the import against what VIF landed in VU1 memory.
            const uint32_t t = top & 0x3FFu;
            const uint32_t hdrOff = (t * 16u) & (dataSize - 1u), vOff = ((t + c.hdrQw) * 16u) & (dataSize - 1u);
            if (hdrOff + c.hdrQw * 16u <= dataSize && std::memcmp(vuData + hdrOff, c.hdr, c.hdrQw * 16u) != 0) ++g_st.vertMismatch;
            else if (vOff + c.nvec * 16u <= dataSize && std::memcmp(vuData + vOff, c.verts.data(), c.nvec * 16u) != 0) ++g_st.vertMismatch;
        }
        bool ok;
        uint32_t clip = t_hostClip; t_clipIn = clip;
        // [xformprof] the CPU chunk transform's share of the kick worker (PS2X_SEAMXFORM_PROF=1 prints per 4096 chunks)
        static const bool s_xprof = [](){ const char *v = std::getenv("PS2X_SEAMXFORM_PROF"); return v && v[0] && v[0] != '0'; }();
        static double s_xms = 0; static uint32_t s_xn = 0;
        struct XT { bool on; std::chrono::steady_clock::time_point t; ~XT() { if (!on) return; s_xms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); if (++s_xn == 4096u) { std::fprintf(stderr, "[seamxform] %.3f ms per 4096 chunks (%.1f us/chunk)\n", s_xms, s_xms * 1000.0 / 4096.0); s_xms = 0; s_xn = 0; } } } xt{s_xprof, std::chrono::steady_clock::now()};
        if (seamxform::hdrOnlyOn() && seamvk::on())
        {   // [hdronly] headers + one empty kick per pass, no transform
            std::vector<uint8_t> &o = t_run.expect; o.clear(); t_run.kicks.clear(); t_run.stMask.clear(); t_run.dontCare.clear();
            uint32_t countRaw; std::memcpy(&countRaw, c.hdr + (isStage || isFx ? 32 : isChar1 ? 64 : 48), 4);
            const uint32_t count = countRaw & 0x7FFFu;
            ok = count != 0u && count <= 300u;
            if (ok && isStage)
            {
                o.insert(o.end(), c.hdr, c.hdr + 32); t_run.kicks.push_back(seamxform::Kick{0u, 32u});
                seamxform::hdrOnlyTag(o, c.hdr + 32); t_run.kicks.push_back(seamxform::Kick{32u, (uint32_t)o.size() - 32u});
            }
            else if (ok && isFx)
            {
                std::memcpy(t_liveVu, vuData, sizeof(t_liveVu));
                ps2xSeamVu1Q(&t_vuQ, &t_vuPendingQ, &t_vuQWait);
                t_fxChunk = &c;
                seamxform::hdrOnlyTag(o, c.hdr + 32); t_run.kicks.push_back(seamxform::Kick{0u, (uint32_t)o.size()});
            }
            else if (ok && isChar1)
            {
                o.insert(o.end(), c.hdr, c.hdr + 32); seamxform::hdrOnlyTag(o, c.hdr + 64);
                t_run.kicks.push_back(seamxform::Kick{0u, (uint32_t)o.size()});
            }
            else if (ok)
            {   // char2 / char3: pass A then pass B
                o.insert(o.end(), c.hdr, c.hdr + 32); seamxform::hdrOnlyTag(o, c.hdr + 48);
                o.insert(o.end(), c.hdr, c.hdr + 16);
                if (isChar2) o.insert(o.end(), c.hdr + 32, c.hdr + 48);
                else { const size_t q = o.size(); o.resize(q + 16); _mm_storeu_ps(reinterpret_cast<float *>(o.data() + q), t_c3.tex26); }
                seamxform::hdrOnlyTag(o, c.hdr + 64);
                t_run.kicks.push_back(seamxform::Kick{0u, (uint32_t)o.size()});
            }
            if (ok) { t_run.clipOut = clip; t_run.hasClipOut = true; }
        }
        else if (isStage)
        {   // The stage program's constants live at qw0..11 for the whole frame; read them live.
            const uint32_t *fanTag = reinterpret_cast<const uint32_t *>(t_mesh->setup) + 1;
            ok = seamxform::stageChunk(vuData, fanTag, c.hdr, c.verts.data(), c.nvec, t_run.expect, t_run.kicks, t_run.stMask);
            if (ok && t_run.kicks.size() > 2u) ++g_st.clipper;
        }
        else if (isFx)
        {
            const uint32_t *fanTag = reinterpret_cast<const uint32_t *>(t_mesh->setup) + 1;
            std::memcpy(t_liveVu, vuData, sizeof(t_liveVu));
            ps2xSeamVu1Q(&t_vuQ, &t_vuPendingQ, &t_vuQWait);
            ok = seamxform::effectsChunk(t_stageConsts, fanTag, c.hdr, c.verts.data(), c.nvec, clip, t_run.expect, t_run.kicks, t_run.stMask, 0u);
            t_fxChunk = &c;
            if (ok) { t_run.clipOut = clip; t_run.hasClipOut = true; if (t_run.kicks.size() > 1u) ++g_st.clipper; }
        }
        else
        {
            if (isChar2)      ok = seamxform::charChunk2(t_cc, c.hdr, c.verts.data(), c.nvec, clip, t_run.expect, t_run.stMask);
            else if (isChar1) ok = seamxform::charChunk1(t_cc, c.hdr, c.verts.data(), c.nvec, clip, t_run.expect);
            else              ok = seamxform::charChunk3(t_c3, c.hdr, c.verts.data(), c.nvec, clip, t_run.expect, t_run.stMask, t_run.dontCare);
            if (ok) { t_run.kicks.push_back(seamxform::Kick{0u, (uint32_t)t_run.expect.size()}); t_run.clipOut = clip; t_run.hasClipOut = true; }
        }
        if (!ok) return false;
        if (seamvk::on() && !t_run.kicks.empty()) recordForVk(c, isStage, isFx, isChar2, isChar1, vuData, static_cast<PS2Memory *>(memory));
        t_run.active = true;
        if (skipOn())
        {   // PS2X_SEAMSKIP=2: bisect mode, skip VU1 but submit nothing.
            static const bool s_noSubmit = [](){ const char *v = std::getenv("PS2X_SEAMSKIP"); return v && v[0] == '2'; }();
            PS2Memory *mem = static_cast<PS2Memory *>(memory);
            if (!s_noSubmit)
            {
#if defined(PS2X_HAVE_SEAMVK)
                if (seamvk::on())
                {   // [seamvk] 'SVKG': the arbiter hands the payload to the GS backend as PATH1 and to the native front-end in order
                    static thread_local std::vector<uint8_t> hbuf;
                    for (const seamxform::Kick &k : t_run.kicks)
                    {
                        seamvk::HostGifHeader h; h.magic = seamvk::kHostGifMagic; h.size = k.len;
                        hbuf.resize(sizeof(h) + k.len);
                        std::memcpy(hbuf.data(), &h, sizeof(h)); std::memcpy(hbuf.data() + sizeof(h), t_run.expect.data() + k.off, k.len);
                        mem->submitGifPacket(GifPathId::HostDraw, hbuf.data(), (uint32_t)hbuf.size());
                    }
                }
                else
#endif
                    for (const seamxform::Kick &k : t_run.kicks) mem->submitGifPacket(GifPathId::Path1, t_run.expect.data() + k.off, k.len);
            }
            ++g_st.skipped;
            t_run.owned = true;
            return true;
        }
        return false;
    }

    bool beforeRun(uint32_t startPc, bool mscnt, uint32_t top, uint8_t *vuData, uint32_t dataSize, void *memory)
    {
        t_run.active = t_run.owned = t_run.hasClipOut = false; t_run.expect.clear(); t_run.kicks.clear(); t_run.stMask.clear(); t_run.actual.clear(); t_run.dontCare.clear(); t_run.actualKicks.clear();
        uint32_t builder = 0, list = 0;
        if (!seamprobe::currentBatch(builder, list)) return false;
        const bool isStage = builder == kStageBuilder, isChar2 = builder == kChar2Builder, isChar1 = builder == kChar1Builder,
                   isChar3 = builder == kChar3Builder, isFx = builder == kFxBuilder;
        const bool stageLike = isStage || isFx;
        if (!stageLike && !isChar2 && !isChar1 && !isChar3) return false;
        if (!mscnt)
        {   // MSCALF pc 0: the setup run. Everything it computes is derived here; skip mode owns it.
            t_chunk = 0; t_builder = builder; t_mesh = seammesh::get(g_ps2WatchRdram, list, isFx);
            if (t_mesh && (isChar2 || isChar1)) seamxform::charConsts(vuData, isChar2, t_cc);
            if (t_mesh && isChar3) seamxform::char3Consts(vuData, t_c3);
            if (t_mesh && stageLike) std::memcpy(t_stageConsts, vuData, sizeof(t_stageConsts));
            bool callSkipped = false;
            {
                std::lock_guard<std::mutex> lk(g_skipMtx);
                if (!g_skipFifo.empty() && g_skipFifo.front() == (list & 0x1FFFFFFFu)) { g_skipFifo.pop_front(); callSkipped = true; }
            }
            if (callSkipped)
            {   // The list was never unpacked: emit every chunk now, in list order.
                if (t_mesh)
                    for (uint32_t i = 0; i < (uint32_t)t_mesh->chunks.size(); ++i)
                    {
                        emitChunk(i, top, vuData, dataSize, memory, false);
                        afterRunOwned();
                    }
                t_run.active = false; t_run.owned = true;   // afterRun: keep t_hostClip as the chunks left it
                ++g_st.skipped;
                return true;
            }
            if (skipOn() && t_mesh) { ++g_st.skipped; return true; }
            return false;
        }
        { std::lock_guard<std::mutex> lk(g_mtx); ++g_st.runs; if (!t_mesh) { ++g_st.noMesh; return false; } }
        const uint32_t ci = t_chunk++;
        if (stageLike && ci == 0u)
        {   // The setup call: stores the fan tag at qw30 and zeroes vf31. Skip mode owns it.
            if (skipOn() && t_mesh->hasSetup) { ++g_st.skipped; return true; }
            return false;
        }
        const uint32_t chunkIdx = stageLike ? ci - 1u : ci;
        return emitChunk(chunkIdx, top, vuData, dataSize, memory, /*verifyCheck=*/true);
    }

    bool dmaCallSkip(uint8_t *rdram, uint32_t listAddr)
    {
        if (!skipOn() || keepCall()) return false;
        const uint32_t list = listAddr & 0x1FFFFFFFu;
        const uint32_t b = seamprobe::builderOfList(list);
        if (!ownedBuilder(b)) return false;
        if (!seammesh::get(rdram, list, b == kFxBuilder)) return false;
        std::lock_guard<std::mutex> lk(g_skipMtx);
        g_skipFifo.push_back(list);
        if (g_skipFifo.size() > 65536) g_skipFifo.pop_front();
        return true;
    }

    void onKick(const uint8_t *data, uint32_t bytes)
    {
        if (t_run.active && !t_run.owned) { t_run.actual.insert(t_run.actual.end(), data, data + bytes); t_run.actualKicks.push_back(bytes); }
    }

    static void afterRunOwned()
    {
        if (t_run.owned && t_run.hasClipOut) t_hostClip = t_run.clipOut;
        t_run.active = t_run.owned = t_run.hasClipOut = false; t_run.expect.clear(); t_run.kicks.clear(); t_run.stMask.clear(); t_run.actual.clear(); t_run.dontCare.clear(); t_run.actualKicks.clear();
    }

    void afterRun()
    {
        // The CLIP history the next program sees: the real VU register when VU1 ran, ours when
        // the host owned the run (VU1 did not run, so the register did not move).
        if (t_run.owned) { if (t_run.hasClipOut) t_hostClip = t_run.clipOut; }
        else t_hostClip = ps2xSeamVu1Clip();
        if (t_run.active && !t_run.owned)
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            // The ST quadwords' w lane is a stale VU register: mask it before comparing.
            std::vector<uint8_t> a = t_run.actual, e = t_run.expect;
            bool same = a.size() == e.size();
            if (same)
            {
                // ST x/y/z: the VU copies vertices by adding vf31, which it zeroes with a multiply that
                // keeps the sign the previous program left there, so -0 and +0 swap freely between the
                // two sides. The GS treats them alike; compare them alike. The w lane is a stale register.
                auto norm = [&](std::vector<uint8_t> &b) {
                    for (uint32_t off : t_run.stMask)
                    {
                        if (off + 4u > b.size()) continue;
                        std::memset(b.data() + off, 0, 4);
                        for (uint32_t k = 12u; k >= 4u; k -= 4u)
                        {
                            uint32_t w; std::memcpy(&w, b.data() + off - k, 4);
                            if (w == 0x80000000u) { w = 0u; std::memcpy(b.data() + off - k, &w, 4); }
                        }
                    }
                };
                norm(a); norm(e);
                for (uint32_t off : t_run.dontCare) if (off + 4u <= a.size()) { std::memset(a.data() + off, 0, 4); std::memset(e.data() + off, 0, 4); }
                same = std::memcmp(a.data(), e.data(), a.size()) == 0;
            }
            if (same) ++g_st.match;
            else
            {
                ++g_st.mismatch;
                if (g_st.printed < 6)
                {
                    ++g_st.printed;
                    std::fprintf(stderr, "[seamverify] MISMATCH builder=0x%06x chunk=%u expect %zu bytes actual %zu bytes | kicks expect", t_builder, t_chunk - 1u, t_run.expect.size(), t_run.actual.size());
                    for (const seamxform::Kick &k : t_run.kicks) std::fprintf(stderr, " %u", k.len);
                    std::fprintf(stderr, " actual");
                    for (uint32_t k : t_run.actualKicks) std::fprintf(stderr, " %u", k);
                    std::fprintf(stderr, "\n");
                    if (t_builder == kFxBuilder && t_fxChunk)
                    {
                        // Strip (last kick) equal? Then positions and ADC flags agree and only the clipper differs.
                        const size_t sl = t_run.kicks.back().len, al = t_run.actualKicks.back();
                        bool stripSame = sl == al && std::memcmp(t_run.expect.data() + t_run.expect.size() - sl, t_run.actual.data() + t_run.actual.size() - al, sl) == 0;
                        std::fprintf(stderr, "[seamverify]   strip %s | VU at entry q=%a pendingQ=%a qWait=%u | clipIn=%06x\n", stripSame ? "IDENTICAL" : "DIFFERS", t_vuQ, t_vuPendingQ, t_vuQWait, t_clipIn);
                        if (!stripSame)
                        {
                            const uint8_t *es = t_run.expect.data() + t_run.expect.size() - sl, *as = t_run.actual.data() + t_run.actual.size() - al;
                            for (size_t off = 0, shown = 0; off + 16u <= sl && shown < 12; off += 16u)
                                if (std::memcmp(es + off, as + off, 16)) { uint32_t e[4], a[4]; std::memcpy(e, es + off, 16); std::memcpy(a, as + off, 16); std::fprintf(stderr, "[seamverify]   strip qw%zu expect %08x %08x %08x %08x actual %08x %08x %08x %08x\n", off / 16u, e[0], e[1], e[2], e[3], a[0], a[1], a[2], a[3]); ++shown; }
                        }
                        for (int q = 0; q < 17; ++q)
                            if (std::memcmp(t_stageConsts + q * 16, t_liveVu + q * 16, 16))
                            { const float *a = reinterpret_cast<const float *>(t_stageConsts + q * 16), *b = reinterpret_cast<const float *>(t_liveVu + q * 16);
                              std::fprintf(stderr, "[seamverify]   consts qw%d snapshot (%g %g %g %g) live (%g %g %g %g)\n", q, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]); }
                        // First 3 vertices raw (4 qw each).
                        for (uint32_t v = 0; v < 3u && v < t_fxChunk->nvec / 4u; ++v)
                        {
                            const float *f = reinterpret_cast<const float *>(t_fxChunk->verts.data() + v * 64u);
                            const uint32_t *u = reinterpret_cast<const uint32_t *>(t_fxChunk->verts.data() + v * 64u);
                            std::fprintf(stderr, "[seamverify]   v%u P=(%g %g %g w=%08x) q1=(%g %g %g %g) col=(%g %g %g %g) T=(%g %g %g %g)\n", v, f[0], f[1], f[2], u[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
                        }
                        // The first two fans, both sides: XYZ words per vertex.
                        auto dumpFans = [&](const char *who, const std::vector<uint8_t> &b, const std::vector<uint32_t> &lens) {
                            size_t off = 0; int fans = 0;
                            for (size_t i = 0; i + 1 < lens.size() && fans < 2; i += 2)
                            {   // [empty 16][fan 16 + n*48]
                                off += lens[i];
                                const uint32_t n = (lens[i + 1] - 16u) / 48u;
                                std::fprintf(stderr, "[seamverify]   %s fan%d n=%u:", who, fans, n);
                                for (uint32_t v = 0; v < n; ++v) { uint32_t x[4]; std::memcpy(x, b.data() + off + 16u + v * 48u + 32u, 16); std::fprintf(stderr, " (%x,%x,%x,%x)", x[0], x[1], x[2], x[3]); }
                                std::fprintf(stderr, "\n");
                                off += lens[i + 1]; ++fans;
                            }
                        };
                        std::vector<uint32_t> el; for (const seamxform::Kick &k : t_run.kicks) el.push_back(k.len);
                        dumpFans("expect", t_run.expect, el); dumpFans("actual", t_run.actual, t_run.actualKicks);
                        // Which assumption set reproduces the VU's kick sizes?
                        const uint32_t *fanTag = reinterpret_cast<const uint32_t *>(t_mesh->setup) + 1;
                        for (uint32_t var = 0; var < 16u; ++var)
                        {
                            std::vector<uint8_t> o; std::vector<seamxform::Kick> ks; std::vector<uint32_t> sm; uint32_t cl = t_clipIn;
                            if (!seamxform::effectsChunk(t_stageConsts, fanTag, t_fxChunk->hdr, t_fxChunk->verts.data(), t_fxChunk->nvec, cl, o, ks, sm, var)) continue;
                            bool sameSizes = ks.size() == t_run.actualKicks.size();
                            for (size_t i = 0; sameSizes && i < ks.size(); ++i) sameSizes = ks[i].len == t_run.actualKicks[i];
                            std::fprintf(stderr, "[seamverify]   variant %2u (M2w=%d M1w=%d clip0=%d noflag=%d): %zu kicks, sizes %s\n", var, var & 1, (var >> 1) & 1, (var >> 2) & 1, (var >> 3) & 1, ks.size(), sameSizes ? "MATCH" : "differ");
                        }
                    }
                    const size_t n = std::min(t_run.expect.size(), t_run.actual.size());
                    size_t shown = 0;
                    for (size_t off = 0; off + 16u <= n && shown < 6; off += 16u)
                    {
                        if (std::memcmp(e.data() + off, a.data() + off, 16) == 0) continue;
                        uint32_t ew[4], g[4]; std::memcpy(ew, e.data() + off, 16); std::memcpy(g, a.data() + off, 16);
                        std::fprintf(stderr, "[seamverify]   qw%-4zu expect %08x %08x %08x %08x  actual %08x %08x %08x %08x\n",
                                     off / 16u, ew[0], ew[1], ew[2], ew[3], g[0], g[1], g[2], g[3]);
                        ++shown;
                    }
                }
            }
        }
        t_run.active = t_run.owned = false;
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
        if (dt >= 5.0)
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            std::fprintf(stderr, "[seam] %.1fs owned runs=%llu match=%llu mismatch=%llu clipperchunks=%llu skipped=%llu | nomesh=%llu chunkOOB=%llu importMismatch=%llu\n",
                         dt, (unsigned long long)g_st.runs, (unsigned long long)g_st.match, (unsigned long long)g_st.mismatch, (unsigned long long)g_st.clipper,
                         (unsigned long long)g_st.skipped, (unsigned long long)g_st.noMesh, (unsigned long long)g_st.chunkOOB, (unsigned long long)g_st.vertMismatch);
            g_st = Stats{g_st.runs * 0, 0, 0, 0, 0, 0, 0, 0, g_st.printed};
            seammesh::report();
            g_t0 = std::chrono::steady_clock::now();
        }
    }
}

#if !defined(PS2X_HAVE_SEAMVK)
// [fighttick] The stream markers live in ps2_seamgs.cpp, which only builds with the native Vulkan renderer;
// without it nothing reads them.
extern "C" void ps2xSeamFlipStamp(unsigned long long, unsigned long long) {}
extern "C" void ps2xSeamTickMark(unsigned long long) {}
#endif
