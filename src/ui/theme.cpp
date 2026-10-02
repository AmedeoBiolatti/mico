#include "ui/theme.h"

#include "core/settings.h"

namespace mico {
namespace {

Theme light() {
  Theme t;
  t.bg = 0xFAFAF8;
  t.panel = 0xF4F3F0;
  t.panel_alt = 0xEBEAE5;
  t.border = 0xD3D0CA;
  t.border_focus = 0x2A8C7C;
  t.text = 0x24292F;
  t.dim = 0x6A737D;
  t.accent = 0x1F7A6C;
  t.user = 0x1B6B5F;
  t.assistant = 0x24292F;
  t.thinking = 0x7457B5;
  t.tool = 0x9C5F10;
  t.ok = 0x2C8440;
  t.err = 0xC0363A;
  t.warn = 0x9C5F10;
  t.code = 0x955A0C;
  t.link = 0x0B62C4;
  t.heading = 0x2958AE;
  t.quote = 0x6A737D;
  t.added = 0x2C8440;
  t.removed = 0xC0363A;
  t.added_bg = 0xE4F3E8;
  t.added_strong = 0xC2E6CB;
  t.removed_bg = 0xFBE8E8;
  t.removed_strong = 0xF3C4C4;
  t.hunk = 0x7457B5;
  t.math = 0x177C89;
  t.code_bg = 0xEBE9E4;
  t.code_text = 0x24292F;
  t.code_keyword = 0x8A3DB2;
  t.code_string = 0x2B7A30;
  t.code_comment = 0x78808B;
  t.code_number = 0xB0530B;
  t.code_type = 0x955A0C;
  t.code_func = 0x1D5DAE;
  t.code_mark = 0xA9AEB5;
  const Color ansi[16] = {0x24292F, 0xC0363A, 0x2C8440, 0x946400, 0x0B62C4, 0x8A3DB2, 0x177C89, 0x6A737D,
                          0x57606A, 0xD64F4B, 0x38A158, 0xB17E00, 0x2D7FE9, 0xA25BD0, 0x1F9FAD, 0x24292F};
  for (int i = 0; i < 16; i++) t.ansi[i] = ansi[i];
  t.working = 0x0B62C4;
  t.idle = 0x8C949E;
  t.attention = 0x9C6400;
  t.user_bg = 0xE2EEEB;
  t.sel_bg = 0xCCE1F2;
  t.sel_inactive = 0xE3E8ED;
  t.menu_bg = 0xFFFFFF;
  t.strip_bg = 0xEBE9E4;
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
      {"light", "a light surface, for a light terminal or a bright room", light()},
      {"high contrast", "black and white, saturated accents", high_contrast()},
      {"warm", "browns and muted earth colours", warm()},
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
