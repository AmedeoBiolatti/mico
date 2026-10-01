#pragma once
#include <algorithm>
#include <string_view>
#include <vector>

#include "math/layout.h"

// Drawing into a coverage mask: the pieces equations and charts are made of.
namespace mico::math {

// Coverage in [0, 1] per pixel, before it is given a colour. It remembers
// which columns of each row were drawn on, so compositing and clearing touch
// the ink and not a whole megapixel chart once per layer.
struct Canvas {
  int w = 0, h = 0;
  std::vector<float> a;
  std::vector<int> lo, hi;                // per row: drawn on over [lo, hi]
  int y0 = 1 << 30, y1 = -1;              // rows drawn on
  Canvas() = default;
  Canvas(int w_, int h_)
      : w(w_), h(h_), a(size_t(w_) * size_t(h_), 0.f), lo(size_t(h_), 1 << 30), hi(size_t(h_), -1) {}
  void add(int x, int y, float v) {
    if (unsigned(x) >= unsigned(w) || unsigned(y) >= unsigned(h) || v <= 0) return;
    float& p = a[size_t(y) * size_t(w) + size_t(x)];
    p = std::min(1.f, p + v);
    mark(x, x, y);
  }
  // Marks a box as drawn on, for writers that store into `a` directly.
  void touch(int ax, int ay, int bx, int by) {
    ax = std::max(0, ax), bx = std::min(w - 1, bx);
    if (ax > bx) return;
    for (int y = std::max(0, ay); y <= std::min(h - 1, by); y++) mark(ax, bx, y);
  }
  bool empty() const { return y1 < y0; }
  void clear() {
    for (int y = std::max(0, y0); y <= std::min(h - 1, y1); y++) {
      if (hi[size_t(y)] >= lo[size_t(y)])
        std::fill(a.begin() + (size_t(y) * size_t(w) + size_t(lo[size_t(y)])),
                  a.begin() + (size_t(y) * size_t(w) + size_t(hi[size_t(y)]) + 1), 0.f);
      lo[size_t(y)] = 1 << 30;
      hi[size_t(y)] = -1;
    }
    y0 = 1 << 30;
    y1 = -1;
  }

 private:
  void mark(int ax, int bx, int y) {
    lo[size_t(y)] = std::min(lo[size_t(y)], ax);
    hi[size_t(y)] = std::max(hi[size_t(y)], bx);
    y0 = std::min(y0, y), y1 = std::max(y1, y);
  }
};

// A layout item, `em` pixels to the em, with its origin at (ox, oy): y grows
// down on the canvas, up in the item.
void draw_glyph(Canvas& c, const Item& it, float em, float ox, float oy);
void draw_rect(Canvas& c, const Item& it, float em, float ox, float oy);
void draw_line(Canvas& c, const Item& it, float em, float ox, float oy);
void draw_item(Canvas& c, const Item& it, float em, float ox, float oy);

// Upright text in the atlas's roman letters, `em` pixels to the em, starting
// at x with its baseline at y. Returns the width drawn.
float draw_text(Canvas& c, std::string_view utf8, float em, float x, float y);
float text_width(std::string_view utf8, float em);

}  // namespace mico::math
