#include "core/workspace.h"

#include <sys/stat.h>
#include <unistd.h>

#include <time.h>

#include <algorithm>
#include <cstdlib>

#include "adapters/adapters.h"
#include "base/text.h"

namespace mico {
namespace {

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

Workspace::Workspace() { store_.scan(); }

Workspace::~Workspace() = default;

std::vector<LiveSession*> Workspace::live_sessions() const {
  std::vector<LiveSession*> out;
  out.reserve(live_.size());
  for (const auto& s : live_) out.push_back(s.get());
  return out;
}

std::string Workspace::usable_cwd(const std::string& want, const std::string& prefer) const {
  auto usable = [](const std::string& d) {
    struct stat st{};
    return !d.empty() && stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
           access(d.c_str(), X_OK) == 0;
  };
  if (usable(want)) return want;
  if (usable(prefer)) return prefer;
  for (const auto& f : store_.folders())
    if (usable(f)) return f;
  if (const char* h = getenv("HOME"); h && usable(h)) return h;
  char buf[4096];
  return getcwd(buf, sizeof buf) ? std::string(buf) : std::string(".");
}

Workspace::Started Workspace::start_agent(const std::string& agent, const std::string& cwd,
                                          const std::string& prefer) {
  Started r;
  const std::string dir = usable_cwd(cwd, prefer);
  Launch l;
  l.agent = agent;
  l.cwd = dir;
  auto s = std::make_unique<LiveSession>();
  // Size is provisional; the front end sizes the pty when it first shows it.
  if (!s->start(l)) {
    r.error = "failed to start " + agent;
    return r;
  }
  r.session = s.get();
  r.how = Started::How::Started;
  r.moved = dir != cwd;
  launched_later(s.get());
  live_.push_back(std::move(s));
  ++sessions_version_;
  return r;
}

Workspace::Started Workspace::continue_session(const std::string& agent, const std::string& session_id,
                                               const std::string& cwd, bool fork, const std::string& prefer) {
  Started r;
  if (session_id.empty()) return r;
  if (!fork) {
    for (const auto& s : live_) {
      if (s->agent() != agent || s->exited() ||
          (s->session_id() != session_id && (s->forked() || s->origin() != session_id))) continue;
      r.session = s.get();
      r.how = Started::How::Running;
      return r;
    }
  }

  // The recorded folder can be gone (an unmounted drive, a deleted checkout).
  // Resuming in it would only make a dead pane; run somewhere that exists.
  const std::string dir = usable_cwd(cwd, prefer);
  Launch l;
  l.agent = agent;
  l.cwd = dir;
  l.origin = session_id;
  l.forked = fork;

  const Adapter* adapter = adapter_for(agent);
  if (!adapter || !adapter->continue_session(l, session_id, fork, &r.note)) {
    r.error = "cannot continue a session for " + agent;
    return r;
  }
  r.forked = l.forked;  // an agent that cannot fork resumes instead
  r.moved = dir != cwd;

  if (!r.forked) {
    for (const auto& previous : live_) {
      if (previous->agent() != agent || !previous->exited() ||
          previous->session_id() != session_id) continue;
      if (!previous->restart(l)) continue;
      r.session = previous.get();
      r.how = Started::How::Restarted;
      launched_later(previous.get());
      ++sessions_version_;
      return r;
    }
  }
  auto s = std::make_unique<LiveSession>();
  if (!s->start(l)) {
    r.error = "failed to start " + agent;
    return r;
  }
  r.session = s.get();
  r.how = Started::How::Started;
  launched_later(s.get());
  live_.push_back(std::move(s));
  ++sessions_version_;
  return r;
}

LiveSession* Workspace::start_command(std::vector<std::string> argv, const std::string& cwd) {
  if (argv.empty()) return nullptr;
  auto s = std::make_unique<LiveSession>();
  Launch l;
  l.agent = argv[0];
  l.cwd = cwd;
  l.argv = std::move(argv);
  if (!s->start(l)) return nullptr;
  launched_later(s.get());
  live_.push_back(std::move(s));
  ++sessions_version_;
  return live_.back().get();
}

bool Workspace::reap(const std::function<void(LiveSession*)>& gone) {
  const bool any = !to_close_.empty();
  for (LiveSession* s : to_close_) {
    for (size_t i = 0; i < live_.size(); i++) {
      if (live_[i].get() != s) continue;
      gone(s);
      unlaunched_.erase(s);
      live_[i]->pty().terminate();
      live_.erase(live_.begin() + long(i));
      ++sessions_version_;
      break;
    }
  }
  to_close_.clear();
  return any;
}

void Workspace::terminate_all() {
  for (auto& s : live_) s->pty().terminate();
}

void Workspace::file_under(LiveSession* s, const std::string& project, const std::string& sub) {
  pending_subs_[s] = {project, sub};
}

std::string Workspace::sub_of(const LiveSession& s) const {
  if (auto it = pending_subs_.find(const_cast<LiveSession*>(&s)); it != pending_subs_.end())
    return it->second.second;
  for (const auto& p : store_.projects()) {
    bool here = s.cwd() == p.path;
    for (const auto& sp : p.subs)
      here |= sp.path != p.path && (s.cwd() == sp.path || s.cwd().starts_with(sp.path + "/"));
    if (here) return store_.sub_for(p, s.agent(), s.session_id(), s.cwd());
  }
  return {};
}

std::string Workspace::title_of(const LiveSession& session) const {
  if (const auto* name = store_.custom_name(session.agent(), session.session_id())) return *name;
  for (const auto& project : store_.projects())
    for (const auto& s : project.sessions)
      if (s.agent == session.agent() && !s.title.empty() &&
          (s.id == session.session_id() || (session.forked() && s.id == session.origin())))
        return (session.forked() ? "Fork: " : "") + text::oneline(s.title, 100);
  return "New " + session.agent() + " chat";
}

void Workspace::launched_later(LiveSession* s) { unlaunched_[s] = now_ms(); }

void Workspace::launch_unsized() {
  const int64_t now = now_ms();
  for (auto it = unlaunched_.begin(); it != unlaunched_.end();) {
    LiveSession* s = it->first;
    if (s->spawned()) {
      it = unlaunched_.erase(it);
    } else if (now - it->second >= kLaunchWaitMs) {
      s->set_geometry(kDefaultCols, kDefaultRows);
      it = unlaunched_.erase(it);
    } else {
      ++it;
    }
  }
}

unsigned Workspace::service(bool usage_wanted) {
  unsigned changed = 0;
  if (!unlaunched_.empty()) {
    const size_t before = unlaunched_.size();
    launch_unsized();
    if (unlaunched_.size() != before) changed |= kSessions;
  }
  for (auto& s : live_)
    if (s->pump()) changed |= kSessions;

  if (!model_probe_.started()) {
    for (const auto& s : live_)
      if (s->adapter() && !s->adapter()->model_picker_command().empty() && !s->cwd().empty()) {
        model_probe_.start(*s->adapter(), s->cwd());
        break;
      }
  } else if (model_probe_.pump()) {
    changed |= kCatalog;  // the model chip menu has entries now
  }
  // New chats started from a sub-project, once their agent names them.
  for (auto it = pending_subs_.begin(); it != pending_subs_.end();) {
    LiveSession* ls = it->first;
    const bool alive = std::any_of(live_.begin(), live_.end(), [&](const auto& p) { return p.get() == ls; });
    if (!alive) { it = pending_subs_.erase(it); continue; }
    if (ls->session_id().empty()) { ++it; continue; }
    store_.assign_sub(it->second.first, ls->agent(), ls->session_id(), it->second.second);
    it = pending_subs_.erase(it);
    changed |= kCatalog;
  }
  // An agent's "/" menu may be read from the agent; ask ahead of the first "/".
  for (const auto& s : live_) commands_.warm(s->agent(), s->cwd());
  if (commands_.pump()) changed |= kCatalog;
  // A search across chats advances in slices, so input stays responsive.
  if (!search_.complete()) {
    search_.step(12);
    changed |= kSearch;
  }
  // The index behind Tools and Diff: its first pass runs as soon as mico is
  // up, so they open on numbers rather than on "reading"; after that it reads
  // whatever a refresh finds changed. Either way in slices between frames.
  if (!activity_warmed_) {
    activity_warmed_ = true;
    activity_.start(store_.projects(), store_);
  }
  if (!activity_.complete()) {
    // Read on worker threads: this only merges what has finished.
    activity_.step(0);
    changed |= kActivity;
  }
  if (usage_wanted && !usage_.complete()) {
    usage_.step(kIndexSliceMs);
    changed |= kUsage;
  }
  return changed;
}

int Workspace::idle_timeout_ms(bool usage_wanted) const {
  int ms = 1000;
  if (!unlaunched_.empty()) ms = 250;  // to launch what nothing sizes, on time
  if (!search_.complete()) ms = 1;  // a search in progress works between frames
  // An index pass reads on worker threads; the loop merges what they finish.
  if (!activity_.complete() || (usage_wanted && !usage_.complete())) ms = std::min(ms, 10);
  if (model_probe_.running() || commands_.probing()) ms = 30;
  for (const auto& s : live_) {
    if (const int t = s->timer_ms(); t >= 0) ms = std::min(ms, t);
    // The spinner's beat, which also catches a quiet agent turning idle.
    if (s->busy()) ms = std::min(ms, 120);
  }
  return ms;
}

void Workspace::collect_fds(std::vector<int>& out) const {
  for (const auto& s : live_) out.push_back(s->pty().fd());
}

bool Workspace::any_busy() const {
  return std::any_of(live_.begin(), live_.end(), [](const auto& s) { return s->busy(); });
}

}  // namespace mico
