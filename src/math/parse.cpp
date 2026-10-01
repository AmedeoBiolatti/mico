#include <cctype>
#include <cstdlib>
#include <cstring>

#include "math/tex.h"
#include "base/text.h"

namespace mico::math {
namespace {

struct Sym {
  std::string_view name;
  char32_t cp;
  Cls cls;
};

// Every command that is one glyph. Greek lowercase maps to the italic block,
// the way TeX sets it; capitals stay upright.
constexpr Sym kSyms[] = {
    // Greek
    {"alpha", 0x1D6FC, Cls::Ord}, {"beta", 0x1D6FD, Cls::Ord}, {"gamma", 0x1D6FE, Cls::Ord},
    {"delta", 0x1D6FF, Cls::Ord}, {"epsilon", 0x1D716, Cls::Ord}, {"varepsilon", 0x1D700, Cls::Ord},
    {"zeta", 0x1D701, Cls::Ord}, {"eta", 0x1D702, Cls::Ord}, {"theta", 0x1D703, Cls::Ord},
    {"vartheta", 0x1D717, Cls::Ord}, {"iota", 0x1D704, Cls::Ord}, {"kappa", 0x1D705, Cls::Ord},
    {"varkappa", 0x1D718, Cls::Ord}, {"lambda", 0x1D706, Cls::Ord}, {"mu", 0x1D707, Cls::Ord},
    {"nu", 0x1D708, Cls::Ord}, {"xi", 0x1D709, Cls::Ord}, {"omicron", 0x1D70A, Cls::Ord},
    {"pi", 0x1D70B, Cls::Ord}, {"varpi", 0x1D71B, Cls::Ord}, {"rho", 0x1D70C, Cls::Ord},
    {"varrho", 0x1D71A, Cls::Ord}, {"sigma", 0x1D70E, Cls::Ord}, {"varsigma", 0x1D70D, Cls::Ord},
    {"tau", 0x1D70F, Cls::Ord}, {"upsilon", 0x1D710, Cls::Ord}, {"phi", 0x1D719, Cls::Ord},
    {"varphi", 0x1D711, Cls::Ord}, {"chi", 0x1D712, Cls::Ord}, {"psi", 0x1D713, Cls::Ord},
    {"omega", 0x1D714, Cls::Ord},
    {"Gamma", 0x393, Cls::Ord}, {"Delta", 0x394, Cls::Ord}, {"Theta", 0x398, Cls::Ord},
    {"Lambda", 0x39B, Cls::Ord}, {"Xi", 0x39E, Cls::Ord}, {"Pi", 0x3A0, Cls::Ord},
    {"Sigma", 0x3A3, Cls::Ord}, {"Upsilon", 0x3A5, Cls::Ord}, {"Phi", 0x3A6, Cls::Ord},
    {"Psi", 0x3A8, Cls::Ord}, {"Omega", 0x3A9, Cls::Ord},
    {"varGamma", 0x1D6E4, Cls::Ord}, {"varDelta", 0x1D6E5, Cls::Ord}, {"varOmega", 0x1D6FA, Cls::Ord},
    // Binary operators
    {"pm", 0xB1, Cls::Bin}, {"mp", 0x2213, Cls::Bin}, {"times", 0xD7, Cls::Bin},
    {"div", 0xF7, Cls::Bin}, {"cdot", 0x22C5, Cls::Bin}, {"ast", 0x2217, Cls::Bin},
    {"star", 0x22C6, Cls::Bin}, {"circ", 0x2218, Cls::Bin}, {"bullet", 0x2219, Cls::Bin},
    {"cap", 0x2229, Cls::Bin}, {"cup", 0x222A, Cls::Bin}, {"sqcap", 0x2293, Cls::Bin},
    {"sqcup", 0x2294, Cls::Bin}, {"vee", 0x2228, Cls::Bin}, {"lor", 0x2228, Cls::Bin},
    {"wedge", 0x2227, Cls::Bin}, {"land", 0x2227, Cls::Bin}, {"setminus", 0x2216, Cls::Bin},
    {"smallsetminus", 0x2216, Cls::Bin}, {"oplus", 0x2295, Cls::Bin}, {"ominus", 0x2296, Cls::Bin},
    {"otimes", 0x2297, Cls::Bin}, {"oslash", 0x2298, Cls::Bin}, {"odot", 0x2299, Cls::Bin},
    {"dagger", 0x2020, Cls::Bin}, {"ddagger", 0x2021, Cls::Bin}, {"uplus", 0x228E, Cls::Bin},
    {"diamond", 0x22C4, Cls::Bin}, {"wr", 0x2240, Cls::Bin}, {"amalg", 0x2A3F, Cls::Bin},
    {"triangleleft", 0x25C1, Cls::Bin}, {"triangleright", 0x25B7, Cls::Bin},
    {"bigtriangleup", 0x25B3, Cls::Bin}, {"bigtriangledown", 0x25BD, Cls::Bin},
    {"cdotp", 0x22C5, Cls::Punct}, {"ldotp", '.', Cls::Punct},
    // Relations
    {"leq", 0x2264, Cls::Rel}, {"le", 0x2264, Cls::Rel}, {"geq", 0x2265, Cls::Rel},
    {"ge", 0x2265, Cls::Rel}, {"leqslant", 0x2A7D, Cls::Rel}, {"geqslant", 0x2A7E, Cls::Rel},
    {"neq", 0x2260, Cls::Rel}, {"ne", 0x2260, Cls::Rel}, {"approx", 0x2248, Cls::Rel},
    {"approxeq", 0x224A, Cls::Rel}, {"equiv", 0x2261, Cls::Rel}, {"sim", 0x223C, Cls::Rel},
    {"simeq", 0x2243, Cls::Rel}, {"cong", 0x2245, Cls::Rel}, {"propto", 0x221D, Cls::Rel},
    {"ll", 0x226A, Cls::Rel}, {"gg", 0x226B, Cls::Rel}, {"lesssim", 0x2272, Cls::Rel},
    {"gtrsim", 0x2273, Cls::Rel}, {"in", 0x2208, Cls::Rel}, {"notin", 0x2209, Cls::Rel},
    {"ni", 0x220B, Cls::Rel}, {"owns", 0x220B, Cls::Rel}, {"subset", 0x2282, Cls::Rel},
    {"supset", 0x2283, Cls::Rel}, {"subseteq", 0x2286, Cls::Rel}, {"supseteq", 0x2287, Cls::Rel},
    {"subsetneq", 0x228A, Cls::Rel}, {"supsetneq", 0x228B, Cls::Rel}, {"nsubseteq", 0x2288, Cls::Rel},
    {"sqsubset", 0x228F, Cls::Rel}, {"sqsupset", 0x2290, Cls::Rel}, {"sqsubseteq", 0x2291, Cls::Rel},
    {"sqsupseteq", 0x2292, Cls::Rel}, {"vdash", 0x22A2, Cls::Rel}, {"dashv", 0x22A3, Cls::Rel},
    {"models", 0x22A8, Cls::Rel}, {"perp", 0x22A5, Cls::Rel}, {"parallel", 0x2225, Cls::Rel},
    {"nparallel", 0x2226, Cls::Rel}, {"mid", 0x2223, Cls::Rel}, {"nmid", 0x2224, Cls::Rel},
    {"prec", 0x227A, Cls::Rel}, {"succ", 0x227B, Cls::Rel}, {"preceq", 0x2AAF, Cls::Rel},
    {"succeq", 0x2AB0, Cls::Rel}, {"asymp", 0x224D, Cls::Rel}, {"doteq", 0x2250, Cls::Rel},
    {"smile", 0x2323, Cls::Rel}, {"frown", 0x2322, Cls::Rel}, {"bowtie", 0x22C8, Cls::Rel},
    {"triangleq", 0x225C, Cls::Rel}, {"coloneqq", 0x2254, Cls::Rel}, {"nleq", 0x2270, Cls::Rel},
    {"ngeq", 0x2271, Cls::Rel}, {"nless", 0x226E, Cls::Rel}, {"ngtr", 0x226F, Cls::Rel},
    {"nsim", 0x2241, Cls::Rel}, {"ncong", 0x2247, Cls::Rel}, {"lhd", 0x22B2, Cls::Rel},
    {"rhd", 0x22B3, Cls::Rel}, {"unlhd", 0x22B4, Cls::Rel}, {"unrhd", 0x22B5, Cls::Rel},
    // Arrows
    {"to", 0x2192, Cls::Rel}, {"rightarrow", 0x2192, Cls::Rel}, {"gets", 0x2190, Cls::Rel},
    {"leftarrow", 0x2190, Cls::Rel}, {"leftrightarrow", 0x2194, Cls::Rel},
    {"Rightarrow", 0x21D2, Cls::Rel}, {"Leftarrow", 0x21D0, Cls::Rel},
    {"Leftrightarrow", 0x21D4, Cls::Rel}, {"mapsto", 0x21A6, Cls::Rel},
    {"longmapsto", 0x27FC, Cls::Rel}, {"longrightarrow", 0x27F6, Cls::Rel},
    {"longleftarrow", 0x27F5, Cls::Rel}, {"longleftrightarrow", 0x27F7, Cls::Rel},
    {"Longrightarrow", 0x27F9, Cls::Rel}, {"Longleftarrow", 0x27F8, Cls::Rel},
    {"Longleftrightarrow", 0x27FA, Cls::Rel}, {"implies", 0x27F9, Cls::Rel},
    {"impliedby", 0x27F8, Cls::Rel}, {"iff", 0x27FA, Cls::Rel}, {"uparrow", 0x2191, Cls::Rel},
    {"downarrow", 0x2193, Cls::Rel}, {"updownarrow", 0x2195, Cls::Rel}, {"Uparrow", 0x21D1, Cls::Rel},
    {"Downarrow", 0x21D3, Cls::Rel}, {"Updownarrow", 0x21D5, Cls::Rel}, {"nearrow", 0x2197, Cls::Rel},
    {"searrow", 0x2198, Cls::Rel}, {"swarrow", 0x2199, Cls::Rel}, {"nwarrow", 0x2196, Cls::Rel},
    {"hookrightarrow", 0x21AA, Cls::Rel}, {"hookleftarrow", 0x21A9, Cls::Rel},
    {"rightharpoonup", 0x21C0, Cls::Rel}, {"leftharpoonup", 0x21BC, Cls::Rel},
    {"rightharpoondown", 0x21C1, Cls::Rel}, {"leftharpoondown", 0x21BD, Cls::Rel},
    {"rightleftharpoons", 0x21CC, Cls::Rel}, {"leadsto", 0x21DD, Cls::Rel},
    {"rightsquigarrow", 0x21DD, Cls::Rel}, {"twoheadrightarrow", 0x21A0, Cls::Rel},
    {"rightarrowtail", 0x21A3, Cls::Rel}, {"leftrightarrows", 0x21C6, Cls::Rel},
    {"rightleftarrows", 0x21C4, Cls::Rel},
    // Ordinary symbols
    {"infty", 0x221E, Cls::Ord}, {"partial", 0x1D715, Cls::Ord}, {"nabla", 0x2207, Cls::Ord},
    {"forall", 0x2200, Cls::Ord}, {"exists", 0x2203, Cls::Ord}, {"nexists", 0x2204, Cls::Ord},
    {"neg", 0xAC, Cls::Ord}, {"lnot", 0xAC, Cls::Ord}, {"emptyset", 0x2205, Cls::Ord},
    {"varnothing", 0x2205, Cls::Ord}, {"aleph", 0x2135, Cls::Ord}, {"beth", 0x2136, Cls::Ord},
    {"gimel", 0x2137, Cls::Ord}, {"hbar", 0x210F, Cls::Ord}, {"hslash", 0x210F, Cls::Ord},
    {"ell", 0x2113, Cls::Ord}, {"wp", 0x2118, Cls::Ord}, {"Re", 0x211C, Cls::Ord},
    {"Im", 0x2111, Cls::Ord}, {"angle", 0x2220, Cls::Ord}, {"measuredangle", 0x2221, Cls::Ord},
    {"top", 0x22A4, Cls::Ord}, {"bot", 0x22A5, Cls::Ord}, {"prime", 0x2032, Cls::Ord},
    {"degree", 0xB0, Cls::Ord}, {"triangle", 0x25B3, Cls::Ord}, {"square", 0x25A1, Cls::Ord},
    {"Box", 0x25A1, Cls::Ord}, {"Diamond", 0x25C7, Cls::Ord}, {"surd", 0x221A, Cls::Ord},
    {"flat", 0x266D, Cls::Ord}, {"natural", 0x266E, Cls::Ord}, {"sharp", 0x266F, Cls::Ord},
    {"clubsuit", 0x2663, Cls::Ord}, {"diamondsuit", 0x2666, Cls::Ord},
    {"heartsuit", 0x2665, Cls::Ord}, {"spadesuit", 0x2660, Cls::Ord}, {"checkmark", 0x2713, Cls::Ord},
    {"imath", 0x131, Cls::Ord}, {"jmath", 0x237, Cls::Ord}, {"eth", 0xF0, Cls::Ord},
    {"backslash", '\\', Cls::Ord}, {"qed", 0x220E, Cls::Ord}, {"blacksquare", 0x220E, Cls::Ord},
    {"complement", 0x2201, Cls::Ord}, {"vert", '|', Cls::Ord}, {"Vert", 0x2016, Cls::Ord},
    {"|", 0x2016, Cls::Ord}, {"ldots", 0x2026, Cls::Inner}, {"dots", 0x2026, Cls::Inner},
    {"dotso", 0x2026, Cls::Inner}, {"dotsc", 0x2026, Cls::Inner}, {"cdots", 0x22EF, Cls::Inner},
    {"dotsb", 0x22EF, Cls::Inner}, {"dotsm", 0x22EF, Cls::Inner}, {"dotsi", 0x22EF, Cls::Inner},
    {"vdots", 0x22EE, Cls::Ord}, {"ddots", 0x22F1, Cls::Inner}, {"colon", ':', Cls::Punct},
    {"%", '%', Cls::Ord}, {"#", '#', Cls::Ord}, {"&", '&', Cls::Ord}, {"$", '$', Cls::Ord},
    {"_", '_', Cls::Ord}, {"lbrace", '{', Cls::Open}, {"rbrace", '}', Cls::Close},
    {"{", '{', Cls::Open}, {"}", '}', Cls::Close}, {"langle", 0x27E8, Cls::Open},
    {"rangle", 0x27E9, Cls::Close}, {"lceil", 0x2308, Cls::Open}, {"rceil", 0x2309, Cls::Close},
    {"lfloor", 0x230A, Cls::Open}, {"rfloor", 0x230B, Cls::Close}, {"lvert", '|', Cls::Open},
    {"rvert", '|', Cls::Close}, {"lVert", 0x2016, Cls::Open}, {"rVert", 0x2016, Cls::Close},
    {"llbracket", 0x27E6, Cls::Open}, {"rrbracket", 0x27E7, Cls::Close},
    {"lbrack", '[', Cls::Open}, {"rbrack", ']', Cls::Close},
};

struct Op {
  std::string_view name;
  char32_t cp;
  bool limits;  // limits above and below in display style
};

constexpr Op kBigOps[] = {
    {"sum", 0x2211, true}, {"prod", 0x220F, true}, {"coprod", 0x2210, true},
    {"bigcup", 0x22C3, true}, {"bigcap", 0x22C2, true}, {"bigvee", 0x22C1, true},
    {"bigwedge", 0x22C0, true}, {"bigoplus", 0x2A01, true}, {"bigotimes", 0x2A02, true},
    {"bigodot", 0x2A00, true}, {"biguplus", 0x2A04, true}, {"bigsqcup", 0x2A06, true},
    {"int", 0x222B, false}, {"intop", 0x222B, false}, {"iint", 0x222C, false},
    {"iiint", 0x222D, false}, {"oint", 0x222E, false}, {"oiint", 0x222F, false},
    {"oiiint", 0x2230, false},
};

// Named operators, set upright. The ones marked take limits in display style.
struct Fn {
  std::string_view name;
  bool limits;
};
constexpr Fn kFns[] = {
    {"arccos", false}, {"arcsin", false}, {"arctan", false}, {"arg", false}, {"cos", false},
    {"cosh", false}, {"cot", false}, {"coth", false}, {"csc", false}, {"deg", false},
    {"det", true}, {"dim", false}, {"exp", false}, {"gcd", true}, {"hom", false},
    {"inf", true}, {"ker", false}, {"lg", false}, {"lim", true}, {"liminf", true},
    {"limsup", true}, {"ln", false}, {"log", false}, {"max", true}, {"min", true},
    {"Pr", true}, {"sec", false}, {"sin", false}, {"sinh", false}, {"sup", true},
    {"tan", false}, {"tanh", false}, {"argmax", true}, {"argmin", true}, {"sgn", false},
    {"tr", false}, {"Tr", false}, {"rank", false}, {"diag", false}, {"span", false},
    {"Var", false}, {"Cov", false}, {"lcm", false}, {"mod", false},
};

const Sym* find_sym(std::string_view name) {
  for (const Sym& s : kSyms)
    if (s.name == name) return &s;
  return nullptr;
}

// Class of a character typed straight into the math.
Cls class_of(char32_t cp) {
  switch (cp) {
    case '+': case 0x2212: case '*': return Cls::Bin;
    case '=': case '<': case '>': case ':': return Cls::Rel;
    case '(': case '[': return Cls::Open;
    case ')': case ']': case '!': case '?': return Cls::Close;
    case ',': case ';': return Cls::Punct;
    default: break;
  }
  for (const Sym& s : kSyms)
    if (s.cp == cp) return s.cls;
  return Cls::Ord;
}

// A letter or digit in one of the math alphabets. Holes in the Unicode
// blocks were filled long ago by letterlike symbols, and the glyphs live there.
char32_t alpha(char32_t c, Font f) {
  const bool up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z', dg = c >= '0' && c <= '9';
  if (!up && !lo && !dg) return c;
  const int i = up ? int(c - 'A') : lo ? int(c - 'a') : int(c - '0');
  switch (f) {
    case Font::Roman: return c;
    case Font::Italic:
      if (dg) return c;
      if (c == 'h') return 0x210E;
      return up ? 0x1D434 + i : 0x1D44E + i;
    case Font::Bold:
      if (dg) return 0x1D7CE + i;
      return up ? 0x1D400 + i : 0x1D41A + i;
    case Font::BoldItalic:
      if (dg) return 0x1D7CE + i;
      return up ? 0x1D468 + i : 0x1D482 + i;
    case Font::BB:
      if (dg) return 0x1D7D8 + i;
      if (up) {
        switch (c) {
          case 'C': return 0x2102; case 'H': return 0x210D; case 'N': return 0x2115;
          case 'P': return 0x2119; case 'Q': return 0x211A; case 'R': return 0x211D;
          case 'Z': return 0x2124; default: return 0x1D538 + i;
        }
      }
      return 0x1D552 + i;
    case Font::Cal:
      if (dg) return c;
      if (up) {
        switch (c) {
          case 'B': return 0x212C; case 'E': return 0x2130; case 'F': return 0x2131;
          case 'H': return 0x210B; case 'I': return 0x2110; case 'L': return 0x2112;
          case 'M': return 0x2133; case 'R': return 0x211B; default: return 0x1D49C + i;
        }
      }
      // The font has no script e, g or o; italic reads close enough.
      if (c == 'e' || c == 'g' || c == 'o') return 0x1D44E + i;
      return 0x1D4B6 + i;
    case Font::Frak:
      if (dg) return c;
      if (up) {
        switch (c) {
          case 'C': return 0x212D; case 'H': return 0x210C; case 'I': return 0x2111;
          case 'R': return 0x211C; case 'Z': return 0x2128; default: return 0x1D504 + i;
        }
      }
      return 0x1D51E + i;
  }
  return c;
}

// Greek typed as Unicode, into the alphabet in force.
char32_t greek(char32_t c, Font f) {
  if (c >= 0x3B1 && c <= 0x3C9) {
    if (f == Font::Roman) return c;
    if (f == Font::Bold || f == Font::BoldItalic) return 0x1D6C2 + (c - 0x3B1);
    return 0x1D6FC + (c - 0x3B1);
  }
  if (c >= 0x391 && c <= 0x3A9 && (f == Font::Bold || f == Font::BoldItalic))
    return 0x1D6A8 + (c - 0x391);
  return c;
}

// A symbol's Greek italic form, re-cast into another alphabet: \mathbf{\alpha}.
char32_t recast(char32_t cp, Font f) {
  if (cp >= 0x1D6FC && cp <= 0x1D714) return greek(0x3B1 + (cp - 0x1D6FC), f);
  if (cp >= 0x391 && cp <= 0x3A9) return greek(cp, f);
  return cp;
}

Node sym(char32_t cp, Cls cls, Font f = Font::Roman) {
  Node n;
  n.k = Node::K::Sym;
  n.cp = cp;
  n.cls = cls;
  n.font = f;
  return n;
}

Node text_node(std::string s, Font f, Cls cls = Cls::Ord) {
  Node n;
  n.k = Node::K::Text;
  n.text = std::move(s);
  n.font = f;
  n.cls = cls;
  return n;
}

Node space(float em) {
  Node n;
  n.k = Node::K::Space;
  n.amount = em;
  return n;
}

Node group(List l, Cls cls = Cls::Ord) {
  Node n;
  n.k = Node::K::Group;
  n.cls = cls;
  n.kids.push_back(std::move(l));
  return n;
}

enum class Stop { End, Brace, Amp, Row, Right, Middle, EndEnv, Bracket };

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  List top() {
    std::vector<std::vector<List>> rows(1);
    bool amp = false;
    for (;;) {
      Stop why;
      List cell = list(&why);
      rows.back().push_back(std::move(cell));
      if (why == Stop::Amp) { amp = true; continue; }
      if (why == Stop::Row) { rows.emplace_back(); continue; }
      if (why == Stop::End) break;
      // A stray } or \right with nothing to close: keep reading.
    }
    if (rows.size() == 1 && rows[0].size() == 1) return std::move(rows[0][0]);
    // Top-level rows: an aligned block if anything aligns, else a gathered one.
    return List{array(std::move(rows), amp ? "rl" : "c", 0, 0, 0, amp ? 0.f : 0.f, true)};
  }

  List list(Stop* why) {
    List out;
    // Nesting is bounded: the parser and the layout both recurse, and the
    // daemon that runs them hosts every agent. Past the limit the rest of the
    // input is dropped rather than risk the stack.
    if (++depth_ > kMaxDepth) {
      depth_--;
      i_ = s_.size();
      *why = Stop::End;
      return out;
    }
    const Font saved = font_;
    int over_at = -1;
    bool over_rule = true;
    char32_t over_open = 0, over_close = 0;
    for (;;) {
      skip_space();
      if (i_ >= s_.size()) { *why = Stop::End; break; }
      const char c = s_[i_];
      if (c == '}') { i_++; *why = Stop::Brace; break; }
      if (c == '&') { i_++; *why = Stop::Amp; break; }
      if (c == ']' && bracket_depth_ > 0) { i_++; *why = Stop::Bracket; break; }
      if (c == '^' || c == '_') {
        i_++;
        List a = arg();
        attach(out, c == '^' ? 1 : 2, std::move(a));
        continue;
      }
      if (c == '\'') {
        size_t n = 0;
        while (i_ < s_.size() && s_[i_] == '\'') { i_++; n++; }
        const char32_t p = n == 1 ? 0x2032 : n == 2 ? 0x2033 : 0x2034;
        attach(out, 1, List{sym(p, Cls::Ord)});
        continue;
      }
      if (c == '{') {
        i_++;
        Stop w;
        List g = list(&w);
        out.push_back(group(std::move(g)));
        continue;
      }
      if (c == '~') { i_++; out.push_back(space(0.333f)); continue; }
      if (c == '\\') {
        Stop w;
        if (command(out, &w)) { *why = w; break; }
        if (over_at < 0 && pending_over_) {
          over_at = int(out.size());
          over_rule = over_kind_ == 0;
          over_open = over_kind_ == 1 ? '(' : 0;
          over_close = over_kind_ == 1 ? ')' : 0;
          pending_over_ = false;
        }
        continue;
      }
      char32_t cp = next_cp();
      if (cp == '-') cp = 0x2212;
      else if (cp == '*') cp = 0x2217;
      if (cp < 0x80 && std::isalnum(int(cp))) {
        out.push_back(sym(alpha(cp, font_), Cls::Ord, font_));
      } else if ((cp >= 0x391 && cp <= 0x3C9)) {
        out.push_back(sym(greek(cp, font_), Cls::Ord, font_));
      } else {
        out.push_back(sym(cp, class_of(cp)));
      }
    }
    font_ = saved;
    depth_--;
    if (over_at >= 0) {
      Node f;
      f.k = Node::K::Frac;
      f.cls = Cls::Inner;
      f.rule = over_rule;
      f.open = over_open;
      f.close = over_close;
      f.kids.emplace_back(out.begin(), out.begin() + over_at);
      f.kids.emplace_back(out.begin() + over_at, out.end());
      out.clear();
      out.push_back(std::move(f));
    }
    return out;
  }

 private:
  void skip_space() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
      i_++;
  }

  char32_t next_cp() {
    const unsigned char b = uint8_t(s_[i_]);
    if (b < 0x80) { i_++; return b; }
    return text::decode(s_, i_);
  }

  std::string_view name() {
    // After the backslash: a run of letters, or one other character.
    const size_t a = i_;
    if (i_ < s_.size() && std::isalpha(uint8_t(s_[i_]))) {
      while (i_ < s_.size() && std::isalpha(uint8_t(s_[i_]))) i_++;
    } else if (i_ < s_.size()) {
      next_cp();
    }
    return s_.substr(a, i_ - a);
  }

  // A brace group's raw text, braces balanced; or one character.
  std::string raw_arg() {
    skip_space();
    if (i_ >= s_.size()) return {};
    if (s_[i_] != '{') {
      const size_t a = i_;
      next_cp();
      return std::string(s_.substr(a, i_ - a));
    }
    size_t depth = 1, j = i_ + 1;
    while (j < s_.size() && depth) {
      if (s_[j] == '\\' && j + 1 < s_.size()) { j += 2; continue; }
      if (s_[j] == '{') depth++;
      else if (s_[j] == '}') depth--;
      j++;
    }
    const size_t end = depth ? j : j - 1;
    std::string r(s_.substr(i_ + 1, end - (i_ + 1)));
    i_ = j;
    return r;
  }

  // The next script or command argument: a group, a command, or a character.
  List arg() {
    skip_space();
    if (i_ >= s_.size()) return {};
    if (s_[i_] == '{') {
      i_++;
      Stop w;
      return list(&w);
    }
    List out;
    if (s_[i_] == '\\') {
      Stop w;
      command(out, &w);
      return out;
    }
    char32_t cp = next_cp();
    if (cp == '-') cp = 0x2212;
    if (cp < 0x80 && std::isalnum(int(cp))) out.push_back(sym(alpha(cp, font_), Cls::Ord, font_));
    else if (cp >= 0x391 && cp <= 0x3C9) out.push_back(sym(greek(cp, font_), Cls::Ord, font_));
    else out.push_back(sym(cp, class_of(cp)));
    return out;
  }

  // [..], when there is one.
  bool optional(List* out) {
    skip_space();
    if (i_ >= s_.size() || s_[i_] != '[') return false;
    i_++;
    bracket_depth_++;
    Stop w;
    *out = list(&w);
    bracket_depth_--;
    return true;
  }

  // Adds a script to the last atom, or to an empty one when there is none.
  void attach(List& out, int slot, List script) {
    // A second script in the same slot (x'^2, or TeX's "double subscript"
    // x_1_2) joins the first rather than nesting: nesting would let a line of
    // x_1_2_3… build a tree as deep as it is long.
    if (!out.empty() && out.back().k == Node::K::Scripts) {
      auto& dst = out.back().kids[size_t(slot)];
      for (auto& n : script) dst.push_back(std::move(n));
      return;
    }
    Node sc;
    sc.k = Node::K::Scripts;
    sc.kids.resize(3);
    if (!out.empty() && out.back().k != Node::K::Space && out.back().k != Node::K::Style) {
      sc.cls = out.back().cls;
      sc.kids[0].push_back(std::move(out.back()));
      out.pop_back();
    }
    sc.kids[size_t(slot)] = std::move(script);
    out.push_back(std::move(sc));
  }

  char32_t delimiter() {
    skip_space();
    if (i_ >= s_.size()) return 0;
    if (s_[i_] == '\\') {
      i_++;
      std::string_view n = name();
      if (n == "{" || n == "lbrace") return '{';
      if (n == "}" || n == "rbrace") return '}';
      if (n == "|" || n == "Vert" || n == "lVert" || n == "rVert") return 0x2016;
      if (n == "vert" || n == "lvert" || n == "rvert" || n == "mid") return '|';
      if (n == "backslash") return '\\';
      if (const Sym* s = find_sym(n)) return s->cp;
      return 0;
    }
    const char32_t cp = next_cp();
    return cp == '.' ? 0 : cp;
  }

  static float dimension(const std::string& d) {
    const char* p = d.c_str();
    char* end = nullptr;
    const float v = strtof(p, &end);
    if (!end || end == p) return 0;
    std::string_view unit(end);
    while (!unit.empty() && unit.front() == ' ') unit.remove_prefix(1);
    if (unit.starts_with("em")) return v;
    if (unit.starts_with("ex")) return v * 0.43f;
    if (unit.starts_with("mu")) return v / 18.f;
    if (unit.starts_with("pt")) return v / 10.f;
    if (unit.starts_with("mm")) return v * 0.285f;
    if (unit.starts_with("cm")) return v * 2.85f;
    if (unit.starts_with("in")) return v * 7.2f;
    return v / 10.f;
  }

  static Node array(std::vector<std::vector<List>> rows, std::string_view align, char32_t open,
                    char32_t close, int8_t style, float colsep, bool lead_ord) {
    while (rows.size() > 1 && rows.back().size() == 1 && rows.back()[0].empty()) rows.pop_back();
    size_t cols = 1;
    for (auto& r : rows) cols = std::max(cols, r.size());
    Node a;
    a.k = Node::K::Array;
    a.cls = open || close ? Cls::Inner : Cls::Ord;
    a.open = open;
    a.close = close;
    a.style = style;
    a.amount = colsep;
    a.cols = uint16_t(cols);
    for (size_t c = 0; c < cols; c++) a.text.push_back(align[c % align.size()]);
    for (auto& r : rows) {
      r.resize(cols);
      for (size_t c = 0; c < cols; c++) {
        // The left half of an aligned pair starts with an empty atom, so
        // "&= b" spaces its relation the way "a = b" would.
        if (lead_ord && a.text[c] == 'l' && c > 0) r[c].insert(r[c].begin(), group({}));
        a.kids.push_back(std::move(r[c]));
      }
    }
    return a;
  }

  Node environment(const std::string& env) {
    std::string_view e = env;
    if (e.ends_with("*")) e.remove_suffix(1);
    std::string spec;
    if (e == "array" || e == "subarray") {
      spec = raw_arg();
      std::string keep;
      for (char c : spec)
        if (c == 'l' || c == 'c' || c == 'r') keep.push_back(c);
      spec = keep.empty() ? "c" : keep;
    }
    if (e == "alignat" || e == "alignedat") raw_arg();
    std::vector<std::vector<List>> rows(1);
    for (;;) {
      Stop why;
      List cell = list(&why);
      rows.back().push_back(std::move(cell));
      if (why == Stop::Amp) continue;
      if (why == Stop::Row) { rows.emplace_back(); continue; }
      if (why == Stop::EndEnv || why == Stop::End) break;
    }
    if (e == "equation" || e == "displaymath" || e == "math" || e == "multline") {
      List all;
      for (auto& r : rows)
        for (auto& c : r)
          for (auto& n : c) all.push_back(std::move(n));
      if (rows.size() > 1) return array(std::move(rows), "c", 0, 0, 0, 0, false);
      return group(std::move(all));
    }
    if (e == "pmatrix") return array(std::move(rows), "c", '(', ')', 1, 1.f, false);
    if (e == "bmatrix") return array(std::move(rows), "c", '[', ']', 1, 1.f, false);
    if (e == "Bmatrix") return array(std::move(rows), "c", '{', '}', 1, 1.f, false);
    if (e == "vmatrix") return array(std::move(rows), "c", '|', '|', 1, 1.f, false);
    if (e == "Vmatrix") return array(std::move(rows), "c", 0x2016, 0x2016, 1, 1.f, false);
    if (e == "smallmatrix") return array(std::move(rows), "c", 0, 0, 2, 0.5f, false);
    if (e == "cases") return array(std::move(rows), "ll", '{', 0, 1, 1.f, false);
    if (e == "dcases") return array(std::move(rows), "ll", '{', 0, 0, 1.f, false);
    if (e == "rcases") return array(std::move(rows), "ll", 0, '}', 1, 1.f, false);
    if (e == "aligned" || e == "align" || e == "split" || e == "alignat" || e == "alignedat" ||
        e == "flalign" || e == "eqnarray")
      return array(std::move(rows), "rl", 0, 0, 0, 0.f, true);
    if (e == "gathered" || e == "gather") return array(std::move(rows), "c", 0, 0, 0, 0, false);
    if (e == "array" || e == "subarray") return array(std::move(rows), spec, 0, 0, 1, 1.f, false);
    return array(std::move(rows), "c", 0, 0, 1, 1.f, false);  // matrix and anything else
  }

  // Parses one command at s_[i_] == '\\'. Returns true when it ends the
  // enclosing list (\\, \right, \end…), with the reason in *why.
  bool command(List& out, Stop* why) {
    // Commands nest through their arguments (\hat\hat\hat…), so they count
    // toward the same depth limit as groups.
    if (depth_ >= kMaxDepth) {
      i_ = s_.size();
      return false;
    }
    depth_++;
    const bool r = command_body(out, why);
    depth_--;
    return r;
  }

  bool command_body(List& out, Stop* why) {
    i_++;
    const std::string_view n = name();
    if (n.empty()) return false;

    if (n == "\\" || n == "cr" || n == "newline") {
      optional_skip();
      *why = Stop::Row;
      return true;
    }
    if (n == "right") { right_ = delimiter(); *why = Stop::Right; return true; }
    if (n == "middle") { middle_ = delimiter(); *why = Stop::Middle; return true; }
    if (n == "end") { raw_arg(); *why = Stop::EndEnv; return true; }

    if (n == "begin") {
      out.push_back(environment(raw_arg()));
      return false;
    }
    if (n == "left") {
      Node d;
      d.k = Node::K::Delim;
      d.cls = Cls::Inner;
      d.open = delimiter();
      std::u32string middles;
      for (;;) {
        Stop w;
        d.kids.push_back(list(&w));
        if (w == Stop::Middle) { middles.push_back(middle_); continue; }
        d.close = w == Stop::Right ? right_ : 0;
        break;
      }
      for (char32_t m : middles) text::encode(m, d.text);
      out.push_back(std::move(d));
      return false;
    }
    if (n == "over" || n == "choose" || n == "atop") {
      pending_over_ = true;
      over_kind_ = n == "over" ? 0 : n == "choose" ? 1 : 2;
      return false;
    }

    // Spacing
    if (n == ",") { out.push_back(space(3.f / 18)); return false; }
    if (n == ":" || n == ">") { out.push_back(space(4.f / 18)); return false; }
    if (n == ";") { out.push_back(space(5.f / 18)); return false; }
    if (n == "!") { out.push_back(space(-3.f / 18)); return false; }
    if (n == " ") { out.push_back(space(0.333f)); return false; }
    if (n == "quad") { out.push_back(space(1)); return false; }
    if (n == "qquad") { out.push_back(space(2)); return false; }
    if (n == "enspace") { out.push_back(space(0.5f)); return false; }
    if (n == "thinspace") { out.push_back(space(3.f / 18)); return false; }
    if (n == "medspace") { out.push_back(space(4.f / 18)); return false; }
    if (n == "thickspace") { out.push_back(space(5.f / 18)); return false; }
    if (n == "negthinspace") { out.push_back(space(-3.f / 18)); return false; }
    if (n == "hspace" || n == "hspace*" || n == "kern" || n == "mkern" || n == "hskip" ||
        n == "mskip") {
      out.push_back(space(dimension(raw_arg())));
      return false;
    }

    // Styles
    if (n == "displaystyle" || n == "textstyle" || n == "scriptstyle" || n == "scriptscriptstyle") {
      Node st;
      st.k = Node::K::Style;
      st.style = n == "displaystyle" ? 0 : n == "textstyle" ? 1 : n == "scriptstyle" ? 2 : 3;
      out.push_back(st);
      return false;
    }
    if (n == "limits" || n == "nolimits") {
      if (!out.empty()) {
        Node& last = out.back().k == Node::K::Scripts && !out.back().kids[0].empty()
                         ? out.back().kids[0].back()
                         : out.back();
        last.limits = n == "limits" ? 1 : 0;
      }
      return false;
    }

    // Fractions and friends
    if (n == "frac" || n == "dfrac" || n == "tfrac" || n == "cfrac" || n == "binom" ||
        n == "dbinom" || n == "tbinom" || n == "genfrac") {
      Node f;
      f.k = Node::K::Frac;
      f.cls = Cls::Inner;
      if (n == "dfrac" || n == "cfrac" || n == "dbinom") f.style = 0;
      if (n == "tfrac" || n == "tbinom") f.style = 1;
      if (n.find("binom") != std::string_view::npos) {
        f.rule = false;
        f.open = '(';
        f.close = ')';
      }
      if (n == "genfrac") {
        std::string l = raw_arg(), r = raw_arg(), t = raw_arg(), st = raw_arg();
        f.open = l.empty() ? 0 : char32_t(uint8_t(l[0]));
        f.close = r.empty() ? 0 : char32_t(uint8_t(r[0]));
        f.rule = t.empty() || dimension(t) > 0;
        if (!st.empty() && st[0] >= '0' && st[0] <= '3') f.style = int8_t(st[0] - '0');
      }
      f.kids.push_back(arg());
      f.kids.push_back(arg());
      out.push_back(std::move(f));
      return false;
    }
    if (n == "sqrt") {
      Node r;
      r.k = Node::K::Sqrt;
      List index;
      optional(&index);
      r.kids.push_back(arg());
      r.kids.push_back(std::move(index));
      out.push_back(std::move(r));
      return false;
    }

    // Delimiters at a fixed size
    if (n.starts_with("big") || n.starts_with("Big")) {
      std::string_view rest = n.substr(3);
      uint8_t size = n[0] == 'b' ? 1 : 2;
      if (rest.starts_with("g")) { size = n[0] == 'b' ? 3 : 4; rest.remove_prefix(1); }
      if (rest.empty() || rest == "l" || rest == "r" || rest == "m") {
        Node b;
        b.k = Node::K::Big;
        b.size = size;
        b.cls = rest == "l" ? Cls::Open : rest == "r" ? Cls::Close : rest == "m" ? Cls::Rel : Cls::Ord;
        b.cp = delimiter();
        out.push_back(b);
        return false;
      }
    }

    // Text and alphabets
    if (n == "text" || n == "textrm" || n == "textup" || n == "textnormal" || n == "mbox" ||
        n == "hbox" || n == "textsf" || n == "texttt" || n == "textbf" || n == "textit" ||
        n == "emph" || n == "mathrm" || n == "operatorname" || n == "operatornamewithlimits") {
      bool star = false;
      if (n == "operatorname" && i_ < s_.size() && s_[i_] == '*') { star = true; i_++; }
      std::string t = raw_arg();
      const bool is_op = n.starts_with("operatorname");
      if (n == "mathrm" && t.find('\\') != std::string::npos) {
        // \mathrm{\Delta x}: math inside, set upright.
        Parser sub(t);
        sub.depth_ = depth_;
        sub.font_ = Font::Roman;
        out.push_back(group(sub.top()));
        return false;
      }
      const Font f = n == "textbf" ? Font::Bold : (n == "textit" || n == "emph") ? Font::Italic : Font::Roman;
      std::string clean;
      for (size_t k = 0; k < t.size(); k++) {
        if (t[k] == '\\' && k + 1 < t.size()) { clean.push_back(t[++k] == ' ' ? ' ' : t[k]); continue; }
        if (t[k] == '{' || t[k] == '}' || t[k] == '$') continue;
        if (t[k] == '~') { clean.push_back(' '); continue; }
        clean.push_back(t[k]);
      }
      Node tn = text_node(std::move(clean), f, is_op ? Cls::Op : Cls::Ord);
      if (is_op) tn.limits = (star || n == "operatornamewithlimits") ? 1 : 0;
      // \mathrm spaces nothing; \text keeps its spaces.
      if (n == "mathrm" || is_op) std::erase(tn.text, ' ');
      out.push_back(std::move(tn));
      return false;
    }
    if (n == "mathbf" || n == "mathit" || n == "mathbb" || n == "mathcal" || n == "mathscr" ||
        n == "mathfrak" || n == "mathsf" || n == "mathtt" || n == "boldsymbol" || n == "bm" ||
        n == "mathnormal" || n == "mathbfit" || n == "Bbb" || n == "pmb") {
      const Font f = n == "mathbf" ? Font::Bold
                     : (n == "boldsymbol" || n == "bm" || n == "mathbfit" || n == "pmb") ? Font::BoldItalic
                     : (n == "mathbb" || n == "Bbb") ? Font::BB
                     : (n == "mathcal" || n == "mathscr") ? Font::Cal
                     : n == "mathfrak" ? Font::Frak
                     : (n == "mathit" || n == "mathnormal") ? Font::Italic
                     : Font::Roman;
      const Font saved = font_;
      font_ = f;
      List a = arg();
      font_ = saved;
      for (auto& node : a)
        if (node.k == Node::K::Sym) node.cp = recast(node.cp, f);
      out.push_back(group(std::move(a)));
      return false;
    }
    if (n == "rm" || n == "bf" || n == "it" || n == "cal" || n == "sf" || n == "tt") {
      font_ = n == "bf" ? Font::Bold : n == "it" ? Font::Italic : n == "cal" ? Font::Cal : Font::Roman;
      return false;
    }

    // Accents
    struct Acc { std::string_view name; char32_t cp; bool wide; };
    static constexpr Acc kAcc[] = {
        {"hat", 0x2C6, false}, {"widehat", 0x2C6, true}, {"check", 0x2C7, false},
        {"widecheck", 0x2C7, true}, {"tilde", 0x2DC, false}, {"widetilde", 0x2DC, true},
        {"acute", 0xB4, false}, {"grave", '`', false}, {"dot", 0x2D9, false},
        {"ddot", 0xA8, false}, {"dddot", 0xA8, false}, {"breve", 0x2D8, false},
        {"bar", 0xAF, false}, {"vec", 0x2192, false}, {"mathring", 0x2DA, false},
    };
    for (const Acc& a : kAcc) {
      if (n != a.name) continue;
      Node ac;
      ac.k = Node::K::Accent;
      ac.cp = a.cp;
      ac.wide = a.wide;
      ac.kids.push_back(arg());
      out.push_back(std::move(ac));
      return false;
    }
    if (n == "overrightarrow" || n == "overleftarrow" || n == "overleftrightarrow" ||
        n == "underrightarrow" || n == "underleftarrow" || n == "underleftrightarrow") {
      Node ac;
      ac.k = Node::K::Accent;
      ac.stretch_arrow = true;
      ac.under = n.starts_with("under");
      ac.cp = n.ends_with("leftrightarrow") ? 0x2194 : n.ends_with("leftarrow") ? 0x2190 : 0x2192;
      ac.kids.push_back(arg());
      out.push_back(std::move(ac));
      return false;
    }
    if (n == "overline" || n == "underline") {
      Node r;
      r.k = Node::K::Rule;
      r.under = n == "underline";
      r.kids.push_back(arg());
      out.push_back(std::move(r));
      return false;
    }
    if (n == "overbrace" || n == "underbrace") {
      Node b;
      b.k = Node::K::Brace;
      b.cls = Cls::Op;
      b.limits = 1;
      b.under = n == "underbrace";
      b.kids.push_back(arg());
      out.push_back(std::move(b));
      return false;
    }
    if (n == "overset" || n == "underset" || n == "stackrel") {
      Node s;
      s.k = Node::K::Stack;
      List top = arg();
      List base = arg();
      s.cls = base.size() == 1 ? base[0].cls : Cls::Ord;
      if (n == "stackrel") s.cls = Cls::Rel;
      s.kids.push_back(std::move(base));
      if (n == "underset") { s.kids.emplace_back(); s.kids.push_back(std::move(top)); }
      else { s.kids.push_back(std::move(top)); s.kids.emplace_back(); }
      out.push_back(std::move(s));
      return false;
    }
    if (n == "xrightarrow" || n == "xleftarrow" || n == "xleftrightarrow" || n == "xRightarrow" ||
        n == "xLeftarrow" || n == "xmapsto") {
      Node s;
      s.k = Node::K::Stack;
      s.cls = Cls::Rel;
      s.wide = true;
      s.cp = n == "xleftarrow" ? 0x2190 : n == "xleftrightarrow" ? 0x2194 : 0x2192;
      List under;
      optional(&under);
      List over = arg();
      s.kids.emplace_back();
      s.kids.push_back(std::move(over));
      s.kids.push_back(std::move(under));
      out.push_back(std::move(s));
      return false;
    }
    if (n == "substack") {
      std::string body = raw_arg();
      Parser sub(body);
      sub.depth_ = depth_;
      std::vector<std::vector<List>> rows(1);
      for (;;) {
        Stop why;
        rows.back().push_back(sub.list(&why));
        if (why == Stop::Row) { rows.emplace_back(); continue; }
        if (why == Stop::Amp) continue;
        break;
      }
      out.push_back(array(std::move(rows), "c", 0, 0, 2, 0, false));
      return false;
    }

    // Boxes and friends
    if (n == "boxed" || n == "fbox" || n == "framebox") {
      Node b;
      b.k = Node::K::Boxed;
      b.kids.push_back(arg());
      out.push_back(std::move(b));
      return false;
    }
    if (n == "phantom" || n == "hphantom" || n == "vphantom") {
      Node p;
      p.k = Node::K::Phantom;
      p.kids.push_back(arg());
      out.push_back(std::move(p));
      return false;
    }
    if (n == "not") {
      skip_space();
      List next;
      if (i_ < s_.size()) {
        Stop w;
        if (s_[i_] == '\\') command(next, &w);
        else next = arg();
      }
      // The common pairs have glyphs of their own.
      if (next.size() == 1 && next[0].k == Node::K::Sym) {
        const char32_t c = next[0].cp;
        const char32_t neg = c == '=' ? 0x2260 : c == 0x2208 ? 0x2209 : c == 0x2261 ? 0x2262
                           : c == '<' ? 0x226E : c == '>' ? 0x226F : c == 0x2282 ? 0x2284
                           : c == 0x2283 ? 0x2285 : c == 0x2286 ? 0x2288 : c == 0x2264 ? 0x2270
                           : c == 0x2265 ? 0x2271 : c == 0x223C ? 0x2241 : c == 0x2203 ? 0x2204 : 0;
        if (neg) { out.push_back(sym(neg, Cls::Rel)); return false; }
      }
      Node no;
      no.k = Node::K::Not;
      no.cls = next.size() == 1 ? next[0].cls : Cls::Rel;
      no.kids.push_back(std::move(next));
      out.push_back(std::move(no));
      return false;
    }
    if (n == "mathop" || n == "mathrel" || n == "mathbin" || n == "mathord" || n == "mathopen" ||
        n == "mathclose" || n == "mathpunct" || n == "mathinner") {
      const Cls c = n == "mathop" ? Cls::Op : n == "mathrel" ? Cls::Rel : n == "mathbin" ? Cls::Bin
                  : n == "mathopen" ? Cls::Open : n == "mathclose" ? Cls::Close
                  : n == "mathpunct" ? Cls::Punct : n == "mathinner" ? Cls::Inner : Cls::Ord;
      out.push_back(group(arg(), c));
      return false;
    }

    // Commands that only colour, label or number: their content, or nothing.
    if (n == "color") { raw_arg(); return false; }
    if (n == "textcolor" || n == "colorbox") { raw_arg(); out.push_back(group(arg())); return false; }
    if (n == "label" || n == "ref" || n == "eqref" || n == "hypertarget") {
      raw_arg();
      return false;
    }
    if (n == "nonumber" || n == "notag" || n == "displaylimits" || n == "nolinebreak" ||
        n == "allowbreak" || n == "left." || n == "relax" || n == "strut" || n == "mathstrut" ||
        n == "big." || n == "noindent" || n == "centering") {
      return false;
    }
    if (n == "cancel" || n == "bcancel" || n == "xcancel" || n == "smash" || n == "mathclap" ||
        n == "mathllap" || n == "mathrlap" || n == "llap" || n == "rlap" || n == "ensuremath" ||
        n == "underbar") {
      out.push_back(group(arg()));
      return false;
    }
    if (n == "tag" || n == "tag*") {
      std::string t = raw_arg();
      out.push_back(space(2));
      out.push_back(text_node(n == "tag" ? "(" + t + ")" : t, Font::Roman));
      return false;
    }
    if (n == "bmod") {
      out.push_back(text_node("mod", Font::Roman, Cls::Bin));
      return false;
    }
    if (n == "pmod" || n == "mod") {
      List a = arg();
      out.push_back(space(n == "pmod" ? 0.9f : 0.9f));
      if (n == "pmod") out.push_back(sym('(', Cls::Open));
      out.push_back(text_node("mod", Font::Roman));
      out.push_back(space(0.333f));
      for (auto& x : a) out.push_back(std::move(x));
      if (n == "pmod") out.push_back(sym(')', Cls::Close));
      return false;
    }

    for (const Op& o : kBigOps) {
      if (n != o.name) continue;
      Node b;
      b.k = Node::K::BigOp;
      b.cls = Cls::Op;
      b.cp = o.cp;
      b.limits = o.limits ? -1 : 0;
      out.push_back(b);
      return false;
    }
    for (const Fn& f : kFns) {
      if (n != f.name) continue;
      Node t = text_node(std::string(n == "liminf" ? "lim inf" : n == "limsup" ? "lim sup"
                                     : n == "argmax" ? "arg max" : n == "argmin" ? "arg min" : n),
                         Font::Roman, Cls::Op);
      t.limits = f.limits ? -1 : 0;
      out.push_back(std::move(t));
      return false;
    }
    if (const Sym* s = find_sym(n)) {
      Node g = sym(s->cp, s->cls);
      g.cp = recast(g.cp, font_);
      out.push_back(g);
      return false;
    }
    // Something this does not know: its name, so a reader can still guess.
    out.push_back(text_node(std::string(n), Font::Roman));
    return false;
  }

  void optional_skip() {
    // \\[4pt]: extra row space, ignored.
    size_t j = i_;
    while (j < s_.size() && s_[j] == ' ') j++;
    if (j < s_.size() && s_[j] == '[') {
      const size_t close = s_.find(']', j);
      if (close != std::string_view::npos && close - j < 16) i_ = close + 1;
    }
  }

  static constexpr int kMaxDepth = 40;
  std::string_view s_;
  size_t i_ = 0;
  int depth_ = 0;
  Font font_ = Font::Italic;
  int bracket_depth_ = 0;
  char32_t right_ = 0, middle_ = 0;
  bool pending_over_ = false;
  int over_kind_ = 0;
};

// Characters Unicode has super- and subscript forms of, as the italic or
// roman glyphs the parser produces.
bool scriptable(char32_t cp, bool sup) {
  char c = 0;
  if (cp < 0x80) c = char(cp);
  else if (cp == 0x2212) c = '-';
  else if (cp >= 0x1D44E && cp <= 0x1D467) c = char('a' + (cp - 0x1D44E));
  else if (cp >= 0x1D434 && cp <= 0x1D44D) c = char('A' + (cp - 0x1D434));
  else if (cp == 0x210E) c = 'h';
  else if (cp == 0x2032 || cp == 0x2033 || cp == 0x2034) return sup;
  if (!c) return false;
  if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '=' || c == '(' || c == ')') return true;
  if (sup) return c == 'n' || c == 'i' || c == 'T';
  return std::strchr("aeoxhklmnpstijuvr", c) != nullptr;
}

bool plain_script(const List& l, bool sup) {
  for (const Node& n : l) {
    if (n.k == Node::K::Group) {
      if (!plain_script(n.kids[0], sup)) return false;
      continue;
    }
    if (n.k != Node::K::Sym || !scriptable(n.cp, sup)) return false;
  }
  return true;
}

}  // namespace

char32_t letter(char32_t c, Font f) { return alpha(c, f); }

List parse(std::string_view src) {
  Parser p(src);
  return p.top();
}

bool needs_drawing(const List& l) {
  for (const Node& n : l) {
    switch (n.k) {
      case Node::K::Sym:
        if (n.font == Font::BB || n.font == Font::Cal || n.font == Font::Frak) return true;
        break;
      case Node::K::Text:
      case Node::K::Space:
      case Node::K::Style:
        break;
      case Node::K::Group:
      case Node::K::Phantom:
        if (needs_drawing(n.kids[0])) return true;
        break;
      case Node::K::BigOp:
        break;
      case Node::K::Scripts: {
        const List& base = n.kids[0];
        // Limits under \sum or \lim have to be drawn; \sin^2 does not.
        if (base.size() == 1 && (base[0].k == Node::K::BigOp ||
                                 (base[0].k == Node::K::Text && base[0].cls == Cls::Op &&
                                  base[0].limits != 0)))
          return true;
        if (needs_drawing(base)) return true;
        if (!plain_script(n.kids[1], true) || !plain_script(n.kids[2], false)) return true;
        break;
      }
      default:
        return true;
    }
  }
  return false;
}

}  // namespace mico::math
