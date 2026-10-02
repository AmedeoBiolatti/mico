#pragma once
#include <cstdint>
#include <string>
#include <vector>

// What mico does for you while you are looking elsewhere: it says when an
// agent has finished or needs you, and the agents a daemon was running when
// it went away are resumed by the next one.
namespace mico {

// How an agent that has finished, or needs you, is announced. Kept in
// ~/.config/mico/notify; desktop unless set.
enum class NotifyMode : uint8_t {
  Off,      // only the sidebar's dots and the status bar's count
  Bell,     // the terminal's bell: most mark the window urgent
  Desktop,  // a desktop notification, through the terminal where it can
};
NotifyMode notify_mode();
void set_notify_mode(NotifyMode m);

// Whether a daemon resumes the agents its predecessor was running. Kept in
// ~/.config/mico/restore; on unless turned off.
bool restore_agents_enabled();
void set_restore_agents(bool on);

// One agent a daemon was running, as much as resuming it needs.
struct RunningAgent {
  std::string agent;       // its adapter: "claude"
  std::string session_id;  // what the agent resumes
  std::string cwd;
  bool operator==(const RunningAgent&) const = default;
};
// $XDG_STATE_HOME/mico/running, one "agent\tid\tcwd" per line.
std::string running_path();
std::vector<RunningAgent> read_running();
// Replaces the file; an empty list removes it.
void write_running(const std::vector<RunningAgent>& agents);

}  // namespace mico
