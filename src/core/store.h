#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "model/adapter.h"

namespace mico {

struct SessionRef {
  std::string id;     // agent session uuid
  std::string path;   // transcript file
  std::string title;  // ai-title, else first user turn
  std::string agent;  // "claude" | "codex" | "pi" | "omp"
  std::string cwd;
  int64_t mtime = 0;
  uint64_t bytes = 0;
  std::string sub{};  // the sub-project it belongs to, empty for none
};

// A part of a project: a name, and the folder its chats run in — the
// project's own folder, or one under it. mico's own grouping; the agents
// never see it.
struct SubProject {
  std::string name;
  std::string path;
};

struct Project {
  std::string path;  // working directory the sessions ran in
  std::string name;  // basename of path
  std::vector<SessionRef> sessions;  // newest first
  std::vector<SubProject> subs;      // in the order they were added
  int64_t mtime = 0;                 // newest session
};

// $XDG_CONFIG_HOME/mico (or ~/.config/mico): where mico keeps its own state.
std::string config_dir();

// Discovers what the agents have already written. Read-only by construction:
// mico never writes into ~/.claude or ~/.codex.
class Store {
 public:
  void scan();
  const std::vector<Project>& projects() const { return projects_; }
  size_t session_count() const;

  // The tracked-folder list, herdr-style: mico shows only these, not every
  // directory an agent has ever run in. Persisted to
  // $XDG_CONFIG_HOME/mico/folders (one absolute path per line).
  bool add_folder(const std::string& path, bool persist = true);  // false if dup / missing
  bool remove_folder(const std::string& path);
  const std::vector<std::string>& folders() const { return folders_; }

  // mico's own marks on a chat, keyed by the agent and the agent's session id.
  // They are mico's alone: a rename overrides the title mico shows, and an
  // archive hides the row. Neither writes into the agent's store. Persisted to
  // $XDG_CONFIG_HOME/mico/names and .../archived.
  const std::string* custom_name(const std::string& agent, const std::string& id) const;
  bool archived(const std::string& agent, const std::string& id) const;
  void set_custom_name(const std::string& agent, const std::string& id,
                       const std::string& name);  // empty name clears it
  void set_archived(const std::string& agent, const std::string& id, bool on);

  // Sub-projects. Which chats one holds: those assigned to it — started from
  // it, or moved there — and, for one in a folder of its own, every chat run
  // in that folder or under it. A sub-project in the project's own folder has
  // only its assigned chats: the folder cannot tell it from the project.
  // Persisted to $XDG_CONFIG_HOME/mico/subprojects.
  bool add_subproject(const std::string& project, const std::string& name, const std::string& path);
  bool remove_subproject(const std::string& project, const std::string& name);
  bool rename_subproject(const std::string& project, const std::string& from, const std::string& to);
  // Puts a chat in `sub` of `project`; "" takes it out of any.
  void assign_sub(const std::string& project, const std::string& agent, const std::string& id,
                  const std::string& sub);
  // The sub-project a chat of `project` belongs to, "" for none. For running
  // chats, which the scan has not filed yet.
  std::string sub_for(const Project& project, const std::string& agent, const std::string& id,
                      const std::string& cwd) const;
  // The sub-project of the stored chat whose transcript is `path`.
  const std::string& sub_of_path(const std::string& path) const;

  static const Adapter* adapter_for(const SessionRef& s);

 private:
  void load_folders();
  void save_folders() const;
  void load_marks();
  void save_marks() const;
  void load_subs();
  void save_subs() const;
  static std::string mark_key(const std::string& agent, const std::string& id) {
    return agent + "\t" + id;
  }
  void scan_claude();
  void scan_codex();
  // pi (earendil-works/pi-coding-agent) and Oh My Pi both write the same
  // session-record JSONL under sessions/<cwd-slug>/<timestamp>_<uuid>.jsonl,
  // so one scan covers both; only the root and the agent label differ.
  void scan_pi_family(const std::string& root, const char* agent);
  void add(SessionRef s);
  Project* project_for(const std::string& cwd);

  std::vector<std::string> folders_;
  std::vector<Project> projects_;
  std::map<std::string, std::string> names_;  // mark_key -> display name
  std::set<std::string> archived_;            // mark_keys
  bool marks_loaded_ = false;
  // project path -> its sub-projects; and project path + '\n' + mark_key ->
  // the sub-project a chat was put in ("-": explicitly none).
  std::map<std::string, std::vector<SubProject>> subs_;
  std::map<std::string, std::string> sub_of_chat_;
  std::map<std::string, std::string> sub_of_path_;  // transcript -> sub, from the scan
  bool subs_loaded_ = false;
};

}  // namespace mico
