#include "math/math.h"
#include "math/picture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include "math/atlas.h"
#include "math/deflate.h"
#include "math/layout.h"
#include "math/raster.h"
#include "math/tex.h"

namespace mico::math {
namespace {

struct State {
  Config cfg;
  uint64_t gen = 1;
  uint32_t next_id = 1;
  std::unordered_map<std::string, uint32_t> by_key;  // 0: drawn as Unicode instead
  std::unordered_map<uint32_t, Image> by_id;
  std::unordered_map<std::string, std::string> lineage;  // lineage -> its latest key
  std::vector<uint32_t> evicted;
  size_t bytes = 0;
};

State& state() {
  static State s;
  return s;
}

void drop_all(State& s) {
  for (auto& [id, im] : s.by_id) s.evicted.push_back(id);
  s.by_id.clear();
  s.by_key.clear();
  s.lineage.clear();
  s.bytes = 0;
  s.gen++;
}

// Ceiling on cached coverage. A chat with hundreds of equations stays far
// under it; past it everything is dropped and redrawn on demand. Pictures and
// charts do not count: they keep their PNG and their pixels come and go
// within the pixel budget (see pixels()), so a chat full of them never
// forces this, which would make every chat lay itself out again and redraw
// all its images, over and over.
constexpr size_t kBudget = 48u << 20;

// Larger than this, a chart is kept as a PNG too, and its pixels may go.
constexpr size_t kEncodeAbove = 256u << 10;

size_t cost(const Image& im) { return im.encoded ? 0 : im.alpha.size() + im.rgba.size(); }

}  // namespace

void configure(const Config& c) {
  State& s = state();
  if (c == s.cfg) return;
  s.cfg = c;
  drop_all(s);
}

const Config& config() { return state().cfg; }
uint64_t generation() { return state().gen; }

Image draw(std::string_view src, bool display, int cell_w, int cell_h, float scale) {
  Image im;
  im.src = std::string(src);
  im.display = display;
  cell_w = std::max(1, cell_w);
  cell_h = std::max(1, cell_h);
  // An em a little under the cell height matches a terminal font's x-height;
  // display math gets a touch more, the way a book sets it.
  const float em = float(cell_h) * (display ? 1.12f : 1.0f) * scale;
  LayoutOptions opt;
  opt.display = display;
  opt.min_scale = std::min(1.f, 11.f / em);
  opt.roomy_fractions = !display;
  const Box b = layout(parse(src), opt);

  const float pad = 1.f;
  const float wpx = std::max(1.f, b.w * em + 2 * pad);
  const float hpx = b.h * em, dpx = std::max(0.f, b.d * em);
  im.cols = std::max(1, int(std::ceil(wpx / float(cell_w))));
  float baseline;
  if (display) {
    const float need = hpx + dpx + 2 * pad;
    im.rows = std::max(1, int(std::ceil(need / float(cell_h))));
    baseline = (float(im.rows * cell_h) - (hpx + dpx)) / 2 + hpx;
  } else {
    // The text around inline math sits on a baseline about four-fifths down
    // its row. Rows are added above and below until the picture fits.
    const float base_off = std::round(float(cell_h) * 0.78f);
    const int above = std::max(0, int(std::ceil((hpx + pad - base_off) / float(cell_h))));
    const int below = std::max(0, int(std::ceil((dpx + pad - (float(cell_h) - base_off)) / float(cell_h))));
    im.rows = above + 1 + below;
    im.base_row = above;
    baseline = float(above * cell_h) + base_off;
  }
  // A runaway layout (thousands of rows) is cut off, not allocated.
  im.cols = std::min(im.cols, 512);
  im.rows = std::min(im.rows, 128);
  im.w = im.cols * cell_w;
  im.h = im.rows * cell_h;

  Canvas c(im.w, im.h);
  const float ox = display ? pad : pad;
  for (const Item& it : b.items) {
    switch (it.kind) {
      case Item::Glyph: draw_glyph(c, it, em, ox, baseline); break;
      case Item::Rect: draw_rect(c, it, em, ox, baseline); break;
      case Item::Line: draw_line(c, it, em, ox, baseline); break;
    }
  }
  im.alpha.resize(c.a.size());
  // Light ink on a dark panel loses its thin strokes to sRGB blending; a
  // gentle lift of the midtones gives them back. A table, not a pow() per
  // pixel: that was a third of drawing an equation.
  static const auto lift = [] {
    std::array<uint8_t, 1025> t{};
    for (size_t i = 0; i < t.size(); i++)
      t[i] = uint8_t(std::lround(std::pow(float(i) / 1024.f, 0.8f) * 255.f));
    return t;
  }();
  for (size_t i = 0; i < c.a.size(); i++)
    im.alpha[i] = lift[size_t(std::clamp(c.a[i], 0.f, 1.f) * 1024.f + 0.5f)];
  return im;
}

// Past these an equation is shown as Unicode: nobody reads a 60-row picture
// in a chat, and a pathological transcript must not cost a second of drawing.
constexpr size_t kMaxSource = 4096;
constexpr int kMaxRows = 48;

const Image* image(std::string_view src, bool display, int max_cols, bool zoomed) {
  State& s = state();
  if (!s.cfg.enabled || max_cols < 1 || src.size() > kMaxSource) return nullptr;
  if (s.bytes > kBudget) drop_all(s);

  std::string key;
  key.reserve(src.size() + 4);
  key += display ? 'D' : 'I';
  key += '0';
  key.append(src);

  if (!display) {
    // Inline math that reads fine as a line of Unicode stays text: it
    // matches the font around it and copies as characters.
    auto it = s.by_key.find(key);
    if (it == s.by_key.end()) {
      if (!needs_drawing(parse(src))) {
        s.by_key.emplace(key, 0);
        return nullptr;
      }
    } else if (it->second == 0) {
      return nullptr;
    }
  }

  // The image `key` names, drawn at `scale` the first time it is asked for.
  auto drawn = [&](float scale) {
    if (auto it = s.by_key.find(key); it != s.by_key.end()) return find(it->second);
    Image im = draw(src, display, s.cfg.cell_w, s.cfg.cell_h, scale);
    im.copy = display ? "$$" + im.src + "$$" : "$" + im.src + "$";
    const uint32_t id = im.id = s.next_id++;
    if (s.next_id >= 0xFFFFFF) s.next_id = 1;  // ids travel as a 24-bit colour
    s.bytes += im.alpha.size();
    s.by_id.emplace(id, std::move(im));
    s.by_key.emplace(key, id);
    return find(id);
  };

  // Display math too wide for the pane is tried smaller before giving up;
  // zoomed, it is tried larger first.
  static constexpr float kScales[] = {1.f, 0.82f, 0.68f};
  static constexpr float kZoomed[] = {2.f, 1.6f, 1.3f};
  if (zoomed && display) {
    key[0] = 'Z';
    for (int tier = 0; tier < 3; tier++) {
      key[1] = char('0' + tier);
      const Image* im = drawn(kZoomed[tier]);
      if (im && im->rows <= 2 * kMaxRows && im->cols <= max_cols) return im;
    }
    key[0] = 'D';
  }
  for (int tier = 0; tier < (display ? 3 : 1); tier++) {
    key[1] = char('0' + tier);
    const Image* im = drawn(kScales[tier]);
    if (im && im->rows > kMaxRows) return nullptr;
    if (im && im->cols <= max_cols) return im;
  }
  return nullptr;
}

const Image* cached(const std::string& key) {
  State& s = state();
  auto it = s.by_key.find(key);
  return it == s.by_key.end() ? nullptr : find(it->second);
}

const Image* store(const std::string& key, Image im, const std::string& lineage) {
  State& s = state();
  if (s.bytes > kBudget) drop_all(s);
  const Image* have = cached(key);
  if (!lineage.empty()) {
    auto l = s.lineage.find(lineage);
    if (l != s.lineage.end() && l->second != key) {
      if (auto k = s.by_key.find(l->second); k != s.by_key.end()) {
        if (auto b = s.by_id.find(k->second); b != s.by_id.end()) {
          s.bytes -= std::min(s.bytes, cost(b->second));
          s.evicted.push_back(b->first);
          s.by_id.erase(b);
        }
        s.by_key.erase(k);
      }
    }
    s.lineage[lineage] = key;
  }
  if (have) return have;
  const uint32_t id = im.id = s.next_id++;
  if (s.next_id >= 0xFFFFFF) s.next_id = 1;
  const bool encode = !im.encoded && im.rgba.size() > kEncodeAbove;
  if (encode) {
    im.encoded = std::make_shared<const std::string>(png(im, 0));
    im.fit_w = im.src_w = im.w;
    im.fit_h = im.src_h = im.h;
  }
  s.bytes += cost(im);
  s.by_id.emplace(id, std::move(im));
  s.by_key.emplace(key, id);
  const Image* stored = find(id);
  if (encode) pixels(*stored);  // its pixels, just drawn, join the others
  return stored;
}

const Image* find(uint32_t id) {
  State& s = state();
  auto it = s.by_id.find(id);
  return it == s.by_id.end() ? nullptr : &it->second;
}

void take_evicted(std::vector<uint32_t>& out) {
  State& s = state();
  out.insert(out.end(), s.evicted.begin(), s.evicted.end());
  s.evicted.clear();
}

std::string png(const Image& im, Color fg) {
  static uint32_t crc_table[256];
  static bool crc_ready = false;
  if (!crc_ready) {
    for (uint32_t n = 0; n < 256; n++) {
      uint32_t c = n;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_table[n] = c;
    }
    crc_ready = true;
  }
  const auto be32 = [](std::string& o, uint32_t v) {
    o.push_back(char(v >> 24));
    o.push_back(char(v >> 16));
    o.push_back(char(v >> 8));
    o.push_back(char(v));
  };
  std::string out("\x89PNG\r\n\x1a\n", 8);
  const auto chunk = [&](const char* type, const std::string& data) {
    be32(out, uint32_t(data.size()));
    std::string body(type, 4);
    body += data;
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : body) c = crc_table[(c ^ ch) & 0xFF] ^ (c >> 8);
    out += body;
    be32(out, c ^ 0xFFFFFFFFu);
  };
  std::string ihdr;
  be32(ihdr, uint32_t(im.w));
  be32(ihdr, uint32_t(im.h));
  ihdr += std::string("\x08\x06\x00\x00\x00", 5);
  chunk("IHDR", ihdr);

  std::string raw;
  raw.reserve(size_t(im.h) * (size_t(im.w) * 4 + 1));
  for (int y = 0; y < im.h; y++) {
    raw.push_back(0);
    for (int x = 0; x < im.w; x++) {
      const size_t i = size_t(y) * size_t(im.w) + size_t(x);
      if (!im.rgba.empty()) {
        raw.append(reinterpret_cast<const char*>(&im.rgba[i * 4]), 4);
        continue;
      }
      raw.push_back(char((fg >> 16) & 0xFF));
      raw.push_back(char((fg >> 8) & 0xFF));
      raw.push_back(char(fg & 0xFF));
      raw.push_back(char(im.alpha[i]));
    }
  }
  const std::string z = zlib_compress(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
  chunk("IDAT", z);
  chunk("IEND", {});
  return out;
}

}  // namespace mico::math
