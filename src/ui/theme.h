#pragma once
#include <vector>

#include "vt/surface.h"

namespace mico {

// One place to change every colour in the product. "Inspect and update the look
// of the chat" starts here.
struct Theme {
  Color bg = 0x101419;
  Color panel = 0x171D25;
  // Every other row of a list (projects, chats): a step off the panel, so a
  // two-line row reads as one and its neighbours apart.
  Color panel_alt = 0x1C232D;
  Color border = 0x303A47;
  Color border_focus = 0x74C7B6;
  Color text = 0xDCE4EE;
  Color dim = 0x929FB1;
  Color accent = 0x89D5C3;
  Color user = 0xADE5D7;
  Color assistant = 0xDCE4EE;
  Color thinking = 0x8A7FBF;
  Color tool = 0xD9A65C;
  Color ok = 0x7FD17F;
  Color err = 0xE06C75;
  Color warn = 0xD9A65C;
  // Markdown inks
  Color code = 0xE5C07B;
  Color link = 0x61AFEF;
  Color heading = 0x8FBCFF;
  Color quote = 0x9099AA;
  Color added = 0x7FD17F;
  Color removed = 0xE06C75;
  // A diff's rows: a tint for each kind of line, a stronger one for the words
  // that changed within it.
  Color added_bg = 0x1B2B24;
  Color added_strong = 0x2A4D35;
  Color removed_bg = 0x2E1E24;
  Color removed_strong = 0x55262F;
  Color hunk = 0x8A7FBF;
  Color math = 0x56B6C2;
  // Fenced code: its own surface, and a syntax palette that stays apart on it.
  Color code_bg = 0x1D2430;
  Color code_text = 0xD7DEE8;
  Color code_keyword = 0xC792EA;
  Color code_string = 0x9BCB82;
  Color code_comment = 0x7A8696;
  Color code_number = 0xF0A86B;
  Color code_type = 0xE5C07B;
  Color code_func = 0x6CB6F2;
  Color code_mark = 0x58636F;
  // Tool output's ANSI colours, the usual sixteen, tuned to sit on the panel.
  Color ansi[16] = {0x5C6370, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xC8CED8,
                    0x7F848E, 0xFF7A85, 0xB5E890, 0xF0D08A, 0x82C4FF, 0xDA9BF0, 0x7FD6E0, 0xFFFFFF};
  // Agent status
  Color working = 0x61AFEF;
  Color idle = 0x6B7280;
  Color attention = 0xE5C07B;
  // A user turn's block tint. Deliberately close to the panel: it should read
  // as a change of surface, not as a highlight competing with selection.
  Color user_bg = 0x213238;
  Color sel_bg = 0x29404A;
  Color sel_inactive = 0x222E39;
  Color menu_bg = 0x212B36;
  // The footer strip (state chips, and the prompt box when unfocused) reads as
  // its own surface below the chat, not as one more row of conversation.
  Color strip_bg = 0x1D2732;
};

// The themes to choose from, the first the default. The settings name the one
// in use (RenderSettings::theme).
struct NamedTheme {
  const char* name;
  const char* detail;
  Theme theme;
};
const std::vector<NamedTheme>& themes();
// The theme the settings name, or the first. Its contents change in place
// when the settings do, so a reference to it stays good.
const Theme& active_theme();

// How much of the conversation the chat view shows. This is the feature the
// PTY plane cannot provide, so it is first-class state rather than a toggle
// buried in a view.
enum class Density { Minimal, Normal, Full };
const char* density_name(Density d);

struct Filters {
  Density density = Density::Normal;
  bool show_thinking() const { return density == Density::Full; }
  bool show_tools() const { return density != Density::Minimal; }
  bool show_results() const { return density == Density::Full; }
  bool show_meta() const { return false; }
};

}  // namespace mico
