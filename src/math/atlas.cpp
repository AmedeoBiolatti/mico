#include "math/atlas.h"
#include "math/deflate.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

// The atlas is linked in whole, zlib-compressed: it halves, and is inflated
// the first time an equation is drawn. An assembler directive rather than a
// generated source file: a 300 KB array literal costs the compiler seconds on
// every build, the directive costs nothing.
extern "C" const unsigned char mico_math_atlas[];
extern "C" const unsigned char mico_math_atlas_end[];
asm(".section .rodata\n"
    ".global mico_math_atlas\n"
    ".global mico_math_atlas_end\n"
    ".balign 16\n"
    "mico_math_atlas:\n"
    ".incbin \"" MICO_MATH_ATLAS "\"\n"
    "mico_math_atlas_end:\n"
    ".previous\n");

namespace mico::math {
namespace {

constexpr size_t kHeader = 16;
constexpr size_t kRecord = 26;

struct Atlas {
  std::vector<uint8_t> raw;           // the inflated atlas; `data` points into it,
                                      // and a move keeps the buffer where it is
  int master = 64;
  std::vector<Glyph> glyphs;          // sorted by codepoint
  std::vector<uint32_t> offsets;      // into the RLE coverage
  const unsigned char* data = nullptr;
  size_t data_len = 0;
  std::vector<std::unique_ptr<uint32_t[]>> sats;
};

template <class T>
T rd(const unsigned char* p) {
  T v;
  memcpy(&v, p, sizeof v);
  return v;
}

const Atlas& atlas() {
  static const Atlas a = [] {
    Atlas a;
    if (!zlib_inflate(mico_math_atlas, size_t(mico_math_atlas_end - mico_math_atlas), a.raw)) return a;
    const unsigned char* p = a.raw.data();
    const size_t len = a.raw.size();
    if (len < kHeader || memcmp(p, "MATL", 4) != 0) return a;
    a.master = int(rd<uint32_t>(p + 8));
    const uint32_t n = rd<uint32_t>(p + 12);
    if (kHeader + size_t(n) * kRecord > len) return a;
    a.glyphs.reserve(n);
    a.offsets.reserve(n);
    const float em = 1000.f;
    for (uint32_t i = 0; i < n; i++) {
      const unsigned char* r = p + kHeader + size_t(i) * kRecord;
      Glyph g;
      g.cp = rd<uint32_t>(r);
      g.adv = float(rd<int16_t>(r + 4)) / em;
      g.x0 = float(rd<int16_t>(r + 6)) / em;
      g.y0 = float(rd<int16_t>(r + 8)) / em;
      g.x1 = float(rd<int16_t>(r + 10)) / em;
      g.y1 = float(rd<int16_t>(r + 12)) / em;
      g.bw = rd<uint16_t>(r + 14);
      g.bh = rd<uint16_t>(r + 16);
      g.left = rd<int16_t>(r + 18);
      g.top = rd<int16_t>(r + 20);
      g.index = i;
      // The font's own box spans the advance, not the ink; the bitmap is the
      // ink, to a master pixel, and italic correction is read off it.
      if (g.bw > 0 && g.bh > 0) {
        const float m = float(a.master);
        g.x0 = float(g.left) / m;
        g.x1 = float(g.left + g.bw) / m;
        g.y1 = float(-g.top) / m;
        g.y0 = float(-(g.top + g.bh)) / m;
      }
      a.glyphs.push_back(g);
      a.offsets.push_back(rd<uint32_t>(r + 22));
    }
    a.data = p + kHeader + size_t(n) * kRecord;
    a.data_len = len - (kHeader + size_t(n) * kRecord);
    a.sats.resize(n);
    return a;
  }();
  return a;
}

}  // namespace

const Glyph* glyph(char32_t cp) {
  const Atlas& a = atlas();
  auto it = std::lower_bound(a.glyphs.begin(), a.glyphs.end(), cp,
                             [](const Glyph& g, char32_t c) { return g.cp < c; });
  return it != a.glyphs.end() && it->cp == cp ? &*it : nullptr;
}

int master_px() { return atlas().master; }

const uint32_t* sat(const Glyph& g) {
  // The cache is filled lazily from a const atlas; the daemon is single-
  // threaded, so there is no one to race.
  Atlas& a = const_cast<Atlas&>(atlas());
  auto& slot = a.sats[g.index];
  if (slot) return slot.get();

  const size_t w = size_t(g.bw), h = size_t(g.bh);
  std::vector<uint8_t> cov(w * h, 0);
  const unsigned char* p = a.data + a.offsets[g.index];
  const unsigned char* end = a.data + a.data_len;
  for (size_t i = 0; i < cov.size() && p < end; p++) {
    if (*p & 0x80) i += size_t(*p & 0x7F) + 1;
    else cov[i++] = *p;
  }

  slot.reset(new uint32_t[(w + 1) * (h + 1)]());
  uint32_t* s = slot.get();
  for (size_t y = 0; y < h; y++) {
    uint32_t run = 0;
    for (size_t x = 0; x < w; x++) {
      run += cov[y * w + x];
      s[(y + 1) * (w + 1) + x + 1] = s[y * (w + 1) + x + 1] + run;
    }
  }
  return s;
}

}  // namespace mico::math
