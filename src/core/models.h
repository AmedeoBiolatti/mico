#pragma once
#include <string>
#include <vector>

#include "adapters/adapter.h"
#include "core/pty.h"
#include "model/commands.h"
#include "vt/vt.h"

namespace mico {

// Reads the models an agent actually offers out of its own picker.
//
// Claude has no CLI that lists them — the list lives behind /model, changes
// with the account, and gains entries when new models ship. Hardcoding it here
// means mico goes stale every time that happens. So mico asks, once, in a
// throwaway process: the user's own sessions are never typed into, never see a
// picker open over their work, and never carry a /model they did not run.
class ModelProbe {
 public:
  // Opens `agent`'s picker (Adapter::model_picker_command) in a throwaway
  // session in `cwd`, and reads it with Adapter::read_model_picker().
  void start(const Adapter& agent, const std::string& cwd);
  bool started() const { return started_; }
  // Drives one step. True when the probe finished this call, successfully or
  // not — the caller repaints because the menu it feeds has changed.
  bool pump();
  bool done() const { return done_; }
  // Running, and paced by time rather than by its pty, whose fd is not polled.
  bool running() const { return started_ && !done_; }

 private:
  enum class Step { Starting, Asking, Reading };
  void finish(std::vector<ModelOption> found, const char* why);

  Pty pty_;
  Vt vt_;
  const Adapter* adapter_ = nullptr;
  std::string agent_;
  Step step_ = Step::Starting;
  bool started_ = false;
  bool done_ = false;
  int64_t began_ms_ = 0;
  int64_t quiet_since_ms_ = 0;
  std::string buf_;
};

// What the last successful probe found, or empty when none has run. The chip
// menu falls back to opening the agent's own picker while this is empty, so a
// probe that never lands costs correctness, not function.
const std::vector<ModelOption>& known_models(const std::string& agent);
void set_known_models(const std::string& agent, std::vector<ModelOption> v);
// The effort levels the agent reports its models support, in its order;
// empty when nothing has said. Read out of claude's initialize answer.
const std::vector<std::string>& known_efforts(const std::string& agent);
void set_known_efforts(const std::string& agent, std::vector<std::string> v);

}  // namespace mico
