#!/usr/bin/env python3
"""PE dependency + layout gate for the Windows release stage.

Analyses every PE under <stage_dir> with pefile and verifies the "portable tree"
contract the flat layout enforces (qt.conf + DLLs next to the executables):

  * every import of every bundled binary resolves either inside the stage's own
    lib/ (Qt6, FFmpeg, VC++ runtime) or to a Windows OS / driver component that
    is guaranteed present (kernels, api-set stubs, vulkan via volk, ...);
  * every required artefact sits where the tree expects it (DLLs, plugins,
    wrapper, licences, default settings).

Exit codes: 0 = release-ready, 1 = problems found, 2 = usage error.

This is the Windows counterpart of scripts/check_floor.sh (the Linux gate
is a glibc ABI check since its libs come from ldd; here the closure is explicit
in the PE imports). Requires pefile (`pip install pefile`); setup.py runs it
directly in stage 4 on Windows.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

try:
    import pefile
except ImportError:  # pragma: no cover
    sys.exit("check_windows_deps.py requires pefile (pip install pefile)")

# Windows components guaranteed on a stock Windows 10/11 install (or loaded
# dynamically by us: vulkan-1 is pulled by volk/paraLLEl-GS when available, and
# the app falls back to OpenGL when it is not). Everything else must ship in
# <stage>/lib or the gate fails.
OS_COMPONENTS = {
    # ntdll + the api-set layer are always present
    "ntdll.dll",
    # CRT: ucrtbase is part of Windows 10 1809+; msvcrt is ancient but present
    "ucrtbase.dll",
    "msvcrt.dll",
    # core Win32
    "kernel32.dll",
    "kernelbase.dll",
    "user32.dll",
    "gdi32.dll",
    "advapi32.dll",
    "shell32.dll",
    "ole32.dll",
    "oleaut32.dll",
    "combase.dll",
    "shlwapi.dll",
    "version.dll",
    "ws2_32.dll",
    "wsock32.dll",
    "bcrypt.dll",
    "secur32.dll",
    "crypt32.dll",
    "wintrust.dll",
    "winmm.dll",
    "wininet.dll",
    "winhttp.dll",
    "rpcrt4.dll",
    "imm32.dll",
    "comctl32.dll",
    "comdlg32.dll",
    "uxtheme.dll",
    "dwmapi.dll",
    "netapi32.dll",
    "iphlpapi.dll",
    "setupapi.dll",
    "mpr.dll",
    "msimg32.dll",
    "psapi.dll",
    "userenv.dll",
    "wtsapi32.dll",
    "cfgmgr32.dll",
    "d2d1.dll",
    "d3d11.dll",
    "d3d9.dll",
    "dxgi.dll",
    "opengl32.dll",
    "glu32.dll",
    "winspool.drv",
    "d3dcompiler_47.dll",
    "wldap32.dll",
    "wtsapi32.dll",
    "dbghelp.dll",
    # Video capture (AVICAP32), authorization (AUTHZ), DNS, DirectWrite
    "avicap32.dll",
    "authz.dll",
    "dnsapi.dll",
    "dwrite.dll",
    # ODBC driver manager (ships with Windows; Qt SQL plugin imports it)
    "odbc32.dll",
    "odbcint.dll",
}
OS_COMPONENTS = {name.lower() for name in OS_COMPONENTS}


def is_os_component(name: str) -> bool:
    low = name.lower()
    if low in OS_COMPONENTS:
        return True
    # api-set contracts / ext-ms-* stubs are OS-backed on Win10+
    return low.startswith("api-ms-win-") or low.startswith("ext-ms-")


MACHINE_AMD64 = 0x8664  # IMAGE_FILE_MACHINE_AMD64
SUBSYSTEM_WINDOWS_GUI = 2  # IMAGE_SUBSYSTEM_WINDOWS_GUI

REQUIRED_LAYOUT = [
    "Launcher.exe",
    "bt3-runner.exe",
    "qt.conf",
    "LICENSE",
    "COPYING.LGPLv3",
    "savedata/settings.toml",
    "savedata/fps60_sites.txt",
    "assets/lib/Qt6Core.dll",
    "assets/lib/Qt6Gui.dll",
    "assets/lib/Qt6Widgets.dll",
    "assets/lib/Qt6Network.dll",
    "assets/lib/qt6/plugins/platforms/qwindows.dll",
    "assets/lib/vcruntime140.dll",
    "assets/lib/vcruntime140_1.dll",
    "assets/lib/msvcp140.dll",
]

REQUIRED_GLOB = [
    ("assets/lib/avcodec-*.dll", "FFmpeg avcodec"),
    ("assets/lib/avformat-*.dll", "FFmpeg avformat"),
    ("assets/lib/avutil-*.dll", "FFmpeg avutil"),
    ("assets/lib/swresample-*.dll", "FFmpeg swresample"),
    ("assets/lib/swscale-*.dll", "FFmpeg swscale"),
]


def collect_pe_files(stage: Path) -> list[Path]:
    return [p for p in stage.rglob("*") if p.suffix.lower() in (".dll", ".exe") and p.is_file()]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stage", type=Path, help="stage dir (as packaged)")
    args = ap.parse_args()

    stage: Path = args.stage.resolve()
    if not stage.is_dir():
        print(f"ERROR: stage dir not found: {stage}", file=sys.stderr)
        return 2

    problems: list[str] = []
    reports: list[str] = []

    # ---- 1. required layout ------------------------------------------------
    for rel in REQUIRED_LAYOUT:
        if not (stage / rel).is_file():
            problems.append(f"missing required artefact: {rel}")
    for pattern, what in REQUIRED_GLOB:
        hits = list(stage.glob(pattern))
        if not hits:
            problems.append(f"missing {what} ({pattern})")

    # ---- 2. available bundled modules (case-insensitive) --------------------
    bundled: set[str] = set()
    for p in collect_pe_files(stage):
        bundled.add(p.name.lower())
    # Qt plugins live one level deeper but are also DLLs; already included above.

    # ---- 3. per-PE analysis --------------------------------------------------
    missing_imports: set[tuple[str, str]] = set()  # (pe name, unresolved dll)
    for pe_path in collect_pe_files(stage):
        rel = pe_path.relative_to(stage)
        try:
            pe = pefile.PE(str(pe_path), fast_load=False)
        except Exception as exc:  # pefile raises various PEFormatError subclasses
            problems.append(f"{rel}: not a parseable PE ({exc})")
            continue

        # machine
        try:
            machine = pe.FILE_HEADER.Machine
            if machine != MACHINE_AMD64:
                problems.append(f"{rel}: machine {machine:#x} is not AMD64")
        except Exception:
            pass

        # subsystem + entry (only for the two executables)
        try:
            subsystem = pe.OPTIONAL_HEADER.Subsystem
        except Exception:
            subsystem = None
        if rel.name.lower() in ("launcher.exe", "bt3-runner.exe"):
            if subsystem != SUBSYSTEM_WINDOWS_GUI:
                problems.append(
                    f"{rel}: subsystem {subsystem if subsystem is not None else 'n/a'} is not Windows GUI "
                    "(a console window would pop up; set WIN32_EXECUTABLE / PS2X_SHOW_WINDOWS_CONSOLE=OFF)"
                )

        # imports
        if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
            for entry in pe.DIRECTORY_ENTRY_IMPORT:
                dll_raw = entry.dll or b""
                if isinstance(dll_raw, bytes):
                    dll_raw = dll_raw.decode("ascii", "ignore")
                dll = dll_raw.strip().lower()
                if not dll:
                    continue
                if is_os_component(dll) or dll in bundled:
                    continue
                missing_imports.add((str(rel), dll_raw))

        # dynamic-link-time library loaders (LoadLibrary) don't show up here;
        # volk->vulkan-1.dll and QuaZip/OpenSSL lookups are intentional.

    # ---- 4. reporting ---------------------------------------------------------
    if missing_imports:
        problems.append("unresolved imports (not in <stage>/lib and not an OS component):")
        for pe_name, dll in sorted(missing_imports):
            problems.append(f"  {pe_name} -> {dll}")
        # also print which are plausibly missing from the kit/redist
        suggestions = sorted({dll for _, dll in missing_imports})
        reports.append("candidates to bundle (DLL imports with no owner): " +
                      ", ".join(suggestions) if suggestions else "")

    for line in reports:
        print(f"report: {line}")

    n_pe = len(collect_pe_files(stage))
    print(f"checked {n_pe} PE files, bundled {len(bundled)} modules")
    if problems:
        print("FAIL:", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1
    print("OK: Windows stage is release-ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())