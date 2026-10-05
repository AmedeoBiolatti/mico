#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Work an agent left running beside its turn — a monitor streaming a log's
// lines back, a command moved to the background — as its transcript tells
// it: started by a call's result, given events, ended by a notice. The agent
// shows it in its own footer ("2 monitors still running"); this is how the
// chat view can too.
namespace mico {

struct BackgroundTask {
  std::string id;    // the agent's own: "b4zoqx6hu"
  std::string kind;  // "monitor" or "shell"
  std::string what;  // its description, else its command, on one line
  int64_t started_ms = 0;  // unix milliseconds
  int64_t expires_ms = 0;  // when a monitor times out; 0 for never
  uint64_t offset = 0;     // the call's transcript line, to open the chat there
  std::string last_event;  // a monitor's latest line
  int64_t last_event_ms = 0;
  int events = 0;
  // Where what it prints goes, when known: progress is read from its end.
  std::string output;
  // The latest progress it printed (a tqdm bar, a [n/m] counter…), read
  // from that file or a monitor's events; `fraction` < 0 when none.
  double fraction = -1;
  int64_t done = -1, total = -1;
  int eta_s = -1;         // the command's own estimate of the time left
  int64_t since_ms = 0;   // when progress was first seen, and how far it was:
  double since_fraction = 0;  // the pace, for an estimate when it gives none
};

struct BackgroundTasks {
  std::vector<BackgroundTask> running;  // oldest first
  // Calls that may start or stop a task, by the agent's call id, until their
  // result says which.
  struct Call {
    std::string kind;  // "monitor", "shell", or "stop"
    std::string what;  // for "stop", the task it stops
    int64_t at_ms = 0, timeout_ms = 0;
    uint64_t offset = 0;
  };
  std::unordered_map<std::string, Call> calls;
  uint64_t version = 0;  // moves on every change

  void start(const std::string& id, const Call& c, const std::string& output = {}) {
    for (const auto& t : running)
      if (t.id == id) return;
    BackgroundTask t;
    t.id = id;
    t.output = output;
    t.kind = c.kind;
    t.what = c.what;
    t.started_ms = c.at_ms;
    t.expires_ms = c.timeout_ms > 0 && c.at_ms > 0 ? c.at_ms + c.timeout_ms : 0;
    t.offset = c.offset;
    running.push_back(std::move(t));
    version++;
  }
  void end(const std::string& id) {
    for (size_t i = 0; i < running.size(); i++)
      if (running[i].id == id) {
        running.erase(running.begin() + long(i));
        version++;
        return;
      }
  }
  // The same notice can be recorded more than once (queued, then delivered):
  // a line that repeats the last is not counted again.
  void event(const std::string& id, const std::string& text, int64_t at_ms) {
    for (auto& t : running)
      if (t.id == id && t.last_event != text) {
        t.last_event = text;
        t.last_event_ms = at_ms;
        t.events++;
        version++;
      }
  }
  // Monitors past their time, a minute's grace given for the notice.
  void expire(int64_t now_ms) {
    for (size_t i = 0; i < running.size();)
      if (running[i].expires_ms && now_ms > running[i].expires_ms + 60000) {
        running.erase(running.begin() + long(i));
        version++;
      } else {
        i++;
      }
  }
  void clear() {
    if (running.empty() && calls.empty()) return;
    running.clear();
    calls.clear();
    version++;
  }
};

}  // namespace mico
