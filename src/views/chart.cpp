#include "views/chart.h"

#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

#include "base/json.h"
#include "base/text.h"

namespace mico::chart {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double number(const js::Value& v) {
  if (v.type == js::Type::Number) {
    double d = kNaN;
    std::from_chars(v.raw.data(), v.raw.data() + v.raw.size(), d);
    return d;
  }
  // "1.5" as a string still plots; anything else is a gap.
  if (v.is_string()) {
    std::string s;
    js::unescape_append(v.body(), s);
    char* end = nullptr;
    const double d = strtod(s.c_str(), &end);
    return end && end != s.c_str() ? d : kNaN;
  }
  return kNaN;
}

std::string str(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  else s = std::string(v.raw);
  return s;
}

// Numbers into `nums`; strings, when every entry is one, into `labels`.
void values(const js::Value& arr, std::vector<double>& nums, std::vector<std::string>* labels) {
  bool all_strings = labels != nullptr;
  std::vector<std::string> strs;
  js::scan_array(arr.raw, [&](const js::Value& v) {
    nums.push_back(number(v));
    if (v.is_string()) strs.push_back(str(v));
    else all_strings = false;
    return nums.size() < 1000000;
  });
  if (all_strings && !strs.empty() && std::isnan(nums.front())) {
    *labels = std::move(strs);
    nums.clear();
  }
}

}  // namespace

bool parse(std::string_view json, Spec& out, std::string* error) {
  out = Spec{};
  size_t a = json.find_first_not_of(" \t\r\n");
  if (a == std::string_view::npos || json[a] != '{') {
    if (error) *error = "a chart is a JSON object";
    return false;
  }
  js::Value x{}, y{}, series{}, labels{}, subplots{};
  bool ok = js::scan_object(json.substr(a), [&](std::string_view k, const js::Value& v) {
    if (k == "type" || k == "kind") {
      const std::string t = str(v);
      if (t == "line") out.kind = Spec::Kind::Line;
      else if (t == "scatter") out.kind = Spec::Kind::Scatter;
      else if (t == "bar" || t == "barh") out.kind = Spec::Kind::Bar;
      else if (t == "hist" || t == "histogram") out.kind = Spec::Kind::Hist;
      else if (t == "spark" || t == "sparkline") out.kind = Spec::Kind::Spark;
      else if (t == "heatmap" || t == "heat") out.kind = Spec::Kind::Heatmap;
    } else if (k == "title") out.title = str(v);
    else if (k == "xlabel") out.xlabel = str(v);
    else if (k == "ylabel") out.ylabel = str(v);
    else if (k == "log_y" || k == "logy") out.log_y = v.is_true();
    else if (k == "height") out.height = int(number(v));
    else if (k == "marker") {
      const std::string m = str(v);
      if (m == "line") out.marker = Spec::Marker::Line;
      else if (m == "block" || m == "blocks") out.marker = Spec::Marker::Block;
      else if (m == "braille" || m == "dots") out.marker = Spec::Marker::Braille;
    }
    else if (k == "file") out.file = str(v);
    else if (k == "font") {
      const std::string f = str(v);
      out.font = (f == "math" || f == "latex" || f == "serif" || f == "tex") ? Spec::Font::Math
                                                                           : Spec::Font::Terminal;
      out.font_set = true;
    }
    else if (k == "subplots" || k == "charts" || k == "panels") subplots = v;
    else if (k == "columns" || k == "cols") out.columns = int(number(v));
    else if (k == "x") x = v;
    else if (k == "y") y = v;
    else if (k == "series") series = v;
    else if (k == "labels" || k == "xlabels") labels = v;
    else if (k == "values" || k == "data") y = v;
    else if (k == "bins") out.bins = int(number(v));
    else if (k == "ylabels") {
      js::scan_array(v.raw, [&](const js::Value& e) { out.ylabels.push_back(str(e)); return out.ylabels.size() < 200; });
    } else if (k == "z") {
      js::scan_array(v.raw, [&](const js::Value& row) {
        std::vector<double> r;
        if (row.is_array()) values(row, r, nullptr);
        out.z.push_back(std::move(r));
        return out.z.size() < 200;
      });
    }
    return true;
  });
  if (!ok) {
    if (error) *error = "not valid JSON";
    return false;
  }
  if (!(out.height >= 3)) out.height = 12;
  out.height = std::min(out.height, 40);
  if (!(out.columns >= 0)) out.columns = 0;

  // A figure: its charts, each parsed as one. They take the figure's font
  // unless they name their own.
  if (subplots.is_array()) {
    std::string why;
    int i = 0;
    bool good = true;
    js::scan_array(subplots.raw, [&](const js::Value& v) {
      i++;
      Spec sub;
      if (!parse(v.raw, sub, &why)) {
        if (error) *error = "subplot " + std::to_string(i) + ": " + why;
        good = false;
        return false;
      }
      if (!sub.font_set) sub.font = out.font;
      out.subplots.push_back(std::move(sub));
      return out.subplots.size() < 12;
    });
    if (!good) return false;
    if (out.subplots.empty()) {
      if (error) *error = "\"subplots\" is empty";
      return false;
    }
    return true;
  }

  if (!out.file.empty()) {
    // Columns by name: "x": "step", "y": "loss" or ["loss", "val_loss"].
    if (x.is_string()) out.xcol = str(x);
    if (y.is_string()) out.ycols.push_back(str(y));
    else if (y.is_array())
      js::scan_array(y.raw, [&](const js::Value& v) { out.ycols.push_back(str(v)); return true; });
    if (out.ycols.empty()) {
      if (error) *error = "a chart from a file needs \"y\": the column(s) to plot";
      return false;
    }
    return true;
  }

  std::vector<double> xs;
  if (labels.is_array()) {
    js::scan_array(labels.raw, [&](const js::Value& v) { out.labels.push_back(str(v)); return true; });
  }
  if (x.is_array()) values(x, xs, &out.labels);
  if (series.is_array()) {
    js::scan_array(series.raw, [&](const js::Value& sv) {
      Series s;
      if (sv.is_array()) {
        values(sv, s.y, nullptr);
      } else if (sv.is_object()) {
        js::scan_object(sv.raw, [&](std::string_view k, const js::Value& v) {
          if (k == "name" || k == "label") s.name = str(v);
          else if (k == "y" || k == "values" || k == "data") values(v, s.y, nullptr);
          else if (k == "x") values(v, s.x, nullptr);
          return true;
        });
      }
      if (s.x.empty()) s.x = xs;
      if (!s.y.empty()) out.series.push_back(std::move(s));
      return out.series.size() < 16;
    });
  } else if (y.is_array()) {
    Series s;
    values(y, s.y, nullptr);
    s.x = xs;
    if (!s.y.empty()) out.series.push_back(std::move(s));
  }
  if (out.kind == Spec::Kind::Heatmap) {
    if (out.z.empty() || out.z.front().empty()) {
      if (error) *error = "a heatmap needs \"z\": rows of values";
      return false;
    }
    return true;
  }
  if (out.series.empty()) {
    if (error) *error = "a chart needs data: \"y\" or \"series\"";
    return false;
  }
  return true;
}

Spec binned(const Spec& s) {
  if (s.kind != Spec::Kind::Hist || s.series.empty()) return s;
  Spec out = s;
  std::vector<double> v;
  for (const auto& ser : s.series)
    for (double x : ser.y)
      if (std::isfinite(x)) v.push_back(x);
  out.series.assign(1, Series{});
  out.series[0].name = s.series[0].name;
  if (v.empty()) return out;
  const auto [lo_it, hi_it] = std::minmax_element(v.begin(), v.end());
  double lo = *lo_it, hi = *hi_it;
  if (hi == lo) { lo -= 0.5; hi += 0.5; }
  // Square-root choice, kept to a readable number of columns.
  const int bins = s.bins > 0 ? std::min(s.bins, 200) : std::clamp(int(std::sqrt(double(v.size()))), 5, 30);
  out.bin0 = lo;
  out.binw = (hi - lo) / bins;
  std::vector<double> counts(size_t(bins), 0);
  for (double x : v) counts[size_t(std::min(bins - 1, int((x - lo) / out.binw)))] += 1;
  for (int i = 0; i < bins; i++) {
    out.series[0].x.push_back(lo + (i + 0.5) * out.binw);
    out.series[0].y.push_back(counts[size_t(i)]);
  }
  return out;
}

namespace {

struct Table {
  int64_t mtime = 0;
  int64_t size = -1;
  std::vector<std::string> names;
  std::vector<std::vector<double>> cols;
};

// A few recently read files. A chat redraws its charts on every relayout
// (a resize, a density change), and a metrics file can be large.
struct Cache {
  std::string path;
  Table table;
};
std::vector<Cache>& cache() {
  static std::vector<Cache> c;
  return c;
}

int column(Table& t, const std::string& name) {
  for (size_t i = 0; i < t.names.size(); i++)
    if (t.names[i] == name) return int(i);
  t.names.push_back(name);
  t.cols.emplace_back();
  return int(t.names.size() - 1);
}

void read_csv(std::string_view data, Table& t) {
  bool header = true;
  std::vector<int> idx;
  size_t row = 0;
  for (size_t pos = 0; pos < data.size();) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string_view::npos) nl = data.size();
    std::string_view line = data.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    const char sep = line.find('\t') != std::string_view::npos && line.find(',') == std::string_view::npos ? '\t' : ',';
    std::vector<std::string_view> cells;
    for (size_t a = 0;;) {
      const size_t b = line.find(sep, a);
      std::string_view c = line.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a);
      while (!c.empty() && (c.front() == ' ' || c.front() == '"')) c.remove_prefix(1);
      while (!c.empty() && (c.back() == ' ' || c.back() == '"')) c.remove_suffix(1);
      cells.push_back(c);
      if (b == std::string_view::npos) break;
      a = b + 1;
    }
    if (header) {
      header = false;
      for (auto c : cells) idx.push_back(column(t, std::string(c)));
      continue;
    }
    for (size_t i = 0; i < idx.size(); i++) {
      double d = kNaN;
      if (i < cells.size() && !cells[i].empty()) {
        const auto r = std::from_chars(cells[i].data(), cells[i].data() + cells[i].size(), d);
        if (r.ec != std::errc()) d = kNaN;
      }
      auto& col = t.cols[size_t(idx[i])];
      col.resize(row, kNaN);
      col.push_back(d);
    }
    row++;
  }
  for (auto& c : t.cols) c.resize(row, kNaN);
}

void read_jsonl(std::string_view data, Table& t) {
  size_t row = 0;
  for (size_t pos = 0; pos < data.size();) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string_view::npos) nl = data.size();
    const std::string_view line = data.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.find('{') == std::string_view::npos) continue;
    bool any = false;
    js::scan_object(line, [&](std::string_view k, const js::Value& v) {
      if (v.type != js::Type::Number && v.type != js::Type::Null) return true;
      auto& col = t.cols[size_t(column(t, std::string(k)))];
      col.resize(row, kNaN);
      col.push_back(number(v));
      any = true;
      return true;
    });
    if (any) row++;
  }
  for (auto& c : t.cols) c.resize(row, kNaN);
}

}  // namespace

bool load_file(Spec& s, const std::string& base_dir, std::string* error, int64_t* mtime_ns,
               std::string* abs_path) {
  std::string path = s.file;
  if (!path.empty() && path[0] == '~') {
    if (const char* h = getenv("HOME")) path = std::string(h) + path.substr(1);
  } else if (!path.empty() && path[0] != '/' && !base_dir.empty()) {
    path = base_dir + "/" + path;
  }
  if (abs_path) *abs_path = path;
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) {
    if (error) *error = "no such file: " + s.file;
    return false;
  }
  const int64_t mtime = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
  if (mtime_ns) *mtime_ns = mtime;
  constexpr int64_t kMaxBytes = 64ll << 20;
  if (st.st_size > kMaxBytes) {
    if (error) *error = "file too large to chart (over 64 MB): " + s.file;
    return false;
  }

  auto& c = cache();
  Table* t = nullptr;
  for (auto& e : c)
    if (e.path == path) t = &e.table;
  if (!t || t->mtime != mtime || t->size != int64_t(st.st_size)) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string data = ss.str();
    Table fresh;
    fresh.mtime = mtime;
    fresh.size = int64_t(st.st_size);
    const size_t first = data.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && data[first] == '{') read_jsonl(data, fresh);
    else read_csv(data, fresh);
    if (!t) {
      if (c.size() >= 8) c.erase(c.begin());
      c.push_back(Cache{path, {}});
      t = &c.back().table;
    }
    *t = std::move(fresh);
  }

  const auto find = [&](const std::string& name) -> const std::vector<double>* {
    for (size_t i = 0; i < t->names.size(); i++)
      if (t->names[i] == name) return &t->cols[i];
    return nullptr;
  };
  const std::vector<double>* xs = s.xcol.empty() ? nullptr : find(s.xcol);
  if (!s.xcol.empty() && !xs) {
    if (error) *error = "no column \"" + s.xcol + "\" in " + s.file;
    return false;
  }
  s.series.clear();
  for (const auto& name : s.ycols) {
    const std::vector<double>* ys = find(name);
    if (!ys) {
      if (error) *error = "no column \"" + name + "\" in " + s.file;
      return false;
    }
    Series ser;
    ser.name = name;
    ser.y = *ys;
    if (xs) ser.x = *xs;
    s.series.push_back(std::move(ser));
  }
  if (s.xlabel.empty()) s.xlabel = s.xcol;
  return true;
}

bool load_files(Spec& s, const std::string& base_dir, std::string* error,
                std::vector<std::pair<std::string, int64_t>>* watched) {
  if (!s.file.empty()) {
    int64_t mtime = 0;
    std::string path;
    const bool ok = load_file(s, base_dir, error, &mtime, &path);
    // Watched even when missing: a run may not have written it yet.
    if (watched) watched->emplace_back(path, ok ? mtime : 0);
    if (!ok) return false;
  }
  for (size_t i = 0; i < s.subplots.size(); i++) {
    std::string why;
    if (!load_files(s.subplots[i], base_dir, &why, watched)) {
      if (error) *error = "subplot " + std::to_string(i + 1) + ": " + why;
      return false;
    }
  }
  return true;
}

std::string format_number(double v) {
  if (std::isnan(v)) return "";
  char b[32];
  const double a = std::fabs(v);
  if (a == 0) return "0";
  if (a >= 1e9) snprintf(b, sizeof b, "%.3gG", v / 1e9);
  else if (a >= 1e6) snprintf(b, sizeof b, "%.3gM", v / 1e6);
  else if (a >= 1e4) snprintf(b, sizeof b, "%.3gk", v / 1e3);
  else if (a >= 1e3) snprintf(b, sizeof b, "%.0f", v);  // %.3g would say 1e+03
  else if (a < 1e-3) snprintf(b, sizeof b, "%.1e", v);
  else snprintf(b, sizeof b, "%.3g", v);
  return b;
}

namespace {

void put(Canvas& c, int x, int y, std::string_view s, uint8_t ink) {
  for (size_t i = 0; i < s.size() && x < c.w;) {
    const char32_t cp = text::decode(s, i);
    if (x >= 0 && y >= 0 && y < c.h) {
      c.at(x, y) = cp;
      c.ink_at(x, y) = ink;
    }
    x += std::max(1, text::cp_width(cp));
  }
}

void legend(const Spec& s, Canvas& c, int y, int x0) {
  int x = x0;
  for (size_t i = 0; i < s.series.size(); i++) {
    const std::string name = s.series[i].name.empty() ? "series " + std::to_string(i + 1) : s.series[i].name;
    if (x + text::str_width(name) + 3 > c.w) break;
    put(c, x, y, "\xE2\x97\x8F", uint8_t(kInkSeries + i % kSeriesColors));  // ●
    put(c, x + 2, y, name, kInkText);
    x += text::str_width(name) + 4;
  }
}

void draw_bars(const Spec& s, int cols, Canvas& c) {
  // One row per category and series: label, bar in eighths, value.
  size_t n = s.labels.size();
  for (const auto& ser : s.series) n = std::max(n, ser.y.size());
  n = std::min<size_t>(n, 200);
  const bool multi = s.series.size() > 1;
  int lw = 0;
  for (size_t i = 0; i < n; i++)
    lw = std::max(lw, text::str_width(i < s.labels.size() ? s.labels[i] : std::to_string(i)));
  lw = std::min(lw, std::max(4, cols / 3));
  double vmax = 0;
  int vw = 0;
  for (const auto& ser : s.series)
    for (size_t i = 0; i < n && i < ser.y.size(); i++)
      if (std::isfinite(ser.y[i])) {
        vmax = std::max(vmax, ser.y[i]);
        vw = std::max(vw, text::str_width(format_number(ser.y[i])));
      }
  const int top = (s.title.empty() ? 0 : 1) + (multi ? 1 : 0);
  const int rows = int(n * s.series.size()) + (multi ? int(n) - 1 : 0);
  c.w = cols;
  c.h = top + std::max(1, rows);
  c.cp.assign(size_t(c.w) * size_t(c.h), U' ');
  c.ink.assign(c.cp.size(), kInkText);
  if (!s.title.empty()) put(c, 0, 0, s.title, kInkTitle);
  if (multi) legend(s, c, s.title.empty() ? 0 : 1, 0);
  const int barw = std::max(1, cols - lw - vw - 3);
  static const char32_t kEighths[] = {U' ', U'▏', U'▎', U'▍', U'▌',
                                      U'▋', U'▊', U'▉', U'█'};
  int y = top;
  for (size_t i = 0; i < n; i++) {
    for (size_t k = 0; k < s.series.size(); k++, y++) {
      if (k == 0) {
        std::string label = i < s.labels.size() ? s.labels[i] : std::to_string(i);
        if (text::str_width(label) > lw) {
          std::string cut;
          int w = 0;
          for (size_t j = 0; j < label.size();) {
            const size_t at = j;
            const char32_t cp = text::decode(label, j);
            if (w + text::cp_width(cp) > lw - 1) break;
            w += text::cp_width(cp);
            cut.append(label, at, j - at);
          }
          label = cut + "\xE2\x80\xA6";
        }
        put(c, lw - text::str_width(label), y, label, kInkText);
      }
      put(c, lw, y, "\xE2\x94\x82", kInkAxis);  // │
      const double v = i < s.series[k].y.size() ? s.series[k].y[i] : kNaN;
      const uint8_t ink = uint8_t(kInkSeries + k % kSeriesColors);
      int eighths = std::isfinite(v) && vmax > 0 ? int(std::lround(std::max(0.0, v) / vmax * barw * 8)) : 0;
      int x = lw + 1;
      for (; eighths >= 8; eighths -= 8) c.at(x, y) = U'█', c.ink_at(x, y) = ink, x++;
      if (eighths > 0) c.at(x, y) = kEighths[eighths], c.ink_at(x, y) = ink, x++;
      put(c, x + 1, y, format_number(v), kInkAxis);
    }
    if (multi) y++;  // a gap between categories
  }
}

}  // namespace

// A line chart drawn with box-drawing characters, one value per column, the
// way asciichart draws: ─ along, ╭╮╰╯ at the turns, │ for the climb. The
// terminal draws these itself, so the line is whole whatever the font.
// Several points in one column are averaged; columns between points are
// interpolated; a gap in the data stays a gap.
template <class F>
void draw_line(const Spec& s, F ty, double xmin, double xmax, double ymin, double ymax, int x0, int top,
               int W, int H, Canvas& c) {
  const auto col_of = [&](double x) {
    return std::clamp(int(std::lround((x - xmin) / (xmax - xmin) * (W - 1))), 0, W - 1);
  };
  const auto row_of = [&](double y) {
    return std::clamp(int(std::lround((ymax - y) / (ymax - ymin) * (H - 1))), 0, H - 1);
  };
  std::vector<double> sum(static_cast<size_t>(W)), val(static_cast<size_t>(W));
  std::vector<int> n(static_cast<size_t>(W));
  for (size_t k = 0; k < s.series.size(); k++) {
    const Series& ser = s.series[k];
    const uint8_t ink = uint8_t(kInkSeries + k % kSeriesColors);
    std::fill(sum.begin(), sum.end(), 0.0);
    std::fill(n.begin(), n.end(), 0);
    std::fill(val.begin(), val.end(), kNaN);
    // Averages per column, then straight lines between consecutive points.
    bool have = false;
    int lc = 0;
    for (size_t i = 0; i < ser.y.size(); i++) {
      const double x = i < ser.x.size() ? ser.x[i] : double(i);
      const double y = ty(ser.y[i]);
      if (!std::isfinite(x) || !std::isfinite(y)) { have = false; continue; }
      const int col = col_of(x);
      sum[size_t(col)] += y;
      n[size_t(col)]++;
      if (have && col > lc + 1) {
        // Filled in below, once both ends' averages are known.
        for (int j = lc + 1; j < col; j++) val[size_t(j)] = -INFINITY;
      }
      have = true;
      lc = col;
    }
    for (int j = 0; j < W; j++)
      if (n[size_t(j)]) val[size_t(j)] = sum[size_t(j)] / n[size_t(j)];
    for (int j = 0; j < W; j++) {
      if (val[size_t(j)] != -INFINITY) continue;
      int a = j - 1, b = j;
      while (b < W && val[size_t(b)] == -INFINITY) b++;
      for (int t = j; t < b; t++)
        val[size_t(t)] = b < W && a >= 0 ? val[size_t(a)] + (val[size_t(b)] - val[size_t(a)]) * double(t - a) / double(b - a) : kNaN;
      j = b;
    }
    const auto set = [&](int col, int row, char32_t g) {
      c.at(x0 + col, top + row) = g;
      c.ink_at(x0 + col, top + row) = ink;
    };
    int prev = -1;
    for (int j = 0; j < W; j++) {
      if (!std::isfinite(val[size_t(j)])) { prev = -1; continue; }
      const int r = row_of(val[size_t(j)]);
      if (prev < 0 || r == prev) {
        set(j, r, U'─');  // ─
      } else if (r < prev) {   // up
        set(j, prev, U'╯');  // ╯
        for (int k2 = r + 1; k2 < prev; k2++) set(j, k2, U'│');
        set(j, r, U'╭');  // ╭
      } else {                 // down
        set(j, prev, U'╮');  // ╮
        for (int k2 = prev + 1; k2 < r; k2++) set(j, k2, U'│');
        set(j, r, U'╰');  // ╰
      }
      prev = r;
    }
  }
}

void draw(const Spec& s, int cols, Canvas& c) {
  cols = std::max(cols, 20);
  if (s.kind == Spec::Kind::Bar) {
    draw_bars(s, cols, c);
    return;
  }
  if (s.kind == Spec::Kind::Hist) {
    // Without pictures, a histogram is its bins as labelled bars.
    Spec b = binned(s);
    b.kind = Spec::Kind::Bar;
    b.labels.clear();
    for (size_t i = 0; i < b.series[0].y.size(); i++)
      b.labels.push_back(format_number(b.bin0 + double(i) * b.binw) + "\xE2\x80\x93" +
                         format_number(b.bin0 + double(i + 1) * b.binw));
    draw_bars(b, cols, c);
    return;
  }
  if (s.kind == Spec::Kind::Heatmap) {
    // Shades for values, a row per row of z.
    static const char32_t kShade[] = {U' ', U'\u2591', U'\u2592', U'\u2593', U'\u2588'};
    double lo = INFINITY, hi = -INFINITY;
    size_t nc = 0;
    for (const auto& r : s.z) {
      nc = std::max(nc, r.size());
      for (double v : r)
        if (std::isfinite(v)) lo = std::min(lo, v), hi = std::max(hi, v);
    }
    int lw = 0;
    for (const auto& l : s.ylabels) lw = std::max(lw, text::str_width(l));
    const int cw = std::clamp(nc ? (cols - lw - 1) / int(nc) : 1, 1, 4);
    const int top = s.title.empty() ? 0 : 1;
    c.w = cols;
    c.h = top + int(s.z.size()) + 1;
    c.cp.assign(size_t(c.w) * size_t(c.h), U' ');
    c.ink.assign(c.cp.size(), kInkText);
    if (top) put(c, 0, 0, s.title, kInkTitle);
    for (size_t y = 0; y < s.z.size(); y++) {
      if (y < s.ylabels.size()) put(c, lw - text::str_width(s.ylabels[y]), top + int(y), s.ylabels[y], kInkAxis);
      for (size_t x = 0; x < s.z[y].size(); x++) {
        const double v = s.z[y][x];
        const int lvl = std::isfinite(v) && hi > lo ? std::clamp(int((v - lo) / (hi - lo) * 4 + 0.5), 0, 4) : 0;
        for (int k = 0; k < cw; k++) {
          const int px = lw + 1 + int(x) * cw + k;
          if (px < c.w) { c.at(px, top + int(y)) = kShade[lvl]; c.ink_at(px, top + int(y)) = kInkSeries; }
        }
      }
    }
    put(c, lw + 1, top + int(s.z.size()), format_number(lo) + " \xE2\x96\x91\xE2\x96\x92\xE2\x96\x93\xE2\x96\x88 " + format_number(hi), kInkAxis);
    return;
  }

  // The data's extent. A log axis plots log10, and skips what it cannot.
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
  const bool empty = !(xmin <= xmax);
  if (empty) xmin = 0, xmax = 1, ymin = 0, ymax = 1;
  if (xmax == xmin) xmin -= 0.5, xmax += 0.5;
  if (ymax == ymin) {
    const double pad = std::max(std::fabs(ymax) * 0.1, 1e-9);
    ymin -= pad, ymax += pad;
  }
  const auto label_y = [&](double v) { return format_number(s.log_y ? std::pow(10.0, v) : v); };

  const int H = s.height;
  const bool multi = s.series.size() > 1 || !s.series.front().name.empty();
  const int top = (s.title.empty() ? 0 : 1) + (multi || !s.ylabel.empty() ? 1 : 0);
  const std::string ytop = label_y(ymax), ymid = label_y((ymin + ymax) / 2), ybot = label_y(ymin);
  const int lw = std::max({text::str_width(ytop), text::str_width(ymid), text::str_width(ybot)});
  const int W = std::max(8, cols - lw - 1);
  c.w = lw + 1 + W;
  c.h = top + H + 2;
  c.cp.assign(size_t(c.w) * size_t(c.h), U' ');
  c.ink.assign(c.cp.size(), kInkText);

  if (!s.title.empty()) put(c, 0, 0, s.title, kInkTitle);
  if (top > (s.title.empty() ? 0 : 1)) {
    const int ly = s.title.empty() ? 0 : 1;
    int x = 0;
    if (!s.ylabel.empty()) {
      put(c, 0, ly, s.ylabel + (s.log_y ? " (log)" : ""), kInkAxis);
      x = text::str_width(s.ylabel) + (s.log_y ? 6 : 0) + 3;
    }
    if (multi) legend(s, c, ly, x);
  }

  // The y axis, with its three labels, and the x axis under the plot.
  for (int r = 0; r < H; r++) {
    const bool tick = r == 0 || r == H - 1 || r == H / 2;
    put(c, lw, top + r, tick ? "\xE2\x94\xA4" : "\xE2\x94\x82", kInkAxis);  // ┤ │
  }
  put(c, lw - text::str_width(ytop), top, ytop, kInkAxis);
  put(c, lw - text::str_width(ymid), top + H / 2, ymid, kInkAxis);
  put(c, lw - text::str_width(ybot), top + H - 1, ybot, kInkAxis);
  put(c, lw, top + H, "\xE2\x94\x94", kInkAxis);  // └
  for (int x = 0; x < W; x++) put(c, lw + 1 + x, top + H, "\xE2\x94\x80", kInkAxis);
  const std::string xl = format_number(xmin), xr = format_number(xmax);
  // The axis's name, when it has one, sits in the middle; else the midpoint.
  const std::string xm = s.xlabel.empty() ? format_number((xmin + xmax) / 2) : s.xlabel;
  put(c, lw + 1, top + H + 1, xl, kInkAxis);
  if (W > text::str_width(xl) + text::str_width(xr) + text::str_width(xm) + 6)
    put(c, lw + 1 + (W - text::str_width(xm)) / 2, top + H + 1, xm, kInkAxis);
  put(c, lw + 1 + W - text::str_width(xr), top + H + 1, xr, kInkAxis);
  if (empty) {
    put(c, lw + 3, top + H / 2, "no data", kInkAxis);
    return;
  }

  // Braille by default: the finest detail, and what the user prefers. Line
  // and block are there for terminals whose font draws braille poorly.
  Spec::Marker marker = s.marker == Spec::Marker::Auto ? Spec::Marker::Braille : s.marker;
  if (marker == Spec::Marker::Line && s.kind == Spec::Kind::Scatter) marker = Spec::Marker::Block;

  if (marker == Spec::Marker::Line) {
    draw_line(s, ty, xmin, xmax, ymin, ymax, lw + 1, top, W, H, c);
    return;
  }

  // Dots on a grid finer than the cells: braille, 2x4 per cell, or block
  // quadrants, 2x2. Lines between points are drawn dot by dot.
  const bool braille = marker == Spec::Marker::Braille;
  const int sw = 2, sh = braille ? 4 : 2;
  const int DW = W * sw, DH = H * sh;
  std::vector<uint8_t> dots(size_t(W) * size_t(H), 0);
  std::vector<uint8_t> owner(size_t(W) * size_t(H), 0);
  static const uint8_t kBraille[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
  static const uint8_t kQuad[2][2] = {{0x1, 0x2}, {0x4, 0x8}};
  const auto plot = [&](int px, int py, uint8_t ink) {
    if (px < 0 || py < 0 || px >= DW || py >= DH) return;
    const size_t cell = size_t(py / sh) * size_t(W) + size_t(px / sw);
    dots[cell] |= braille ? kBraille[py % 4][px % 2] : kQuad[py % 2][px % 2];
    owner[cell] = ink;
  };
  const auto to_dot = [&](double x, double y, int* px, int* py) {
    *px = int(std::lround((x - xmin) / (xmax - xmin) * (DW - 1)));
    *py = int(std::lround((ymax - y) / (ymax - ymin) * (DH - 1)));
  };
  for (size_t k = 0; k < s.series.size(); k++) {
    const Series& ser = s.series[k];
    const uint8_t ink = uint8_t(kInkSeries + k % kSeriesColors);
    bool have = false;
    int lx = 0, ly = 0;
    for (size_t i = 0; i < ser.y.size(); i++) {
      const double x = i < ser.x.size() ? ser.x[i] : double(i);
      const double y = ty(ser.y[i]);
      if (!std::isfinite(x) || !std::isfinite(y)) { have = false; continue; }  // a gap
      int px, py;
      to_dot(x, y, &px, &py);
      if (s.kind == Spec::Kind::Line && have) {
        // Bresenham from the last point.
        int x0 = lx, y0 = ly;
        const int dx = std::abs(px - x0), sx = x0 < px ? 1 : -1;
        const int dy = -std::abs(py - y0), sy = y0 < py ? 1 : -1;
        int err = dx + dy;
        for (;;) {
          plot(x0, y0, ink);
          if (x0 == px && y0 == py) break;
          const int e2 = 2 * err;
          if (e2 >= dy) { err += dy; x0 += sx; }
          if (e2 <= dx) { err += dx; y0 += sy; }
        }
      } else {
        plot(px, py, ink);
      }
      have = true;
      lx = px, ly = py;
    }
  }
  // ▘▝▀▖▌▞▛▗▚▐▜▄▙▟█ by quadrant bits: top-left 1, top-right 2, bottom-left 4, bottom-right 8.
  static const char32_t kQuadGlyph[16] = {U' ',      U'▘', U'▝', U'▀', U'▖', U'▌',
                                          U'▞', U'▛', U'▗', U'▚', U'▐', U'▜',
                                          U'▄', U'▙', U'▟', U'█'};
  for (int r = 0; r < H; r++)
    for (int col = 0; col < W; col++) {
      const size_t cell = size_t(r) * size_t(W) + size_t(col);
      if (!dots[cell]) continue;
      c.at(lw + 1 + col, top + r) = braille ? char32_t(0x2800 + dots[cell]) : kQuadGlyph[dots[cell] & 0xF];
      c.ink_at(lw + 1 + col, top + r) = owner[cell];
    }
}

}  // namespace mico::chart
