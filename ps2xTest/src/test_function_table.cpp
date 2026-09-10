#include "ps2_runtime.h"
#include "ps2_settings_overlay.h"
#include "runtime/ps2_memory.h"

#include <algorithm>

// For Unit tests link ps2_runtime without the generated runner source.
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = PS2_RAM_SIZE;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = (g_ps2RecompiledFunctionTableEnd - g_ps2RecompiledFunctionTableBase) >> 2;

PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[g_ps2RecompiledFunctionTableSlotCount] = {};

// Game-specific symbols normally supplied by generated runner/main sources.
// Tests link ps2_runtime on its own, so provide inert tables and host state here.
extern const uint32_t g_ps2OverlayFunctionTableBase = 0x334c00u;
extern const uint32_t g_ps2OverlayFunctionTableEnd = 0x334c04u;
extern const uint32_t g_ps2OverlayFunctionTableSlotCount = 1u;
PS2Runtime::RecompiledFunction g_ps2OverlayFunctionTable[g_ps2OverlayFunctionTableSlotCount] = {};
int g_ps2ReplayVsync = 0;
bool PS2SettingsOverlay::s_widescreen = false;

extern "C" const char *ps2xExeDirC()
{
    return ".";
}

void reset_ps2_test_function_table()
{
    std::fill(g_ps2RecompiledFunctionTable,
              g_ps2RecompiledFunctionTable + g_ps2RecompiledFunctionTableSlotCount,
              nullptr);
}
