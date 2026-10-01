#include "core/models.h"

#include <time.h>

#include <algorithm>
#include <map>

#include "base/log.h"
#include "adapters/screen.h"

namespace mico {
namespace {

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

void ModelProbe::start(const Adapter& agent, const std::string& cwd) {
  if (started_) return;
  started_ = true;
  adapter_ = &agent;
  agent_ = agent.id();
  began_ms_ = now_ms();
  quiet_since_ms_ = began_ms_;
  // Big enough that the picker is never truncated or scrolled into a shape
  // the parser cannot see.
  vt_.resize(120, 50);
  if (!pty_.spawn({agent_}, cwd, 120, 50)) done_ = true;
}

void ModelProbe::finish(std::vector<ModelOption> found, const char* why) {
  // The command catalog's probe may read the same list out of the agent's own
  // answer, with descriptions this scrape cannot see; when that landed first,
  // its list stands.
  if (!found.empty() && !known_models(agent_).empty()) return;
  if (!found.empty()) {
    std::string list;
    for (const auto& m : found) list += " " + m.value;
    MLOG("model probe: %s found %zu:%s", agent_.c_str(), found.size(), list.c_str());
    set_known_models(agent_, std::move(found));
  } else {
    // Without the screen there is no telling a restyled picker from one that
    // never opened, so the log gets what the probe saw.
    MLOG("model probe: %s found nothing (%s); screen was:", agent_.c_str(), why);
    for (int i = 0; i < vt_.total_rows(); i++)
      if (std::string r = row_text(vt_.row(i)); !r.empty()) MLOG("  | %s", r.c_str());
  }
  pty_.terminate();
  done_ = true;
}

bool ModelProbe::pump() {
  if (!started_) return false;
  if (done_) {
    pty_.poll_exit();  // reap the killed probe rather than leave a zombie
    return false;
  }
  pty_.poll_exit();
  const int64_t now = now_ms();

  buf_.clear();
  const bool alive = pty_.read_available(buf_);
  if (!buf_.empty()) {
    vt_.write(buf_);
    quiet_since_ms_ = now;
  }

  // A picker that never arrives must not hold a process open for the life of
  // the app.
  if (!alive || pty_.exited() || now - began_ms_ > 25000) {
    finish(adapter_->read_model_picker(vt_), pty_.exited() || !alive ? "exited" : "timed out");
    return true;
  }

  // The trust dialog wants a decision about the user's files. Answering it on
  // their behalf is not mico's call, so the probe simply gives up.
  if (adapter_->startup_prompt(vt_)) {
    finish({}, "trust dialog");
    return true;
  }

  switch (step_) {
    case Step::Starting:
      // An agent paints its banner, its tips and its input box in several
      // bursts; a settled screen is the only honest "ready".
      if (now - quiet_since_ms_ > 700 && now - began_ms_ > 1200) {
        pty_.write(adapter_->model_picker_command());
        step_ = Step::Asking;
        quiet_since_ms_ = now;
      }
      return false;
    case Step::Asking:
      if (now - quiet_since_ms_ > 500) step_ = Step::Reading;
      return false;
    case Step::Reading: {
      std::vector<ModelOption> found = adapter_->read_model_picker(vt_);
      if (found.size() >= 2) {
        finish(std::move(found), "");
        return true;
      }
      // Still drawing, or the picker never opened. The overall timeout above
      // ends it either way.
      return false;
    }
  }
  return false;
}

namespace {
std::map<std::string, std::vector<ModelOption>>& cache() {
  static std::map<std::string, std::vector<ModelOption>> c;
  return c;
}
}  // namespace

const std::vector<ModelOption>& known_models(const std::string& agent) {
  static const std::vector<ModelOption> none;
  auto it = cache().find(agent);
  return it == cache().end() ? none : it->second;
}

const std::vector<std::string>& known_efforts(const std::string& agent) {
  static std::map<std::string, std::vector<std::string>> lists;
  return lists[agent];
}

void set_known_efforts(const std::string& agent, std::vector<std::string> v) {
  const_cast<std::vector<std::string>&>(known_efforts(agent)) = std::move(v);
}

void set_known_models(const std::string& agent, std::vector<ModelOption> v) {
  cache()[agent] = std::move(v);
}

}  // namespace mico
