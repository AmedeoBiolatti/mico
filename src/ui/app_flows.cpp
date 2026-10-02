// Pickers mico answers itself: the chat switcher, starting an agent, and
// choosing a folder. Each is a centered Picker in the App's menu slot, so
// the keys, the mouse and the filtering are the ones every menu has.

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "adapters/adapters.h"
#include "core/store.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/list.h"
#include "views/views.h"

namespace mico {

std::vector<std::string> split_command(const std::string& s);  // app.cpp

namespace {

std::string home_dir() {
  const char* e = getenv("HOME");
  std::string s = e ? e : "";
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

// "/home/me/src" -> "~/src", the way a person types it.
std::string abbreviate(const std::string& path) {
  const std::string h = home_dir();
  if (!h.empty() && path.starts_with(h) && (path.size() == h.size() || path[h.size()] == '/'))
    return "~" + path.substr(h.size());
  return path;
}

// "~/src" -> "/home/me/src"; relative paths are taken from `from`.
std::string expand(const std::string& typed, const std::string& from) {
  if (typed == "~") return home_dir();
  if (typed.starts_with("~/")) return home_dir() + typed.substr(1);
  if (typed.starts_with('/')) return typed;
  return from + "/" + typed;
}

bool is_dir(const std::string& p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_like(std::string_view q) { return q.starts_with('/') || q.starts_with('~') || q.starts_with('.'); }

std::string trim_slashes(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

bool on_path(const std::string& bin) {
  const char* path = getenv("PATH");
  if (!path) return false;
  std::string_view ps(path);
  while (!ps.empty()) {
    const size_t c = ps.find(':');
    const std::string dir(ps.substr(0, c));
    if (!dir.empty() && access((dir + "/" + bin).c_str(), X_OK) == 0) return true;
    if (c == std::string_view::npos) break;
    ps.remove_prefix(c + 1);
  }
  return false;
}

bool is_agent(const std::string& cmd) {
  return adapter_for(cmd) != nullptr;
}

}  // namespace

// ------------------------------------------------------------------ opening

void App::open_flow(std::string flow, std::string title, std::string footer, std::string carry,
                    std::string query) {
  close_menu();
  Picker::Options o;
  o.frame = Picker::Frame::Popup;
  o.filter = true;
  o.show_query = true;
  o.title = std::move(title);
  o.footer_text = std::move(footer);
  // In a folder or command step Tab puts the item in the query to go on
  // from; the switcher has nothing to go on to.
  o.tab_completes = flow != "switch";
  o.back_on_empty = flow == "agent_dir";
  o.query_placeholder = flow == "switch"  ? "a chat, a folder or an agent"
                        : flow == "agent" ? "an agent, or any command"
                        : flow == "agent_dir" ? "a tracked folder, or ~/ or / to browse"
                                               : "a path";
  Menu m;
  m.flow = std::move(flow);
  m.carry = std::move(carry);
  m.centered = true;
  m.picker = Picker(std::move(o));
  menu_ = std::move(m);
  menu_->picker.set_query(query);
  menu_->picker.set_items(flow_items(menu_->flow, query));
}

void App::open_picker(Pane* owner, std::string title, std::vector<PickItem> items, int cursor_item,
                      std::string footer) {
  close_menu();
  Picker::Options o;
  o.frame = Picker::Frame::Popup;
  o.filter = true;
  o.show_query = true;
  o.title = std::move(title);
  o.footer_text = std::move(footer);
  Menu m;
  m.owner = owner;
  m.centered = true;
  m.picker = Picker(std::move(o));
  menu_ = std::move(m);
  menu_->picker.set_items(std::move(items));
  menu_->picker.set_cursor(cursor_item);
}

void App::open_picker_above(Pane* owner, Point anchor, std::string title,
                            std::vector<PickItem> items, int cursor_item) {
  const bool many = items.size() > 8;
  open_picker(owner, std::move(title), std::move(items), cursor_item);
  if (!menu_) return;
  menu_->at = anchor;
  menu_->centered = false;
  menu_->above = true;
  menu_->picker.options().show_query = many;
}

void App::open_switcher() {
  open_flow("switch", "Go to chat",
            "\xE2\x86\x91\xE2\x86\x93 move \xC2\xB7 enter open \xC2\xB7 esc close");
}

void App::open_new_agent(std::string command) {
  if (command.empty()) {
    open_flow("agent", "New agent",
              "enter choose \xC2\xB7 tab edit \xC2\xB7 esc close");
    return;
  }
  std::string title = "Run " + command + " in";  // before `command` is moved
  open_flow("agent_dir", std::move(title),
            "enter run here \xC2\xB7 tab open folder \xC2\xB7 \xE2\x8C\xAB back \xC2\xB7 esc close",
            std::move(command));
}

void App::pick_subproject_folder(const std::string& project) {
  open_flow("subdir", "Sub-project of " + abbreviate(project) + " \xE2\x80\x94 its folder",
            "enter use \xC2\xB7 tab open folder \xC2\xB7 esc close", project, abbreviate(project) + "/");
}

void App::pick_folder() {
  open_flow("folder", "Track a folder",
            "enter add \xC2\xB7 tab open folder \xC2\xB7 esc close", {}, "~/");
}

// -------------------------------------------------------------------- items

std::vector<PickItem> App::flow_items(const std::string& flow, const std::string& query) {
  // A folder listing reads in its own order, alphabetical; a match ranking
  // would put the shortest names first.
  if (menu_ && menu_->flow == flow)
    menu_->picker.options().ranked =
        !(flow == "folder" || flow == "subdir" || (flow == "agent_dir" && path_like(query)));
  if (flow == "switch") return switcher_items();
  if (flow == "agent_dir") return folder_items(query, true);
  if (flow == "folder") return folder_items(query, false);
  if (flow == "subdir") {
    // Browsing, but only inside the project: its own folder first, then the
    // folders under it; what is typed can be used as it is.
    const std::string project = menu_ ? menu_->carry : std::string();
    auto inside = [&](const std::string& p) { return p == project || p.starts_with(project + "/"); };
    std::vector<PickItem> out;
    PickItem same;
    // Labelled with its path, so the path typed to browse still matches it.
    same.label = abbreviate(project) + "/";
    same.detail = "same folder as the project: holds the chats you put in it";
    same.id = "dir:" + project;
    out.push_back(std::move(same));
    for (auto& it : folder_items(query, false)) {
      const std::string dir = it.id.substr(4);
      if (it.pinned) {
        it.label = "Use \xE2\x80\x9C" + (query.empty() ? abbreviate(project) : query) + "\xE2\x80\x9D";
        it.detail.clear();
        it.enabled = is_dir(dir) && inside(dir);
        if (!it.enabled) it.detail = is_dir(dir) ? "not inside the project" : "not a folder";
        if (dir == project) continue;  // the first row says it already
      } else if (!inside(dir)) {
        continue;
      }
      out.push_back(std::move(it));
    }
    return out;
  }

  // "agent": the four known agents, then commands typed here before, then
  // whatever is being typed, as a command of its own.
  if (!recent_loaded_) {
    recent_loaded_ = true;
    if (FILE* f = fopen((config_dir() + "/commands").c_str(), "r")) {
      char line[1024];
      while (fgets(line, sizeof line, f)) {
        std::string c = line;
        while (!c.empty() && (c.back() == '\n' || c.back() == '\r')) c.pop_back();
        if (!c.empty()) recent_commands_.push_back(c);
      }
      fclose(f);
    }
  }
  std::vector<PickItem> out;
  for (const Adapter* a : all_adapters()) {
    const std::string id(a->id());
    PickItem it;
    it.label = id;
    it.detail = a->name();
    it.id = "cmd:" + id;
    it.group = "Agents";
    if (!on_path(id.c_str())) it.hint = "not on PATH";
    out.push_back(std::move(it));
  }
  for (const auto& c : recent_commands_) {
    if (is_agent(c)) continue;
    PickItem it;
    it.label = c;
    it.id = "cmd:" + c;
    it.group = "Recent commands";
    out.push_back(std::move(it));
  }
  std::string typed = query;
  while (!typed.empty() && typed.back() == ' ') typed.pop_back();
  if (!typed.empty() && !is_agent(typed) &&
      std::find(recent_commands_.begin(), recent_commands_.end(), typed) == recent_commands_.end()) {
    PickItem it;
    it.label = "Run \xE2\x80\x9C" + typed + "\xE2\x80\x9D";  // “…”
    it.detail = "in a terminal pane";
    it.id = "cmd:" + typed;
    it.pinned = true;
    out.push_back(std::move(it));
  }
  return out;
}

std::vector<PickItem> App::switcher_items() const {
  std::vector<PickItem> out;
  auto folder_name = [](const std::string& cwd) {
    const size_t s = cwd.find_last_of('/');
    return s == std::string::npos ? cwd : cwd.substr(s + 1);
  };
  // Running agents first, the ones waiting on you at the top.
  std::vector<std::pair<int, PickItem>> live;
  std::set<std::string> live_keys;
  for (const auto& sp : ws_.live()) {
    const LiveSession& s = *sp;
    live_keys.insert(s.agent() + "\t" + s.session_id());
    if (s.exited() && ws_.store().archived(s.agent(), s.session_id())) continue;
    const ChatState cs = chat_state(&s, theme());
    PickItem it;
    it.label = session_title(s);
    it.lead = std::string();
    text::encode(cs.glyph, it.lead);
    it.lead_color = cs.color;
    it.detail = folder_name(s.cwd()) + " \xC2\xB7 " + agent_label(s.agent());
    it.hint = s.exited() ? "stopped" : cs.word;
    it.id = "live:" + std::to_string(reinterpret_cast<uintptr_t>(&s));
    const int rank = s.exited() ? 4 : cs.rank;
    it.group = rank == 0   ? "Needs you"
               : rank == 1 ? "New reply"
               : rank == 2 ? "Working"
               : rank == 3 ? "Ready"
                           : "Stopped";
    live.push_back({rank, std::move(it)});
  }
  std::stable_sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [rank, it] : live) out.push_back(std::move(it));

  // Then every stored chat, newest first, whichever folder it is in.
  std::vector<const SessionRef*> stored;
  for (const auto& p : ws_.store().projects())
    for (const auto& s : p.sessions)
      if (!live_keys.count(s.agent + "\t" + s.id) && !ws_.store().archived(s.agent, s.id)) stored.push_back(&s);
  std::stable_sort(stored.begin(), stored.end(),
                   [](const SessionRef* a, const SessionRef* b) { return a->mtime > b->mtime; });
  if (stored.size() > 500) stored.resize(500);
  for (const SessionRef* s : stored) {
    PickItem it;
    if (const std::string* n = ws_.store().custom_name(s->agent, s->id)) it.label = *n;
    else it.label = s->title.empty() ? s->id : text::oneline(s->title, 200);
    it.lead = "\xC2\xB7";  // ·
    it.lead_color = theme().dim;
    it.detail = folder_name(s->cwd) + " \xC2\xB7 " + agent_label(s->agent);
    it.hint = rel_time(s->mtime);
    it.id = "path:" + s->path;
    it.group = "Recent";
    out.push_back(std::move(it));
  }
  return out;
}

std::vector<PickItem> App::folder_items(const std::string& query, bool tracked) {
  std::vector<PickItem> out;
  const Project* cur = current_project();
  const std::string from = cur ? cur->path : home_dir();

  if (tracked && !path_like(query)) {
    // The folders mico tracks, the selected one first.
    std::vector<std::string> dirs = ws_.store().folders();
    if (cur) {
      auto it = std::find(dirs.begin(), dirs.end(), cur->path);
      if (it != dirs.end()) std::rotate(dirs.begin(), it, it + 1);
    }
    for (const auto& d : dirs) {
      PickItem it;
      it.label = abbreviate(d);
      it.id = "dir:" + d;
      it.group = "Tracked folders";
      if (cur && d == cur->path) it.hint = "selected";
      if (!is_dir(d)) {
        it.hint = "missing";
        it.enabled = false;
      }
      out.push_back(std::move(it));
    }
    return out;
  }

  // Browsing: the folders inside the one the query names so far. What
  // follows its last slash narrows them.
  const std::string typed = query.empty() ? "~/" : query;
  const size_t slash = typed.rfind('/');
  const std::string shown = slash == std::string::npos ? (typed.starts_with('~') ? "~/" : "") : typed.substr(0, slash + 1);
  const std::string partial = slash == std::string::npos ? typed : typed.substr(slash + 1);
  const std::string base = shown.empty() ? from : trim_slashes(expand(shown, from));
  if (base != browse_dir_) {
    browse_dir_ = base;
    browse_names_.clear();
    if (DIR* d = opendir(base.c_str())) {
      while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        bool dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) dir = is_dir(base + "/" + n);
        if (dir) browse_names_.push_back(n);
      }
      closedir(d);
    }
    std::sort(browse_names_.begin(), browse_names_.end());
  }
  const bool hidden = partial.starts_with('.');
  const std::string prefix = shown.empty() ? abbreviate(from) + "/" : shown;
  for (const auto& n : browse_names_) {
    if (n.starts_with('.') && !hidden) continue;
    PickItem it;
    it.label = prefix + n + "/";
    it.id = "dir:" + (base == "/" ? "" : base) + "/" + n;
    out.push_back(std::move(it));
    if (out.size() >= 1000) break;
  }
  // The query itself, for a folder the listing cannot reach or one that
  // matches nothing yet.
  const std::string whole = trim_slashes(expand(typed, from));
  PickItem use;
  use.label = std::string(tracked ? "Use " : "Add ") + "\xE2\x80\x9C" + typed + "\xE2\x80\x9D";
  use.id = "dir:" + whole;
  use.pinned = true;
  if (!is_dir(whole)) {
    use.detail = "not a folder";
    use.enabled = false;
  } else if (!tracked) {
    for (const auto& f : ws_.store().folders())
      if (f == whole) {
        use.detail = "already tracked";
        use.enabled = false;
      }
  }
  out.push_back(std::move(use));
  return out;
}

// ------------------------------------------------------------------ answers

void App::remember_command(const std::string& cmd) {
  if (cmd.empty()) return;
  recent_commands_.erase(std::remove(recent_commands_.begin(), recent_commands_.end(), cmd),
                         recent_commands_.end());
  recent_commands_.insert(recent_commands_.begin(), cmd);
  if (recent_commands_.size() > 20) recent_commands_.resize(20);
  if (FILE* f = fopen((config_dir() + "/commands").c_str(), "w")) {
    for (const auto& c : recent_commands_) fprintf(f, "%s\n", c.c_str());
    fclose(f);
  }
}

void App::flow_chosen(const PickItem& it) {
  if (!menu_) return;
  const std::string flow = menu_->flow;
  const std::string carry = menu_->carry;

  if (flow == "switch") {
    close_menu();
    if (it.id.starts_with("live:")) {
      const uintptr_t want = std::strtoull(it.id.c_str() + 5, nullptr, 10);
      for (const auto& sp : ws_.live()) {
        if (reinterpret_cast<uintptr_t>(sp.get()) != want) continue;
        const auto& ps = ws_.store().projects();
        for (size_t p = 0; p < ps.size(); p++)
          if (ps[p].path == sp->cwd()) select_project(int(p));
        select_live(sp.get());
        show_tab(0);
        open_selected_chat();
        return;
      }
      set_status("that agent is gone");
      return;
    }
    const std::string path = it.id.substr(5);
    const auto& ps = ws_.store().projects();
    for (size_t p = 0; p < ps.size(); p++)
      for (size_t i = 0; i < ps[p].sessions.size(); i++)
        if (ps[p].sessions[i].path == path) {
          force_select(int(p), int(i));
          show_tab(0);
          layout_dirty_ = true;
          open_selected_chat();
          return;
        }
    return;
  }
  if (flow == "agent") {
    open_new_agent(it.id.substr(4));
    return;
  }
  if (flow == "agent_dir") {
    close_menu();
    const std::string dir = it.id.substr(4);
    remember_command(carry);
    auto argv = split_command(carry);
    if (argv.empty()) return;
    // A bare agent name goes through the agent path so it gets a
    // pre-assigned session id and a chat view; anything else runs verbatim.
    if (argv.size() == 1 && is_agent(argv[0])) spawn_agent(argv[0], dir);
    else spawn_raw(std::move(argv), dir);
    return;
  }
  if (flow == "subdir") {
    close_menu();
    const std::string dir = it.id.substr(4);
    // A name to go with it: the folder's own, for one under the project.
    std::string name = dir == carry ? std::string() : dir.substr(dir.find_last_of('/') + 1);
    ask("Sub-project name", "add_sub_name", name, carry + "\n" + dir);
    return;
  }
  if (flow == "folder") {
    close_menu();
    const std::string dir = it.id.substr(4);
    if (ws_.store().add_folder(dir)) {
      set_status("tracking " + abbreviate(dir));
      select_project(int(ws_.store().projects().size()) - 1);
    } else {
      set_status("could not add " + abbreviate(dir));
    }
  }
}

void App::flow_complete(const PickItem& it) {
  if (!menu_) return;
  Picker& pk = menu_->picker;
  std::string q;
  if (menu_->flow == "agent") {
    q = it.id.substr(4) + " ";  // to add arguments to
  } else if (it.id.starts_with("dir:")) {
    q = abbreviate(it.id.substr(4));
    if (!q.ends_with('/')) q += "/";
  } else {
    return;
  }
  pk.set_query(q);
  pk.set_items(flow_items(menu_->flow, q));
  pk.cursor_to_first();
}

void App::flow_back() {
  if (menu_ && menu_->flow == "agent_dir") {
    const std::string cmd = menu_->carry;
    open_flow("agent", "New agent", "enter choose \xC2\xB7 tab edit \xC2\xB7 esc close", {},
              is_agent(cmd) ? std::string() : cmd);
  }
}

}  // namespace mico
