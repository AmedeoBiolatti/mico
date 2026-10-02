#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/activity.h"
#include "core/away.h"
#include "core/commands.h"
#include "core/files.h"
#include "core/git.h"
#include "core/models.h"
#include "core/search.h"
#include "core/session.h"
#include "core/store.h"
#include "core/usage.h"

namespace mico {

// mico's state apart from any way of showing it: the tracked folders and their
// stored chats, the agents running now, and the indexes over their
// transcripts. A front end reads it and asks it to act, and turns what it
// reports into focus, selection and status of its own. It draws nothing and
// knows nothing of panes: one workspace can serve every client attached to a
// daemon.
class Workspace {
 public:
  // How long an index pass reads between frames: short enough that a key is
  // answered at once, long enough that a gigabyte of transcripts takes seconds.
  static constexpr int kIndexSliceMs = 25;
  // How long a new session waits for a front end to size it, and the size it
  // gets when none does.
  static constexpr int kLaunchWaitMs = 1500;
  static constexpr int kDefaultCols = 120, kDefaultRows = 40;

  Workspace();  // scans the store
  ~Workspace();
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

  Store& store() { return store_; }
  const Store& store() const { return store_; }
  // Kept here, not in the views that show them: a view is rebuilt whenever
  // its tab changes, and a per-file cache has to survive that.
  UsageIndex& usage() { return usage_; }
  ChatSearch& search() { return search_; }
  ActivityIndex& activity() { return activity_; }
  // What "/" offers for an agent in a folder, and what "@" can name there,
  // asked once per folder for all its chats.
  CommandCatalog& commands() { return commands_; }
  FileIndex& files() { return files_; }
  // What git says about the folders and the commits agents made in them.
  GitIndex& git() { return git_; }

  // --- Running agents ------------------------------------------------------

  const std::vector<std::unique_ptr<LiveSession>>& live() const { return live_; }
  std::vector<LiveSession*> live_sessions() const;
  bool has_live() const { return !live_.empty(); }
  // Moves when an agent starts, restarts or goes: the set of running sessions,
  // not what each is doing (LiveSession::generation()).
  uint64_t sessions_version() const { return sessions_version_; }

  // A directory that actually exists to run in. A tracked folder can be gone —
  // a disconnected network drive, a deleted checkout — and spawning there just
  // produces a dead pane. Returns `want` when it is usable, else `prefer` (the
  // folder the user has selected), another tracked folder, or $HOME.
  std::string usable_cwd(const std::string& want, const std::string& prefer = {}) const;

  // What starting or continuing an agent came to.
  struct Started {
    LiveSession* session = nullptr;  // null when nothing could start
    enum class How {
      Failed,     // see `error`
      Running,    // it was already running: `session` is that one
      Restarted,  // its exited pane runs again
      Started,    // a new session
    } how = How::Failed;
    bool forked = false;
    bool moved = false;  // its folder was unusable: it runs in session->cwd()
    std::string note;    // what the agent's adapter said about it
    std::string error;
  };
  // Spawns `agent` in `cwd` (or, when that is unusable, see usable_cwd()).
  Started start_agent(const std::string& agent, const std::string& cwd, const std::string& prefer = {});
  // Continues stored session `session_id`: resuming it, or with `fork`,
  // branching it into a new one. Both shell out to the agent's own resume and
  // fork rather than touching its transcript files. A resume of one that is
  // running already reports it, rather than starting a second.
  Started continue_session(const std::string& agent, const std::string& session_id,
                           const std::string& cwd, bool fork, const std::string& prefer = {});
  // Runs an arbitrary command. Used for agents mico has no adapter for, for an
  // editor, and for testing the pty plane without starting a real agent.
  LiveSession* start_command(std::vector<std::string> argv, const std::string& cwd);

  // Stops `s` and lets it go, at the next reap(): the caller is usually
  // something that refers to it.
  void close(LiveSession* s) { to_close_.push_back(s); }
  bool closing() const { return !to_close_.empty(); }
  // Lets go of what close() queued, calling `gone` with each just before it is
  // destroyed, so a front end can drop what refers to it. True if any went.
  bool reap(const std::function<void(LiveSession*)>& gone);
  // Stops every agent: mico is going away and taking them with it.
  void terminate_all();

  // Files the new chat `s` under sub-project `sub` of folder `project` once
  // its agent has said its id.
  void file_under(LiveSession* s, const std::string& project, const std::string& sub);
  // The sub-project a running chat is in: the one it is being filed under
  // while its id is unknown, else what the store says.
  std::string sub_of(const LiveSession& s) const;
  // What a running chat is called: the user's name for it, else its stored
  // title, else "New <agent> chat".
  std::string title_of(const LiveSession& s) const;

  // --- Across a restart -----------------------------------------------------

  // From now on, keeps running_path() listing the agents running, so that if
  // this daemon goes without being told to stop them — a reboot, a crash,
  // `mico kill` — the next one can resume them. Only a daemon does this: a
  // --local mico's agents are meant to go with it.
  void remember_running() { remember_running_ = true; }
  // Resumes the agents the list names, each in place, as the chat list's
  // "resume" would. Returns how many started.
  int restore_running();
  // The agents were stopped on purpose: there is nothing to resume.
  void forget_running();

  // --- Advancing -------------------------------------------------------------

  // What service() moved, for a front end to decide what to redraw.
  enum Changed : unsigned {
    kSessions = 1u << 0,  // an agent wrote, changed state, started or went
    kCatalog = 1u << 1,   // commands, models or sub-project filing
    kSearch = 1u << 2,
    kActivity = 1u << 3,  // the index behind Tools and Diff
    kUsage = 1u << 4,
    kGit = 1u << 5,       // a status, a commit or a blame came back from git
  };
  // Advances agents, probes and index passes by a slice each. The usage index
  // reads only while `usage_wanted`: it is costly and only one view shows it.
  unsigned service(bool usage_wanted);
  // How long the event loop may sleep before service() has timed work to do.
  // Agent output wakes the loop on its own; this covers what the clock moves.
  int idle_timeout_ms(bool usage_wanted) const;
  // Every fd whose readiness means service() has work: the agents' ptys.
  void collect_fds(std::vector<int>& out) const;
  // True while some agent is working.
  bool any_busy() const;
  // For the status bar, once: the machine is low on memory, and which agent's
  // work holds the most. Empty when there is nothing new to say.
  std::string take_memory_warning() { return std::exchange(memory_warning_, {}); }

 private:
  Store store_;
  UsageIndex usage_;
  ChatSearch search_;
  ActivityIndex activity_;
  bool activity_warmed_ = false;
  CommandCatalog commands_;
  FileIndex files_;
  GitIndex git_;
  // Whether each agent was working when last looked at: when one stops, its
  // folder's git status is read again, since that is when it changes.
  std::map<LiveSession*, bool> was_busy_;
  std::vector<std::unique_ptr<LiveSession>> live_;
  std::vector<LiveSession*> to_close_;
  uint64_t sessions_version_ = 1;
  // Asks an agent what models it offers, once, in a process of its own.
  // Started from the first live session so it inherits a directory the user
  // has already trusted — an untrusted one would only get a dialog.
  ModelProbe model_probe_;
  // Sessions started but not yet launched, with when they were asked for. An
  // agent is launched by the first front end that sizes it, so it starts at
  // the size it is drawn at; one that nothing sizes (started from a client
  // that draws no terminal, or not shown) is launched at a default size.
  std::map<LiveSession*, int64_t> unlaunched_;
  void launched_later(LiveSession* s);
  void launch_unsized();

  // What each agent's process tree holds, looked at every 15 s (every 3 s
  // while memory is short) and written to the log as it crosses 1, 2, 4 GB…,
  // and all of them while the machine runs low: a kill for memory leaves no
  // line of its own, so these are what say whose work it was.
  void watch_memory();
  std::string describe(LiveSession& s) const;
  int64_t mem_checked_ms_ = 0, low_logged_ms_ = 0, warned_ms_ = 0;
  bool mem_tight_ = false;
  std::map<LiveSession*, int> mem_level_;  // the size last logged, as a power of two in GB
  std::string memory_warning_;

  bool remember_running_ = false;
  std::vector<RunningAgent> running_written_;
  // When each exited agent was first seen exited. One stays listed a while:
  // at shutdown the agents are often killed a moment before their daemon,
  // and an agent the user ended is still dropped soon enough.
  std::map<LiveSession*, int64_t> exited_at_;
  static constexpr int kExitGraceMs = 10000;
  void note_running();

  // New chats started from a sub-project, filed under it once their agent
  // has told us their id: project path and sub-project name.
  std::map<LiveSession*, std::pair<std::string, std::string>> pending_subs_;
};

}  // namespace mico
