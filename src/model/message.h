#pragma once
#include <string>

namespace mico {

// A message as composed: text, and images as their file paths, in order. An
// image has to reach the agent as a paste of just its path; that is what makes
// claude and codex attach it.
struct MessagePart {
  bool image = false;
  std::string text;  // text, or an image's path
};

}  // namespace mico
