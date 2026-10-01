#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Charts an agent draws in the chat, in the terminal-plot style of wandb's
// leet or ratatui: a fenced block tagged `chart` holding a small JSON spec.
//
//   ```chart
//   {"type": "line", "title": "loss", "x": [0, 1, 2],
//    "series": [{"name": "train", "y": [2.1, 1.4, 0.9]}, {"name": "val", "y": [2.3, 1.7, 1.2]}]}
//   ```
//
// or, for data already on disk, columns of a CSV (with a header) or a JSONL
// file, re-read whenever the file changes, so a training run plots live:
//
//   ```chart
//   {"type": "line", "file": "runs/metrics.csv", "x": "step", "y": ["loss", "val_loss"]}
//   ```
//
// Types: "line" (several series in colour), "scatter", "bar" (horizontal,
// labelled). Options: title, xlabel, ylabel, "log_y": true, "height": rows,
// "marker": "braille" (the default: finest), "line" (box drawing: whole in
// any font), "block" (2x2 blocks). Anything that does not parse is shown as
// the code it was.
namespace mico::chart {

struct Series {
  std::string name;
  std::vector<double> x;  // empty: 0, 1, 2, …
  std::vector<double> y;  // NaN: a gap
};

struct Spec {
  enum class Kind { Line, Scatter, Bar, Hist, Spark, Heatmap } kind = Kind::Line;
  std::string title, xlabel, ylabel;
  std::vector<std::string> labels;  // a bar chart's categories; a heatmap's columns
  std::vector<std::string> ylabels;  // a heatmap's rows
  std::vector<std::vector<double>> z;  // a heatmap's values, row by row
  int bins = 0;                        // a histogram's (0: chosen from the data)
  double bin0 = 0, binw = 1;           // a binned histogram's first edge and width
  std::vector<Series> series;
  bool log_y = false;
  int height = 12;
  // How the data is drawn. Braille (the default) has the finest detail, but
  // its look depends on the font: one without braille (Ubuntu Sans Mono)
  // borrows a proportional font's glyphs, which leave gaps. Box-drawing lines
  // and block quadrants are drawn by the terminal itself (VTE, kitty, …) and
  // come out whole in any font.
  enum class Marker { Auto, Line, Block, Braille } marker = Marker::Auto;
  // Data from a file instead of inline.
  std::string file, xcol;
  std::vector<std::string> ycols;
  // Where images can be shown: labels in the terminal's own font around a
  // picture of the plot (the default), or the whole chart one picture with
  // its labels typeset like the equations ("math").
  enum class Font { Terminal, Math } font = Font::Terminal;
  bool font_set = false;
  // A figure of several charts instead of one: side by side, `columns` to a
  // row (0: all in one row). The title, if any, goes above them all.
  std::vector<Spec> subplots;
  int columns = 0;
};

// Parses a block's JSON. False, with a reason, when it is not a chart.
bool parse(std::string_view json, Spec& out, std::string* error);

// Loads the data of `s` and of each of its subplots that reads a file,
// recording each file read in `watched` (path, mtime). False, with a reason,
// when one cannot be read.
bool load_files(Spec& s, const std::string& base_dir, std::string* error,
                std::vector<std::pair<std::string, int64_t>>* watched);

// Fills `s.series` from `s.file` (relative to `base_dir`), and reports the
// file's modification time so a caller can redraw when it changes. Parsed
// files are cached by path, size and time: a relayout does not re-read them.
bool load_file(Spec& s, const std::string& base_dir, std::string* error, int64_t* mtime_ns,
               std::string* abs_path);

// A drawn chart: rows of cells, each a glyph and an ink (0 text, 1 axis/dim,
// 2 title, 3+ series colours in order).
struct Canvas {
  int w = 0, h = 0;
  std::vector<char32_t> cp;
  std::vector<uint8_t> ink;
  char32_t& at(int x, int y) { return cp[size_t(y) * size_t(w) + size_t(x)]; }
  uint8_t& ink_at(int x, int y) { return ink[size_t(y) * size_t(w) + size_t(x)]; }
};
inline constexpr uint8_t kInkText = 0, kInkAxis = 1, kInkTitle = 2, kInkSeries = 3;
inline constexpr int kSeriesColors = 6;

// A histogram's values counted into bins: a series of counts, with bin0 and
// binw saying where the bins are. Anything else comes back unchanged.
Spec binned(const Spec& s);

// Draws `s` `cols` wide. The height follows from the spec and the data.
void draw(const Spec& s, int cols, Canvas& out);

// Compact axis labels: 0.035, 1.2k, 3.4M, 1e-05.
std::string format_number(double v);

}  // namespace mico::chart

namespace mico::math { struct Image; }

namespace mico::chart {

// A chart laid out for the chat: rows of pieces, each either text in an ink
// (drawn by the terminal, in its font) or a run of cells of a picture.
struct Piece {
  std::string text;
  uint8_t ink = kInkText;
  uint32_t image = 0;  // non-zero: cells 0..cols-1 of row `row` of this picture
  int row = 0, cols = 0;
  int width() const;
};
struct Figure {
  int width = 0;
  std::vector<std::vector<Piece>> rows;
};

// `s` laid out at most `cols` wide, the best way the terminals attached can
// show it: labels in cells around a picture of the plot, one whole picture,
// or (no images) entirely in cells. Subplots are laid side by side.
// `identity` names the chart (its block's text), so a live chart's next frame
// replaces its last picture.
void figure(const Spec& s, std::string_view identity, int cols, Figure& out);

// `s` as one picture at most `cols` cells wide, labels typeset in the atlas's
// font ("font": "math"). Null when images are off or it would be too narrow.
const math::Image* image(const Spec& s, std::string_view identity, int cols);

// The plot area of `s` as a picture and every label as text, in the
// terminal's own font: ticks sit on cell centres so their labels line up.
// False when images are off, there is no data, or it does not fit.
bool cells_figure(const Spec& s, std::string_view identity, int cols, Figure& out);

}  // namespace mico::chart
