#pragma once
#include <cstdint>
#include <string>
#include <vector>

// How the chat draws each kind of thing agents write, one of a few ways each,
// and the colours it draws them in. Set in the Settings tab and kept in
// ~/.config/mico/render. Independent of density: density says how much of the
// conversation to show, these how to show it.
namespace mico {

// The ways each part can be drawn. The first of each is the plainest.
enum class Pictures : uint8_t { Off, Small, Medium, Large };
enum class Equations : uint8_t { Source, Unicode, Typeset };
enum class Charts : uint8_t { Source, Text, Pictures };
enum class Diagrams : uint8_t { Source, Drawn };
enum class CodeColours : uint8_t { Off, Blocks, Everywhere };
enum class JsonResults : uint8_t { Raw, Laid };
enum class Notebooks : uint8_t { Raw, Cells };
enum class OutputColours : uint8_t { Plain, Kept };
enum class Links : uint8_t { Off, Urls, UrlsAndPaths };
enum class Progress : uint8_t { Off, Bar };

// One of the parts above, and its ways, for the settings that list them.
struct RenderChoice {
  const char* key;     // in the file: "pictures"
  const char* label;   // "Pictures"
  const char* detail;  // what the part is
  struct Variant {
    const char* name;    // in the file and on screen: "medium"
    const char* detail;  // what this way draws
  };
  std::vector<Variant> variants;
  uint8_t fallback;    // the default
  uint8_t legacy_on;   // what "on" meant when each part was a switch ("off" is 0)
};
enum RenderPart : uint8_t {
  kPictures, kEquations, kCharts, kDiagrams, kCodeColours, kJsonResults, kNotebooks,
  kOutputColours, kLinks, kProgress, kRenderParts
};
// In the order the settings list them; indexed by RenderPart.
const std::vector<RenderChoice>& render_choices();

struct RenderSettings {
  uint8_t way[kRenderParts];
  std::string theme = "dark";  // a name from themes(), ui/theme.h

  RenderSettings();
  Pictures pictures() const { return Pictures(way[kPictures]); }
  Equations equations() const { return Equations(way[kEquations]); }
  Charts charts() const { return Charts(way[kCharts]); }
  Diagrams diagrams() const { return Diagrams(way[kDiagrams]); }
  CodeColours code_colours() const { return CodeColours(way[kCodeColours]); }
  JsonResults json() const { return JsonResults(way[kJsonResults]); }
  Notebooks notebooks() const { return Notebooks(way[kNotebooks]); }
  OutputColours output_colours() const { return OutputColours(way[kOutputColours]); }
  Links links() const { return Links(way[kLinks]); }
  Progress progress() const { return Progress(way[kProgress]); }
  // The most rows a picture may take, by its size; 0 when pictures are off.
  int picture_rows() const;

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
