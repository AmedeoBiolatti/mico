#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "term/surface.h"

// The kitty graphics protocol, as far as mico needs it: an image is sent once
// with a virtual placement, and from then on it is drawn by the placeholder
// cells the encoder writes. Each terminal keeps its own set of what it holds.
namespace mico::math {

// Appends the transmission of every image drawn on `s` that `sent` has not
// had yet, and records them in `sent`.
// `tmux` wraps each command for tmux's passthrough.
void send_images(const Surface& s, std::unordered_set<uint32_t>& sent, std::string& out,
                 bool tmux = false);

// Frees images this terminal holds that are no longer drawn anywhere.
void free_images(const std::vector<uint32_t>& ids, std::unordered_set<uint32_t>& sent,
                 std::string& out, bool tmux = false);

}  // namespace mico::math
