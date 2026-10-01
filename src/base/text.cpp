#include "base/text.h"

#include <cstring>
#include <algorithm>

namespace mico::text {
namespace {

struct Range { char32_t lo, hi; };

// Zero-width: combining marks, joiners, variation selectors.
constexpr Range kZero[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x0816, 0x0819}, {0x08E3, 0x0903}, {0x093A, 0x093C},
    {0x0951, 0x0957}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
    {0x20D0, 0x20F0}, {0x2CEF, 0x2CF1}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0xE0100, 0xE01EF},
};

// Double-width: CJK, Hangul, and the emoji blocks agents actually emit.
constexpr Range kWide[] = {
    {0x1100, 0x115F},   {0x2E80, 0x303E},   {0x3041, 0x33FF},
    {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x1F004, 0x1F004},
    {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF},
    {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

template <size_t N>
bool in_ranges(char32_t cp, const Range (&t)[N]) {
  size_t lo = 0, hi = N;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < t[mid].lo) hi = mid;
    else if (cp > t[mid].hi) lo = mid + 1;
    else return true;
  }
  return false;
}

bool is_space(char32_t c) { return c == U' ' || c == U'\t'; }

constexpr char kNewline = 0x0A;
constexpr char kReturn = 0x0D;

}  // namespace

char32_t decode(std::string_view s, size_t& i) {
  if (i >= s.size()) return 0;
  auto b0 = uint8_t(s[i]);
  auto cont = [&](size_t k) {
    return i + k < s.size() && (uint8_t(s[i + k]) & 0xC0) == 0x80;
  };
  if (b0 < 0x80) { i += 1; return b0; }
  if ((b0 & 0xE0) == 0xC0 && cont(1)) {
    char32_t cp = char32_t(b0 & 0x1F) << 6 | (uint8_t(s[i + 1]) & 0x3F);
    i += 2;
    return cp;
  }
  if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
    char32_t cp = char32_t(b0 & 0x0F) << 12 | char32_t(uint8_t(s[i + 1]) & 0x3F) << 6 |
                  (uint8_t(s[i + 2]) & 0x3F);
    i += 3;
    return cp;
  }
  if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
    char32_t cp = char32_t(b0 & 0x07) << 18 | char32_t(uint8_t(s[i + 1]) & 0x3F) << 12 |
                  char32_t(uint8_t(s[i + 2]) & 0x3F) << 6 | (uint8_t(s[i + 3]) & 0x3F);
    i += 4;
    return cp;
  }
  i += 1;
  return 0xFFFD;
}

void encode(char32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(char(cp));
  } else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

int cp_width(char32_t cp) {
  // Transcripts are overwhelmingly ASCII and this runs per glyph, twice, in
  // both wrapping and drawing.
  if (cp < 0x7F) return cp >= 0x20 ? 1 : 0;
  if (cp == 0) return 0;
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;  // control
  if (in_ranges(cp, kZero)) return 0;
  if (in_ranges(cp, kWide)) return 2;
  return 1;
}

int str_width(std::string_view s) {
  int w = 0;
  for (size_t i = 0; i < s.size();) w += cp_width(decode(s, i));
  return w;
}

std::string ellipsize(std::string_view s, int max_cols) {
  if (max_cols <= 0) return {};
  if (str_width(s) <= max_cols) return std::string(s);
  std::string out;
  int w = 0;
  for (size_t i = 0; i < s.size();) {
    size_t start = i;
    char32_t cp = decode(s, i);
    int cw = cp_width(cp);
    if (w + cw > max_cols - 1) break;
    out.append(s.substr(start, i - start));
    w += cw;
  }
  out += "\xE2\x80\xA6";  // U+2026
  return out;
}

std::string oneline(std::string_view s, int max_cols) {
  std::string flat;
  bool pending_space = false;
  for (size_t i = 0; i < s.size();) {
    size_t start = i;
    char32_t cp = decode(s, i);
    if (cp == U'\n' || cp == U'\r' || is_space(cp)) {
      pending_space = !flat.empty();
      continue;
    }
    if (cp < 0x20) continue;
    if (pending_space) { flat.push_back(' '); pending_space = false; }
    flat.append(s.substr(start, i - start));
  }
  return max_cols > 0 ? ellipsize(flat, max_cols) : flat;
}

void wrap_spans(std::string_view s, int cols, std::vector<Span>& out, size_t max_spans) {
  out.clear();
  if (cols <= 0 || max_spans == 0) return;

  size_t line_start = 0;
  while (line_start <= s.size() && out.size() < max_spans) {
    // A trailing newline ends the last line; it does not start an empty one.
    if (line_start == s.size() && line_start > 0 && s[line_start - 1] == kNewline) break;
    size_t nl = s.find(kNewline, line_start);
    size_t line_end = nl == std::string_view::npos ? s.size() : nl;
    if (line_end > line_start && s[line_end - 1] == kReturn) line_end--;

    if (line_end == line_start) {
      out.push_back(Span{uint32_t(line_start), 0});
    } else {
      // Greedy fill, remembering the last space so we can break on words.
      size_t i = line_start, seg = line_start;
      size_t last_break = std::string_view::npos;
      int w = 0;
      while (i < line_end) {
        size_t cp_start = i;
        char32_t cp;
        int cw;
        // Nearly every byte of a transcript is ASCII: take it as it is rather
        // than through the decoder and the width tables.
        if (const unsigned char b = uint8_t(s[i]); b < 0x80) {
          cp = b;
          cw = b >= 0x20 && b < 0x7F ? 1 : 0;
          i++;
        } else {
          cp = decode(s, i);
          if (i > line_end) i = line_end;
          cw = cp_width(cp);
        }
        if (w + cw > cols) {
          size_t cut = (last_break != std::string_view::npos && last_break > seg) ? last_break
                                                                                 : cp_start;
          if (cut <= seg) cut = i;  // a single glyph wider than the whole line
          out.push_back(Span{uint32_t(seg), uint32_t(cut - seg)});
          if (out.size() >= max_spans) return;
          seg = cut;
          while (seg < line_end && s[seg] == ' ') seg++;
          // Resume at the new segment start, never at the glyph that
          // overflowed: restarting later drops a character from the width
          // count and every following line comes out too wide.
          i = seg;
          last_break = std::string_view::npos;
          w = 0;
          continue;
        }
        w += cw;
        if (cp == U' ') last_break = cp_start;
      }
      if (seg < line_end) out.push_back(Span{uint32_t(seg), uint32_t(line_end - seg)});
    }

    if (nl == std::string_view::npos) break;
    line_start = nl + 1;
  }
}

std::string fold(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

size_t find_folded(std::string_view hay, std::string_view needle, size_t from) {
  if (needle.empty()) return from <= hay.size() ? from : std::string_view::npos;
  if (hay.size() < needle.size()) return std::string_view::npos;
  const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : char(c); };
  const char c0 = needle[0];
  const char c1 = c0 >= 'a' && c0 <= 'z' ? char(c0 - 'a' + 'A') : c0;
  const size_t last = hay.size() - needle.size();
  const char* base = hay.data();
  const char* end = base + last + 1;  // candidate starts lie in [base, end)
  // The next occurrence of each case of the first byte, kept between
  // candidates: re-searching both from every candidate turns a first byte
  // that is common in one case and absent in the other into a quadratic scan.
  const auto next = [&](char c, const char* from) {
    if (from >= end) return end;
    const void* p = memchr(from, c, size_t(end - from));
    return p ? static_cast<const char*>(p) : end;
  };
  const char* pa = next(c0, base + from);
  const char* pb = c1 != c0 ? next(c1, base + from) : end;
  while (true) {
    const char* p = std::min(pa, pb);
    if (p >= end) return std::string_view::npos;
    size_t k = 1;
    while (k < needle.size() && lower(static_cast<unsigned char>(p[k])) == needle[k]) k++;
    if (k == needle.size()) return size_t(p - base);
    if (p == pa) pa = next(c0, p + 1);
    if (p == pb) pb = next(c1, p + 1);
  }
  return std::string_view::npos;
}

}  // namespace mico::text
