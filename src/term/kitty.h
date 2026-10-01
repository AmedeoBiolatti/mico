#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "vt/surface.h"

// The kitty graphics protocol, as far as mico needs it: an image is sent once
// with a virtual placement, and from then on it is drawn by the placeholder
// cells the encoder writes. Each terminal keeps its own set of what it holds.
namespace mico::math {

// The images one terminal holds, what they take there, and the frame each
// was last drawn in.
struct KittyHeld {
  struct Entry {
    uint64_t bytes = 0, frame = 0;
  };
  std::unordered_map<uint32_t, Entry> images;
  uint64_t bytes = 0, frame = 0;
  bool has(uint32_t id) const { return images.count(id) != 0; }
  bool empty() const { return images.empty(); }
};

// Appends the transmission of every image drawn on `s` that `held` does not
// have yet, and records them in `held`. A picture whose pixels are still
// being prepared is left out, to be sent by a later frame (see
// collect_prepared() in math/picture.h). Past a budget, the images drawn
// longest ago are freed from the terminal, and sent again if they come back.
// `tmux` wraps each command for tmux's passthrough.
void send_images(const Surface& s, KittyHeld& held, std::string& out, bool tmux = false);

// Frees images this terminal holds that are no longer drawn anywhere.
void free_images(const std::vector<uint32_t>& ids, KittyHeld& held, std::string& out,
                 bool tmux = false);

}  // namespace mico::math
