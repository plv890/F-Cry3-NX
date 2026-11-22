// A few lines of text the deko3d presenter paints over the guest output (a game's options menu), centered in a black
// box with the FPS counter's dot font: no ImGui and no UI drawer, so presentation stays on the game's thread (a UI
// drawer moves every paint to the UI thread; Dead Space 3's screen went black that way). Upper-case letters, digits,
// space and . : / ( ) - + > ! , x. Thread-safe; an empty list takes it away.

#pragma once

#include <string>
#include <vector>

namespace rex::ui {

void FijarSuperposicion(std::vector<std::string> lineas);

}  // namespace rex::ui
