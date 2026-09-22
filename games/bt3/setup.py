#!/usr/bin/env python3
"""Build, deploy and package Dragon Ball Z: Budokai Tenkaichi 3 (SLUS_216.78, USA).

One script, four stages:

    stage 1  detect    identify the platform, toolchain, package manager and build inputs
    stage 2  deps      report and (interactively) install missing dependencies
    stage 3  build     the original pipeline: extract, recompile, generate, patch, build the runner
    stage 4  package   assemble the portable tree and produce the release artifact for this OS

The game's code is generated locally from YOUR copy of the game -- this repository ships no game
code or assets.

    python3 games/bt3/setup.py <iso|elf> [--stage N] [-y] [--deploy OUT] [--no-package]

The release artifact is produced by default (stage 4); pass --no-package to assemble only the
deploy tree. Backwards-compatible flags kept for the build/release scripts: --jobs, --skip-setup,
--gen-only, --deploy. See --help and --list-stages.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform as _platform
import shutil
import stat
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

HERE = Path(__file__).resolve().parent
try:   # console UI (TUI + plain fallback); optional so a missing file never breaks the build
    from setup_ui import Caps, View
except Exception:   # noqa: BLE001
    Caps = View = None   # type: ignore
ROOT = HERE.parent.parent
# Overridable so a build can generate into a build-local dir (PS2X_BUILD_DIR)
# instead of the source tree. Defaults to <repo>/build.
BUILD = Path(os.environ.get("PS2X_BUILD_DIR") or str(ROOT / "build"))
WORK = HERE / "work"
ELF_SHA256 = "811188ba9b416500d921cd4d9514df0cbf42f3a41a99cf5aac5a3da37171bf99"
# Generated TUs are huge; high job counts can exhaust RAM. This is only the fallback: by default the
# job count is auto-sized from CPU/RAM (see plan_build), and 16 GB lands back on this value.
DEFAULT_JOBS = "3"

# Pinned tool versions (stage 2 installs exactly these; keep in sync with the release builds).
CMAKE_MIN = (3, 21)
QT_VERSION = "6.5.3"
# Preferred Windows kit. 6.5.x only ships win64_msvc2019_64 (no msvc2022 kit), so that is the pin;
# _pick_qt_kit() falls back to whatever aqt offers for the configured version.
QT_KIT_WINDOWS = "win64_msvc2019_64"
MESA_LAVAPIPE_VERSION = "26.2.0"
DEPS_7ZR_URL = "https://www.7-zip.org/a/7zr.exe"

# Linux package groups per package manager (stage 2). Split so a missing FFmpeg or Qt can be
# installed without re-running the whole toolchain install; "extras" are optional (a failure only
# warns). Package names follow each distro family.
LINUX_GROUPS: dict[str, dict[str, str]] = {
    "apt": {   # Debian, Ubuntu, Mint, Pop!_OS, Kali, Raspberry Pi OS
        "toolchain": "clang cmake ninja-build pkg-config git libx11-dev libxrandr-dev "
                     "libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev libglu1-mesa-dev "
                     "libarchive-tools p7zip-full",
        "ffmpeg": "libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev",
        "qt": "qt6-base-dev",
        "extras": "ccache mold",
    },
    "dnf": {   # Fedora, RHEL 8+, CentOS Stream, Nobara
        "toolchain": "clang cmake ninja-build pkgconf-pkg-config git libX11-devel "
                     "libXrandr-devel libXi-devel libXcursor-devel libXinerama-devel mesa-libGL-devel "
                     "mesa-libGLU-devel libarchive p7zip",
        "ffmpeg": "ffmpeg-devel",   # needs RPM Fusion on Fedora
        "qt": "qt6-qtbase-devel",
        "extras": "ccache mold",
    },
    "yum": {   # older RHEL/CentOS (dnf preferred when present)
        "toolchain": "clang cmake ninja-build pkgconfig git libX11-devel libXrandr-devel "
                     "libXi-devel libXcursor-devel libXinerama-devel mesa-libGL-devel "
                     "mesa-libGLU-devel libarchive",
        "ffmpeg": "ffmpeg-devel",
        "qt": "qt6-qtbase-devel",
        "extras": "ccache",
    },
    "pacman": {   # Arch, Manjaro, EndeavourOS
        "toolchain": "clang cmake ninja pkgconf git libx11 libxrandr libxi libxcursor "
                     "libxinerama mesa glu libarchive p7zip",
        "ffmpeg": "ffmpeg",
        "qt": "qt6-base",
        "extras": "ccache mold",
    },
    "zypper": {   # openSUSE Tumbleweed/Leap
        "toolchain": "clang cmake ninja pkg-config git libX11-devel libXrandr-devel "
                     "libXi-devel libXcursor-devel libXinerama-devel Mesa-libGL-devel glu-devel "
                     "libarchive p7zip",
        "ffmpeg": "ffmpeg-devel",   # may need the Packman repo
        "qt": "qt6-base-devel",
        "extras": "ccache mold",
    },
    "apk": {   # Alpine (musl; best effort)
        "toolchain": "clang cmake ninja pkgconf git libx11-dev libxrandr-dev libxi-dev "
                     "libxcursor-dev libxinerama-dev mesa-dev glu-dev libarchive-tools",
        "ffmpeg": "ffmpeg-dev",
        "qt": "qt6-qtbase-dev",
        "extras": "ccache mold",
    },
    "xbps": {   # Void
        "toolchain": "clang cmake ninja pkgconf git libX11-devel libXrandr-devel libXi-devel "
                     "libXcursor-devel libXinerama-devel MesaLib-devel glu-devel libarchive-tools",
        "ffmpeg": "ffmpeg-devel",
        "qt": "qt6-base-devel",
        "extras": "ccache mold",
    },
    "eopkg": {   # Solus
        "toolchain": "clang cmake ninja pkgconf git libx11-devel libxrandr-devel libxi-devel "
                     "libxcursor-devel libxinerama-devel mesa-devel glu-devel libarchive",
        "ffmpeg": "ffmpeg-devel",
        "qt": "qt6-base-devel",
        "extras": "ccache",
    },
}

# Gentoo has no safe unattended package install (USE flags, source builds): stage 2 prints the atoms.
GENTOO_HINT = ("emerge -a sys-devel/clang dev-build/cmake dev-build/ninja dev-util/pkgconf "
               "dev-vcs/git x11-libs/libX11 x11-libs/libXrandr x11-libs/libXi x11-libs/libXcursor "
               "x11-libs/libXinerama media-libs/mesa media-libs/glu app-arch/libarchive "
               "media-libs/ffmpeg dev-qt/qtbase")

# Stage registry: name -> (number, callable). Order matters.
STAGES: list[tuple[str, str]] = [
    ("1", "detect  - platform, toolchain, package manager and build inputs"),
    ("2", "deps    - report and install missing dependencies"),
    ("3", "build   - extract, recompile, generate, patch, build the runner"),
    ("4", "package - assemble the deploy tree and the release artifact"),
]


# ------------------------------------------------------------------------------------------------
# Output: console gated by --log-level + a complete execution log (always, every line)
# ------------------------------------------------------------------------------------------------
LOG_LEVELS = {0: "silent", 1: "errors", 2: "errors+warnings", 3: "info", 4: "verbose"}


class CommandError(RuntimeError):
    def __init__(self, cmdline: str, code: int):
        super().__init__(f"command failed (exit {code}): {cmdline}")
        self.cmdline = cmdline
        self.code = code


class Logger:
    """Console output filtered by level; the log file always receives everything.

    Levels: 0 silent, 1 errors, 2 errors+warnings, 3 info/steps (default), 4 verbose (every command).
    Subprocess output is classified per line, so at level 2 a long build still shows warnings/errors
    on screen while the log file keeps the full transcript.
    """

    def __init__(self, path: Optional[Path], level: int):
        self.level = level
        self.path = path
        self._fh = None
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
            # "w": every run overwrites the previous log (the default path is a fixed setup.log).
            self._fh = open(path, "w", encoding="utf-8", errors="replace")
            self._fh.write(f"===== setup.py run {time.strftime('%Y-%m-%d %H:%M:%S')} "
                           f"=====\n")

    def _write_file(self, text: str) -> None:
        if self._fh:
            self._fh.write(text if text.endswith("\n") else text + "\n")
            self._fh.flush()

    def _emit(self, text: str, kind: int, stream=None) -> None:
        self._write_file(text)
        if self.level >= kind:
            stream = stream or sys.stdout
            stream.write(text if text.endswith("\n") else text + "\n")
            stream.flush()

    # kinds: 0 always, 1 error, 2 warning, 3 info, 4 verbose
    def banner(self, msg: str) -> None: self._emit(msg, 0)
    def error(self, msg: str) -> None: self._emit(f"ERROR: {msg}", 1, sys.stderr)
    def warn(self, msg: str) -> None: self._emit(f"WARNING: {msg}", 2, sys.stderr)
    def info(self, msg: str = "") -> None: self._emit(msg, 3)
    def step(self, msg: str) -> None: self._emit(f"\n== {msg}", 3)
    def verbose(self, msg: str) -> None: self._emit(msg, 4)

    def raw(self, text: str) -> None:
        """Straight to the log, never to the console (quiet subprocesses)."""
        self._write_file(text)

    @staticmethod
    def classify(line: str) -> int:
        low = line.lower()
        if any(k in low for k in ("error", "failed", "undefined symbol", "fatal", "cannot open")):
            return 1
        if "warning" in low:
            return 2
        return 3

    def process_line(self, line: str) -> None:
        self._emit(line.rstrip("\n"), self.classify(line))

    def close(self) -> None:
        if self._fh:
            self._fh.flush()
            self._fh.close()
            self._fh = None


LOG = Logger(None, 3)

# The console view (setup_ui.View) when the UI module is present and stdout is a terminal; every
# consumer guards on it, so a plain run still works exactly as before.
VIEW = None


def die(msg: str, code: int = 1, stage: str = "") -> "None":
    where = f" at stage {stage}" if stage else ""
    LOG.error(f"{msg}{where}")
    if VIEW is not None:
        try:
            VIEW.close()
        except Exception:   # noqa: BLE001
            pass
    if LOG.path:
        print(f"FAILED{where}. Full log: {LOG.path}", file=sys.stderr)
    LOG.close()
    sys.exit(code)


def warn(msg: str) -> None:
    LOG.warn(msg)


def step(msg: str) -> None:
    LOG.step(msg)


def _child_env() -> dict:
    """Subprocess environment: force a C locale when none is generated (minimal Arch images print
    'bsdtar: Failed to set default locale' and locale-dependent tool output otherwise)."""
    env = dict(os.environ)
    env.setdefault("LC_ALL", "C")
    env.setdefault("LANG", "C")
    return env


def run(cmd, quiet: bool = False, **kw) -> None:
    """Run a command with live, tee'd output (classified per line into the log).

    Raises CommandError on failure so callers can add context; the top level turns it into a clean
    FAILED message plus the log path instead of a traceback.
    """
    cmdline = " ".join(str(c) for c in cmd)
    LOG.verbose("+ " + cmdline)
    popen_kw = dict(kw)
    popen_kw.pop("check", None)
    popen_kw.pop("stdout", None)
    popen_kw.pop("stderr", None)
    popen_kw.setdefault("env", _child_env())
    p = subprocess.Popen([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace", bufsize=1, **popen_kw)
    assert p.stdout is not None
    for line in p.stdout:
        if quiet:
            LOG.raw(line)
        else:
            LOG.process_line(line)
            if VIEW is not None:
                VIEW.feed(line)   # drives the progress bar from ninja's [n/m]
    p.wait()
    if p.returncode != 0:
        raise CommandError(cmdline, p.returncode)


def run_capture(cmd) -> str:
    LOG.verbose("+ " + " ".join(str(c) for c in cmd) + "  (captured)")
    try:
        r = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, errors="replace",
                           env=_child_env())
        return r.stdout or ""
    except OSError:
        return ""


def has_tty() -> bool:
    try:
        return sys.stdin.isatty() and sys.stdout.isatty()
    except Exception:   # noqa: BLE001
        return False


def _ask(prompt: str) -> str:
    """Read a line; when the TUI owns the screen it suspends/resumes around the prompt."""
    if VIEW is not None and getattr(VIEW, "tui", False):
        return VIEW.prompt(prompt)
    return input(prompt)


def ask_yes_no(ctx: "Context", question: str, default: bool = True) -> bool:
    """Interactive yes/no. -y answers yes, --non-interactive (or no TTY) answers the default."""
    if ctx.args.yes:
        print(f"{question} [auto-yes]")
        return True
    if not ctx.interactive:
        print(f"{question} [non-interactive -> {'yes' if default else 'no'}]")
        return default
    suffix = "[Y/n]" if default else "[y/N]"
    try:
        ans = _ask(f"{question} {suffix} ").strip().lower()
    except EOFError:
        return default
    if not ans:
        return default
    return ans in ("y", "yes", "s", "si", "sí")


def ask_path(ctx: "Context", prompt: str, what: str) -> Optional[Path]:
    """Interactive path prompt. Returns None in non-interactive mode (caller decides how to fail)."""
    if not ctx.interactive or ctx.args.yes:
        return None
    try:
        raw = _ask(f"{prompt} ").strip().strip('"').strip("'")
    except EOFError:
        return None
    if not raw:
        return None
    p = Path(raw).expanduser()
    if not p.exists():
        print(f"  {what} not found: {p}")
        return None
    return p.resolve()


def ask_destination(ctx: "Context", default_dir: Path) -> Optional[Path]:
    """Ask where to send the finished release artifact (created if missing).
    Enter accepts the default; 'skip' (or an empty answer for the empty default) copies nothing.
    Non-interactive returns None; -y accepts the default."""
    if not ctx.interactive:
        return None
    if ctx.args.yes:
        return default_dir
    try:
        raw = _ask(
            f"Where should the compressed release go? "
            f"(Enter = {default_dir}, 'skip' = leave it in {default_dir.parent}) "
        ).strip()
    except EOFError:
        return None
    raw = raw.strip().strip('"').strip("'")
    if raw.lower() in ("skip", "no", "n", "-"):
        return None
    dest = Path(raw).expanduser() if raw else default_dir
    try:
        dest.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        warn(f"cannot use destination {dest}: {e}")
        return None
    return dest.resolve()


# ------------------------------------------------------------------------------------------------
# Filesystem helpers
# ------------------------------------------------------------------------------------------------
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
    LOG.info(f"synced {src} -> {dst} ({copied} updated, {len(src_names)} total)")


def copytree_overlay(src: Path, dst: Path) -> None:
    """copy_tree-like that overwrites instead of failing on existing dirs."""
    if src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        for child in src.iterdir():
            copytree_overlay(child, dst / child.name)
    elif src.is_file():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


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
    return (None, None)


def find_binary(name: str) -> Path:
    exe = name + (".exe" if os.name == "nt" else "")
    hits = sorted(BUILD.rglob(exe))
    if not hits:
        die(f"{exe} not found under {BUILD} after build")
    return hits[0]


# ------------------------------------------------------------------------------------------------
# Stage 1: platform + dependency detection
# ------------------------------------------------------------------------------------------------
def _cmake_version() -> Optional[tuple[int, int]]:
    if not shutil.which("cmake"):
        return None
    txt = run_capture(["cmake", "--version"])
    for line in txt.splitlines():
        if line.lower().startswith("cmake version"):
            parts = line.split("version", 1)[1].strip().split()[0].split(".")
            try:
                return (int(parts[0]), int(parts[1]))
            except (ValueError, IndexError):
                return None
    return None


def _distro_name() -> str:
    try:
        txt = Path("/etc/os-release").read_text(errors="replace")
    except OSError:
        return ""
    for line in txt.splitlines():
        if line.startswith("ID="):
            return line.split("=", 1)[1].strip().strip('"')
    return ""


def _detect_pkg_mgr() -> Optional[str]:
    """First package manager present, in preference order. dnf before yum (yum is a shim on Fedora),
    apt before nothing else on Debian-likes."""
    for name, probe in (
        ("apt", "apt-get"), ("dnf", "dnf"), ("yum", "yum"), ("pacman", "pacman"),
        ("zypper", "zypper"), ("apk", "apk"), ("xbps", "xbps-install"),
        ("eopkg", "eopkg"), ("emerge", "emerge"),
    ):
        if shutil.which(probe):
            return name
    return None


def _distro_pretty() -> str:
    try:
        txt = Path("/etc/os-release").read_text(errors="replace")
    except OSError:
        return ""
    for line in txt.splitlines():
        if line.startswith("PRETTY_NAME="):
            return line.split("=", 1)[1].strip().strip('"')
    return ""


def _qt_prefix() -> Optional[Path]:
    for env in ("QT_ROOT", "QTDIR", "CMAKE_PREFIX_PATH"):
        v = os.environ.get(env)
        if not v:
            continue
        for candidate in v.split(os.pathsep):
            p = Path(candidate)
            if (p / "lib" / "cmake" / "Qt6").is_dir() or (p / "bin" / "qmake6").exists():
                return p
    candidates = [
        ROOT / "build" / "qt" / QT_VERSION / QT_KIT_WINDOWS,
        Path.home() / "Qt" / QT_VERSION / QT_KIT_WINDOWS,
    ]
    # Any kit already unpacked under build/qt/<ver>/<kit> counts, but only on Windows: those kits are
    # MSVC builds and unusable on Linux/macOS.
    if os.name == "nt" and (ROOT / "build" / "qt").is_dir():
        candidates += sorted((ROOT / "build" / "qt").glob(f"*/{QT_KIT_WINDOWS}"))
        candidates += sorted((ROOT / "build" / "qt").glob("*/*"))
    if sys.platform == "darwin":
        brew = shutil.which("brew")
        if brew:
            prefix = run_capture([brew, "--prefix", "qt"]).strip()
            if prefix:
                candidates.insert(0, Path(prefix))
    for c in candidates:
        if (c / "lib" / "cmake" / "Qt6").is_dir() or (c / "bin" / "qmake6").exists():
            return c
    for p in (Path("/usr/lib/cmake/Qt6"), Path("/usr/lib/x86_64-linux-gnu/cmake/Qt6")):
        if p.is_dir():
            return p.parent.parent
    return None


def _mesa_dir_present(c: Optional[Path]) -> bool:
    return bool(c) and ((c / "vulkan_lvp.dll").exists() or (c / "lavapipe" / "vulkan_lvp.dll").exists())


def _vswhere() -> Optional[str]:
    on_path = shutil.which("vswhere")
    if on_path:
        return on_path
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if not base:
            continue
        p = Path(base) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if p.exists():
            return str(p)
    return None


def _msvc_toolset_present() -> bool:
    """True when a VC toolset (and preferably clang-cl) is installed, even outside a dev prompt."""
    if os.environ.get("VCToolsInstallDir"):
        return True
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if not base:
            continue
        for edition in ("BuildTools", "Community", "Professional", "Enterprise"):
            vc = Path(base) / "Microsoft Visual Studio" / "2022" / edition / "VC" / "Auxiliary" / "Build"
            if (vc / "vcvars64.bat").exists():
                return True
    return False



# --- hardware auto-tune ---------------------------------------------------------------------------
# The runner's generated translation units are huge: the historical -j3 default assumes ~16 GB of
# RAM (see DEFAULT_JOBS). Auto-tune picks the largest job count that stays inside a safe memory
# budget and only enables optional accelerators that are actually installed. Everything is a
# suggestion: --no-autotune, an explicit --jobs N, or a failed probe fall back to the safe defaults.
MEM_PER_JOB_GB = 4.0    # budget per parallel compile; a 16 GB machine lands on the historical -j3
RAM_RESERVE_GB = 3.0    # leave room for the OS, the recompiler and the final link


@dataclass
class Hardware:
    logical_cores: int
    physical_cores: int
    ram_gb: Optional[float]     # None when it cannot be probed -> stay conservative

    @property
    def ram_known(self) -> bool:
        return self.ram_gb is not None


def _cpu_cores() -> tuple:
    """(logical, physical) CPU cores. physical degrades to logical when it cannot be read."""
    logical = os.cpu_count() or 1
    physical = logical
    try:
        if sys.platform.startswith("linux"):
            seen = set()
            cur: dict = {}
            for line in Path("/proc/cpuinfo").read_text(errors="replace").splitlines() + [""]:
                if not line.strip():
                    if cur:
                        seen.add((cur.get("physical id", "0"),
                                  cur.get("core id", cur.get("processor", "0"))))
                        cur = {}
                    continue
                if ":" in line:
                    k, v = line.split(":", 1)
                    cur[k.strip()] = v.strip()
            if seen:
                physical = len(seen)
        elif sys.platform == "darwin":
            out = run_capture(["sysctl", "-n", "hw.physicalcpu"]).strip()
            if out.isdigit():
                physical = int(out)
    except Exception:   # noqa: BLE001 - a failed probe must never break the build
        physical = logical
    return logical, max(1, min(physical, logical))


def _ram_gb() -> Optional[float]:
    """Total usable RAM in GB, capped by the cgroup limit inside a container. None when unknown."""
    total = None
    try:
        if sys.platform.startswith("linux"):
            for line in Path("/proc/meminfo").read_text(errors="replace").splitlines():
                if line.startswith("MemTotal:"):
                    total = int(line.split()[1]) / (1024 * 1024)
                    break
            for cg in ("/sys/fs/cgroup/memory.max",
                       "/sys/fs/cgroup/memory/memory.limit_in_bytes"):
                try:
                    raw = Path(cg).read_text().strip()
                except OSError:
                    continue
                if raw and raw != "max" and raw.isdigit():
                    limit = int(raw) / (1024 ** 3)
                    total = min(total, limit) if total else limit
                    break
        elif sys.platform == "darwin":
            out = run_capture(["sysctl", "-n", "hw.memsize"]).strip()
            if out.isdigit():
                total = int(out) / (1024 ** 3)
        elif os.name == "nt":
            import ctypes

            class _MemStatus(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                            ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                            ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                            ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                            ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
            st = _MemStatus()
            st.dwLength = ctypes.sizeof(_MemStatus)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
                total = st.ullTotalPhys / (1024 ** 3)
    except Exception:   # noqa: BLE001
        return None
    return total if total and total > 0 else None


def _ccache_available() -> bool:
    """ccache as a launcher only when sccache is absent (the runtime prefers sccache itself)."""
    return bool(shutil.which("ccache")) and not shutil.which("sccache")


def detect_hardware() -> Hardware:
    logical, physical = _cpu_cores()
    return Hardware(logical, physical, _ram_gb())


def plan_build(hw: Hardware) -> tuple:
    """(jobs, unity_batch, use_ccache): the largest values that stay within the RAM budget.
    jobs is None when RAM could not be probed (the caller keeps DEFAULT_JOBS)."""
    if not hw.ram_known:
        return None, 8, _ccache_available()
    budget = max(0.0, hw.ram_gb - RAM_RESERVE_GB)
    by_mem = int(budget // MEM_PER_JOB_GB)
    jobs = max(1, min(hw.physical_cores, by_mem)) if by_mem >= 1 else 1
    # Very low RAM: smaller unity batches keep a single compile within budget.
    batch = 4 if hw.ram_gb < 12 else 8
    return jobs, batch, _ccache_available()


@dataclass
class PlatformInfo:
    os: str                 # windows | linux | macos
    arch: str
    distro: str
    pkg_mgr: Optional[str]
    in_container: bool
    interactive: bool
    tools: dict = field(default_factory=dict)      # name -> description/path/None
    qt_prefix: Optional[Path] = None
    lavapipe_dir: Optional[Path] = None
    unity_batch: Optional[int] = None              # auto-tune: unity batch, None = CMake default
    use_ccache: bool = False                       # auto-tune: ccache launcher when sccache is absent

    @property
    def is_windows(self) -> bool:
        return self.os == "windows"

    @property
    def is_macos(self) -> bool:
        return self.os == "macos"

    def exe(self, name: str) -> str:
        return name + ".exe" if self.is_windows else name

    # --- toolchain -------------------------------------------------------------------------------
    def vs_dev_prompt(self) -> bool:
        """True when this shell has the MSVC environment loaded (vcvars) or clang-cl available."""
        return bool(os.environ.get("VCToolsInstallDir") or os.environ.get("INCLUDE"))

    def windows_generator_flags(self, build_dir: Path) -> list:
        """Generator/toolset flags for a Windows configure. An existing cache dictates the generator:
        a Ninja cache must not get -T (Visual Studio only), and a VS cache must not get -G Ninja."""
        cache = build_dir / "CMakeCache.txt"
        if cache.exists():
            gen = ""
            for line in cache.read_text(errors="replace").splitlines():
                if line.startswith("CMAKE_GENERATOR:"):
                    gen = line.split("=", 1)[1]
                    break
            if "Visual Studio" not in gen:
                return []
        # Prefer Ninja + clang-cl in a developer prompt: Ninja compiles every file in parallel,
        # whereas the Visual Studio generator hands the runner's ~1000 unity units to MSBuild.
        if (os.environ.get("PS2X_SETUP_GENERATOR", "ninja").lower() != "vs"
                and self.vs_dev_prompt() and shutil.which("ninja") and shutil.which("clang-cl")):
            return ["-G", "Ninja", "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
        return ["-T", os.environ.get("PS2X_SETUP_TOOLSET", "ClangCL")]

    def configure_extra(self, build_dir: Path) -> list:
        """CMake flags for this platform. Mirrors the historical setup.py behaviour."""
        extra = ["-DCMAKE_BUILD_TYPE=Release"]   # a Windows Ninja/clang-cl configure once came up Debug
        # Performance auto-tune (see plan_build). Only non-default values are emitted so the cache
        # stays clean and a rebuild only reconfigures when something actually changed.
        if self.unity_batch and self.unity_batch != 8:
            extra.append(f"-DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE={self.unity_batch}")
        if self.use_ccache:
            extra += ["-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                      "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"]
        if self.is_macos:
            extra += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
            # paraLLEl-GS does not build on macOS (Granite's sleep_until_nsecs has no Darwin path).
            if os.environ.get("PS2X_SETUP_PGS") != "1":
                extra.append("-DPS2X_DISABLE_PGS=ON")
            if os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
                extra.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=" + os.environ["MACOSX_DEPLOYMENT_TARGET"])
        if not self.is_windows:
            # Prefer Ninja on Linux/macOS too: the default generator is Unix Makefiles, which minimal
            # distros (Arch) do not ship, and CMake then fails with "unable to find a build program".
            if not (build_dir / "CMakeCache.txt").exists() and shutil.which("ninja"):
                extra += ["-G", "Ninja"]
            return extra
        # GUI subsystem for the shipped runner: without this the fresh configure produces a console
        # binary and the PE gate rejects it (a console window pops up next to the game). The old
        # build-windows.ps1 pre-configured this; PS2X_SETUP_CONSOLE=1 keeps the console for debugging.
        extra.append("-DPS2X_SHOW_WINDOWS_CONSOLE="
                     + ("ON" if os.environ.get("PS2X_SETUP_CONSOLE") == "1" else "OFF"))
        return extra + self.windows_generator_flags(build_dir)


def detect_platform() -> PlatformInfo:
    if os.name == "nt":
        osname = "windows"
    elif sys.platform == "darwin":
        osname = "macos"
    else:
        osname = "linux"
    arch = _platform.machine().lower() or "unknown"
    in_container = (not has_tty()) or Path("/.dockerenv").exists() or \
        bool(os.environ.get("BT3_IN_CONTAINER"))
    info = PlatformInfo(
        os=osname,
        arch=arch,
        distro=_distro_name() if osname == "linux" else "",
        pkg_mgr=_detect_pkg_mgr() if osname == "linux" else ("brew" if osname == "macos" else "winget"),
        in_container=in_container,
        interactive=has_tty() and not in_container,
    )
    cmv = _cmake_version()
    info.tools["cmake"] = (f"{cmv[0]}.{cmv[1]}" if cmv else None)
    info.tools["cmake_ok"] = "yes" if (cmv and cmv >= CMAKE_MIN) else "no"
    for name in ("ninja", "git", "clang", "clang-cl", "clang++", "bsdtar", "7z", "pkg-config", "ccache", "mold"):
        info.tools[name] = shutil.which(name)
    info.qt_prefix = _qt_prefix()
    info.tools["qt"] = str(info.qt_prefix) if info.qt_prefix else None
    if osname == "windows":
        info.tools["vswhere"] = _vswhere()
        info.tools["msvc"] = "yes" if _msvc_toolset_present() else None
        info.tools["vcvars"] = "loaded" if info.vs_dev_prompt() else None
        for c in (ROOT / "build" / "mesa" / "x64",
                  Path(os.environ["PS2X_MESA_DIR"]) if os.environ.get("PS2X_MESA_DIR") else None):
            if _mesa_dir_present(c):
                info.lavapipe_dir = c
                break
    return info


def print_platform_report(info: PlatformInfo) -> None:
    distro = f" distro={info.distro}" if info.distro else ""
    pretty = _distro_pretty() if info.os == "linux" else ""
    if pretty and f'"{pretty}"' != f'"{info.distro}"':
        distro += f' ({pretty})'
    LOG.info(f"Platform : {info.os} ({info.arch}){distro}")
    LOG.info(f"Package  : {info.pkg_mgr or 'not detected'}"
          + ("  [container/non-interactive]" if info.in_container else ""))
    LOG.info(f"Build dir: {BUILD}")
    LOG.info(f"Work dir : {WORK}")
    LOG.info("Tools:")
    for name in ("cmake", "cmake_ok", "ninja", "git", "clang", "clang-cl", "bsdtar", "7z",
                 "pkg-config", "ccache", "mold", "vswhere", "msvc", "vcvars", "qt"):
        if name not in info.tools:
            continue
        value = info.tools[name]
        mark = "OK " if value else "-- "
        LOG.info(f"  {mark}{name:<11} {value or 'missing'}")


def stage_detect(ctx: "Context") -> None:
    step("stage 1: platform detection")
    if VIEW is not None:
        p = ctx.platform
        t = p.tools
        have = [k for k, v in t.items() if v]
        VIEW.stage(1, 4, "detect")
        VIEW.item(1, 3, "Platform", "ok", f"{p.os} {p.arch}")
        VIEW.item(2, 3, "Package manager", "ok" if p.pkg_mgr else "skip", p.pkg_mgr or "none")
        VIEW.item(3, 3, "Toolchain probe", "ok" if have else "miss", ", ".join(have[:6]) or "none")
    print_platform_report(ctx.platform)
    if ctx.args.report == "json":
        LOG.info(json.dumps({
            "os": ctx.platform.os, "arch": ctx.platform.arch, "distro": ctx.platform.distro,
            "pkg_mgr": ctx.platform.pkg_mgr, "in_container": ctx.platform.in_container,
            "tools": {k: (str(v) if v is not None else None) for k, v in ctx.platform.tools.items()},
        }, indent=2))


# ------------------------------------------------------------------------------------------------
# Stage 2: dependencies
# ------------------------------------------------------------------------------------------------
@dataclass
class Dep:
    name: str
    check: Callable[[PlatformInfo], bool]
    hint: str            # what to install (shown to the user)
    install: Optional[Callable[["Context"], None]] = None   # filled in stage 2 execution
    optional: bool = False   # a failure warns instead of stopping the build
    droppable: Optional["Droppable"] = None   # [deps-ui] a release archive the user can drop on us
    needs_admin: bool = False                 # [deps-ui] the only one: VS Build Tools


def _have(name: str) -> Callable[[PlatformInfo], bool]:
    return lambda info: bool(shutil.which(name))


def _download(url: str, dst: Path) -> None:
    import urllib.request
    print(f"+ download {url} -> {dst}")
    dst.parent.mkdir(parents=True, exist_ok=True)

    def hook(blocks, bs, total):
        if total > 0:
            print(f"\r  {min(100, blocks * bs * 100 // total):3d}%", end="", flush=True)

    urllib.request.urlretrieve(url, dst, reporthook=hook)
    print()


def _inst_winget(ctx: "Context", pkg_id: str, override: Optional[str] = None) -> None:
    exe = shutil.which("winget")
    if not exe:
        raise RuntimeError("winget not found (install 'App Installer' from the Microsoft Store)")
    cmd = [exe, "install", "--id", pkg_id, "--accept-package-agreements",
           "--accept-source-agreements", "--silent", "--disable-interactivity"]
    if override:
        cmd += ["--override", override]
    run(cmd)


def _inst_pip(ctx: "Context", module: str) -> None:
    run([sys.executable, "-m", "pip", "install", "--upgrade", module])


def _pick_qt_kit() -> str:
    """The Windows Qt kit aqt actually offers for QT_VERSION. 6.5.x has no msvc2022_64, and a hard pin
    made the install fail with 'packages [qt_base] were not found'."""
    archs = run_capture([sys.executable, "-m", "aqt", "list-qt", "windows", "desktop",
                         "--arch", QT_VERSION]).split()
    if QT_KIT_WINDOWS in archs:
        return QT_KIT_WINDOWS
    for pref in ("win64_msvc2022_64", "win64_msvc2019_64"):
        if pref in archs:
            return pref
    for a in archs:
        if a.startswith("win64_msvc") and a.endswith("_64"):
            return a
    return QT_KIT_WINDOWS


def _inst_aqt(ctx: "Context") -> None:
    kit = _pick_qt_kit()
    LOG.info(f"  aqt kit: {kit}")
    run([sys.executable, "-m", "aqt", "install-qt", "windows", "desktop",
         QT_VERSION, kit, "--outputdir", str(ROOT / "build" / "qt")])


def _inst_mesa(ctx: "Context") -> None:
    mesa_dir = ROOT / "build" / "mesa"
    seven = mesa_dir / "7zr.exe"
    if not seven.exists():
        _download(DEPS_7ZR_URL, seven)
    archive = mesa_dir / "mesa.7z"
    _download(f"https://github.com/pal1000/mesa-dist-win/releases/download/"
              f"{MESA_LAVAPIPE_VERSION}/mesa3d-{MESA_LAVAPIPE_VERSION}-release-msvc.7z", archive)
    run([seven, "x", archive, f"-o{mesa_dir}", "-y"])
    archive.unlink(missing_ok=True)


# --- Windows: portable / droppable dependency installs ------------------------------------------
# CMake, Ninja and Mesa ship PORTABLE archives, so a dependency that winget cannot deliver can be
# installed by simply extracting the release the user downloads: no admin, no PATH/registry edit,
# no winget. The archive is unpacked under build/tools (or build/mesa) and the tool is used by
# absolute path for the rest of the run.
@dataclass
class Droppable:
    url_page: str      # releases page to send the user to
    filename: str      # the concrete file to grab there (shown to the user)
    glob: str          # accepted dropped file names (fnmatch, lowercase)
    dest: Path         # where to extract
    artifact: str      # glob that must exist afterwards (the exe / dll)
    run_installer: bool = False   # .exe/.msi that must be RUN (interactive) instead of extracted


def _module_ok(module: str) -> bool:
    """Honest check: import it in THIS interpreter. `pip show` lies -- a broken/partial dist-info
    (or a Microsoft Store Python) passes it while `python -m <mod>` fails."""
    try:
        r = subprocess.run([sys.executable, "-c", f"import {module}"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode == 0
    except OSError:
        return False


def _is_store_python() -> bool:
    """True for the Microsoft Store Python (App Execution Alias under WindowsApps). Seen on a user
    box: `pip show aqtinstall` was OK but `python -m aqt` -> 'No module named aqt'."""
    exe = (sys.executable or "").lower()
    return "windowsapps" in exe or "pythonsoftwarefoundation" in exe


def _extract_archive(archive: Path, dest: Path) -> None:
    import zipfile
    name = archive.name.lower()
    dest.mkdir(parents=True, exist_ok=True)
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    elif name.endswith(".7z"):
        seven = BUILD / "mesa" / "7zr.exe"
        if not seven.exists():
            _download(DEPS_7ZR_URL, seven)
        run([seven, "x", archive, f"-o{dest}", "-y"])
    elif name.endswith(".whl"):
        run([sys.executable, "-m", "pip", "install", str(archive)])
    else:
        raise RuntimeError(f"unsupported archive type: {archive.name}")


def _install_dropped(d: "Dep", archive: Path) -> None:
    """Install a dependency from a user-dropped archive. Raises on anything unexpected; the caller
    turns that into a message, never an abort."""
    import fnmatch
    spec = d.droppable
    if spec is None:
        raise RuntimeError(f"{d.name} has no drop-in archive; {d.hint}")
    name = archive.name.lower()
    if name.endswith((".exe", ".msi")):
        # An official installer (Qt): run it interactively, then the caller re-checks the dep.
        run([str(archive)] if name.endswith(".exe") else ["msiexec", "/i", str(archive)])
        return
    if not fnmatch.fnmatch(name, spec.glob.lower()):
        warn(f"{archive.name} does not look like {spec.glob}; trying anyway")
    _extract_archive(archive, spec.dest)
    hits = list(spec.dest.glob(spec.artifact))
    if not hits:
        raise RuntimeError(f"{d.name}: '{spec.artifact}' not found after extracting {archive.name}")
    hit = hits[0]
    if hit.suffix.lower() == ".exe":
        os.environ["PATH"] = str(hit.parent) + os.pathsep + os.environ.get("PATH", "")
    LOG.info(f"  {d.name}: installed from {archive.name} -> {hit.parent}")


def _ask_dep_action(name: str) -> str:
    """The action bar for a failed dependency: retry / abort / dump log / continue."""
    try:
        raw = _ask(f"  [{name}] [R]etry  [A]bort  [D]ump log  [C]ontinue : ").strip().lower()
    except EOFError:
        return "c"
    if raw in ("r", "retry"):
        return "r"
    if raw in ("a", "abort", "q", "quit"):
        return "a"
    if raw in ("d", "dump"):
        return "d"
    return "c"


def _dump_log_tail(n: int = 20) -> None:
    if not LOG.path or not Path(LOG.path).exists():
        print("  (no log file)")
        return
    lines = Path(LOG.path).read_text(errors="replace").splitlines()
    print(f"  ---- last {min(n, len(lines))} lines of {LOG.path} ----")
    for ln in lines[-n:]:
        print("  " + ln)


_PACMAN_READY = False


def _pacman_prepare(ctx: "Context") -> None:
    """A fresh Arch image (or a stale keyring) cannot install anything: pacman fails with 'keyring is
    not writable' / 'required key missing from keyring'. Probe, reset the keyring from the files
    shipped with pacman-key when needed, then upgrade."""
    global _PACMAN_READY
    if _PACMAN_READY:
        return
    _PACMAN_READY = True
    need_sudo = hasattr(os, "geteuid") and os.geteuid() != 0
    sudo = ["sudo"] if need_sudo else []
    gnupg = Path("/etc/pacman.d/gnupg")

    def _try(argv) -> bool:
        LOG.verbose("+ " + " ".join(argv) + "  (probe)")
        try:
            return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, errors="replace", env=_child_env()).returncode == 0
        except OSError:
            return False

    if not _try(sudo + ["pacman", "-Sy", "--noconfirm", "--disable-download-timeout"]):
        print("  pacman: resetting the keyring (fresh install)")
        run(sudo + ["rm", "-rf", str(gnupg)])
        run(sudo + ["pacman-key", "--init"])
        run(sudo + ["pacman-key", "--populate", "archlinux"])
        run(sudo + ["pacman", "-Sy", "--noconfirm", "--disable-download-timeout"])
    step("pacman: upgrading the system (first install on this machine)")
    run(sudo + ["pacman", "-Syu", "--noconfirm", "--needed", "--disable-download-timeout"])


def _inst_pkg(ctx: "Context", group: str) -> None:
    """Install a distro package group with the detected manager (sudo when not root)."""
    mgr = ctx.platform.pkg_mgr or "apt"
    packages = LINUX_GROUPS.get(mgr, {}).get(group)
    if not packages:
        if mgr == "emerge":
            raise RuntimeError(f"Gentoo is not automated; run manually:\n    {GENTOO_HINT}")
        raise RuntimeError(f"no package mapping for {mgr}/{group}")
    install = {
        "apt": ["apt-get", "install", "-y"],
        "dnf": ["dnf", "install", "-y"],
        "yum": ["yum", "install", "-y"],
        "pacman": ["pacman", "-S", "--noconfirm", "--needed"],
        "zypper": ["zypper", "--non-interactive", "install"],
        "apk": ["apk", "add"],
        "xbps": ["xbps-install", "-y"],
        "eopkg": ["eopkg", "install", "-y"],
    }.get(mgr)
    if install is None:
        raise RuntimeError(f"unsupported package manager: {mgr}")
    if mgr == "pacman":
        _pacman_prepare(ctx)
    refresh = {
        "apt": ["apt-get", "update"],
        "apk": ["apk", "update"],
        "xbps": ["xbps-install", "-S"],
    }.get(mgr)
    need_sudo = hasattr(os, "geteuid") and os.geteuid() != 0
    if refresh:
        run((["sudo"] if need_sudo else []) + refresh)
    run((["sudo"] if need_sudo else []) + install + packages.split())


def _inst_brew(ctx: "Context", packages: str) -> None:
    run(["brew", "install"] + packages.split())


def _inst_xcode(ctx: "Context") -> None:
    run(["xcode-select", "--install"])


def _deps_for_platform(info: PlatformInfo) -> list[Dep]:
    """The dependency list for this platform. Checks are real (versions/kits), not just PATH probes."""
    if info.is_windows:
        cmv = _cmake_version()
        vs_components = ("--quiet --wait --norestart --nocache "
                         "--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
                         "--add Microsoft.VisualStudio.Component.VC.Llvm.Clang "
                         "--add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset "
                         "--add Microsoft.VisualStudio.Component.Windows11SDK.22621")
        return [
            Dep("Visual Studio Build Tools + ClangCL",
                lambda i: i.tools.get("msvc") == "yes" or bool(i.tools.get("vswhere")),
                f"winget install Microsoft.VisualStudio.2022.BuildTools --override \"{vs_components}\"",
                lambda ctx: _inst_winget(ctx, "Microsoft.VisualStudio.2022.BuildTools",
                                         override=vs_components),
                needs_admin=True),
            Dep(f"CMake >= {CMAKE_MIN[0]}.{CMAKE_MIN[1]}",
                lambda i: bool(cmv and cmv >= CMAKE_MIN),
                "winget install Kitware.CMake",
                lambda ctx: _inst_winget(ctx, "Kitware.CMake"),
                droppable=Droppable("https://github.com/Kitware/CMake/releases",
                                    "cmake-<ver>-windows-x86_64.zip", "cmake-*windows*.zip",
                                    BUILD / "tools" / "cmake", "**/bin/cmake.exe")),
            Dep("Ninja", _have("ninja"), "winget install Ninja-build.Ninja",
                lambda ctx: _inst_winget(ctx, "Ninja-build.Ninja"),
                droppable=Droppable("https://github.com/ninja-build/ninja/releases",
                                    "ninja-win.zip", "ninja-win*.zip",
                                    BUILD / "tools" / "ninja", "**/ninja.exe")),
            Dep("Python 3", _have("python"), "winget install Python.Python.3.12",
                lambda ctx: _inst_winget(ctx, "Python.Python.3.12")),
            Dep("aqtinstall (pip)",
                lambda i: _module_ok("aqt"),   # honest: `pip show` passes on a broken/Store Python
                "python -m pip install aqtinstall",
                lambda ctx: _inst_pip(ctx, "aqtinstall")),
            Dep("pefile (pip)",
                lambda i: _module_ok("pefile"),
                "python -m pip install pefile",
                lambda ctx: _inst_pip(ctx, "pefile")),
            Dep(f"Qt {QT_VERSION} ({QT_KIT_WINDOWS})",
                lambda i: bool(i.qt_prefix) or _qt_prefix() is not None,
                f"aqt install-qt windows desktop {QT_VERSION} {QT_KIT_WINDOWS} --outputdir build/qt",
                _inst_aqt,
                droppable=Droppable("https://www.qt.io/download-qt-installer",
                                    "qt-unified-windows-x64-*-online.exe", "qt-unified-windows*.exe",
                                    BUILD / "qt", "**/bin/qmake.exe")),
            Dep("Mesa lavapipe (Vulkan fallback)",
                lambda i: bool(i.lavapipe_dir) or _mesa_dir_present(ROOT / "build" / "mesa" / "x64"),
                f"download mesa-dist-win {MESA_LAVAPIPE_VERSION} and extract to build/mesa",
                _inst_mesa,
                droppable=Droppable("https://github.com/pal1000/mesa-dist-win/releases",
                                    f"mesa3d-{MESA_LAVAPIPE_VERSION}-release-msvc.7z", "mesa3d-*.7z",
                                    BUILD / "mesa", "**/vulkan_lvp.dll")),
        ]
    if info.is_macos:
        return [
            Dep("Xcode Command Line Tools", lambda i: bool(shutil.which("clang")),
                "xcode-select --install", _inst_xcode),
            Dep(f"CMake >= {CMAKE_MIN[0]}.{CMAKE_MIN[1]}",
                lambda i: bool(_cmake_version() and _cmake_version() >= CMAKE_MIN),
                "brew install cmake", lambda ctx: _inst_brew(ctx, "cmake")),
            Dep("Ninja", _have("ninja"), "brew install ninja", lambda ctx: _inst_brew(ctx, "ninja")),
            Dep("pkg-config", _have("pkg-config"), "brew install pkg-config",
                lambda ctx: _inst_brew(ctx, "pkg-config")),
            Dep("FFmpeg", lambda i: bool(shutil.which("pkg-config")) and
                subprocess.run(["pkg-config", "--exists", "libavcodec"], capture_output=True).returncode == 0,
                "brew install ffmpeg", lambda ctx: _inst_brew(ctx, "ffmpeg")),
            Dep("Qt 6", lambda i: bool(i.qt_prefix), "brew install qt",
                lambda ctx: _inst_brew(ctx, "qt")),
        ]
    mgr = info.pkg_mgr or "apt"
    if mgr == "emerge":
        return [
            Dep("Gentoo build packages",
                lambda i: all(shutil.which(t) for t in ("clang", "cmake", "ninja", "pkg-config")),
                GENTOO_HINT, None),
        ]
    groups = LINUX_GROUPS.get(mgr, {})
    pkg_hint = lambda group: f"{mgr} install {groups.get(group, '<packages>')}"   # noqa: E731
    deps = [
        Dep("build toolchain (clang/cmake/ninja/pkg-config)", lambda i: all(
                shutil.which(t) for t in ("clang", "cmake", "ninja", "pkg-config")),
            pkg_hint("toolchain"), lambda ctx: _inst_pkg(ctx, "toolchain")),
        Dep("FFmpeg dev libs", lambda i: bool(shutil.which("pkg-config")) and
            subprocess.run(["pkg-config", "--exists", "libavcodec"], capture_output=True).returncode == 0,
            pkg_hint("ffmpeg"), lambda ctx: _inst_pkg(ctx, "ffmpeg")),
        Dep("Qt 6 (launcher)", lambda i: bool(i.qt_prefix),
            pkg_hint("qt"), lambda ctx: _inst_pkg(ctx, "qt")),
    ]
    if groups.get("extras"):
        deps.append(Dep("build cache (ccache/mold, optional)",
                        lambda i: bool(shutil.which("ccache") or shutil.which("mold")),
                        pkg_hint("extras"), lambda ctx: _inst_pkg(ctx, "extras"), optional=True))
    return deps


def deps_for(info: PlatformInfo) -> list[Dep]:
    """The platform dependencies."""
    return _deps_for_platform(info)

def _is_admin() -> bool:
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:   # noqa: BLE001  (not Windows / no shell32)
        return True


def _dep_manual(d: "Dep") -> None:
    """Print where to get the dependency and where to put it (the last rung)."""
    LOG.info(f"    use: {d.hint}")
    if d.droppable is not None:
        LOG.info(f"    or download : {d.droppable.url_page}   ({d.droppable.filename})")
        LOG.info(f"       and drop : drag the file here, or copy it into {BUILD / 'deps-inbox'}")


def _try_drop(ctx: "Context", d: "Dep") -> bool:
    """The drop-in rung: print the link + filename, take a dragged path (or anything in the inbox)."""
    spec = d.droppable
    if spec is None or not ctx.interactive:
        return False
    inbox = BUILD / "deps-inbox"
    inbox.mkdir(parents=True, exist_ok=True)
    LOG.info(f"    download : {spec.url_page}   ({spec.filename})")
    LOG.info(f"    drop it  : drag the file here, or copy it into {inbox}")
    p = ask_path(ctx, "    file (Enter to skip):", "file")
    if p is None:
        cands = sorted(x for x in inbox.iterdir() if x.is_file())
        p = cands[0] if cands else None
    if p is None:
        return False
    try:
        _install_dropped(d, p)
    except Exception as e:   # noqa: BLE001
        warn(f"could not install {d.name} from {p.name}: {e}")
        return False
    try:   # remember it so a re-run does not ask again
        (BUILD / "deps-cache").mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, BUILD / "deps-cache" / p.name)
    except Exception:   # noqa: BLE001
        pass
    return bool(d.check(ctx.platform))


def stage_deps(ctx: "Context") -> None:
    """Report the platform dependencies, then resolve what is missing -- NEVER aborting on a single
    failure.

    Windows rung per dependency: honest check -> auto (winget/aqt) -> a dropped release archive
    (portable, no admin) -> manual instructions. A failure is accumulated and reported in the final
    RESULT; only the user's explicit Abort stops the run.
    """
    step("stage 2: dependencies")
    if VIEW is not None:
        VIEW.stage(2, 4, "dependencies" + (" (Windows)" if ctx.platform.is_windows else ""))
    deps = deps_for(ctx.platform)
    missing = [d for d in deps if not d.check(ctx.platform)]
    resolved, failed = [], []
    for idx, d in enumerate(deps, 1):
        done = d not in missing
        if done:
            resolved.append(d.name)
        if VIEW is not None:
            VIEW.item(idx, len(deps), d.name, "ok" if done else "miss")
        else:
            LOG.info(f"  {'OK ' if done else '-- '}{d.name}")
    ctx.deps_resolved, ctx.deps_failed = resolved, failed
    if not missing:
        LOG.info("All dependencies present.")
        if VIEW is not None:
            VIEW.summary_row("resolved", f"{len(resolved)}/{len(deps)}")
        return

    if ctx.args.dry_run or ctx.args.no_deps or ctx.args.check:
        LOG.info(f"{len(missing)} dependency(ies) missing (not installing):")
        for d in missing:
            LOG.info(f"  - {d.name}\n      {d.hint}")
        if VIEW is not None:
            for i, d in enumerate(missing, 1):
                VIEW.item(i, len(missing), d.name, "skip" if d.optional else "miss")
                if not d.optional:
                    VIEW.failed(d.name,
                                d.droppable.url_page if d.droppable else "",
                                d.droppable.filename if d.droppable else "",
                                f"drag the file here, or copy it into {BUILD / 'deps-inbox'}")
        return

    if _is_store_python():
        warn("this is the Microsoft Store Python: `pip show` can say a module is installed while "
             "`python -m <mod>` cannot import it. If that happens, install Python from python.org "
             "(or `winget install Python.Python.3.12`).")

    total = len(missing)
    for n, d in enumerate(missing, 1):
        if VIEW is not None:
            VIEW.item(n, total, d.name, "running")
            VIEW.summary_row("resolved", str(len(resolved)))
            VIEW.summary_row("failed", str(len(failed)))
        if d.needs_admin and not _is_admin():
            warn(f"{d.name} needs administrator rights; run the terminal as administrator for it.")
        if not ctx.interactive and not ctx.args.yes and not ctx.args.install_deps:
            failed.append(d)
            if VIEW is not None:
                VIEW.item(n, total, d.name, "fail")
            LOG.info(f"  cannot install {d.name} non-interactively; {d.hint}")
            continue
        while True:
            if not ask_yes_no(ctx, f"Install {d.name} now?", default=True):
                break
            ok = False
            if d.install is not None:
                try:
                    d.install(ctx)
                    ok = d.check(ctx.platform)
                except Exception as e:   # noqa: BLE001
                    warn(f"automatic install of {d.name} failed: {e}")
            if not ok:
                ok = _try_drop(ctx, d)
            if ok:
                resolved.append(d.name)
                if VIEW is not None:
                    VIEW.item(n, total, d.name, "ok")
                LOG.info(f"  installed: {d.name}")
                break
            _dep_manual(d)
            if VIEW is not None:
                VIEW.failed(d.name,
                            d.droppable.url_page if d.droppable else "",
                            d.droppable.filename if d.droppable else "",
                            f"drag the file here, or copy it into {BUILD / 'deps-inbox'}")
            action = "a" if (not ctx.interactive or ctx.args.yes) else _ask_dep_action(d.name)
            if action == "r":
                continue
            if action == "d":
                _dump_log_tail()
                continue
            if action == "a":
                die(f"aborted at dependency: {d.name}\n  {d.hint}", 2, stage="2 deps")
            failed.append(d)
            if VIEW is not None:
                VIEW.item(n, total, d.name, "fail")
            break

    # detect_platform() cached qt_prefix before deps ran; aqt or a dropped Qt installer may have made
    # Qt available since, so refresh it for the rest of this run (launcher config / Windows bundle).
    ctx.platform.qt_prefix = _qt_prefix() or ctx.platform.qt_prefix
    if ctx.platform.qt_prefix:
        ctx.platform.tools["qt"] = str(ctx.platform.qt_prefix)


# ------------------------------------------------------------------------------------------------
# Stage 3: build pipeline
# ------------------------------------------------------------------------------------------------
def _vs_env_loaded() -> bool:
    return bool(os.environ.get("VCToolsInstallDir") or os.environ.get("INCLUDE"))


_VS_ENV_TRIED = False


def ensure_msvc_env(ctx: "Context") -> None:
    """Import the MSVC environment (vcvars64.bat) into this process so CMake/clang-cl work from a
    plain shell -- the launcher/runtime configure fails with -T ClangCL without it. No-op off Windows
    and when a developer prompt is already loaded."""
    global _VS_ENV_TRIED
    if not ctx.platform.is_windows or _vs_env_loaded():
        return
    if _VS_ENV_TRIED:
        return
    _VS_ENV_TRIED = True
    cands = []
    vswhere = _vswhere()
    if vswhere:
        txt = run_capture([vswhere, "-latest", "-products", "*", "-requires",
                           "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                           "-property", "installationPath"])
        install = txt.strip().splitlines()[0].strip() if txt.strip() else ""
        if install:
            cands.append(Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat")
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if not base:
            continue
        for edition in ("BuildTools", "Community", "Professional", "Enterprise"):
            cands.append(Path(base) / "Microsoft Visual Studio" / "2022" / edition /
                         "VC" / "Auxiliary" / "Build" / "vcvars64.bat")
    vcvars = next((c for c in cands if c.exists()), None)
    if not vcvars:
        warn("vcvars64.bat not found; if the configure fails, open an 'x64 Native Tools Command Prompt'")
        return
    step(f"loading the MSVC environment ({vcvars})")
    # A temporary .bat that calls vcvars and dumps the environment: quoting a path with spaces through
    # `cmd /c` silently produces nothing, which left INCLUDE unset and forced the MSBuild fallback.
    import tempfile
    bat = Path(tempfile.gettempdir()) / "bt3_vcenv.bat"
    bat.write_text(f'@echo off\r\ncall "{vcvars}" >nul 2>&1\r\nset\r\n', encoding="ascii")
    try:
        out = run_capture([str(bat)])
    finally:
        bat.unlink(missing_ok=True)
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            os.environ[k] = v
    # clang-cl lives in the VS LLVM toolset, which vcvars64 does not add to PATH. Without it the
    # Windows configure falls back to the Visual Studio generator + -T ClangCL (and, when the ClangCL
    # integration is absent, fails with MSB8020).
    if not shutil.which("clang-cl"):
        bin_dirs = []
        if vswhere:
            for hit in run_capture([vswhere, "-latest", "-products", "*", "-find",
                                    r"VC\Tools\Llvm\x64\bin\clang-cl.exe"]).splitlines():
                hit = hit.strip()
                if hit:
                    bin_dirs.append(Path(hit).parent)
        install = vcvars.parents[3]
        bin_dirs += [install / "VC" / "Tools" / "Llvm" / "x64" / "bin",
                     install / "VC" / "Tools" / "Llvm" / "bin"]
        for b in bin_dirs:
            if (b / "clang-cl.exe").exists():
                os.environ["PATH"] = str(b) + os.pathsep + os.environ.get("PATH", "")
                LOG.info(f"  clang-cl: added {b} to PATH")
                break
        else:
            warn("clang-cl not found; the configure will fall back to MSBuild + -T ClangCL "
                 "(install the 'C++ Clang Compiler for Windows' component)")


def configured(build_dir: Path, info: PlatformInfo) -> bool:
    """True when the build dir holds a COMPLETED configure. CMakeCache.txt alone is not enough:
    a half-failed configure leaves the cache behind and `cmake --build` then dies."""
    if not (build_dir / "CMakeCache.txt").exists():
        return False
    cache_text = (build_dir / "CMakeCache.txt").read_text(errors="replace")
    if info.is_macos and os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
        desired = os.environ["MACOSX_DEPLOYMENT_TARGET"]
        if not any(line.startswith("CMAKE_OSX_DEPLOYMENT_TARGET:") and line.endswith("=" + desired)
                   for line in cache_text.splitlines()):
            return False
    if info.is_macos:
        desired_pgs = "OFF" if os.environ.get("PS2X_SETUP_PGS") == "1" else "ON"
        if not any(line.startswith("PS2X_DISABLE_PGS:") and line.endswith("=" + desired_pgs)
                   for line in cache_text.splitlines()):
            return False
    if info.is_windows:
        # The shipped runner must be a GUI binary; a cache configured with the console on (or before
        # this flag existed) has to be reconfigured or the PE gate rejects subsystem 3.
        desired_console = "ON" if os.environ.get("PS2X_SETUP_CONSOLE") == "1" else "OFF"
        if not any(line.startswith("PS2X_SHOW_WINDOWS_CONSOLE:") and line.endswith("=" + desired_console)
                   for line in cache_text.splitlines()):
            return False
    if info.unity_batch is not None and info.unity_batch != 8:
        if not any(line.startswith("PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE:")
                   and line.endswith("=" + str(info.unity_batch)) for line in cache_text.splitlines()):
            return False
    if info.use_ccache:
        if not any(line.startswith("CMAKE_CXX_COMPILER_LAUNCHER:") and "ccache" in line
                   for line in cache_text.splitlines()):
            return False
    if any((build_dir / f).exists() for f in ("build.ninja", "Makefile", "ALL_BUILD.vcxproj")):
        return True
    return any(build_dir.glob("*.sln"))


def cmake_configure(info: PlatformInfo, build_dir: Path) -> None:
    run(["cmake", "-S", ROOT, "-B", build_dir] + info.configure_extra(build_dir))


def cmake_build(info: PlatformInfo, build_dir: Path, target: str, jobs: str) -> None:
    cmd = ["cmake", "--build", build_dir, "--target", target, "-j", jobs]
    if info.is_windows:
        cmd += ["--config", "Release"]   # multi-config generators
    run(cmd)


def extract_inputs(ctx: "Context") -> None:
    """Step 1: obtain the two files source generation needs (the boot ELF and DBZP.BIN)."""
    if ctx.args.skip_setup:
        LOG.info("--skip-setup: reusing existing games/bt3/work/ and generated sources")
        return
    WORK.mkdir(parents=True, exist_ok=True)
    src = ctx.src
    if src.suffix.lower() == ".iso":
        kind, exe = find_extractor()
        if not exe:
            die("need bsdtar/tar (Windows 10+ ships tar.exe) or 7z to extract the ISO")
        members = ["SLUS_216.78", "BIN/DBZP.BIN"]
        step(f"extracting {', '.join(members)} from the ISO with {exe}")
        if kind == "tar":
            run([exe, "-xf", src, "-C", WORK, *members])
        else:
            run([exe, "x", "-y", f"-o{WORK}", src, *members], stdout=subprocess.DEVNULL)
        if not ctx.elf.is_file():
            die("SLUS_216.78 not found in ISO (is this the USA release?)")
        if not (WORK / "BIN" / "DBZP.BIN").is_file():
            die("BIN/DBZP.BIN not found in ISO (is this the USA release?)")
        make_writable(WORK)
    else:
        # A bare ELF build: the caller supplies SLUS_216.78 directly. BIN/DBZP.BIN still has to be
        # present in WORK.
        if WORK.exists():
            make_writable(WORK)
        shutil.copyfile(src, ctx.elf)
        make_writable(WORK)
        LOG.info("NOTE: you passed a bare ELF. The build also needs the ISO's")
        LOG.info(f"      BIN/DBZP.BIN next to it in {WORK}.")


def verify_elf(ctx: "Context") -> None:
    if ctx.args.skip_setup:
        return
    got = sha256_of(ctx.elf)
    if got != ELF_SHA256:
        print(f"ERROR: ELF sha256 mismatch.\n  expected: {ELF_SHA256}\n  got:      {got}")
        print("Only the USA release (SLUS-21678) is supported. Set PS2X_SETUP_FORCE=1 to continue anyway.")
        if os.environ.get("PS2X_SETUP_FORCE") != "1":
            sys.exit(1)


def gen_vu1(ctx: "Context") -> None:
    """Step 2b: the static VU1 recompiler input, cut from the ELF and translated to C++.
    Runs with --skip-setup too: it takes seconds and a tree without the file still builds."""
    step("generating VU1 programs from the ELF")
    sys.path.insert(0, str(HERE))
    from vu1_programs import generate as generate_vu1
    generate_vu1(ctx.elf, ROOT / "ps2xRuntime", WORK / "vu1")


def fetch_submodules() -> None:
    """The paraLLEl-GS backend lives in a git submodule; CMake builds it only when present."""
    if os.environ.get("PS2X_SETUP_NO_SUBMODULES") or not (ROOT / ".gitmodules").exists() or not shutil.which("git"):
        return
    try:
        run(["git", "-C", ROOT, "submodule", "update", "--init", "--recursive"])
    except Exception as e:   # noqa: BLE001
        print(f"== submodule fetch failed ({e}); building without the paraLLEl-GS backend")


def build_recompiler(ctx: "Context") -> Path:
    step("building recompiler")
    if not configured(BUILD, ctx.platform):
        cmake_configure(ctx.platform, BUILD)
    cmake_build(ctx.platform, BUILD, "ps2_recomp", str(os.cpu_count() or 4))
    return find_binary("ps2_recomp")


def generate_runner(ctx: "Context", recomp: Path) -> None:
    """Step 4: deduplicate/split the function map and generate the runner sources."""
    step("generating runner sources")
    sys.path.insert(0, str(HERE))
    from split_functions import split_csv
    split = WORK / "functions_split.csv"
    split_csv(ctx.elf, HERE / "functions.csv", split)
    out = WORK / "output"
    if out.exists():
        shutil.rmtree(out)
    cfg_text = (HERE / "config.toml.in").read_text()
    cfg_text = (cfg_text.replace("@ELF@", ctx.elf.as_posix())
                        .replace("@CSV@", split.as_posix())
                        .replace("@OUT@", out.as_posix() + "/"))
    (WORK / "config.toml").write_text(cfg_text)
    run([recomp, WORK / "config.toml"])

    # Step 5: post-generation patches + the overlay module from DBZP.BIN.
    run([sys.executable, HERE / "apply_patches.py", out])
    step("generating overlay sources from BIN/DBZP.BIN")
    run([sys.executable, HERE / "gen_overlay.py",
         "--recomp", recomp, "--dbzp", WORK / "BIN" / "DBZP.BIN",
         "--work", WORK / "overlay", "--runtime", ROOT / "ps2xRuntime"])
    run([sys.executable, HERE / "apply_overlay_patches.py", ROOT / "ps2xRuntime"])

    # Step 6: install into the runtime tree.
    step("installing runner sources")
    rt = ROOT / "ps2xRuntime"
    sync_tree(out, rt / "src" / "runner",
              exclude=("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"))
    for h in ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"):
        shutil.copyfile(out / h, rt / "include" / h)


def build_runner(ctx: "Context", jobs: str) -> Path:
    """Step 7: build the game. Reconfigure explicitly when the runner source SET changed: the
    Visual Studio generator does not reliably re-glob within the same build invocation."""
    rt = ROOT / "ps2xRuntime"
    cache = BUILD / "CMakeCache.txt"
    need_cfg = not configured(BUILD, ctx.platform)
    if not need_cfg:
        ct = cache.stat().st_mtime
        for d in (rt / "src" / "runner", rt / "src" / "runner_overlay"):
            if d.exists() and d.stat().st_mtime > ct:
                need_cfg = True
                break
    if need_cfg:
        cmake_configure(ctx.platform, BUILD)
    step(f"building ps2EntryRunner (-j{jobs}, this takes a while)")
    cmake_build(ctx.platform, BUILD, "ps2EntryRunner", jobs)
    return find_binary("ps2EntryRunner")


def stage_build(ctx: "Context") -> None:
    step("stage 3: build")
    if VIEW is not None:
        VIEW.stage(3, 4, "build")

    def item(n: int, name: str, status: str = "running") -> None:
        if VIEW is not None:
            VIEW.item(n, 7, name, status)

    ensure_msvc_env(ctx)
    if ctx.args.skip_setup:
        if not (WORK / "SLUS_216.78").is_file():
            die("--skip-setup requires an existing games/bt3/work/ (no SLUS_216.78 found)")
        item(1, "Extract game data", "skip")
        item(2, "Verify ELF", "skip")
    else:
        item(1, "Extract game data")
        extract_inputs(ctx)
        item(2, "Verify ELF")
        verify_elf(ctx)
    item(3, "Generate VU1")
    gen_vu1(ctx)
    if ctx.args.skip_setup:
        item(4, "Fetch submodules", "skip")
        item(5, "Build recompiler", "skip")
        item(6, "Generate runner", "skip")
    else:
        item(4, "Fetch submodules")
        fetch_submodules()
        item(5, "Build recompiler")
        recomp = build_recompiler(ctx)
        item(6, "Generate runner")
        generate_runner(ctx, recomp)
    if ctx.args.gen_only:
        LOG.info("--gen-only: runner + overlay sources generated (skipping runner build)")
        ctx.runner = None
        return
    item(7, "Build runner")
    ctx.runner = build_runner(ctx, ctx.jobs)
    item(7, "Build runner", "ok")


# ------------------------------------------------------------------------------------------------
# Stage 4: deploy (+ packaging, added in the packaging phase)
# ------------------------------------------------------------------------------------------------
def deploy_tree(runner: Path, out: Path) -> None:
    """Assemble the portable tree in OUT.

    layout: OUT/savedata/ (settings.toml; existing user saves are preserved), OUT/assets/ (fonts).
    No game data is deployed: the launcher's install wizard extracts SLUS_216.78 + BIN/ DATA/ IRX/
    SYSTEM.CNF from the user's own ISO into OUT/data on first run.
    """
    step(f"assembling deploy tree in {out}")
    out.mkdir(parents=True, exist_ok=True)
    save_dst = out / "savedata"
    save_dst.mkdir(parents=True, exist_ok=True)
    cfg_src = runner.parent / "settings.toml"
    cfg_dst = save_dst / "settings.toml"
    if cfg_src.exists() and not cfg_dst.exists():
        shutil.copy2(cfg_src, cfg_dst)
        LOG.info(f"  copied default settings -> {cfg_dst}")
    for a in ("assets",):
        src = runner.parent / a
        if src.exists():
            copytree_overlay(src, out / a)
    if os.name == "nt":
        for p in runner.parent.glob("*.dll"):
            shutil.copy2(p, out / p.name)
    shutil.copy2(runner, out / runner.name)
    shutil.copymode(runner, out / runner.name)
    LOG.info(f"  runner -> {out / runner.name}")
    LOG.info(f"Deploy tree ready: {out}")


def _release_out(ctx: "Context") -> Path:
    if ctx.args.output:
        return Path(ctx.args.output).resolve()
    sub = "release-windows" if ctx.platform.is_windows else "release"
    return BUILD / sub / "out"


def _is_qt_debug_dll(p: Path) -> bool:
    """Qt ships debug DLLs next to the release ones (Qt6Cored.dll). Only treat a file as debug when
    the release counterpart exists: some TLS backends legitimately end in 'd.dll'."""
    n = p.name
    if not n.endswith("d.dll"):
        return False
    return p.with_name(n[:-5] + ".dll").exists()


def build_launcher(ctx: "Context") -> Optional[Path]:
    """Configure + build the Qt launcher and return the built artifact (.exe, binary or .app)."""
    if ctx.args.skip_launcher or ctx.platform.is_macos:
        return None
    step("building the Qt launcher")
    src = ROOT / "ps2xRuntime" / "src" / "launcher"
    bdir = BUILD / ("launcher_qt" if ctx.platform.is_windows else "launcher")
    extra = ["-DCMAKE_BUILD_TYPE=Release"]
    if ctx.platform.qt_prefix:
        extra.append("-DCMAKE_PREFIX_PATH=" + str(ctx.platform.qt_prefix))
    if ctx.platform.is_windows:
        extra += ctx.platform.windows_generator_flags(bdir)
    else:
        # Same as the main tree: use Ninja when available, otherwise CMake defaults to Unix Makefiles
        # (not installed on minimal distros like Arch) and the configure fails.
        if not (bdir / "CMakeCache.txt").exists() and shutil.which("ninja"):
            extra += ["-G", "Ninja"]
        if ctx.platform.is_macos and os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
            extra.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=" + os.environ["MACOSX_DEPLOYMENT_TARGET"])
    run(["cmake", "-S", src, "-B", bdir] + extra)
    cmake_build(ctx.platform, bdir, "Launcher", ctx.jobs)
    exe = bdir / ctx.platform.exe("Launcher")
    if not exe.exists():
        die(f"Launcher not found after build ({exe})")
    return exe


def _vc_runtime_dll(name: str) -> Optional[Path]:
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if not base:
            continue
        for edition in ("BuildTools", "Community", "Professional", "Enterprise"):
            root = Path(base) / "Microsoft Visual Studio" / "2022" / edition / "VC" / "Redist" / "MSVC"
            if root.is_dir():
                hits = [h for h in sorted(root.rglob(name), reverse=True) if "x64" in h.parts]
                if hits:
                    return hits[0]
    sys32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / name
    return sys32 if sys32.exists() else None


def copy_licences(stage: Path) -> None:
    """GPL-3.0 for the project and the LGPL-3.0 for the bundled paraLLEl-GS (the PE/floor gates and
    the packaging checks require both)."""
    for lic, name in ((ROOT / "LICENSE", "LICENSE"),
                      (ROOT / "ps2xRuntime" / "third_party" / "parallel-gs" / "COPYING.LGPLv3",
                       "COPYING.LGPLv3")):
        if lic.exists():
            shutil.copy2(lic, stage / name)
        else:
            warn(f"licence file not found: {lic}")


def bundle_windows(ctx: "Context", stage: Path, runner: Path, launcher: Path) -> None:
    """Flat self-contained layout: Qt + VC runtime + FFmpeg in lib/, critical DLLs next to the EXEs,
    qt.conf for plugin discovery, lavapipe as the software Vulkan fallback."""
    step("bundling the Windows runtime")
    stage_assets = stage / "assets"
    stage_assets.mkdir(parents=True, exist_ok=True)
    stage_lib = stage_assets / "lib"
    stage_lib.mkdir(parents=True, exist_ok=True)

    # The launcher boots <appDir>/bt3-runner.exe: rename the built ps2EntryRunner.exe.
    runner_name = "bt3-runner.exe"
    if (stage / runner_name).exists():
        pass
    elif (stage / "ps2EntryRunner.exe").exists():
        (stage / "ps2EntryRunner.exe").rename(stage / runner_name)
    else:
        shutil.copy2(runner, stage / runner_name)

    qt_bin = (ctx.platform.qt_prefix / "bin") if ctx.platform.qt_prefix else None
    if not qt_bin or not qt_bin.is_dir():
        die("Qt bin directory not found; install Qt (stage 2) or set QT_ROOT")
    for p in qt_bin.glob("Qt6*.dll"):
        if not _is_qt_debug_dll(p):
            shutil.copy2(p, stage_lib / p.name)

    plugins_src = None
    for cand in (ctx.platform.qt_prefix / "plugins", ctx.platform.qt_prefix / "share" / "qt6" / "plugins"):
        if cand.is_dir():
            plugins_src = cand
            break
    plugins_dst = stage_lib / "qt6" / "plugins"
    if plugins_src:
        plugins_dst.mkdir(parents=True, exist_ok=True)
        for p in plugins_src.rglob("*"):
            if p.is_dir():
                continue
            rel = p.relative_to(plugins_src)
            if rel.parts and rel.parts[0] == "sqldrivers":
                continue   # qsqlpsql needs a non-system LIBPQ.dll; unused
            if _is_qt_debug_dll(p):
                continue
            dst = plugins_dst / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
    else:
        warn("Qt plugins directory not found; the platform plugin will be missing")

    # FFmpeg DLLs staged next to the runner by CMake POST_BUILD.
    for pattern in ("avcodec-*.dll", "avformat-*.dll", "avutil-*.dll",
                    "swresample-*.dll", "swscale-*.dll"):
        for p in stage.glob(pattern):
            shutil.move(str(p), str(stage_lib / p.name))

    for dll in ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                "msvcp140_1.dll", "msvcp140_2.dll"):
        src = _vc_runtime_dll(dll)
        if src:
            shutil.copy2(src, stage_lib / dll)
        else:
            warn(f"VC++ runtime {dll} not found; the target machine must install the VC++ redistributable")

    # Windows resolves DLLs from the EXE's directory before main(): flatten EVERY bundled DLL next to
    # the executables so a double-click on bt3-runner.exe works (and the launcher does not need PATH
    # gymnastics). assets/lib stays the canonical bundle; the flat copies are the load-time safety net.
    flat = 0
    for p in stage_lib.glob("*.dll"):
        shutil.copy2(p, stage / p.name)
        flat += 1
    print(f"  flattened {flat} DLLs next to the executables")

    (stage / "qt.conf").write_text("[Paths]\nPrefix = .\nPlugins = assets/lib/qt6/plugins\n", encoding="ascii")

    copy_licences(stage)

    lvp = ctx.platform.lavapipe_dir
    if lvp and (lvp / "vulkan_lvp.dll").exists():
        lvp_dst = stage_assets / "lavapipe"
        lvp_dst.mkdir(exist_ok=True)
        shutil.copy2(lvp / "vulkan_lvp.dll", lvp_dst / "vulkan_lvp.dll")
        icd = lvp / "lvp_icd.x86_64.json"
        if icd.exists():
            shutil.copy2(icd, lvp_dst / "lvp_icd.x86_64.json")
    else:
        warn("lavapipe not found; the Windows Vulkan fallback will be unavailable (stage 2 installs it)")

    if launcher is not None:
        shutil.copy2(launcher, stage / "Launcher.exe")
        assets = launcher.parent / "assets"
        if assets.is_dir():
            copytree_overlay(assets, stage / "assets")


LINUX_LIB_BLACKLIST = (
    "libc.so", "libm.so", "libmvec.so", "libpthread.so", "libdl.so", "librt.so", "libutil.so",
    "libresolv.so", "libnsl.so", "libstdc++.so", "libgcc_s.so", "ld-linux",
)


def _ldd_deps(path: Path) -> list[Path]:
    if not shutil.which("ldd"):
        return []
    out = run_capture(["ldd", str(path)])
    deps = []
    for line in out.splitlines():
        if "=>" in line:
            target = line.split("=>", 1)[1].strip().split(" ")[0]
        else:
            target = line.strip().split(" ")[0]
        if target.startswith("/") and Path(target).exists():
            deps.append(Path(target))
    return deps


def _bundle_closure(binaries: list[Path], stage_lib: Path) -> None:
    """Copy the transitive ldd closure of the binaries into lib/, minus the glibc/C++ core (the
    release relies on the distro's own runtime; bundling it caused GLIBC_PRIVATE clashes)."""
    stage_lib.mkdir(parents=True, exist_ok=True)
    seen: set[str] = set()
    queue = list(binaries)
    while queue:
        cur = queue.pop()
        for dep in _ldd_deps(cur):
            name = dep.name
            if name in seen or name.startswith(LINUX_LIB_BLACKLIST):
                continue
            seen.add(name)
            dst = stage_lib / name
            # A plugin's dependency can resolve (through ..) to a file already bundled: copying it
            # again raises SameFileError. Compare resolved paths and just reuse it.
            try:
                if dst.exists() and dep.resolve() == dst.resolve():
                    queue.append(dst)
                    continue
            except OSError:
                pass
            shutil.copy2(dep, dst)
            queue.append(dst)


def bundle_linux(ctx: "Context", stage: Path, runner: Path, launcher: Optional[Path]) -> None:
    step("bundling the Linux runtime")
    stage_assets = stage / "assets"
    stage_assets.mkdir(parents=True, exist_ok=True)
    stage_lib = stage_assets / "lib"
    stage_lib.mkdir(parents=True, exist_ok=True)

    bin_runner = stage / "bt3-runner"
    shutil.copy2(runner, bin_runner)
    bin_runner.chmod(bin_runner.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    raw = stage / "ps2EntryRunner"   # deploy_tree dropped the build name here; the launcher boots bt3-runner
    if raw.exists() and raw != bin_runner:
        raw.unlink()
    targets = [bin_runner]

    if launcher is not None:
        bin_launcher = stage / "Launcher"
        shutil.copy2(launcher, bin_launcher)
        bin_launcher.chmod(bin_launcher.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
        targets.append(bin_launcher)
        assets = launcher.parent / "assets"
        if assets.is_dir():
            copytree_overlay(assets, stage / "assets")

    _bundle_closure(targets, stage_lib)

    # Qt platform plugins are dlopened, so ldd does not see them: copy them explicitly plus their own
    # dependencies.
    plugin_src = None
    for cand in (Path("/usr/lib/x86_64-linux-gnu/qt6/plugins"), Path("/usr/lib/qt6/plugins")):
        if cand.is_dir():
            plugin_src = cand
            break
    plugins_dst = stage_lib / "qt6" / "plugins" / "platforms"
    if plugin_src and (plugin_src / "platforms").is_dir():
        plugins_dst.mkdir(parents=True, exist_ok=True)
        extra_bins = []
        for name in ("libqxcb.so", "libqoffscreen.so"):
            src = plugin_src / "platforms" / name
            if src.exists():
                shutil.copy2(src, plugins_dst / name)
                extra_bins.append(plugins_dst / name)
        if extra_bins:
            _bundle_closure(extra_bins, stage_lib)
    else:
        warn("Qt platform plugins not found; the launcher will not start without them")

    copy_licences(stage)

    (stage / "logs").mkdir(exist_ok=True)
    (stage / "savedata" / "BASLUS-21678DBZT3").mkdir(parents=True, exist_ok=True)
    installer = ROOT / "scripts" / "install-game.sh.in"
    if installer.exists():
        dst = stage / "install game.sh"
        shutil.copy2(installer, dst)
        dst.chmod(dst.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def seed_savedata(ctx: "Context", stage: Path) -> None:
    """settings.toml (only when absent), fps60 pacing table and the memory-card slot."""
    savedata = stage / "savedata"
    savedata.mkdir(parents=True, exist_ok=True)
    default = ROOT / "scripts" / "settings.toml.default"
    cfg = savedata / "settings.toml"
    if default.exists() and not cfg.exists():
        shutil.copy2(default, cfg)
    fps60 = HERE / "fps60_sites.txt"
    if fps60.exists():
        shutil.copy2(fps60, savedata / "fps60_sites.txt")
    (savedata / "BASLUS-21678DBZT3").mkdir(exist_ok=True)


def run_gate(ctx: "Context", stage: Path) -> None:
    if ctx.platform.is_windows:
        gate = ROOT / "scripts" / "check_windows_deps.py"
        if gate.exists():
            step("running the PE dependency gate")
            run([sys.executable, str(gate), str(stage)])
        else:
            warn("PE gate script not found; skipping")
    elif ctx.platform.os == "linux":
        gate = ROOT / "scripts" / "check_floor.sh"
        if gate.exists() and shutil.which("bash"):
            step("running the glibc floor gate")
            run(["bash", str(gate), str(stage)])


def package_artifact(ctx: "Context", out_root: Path, stage: Path) -> None:
    """Assemble the portable tree and produce the archive + sha256 for this OS."""
    import datetime
    import tarfile
    import zipfile

    tree_name = "Dragon Ball Budokai Tenkaichi 3 Recompiled"
    out_root.mkdir(parents=True, exist_ok=True)
    tree = out_root / tree_name
    if tree.exists():
        shutil.rmtree(tree)
    copytree_overlay(stage, tree)

    if ctx.platform.is_windows:
        archive = out_root / "BT3-Recomp-win-x86_64.zip"
        step(f"packaging {archive.name}")
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in sorted(tree.rglob("*")):
                if p.is_file():
                    z.write(p, p.relative_to(out_root).as_posix())
    else:
        # The artifact name carries the OS it was built for (win / linux / macos).
        tag = "macos" if ctx.platform.is_macos else "linux"
        archive = out_root / f"BT3-Recomp-{tag}-x86_64.tar.gz"
        step(f"packaging {archive.name}")
        with tarfile.open(archive, "w:gz") as t:
            t.add(tree, arcname=tree_name)

    shutil.rmtree(tree)
    digest = sha256_of(archive)
    base = archive.name
    for suffix in (".tar.gz", ".zip"):
        if base.endswith(suffix):
            base = base[: -len(suffix)]
            break
    checksum = out_root / (base + ".sha256")
    checksum.write_text(f"{digest}  {archive.name}\n")
    LOG.info(f"  {archive}  ({archive.stat().st_size / (1 << 20):.1f} MB)")
    LOG.info(f"  sha256: {digest}")

    dest: Optional[Path] = None
    if ctx.args.dest:
        dest = Path(ctx.args.dest).expanduser()
    elif not ctx.args.no_desktop_copy:
        desktop = Path.home() / "Desktop"
        dest = ask_destination(ctx, desktop if desktop.is_dir() else out_root)
    if dest is not None:
        try:
            dest.mkdir(parents=True, exist_ok=True)
            for art in (archive, checksum):
                shutil.copy2(art, dest / art.name)
            # The run log is written until the very end, so remember the destination and let main()
            # drop the complete log there after LOG.close() (see _copy_log_next_to_artifact).
            ctx.log_dest = dest
            LOG.info(f"  copied to {dest}")
        except OSError as e:
            warn(f"could not copy the artifact to {dest}: {e}")


def _copy_log_next_to_artifact(ctx: "Context") -> None:
    """Place the completed run log beside the artifact the user asked for. No-op unless
    package_artifact chose a destination, or when the run had no log file."""
    if ctx.log_dest is None or LOG.path is None:
        return
    src = Path(LOG.path)
    if not src.is_file():
        return
    dst = ctx.log_dest / src.name
    try:
        if src.resolve() != dst.resolve():
            shutil.copy2(src, dst)
        LOG.info(f"  log -> {dst}")
    except OSError as e:
        print(f"WARNING: could not copy the log to {ctx.log_dest}: {e}", file=sys.stderr)


def stage_package(ctx: "Context") -> None:
    step("stage 4: deploy/package")
    if VIEW is not None:
        VIEW.stage(4, 4, "package")
    # detect_platform() cached qt_prefix before deps ran; a stage 2 (or an external aqt/brew install)
    # may have made Qt available since, so re-detect before the launcher/bundle steps use it.
    ctx.platform.qt_prefix = _qt_prefix() or ctx.platform.qt_prefix
    ensure_msvc_env(ctx)
    if ctx.runner is None:
        ctx.runner = find_binary("ps2EntryRunner")

    if ctx.args.deploy and not ctx.args.package:
        deploy_tree(ctx.runner, Path(ctx.args.deploy).resolve())
        return

    out_root = _release_out(ctx)
    stage = out_root / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)

    def item(n: int, name: str, status: str = "running") -> None:
        if VIEW is not None:
            VIEW.item(n, 6, name, status)

    item(1, "Assemble deploy tree")
    deploy_tree(ctx.runner, stage)
    item(2, "Seed savedata")
    seed_savedata(ctx, stage)
    if ctx.args.deploy:
        deploy_tree(ctx.runner, Path(ctx.args.deploy).resolve())

    if ctx.platform.is_macos:
        bundler = ROOT / "tools" / "macos" / "deploy.py"
        if not bundler.exists():
            die("macOS bundler not found (tools/macos/deploy.py)")
        app = out_root / "BT3-Recomp.app"
        run([sys.executable, str(bundler), "--skip-build", "--output", str(app), "--jobs", ctx.jobs])
        LOG.info(f"App bundle ready: {app}")
        return

    item(3, "Build launcher (Qt)")
    launcher = build_launcher(ctx)
    item(4, "Bundle runtime")
    if ctx.platform.is_windows:
        bundle_windows(ctx, stage, ctx.runner, launcher)
    else:
        bundle_linux(ctx, stage, ctx.runner, launcher)

    if not ctx.args.no_gate:
        item(5, "Dependency gate")
        run_gate(ctx, stage)

    if ctx.args.package:
        item(6, "Package artifact")
        package_artifact(ctx, out_root, stage)
# ------------------------------------------------------------------------------------------------
# CLI + stage runner
# ------------------------------------------------------------------------------------------------
@dataclass
class Context:
    args: argparse.Namespace
    platform: PlatformInfo
    interactive: bool
    jobs: str
    src: Optional[Path] = None
    elf: Path = field(default_factory=lambda: WORK / "SLUS_216.78")
    runner: Optional[Path] = None
    run_mode: int = 1   # highest stage number requested
    stage_name: str = ""   # for FAILED messages ("3 build" ...)
    log_dest: Optional[Path] = None   # where the artifact was sent, to drop the log next to it


def parse_args(argv=None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Build (deploy and package) Dragon Ball Z: Budokai Tenkaichi 3.")
    ap.add_argument("src", nargs="?", metavar="<iso|elf>",
                    help="BT3 USA ISO or bare SLUS_216.78 ELF (not needed with --skip-setup)")
    ap.add_argument("--jobs", default="auto", metavar="N",
                    help=f"parallel jobs for the build; 'auto' (default) sizes it from CPU/RAM, "
                         f"N forces it, and the conservative fallback is {DEFAULT_JOBS}")
    ap.add_argument("--no-autotune", dest="no_autotune", action="store_true",
                    help="disable hardware auto-tuning (keep the conservative build defaults)")
    ap.add_argument("--stage", metavar="N", help="run only this stage (1-4)")
    ap.add_argument("--stages", metavar="A-B", help="run a stage range, e.g. 3-4")
    ap.add_argument("--list-stages", action="store_true", help="print the stages and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="stage 1 + a report of what would be done; no changes")
    ap.add_argument("--report", choices=("text", "json"), default="text",
                    help="format of the stage 1 report")
    ap.add_argument("-y", "--yes", action="store_true", help="answer yes to every prompt")
    ap.add_argument("--non-interactive", action="store_true", help="never prompt")
    ap.add_argument("--install-deps", action="store_true", help="install missing dependencies")
    ap.add_argument("--no-deps", action="store_true", help="never install dependencies")
    ap.add_argument("--deps-only", action="store_true", help="stop after stage 2")
    ap.add_argument("--deploy", metavar="OUT",
                    help="after the build, assemble the playable tree into OUT")
    ap.add_argument("--package", dest="package", action="store_true", default=True,
                    help="produce the release artifact for this OS (+ checksum); on by default")
    ap.add_argument("--no-package", dest="package", action="store_false",
                    help="do not produce the release artifact (stage 4 assembles the deploy tree only)")
    ap.add_argument("--output", metavar="DIR",
                    help="where the stage tree and the artifact go (default build/release-<os>/out)")
    ap.add_argument("--skip-launcher", action="store_true",
                    help="do not build the Qt launcher (developer tree without the UI)")
    ap.add_argument("--no-gate", dest="no_gate", action="store_true",
                    help="skip the release gate (PE imports / glibc floor)")
    ap.add_argument("--dest", metavar="DIR",
                    help="copy the release artifact (+ checksum) into DIR (skips the prompt)")
    ap.add_argument("--no-desktop-copy", dest="no_desktop_copy", action="store_true",
                    help="never offer to copy the release artifact anywhere")
    ap.add_argument("--skip-setup", action="store_true",
                    help="skip ISO/recompile/patches; only rebuild the runner (+deploy)")
    ap.add_argument("--gen-only", action="store_true",
                    help="stop after recompile/generation/patches; do not build the runner")
    ap.add_argument("--log", metavar="PATH",
                    help="execution log (default build/setup.log; overwritten on each run)")
    ap.add_argument("--no-log", dest="no_log", action="store_true", help="no log file")
    ap.add_argument("--log-level", dest="log_level", type=int, default=3, choices=(0, 1, 2, 3, 4),
                    help="0 silent, 1 errors, 2 errors+warnings, 3 info (default), 4 verbose")
    ap.add_argument("-q", "--quiet", action="store_true", help="console: errors only (= --log-level 1)")
    ap.add_argument("-v", "--verbose", action="store_true", help="console: every command (= level 4)")
    ap.add_argument("--plain", action="store_true",
                    help="no full-screen UI: print the steps as plain lines (automatic when stdout is "
                         "not a terminal, e.g. redirected to a log)")
    ap.add_argument("--no-color", dest="no_color", action="store_true", help="disable ANSI colors")
    ap.add_argument("--check", action="store_true",
                    help="report the dependencies and exit; never installs or mutates anything")
    args = ap.parse_args(argv)
    if args.skip_setup and args.gen_only:
        die("--skip-setup and --gen-only are mutually exclusive")
    if args.install_deps and args.no_deps:
        die("--install-deps and --no-deps are mutually exclusive")
    return args


def wanted_stages(args: argparse.Namespace) -> set:
    if args.deps_only:
        return {"1", "2"}
    if args.stage:
        return {"1", args.stage}
    if args.stages:
        spec = args.stages
        a_str, b_str = spec.split("-", 1) if "-" in spec else (spec, spec)
        try:
            a, b = int(a_str), int(b_str)
        except ValueError:
            die(f"bad --stages value: {spec} (use N or A-B)")
        if a > b:
            a, b = b, a
        return {str(n) for n in range(max(1, a), min(4, b) + 1)} | {"1"}
    return {"1", "2", "3", "4"}


def resolve_source(ctx: "Context") -> None:
    """Fill ctx.src (and prompt for it when interactive)."""
    if ctx.args.skip_setup or ctx.args.dry_run:
        if ctx.args.src:
            ctx.src = Path(ctx.args.src).expanduser()
        return
    raw = ctx.args.src
    if not raw and ctx.interactive:
        p = ask_path(ctx, "BT3 USA ISO required. Paste the path (drag & drop works):", "file")
        if p:
            ctx.src = p
    elif raw:
        ctx.src = Path(raw).expanduser()
    if ctx.src is None:
        if ctx.interactive:
            die("no ISO/ELF provided")
        die("missing the BT3 ISO or SLUS_216.78 ELF path")
    if not ctx.src.exists():
        die(f"{ctx.src} does not exist")


WELCOME_TITLE = "BT3-Recomp   -   DEVELOPER SETUP"
WELCOME = [
    "This builds the emulator FROM SOURCE. It is NOT the end-user installer.",
    "",
    "  * Just want to PLAY? Download a release build instead of running this.",
    "  * This flow needs an ISO/dump of Budokai Tenkaichi 3 (USA), several GB",
    "    of disk, and it installs a C++ toolchain + Qt on this machine.",
    "  * It builds the runner and the launcher, packages a release artifact",
    "    and then asks where to put it.",
]


def stage_welcome(ctx: "Context") -> None:
    """Pre-stage: make it unmistakable that this is the developer build flow, not the player one."""
    try:
        from setup_ui import splash_box   # boxed panel, same look as the TUI frame
        splash_box(WELCOME_TITLE, WELCOME)
    except Exception:   # noqa: BLE001
        for ln in WELCOME:
            print(ln)
    if ctx.interactive and not ctx.args.yes and not ctx.args.dry_run and not ctx.args.check:
        try:
            _ask("  Press Enter to continue (Ctrl-C to cancel): ")
        except EOFError:
            pass


def _finish(ctx: "Context", mode: str) -> None:
    """Print the final RESULT block (plain lines -- they stay in the scrollback) and leave the TUI."""
    if VIEW is None:
        return
    lines = ["", "=" * 70, "  RESULT"]
    res = getattr(ctx, "deps_resolved", [])
    fail = getattr(ctx, "deps_failed", [])
    if res:
        lines.append("    OK      " + ", ".join(res))
    for d in fail:
        lines.append(f"    FAIL    {d.name}")
        if getattr(d, "droppable", None) is not None:
            lines.append(f"            {d.droppable.url_page}   ({d.droppable.filename})")
            lines.append(f"            drop it: drag the file here, or copy it into "
                         f"{BUILD / 'deps-inbox'}")
    if fail:
        lines.append("")
        lines.append("    Fix the item(s) above and re-run setup.")
    if LOG.path:
        lines.append(f"  Full log : {LOG.path}")
    lines.append("=" * 70)
    VIEW.finish(lines)


def main(argv=None) -> None:
    global LOG
    args = parse_args(argv)
    if args.list_stages:
        for num, desc in STAGES:
            print(f"  --stage {num}   {desc}")
        return

    level = 4 if args.verbose else (1 if args.quiet else args.log_level)
    log_path = None
    if not args.no_log:
        log_path = Path(args.log).expanduser() if args.log else BUILD / "setup.log"
    LOG = Logger(log_path, level)
    LOG.banner(f"[setup] console={LOG_LEVELS.get(level, level)}"
               + (f" | log: {LOG.path}" if LOG.path else " | no log file"))

    info = detect_platform()
    interactive = info.interactive and not args.non_interactive
    ctx = Context(args=args, platform=info, interactive=interactive, jobs=str(args.jobs))

    global VIEW
    if args.check:
        args.no_deps = True

    # The welcome runs BEFORE the TUI owns the screen: a plain message + Enter is far more reliable
    # than a splash inside a frame we are about to clear.
    if level >= 3:
        stage_welcome(ctx)

    # [ui] Console view: full-screen TUI on a terminal, plain lines otherwise (auto). --plain /
    # --no-color force the fallbacks; a missing setup_ui module leaves VIEW None (old behaviour).
    if View is not None and level >= 3:
        VIEW = View(log=LOG, caps=Caps(force_plain=args.plain,
                                       color=(False if args.no_color else None)), level=level)
        VIEW.start()   # own the screen BEFORE the first header/summary draw
        VIEW.header("BT3-Recomp", "build setup")
        VIEW.summary_row("source", str(ROOT))
        if LOG.path:
            VIEW.summary_row("log", Path(LOG.path).name)

    # Hardware auto-tune: pick the largest safe -j and optional build accelerators. An explicit
    # numeric --jobs or --no-autotune wins; a failed RAM probe keeps DEFAULT_JOBS (conservative).
    raw_jobs = str(args.jobs).strip()
    if args.no_autotune or raw_jobs.isdigit():
        ctx.jobs = raw_jobs if raw_jobs.isdigit() else DEFAULT_JOBS
    else:
        hw = detect_hardware()
        jobs, batch, use_ccache = plan_build(hw)
        if jobs is None:
            ctx.jobs = DEFAULT_JOBS
            LOG.info(f"[autotune] RAM not detected; keeping the safe default -j{ctx.jobs}")
        else:
            ram = f"{hw.ram_gb:.0f} GB" if hw.ram_known else "unknown RAM"
            LOG.info(f"[autotune] {hw.physical_cores} cores / {ram} -> -j{jobs}, "
                     f"unity batch {batch}" + (", ccache" if use_ccache else ""))
            ctx.jobs = str(jobs)
            info.unity_batch = batch
            info.use_ccache = use_ccache
            if VIEW is not None:
                VIEW.summary_row("core", f"{hw.physical_cores}")
                VIEW.summary_row("RAM", ram)
                VIEW.summary_row("jobs", f"-j{ctx.jobs}")

    # --dry-run implies --no-deps and stops before stage 3.
    if args.dry_run:
        args.no_deps = True

    stages = wanted_stages(args)
    ctx.run_mode = max(int(s) for s in stages)

    try:
        ctx.stage_name = "1 detect"
        stage_detect(ctx)

        if "2" in stages:
            ctx.stage_name = "2 deps"
            stage_deps(ctx)

        if args.check:
            _finish(ctx, "check")
            LOG.banner("[setup] check done" + (f" | log: {LOG.path}" if LOG.path else ""))
            LOG.close()
            return

        if args.dry_run:
            if "3" in stages:
                LOG.info("\ndry-run: would build (stage 3) and deploy"
                         + (" and package" if args.package else "") + ".")
            LOG.banner("[setup] dry-run ok" + (f" | log: {LOG.path}" if LOG.path else ""))
            LOG.close()
            return

        if "3" in stages and not args.deps_only:
            ctx.stage_name = "3 build"
            resolve_source(ctx)
            stage_build(ctx)

        if "4" in stages and not args.deps_only:
            ctx.stage_name = "4 package"
            if ctx.runner is None and (args.deploy or args.package):
                # --stage 4 (or --skip-setup) with an existing build: locate the runner.
                ctx.runner = find_binary("ps2EntryRunner")
            stage_package(ctx)

        if ctx.runner is not None and not args.deploy:
            env_line = ("set PS2X_CD_IMAGE=<path to your BT3 ISO>& " if info.is_windows else
                        'env PS2X_CD_IMAGE="<path to your BT3 ISO>" ')
            step("done")
            LOG.info(f"Run with:\n\n  cd {ctx.runner.parent}\n  {env_line}\\\n      {ctx.runner} {ctx.elf}\n")
    except CommandError as e:
        die(str(e), 2, stage=ctx.stage_name)
    except KeyboardInterrupt:
        die("interrupted by the user", 130, stage=ctx.stage_name)
    finally:
        if VIEW is not None:
            try:
                VIEW.close()   # never leave the terminal with a hidden cursor / colors on
            except Exception:   # noqa: BLE001
                pass

    LOG.banner("[setup] done" + (f" | log: {LOG.path}" if LOG.path else ""))
    _finish(ctx, "done")
    LOG.close()
    _copy_log_next_to_artifact(ctx)


if __name__ == "__main__":
    main()
