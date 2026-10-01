#include "math/raster.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "math/atlas.h"
#include "term/text.h"

namespace mico::math {
namespace {

// Coverage of [a, b) within pixel [p, p+1).
float overlap(float a, float b, int p) {
  return std::max(0.f, std::min(b, float(p + 1)) - std::max(a, float(p)));
}

}  // namespace

// Resamples a glyph's master bitmap onto the canvas. Each target pixel takes
// the mean coverage of the source area it covers, read off the summed-area
// table in four lookups; bilinear interpolation of that table is exact for a
// bitmap that is constant across each pixel, so this is a true box filter at
// any scale and subpixel position.
void draw_glyph(Canvas& c, const Item& it, float em, float ox, float oy) {
  const Glyph* g = glyph(it.cp);
  if (!g || g->bw == 0 || g->bh == 0) return;
  const uint32_t* t = sat(*g);
  const float m = float(master_px());
  const float sx = it.a * em / m, sy = it.b * em / m;
  if (sx <= 0 || sy <= 0) return;
  const float X = ox + it.x * em, Y = oy - it.y * em;
  const int bw = g->bw, bh = g->bh, stride = bw + 1;
  const int x0 = std::max(0, int(std::floor(X + float(g->left) * sx)));
  const int x1 = std::min(c.w, int(std::ceil(X + float(g->left + bw) * sx)));
  const int y0 = std::max(0, int(std::floor(Y + float(g->top) * sy)));
  const int y1 = std::min(c.h, int(std::ceil(Y + float(g->top + bh) * sy)));
  if (x0 >= x1 || y0 >= y1) return;
  const float norm = sx * sy / 127.f;

  // Where each target column's edges fall in the table, worked out once per
  // glyph rather than once per pixel.
  struct Col {
    int i;
    float f;
  };
  static thread_local std::vector<Col> cols;
  static thread_local std::vector<float> r0, r1;
  const auto at = [](float u, int n) {
    u = std::clamp(u, 0.f, float(n));
    const int i = std::min(int(u), n - 1);
    return Col{i, u - float(i)};
  };
  cols.resize(size_t(x1 - x0 + 1));
  for (int px = x0; px <= x1; px++) cols[size_t(px - x0)] = at((float(px) - X) / sx - float(g->left), bw);
  r0.resize(size_t(stride));
  r1.resize(size_t(stride));
  // A table row interpolated at v: the table is then only read along u.
  const auto row = [&](float v, std::vector<float>& out) {
    const Col cv = at(v, bh);
    const uint32_t* a = t + size_t(cv.i) * size_t(stride);
    const uint32_t* b = a + stride;
    for (int u = 0; u < stride; u++) out[size_t(u)] = float(a[u]) + (float(b[u]) - float(a[u])) * cv.f;
  };
  const auto lerp = [](const std::vector<float>& r, Col k) {
    return r[size_t(k.i)] + (r[size_t(k.i) + 1] - r[size_t(k.i)]) * k.f;
  };
  for (int py = y0; py < y1; py++) {
    row((float(py) - Y) / sy - float(g->top), r0);
    row((float(py + 1) - Y) / sy - float(g->top), r1);
    for (int px = x0; px < x1; px++) {
      const Col k0 = cols[size_t(px - x0)], k1 = cols[size_t(px - x0 + 1)];
      const float sum = lerp(r1, k1) - lerp(r1, k0) - lerp(r0, k1) + lerp(r0, k0);
      c.add(px, py, sum * norm);
    }
  }
}

void draw_rect(Canvas& c, const Item& it, float em, float ox, float oy) {
  float x0 = ox + it.x * em, x1 = x0 + it.a * em;
  float yt = oy - (it.y + it.b) * em, yb = oy - it.y * em;
  // Thin rules snap to whole pixels: a bar straddling two rows at half
  // strength reads as a smudge, one crisp row reads as a line.
  if (yb - yt < 2.5f) {
    const float hs = std::max(1.f, std::round(yb - yt));
    const float top = std::round((yt + yb) / 2 - hs / 2);
    yt = top;
    yb = top + hs;
  }
  if (x1 - x0 < 2.5f) {
    const float ws = std::max(1.f, std::round(x1 - x0));
    const float left = std::round((x0 + x1) / 2 - ws / 2);
    x0 = left;
    x1 = left + ws;
  }
  for (int py = std::max(0, int(std::floor(yt))); py < std::min(c.h, int(std::ceil(yb))); py++) {
    const float fy = overlap(yt, yb, py);
    for (int px = std::max(0, int(std::floor(x0))); px < std::min(c.w, int(std::ceil(x1))); px++)
      c.add(px, py, fy * overlap(x0, x1, px));
  }
}

void draw_line(Canvas& c, const Item& it, float em, float ox, float oy) {
  const float ax = ox + it.x * em, ay = oy - it.y * em;
  const float bx = ox + it.a * em, by = oy - it.b * em;
  const float tw = it.t * em;
  const float t = std::max(1.f, tw), k = std::min(1.f, tw);  // too thin: 1px, fainter
  const float r = t / 2 + 1;
  const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
  const float reach2 = (t / 2 + 0.5f) * (t / 2 + 0.5f);
  c.touch(int(std::floor(std::min(ax, bx) - r)), int(std::floor(std::min(ay, by) - r)),
          int(std::ceil(std::max(ax, bx) + r)), int(std::ceil(std::max(ay, by) + r)));
  for (int py = std::max(0, int(std::floor(std::min(ay, by) - r))); py <= std::min(c.h - 1, int(std::ceil(std::max(ay, by) + r))); py++) {
    for (int px = std::max(0, int(std::floor(std::min(ax, bx) - r))); px <= std::min(c.w - 1, int(std::ceil(std::max(ax, bx) + r))); px++) {
      const float cx = float(px) + 0.5f, cy = float(py) + 0.5f;
      float u = len2 > 0 ? ((cx - ax) * dx + (cy - ay) * dy) / len2 : 0;
      u = std::clamp(u, 0.f, 1.f);
      const float ex = ax + u * dx - cx, ey = ay + u * dy - cy;
      const float d2 = ex * ex + ey * ey;
      if (d2 >= reach2) continue;  // most of the box: nowhere near the stroke
      const float v = std::clamp(t / 2 + 0.5f - std::sqrt(d2), 0.f, 1.f);
      if (v > 0) {
        float& p = c.a[size_t(py) * size_t(c.w) + size_t(px)];
        p = std::max(p, v * k);  // strokes meet at joints: no double coverage
      }
    }
  }
}

void draw_item(Canvas& c, const Item& it, float em, float ox, float oy) {
  switch (it.kind) {
    case Item::Glyph: draw_glyph(c, it, em, ox, oy); break;
    case Item::Rect: draw_rect(c, it, em, ox, oy); break;
    case Item::Line: draw_line(c, it, em, ox, oy); break;
  }
}

float text_width(std::string_view s, float em) {
  float w = 0;
  for (size_t i = 0; i < s.size();) {
    const char32_t cp = text::decode(s, i);
    if (cp == ' ') { w += 0.333f * em; continue; }
    const Glyph* g = glyph(cp);
    if (!g) g = glyph('?');
    if (g) w += g->adv * em;
  }
  return w;
}

float draw_text(Canvas& c, std::string_view s, float em, float x, float y) {
  const float x0 = x;
  for (size_t i = 0; i < s.size();) {
    const char32_t cp = text::decode(s, i);
    if (cp == ' ') { x += 0.333f * em; continue; }
    const Glyph* g = glyph(cp);
    if (!g) g = glyph('?');
    if (!g) continue;
    Item it{Item::Glyph};
    it.cp = g->cp;
    it.a = it.b = 1.f;
    draw_glyph(c, it, em, x, y);
    x += g->adv * em;
  }
  return x - x0;
}

}  // namespace mico::math
