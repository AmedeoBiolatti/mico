#pragma once

namespace mico {

// What a terminal can draw beyond text, learnt by asking it at startup.
struct GfxCaps {
  bool kitty = false;          // kitty graphics with Unicode placeholders
  bool sixel = false;          // sixel images (only when kitty is not available)
  bool tmux = false;           // inside tmux: graphics escapes are wrapped for passthrough
  int cell_w = 0, cell_h = 0;  // pixels per cell; 0 when the terminal will not say
  bool any() const { return kitty || sixel; }
  bool operator==(const GfxCaps&) const = default;
};

}  // namespace mico
