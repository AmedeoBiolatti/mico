#include "core/settings.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#include "core/store.h"

namespace mico {
namespace {

struct Field {
  const char* name;
  bool RenderSettings::*member;
};
constexpr Field kFields[] = {
    {"pictures", &RenderSettings::pictures},   {"math", &RenderSettings::math},
    {"charts", &RenderSettings::charts},       {"diagrams", &RenderSettings::diagrams},
    {"highlight", &RenderSettings::highlight}, {"json", &RenderSettings::json},
    {"notebooks", &RenderSettings::notebooks}, {"ansi", &RenderSettings::ansi},
    {"links", &RenderSettings::links},         {"progress", &RenderSettings::progress},
};

std::optional<RenderSettings>& current() {
  static std::optional<RenderSettings> s;
  return s;
}
uint64_t& gen() {
  static uint64_t g = 1;
  return g;
}

std::string path() { return config_dir() + "/render"; }

// "name off" lines; what is not named keeps its default.
RenderSettings load() {
  RenderSettings s;
  FILE* f = fopen(path().c_str(), "r");
  if (!f) return s;
  char line[128];
  while (fgets(line, sizeof line, f)) {
    std::string_view l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.remove_suffix(1);
    const size_t sp = l.find(' ');
    if (sp == std::string_view::npos) continue;
    const std::string_view name = l.substr(0, sp), value = l.substr(sp + 1);
    for (const auto& fd : kFields)
      if (name == fd.name) s.*fd.member = value != "off";
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
    for (const auto& fd : kFields) fprintf(f, "%s %s\n", fd.name, s.*fd.member ? "on" : "off");
    fclose(f);
  }
}

uint64_t render_settings_generation() { return gen(); }

void reload_render_settings() {
  current().reset();
  gen()++;
}

}  // namespace mico
