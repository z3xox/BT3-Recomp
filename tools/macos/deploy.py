#!/usr/bin/env python3
"""Build a native macOS bundle. Game data and saves stay outside the signed app."""
import argparse
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def output(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


# Must match PRODUCT_NAME in games/bt3/setup.py. It carries spaces on purpose.
RUNNER_NAME = "Dragon Ball Z Budokai Tenkaichi 3 - Recompiled"


def version(value):
    return tuple((list(map(int, value.split("."))) + [0, 0])[:3])


def make_icns(source, destination, workdir):
    """Convert the app artwork to the native Finder/Dock icon format."""
    iconset = workdir / "BT3-Recomp.iconset"
    iconset.mkdir()
    # iconutil requires square source images. Crop the artwork around its center
    # before generating the standard 1x/2x icon sizes.
    square = workdir / "icon-square.png"
    run("sips", "-c", "5178", "5178", source, "--out", square)
    for size in (16, 32, 128, 256, 512):
        run("sips", "-z", str(size), str(size), square,
            "--out", iconset / f"icon_{size}x{size}.png")
        run("sips", "-z", str(size * 2), str(size * 2), square,
            "--out", iconset / f"icon_{size}x{size}@2x.png")
    run("iconutil", "-c", "icns", iconset, "-o", destination)


def audit(app, minimum):
    """Reject unresolved external libraries and a falsely advertised OS floor."""
    required_arches = set(output("lipo", "-archs", app / "Contents/MacOS" / RUNNER_NAME).split())
    macho = []
    seen = set()
    for path in app.rglob("*"):
        if not path.is_file() or path.resolve() in seen:
            continue
        seen.add(path.resolve())
        with path.open("rb") as f:
            magic = f.read(4)
        if magic not in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe"):
            continue
        macho.append(path)
        arches = set(output("lipo", "-archs", path).split())
        if not required_arches.issubset(arches):
            raise RuntimeError(f"Architecture mismatch in {path}: {arches}, runner needs {required_arches}")
        install_ids = set(output("otool", "-D", path).splitlines()[1:])
        for line in output("otool", "-L", path).splitlines()[1:]:
            dep = line.strip().split(" (compatibility version", 1)[0]
            if dep in install_ids:
                continue
            if dep.startswith("/") and not dep.startswith(("/usr/lib/", "/System/Library/")):
                raise RuntimeError(f"Unbundled dependency in {path}: {dep}")
        lines = output("otool", "-l", path).splitlines()
        old_min = False
        for line in lines:
            fields = line.split()
            if fields[:1] == ["cmd"]:
                old_min = fields[1] == "LC_VERSION_MIN_MACOSX"
            if len(fields) == 2 and (fields[0] == "minos" or old_min and fields[0] == "version"):
                if version(fields[1]) > version(minimum):
                    raise RuntimeError(f"{path.name} requires macOS {fields[1]}, above requested {minimum}")
    if not macho:
        raise RuntimeError("Bundle contains no Mach-O binaries")
    return macho


def dylib_closure(binary, frameworks):
    """Copy the runner's non-system dylibs into Contents/Frameworks and repoint every load path at
    @rpath. macdeployqt used to do this for the Qt launcher; the front-end has no Qt, so the closure
    is walked here. audit() then rejects anything still pointing outside the bundle."""
    rpath = "@executable_path/../Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    bundled = {}
    queue = [binary]
    walked = set()
    while queue:
        cur = queue.pop()
        key = str(cur)
        if key in walked:
            continue
        walked.add(key)
        run("install_name_tool", "-add_rpath", rpath, cur)
        for line in output("otool", "-L", cur).splitlines()[1:]:
            dep = line.strip().split(" (compatibility version", 1)[0]
            if not dep.startswith("/") or dep.startswith(("/usr/lib/", "/System/Library/")):
                continue
            name = Path(dep).name
            if name not in bundled:
                dst = frameworks / name
                if not dst.exists():
                    shutil.copy2(dep, dst)
                bundled[name] = dst
                run("install_name_tool", "-id", f"@rpath/{name}", dst)
                queue.append(dst)
            run("install_name_tool", "-change", dep, f"@rpath/{name}", cur)


def bundle_moltenvk(build, frameworks):
    """[moltenvk] The native Vulkan renderer dlopens MoltenVK at run time, so the link closure never sees it.
    Copy the library the build found (PS2X_MOLTENVK_LIBRARY in the CMake cache); without it the runner
    still works, on OpenGL only."""
    cache = build / "CMakeCache.txt"
    lib = None
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("PS2X_MOLTENVK_LIBRARY:"):
                value = line.split("=", 1)[1].strip()
                if value and not value.endswith("-NOTFOUND"):
                    lib = Path(value).resolve()
    if not lib or not lib.is_file():
        print("MoltenVK not bundled: the native Vulkan renderer will be unavailable")
        return
    dst = frameworks / "libMoltenVK.dylib"
    shutil.copy2(lib, dst)
    dst.chmod(0o755)
    run("install_name_tool", "-id", "@rpath/libMoltenVK.dylib", dst)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, help="BT3 USA ISO (SLUS-21678)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/macos-dist/BT3-Recomp.app",
                        help="destination .app; replaced only after packaging and verification succeed")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--skip-setup", action="store_true", help="reuse generated sources and rebuild")
    parser.add_argument("--skip-build", action="store_true", help="package existing build products only")
    parser.add_argument("--deployment-target", default=os.environ.get("MACOSX_DEPLOYMENT_TARGET") or platform.mac_ver()[0],
                        help="minimum macOS version (defaults to this Mac; dependencies are checked)")
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("this script must run on macOS")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if not args.skip_setup and not args.skip_build and not args.iso:
        parser.error("provide --iso, --skip-setup or --skip-build")
    dest = args.output.expanduser().absolute()
    if dest.suffix != ".app":
        parser.error("--output must name a .app path")
    build = Path(os.environ.get("PS2X_BUILD_DIR") or ROOT / "build").resolve()
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET=args.deployment_target)
    if not args.skip_build:
        setup = ["python3", ROOT / "games/bt3/setup.py"]
        setup += ["--skip-setup"] if args.skip_setup else [args.iso.expanduser().resolve()]
        run(*setup, "--jobs", args.jobs, env=env)
    runner = build / "ps2xRuntime/ps2EntryRunner"
    if not runner.is_file():
        parser.error("runner is missing; build first")
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Publish only after deployment and verification succeed.
    with tempfile.TemporaryDirectory(prefix=".bt3-stage-", dir=dest.parent) as tmp:
        app = Path(tmp) / "BT3-Recomp.app"
        # The front-end lives inside the runner, so the bundle is just the runner plus its
        # resources, closure and icon; there is no second executable to embed.
        bundled_runner = app / "Contents/MacOS" / RUNNER_NAME
        resources = app / "Contents/Resources"
        bundled_runner.parent.mkdir(parents=True)
        resources.mkdir(parents=True)
        shutil.copy2(runner, bundled_runner)
        shutil.copytree(ROOT / "ps2xRuntime/assets", resources / "assets", dirs_exist_ok=True)
        info = {
            "CFBundleName": "BT3-Recomp",
            "CFBundleDisplayName": "BT3-Recomp",
            "CFBundleIdentifier": "org.bt3recomp.app",
            "CFBundleExecutable": RUNNER_NAME,
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": "1.0",
            "CFBundleVersion": "1.0",
            "CFBundleIconFile": "BT3-Recomp.icns",
            "NSHighResolutionCapable": True,
            "LSMinimumSystemVersion": args.deployment_target,
        }
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
        make_icns(resources / "assets/icon.png", resources / "BT3-Recomp.icns", Path(tmp))
        dylib_closure(bundled_runner, app / "Contents/Frameworks")
        bundle_moltenvk(build, app / "Contents/Frameworks")
        binaries = audit(app, args.deployment_target)
        # Sign inside out: the runner and every deployed dylib.
        for binary in sorted(binaries, key=lambda p: len(p.parts), reverse=True):
            run("codesign", "--force", "--sign", "-", binary)
        for framework in app.glob("Contents/Frameworks/*.framework"):
            run("codesign", "--force", "--sign", "-", framework)
        run("codesign", "--force", "--sign", "-", app)
        run("codesign", "--verify", "--deep", "--strict", app)
        # Keep one predictable app path for local use.  Retain the previous bundle until the
        # newly staged bundle has passed deployment and signature verification, so a failed
        # publish never leaves the user without a runnable app.
        previous = Path(tmp) / "previous.app"
        had_previous = dest.exists()
        if had_previous:
            dest.rename(previous)
        try:
            app.rename(dest)
        except Exception:
            if had_previous:
                previous.rename(dest)
            raise
        if had_previous:
            shutil.rmtree(previous)

    # The runtime anchors data/, savedata/ and logs/ to the executable directory, which inside a
    # bundle is Contents/MacOS. Leaving them there would put gigabytes of game data (and the
    # player's saves) inside the app, where replacing or moving the bundle loses them. Linking the
    # writable names to Application Support is what actually makes the promise below true, and it
    # needs no environment variable, so it works from Finder, Dock and `open` alike.
    support = Path.home() / "Library" / "Application Support" / "BT3-Recomp"
    macos_dir = dest / "Contents" / "MacOS"
    linked = []
    for name in ("data", "savedata", "mods", "logs"):
        target = support / name
        target.mkdir(parents=True, exist_ok=True)
        link = macos_dir / name
        if link.is_symlink() or link.exists():
            if link.is_symlink() and link.resolve() == target.resolve():
                linked.append(name)
                continue
            if link.is_dir() and not link.is_symlink():
                # First run on a bundle that already collected data: move it out, then link.
                for child in link.iterdir():
                    shutil.move(str(child), str(target / child.name))
                shutil.rmtree(link)
            else:
                link.unlink()
        link.symlink_to(target, target_is_directory=True)
        linked.append(name)
    print(f"Ready: {dest}")
    print(f"Game data and saves: {support}  (linked into the bundle: {', '.join(linked)})")
    print("Local ad-hoc signature; Developer ID and notarization are not performed.")


if __name__ == "__main__":
    main()
