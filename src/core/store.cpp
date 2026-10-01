#include "core/store.h"

#include "adapters/adapters.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

#include "base/fs.h"
#include "base/json.h"
#include "base/log.h"
#include <functional>
#include <map>

namespace mico {

std::string config_dir() {
  if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/mico";
  return fs::home() + "/.config/mico";
}

namespace {

std::string folders_file() { return config_dir() + "/folders"; }

std::string basename_of(const std::string& p) {
  if (p.empty() || p == "/") return p;
  size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

const Adapter* Store::adapter_for(const SessionRef& s) {
  return mico::adapter_for(s.agent);  // null: no chat view, raw pane only
}

Project* Store::project_for(const std::string& cwd) {
  for (auto& p : projects_)
    if (p.path == cwd) return &p;
  // A chat run in a sub-project's own folder, or under it, is the project's.
  for (auto& p : projects_)
    for (const auto& sp : p.subs)
      if (sp.path != p.path && (cwd == sp.path || cwd.starts_with(sp.path + "/"))) return &p;
  return nullptr;
}

// A session attaches only to a folder that is already tracked; anything whose
// cwd is not in the list is ignored, so the project view stays curated.
void Store::add(SessionRef s) {
  Project* p = project_for(s.cwd);
  if (!p) return;
  p->mtime = std::max(p->mtime, s.mtime);
  p->sessions.push_back(std::move(s));
}

void Store::load_folders() {
  folders_.clear();
  std::string buf;
  std::string_view blob = fs::read_prefix(folders_file(), 64u << 10, buf);
  fs::for_each_line(blob, [&](std::string_view line) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.remove_suffix(1);
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    if (!line.empty() && line[0] != '#') folders_.emplace_back(line);
    return true;
  });
  if (folders_.empty()) {
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) {
      folders_.emplace_back(cwd);
      save_folders();
    }
  }
}

void Store::save_folders() const {
  mkdir(config_dir().c_str(), 0700);
  const std::string tmp = folders_file() + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  for (const auto& f : folders_) {
    const std::string line = f + "\n";
    if (write(fd, line.data(), line.size()) < 0) break;
  }
  ::close(fd);
  rename(tmp.c_str(), folders_file().c_str());
}

bool Store::add_folder(const std::string& path, bool persist) {
  ++version_;
  if (path.empty()) return false;
  std::string p = path;
  if (p[0] == '~' && (p.size() == 1 || p[1] == '/')) p = fs::home() + p.substr(1);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  if (!fs::exists(p)) return false;
  for (const auto& f : folders_)
    if (f == p) return false;
  folders_.push_back(p);
  if (persist) save_folders();
  for (const Adapter* a : all_adapters()) a->trust_folder(p);
  scan();
  MLOG("folder added: %s", p.c_str());
  return true;
}

bool Store::remove_folder(const std::string& path) {
  ++version_;
  for (size_t i = 0; i < folders_.size(); i++)
    if (folders_[i] == path) {
      folders_.erase(folders_.begin() + long(i));
      save_folders();
      // Its sub-projects go with it.
      if (subs_.erase(path)) {
        std::erase_if(sub_of_chat_, [&](const auto& kv) { return kv.first.starts_with(path + "\n"); });
        save_subs();
      }
      scan();
      return true;
    }
  return false;
}

void Store::scan() {
  ++version_;
  if (folders_.empty()) load_folders();
  if (!marks_loaded_) load_marks();
  if (!subs_loaded_) load_subs();

  projects_.clear();
  for (const auto& f : folders_) {
    Project pr;
    pr.path = f;
    pr.name = basename_of(f);
    if (auto it = subs_.find(f); it != subs_.end()) pr.subs = it->second;
    projects_.push_back(std::move(pr));
  }

  for (const Adapter* a : all_adapters())
    a->list_sessions([&](SessionRef&& ref) { add(std::move(ref)); });

  sub_of_path_.clear();
  for (auto& p : projects_) {
    std::sort(p.sessions.begin(), p.sessions.end(),
              [](const SessionRef& a, const SessionRef& b) { return a.mtime > b.mtime; });
    for (auto& s : p.sessions) {
      s.sub = sub_for(p, s.agent, s.id, s.cwd);
      if (!s.sub.empty()) sub_of_path_[s.path] = s.sub;
    }
  }
  std::sort(projects_.begin(), projects_.end(),
            [](const Project& a, const Project& b) { return a.mtime > b.mtime; });
}

size_t Store::session_count() const {
  size_t n = 0;
  for (const auto& p : projects_) n += p.sessions.size();
  return n;
}

// ------------------------------------------------------------------- marks

void Store::load_marks() {
  marks_loaded_ = true;
  names_.clear();
  archived_.clear();

  std::string buf;
  std::string_view blob = fs::read_prefix(config_dir() + "/names", 256u << 10, buf);
  fs::for_each_line(blob, [&](std::string_view line) {
    // "<agent>\t<id>\t<name>": the name is everything after the last tab, so
    // it may contain anything but a tab.
    const size_t tab = line.rfind('\t');
    if (tab == std::string_view::npos || tab == 0) return true;
    std::string name(line.substr(tab + 1));
    if (!name.empty()) names_.emplace(std::string(line.substr(0, tab)), std::move(name));
    return true;
  });

  std::string abuf;
  std::string_view ablob = fs::read_prefix(config_dir() + "/archived", 256u << 10, abuf);
  fs::for_each_line(ablob, [&](std::string_view line) {
    if (!line.empty()) archived_.emplace(line);
    return true;
  });
}

void Store::save_marks() const {
  mkdir(config_dir().c_str(), 0700);
  auto write_lines = [](const std::string& path, const std::string& body) {
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    if (write(fd, body.data(), body.size()) < 0) {}
    ::close(fd);
    rename(tmp.c_str(), path.c_str());
  };
  std::string names;
  for (const auto& [key, name] : names_) names += key + "\t" + name + "\n";
  write_lines(config_dir() + "/names", names);
  std::string arch;
  for (const auto& key : archived_) arch += key + "\n";
  write_lines(config_dir() + "/archived", arch);
}

// ------------------------------------------------------------ sub-projects

// "sub\t<project>\t<name>\t<folder>" and "chat\t<project>\t<agent>\t<id>\t<sub>",
// one per line.
void Store::load_subs() {
  subs_loaded_ = true;
  subs_.clear();
  sub_of_chat_.clear();
  std::string buf;
  std::string_view blob = fs::read_prefix(config_dir() + "/subprojects", 256u << 10, buf);
  fs::for_each_line(blob, [&](std::string_view line) {
    std::vector<std::string> f;
    for (size_t a = 0; a <= line.size();) {
      size_t b = line.find('\t', a);
      if (b == std::string_view::npos) b = line.size();
      f.emplace_back(line.substr(a, b - a));
      a = b + 1;
    }
    if (f.size() == 4 && f[0] == "sub" && !f[2].empty()) subs_[f[1]].push_back({f[2], f[3]});
    else if (f.size() == 5 && f[0] == "chat") sub_of_chat_[f[1] + "\n" + mark_key(f[2], f[3])] = f[4];
    return true;
  });
}

void Store::save_subs() const {
  mkdir(config_dir().c_str(), 0700);
  std::string body;
  for (const auto& [project, list] : subs_)
    for (const auto& sp : list) body += "sub\t" + project + "\t" + sp.name + "\t" + sp.path + "\n";
  for (const auto& [key, sub] : sub_of_chat_) {
    const size_t nl = key.find('\n');
    body += "chat\t" + key.substr(0, nl) + "\t" + key.substr(nl + 1) + "\t" + sub + "\n";
  }
  const std::string path = config_dir() + "/subprojects", tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  if (write(fd, body.data(), body.size()) < 0) {}
  ::close(fd);
  rename(tmp.c_str(), path.c_str());
}

bool Store::add_subproject(const std::string& project, const std::string& name, const std::string& path) {
  ++version_;
  if (!subs_loaded_) load_subs();
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  if (name.empty() || name.find('\t') != std::string::npos || name.find('\n') != std::string::npos)
    return false;
  if (p != project && !p.starts_with(project + "/")) return false;  // inside the project only
  if (!fs::exists(p)) return false;
  auto& list = subs_[project];
  for (const auto& sp : list)
    if (sp.name == name) return false;
  list.push_back({name, p});
  save_subs();
  scan();
  return true;
}

bool Store::remove_subproject(const std::string& project, const std::string& name) {
  ++version_;
  auto it = subs_.find(project);
  if (it == subs_.end()) return false;
  const size_t before = it->second.size();
  std::erase_if(it->second, [&](const SubProject& sp) { return sp.name == name; });
  if (it->second.size() == before) return false;
  if (it->second.empty()) subs_.erase(it);
  // Its chats go back to the project.
  std::erase_if(sub_of_chat_, [&](const auto& kv) {
    return kv.second == name && kv.first.starts_with(project + "\n");
  });
  save_subs();
  scan();
  return true;
}

bool Store::rename_subproject(const std::string& project, const std::string& from, const std::string& to) {
  ++version_;
  auto it = subs_.find(project);
  if (it == subs_.end() || to.empty() || to.find('\t') != std::string::npos) return false;
  SubProject* found = nullptr;
  for (auto& sp : it->second) {
    if (sp.name == to) return false;
    if (sp.name == from) found = &sp;
  }
  if (!found) return false;
  found->name = to;
  for (auto& [key, sub] : sub_of_chat_)
    if (sub == from && key.starts_with(project + "\n")) sub = to;
  save_subs();
  scan();
  return true;
}

void Store::assign_sub(const std::string& project, const std::string& agent, const std::string& id,
                       const std::string& sub) {
  ++version_;
  if (id.empty()) return;
  if (!subs_loaded_) load_subs();
  const std::string key = project + "\n" + mark_key(agent, id);
  // Taking a chat out of a sub-project whose folder would claim it anyway
  // needs saying so: "-" is an explicit none.
  sub_of_chat_[key] = sub.empty() ? "-" : sub;
  save_subs();
  for (auto& p : projects_)
    if (p.path == project)
      for (auto& s : p.sessions)
        if (s.agent == agent && s.id == id) {
          s.sub = sub;
          if (sub.empty()) sub_of_path_.erase(s.path);
          else sub_of_path_[s.path] = sub;
        }
}

std::string Store::sub_for(const Project& project, const std::string& agent, const std::string& id,
                           const std::string& cwd) const {
  if (project.subs.empty()) return {};
  if (!id.empty())
    if (auto it = sub_of_chat_.find(project.path + "\n" + mark_key(agent, id)); it != sub_of_chat_.end()) {
      if (it->second == "-") return {};
      for (const auto& sp : project.subs)
        if (sp.name == it->second) return sp.name;
    }
  // By folder: the sub-project with the deepest folder holding the chat's.
  const SubProject* best = nullptr;
  for (const auto& sp : project.subs) {
    if (sp.path == project.path) continue;
    if (cwd != sp.path && !cwd.starts_with(sp.path + "/")) continue;
    if (!best || sp.path.size() > best->path.size()) best = &sp;
  }
  return best ? best->name : std::string();
}

const std::string& Store::sub_of_path(const std::string& path) const {
  static const std::string none;
  auto it = sub_of_path_.find(path);
  return it == sub_of_path_.end() ? none : it->second;
}

const std::string* Store::custom_name(const std::string& agent, const std::string& id) const {
  if (id.empty()) return nullptr;
  auto it = names_.find(mark_key(agent, id));
  return it == names_.end() || it->second.empty() ? nullptr : &it->second;
}

bool Store::archived(const std::string& agent, const std::string& id) const {
  return !id.empty() && archived_.count(mark_key(agent, id)) != 0;
}

void Store::set_custom_name(const std::string& agent, const std::string& id,
                            const std::string& name) {
  ++version_;
  if (id.empty() || agent.empty()) return;
  const std::string key = mark_key(agent, id);
  if (name.empty()) names_.erase(key);
  else names_[key] = name;
  save_marks();
}

void Store::set_archived(const std::string& agent, const std::string& id, bool on) {
  ++version_;
  if (id.empty() || agent.empty()) return;
  const std::string key = mark_key(agent, id);
  if (on) archived_.insert(key);
  else archived_.erase(key);
  save_marks();
}

}  // namespace mico
