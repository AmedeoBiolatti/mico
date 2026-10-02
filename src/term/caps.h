#pragma once
#include <cstdint>

namespace mico {

// How a terminal raises a desktop notification. Bell is what every terminal
// has: most mark the window urgent on it.
enum class NotifyEscape : uint8_t {
  Bell,    // BEL
  Osc9,    // iTerm2's OSC 9: a body only
  Osc777,  // OSC 777;notify;title;body: foot, Ghostty, WezTerm
  Osc99,   // kitty's own notification protocol
};

// What a terminal can draw beyond text, learnt by asking it at startup.
struct GfxCaps {
  bool kitty = false;          // kitty graphics with Unicode placeholders
  bool sixel = false;          // sixel images (only when kitty is not available)
  bool tmux = false;           // inside tmux: graphics escapes are wrapped for passthrough
  int cell_w = 0, cell_h = 0;  // pixels per cell; 0 when the terminal will not say
  NotifyEscape notify = NotifyEscape::Bell;
  bool any() const { return kitty || sixel; }
  bool operator==(const GfxCaps&) const = default;
};

}  // namespace mico
