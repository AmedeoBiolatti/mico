#pragma once
#include <string>
#include <string_view>

#include "term/caps.h"
#include "vt/surface.h"

namespace mico {

// Appends the escape sequences that turn `front` into `back`, and updates
// `front` to match. Only changed cells are written.
//
// This is the entire daemon-to-client payload: the daemon renders, diffs, and
// ships bytes the client writes to its terminal verbatim.
// Returns true if anything was written; an unchanged frame emits nothing, so
// an idle daemon sends no bytes at all.
// How a terminal is shown image cells: not at all (blanks), as kitty Unicode
// placeholders, or not by the encoder — a sixel pass draws them afterwards,
// so the encoder leaves them, and their `front` cells, untouched.
enum class ImageMode : uint8_t { None, Kitty, Sixel };

bool encode_frame(const Surface& back, Surface& front, std::string& out, bool full,
                  ImageMode images = ImageMode::None);

// One cell written at (x, y) with its full style, for a pass that has drawn
// over it and must put it back.
void encode_cell_at(const Cell& c, int x, int y, std::string& out);

// Terminal control that travels with a frame rather than through a new message
// type: the client writes whatever bytes it is given.
std::string mouse_mode_seq(bool on, bool any_event = false);
// OSC 52: sets the system clipboard, and works across ssh.
std::string clipboard_seq(std::string_view text);
// A desktop notification, the way `how` says this terminal raises one; a
// bell where it has no other way. It too crosses ssh.
std::string notify_seq(NotifyEscape how, std::string_view title, std::string_view body);

}  // namespace mico
