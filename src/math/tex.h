#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The math an agent writes, parsed into the handful of structures TeX itself
// lays out: atoms, fractions, radicals, scripts, delimiters, arrays, accents.
// Forgiving by design: an unknown command becomes its name, an unbalanced
// brace closes at the end, and nothing ever fails to parse.
namespace mico::math {

// TeX's atom classes; they decide the spacing between neighbours.
enum class Cls : uint8_t { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

// The alphabets letters are drawn from.
enum class Font : uint8_t { Italic, Roman, Bold, BoldItalic, BB, Cal, Frak };

struct Node;
using List = std::vector<Node>;

struct Node {
  enum class K : uint8_t {
    Sym,      // one glyph: cp
    Text,     // upright text, spaces kept: text (UTF-8); also \sin and friends
    Group,    // {…}: kids[0]
    Frac,     // kids[0] over kids[1]; rule = false for \binom; open/close delimiters
    Sqrt,     // kids[0] under the radical, kids[1] the index (may be empty)
    Scripts,  // kids[0] base, kids[1] superscript, kids[2] subscript
    BigOp,    // a large operator glyph (cp) that takes limits
    Delim,    // \left open … \right close around kids[0]; \middle splits kids
    Big,      // a fixed-size delimiter (\big, \Bigg…): cp, size
    Array,    // rows of cells: kids row-major, `cols` per row; align per column
    Accent,   // cp over (or, with under, beneath) kids[0]
    Rule,     // \overline / \underline: kids[0]
    Brace,    // \overbrace / \underbrace: kids[0]
    Stack,    // \overset / \underset: kids[0] base, kids[1] over, kids[2] under
    Space,    // horizontal space: amount (em)
    Style,    // \displaystyle…: switches the rest of its list to `style`
    Boxed,    // \boxed: kids[0] in a frame
    Phantom,  // takes kids[0]'s room and draws nothing
    Not,      // a slash through the next atom
  };
  K k = K::Sym;
  Cls cls = Cls::Ord;
  Font font = Font::Italic;
  char32_t cp = 0;
  char32_t open = 0, close = 0;  // Delim, Frac (\binom), Array delimiters
  bool rule = true;              // Frac: draw the bar
  bool under = false;            // Accent, Rule, Brace: below instead of above
  bool wide = false;             // Accent: stretch to the base's width
  bool stretch_arrow = false;    // Accent: \overrightarrow and kin
  int8_t limits = -1;            // BigOp: -1 per style, 0 beside, 1 above/below
  int8_t style = -1;             // Frac: forced style; Style: the style
  uint8_t size = 0;              // Big: 1..4
  uint16_t cols = 0;             // Array
  float amount = 0;              // Space
  std::string text;              // Text; Array: one alignment letter per column
  std::vector<List> kids;
};

// A letter or digit in one of the math alphabets (other characters unchanged).
char32_t letter(char32_t c, Font f);

// Parses a math span's body (without the $ / \( / $$ around it). Top-level
// \\ and & make the result a single Array, the way an aligned block reads.
List parse(std::string_view src);

// True if the expression needs drawing rather than a line of Unicode: it has
// a fraction, radical, array, accent, stretched delimiter, big operator with
// limits, or a script Unicode has no characters for.
bool needs_drawing(const List& l);

}  // namespace mico::math
