// vu1_jit_ops.inc -- [vu1jit] compile-time specialised VU1 instruction helpers for the STATIC VU1 recompiler.
// Included at the END of ps2_vu1.cpp so it sees the interpreter's statics (kDestBlend, vuClampFloat, the Q/CLIP
// pipeline globals, vuLaneCopy...). Every helper takes the raw instruction word as a template argument, so the
// field decode, the dest mask and the broadcast lane are all constant-folded; a helper that does not implement
// its word returns false and the generated code falls back to the interpreter's execUpper()/execLower() for
// that instruction (exact semantics by construction). The arithmetic and the flag/clamp/commit sequence are the
// interpreter's fast paths (execUpperFast / execLowerFast) transcribed one-to-one -- keep them in step.
namespace vujit
{
constexpr uint32_t jOP(uint32_t w) { return w & 0x3Fu; }
constexpr uint32_t jSOP(uint32_t w) { return (w & 0x3u) | ((w >> 4) & 0x7Cu); }
constexpr uint8_t jDEST(uint32_t w) { return (uint8_t)((w >> 21) & 0xFu); }
constexpr uint8_t jFT(uint32_t w) { return (uint8_t)((w >> 16) & 0x1Fu); }
constexpr uint8_t jFS(uint32_t w) { return (uint8_t)((w >> 11) & 0x1Fu); }
constexpr uint8_t jFD(uint32_t w) { return (uint8_t)((w >> 6) & 0x1Fu); }

// [vuqlazy] The generated code (work/rig/gen_vu1.py) does NOT tick the Q / clip pipelines per pair. It keeps
// DEADLINES -- qDead / cDead = the value of cyc at which the pending value commits, 0 = idle -- and makes the
// real state exact only where an op can observe it. These two helpers are that sync, used at the interpreter
// FALLBACK sites (inside the `if (!jitUpper/jitLower(...))`, so the fast path pays nothing) and at returns.
// ⚠ IF YOU ADD A READ OF st.q OR st.clip TO A HELPER BELOW, add its opcode to the classifier sets at the top
// of gen_vu1.py and regenerate -- otherwise that op reads a stale value on the recompiled path only.
static inline void jitPipeSync(VU1State &st, uint32_t cyc, uint32_t &qDead, uint32_t &cDead)
{
    if (qDead) { if (cyc >= qDead) { st.q = st.pendingQ; st.qWait = 0u; qDead = 0u; } else st.qWait = qDead - cyc; }
    if (cDead) { if (cyc >= cDead) { st.clip = g_pendingClip; g_clipWait = 0u; cDead = 0u; } else g_clipWait = cDead - cyc; }
}
static inline void jitPipeArm(VU1State &st, uint32_t cyc, uint32_t &qDead, uint32_t &cDead)
{
    qDead = st.qWait ? cyc + st.qWait : 0u;
    cDead = g_clipWait ? cyc + g_clipWait : 0u;
}
constexpr uint8_t jVIT(uint32_t w) { return (uint8_t)((w >> 16) & 0xFu); }
constexpr uint8_t jVIS(uint32_t w) { return (uint8_t)((w >> 11) & 0xFu); }
constexpr uint8_t jVID(uint32_t w) { return (uint8_t)((w >> 6) & 0xFu); }
constexpr int16_t jIMM11(uint32_t w) { return (int16_t)(int32_t)((int32_t)(w << 21) >> 21); }

static const int s_jMacMode = [](){ const char *v = std::getenv("PS2X_VU_MACFLAGS"); return v ? std::atoi(v) : 1; }();
// [macelide] set from Prog::needsMac before each recompiled program runs. false = this program
// never reads the MAC flags, so commitF can skip computing them. PS2X_VU_MACFLAGS=0 still forces
// them off everywhere; PS2X_VU_MACELIDE=0 disables the per-program elision (A/B escape hatch).
bool g_jitMacNeeded = true;
static const bool s_jMacElide = [](){ const char *v = std::getenv("PS2X_VU_MACELIDE"); return !(v && v[0] == '0'); }();
static const bool s_jClamp = vuClampEnabled();
static thread_local float s_jSink[4];   // vf0 write sink (vf0 is the constant (0,0,0,1))

// commitFast, with the dest mask as a template argument.
template <uint8_t DEST>
static inline __attribute__((always_inline)) void commitF(VU1State &st, float *dst, __m128 r)
{
    if (s_jMacMode != 0 && (g_jitMacNeeded || !s_jMacElide))
    {
        static constexpr uint8_t rev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        constexpr uint32_t laneDest = rev4[DEST & 0xFu];
        const uint32_t zm = (uint32_t)_mm_movemask_ps(_mm_cmpeq_ps(r, _mm_setzero_ps())) & laneDest;
        const uint32_t sm = (uint32_t)_mm_movemask_ps(r) & laneDest & ~zm;
        if (s_jMacMode == 2)
            st.mac = zm | (sm << 4);
        else
            st.mac = (uint32_t)rev4[zm] | ((uint32_t)rev4[sm] << 4);
        const uint32_t zAny = zm ? 1u : 0u, sAny = sm ? 2u : 0u;
        st.status = (st.status & ~0x3u) | zAny | sAny | (zAny << 6) | (sAny << 6);
    }
    if (s_jClamp)
    {
        const __m128i ui = _mm_castps_si128(r);
        const __m128i expAll = _mm_set1_epi32(0x7F800000);
        const __m128i infNan = _mm_cmpeq_epi32(_mm_and_si128(ui, expAll), expAll);
        if (_mm_movemask_ps(_mm_castsi128_ps(infNan)))
        {
            alignas(16) float tmp[4];
            _mm_store_ps(tmp, r);
            for (int c = 0; c < 4; ++c) tmp[c] = vuClampFloat(tmp[c]);
            r = _mm_load_ps(tmp);
        }
    }
    constexpr uint8_t d = DEST & 0xFu;
    if constexpr (d == 0xFu) { _mm_storeu_ps(dst, r); return; }
    else if constexpr (d == 0u) { return; }
    else
    {
        const __m128 m = _mm_load_ps((const float *)kDestBlend[d]);
        _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(m, r), _mm_andnot_ps(m, _mm_loadu_ps(dst))));
    }
}
// commitRaw: lane blend only (ITOF/FTOI/ABS: the interpreter's applyDest sets no flags and never clamps)
template <uint8_t DEST>
static inline __attribute__((always_inline)) void commitRaw(float *dst, __m128 r)
{
    constexpr uint8_t d = DEST & 0xFu;
    if constexpr (d == 0xFu) { _mm_storeu_ps(dst, r); }
    else if constexpr (d == 0u) { }
    else
    {
        const __m128 m = _mm_load_ps((const float *)kDestBlend[d]);
        _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(m, r), _mm_andnot_ps(m, _mm_loadu_ps(dst))));
    }
}

// ---------------------------------------------------------------- upper
template <uint32_t W>
// [vurestrict] st is the VU1 register file, vuData is VU1 data memory -- two separate allocations that
// never alias. Saying so matters: vuData is a uint8_t*, which aliases EVERYTHING by the strict-aliasing
// rules, so without __restrict every SQ/ISW store through it forces the compiler to assume st.vf may have
// changed and reload it. That reload, not the per-pair labels, is the suspected reason the VU1 body cannot
// keep vector registers live (see the superblock result in the ledger).
static inline bool jitUpper(VU1State &__restrict st)
{
    constexpr uint32_t op = jOP(W);
    constexpr uint8_t dest = jDEST(W), ft = jFT(W), fs = jFS(W), fd = jFD(W);
    const __m128 vs = _mm_loadu_ps(st.vf[fs]);
    const float *vtp = st.vf[ft];
    if constexpr (op >= 0x3Cu)
    {
        constexpr uint32_t sop = jSOP(W);
        constexpr uint8_t bc = (uint8_t)(W & 3u);
        float *vtd = (ft == 0u) ? s_jSink : st.vf[ft];
        if constexpr (sop >= 0x10u && sop <= 0x13u)
        {   // ITOF0/4/12/15: exact reciprocals of 1/16/4096/32768
            constexpr float kScale = (sop == 0x10u) ? 1.0f : (sop == 0x11u) ? (1.0f / 16.0f) : (sop == 0x12u) ? (1.0f / 4096.0f) : (1.0f / 32768.0f);
            commitRaw<dest>(vtd, _mm_mul_ps(_mm_cvtepi32_ps(_mm_castps_si128(vs)), _mm_set1_ps(kScale)));
            return true;
        }
        else if constexpr (sop >= 0x14u && sop <= 0x17u)
        {   // FTOI0/4/12/15 with vuFtoi() saturation: NaN / below INT32_MIN -> by sign, >= 2^31 -> INT32_MAX
            constexpr float kMul = (sop == 0x14u) ? 1.0f : (sop == 0x15u) ? 16.0f : (sop == 0x16u) ? 4096.0f : 32768.0f;
            const __m128 f = (sop == 0x14u) ? vs : _mm_mul_ps(vs, _mm_set1_ps(kMul));
            __m128i iv = _mm_cvttps_epi32(f);
            const __m128i bad = _mm_cmpeq_epi32(iv, _mm_set1_epi32((int)0x80000000));
            if (_mm_movemask_epi8(bad))
            {
                const __m128i neg = _mm_srai_epi32(_mm_castps_si128(f), 31);
                const __m128i satv = _mm_or_si128(_mm_and_si128(neg, _mm_set1_epi32((int)0x80000000)),
                                                  _mm_andnot_si128(neg, _mm_set1_epi32(0x7FFFFFFF)));
                iv = _mm_or_si128(_mm_and_si128(bad, satv), _mm_andnot_si128(bad, iv));
            }
            commitRaw<dest>(vtd, _mm_castsi128_ps(iv));
            return true;
        }
        else if constexpr (sop == 0x1Du)
        {   // ABS
            commitRaw<dest>(vtd, _mm_and_ps(vs, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))));
            return true;
        }
        else if constexpr (sop == 0x1Fu)
        {   // CLIP (verbatim from the interpreter, incl. the [clippipe] 4-cycle latency model)
            // [clipsse] the six scalar compares as two vector compares + a 64-entry flag table. IEEE ordered compares,
            // so NaN gives false exactly like the scalar form; bit-identical to the interpreter's CLIP.
            const float w = std::fabs(vtp[3]);
            const __m128 vsv = _mm_loadu_ps(st.vf[fs]);
            const __m128 pw = _mm_set1_ps(w), nw = _mm_set1_ps(-w);
            const int gt = _mm_movemask_ps(_mm_cmpgt_ps(vsv, pw)) & 7;   // x>w, y>w, z>w
            const int lt = _mm_movemask_ps(_mm_cmplt_ps(vsv, nw)) & 7;   // x<-w, y<-w, z<-w
            static const uint8_t kClipFlags[64] = {   // index = gt | (lt << 3) -> PS2 order x+ x- y+ y- z+ z-
                0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15, 0x02,0x03,0x06,0x07,0x12,0x13,0x16,0x17,
                0x08,0x09,0x0C,0x0D,0x18,0x19,0x1C,0x1D, 0x0A,0x0B,0x0E,0x0F,0x1A,0x1B,0x1E,0x1F,
                0x20,0x21,0x24,0x25,0x30,0x31,0x34,0x35, 0x22,0x23,0x26,0x27,0x32,0x33,0x36,0x37,
                0x28,0x29,0x2C,0x2D,0x38,0x39,0x3C,0x3D, 0x2A,0x2B,0x2E,0x2F,0x3A,0x3B,0x3E,0x3F };
            const uint32_t flags = kClipFlags[gt | (lt << 3)];
            const uint32_t base = (g_clipWait > 0u) ? g_pendingClip : st.clip;
            const uint32_t nv = ((base << 6) | flags) & 0xFFFFFFu;
            if (g_clipPipe)
            {
                if (g_clipWait > 0u) st.clip = g_pendingClip;
                g_pendingClip = nv;
                g_clipWait = 4u;
            }
            else
                st.clip = nv;
            ++g_clipOpN;
            return true;
        }
        else if constexpr (sop == 0x2Fu || sop == 0x30u)
        {
            return true;   // NOP
        }
        else
        {   // ACC-writing group
            const __m128 bcv = _mm_set1_ps(vtp[bc]);
            const __m128 acc = _mm_loadu_ps(st.acc);
            __m128 r;
            if constexpr (sop <= 0x03u)      r = _mm_add_ps(vs, bcv);                              // ADDAbc
            else if constexpr (sop <= 0x07u) r = _mm_sub_ps(vs, bcv);                              // SUBAbc
            else if constexpr (sop <= 0x0Bu) r = _mm_add_ps(acc, _mm_mul_ps(vs, bcv));             // MADDAbc
            else if constexpr (sop <= 0x0Fu) r = _mm_sub_ps(acc, _mm_mul_ps(vs, bcv));             // MSUBAbc
            else if constexpr (sop >= 0x18u && sop <= 0x1Bu) r = _mm_mul_ps(vs, bcv);              // MULAbc
            else if constexpr (sop == 0x1Cu) r = _mm_mul_ps(vs, _mm_set1_ps(st.q));                // MULAq
            else if constexpr (sop == 0x1Eu) r = _mm_mul_ps(vs, _mm_set1_ps(st.i));                // MULAi
            else if constexpr (sop == 0x20u) r = _mm_add_ps(vs, _mm_set1_ps(st.q));                // ADDAq
            else if constexpr (sop == 0x21u) r = _mm_add_ps(acc, _mm_mul_ps(vs, _mm_set1_ps(st.q)));   // MADDAq
            else if constexpr (sop == 0x22u) r = _mm_add_ps(vs, _mm_set1_ps(st.i));                // ADDAi
            else if constexpr (sop == 0x23u) r = _mm_add_ps(acc, _mm_mul_ps(vs, _mm_set1_ps(st.i)));   // MADDAi
            else if constexpr (sop == 0x24u) r = _mm_sub_ps(vs, _mm_set1_ps(st.q));                // SUBAq
            else if constexpr (sop == 0x25u) r = _mm_sub_ps(acc, _mm_mul_ps(vs, _mm_set1_ps(st.q)));   // MSUBAq
            else if constexpr (sop == 0x26u) r = _mm_sub_ps(vs, _mm_set1_ps(st.i));                // SUBAi
            else if constexpr (sop == 0x27u) r = _mm_sub_ps(acc, _mm_mul_ps(vs, _mm_set1_ps(st.i)));   // MSUBAi
            else if constexpr (sop == 0x28u) r = _mm_add_ps(vs, _mm_loadu_ps(vtp));                // ADDA
            else if constexpr (sop == 0x29u) r = _mm_add_ps(acc, _mm_mul_ps(vs, _mm_loadu_ps(vtp)));   // MADDA
            else if constexpr (sop == 0x2Au) r = _mm_mul_ps(vs, _mm_loadu_ps(vtp));                // MULA
            else if constexpr (sop == 0x2Cu) r = _mm_sub_ps(vs, _mm_loadu_ps(vtp));                // SUBA
            else if constexpr (sop == 0x2Du) r = _mm_sub_ps(acc, _mm_mul_ps(vs, _mm_loadu_ps(vtp)));   // MSUBA
            else if constexpr (sop == 0x2Eu)
            {   // OPMULA: acc = (vs.y*vt.z, vs.z*vt.x, vs.x*vt.y, 0)
                const __m128 vt4 = _mm_loadu_ps(vtp);
                r = _mm_mul_ps(_mm_shuffle_ps(vs, vs, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(vt4, vt4, _MM_SHUFFLE(3, 1, 0, 2)));
                r = _mm_and_ps(r, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
            }
            else return false;   // ITOF/FTOI handled above; anything else -> interpreter
            commitF<dest>(st, st.acc, r);
            return true;
        }
    }
    else
    {
        constexpr uint8_t bc = (uint8_t)(op & 3u);
        const __m128 bcv = _mm_set1_ps(vtp[bc]);
        __m128 r;
        if constexpr (op <= 0x03u)      r = _mm_add_ps(vs, bcv);                                   // ADDbc
        else if constexpr (op <= 0x07u) r = _mm_sub_ps(vs, bcv);                                   // SUBbc
        else if constexpr (op <= 0x0Bu) r = _mm_add_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, bcv)); // MADDbc
        else if constexpr (op <= 0x0Fu) r = _mm_sub_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, bcv)); // MSUBbc
        else if constexpr (op >= 0x18u && op <= 0x1Bu) r = _mm_mul_ps(vs, bcv);                   // MULbc
        else if constexpr (op == 0x1Cu) r = _mm_mul_ps(vs, _mm_set1_ps(st.q));                     // MULq
        else if constexpr (op == 0x1Eu) r = _mm_mul_ps(vs, _mm_set1_ps(st.i));                     // MULi
        else if constexpr (op == 0x20u) r = _mm_add_ps(vs, _mm_set1_ps(st.q));                     // ADDq
        else if constexpr (op == 0x21u) r = _mm_add_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, _mm_set1_ps(st.q)));   // MADDq
        else if constexpr (op == 0x22u) r = _mm_add_ps(vs, _mm_set1_ps(st.i));                     // ADDi
        else if constexpr (op == 0x23u) r = _mm_add_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, _mm_set1_ps(st.i)));   // MADDi
        else if constexpr (op == 0x24u) r = _mm_sub_ps(vs, _mm_set1_ps(st.q));                     // SUBq
        else if constexpr (op == 0x25u) r = _mm_sub_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, _mm_set1_ps(st.q)));   // MSUBq
        else if constexpr (op == 0x26u) r = _mm_sub_ps(vs, _mm_set1_ps(st.i));                     // SUBi
        else if constexpr (op == 0x27u) r = _mm_sub_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, _mm_set1_ps(st.i)));   // MSUBi
        else if constexpr (op == 0x28u) r = _mm_add_ps(vs, _mm_loadu_ps(vtp));                     // ADD
        else if constexpr (op == 0x29u) r = _mm_add_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, _mm_loadu_ps(vtp)));   // MADD
        else if constexpr (op == 0x2Au) r = _mm_mul_ps(vs, _mm_loadu_ps(vtp));                     // MUL
        else if constexpr (op == 0x2Cu) r = _mm_sub_ps(vs, _mm_loadu_ps(vtp));                     // SUB
        else if constexpr (op == 0x2Du) r = _mm_sub_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(vs, _mm_loadu_ps(vtp)));   // MSUB
        else if constexpr (op == 0x2Eu)
        {   // OPMSUB: vd = (acc.x - vs.y*vt.z, acc.y - vs.z*vt.x, acc.z - vs.x*vt.y, 0)
            const __m128 vt4 = _mm_loadu_ps(vtp);
            r = _mm_sub_ps(_mm_loadu_ps(st.acc), _mm_mul_ps(_mm_shuffle_ps(vs, vs, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(vt4, vt4, _MM_SHUFFLE(3, 1, 0, 2))));
            r = _mm_and_ps(r, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
        }
        else return false;   // MAX/MINI families set no flags: interpreter
        float *dst = (fd == 0u) ? s_jSink : st.vf[fd];
        commitF<dest>(st, dst, r);
        return true;
    }
}

// ---------------------------------------------------------------- lower (non-branch)
template <uint32_t LO>
static inline bool jitLower(VU1State &__restrict st, uint8_t *__restrict vuData, uint32_t dataSize)
{
    if constexpr (LO == 0u || LO == 0x8000033Cu) return true;   // NOP
    constexpr uint32_t opHi = (LO >> 25) & 0x7Fu;
    constexpr uint8_t dest = jDEST(LO);
    constexpr uint8_t vfT = jFT(LO), vfS = jFS(LO), viT = jVIT(LO), viS = jVIS(LO), viD = jVID(LO);
    constexpr int16_t imm11 = jIMM11(LO);
    auto lqTo = [&](uint32_t addr, uint8_t vfDst) {
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize) { float tmp[4]; std::memcpy(tmp, vuData + addr, 16); if (vfDst != 0u) vuLaneCopy(st.vf[vfDst], tmp, dest); } };
    auto sqFrom = [&](uint32_t addr, uint8_t vfSrc) {
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4]; std::memcpy(tmp, vuData + addr, 16);
            const float *src = st.vf[vfSrc];
            if constexpr (dest & 0x8) tmp[0] = src[0];
            if constexpr (dest & 0x4) tmp[1] = src[1];
            if constexpr (dest & 0x2) tmp[2] = src[2];
            if constexpr (dest & 0x1) tmp[3] = src[3];
            std::memcpy(vuData + addr, tmp, 16);
        } };
    if constexpr (opHi == 0x00u) { lqTo(((uint32_t)(int32_t)(st.vi[viS] + imm11)) * 16u, vfT); return true; }          // LQ
    else if constexpr (opHi == 0x01u) { sqFrom(((uint32_t)(int32_t)(st.vi[viT] + imm11)) * 16u, vfS); return true; }   // SQ
    else if constexpr (opHi == 0x04u)
    {   // ILW
        uint32_t addr = ((uint32_t)(int32_t)(st.vi[viS] + imm11)) * 16u; addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            constexpr int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
            uint32_t v; std::memcpy(&v, vuData + addr + comp * 4, 4);
            if (viT != 0) st.vi[viT] = (int32_t)(int16_t)(v & 0xFFFF);
        }
        return true;
    }
    else if constexpr (opHi == 0x05u)
    {   // ISW
        uint32_t addr = ((uint32_t)(int32_t)(st.vi[viS] + imm11)) * 16u; addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            const uint32_t val = (uint32_t)(uint16_t)(st.vi[viT] & 0xFFFF);
            if constexpr (dest & 0x8) std::memcpy(vuData + addr + 0, &val, 4);
            if constexpr (dest & 0x4) std::memcpy(vuData + addr + 4, &val, 4);
            if constexpr (dest & 0x2) std::memcpy(vuData + addr + 8, &val, 4);
            if constexpr (dest & 0x1) std::memcpy(vuData + addr + 12, &val, 4);
        }
        return true;
    }
    else if constexpr (opHi == 0x08u) { constexpr int16_t imm15 = (int16_t)((LO & 0x7FFu) | ((LO >> 10) & 0x7800u)); if (viT != 0) st.vi[viT] = (int16_t)(st.vi[viS] + imm15); return true; }   // IADDIU
    else if constexpr (opHi == 0x09u) { constexpr int16_t imm15 = (int16_t)((LO & 0x7FFu) | ((LO >> 10) & 0x7800u)); if (viT != 0) st.vi[viT] = (int16_t)(st.vi[viS] - imm15); return true; }   // ISUBIU
    else if constexpr (opHi == 0x12u) { constexpr uint32_t imm24 = LO & 0xFFFFFFu; st.vi[1] = ((st.clip & imm24) != 0u) ? 1 : 0; ++g_fcandN; if (st.vi[1]) ++g_fcandNZ; return true; }   // FCAND
    else if constexpr (opHi == 0x13u) { constexpr uint32_t imm24 = LO & 0xFFFFFFu; st.vi[1] = ((st.clip | imm24) == 0xFFFFFFu) ? 1 : 0; return true; }   // FCOR
    else if constexpr (opHi == 0x1Cu) { if (viT != 0) st.vi[viT] = (int32_t)(st.clip & 0xFFFu); return true; }   // FCGET
    else if constexpr (opHi == 0x40u)
    {
        constexpr uint32_t funct = LO & 0x3Fu;
        if constexpr (funct == 0x30u) { if (viD != 0) st.vi[viD] = (int16_t)(st.vi[viS] + st.vi[viT]); return true; }   // IADD
        else if constexpr (funct == 0x31u) { if (viD != 0) st.vi[viD] = (int16_t)(st.vi[viS] - st.vi[viT]); return true; }   // ISUB
        else if constexpr (funct == 0x32u) { constexpr int16_t imm5 = (int16_t)(int8_t)(((LO >> 6) & 0x1Fu) | (((LO >> 6) & 0x10u) ? 0xE0u : 0u)); if (viT != 0) st.vi[viT] = (int16_t)(st.vi[viS] + imm5); return true; }   // IADDI
        else if constexpr (funct == 0x34u) { if (viD != 0) st.vi[viD] = st.vi[viS] & st.vi[viT]; return true; }   // IAND
        else if constexpr (funct == 0x35u) { if (viD != 0) st.vi[viD] = st.vi[viS] | st.vi[viT]; return true; }   // IOR
        else if constexpr (funct >= 0x3Cu)
        {
            constexpr uint32_t f2 = jSOP(LO);
            if constexpr (f2 == 0x30u) { float tmp[4]; std::memcpy(tmp, st.vf[vfS], 16); if (vfT != 0u) vuLaneCopy(st.vf[vfT], tmp, dest); return true; }   // MOVE
            else if constexpr (f2 == 0x31u) { const float *v = st.vf[vfS]; float tmp[4] = {v[1], v[2], v[3], v[0]}; if (vfT != 0u) vuLaneCopy(st.vf[vfT], tmp, dest); return true; }   // MR32
            else if constexpr (f2 == 0x34u) { lqTo(((uint32_t)(uint16_t)st.vi[viS]) * 16u, vfT); if (viS != 0) st.vi[viS] = (int16_t)(st.vi[viS] + 1); return true; }   // LQI
            else if constexpr (f2 == 0x35u) { sqFrom(((uint32_t)(uint16_t)st.vi[viT]) * 16u, vfS); if (viT != 0) st.vi[viT] = (int16_t)(st.vi[viT] + 1); return true; }   // SQI
            else if constexpr (f2 == 0x36u) { if (viS != 0) st.vi[viS] = (int16_t)(st.vi[viS] - 1); lqTo(((uint32_t)(uint16_t)st.vi[viS]) * 16u, vfT); return true; }   // LQD
            else if constexpr (f2 == 0x37u) { if (viT != 0) st.vi[viT] = (int16_t)(st.vi[viT] - 1); sqFrom(((uint32_t)(uint16_t)st.vi[viT]) * 16u, vfS); return true; }   // SQD
            else if constexpr (f2 == 0x38u)
            {   // DIV
                const float num = st.vf[vfS][dest & 3u], den = st.vf[vfT][(dest >> 2) & 3u];
                float res;
                if (den != 0.0f) res = vuClampFloat(num / den);
                else res = (num >= 0.0f) ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max();
                if (g_qPipe) { if (st.qWait > 0u) st.q = st.pendingQ; st.pendingQ = res; st.qWait = 7u; } else st.q = res;
                return true;
            }
            else if constexpr (f2 == 0x39u)
            {   // SQRT
                const float res = vuClampFloat(std::sqrt(std::fabs(st.vf[vfT][(dest >> 2) & 3u])));
                if (g_qPipe) { if (st.qWait > 0u) st.q = st.pendingQ; st.pendingQ = res; st.qWait = 7u; } else st.q = res;
                return true;
            }
            else if constexpr (f2 == 0x3Au)
            {   // RSQRT
                const float num = st.vf[vfS][dest & 3u], den = std::sqrt(std::fabs(st.vf[vfT][(dest >> 2) & 3u]));
                float res;
                if (den != 0.0f) res = vuClampFloat(num / den);
                else res = std::numeric_limits<float>::max();
                if (g_qPipe) { if (st.qWait > 0u) st.q = st.pendingQ; st.pendingQ = res; st.qWait = 13u; } else st.q = res;
                return true;
            }
            else if constexpr (f2 == 0x3Bu) { if (st.qWait > 0u) { st.q = st.pendingQ; st.qWait = 0u; } return true; }   // WAITQ
            else if constexpr (f2 == 0x3Cu)
            {   // MTIR
                constexpr int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                uint32_t fval; std::memcpy(&fval, &st.vf[vfS][comp], 4);
                if (viT != 0) st.vi[viT] = (int32_t)(int16_t)(fval & 0xFFFF);
                return true;
            }
            else if constexpr (f2 == 0x3Du)
            {   // MFIR
                float result[4]; int32_t val = (int32_t)(int16_t)(st.vi[viS] & 0xFFFF);
                std::memcpy(&result[0], &val, 4); result[1] = result[0]; result[2] = result[0]; result[3] = result[0];
                if (vfT != 0u) vuLaneCopy(st.vf[vfT], result, dest);
                return true;
            }
            else if constexpr (f2 == 0x3Eu)
            {   // ILWR
                uint32_t addr = ((uint32_t)(uint16_t)st.vi[viS]) * 16u; addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    constexpr int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                    uint32_t v; std::memcpy(&v, vuData + addr + comp * 4, 4);
                    if (viT != 0) st.vi[viT] = (int32_t)(int16_t)(v & 0xFFFF);
                }
                return true;
            }
            else if constexpr (f2 == 0x3Fu)
            {   // ISWR
                uint32_t addr = ((uint32_t)(uint16_t)st.vi[viS]) * 16u; addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    const uint32_t val = (uint32_t)(uint16_t)(st.vi[viT] & 0xFFFF);
                    if constexpr (dest & 0x8) std::memcpy(vuData + addr + 0, &val, 4);
                    if constexpr (dest & 0x4) std::memcpy(vuData + addr + 4, &val, 4);
                    if constexpr (dest & 0x2) std::memcpy(vuData + addr + 8, &val, 4);
                    if constexpr (dest & 0x1) std::memcpy(vuData + addr + 12, &val, 4);
                }
                return true;
            }
            else if constexpr (f2 == 0x68u) { if (viT != 0) st.vi[viT] = (int32_t)(st.top & 0x3FFu); return true; }    // XTOP
            else if constexpr (f2 == 0x69u) { if (viT != 0) st.vi[viT] = (int32_t)(st.itop & 0x3FFu); return true; }   // XITOP
            else return false;   // XGKICK, E*, R*, WAITP...: interpreter
        }
        else return false;
    }
    else return false;   // branches are emitted by the generator; anything else -> interpreter
}
} // namespace vujit
