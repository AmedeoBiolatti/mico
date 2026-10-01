#pragma once
#include <cstdint>
#include <string>

namespace mico {

// A stored session, as the chat list shows it: what an adapter's
// list_sessions() reads off the head of a transcript.
struct SessionRef {
  std::string id;     // agent session uuid
  std::string path;   // transcript file
  std::string title;  // ai-title, else first user turn
  std::string agent;  // its adapter's id(): "claude", "codex", "pi", "omp"
  std::string cwd;
  int64_t mtime = 0;
  uint64_t bytes = 0;
  std::string sub{};  // the sub-project it belongs to, empty for none
};

}  // namespace mico
