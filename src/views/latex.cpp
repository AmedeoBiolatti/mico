#include "views/latex.h"

namespace mico::latex {
namespace {

struct Sym {
  std::string_view cmd;
  std::string_view uni;
};

// Commands an agent actually writes. A function name like \sin has no glyph
// of its own — the mapping is just the bare word — so it still reads right
// once the backslash is gone.
constexpr Sym kSymbols[] = {
    // Greek, lowercase
    {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"},
    {"epsilon", "ε"}, {"varepsilon", "ε"}, {"zeta", "ζ"}, {"eta", "η"},
    {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"},
    {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"},
    {"pi", "π"}, {"varpi", "ϖ"}, {"rho", "ρ"}, {"varrho", "ϱ"},
    {"sigma", "σ"}, {"varsigma", "ς"}, {"tau", "τ"}, {"upsilon", "υ"},
    {"phi", "φ"}, {"varphi", "ϕ"}, {"chi", "χ"}, {"psi", "ψ"},
    {"omega", "ω"},
    // Greek, uppercase (skip the ones that are just a Latin capital)
    {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"}, {"Lambda", "Λ"},
    {"Xi", "Ξ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Upsilon", "Υ"},
    {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
    // Binary operators and relations
    {"times", "×"}, {"div", "÷"}, {"cdot", "·"}, {"pm", "±"},
    {"mp", "∓"}, {"approx", "≈"}, {"neq", "≠"}, {"leq", "≤"},
    {"le", "≤"}, {"geq", "≥"}, {"ge", "≥"}, {"ll", "≪"},
    {"gg", "≫"}, {"equiv", "≡"}, {"sim", "∼"}, {"simeq", "≃"},
    {"cong", "≅"}, {"propto", "∝"},
    // Set theory
    {"in", "∈"}, {"notin", "∉"}, {"subset", "⊂"}, {"subseteq", "⊆"},
    {"supset", "⊃"}, {"supseteq", "⊇"}, {"cup", "∪"}, {"cap", "∩"},
    {"emptyset", "∅"}, {"varnothing", "∅"}, {"setminus", "∖"},
    // Logic
    {"forall", "∀"}, {"exists", "∃"}, {"nexists", "∄"}, {"wedge", "∧"},
    {"land", "∧"}, {"vee", "∨"}, {"lor", "∨"}, {"neg", "¬"},
    {"lnot", "¬"}, {"top", "⊤"}, {"bot", "⊥"},
    // Arrows
    {"to", "→"}, {"rightarrow", "→"}, {"longrightarrow", "→"},
    {"leftarrow", "←"}, {"longleftarrow", "←"}, {"leftrightarrow", "↔"},
    {"Rightarrow", "⇒"}, {"Leftarrow", "⇐"}, {"Leftrightarrow", "⇔"},
    {"mapsto", "↦"},
    // Calculus and big operators
    {"infty", "∞"}, {"partial", "∂"}, {"nabla", "∇"}, {"sum", "∑"},
    {"prod", "∏"}, {"int", "∫"}, {"oint", "∮"}, {"cdots", "⋯"},
    {"ldots", "…"}, {"dots", "…"}, {"vdots", "⋮"}, {"ddots", "⋱"},
    // Misc
    {"angle", "∠"}, {"perp", "⊥"}, {"parallel", "∥"}, {"otimes", "⊗"},
    {"oplus", "⊕"}, {"hbar", "ℏ"}, {"ell", "ℓ"}, {"Re", "ℜ"},
    {"Im", "ℑ"}, {"aleph", "ℵ"}, {"circ", "∘"}, {"prime", "′"},
    {"dagger", "†"}, {"star", "⋆"}, {"ast", "∗"}, {"degree", "°"},
    // Named functions: the word itself, no glyph of its own
    {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"}, {"cot", "cot"}, {"sec", "sec"},
    {"csc", "csc"}, {"log", "log"}, {"ln", "ln"}, {"exp", "exp"}, {"lim", "lim"},
    {"max", "max"}, {"min", "min"}, {"gcd", "gcd"}, {"det", "det"}, {"dim", "dim"},
    {"ker", "ker"}, {"sup", "sup"}, {"inf", "inf"}, {"arg", "arg"},
};

std::string_view lookup(std::string_view name) {
  for (const Sym& s : kSymbols)
    if (s.cmd == name) return s.uni;
  return {};
}

bool is_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Unicode only has glyphs for a working subset of ASCII in super/subscript
// form. Anything outside it is reported by returning an empty view, so the
// caller can fall back to a parenthesised form instead of silently dropping it.
std::string_view superscript_of(char c) {
  switch (c) {
    case '0': return "⁰"; case '1': return "¹"; case '2': return "²";
    case '3': return "³"; case '4': return "⁴"; case '5': return "⁵";
    case '6': return "⁶"; case '7': return "⁷"; case '8': return "⁸";
    case '9': return "⁹"; case '+': return "⁺"; case '-': return "⁻";
    case '=': return "⁼"; case '(': return "⁽"; case ')': return "⁾";
    case 'n': return "ⁿ"; case 'i': return "ⁱ"; case 'T': return "ᵀ";
    default: return {};
  }
}

std::string_view subscript_of(char c) {
  switch (c) {
    case '0': return "₀"; case '1': return "₁"; case '2': return "₂";
    case '3': return "₃"; case '4': return "₄"; case '5': return "₅";
    case '6': return "₆"; case '7': return "₇"; case '8': return "₈";
    case '9': return "₉"; case '+': return "₊"; case '-': return "₋";
    case '=': return "₌"; case '(': return "₍"; case ')': return "₎";
    case 'a': return "ₐ"; case 'e': return "ₑ"; case 'o': return "ₒ";
    case 'x': return "ₓ"; case 'h': return "ₕ"; case 'k': return "ₖ";
    case 'l': return "ₗ"; case 'm': return "ₘ"; case 'n': return "ₙ";
    case 'p': return "ₚ"; case 's': return "ₛ"; case 't': return "ₜ";
    case 'i': return "ᵢ"; case 'j': return "ⱼ"; case 'u': return "ᵤ";
    case 'v': return "ᵥ"; case 'r': return "ᵣ";
    default: return {};
  }
}

void convert(std::string_view s, std::string& out);

// Reads the next "unit" after ^, _, \frac or \sqrt: a brace group, a single
// backslash command, or one plain character. Advances `i` past it.
std::string next_unit(std::string_view s, size_t& i) {
  if (i < s.size() && s[i] == '{') {
    size_t depth = 1, j = i + 1;
    while (j < s.size() && depth > 0) {
      if (s[j] == '{') depth++;
      else if (s[j] == '}') depth--;
      j++;
    }
    std::string inner;
    convert(s.substr(i + 1, (depth == 0 ? j - 1 : j) - (i + 1)), inner);
    i = j;
    return inner;
  }
  if (i < s.size() && s[i] == '\\') {
    size_t j = i + 1;
    while (j < s.size() && is_ident(s[j])) j++;
    std::string inner;
    convert(s.substr(i, j - i), inner);
    i = j;
    return inner;
  }
  if (i < s.size()) {
    std::string inner(1, s[i]);
    i++;
    return inner;
  }
  return {};
}

// Maps every byte of `unit` through a script table. An empty result means
// "not fully representable", which the caller treats as a signal to fall
// back to a parenthesised form rather than show a half-converted mess.
std::string scripted(const std::string& unit, std::string_view (*table)(char)) {
  std::string r;
  for (char c : unit) {
    std::string_view m = table(c);
    if (m.empty()) return {};
    r += m;
  }
  return r;
}

void convert(std::string_view s, std::string& out) {
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];

    if (c == '\\' && i + 1 < s.size()) {
      const char next = s[i + 1];
      if (!is_ident(next)) {
        // \{ \} \% \$ \& \_ \# \, (escaped literal), \| (a norm) or \\ (line break).
        if (next == '|') out += "‖";
        else out += (next == '\\') ? ' ' : next;
        i += 2;
        continue;
      }
      size_t j = i + 1;
      while (j < s.size() && is_ident(s[j])) j++;
      const std::string_view name = s.substr(i + 1, j - i - 1);
      i = j;

      if (name == "frac" || name == "dfrac" || name == "tfrac") {
        const std::string num = next_unit(s, i);
        const std::string den = next_unit(s, i);
        const bool simple_num = num.find(' ') == std::string::npos && num.size() <= 3;
        const bool simple_den = den.find(' ') == std::string::npos && den.size() <= 3;
        out += simple_num ? num : ("(" + num + ")");
        out += "⁄";  // fraction slash
        out += simple_den ? den : ("(" + den + ")");
        continue;
      }
      if (name == "sqrt") {
        std::string index;
        if (i < s.size() && s[i] == '[') {
          const size_t close = s.find(']', i + 1);
          if (close != std::string_view::npos) {
            convert(s.substr(i + 1, close - i - 1), index);
            i = close + 1;
          }
        }
        const std::string radicand = next_unit(s, i);
        if (!index.empty()) {
          const std::string sup = scripted(index, superscript_of);
          out += !sup.empty() ? sup : (index + "-");
        }
        out += "√";
        out += radicand.find(' ') == std::string::npos ? radicand : ("(" + radicand + ")");
        continue;
      }
      if (name == "text" || name == "mathrm" || name == "mathbf" || name == "boldsymbol" ||
          name == "mathit" || name == "operatorname" || name == "mathcal" || name == "mathbb") {
        out += next_unit(s, i);
        continue;
      }
      if (name == "left" || name == "right") continue;  // delimiter sizing only

      const std::string_view sym = lookup(name);
      // An unrecognised command still shows its name — a reader can guess at
      // "\oslash", but nothing at all gives them no chance to.
      out += !sym.empty() ? sym : name;
      continue;
    }

    if (c == '^' && i + 1 < s.size()) {
      size_t j = i + 1;
      const std::string unit = next_unit(s, j);
      const std::string sup = scripted(unit, superscript_of);
      out += !sup.empty() ? sup : ("^(" + unit + ")");
      i = j;
      continue;
    }
    if (c == '_' && i + 1 < s.size()) {
      size_t j = i + 1;
      const std::string unit = next_unit(s, j);
      const std::string sub = scripted(unit, subscript_of);
      out += !sub.empty() ? sub : ("_(" + unit + ")");
      i = j;
      continue;
    }

    // Bare grouping braces carry no meaning of their own once a command has
    // consumed its arguments; drop them rather than show LaTeX punctuation.
    if (c == '{' || c == '}') { i++; continue; }
    if (c == '~') { out += ' '; i++; continue; }  // non-breaking space ligature

    out.push_back(c);
    i++;
  }
}

}  // namespace

void to_unicode(std::string_view in, std::string& out) { convert(in, out); }

}  // namespace mico::latex
