// fc3-switch (2026-11-01): symbols the vendored deko3d renderer and ds1-nx game layer expect from rexglue-sdk-nx's
// Switch input driver and profiler (switch_input_driver.cpp, switch_perf.cpp there), which our SDK's versions do not
// have. Shortcut requests stay off; the long-frame hook and the stack-sampling query are no-ops. Debug-only features.

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(switch_contador_fps, false, "UI/Switch",
                    "FPS counter at the top left (deko3d presenter)");
REXCVAR_DEFINE_BOOL(switch_volcado_pedido, false, "UI/Switch", "Frame dump requested (cleared by whoever does it)");
REXCVAR_DEFINE_BOOL(switch_omitir_pedido, false, "UI/Switch", "Next draw-skip mode requested (cleared when done)");
REXCVAR_DEFINE_BOOL(switch_captura_pedida, false, "UI/Switch", "Capture requested (cleared when done)");

extern "C" void RexSwitchPerfTiron(uint64_t, uint64_t) {}
extern "C" bool RexSwitchPerfStacksActive(void) { return false; }
