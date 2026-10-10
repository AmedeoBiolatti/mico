#pragma once
#include <string>
#include <string_view>
#include <vector>

// Reading and extending the command line an agent is launched with.
namespace mico::cmdline {

// The program's own name, its directory stripped: "claude" for
// /usr/local/bin/claude. Empty for an empty command line.
std::string_view program(const std::vector<std::string>& argv);
// True when any argument contains `needle`: a flag the user already passed,
// in any of its spellings.
bool mentions(const std::vector<std::string>& argv, std::string_view needle);
// True when the command line passes `flag`, alone or as `flag=value`.
bool has_flag(const std::vector<std::string>& argv, std::string_view flag);

// Resolve the last working-directory flag against the launch folder, and
// make its argument absolute so repeated preparation cannot resolve it twice.
// The PTY still starts in the original folder; the agent changes it itself.
void adopt_cwd(std::vector<std::string>& argv, std::string& cwd,
               const std::vector<std::string_view>& flags);

// Reads the id a command line passes with --session-id into `id`, or, when it
// passes none, appends a fresh one. However it was launched, a session of an
// agent that takes an id gets one mico knows, so the chat view can find its
// transcript: "claude --model x" typed into the new-agent prompt would
// otherwise render raw-only.
void adopt_session_id(std::vector<std::string>& argv, std::string& id);

// `v` as a TOML basic string, quotes included.
std::string toml_string(std::string_view v);
// `v` as a JSON string, quotes included. Only quotes and backslashes are
// escaped: enough for a path.
std::string json_string(std::string_view v);

}  // namespace mico::cmdline
