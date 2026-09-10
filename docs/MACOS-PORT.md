# Port a macOS — estado actual y plan

## Actualización de implementación · 2026-09-09

El informe original que sigue se conserva como diagnóstico inicial; sus afirmaciones
«no compila» y «no se ha compilado» describen el checkout anterior al port.

Cambios implementados:

- B1–B3: sampler EE desactivado en plataformas sin backend; el contador de fases
  usa `mach_absolute_time()` en macOS, incluida la calibración del runtime.
- B4: lector evdev con stub fuera de Linux, sin quitar la pestaña de bindings.
  La captura se deshabilita y se mantienen los índices lógicos de mandos y las
  configuraciones guardadas. Los mandos del juego siguen usando GLFW.
- Otro bloqueo real: `Kernel/Syscalls/Thread.cpp` incluía `xmmintrin.h` sin guarda.
  Ahora usa la cabecera SIMD del runtime.
- CMake selecciona SIMD según la arquitectura de salida y rechaza Universal 2;
  macOS compila sin contracción FMA para conservar los redondeos separados.
- El runner resuelve su directorio con `_NSGetExecutablePath()`; el fallback
  anterior de `/proc/self/exe` ubicaba mal preferencias y assets en macOS.
- La primera generación real expuso una carrera del recompilador: un resultado
  recién terminado podía seguir en `readyCode` al comprobar si faltaba una función.
  Se corrigió la comprobación y el límite de resultados pendientes.
- `build_and_deploy_macos.sh` y `tools/macos/deploy.py` preparan un `.app` con
  `macdeployqt`, revisión de dependencias/arquitecturas/versión mínima y firma ad-hoc.
  Datos, partidas y ajustes quedan fuera del bundle, en
  `~/Library/Application Support/BT3-Recomp/`.

Comandos del port:

```sh
brew install cmake ninja pkg-config ffmpeg qt
python3 games/bt3/setup.py /ruta/bt3-usa.iso --jobs 3
./build_and_deploy_macos.sh --skip-setup --output /ruta/BT3-Recomp.app
```

La versión mínima por defecto es la del Mac de compilación. No se promete una
versión más antigua que la requerida por las dylibs de Homebrew. La firma ad-hoc
es para uso local: no equivale a Developer ID ni a notarización.

Pruebas efectuadas hasta ahora en Apple Silicon, macOS 26.2, AppleClang 17:

- Verificación SHA-256 de la ISO USA y generación completa de runner y overlay.
- Compilación del runtime y del launcher Qt; arranque breve del launcher, también
  desde un directorio de trabajo distinto al del bundle.
- Bundle ARM64 autónomo de 178 MB: dependencias externas auditadas, firma ad-hoc
  verificada con `codesign --verify --deep --strict` y plugin Cocoa incluido.
- Ejecución del runner empaquetado con los datos extraídos de `SLUS-21678`: inicia
  Cocoa/OpenGL 4.1 sobre Apple M1 Pro, Core Audio, carga el ELF y entra al bucle
  del juego; el cierre solicitado termina limpiamente.
- Veinte generaciones paralelas del mapa de gaps coinciden byte a byte con la
  generación secuencial (`tools/tests/recomp_parallel_smoke.py`).
- Pruebas MMI/SIMD contra referencias escalares: pasan en ARM64 y en x86-64 bajo
  Rosetta. Contador de fases y stubs también probados en ambas arquitecturas.

La compilación completa del runner y la prueba gráfica básica pasan en Apple Silicon.
La paridad visual con un Mac Intel físico, el rendimiento en combate, el audio y
los mandos reales requieren validación adicional. El backend Metal/MoltenVK y el
profiler EE nativo de la fase opcional no se implementaron.

---

> **Informe técnico** · BT3-Recomp (SLUS-21678) · 2026-09-09
>
> | | |
> |---|---|
> | Commit analizado | `813b6a6` (`main`, limpia) |
> | Método | Análisis estático del árbol |
> | Compilado / ejecutado | **No** |
> | Soporte declarado hoy | Linux · Windows (experimental) |

Estado del repositorio frente a macOS, los cuatro puntos que impiden compilar hoy, y un plan
por fases con puertas de decisión. **El núcleo es portable; el empaquetado no lo es en absoluto.**

---

## Índice

1. [Veredicto](#0-veredicto)
2. [Qué es esto, para quien llegue de nuevo](#1-qué-es-esto-para-quien-llegue-de-nuevo)
3. [Lo que ya funciona sin tocar nada](#2-lo-que-ya-funciona-sin-tocar-nada)
4. [Bloqueos de compilación](#3-bloqueos-de-compilación)
5. [Empaquetado: aquí está el trabajo de verdad](#4-empaquetado-aquí-está-el-trabajo-de-verdad)
6. [Techo gráfico: el coste que hay que aceptar](#5-techo-gráfico-el-coste-que-hay-que-aceptar)
7. [Riesgo SIMD en Apple Silicon](#6-riesgo-simd-en-apple-silicon)
8. [Plan por fases](#7-plan-por-fases)
9. [Trampas conocidas](#8-trampas-conocidas)
10. [Punto de arranque](#9-punto-de-arranque)
11. [Qué no se ha verificado](#10-qué-no-se-ha-verificado)

---

## 0. Veredicto

macOS **no está soportado hoy** y no compila. Pero el trabajo es menor de lo que el README
sugiere: no hay `mmap`, ni JIT, ni memoria ejecutable, ni `dlopen`, ni ensamblador inline. La
memoria invitada es `new uint8_t[]` plano. El renderer es OpenGL 3.3, dentro del techo de macOS.
La ruta ARM64 vía `sse2neon` ya existe en el CMake, con un caso `APPLE` escrito explícitamente.

Los bloqueos de compilación son **cuatro sitios en tres ficheros**, todos en código opcional (dos
profilers y el lector evdev del launcher). Un día de trabajo debería dar un binario que arranca en
un Mac Intel.

El coste real está en dos sitios distintos:

- **El empaquetado**, que es ELF de punta a punta y hay que reescribir, no parchear.
- **El rendimiento**, porque macOS se queda sin el camino rápido de vértices que el propio
  repositorio describe como determinante en máquinas modestas.

---

## 1. Qué es esto, para quien llegue de nuevo

BT3-Recomp no es un emulador. Es una **recompilación estática**: el ejecutable MIPS del juego
(PS2, USA, SLUS-21678) y su overlay se traducen a **~7.800 ficheros C++** en tiempo de compilación,
a partir de la ISO del propio usuario, y se enlazan contra un runtime que emula el hardware
alrededor (GS, VIF, VU1, IOP, pads) con un renderer OpenGL. El repositorio no contiene código ni
assets del juego.

Consecuencia importante para el port: **no hay generación de código en ejecución**. Nada de páginas
`PROT_EXEC`, nada que choque con la firma de código ni con el *hardened runtime* de macOS. Eso
elimina de golpe la parte que normalmente hace difícil portar un emulador a Apple.

| Componente | Rol | Relevancia para macOS |
|---|---|---|
| `ps2xRecomp` | El recompilador: ELF → C++ | C++ puro, portable. Sin hallazgos. |
| `ps2xRuntime` | Runtime + renderer + runner del juego | Donde están los 4 bloqueos. |
| `ps2xRuntime/src/launcher` | Launcher Qt6 (configuración, wizard) | Sin guardas de plataforma. Bloqueo. |
| `ps2xAnalyzer` / `ps2xTest` | Herramientas de análisis y tests | Sin hallazgos de plataforma. |
| `ps2xStudio` | Editor; 4 fetches git al configurar | `OFF` por defecto en setup.py. Ignorar. |
| `games/bt3/setup.py` | Pipeline: ISO → generación → build | Solo dos ramas: Windows y «el resto». |
| `build_and_deploy.sh` | Ensambla el ELF autoextraíble | Inservible en macOS. Reescritura. |
| `tools/release/` | Release reproducible en Docker | Sin equivalente macOS. Ver fase 4. |

> El pipeline de generación tarda unos minutos con `--jobs 16`; el default conservador es `-j3`.
> Pide ~16 GB de RAM y ~10 GB de disco.

---

## 2. Lo que ya funciona sin tocar nada

Conviene inventariarlo primero, porque es la mayor parte del sistema y evita trabajo especulativo.

| Pieza | Evidencia | Por qué no es problema |
|---|---|---|
| Memoria invitada | `ps2_memory.cpp:362-403` | RDRAM, scratchpad, IOP RAM, VRAM del GS y VU0/VU1 son `new uint8_t[]`. Cero `mmap`, cero `MAP_FIXED`. |
| Detección de arquitectura | `CMakeLists.txt:43-49` | `CMAKE_SYSTEM_PROCESSOR` casa `arm64`, que es lo que reporta macOS en Apple Silicon. |
| Ruta SIMD para ARM | `CMakeLists.txt:56-90` | Trae `sse2neon` v1.9.1 por FetchContent y define `USE_SSE2NEON`. La rama `AARCH64 AND APPLE` (líneas 72-74) ya está escrita. |
| Cabecera de macros SIMD | `ps2_runtime_macros.h:8-14` | `_MSC_VER` → `USE_SSE2NEON` → `immintrin.h`. Los tres caminos ya existen. |
| Los ~7.800 ficheros generados | `ps2_recompiler.cpp:105`<br>`function_emitter.cpp:45` | El codegen emite **únicamente** `#include "ps2_runtime_macros.h"`. Arreglar esa cabecera arregla todo el código generado de golpe. |
| Nombres de hilo | `ThreadNaming.h:47` | Ya tiene rama `__APPLE__` con la firma correcta de `pthread_setname_np`. |
| evdev del runtime | `ps2xRuntime/CMakeLists.txt:426` | `if(UNIX AND NOT APPLE)` ya excluye `pad_evdev_linux.cpp`. |
| Mandos | `pad_config.cpp:7` | `PadEvdevStub` sustituye al lector nativo donde no hay evdev. Los mandos entran por GLFW, que en macOS usa IOKit. **No es un bloqueo.** |
| Los 27 `__linux__` | 6 ficheros | Todos son evdev o *thread pinning* (`ps2_runtime.cpp:360`, `:4133`, `:5285`). Funcionalidad opcional, ya compila fuera limpiamente. |
| Carga de GL por nombre | `ps2_gs_gpu_renderer.cpp:389` | Usa `dlsym(RTLD_DEFAULT, …)`, que funciona igual en macOS. |
| Shaders | 11 sitios, `#version 330` | GLSL 3.30 entra en el techo GL 4.1 de macOS. Sin *compute*, sin SSBO. |
| Dependencias | `ps2xRuntime/CMakeLists.txt:65-170`, `:361-372` | raylib 5.5, imgui (`docking`) y rlImGui por FetchContent; FFmpeg por `pkg-config`. Todo resoluble con Homebrew. |
| Parche de raylib | `patches/raylib-5.5-ps2x.patch` | Solo toca `src/config.h` y `src/rlgl.h`. Neutral de plataforma; se aplica igual. |
| Extracción de la ISO | `setup.py:50-58` | Busca `bsdtar` y cae a `tar`. El `tar` de macOS *es* bsdtar y lee ISO9660 directamente. |
| Compilador | README | El código VU1 generado requiere Clang (MSVC no puede compilarlo). En macOS Clang es el compilador por defecto: ventaja, no obstáculo. |

> [!TIP]
> **El hallazgo que más ahorra.** Los ficheros generados solo incluyen `ps2_runtime_macros.h`. No
> hay que tocar el codegen ni regenerar nada para dar soporte a Apple Silicon a nivel de SIMD: la
> cabecera ya resuelve las tres familias de intrínsecos y los 30.084 usos de `_mm_*` del runtime
> pasan por ella.

---

## 3. Bloqueos de compilación

Cuatro sitios. Ninguno afecta a funcionalidad del juego: dos profilers opt-in y el lector evdev del
launcher. Todos se resuelven con guardas de plataforma y stubs.

### B1 — El profiler EE usa APIs exclusivas de glibc/Linux

**Fichero:** `ps2xRuntime/src/lib/ps2_eeprof.cpp:20-35` y siguientes

**Qué rompe:**

- `timer_create(CLOCK_THREAD_CPUTIME_ID, …)` (`:255`) y `timer_settime` (`:259`) — los timers POSIX
  no existen en macOS.
- `SIGEV_THREAD_ID` y `sev._sigev_un._tid` (`:254`) — extensión de Linux.
- `syscall(SYS_gettid)` (`:254`) — no hay `SYS_gettid` en macOS.
- `((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP]` (`:150`) — layout de glibc x86-64; en macOS es
  `uc_mcontext->__ss.__rip`, y en ARM64 no hay RIP.

**Por qué duele:** el fichero está en la lista de fuentes de `ps2_runtime`
(`ps2xRuntime/CMakeLists.txt:398`), así que se compila siempre. La rama `#else` asume Linux sin
comprobarlo.

**Arreglo:** reguardar la rama como `#if defined(__linux__)` y dejar stubs no-op en macOS. Es un
profiler activado por `PS2X_EEPROF`: no se pierde nada del juego. Un port completo lo
reimplementaría con `dispatch_source` + `thread_get_state`.

**Esfuerzo:** ≈1 h para el stub. 1-2 días si se quiere el profiler funcionando de verdad (no urgente).

### B2 — `x86intrin.h` y `__rdtsc()` sin guarda

**Fichero:** `ps2xRuntime/include/runtime/ps2_guestprof.h:7`, con usos en `:20`, `:29`, `:38`

**Qué rompe:** `#include <x86intrin.h>` incondicional. En Apple Silicon la cabecera no existe y
`__rdtsc()` tampoco (`sse2neon` ofrece `_rdtsc()`, con un underscore).

**Arreglo:** un shim: en `__aarch64__` leer `cntvct_el0` vía `__builtin_arm_rsr64`, o
`mach_absolute_time()`. Ojo: la frecuencia del contador ARM no es la del TSC, y `ps2_runtime.cpp`
calibra los ticks contra el reloj de pared en cada impresión, así que el ratio se corrige solo.

**Esfuerzo:** ≈30 min.

### B3 — Un `__rdtsc()` más, fuera del guardado

**Fichero:** `ps2xRuntime/src/lib/ps2_runtime.cpp:5226`

**Qué rompe:** mismo intrínseco, en el bloque de calibración de `guestprof`. Se arregla con el shim
de B2; se lista aparte para que no se olvide al hacer grep solo en cabeceras.

**Esfuerzo:** incluido en B2.

### B4 — El launcher Qt no tenía ninguna guarda de plataforma *(resuelto aguas arriba)*

**Estado:** ya no hace falta nada aquí. El diagnóstico original era que
`src/launcher/evdev_reader.cpp` y `src/launcher/tab_bindings.cpp` incluían `<linux/input.h>` sin
guardas y que `src/launcher/CMakeLists.txt` los metía en la compilación con un `file(GLOB *.cpp)`,
de modo que el launcher nunca había compilado fuera de Linux.

El puerto de input multiplataforma de upstream (`input_reader.{h,cpp}`, GLFW para joysticks y
eventos de teclado de Qt) eliminó `evdev_reader` por completo y con él la raíz del problema: no
queda un solo `linux/input.h` ni `/dev/input` en el launcher, y el mismo código compila en Linux,
Windows y macOS. Este puerto ya no toca esos ficheros; la solución de upstream es mejor que el
stub «no disponible» que se había previsto aquí.

**Esfuerzo:** 0 — resuelto aguas arriba.

> Verificado **ausente** en todo el árbol (excluyendo `thirdparty/`): ensamblador inline,
> `__builtin_ia32_*`, `__cpuid`, `mmap`, `VirtualAlloc`, `dlopen`. El único `sys/syscall.h` es el de
> B1 y los únicos `linux/input.h` son los de B4 más `pad_evdev_linux.cpp`, que ya está excluido.

---

## 4. Empaquetado: aquí está el trabajo de verdad

> **Nota de actualización.** Upstream pasó desde entonces a distribuir una **carpeta portable**
> idéntica en Linux, Windows y macOS, en lugar del ELF autoextraíble. La tabla de abajo se
> conserva porque el análisis de cada dependencia de Linux sigue siendo válido, y porque explica
> por qué en macOS la ruta es un bundle `.app`; lo que ya no aplica es el stub autoextraíble.


El formato de distribución en Linux es **un único ELF autoextraíble**:
`[stub estático][payload tar+zstd][footer de 32 B]`. En el primer arranque se descomprime en
`~/.cache/bt3-recomp/<seed>/`, donde la *seed* viene del hash del payload, de forma que cada
rebuild invalida su propia caché. Está documentado en `docs/DEPLOY.md`.

**Nada de esto se traslada a macOS.** No es cuestión de parchear el script: el concepto entero
(stub enlazado estáticamente, `LD_LIBRARY_PATH`, formato ELF) no tiene equivalente.

| Dependencia de Linux | Dónde | Equivalente en macOS |
|---|---|---|
| `gcc -static` | `build_and_deploy.sh:88` | **Ninguno.** macOS no permite enlazar libc estáticamente. El stub no es viable. |
| `readlink("/proc/self/exe")` | `tools/selfx/stub.c:276` | `_NSGetExecutablePath()` |
| `LD_LIBRARY_PATH` | `stub.c:372`, `:417` | `@rpath` / `@executable_path` vía `install_name_tool` |
| `ldd` + `mapfile` | `build_and_deploy.sh:97` | `otool -L`. Y `mapfile` no existe en el bash 3.2 de Apple. |
| Lista negra de glibc | `build_and_deploy.sh:112` | Innecesaria: macOS no tiene el problema de `GLIBC_PRIVATE`. |
| `sha256sum` | `build_and_deploy.sh:125` | `shasum -a 256` |
| `nproc` | `build_and_deploy.sh:27` | `sysctl -n hw.ncpu` |
| `realpath -m` | `build_and_deploy.sh:49` | No existe. `python3 -c os.path.abspath` o coreutils de brew. |
| Ruta Qt6 fija | `build_and_deploy.sh:148` | `/usr/lib/cmake/Qt6/Qt6Config.cmake` nunca existirá; usar `CMAKE_PREFIX_PATH` con `brew --prefix qt6`. |
| Concatenar ELF + footer | `build_and_deploy.sh:128-135` | Bundle `.app`, o DMG. Mach-O no admite este truco tal cual. |
| Copia del runner | `setup.py:212-217` | La rama `else` asume Linux; en macOS cae aquí por `os.name == "posix"` (`setup.py:36`). |
| Release en Docker | `tools/release/` | Ubuntu 22.04 fija el suelo glibc 2.35, con `check_floor.sh` como puerta. En macOS el equivalente es `-mmacosx-version-min` + `MACOSX_DEPLOYMENT_TARGET`, y no hay contenedor: hace falta un Mac. |

### Forma que debería tomar

- Un bundle `BT3-Recomp.app`: binario en `Contents/MacOS`, dylibs en `Contents/Frameworks`, assets
  en `Contents/Resources`, más un `Info.plist` con la versión mínima de sistema.
- `install_name_tool` / `@rpath` para reubicar dylibs. Existen `dylibbundler` y `macdeployqt`;
  **`macdeployqt`** resuelve además los plugins de Qt, que es la parte que siempre se olvida y
  produce el fallo «could not find the Qt platform plugin cocoa».
- `codesign --sign -` (ad-hoc) como mínimo. Sin firma, Gatekeeper mata cualquier binario que el
  usuario haya descargado. Para distribuir de verdad hacen falta Developer ID + notarización, lo que
  implica cuenta de pago de Apple: **decisión de producto, no técnica**.
- Universal 2 (`x86_64;arm64`) es posible con `CMAKE_OSX_ARCHITECTURES`, pero duplica un build que
  ya es de 7.800 unidades de traducción. Recomendación: dos artefactos separados.

---

## 5. Techo gráfico: el coste que hay que aceptar

macOS congeló OpenGL en **4.1** (y lo declaró obsoleto en 10.14). El renderer usa dos cosas por
encima de esa línea. Las dos degradan solas, sin fallar — y ahí está el problema: no se rompe, se
ralentiza en silencio.

### R1 — Se pierde el anillo de vértices persistente

**Qué:** `glBufferStorage` con `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` requiere
`GL_ARB_buffer_storage` (GL 4.4). Es la optimización `[vbring]` del parche de raylib.

**Comportamiento:** está gateada en `patches/raylib-5.5-ps2x.patch:104` por
`glBufferStorage != NULL`, así que macOS cae al camino original de `glBufferSubData`. Funciona. Pero
según el comentario del propio parche, ese camino costaba **~80 ms de GPU por segundo y ~2,5 µs de
CPU por flush**, con ~3.000 flushes por frame, y «dominaba las máquinas de gama baja».

**Implicación:** hay que medir antes de prometer nada. Si un Mac moderno absorbe el coste, no hay
problema; si no, la salida es un backend Metal o Vulkan vía MoltenVK, y eso ya es otro proyecto, no
un port.

### R2 — `glTextureBarrier` cae a `glFinish()`

**Qué:** `glTextureBarrier` es GL 4.5 (o `NV_texture_barrier`). `ps2_gs_gpu_renderer.cpp:392-396`
lo busca por `dlsym`, prueba la variante NV, y si no hay ninguna ejecuta `glFinish()`: un
sincronizado completo de la tubería donde solo se pedía una barrera de textura.

**Atenuante:** el comentario de `:866` dice que para el caso `[rtsnap]` ni `glFinish` ni
`glTextureBarrier` resolvían nada y que la solución fue un blit del framebuffer. Es decir: el camino
crítico ya no depende de la barrera. El impacto es menor que R1.

---

## 6. Riesgo SIMD en Apple Silicon

El runtime tiene **30.084 usos de intrínsecos `_mm_*`** (excluyendo `thirdparty/`) repartidos por el
rasterizador, el GS, el intérprete VIF1 y VU1. En ARM64 todos pasan por `sse2neon`. Ese es el punto
donde un port puede producir bugs sutiles y caros de diagnosticar: no fallos de compilación, sino
píxeles y geometría ligeramente mal por diferencias de redondeo o saturación.

La buena noticia es que el conjunto de intrínsecos de SSE4.1 realmente usados es corto, y
`sse2neon` v1.9.1 los cubre todos:

```
_mm_blendv_epi8   _mm_blendv_ps     _mm_cvtepi32_ps
_mm_extract_epi32 _mm_extract_epi64 _mm_insert_epi8
_mm_max_epi32     _mm_min_epi32     _mm_mullo_epi32
```

Además, `ps2_runtime_macros.h` no usa los intrínsecos en crudo: los envuelve en macros `PS2_*` por
instrucción MMI del R5900 (`PS2_PEXTLW`, `PS2_PADDW`, `PS2_PMAXW`…). Eso concentra la superficie de
riesgo en un fichero y da un sitio natural donde escribir tests diferenciales x86 ↔ ARM si aparecen
discrepancias.

> [!IMPORTANT]
> **No empezar por Apple Silicon.** Un Mac Intel valida el port *sin* meter `sse2neon` en la
> ecuación: si el juego pinta mal en Intel, el problema es del port; si pinta bien en Intel y mal en
> ARM, el problema es SIMD. Separar esas dos variables ahorra días.

---

## 7. Plan por fases

Las fases están numeradas porque el orden importa: cada una tiene una puerta que evita gastar
trabajo en la siguiente sobre una base que no se sostiene.

### Fase 1 — Compilar en Mac Intel · ≈1 día · B1·B2·B3·B4

Resolver los cuatro bloqueos con guardas y stubs. Nada de reimplementar profilers ni backends de
input: el objetivo es un binario que enlaza. Dependencias por Homebrew
(`cmake ninja pkg-config ffmpeg qt6 zstd`) y configurar con `CMAKE_PREFIX_PATH` apuntando a Qt6.

**Puerta:** `ps2EntryRunner` enlaza y arranca sin caerse antes del primer frame.

### Fase 2 — Que pinte · ≈1-3 días · sin empaquetar

Ejecutar desde `build/ps2xRuntime` con `PS2X_CD_IMAGE` apuntando a la ISO, sin bundle ni launcher.
Aquí se descubre si el perfil *core* estricto de macOS acepta el renderer, si los mandos entran por
GLFW, y si audio y vídeo (FFmpeg) funcionan. Es la fase con más incertidumbre real y donde vive el
riesgo del proyecto.

**Puerta:** un combate jugable a velocidad razonable. Medir aquí el impacto de R1 antes de decidir
cualquier cosa sobre backends gráficos.

### Fase 3 — Apple Silicon · ≈2-5 días · riesgo alto, poco predecible

Compilar en arm64 y validar `sse2neon` contra el comportamiento observado en Intel: mismas escenas,
misma captura, comparar. Los fallos aquí son visuales y silenciosos, no crashes. La estimación es la
menos fiable del informe.

**Puerta:** paridad visual con la build Intel en un conjunto acordado de escenas.

### Fase 4 — Bundle `.app` y distribución · ≈2-3 días · reescritura, no parche

Un `build_and_deploy_macos.sh` nuevo, hermano del de Linux, no una versión con `if`. Bundle,
`macdeployqt`, firma ad-hoc, y un `MACOSX_DEPLOYMENT_TARGET` declarado que cumpla el papel que en
Linux cumple el suelo de glibc 2.35. Actualizar `README.md` y `docs/DEPLOY.md`, que hoy afirman
«Linux o Windows».

**Puerta:** el `.app` arranca en un Mac que no es el de compilación y sin herramientas de desarrollo
instaladas.

### Fase 5 — Opcional: recuperar lo degradado · solo si la fase 2 lo pide

Profiler EE nativo (`dispatch_source` + `thread_get_state`), input nativo vía IOKit/GameController
para lo que GLFW no mapee, y —si el rendimiento no llegó— backend Metal o MoltenVK. Cada punto es
independiente y ninguno bloquea la distribución.

### Resumen de estimaciones

| Hito | Estimación | Confianza |
|---|---|---|
| Compila y arranca (Intel) | 1-2 días | Alta — el trabajo está identificado línea a línea |
| Jugable sin empaquetar (Intel) | +1-3 días | Media — depende de sorpresas del driver GL |
| Paridad en Apple Silicon | +2-5 días | Baja — depende de cuántos bugs de `sse2neon` aparezcan |
| `.app` distribuible | +2-3 días | Alta — trabajo conocido, solo laborioso |
| **Total, ambas arquitecturas** | **1-2 semanas** | Media |

Sin contar notarización de Apple, que es trámite administrativo y cuenta de pago.

---

## 8. Trampas conocidas

Cosas que van a costar tiempo a quien no las sepa de antemano.

- **No parchear `build_and_deploy.sh`.** Tiene siete dependencias de Linux entrelazadas, una de
  ellas (`gcc -static`) sin equivalente. Un script hermano sale más limpio y no rompe el camino de
  Linux, que funciona.
- **El bash de macOS es 3.2.** Cualquier script nuevo que use `mapfile`, `${x,,}` o arrays
  asociativos falla en un Mac limpio. O se ceñe a bash 3.2, o declare explícitamente que necesita el
  bash de Homebrew.
- **El launcher de Qt6 sin `macdeployqt` arrancará en la máquina de compilación y en ninguna otra.**
  El fallo típico es «could not find the Qt platform plugin cocoa», y confunde porque el binario
  existe y tiene permisos.
- **Gatekeeper.** Un `.app` sin firmar que se descargue queda en cuarentena y no arranca; en local
  funciona, en manos de otro no. Es la clase de bug que aparece justo al publicar.
- **La región está fijada.** Los mapas de funciones comiteados son del ejecutable USA (SLUS-21678).
  El port no cambia eso y no hay que prometer otras regiones.
- **`ps2xStudio` hace cuatro fetches git al configurar** y ya abortó builds de usuarios por un fallo
  de red. `setup.py` lo pone en `OFF` salvo `PS2X_SETUP_STUDIO=1`; un `cmake` a mano sobre la raíz sí
  lo activa. Mantenerlo apagado durante el port.
- **El build por defecto es `-j3`.** Con 7.800 unidades de traducción eso es una eternidad. Pasar
  `--jobs` con el número de núcleos, vigilando la RAM (~16 GB recomendados).

---

## 9. Punto de arranque

Para quien continúe: esto es lo que hay que ejecutar primero, antes de escribir una línea.

```sh
# 1. Dependencias (Mac Intel para la fase 1)
brew install cmake ninja pkg-config ffmpeg qt6 zstd python@3.12

# 2. Confirmar los 4 bloqueos en el checkout actual (deben salir estos y solo estos)
grep -rn "x86intrin\|__rdtsc" ps2xRuntime/include ps2xRuntime/src
grep -rn "linux/input.h" ps2xRuntime/src/launcher
sed -n '20,35p;150p;250,260p' ps2xRuntime/src/lib/ps2_eeprof.cpp

# 3. Configurar solo el runtime; Studio fuera, Qt6 localizado por brew
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_STUDIO=OFF \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt6)"

# 4. Pipeline completo desde la ISO (ajustar --jobs a los nucleos reales)
python3 games/bt3/setup.py /ruta/bt3-usa.iso --jobs 10

# 5. Ejecutar sin empaquetar (fase 2)
cd build/ps2xRuntime
PS2X_CD_IMAGE=/ruta/bt3-usa.iso ./ps2EntryRunner ../../games/bt3/work/SLUS_216.78
```

Variables de entorno útiles durante el diagnóstico, todas ya en el código: `PS2X_VBRING=0`
(desactiva el anillo de vértices, para comparar con el camino lento), `PS2X_RTSNAP=0`,
`PS2X_GUESTPROF=1`, `PS2X_EEPROF`, `PS2X_PIN` (solo Linux).

---

## 10. Qué no se ha verificado

Honestidad sobre el alcance de este informe, para que nadie lo tome por más de lo que es.

- **No se ha compilado nada.** Todo el informe es lectura del código en el commit `813b6a6`. Los
  bloqueos B1-B4 están identificados por inspección, no por un error de compilador. Es muy probable
  que la primera compilación real destape más, típicamente cabeceras transitivas y
  warnings-as-errors.
- **No se ha ejecutado el juego** en ninguna plataforma, así que el comportamiento del renderer bajo
  el perfil *core* estricto de macOS es predicción razonada, no dato.
- **No se ha auditado `thirdparty/`** más allá de comprobar que `xxhash.h` no aporta problemas.
  Puede haber más ahí.
- **No se han revisado los ~7.800 ficheros generados** uno a uno; la conclusión de que basta con
  `ps2_runtime_macros.h` viene de que el codegen solo emite ese include (`ps2_recompiler.cpp:105`,
  `function_emitter.cpp:45`).
- **Sin CI.** El repositorio no tiene workflows de GitHub; el único artefacto de release automatizado
  es `tools/release/Dockerfile`, que es Linux. Un macOS en CI necesitaría runners macOS, que es coste
  y decisión aparte.

---

*Informe elaborado por análisis estático del árbol en `main` @ `813b6a6`, 2026-09-09. Todas las
referencias `fichero:línea` corresponden a ese commit y se desplazarán con futuros cambios.*

*BT3-Recomp se construye sobre PS2Recomp. El repositorio no contiene código, assets ni media del
juego: la recompilación parte de la ISO del propio usuario.*
