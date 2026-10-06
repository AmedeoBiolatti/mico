#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/color.h"
#include "model/event.h"
#include "base/text.h"
#include "views/code.h"

// Just enough markdown for what coding agents actually emit. Not a spec-
// compliant parser: it is a renderer for prose, and anything it does not
// recognise falls through as plain text rather than being mangled.
namespace mico::md {

// Inline role of a run of text. Resolved to colours at draw time against the
// row's base style, so the theme never touches layout.
enum class Ink : uint8_t {
  Text,
  Bold,
  Italic,
  Code,
  Link,
  Heading,
  Quote,
  Bullet,
  Rule,
  Added,
  Removed,
  Hunk,
  Math,
  // One row of an equation drawn as an image; see image_ref().
  MathImage,
  // Fenced code: plain text, then what the highlighter found, then the
  // block's marks (a wrapped line's ↪, the language label).
  CodeText, CodeKeyword, CodeString, CodeComment, CodeNumber, CodeType, CodeFunc, CodeMark,
  // A chart block: its axes and labels, and one colour per series.
  ChartAxis,
  Series1, Series2, Series3, Series4, Series5, Series6,
  // Tool output's own ANSI colours, the sixteen of a terminal palette.
  Ansi0, Ansi1, Ansi2, Ansi3, Ansi4, Ansi5, Ansi6, Ansi7,
  Ansi8, Ansi9, Ansi10, Ansi11, Ansi12, Ansi13, Ansi14, Ansi15,
};

// Seg::attr bits: what ANSI output asked for beyond a colour.
inline constexpr uint8_t kAttrBold = 1, kAttrDim = 2, kAttrItalic = 4, kAttrUnderline = 8, kAttrStrike = 16;
// A diff's changed words, inside a line that was otherwise kept.
inline constexpr uint8_t kAttrStrong = 32;

// The bar down a quote's or a callout's left edge. A copy leaves it out.
inline constexpr std::string_view kQuoteBar = "\xE2\x96\x8E ";

// High bit of `off` selects the scratch arena over the event arena.
inline constexpr uint32_t kScratchBit = 0x80000000u;

struct Seg {
  uint32_t off;
  uint32_t len;
  Ink ink;
  uint8_t attr = 0;   // extra attributes: an ANSI colour run's bold, dim…
  uint16_t link = 0;  // a links:: id when the run is (part of) a hyperlink
  uint16_t paint = 0; // a paint_id(): colours output gave exactly, beyond its ink
};
static_assert(sizeof(Seg) == 16);

// What ANSI output asks for beyond the sixteen colours an Ink names: a
// 256-colour or 24-bit foreground, and any background. Either colour is
// 0xRRGGBB, kDefaultColor for none, or palette(n), the theme's ANSI colour n.
// Kept by id, so a Seg stays small; id 0 is no paint.
struct Paint {
  Color fg = kDefaultColor, bg = kDefaultColor;
  bool operator==(const Paint&) const = default;
};
constexpr Color palette(int n) { return -2 - n; }
constexpr int palette_index(Color c) { return c <= -2 ? -2 - c : -1; }
uint16_t paint_id(Paint p);
Paint paint(uint16_t id);

struct Line {
  uint32_t seg_first;
  uint16_t seg_count;
  uint8_t indent;
  uint8_t flags = 0;
};
// A row added above a line of text to make room for a tall inline equation:
// the line's own text (and a list item's bullet) is on a later row.
inline constexpr uint8_t kLeadRow = 1;
// A row of a fenced code block: drawn on the code background.
inline constexpr uint8_t kCodeRow = 2;
// A diff's added or removed line: drawn on a green or red tint.
inline constexpr uint8_t kAddRow = 4, kDelRow = 8;

// Buffers render() reuses from one call to the next, so laying out a long
// chat does not allocate per message, per line or per table cell. Optional:
// without one, render() makes its own each call.
struct Work {
  std::vector<Seg> runs;               // the line being laid out
  std::vector<std::string_view> rows;  // a table's source lines
  std::vector<std::string_view> cells;  // its cells, row after row
  std::vector<uint32_t> row_start;     // where each row's cells begin, then the end
  std::vector<char> align;
  std::vector<int> width;
  std::vector<Seg> cell_runs;
  std::string flat;                    // a cell's text with its markers removed
  std::vector<code::Run> code_runs;    // a code line's coloured runs
  std::string code_line;               // a code line with its tabs expanded
};

// What a ```chart block needs from its surroundings: the folder a relative
// data file is read from, and a list to record the files it read, so the chat
// can redraw the chart when one changes.
// What a laid-out image is drawn from, the same for all its sizes: an
// equation's TeX, a picture's file or data key, a chart's spec (a subplot's
// image names its figure's spec, then "#n").
std::string image_source(std::string_view src);

struct ChartEnv {
  std::string base_dir;
  std::vector<std::pair<std::string, int64_t>>* watched = nullptr;  // path, mtime
  // The picture, equation or chart zoomed into (what it is drawn from: see
  // image_source()), laid out as large as `zoom_rows` rows and the width
  // allow; empty for none.
  std::string zoom;
  int zoom_rows = 0;
  bool zoomed(std::string_view src) const { return !zoom.empty() && zoom == src; }
};

struct Out {
  // Stop after this many lines. Capped blocks (a collapsed tool result shows
  // three) must not lay out a hundred thousand rows to then throw them away.
  size_t max_lines = size_t(-1);
  Arena* scratch;
  std::vector<Seg>* segs;
  std::vector<Line>* lines;
  // Reused across calls so a steady layout allocates nothing.
  std::vector<text::Span>* spans;
  std::vector<Seg>* inline_scratch;
  Work* work = nullptr;
  const ChartEnv* charts = nullptr;  // null: charts still draw, from inline data
};

// Renders `text` wrapped to `cols`, appending to out.lines / out.segs.
// `base` is the offset of `text` within the arena named by `in_scratch`.
void render(std::string_view text, uint32_t base, bool in_scratch, int cols, Out& out);

// Tool output: its ANSI colours kept, links found. With `lang`, a numbered
// listing (Read, cat -n) is coloured as that language — every line of it
// when `all_code` (a JSON result laid out by mico).
void render_output(std::string_view text, uint32_t base, bool in_scratch, int cols, Out& out,
                   const code::Lang* lang, bool all_code = false);

// Renders a unified-diff-ish blob: lines are classified by their first
// character and never wrapped on words, since alignment carries meaning.
void render_diff(std::string_view text, uint32_t base, bool in_scratch, int cols, Out& out,
                 const code::Lang* lang = nullptr);

}  // namespace mico::md

namespace mico::math { struct Image; }

namespace mico::md {

// A picture laid out as rows of image cells, `indent` columns in.
void picture_rows(const math::Image& im, uint8_t indent, Out& out);

// An image row's segment names a run of no-break spaces, one per column, in
// the scratch arena; the eight bytes before it say which image and which of
// its rows. False for any other segment.
bool image_ref(const Arena& scratch, const Seg& s, uint32_t* id, int* row, int* cols);

// The URLs in `s`, as [begin, end) byte ranges: http(s)://, www., file://.
// Trailing punctuation is left out, and a closing bracket only kept when the
// URL opened one (a Wikipedia link), so "see https://x.org/a)." links just
// the address.
void find_urls(std::string_view s, std::vector<std::pair<size_t, size_t>>& out);
// The link to open for a URL found by find_urls: www. gains https://.
std::string url_target(std::string_view found);

// Everything in `s` a click can open: URLs, and paths of files that exist
// (relative to `base`, the agent's folder), with a :line[:col] or #L line
// when one follows. A file's target is file://<abs path>, with #L<line>.
struct LinkHit {
  size_t begin, end;
  std::string target;
};
void find_links(std::string_view s, const std::string& base, std::vector<LinkHit>& out);

}  // namespace mico::md
