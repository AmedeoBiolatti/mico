#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "adapters/adapter.h"
#include "base/path.h"
#include "model/session_ref.h"

namespace mico {

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

// A setting: one short line in a file of its own in config_dir(). What it
// says, trimmed, or "" when it is not set. Read from the file each time, so a
// change made by another mico (or by hand) counts at once.
std::string read_setting(std::string_view name);
// Writes it whole or not at all, for the user alone.
bool write_setting(std::string_view name, std::string_view value);
// An on/off setting: `fallback` unless its file says "on" or "off".
bool setting_on(std::string_view name, bool fallback);
void set_setting_on(std::string_view name, bool on);

// Discovers what the agents have already written. Read-only by construction:
// mico never writes into ~/.claude or ~/.codex.
class Store {
 public:
  void scan();
  // Moves whenever anything a listing shows may have changed: a scan, a folder,
  // a name, an archive mark, a sub-project. A front end that keeps a copy
  // compares it to know when to read again.
  uint64_t version() const { return version_; }
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
  void add(SessionRef s);
  Project* project_for(const std::string& cwd);

  std::vector<std::string> folders_;
  std::vector<Project> projects_;
  std::map<std::string, std::string> names_;  // mark_key -> display name
  std::set<std::string> archived_;            // mark_keys
  bool marks_loaded_ = false;
  uint64_t version_ = 1;
  // project path -> its sub-projects; and project path + '\n' + mark_key ->
  // the sub-project a chat was put in ("-": explicitly none).
  std::map<std::string, std::vector<SubProject>> subs_;
  std::map<std::string, std::string> sub_of_chat_;
  std::map<std::string, std::string> sub_of_path_;  // transcript -> sub, from the scan
  bool subs_loaded_ = false;
};

}  // namespace mico
