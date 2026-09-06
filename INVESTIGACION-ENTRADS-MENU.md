# Investigación: Entradas de Menú y Textos en BT3

## Fecha: 2026-09-04

---

## Parte 1: Cómo se agregan entries para personajes

### Fuentes principales
- **KkCap/BT3-DLC-Mod** (GitHub): https://github.com/KkCap/BT3-DLC-Mod
- **KkTeamTenkaichi**: https://kkteamtenkaichi.blogspot.com/p/tutorials.html
- **ViveTheModder/dbzbt3-research**: https://github.com/ViveTheModder/dbzbt3-research

### Estructura del Roster (common.h del DLC mod)

```c
typedef struct {
    uint32_t headCharacterId;      // ID del personaje
    uint32_t transformationCount;  // Número de transformaciones
    uint32_t transfCharacterIds[7]; // IDs de transformaciones (máx 7)
} RosterEntry;  // 36 bytes por entry

typedef struct {
    uint32_t zero;
    uint32_t unk0;
    uint32_t rosterSize0;          // Tamaño del roster
    uint32_t rosterSize1;
    RosterEntry* rosterP0;         // Puntero al array de entries
    RosterEntry* rosterP1;
    uint32_t rosterSize2;
    uint32_t rosterSize3;
} RosterHeader;  // 32 bytes
```

### Constantes importantes del DLC mod

| Constante | Valor | Significado |
|-----------|-------|-------------|
| `ORIGINAL_CHARACTER_COUNT` | 161 | Personajes originales del juego |
| `AFS_INDEX_TO_CHARACTER_ID(I)` | `((I) - 1424)/10` | Índice 1424 = primer personaje |
| `CHARACTER_TO_MODEL_AFS_INDEX(ID,COL)` | `1424 + (ID) * 10 + (COL)` | Cada personaje = 10 entradas AFS |
| `MOD_AFS_FILES_PER_CHARACTER` | 12 | Archivos por personaje en MOD.AFS |
| `DLC_AFS_FILES_PER_CHARACTER` | 26 | Archivos por personaje en DLC.AFS |

### Direcciones de memoria (RAM) del DLC mod

```
INFO_CHUNK_PTR_ADDR:    0x003b38d4  // Puntero a info del roster
RES_CHUNK_PTR_ADDR:     0x0031e794  // Puntero a recursos de personajes
MODEL_P1_PTR_ADDR:      0x0031e9bc  // Puntero a modelo jugador 1
FA_P1_PTR_ADDR:         0x0031e9d4  // Puntero a FA jugador 1
KZE_P1_PTR_ADDR:        0x0031e9ec  // Puntero a KZE jugador 1
```

### Cómo funciona el DLC mod (2 etapas)

1. **Modloader** (inyectado en ISO en offset `0x350000`):
   - Parchea instrucción en `0x100630` para saltar al modloader
   - Abre `cdrom0:\BIN\MOD.BIN;1`
   - Reserva 16KB de memoria
   - Lee MOD.BIN y ejecuta su entry point
   - Salta al entry point original del juego

2. **MOD.BIN** (código del mod):
   - Hook del sistema de archivos del juego
   - Redirige a AFS custom (MOD.AFS, DLC.AFS)
   - Modifica el roster en memoria
   - Maneja renderizado de nuevos personajes

### Patrón para agregar una entry a un menú

1. **Encontrar dónde se almacenan los datos**:
   - Para personajes: Array `RosterEntry` en memoria
   - Para menú principal: Array en `Main_US.pak` (dentro de AFS)

2. **Agregar nueva entry al array**

3. **Aumentar el contador** (rosterSize)

4. **Agregar assets necesarios**:
   - Personajes: Modelos, texturas, audio en AFS
   - Menú principal: Texturas de menú en AFS

### Conclusión Parte 1

**Los datos de menú/personajes están en archivos AFS, no en el binario.** El DLC mod confirma que para agregar entries reales se necesita:
- Modificar archivos AFS (usando AFS Explorer)
- O usar un approach de modloader como el DLC mod

---

## Parte 2: Parches de traducción y ubicación de textos

### Fuentes principales
- **MaxBound Studios** (traducción PT-BR): https://maxboundstudiosbr.com/
- **ViveTheModder/bt3-file-dump-organizer**: https://github.com/ViveTheModder/bt3-file-dump-organizer
- **ViveTheModder/afl-editor**: https://github.com/ViveTheModder/afl-editor
- **ViveTheModder/dbzbt3-research**: https://github.com/ViveTheModder/dbzbt3-research

### Estructura de archivos AFS en BT3

| Archivo | Contenido |
|---------|-----------|
| `PZS3US0.AFS` | Solo para PAL (selección de idioma) |
| `PZS3US1.AFS` | **Esenciales**: resident, menús, personajes, etc. |
| `PZS3US2.AFS` | Solo ADX: voces, música, efectos de sonido |

**Nota**: `PZS3US1.AFS` es el archivo principal donde están los textos y gráficos de menú.

### Herramienta bt3-file-dump-organizer

Organiza contenido extraído de AFS en 4 categorías:
1. **Characters** → Modelos, texturas, datos de personajes
2. **Maps** → Escenarios
3. **Menus** → **Textos y gráficos de menús**
4. **Scenarios** → Archivos de historia

### Traducción MaxBound Studios (PT-BR)

**Estado de la traducción**:
- Textos: ~90%
- Acentos: 100%
- Gráficos: 100%
- Dublagem: 100%
- Revisión: 100%

**Modificaciones incluidas**:
- Nuevo modo historia
- Todas las falas foram substituídas pela dublagem do próprio anime
- Novo visual adicionado
- Diversos novos personagens e variações adicionadas
- **Menu Principal 100% traduzido**
- **Nova opção de idioma PT-BR incluído na tela de seleção**

### Nuevos AFLs de ViveTheModder

Los AFLs (AFS File Lists) mapean los nombres reales de los archivos dentro de AFS:

| AFL | Región |
|-----|--------|
| `PZS3US1.AFL` | NTSC-U (Americano) |
| `PZS3US2.AFL` | NTSC-U (Americano) |
| `PZS3JP1.AFL` | NTSC-J (Japonés) |
| `PZS3JP2.AFL` | NTSC-J (Japonés) |
| `PZS3EU1.AFL` | PAL (Europeo) |
| `PZS3EU2.AFL` | PAL (Europeo) |

**Nota**: Los AFLs antiguos tenían nombres obfuscados (`.unk`). Los nuevos AFLs tienen nombres reales.

### Herramienta afl-editor

Java tool (CLI + GUI) que:
- Busca y reemplaza strings en archivos AFL
- Permite editar nombres de archivo en lote
- Previene nombres duplicados
- Soporta múltiples AFLs simultáneamente

### Cómo funciona un parche de traducción

1. **Extraer archivos de AFS** usando AFS Explorer
2. **Identificar archivos de menú** (categoría "Menus")
3. **Modificar textos** en los archivos `.unk` o `.pak`
4. **Reinsertar archivos** en AFS
5. **Rebuild ISO** usando UltraISO o ImgBurn

### Puntos de llamado para textos del menú

Los textos del menú principal están en archivos dentro de `PZS3US1.AFS`. El juego los carga usando el sistema de archivos AFS. Para encontrar los puntos de llamado exactos:

1. Buscar en la categoría "Menus" del dump
2. Identificar archivos como `Main_US.pak` o similares
3. Rastrear desde esos archivos hacia el código que los carga
4. Las direcciones en el overlay (como `0x07C3D0` para `mc_menu_plate_%d`) son referencias a estos archivos

### Conclusión Parte 2

**Los textos del menú están en archivos dentro de `PZS3US1.AFS`, no en el binario.** Los traductores modifican estos archivos usando herramientas como AFS Explorer. El archivo `mc_menu_plate_%d` (overlay `0x07C3D0`) es una referencia a texturas de menú que están en AFS.

---

## Síntesis: Relación entre ambas investigaciones

| Aspecto | Personajes (DLC mod) | Menú principal | Traducciones |
|---------|----------------------|----------------|--------------|
| **Ubicación** | Array `RosterEntry` en memoria | Array en `Main_US.pak` (AFS) | Archivos en `PZS3US1.AFS` |
| **Contador** | `rosterSize0/1/2/3` | 9 entries hardcodeadas | N/A |
| **Assets** | Modelos, texturas, audio en AFS | Texturas de menú en AFS | Textos en archivos .unk/.pak |
| **Modificación** | Hook de archivo + memoria | Solo binario (insuficiente) | AFS Explorer |
| **Herramienta** | KkTeamPEditor | Parches binarios | AFS Explorer, afl-editor |

### Patrón unificado

Tanto para personajes como para menús:
1. Los **datos** están en archivos AFS
2. El **código** en el overlay binary referencia esos archivos
3. Para modificar se necesita **herramientas AFS** (no solo parches binarios)

---

## Próximos pasos sugeridos

1. **Usar bt3-file-dump-organizer** para extraer y categorizar archivos de `PZS3US1.AFS`
2. **Identificar archivos de menú** específicos
3. **Rastrear puntos de llamado** desde esos archivos hacia el overlay
4. **Decidir approach**: AFS Explorer (Windows/Wine) vs modloader personalizado
