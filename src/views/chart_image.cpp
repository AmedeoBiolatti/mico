#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "math/math.h"
#include "math/raster.h"
#include "base/text.h"
#include "ui/theme.h"
#include "views/chart.h"

namespace mico::chart {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Straight-alpha RGBA, built up layer by layer: each layer is a coverage
// mask and one colour, laid over what is already there. Kept in the bytes it
// is sent as, so there is nothing to convert at the end.
struct Picture {
  int w, h;
  std::vector<uint8_t> px;
  Picture(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * size_t(h_) * 4, 0) {}
  // A solid box straight onto the picture: grid lines and spines, which as a
  // layer would make every row of the plot a span to composite.
  void fill(int x0, int y0, int x1, int y1, Color col, float v) {
    const float cr = float((col >> 16) & 0xFF), cg = float((col >> 8) & 0xFF), cb = float(col & 0xFF);
    for (int y = std::max(0, y0); y < std::min(h, y1); y++)
      for (int x = std::max(0, x0); x < std::min(w, x1); x++) {
        uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
        const float da = float(p[3]) / 255.f * (1 - v), oa = v + da;
        p[0] = uint8_t((cr * v + float(p[0]) * da) / oa + 0.5f);
        p[1] = uint8_t((cg * v + float(p[1]) * da) / oa + 0.5f);
        p[2] = uint8_t((cb * v + float(p[2]) * da) / oa + 0.5f);
        p[3] = uint8_t(oa * 255.f + 0.5f);
      }
  }
  // Lays `c` over the picture in `col`, and clears it for the next layer.
  void paint(math::Canvas& c, Color col, float opacity = 1.f) {
    const float cr = float((col >> 16) & 0xFF), cg = float((col >> 8) & 0xFF), cb = float(col & 0xFF);
    for (int y = std::max(0, c.y0); y <= std::min(h - 1, c.y1); y++)
      for (int x = c.lo[size_t(y)]; x <= c.hi[size_t(y)]; x++) {
        const size_t i = size_t(y) * size_t(w) + size_t(x);
        const float v = c.a[i] * opacity;
        if (v <= 0) continue;
        uint8_t* p = &px[i * 4];
        const float da = float(p[3]) / 255.f * (1 - v), oa = v + da;
        p[0] = uint8_t((cr * v + float(p[0]) * da) / oa + 0.5f);
        p[1] = uint8_t((cg * v + float(p[1]) * da) / oa + 0.5f);
        p[2] = uint8_t((cb * v + float(p[2]) * da) / oa + 0.5f);
        p[3] = uint8_t(oa * 255.f + 0.5f);
      }
    c.clear();
  }
};

// The coverage canvas, kept from one chart to the next: every layer is
// cleared as it is painted, so it is handed over clean, and a megapixel of
// floats is not allocated and zeroed per chart.
math::Canvas& scratch(int w, int h) {
  static math::Canvas c;
  if (c.w != w || c.h != h) c = math::Canvas(w, h);
  return c;
}

// A stroke in pixel coordinates, y down.
void seg(math::Canvas& c, float x0, float y0, float x1, float y1, float t) {
  math::Item it{math::Item::Line};
  it.x = x0;
  it.y = -y0;
  it.a = x1;
  it.b = -y1;
  it.t = t;
  math::draw_line(c, it, 1.f, 0, 0);
}

// Round-numbered ticks across [lo, hi]: steps of 1, 2 or 5 times a power of ten.
std::vector<double> nice_ticks(double lo, double hi, int most) {
  std::vector<double> t;
  if (!(hi > lo) || most < 2) return t;
  const double raw = (hi - lo) / double(most - 1);
  const double mag = std::pow(10.0, std::floor(std::log10(raw)));
  const double n = raw / mag;
  const double step = (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
  for (double v = std::ceil(lo / step - 1e-9) * step; v <= hi + step * 1e-9; v += step)
    t.push_back(std::fabs(v) < step * 1e-9 ? 0.0 : v);
  return t;
}

// An axis's labels share one precision: 0.5, 1.0, 1.5 rather than 0.5, 1, 1.5.
std::string tick_label(double v, double step) {
  if (std::fabs(v) >= 1e4 || (step > 0 && step < 1e-3)) return format_number(v);
  const int decimals = step > 0 ? std::clamp(int(std::ceil(-std::log10(step) - 1e-9)), 0, 6) : 0;
  char b[32];
  snprintf(b, sizeof b, "%.*f", decimals, v);
  return b;
}

double step_of(const std::vector<double>& t) { return t.size() > 1 ? t[1] - t[0] : 0; }

std::string clip_text(const std::string& s, float em, float max_w) {
  if (math::text_width(s, em) <= max_w) return s;
  std::string cut;
  for (size_t i = 0; i < s.size();) {
    text::decode(s, i);
    if (math::text_width(s.substr(0, i) + "\xE2\x80\xA6", em) > max_w) break;
    cut.assign(s, 0, i);
  }
  return cut + "\xE2\x80\xA6";
}

uint64_t fnv(uint64_t h, const void* p, size_t n) {
  const auto* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
uint64_t fnv(uint64_t h, std::string_view s) { return fnv(fnv(h, s.data(), s.size()), "\x1f", 1); }

struct Style {
  Color text, dim, grid, title;
  Color series[kSeriesColors];
};

Style theme_style() {
  const Theme& th = active_theme();
  // The same inks the cell chart uses, so a chart keeps its colours when it
  // falls back to text.
  return Style{th.text, th.dim, th.border, th.heading,
               {th.accent, th.link, th.code, th.removed, th.thinking, th.added}};
}

std::string series_name(const Spec& s, size_t i) {
  return s.series[i].name.empty() ? "series " + std::to_string(i + 1) : s.series[i].name;
}

// Title on the left, legend on the right; the legend drops to its own line
// when both do not fit. Returns the y below the header.
float header(const Spec& s, const Style& st, Picture& pic, math::Canvas& c, float pad, float ch,
             bool legend, bool dots) {
  const float em_title = ch * 0.82f, em = ch * 0.72f, sample = em * 1.4f, t = std::max(1.5f, ch * 0.09f);
  float legend_w = 0;
  if (legend)
    for (size_t i = 0; i < s.series.size(); i++)
      legend_w += sample + em * 0.4f + math::text_width(series_name(s, i), em) + em * 1.2f;
  const float title_w = s.title.empty() ? 0 : math::text_width(s.title, em_title);
  float y = pad;
  const bool one_line = title_w + legend_w + em * 2 < float(pic.w) - 2 * pad;
  if (!s.title.empty()) {
    math::draw_text(c, s.title, em_title, pad, y + ch * 0.75f);
    pic.paint(c, st.title);
  }
  if (legend) {
    if (!s.title.empty() && !one_line) y += ch;
    float x = std::max(pad, float(pic.w) - pad - legend_w + em * 1.2f);
    if (!one_line) x = pad;
    for (size_t i = 0; i < s.series.size(); i++) {
      const float base = y + ch * 0.75f;
      if (dots) seg(c, x + sample / 2, base - em * 0.3f, x + sample / 2, base - em * 0.3f, t * 2.4f);
      else seg(c, x, base - em * 0.3f, x + sample, base - em * 0.3f, t);
      pic.paint(c, st.series[i % kSeriesColors]);
      x += sample + em * 0.4f;
      x += math::draw_text(c, series_name(s, i), em, x, base);
      pic.paint(c, st.text);
      x += em * 1.2f;
    }
  }
  if (!s.title.empty() || legend) y += ch;
  return y;
}

math::Image finish(Picture& pic, int cols, int rows, const Spec& s) {
  math::Image im;
  im.cols = cols;
  im.rows = rows;
  im.w = pic.w;
  im.h = pic.h;
  im.rgba = std::move(pic.px);
  im.display = true;
  im.copy = s.title.empty() ? "[chart]" : "[chart: " + s.title + "]";
  return im;
}

math::Image draw_bars(const Spec& s, int cols, int cw, int ch, const Style& st) {
  size_t n = s.labels.size();
  for (const auto& ser : s.series) n = std::max(n, ser.y.size());
  n = std::min<size_t>(n, 200);
  const size_t k = std::max<size_t>(1, s.series.size());
  const bool multi = k > 1;
  const float fch = float(ch), pad = fch * 0.3f, em = fch * 0.72f;
  const float bar_h = fch * 0.72f, group_gap = multi ? fch * 0.5f : fch * 0.28f;
  const float head = (s.title.empty() && !multi) ? 0 : fch * ((!s.title.empty() && multi) ? 2 : 1);
  const float body = float(n) * (float(k) * bar_h + group_gap);
  const int rows = std::max(2, int(std::ceil((pad * 2 + head + body) / fch)));
  Picture pic(cols * cw, rows * ch);
  math::Canvas& c = scratch(pic.w, pic.h);
  const float top = header(s, st, pic, c, pad, fch, multi, false) + (head > 0 ? fch * 0.2f : 0);

  float lw = 0, vw = 0;
  double vmax = 0;
  for (size_t i = 0; i < n; i++)
    lw = std::max(lw, math::text_width(i < s.labels.size() ? s.labels[i] : std::to_string(i), em));
  for (const auto& ser : s.series)
    for (size_t i = 0; i < n && i < ser.y.size(); i++)
      if (std::isfinite(ser.y[i])) {
        vmax = std::max(vmax, ser.y[i]);
        vw = std::max(vw, math::text_width(format_number(ser.y[i]), em));
      }
  lw = std::min(lw, float(pic.w) / 3);
  const float x0 = pad + lw + em * 0.5f, span = std::max(1.f, float(pic.w) - x0 - pad - vw - em * 0.5f);

  float y = top;
  for (size_t i = 0; i < n; i++) {
    const std::string label = clip_text(i < s.labels.size() ? s.labels[i] : std::to_string(i), em, lw);
    const float group = float(k) * bar_h;
    math::draw_text(c, label, em, x0 - em * 0.5f - math::text_width(label, em), y + group / 2 + em * 0.3f);
    pic.paint(c, st.text);
    for (size_t j = 0; j < k && j < s.series.size(); j++) {
      const double v = i < s.series[j].y.size() ? s.series[j].y[i] : kNaN;
      const float len = std::isfinite(v) && vmax > 0 ? float(std::max(0.0, v) / vmax) * span : 0.f;
      const float by = y + float(j) * bar_h;
      math::Item r{math::Item::Rect};
      r.x = x0;
      r.y = -(by + bar_h * 0.9f);
      r.a = len;
      r.b = bar_h * 0.8f;
      math::draw_rect(c, r, 1.f, 0, 0);
      pic.paint(c, st.series[j % kSeriesColors]);
      math::draw_text(c, format_number(v), em * 0.9f, x0 + len + em * 0.3f, by + bar_h * 0.5f + em * 0.3f);
      pic.paint(c, st.dim);
    }
    y += float(k) * bar_h + group_gap;
  }
  pic.fill(int(x0), int(top - fch * 0.1f), int(x0) + 1, int(y - group_gap + fch * 0.1f), st.dim, 1.f);
  return finish(pic, cols, rows, s);
}

math::Image draw_xy(const Spec& s, int cols, int cw, int ch, const Style& st) {
  const auto ty = [&](double v) { return s.log_y ? (v > 0 ? std::log10(v) : kNaN) : v; };
  double xmin = INFINITY, xmax = -INFINITY, ymin = INFINITY, ymax = -INFINITY;
  size_t points = 0;
  for (const auto& ser : s.series)
    for (size_t i = 0; i < ser.y.size(); i++) {
      const double x = i < ser.x.size() ? ser.x[i] : double(i);
      const double y = ty(ser.y[i]);
      if (!std::isfinite(x) || !std::isfinite(y)) continue;
      xmin = std::min(xmin, x), xmax = std::max(xmax, x);
      ymin = std::min(ymin, y), ymax = std::max(ymax, y);
      points++;
    }
  const bool empty = points == 0;
  if (empty) xmin = 0, xmax = 1, ymin = 0, ymax = 1;
  if (xmax == xmin) xmin -= 0.5, xmax += 0.5;
  if (ymax == ymin) {
    const double p = std::max(std::fabs(ymax) * 0.1, 1e-9);
    ymin -= p, ymax += p;
  }
  // A little air above and below the data, as matplotlib leaves.
  const double ypad = (ymax - ymin) * 0.05;
  ymin -= ypad, ymax += ypad;

  const bool multi = s.series.size() > 1 || !s.series.front().name.empty();
  const bool dots = s.kind == Spec::Kind::Scatter;
  const float fch = float(ch), pad = fch * 0.3f, em = fch * 0.72f;
  const int plot_rows = std::clamp(s.height, 4, 40);
  const int rows = plot_rows + (s.title.empty() && !multi ? 0 : 1) + (s.ylabel.empty() ? 0 : 1) +
                   (s.xlabel.empty() ? 1 : 2);
  Picture pic(cols * cw, rows * ch);
  math::Canvas& c = scratch(pic.w, pic.h);
  float top = header(s, st, pic, c, pad, fch, multi, dots);
  if (!s.ylabel.empty()) {
    math::draw_text(c, s.ylabel + (s.log_y ? " (log)" : ""), em, pad, top + fch * 0.75f);
    pic.paint(c, st.dim);
    top += fch;
  }
  top += fch * 0.3f;
  const float bottom = float(pic.h) - pad - fch * 0.95f - (s.xlabel.empty() ? 0 : fch);

  std::vector<double> yt;
  bool decades = false;
  if (s.log_y) {
    for (double e = std::ceil(ymin); e <= std::floor(ymax); e++) yt.push_back(e);
    decades = yt.size() >= 2;
    if (!decades) yt = nice_ticks(ymin, ymax, 4);
  } else {
    yt = nice_ticks(ymin, ymax, std::max(3, int((bottom - top) / (fch * 1.6f))));
  }
  const double ystep = step_of(yt);
  const auto label_y = [&](double v) {
    if (!s.log_y) return tick_label(v, ystep);
    const double p = std::pow(10.0, v);
    return decades ? tick_label(p, std::min(p, 1.0)) : format_number(p);
  };
  float ylw = 0;
  for (double v : yt) ylw = std::max(ylw, math::text_width(label_y(v), em));
  const float left = pad + ylw + em * 0.6f;
  const float xr_w = math::text_width(format_number(xmax), em);
  const float right = float(pic.w) - pad - xr_w / 2;
  const float pw = std::max(1.f, right - left), ph = std::max(1.f, bottom - top);
  const auto px = [&](double x) { return left + float((x - xmin) / (xmax - xmin)) * pw; };
  const auto py = [&](double y) { return bottom - float((y - ymin) / (ymax - ymin)) * ph; };

  float xlw = 0;
  const std::vector<double> probe = nice_ticks(xmin, xmax, 6);
  for (double v : probe) xlw = std::max(xlw, math::text_width(tick_label(v, step_of(probe)), em));
  const std::vector<double> xt = nice_ticks(xmin, xmax, std::max(2, int(pw / (xlw + em * 2.5f))));
  const double xstep = step_of(xt);

  // Grid, then the two spines, then the tick labels.
  const int L = int(left), R = int(std::ceil(right)), T = int(top), B = int(bottom);
  for (double v : yt)
    if (v >= ymin && v <= ymax) pic.fill(L, int(py(v)), R, int(py(v)) + 1, st.grid, 0.6f);
  for (double v : xt)
    if (v >= xmin && v <= xmax) pic.fill(int(px(v)), T, int(px(v)) + 1, B, st.grid, 0.6f);
  pic.fill(L, T, L + 1, B + 1, st.dim, 1.f);
  pic.fill(L, B, R, B + 1, st.dim, 1.f);
  for (double v : yt) {
    if (v < ymin || v > ymax) continue;
    const std::string l = label_y(v);
    math::draw_text(c, l, em, left - em * 0.4f - math::text_width(l, em), py(v) + em * 0.3f);
  }
  for (double v : xt) {
    if (v < xmin || v > xmax) continue;
    const std::string l = tick_label(v, xstep);
    math::draw_text(c, l, em, px(v) - math::text_width(l, em) / 2, bottom + fch * 0.85f);
  }
  if (!s.xlabel.empty())
    math::draw_text(c, s.xlabel, em, left + (pw - math::text_width(s.xlabel, em)) / 2,
                    bottom + fch * 1.85f);
  pic.paint(c, st.dim);

  if (empty) {
    math::draw_text(c, "no data", em, left + pw / 2 - math::text_width("no data", em) / 2, top + ph / 2);
    pic.paint(c, st.dim);
    return finish(pic, cols, rows, s);
  }

  // The data, one layer per series, kept inside the plot box. A series with
  // far more points than pixels is thinned: nobody sees the difference.
  const float t = std::max(1.5f, fch * 0.09f), dot = std::max(3.4f, fch * 0.26f);
  for (size_t k = 0; k < s.series.size(); k++) {
    const Series& ser = s.series[k];
    const size_t step = std::max<size_t>(1, ser.y.size() / size_t(std::max(1.f, pw * 3)));
    bool have = false;
    float lx = 0, ly = 0;
    for (size_t i = 0; i < ser.y.size(); i += step) {
      const double x = i < ser.x.size() ? ser.x[i] : double(i);
      const double y = ty(ser.y[i]);
      if (!std::isfinite(x) || !std::isfinite(y)) { have = false; continue; }  // a gap
      const float X = px(x), Y = py(y);
      if (dots) seg(c, X, Y, X, Y, dot);
      else if (have) seg(c, lx, ly, X, Y, t);
      else seg(c, X, Y, X, Y, t);  // a lone point still shows
      have = true;
      lx = X, ly = Y;
    }
    // Kept inside the plot box: each row's span is trimmed to it.
    const int cl = int(left) - 1, cr = int(right) + 1, ct = int(top) - 1, cb = int(bottom) + 1;
    for (int yy = std::max(0, c.y0); yy <= std::min(c.h - 1, c.y1); yy++) {
      const int lo = c.lo[size_t(yy)], hi = c.hi[size_t(yy)];
      if (hi < lo) continue;
      float* row = c.a.data() + size_t(yy) * size_t(c.w);
      if (yy < ct || yy > cb) {
        std::fill(row + lo, row + hi + 1, 0.f);
        continue;
      }
      if (lo < cl) std::fill(row + lo, row + std::min(hi + 1, cl), 0.f);
      if (hi > cr) std::fill(row + std::max(lo, cr + 1), row + hi + 1, 0.f);
    }
    pic.paint(c, st.series[k % kSeriesColors]);
  }
  return finish(pic, cols, rows, s);
}

// Ticks the axis runs between: from the last round number at or below `lo`
// to the first at or above `hi`, `n` steps of `step`.
struct Ticks {
  double t0 = 0, step = 1;
  int n = 1;
  double end() const { return t0 + step * n; }
  double at(int i) const { return t0 + step * i; }
};

Ticks enclose(double lo, double hi, int most, bool integral) {
  if (!(hi > lo)) hi = lo + 1;
  most = std::max(2, most);
  const double raw = (hi - lo) / double(most - 1);
  const double mag = std::pow(10.0, std::floor(std::log10(raw)));
  const double r = raw / mag;
  double step = (r <= 1 ? 1 : r <= 2 ? 2 : r <= 5 ? 5 : 10) * mag;
  if (integral) step = std::max(1.0, std::round(step));
  Ticks t;
  t.step = step;
  t.t0 = std::floor(lo / step + 1e-9) * step;
  double t1 = std::ceil(hi / step - 1e-9) * step;
  if (t1 <= t.t0) t1 = t.t0 + step;
  t.n = std::max(1, int(std::lround((t1 - t.t0) / step)));
  return t;
}

Piece text_piece(std::string t, uint8_t ink) {
  Piece p;
  p.text = std::move(t);
  p.ink = ink;
  return p;
}

std::string spaces(int n) { return std::string(size_t(std::max(0, n)), ' '); }

// Right-aligned in `w` columns (labels here are ASCII digits and words).
std::string right(const std::string& t, int w) { return spaces(w - text::str_width(t)) + t; }

std::string clip_cells(const std::string& t, int w) {
  if (text::str_width(t) <= w) return t;
  std::string cut;
  int used = 0;
  for (size_t i = 0; i < t.size();) {
    const size_t at = i;
    const char32_t cp = text::decode(t, i);
    if (used + text::cp_width(cp) > w - 1) break;
    used += text::cp_width(cp);
    cut.append(t, at, i - at);
  }
  return cut + "\xE2\x80\xA6";
}

// The title on the left and the legend on the right, as text rows; the
// legend gets its own row when both do not fit.
void header_rows(const Spec& s, int width, bool legend, const char* marker, Figure& f) {
  std::vector<Piece> keys;
  int kw = 0;
  if (legend)
    for (size_t i = 0; i < s.series.size(); i++) {
      const std::string name = series_name(s, i);
      if (i) keys.push_back(text_piece("   ", kInkText));
      keys.push_back(text_piece(marker, uint8_t(kInkSeries + i % kSeriesColors)));
      keys.push_back(text_piece(" " + name, kInkText));
      kw += (i ? 3 : 0) + text::str_width(marker) + 1 + text::str_width(name);
    }
  const int tw = text::str_width(s.title);
  if (!s.title.empty() && legend && tw + 3 + kw <= width) {
    std::vector<Piece> row{text_piece(s.title, kInkTitle), text_piece(spaces(width - tw - kw), kInkText)};
    row.insert(row.end(), keys.begin(), keys.end());
    f.rows.push_back(std::move(row));
    return;
  }
  if (!s.title.empty()) f.rows.push_back({text_piece(s.title, kInkTitle)});
  if (legend) f.rows.push_back(keys);
}

uint64_t hash_data(const Spec& s, std::string_view identity) {
  uint64_t h = 1469598103934665603ull;
  h = fnv(h, &s.kind, sizeof s.kind);
  h = fnv(h, &s.log_y, sizeof s.log_y);
  for (const auto& l : s.labels) h = fnv(h, l);
  // A live chart's frames replace each other, so it never shares a picture
  // with another chart that happens to hold the same numbers.
  if (!s.file.empty()) h = fnv(fnv(h, s.file), identity);
  for (const auto& ser : s.series) {
    h = fnv(h, ser.name);
    h = fnv(h, ser.x.data(), ser.x.size() * sizeof(double));
    h = fnv(h, ser.y.data(), ser.y.size() * sizeof(double));
  }
  return h;
}

bool cells_xy(const Spec& s, std::string_view identity, int cols, int cw, int ch, Figure& f) {
  const auto ty = [&](double v) { return s.log_y ? (v > 0 ? std::log10(v) : kNaN) : v; };
  double xmin = INFINITY, xmax = -INFINITY, ymin = INFINITY, ymax = -INFINITY;
  for (const auto& ser : s.series)
    for (size_t i = 0; i < ser.y.size(); i++) {
      const double x = i < ser.x.size() ? ser.x[i] : double(i);
      const double y = ty(ser.y[i]);
      if (!std::isfinite(x) || !std::isfinite(y)) continue;
      xmin = std::min(xmin, x), xmax = std::max(xmax, x);
      ymin = std::min(ymin, y), ymax = std::max(ymax, y);
    }
  if (!(xmin <= xmax)) return false;  // no data: the cell chart says so
  const bool hist = s.kind == Spec::Kind::Hist;
  if (hist) {
    // Columns stand on zero and span their bins' edges.
    xmin = s.bin0;
    xmax = s.bin0 + s.binw * double(s.series.front().y.size());
    ymin = std::min(ymin, 0.0);
  }
  if (xmax == xmin) xmin -= 0.5, xmax += 0.5;
  if (ymax == ymin) {
    const double p = std::max(std::fabs(ymax) * 0.1, 1e-9);
    ymin -= p, ymax += p;
  }

  // Rows: the y ticks a whole number of rows apart, so each sits on a row's
  // centre and its label in the margin cell beside it.
  const int want = std::clamp(s.height, 4, 40);
  Ticks yt;
  int rr = 0;
  for (int most = std::max(2, want / 3 + 1); most >= 2 && rr < 1; most--) {
    yt = enclose(ymin, ymax, most, s.log_y);
    rr = (want - 1) / yt.n;
  }
  if (rr < 1) return false;
  const int plot_rows = yt.n * rr + 1;
  const auto ylabel_of = [&](int i) {
    const double v = yt.at(i);
    return s.log_y ? tick_label(std::pow(10.0, v), std::min(std::pow(10.0, v), 1.0)) : tick_label(v, yt.step);
  };
  int margin = 0;
  for (int i = 0; i <= yt.n; i++) margin = std::max(margin, text::str_width(ylabel_of(i)));
  margin += 1;

  // Columns: likewise, the x ticks a whole number of columns apart, as many
  // as fit with their labels clear of each other.
  Ticks xt;
  int cc = 0, xw = 0, rpad = 0;
  for (int most = std::max(2, cols / 8); most >= 2; most--) {
    xt = enclose(xmin, xmax, most, false);
    xw = 0;
    for (int i = 0; i <= xt.n; i++) xw = std::max(xw, text::str_width(tick_label(xt.at(i), xt.step)));
    rpad = xw / 2 + 1;
    cc = (cols - margin - rpad - 1) / xt.n;
    if (cc >= xw + 2) break;
  }
  if (cc < 2) return false;
  const int plot_cols = xt.n * cc + 1;
  if (plot_cols < 10) return false;
  const int width = margin + plot_cols + rpad;

  // The picture: grid, spines and data, nothing else.
  const math::Config& cfg = math::config();
  uint64_t h = hash_data(s, identity);
  for (int v : {cw, ch, plot_cols, plot_rows, cc, rr, int(s.kind == Spec::Kind::Scatter)}) h = fnv(h, &v, sizeof v);
  for (double v : {xt.t0, xt.step, yt.t0, yt.step}) h = fnv(h, &v, sizeof v);
  char key[48];
  snprintf(key, sizeof key, "chartcells:%016llx", static_cast<unsigned long long>(h));
  const math::Image* im = math::cached(key);
  if (!im) {
    const Style st = theme_style();
    Picture pic(plot_cols * cw, plot_rows * ch);
    math::Canvas& c = scratch(pic.w, pic.h);
    const float fcw = float(cw), fch = float(ch);
    const auto px = [&](double x) { return (float((x - xt.t0) / (xt.end() - xt.t0)) * float(xt.n * cc) + 0.5f) * fcw; };
    const auto py = [&](double y) { return (float((yt.end() - y) / (yt.end() - yt.t0)) * float(yt.n * rr) + 0.5f) * fch; };
    const int L = int(px(xt.t0)), R = int(px(xt.end())), T = int(py(yt.end())), B = int(py(yt.t0));
    for (int i = 0; i <= yt.n; i++) pic.fill(L, int(py(yt.at(i))), R + 1, int(py(yt.at(i))) + 1, st.grid, 0.6f);
    for (int i = 0; i <= xt.n; i++) pic.fill(int(px(xt.at(i))), T, int(px(xt.at(i))) + 1, B + 1, st.grid, 0.6f);
    pic.fill(L, T, L + 1, B + 1, st.dim, 1.f);
    pic.fill(L, B, R + 1, B + 1, st.dim, 1.f);
    const bool dots = s.kind == Spec::Kind::Scatter;
    const float t = std::max(1.5f, fch * 0.09f), dot = std::max(3.4f, fch * 0.26f);
    const float pw = float(R - L);
    if (hist) {
      // A column per bin, a pixel of air between neighbours.
      const Series& ser = s.series.front();
      for (size_t i = 0; i < ser.y.size(); i++) {
        if (!std::isfinite(ser.y[i]) || ser.y[i] <= 0) continue;
        const float x0 = px(s.bin0 + s.binw * double(i)) + 1, x1 = px(s.bin0 + s.binw * double(i + 1)) - 1;
        const float y0 = py(ser.y[i]), y1 = py(0);
        math::Item r{math::Item::Rect};
        r.x = x0;
        r.y = -y1;
        r.a = std::max(1.f, x1 - x0);
        r.b = std::max(1.f, y1 - y0);
        math::draw_rect(c, r, 1.f, 0, 0);
      }
      pic.paint(c, st.series[0], 0.9f);
    }
    for (size_t k = 0; k < s.series.size() && !hist; k++) {
      const Series& ser = s.series[k];
      const size_t step = std::max<size_t>(1, ser.y.size() / size_t(std::max(1.f, pw * 3)));
      bool have = false;
      float lx = 0, ly = 0;
      for (size_t i = 0; i < ser.y.size(); i += step) {
        const double x = i < ser.x.size() ? ser.x[i] : double(i);
        const double y = ty(ser.y[i]);
        if (!std::isfinite(x) || !std::isfinite(y)) { have = false; continue; }  // a gap
        const float X = px(x), Y = py(y);
        if (dots) seg(c, X, Y, X, Y, dot);
        else if (have) seg(c, lx, ly, X, Y, t);
        else seg(c, X, Y, X, Y, t);  // a lone point still shows
        have = true;
        lx = X, ly = Y;
      }
      pic.paint(c, st.series[k % kSeriesColors]);
    }
    math::Image img;
    img.cols = plot_cols;
    img.rows = plot_rows;
    img.w = pic.w;
    img.h = pic.h;
    img.rgba = std::move(pic.px);
    img.display = true;
    img.copy = s.title.empty() ? "[chart]" : "[chart: " + s.title + "]";
    img.src = std::string(identity);
    im = math::store(key, std::move(img), s.file.empty() ? std::string() : "cells|" + std::string(identity));
  }
  (void)cfg;

  // The text around it.
  f.rows.clear();
  f.width = width;
  // A lone series under a title needs no legend: the title already says it.
  const bool multi = s.series.size() > 1 || (!s.series.front().name.empty() && s.title.empty());
  header_rows(s, width, multi, s.kind == Spec::Kind::Scatter ? "\xE2\x97\x8F" : "\xE2\x94\x81\xE2\x94\x81", f);
  if (!s.ylabel.empty()) f.rows.push_back({text_piece(s.ylabel + (s.log_y ? " (log)" : ""), kInkAxis)});
  for (int r = 0; r < plot_rows; r++) {
    Piece img;
    img.image = im->id;
    img.row = r;
    img.cols = plot_cols;
    const std::string label = r % rr == 0 ? ylabel_of(yt.n - r / rr) : std::string();
    f.rows.push_back({text_piece(right(label, margin - 1) + " ", kInkAxis), img});
  }
  std::string axis = spaces(width);
  int free_from = 0;
  for (int i = 0; i <= xt.n; i++) {
    const std::string l = tick_label(xt.at(i), xt.step);
    const int len = int(l.size());
    const int at = std::clamp(margin + i * cc - len / 2, 0, width - len);
    if (at < free_from) continue;
    axis.replace(size_t(at), l.size(), l);
    free_from = at + len + 1;
  }
  while (!axis.empty() && axis.back() == ' ') axis.pop_back();
  f.rows.push_back({text_piece(axis, kInkAxis)});
  if (!s.xlabel.empty()) {
    const std::string xl = clip_cells(s.xlabel, plot_cols);
    f.rows.push_back({text_piece(spaces(margin + (plot_cols - text::str_width(xl)) / 2) + xl, kInkAxis)});
  }
  return true;
}

bool cells_bars(const Spec& s, std::string_view identity, int cols, int cw, int ch, Figure& f) {
  size_t n = s.labels.size();
  for (const auto& ser : s.series) n = std::max(n, ser.y.size());
  n = std::min<size_t>(n, 60);
  if (n == 0) return false;
  const size_t k = s.series.size();
  const bool multi = k > 1;
  int lw = 0, vw = 0;
  double vmax = 0;
  const auto label_of = [&](size_t i) { return i < s.labels.size() ? s.labels[i] : std::to_string(i); };
  for (size_t i = 0; i < n; i++) lw = std::max(lw, text::str_width(label_of(i)));
  for (const auto& ser : s.series)
    for (size_t i = 0; i < n && i < ser.y.size(); i++)
      if (std::isfinite(ser.y[i])) {
        vmax = std::max(vmax, ser.y[i]);
        vw = std::max(vw, text::str_width(format_number(ser.y[i])));
      }
  lw = std::min(lw, std::max(4, cols / 3));
  const int margin = lw + 1;
  const int bmax = cols - margin - 1 - vw;
  if (bmax < 8) return false;
  const int bars = int(n * k);

  const float fcw = float(cw), fch = float(ch);
  const auto length = [&](double v) {
    return std::isfinite(v) && vmax > 0 ? float(std::max(0.0, v) / vmax) * (float(bmax) * fcw - 2) : 0.f;
  };
  uint64_t h = hash_data(s, identity);
  for (int v : {cw, ch, bmax, bars}) h = fnv(h, &v, sizeof v);
  char key[48];
  snprintf(key, sizeof key, "chartbars:%016llx", static_cast<unsigned long long>(h));
  const math::Image* im = math::cached(key);
  if (!im) {
    const Style st = theme_style();
    Picture pic(bmax * cw, bars * ch);
    math::Canvas& c = scratch(pic.w, pic.h);
    pic.fill(0, 0, 1, pic.h, st.dim, 1.f);
    for (size_t i = 0; i < n; i++)
      for (size_t j = 0; j < k; j++) {
        const float len = length(i < s.series[j].y.size() ? s.series[j].y[i] : kNaN);
        if (len <= 0) continue;
        const float top = float(i * k + j) * fch;
        math::Item r{math::Item::Rect};
        r.x = 1;
        r.y = -(top + fch * 0.8f);
        r.a = len;
        r.b = fch * 0.6f;
        math::draw_rect(c, r, 1.f, 0, 0);
        pic.paint(c, st.series[j % kSeriesColors]);
      }
    math::Image img;
    img.cols = bmax;
    img.rows = bars;
    img.w = pic.w;
    img.h = pic.h;
    img.rgba = std::move(pic.px);
    img.display = true;
    img.copy = s.title.empty() ? "[chart]" : "[chart: " + s.title + "]";
    img.src = std::string(identity);
    im = math::store(key, std::move(img), s.file.empty() ? std::string() : "bars|" + std::string(identity));
  }

  f.rows.clear();
  f.width = margin + bmax + 1 + vw;
  header_rows(s, f.width, multi, "\xE2\x96\xA0", f);
  for (size_t i = 0; i < n; i++) {
    if (i > 0 && multi) f.rows.push_back({});
    for (size_t j = 0; j < k; j++) {
      const double v = i < s.series[j].y.size() ? s.series[j].y[i] : kNaN;
      // Only as many of the picture's cells as the bar covers; the value
      // follows it as text.
      Piece img;
      img.image = im->id;
      img.row = int(i * k + j);
      img.cols = std::clamp(int(std::ceil((length(v) + 2) / fcw)), 1, bmax);
      const std::string label = j == 0 ? clip_cells(label_of(i), lw) : std::string();
      f.rows.push_back({text_piece(right(label, lw) + " ", kInkText), img,
                        text_piece(" " + format_number(v), kInkAxis)});
    }
  }
  return true;
}

// A heatmap: the grid a picture, a cell per value in a dark-friendly
// gradient (viridis); row labels in the margin, column labels under it, the
// range under that — all text.
bool cells_heat(const Spec& s, std::string_view identity, int cols, int cw, int ch, Figure& f) {
  double lo = INFINITY, hi = -INFINITY;
  size_t nc = 0;
  for (const auto& r : s.z) {
    nc = std::max(nc, r.size());
    for (double v : r)
      if (std::isfinite(v)) lo = std::min(lo, v), hi = std::max(hi, v);
  }
  if (nc == 0 || !(lo <= hi)) return false;
  int margin = 0;
  for (const auto& l : s.ylabels) margin = std::max(margin, text::str_width(l));
  margin = std::min(margin, cols / 3) + (margin ? 1 : 0);
  const int cell = std::clamp((cols - margin) / int(nc), 1, 6);
  if (cell < 1) return false;
  const int pcols = cell * int(nc), prow = int(s.z.size());

  uint64_t h = 1469598103934665603ull;
  for (const auto& r : s.z) h = fnv(h, r.data(), r.size() * sizeof(double));
  for (int v : {cw, ch, cell}) h = fnv(h, &v, sizeof v);
  char key[48];
  snprintf(key, sizeof key, "chartheat:%016llx", static_cast<unsigned long long>(h));
  const math::Image* im = math::cached(key);
  if (!im) {
    static constexpr int kStops[5][3] = {{68, 1, 84}, {59, 82, 139}, {33, 145, 140}, {94, 201, 98}, {253, 231, 37}};
    Picture pic(pcols * cw, prow * ch);
    for (int y = 0; y < prow; y++)
      for (size_t x = 0; x < s.z[size_t(y)].size(); x++) {
        const double v = s.z[size_t(y)][x];
        if (!std::isfinite(v)) continue;
        const double t = hi > lo ? (v - lo) / (hi - lo) * 4 : 2;
        const int k = std::clamp(int(t), 0, 3);
        const double u = std::clamp(t - k, 0.0, 1.0);
        Color col = 0;
        for (int ch3 = 0; ch3 < 3; ch3++)
          col = (col << 8) | Color(std::lround(kStops[k][ch3] + (kStops[k + 1][ch3] - kStops[k][ch3]) * u));
        // A pixel of panel between cells keeps them apart.
        pic.fill(int(x) * cell * cw + 1, y * ch + 1, (int(x) + 1) * cell * cw - 1, (y + 1) * ch - 1, col, 1.f);
      }
    math::Image img;
    img.cols = pcols;
    img.rows = prow;
    img.w = pic.w;
    img.h = pic.h;
    img.rgba = std::move(pic.px);
    img.display = true;
    img.copy = s.title.empty() ? "[heatmap]" : "[heatmap: " + s.title + "]";
    img.src = std::string(identity);
    im = math::store(key, std::move(img));
  }

  f.rows.clear();
  f.width = margin + pcols;
  if (!s.title.empty()) f.rows.push_back({text_piece(s.title, kInkTitle)});
  for (int y = 0; y < prow; y++) {
    Piece img;
    img.image = im->id;
    img.row = y;
    img.cols = pcols;
    const std::string l = size_t(y) < s.ylabels.size() ? clip_cells(s.ylabels[size_t(y)], margin - 1) : std::string();
    f.rows.push_back({text_piece(right(l, margin - (margin ? 1 : 0)) + (margin ? " " : ""), kInkAxis), img});
  }
  if (!s.labels.empty()) {
    // Column labels where they fit: every one, or every few.
    std::string axis = spaces(f.width);
    int free_from = 0;
    for (size_t x = 0; x < nc && x < s.labels.size(); x++) {
      const std::string l = clip_cells(s.labels[x], std::max(1, cell * 3));
      const int len = text::str_width(l);
      const int at = std::clamp(margin + int(x) * cell + cell / 2 - len / 2, 0, std::max(0, f.width - len));
      if (at < free_from || len != int(l.size())) continue;
      axis.replace(size_t(at), l.size(), l);
      free_from = at + len + 1;
    }
    while (!axis.empty() && axis.back() == ' ') axis.pop_back();
    f.rows.push_back({text_piece(axis, kInkAxis)});
  }
  f.rows.push_back({text_piece(spaces(margin) + format_number(lo) + " \xE2\x86\x92 " + format_number(hi), kInkAxis)});
  return true;
}

}  // namespace

const math::Image* image(const Spec& s, std::string_view identity, int cols) {
  const math::Config& cfg = math::config();
  if (!cfg.enabled || s.series.empty()) return nullptr;
  // Wide enough to read, not so wide a line chart turns into a ribbon.
  cols = std::min(cols, 110);
  if (cols < 30) return nullptr;

  uint64_t h = 1469598103934665603ull;
  h = fnv(h, &s.kind, sizeof s.kind);
  h = fnv(h, &s.log_y, sizeof s.log_y);
  h = fnv(h, &s.height, sizeof s.height);
  h = fnv(h, &cols, sizeof cols);
  h = fnv(h, &cfg.cell_w, sizeof cfg.cell_w);
  h = fnv(h, &cfg.cell_h, sizeof cfg.cell_h);
  h = fnv(h, s.title);
  h = fnv(h, s.xlabel);
  h = fnv(h, s.ylabel);
  for (const auto& l : s.labels) h = fnv(h, l);
  // A live chart's frames replace each other, so it never shares a picture
  // with another chart that happens to hold the same numbers.
  if (!s.file.empty()) h = fnv(fnv(h, s.file), identity);
  for (const auto& ser : s.series) {
    h = fnv(h, ser.name);
    h = fnv(h, ser.x.data(), ser.x.size() * sizeof(double));
    h = fnv(h, ser.y.data(), ser.y.size() * sizeof(double));
  }
  char key[40];
  snprintf(key, sizeof key, "chart:%016llx", static_cast<unsigned long long>(h));
  if (const math::Image* im = math::cached(key)) return im;

  const Style st = theme_style();
  math::Image im = s.kind == Spec::Kind::Bar ? draw_bars(s, cols, cfg.cell_w, cfg.cell_h, st)
                                             : draw_xy(s, cols, cfg.cell_w, cfg.cell_h, st);
  im.src = std::string(identity);
  // Only a chart read from a file changes under the same block; that one's
  // frames replace each other.
  const std::string lineage = s.file.empty() ? std::string() : "chart|" + std::string(identity);
  return math::store(key, std::move(im), lineage);
}

}  // namespace mico::chart

namespace mico::chart {

bool cells_figure(const Spec& s, std::string_view identity, int cols, Figure& out) {
  const math::Config& cfg = math::config();
  if (!cfg.enabled) return false;
  cols = std::min(cols, 110);
  if (cols < 24) return false;
  if (s.kind == Spec::Kind::Heatmap) return cells_heat(s, identity, cols, cfg.cell_w, cfg.cell_h, out);
  if (s.series.empty()) return false;
  return s.kind == Spec::Kind::Bar ? cells_bars(s, identity, cols, cfg.cell_w, cfg.cell_h, out)
                                   : cells_xy(s, identity, cols, cfg.cell_w, cfg.cell_h, out);
}

}  // namespace mico::chart
