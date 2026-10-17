#include "views/markdown_style.h"

namespace mico {

// The row supplies the colour context, the segment's ink modifies it. Keeping
// them separate is why a bold run inside a user turn still reads as the user.
Style ink_style(Style base, md::Ink ink, const Theme& th) {
  switch (ink) {
    case md::Ink::Bold: base.a |= attr::kBold; break;
    case md::Ink::Italic: base.a |= attr::kItalic; break;
    case md::Ink::Code: base.fg = th.code; break;
    case md::Ink::Link: base.fg = th.link; base.a |= attr::kUnderline; break;
    case md::Ink::Heading: base.fg = th.heading; base.a |= attr::kBold; break;
    case md::Ink::Quote: base.fg = th.quote; base.a |= attr::kItalic; break;
    case md::Ink::Bullet: base.fg = th.accent; break;
    case md::Ink::Rule: base.fg = th.border; break;
    case md::Ink::Added: base.fg = th.added; break;
    case md::Ink::Removed: base.fg = th.removed; break;
    case md::Ink::Hunk: base.fg = th.hunk; base.a |= attr::kBold; break;
    case md::Ink::Math: base.fg = th.math; break;
    case md::Ink::CodeText: base.fg = th.code_text; break;
    case md::Ink::CodeKeyword: base.fg = th.code_keyword; break;
    case md::Ink::CodeString: base.fg = th.code_string; break;
    case md::Ink::CodeComment: base.fg = th.code_comment; base.a |= attr::kItalic; break;
    case md::Ink::CodeNumber: base.fg = th.code_number; break;
    case md::Ink::CodeType: base.fg = th.code_type; break;
    case md::Ink::CodeFunc: base.fg = th.code_func; break;
    case md::Ink::CodeMark: base.fg = th.code_mark; break;
    case md::Ink::Ansi0: case md::Ink::Ansi1: case md::Ink::Ansi2: case md::Ink::Ansi3:
    case md::Ink::Ansi4: case md::Ink::Ansi5: case md::Ink::Ansi6: case md::Ink::Ansi7:
    case md::Ink::Ansi8: case md::Ink::Ansi9: case md::Ink::Ansi10: case md::Ink::Ansi11:
    case md::Ink::Ansi12: case md::Ink::Ansi13: case md::Ink::Ansi14: case md::Ink::Ansi15:
      base.fg = th.ansi[int(ink) - int(md::Ink::Ansi0)];
      break;
    // Charts: dim axes, and series in colours that stay apart on the panel.
    case md::Ink::ChartAxis: base.fg = th.dim; break;
    case md::Ink::Series1: base.fg = th.accent; break;
    case md::Ink::Series2: base.fg = th.link; break;
    case md::Ink::Series3: base.fg = th.code; break;
    case md::Ink::Series4: base.fg = th.removed; break;
    case md::Ink::Series5: base.fg = th.thinking; break;
    case md::Ink::Series6: base.fg = th.added; break;
    default: break;
  }
  return base;
}

}  // namespace mico
