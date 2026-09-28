#!/usr/bin/env python3
"""Generate the BT3 overlay runner sources from the user's DBZP.BIN.

DBZP.BIN is the game's overlay binary (the actual gameplay code), loaded by the
boot ELF at 0x334c00. This script:

  1. wraps the raw binary as a minimal MIPS ELF so the recompiler can parse it,
  2. recompiles it three times with the committed function maps
     (dbzp_funcs.csv = main map, dbzp_gaps.csv / dbzp_missing.csv = code blocks
     Ghidra's halt_baddata truncation left out of the main map),
  3. post-processes the output: renames the function table to the overlay table,
     inserts the known callback/jump-table re-entry labels the recompiler's
     analysis misses, and appends dense-table registrations for the gap blocks,
  4. installs the result into ps2xRuntime/src/runner_overlay/ (+ the header).

Usage: gen_overlay.py --recomp <ps2_recomp> --dbzp <DBZP.BIN> --work <dir> --runtime <ps2xRuntime dir>
"""
import argparse
import csv
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = 0x334C00

# Callback / jump-table re-entry points the recompiler's static analysis misses.
# Each gets a `case`+`label` inserted into its containing function and a dense-table
# slot registration, so an indirect guest jump to the address resolves mid-function.
REENTRY_LABELS = [
    (0x335970, "callback re-entry into the new-game load path"),
    (0x337680, "flash-player callback re-entry"),
    (0x3448A8, "P1-vs-COM jump-table target"),
    (0x354098, "duel-menu jump-table target"),
    (0x37E1B8, "Ultimate Training entry jump-table target"),
    (0x359870, "Ultimate Training menu jump-table target"),
    (0x3599E8, "Ultimate Training menu jump-table target"),
    (0x35ABB0, "Ultimate Training menu jump-table target"),
    (0x3A9C18, "Data Center entry jump-table target"),
    (0x3AB2D0, "Data Center menu jump-table target"),
    (0x3AB2D8, "Data Center menu jump-table target"),
    (0x39EC48, "Evolution Z menu jump-table target"),
    (0x39F350, "Evolution Z menu jump-table target"),
    (0x39A5E0, "Evolution Z callback return site"),
    (0x39A7A0, "Evolution Z: last instruction before the f_39a7a4 truncation gap"),
]

RENAMES = [
    ('#include "ps2_recompiled_functions.h"', '#include "ps2_overlay_functions.h"'),
    ("g_ps2RecompiledFunctionTable", "g_ps2OverlayFunctionTable"),
    ("GeneratedFunctionTableInitializer", "OverlayFunctionTableInitializer"),
    ("g_generatedFunctionTableInitializer", "g_overlayFunctionTableInitializer"),
]


def make_wrapper_elf(dbzp: Path, dst: Path) -> None:
    code = dbzp.read_bytes()
    size = len(code)
    EHDR, PHDR, SHENT = 52, 32, 40
    phoff = EHDR
    text_off = EHDR + PHDR
    text_end = text_off + size
    shstr = b"\x00.text\x00.shstrtab\x00"
    shstr_off = text_end
    shstr_end = shstr_off + len(shstr)
    shoff = (shstr_end + 3) & ~3
    ehdr = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8
    ehdr += struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, BASE, phoff, shoff, 0x20924001,
                        EHDR, PHDR, 1, SHENT, 3, 2)
    phdr = struct.pack("<IIIIIIII", 1, text_off, BASE, BASE, size, size, 5, 0x1000)
    sh_null = b"\x00" * SHENT
    sh_text = struct.pack("<IIIIIIIIII", shstr.index(b".text"), 1, 0x6, BASE,
                          text_off, size, 0, 0, 16, 0)
    sh_shstr = struct.pack("<IIIIIIIIII", shstr.index(b".shstrtab"), 3, 0, 0,
                           shstr_off, len(shstr), 0, 0, 1, 0)
    out = bytearray()
    out += ehdr + phdr + code + shstr
    out += b"\x00" * (shoff - shstr_end)
    out += sh_null + sh_text + sh_shstr
    dst.write_bytes(out)
    print(f"wrapped {dbzp.name}: .text 0x{BASE:x}..0x{BASE + size:x}")


def run_recomp(recomp: Path, elf: Path, csv_path: Path, outdir: Path, work: Path) -> None:
    cfg = work / f"config_{csv_path.stem}.toml"
    # Forward slashes: backslash is an escape character in TOML strings (Windows).
    cfg.write_text(
        "[general]\n"
        f'input = "{elf.as_posix()}"\n'
        f'ghidra_output = "{csv_path.as_posix()}"\n'
        f'output = "{outdir.as_posix()}/"\n'
        "single_file_output = true\n"
        "patch_syscalls = false\n"
        "patch_cop0 = true\n"
        "patch_cache = true\n"
        "stubs = []\n"
        "skip = []\n\n[mmio]\n\n[jump_tables]\n"
    )
    subprocess.run([str(recomp), str(cfg)], check=True, stdout=subprocess.DEVNULL)
    print(f"recompiled {csv_path.name} -> {outdir.name}")


def apply_renames(text: str) -> str:
    for old, new in RENAMES:
        text = text.replace(old, new)
    return text


def insert_reentry_labels(lines: list) -> None:
    """fixgap algorithm: label the instruction, add a resume-switch case."""
    for addr, why in REENTRY_LABELS:
        lo = f"0x{addr:06x}"
        instr_pat = re.compile(rf"^\s*// {lo}: 0x")
        instr_i = next((i for i, l in enumerate(lines) if instr_pat.match(l)), None)
        if instr_i is None:
            sys.exit(f"ERROR: re-entry {lo}: instruction line not found (generator output changed?)")
        if any(f"label_{addr:06x}:" in l for l in lines):
            continue  # generator already emitted this label
        lines.insert(instr_i, f"label_{addr:06x}:\n")
        fn_i = next((i for i in range(instr_i, -1, -1)
                     if re.match(r"^void (f_[0-9a-f]+_0x[0-9a-f]+)\(", lines[i])), None)
        if fn_i is None:
            sys.exit(f"ERROR: re-entry {lo}: containing function not found")
        sw_i = next((i for i in range(fn_i, fn_i + 8) if "switch (ctx->pc)" in lines[i]), None)
        if sw_i is None:
            sys.exit(f"ERROR: re-entry {lo}: no resume switch in containing function")
        def_i = next((i for i in range(sw_i, sw_i + 400) if "default: break;" in lines[i]), None)
        if def_i is None:
            def_i = sw_i + 1  # big switch without default in window: insert right after it
        lines.insert(def_i, f"        case 0x{addr:x}u: goto label_{addr:06x}; // {why}\n")


def stitch_truncated_predecessors(lines: list, gaps_csv: Path) -> int:
    """A function truncated at a gap boundary falls off its C++ end without
    advancing ctx->pc -> the dispatcher re-enters at the same pc forever (black
    screen). For each gap start G whose preceding instruction G-4 exists in the
    main module, append `ctx->pc = G; return;` after that instruction so control
    flows into the (range-registered) gap function."""
    import csv as _csv
    stitched = 0
    with open(gaps_csv) as f:
        for row in _csv.DictReader(f):
            g = int(row["start"], 16)
            prev = g - 4
            pat = re.compile(rf"^\s*// 0x{prev:06x}: 0x")
            ii = next((i for i, l in enumerate(lines) if pat.match(l)), None)
            if ii is None:
                continue  # predecessor not in the main module (gap follows another gap)
            # find the end of this instruction's emitted statements: the next line that
            # starts a new instruction comment, a label, or closes the function.
            j = ii + 1
            while j < len(lines) and not (
                re.match(r"^\s*// 0x[0-9a-f]{6}: 0x", lines[j])
                or lines[j].startswith("label_")
                or lines[j].startswith("}")
            ):
                j += 1
            marker = f"ctx->pc = 0x{g:x}u; // gap-boundary stitch"
            if any(marker in l for l in lines[max(0, j - 3):j + 1]):
                continue
            lines.insert(j, f"    {marker}\n    return;\n")
            stitched += 1
    return stitched


def containing_function(lines: list, addr: int) -> str:
    instr_pat = re.compile(rf"^\s*// 0x{addr:06x}: 0x")
    instr_i = next(i for i, l in enumerate(lines) if instr_pat.match(l))
    for i in range(instr_i, -1, -1):
        m = re.match(r"^void (f_[0-9a-f]+_0x[0-9a-f]+)\(", lines[i])
        if m:
            return m.group(1)
    raise RuntimeError(f"no containing function for 0x{addr:x}")


def range_registrar(struct_name: str, csv_path: Path, comment: str) -> str:
    rows = []
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            start, end = int(row["start"], 16), int(row["end"], 16)
            fn = f"{row['name']}_0x{start:x}"
            rows.append((f"        for (uint32_t i={(start - BASE) // 4}u; "
                         f"i<{(end - BASE) // 4}u; ++i) g_ps2OverlayFunctionTable[i] = {fn};\n"))
    return (
        f"\n// {comment}\n"
        "extern PS2Runtime::RecompiledFunction g_ps2OverlayFunctionTable[];\n"
        "namespace {\n"
        f"struct {struct_name} {{\n"
        f"    {struct_name}() {{\n"
        + "".join(rows) +
        "    }\n};\n"
        f"{struct_name} s_{struct_name[0].lower()}{struct_name[1:]};\n"
        "}\n"
    )


# Mid-function entry points the recompiler does not register on its own.
#
# A loop whose body starts inside one generated function but whose back-edge lands in the
# NEXT one cannot be compiled as a `goto` -- the generator emits `ctx->pc = <target>; return;`
# and leaves the dispatcher to resume there. That only works if <target> is a registered
# entry, and a plain mid-sequence instruction never is, so dispatch dies with
# "No exact recompiled function for guest PC <target>".
#
# 0x341358: `addiu $s0, $s1, 0x1`, the head of a loop inside f_3412e8 (which ends at
# 0x341380) whose `bnez` back-edge sits at 0x341444, inside f_341380. Hit by surrendering
# a fight.
# 0x34c0b0: inside f_34bf60 (0x34bf60-0x34c0d8); hit live on 2026-09-05 ("No exact recompiled function for
# guest PC 0x34c0b0", ~24k times in one session) and fixed by hand with fixgap.py in the dev tree only --
# users regenerate from this file, so it lives here now.
# 0x39aaa0: inside f_39aa20 (0x39aa20-0x39aae0) at `sll $a3, $s1, 5`, the middle of a straight-line
# argument-setup sequence. Item Shop (state 0x32) black-screened and hung, dispatching here ~268k times
# per session.
# 0x36fbc0: inside f_36fb48 (0x36fb48-0x36fc00). Ultimate Battle's submenu (0xd->0xe->0xf) hung the same
# way, ~300k iterations. Same mechanism, different screen -- which is why both are listed here rather
# than chasing them one at a time.
# See docs/ITEM-SHOP-CRASH.md. Note these addresses are inside DBZP.BIN (mapped 0x334c00..0x3be71c),
# not the main ELF, so readelf on SLUS_216.78 alone will not show that they are code at all.
MID_FUNCTION_ENTRIES = (0x341358, 0x34c0b0, 0x39aaa0, 0x36fbc0)


def register_mid_function_entries(lines: list, reg: str, addrs) -> tuple:
    """Give each address a label, a resume-switch case, and an overlay table entry."""
    base = 0x334C00  # overlay table base: index = (addr - base) / 4
    for addr in addrs:
        lo = f"0x{addr:06x}"
        instr = re.compile(rf"^\s*// {lo}: 0x")
        i = next((n for n, l in enumerate(lines) if instr.match(l)), None)
        if i is None:
            raise SystemExit(f"gen_overlay: no instruction line for {lo}; "
                             "the overlay layout changed and MID_FUNCTION_ENTRIES needs review")
        label = f"label_{addr:06x}:"
        if not any(label in l for l in lines):
            lines.insert(i, label + "\n")
            i += 1
        fn = None
        for n in range(i, -1, -1):
            m = re.match(r"^void (f_[0-9a-f]+_0x[0-9a-f]+)\(", lines[n])
            if m:
                fn = m.group(1)
                sw = next((k for k in range(n, n + 8) if "switch (ctx->pc)" in lines[k]), None)
                break
        if fn is None or sw is None:
            raise SystemExit(f"gen_overlay: no containing function/switch for {lo}")
        case = f"        case 0x{addr:x}u: goto label_{addr:06x}; // mid-function entry\n"
        if not any(f"case 0x{addr:x}u:" in lines[k] for k in range(sw, sw + 64)):
            end = next((k for k in range(sw, sw + 400) if "default: break;" in lines[k]), sw + 1)
            lines.insert(end, case)
        idx = (addr - base) // 4
        if f"[{idx}]" not in reg:
            anchor = re.search(rf"g_ps2OverlayFunctionTable\[\d+\] = {fn};[^\n]*\n", reg)
            if not anchor:
                raise SystemExit(f"gen_overlay: no anchor registration for {fn} ({lo})")
            reg = (reg[:anchor.end()]
                   + f"        g_ps2OverlayFunctionTable[{idx}] = {fn}; // {lo} mid-function entry\n"
                   + reg[anchor.end():])
        print(f"registered mid-function entry {lo} -> {fn} (idx {idx})")
    return lines, reg


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--recomp", required=True, type=Path)
    ap.add_argument("--dbzp", required=True, type=Path)
    ap.add_argument("--work", required=True, type=Path)
    ap.add_argument("--runtime", required=True, type=Path)
    args = ap.parse_args()

    work = args.work
    work.mkdir(parents=True, exist_ok=True)
    elf = work / "DBZP_wrapped.elf"
    make_wrapper_elf(args.dbzp, elf)

    outs = {}
    for stem in ("dbzp_funcs", "dbzp_gaps", "dbzp_missing"):
        outdir = work / f"out_{stem}"
        run_recomp(args.recomp, elf, HERE / f"{stem}.csv", outdir, work)
        outs[stem] = outdir

    # Main overlay module: renames + re-entry labels.
    main_cpp = (outs["dbzp_funcs"] / "ps2_recompiled_functions.cpp").read_text()
    main_cpp = apply_renames(main_cpp)
    lines = main_cpp.splitlines(keepends=True)
    insert_reentry_labels(lines)
    n_stitch = stitch_truncated_predecessors(lines, HERE / "dbzp_gaps.csv")
    print(f"gap-boundary stitches applied: {n_stitch}")

    # Register file: renames + dense-table slots for each re-entry label.
    reg = apply_renames((outs["dbzp_funcs"] / "register_functions.cpp").read_text())
    extra = "".join(
        f"        g_ps2OverlayFunctionTable[{(addr - BASE) // 4}] = "
        f"{containing_function(lines, addr)}; // 0x{addr:x} {why}\n"
        for addr, why in REENTRY_LABELS)
    closing = "    }\n};\nstatic const OverlayFunctionTableInitializer"
    if closing not in reg:
        sys.exit("ERROR: initializer closing not found in register output")
    # Append inside the initializer's constructor, just before its closing brace.
    reg = reg.replace(closing, extra + closing, 1)

    # Gap modules: renames + registrar structs derived from the CSVs.
    gaps_cpp = apply_renames((outs["dbzp_gaps"] / "ps2_recompiled_functions.cpp").read_text())
    gaps_cpp += range_registrar(
        "OverlayGapRegistrar", HERE / "dbzp_gaps.csv",
        "Register the gap blocks (code Ghidra truncation left out of the main map): "
        "each fills its whole slot range so entry and internal return addresses resolve.")
    missing_cpp = apply_renames((outs["dbzp_missing"] / "ps2_recompiled_functions.cpp").read_text())
    missing_cpp += range_registrar(
        "OverlayMissingRegistrar", HERE / "dbzp_missing.csv",
        "Register the post-logos block at 0x3376b8 (same truncation story).")

    header = (outs["dbzp_funcs"] / "ps2_recompiled_functions.h").read_text()
    header = header.replace("PS2_RECOMPILED_FUNCTIONS_H", "PS2_OVERLAY_FUNCTIONS_H")
    header = header.replace("// PS2_RECOMPILED_FUNCTIONS_H", "// PS2_OVERLAY_FUNCTIONS_H")

    def install(path: Path, text: str) -> None:
        # Write only on change: an untouched mtime lets incremental builds skip
        # recompiling these (one of them is a 487K-line TU).
        if not path.is_file() or path.read_text() != text:
            path.write_text(text)

    lines, reg = register_mid_function_entries(lines, reg, MID_FUNCTION_ENTRIES)

    dst = args.runtime / "src" / "runner_overlay"
    dst.mkdir(parents=True, exist_ok=True)
    install(dst / "overlay_functions.cpp", "".join(lines))
    install(dst / "overlay_register.cpp", reg)
    install(dst / "f_gaps_extra.cpp", gaps_cpp)
    install(dst / "f_3376b8_extra.cpp", missing_cpp)
    install(args.runtime / "include" / "ps2_overlay_functions.h", header)
    print(f"installed overlay sources -> {dst}")


if __name__ == "__main__":
    main()
