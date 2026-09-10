# Lecciones aprendidas

## 2026-09-09 — Port launcher multiplataforma (GLFW + QKeyEvent)

- **`find | head -40` bajo `set -o pipefail` = SIGPIPE (rc=141)**: el pipeline
  muere si `head` cierra el pipe antes de que `find` termine. En scripts con
  `pipefail` hay que tolerarlo (`|| true`) o no truncar con `head`. El clone
  local tenía este bug desde PR #6 sin mergear.
- **GLFW via FetchContent en ubuntu:22.04 falla si falta `wayland-scanner`**:
  GLFW intenta compilar tanto backend X11 como Wayland. En el contenedor base
  hay que forzar `GLFW_BUILD_WAYLAND=OFF` (y `GLFW_BUILD_X11=ON`) en Linux;
  Windows/macOS usan su backend nativo sin ambiente `WAYLAND/X11`.
- **`std::rename` no sobrescribe en Windows**: falla si el destino existe.
  Para reemplazo atómico de `pad_p*.conf` hay que usar `MoveFileEx` con
  `MOVEFILE_REPLACE_EXISTING` bajo `_WIN32`.
- **El tarball release se armaba sin `data/`**: `package.sh` solo volcaba
  Launcher+bt3-runner+lib+assets y creaba `savedata`; el juego extraído quedaba
  en `games/bt3/work/`. `entrypoint.sh` (docker) debía copiar `data/` del work
  al stage. Regla: cada stage/artefacto debe validar su contenido (el tarball
  actualizado pasó de 94 MB a 1.9 GB con la data correcta).
- **El teclado Qt no es un dispositivo**: no se puede abrir como node evdev.
  Captura vía `eventFilter` en el tab + traducción `Qt::Key -> raylib code`,
  inyectado al reader con `captureKey`/`takeLastKey`.
- **Mantener la API `evin::` estable al portar**: así los tabs (bind capture +
  live test) solo cambian el include y el nombre del translate, no su lógica.