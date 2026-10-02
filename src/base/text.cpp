#include "base/text.h"

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

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
    {0xE0020, 0xE007F}, {0xE0100, 0xE01EF},
};

// Double-width: what Unicode 15 calls East Asian Wide or Fullwidth, the table
// terminals' wcwidth is built from, so a column here is a column there. The
// two supplemental emoji blocks count whole, so emoji newer than the table
// still take two.
constexpr Range kWide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
    {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
    {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728},
    {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
    {0x2E80, 0x2E99}, {0x2E9B, 0x2EF3}, {0x2F00, 0x2FD5}, {0x2FF0, 0x2FFB}, {0x3000, 0x3029},
    {0x302E, 0x303E}, {0x3041, 0x3096}, {0x309B, 0x30FF}, {0x3105, 0x312F}, {0x3131, 0x318E},
    {0x3190, 0x31E3}, {0x31F0, 0x321E}, {0x3220, 0x3247}, {0x3250, 0x4DBF}, {0x4E00, 0xA48C},
    {0xA490, 0xA4C6}, {0xA960, 0xA97C}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
    {0xFE30, 0xFE52}, {0xFE54, 0xFE66}, {0xFE68, 0xFE6B}, {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6},
    {0x16FE0, 0x16FE3}, {0x16FF0, 0x16FF1}, {0x17000, 0x187F7}, {0x18800, 0x18CD5}, {0x18D00, 0x18D08},
    {0x1AFF0, 0x1AFF3}, {0x1AFF5, 0x1AFFB}, {0x1AFFD, 0x1AFFE}, {0x1B000, 0x1B122}, {0x1B132, 0x1B132},
    {0x1B150, 0x1B152}, {0x1B155, 0x1B155}, {0x1B164, 0x1B167}, {0x1B170, 0x1B2FB}, {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B},
    {0x1F240, 0x1F248}, {0x1F250, 0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7},
    {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0},
    {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
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
  if (cp >= kGlyphBase) {
    out += glyph_text(cp);
    return;
  }
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
  for (size_t i = 0; i < s.size();) {
    if (uint8_t(s[i]) < 0x80) {
      w += cp_width(char32_t(s[i++]));
      continue;
    }
    int gw;
    i = glyph_end(s, i, &gw);
    w += gw;
  }
  return w;
}

namespace {

constexpr char32_t kZwj = 0x200D, kVs15 = 0xFE0E, kVs16 = 0xFE0F;
bool regional(char32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
bool skin_tone(char32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }

// Sequences seen so far, each under one id for as long as the process runs.
// There are only so many emoji, so this stays small; a deque, so a view of
// an entry outlives later ones being added. The lock is for this layer's
// worker-thread users; plain code points, nearly everything, never take it.
struct Glyphs {
  std::mutex mu;
  std::deque<std::string> text;
  std::unordered_map<std::string_view, char32_t> ids;
};
Glyphs& glyphs() {
  static Glyphs* g = new Glyphs;  // never destroyed: cells outlive statics
  return *g;
}
constexpr size_t kMaxGlyphs = 1 << 16;

}  // namespace

size_t glyph_end(std::string_view s, size_t i, int* width) {
  const char32_t base = decode(s, i);
  int w = cp_width(base);
  if (base < 0x80 || w == 0) {
    *width = w;
    return i;
  }
  bool pair = regional(base);  // a flag still waiting for its second letter
  while (i < s.size()) {
    size_t j = i;
    const char32_t cp = decode(s, j);
    if (cp == kVs16) {
      if (w == 1) w = 2;  // the emoji form of a symbol drawn as text by default
    } else if (cp == kVs15) {
      // The text form: as wide as it already was.
    } else if (cp == kZwj) {
      // Joins the next picture into this one: 👩 ZWJ 💻 is one glyph.
      if (j < s.size() && uint8_t(s[j]) >= 0x80) decode(s, j);
    } else if (pair && regional(cp)) {
      pair = false;
      w = 2;
    } else if (!skin_tone(cp) && cp_width(cp) != 0) {
      break;
    }
    i = j;
  }
  *width = w;
  return i;
}

char32_t next_glyph(std::string_view s, size_t& i, int* width) {
  const size_t at = i;
  size_t one = i;
  const char32_t cp = decode(s, one);
  i = glyph_end(s, at, width);
  if (i == one) return cp;
  const std::string_view seq = s.substr(at, i - at);
  Glyphs& g = glyphs();
  std::lock_guard lock(g.mu);
  if (auto it = g.ids.find(seq); it != g.ids.end()) return it->second;
  if (g.text.size() >= kMaxGlyphs) return cp;  // full: the first code point alone
  const char32_t id = kGlyphBase + char32_t(g.text.size());
  g.ids.emplace(g.text.emplace_back(seq), id);
  return id;
}

std::string_view glyph_text(char32_t id) {
  if (id < kGlyphBase) return {};
  Glyphs& g = glyphs();
  std::lock_guard lock(g.mu);
  const size_t k = size_t(id - kGlyphBase);
  return k < g.text.size() ? std::string_view(g.text[k]) : std::string_view();
}

std::string ellipsize(std::string_view s, int max_cols) {
  if (max_cols <= 0) return {};
  if (str_width(s) <= max_cols) return std::string(s);
  std::string out;
  int w = 0;
  for (size_t i = 0; i < s.size();) {
    size_t start = i;
    int cw;
    i = glyph_end(s, i, &cw);
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
          cp = U'x';  // only a space matters below, and this is never one
          i = glyph_end(s, i, &cw);
          if (i > line_end) i = line_end;
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

// How common a byte is in transcript text — JSON around English and code —
// highest first: how a search picks the second byte it tests.
struct ByteRank {
  uint8_t rank[256];
  ByteRank() {
    static const char kCommon[] =
        " e\"tao,insr:hl\nd_cu{}m.pfg\\ywb0v1-/k2T3S4ACI5E6R7O8P9NLMDBxFjqzHWGUVYKQXZJ"
        "[]()=;'<>*#@!?$%&+|^~`";
    for (int i = 0; i < 256; i++) rank[i] = 0;  // not listed: rare
    const int n = int(sizeof kCommon) - 1;
    for (int i = 0; i < n; i++) rank[uint8_t(kCommon[i])] = uint8_t(n - i);
  }
};
const ByteRank kRank;

// The positions a search tests: the needle's first byte, and its rarest other
// byte. Rarity alone fails on the blobs transcripts are full of: base64 is
// letters, digits and '/', so a needle's rarest letters match all through an
// image, where its first byte — for a JSON key, a quote — never occurs and
// skips the whole blob. The pair rejects both prose and JSON structure.
void anchors(std::string_view needle, bool fold, size_t& i1, size_t& i2) {
  const auto score = [&](size_t i) {
    char c = needle[i];
    if (fold && c >= 'a' && c <= 'z') {  // a letter in either case: the commoner case counts
      const int lo = kRank.rank[uint8_t(c)], up = kRank.rank[uint8_t(c - 'a' + 'A')];
      return std::max(lo, up);
    }
    return int(kRank.rank[uint8_t(c)]);
  };
  i1 = 0;
  i2 = needle.size() - 1;
  for (size_t i = 1; i < needle.size(); i++)
    if (score(i) < score(i2)) i2 = i;
}

// Tests sixteen candidate positions at a time against two bytes of the needle
// (see anchors()), and compares the whole needle only where both match. SSE2 is
// part of x86-64 itself, so this needs no flags and no dispatch. With `kFold`
// letters match in either case: the needle is lower-case, and a hay byte OR
// 0x20 equals a lower-case letter only when it is that letter.
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
  if (!kFold && k == 1) {
    const void* p = memchr(h + from, nd[0], n - from);
    return p ? size_t(static_cast<const char*>(p) - h) : std::string_view::npos;
  }
  size_t i = from;
#if defined(__SSE2__)
  if (!kFold && k >= 2) {
    // Exact: memchr to the next first byte, which crosses a blob without one
    // (base64, a long run of prose) at full speed, then sixteen positions from
    // there against the second anchor, which handles a stretch where the first
    // byte is everywhere (JSON's quotes) without a memchr call per hit.
    size_t i1, i2;
    anchors(needle, false, i1, i2);
    const __m128i c1 = _mm_set1_epi8(nd[i1]), c2 = _mm_set1_epi8(nd[i2]);
    // memchr is called only after a block holding no first byte at all, so a
    // stretch dense with it (JSON's quotes) stays in the vector loop.
    const size_t last = n - k;  // the last possible start
    bool skip = true;
    while (i <= last) {
      if (skip) {
        const void* q = memchr(h + i, nd[0], last - i + 1);
        if (!q) return std::string_view::npos;
        i = size_t(static_cast<const char*>(q) - h);
      }
      if (i + k - 1 + 16 > n) break;  // too near the end for a block: the loop below
      const __m128i a = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i + i1)), c1);
      const __m128i b = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i + i2)), c2);
      skip = _mm_movemask_epi8(a) == 0;
      unsigned mask = unsigned(_mm_movemask_epi8(_mm_and_si128(a, b)));
      while (mask) {
        const size_t at = i + size_t(__builtin_ctz(mask));
        if (match(at)) return at;
        mask &= mask - 1;
      }
      i += 16;
    }
  }
  if (kFold && k >= 2) {
    size_t i1, i2;
    anchors(needle, kFold, i1, i2);
    const __m128i c1 = _mm_set1_epi8(nd[i1]), c2 = _mm_set1_epi8(nd[i2]);
    const __m128i f1 = _mm_set1_epi8(kFold && letter(nd[i1]) ? 0x20 : 0);
    const __m128i f2 = _mm_set1_epi8(kFold && letter(nd[i2]) ? 0x20 : 0);
    for (; i + k - 1 + 16 <= n; i += 16) {
      __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i + i1));
      __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h + i + i2));
      if constexpr (kFold) {
        a = _mm_or_si128(a, f1);
        b = _mm_or_si128(b, f2);
      }
      unsigned mask = unsigned(_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(a, c1), _mm_cmpeq_epi8(b, c2))));
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
  return find_impl<false>(hay, needle, from);
}

size_t find_folded(std::string_view hay, std::string_view needle, size_t from) {
  return find_impl<true>(hay, needle, from);
}

}  // namespace mico::text
