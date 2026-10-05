#include "ui/theme.h"

#include "core/settings.h"

namespace mico {
namespace {

Theme light() {
  // A mid-tone paper, not white: surfaces step darker so panels, code and
  // selections stay distinct, and inks are deep enough to hold contrast on it.
  Theme t;
  t.bg = 0xE6E4DE;
  t.panel = 0xDCDAD3;
  t.panel_alt = 0xD2D0C8;
  t.border = 0xA9A69C;
  t.border_focus = 0x1F7A6C;
  t.text = 0x1B1F24;
  t.dim = 0x525A64;
  t.accent = 0x15695C;
  t.user = 0x135A4F;
  t.assistant = 0x1B1F24;
  t.thinking = 0x5E41A0;
  t.tool = 0x84500A;
  t.ok = 0x1F6F32;
  t.err = 0xB02A2E;
  t.warn = 0x84500A;
  t.code = 0x7E4A08;
  t.link = 0x0A55AB;
  t.heading = 0x1F4A96;
  t.quote = 0x545C66;
  t.added = 0x1F6F32;
  t.removed = 0xB02A2E;
  t.added_bg = 0xCCE4D2;
  t.added_strong = 0xA5D1B0;
  t.removed_bg = 0xEBD0D0;
  t.removed_strong = 0xDDA9A9;
  t.hunk = 0x5E41A0;
  t.math = 0x0F6874;
  t.code_bg = 0xD3D1C9;
  t.code_text = 0x1B1F24;
  t.code_keyword = 0x7A2FA3;
  t.code_string = 0x1F6A25;
  t.code_comment = 0x626A75;
  t.code_number = 0x9A4604;
  t.code_type = 0x7E4A08;
  t.code_func = 0x154F9A;
  t.code_mark = 0x8E8B82;
  const Color ansi[16] = {0x1B1F24, 0xB02A2E, 0x1F6F32, 0x7E5800, 0x0A55AB, 0x7A2FA3, 0x0F6874, 0x5A626C,
                          0x424A54, 0xC8433F, 0x2E8A48, 0x956A00, 0x2370D4, 0x9449BE, 0x1A8896, 0x1B1F24};
  for (int i = 0; i < 16; i++) t.ansi[i] = ansi[i];
  t.working = 0x0A55AB;
  t.idle = 0x7A828C;
  t.attention = 0x84580A;
  t.user_bg = 0xCADBD6;
  t.sel_bg = 0xB3CCE0;
  t.sel_inactive = 0xCDD4DA;
  t.menu_bg = 0xEEEDE9;
  t.strip_bg = 0xD0CEC6;
  return t;
}

Theme dracula() {
  Theme t;
  t.bg = 0x282A36;
  t.panel = 0x2D2F3D;
  t.panel_alt = 0x343746;
  t.border = 0x44475A;
  t.border_focus = 0xBD93F9;
  t.text = 0xF8F8F2;
  t.dim = 0x9AA3CC;
  t.accent = 0xBD93F9;
  t.user = 0x8BE9FD;
  t.assistant = 0xF8F8F2;
  t.thinking = 0x9AA3CC;
  t.tool = 0xFFB86C;
  t.ok = 0x50FA7B;
  t.err = 0xFF5555;
  t.warn = 0xFFB86C;
  t.code = 0xF1FA8C;
  t.link = 0x8BE9FD;
  t.heading = 0xBD93F9;
  t.quote = 0x8A93BD;
  t.added = 0x50FA7B;
  t.removed = 0xFF5555;
  t.added_bg = 0x1F3A2D;
  t.added_strong = 0x2C6340;
  t.removed_bg = 0x3E2631;
  t.removed_strong = 0x70303D;
  t.hunk = 0xFF79C6;
  t.math = 0x8BE9FD;
  t.code_bg = 0x21222C;
  t.code_text = 0xF8F8F2;
  t.code_keyword = 0xFF79C6;
  t.code_string = 0xF1FA8C;
  t.code_comment = 0x6272A4;
  t.code_number = 0xBD93F9;
  t.code_type = 0x8BE9FD;
  t.code_func = 0x50FA7B;
  t.code_mark = 0x565A72;
  const Color ansi[16] = {0x6272A4, 0xFF5555, 0x50FA7B, 0xF1FA8C, 0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xF8F8F2,
                          0x7B86B3, 0xFF6E6E, 0x69FF94, 0xFFFFA5, 0xD6ACFF, 0xFF92DF, 0xA4FFFF, 0xFFFFFF};
  for (int i = 0; i < 16; i++) t.ansi[i] = ansi[i];
  t.working = 0x8BE9FD;
  t.idle = 0x6272A4;
  t.attention = 0xF1FA8C;
  t.user_bg = 0x343A52;
  t.sel_bg = 0x44475A;
  t.sel_inactive = 0x3A3D4E;
  t.menu_bg = 0x343746;
  t.strip_bg = 0x303241;
  return t;
}

Theme high_contrast() {
  Theme t;
  t.bg = 0x000000;
  t.panel = 0x000000;
  t.panel_alt = 0x111417;
  t.border = 0x8A8A8A;
  t.border_focus = 0x00E8C2;
  t.text = 0xFFFFFF;
  t.dim = 0xC4C4C4;
  t.accent = 0x00E8C2;
  t.user = 0x7DFFE2;
  t.assistant = 0xFFFFFF;
  t.thinking = 0xC9B2FF;
  t.tool = 0xFFC94D;
  t.ok = 0x5CFF7A;
  t.err = 0xFF5E6C;
  t.warn = 0xFFC94D;
  t.code = 0xFFDA66;
  t.link = 0x6CC6FF;
  t.heading = 0xA2CCFF;
  t.quote = 0xD0D0D0;
  t.added = 0x5CFF7A;
  t.removed = 0xFF5E6C;
  t.added_bg = 0x0C3016;
  t.added_strong = 0x1D622D;
  t.removed_bg = 0x3A0C12;
  t.removed_strong = 0x741E29;
  t.hunk = 0xC9B2FF;
  t.math = 0x5CE8F2;
  t.code_bg = 0x141414;
  t.code_text = 0xFFFFFF;
  t.code_keyword = 0xE4A4FF;
  t.code_string = 0xADF482;
  t.code_comment = 0xA8A8A8;
  t.code_number = 0xFFB46E;
  t.code_type = 0xFFDA66;
  t.code_func = 0x82CBFF;
  t.code_mark = 0x8A8A8A;
  const Color ansi[16] = {0x8A8A8A, 0xFF5E6C, 0x5CFF7A, 0xFFDA66, 0x6CC6FF, 0xE4A4FF, 0x5CE8F2, 0xE6E6E6,
                          0xB0B0B0, 0xFF8A94, 0x9CFFAE, 0xFFE89A, 0xA4DCFF, 0xF0C8FF, 0x9CF4FA, 0xFFFFFF};
  for (int i = 0; i < 16; i++) t.ansi[i] = ansi[i];
  t.working = 0x6CC6FF;
  t.idle = 0x9E9E9E;
  t.attention = 0xFFDA66;
  t.user_bg = 0x0D2B2A;
  t.sel_bg = 0x1E4F6E;
  t.sel_inactive = 0x1B2833;
  t.menu_bg = 0x161616;
  t.strip_bg = 0x121212;
  return t;
}

Theme warm() {
  Theme t;
  t.bg = 0x1D2021;
  t.panel = 0x282828;
  t.panel_alt = 0x302E2C;
  t.border = 0x504945;
  t.border_focus = 0x8EC07C;
  t.text = 0xEBDBB2;
  t.dim = 0xA89984;
  t.accent = 0x8EC07C;
  t.user = 0x9CCFA0;
  t.assistant = 0xEBDBB2;
  t.thinking = 0xD3869B;
  t.tool = 0xFABD2F;
  t.ok = 0xB8BB26;
  t.err = 0xFB4934;
  t.warn = 0xFE8019;
  t.code = 0xFABD2F;
  t.link = 0x83A598;
  t.heading = 0x83A598;
  t.quote = 0xA89984;
  t.added = 0xB8BB26;
  t.removed = 0xFB4934;
  t.added_bg = 0x2E3324;
  t.added_strong = 0x45502A;
  t.removed_bg = 0x3C2422;
  t.removed_strong = 0x6A2E28;
  t.hunk = 0xD3869B;
  t.math = 0x8EC07C;
  t.code_bg = 0x32302F;
  t.code_text = 0xEBDBB2;
  t.code_keyword = 0xD3869B;
  t.code_string = 0xB8BB26;
  t.code_comment = 0x928374;
  t.code_number = 0xFE8019;
  t.code_type = 0xFABD2F;
  t.code_func = 0x8EC07C;
  t.code_mark = 0x665C54;
  const Color ansi[16] = {0x7C6F64, 0xFB4934, 0xB8BB26, 0xFABD2F, 0x83A598, 0xD3869B, 0x8EC07C, 0xD5C4A1,
                          0x928374, 0xFC6A57, 0xCDD03F, 0xFBCB55, 0x9DBBAE, 0xDEA0B1, 0xA6CF97, 0xFBF1C7};
  for (int i = 0; i < 16; i++) t.ansi[i] = ansi[i];
  t.working = 0x83A598;
  t.idle = 0x7C6F64;
  t.attention = 0xFABD2F;
  t.user_bg = 0x2F3326;
  t.sel_bg = 0x45403D;
  t.sel_inactive = 0x3A3634;
  t.menu_bg = 0x32302F;
  t.strip_bg = 0x2C2B2A;
  return t;
}

}  // namespace

const std::vector<NamedTheme>& themes() {
  static const std::vector<NamedTheme> all = {
      {"dark", "mico's own: deep blue-grey, teal accents", Theme{}},
      {"light", "a mid-tone paper with dark ink, for a light terminal or a bright room", light()},
      {"high contrast", "black and white, saturated accents", high_contrast()},
      {"warm", "browns and muted earth colours", warm()},
      {"dracula", "purple, pink and cyan on a dark slate", dracula()},
  };
  return all;
}

const Theme& active_theme() {
  static Theme current;
  static uint64_t seen = 0;
  if (seen != render_settings_generation()) {
    seen = render_settings_generation();
    current = themes().front().theme;
    for (const auto& t : themes())
      if (render_settings().theme == t.name) current = t.theme;
  }
  return current;
}

}  // namespace mico
