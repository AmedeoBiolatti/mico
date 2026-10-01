#pragma once
#include <vector>

#include "math/tex.h"

// TeX's layout rules, simplified: inter-atom spacing by class, fractions on
// the math axis, radicals, scripts and limits, delimiters that grow with what
// they enclose, arrays and accents. Units are em, y up from the baseline.
namespace mico::math {

struct Item {
  enum Kind : uint8_t { Glyph, Rect, Line } kind;
  char32_t cp = 0;
  // Glyph: pen at (x, y), scaled by (a, b).
  // Rect:  lower-left corner at (x, y), a wide, b high.
  // Line:  from (x, y) to (a, b), t thick, round ends.
  float x = 0, y = 0, a = 0, b = 0, t = 0;
};

struct Box {
  float w = 0, h = 0, d = 0;  // width, height above and depth below the baseline
  float ic = 0;               // italic correction of a lone glyph: where a superscript goes
  std::vector<Item> items;
};

struct LayoutOptions {
  bool display = true;
  // Script and scriptscript scales never go below this. The caller sets it
  // from the pixel size: a legible subscript matters more than TeX's ratios.
  float min_scale = 0.5f;
  // Fractions in running text set their parts at text size, as \dfrac does,
  // rather than TeX's script size: the chat gives them the rows they need.
  bool roomy_fractions = false;
};

Box layout(const List& l, const LayoutOptions& opt);

}  // namespace mico::math
