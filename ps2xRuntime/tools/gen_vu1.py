#!/usr/bin/env python3
"""gen_vu1.py -- [vu1jit] static VU1 recompiler generator.

Input : the 16 KB microcode images dumped by PS2X_VUMICRO=1 (work/vumicro_<hash>.bin) plus their program extents.
Output: src/lib/vu1_jit_gen.inc -- one C++ function per distinct program body, a label per instruction pair,
        each pair = calls of the compile-time specialised helpers in vu1_jit_ops.inc (jitUpper<W>/jitLower<W>),
        branches = gotos with the delay-slot pair emitted before the jump, JR/JALR = computed goto, E-bit =
        delay-slot pair then return. Anything a helper does not implement falls back to the interpreter via
        vu.jitSlowUpper()/jitSlowLower(), followed by the interpreter's vf0/vi0 reset.
Usage : gen_vu1.py <out.inc> <image.bin:extent_hex> [...]
"""
import sys, struct, hashlib

# [vuqlazy] the emitted pipeline snippets. qDead / cDead hold the value of cyc at which a pending Q / clip
# commits (0 = nothing pending), so a straight-line pair costs NOTHING instead of two counter loads.
#   *_RESOLVE: commit if due (enough for a pure reader of st.q / st.clip)
#   *_SYNC   : make the COUNTER exact too (st.qWait / g_clipWait), for ops that read it and at every return
#   *_ARM    : re-derive the deadline after an op that may have written the counter
Q_RESOLVE = 'if (qDead && cyc >= qDead) { st.q = st.pendingQ; st.qWait = 0u; qDead = 0u; }'
Q_SYNC    = 'if (qDead) { if (cyc >= qDead) { st.q = st.pendingQ; st.qWait = 0u; qDead = 0u; } else st.qWait = qDead - cyc; }'
Q_ARM     = 'qDead = st.qWait ? cyc + st.qWait : 0u;'
C_SYNC    = 'if (cDead) { if (cyc >= cDead) { st.clip = g_pendingClip; g_clipWait = 0u; cDead = 0u; } else g_clipWait = cDead - cyc; }'
C_ARM     = 'cDead = g_clipWait ? cyc + g_clipWait : 0u;'
PAIRC = 'g_vu1PairCount.fetch_add(cyc, std::memory_order_relaxed); '   # [vupairs] once per run, not per pair
PIPE_EXIT = Q_SYNC + ' ' + C_SYNC + ' '
# the fallback sites call the shared helpers instead of inlining the snippets: the fallback is cold, and
# inlining it at ~12k sites doubled the generated source (11.5 -> 24.5 MB) for no codegen benefit
PIPE_SLOW = 'vujit::jitPipeSync(st, cyc, qDead, cDead); '     # before an interpreter fallback: it may read either
PIPE_REARM = 'vujit::jitPipeArm(st, cyc, qDead, cDead); '     # ...and after it, because it may have written either

BRANCH_OPS = {0x20: 'B', 0x21: 'BAL', 0x24: 'JR', 0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}

# [vuqlazy] Which pairs can OBSERVE the Q / clip pipelines. The lazy scheme below does nothing per pair
# and makes the state exact only here, so these sets must stay in step with vu1_jit_ops.inc:
#   Q is read by the q-broadcast upper ops (MULq/ADDq/MADDq/SUBq/MSUBq and their ACC "A" forms);
#   st.qWait is read/written by the 0x40-group lower ops DIV/SQRT/RSQRT/WAITQ;
#   st.clip is read by FCAND/FCOR/FCGET and read+written by the CLIP upper op (which owns g_clipWait).
# Anything the helpers do not implement goes to the interpreter, and THAT path syncs at its call site,
# so an op missing from these sets is still safe as long as no jit HELPER gains a new q/clip read.
Q_BC_OPS = {0x1C, 0x20, 0x21, 0x24, 0x25}          # jOP for op < 0x3C, jSOP for the "A" group
CLIP_SOP = 0x1F                                     # jSOP of the CLIP upper op
CLIP_READ_LOWER = {0x12, 0x13, 0x1C}                # FCAND / FCOR / FCGET (lower opHi)

def jsop(w):
    return (w & 3) | ((w >> 4) & 0x7C)

def upper_reads_q(up):
    op = up & 0x3F
    return (jsop(up) in Q_BC_OPS) if op >= 0x3C else (op in Q_BC_OPS)

def upper_is_clip(up):
    return (up & 0x3F) >= 0x3C and jsop(up) == CLIP_SOP

def lower_touches_qwait(lo, loi):
    # the whole 0x40 group, not just DIV/SQRT/RSQRT/WAITQ: it is a small superset and it also covers
    # the group's rarer members reaching the interpreter with the counters live
    return (not loi) and ((lo >> 25) & 0x7F) == 0x40

def lower_reads_clip(lo, loi):
    return (not loi) and ((lo >> 25) & 0x7F) in CLIP_READ_LOWER

def upper_is_nop(up):
    funct = up & 0x3F
    if funct < 0x3C: return False
    sub = (up & 3) | ((up >> 4) & 0x7C)
    return sub in (0x2F, 0x30)

def imm11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v

def fnv(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

import os
MODE = os.environ.get('VU1GEN_QMODE', 'lazy')   # lazy (default) | eager (the old per-pair countdown) | none (ceiling test, WRONG)

def emit_pair_body(out, k, lo, up, pairs, inline_mode):
    """Emit the per-pair code (no label). inline_mode: this pair is emitted as a delay slot -> its own control
    flow (branch/E-bit) is ignored (matches the interpreter: a branch in a delay slot is lost)."""
    pc = 8 * k
    loi = (up >> 31) & 1
    ebit = (up >> 30) & 1
    op_hi = (lo >> 25) & 0x7F
    is_branch = (not loi) and op_hi in BRANCH_OPS
    out.append(f'        // pc 0x{pc:x}  lo=0x{lo:08x} up=0x{up:08x}')
    # [vujit-lean] the cycle bound only matters where a loop can close (a branch / JR / JALR) or the program ends
    # (E-bit); straight-line pairs just count. st.pc is stored only where something may read it: branch pairs
    # (the bound's return), E-bit pairs, and any lower op that can reach the interpreter's slow path or its
    # diagnostics (flag ops, the 0x40 special group incl. XGKICK/E*/R*/WAITP). Plain loads/stores/immediates and
    # the lower NOP word (0x8000033C) never read it.
    mode = MODE
    if is_branch or ebit:
        # the bound is checked BEFORE the pair runs, so this pair's pipeline tick must not have happened:
        # keep cyc as "pairs executed" by incrementing after the check, not inside it
        out.append('        if (cyc >= maxCycles) { ' + (PIPE_EXIT if mode == 'lazy' else '') + PAIRC + f'st.pc = 0x{pc:x}u; return; }}')
    out.append('        ++cyc;')
    if mode == 'eager':
        out.append('        if (st.qWait > 0u && --st.qWait == 0u) st.q = st.pendingQ;')
        out.append('        if (g_clipWait > 0u && --g_clipWait == 0u) st.clip = g_pendingClip;')
    elif mode == 'lazy':
        # nothing per pair -- only where this pair can observe a pipeline
        if upper_reads_q(up) and not upper_is_nop(up):
            out.append('        ' + Q_RESOLVE)
        if lower_touches_qwait(lo, loi):
            out.append('        ' + Q_SYNC)
        if upper_is_clip(up) or lower_reads_clip(lo, loi):
            out.append('        ' + C_SYNC)
    plain_lower = loi or lo == 0x8000033C or op_hi in (0x00, 0x01, 0x04, 0x05, 0x08, 0x09)
    if is_branch or ebit or not plain_lower:
        out.append(f'        st.pc = 0x{pc:x}u;')
    out.append('        {')
    out.append('            bool fb = false;')
    slow_pre, slow_post = (PIPE_SLOW, PIPE_REARM) if mode == 'lazy' else ('', '')
    up_code = '' if upper_is_nop(up) else f'if (!vujit::jitUpper<0x{up:08x}u>(st)) {{ {slow_pre}vu.jitSlowUpper(0x{up:08x}u); {slow_post}fb = true; }}'
    if is_branch:
        name = BRANCH_OPS[op_hi]
        vit, vis = (lo >> 16) & 0xF, (lo >> 11) & 0xF
        tgt = ((pc + 8 + imm11(lo) * 8) & 0x3FFF)
        cond = {'B': 'true', 'BAL': 'true',
                'IBEQ': f'((int16_t)st.vi[{vis}] == (int16_t)st.vi[{vit}])', 'IBNE': f'((int16_t)st.vi[{vis}] != (int16_t)st.vi[{vit}])',
                'IBLTZ': f'((int16_t)st.vi[{vis}] < 0)', 'IBGTZ': f'((int16_t)st.vi[{vis}] > 0)',
                'IBLEZ': f'((int16_t)st.vi[{vis}] <= 0)', 'IBGEZ': f'((int16_t)st.vi[{vis}] >= 0)',
                'JR': 'true', 'JALR': 'true'}[name]
        lo_code = []
        if name in ('JR', 'JALR'):
            lo_code.append(f'jtgt = ((uint32_t)(uint16_t)st.vi[{vis}] * 8u) & 0x3FFFu;')
        if name in ('BAL', 'JALR') and vit != 0:
            lo_code.append(f'st.vi[{vit}] = {k + 2};')
        lo_code.append(f'br = {cond};')
        lo_code = ' '.join(lo_code)
    else:
        lo_code = f'if (!vujit::jitLower<0x{lo:08x}u>(st, vuData, dataSize)) {{ {slow_pre}vu.jitSlowLower(0x{lo:08x}u, vuData, dataSize, gs, memory, 0x{up:08x}u); {slow_post}fb = true; }}'
    if loi:
        out.append(f'            {up_code}')
        out.append(f'            {{ const uint32_t w = 0x{lo:08x}u; std::memcpy(&st.i, &w, 4); }}')
    else:
        out.append(f'            if constexpr (vuLowerShouldRunBeforeUpper(0x{up:08x}u, 0x{lo:08x}u)) {{ {lo_code} {up_code} }}')
        out.append(f'            else {{ {up_code} {lo_code} }}')
    out.append('            if (fb) { _mm_storeu_ps(st.vf[0], _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f)); st.vi[0] = 0; }')
    out.append('        }')
    if mode == 'lazy':
        if lower_touches_qwait(lo, loi):
            out.append('        ' + Q_ARM)
        if upper_is_clip(up):
            out.append('        ' + C_ARM)
    return is_branch, bool(ebit), (BRANCH_OPS[op_hi] if is_branch else None), (((pc + 8 + imm11(lo) * 8) & 0x3FFF) if is_branch else None)

# [vusuper] Entry PCs each program is actually entered at, from a live fight with PS2X_VUENTRY=1
# (work/drv_VUENT/run.log, 2026-09-04). Keyed by the FNV hash the Prog table uses. WHY THIS EXISTS:
# the generated body gives EVERY pair a label so `goto *jt[st.pc>>3]` can land anywhere, and taking a
# label's address makes the compiler treat that pair as a basic block with unknown predecessors -- so
# nothing, vf registers above all, can stay live across a pair boundary. Only ~5% of pairs are static
# branch targets and only 3-4 PCs per program are ever entered, so labelling just those turns ~91% of
# pairs into straight-line code the compiler can actually optimise across.
# A program NOT listed here keeps a label on every pair (the old behaviour) -- that is the safe default,
# and it is why 0xff81d66058071344 (the destructible-scenery program, which the rig cannot reach) is absent.
VU_ENTRIES = {
    0x0b7f264e0237b48e: [0x0, 0x78, 0xa8, 0x228],
    0x28ce214c0c35fc68: [0x0, 0x78, 0x2e0],
    0x7e8efb7575b22f69: [0x0, 0x80, 0x318],
    0x0492d545dc59311d: [0x0, 0x120, 0x3e8],
    0x05c48715b8434850: [0x0, 0x78, 0xa8],
}

def label_set(pairs, npairs, entries):
    """Pairs that must keep a label: every place control can ARRIVE at from outside its predecessor."""
    if entries is None:
        return set(range(npairs))            # unknown entry set -> keep the full jump table
    lab = {0}
    for pc in entries:
        lab.add((pc // 8) % npairs)
    for k in range(npairs):
        lo, up = pairs[k]
        if (up >> 31) & 1:                   # loi: the lower word is an immediate, not an instruction
            continue
        op = (lo >> 25) & 0x7F
        if op not in BRANCH_OPS:
            continue
        nm = BRANCH_OPS[op]
        if nm in ('JR', 'JALR'):
            lab.add((k + 2) % npairs)        # JALR's link value, i.e. where a matching JR returns to
        else:
            lab.add((((8 * k) + 8 + imm11(lo) * 8) & 0x3FFF) // 8 % npairs)
    return lab

def gen_program(name, image, npairs, entries=None):
    pairs = [(struct.unpack_from('<I', image, 8 * k)[0], struct.unpack_from('<I', image, 8 * k + 4)[0]) for k in range(npairs)]
    out = []
    # [vurestrict] see vu1_jit_ops.inc: st and vuData are separate allocations, and vuData being a
    # uint8_t* otherwise forces a reload of st after every store through it
    out.append(f'static void {name}(VU1Interpreter &vu, VU1State &__restrict st, uint8_t *__restrict vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t maxCycles)')
    out.append('{')
    out.append('    (void)gs; (void)memory;')
    out.append('    uint32_t cyc = 0; bool br = false; uint32_t jtgt = 0; (void)br; (void)jtgt;')
    if MODE == 'lazy':
        out.append('    // [vuqlazy] deadlines, not counters: cyc at which a pending Q / clip commits (0 = idle)')
        out.append('    uint32_t qDead = st.qWait, cDead = g_clipWait; (void)qDead; (void)cDead;')
    lab = label_set(pairs, npairs, entries)
    # [vusuper] dispatch only to pairs control can ARRIVE at. A pc outside that set means the census in
    # VU_ENTRIES is stale (a new entry point, or a JR to somewhere we did not predict): say so loudly and
    # hand the run back rather than jumping into the middle of a superblock.
    def dispatch(expr, what):
        if len(lab) == npairs:
            # every pair is a target: the original computed goto is both smaller and what the committed
            # .inc contains, so emit exactly that
            return ['    static void *const jt[' + str(npairs) + '] = { ' + ', '.join(f'&&L_{k}' for k in range(npairs)) + ' };',
                    f'    goto *jt[({expr} >> 3) & {npairs - 1}u];'] if expr == 'st.pc' else \
                   [f'            goto *jt[({expr} >> 3) & {npairs - 1}u];']
        o = [f'    switch (({expr} >> 3) & {npairs - 1}u)', '    {']
        for t in sorted(lab):
            o.append(f'        case {t}u: goto L_{t};')
        o.append('        default: { static int n_ = 0; if (n_ < 8) { ++n_; std::fprintf(stderr,'
                 f' "[vu1jit] UNLABELLED {what} pc 0x%%x in {name} -- add it to VU_ENTRIES in gen_vu1.py\\n",'
                 f' (unsigned)({expr}) & 0x3FFFu); }} return; }}')
        o.append('    }')
        return o
    out.extend(dispatch('st.pc', 'entry'))
    warn = 0
    for k in range(npairs):
        lo, up = pairs[k]
        if k in lab: out.append(f'L_{k}:')
        out.append('    {')
        is_branch, ebit, bname, tgt = emit_pair_body(out, k, lo, up, pairs, False)
        nxt = k + 1
        if ebit:
            # delay slot pair, then stop
            if nxt < npairs:
                nlo, nup = pairs[nxt]
                if ((nlo >> 25) & 0x7F) in BRANCH_OPS or ((nup >> 30) & 1):
                    warn += 1
                emit_pair_body(out, nxt, nlo, nup, pairs, True)
            out.append('        ' + (PIPE_EXIT if MODE == 'lazy' else '') + PAIRC + f'st.ebit = true; st.pc = 0x{8 * (k + 2):x}u; return;')
        elif is_branch:
            out.append('        if (br) {')
            if nxt < npairs:
                nlo, nup = pairs[nxt]
                if ((nlo >> 25) & 0x7F) in BRANCH_OPS or ((nup >> 30) & 1):
                    warn += 1
                emit_pair_body(out, nxt, nlo, nup, pairs, True)
            if bname in ('JR', 'JALR'):
                d = dispatch('jtgt', 'jump-target')
                for ln in d:
                    out.append(ln if len(lab) == npairs else ('    ' + ln))
            else:
                out.append(f'            goto L_{tgt // 8};')
            out.append('        }')
        out.append('    }')
    out.append('    ' + (PIPE_EXIT if MODE == 'lazy' else '') + PAIRC + f'st.pc = 0x{8 * npairs:x}u & 0x3FFFu; return;')
    out.append('}')
    return '\n'.join(out), warn

def main():
    outp = sys.argv[1]
    progs = []
    seen = {}
    for spec in sys.argv[2:]:
        path, ext = spec.split(':')
        image = open(path, 'rb').read()
        extent = int(ext, 16)
        body = image[:extent]
        bh = hashlib.md5(body).hexdigest()[:8]
        if bh in seen:
            continue
        seen[bh] = True
        progs.append((bh, image, extent))
    parts = ['// GENERATED by work/rig/gen_vu1.py -- do not edit. [vu1jit] static VU1 recompiler output.', '']
    total_warn = 0
    for bh, image, extent in progs:
        # [vusuper] REVERTED 2026-09-04: passing VU_ENTRIES here labels only the pairs control can arrive
        # at, but the entry/jump-target set is NOT STABLE between fights (see the ledger) and the measured
        # win was nil. None = label every pair, the safe original behaviour.
        code, warn = gen_program(f'vu1jit_{bh}', image, len(image) // 8, None)
        total_warn += warn
        parts.append(code)
        parts.append('')
    parts.append('namespace vujit {')
    parts.append('const Prog kPrograms[] = {')
    for bh, image, extent in progs:
        parts.append(f'    {{ 0x{extent:x}u, 0x{fnv(image[:extent]):016x}ull, &vu1jit_{bh} }},   // body md5 {bh}')
    parts.append('};')
    parts.append(f'const int kProgramCount = {len(progs)};')
    parts.append('}')
    open(outp, 'w').write('\n'.join(parts) + '\n')
    print(f'wrote {outp}: {len(progs)} programs, {sum(len(i)//8 for _, i, _ in progs)} pairs, {total_warn} delay-slot control-flow warnings')

if __name__ == '__main__':
    main()
