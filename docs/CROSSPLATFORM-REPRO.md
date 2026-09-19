# Cross-platform reproducibility review

Scope: the changes made for the **opening-FMV override**, the **texture-pack path
(`data/Textures`)**, and the **launcher texture-pack section (Misc tab + install)**.
Target platforms: Linux (reference), Windows (experimental), macOS (not currently
supported — see `docs/MACOS-PORT.md`).

This is an assessment only; no behavior was changed while writing it.

## Summary

| Area | Linux | Windows | macOS |
|---|---|---|---|
| FFmpeg (avformat/avcodec/swscale) linkage | pkg-config | prebuilt shared + DLL staging | pkg-config (Homebrew) |
| Qt Network (Install → Download) | present | present | present |
| OS-specific code in new files | none | none | none |
| `data/Textures` path resolution | OK | OK | OK (with launcher) |
| Texture pack unpacker | 7z/unrar/bsdtar/tar | `tar` = bsdtar (Win10+) | `bsdtar` |
| AV1 decode of the injected MP4 | depends on FFmpeg build | depends on FFmpeg build | depends on FFmpeg build |

## Portable / confirmed OK

- **FFmpeg**: the runtime links `libavformat`, `libavcodec`, `libavutil`,
  `libswresample`, `libswscale` on every platform. Windows uses a prebuilt shared
  build (`ps2xRuntime/CMakeLists.txt:284-368`, `avformat.lib` at `:306`) and stages
  all runtime DLLs with a `*.dll` glob (`ps2xRuntime/cmake/CopyFfmpegDlls.cmake:9`),
  so `avformat-*.dll` ships next to the runner. Linux/macOS resolve via
  `pkg-config` (`:370-380`). The new module adds no new dependencies.
- **Qt Network**: the launcher requires it on all platforms
  (`ps2xRuntime/src/launcher/CMakeLists.txt:14-16`) and links `Qt::Network`
  (`:68`). Available in the standard Qt installers and Homebrew Qt.
- **No platform-specific code in the new files**: `ps2_fmv_override.cpp`,
  `tex_pack.cpp`, `tex_install_dialog.cpp` use `std::filesystem`, `ps2xExeDirC()`
  and `PS2X_EXEDIR`. The only `#ifdef _WIN32` in `CD.cpp:563` is pre-existing and
  unrelated to `overrideCdFile`.
- **Path resolution is consistent**: the launcher writes packs to
  `apppaths::userRoot()/data/Textures` and exports
  `PS2X_EXEDIR=apppaths::userRoot()` (`launcher_window.cpp:227`); the runtime reads
  `<exeDir>/data/Textures` where `exeDir == PS2X_EXEDIR` → the same folder on
  Linux/Windows/macOS.
- **Case**: the folder is spelled `Textures` everywhere. Windows and macOS are
  case-insensitive, so there is no `textures` vs `Textures` collision.
- **Unpacker**: `pickUnpacker()` prefers `7zz`, `7z`, `unrar`, then `bsdtar`/`tar`.
  Windows 10+ and macOS ship `tar` = bsdtar (libarchive), which reads 7z, so the
  Install path works without 7-Zip installed.

## Risks and caveats

1. **AV1 decode depends on the FFmpeg build.** The reference opening MP4 is AV1
   (2880x2156). Decoding it needs libdav1d (or a native AV1 decoder). The system
   FFmpeg on the dev machine and the Windows prebuilt (n7.1) include it; a native
   Linux build bundles the build host's FFmpeg, whose libavcodec must include an
   AV1 decoder for the override to work. If a target's FFmpeg lacks AV1,
   `avcodec_find_decoder()` returns null and the override fails
   (the current message is generic: `[fmvoverride] decoder init failed`).
   **Action:** verify AV1 per target, or ship an H.264 variant of the opening.

2. **macOS is not supported today** (pre-existing, not caused by these changes):
   four compile blockers are documented in `docs/MACOS-PORT.md`
   (`syscall(SYS_gettid)`, ucontext `REG_RIP` layout, `xmmintrin.h` without a
   guard, and the launcher lacking platform guards). The new code is standard
   Qt/filesystem and adds no further blockers, but macOS reproducibility remains
   gated on those fixes.

3. **macOS bundle vs `userRoot`.** The runtime reads `<exeDir>/data/Textures`. With
   the launcher, `exeDir == PS2X_EXEDIR == ~/Library/Application Support/BT3-Recomp`,
   which is correct. If the runner is launched **directly** (without the launcher)
   on macOS, `ps2xExeDirC()` returns `…/Contents/MacOS`, so `data/Textures` would
   not be found. The CMake POST_BUILD staging into
   `$<TARGET_FILE_DIR>/data/Textures` lands inside the `.app` and is unused on
   macOS (harmless).

4. **Install without 7-Zip on Windows** relies on `tar.exe` (bsdtar) reading 7z. On
   older installs that lack it, a `.7z` download cannot be extracted. Consider
   bundling `7zr` or linking libarchive in the launcher.

5. **Windows packaging is a real flow.** `scripts/build-windows.ps1` (with
   `scripts/package-windows.ps1`) drives `games/bt3/setup.py` natively: ClangCL +
   Ninja, the Qt MSVC kit via aqtinstall, the FFmpeg prebuilt, the VC++ runtime
   DLLs and the PE dependency gate. The Windows branch lives in `setup.py`; the
   new code adds nothing Windows-specific.

## References

- `ps2xRuntime/src/lib/ps2_fmv_override.cpp` — FMV override module (FFmpeg/avformat).
- `ps2xRuntime/src/launcher/tex_pack.{h,cpp}`, `tex_install_dialog.{h,cpp}` — pack
  location, download (Qt Network), unpacker selection.
- `ps2xRuntime/CMakeLists.txt:284-380` — FFmpeg linkage (Windows prebuilt / pkg-config).
- `ps2xRuntime/cmake/CopyFfmpegDlls.cmake:9` — Windows FFmpeg DLL staging.
- `ps2xRuntime/src/launcher/CMakeLists.txt:14-16,68` — Qt Network.
- `ps2xRuntime/src/launcher/launcher_window.cpp:227` — `PS2X_EXEDIR` export.
- `ps2xRuntime/src/lib/ps2_texreplace.cpp` — `data/Textures` default (via `ps2xExeDirC`).
- `docs/MACOS-PORT.md` — macOS status and blockers.
