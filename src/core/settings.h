#pragma once
#include <cstdint>
#include <string>

// What the chat renders, each part on or off, set in the Settings tab and
// kept in ~/.config/mico/render. Independent of density: density says how
// much of the conversation to show, these how to show it.
namespace mico {

struct RenderSettings {
  bool pictures = true;   // screenshots, image files, plots — shown at every density
  bool math = true;       // LaTeX typeset as pictures (off: Unicode)
  bool charts = true;     // ```chart blocks and the plot tool drawn (off: their JSON)
  bool diagrams = true;   // ```mermaid drawn (off: its source)
  bool highlight = true;  // code coloured by language
  bool json = true;       // JSON results laid out and foldable
  bool notebooks = true;  // notebooks shown as cells, outputs, plots
  bool ansi = true;       // tool output keeps its colours
  bool links = true;      // URLs and file paths found and clickable
  bool progress = true;   // a running command's progress drawn as a bar

  bool operator==(const RenderSettings&) const = default;
};

// The settings in force: read from the file the first time.
const RenderSettings& render_settings();
// Puts `s` in force and saves it (unless `save` is false: the tests). Every
// chat lays itself out again.
void set_render_settings(const RenderSettings& s, bool save = true);
// Bumped by each change, so a layout can tell it is out of date.
uint64_t render_settings_generation();

// For tests: forget what was read, so the next call reads the file again.
void reload_render_settings();

}  // namespace mico
