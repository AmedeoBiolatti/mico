#include "views/markdown.h"

#include <cctype>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <ctime>
#include <unordered_map>

#include <sys/stat.h>

#include <fstream>

#include "core/settings.h"
#include "math/math.h"
#include "math/picture.h"
#include "term/links.h"
#include "views/chart.h"
#include "views/diagram.h"
#include "views/latex.h"

namespace mico::md {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }

std::string_view trim_left(std::string_view s) {
  size_t i = 0;
  while (i < s.size() && is_space(s[i])) i++;
  return s.substr(i);
}

// "---", "***", "___" on a line of their own.
bool is_rule(std::string_view s) {
  s = trim_left(s);
  if (s.size() < 3) return false;
  char c = s[0];
  if (c != '-' && c != '*' && c != '_') return false;
  for (char x : s)
    if (x != c && !is_space(x)) return false;
  return true;
}

int heading_level(std::string_view s) {
  int n = 0;
  while (n < int(s.size()) && s[size_t(n)] == '#') n++;
  if (n == 0 || n > 6) return 0;
  if (n < int(s.size()) && !is_space(s[size_t(n)])) return 0;
  return n;
}

// "- ", "* ", "+ " or "1. ". Returns the width of the marker, else 0.
size_t list_marker(std::string_view s, bool* ordered) {
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') i++;
  size_t indent = i;
  if (i < s.size() && (s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < s.size() &&
      is_space(s[i + 1])) {
    *ordered = false;
    return indent + 2;
  }
  size_t d = i;
  while (d < s.size() && s[d] >= '0' && s[d] <= '9') d++;
  if (d > i && d + 1 < s.size() && (s[d] == '.' || s[d] == ')') && is_space(s[d + 1])) {
    *ordered = true;
    return d + 2 - 0;
  }
  return 0;
}

bool fence_at(std::string_view s) {
  std::string_view t = trim_left(s);
  return t.size() >= 3 && (t.compare(0, 3, "```") == 0 || t.compare(0, 3, "~~~") == 0);
}

// The language after an opening fence: "```chart" -> "chart".
std::string_view fence_lang(std::string_view s) {
  std::string_view t = trim_left(s);
  t.remove_prefix(std::min<size_t>(3, t.size()));
  while (!t.empty() && (t.front() == '`' || t.front() == '~' || t.front() == ' ')) t.remove_prefix(1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.remove_suffix(1);
  return t;
}

// One row of an equation image: an 8-byte record, then a no-break space per
// column. The segment names the spaces, so anything that reads its text sees
// blank columns of the right width; image_ref() reads the record back.
Seg image_seg(Arena& a, uint32_t id, int row, int cols) {
  char rec[8];
  const uint16_t r = uint16_t(row), c = uint16_t(cols);
  memcpy(rec, &id, 4);
  memcpy(rec + 4, &r, 2);
  memcpy(rec + 6, &c, 2);
  a.put(std::string_view(rec, 8));
  const uint32_t at = a.open();
  for (int i = 0; i < cols; i++) a.put("\xC2\xA0");
  const Str t = a.close(at);
  return Seg{t.off | kScratchBit, t.len, Ink::MathImage};
}

// A picture laid out as rows, one line of placeholder cells each.
void emit_image(const math::Image& im, uint8_t indent, Out& out);

// A line that is only ![alt](target): drawn as the picture, when target is an
// image file on disk. The file is watched like a chart's data, so an image the
// agent writes again (a plot it regenerates) is drawn again.
bool emit_picture_line(std::string_view alt, std::string_view target, int cols, Out& out) {
  const int rows = render_settings().picture_rows();
  if (rows == 0) return false;
  if (target.starts_with("data:image/")) {
    // Inline data (a notebook's plot): named by the data itself.
    const size_t comma = target.find(',');
    if (comma == std::string_view::npos || target.substr(0, comma).find(";base64") == std::string_view::npos) return false;
    const std::string_view b64 = target.substr(comma + 1);
    if (b64.size() < 16) return false;
    const std::string key = "data:" + std::to_string(b64.size()) + ":" + std::string(b64.substr(0, 40)) +
                            std::string(b64.substr(b64.size() - 40));
    const bool zoom = out.charts && out.charts->zoomed(key);
    const math::Image* im = math::picture(
        key, [b64](std::string& bytes) { return math::base64_decode(b64, bytes); }, cols - 2,
        zoom ? out.charts->zoom_rows : rows, "[" + std::string(alt.empty() ? "image" : alt) + "]", zoom);
    if (!im) return false;
    emit_image(*im, 2, out);
    return true;
  }
  if (target.starts_with("http://") || target.starts_with("https://") || target.starts_with("data:")) return false;
  std::string path(target);
  if (path.starts_with("file://")) path.erase(0, 7);
  if (path.starts_with("~/")) {
    const char* home = getenv("HOME");
    if (home) path = std::string(home) + path.substr(1);
  } else if (!path.starts_with("/") && out.charts && !out.charts->base_dir.empty()) {
    path = out.charts->base_dir + "/" + path;
  }
  struct stat st{};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  const int64_t mtime = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
  if (out.charts && out.charts->watched) out.charts->watched->emplace_back(path, mtime);
  const std::string key = "file:" + path + ":" + std::to_string(mtime) + ":" + std::to_string(st.st_size);
  const bool zoom = out.charts && out.charts->zoomed(key);
  const math::Image* im = math::picture(
      key,
      [&path](std::string& bytes) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        bytes.assign(std::istreambuf_iterator<char>(f), {});
        return !bytes.empty();
      },
      cols - 2, zoom ? out.charts->zoom_rows : rows, "![" + std::string(alt) + "](" + std::string(target) + ")", zoom);
  if (!im) return false;
  emit_image(*im, 2, out);
  return true;
}

void emit_image(const math::Image& im, uint8_t indent, Out& out) {
  for (int r = 0; r < im.rows; r++) {
    if (out.lines->size() >= out.max_lines) break;
    out.segs->push_back(image_seg(*out.scratch, im.id, r, im.cols));
    out.lines->push_back(Line{uint32_t(out.segs->size() - 1), 1, indent});
  }
}

// A display equation as image rows. False when it is not drawn as an image
// (no graphics, or too wide), and the caller shows it as Unicode instead.
bool emit_display_math(std::string_view src, int cols, uint8_t indent, Out& out) {
  if (render_settings().equations() != Equations::Typeset) return false;
  const math::Image* im = math::image(src, true, cols - indent, out.charts && out.charts->zoomed(src));
  if (!im) return false;
  emit_image(*im, indent, out);
  return true;
}

// A chart's figure as lines: text pieces in their inks, picture pieces as
// image cells. Never wrapped: a chart is laid out to the width it is given.
void emit_figure(const chart::Figure& f, int indent, Out& out) {
  static constexpr Ink kSeries[] = {Ink::Series1, Ink::Series2, Ink::Series3,
                                    Ink::Series4, Ink::Series5, Ink::Series6};
  const auto ink_of = [](uint8_t i) {
    if (i == chart::kInkAxis) return Ink::ChartAxis;
    if (i == chart::kInkTitle) return Ink::Heading;
    if (i >= chart::kInkSeries) return kSeries[(i - chart::kInkSeries) % chart::kSeriesColors];
    return Ink::Text;
  };
  for (const auto& row : f.rows) {
    if (out.lines->size() >= out.max_lines) return;
    const uint32_t first = uint32_t(out.segs->size());
    for (const chart::Piece& p : row) {
      if (p.image) {
        out.segs->push_back(image_seg(*out.scratch, p.image, p.row, p.cols));
      } else if (!p.text.empty()) {
        const Str r = out.scratch->add(p.text);
        out.segs->push_back(Seg{r.off | kScratchBit, r.len, ink_of(p.ink)});
      }
    }
    out.lines->push_back(Line{first, uint16_t(out.segs->size() - first), uint8_t(indent)});
  }
}

// A ```chart block, from the line after its fence to the closing one. Returns
// false when it is not a chart after all, and it is shown as code instead,
// with the reason under it.
bool render_chart(std::string_view body, int cols, Out& out, std::string* error) {
  if (render_settings().charts() == Charts::Source) return false;
  chart::Spec spec;
  if (!chart::parse(body, spec, error)) return false;
  const std::string base = out.charts ? out.charts->base_dir : std::string();
  if (!chart::load_files(spec, base, error, out.charts ? out.charts->watched : nullptr)) return false;
  // Labels in the terminal's font around a picture of the plot, one whole
  // picture, or all in cells: whichever the terminals attached can show.
  // Zoomed into: as tall as the pane allows, past the height it asked for.
  if (out.charts && out.charts->zoomed(body)) {
    const int tall = std::max(spec.height, out.charts->zoom_rows - (spec.title.empty() ? 3 : 5));
    spec.height = tall;
    for (auto& sub : spec.subplots) sub.height = std::max(sub.height, tall / std::max<int>(1, int(spec.subplots.size())));
  }
  chart::Figure f;
  chart::figure(spec, body, cols - 2, f);
  emit_figure(f, 2, out);
  return true;
}

// A ```mermaid block drawn in box-drawing cells. False (shown as code) when
// it is a kind not drawn, or too wide.
bool render_diagram(std::string_view body, int cols, Out& out) {
  if (render_settings().diagrams() == Diagrams::Source) return false;
  chart::Figure f;
  if (!diagram::draw(body, cols - 2, f)) return false;
  emit_figure(f, 2, out);
  return true;
}

// Splits inline markup into runs. Offsets are relative to `s` and have to be
// rebased by the caller — except a math run, which is already an absolute
// scratch-arena offset (kScratchBit set) and must be passed through as-is.
// `scratch` is null at call sites that do not want math expanded (currently
// table cells, which flatten every run back to plain text and have no use
// for one pointing somewhere else); markers are dropped everywhere else, so
// the runs concatenate into the text the reader should see.
// Splits runs at the URLs inside them. A URL in plain text becomes a link
// run; in code or emphasis it keeps its ink and gains the link.
void link_runs(std::string_view s, std::vector<Seg>& runs, size_t from, const std::string& base) {
  std::vector<LinkHit> found;
  std::vector<Seg> out;
  for (size_t k = 0; k < runs.size(); k++) {
    const Seg r = runs[k];
    if (k < from || (r.off & kScratchBit) || r.link || r.ink == Ink::Link || r.ink == Ink::MathImage) {
      out.push_back(r);
      continue;
    }
    find_links(s.substr(r.off, r.len), base, found);
    size_t at = 0;
    for (const auto& h : found) {
      const size_t a = h.begin, b = h.end;
      if (a < at) continue;
      if (a > at) out.push_back(Seg{r.off + uint32_t(at), uint32_t(a - at), r.ink, r.attr});
      out.push_back(Seg{r.off + uint32_t(a), uint32_t(b - a), r.ink == Ink::Text ? Ink::Link : r.ink, r.attr,
                        links::intern(h.target)});
      at = b;
    }
    if (at < r.len) out.push_back(Seg{r.off + uint32_t(at), uint32_t(r.len - at), r.ink, r.attr});
  }
  runs.swap(out);
}

// `max_cols` > 0 lets math that needs drawing become an image that wide at most.
void split_inline(std::string_view s, Ink base, std::vector<Seg>& out, Arena* scratch = nullptr,
                  int max_cols = 0, const std::string& link_base = std::string()) {
  const size_t first_run = out.size();
  size_t i = 0, run_start = 0;
  // Once a search for a closing marker fails, it fails from every later
  // position too, so it is not repeated. Without this each '_' in a line of
  // snake_case identifiers searched the rest of the line again.
  enum : unsigned { kDollar = 1, kParen = 2, kTick = 4, kStar = 8, kStar2 = 16,
                    kUnder = 32, kUnder2 = 64, kBracket = 128 };
  unsigned absent = 0;
  // Equations as their source: $…$ and \(…\) are text like any other.
  const bool math_source = render_settings().equations() == Equations::Source;
  const auto find = [&](std::string_view needle, size_t from, unsigned bit) {
    if (absent & bit) return std::string_view::npos;
    const size_t at = s.find(needle, from);
    if (at == std::string_view::npos) absent |= bit;
    return at;
  };
  auto flush = [&](size_t end) {
    if (end > run_start) out.push_back(Seg{uint32_t(run_start), uint32_t(end - run_start), base});
  };

  while (i < s.size()) {
    char c = s[i];

    if (scratch && !math_source && c == '$' && i + 1 < s.size() && s[i + 1] != '$') {
      // $...$ inline math. $$ is display math, handled by the caller instead.
      size_t close = find("$", i + 1, kDollar);
      if (close != std::string_view::npos && close > i + 1) {
        flush(i);
        if (const math::Image* im =
                max_cols > 0 && render_settings().equations() == Equations::Typeset ? math::image(s.substr(i + 1, close - i - 1), false, max_cols) : nullptr) {
          out.push_back(image_seg(*scratch, im->id, im->base_row, im->cols));
          i = run_start = close + 1;
          continue;
        }
        std::string rendered;
        latex::to_unicode(s.substr(i + 1, close - i - 1), rendered);
        if (!rendered.empty()) {
          Str m = scratch->add(rendered);
          out.push_back(Seg{m.off | kScratchBit, m.len, Ink::Math});
        }
        i = run_start = close + 1;
        continue;
      }
    } else if (scratch && !math_source && c == '\\' && i + 2 < s.size() && s[i + 1] == '(') {
      size_t close = find("\\)", i + 2, kParen);
      if (close != std::string_view::npos) {
        flush(i);
        if (const math::Image* im =
                max_cols > 0 && render_settings().equations() == Equations::Typeset ? math::image(s.substr(i + 2, close - i - 2), false, max_cols) : nullptr) {
          out.push_back(image_seg(*scratch, im->id, im->base_row, im->cols));
          i = run_start = close + 2;
          continue;
        }
        std::string rendered;
        latex::to_unicode(s.substr(i + 2, close - i - 2), rendered);
        if (!rendered.empty()) {
          Str m = scratch->add(rendered);
          out.push_back(Seg{m.off | kScratchBit, m.len, Ink::Math});
        }
        i = run_start = close + 2;
        continue;
      }
    } else if (c == '~' && i + 2 < s.size() && s[i + 1] == '~' && s[i + 2] != ' ') {
      // ~~struck~~
      const size_t close = s.find("~~", i + 2);
      if (close != std::string_view::npos && close > i + 2) {
        flush(i);
        out.push_back(Seg{uint32_t(i + 2), uint32_t(close - i - 2), base, kAttrStrike});
        i = run_start = close + 2;
        continue;
      }
    } else if (c == '`') {
      size_t close = find("`", i + 1, kTick);
      if (close != std::string_view::npos && close > i + 1) {
        flush(i);
        out.push_back(Seg{uint32_t(i + 1), uint32_t(close - i - 1), Ink::Code});
        i = run_start = close + 1;
        continue;
      }
    } else if ((c == '*' || c == '_') && i + 1 < s.size()) {
      const bool strong = s[i + 1] == c;
      const size_t mark = strong ? 2 : 1;
      // Underscores inside a word are the word's: snake_case_names stay whole.
      const auto word = [](char ch) { return std::isalnum(uint8_t(ch)) || uint8_t(ch) >= 0x80; };
      const bool intraword = c == '_' && i > 0 && word(s[i - 1]);
      // A marker must be followed by content, not whitespace.
      if (!intraword && i + mark < s.size() && !is_space(s[i + mark])) {
        std::string_view needle = strong ? (c == '*' ? "**" : "__") : std::string_view(&s[i], 1);
        const unsigned bit = c == '*' ? (strong ? kStar2 : kStar) : (strong ? kUnder2 : kUnder);
        size_t close = find(needle, i + mark, bit);
        while (c == '_' && close != std::string_view::npos && close + needle.size() < s.size() &&
               word(s[close + needle.size()]))
          close = s.find(needle, close + 1);
        if (close != std::string_view::npos && close > i + mark) {
          flush(i);
          out.push_back(Seg{uint32_t(i + mark), uint32_t(close - i - mark),
                            strong ? Ink::Bold : Ink::Italic});
          i = run_start = close + needle.size();
          continue;
        }
      }
    } else if (c == '[') {
      size_t close = find("]", i + 1, kBracket);
      if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(') {
        size_t paren = s.find(')', close + 2);
        // ![alt](src) in running text: a link to the image, marked as one.
        const bool image = i > run_start && s[i - 1] == '!';
        if (paren != std::string_view::npos && image && scratch) {
          flush(i - 1);
          std::string_view target = trim_left(s.substr(close + 2, paren - close - 2));
          if (const size_t sp = target.find(' '); sp != std::string_view::npos) target = target.substr(0, sp);
          const std::string label = "\xE2\x96\xA3 " + std::string(close > i + 1 ? s.substr(i + 1, close - i - 1) : "image");
          const Str m = scratch->add(label);
          out.push_back(Seg{m.off | kScratchBit, m.len, Ink::Link, 0, links::intern(url_target(target))});
          i = run_start = paren + 1;
          continue;
        }
        if (paren != std::string_view::npos) {
          flush(i);
          // Show the label, not the target: a URL inline is noise in a chat.
          // The target is what a click on the label opens.
          std::string_view target = trim_left(s.substr(close + 2, paren - close - 2));
          if (const size_t sp = target.find(' '); sp != std::string_view::npos) target = target.substr(0, sp);  // [a](url "title")
          if (target.starts_with('<') && target.ends_with('>')) target = target.substr(1, target.size() - 2);
          out.push_back(Seg{uint32_t(i + 1), uint32_t(close - i - 1), Ink::Link, 0, links::intern(url_target(target))});
          i = run_start = paren + 1;
          continue;
        }
      }
    } else if (c == '<' && (s.compare(i + 1, 4, "http") == 0 || s.compare(i + 1, 4, "www.") == 0)) {
      // <https://…>: an autolink, shown without its brackets.
      const size_t close = s.find('>', i + 1);
      if (close != std::string_view::npos && s.substr(i + 1, close - i - 1).find(' ') == std::string_view::npos) {
        flush(i);
        const std::string_view u = s.substr(i + 1, close - i - 1);
        out.push_back(Seg{uint32_t(i + 1), uint32_t(u.size()), Ink::Link, 0, links::intern(url_target(u))});
        i = run_start = close + 1;
        continue;
      }
    }
    i++;
  }
  flush(s.size());
  link_runs(s, out, first_run, link_base);
}

// "| a | b |" — a table row. Agents emit these constantly and raw pipes read
// terribly, so they get laid out as columns.
bool is_table_row(std::string_view s) {
  s = trim_left(s);
  return !s.empty() && s[0] == '|';
}

// A separator row is all dashes and colons: it marks the header above it and
// carries per-column alignment.
bool is_separator_cell(std::string_view c) {
  c = trim_left(c);
  while (!c.empty() && is_space(c.back())) c.remove_suffix(1);
  if (c.empty()) return false;
  bool dash = false;
  for (size_t i = 0; i < c.size(); i++) {
    if (c[i] == '-') { dash = true; continue; }
    if (c[i] == ':' && (i == 0 || i + 1 == c.size())) continue;
    return false;
  }
  return dash;
}

std::string_view trim(std::string_view s) {
  s = trim_left(s);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

// Appends the cells of `line` to `out`.
void split_cells(std::string_view line, std::vector<std::string_view>& out) {
  std::string_view s = trim(line);
  if (!s.empty() && s.front() == '|') s.remove_prefix(1);
  if (!s.empty() && s.back() == '|') s.remove_suffix(1);
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    // A backslash-escaped pipe is content, not a column break — it is how a
    // table cell writes a pipe at all.
    if (i < s.size() && s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') {
      i++;
      continue;
    }
    if (i == s.size() || s[i] == '|') {
      out.push_back(trim(s.substr(start, i - start)));
      start = i + 1;
    }
  }
}

}  // namespace

void picture_rows(const math::Image& im, uint8_t indent, Out& out) { emit_image(im, indent, out); }

std::string image_source(std::string_view src) {
  // A subplot's "#n": only digits after the last '#'.
  const size_t hash = src.rfind('#');
  if (hash != std::string_view::npos && hash + 1 < src.size() && hash > 0 &&
      src.find_first_not_of("0123456789", hash + 1) == std::string_view::npos)
    return std::string(src.substr(0, hash));
  return std::string(src);
}

bool image_ref(const Arena& scratch, const Seg& s, uint32_t* id, int* row, int* cols) {
  if (s.ink != Ink::MathImage || !(s.off & kScratchBit)) return false;
  const uint32_t off = s.off & ~kScratchBit;
  if (off < 8) return false;
  const std::string_view rec = scratch.view(Str{off - 8, 8});
  uint16_t r, c;
  memcpy(id, rec.data(), 4);
  memcpy(&r, rec.data() + 4, 2);
  memcpy(&c, rec.data() + 6, 2);
  *row = r;
  *cols = c;
  return true;
}

void find_urls(std::string_view s, std::vector<std::pair<size_t, size_t>>& out) {
  size_t i = 0;
  while (i < s.size()) {
    size_t at = std::string_view::npos, scheme = 0;
    for (std::string_view p : {std::string_view("https://"), std::string_view("http://"),
                               std::string_view("file://"), std::string_view("www.")}) {
      const size_t k = s.find(p, i);
      if (k < at) {
        at = k;
        scheme = p.size();
      }
    }
    if (at == std::string_view::npos) return;
    // www. only starts a word; a scheme may follow a quote or a bracket.
    const bool www = s.compare(at, 4, "www.") == 0;
    if (www && at > 0 && (std::isalnum(uint8_t(s[at - 1])) || s[at - 1] == '/' || s[at - 1] == '.')) {
      i = at + 4;
      continue;
    }
    size_t e = at + scheme;
    int depth = 0;
    while (e < s.size()) {
      const unsigned char c = uint8_t(s[e]);
      if (c <= 0x20 || c == 0x7F || c == '<' || c == '>' || c == '"' || c == '`' || c == '|') break;
      if (c == '(' || c == '[') depth++;
      if (c == ')' || c == ']') {
        if (depth == 0) break;  // the bracket around the URL, not part of it
        depth--;
      }
      e++;
    }
    // Sentence punctuation after a URL is the sentence's.
    while (e > at + scheme && std::strchr(".,;:!?'*_~", s[e - 1])) e--;
    if (e > at + scheme + (www ? 1 : 0)) out.emplace_back(at, e);
    i = std::max(e, at + scheme);
  }
}

std::string url_target(std::string_view found) {
  if (found.starts_with("www.")) return "https://" + std::string(found);
  return std::string(found);
}

// Lines still allowed under out.max_lines. Wrapping stops there: a paragraph
// in a block capped at three lines is never wrapped past its third.
static size_t room(const Out& out) {
  return out.lines->size() < out.max_lines ? out.max_lines - out.lines->size() : 0;
}

// Emits one wrapped block. `runs` hold offsets relative to `text`; they are
// concatenated into the scratch arena so wrapping sees contiguous bytes, then
// sliced back apart along the wrap points.
static void emit_block(std::string_view text, uint32_t base, bool in_scratch,
                       const std::vector<Seg>& runs, int cols, uint8_t indent, Out& out) {
  if (cols < 1) cols = 1;
  const uint32_t flag = in_scratch ? kScratchBit : 0u;

  // Fast path: one plain run means the arena text is already contiguous and can
  // be referenced directly, with no copy at all.
  if (runs.size() == 1 && runs[0].ink == Ink::Text && runs[0].attr == 0 && runs[0].link == 0) {
    std::string_view body = text.substr(runs[0].off, runs[0].len);
    text::wrap_spans(body, cols, *out.spans, room(out));
    for (const auto& sp : *out.spans) {
      if (out.lines->size() >= out.max_lines) return;
      out.segs->push_back(
          Seg{(base + runs[0].off + sp.off) | flag, sp.len, runs[0].ink});
      out.lines->push_back(Line{uint32_t(out.segs->size() - 1), 1, indent});
    }
    return;
  }

  // Styled path: markers were dropped, so the visible text is not contiguous in
  // the source. Copy the runs into scratch, wrap that, then re-split.
  const uint32_t start = out.scratch->open();
  std::vector<Seg>& flat = *out.inline_scratch;
  flat.clear();
  // Inline equations in this block: which flat run, and which image.
  std::vector<std::pair<size_t, uint32_t>> images;
  for (const auto& r : runs) {
    uint32_t at = uint32_t(out.scratch->open() - start);
    if (uint32_t id; r.ink == Ink::MathImage) {
      int row, ncols;
      if (image_ref(*out.scratch, r, &id, &row, &ncols)) images.emplace_back(flat.size(), id);
    }
    if (r.off & kScratchBit) {
      // A math run: already in scratch. Copy through a temporary rather than
      // put(view-of-self) — appending can grow and reallocate the very
      // buffer the view points into.
      std::string snippet(out.scratch->view(Str{r.off & ~kScratchBit, r.len}));
      out.scratch->put(snippet);
    } else {
      out.scratch->put(text.substr(r.off, r.len));
    }
    flat.push_back(Seg{at, r.len, r.ink, r.attr, r.link});
  }
  const Str blob = out.scratch->close(start);
  std::string_view body = out.scratch->view(blob);

  text::wrap_spans(body, cols, *out.spans, room(out));
  if (images.empty()) {
    for (const auto& sp : *out.spans) {
      if (out.lines->size() >= out.max_lines) return;
      const uint32_t line_lo = sp.off, line_hi = sp.off + sp.len;
      const uint32_t first = uint32_t(out.segs->size());
      uint16_t count = 0;
      for (const auto& f : flat) {
        const uint32_t lo = std::max(line_lo, f.off);
        const uint32_t hi = std::min(line_hi, f.off + f.len);
        if (lo >= hi) continue;
        out.segs->push_back(Seg{(blob.off + lo) | kScratchBit, hi - lo, f.ink, f.attr, f.link});
        count++;
      }
      out.lines->push_back(Line{first, count, indent});
    }
    return;
  }

  // With inline equations. An equation wraps as one word (its no-break
  // spaces never break), and a line holding a tall one grows rows above and
  // below it: the text stays on the equation's baseline row, the extra rows
  // carry only the equation's other rows, at the same columns.
  struct Placed {
    uint32_t id;
    int x, rows, base, cols;
  };
  std::vector<Placed> placed;
  for (const auto& sp : *out.spans) {
    if (out.lines->size() >= out.max_lines) return;
    const uint32_t line_lo = sp.off, line_hi = sp.off + sp.len;
    const uint32_t first = uint32_t(out.segs->size());
    uint16_t count = 0;
    placed.clear();
    int x = 0;
    for (size_t fi = 0; fi < flat.size(); fi++) {
      const Seg& f = flat[fi];
      const uint32_t lo = std::max(line_lo, f.off);
      const uint32_t hi = std::min(line_hi, f.off + f.len);
      if (lo >= hi) continue;
      Seg sg{(blob.off + lo) | kScratchBit, hi - lo, f.ink, f.attr, f.link};
      if (f.ink == Ink::MathImage) {
        const math::Image* im = nullptr;
        for (const auto& [k, id] : images)
          if (k == fi) im = math::find(id);
        if (im && lo == f.off && hi == f.off + f.len) {
          placed.push_back({im->id, x, im->rows, im->base_row, im->cols});
          sg = image_seg(*out.scratch, im->id, im->base_row, im->cols);
        } else {
          sg.ink = Ink::Text;  // split by a hard wrap: blank, not half a picture
        }
      }
      x += text::str_width(out.scratch->view(Str{sg.off & ~kScratchBit, sg.len}));
      out.segs->push_back(sg);
      count++;
    }
    if (placed.empty()) {
      out.lines->push_back(Line{first, count, indent});
      continue;
    }
    int above = 0, below = 0;
    for (const Placed& p : placed) {
      above = std::max(above, p.base);
      below = std::max(below, p.rows - p.base - 1);
    }
    const auto extra = [&](int k, uint8_t flags) {
      const uint32_t f2 = uint32_t(out.segs->size());
      uint16_t n = 0;
      int cx = 0;
      for (const Placed& p : placed) {
        const int r = p.base + k;
        if (r < 0 || r >= p.rows) continue;
        if (p.x > cx) {
          const Str gap = out.scratch->add(std::string(size_t(p.x - cx), ' '));
          out.segs->push_back(Seg{gap.off | kScratchBit, gap.len, Ink::Text});
          n++;
        }
        out.segs->push_back(image_seg(*out.scratch, p.id, r, p.cols));
        n++;
        cx = p.x + p.cols;
      }
      out.lines->push_back(Line{f2, n, indent, flags});
    };
    for (int k = -above; k < 0; k++) {
      if (out.lines->size() >= out.max_lines) return;
      extra(k, kLeadRow);
    }
    if (out.lines->size() >= out.max_lines) return;
    out.lines->push_back(Line{first, count, indent});
    for (int k = 1; k <= below; k++) {
      if (out.lines->size() >= out.max_lines) return;
      extra(k, 0);
    }
  }
}

// A cell's text as shown: inline markers dropped, escaped pipes unescaped.
static void flatten_cell(std::string_view text, std::vector<Seg>& runs, std::string& flat) {
  runs.clear();
  split_inline(text, Ink::Text, runs);
  flat.clear();
  for (const auto& r : runs) flat.append(text.substr(r.off, r.len));
  for (size_t i = 0; i + 1 < flat.size(); i++)
    if (flat[i] == '\\' && flat[i + 1] == '|') flat.erase(i, 1);
}

// A number as a table holds one: 1,024 · -3.5 · 42% · 12 ms · $9 · 3.2x.
static bool numeric(std::string_view c) {
  c = trim(c);
  if (c.empty()) return true;  // an empty cell does not decide
  if (c.front() == '-' || c.front() == '+' || c.front() == '$') c.remove_prefix(1);
  size_t i = 0;
  bool digit = false;
  while (i < c.size() && (std::isdigit(uint8_t(c[i])) || c[i] == ',' || c[i] == '.' || c[i] == '_')) {
    digit |= std::isdigit(uint8_t(c[i])) != 0;
    i++;
  }
  if (!digit) return false;
  std::string_view unit = trim(c.substr(i));
  return unit.size() <= 3 && unit.find_first_of("0123456789") == std::string_view::npos;
}

// Lays out a collected run of table rows as aligned columns. A cell too long
// for its column wraps onto more lines of its row rather than being cut.
static void emit_table(const std::vector<std::string_view>& rows, int cols, Out& out, Work& wk) {
  if (rows.empty()) return;

  // One flat array of cells, row after row, instead of a vector per row.
  std::vector<std::string_view>& cells = wk.cells;
  std::vector<uint32_t>& row_start = wk.row_start;
  cells.clear();
  row_start.clear();
  const auto ncells = [&](size_t i) { return size_t(row_start[i + 1] - row_start[i]); };
  const auto cell = [&](size_t i, size_t c) { return cells[row_start[i] + c]; };
  int sep_row = -1;
  for (size_t i = 0; i < rows.size(); i++) {
    row_start.push_back(uint32_t(cells.size()));
    split_cells(rows[i], cells);
    bool sep = cells.size() > row_start.back();
    for (size_t c = row_start.back(); c < cells.size(); c++)
      if (!is_separator_cell(cells[c])) sep = false;
    if (sep && sep_row < 0) sep_row = int(i);
  }
  row_start.push_back(uint32_t(cells.size()));
  const size_t nrows = rows.size();

  // The separator row defines the table's shape. Without that rule a stray row
  // with an extra pipe would invent a column that is empty everywhere else.
  size_t ncols = 0;
  if (sep_row >= 0) {
    ncols = ncells(size_t(sep_row));
  } else {
    for (size_t i = 0; i < nrows; i++) ncols = std::max(ncols, ncells(i));
  }
  if (ncols == 0) return;

  // The text each cell shows, flattened once.
  std::vector<std::string> flat(nrows * ncols);
  for (size_t i = 0; i < nrows; i++) {
    if (int(i) == sep_row) continue;
    for (size_t c = 0; c < ncols; c++)
      if (c < ncells(i)) flatten_cell(cell(i, c), wk.cell_runs, flat[i * ncols + c]);
  }

  std::vector<char>& align = wk.align;
  align.assign(ncols, 'l');
  std::vector<bool> chosen(ncols, false);
  if (sep_row >= 0) {
    const size_t sr = size_t(sep_row);
    for (size_t c = 0; c < ncells(sr) && c < ncols; c++) {
      std::string_view t = trim(cell(sr, c));
      const bool l = !t.empty() && t.front() == ':';
      const bool r = !t.empty() && t.back() == ':';
      align[c] = (l && r) ? 'c' : r ? 'r' : 'l';
      chosen[c] = l || r;
    }
  }
  // A column of numbers lines up on its right, unless the table says otherwise.
  for (size_t c = 0; c < ncols; c++) {
    if (chosen[c]) continue;
    bool nums = false, all = true;
    for (size_t i = size_t(std::max(0, sep_row + 1)); i < nrows; i++) {
      const std::string& t = flat[i * ncols + c];
      if (trim(t).empty()) continue;
      nums = true;
      all &= numeric(t);
    }
    if (nums && all) align[c] = 'r';
  }

  std::vector<int>& width = wk.width;
  width.assign(ncols, 0);
  for (size_t i = 0; i < nrows; i++) {
    if (int(i) == sep_row) continue;
    for (size_t c = 0; c < ncols; c++) width[c] = std::max(width[c], text::str_width(flat[i * ncols + c]));
  }

  // " cell │ cell │ cell" — one space of padding each side of every divider.
  const int chrome = int(ncols - 1) * 3 + 2;
  int total = chrome;
  for (int w : width) total += w;
  // Narrow the widest column until it fits: its cells wrap.
  while (total > cols) {
    size_t widest = 0;
    for (size_t c = 1; c < ncols; c++)
      if (width[c] > width[widest]) widest = c;
    if (width[widest] <= 6) break;
    width[widest]--;
    total--;
  }

  std::vector<std::vector<text::Span>> wrapped(ncols);
  for (size_t i = 0; i < nrows; i++) {
    if (out.lines->size() >= out.max_lines) return;
    const Ink row_ink = (sep_row > 0 && int(i) < sep_row) ? Ink::Heading : Ink::Text;

    if (int(i) == sep_row) {
      // Each junction must land exactly on the divider above and below it.
      // A content row spends one space before a cell and one after, so every
      // column's rule is its width plus two — except the last, which has no
      // trailing space to cover.
      const uint32_t off = out.scratch->open();
      for (size_t c = 0; c < ncols; c++) {
        if (c) out.scratch->put("\xE2\x94\xBC");  // U+253C
        for (int k = 0; k < width[c] + (c + 1 == ncols ? 1 : 2); k++)
          out.scratch->put("\xE2\x94\x80");       // U+2500
      }
      const Str line = out.scratch->close(off);
      out.segs->push_back(Seg{line.off | kScratchBit, line.len, Ink::Rule});
      out.lines->push_back(Line{uint32_t(out.segs->size() - 1), 1, 0});
      continue;
    }

    size_t height = 1;
    for (size_t c = 0; c < ncols; c++) {
      text::wrap_spans(flat[i * ncols + c], std::max(1, width[c]), wrapped[c], 64);
      height = std::max(height, wrapped[c].size());
    }
    for (size_t k = 0; k < height; k++) {
      if (out.lines->size() >= out.max_lines) return;
      const uint32_t first_seg = uint32_t(out.segs->size());
      uint16_t nseg = 0;
      // Dividers get their own segments so they can be drawn in the rule's
      // colour instead of shouting as loudly as the data.
      for (size_t c = 0; c < ncols; c++) {
        const Str d = out.scratch->add(c ? " \xE2\x94\x82 " : " ");  // U+2502
        out.segs->push_back(Seg{d.off | kScratchBit, d.len, c ? Ink::Rule : row_ink});
        nseg++;
        const std::string& f = flat[i * ncols + c];
        const std::string_view part = k < wrapped[c].size() ? std::string_view(f).substr(wrapped[c][k].off, wrapped[c][k].len)
                                                            : std::string_view();
        const int w = text::str_width(part);
        const int pad = std::max(0, width[c] - w);
        const int left = align[c] == 'r' ? pad : align[c] == 'c' ? pad / 2 : 0;
        // The final column needs no trailing padding: it is dead space at
        // the end of the line, and it makes empty cells look like a column.
        const int right = c + 1 < ncols ? pad - left : 0;
        const std::string padded = std::string(size_t(left), ' ') + std::string(part) + std::string(size_t(right), ' ');
        if (!padded.empty()) {
          const Str cs = out.scratch->add(padded);
          out.segs->push_back(Seg{cs.off | kScratchBit, cs.len, row_ink});
          nseg++;
        }
      }
      // Trim dead space off the end of the row.
      while (nseg > 0) {
        Seg& last = (*out.segs)[first_seg + nseg - 1];
        std::string_view v = out.scratch->view(Str{last.off & ~kScratchBit, last.len});
        size_t keep = v.size();
        while (keep > 0 && v[keep - 1] == ' ') keep--;
        last.len = uint32_t(keep);
        if (keep) break;
        out.segs->pop_back();
        nseg--;
      }
      out.lines->push_back(Line{first_seg, nseg, 0});
    }
  }
}

// A line that is nothing but $$...$$ or \[...\] — display math written on one
// line, the common case for a short equation.
bool display_math_line(std::string_view line, std::string_view* inner) {
  std::string_view t = trim(line);
  if (t.size() >= 4 && t.compare(0, 2, "$$") == 0 && t.compare(t.size() - 2, 2, "$$") == 0) {
    *inner = t.substr(2, t.size() - 4);
    return true;
  }
  if (t.size() >= 4 && t.compare(0, 2, "\\[") == 0 && t.compare(t.size() - 2, 2, "\\]") == 0) {
    *inner = t.substr(2, t.size() - 4);
    return true;
  }
  return false;
}

// Puts a segment at the front of lines [from, end) — a list item's marker on
// its first line, a quote's bar on every line — moving later lines' segments
// along with it.
static void hang(Out& out, size_t from, size_t count, Seg seg, uint8_t indent) {
  const size_t end = std::min(out.lines->size(), from + count);
  for (size_t li = from; li < end; li++) {
    Line& L = (*out.lines)[li];
    const uint32_t pivot = L.seg_first;
    out.segs->push_back(seg);
    std::rotate(out.segs->begin() + pivot, out.segs->end() - 1, out.segs->end());
    L.seg_count++;
    L.indent = indent;
    for (size_t lj = 0; lj < out.lines->size(); lj++)
      if (lj != li && (*out.lines)[lj].seg_first >= pivot) (*out.lines)[lj].seg_first++;
  }
}

// One line of a fenced code block: tabs expanded, coloured, and wrapped at
// the code's own width — on a space when one is near the end, else where it
// must — with each continuation set under the line's indentation and marked
// in the padding column. `label`, the block's language, goes at the right of
// its first line when there is room.
static void emit_code(std::string_view text, uint32_t base, bool in_scratch, std::string_view line,
                      uint32_t line_off, const code::Lang* lang, code::State& st,
                      std::string_view label, int cols, Out& out, Work& wk) {
  constexpr uint8_t kIndent = 2;
  const int width = std::max(8, cols - kIndent - 1);  // one column of padding inside
  // Tabs to four-column stops: a tab is no width at all to the terminal.
  std::string_view src = line;
  uint32_t src_off = (base + line_off) | (in_scratch ? kScratchBit : 0u);
  if (line.find('\t') != std::string_view::npos) {
    std::string& exp = wk.code_line;
    exp.clear();
    int col = 0;
    for (size_t i = 0; i < line.size();) {
      if (line[i] == '\t') {
        const int to = (col / 4 + 1) * 4;
        exp.append(size_t(to - col), ' ');
        col = to;
        i++;
        continue;
      }
      const size_t at = i;
      int cw;
      i = text::glyph_end(line, i, &cw);
      exp.append(line, at, i - at);
      col += std::max(0, cw);
    }
    src_off = out.scratch->add(exp).off | kScratchBit;
    src = exp;
  }
  code::highlight(src, lang, st, wk.code_runs);
  // URLs and file paths in the code: their runs keep their colour and gain the link.
  std::vector<LinkHit> found;
  find_links(src, out.charts ? out.charts->base_dir : std::string(), found);
  std::vector<std::pair<size_t, size_t>> urls;
  std::vector<uint16_t> url_ids;
  for (const auto& h : found) {
    urls.emplace_back(h.begin, h.end);
    url_ids.push_back(links::intern(h.target));
  }

  const auto ink_of = [](code::Tok t) {
    switch (t) {
      case code::Tok::Keyword: return Ink::CodeKeyword;
      case code::Tok::String: return Ink::CodeString;
      case code::Tok::Comment: return Ink::CodeComment;
      case code::Tok::Number: return Ink::CodeNumber;
      case code::Tok::Type: return Ink::CodeType;
      case code::Tok::Func: return Ink::CodeFunc;
      case code::Tok::Added: return Ink::Added;
      case code::Tok::Removed: return Ink::Removed;
      case code::Tok::Hunk: return Ink::Hunk;
      default: return Ink::CodeText;
    }
  };
  const auto mark = [&](std::string_view t, Ink ink) {
    const Str r = out.scratch->add(t);
    out.segs->push_back(Seg{r.off | kScratchBit, r.len, ink});
  };

  // Where the rows break.
  const size_t lead = src.find_first_not_of(' ') == std::string_view::npos ? 0 : src.find_first_not_of(' ');
  const int hang = int(std::min<size_t>(lead, size_t(width / 2)));  // continuations' indent
  std::vector<std::pair<size_t, size_t>> rows;
  size_t start = 0;
  int w = 0, limit = width;
  size_t space = std::string_view::npos;
  int space_w = 0;
  for (size_t i = 0; i < src.size();) {
    const size_t at = i;
    const char32_t cp = uint8_t(src[at]) < 0x80 ? char32_t(src[at]) : U'\uFFFD';  // only ASCII is tested
    int cw;
    i = text::glyph_end(src, i, &cw);
    cw = std::max(0, cw);
    if (w + cw > limit && at > start) {
      size_t cut = at;
      if (space != std::string_view::npos && space > start && space_w * 20 >= limit * 7) cut = space;
      rows.emplace_back(start, cut);
      start = cut;
      limit = width - hang;
      // The rest of the line starts the next row, measured again from there.
      w = 0;
      space = std::string_view::npos;
      i = start;
      continue;
    }
    w += cw;
    // A break at a space, or after a comma or an opening bracket, keeps a
    // token whole. A space goes to the start of the next row, so copying the
    // rows back together loses nothing.
    if (cp == U' ' && at > lead) {
      space = at;
      space_w = w - cw;
    } else if ((cp == U',' || cp == U'(' || cp == U'[' || cp == U'{') && at > lead) {
      space = i;
      space_w = w;
    }
  }
  rows.emplace_back(start, src.size());

  for (size_t r = 0; r < rows.size(); r++) {
    if (out.lines->size() >= out.max_lines) return;
    const uint32_t first = uint32_t(out.segs->size());
    if (r == 0) {
      mark(" ", Ink::CodeMark);
    } else {
      mark("\xE2\x86\xAA", Ink::CodeMark);  // ↪
      if (hang) mark(std::string(size_t(hang), ' '), Ink::CodeMark);
    }
    const auto [lo, hi] = rows[r];
    int used = 0;
    for (const code::Run& run : wk.code_runs) {
      size_t a = std::max<size_t>(lo, run.off);
      const size_t b = std::min<size_t>(hi, run.off + run.len);
      if (a >= b) continue;
      used += text::str_width(src.substr(a, b - a));
      // Cut at URL edges, so a link's cells carry it and nothing else does.
      for (size_t u = 0; u < urls.size() && a < b; u++) {
        const auto [ua, ub] = urls[u];
        if (ub <= a || ua >= b) continue;
        if (ua > a) out.segs->push_back(Seg{src_off + uint32_t(a), uint32_t(ua - a), ink_of(run.tok)});
        const size_t e = std::min(b, ub);
        out.segs->push_back(Seg{src_off + uint32_t(std::max(a, ua)), uint32_t(e - std::max(a, ua)),
                                ink_of(run.tok), 0, url_ids[u]});
        a = e;
      }
      if (a < b) out.segs->push_back(Seg{src_off + uint32_t(a), uint32_t(b - a), ink_of(run.tok)});
    }
    if (r == 0 && !label.empty()) {
      const int lw = text::str_width(label);
      if (used + lw + 3 <= width) {
        mark(std::string(size_t(width - used - lw), ' '), Ink::CodeText);
        mark(label, Ink::CodeMark);
      }
    }
    out.lines->push_back(Line{first, uint16_t(out.segs->size() - first), kIndent, kCodeRow});
  }
}

// A display block opened on `line` and closed on a later one:
// $$ … $$, \[ … \], or a bare \begin{align} … \end{align}. Sets the math
// inside (environments keep their \begin and \end, which the parser reads)
// and where the text resumes. False when the line opens no block, or the
// block has not closed yet.
bool display_block(std::string_view text, std::string_view line, std::string_view* body,
                   size_t* after) {
  const std::string_view t = trim(line);
  const size_t lead = size_t(t.data() - text.data());
  size_t body_at = 0;
  std::string closer;
  bool keep_closer = false;
  if (t.starts_with("$$") && !(t.size() >= 4 && t.ends_with("$$"))) {
    body_at = lead + 2;
    closer = "$$";
  } else if (t.starts_with("\\[") && !(t.size() >= 4 && t.ends_with("\\]"))) {
    body_at = lead + 2;
    closer = "\\]";
  } else if (t.starts_with("\\begin{")) {
    const size_t e = t.find('}');
    if (e == std::string_view::npos) return false;
    const std::string_view env = t.substr(7, e - 7);
    static constexpr std::string_view kEnvs[] = {
        "equation", "equation*", "align", "align*", "aligned", "gather", "gather*",
        "multline", "multline*", "eqnarray", "eqnarray*", "displaymath", "split",
        "alignat", "alignat*", "flalign", "flalign*", "gathered"};
    bool known = false;
    for (std::string_view k : kEnvs) known |= env == k;
    if (!known) return false;
    body_at = lead;
    closer = "\\end{" + std::string(env) + "}";
    keep_closer = true;
  } else {
    return false;
  }
  // Bounded: an opener that never closes must not scan a whole transcript.
  const size_t limit = std::min(text.size(), body_at + (16u << 10));
  const size_t at = text.substr(0, limit).find(closer, body_at);
  if (at == std::string_view::npos) return false;
  const size_t end = keep_closer ? at + closer.size() : at;
  *body = text.substr(body_at, end - body_at);
  const size_t nl = text.find('\n', at + closer.size());
  *after = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
  return true;
}

void render(std::string_view text, uint32_t base, bool in_scratch, int cols, Out& out) {
  Work own;
  Work& wk = out.work ? *out.work : own;
  std::vector<Seg>& runs = wk.runs;
  bool in_fence = false;
  bool in_mathblock = false;  // between a line that is just "$$" and its close
  // Equations as their source: $$ blocks are text like any other.
  const bool math_source = render_settings().equations() == Equations::Source;
  const std::string link_base = out.charts ? out.charts->base_dir : std::string();  // where paths are found
  int item_indent = -1;   // the open list item's text column, for its continuation lines
  Ink callout = Ink::Rule;  // the open quote's bar: a callout's colour, else the rule's
  bool in_quote = false;
  const code::Lang* fence_lang_ = nullptr;  // the open fence's language
  std::string_view fence_label;             // and its name as written, for its first line
  code::State code_state;

  size_t pos = 0;
  while (pos <= text.size()) {
    if (out.lines->size() >= out.max_lines) return;
    // A trailing newline ends the last line; it does not start an empty one.
    if (pos == text.size() && pos > 0 && text[pos - 1] == '\n') break;
    size_t nl = text.find('\n', pos);
    size_t end = nl == std::string_view::npos ? text.size() : nl;
    if (end > pos && text[end - 1] == '\r') end--;
    std::string_view line = text.substr(pos, end - pos);
    const uint32_t line_off = uint32_t(pos);

    // A line that is only an image: the picture itself, when it can be drawn.
    if (!in_fence && !in_mathblock && math::config().enabled) {
      const std::string_view t = trim(line);
      if (t.starts_with("![") && t.ends_with(")")) {
        const size_t mid = t.find("](");
        if (mid != std::string_view::npos && t.find("](", mid + 2) == std::string_view::npos) {
          std::string_view target = t.substr(mid + 2, t.size() - mid - 3);
          if (const size_t sp = target.find(' '); sp != std::string_view::npos) target = target.substr(0, sp);
          if (emit_picture_line(t.substr(2, mid - 2), target, cols, out)) {
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
            continue;
          }
        }
      }
    }
    if (!in_fence && fence_at(line) && (fence_lang(line) == "chart" || fence_lang(line) == "mermaid")) {
      const bool mermaid = fence_lang(line) == "mermaid";
      // Find the closing fence; the block between is the chart's spec.
      const size_t body_at = nl == std::string_view::npos ? text.size() : nl + 1;
      size_t scan = body_at, close_at = std::string_view::npos, after = text.size() + 1;
      while (scan < text.size()) {
        size_t n2 = text.find('\n', scan);
        const size_t e2 = n2 == std::string_view::npos ? text.size() : n2;
        if (fence_at(text.substr(scan, e2 - scan))) {
          close_at = scan;
          after = n2 == std::string_view::npos ? text.size() + 1 : n2 + 1;
          break;
        }
        scan = n2 == std::string_view::npos ? text.size() : n2 + 1;
      }
      // Still streaming in, no closing fence yet: shown as code until it is.
      std::string error;
      if (close_at != std::string_view::npos &&
          (mermaid ? render_diagram(text.substr(body_at, close_at - body_at), cols, out)
                   : render_chart(text.substr(body_at, close_at - body_at), cols, out, &error))) {
        pos = after;
        continue;
      }
      if (!error.empty()) {
        const Str r = out.scratch->add("chart: " + error);
        out.segs->push_back(Seg{r.off | kScratchBit, r.len, Ink::ChartAxis});
        out.lines->push_back(Line{uint32_t(out.segs->size() - 1), 1, 2});
      }
    }
    if (fence_at(line)) {
      in_fence = !in_fence;
      if (in_fence) {
        const std::string_view info = fence_lang(line);
        fence_lang_ = code::lang_of(info);
        fence_label = info.substr(0, std::min(info.find(' '), info.size()));
        code_state = {};
      }
      // The fence itself is not shown; the indent and colour say "code".
      pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
      continue;
    }
    // A display block over several lines, drawn whole once it has closed.
    // Until then (still streaming) it is shown line by line as Unicode.
    if (!in_fence && !in_mathblock && !math_source && math::config().enabled) {
      std::string_view body;
      size_t after = 0;
      if (display_block(text, line, &body, &after) && emit_display_math(body, cols, 2, out)) {
        pos = after;
        continue;
      }
    }
    if (!in_fence && !math_source && trim(line) == "$$") {
      in_mathblock = !in_mathblock;
      pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
      continue;
    }

    runs.clear();
    uint8_t indent = 0;

    if (in_mathblock) {
      // A multi-line equation block: each line is raw LaTeX, not markdown —
      // no wrapping on words, no inline markup pass.
      std::string rendered;
      latex::to_unicode(line, rendered);
      const Str r = out.scratch->add(rendered);
      runs.push_back(Seg{r.off | kScratchBit, r.len, Ink::Math});
      indent = 2;
      emit_block(text, base, in_scratch, runs, cols - indent, indent, out);
    } else if (std::string_view inner; !math_source && display_math_line(line, &inner)) {
      if (emit_display_math(inner, cols, 2, out)) {
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
        continue;
      }
      std::string rendered;
      latex::to_unicode(inner, rendered);
      const Str r = out.scratch->add(rendered);
      runs.push_back(Seg{r.off | kScratchBit, r.len, Ink::Math});
      indent = 2;
      emit_block(text, base, in_scratch, runs, cols - indent, indent, out);
    } else if (in_fence) {
      // Code is never word-wrapped or inline-parsed; it is shown as written,
      // coloured, on its own background.
      emit_code(text, base, in_scratch, line, line_off, fence_lang_, code_state, fence_label, cols, out, wk);
      fence_label = {};
    } else if (is_table_row(line)) {
      // Consume the whole run of table rows, then lay them out together: the
      // column widths depend on every row.
      std::vector<std::string_view>& lines = wk.rows;
      lines.clear();
      size_t scan = pos;
      while (scan <= text.size()) {
        size_t n2 = text.find('\n', scan);
        size_t e2 = n2 == std::string_view::npos ? text.size() : n2;
        if (e2 > scan && text[e2 - 1] == '\r') e2--;
        std::string_view l2 = text.substr(scan, e2 - scan);
        if (!is_table_row(l2)) break;
        lines.push_back(l2);
        if (n2 == std::string_view::npos) { scan = text.size() + 1; break; }
        scan = n2 + 1;
      }
      emit_table(lines, cols, out, wk);
      pos = scan;
      continue;
    } else if (line.empty()) {
      in_quote = false;  // the next > starts a quote of its own
      out.lines->push_back(Line{uint32_t(out.segs->size()), 0, 0});
    } else if (is_rule(line)) {
      const uint32_t at = out.scratch->open();
      for (int i = 0; i < std::min(cols, 40); i++) out.scratch->put("\xE2\x94\x80");
      const Str r = out.scratch->close(at);
      out.segs->push_back(Seg{r.off | kScratchBit, r.len, Ink::Rule});
      out.lines->push_back(Line{uint32_t(out.segs->size() - 1), 1, 0});
    } else if (int h = heading_level(line); h > 0) {
      std::string_view body = trim_left(line.substr(size_t(h)));
      const uint32_t off = uint32_t(body.data() - text.data());
      split_inline(body, Ink::Heading, runs, out.scratch, cols, link_base);
      for (auto& r : runs) {
        if (r.off & kScratchBit) continue;  // a math run: already absolute
        r.off += off;
        if (r.ink == Ink::Text) r.ink = Ink::Heading;
      }
      emit_block(text, base, in_scratch, runs, cols, 0, out);
    } else if (line[0] == '>') {
      std::string_view body = trim_left(line.substr(1));
      if (!in_quote) callout = Ink::Rule;
      in_quote = true;
      item_indent = -1;
      // A GitHub callout names its kind on its first line: > [!WARNING].
      static constexpr struct {
        std::string_view tag, label;
        Ink ink;
      } kCallouts[] = {{"[!NOTE]", "\xE2\x84\xB9 Note", Ink::Link},
                       {"[!TIP]", "\xE2\x9C\x93 Tip", Ink::Added},
                       {"[!IMPORTANT]", "\xE2\x9D\xA2 Important", Ink::Hunk},
                       {"[!WARNING]", "\xE2\x96\xB2 Warning", Ink::Code},
                       {"[!CAUTION]", "\xE2\x9C\x96 Caution", Ink::Removed}};
      bool header = false;
      for (const auto& k : kCallouts) {
        if (!trim(body).starts_with(k.tag)) continue;
        callout = k.ink;
        const Str bar = out.scratch->add(kQuoteBar);
        const Str lab = out.scratch->add(k.label);
        out.segs->push_back(Seg{bar.off | kScratchBit, bar.len, k.ink});
        out.segs->push_back(Seg{lab.off | kScratchBit, lab.len, k.ink, kAttrBold});
        out.lines->push_back(Line{uint32_t(out.segs->size() - 2), 2, 2});
        body = trim_left(trim(body).substr(k.tag.size()));
        header = true;
        break;
      }
      if (header && body.empty()) {
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
        continue;
      }
      const uint32_t off = uint32_t(body.data() - text.data());
      split_inline(body, Ink::Quote, runs, out.scratch, cols - 4, link_base);
      for (auto& r : runs) {
        if (r.off & kScratchBit) continue;  // a math run: already absolute
        // A callout's text is plain; a quote's is the quote's.
        if (r.ink == Ink::Text || r.ink == Ink::Quote) r.ink = callout == Ink::Rule ? Ink::Quote : Ink::Text;
        r.off += off;
      }
      const size_t before = out.lines->size();
      if (body.empty()) out.lines->push_back(Line{uint32_t(out.segs->size()), 0, 4});
      else emit_block(text, base, in_scratch, runs, cols - 4, 4, out);
      // A bar down the quote's left edge, in the callout's colour.
      const Str bar = out.scratch->add(kQuoteBar);
      hang(out, before, out.lines->size() - before, Seg{bar.off | kScratchBit, bar.len, callout}, 2);
    } else if (bool ordered = false; size_t mark = list_marker(line, &ordered)) {
      in_quote = false;
      // Nesting from the indentation; a marker that says what kind of item.
      const size_t lead = line.find_first_not_of(' ');
      const int level = std::min(5, int(lead == std::string_view::npos ? 0 : lead / 2));
      std::string_view body = line.substr(mark);
      std::string marker;
      bool done = false;
      if (!ordered && (body.starts_with("[ ] ") || body == "[ ]")) {
        marker = "\xE2\x98\x90 ";  // ☐
        body.remove_prefix(std::min<size_t>(4, body.size()));
      } else if (!ordered && (body.starts_with("[x] ") || body.starts_with("[X] ") || body == "[x]")) {
        marker = "\xE2\x98\x91 ";  // ☑
        done = true;
        body.remove_prefix(std::min<size_t>(4, body.size()));
      } else if (ordered) {
        marker = std::string(trim(line.substr(0, mark))) + " ";  // "1." kept: it says which step
      } else {
        static constexpr std::string_view kBullets[] = {"\xE2\x80\xA2 ", "\xE2\x97\xA6 ", "\xE2\x96\xAA "};
        marker = std::string(kBullets[level % 3]);
      }
      const int mw = text::str_width(marker);
      const uint8_t at_indent = uint8_t(2 + 2 * level);
      const uint32_t off = uint32_t(body.data() - text.data());
      split_inline(body, Ink::Text, runs, out.scratch, cols - at_indent - mw, link_base);
      for (auto& r : runs) {
        if (!(r.off & kScratchBit)) r.off += off;
        if (done) r.attr |= kAttrDim | kAttrStrike;  // a finished task reads as finished
      }
      const size_t before = out.lines->size();
      emit_block(text, base, in_scratch, runs, cols - at_indent - mw, uint8_t(at_indent + mw), out);
      // The marker hangs off the item's first line of text: rows an inline
      // equation added above it come first but carry no text.
      size_t head = before;
      while (head < out.lines->size() && ((*out.lines)[head].flags & kLeadRow)) head++;
      const Str m = out.scratch->add(marker);
      hang(out, head, 1, Seg{m.off | kScratchBit, m.len, Ink::Bullet}, at_indent);
      item_indent = at_indent + mw;
    } else if (item_indent >= 0 && line.size() > 2 && line[0] == ' ' && line[1] == ' ' &&
               trim(line).size() > 0) {
      // An item's text going on, under the item rather than at the margin.
      const std::string_view body = trim_left(line);
      const uint32_t off = uint32_t(body.data() - text.data());
      split_inline(body, Ink::Text, runs, out.scratch, cols - item_indent, link_base);
      for (auto& r : runs) if (!(r.off & kScratchBit)) r.off += off;
      emit_block(text, base, in_scratch, runs, cols - item_indent, uint8_t(item_indent), out);
    } else if (const std::string_view t = trim(line);
               t == "<details>" || t == "<details open>" || t == "</details>" || t == "</summary>") {
      // <details> folds nothing here: its summary is shown as a heading.
    } else if (t.starts_with("<summary>")) {
      std::string_view label = t.substr(9);
      if (const size_t e = label.find("</summary>"); e != std::string_view::npos) label = label.substr(0, e);
      const Str m = out.scratch->add("\xE2\x96\xBE " + std::string(label));
      out.segs->push_back(Seg{m.off | kScratchBit, m.len, Ink::Heading});
      out.lines->push_back(Line{uint32_t(out.segs->size() - 1), 1, 0});
    } else {
      if (line[0] != ' ') item_indent = -1;
      in_quote = false;
      split_inline(line, Ink::Text, runs, out.scratch, cols, link_base);
      for (auto& r : runs) if (!(r.off & kScratchBit)) r.off += line_off;
      emit_block(text, base, in_scratch, runs, cols, 0, out);
    }

    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
}

namespace {

// Whether a path names a file, remembered for a few seconds: layout asks again
// on every relayout, and a file an agent just wrote should still turn up.
bool is_file(const std::string& path) {
  struct Seen {
    bool file;
    int64_t at;
  };
  static std::unordered_map<std::string, Seen> seen;
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const int64_t now = int64_t(ts.tv_sec);
  if (auto it = seen.find(path); it != seen.end() && now - it->second.at < 5) return it->second.file;
  if (seen.size() > 20000) seen.clear();
  struct stat st{};
  const bool file = stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
  seen[path] = Seen{file, now};
  return file;
}

bool path_char(char c) {
  return std::isalnum(uint8_t(c)) || c == '_' || c == '.' || c == '/' || c == '-' || c == '~' || c == '+' ||
         c == '@' || c == '%';
}

// File paths in `s` outside the ranges already taken (URLs).
void find_paths(std::string_view s, const std::string& base, const std::vector<LinkHit>& taken,
                std::vector<LinkHit>& out) {
  size_t i = 0;
  while (i < s.size()) {
    if (!path_char(s[i]) || (i > 0 && path_char(s[i - 1]))) { i++; continue; }
    size_t e = i;
    while (e < s.size() && path_char(s[e])) e++;
    size_t pend = e;
    // Sentence punctuation is not the path's.
    while (pend > i && (s[pend - 1] == '.' || s[pend - 1] == '-')) pend--;
    std::string_view p = s.substr(i, pend - i);
    // :line, :line:col, #Lline after it.
    int line = 0;
    size_t end = pend;
    if (pend < s.size() && s[pend] == ':' && pend + 1 < s.size() && std::isdigit(uint8_t(s[pend + 1]))) {
      size_t k = pend + 1;
      while (k < s.size() && std::isdigit(uint8_t(s[k]))) k++;
      line = std::atoi(std::string(s.substr(pend + 1, k - pend - 1)).c_str());
      end = k;
      if (k + 1 < s.size() && s[k] == ':' && std::isdigit(uint8_t(s[k + 1]))) {
        k++;
        while (k < s.size() && std::isdigit(uint8_t(s[k]))) k++;
        end = k;
      }
    } else if (s.compare(pend, 2, "#L") == 0) {
      size_t k = pend + 2;
      while (k < s.size() && std::isdigit(uint8_t(s[k]))) k++;
      if (k > pend + 2) {
        line = std::atoi(std::string(s.substr(pend + 2, k - pend - 2)).c_str());
        end = k;
      }
    }
    i = std::max(e, end);
    // Something that could be a path: a folder in it, or a name with an
    // extension; not a version number, not a URL's remains.
    const size_t dot = p.rfind('.');
    const bool has_dir = p.find('/') != std::string_view::npos;
    const bool has_ext = dot != std::string_view::npos && dot > 0 && dot + 1 < p.size() &&
                         std::isalpha(uint8_t(p[dot + 1])) && p.size() - dot <= 11;
    if (p.size() < 3 || (!has_dir && !has_ext) || p.find("//") != std::string_view::npos) continue;
    const size_t begin = size_t(p.data() - s.data());
    bool overlaps = false;
    for (const auto& t : taken) overlaps |= t.begin < end && begin < t.end;  // inside a URL
    if (overlaps) continue;
    std::string abs(p);
    if (abs.starts_with("~/")) {
      const char* home = getenv("HOME");
      if (!home) continue;
      abs = std::string(home) + abs.substr(1);
    } else if (!abs.starts_with("/")) {
      if (base.empty()) continue;
      abs = base + "/" + abs;
    }
    if (!is_file(abs)) continue;
    out.push_back(LinkHit{begin, end, "file://" + abs + (line > 0 ? "#L" + std::to_string(line) : "")});
  }
}

// The xterm palette's colours 16 to 255: a 6×6×6 cube, then 24 greys.
Color xterm256(int n) {
  if (n >= 232) {
    const int v = 8 + (n - 232) * 10;
    return Color(v << 16 | v << 8 | v);
  }
  n -= 16;
  static constexpr int kLevel[6] = {0, 95, 135, 175, 215, 255};
  return Color(kLevel[n / 36] << 16 | kLevel[(n / 6) % 6] << 8 | kLevel[n % 6]);
}

// Output with escape sequences: their text, and its colour runs. Only SGR
// survives, as colour; every other sequence goes. The sixteen colours of a
// terminal palette are the theme's; any other colour, and a background, is
// kept exactly, as a paint. A carriage return within a line starts it again,
// the way a progress bar redraws itself.
void strip_ansi(std::string_view in, std::string& text, std::vector<Seg>& runs) {
  text.clear();
  runs.clear();
  int fg = -1;                  // a palette colour, drawn as its Ink
  Color exact = kDefaultColor;  // or a colour of its own
  Color bg = kDefaultColor;
  uint8_t attr = 0;
  size_t line_start = 0;
  uint16_t pid = 0;
  const auto repaint = [&] {
    pid = exact == kDefaultColor && bg == kDefaultColor ? 0 : paint_id(Paint{exact, bg});
  };
  const auto ink = [&] { return fg < 0 ? Ink::Text : Ink(int(Ink::Ansi0) + fg); };
  const auto mark = [&] {
    if (!runs.empty() && runs.back().off + runs.back().len == text.size() && runs.back().ink == ink() &&
        runs.back().attr == attr && runs.back().paint == pid)
      return;
    runs.push_back(Seg{uint32_t(text.size()), 0, ink(), attr, 0, pid});
  };
  mark();
  for (size_t i = 0; i < in.size();) {
    const char c = in[i];
    if (c == '\x1b' && i + 1 < in.size()) {
      if (in[i + 1] == '[') {
        size_t k = i + 2;
        while (k < in.size() && (std::isdigit(uint8_t(in[k])) || in[k] == ';' || in[k] == '?' || in[k] == ':')) k++;
        if (k < in.size() && in[k] == 'm') {
          std::vector<int> p;
          int cur = -1;
          for (size_t j = i + 2; j < k; j++) {
            if (std::isdigit(uint8_t(in[j]))) cur = (cur < 0 ? 0 : cur * 10) + (in[j] - '0');
            else { p.push_back(cur < 0 ? 0 : cur); cur = -1; }
          }
          p.push_back(cur < 0 ? 0 : cur);
          for (size_t j = 0; j < p.size(); j++) {
            const int v = p[j];
            if (v == 0) { fg = -1; exact = bg = kDefaultColor; attr = 0; }
            else if (v == 1) attr |= kAttrBold;
            else if (v == 2) attr |= kAttrDim;
            else if (v == 3) attr |= kAttrItalic;
            else if (v == 4) attr |= kAttrUnderline;
            else if (v == 9) attr |= kAttrStrike;
            else if (v == 22) attr &= uint8_t(~(kAttrBold | kAttrDim));
            else if (v == 23) attr &= uint8_t(~kAttrItalic);
            else if (v == 24) attr &= uint8_t(~kAttrUnderline);
            else if (v == 29) attr &= uint8_t(~kAttrStrike);
            else if (v >= 30 && v <= 37) { fg = v - 30; exact = kDefaultColor; }
            else if (v >= 90 && v <= 97) { fg = v - 90 + 8; exact = kDefaultColor; }
            else if (v == 39) { fg = -1; exact = kDefaultColor; }
            else if (v >= 40 && v <= 47) bg = palette(v - 40);
            else if (v >= 100 && v <= 107) bg = palette(v - 100 + 8);
            else if (v == 49) bg = kDefaultColor;
            else if ((v == 38 || v == 48) && j + 1 < p.size()) {
              // 5;n picks from the 256 (its first sixteen are the palette's);
              // 2;r;g;b says the colour outright.
              Color to = kDefaultColor;
              int pal = -1;
              if (p[j + 1] == 5 && j + 2 < p.size()) {
                const int n = std::clamp(p[j + 2], 0, 255);
                if (n < 16) pal = n;
                else to = xterm256(n);
                j += 2;
              } else if (p[j + 1] == 2 && j + 4 < p.size()) {
                to = Color(std::clamp(p[j + 2], 0, 255) << 16 | std::clamp(p[j + 3], 0, 255) << 8 |
                           std::clamp(p[j + 4], 0, 255));
                j += 4;
              } else {
                continue;
              }
              if (v == 38) {
                fg = pal;
                exact = to;
              } else {
                bg = pal >= 0 ? palette(pal) : to;
              }
            }
          }
          repaint();
          mark();
        }
        i = k + 1;
        continue;
      }
      if (in[i + 1] == ']') {  // OSC, to BEL or ST
        size_t k = i + 2;
        while (k < in.size() && in[k] != '\x07' && !(in[k] == '\x1b' && k + 1 < in.size() && in[k + 1] == '\\')) k++;
        i = k < in.size() && in[k] == '\x07' ? k + 1 : k + 2;
        continue;
      }
      i += 2;
      continue;
    }
    if (c == '\r') {
      if (i + 1 < in.size() && in[i + 1] == '\n') { i++; continue; }
      // Back to the line's start: what follows overwrites it.
      text.resize(line_start);
      while (!runs.empty() && runs.back().off >= text.size()) runs.pop_back();
      if (!runs.empty()) runs.back().len = uint32_t(std::min<size_t>(runs.back().len, text.size() - runs.back().off));
      mark();
      i++;
      continue;
    }
    if (uint8_t(c) < 0x20 && c != '\n' && c != '\t') { i++; continue; }
    text.push_back(c);
    runs.back().len++;
    if (c == '\n') line_start = text.size();
    i++;
  }
  std::erase_if(runs, [](const Seg& r) { return r.len == 0; });
}

}  // namespace

namespace {
// Every paint seen, by id; output uses a handful of colours, a gradient a few
// hundred, so this stays small. Full, a new paint is dropped to its ink.
struct Paints {
  std::mutex mu;
  std::vector<Paint> all{Paint{}};  // id 0: none
  std::unordered_map<uint64_t, uint16_t> ids;
};
Paints& paints() {
  static Paints* p = new Paints;  // never destroyed: laid-out rows outlive statics
  return *p;
}
}  // namespace

uint16_t paint_id(Paint p) {
  Paints& ps = paints();
  const uint64_t key = uint64_t(uint32_t(p.fg)) << 32 | uint32_t(p.bg);
  std::lock_guard lock(ps.mu);
  if (auto it = ps.ids.find(key); it != ps.ids.end()) return it->second;
  if (ps.all.size() > 0xFFFF) return 0;
  const auto id = uint16_t(ps.all.size());
  ps.all.push_back(p);
  ps.ids.emplace(key, id);
  return id;
}

Paint paint(uint16_t id) {
  Paints& ps = paints();
  std::lock_guard lock(ps.mu);
  return id < ps.all.size() ? ps.all[id] : Paint{};
}

void find_links(std::string_view s, const std::string& base, std::vector<LinkHit>& out) {
  out.clear();
  if (render_settings().links() == Links::Off) return;
  std::vector<std::pair<size_t, size_t>> urls;
  find_urls(s, urls);
  for (auto [a, b] : urls) out.push_back(LinkHit{a, b, url_target(s.substr(a, b - a))});
  if (render_settings().links() == Links::UrlsAndPaths &&
      (s.find('.') != std::string_view::npos || s.find('/') != std::string_view::npos)) {
    std::vector<LinkHit> paths;
    find_paths(s, base, out, paths);
    out.insert(out.end(), paths.begin(), paths.end());
    std::sort(out.begin(), out.end(), [](const LinkHit& a, const LinkHit& b) { return a.begin < b.begin; });
  }
}

void render_output(std::string_view text, uint32_t base, bool in_scratch, int cols, Out& out,
                   const code::Lang* lang, bool all_code) {
  if (cols < 4) cols = 4;
  const std::string link_base = out.charts ? out.charts->base_dir : std::string();
  // The text the rows show, and where it lives: the event's own bytes when
  // they need no change, a cleaned copy in scratch when escapes come out.
  std::string clean;
  std::vector<Seg> colour;
  uint32_t ref;
  std::string_view body;
  if (text.find('\x1b') != std::string_view::npos || text.find('\r') != std::string_view::npos) {
    strip_ansi(text, clean, colour);
    if (render_settings().output_colours() == OutputColours::Plain) colour.assign(1, Seg{0, uint32_t(clean.size()), Ink::Text});
    ref = out.scratch->add(clean).off | kScratchBit;
    body = clean;
  } else {
    colour.push_back(Seg{0, uint32_t(text.size()), Ink::Text});
    ref = base | (in_scratch ? kScratchBit : 0u);
    body = text;
  }

  // A file's contents, as Read and cat -n number them: "  12→code" or "12\tcode".
  const auto numbered = [](std::string_view l, size_t* after) {
    size_t k = 0;
    while (k < l.size() && l[k] == ' ') k++;
    const size_t d = k;
    while (k < l.size() && std::isdigit(uint8_t(l[k]))) k++;
    if (k == d) return false;
    if (l.compare(k, 3, "\xE2\x86\x92") == 0) { *after = k + 3; return true; }
    if (k < l.size() && l[k] == '\t') { *after = k + 1; return true; }
    return false;
  };
  code::State cst;
  std::vector<code::Run> hl;
  std::vector<LinkHit> links_found;
  std::vector<Seg> runs;
  for (size_t pos = 0; pos <= body.size();) {
    if (out.lines->size() >= out.max_lines) return;
    if (pos == body.size() && pos > 0 && body[pos - 1] == '\n') break;
    size_t nl = body.find('\n', pos);
    if (nl == std::string_view::npos) nl = body.size();
    std::string_view line = body.substr(pos, nl - pos);

    // The line's runs: its colours, or the language's.
    runs.clear();
    size_t content = 0;
    const bool listing = lang && (all_code || numbered(line, &content));
    if (listing) {
      if (content) runs.push_back(Seg{uint32_t(pos), uint32_t(content), Ink::CodeMark});
      code::highlight(line.substr(content), lang, cst, hl);
      for (const auto& r : hl) {
        static constexpr Ink kTok[] = {Ink::CodeText, Ink::CodeKeyword, Ink::CodeString, Ink::CodeComment, Ink::CodeNumber,
                                       Ink::CodeType, Ink::CodeFunc,    Ink::Added,      Ink::Removed,     Ink::Hunk};
        runs.push_back(Seg{uint32_t(pos + content + r.off), r.len, kTok[int(r.tok)]});
      }
    } else {
      for (const Seg& c : colour) {
        const size_t a = std::max<size_t>(c.off, pos), b = std::min<size_t>(c.off + c.len, nl);
        if (a < b) runs.push_back(Seg{uint32_t(a), uint32_t(b - a), c.ink, c.attr, 0, c.paint});
      }
    }
    // Links cut the runs where they start and end.
    find_links(line, link_base, links_found);
    if (!links_found.empty()) {
      std::vector<Seg> cut;
      for (const Seg& r : runs) {
        size_t a = r.off;
        const size_t b = size_t(r.off) + r.len;
        for (const auto& h : links_found) {
          const size_t la = pos + h.begin, lb = pos + h.end;
          if (lb <= a || la >= b) continue;
          if (la > a) cut.push_back(Seg{uint32_t(a), uint32_t(la - a), r.ink, r.attr, 0, r.paint});
          const size_t s0 = std::max(a, la), e = std::min(b, lb);
          cut.push_back(Seg{uint32_t(s0), uint32_t(e - s0), r.ink == Ink::Text && !r.paint ? Ink::Link : r.ink, r.attr,
                            links::intern(h.target), r.paint});
          a = e;
        }
        if (a < b) cut.push_back(Seg{uint32_t(a), uint32_t(b - a), r.ink, r.attr, 0, r.paint});
      }
      runs.swap(cut);
    }

    // Wrapped like prose: on spaces, a word longer than the line cut where it
    // must. A listing's continuation sits under its code, not its number.
    // Output that is indented (a todo list, a tree) keeps its indentation on
    // the rows a long line wraps onto, under the text rather than at the
    // margin: after a bullet's "- " too.
    size_t hang = 0;
    if (!listing) {
      while (hang < line.size() && line[hang] == ' ') hang++;
      if (hang && hang + 1 < line.size() && (line[hang] == '-' || line[hang] == '*' || line[hang] == '+') &&
          line[hang + 1] == ' ')
        hang += 2;
      if (hang >= line.size() || int(hang) * 2 > cols) hang = 0;
    }
    if (hang) text::wrap_spans(line.substr(hang), cols - int(hang), *out.spans, out.max_lines - out.lines->size());
    else text::wrap_spans(line, cols, *out.spans, out.max_lines - out.lines->size());
    bool first = true;
    for (const auto& sp : *out.spans) {
      if (out.lines->size() >= out.max_lines) return;
      const uint32_t first_seg = uint32_t(out.segs->size());
      // The first row of an indented line starts at its indentation.
      const size_t lo = hang ? (first ? pos : pos + hang + sp.off) : pos + sp.off;
      const size_t hi = hang ? pos + hang + sp.off + sp.len : lo + sp.len;
      if ((listing || hang) && !first) {
        const Str pad = out.scratch->add(std::string(listing ? content : hang, ' '));
        out.segs->push_back(Seg{pad.off | kScratchBit, pad.len, listing ? Ink::CodeMark : Ink::Text});
      }
      for (const Seg& r : runs) {
        const size_t a = std::max<size_t>(r.off, lo), b = std::min<size_t>(size_t(r.off) + r.len, hi);
        if (a < b) out.segs->push_back(Seg{ref + uint32_t(a), uint32_t(b - a), r.ink, r.attr, r.link, r.paint});
      }
      out.lines->push_back(Line{first_seg, uint16_t(out.segs->size() - first_seg), 0});
      first = false;
    }
    if (out.spans->empty()) out.lines->push_back(Line{uint32_t(out.segs->size()), 0, 0});
    pos = nl + 1;
    if (nl == body.size()) break;
  }
}

static Ink tok_ink(code::Tok t) {
  static constexpr Ink kTok[] = {Ink::CodeText, Ink::CodeKeyword, Ink::CodeString, Ink::CodeComment, Ink::CodeNumber,
                                 Ink::CodeType, Ink::CodeFunc,    Ink::Added,      Ink::Removed,     Ink::Hunk};
  return kTok[int(t)];
}

void render_diff(std::string_view text, uint32_t base, bool in_scratch, int cols, Out& out,
                 const code::Lang* lang) {
  const uint32_t flag = in_scratch ? kScratchBit : 0u;
  struct DL {
    size_t off, len;
    char kind;              // '+', '-', ' ' (kept), 'h' (a header or hunk)
    size_t lo = 0, hi = 0;  // what changed, within the line, when it pairs with one
  };
  std::vector<DL> ls;
  for (size_t pos = 0; pos <= text.size();) {
    if (pos == text.size() && pos > 0 && text[pos - 1] == '\n') break;
    size_t nl = text.find('\n', pos);
    size_t end = nl == std::string_view::npos ? text.size() : nl;
    if (end > pos && text[end - 1] == '\r') end--;
    const std::string_view line = text.substr(pos, end - pos);
    char kind = ' ';
    if (line.starts_with("@@") || line.starts_with("***") || line.starts_with("+++ ") ||
        line.starts_with("--- ") || line.starts_with("diff --git"))
      kind = 'h';
    else if (!line.empty() && (line[0] == '+' || line[0] == '-')) kind = line[0];
    ls.push_back(DL{pos, line.size(), kind});
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  // Removed lines and the added ones right after them pair up in order; in
  // each pair, what lies between the common start and end is the change.
  for (size_t i = 0; i < ls.size();) {
    if (ls[i].kind != '-') { i++; continue; }
    size_t d = i;
    while (d < ls.size() && ls[d].kind == '-') d++;
    size_t a = d;
    while (a < ls.size() && ls[a].kind == '+') a++;
    for (size_t k = 0; k < std::min(d - i, a - d); k++) {
      DL& x = ls[i + k];
      DL& y = ls[d + k];
      const std::string_view sx = text.substr(x.off + 1, x.len - 1), sy = text.substr(y.off + 1, y.len - 1);
      size_t pre = 0;
      while (pre < sx.size() && pre < sy.size() && sx[pre] == sy[pre]) pre++;
      while (pre > 0 && (uint8_t(sx[pre]) & 0xC0) == 0x80) pre--;
      size_t suf = 0;
      while (suf < sx.size() - pre && suf < sy.size() - pre && sx[sx.size() - 1 - suf] == sy[sy.size() - 1 - suf]) suf++;
      while (suf > 0 && (uint8_t(sx[sx.size() - suf]) & 0xC0) == 0x80) suf--;
      // A line changed nearly throughout is just a different line.
      const size_t cx = sx.size() - pre - suf, cy = sy.size() - pre - suf;
      if ((cx + cy) * 10 > (sx.size() + sy.size()) * 8) continue;
      x.lo = 1 + pre, x.hi = 1 + pre + cx;
      y.lo = 1 + pre, y.hi = 1 + pre + cy;
    }
    i = a;
  }

  std::vector<code::Run> hl;
  code::State st;
  for (const DL& l : ls) {
    if (out.lines->size() >= out.max_lines) return;
    const std::string_view line = text.substr(l.off, l.len);
    // A file header names the language of what follows it.
    for (std::string_view tag : {std::string_view("*** Update File: "), std::string_view("*** Add File: "),
                                 std::string_view("+++ b/"), std::string_view("+++ ")}) {
      if (line.starts_with(tag)) {
        if (const code::Lang* found = code::lang_of_path(line.substr(tag.size()))) lang = found;
        st = {};
        break;
      }
    }
    std::vector<Seg> runs;
    if (l.kind == 'h') {
      runs.push_back(Seg{uint32_t(l.off), uint32_t(l.len), Ink::Hunk});
    } else {
      const Ink mark = l.kind == '+' ? Ink::Added : l.kind == '-' ? Ink::Removed : Ink::CodeMark;
      const size_t body = line.empty() ? 0 : 1;
      if (body) runs.push_back(Seg{uint32_t(l.off), 1, mark});
      if (lang) {
        code::highlight(line.substr(body), lang, st, hl);
        for (const auto& r : hl) runs.push_back(Seg{uint32_t(l.off + body + r.off), r.len, tok_ink(r.tok)});
      } else if (l.len > body) {
        runs.push_back(Seg{uint32_t(l.off + body), uint32_t(l.len - body),
                           l.kind == '+' ? Ink::Added : l.kind == '-' ? Ink::Removed : Ink::Text});
      }
      // The changed part, cut out of whatever run it falls in.
      if (l.hi > l.lo) {
        std::vector<Seg> cut;
        const size_t lo = l.off + l.lo, hi = l.off + l.hi;
        for (const Seg& r : runs) {
          const size_t a = r.off, b = size_t(r.off) + r.len;
          if (b <= lo || a >= hi) { cut.push_back(r); continue; }
          if (a < lo) cut.push_back(Seg{uint32_t(a), uint32_t(lo - a), r.ink});
          cut.push_back(Seg{uint32_t(std::max(a, lo)), uint32_t(std::min(b, hi) - std::max(a, lo)), r.ink, kAttrStrong});
          if (b > hi) cut.push_back(Seg{uint32_t(hi), uint32_t(b - hi), r.ink});
        }
        runs.swap(cut);
      }
    }
    // Truncated, never wrapped: a wrapped diff line reads as a change that is
    // not there. Cut by columns, on a character's edge.
    const uint32_t first = uint32_t(out.segs->size());
    int used = 0;
    for (const Seg& r : runs) {
      if (used >= cols) break;
      const std::string_view t = text.substr(r.off, r.len);
      size_t i = 0;
      int w = 0;
      while (i < t.size()) {
        int cw;
        const size_t j = text::glyph_end(t, i, &cw);
        cw = std::max(0, cw);
        if (used + w + cw > cols) break;
        w += cw;
        i = j;
      }
      if (i) out.segs->push_back(Seg{(base + r.off) | flag, uint32_t(i), r.ink, r.attr});
      used += w;
      if (i < t.size()) break;
    }
    const uint8_t flags = l.kind == '+' ? kAddRow : l.kind == '-' ? kDelRow : 0;
    out.lines->push_back(Line{first, uint16_t(out.segs->size() - first), 0, flags});
  }
}

}  // namespace mico::md
