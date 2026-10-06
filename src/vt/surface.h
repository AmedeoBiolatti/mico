#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/color.h"
#include "vt/geom.h"

namespace mico {

namespace attr {
inline constexpr uint16_t kNone = 0;
inline constexpr uint16_t kBold = 1 << 0;
inline constexpr uint16_t kDim = 1 << 1;
inline constexpr uint16_t kItalic = 1 << 2;
inline constexpr uint16_t kUnderline = 1 << 3;
inline constexpr uint16_t kReverse = 1 << 4;
// Not a text attribute: the cell is one cell of an image. Its fg holds the
// image id and its cp packs (row << 16) | column. The encoder writes it as a
// kitty Unicode placeholder; nothing else ever sees it as a character.
inline constexpr uint16_t kImage = 1 << 5;
// Not text attributes either: how a selection copies the cell. Decoration (a
// code block's padding, its wrap marks, its language label) copies as
// nothing; a row holding a kJoin cell continues the row above, so copying a
// wrapped line gives it back whole.
inline constexpr uint16_t kDecor = 1 << 6;
inline constexpr uint16_t kJoin = 1 << 7;
inline constexpr uint16_t kStrike = 1 << 8;  // SGR 9: struck through
}  // namespace attr

struct Style {
  Color fg = kDefaultColor;
  Color bg = kDefaultColor;
  uint16_t a = attr::kNone;

  Style with_fg(Color c) const { return Style{c, bg, a}; }
  Style with_bg(Color c) const { return Style{fg, c, a}; }
  Style plus(uint16_t extra) const { return Style{fg, bg, uint16_t(a | extra)}; }
  bool operator==(const Style&) const = default;
};

struct Cell {
  char32_t cp = U' ';
  Style st{};
  // 0 marks the trailing half of a double-width glyph; it is never emitted.
  uint8_t width = 1;
  uint16_t link = 0;  // a links:: id: the cell is part of a hyperlink
  bool operator==(const Cell&) const = default;
};
static_assert(sizeof(Cell) == 20, "a link id rides in padding, not a bigger cell");

// A full-screen grid of cells. Panes never touch this directly; they draw
// through a Painter, which clips and translates into pane-local coordinates.
class Surface {
 public:
  void resize(int w, int h);
  int width() const { return w_; }
  int height() const { return h_; }

  Cell& at(int x, int y) { return cells_[size_t(y) * w_ + x]; }
  const Cell& at(int x, int y) const { return cells_[size_t(y) * w_ + x]; }
  bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }

  void clear(Style st = {});

 private:
  int w_ = 0, h_ = 0;
  std::vector<Cell> cells_;
};

// Orders a selection's two ends into reading order and clamps both into
// `area` — the region the drag began in. The result is inclusive of both ends.
void normalize_selection(Point a, Point b, Rect area, Point& top, Point& bot);

// The text a selection covers, read off a composed Surface: one line per screen
// row, trailing blanks trimmed, a double-width glyph counted once. Intermediate
// rows are read full width, the way a linear text selection works. Decoration
// (attr::kDecor: bars, frames, the scrollbar, a cursor) is never copied: before
// a row's text it reads as blank, keeping the indent as drawn, and after it as
// nothing. The indent all the lines share is dropped, keeping any deeper. An image
// cell reads as nothing; with `image_text`, the first cell of each image
// selected reads as what it returns for the image's id (an equation's LaTeX),
// and rows holding nothing but image cells are dropped.
std::string selection_text(const Surface& s, Point top, Point bot, Rect area,
                           std::string (*image_text)(uint32_t id) = nullptr);

// Clipped, translated drawing handle handed to a pane. Coordinates passed to a
// Painter are local to its rect; anything outside is silently dropped.
class Painter {
 public:
  Painter(Surface& s, Rect clip) : s_(s), clip_(clip) {}

  int width() const { return clip_.w; }
  int height() const { return clip_.h; }
  Rect rect() const { return clip_; }

  // Sub-region of this painter, in local coordinates.
  Painter sub(Rect local) const {
    Rect abs{clip_.x + local.x, clip_.y + local.y, local.w, local.h};
    return Painter(s_, abs.intersect(clip_));
  }

  // Inline: this is called once per glyph and once per cleared cell, which is
  // tens of thousands of times a frame.
  void put(int x, int y, char32_t cp, Style st, uint8_t width = 1) {
    if (unsigned(x) >= unsigned(clip_.w) || unsigned(y) >= unsigned(clip_.h)) return;
    const int ax = clip_.x + x, ay = clip_.y + y;
    if (!s_.in_bounds(ax, ay)) return;
    // Overwhelmingly the common case: a single-width glyph replacing another.
    // Neither half of a double-width pair is involved, so none of the repair
    // work applies and the cell is a straight store.
    Cell& dst = s_.at(ax, ay);
    if (width == 1 && dst.width == 1) {
      dst = Cell{cp, st, 1, link_};
      return;
    }
    put_abs(ax, ay, cp, st, width, x + 1 < clip_.w);
  }
  void fill(Rect local, Style st, char32_t cp = U' ');
  void clear(Style st) { fill(Rect{0, 0, clip_.w, clip_.h}, st); }

  // Draws UTF-8 text; returns the number of columns advanced. Stops at the
  // right edge of the clip rect.
  int text(int x, int y, std::string_view utf8, Style st);
  // Like text(), but truncates with an ellipsis if it would exceed max_w.
  int text_clipped(int x, int y, std::string_view utf8, Style st, int max_w);

  // Cells drawn from now on belong to this hyperlink (0: none).
  void set_link(uint16_t id) { link_ = id; }

  void hline(int x, int y, int len, char32_t cp, Style st);
  void vline(int x, int y, int len, char32_t cp, Style st);
  void box(Rect local, Style st);

 private:
  // Shared by put() and the fill fast path; `room` says whether a double-width
  // glyph has a second column available inside the clip.
  void put_abs(int ax, int ay, char32_t cp, Style st, uint8_t width, bool room);

  // First cell of a clip-local row, or null if the row is outside the surface.
  // Lets the text loops store straight into the grid: a line of ASCII is the
  // overwhelming case and does not need a call and a bounds check per glyph.
  Cell* row_base(int y) {
    const int ay = clip_.y + y;
    if (unsigned(y) >= unsigned(clip_.h) || !s_.in_bounds(clip_.x, ay)) return nullptr;
    return &s_.at(clip_.x, ay);
  }

  Surface& s_;
  Rect clip_;
  uint16_t link_ = 0;
};

}  // namespace mico
