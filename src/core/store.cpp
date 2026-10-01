#include "core/store.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include <functional>
#include <map>

namespace mico {

std::string config_dir() {
  if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/mico";
  return fs::home() + "/.config/mico";
}

namespace {

std::string folders_file() { return config_dir() + "/folders"; }

// Claude Code's per-folder trust lives in ~/.claude.json under
// projects["<abs path>"].hasTrustDialogAccepted. Writing that entry ahead of
// time is exactly what clicking "Yes, I trust this folder" does — so a folder
// the user added to mico never shows the dialog at all. A targeted text merge,
// not a full re-serialise: only insert an entry that is not already there.
void claude_pretrust(const std::string& abspath) {
  const std::string file = fs::home() + "/.claude.json";
  std::string buf;
  std::string_view blob = fs::read_prefix(file, 8u << 20, buf);
  if (blob.empty()) return;
  std::string doc(blob);

  const size_t pk = doc.find("\"projects\"");
  if (pk == std::string::npos) return;
  const size_t brace = doc.find('{', pk);
  if (brace == std::string::npos) return;

  // JSON-escape the path (backslash and quote only; Linux paths rarely need it).
  std::string key = "\"";
  for (char c : abspath) {
    if (c == '\\' || c == '"') key.push_back('\\');
    key.push_back(c);
  }
  key.push_back('"');

  // Already present anywhere in the projects object? Leave it alone.
  if (doc.find(key, brace) != std::string::npos) return;

  const std::string entry = "\n    " + key + ": {\"hasTrustDialogAccepted\": true},";
  doc.insert(brace + 1, entry);

  const std::string tmp = file + ".mico.tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  bool ok = write(fd, doc.data(), doc.size()) == ssize_t(doc.size());
  ::close(fd);
  if (ok) rename(tmp.c_str(), file.c_str());
  else unlink(tmp.c_str());
}

std::string basename_of(const std::string& p) {
  if (p.empty() || p == "/") return p;
  size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

// Wrapper text the agents inject around real user input; a session titled
// "<command-name>/model" helps nobody.
bool is_noise_prompt(std::string_view t) {
  static constexpr std::string_view kMarkers[] = {
      "<local-command", "<command-name>", "<system-reminder>", "Caveat:",
      "# AGENTS.md instructions", "<skills_instructions>", "<user_instructions>",
      "<environment_context>", "<recommended_plugins>", "<system-reminder>"};
  for (auto m : kMarkers)
    if (t.size() >= m.size() && t.compare(0, m.size(), m) == 0) return true;
  return false;
}

// Codex keeps its own names for sessions in one small index, appended to as a
// name is refined. Later entries win. Reading this once beats scraping 200
// rollouts, and it is the name codex itself shows.
std::map<std::string, std::string> load_codex_names() {
  std::map<std::string, std::string> names;
  std::string buf;
  std::string_view blob = fs::read_prefix(fs::home() + "/.codex/session_index.jsonl", 4u << 20, buf);
  fs::for_each_line(blob, [&](std::string_view line) {
    std::string_view id, name;
    js::scan_object(line, [&](std::string_view k, const js::Value& v) {
      if (k == "id") id = v.body();
      else if (k == "thread_name") name = v.body();
      return true;
    });
    if (id.empty() || name.empty()) return true;
    std::string decoded;
    js::unescape_append(name, decoded);
    if (!decoded.empty()) names[std::string(id)] = std::move(decoded);
    return true;
  });
  return names;
}

}  // namespace

const Adapter* Store::adapter_for(const SessionRef& s) {
  if (s.agent == "claude") return &claude_adapter();
  if (s.agent == "codex") return &codex_adapter();
  if (s.agent == "pi") return &pi_adapter();
  if (s.agent == "omp") return &omp_adapter();
  return nullptr;  // unknown agent: no chat view, raw pane only (M2)
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
  if (path.empty()) return false;
  std::string p = path;
  if (p[0] == '~' && (p.size() == 1 || p[1] == '/')) p = fs::home() + p.substr(1);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  if (!fs::exists(p)) return false;
  for (const auto& f : folders_)
    if (f == p) return false;
  folders_.push_back(p);
  if (persist) save_folders();
  claude_pretrust(p);
  scan();
  MLOG("folder added: %s", p.c_str());
  return true;
}

bool Store::remove_folder(const std::string& path) {
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

  scan_claude();
  scan_codex();
  scan_pi_family(fs::home() + "/.pi/agent/sessions", "pi");
  scan_pi_family(fs::home() + "/.omp/agent/sessions", "omp");

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

void Store::scan_claude() {
  std::string buf;
  const std::string root = fs::home() + "/.claude/projects";
  fs::list_dir(root, true, [&](const std::string& slug) {
    const std::string dir = root + "/" + slug;
    fs::list_dir(dir, false, [&](const std::string& fname) {
      if (!fs::has_suffix(fname, ".jsonl")) return;

      SessionRef s;
      s.agent = "claude";
      s.path = dir + "/" + fname;
      s.id = fname.substr(0, fname.size() - 6);
      s.mtime = fs::mtime(s.path, &s.bytes);

      // A transcript's ai-title lands early — measured across this machine's
      // sessions, by 54 KB — so a prefix answers almost every file. Only one
      // that tells us nothing is worth reading in full.
      std::string first_user;
      auto scan = [&](std::string_view blob) {
      fs::for_each_line(blob, [&](std::string_view line) {
        // Listing needs three things: cwd, the ai-title, and the first real
        // user turn. Anything else is skipped before it is scanned.
        const bool maybe_title = line.find("ai-title") != std::string_view::npos;
        const bool maybe_cwd = s.cwd.empty() && line.find("\"cwd\":") != std::string_view::npos;
        const bool maybe_user =
            first_user.empty() && line.find("\"type\":\"user\"") != std::string_view::npos;
        if (!maybe_title && !maybe_cwd && !maybe_user) return true;

        std::string_view type, title;
        js::Value message{};
        bool meta = false;
        js::scan_object(line, [&](std::string_view k, const js::Value& v) {
          if (k == "cwd" && v.is_string() && s.cwd.empty()) s.cwd = std::string(v.body());
          else if (k == "type") type = v.body();
          else if (k == "aiTitle") title = v.body();
          else if (k == "isMeta") meta = v.is_true();
          else if (k == "message") message = v;
          return true;
        });

        if (type == "ai-title" && !title.empty()) {
          s.title.clear();
          js::unescape_append(title, s.title);
          // The best title available: stop looking, once the folder is known
          // too. Without it the chat would be filed under no tracked folder.
          return s.cwd.empty();
        }
        if (first_user.empty() && type == "user" && !meta && message.is_object()) {
          js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
            if (k != "content" || !v.is_string()) return true;
            std::string text;
            js::unescape_append(v.body(), text);
            if (!text.empty() && !is_noise_prompt(text)) first_user = std::move(text);
            return false;
          });
        }
        return true;
      });
      };

      constexpr size_t kPrefix = 128u << 10;
      scan(fs::read_prefix(s.path, kPrefix, buf));
      if (s.title.empty() && first_user.empty() && s.bytes > kPrefix)
        scan(fs::read_prefix(s.path, 4u << 20, buf));

      if (s.title.empty()) s.title = first_user;
      if (s.cwd.empty()) {
        // Fall back to decoding the directory slug: "-home-me-proj" -> "/home/me/proj".
        s.cwd = slug;
        for (char& c : s.cwd)
          if (c == '-') c = '/';
      }
      add(std::move(s));
    });
  });
}

void Store::scan_codex() {
  std::string buf;
  const std::map<std::string, std::string> names = load_codex_names();
  const std::string root = fs::home() + "/.codex/sessions";
  // Layout is sessions/YYYY/MM/DD/rollout-*.jsonl.
  fs::list_dir(root, true, [&](const std::string& y) {
    fs::list_dir(root + "/" + y, true, [&](const std::string& m) {
      const std::string md = root + "/" + y + "/" + m;
      fs::list_dir(md, true, [&](const std::string& dd) {
        const std::string dir = md + "/" + dd;
        fs::list_dir(dir, false, [&](const std::string& fname) {
          if (!fs::has_suffix(fname, ".jsonl")) return;

          SessionRef s;
          s.agent = "codex";
          s.path = dir + "/" + fname;
          s.mtime = fs::mtime(s.path, &s.bytes);

          // Only the head: session_meta is line 0, and measured across this
          // machine's rollouts the first real user turn lands by 14 KB
          // (median 8 KB). 64 KB is a wide margin and avoids reading ~100 MB
          // across a few hundred sessions just to build a list.
          // session_meta is line 0, so a small read identifies the rollout.
          // Codex's own name usually covers the title, and only a session the
          // index has never heard of needs the deeper scan for a first turn.
          auto scan = [&](std::string_view blob) {
          fs::for_each_line(blob, [&](std::string_view line) {
            const bool maybe_meta = line.find("session_meta") != std::string_view::npos;
            const bool maybe_user = line.find("\"role\":\"user\"") != std::string_view::npos;
            if (!maybe_meta && !maybe_user) return true;

            std::string_view type;
            js::Value payload{};
            js::scan_object(line, [&](std::string_view k, const js::Value& v) {
              if (k == "type") type = v.body();
              else if (k == "payload") { payload = v; return false; }
              return true;
            });
            if (!payload.is_object()) return true;

            if (type == "session_meta") {
              js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
                if (k == "id" || k == "session_id") s.id = std::string(v.body());
                else if (k == "cwd") {
                  s.cwd.clear();
                  js::unescape_append(v.body(), s.cwd);
                }
                return true;
              });
              return true;
            }
            if (type != "response_item") return true;

            std::string_view ptype, role;
            js::Value content{};
            js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
              if (k == "type") ptype = v.body();
              else if (k == "role") role = v.body();
              else if (k == "content") content = v;
              return true;
            });
            if (ptype != "message" || role != "user" || !content.is_array()) return true;

            bool done = false;
            js::scan_array(content.raw, [&](const js::Value& b) {
              if (!b.is_object()) return true;
              js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
                if (k != "text" || !v.is_string()) return true;
                std::string text;
                js::unescape_append(v.body(), text);
                if (!text.empty() && !is_noise_prompt(text)) { s.title = std::move(text); done = true; }
                return false;
              });
              return !done;
            });
            return !done;
          });
          };

          // session_meta carries the whole system preamble: measured across
          // this machine's rollouts it reaches 21 KB, and two thirds of them
          // exceed 8 KB. Read past it, or the very line that identifies the
          // session is the one that gets cut off.
          constexpr size_t kMetaPrefix = 32u << 10;
          // Parse the complete metadata even when its preamble exceeds the
          // listing prefix. scan() expects newline-terminated records.
          const std::string meta(fs::read_first_line(s.path, buf));
          scan(meta + "\n");
          scan(fs::read_prefix(s.path, kMetaPrefix, buf));

          // Prefer codex's own name for the thread; the first user turn is
          // only a fallback, and a bare uuid is the last resort. The lookup has
          // to come after the id is known, so a deeper read cannot skip it.
          if (auto it = names.find(s.id); it != names.end() && !it->second.empty())
            s.title = it->second;
          else if (s.title.empty() && s.bytes > kMetaPrefix)
            scan(fs::read_prefix(s.path, 96u << 10, buf));
          if (s.id.empty()) return;  // a filename is not a resumable session id
          if (s.cwd.empty()) s.cwd = "(unknown)";
          add(std::move(s));
        });
      });
    });
  });
}

void Store::scan_pi_family(const std::string& root, const char* agent) {
  std::string buf;
  fs::list_dir(root, true, [&](const std::string& slug) {
    const std::string dir = root + "/" + slug;
    fs::list_dir(dir, false, [&](const std::string& fname) {
      // omp keeps a same-named scratch directory (bash logs) beside each
      // transcript; it carries no extension, so the suffix check alone
      // already steps over it.
      if (!fs::has_suffix(fname, ".jsonl")) return;

      SessionRef s;
      s.agent = agent;
      s.path = dir + "/" + fname;
      s.mtime = fs::mtime(s.path, &s.bytes);

      std::string first_user;
      auto scan = [&](std::string_view blob) {
      fs::for_each_line(blob, [&](std::string_view line) {
        std::string_view type;
        js::Value message{};
        std::string_view title;
        js::scan_object(line, [&](std::string_view k, const js::Value& v) {
          if (k == "type") {
            type = v.body();
            // omp's "custom"/"custom_message" telemetry outnumbers everything
            // else in a session by an order of magnitude; reject it after one
            // member rather than walk its (often large) payload.
            if (type != "session" && type != "title" && type != "title_change" &&
                type != "message")
              return false;
            return true;
          }
          if (k == "cwd" && s.cwd.empty()) s.cwd = std::string(v.body());
          else if (k == "id" && s.id.empty() && type == "session") s.id = std::string(v.body());
          else if (k == "title") title = v.body();
          else if (k == "message") message = v;
          return true;
        });

        // omp names sessions itself, and a name it chose beats anything
        // mined from the transcript.
        if ((type == "title" || type == "title_change") && !title.empty()) {
          s.title.clear();
          js::unescape_append(title, s.title);
          return s.cwd.empty();  // keep reading only if cwd is still unknown
        }

        if (first_user.empty() && type == "message" && message.is_object()) {
          std::string_view role;
          js::Value content{};
          js::scan_object(message.raw, [&](std::string_view mk, const js::Value& mv) {
            if (mk == "role") { role = mv.body(); return true; }
            if (mk == "content") { content = mv; return false; }
            return true;
          });
          if (role == "user" && content.is_array()) {
            js::scan_array(content.raw, [&](const js::Value& b) {
              if (!b.is_object()) return true;
              bool done = false;
              js::scan_object(b.raw, [&](std::string_view bk, const js::Value& bv) {
                if (bk != "text" || !bv.is_string()) return true;
                std::string text;
                js::unescape_append(bv.body(), text);
                if (!text.empty() && !is_noise_prompt(text)) { first_user = std::move(text); done = true; }
                return false;
              });
              return !done;
            });
          }
        }
        return s.cwd.empty() || (s.title.empty() && first_user.empty());
      });
      };

      constexpr size_t kPrefix = 128u << 10;
      scan(fs::read_prefix(s.path, kPrefix, buf));
      if (s.title.empty() && first_user.empty() && s.bytes > kPrefix)
        scan(fs::read_prefix(s.path, 4u << 20, buf));

      if (s.title.empty()) s.title = first_user;
      if (s.id.empty()) {
        // Fall back to the uuid half of "<timestamp>_<uuid>.jsonl".
        const std::string stem = fname.substr(0, fname.size() - 6);
        const size_t us = stem.find('_');
        s.id = us == std::string::npos ? stem : stem.substr(us + 1);
      }
      if (s.cwd.empty()) s.cwd = "(unknown)";
      add(std::move(s));
    });
  });
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
  if (id.empty() || agent.empty()) return;
  const std::string key = mark_key(agent, id);
  if (name.empty()) names_.erase(key);
  else names_[key] = name;
  save_marks();
}

void Store::set_archived(const std::string& agent, const std::string& id, bool on) {
  if (id.empty() || agent.empty()) return;
  const std::string key = mark_key(agent, id);
  if (on) archived_.insert(key);
  else archived_.erase(key);
  save_marks();
}

}  // namespace mico
