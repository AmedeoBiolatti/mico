#pragma once
#include <string>

#include "term/surface.h"

// Sixel, for terminals without kitty's placeholders (foot, WezTerm, Windows
// Terminal, xterm -ti vt340…). A sixel picture is painted at the cursor and
// forgotten: the terminal keeps pixels, not an image it could move. So the
// picture is redrawn, cropped to what is visible, whenever an image's cells
// change — it scrolled, it was uncovered — and text written over it later
// replaces it, the way text replaces text.
namespace mico::math {

// Draws every image on `back` whose cells differ from `front`, and brings
// `front` up to date for them. Run after encode_frame(…, ImageMode::Sixel).
void sixel_pass(const Surface& back, Surface& front, std::string& out);

// One picture: `alpha` (w x h, row stride `stride`) in `fg` over `bg`.
std::string sixel(const uint8_t* alpha, int stride, int w, int h, Color fg, Color bg);
// A full-colour picture (straight-alpha RGBA, row stride in pixels) over `bg`,
// quantized to at most 255 colours.
std::string sixel_rgba(const uint8_t* rgba, int stride, int w, int h, Color bg);

}  // namespace mico::math
