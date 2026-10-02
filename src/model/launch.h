#pragma once
#include <string>
#include <vector>

namespace mico {

// How to start an agent. Fresh sessions, resumes and forks differ only in
// argv and in whether the transcript id is known up front, so they share one
// path rather than three.
struct Launch {
  std::string agent;  // "claude", "codex", or a bare command
  std::string cwd;
  std::vector<std::string> argv;  // empty means "the default new-session argv"
  // Known transcript id. Claude honours --session-id even when forking, so a
  // fork is still correlated exactly. Codex has no such flag, and its id must
  // be discovered after the fact.
  std::string session_id;
  std::string origin;  // session this one continues from, for the title
  bool forked = false;
};

// What mico offers an agent at launch, on top of what it was asked to run.
// Appended, never replacing: an agent's own instructions stay, and a command
// line that already sets one of these keeps its own.
struct LaunchExtras {
  std::string hints;     // what mico can draw, for the agent's instructions; empty: none
  std::string mcp_exe;   // mico itself, to run as an MCP server (`mico --mcp`); empty: none
  std::string status_exe;  // mico itself, as the agent's status line, for its usage limits; empty: none
};

}  // namespace mico
