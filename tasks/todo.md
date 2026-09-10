# Extracción de archivos con nombres reales ("propper names and formats")

## Contexto / Diagnóstico
- La tool de referencia (`AFS-Manager-CLI` = repo `MatrixDJ96/DBZBT3`) y la tabla embebida
  del AFS usan nombres **truncados/ofuscados** (ej. `WorldTou`, `VIC-JP-B-`, `res`).
- El `.db` (`debug_font_PS2_.db`) NO es una lista: es un **font/glifo** (bytes de
  píxeles). La lista de carga real del ISO es `PZS3US.DIR` (solo los 8 archivos tope)
  y los `.ALG` son offsets de alineado. No hay otra fuente de nombres en el ISO.
- Comunidad: los **AFL nuevos** de ViveTheModder (`vitetheModder.github.io`, repo Apache-2.0)
  traen el nombre real + formato de cada archivo, alineados por índice del AFS:
  - PZS3US1: 3399 archivos → `.pak` (2327), `.cdbt` (658), `.dbt` (323), `.gsc` (50), `.cpak` (41)
  - PZS3US2: 65201 archivos → `.adx` (65201, todo sonido/voces)
  - 0 duplicados, 0 nombres vacíos, 0 no-ASCII → formatos reales legibles.

## Elementos verificables
- [x] Descargar AFL nuevos y confirmar formato binario (`AFL\0` + u32s + count@12 + names 32B@16).
- [x] `loadNameTable` parsea binario AFL (valida count == entries) + legacy texto (`idx\tname`);
      prioridad: `.afl` real → tabla embebida → fallback.
- [x] Reemplazar `assets/PZS3US1.afl`, `assets/PZS3US2.afl` con los AFL nuevos.
- [x] NOTICE Apache-2.0 + crédito ViveTheModder en README.
- [x] CMake POST_BUILD copia `PZS3US1.afl`, `PZS3US2.afl`, `NOTICE` a `assets/` del build.
- [x] Launcher Qt compila localmente con los assets nuevos (build/assets verificado).
- [x] Re-extracción: PZS3US1 (3399) + PZS3US2 (65201) byte-idénticos vs AFS (0 mismatches).
- [x] Regenerar `portable/data/DATA` (idx v3 + nombres reales): PZS3US0/1/2 OK.
- [x] Validación contrato runtime: idx v3 resuelve cada slot a `folder/<name>` byte-idéntico (python).
- [ ] Rebuild docker + package + redeploy al Escritorio (aún con runner build v2 en portable).
- [ ] Commit + push del fix AFS (PR #5).

## Notas
- PZS3US0.AFS (14336B) recuperado del ISO: 1 entrada `boot_texture_PS2_.d` (12888B).
- Los AFL nuevos quedan embebidos en `src/launcher/assets/` para que el usuario final
  los tenga (no accede a internet/repo).
---

# Port launcher → Windows/macOS (input multiplataforma) — 2026-09-09

## Tarea
Reemplazar el backend de input Linux del launcher (evdev, linux/input.h) por uno
multiplataforma, y portabilizar los puntos POSIX del launcher. GitHub Actions
quedó abandonado por decisión del usuario; la validación Windows es dual-boot.

## Elementos verificables
- [x] `input_reader.{h,cpp}` GLFW 3.4 (FetchContent) joystick + QKeyEvent teclado; API `evin::` estable.
- [x] Borrar `evdev_reader.{h,cpp}`; tabs usan `input_reader.h`; sin `linux/input.h`.
- [x] `tab_bindings`: `evKeyToRaylib` → `qtKeyToRaylib` + eventFilter Qt (captura teclado).
- [x] Launcher CMake: GLFW FetchContent 3.4, `if(NOT MSVC)` en `-Wall -Wextra`, POST_BUILD copia `background.png`/`icon.png`.
- [x] `_WIN32`: `bt3-runner.exe`, LD_LIBRARY_PATH solo POSIX, `MoveFileEx` para rename atómico en `pad_config_reader.cpp`, setPermissions no-op en Windows (`extract_worker.cpp`).
- [x] Build launcher local (Arch): CONFIG/BUILD OK, smoke offscreen rc=124 (sin crash).
- [x] Fix GLFW Wayland en el contenedor: `GLFW_BUILD_WAYLAND OFF` (falta wayland-scanner en ubuntu:22.04).
- [x] Fix `entrypoint.sh` SIGPIPE (`find | head -40` + pipefail) — el clone local aún no tenía el fix de PR #6.
- [x] `entrypoint.sh` + `package.sh` ahora incluyen `data/` en stage/tarball (faltaba; el tarball salía sin el juego).
- [x] Flujo docker de 0: runner+launcher compilan (glibc floor 2.35), tarball 1.9G con `data/` + `install game.sh` + `.sha256`.
- [x] descomprimir → `install game.sh` (HOME temporal) → .desktop válido → Launcher corre → savedata preservado en re-install.
- [x] Docs: `docs/DEPLOY.md` y `README.md` reescritos (sin SELFX/stub; portable tripla).

## Notas
- Tarball release nuevo en `~/Escritorio/` (BT3-Recomp-x86_64.tar.gz 1.9G + .sha256), regenerado del flujo docker.
- Portal del Escritorio ahora con launcher contenedor (glibc 2.35) reemplazado.
- Queda como tarea futura: build Windows real via dual-boot del usuario, y PR con todo esto.
