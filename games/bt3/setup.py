#!/usr/bin/env python3
"""Build Dragon Ball Z: Budokai Tenkaichi 3 (SLUS_216.78, USA) from your own disc image.

    python3 games/bt3/setup.py <path-to-BT3-USA.iso | path-to-SLUS_216.78> [jobs]

Cross-platform (Linux tested; Windows experimental). The game's code is generated
locally from YOUR copy of the game — this repository ships no game code or assets.
Steps: extract/verify the game files, build the recompiler, generate the runner
sources, generate the overlay module, apply patches, build the runner.
"""
import hashlib
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BUILD = ROOT / "build"
WORK = HERE / "work"
ELF_SHA256 = "811188ba9b416500d921cd4d9514df0cbf42f3a41a99cf5aac5a3da37171bf99"
IS_WINDOWS = os.name == "nt"
# Generated TUs are huge; high job counts can exhaust RAM (16 GB: keep <= 3).
DEFAULT_JOBS = "3"


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw) -> None:
    print("+ " + " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def find_extractor():
    # bsdtar reads ISO9660 directly; Windows 10+ ships it as tar.exe.
    for name in ("bsdtar", "tar"):
        exe = shutil.which(name)
        if exe:
            return ("tar", exe)
    for name in ("7z", "7za"):
        exe = shutil.which(name)
        if exe:
            return ("7z", exe)
    die("need bsdtar/tar (Windows 10+ ships tar.exe) or 7z to extract the ISO")


def make_writable(root: Path) -> None:
    # ISO9660 files extract read-only; the game opens some (e.g. BIN/DBZP.BIN) read-write.
    for p in root.rglob("*"):
        try:
            p.chmod(p.stat().st_mode | stat.S_IWRITE)
        except OSError:
            pass


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sync_tree(src: Path, dst: Path, exclude=()) -> None:
    """rsync -a --checksum --delete equivalent: copy changed files, remove extras."""
    dst.mkdir(parents=True, exist_ok=True)
    src_names = {p.name for p in src.iterdir() if p.is_file() and p.name not in exclude}
    for p in sorted(dst.iterdir()):
        if p.is_file() and p.name not in src_names:
            p.unlink()
    copied = 0
    for name in sorted(src_names):
        s, d = src / name, dst / name
        if not d.exists() or s.stat().st_size != d.stat().st_size or \
           sha256_of(s) != sha256_of(d):
            shutil.copyfile(s, d)
            copied += 1
    print(f"synced {src} -> {dst} ({copied} updated, {len(src_names)} total)")


def find_binary(name: str) -> Path:
    exe = name + (".exe" if IS_WINDOWS else "")
    hits = sorted(BUILD.rglob(exe))
    if not hits:
        die(f"{exe} not found under {BUILD} after build")
    return hits[0]


# Windows builds use the Clang toolset of the Visual Studio Build Tools ("C++ Clang Compiler for
# Windows" + "MSBuild support for LLVM (clang-cl) toolset" in the installer): the static VU1
# recompiler is computed-goto code and the runtime uses GCC/Clang builtins, which MSVC cannot
# compile. PS2X_SETUP_TOOLSET overrides (e.g. "v143" to try plain MSVC).
def cmake_configure_extra() -> list:
    """-T only makes sense for the Visual Studio generator; a Ninja build directory (clang-cl via
    -DCMAKE_CXX_COMPILER=clang-cl) must not get it, or the reconfigure fails."""
    extra = ["-DCMAKE_BUILD_TYPE=Release"]   # explicit: a Windows Ninja/clang-cl configure came up Debug (/Od /RTC1 -MDd)
    if not IS_WINDOWS:
        return extra
    cache = BUILD / "CMakeCache.txt"
    if cache.exists():
        gen = ""
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("CMAKE_GENERATOR:"):
                gen = line.split("=", 1)[1]
                break
        if "Visual Studio" not in gen:
            return extra
    return extra + ["-T", os.environ.get("PS2X_SETUP_TOOLSET", "ClangCL")]


def cmake_build(target: str, jobs: str) -> None:
    cmd = ["cmake", "--build", BUILD, "--target", target, "-j", jobs]
    if IS_WINDOWS:
        cmd += ["--config", "Release"]  # multi-config generators (Visual Studio)
    run(cmd)


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    src = Path(sys.argv[1]).resolve()
    jobs = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_JOBS
    if not src.exists():
        die(f"{src} does not exist")
    WORK.mkdir(parents=True, exist_ok=True)
    elf = WORK / "SLUS_216.78"

    # 1. Obtain the game files. The runtime reads loose files (BIN/DBZP.BIN, IRX/,
    #    DATA/) from the directory the ELF lives in, so extract the WHOLE ISO tree.
    if src.suffix.lower() == ".iso":
        kind, exe = find_extractor()
        print(f"== extracting ISO contents (~4 GB) with {exe}")
        if kind == "tar":
            run([exe, "-xf", src, "-C", WORK])
        else:
            run([exe, "x", "-y", f"-o{WORK}", src], stdout=subprocess.DEVNULL)
        if not elf.is_file():
            die("SLUS_216.78 not found in ISO (is this the USA release?)")
        make_writable(WORK)
    else:
        shutil.copyfile(src, elf)
        print("NOTE: you passed a bare ELF. The game also needs the ISO's BIN/, IRX/")
        print(f"      and DATA/ directories next to it in {WORK}.")

    # 2. Verify it is the expected USA ELF.
    got = sha256_of(elf)
    if got != ELF_SHA256:
        print(f"ERROR: ELF sha256 mismatch.\n  expected: {ELF_SHA256}\n  got:      {got}")
        print("Only the USA release (SLUS-21678) is supported. Set PS2X_SETUP_FORCE=1 to continue anyway.")
        if os.environ.get("PS2X_SETUP_FORCE") != "1":
            sys.exit(1)

    # 3. Configure + build the recompiler. Configure only once: the globs use
    #    CONFIGURE_DEPENDS, so later builds re-run cmake by themselves when the
    #    source set changes — and an unnecessary reconfigure rewrites the MSVC
    #    project files, which makes MSBuild rebuild everything from scratch.
    print("== building recompiler")
    if not (BUILD / "CMakeCache.txt").exists():
        run(["cmake", "-S", ROOT, "-B", BUILD] + cmake_configure_extra())
    cmake_build("ps2_recomp", str(os.cpu_count() or 4))
    recomp = find_binary("ps2_recomp")

    # 4. Generate the runner sources. The function map first gets its oversized
    #    Ghidra-truncation rows deduplicated and split into compiler-friendly
    #    chunks (see split_functions.py) — without this, single generated
    #    functions reach ~100K lines and exhaust MSVC's heap.
    print("== generating runner sources")
    sys.path.insert(0, str(HERE))
    from split_functions import split_csv
    split = WORK / "functions_split.csv"
    split_csv(elf, HERE / "functions.csv", split)
    out = WORK / "output"
    if out.exists():
        shutil.rmtree(out)
    cfg_text = (HERE / "config.toml.in").read_text()
    cfg_text = (cfg_text.replace("@ELF@", elf.as_posix())
                        .replace("@CSV@", split.as_posix())
                        .replace("@OUT@", out.as_posix() + "/"))
    (WORK / "config.toml").write_text(cfg_text)
    run([recomp, WORK / "config.toml"])

    # 5. Post-generation patches + the overlay module from DBZP.BIN.
    run([sys.executable, HERE / "apply_patches.py", out])
    print("== generating overlay sources from BIN/DBZP.BIN")
    run([sys.executable, HERE / "gen_overlay.py",
         "--recomp", recomp, "--dbzp", WORK / "BIN" / "DBZP.BIN",
         "--work", WORK / "overlay", "--runtime", ROOT / "ps2xRuntime"])
    run([sys.executable, HERE / "apply_overlay_patches.py", ROOT / "ps2xRuntime"])

    # 6. Install into the runtime tree.
    print("== installing runner sources")
    rt = ROOT / "ps2xRuntime"
    sync_tree(out, rt / "src" / "runner",
              exclude=("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"))
    for h in ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"):
        shutil.copyfile(out / h, rt / "include" / h)

    # 7. Build the game. CONFIGURE_DEPENDS re-globs on Makefile generators, but the
    #    Visual Studio generator does not reliably pick up a changed source SET within
    #    the same build invocation (fresh Windows builds linked without main/the
    #    function tables). Reconfigure explicitly when the runner/overlay file set
    #    changed since the last configure; content-only changes still skip it.
    cache = BUILD / "CMakeCache.txt"
    need_cfg = not cache.exists()
    if not need_cfg:
        ct = cache.stat().st_mtime
        for d in (rt / "src" / "runner", rt / "src" / "runner_overlay"):
            if d.exists() and d.stat().st_mtime > ct:
                need_cfg = True
                break
    if need_cfg:
        run(["cmake", "-S", ROOT, "-B", BUILD] + cmake_configure_extra())
    print(f"== building ps2EntryRunner (-j{jobs}, this takes a while)")
    cmake_build("ps2EntryRunner", jobs)
    runner = find_binary("ps2EntryRunner")

    env_line = ("set PS2X_CD_IMAGE=<path to your BT3 ISO>& " if IS_WINDOWS else
                'env PS2X_CD_IMAGE="<path to your BT3 ISO>" ')
    print(f"""
Done. Run with:

  cd {runner.parent}
  {env_line}\\
      {runner} {elf}
""" if not IS_WINDOWS else f"""
Done. Run with (cmd.exe):

  cd {runner.parent}
  set PS2X_CD_IMAGE=<path to your BT3 ISO>
  {runner} {elf}
""")


if __name__ == "__main__":
    main()
