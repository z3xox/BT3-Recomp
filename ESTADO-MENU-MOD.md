# BT3 Menu Mod - Estado Actual y Lecciones Aprendidas

## Objetivo
Agregar una 10ma entry funcional al menú principal de BT3 PS2 (NTSC-U, SLUS-21678).

## Lo que funciona
- **Recomp runner**: Boot con `SLUS_216.78` como ELF alcanza `bt3state=0x1` (intro screen)
- **Slot 16 fix**: `g_ps2OverlayFunctionTable[16]` necesario para ejecutar overlay
- **13+ patches binarios** en DBZP.BIN verificados 100%:
  - 8 originales (render loops, bounds, count, validation)
  - 1 count general (`addiu $s1, $zero, 10`)
  - 12 dispatcher count instances (`addiu $a2, $zero, 10`)
  - 1 handler stub (`jr $ra` + `nop`)
  - 1 jump table slot 10 (`0x003B4EF8`)

## Por qué no funciona el approach binario

### Descubrimiento clave
El menú principal de BT3 **no define las entries como constantes en el binario**. Las entries son **datos cargados desde archivos AFS en el disco** en tiempo de ejecución.

### Flujo del menú
1. **Dispatcher** (`f_34D468`, overlay 0x18868): Orquesta el estado del menú
2. **Plate texture loader** (`f_349FE0`, overlay 0x153E0): Dispatch por tipo de entry
3. **Jump table** (overlay 0x07F690, RAM 0x3B4290): 9 handlers, uno por entry
4. **Second-level dispatch** (overlay 0x07F2F0, RAM 0x3B3EF0): 11 handlers por tipo

### Estructura del menú
- **String format**: `"mc_menu_plate_%d"` en overlay 0x07C3D0 (RAM 0x3B4FD0)
- **Type ID array**: 11 tipos en overlay 0x07C3A0: `[4,4,4,5,5,5,5,4,5,5,4]`
- **Entry count**: Hardcoded en instruction `addiu $s1, $zero, 9` (patcheado a 10)
- **Validation**: `sltiu $v0, $v1, 9` en overlay 0x190E4 (patcheado a 10)

### Datos del menú en AFS
- `PZS3US1.AFS` (1.6GB, 3400 archivos): Contiene menús, personajes, texturas
- `Main_US.pak` (entry 449): Probablemente contiene las definitions del main menu
- `TextPack_US.cpak` (entry 478): Contiene texturas de texto
- Las texturas `mc_menu_plate_%d` están embebidas en estos archivos, no como entries separadas en el AFS

### El problema fundamental
Los patches binarios cambian:
- Los límites de validación del cursor ✓
- El count para comparaciones ✓
- Los loops de render ✓

Pero NO cambian:
- Las entries reales del menú (datos en AFS)
- Las texturas de las entries (en AFS)
- Las acciones asociadas a cada entry (en AFS)

## Investigaciones adicionales

### Cómo se agregan entries para personajes (DLC mod)
Ver documento completo: `INVESTIGACION-ENTRADS-MENU.md`

**Hallazgos clave**:
- Estructura `RosterEntry`: `headCharacterId`, `transformationCount`, `transfCharacterIds[7]`
- Original: 161 personajes, cada uno con 10 entradas AFS (desde índice 1424)
- DLC mod usa modloader + MOD.BIN para hook del sistema de archivos
- Los datos del roster están en memoria, pero se cargan desde AFS

### Parches de traducción y ubicación de textos
Ver documento completo: `INVESTIGACION-ENTRADS-MENU.md`

**Hallazgos clave**:
- Textos del menú están en `PZS3US1.AFS`, no en el binario
- MaxBound Studios tradujo PT-BR modificando archivos AFS
- `bt3-file-dump-organizer` categoriza: Characters, Maps, **Menus**, Scenarios
- Nuevos AFLs de ViveTheModder tienen nombres reales (no `.unk`)
- `afl-editor` permite buscar/reemplazar strings en AFLs

### Patrón unificado
Tanto para personajes como para menús:
1. Los **datos** están en archivos AFS
2. El **código** en el overlay binary referencia esos archivos
3. Para modificar se necesita **herramientas AFS** (no solo parches binarios)

## Herramientas necesarias para continuar
1. **AFS Explorer v3.7** (Windows): Para modificar `PZS3US1.AFS`
2. **AFL files**: `PZS3US1.AFL` descargado de vivethemodder.github.io
3. **UltraISO/ImgBurn**: Para reconstruir el ISO
4. **SpikeSoft** (GitHub: HiroTex/SpikeSoft): Alternativa moderna para managing AFS files

## Pasos pendientes (Approach A: Direct PS2 binary mod)
1. Abrir ISO en AFS Explorer
2. Importar `PZS3US1.AFL` para obtener nombres reales de archivos
3. Localizar `Main_US.pak` y entender su estructura
4. Agregar entry #9 al menú (textura + handler)
5. Reconstruir ISO y probar

## Pasos pendientes (Approach B: Recomp runner)
1. Usar `bt3MenuEntryHook` en `game_overrides.cpp` (backup en /tmp)
2. Aplicar patches 9-10 en `overlay_functions.cpp`
3. Compilar y probar en recomp runner (ya llega a bt3state=0x1)
4. Dar forma visual a la entry desde el recomp

## Archivos importantes
- `/home/rexx/Escritorio/BT3_patched.iso` - ISO parcheada para PCSX2
- `/home/rexx/Escritorio/BT3-Recomp/games/bt3/work/BIN/DBZP.BIN` - Overlay parcheado
- `/home/rexx/Escritorio/BT3-Recomp/games/bt3/work/SLUS_216.78` - Boot ELF (sin cambios)
- `/home/rexx/Proyectos/analisis/patches_menu10.txt` - Documentación de patches
- `/tmp/runner_overlay_backup/` - Backup de overlay del recomp
- `/tmp/game_overrides_backup.cpp` - Backup con bt3MenuEntryHook
- `/tmp/PZS3US1.AFL` - File list para AFS Explorer
