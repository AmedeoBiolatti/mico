#include "math/sixel.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "math/math.h"
#include "math/picture.h"
#include "term/encoder.h"

namespace mico::math {
namespace {

constexpr int kLevels = 16;  // coverage steps: sixel has a palette, not alpha
constexpr int kMaxTile = 1000;

void append_int(std::string& o, int v) {
  char buf[16];
  const int n = snprintf(buf, sizeof buf, "%d", v);
  o.append(buf, size_t(n));
}

// A run of one sixel character, compressed when it pays.
void append_run(std::string& o, char ch, int n) {
  if (n <= 0) return;
  if (n >= 4) {
    o += '!';
    append_int(o, n);
    o += ch;
    return;
  }
  o.append(size_t(n), ch);
}

}  // namespace

namespace {

// A picture of palette indices, as sixel: a band of six rows at a time, one
// pass over the band per colour in it.
std::string encode(const std::vector<uint8_t>& idx, int w, int h, const std::vector<Color>& palette) {
  std::string o;
  // P2=1: pixels no colour sets stay as they were. Every pixel is set here,
  // so this only keeps terminals from filling past the last band. The raster
  // attributes ask for square pixels.
  o += "\x1bP0;1;0q\"1;1;";
  append_int(o, w);
  o += ';';
  append_int(o, h);
  for (size_t k = 0; k < palette.size(); k++) {
    const auto pct = [&](int shift) { return (((palette[k] >> shift) & 0xFF) * 100 + 127) / 255; };
    o += '#';
    append_int(o, int(k));
    o += ";2;";
    append_int(o, pct(16));
    o += ';';
    append_int(o, pct(8));
    o += ';';
    append_int(o, pct(0));
  }
  // One pass over a band fills every colour's row of sixel bits and the span
  // it covers; each colour then writes only its span. Scanning the whole
  // width once per colour made an anti-aliased chart, a hundred-odd colours,
  // cost a hundred passes.
  const size_t nc = palette.size();
  std::vector<uint8_t> bits(nc * size_t(w), 0);
  std::vector<int> lo(nc, w), hi(nc, -1);
  std::vector<uint16_t> seen;
  seen.reserve(nc);
  for (int band = 0; band < h; band += 6) {
    const int rows = std::min(6, h - band);
    seen.clear();
    for (int i = 0; i < rows; i++) {
      const uint8_t* row = idx.data() + size_t(band + i) * size_t(w);
      const uint8_t bit = uint8_t(1 << i);
      // A run of one colour at a time: its span is updated once per run.
      for (int x = 0; x < w;) {
        const uint8_t k = row[x];
        int e = x + 1;
        while (e < w && row[e] == k) e++;
        if (hi[k] < 0) seen.push_back(k);
        lo[k] = std::min(lo[k], x);
        hi[k] = std::max(hi[k], e - 1);
        uint8_t* line = bits.data() + size_t(k) * size_t(w);
        for (int j = x; j < e; j++) line[j] |= bit;
        x = e;
      }
    }
    std::sort(seen.begin(), seen.end());
    for (uint16_t k : seen) {
      uint8_t* line = bits.data() + size_t(k) * size_t(w);
      o += '#';
      append_int(o, int(k));
      append_run(o, '?', lo[k]);  // columns before its first
      for (int x = lo[k]; x <= hi[k];) {
        int n = 1;
        while (x + n <= hi[k] && line[x + n] == line[x]) n++;
        append_run(o, char(63 + line[x]), n);
        x += n;
      }
      o += '$';  // back to the band's start for the next colour
      std::fill(line + lo[k], line + hi[k] + 1, uint8_t(0));
      lo[k] = w;
      hi[k] = -1;
    }
    o += '-';
  }
  o += "\x1b\\";
  return o;
}

}  // namespace

std::string sixel(const uint8_t* alpha, int stride, int w, int h, Color fg, Color bg) {
  if (bg == kDefaultColor) bg = 0;
  std::vector<Color> palette(kLevels);
  for (int k = 0; k < kLevels; k++) {
    Color c = 0;
    for (int shift : {16, 8, 0}) {
      const int f = (fg >> shift) & 0xFF, b = (bg >> shift) & 0xFF;
      c |= Color(b + (f - b) * k / (kLevels - 1)) << shift;
    }
    palette[size_t(k)] = c;
  }
  std::vector<uint8_t> idx(size_t(w) * size_t(h));
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      idx[size_t(y) * size_t(w) + size_t(x)] =
          uint8_t((int(alpha[size_t(y) * size_t(stride) + size_t(x)]) * (kLevels - 1) + 127) / 255);
  return encode(idx, w, h, palette);
}

std::string sixel_rgba(const uint8_t* rgba, int stride, int w, int h, Color bg) {
  if (bg == kDefaultColor) bg = 0;
  const int br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
  // Composite onto the background, then 12-bit colour: few enough distinct
  // values to count, and a chart has only a handful of inks anyway.
  std::vector<uint16_t> key(size_t(w) * size_t(h));
  std::vector<uint32_t> count(4096, 0);
  // Each bucket's colour is the mean of what fell in it, not its centre:
  // the background, most of the picture, then comes out exact.
  std::vector<uint64_t> sum_r(4096, 0), sum_g(4096, 0), sum_b(4096, 0);
  // Most of a chart is transparent: those pixels are the background, and are
  // counted, not composited.
  const uint16_t bg_key = uint16_t((br >> 4) << 8 | (bgc >> 4) << 4 | (bb >> 4));
  uint32_t clear = 0;
  for (int y = 0; y < h; y++) {
    const uint8_t* row = rgba + size_t(y) * size_t(stride) * 4;
    uint16_t* out = key.data() + size_t(y) * size_t(w);
    for (int x = 0; x < w; x++) {
      const uint8_t* p = row + size_t(x) * 4;
      const int a = p[3];
      if (a == 0) {
        out[x] = bg_key;
        clear++;
        continue;
      }
      const int r = br + (p[0] - br) * a / 255, g = bgc + (p[1] - bgc) * a / 255, b = bb + (p[2] - bb) * a / 255;
      const uint16_t k = uint16_t((r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
      out[x] = k;
      count[k]++;
      sum_r[k] += uint64_t(r);
      sum_g[k] += uint64_t(g);
      sum_b[k] += uint64_t(b);
    }
  }
  count[bg_key] += clear;
  sum_r[bg_key] += uint64_t(br) * clear;
  sum_g[bg_key] += uint64_t(bgc) * clear;
  sum_b[bg_key] += uint64_t(bb) * clear;
  // The most used colours get the palette; the rest take their nearest.
  std::vector<uint16_t> used;
  for (uint16_t k = 0; k < 4096; k++)
    if (count[k]) used.push_back(k);
  std::sort(used.begin(), used.end(), [&](uint16_t a, uint16_t b) { return count[a] > count[b]; });
  const size_t n = std::min<size_t>(used.size(), 255);
  std::vector<Color> palette(n);
  std::vector<uint8_t> map(4096, 0);
  for (size_t i = 0; i < n; i++) {
    const uint16_t k = used[i];
    palette[i] = Color(sum_r[k] / count[k]) << 16 | Color(sum_g[k] / count[k]) << 8 |
                 Color(sum_b[k] / count[k]);
    map[k] = uint8_t(i);
  }
  for (size_t j = n; j < used.size(); j++) {
    const int r = (used[j] >> 8) & 15, g = (used[j] >> 4) & 15, b = used[j] & 15;
    int best = 0, bd = 1 << 30;
    for (size_t i = 0; i < n; i++) {
      const int dr = r - ((used[i] >> 8) & 15), dg = g - ((used[i] >> 4) & 15), db = b - (used[i] & 15);
      const int d = dr * dr + dg * dg + db * db;
      if (d < bd) { bd = d; best = int(i); }
    }
    map[used[j]] = uint8_t(best);
  }
  std::vector<uint8_t> idx(key.size());
  for (size_t i = 0; i < key.size(); i++) idx[i] = map[key[i]];
  if (palette.empty()) palette.push_back(bg);
  return encode(idx, w, h, palette);
}

namespace {

// Encoded pictures by what was cropped from which image, over what: a
// picture that is wholly on screen is the same sixel from one scroll step to
// the next, and encoding it again each time was most of a scroll's cost.
struct CacheKey {
  uint32_t id;
  int r0, c0, w, h;
  Color fg, bg;
  bool operator==(const CacheKey&) const = default;
};
struct Cached {
  CacheKey key;
  std::string data;
};
std::vector<Cached>& sixel_cache() {
  static std::vector<Cached> c;
  return c;
}

const std::string& encoded(const Image& im, const CacheKey& k, int cw, int ch) {
  auto& cache = sixel_cache();
  for (size_t i = 0; i < cache.size(); i++)
    if (cache[i].key == k) {
      std::rotate(cache.begin(), cache.begin() + ptrdiff_t(i), cache.begin() + ptrdiff_t(i) + 1);
      return cache.front().data;
    }
  static const std::string none;
  if (!pixels(im) || (im.rgba.empty() && im.alpha.size() < size_t(im.w) * size_t(im.h))) return none;
  const size_t corner = size_t(k.r0 * ch) * size_t(im.w) + size_t(k.c0 * cw);
  std::string data = im.rgba.empty()
                         ? sixel(im.alpha.data() + corner, im.w, k.w * cw, k.h * ch, k.fg, k.bg)
                         : sixel_rgba(im.rgba.data() + corner * 4, im.w, k.w * cw, k.h * ch, k.bg);
  constexpr size_t kEntries = 48, kBytes = 24u << 20;
  cache.insert(cache.begin(), Cached{k, std::move(data)});
  size_t total = 0, keep = 0;
  while (keep < cache.size() && keep < kEntries && total + cache[keep].data.size() <= kBytes)
    total += cache[keep++].data.size();
  cache.resize(std::max<size_t>(1, keep));
  return cache.front().data;
}

}  // namespace

void sixel_pass(const Surface& back, Surface& front, std::string& out) {
  if (front.width() != back.width() || front.height() != back.height()) return;
  std::vector<uint32_t> dirty;
  for (int y = 0; y < back.height(); y++)
    for (int x = 0; x < back.width(); x++) {
      const Cell& c = back.at(x, y);
      if (!(c.st.a & attr::kImage) || front.at(x, y) == c) continue;
      const uint32_t id = uint32_t(c.st.fg) & 0xFFFFFF;
      if (std::find(dirty.begin(), dirty.end(), id) == dirty.end()) dirty.push_back(id);
    }
  if (dirty.empty()) return;

  const size_t mark = out.size();
  out += "\x1b[?2026h";
  const Color fg = config().fg;
  for (uint32_t id : dirty) {
    const Image* im = find(id);
    // Where the image shows: its cells' bounding box, and which of its own
    // cells sits at the box's corner (read off any one cell: a menu may be
    // covering the corner itself).
    int x0 = back.width(), y0 = back.height(), x1 = -1, y1 = -1, ax = -1, ay = -1;
    for (int y = 0; y < back.height(); y++)
      for (int x = 0; x < back.width(); x++) {
        const Cell& c = back.at(x, y);
        if (!(c.st.a & attr::kImage) || (uint32_t(c.st.fg) & 0xFFFFFF) != id) continue;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
        if (ax < 0) { ax = x; ay = y; }
      }
    if (!im || x1 < 0 || im->cols <= 0 || im->rows <= 0) continue;
    const Cell& anchor = back.at(ax, ay);
    const int r0 = int(anchor.cp >> 16) - (ay - y0), c0 = int(anchor.cp & 0xFFFF) - (ax - x0);
    const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    if (r0 < 0 || c0 < 0 || r0 + bh > im->rows || c0 + bw > im->cols) continue;
    const int cw = im->w / im->cols, ch = im->h / im->rows;

    // In tiles of at most 1000 pixels a side, split on cell boundaries:
    // xterm drops a whole sixel larger than that (its maxGraphicSize).
    const int tw = std::max(1, kMaxTile / cw), th = std::max(1, kMaxTile / ch);
    for (int ty = 0; ty < bh; ty += th)
      for (int tx = 0; tx < bw; tx += tw) {
        const int w = std::min(tw, bw - tx), h = std::min(th, bh - ty);
        char cup[24];
        snprintf(cup, sizeof cup, "\x1b[%d;%dH", y0 + ty + 1, x0 + tx + 1);
        out += cup;
        out += encoded(*im, CacheKey{id, r0 + ty, c0 + tx, w, h, fg, anchor.st.bg}, cw, ch);
      }

    // The picture covered its whole box; whatever else is in the box (a menu
    // over it, text beside an inline equation) goes back on top.
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        const Cell& c = back.at(x, y);
        if ((c.st.a & attr::kImage) && (uint32_t(c.st.fg) & 0xFFFFFF) == id) {
          front.at(x, y) = c;
          continue;
        }
        if (c.st.a & attr::kImage) continue;  // another image: its own turn
        encode_cell_at(c, x, y, out);
        front.at(x, y) = c;
      }
  }
  if (out.size() == mark + 8) {
    out.resize(mark);
    return;
  }
  out += "\x1b[?2026l";
}

}  // namespace mico::math
