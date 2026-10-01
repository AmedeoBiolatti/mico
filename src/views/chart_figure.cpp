#include <algorithm>
#include <cmath>
#include <string>

#include "math/math.h"
#include "base/text.h"
#include "views/chart.h"

namespace mico::chart {
namespace {

// Columns between subplots side by side.
constexpr int kGap = 3;
// Narrower than this a subplot cannot hold a plot and its labels.
constexpr int kMinSubplot = 28;

// A cell chart's rows, cut into runs of one ink.
void from_canvas(const Canvas& c, Figure& f) {
  f.width = c.w;
  f.rows.clear();
  std::string cell;
  for (int y = 0; y < c.h; y++) {
    std::vector<Piece> row;
    for (int x = 0; x < c.w;) {
      const uint8_t ink = c.ink[size_t(y) * size_t(c.w) + size_t(x)];
      Piece p;
      p.ink = ink;
      while (x < c.w && c.ink[size_t(y) * size_t(c.w) + size_t(x)] == ink) {
        cell.clear();
        text::encode(c.cp[size_t(y) * size_t(c.w) + size_t(x)], cell);
        p.text += cell;
        x++;
      }
      row.push_back(std::move(p));
    }
    f.rows.push_back(std::move(row));
  }
}

void from_image(const math::Image& im, Figure& f) {
  f.width = im.cols;
  f.rows.clear();
  for (int r = 0; r < im.rows; r++) {
    Piece p;
    p.image = im.id;
    p.row = r;
    p.cols = im.cols;
    f.rows.push_back({p});
  }
}

int row_width(const std::vector<Piece>& row) {
  int w = 0;
  for (const Piece& p : row) w += p.width();
  return w;
}

void pad(std::vector<Piece>& row, int to) {
  const int w = row_width(row);
  if (w < to) {
    Piece p;
    p.text.assign(size_t(to - w), ' ');
    row.push_back(std::move(p));
  }
}

// Several charts side by side, `per_row` to a row, each a column of the
// figure. Rows of subplots are a blank line apart.
void compose(const Spec& s, std::string_view identity, int cols, Figure& out) {
  const int n = int(s.subplots.size());
  int per_row = s.columns > 0 ? std::min(s.columns, n) : n;
  // Too many for the width: fewer to a row rather than unreadably narrow.
  while (per_row > 1 && (cols - kGap * (per_row - 1)) / per_row < kMinSubplot) per_row--;
  const int each = std::max(10, (cols - kGap * (per_row - 1)) / per_row);

  out.rows.clear();
  out.width = 0;
  if (!s.title.empty()) {
    Piece t;
    t.text = s.title;
    t.ink = kInkTitle;
    out.rows.push_back({t});
    out.rows.push_back({});
  }
  for (int first = 0; first < n; first += per_row) {
    const int count = std::min(per_row, n - first);
    std::vector<Figure> parts(static_cast<size_t>(count));
    size_t tallest = 0;
    for (int k = 0; k < count; k++) {
      const std::string id = std::string(identity) + "#" + std::to_string(first + k);
      figure(s.subplots[size_t(first + k)], id, each, parts[size_t(k)]);
      tallest = std::max(tallest, parts[size_t(k)].rows.size());
    }
    if (first > 0) out.rows.push_back({});
    for (size_t r = 0; r < tallest; r++) {
      std::vector<Piece> row;
      for (int k = 0; k < count; k++) {
        const Figure& part = parts[size_t(k)];
        if (k > 0) pad(row, k * (each + kGap));
        if (r < part.rows.size())
          for (const Piece& p : part.rows[r]) row.push_back(p);
      }
      out.width = std::max(out.width, row_width(row));
      out.rows.push_back(std::move(row));
    }
  }
}

}  // namespace

int Piece::width() const { return image ? cols : text::str_width(text); }

// A sparkline: one row of eighth blocks per series, in its colour, with its
// last value and range. Text, so it reads the same in every terminal.
static void spark_figure(const Spec& s, int cols, Figure& out) {
  static constexpr std::string_view kTicks[] = {"\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84",
                                                "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88"};
  out.rows.clear();
  out.width = 0;
  if (!s.title.empty()) out.rows.push_back({Piece{s.title, kInkTitle}});
  int nw = 0;
  for (const auto& ser : s.series) nw = std::max(nw, text::str_width(ser.name));
  for (size_t k = 0; k < s.series.size(); k++) {
    const Series& ser = s.series[k];
    double lo = INFINITY, hi = -INFINITY, last = NAN;
    for (double v : ser.y)
      if (std::isfinite(v)) lo = std::min(lo, v), hi = std::max(hi, v), last = v;
    const std::string tail = "  " + format_number(last) + "  (" + format_number(lo) + "\xE2\x80\x93" + format_number(hi) + ")";
    const int room = std::max(4, cols - (nw ? nw + 1 : 0) - text::str_width(tail));
    // Too many points for the room: each tick is the mean of its share.
    const size_t n = ser.y.size(), w = std::min<size_t>(n, size_t(room));
    std::string ticks;
    for (size_t i = 0; i < w; i++) {
      double sum = 0;
      int cnt = 0;
      for (size_t j = i * n / w; j < std::max((i + 1) * n / w, i * n / w + 1) && j < n; j++)
        if (std::isfinite(ser.y[j])) sum += ser.y[j], cnt++;
      if (!cnt) { ticks += " "; continue; }
      const double v = sum / cnt;
      ticks += kTicks[hi > lo ? std::clamp(int((v - lo) / (hi - lo) * 7 + 0.5), 0, 7) : 3];
    }
    std::vector<Piece> row;
    if (nw) row.push_back(Piece{ser.name + std::string(size_t(nw + 1 - text::str_width(ser.name)), ' '), kInkText});
    row.push_back(Piece{ticks, uint8_t(kInkSeries + k % kSeriesColors)});
    row.push_back(Piece{tail, kInkAxis});
    int width = 0;
    for (const auto& p : row) width += p.width();
    out.width = std::max(out.width, width);
    out.rows.push_back(std::move(row));
  }
}

void figure(const Spec& in, std::string_view identity, int cols, Figure& out) {
  if (!in.subplots.empty()) {
    compose(in, identity, cols, out);
    return;
  }
  if (in.kind == Spec::Kind::Spark) {
    spark_figure(in, cols, out);
    return;
  }
  // A histogram is drawn from its bins.
  const Spec hist = in.kind == Spec::Kind::Hist ? binned(in) : Spec{};
  const Spec& s = in.kind == Spec::Kind::Hist ? hist : in;
  if (math::config().enabled && (s.kind == Spec::Kind::Hist || s.kind == Spec::Kind::Heatmap)) {
    if (cells_figure(s, identity, cols, out)) return;
    Canvas c;
    draw(in, cols, c);
    from_canvas(c, out);
    return;
  }
  if (math::config().enabled) {
    if (s.font == Spec::Font::Math) {
      if (const math::Image* im = image(s, identity, cols)) {
        from_image(*im, out);
        return;
      }
    } else if (cells_figure(s, identity, cols, out)) {
      return;
    }
  }
  Canvas c;
  draw(in, cols, c);
  from_canvas(c, out);
}

}  // namespace mico::chart
