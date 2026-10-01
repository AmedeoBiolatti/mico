#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "model/commands.h"

// Built-in "/" menus, written down as tables.
namespace mico {

// A table row; std::string cannot be constexpr.
struct Row {
  const char* name;
  const char* description;
  const char* hint;
};

template <size_t N>
std::vector<SlashCommand> copy(const Row (&a)[N]) {
  std::vector<SlashCommand> out;
  out.reserve(N);
  for (const Row& r : a) out.push_back({r.name, r.description, r.hint});
  return out;
}

// The first line of a description, trimmed: menus show one row.
inline std::string one_line(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n\"'");
  if (a == std::string_view::npos) return {};
  s.remove_prefix(a);
  s = s.substr(0, s.find('\n'));
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '"' || s.back() == '\''))
    s.remove_suffix(1);
  return std::string(s);
}

}  // namespace mico
