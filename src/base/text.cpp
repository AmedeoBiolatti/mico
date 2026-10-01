#include "base/text.h"

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

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

namespace {

// Where a substring search spends its time is in rejecting positions. Testing
// sixteen at once against the needle's first and last byte (Muła's filter)
// leaves a handful of candidates per kilobyte of JSON, where testing the first
// byte alone stops at every quote. SSE2 is part of x86-64 itself, so this
// needs no flags and no dispatch. `fold` compares letters case-insensitively:
// the needle is lower-case, and a hay byte OR 0x20 equals a lower-case letter
// only when it is that letter in either case.
template <bool kFold>
size_t find_impl(std::string_view hay, std::string_view needle, size_t from) {
  const size_t n = hay.size(), k = needle.size();
  if (k == 0) return from <= n ? from : std::string_view::npos;
  if (n < k || from > n - k) return std::string_view::npos;
  const char* h = hay.data();
  const char* nd = needle.data();
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
  const auto letter = [](char c) { return c >= 'a' && c <= 'z'; };
  const auto match = [&](size_t i) {
    if constexpr (kFold) {
      for (size_t j = 0; j < k; j++)
        if (lower(h[i + j]) != nd[j]) return false;
      return true;
    } else {
      return memcmp(h + i, nd, k) == 0;
    }
  };
  size_t i = from;
#if defined(__SSE2__)
  if (k >= 2) {
    const __m128i first = _mm_set1_epi8(nd[0]), last = _mm_set1_epi8(nd[k - 1]);
    const __m128i fold_first = _mm_set1_epi8(kFold && letter(nd[0]) ? 0x20 : 0);
    const __m128i fold_last = _mm_set1_epi8(kFold && letter(nd[k - 1]) ? 0x20 : 0);
    for (; i + k - 1 + 16 <= n; i += 16) {
      __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i));
      __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i + k - 1));
      if constexpr (kFold) {
        a = _mm_or_si128(a, fold_first);
        b = _mm_or_si128(b, fold_last);
      }
      unsigned mask = unsigned(_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(a, first), _mm_cmpeq_epi8(b, last))));
      while (mask) {
        const size_t at = i + size_t(__builtin_ctz(mask));
        if (match(at)) return at;
        mask &= mask - 1;
      }
    }
  }
#endif
  for (; i + k <= n; i++)
    if ((kFold ? lower(h[i]) : h[i]) == nd[0] && match(i)) return i;
  return std::string_view::npos;
}

}  // namespace

size_t find(std::string_view hay, std::string_view needle, size_t from) {
  // glibc's memmem is vectorised, and measured three times quicker than the
  // filter above on transcript text; the filter earns its keep folded.
  if (from > hay.size()) return std::string_view::npos;
  const void* p = memmem(hay.data() + from, hay.size() - from, needle.data(), needle.size());
  return p ? size_t(static_cast<const char*>(p) - hay.data()) : std::string_view::npos;
}

size_t find_folded(std::string_view hay, std::string_view needle, size_t from) {
  return find_impl<true>(hay, needle, from);
}

}  // namespace mico::text
