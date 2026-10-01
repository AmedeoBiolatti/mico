#pragma once
#include <string_view>

// What listing several agents' stored sessions has in common.
namespace mico {

// Wrapper text the agents inject around real user input; a session titled
// "<command-name>/model" helps nobody.
inline bool is_noise_prompt(std::string_view t) {
  static constexpr std::string_view kMarkers[] = {
      "<local-command", "<command-name>", "<system-reminder>", "Caveat:",
      "# AGENTS.md instructions", "<skills_instructions>", "<user_instructions>",
      "<environment_context>", "<recommended_plugins>", "<system-reminder>"};
  for (auto m : kMarkers)
    if (t.size() >= m.size() && t.compare(0, m.size(), m) == 0) return true;
  return false;
}

}  // namespace mico
