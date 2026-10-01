#include "math/layout.h"

#include <algorithm>
#include <cmath>

#include "math/atlas.h"
#include "base/text.h"

namespace mico::math {
namespace {

// Latin Modern Math's own constants, in em. Rounded: nobody can see the
// third decimal of an equation drawn twenty pixels high.
constexpr float kAxis = 0.25f;     // the line fractions and big operators centre on
constexpr float kRule = 0.04f;     // fraction bars, radical overbars
constexpr float kXHeight = 0.43f;

enum : int { kD = 0, kT = 1, kS = 2, kSS = 3 };

struct St {
  int s;
  bool cramped;
};

// TeX's inter-atom spacing, in thin (1), medium (2) and thick (3) spaces.
// Negative entries apply only in display and text style.
constexpr int8_t kSpacing[8][8] = {
    //Ord Op Bin Rel Open Close Punct Inner
    {0, 1, -2, -3, 0, 0, 0, -1},     // Ord
    {1, 1, 0, -3, 0, 0, 0, -1},      // Op
    {-2, -2, 0, 0, -2, 0, 0, -2},    // Bin
    {-3, -3, 0, 0, -3, 0, 0, -3},    // Rel
    {0, 0, 0, 0, 0, 0, 0, 0},        // Open
    {0, 1, -2, -3, 0, 0, 0, -1},     // Close
    {-1, -1, 0, -1, -1, -1, -1, -1}, // Punct
    {-1, 1, -2, -3, -1, 0, -1, -1},  // Inner
};

float spacing_mu(Cls a, Cls b, int style) {
  int v = kSpacing[int(a)][int(b)];
  if (v < 0) {
    if (style >= kS) return 0;
    v = -v;
  }
  return v == 1 ? 3.f : v == 2 ? 4.f : v == 3 ? 5.f : 0.f;
}

bool italic_cp(char32_t cp) {
  return (cp >= 0x1D434 && cp <= 0x1D467) || cp == 0x210E || (cp >= 0x1D6E2 && cp <= 0x1D71B) ||
         (cp >= 0x1D468 && cp <= 0x1D49B);
}

// A glyph the atlas lacks, drawn as the nearest one it has.
const Glyph* find_glyph(char32_t cp) {
  if (const Glyph* g = glyph(cp)) return g;
  char32_t alt = 0;
  switch (cp) {
    case 0x2A7D: alt = 0x2264; break;
    case 0x2A7E: alt = 0x2265; break;
    case 0x2AAF: alt = 0x2AAF; break;
    case 0x2A3F: alt = 0x2210; break;
    case 0x2213: alt = 0xB1; break;
    case 0x2016: alt = '|'; break;
    default: break;
  }
  if (cp >= 0x1D400 && cp < 0x1D6A4) {
    // Some math alphabet: fall back to the italic letter, then plain ASCII.
    const int i = int((cp - 0x1D400) % 52);
    const char32_t ascii = i < 26 ? char32_t('A' + i) : char32_t('a' + i - 26);
    if (const Glyph* g = glyph(ascii == 'h' ? 0x210E : 0x1D434 + char32_t(i))) return g;
    alt = ascii;
  }
  if (cp >= 0x1D6A8 && cp <= 0x1D7C9) alt = cp;  // Greek: no better guess
  if (alt && alt != cp)
    if (const Glyph* g = glyph(alt)) return g;
  return glyph('?');
}

void place(Box& dst, const Box& src, float dx, float dy) {
  for (Item it : src.items) {
    it.x += dx;
    it.y += dy;
    if (it.kind == Item::Line) {
      it.a += dx;
      it.b += dy;
    }
    dst.items.push_back(it);
  }
}

void line(Box& b, float x0, float y0, float x1, float y1, float t) {
  Item it{Item::Line};
  it.x = x0;
  it.y = y0;
  it.a = x1;
  it.b = y1;
  it.t = t;
  b.items.push_back(it);
}

void rect(Box& b, float x, float y, float w, float h) {
  Item it{Item::Rect};
  it.x = x;
  it.y = y;
  it.a = w;
  it.b = h;
  b.items.push_back(it);
}

void glyph_item(Box& b, const Glyph* g, float x, float y, float sx, float sy) {
  Item it{Item::Glyph};
  it.cp = g->cp;
  it.x = x;
  it.y = y;
  it.a = sx;
  it.b = sy;
  b.items.push_back(it);
}

// A horizontal arrow from x0 to x1 on y, for \overrightarrow and \xrightarrow.
void arrow(Box& b, float x0, float x1, float y, char32_t kind, float sc) {
  const float t = 0.04f * sc, hs = 0.14f * sc;
  line(b, x0, y, x1, y, t);
  if (kind == 0x2192 || kind == 0x2194) {
    line(b, x1 - hs, y + hs * 0.75f, x1, y, t);
    line(b, x1 - hs, y - hs * 0.75f, x1, y, t);
  }
  if (kind == 0x2190 || kind == 0x2194) {
    line(b, x0 + hs, y + hs * 0.75f, x0, y, t);
    line(b, x0 + hs, y - hs * 0.75f, x0, y, t);
  }
}

class Layout {
 public:
  explicit Layout(const LayoutOptions& o) : o_(o) {}

  Box hlist(const List& l, St st) {
    // The parser bounds nesting already; this is the backstop.
    if (depth_ > 128) return Box{};
    struct Depth {
      int& d;
      explicit Depth(int& x) : d(++x) {}
      ~Depth() { --d; }
    } guard(depth_);
    struct A {
      Box b;
      Cls c;
      bool glue;
    };
    std::vector<A> atoms;
    atoms.reserve(l.size());
    for (const Node& n : l) {
      if (n.k == Node::K::Style) {
        st.s = n.style;
        continue;
      }
      if (n.k == Node::K::Space) {
        Box b;
        b.w = n.amount * scale(st.s);
        atoms.push_back({std::move(b), Cls::Ord, true});
        continue;
      }
      Cls c = n.cls;
      Box b = node(n, st, &c);
      atoms.push_back({std::move(b), c, false});
    }

    // A binary operator with nothing to operate on is an ordinary symbol:
    // the minus in "-x", the plus in "(+)".
    int prev = -1;
    for (size_t i = 0; i < atoms.size(); i++) {
      if (atoms[i].glue) continue;
      Cls& c = atoms[i].c;
      if (c == Cls::Bin) {
        if (prev < 0) c = Cls::Ord;
        else {
          const Cls p = atoms[size_t(prev)].c;
          if (p == Cls::Bin || p == Cls::Op || p == Cls::Rel || p == Cls::Open || p == Cls::Punct)
            c = Cls::Ord;
        }
      }
      if ((c == Cls::Rel || c == Cls::Close || c == Cls::Punct) && prev >= 0 &&
          atoms[size_t(prev)].c == Cls::Bin)
        atoms[size_t(prev)].c = Cls::Ord;
      prev = int(i);
    }
    if (prev >= 0 && atoms[size_t(prev)].c == Cls::Bin) atoms[size_t(prev)].c = Cls::Ord;

    Box out;
    const float mu = scale(st.s) / 18.f;
    float x = 0;
    prev = -1;
    for (size_t i = 0; i < atoms.size(); i++) {
      const A& a = atoms[i];
      if (!a.glue && prev >= 0) x += spacing_mu(atoms[size_t(prev)].c, a.c, st.s) * mu;
      place(out, a.b, x, 0);
      out.h = std::max(out.h, a.b.h);
      out.d = std::max(out.d, a.b.d);
      x += a.b.w;
      if (!a.glue) prev = int(i);
    }
    out.w = x;
    if (atoms.size() == 1) out.ic = atoms[0].b.ic;
    return out;
  }

 private:
  float scale(int s) const {
    if (s <= kT) return 1.f;
    if (s == kS) return std::min(1.f, std::max(0.72f, o_.min_scale * 1.12f));
    return std::min(1.f, std::max(0.58f, o_.min_scale));
  }
  static int sup_style(int s) { return s <= kT ? kS : kSS; }
  static int frac_style(int s) { return s == kD ? kT : s == kT ? kS : kSS; }

  Box glyph_box(char32_t cp, float sc) {
    Box b;
    const Glyph* g = find_glyph(cp);
    if (!g) {
      b.w = 0.5f * sc;
      return b;
    }
    b.w = g->adv * sc;
    b.h = std::max(0.f, g->y1) * sc;
    b.d = std::max(0.f, -g->y0) * sc;
    b.ic = std::max(0.f, g->x1 - g->adv) * sc;
    glyph_item(b, g, 0, 0, sc, sc);
    return b;
  }

  Box text_box(const Node& n, St st) {
    const float sc = scale(st.s);
    Box b;
    float x = 0;
    for (size_t i = 0; i < n.text.size();) {
      const char32_t cp = text::decode(n.text, i);
      if (cp == ' ') {
        x += 0.333f * sc;
        continue;
      }
      Box g = glyph_box(letter(cp, n.font), sc);
      place(b, g, x, 0);
      b.h = std::max(b.h, g.h);
      b.d = std::max(b.d, g.d);
      x += g.w;
    }
    b.w = x;
    return b;
  }

  Box node(const Node& n, St st, Cls* cls) {
    const float sc = scale(st.s);
    switch (n.k) {
      case Node::K::Sym: return glyph_box(n.cp, sc);
      case Node::K::Text: return text_box(n, st);
      case Node::K::Group: return hlist(n.kids[0], st);
      case Node::K::Frac: return frac(n, st);
      case Node::K::Sqrt: return sqrt(n, st);
      case Node::K::Scripts: return scripts(n, st, cls);
      case Node::K::BigOp: return bigop(n.cp, st);
      case Node::K::Delim: return delim(n, st);
      case Node::K::Big: {
        static constexpr float kSizes[] = {1.f, 1.2f, 1.6f, 2.1f, 2.6f};
        return make_delim(n.cp, kSizes[std::min<int>(n.size, 4)] * sc, sc);
      }
      case Node::K::Array: return array(n, st);
      case Node::K::Accent: return accent(n, st);
      case Node::K::Rule: return rule(n, st);
      case Node::K::Brace: return brace(n, st);
      case Node::K::Stack: return stack(n, st);
      case Node::K::Boxed: return boxed(n, st);
      case Node::K::Phantom: {
        Box b = hlist(n.kids[0], st);
        b.items.clear();
        return b;
      }
      case Node::K::Not: {
        Box b = hlist(n.kids[0], st);
        const float cx = b.w / 2, a = kAxis * sc;
        line(b, cx - 0.13f * sc, a - 0.42f * sc, cx + 0.13f * sc, a + 0.42f * sc, 0.04f * sc);
        b.h = std::max(b.h, a + 0.42f * sc);
        b.d = std::max(b.d, 0.42f * sc - a);
        return b;
      }
      case Node::K::Space:
      case Node::K::Style: break;
    }
    return Box{};
  }

  // A delimiter at least `size` tall, centred on the axis: the glyph itself
  // when it is big enough, scaled a little when it nearly is, and assembled
  // from top, extender and bottom pieces beyond that.
  Box make_delim(char32_t cp, float size, float sc) {
    Box b;
    if (!cp) {
      b.w = 0.12f * sc;
      return b;
    }
    const Glyph* g = find_glyph(cp);
    if (!g) return b;
    const float axis = kAxis * sc;
    const float nh = (g->y1 - g->y0) * sc;
    if (size <= nh * 1.05f) return glyph_box(cp, sc);

    struct Pieces {
      char32_t cp, top, ext, mid, bot;
    };
    static constexpr Pieces kPieces[] = {
        {'(', 0x239B, 0x239C, 0, 0x239D},       {')', 0x239E, 0x239F, 0, 0x23A0},
        {'[', 0x23A1, 0x23A2, 0, 0x23A3},       {']', 0x23A4, 0x23A5, 0, 0x23A6},
        {'{', 0x23A7, 0x23AA, 0x23A8, 0x23A9},  {'}', 0x23AB, 0x23AA, 0x23AC, 0x23AD},
        {0x2308, 0x23A1, 0x23A2, 0, 0x23A2},    {0x2309, 0x23A4, 0x23A5, 0, 0x23A5},
        {0x230A, 0x23A2, 0x23A2, 0, 0x23A3},    {0x230B, 0x23A5, 0x23A5, 0, 0x23A6},
    };
    const Pieces* pc = nullptr;
    for (const Pieces& p : kPieces)
      if (p.cp == cp) pc = &p;
    const Glyph *top = pc ? glyph(pc->top) : nullptr, *ext = pc ? glyph(pc->ext) : nullptr,
                *bot = pc ? glyph(pc->bot) : nullptr, *mid = pc && pc->mid ? glyph(pc->mid) : nullptr;
    const float y_top = axis + size / 2, y_bot = axis - size / 2;
    b.h = y_top;
    b.d = -y_bot;

    if (top && ext && bot && size > nh * 1.5f) {
      const float th = (top->y1 - top->y0) * sc, bh = (bot->y1 - bot->y0) * sc;
      const float mh = mid ? (mid->y1 - mid->y0) * sc : 0;
      const float fixed = th + bh + mh;
      const float sy = size < fixed ? size / fixed : 1.f;
      glyph_item(b, top, 0, y_top - top->y1 * sc * sy, sc, sc * sy);
      glyph_item(b, bot, 0, y_bot - bot->y0 * sc * sy, sc, sc * sy);
      std::vector<std::pair<float, float>> gaps;
      if (mid) {
        glyph_item(b, mid, 0, axis - (mid->y0 + mid->y1) / 2 * sc * sy, sc, sc * sy);
        gaps.push_back({y_bot + bh * sy, axis - mh * sy / 2});
        gaps.push_back({axis + mh * sy / 2, y_top - th * sy});
      } else {
        gaps.push_back({y_bot + bh * sy, y_top - th * sy});
      }
      // Extenders are straight, so stretching one loses nothing. A hair of
      // overlap keeps a seam from showing between the pieces.
      const float eh = (ext->y1 - ext->y0) * sc, o = 0.02f * sc;
      for (auto [lo, hi] : gaps) {
        if (hi - lo <= 0 || eh <= 0) continue;
        const float sye = (hi - lo + 2 * o) / eh;
        glyph_item(b, ext, 0, (lo - o) - ext->y0 * sc * sye, sc, sc * sye);
      }
      b.w = top->adv * sc;
      return b;
    }

    // Scale the glyph itself. Straight bars only get taller; curved ones
    // widen a little so their strokes do not look pinched.
    const float sy = size / nh;
    const bool bar = cp == '|' || cp == 0x2016 || cp == 0x2223 || cp == 0x2225;
    const float sx = bar ? 1.f : std::min(1.f + (sy - 1.f) * 0.25f, 1.5f);
    const float center = (g->y0 + g->y1) / 2 * sc * sy;
    glyph_item(b, g, 0, axis - center, sc * sx, sc * sy);
    b.w = g->adv * sc * sx;
    return b;
  }

  Box wrap_delims(const Box& inner, char32_t open, char32_t close, float sc, float pad = 0) {
    const float axis = kAxis * sc;
    const float delta = std::max(inner.h - axis, inner.d + axis);
    const float size = std::max(2 * delta * 0.901f, 2 * delta - 0.5f * sc);
    Box l = make_delim(open, size, sc), r = make_delim(close, size, sc);
    Box out;
    place(out, l, 0, 0);
    place(out, inner, l.w + pad, 0);
    place(out, r, l.w + pad + inner.w + pad, 0);
    out.w = l.w + inner.w + r.w + 2 * pad;
    out.h = std::max({inner.h, l.h, r.h});
    out.d = std::max({inner.d, l.d, r.d});
    return out;
  }

  Box frac(const Node& n, St st) {
    const int cur = n.style >= 0 ? n.style : st.s;
    const float sc = scale(cur);
    const bool roomy = cur == kT && o_.roomy_fractions && n.style < 0;
    const int fs = roomy ? kT : frac_style(cur);
    Box num = hlist(n.kids[0], {fs, st.cramped});
    Box den = hlist(n.kids[1], {fs, true});
    const bool disp = cur == kD || roomy;
    const float axis = kAxis * sc, th = n.rule ? kRule * sc : 0;
    float u = (disp ? 0.677f : 0.394f) * sc, v = (disp ? 0.686f : 0.345f) * sc;
    if (n.rule) {
      const float gap = (disp ? 0.12f : 0.06f) * sc;
      if ((u - num.d) - (axis + th / 2) < gap) u = gap + axis + th / 2 + num.d;
      if ((axis - th / 2) - (den.h - v) < gap) v = gap - axis + th / 2 + den.h;
    } else {
      const float clr = (disp ? 0.28f : 0.12f) * sc;
      const float gap = (u - num.d) - (den.h - v);
      if (gap < clr) {
        u += (clr - gap) / 2;
        v += (clr - gap) / 2;
      }
    }
    const float pad = 0.1f * sc, inner = std::max(num.w, den.w);
    Box b;
    b.w = inner + 2 * pad;
    place(b, num, pad + (inner - num.w) / 2, u);
    place(b, den, pad + (inner - den.w) / 2, -v);
    if (n.rule) rect(b, pad, axis - th / 2, inner, th);
    b.h = std::max(u + num.h, axis + th);
    b.d = std::max(v + den.d, 0.f);
    if (n.open || n.close) return wrap_delims(b, n.open, n.close, sc);
    return b;
  }

  Box sqrt(const Node& n, St st) {
    const float sc = scale(st.s);
    Box body = hlist(n.kids[0], {st.s, true});
    const float th = kRule * sc;
    const float gap = (st.s == kD ? 0.148f : 0.07f) * sc;
    const float top = body.h + gap + th;  // top of the overbar
    const float total = std::max(0.9f * sc, top + body.d + 0.12f * sc);
    const float yb = top - total;          // where the radical's point reaches down to
    const float hook = std::min(total, 1.0f * sc);
    const float ws = (0.56f + 0.06f * std::min(2.f, std::max(0.f, total / sc - 1.f))) * sc;

    float ox = 0;
    Box ix;
    const bool has_index = !n.kids[1].empty();
    if (has_index) {
      ix = hlist(n.kids[1], {kSS, true});
      ox = std::max(0.f, 0.1f * sc + ix.w - 0.45f * ws);
    }

    Box b;
    // The radical sign, stroked: a short tick up, a heavy downstroke to the
    // point, and a thin line up to the overbar. Drawn rather than scaled from
    // the font's glyph, so it is right at any height.
    line(b, ox + 0.02f * ws, yb + 0.50f * hook, ox + 0.22f * ws, yb + 0.58f * hook, 0.04f * sc);
    line(b, ox + 0.22f * ws, yb + 0.58f * hook, ox + 0.50f * ws, yb + 0.02f * sc, 0.085f * sc);
    line(b, ox + 0.50f * ws, yb + 0.02f * sc, ox + ws, top - th / 2, 0.042f * sc);
    const float bx = ox + ws + 0.05f * sc;
    rect(b, ox + ws - 0.01f * sc, top - th, body.w + 0.1f * sc + 0.01f * sc, th);
    place(b, body, bx, 0);
    b.w = bx + body.w + 0.1f * sc;
    b.h = top + 0.04f * sc;
    b.d = std::max(body.d, -yb);
    if (has_index) {
      const float iy = yb + 0.6f * total;
      place(b, ix, 0.1f * sc + std::max(0.f, 0.45f * ws - 0.1f * sc - ix.w) , iy);
      b.h = std::max(b.h, iy + ix.h);
    }
    return b;
  }

  Box bigop(char32_t cp, St st) {
    const float sc = scale(st.s);
    const Glyph* g = find_glyph(cp);
    Box b;
    if (!g) return b;
    const bool integral = cp >= 0x222B && cp <= 0x2233;
    const float k = st.s == kD ? (integral ? 1.9f : 1.45f) : 1.f;
    const float s = sc * k;
    const float pen = kAxis * sc - (g->y0 + g->y1) / 2 * s;
    glyph_item(b, g, 0, pen, s, s);
    b.w = g->adv * s;
    b.h = g->y1 * s + pen;
    b.d = -(g->y0 * s + pen);
    b.ic = std::max(0.f, g->x1 - g->adv) * s;
    return b;
  }

  bool takes_limits(const List& base, int style) const {
    if (base.size() != 1) return false;
    const Node& op = base[0];
    if (op.k == Node::K::Brace) return true;
    if (op.k == Node::K::BigOp || (op.k == Node::K::Text && op.cls == Cls::Op))
      return op.limits == 1 || (op.limits == -1 && style == kD);
    return false;
  }

  Box scripts(const Node& n, St st, Cls* cls) {
    const List& base = n.kids[0];
    const bool has_sup = !n.kids[1].empty(), has_sub = !n.kids[2].empty();
    const float sc = scale(st.s);
    const int ss = sup_style(st.s);
    Box b = hlist(base, st);
    Box sup = has_sup ? hlist(n.kids[1], {ss, st.cramped}) : Box{};
    Box sub = has_sub ? hlist(n.kids[2], {ss, true}) : Box{};
    *cls = base.size() == 1 ? base[0].cls : Cls::Ord;

    if (takes_limits(base, st.s)) {
      const float w = std::max({b.w, sup.w, sub.w});
      Box out;
      out.w = w;
      place(out, b, (w - b.w) / 2, 0);
      out.h = b.h;
      out.d = b.d;
      if (has_sup) {
        const float y = b.h + 0.12f * sc + sup.d;
        place(out, sup, (w - sup.w) / 2 + b.ic / 2, y);
        out.h = y + sup.h + 0.05f * sc;
      }
      if (has_sub) {
        const float y = -(b.d + 0.1f * sc + sub.h);
        place(out, sub, (w - sub.w) / 2 - b.ic / 2, y);
        out.d = -y + sub.d + 0.05f * sc;
      }
      *cls = Cls::Op;
      return out;
    }

    const bool single = base.size() == 1 && base[0].k == Node::K::Sym;
    const float ssc = scale(ss);
    float u = single ? 0 : b.h - 0.25f * ssc;
    float v = single ? 0 : b.d + 0.2f * ssc;
    const float sup_up = (st.cramped ? 0.289f : 0.363f) * sc;
    if (has_sup) u = std::max({u, sup_up, sup.d + 0.108f * sc});
    if (has_sub && !has_sup) v = std::max({v, 0.247f * sc, sub.h - 0.344f * sc});
    if (has_sub && has_sup) {
      v = std::max(v, 0.247f * sc);
      const float gap = (u - sup.d) - (sub.h - v);
      if (gap < 0.2f * sc) v += 0.2f * sc - gap;
      const float psi = 0.344f * sc - (u - sup.d);
      if (psi > 0) {
        u += psi;
        v -= psi;
      }
    }
    Box out;
    place(out, b, 0, 0);
    out.h = b.h;
    out.d = b.d;
    float w = b.w;
    if (has_sup) {
      place(out, sup, b.w + b.ic, u);
      out.h = std::max(out.h, u + sup.h);
      w = std::max(w, b.w + b.ic + sup.w);
    }
    if (has_sub) {
      // Under an integral's slant the subscript tucks in a little.
      const float tuck = base.size() == 1 && base[0].k == Node::K::BigOp ? b.ic * 0.6f : 0.f;
      place(out, sub, b.w - tuck, -v);
      out.d = std::max(out.d, v + sub.d);
      w = std::max(w, b.w - tuck + sub.w);
    }
    out.w = w + 0.056f * sc;
    return out;
  }

  Box delim(const Node& n, St st) {
    const float sc = scale(st.s);
    std::vector<Box> segs;
    float h = 0, d = 0;
    for (const List& k : n.kids) {
      segs.push_back(hlist(k, st));
      h = std::max(h, segs.back().h);
      d = std::max(d, segs.back().d);
    }
    const float axis = kAxis * sc;
    const float delta = std::max(h - axis, d + axis);
    const float size = std::max(2 * delta * 0.901f, 2 * delta - 0.5f * sc);
    std::vector<char32_t> mids;
    for (size_t i = 0; i < n.text.size();) mids.push_back(text::decode(n.text, i));

    Box out;
    float x = 0;
    auto add = [&](const Box& b) {
      place(out, b, x, 0);
      x += b.w;
      out.h = std::max(out.h, b.h);
      out.d = std::max(out.d, b.d);
    };
    add(make_delim(n.open, size, sc));
    for (size_t i = 0; i < segs.size(); i++) {
      if (i > 0) {
        x += 0.17f * sc;
        add(make_delim(i - 1 < mids.size() ? mids[i - 1] : 0, size, sc));
        x += 0.17f * sc;
      }
      add(segs[i]);
    }
    add(make_delim(n.close, size, sc));
    out.w = x;
    return out;
  }

  Box array(const Node& n, St st) {
    const int cs = st.s >= kS ? std::max<int>(n.style, st.s) : std::max<int>(n.style, 0);
    const float sc = scale(cs), outer = scale(st.s);
    const size_t cols = std::max<size_t>(1, n.cols);
    const size_t rows = n.kids.size() / cols;
    std::vector<Box> cells;
    cells.reserve(n.kids.size());
    std::vector<float> wid(cols, 0), ht(rows, 0), dp(rows, 0);
    for (size_t r = 0; r < rows; r++) {
      for (size_t c = 0; c < cols; c++) {
        cells.push_back(hlist(n.kids[r * cols + c], {cs, false}));
        const Box& b = cells.back();
        wid[c] = std::max(wid[c], b.w);
        ht[r] = std::max(ht[r], b.h);
        dp[r] = std::max(dp[r], b.d);
      }
      // Struts: rows of short content still sit a line apart.
      ht[r] = std::max(ht[r], 0.75f * sc);
      dp[r] = std::max(dp[r], 0.3f * sc);
    }
    const bool aligned = n.text.find('r') != std::string::npos && n.text.find('l') != std::string::npos &&
                         !n.open && !n.close;
    const float gap = (n.style == kD ? 0.3f : 0.12f) * sc;
    std::vector<float> xs(cols, 0);
    float x = 0;
    for (size_t c = 0; c < cols; c++) {
      if (c > 0) {
        // An aligned block's columns pair up: no space inside a pair, a wide
        // one between pairs.
        if (aligned) x += (c % 2 == 0) ? 1.5f * sc : 0;
        else x += n.amount * sc;
      }
      xs[c] = x;
      x += wid[c];
    }
    float total = 0;
    for (size_t r = 0; r < rows; r++) total += ht[r] + dp[r] + (r ? gap : 0);

    Box inner;
    inner.w = x;
    const float top = kAxis * outer + total / 2;
    float y = top;
    for (size_t r = 0; r < rows; r++) {
      if (r) y -= gap;
      y -= ht[r];
      for (size_t c = 0; c < cols; c++) {
        const Box& b = cells[r * cols + c];
        const char al = c < n.text.size() ? n.text[c] : 'c';
        const float cx = al == 'l' ? xs[c] : al == 'r' ? xs[c] + wid[c] - b.w : xs[c] + (wid[c] - b.w) / 2;
        place(inner, b, cx, y);
      }
      y -= dp[r];
    }
    inner.h = top;
    inner.d = total - top;
    if (n.open || n.close) return wrap_delims(inner, n.open, n.close, outer, 0.1f * sc);
    return inner;
  }

  Box accent(const Node& n, St st) {
    const float sc = scale(st.s);
    Box base = hlist(n.kids[0], {st.s, true});
    const bool single = n.kids[0].size() == 1 && n.kids[0][0].k == Node::K::Sym;
    const float skew = single && italic_cp(n.kids[0][0].cp) ? 0.05f * sc + base.ic * 0.5f : 0.f;
    Box out;
    place(out, base, 0, 0);
    out.w = base.w;
    out.h = base.h;
    out.d = base.d;
    if (n.stretch_arrow) {
      const float y = n.under ? -(base.d + 0.14f * sc) : base.h + 0.16f * sc;
      arrow(out, 0, std::max(base.w, 0.5f * sc), y, n.cp, sc);
      if (n.under) out.d = -y + 0.14f * sc;
      else out.h = y + 0.14f * sc;
      out.w = std::max(base.w, 0.5f * sc);
      return out;
    }
    const Glyph* g = find_glyph(n.cp);
    if (!g) return out;
    float sx = sc, sy = sc;
    if (n.cp == 0x2192) {  // \vec: a small arrow, not a full-width one
      sx = 0.5f * sc;
      sy = 0.6f * sc;
    }
    const float ink_w = std::max(0.01f, g->x1 - g->x0);
    if (n.wide && base.w > ink_w * sc * 1.3f) {
      sx = std::min(base.w * 0.95f / ink_w, 6.f * sc);
      sy = sc * std::min(1.5f, 1.f + 0.08f * (sx / sc - 1.f));
    }
    // Accents sit on top of whatever they cover, clear of it.
    const float bottom = std::max(base.h, kXHeight * sc) + 0.06f * sc;
    const float pen_y = bottom - g->y0 * sy;
    const float pen_x = base.w / 2 + skew - (g->x0 + g->x1) / 2 * sx;
    glyph_item(out, g, pen_x, pen_y, sx, sy);
    out.h = std::max(out.h, pen_y + g->y1 * sy);
    return out;
  }

  Box rule(const Node& n, St st) {
    const float sc = scale(st.s);
    Box base = hlist(n.kids[0], {st.s, !n.under || st.cramped});
    const float th = kRule * sc, gap = 3 * th;
    Box out;
    place(out, base, 0, 0);
    out.w = base.w;
    out.h = base.h;
    out.d = base.d;
    if (n.under) {
      rect(out, 0, -(base.d + gap + th), base.w, th);
      out.d = base.d + gap + 2 * th;
    } else {
      rect(out, 0, base.h + gap, base.w, th);
      out.h = base.h + gap + 2 * th;
    }
    return out;
  }

  Box brace(const Node& n, St st) {
    const float sc = scale(st.s);
    Box base = hlist(n.kids[0], st);
    const float w = std::max(base.w, 0.5f * sc);
    const float hb = std::min(0.3f, 0.15f + 0.01f * w / sc) * sc;
    const float t = 0.04f * sc;
    Box out;
    place(out, base, (w - base.w) / 2, 0);
    out.w = w;
    out.h = base.h;
    out.d = base.d;
    // Ends curl toward what the brace covers, the point faces away from it.
    const float dir = n.under ? -1.f : 1.f;
    const float yb = n.under ? -(base.d + 0.1f * sc) : base.h + 0.1f * sc;
    const float ym = yb + dir * hb * 0.5f, yt = yb + dir * hb;
    const float c = w / 2, e = std::min(hb * 0.5f, w / 4);
    line(out, 0, yb, e, ym, t);
    line(out, e, ym, c - e, ym, t);
    line(out, c - e, ym, c, yt, t);
    line(out, c, yt, c + e, ym, t);
    line(out, c + e, ym, w - e, ym, t);
    line(out, w - e, ym, w, yb, t);
    if (n.under) out.d = -yt + 0.05f * sc;
    else out.h = yt + 0.05f * sc;
    return out;
  }

  Box stack(const Node& n, St st) {
    const float sc = scale(st.s);
    const int ss = sup_style(st.s);
    Box over = hlist(n.kids[1], {ss, st.cramped});
    Box under = hlist(n.kids[2], {ss, true});
    Box base;
    if (n.wide) {
      const float w = std::max(std::max(over.w, under.w) + 0.8f * sc, 1.2f * sc);
      const float y = kAxis * sc;
      arrow(base, 0.05f * sc, w - 0.05f * sc, y, n.cp, sc);
      base.w = w;
      base.h = y + 0.12f * sc;
      base.d = 0;
    } else {
      base = hlist(n.kids[0], st);
    }
    const float w = std::max({base.w, over.w, under.w});
    Box out;
    out.w = w;
    place(out, base, (w - base.w) / 2, 0);
    out.h = base.h;
    out.d = base.d;
    if (!n.kids[1].empty()) {
      const float y = base.h + 0.1f * sc + over.d;
      place(out, over, (w - over.w) / 2, y);
      out.h = y + over.h;
    }
    if (!n.kids[2].empty()) {
      const float y = -(base.d + 0.1f * sc + under.h);
      place(out, under, (w - under.w) / 2, y);
      out.d = -y + under.d;
    }
    return out;
  }

  Box boxed(const Node& n, St st) {
    const float sc = scale(st.s);
    Box inner = hlist(n.kids[0], st);
    const float pad = 0.25f * sc, t = 0.04f * sc;
    Box out;
    out.w = inner.w + 2 * pad;
    out.h = inner.h + pad;
    out.d = inner.d + pad;
    place(out, inner, pad, 0);
    rect(out, 0, -out.d, out.w, t);
    rect(out, 0, out.h - t, out.w, t);
    rect(out, 0, -out.d, t, out.h + out.d);
    rect(out, out.w - t, -out.d, t, out.h + out.d);
    return out;
  }

  LayoutOptions o_;
  int depth_ = 0;
};

}  // namespace

Box layout(const List& l, const LayoutOptions& opt) {
  Layout lay(opt);
  return lay.hlist(l, St{opt.display ? kD : kT, false});
}

}  // namespace mico::math
