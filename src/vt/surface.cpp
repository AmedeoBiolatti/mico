#include "vt/surface.h"

#include <algorithm>

#include "base/text.h"

namespace mico {

void Surface::resize(int w, int h) {
  w = w > 0 ? w : 0;
  h = h > 0 ? h : 0;
  // Resizing to the size it already is must not destroy what is on it. A
  // caller that resizes every frame and only redraws when something changed
  // would otherwise present a blank screen on every idle tick.
  if (w == w_ && h == h_ && cells_.size() == size_t(w) * size_t(h)) return;
  w_ = w;
  h_ = h;
  cells_.assign(size_t(w_) * h_, Cell{});
}

void Surface::clear(Style st) {
  for (auto& c : cells_) c = Cell{U' ', st, 1};
}

void normalize_selection(Point a, Point b, Rect area, Point& top, Point& bot) {
  auto clamp = [&](Point p) {
    p.x = std::clamp(p.x, area.x, std::max(area.x, area.right() - 1));
    p.y = std::clamp(p.y, area.y, std::max(area.y, area.bottom() - 1));
    return p;
  };
  a = clamp(a);
  b = clamp(b);
  if (b.y < a.y || (b.y == a.y && b.x < a.x)) std::swap(a, b);
  top = a;
  bot = b;
}

namespace {
// The blank cells a continuation row starts with before its wrap mark: the
// chat's own indent, not part of the line being continued.
int continuation_indent(const Surface& s, int y, int x0, int x1) {
  int n = 0;
  for (int x = std::max(0, x0); x <= x1 && x < s.width(); x++) {
    const Cell& c = s.at(x, y);
    if (c.st.a & attr::kDecor) return n;
    if (c.cp != U' ' && c.cp != 0) return 0;
    n++;
  }
  return 0;
}
}  // namespace

std::string selection_text(const Surface& s, Point top, Point bot, Rect area,
                           std::string (*image_text)(uint32_t id)) {
  std::string out;
  std::vector<uint32_t> named;  // images already written out
  bool first = true;
  for (int y = std::max(0, top.y); y <= bot.y && y < s.height(); y++) {
    const int x0 = (y == top.y) ? top.x : area.x;
    const int x1 = (y == bot.y) ? bot.x : area.right() - 1;
    std::string line;
    bool has_image = false, text = false, join = false;
    for (int x = std::max(0, x0); x <= x1 && x < s.width(); x++) {
      const Cell& c = s.at(x, y);
      if (c.width == 0) continue;  // trailing half of a double-width glyph
      if (c.st.a & attr::kDecor) {
        join |= (c.st.a & attr::kJoin) != 0;
        continue;
      }
      if (c.st.a & attr::kImage) {
        has_image = true;
        const uint32_t id = uint32_t(c.st.fg) & 0xFFFFFF;
        if (!image_text || std::find(named.begin(), named.end(), id) != named.end()) continue;
        named.push_back(id);
        line += image_text(id);
        text = true;
        continue;
      }
      text |= c.cp != U' ' && c.cp != 0;
      text::encode(c.cp ? c.cp : U' ', line);
    }
    // An equation's other rows: nothing of their own to copy.
    if (image_text && has_image && !text) continue;
    while (!line.empty() && line.back() == ' ') line.pop_back();
    if (join && !first) {
      // A wrapped line's continuation: back onto the line it came from,
      // without the indentation it was drawn with.
      size_t lead = 0;
      while (lead < line.size() && line[lead] == ' ' && lead < size_t(continuation_indent(s, y, x0, x1))) lead++;
      out += line.substr(lead);
      continue;
    }
    if (!first) out.push_back('\n');
    first = false;
    out += line;
  }
  return out;
}

void Painter::put_abs(int ax, int ay, char32_t cp, Style st, uint8_t width, bool room) {
  // Overwriting either half of a double-width glyph must erase the other half,
  // or the terminal is left rendering a stale trailing column.
  Cell& prev = s_.at(ax, ay);
  if (prev.width == 0 && ax > 0) s_.at(ax - 1, ay) = Cell{U' ', st, 1};
  if (prev.width == 2 && s_.in_bounds(ax + 1, ay)) s_.at(ax + 1, ay) = Cell{U' ', st, 1};

  prev = Cell{cp, st, width, link_};
  if (width == 2) {
    if (!room || !s_.in_bounds(ax + 1, ay)) {
      // No room for the second half: degrade to a space rather than overflow.
      prev = Cell{U' ', st, 1};
      return;
    }
    s_.at(ax + 1, ay) = Cell{U' ', st, 0, link_};
  }
}

void Painter::fill(Rect local, Style st, char32_t cp) {
  const Rect r = local.intersect(Rect{0, 0, clip_.w, clip_.h});
  if (r.empty()) return;
  const Cell c{cp, st, 1};

  // Every filled cell is fully overwritten, so the only wide-glyph repair
  // needed is at the two edges. The interior is a straight run — this is the
  // hottest loop in the renderer, and going through put() per cell made a
  // full-pane clear cost more than everything else in a frame combined.
  for (int y = r.y; y < r.bottom(); y++) {
    const int ay = clip_.y + y;
    const int ax = clip_.x + r.x;
    if (ax > 0 && s_.in_bounds(ax - 1, ay) && s_.at(ax - 1, ay).width == 2)
      s_.at(ax - 1, ay) = Cell{U' ', st, 1};
    const int right = ax + r.w;
    if (s_.in_bounds(right, ay) && s_.at(right, ay).width == 0)
      s_.at(right, ay) = Cell{U' ', st, 1};

    Cell* row = &s_.at(ax, ay);
    std::fill(row, row + r.w, c);
  }
}

int Painter::text(int x, int y, std::string_view utf8, Style st) {
  Cell* const row = row_base(y);
  int col = x;
  for (size_t i = 0; i < utf8.size();) {
    // ASCII is almost all of it, and decoding it is a compare and an increment.
    const unsigned char b = (unsigned char)utf8[i];
    if (b < 0x80) {
      if (b == '\n' || b == '\r') break;
      i++;
      if (b < 0x20) continue;
      if (col >= clip_.w) break;
      if (row && row[col].width == 1) row[col] = Cell{char32_t(b), st, 1, link_};
      else put(col, y, char32_t(b), st, 1);
      col++;
      continue;
    }
    // An emoji with its selector, skin tone or joined partners is one cell's.
    int w;
    const char32_t cp = text::next_glyph(utf8, i, &w);
    if (w == 0) continue;  // a stray combining mark, with nothing to combine with
    if (col >= clip_.w) break;
    put(col, y, cp, st, uint8_t(w));
    col += w;
  }
  return col - x;
}

int Painter::text_clipped(int x, int y, std::string_view utf8, Style st, int max_w) {
  const int limit = std::min(max_w, clip_.w - x);
  if (limit <= 0) return 0;

  // One pass: draw until the budget runs out, and only then decide where the
  // ellipsis goes. Measuring first meant decoding every glyph twice, and this
  // runs for every visible row of every frame.
  int col = x;
  int last_start = x;  // column the most recent glyph began at
  size_t i = 0;
  bool truncated = false;

  Cell* const row = row_base(y);
  while (i < utf8.size()) {
    const size_t start = i;
    const unsigned char b = (unsigned char)utf8[i];
    if (b < 0x80) {
      if (b == '\n' || b == '\r') break;
      i++;
      if (b < 0x20) continue;
      if (col - x + 1 > limit) { i = start; truncated = true; break; }
      last_start = col;
      if (row && row[col].width == 1) row[col] = Cell{char32_t(b), st, 1, link_};
      else put(col, y, char32_t(b), st, 1);
      col++;
      continue;
    }
    int w;
    const char32_t cp = text::next_glyph(utf8, i, &w);
    if (w == 0) continue;
    if (col - x + w > limit) {
      i = start;
      truncated = true;
      break;
    }
    last_start = col;
    put(col, y, cp, st, uint8_t(w));
    col += w;
  }

  if (truncated) {
    // The ellipsis takes one column. If the budget has none left, it replaces
    // the last glyph rather than landing on top of one — writing into the
    // trailing half of a double-width glyph would erase it and overrun the
    // width that was asked for.
    int at = col;
    if (at - x + 1 > limit) at = last_start;
    if (at >= x) {
      put(at, y, U'\u2026', st, 1);
      col = at + 1;
    }
  }
  return col - x;
}

void Painter::hline(int x, int y, int len, char32_t cp, Style st) {
  for (int i = 0; i < len; i++) put(x + i, y, cp, st, 1);
}

void Painter::vline(int x, int y, int len, char32_t cp, Style st) {
  for (int i = 0; i < len; i++) put(x, y + i, cp, st, 1);
}

void Painter::box(Rect r, Style st) {
  if (r.w < 2 || r.h < 2) return;
  hline(r.x + 1, r.y, r.w - 2, U'─', st);
  hline(r.x + 1, r.bottom() - 1, r.w - 2, U'─', st);
  vline(r.x, r.y + 1, r.h - 2, U'│', st);
  vline(r.right() - 1, r.y + 1, r.h - 2, U'│', st);
  put(r.x, r.y, U'╭', st);
  put(r.right() - 1, r.y, U'╮', st);
  put(r.x, r.bottom() - 1, U'╰', st);
  put(r.right() - 1, r.bottom() - 1, U'╯', st);
}

}  // namespace mico
