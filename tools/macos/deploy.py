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


def version(value):
    return tuple((list(map(int, value.split("."))) + [0, 0])[:3])


def audit(app, minimum):
    """Reject unresolved external libraries and a falsely advertised OS floor."""
    required_arches = set(output("lipo", "-archs", app / "Contents/MacOS/bt3-runner").split())
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
    if not (app / "Contents/PlugIns/platforms/libqcocoa.dylib").is_file():
        raise RuntimeError("Qt Cocoa platform plugin is missing")
    return macho


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, help="BT3 USA ISO (SLUS-21678)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/macos-dist/BT3-Recomp.app",
                        help="destination .app; must not already exist")
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
    if dest.suffix != ".app" or dest.exists():
        parser.error("--output must name a new .app path")
    build = Path(os.environ.get("PS2X_BUILD_DIR") or ROOT / "build").resolve()
    qt = Path(output("brew", "--prefix", "qt"))
    deployqt = qt / "bin/macdeployqt"
    if not deployqt.is_file():
        parser.error(f"macdeployqt not found at {deployqt}")
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET=args.deployment_target)
    if not args.skip_build:
        setup = ["python3", ROOT / "games/bt3/setup.py"]
        setup += ["--skip-setup"] if args.skip_setup else [args.iso.expanduser().resolve()]
        run(*setup, "--jobs", args.jobs, env=env)
        run("cmake", "-S", ROOT / "ps2xRuntime/src/launcher", "-B", build / "launcher",
            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={qt}",
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={args.deployment_target}", env=env)
        run("cmake", "--build", build / "launcher", "-j", args.jobs, env=env)
    runner = build / "ps2xRuntime/ps2EntryRunner"
    launcher = build / "launcher/Launcher.app"
    if not runner.is_file() or not launcher.is_dir():
        parser.error("runner or Launcher.app is missing; build first")
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Publish only after deployment and verification succeed.
    with tempfile.TemporaryDirectory(prefix=".bt3-stage-", dir=dest.parent) as tmp:
        app = Path(tmp) / "BT3-Recomp.app"
        shutil.copytree(launcher, app, symlinks=True)
        bundled_runner = app / "Contents/MacOS/bt3-runner"
        shutil.copy2(runner, bundled_runner)
        resources = app / "Contents/Resources"
        shutil.copytree(ROOT / "ps2xRuntime/assets", resources / "assets", dirs_exist_ok=True)
        for name in ("background.png", "icon.png"):
            src = ROOT / "ps2xRuntime/src/launcher/assets" / name
            if src.is_file():
                shutil.copy2(src, resources / "assets" / name)
        plist = app / "Contents/Info.plist"
        info = plistlib.loads(plist.read_bytes())
        info["LSMinimumSystemVersion"] = args.deployment_target
        plist.write_bytes(plistlib.dumps(info))
        # Homebrew's Qt plugins use @rpath for non-Qt dependencies. macdeployqt
        # only searches Qt's own prefix by default, so provide every installed
        # formula lib directory and let it close the complete dependency graph.
        brew_opt = Path(output("brew", "--prefix")) / "opt"
        library_paths = sorted(
            path for formula in brew_opt.iterdir()
            if (path := formula / "lib").is_dir()
        )
        deploy_args = [deployqt, app, f"-executable={bundled_runner}",
                       "-always-overwrite", "-no-codesign", "-no-plugins"]
        deploy_args += [f"-libpath={path}" for path in library_paths]
        run(*deploy_args)
        # The launcher only needs Qt's Cocoa platform plugin. Copying every
        # installed plugin drags WebEngine/PDF/virtual-keyboard dependency
        # trees into an otherwise small Widgets application.
        cocoa_src = qt / "share/qt/plugins/platforms/libqcocoa.dylib"
        cocoa_dst = app / "Contents/PlugIns/platforms/libqcocoa.dylib"
        cocoa_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(cocoa_src, cocoa_dst)
        run("install_name_tool", "-rpath", "@loader_path/../../../../lib",
            "@loader_path/../../Frameworks", cocoa_dst)
        binaries = audit(app, args.deployment_target)
        # Sign inside out, including the non-Qt runner and every deployed dylib.
        for binary in sorted(binaries, key=lambda p: len(p.parts), reverse=True):
            run("codesign", "--force", "--sign", "-", binary)
        for framework in app.glob("Contents/Frameworks/*.framework"):
            run("codesign", "--force", "--sign", "-", framework)
        run("codesign", "--force", "--sign", "-", app)
        run("codesign", "--verify", "--deep", "--strict", app)
        app.rename(dest)
    print(f"Ready: {dest}\nGame data and saves: ~/Library/Application Support/BT3-Recomp")
    print("Local ad-hoc signature; Developer ID and notarization are not performed.")


if __name__ == "__main__":
    main()
