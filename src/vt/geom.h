#pragma once
#include <algorithm>

namespace mico {

struct Point { int x = 0, y = 0; };

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;

  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(Point p) const {
    return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
  }
  Rect intersect(const Rect& o) const {
    int nx = std::max(x, o.x), ny = std::max(y, o.y);
    int nr = std::min(right(), o.right()), nb = std::min(bottom(), o.bottom());
    return Rect{nx, ny, std::max(0, nr - nx), std::max(0, nb - ny)};
  }
  Rect inset(int dx, int dy) const { return Rect{x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
};

}  // namespace mico
