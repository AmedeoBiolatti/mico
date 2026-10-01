#include "core/settings.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#include "core/store.h"

namespace mico {

const std::vector<RenderChoice>& render_choices() {
  static const std::vector<RenderChoice> c = {
      {"pictures", "Pictures", "screenshots, image files and plots, at every density",
       {{"off", "a line naming the picture"},
        {"small", "up to 12 rows"},
        {"medium", "up to 24 rows"},
        {"large", "up to 48 rows"}},
       uint8_t(Pictures::Medium), uint8_t(Pictures::Medium)},
      {"math", "Equations", "LaTeX in replies",
       {{"source", "the LaTeX as written"},
        {"unicode", "approximated in text"},
        {"typeset", "drawn as pictures where the terminal can, else in text"}},
       uint8_t(Equations::Typeset), uint8_t(Equations::Typeset)},
      {"charts", "Charts", "```chart blocks and the plot tool",
       {{"source", "their JSON"},
        {"text", "drawn in characters"},
        {"pictures", "drawn as pictures where the terminal can, else in characters"}},
       uint8_t(Charts::Pictures), uint8_t(Charts::Pictures)},
      {"diagrams", "Diagrams", "```mermaid flowcharts, sequences and states",
       {{"source", "the mermaid as written"}, {"drawn", "drawn in box-drawing characters"}},
       uint8_t(Diagrams::Drawn), uint8_t(Diagrams::Drawn)},
      {"highlight", "Code colours", "code coloured by its language",
       {{"off", "plain"},
        {"blocks", "in fenced code blocks"},
        {"everywhere", "and in files a tool printed"}},
       uint8_t(CodeColours::Everywhere), uint8_t(CodeColours::Everywhere)},
      {"json", "JSON results", "tool results that are JSON",
       {{"raw", "as the tool wrote them"}, {"laid out", "indented, with containers to fold"}},
       uint8_t(JsonResults::Laid), uint8_t(JsonResults::Laid)},
      {"notebooks", "Notebooks", "Jupyter notebooks a tool printed",
       {{"raw", "their JSON"}, {"cells", "cells, outputs, tables and plots"}},
       uint8_t(Notebooks::Cells), uint8_t(Notebooks::Cells)},
      {"ansi", "Output colours", "the colours commands print",
       {{"plain", "colours taken out"}, {"kept", "as the command printed them"}},
       uint8_t(OutputColours::Kept), uint8_t(OutputColours::Kept)},
      {"links", "Links", "addresses found in text, made clickable",
       {{"off", "none"},
        {"urls", "web addresses"},
        {"urls and paths", "web addresses and file paths"}},
       uint8_t(Links::UrlsAndPaths), uint8_t(Links::UrlsAndPaths)},
      {"progress", "Progress", "a running command's progress, in the activity row",
       {{"off", "the command line alone"}, {"bar", "a bar, how far, and the time left"}},
       uint8_t(Progress::Bar), uint8_t(Progress::Bar)},
  };
  return c;
}

RenderSettings::RenderSettings() {
  const auto& c = render_choices();
  for (size_t i = 0; i < kRenderParts; i++) way[i] = c[i].fallback;
}

int RenderSettings::picture_rows() const {
  switch (pictures()) {
    case Pictures::Off: return 0;
    case Pictures::Small: return 12;
    case Pictures::Medium: return 24;
    case Pictures::Large: return 48;
  }
  return 24;
}

namespace {

std::optional<RenderSettings>& current() {
  static std::optional<RenderSettings> s;
  return s;
}
uint64_t& gen() {
  static uint64_t g = 1;
  return g;
}

std::string path() { return config_dir() + "/render"; }

// "name way" lines; what is not named, or names a way that does not exist,
// keeps its default. "on" and "off" are read as they meant when each part
// was a switch.
RenderSettings load() {
  RenderSettings s;
  FILE* f = fopen(path().c_str(), "r");
  if (!f) return s;
  const auto& choices = render_choices();
  char line[256];
  while (fgets(line, sizeof line, f)) {
    std::string_view l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.remove_suffix(1);
    const size_t sp = l.find(' ');
    if (sp == std::string_view::npos) continue;
    const std::string_view name = l.substr(0, sp), value = l.substr(sp + 1);
    if (name == "theme") {
      s.theme = std::string(value);
      continue;
    }
    for (size_t i = 0; i < choices.size(); i++) {
      if (name != choices[i].key) continue;
      if (value == "on") s.way[i] = choices[i].legacy_on;
      else if (value == "off") s.way[i] = 0;
      for (size_t v = 0; v < choices[i].variants.size(); v++)
        if (value == choices[i].variants[v].name) s.way[i] = uint8_t(v);
    }
  }
  fclose(f);
  return s;
}

}  // namespace

const RenderSettings& render_settings() {
  if (!current()) current() = load();
  return *current();
}

void set_render_settings(const RenderSettings& s, bool save) {
  if (current() && *current() == s) return;
  current() = s;
  gen()++;
  if (!save) return;
  mkdir(config_dir().c_str(), 0700);
  if (FILE* f = fopen(path().c_str(), "w")) {
    fprintf(f, "theme %s\n", s.theme.c_str());
    const auto& choices = render_choices();
    for (size_t i = 0; i < choices.size(); i++)
      fprintf(f, "%s %s\n", choices[i].key, choices[i].variants[s.way[i]].name);
    fclose(f);
  }
}

uint64_t render_settings_generation() { return gen(); }

void reload_render_settings() {
  current().reset();
  gen()++;
}

}  // namespace mico
