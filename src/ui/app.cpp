#include "ui/app.h"

#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <unordered_set>

#include "adapters/adapters.h"
#include "base/fs.h"
#include "base/log.h"
#include "core/away.h"
#include "core/opener.h"
#include "core/pty.h"
#include "core/settings.h"
#include "core/web_access.h"
#include "core/x11_clipboard.h"
#include "math/picture.h"
#include "term/kitty.h"
#include "math/math.h"
#include "term/sixel.h"
#include "term/encoder.h"
#include "base/text.h"
#include "views/views.h"

namespace mico {

const char* density_name(Density d) {
  switch (d) {
    case Density::Minimal: return "minimal";
    case Density::Normal: return "normal";
    default: return "full";
  }
}

App::App() : own_ws_(std::make_unique<Workspace>()), ws_(*own_ws_) {
  load_layout();
  load_view();
  build_layout();
}

App::App(Workspace& ws) : ws_(ws) {
  load_layout();
  load_view();
  build_layout();
}

namespace {

// A value on one line: newlines and backslashes escaped.
std::string view_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out;
}

std::string view_unescape(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      out += s[++i] == 'n' ? '\n' : s[i];
    } else {
      out += s[i];
    }
  }
  return out;
}

constexpr size_t kViewHistory = 100;  // command lines kept

}  // namespace

// "key value" lines; one "command" line per history entry, oldest first.
std::string App::view_state() const {
  const char* density = filters_.density == Density::Minimal ? "minimal"
                        : filters_.density == Density::Full  ? "full"
                                                             : "normal";
  // A live chat is remembered by its transcript: after a restart it is a
  // stored chat to resume.
  const std::string chat = selected_live_ ? selected_live_->transcript() : selected_path_;
  std::string s;
  s += "density " + std::string(density) + "\n";
  s += "tab " + tabs_[tab_].name + "\n";
  s += "folder " + view_escape(project_path_) + "\n";
  s += "sub " + view_escape(sub_) + "\n";
  s += "chat " + view_escape(chat) + "\n";
  s += "all-folders " + std::string(all_folders_ ? "on" : "off") + "\n";
  s += "chat-filter " + std::string(chat_filter_ ? "on" : "off") + "\n";
  s += "diff-span " + std::to_string(diff_.span) + "\n";
  s += "diff-by " + std::string(diff_.group == 2 ? "commit" : diff_.group == 1 ? "chat" : "file") + "\n";
  s += "diff-selected " + view_escape(diff_.sel_key) + "\n";
  const size_t from = cmd_history_.size() > kViewHistory ? cmd_history_.size() - kViewHistory : 0;
  for (size_t i = from; i < cmd_history_.size(); i++) s += "command " + view_escape(cmd_history_[i]) + "\n";
  return s;
}

void App::load_view() {
  std::string buf;
  const std::string_view text = fs::read_prefix(config_dir() + "/view", 1u << 20, buf);
  for (size_t at = 0; at < text.size();) {
    size_t nl = text.find('\n', at);
    if (nl == std::string_view::npos) nl = text.size();
    const std::string_view line = text.substr(at, nl - at);
    at = nl + 1;
    const size_t sp = line.find(' ');
    const std::string_view key = line.substr(0, sp);
    const std::string value = sp == std::string_view::npos ? std::string() : view_unescape(line.substr(sp + 1));
    if (key == "density") {
      filters_.density = value == "minimal" ? Density::Minimal : value == "full" ? Density::Full : Density::Normal;
    } else if (key == "tab") {
      // By name; a number is from before the Git tab, when Settings was 5.
      for (size_t t = 0; t < tabs_.size(); t++)
        if (tabs_[t].name == value) tab_ = t;
      if (!value.empty() && std::isdigit(uint8_t(value[0]))) {
        const size_t t = size_t(std::atoi(value.c_str()));
        if (t < 5) tab_ = t;
        else if (t == 5) tab_ = 6;
      }
    } else if (key == "folder") {
      project_path_ = value;
    } else if (key == "sub") {
      sub_ = value;
    } else if (key == "chat") {
      selected_path_ = value;
    } else if (key == "all-folders") {
      all_folders_ = value == "on";
    } else if (key == "chat-filter") {
      chat_filter_ = value == "on";
    } else if (key == "diff-span") {
      diff_.span = std::atoi(value.c_str());
    } else if (key == "diff-by") {
      diff_.group = value == "commit" ? 2 : value == "chat" ? 1 : 0;
    } else if (key == "diff-selected") {
      diff_.sel_key = value;
    } else if (key == "command" && !value.empty()) {
      cmd_history_.push_back(value);
    }
  }
  if (all_folders_) chat_filter_ = false;
  saved_view_ = view_state();
}

void App::save_view_if_changed() {
  std::string now = view_state();
  if (now == saved_view_) return;
  fs::make_dirs(config_dir());
  const std::string path = config_dir() + "/view", tmp = path + ".tmp";
  if (FILE* f = fopen(tmp.c_str(), "w")) {
    const bool ok = fwrite(now.data(), 1, now.size(), f) == now.size();
    if (fclose(f) == 0 && ok) rename(tmp.c_str(), path.c_str());
    else unlink(tmp.c_str());
  }
  saved_view_ = std::move(now);
}

LayoutMode layout_mode() {
  std::string buf;
  const std::string_view v = fs::read_prefix(config_dir() + "/layout-mode", 64, buf);
  return v.starts_with("wide") ? LayoutMode::Wide : v.starts_with("compact") ? LayoutMode::Compact : LayoutMode::Auto;
}

void set_layout_mode(LayoutMode m) {
  mkdir(config_dir().c_str(), 0700);
  if (FILE* f = fopen((config_dir() + "/layout-mode").c_str(), "w")) {
    fputs(m == LayoutMode::Wide ? "wide\n" : m == LayoutMode::Compact ? "compact\n" : "auto\n", f);
    fclose(f);
  }
}

App::~App() = default;

void App::build_layout() {
  if (root_) {
    if (auto pane = root_->take_session_pane()) {
      LiveSession* session = pane->session();
      session_panes_[session] = std::move(pane);
    }
  }
  std::erase_if(session_panes_, [&](const auto& entry) {
    return std::none_of(ws_.live().begin(), ws_.live().end(),
                        [&](const auto& s) { return s.get() == entry.first; });
  });
  // Compact: the one pane the screen shows.
  if (compact_ && screen_ != Screen::Main) {
    root_ = Node::leaf(screen_ == Screen::Folders ? make_project_list() : make_chat_list());
    root_->for_each_pane([&](Pane& p) { p.set_app(this); });
    layout_dirty_ = false;
    return;
  }

  // Sidebar: the tracked folders, then the unified chat list for the selected
  // folder (running sessions and stored transcripts together). It stays on
  // every tab: on Sessions it picks the chat, elsewhere what the tab covers.
  std::vector<std::unique_ptr<Node>> left;
  left.push_back(Node::leaf(make_project_list()));
  left.push_back(Node::leaf(make_chat_list()));
  auto sidebar = Node::split(SplitDir::Vertical, std::move(left), {0.28f, 0.72f});

  // Main area: the tab. On Sessions, the pane for whatever the list has
  // selected — a live session's own view, or a read-only browse of a stored
  // transcript. Switching away leaves that selection as it was.
  std::unique_ptr<Node> main;
  if (tab_ == 1) main = Node::leaf(make_usage_view(ws_.usage()));
  else if (tab_ == 2) main = Node::leaf(make_search_view(ws_.search()));
  else if (tab_ == 3) main = Node::leaf(make_tools_view(ws_.activity()));
  else if (tab_ == 4) main = Node::leaf(make_diff_view(ws_.activity(), diff_));
  else if (tab_ == 5) main = Node::leaf(make_git_view(git_));
  else if (tab_ == 6) main = Node::leaf(make_settings_view());
  if (!main && selected_live_) {
    bool alive = false;
    for (auto& s : ws_.live())
      if (s.get() == selected_live_) alive = true;
    if (alive) {
      auto& pane = session_panes_[selected_live_];
      if (!pane) pane = make_session_pane(selected_live_);
      main = Node::leaf(std::move(pane));
    }
  }
  if (!main) {
    if (tab_ == 0) selected_live_ = nullptr;
    main = Node::leaf(make_chat_view());
  }

  if (compact_) {
    root_ = std::move(main);
    root_->for_each_pane([&](Pane& p) { p.set_app(this); });
    layout_dirty_ = false;
    return;
  }

  std::vector<std::unique_ptr<Node>> cols;
  cols.push_back(std::move(sidebar));
  cols.push_back(std::move(main));
  root_ = Node::split(SplitDir::Horizontal, std::move(cols), {0.30f, 0.70f});

  root_->for_each_pane([&](Pane& p) { p.set_app(this); });
  // Give the rebuilt tree back whatever sizes the user dragged.
  size_t idx = 0;
  root_->apply_fractions(layout_fracs_, idx);
  layout_dirty_ = false;
}

// Split sizes live beside the folder list so a restart looks the way it was
// left. One line per split, fractions space-separated, in placement order.
void App::load_layout() {
  FILE* f = fopen((config_dir() + "/layout").c_str(), "r");
  if (!f) return;
  std::vector<std::vector<float>> rows;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    std::vector<float> row;
    const char* p = line;
    char* end = nullptr;
    for (;;) {
      const float v = strtof(p, &end);
      if (end == p) break;
      row.push_back(v);
      p = end;
    }
    if (!row.empty()) rows.push_back(std::move(row));
  }
  fclose(f);
  layout_fracs_ = std::move(rows);
}

void App::save_layout() {
  if (!root_) return;
  layout_fracs_.clear();
  root_->collect_fractions(layout_fracs_);
  if (layout_fracs_.empty()) return;
  mkdir(config_dir().c_str(), 0700);
  FILE* f = fopen((config_dir() + "/layout").c_str(), "w");
  if (!f) return;
  for (const auto& row : layout_fracs_) {
    for (size_t i = 0; i < row.size(); i++) fprintf(f, i ? " %.4f" : "%.4f", row[i]);
    fputc('\n', f);
  }
  fclose(f);
}

void App::open_search(std::string query) {
  show_tab(2);
  // Asked with a query — "every chat" from a chat's find bar, or :search —
  // it covers every folder; the sidebar can narrow it after.
  if (!query.empty()) set_all_folders(true);
  search_request_ = std::move(query);
  search_requested_ = true;
  mark_dirty();
}

void App::open_at(const std::string& path, uint64_t offset, const std::string& query) {
  LiveSession* live = nullptr;
  for (const auto& s : ws_.live())
    if (s->transcript() == path) live = s.get();
  if (live) {
    select_live(live);
  } else {
    const auto& ps = ws_.store().projects();
    for (size_t p = 0; p < ps.size(); p++)
      for (size_t i = 0; i < ps[p].sessions.size(); i++)
        if (ps[p].sessions[i].path == path) force_select(int(p), int(i));
  }
  reveal_ = Reveal{path, offset, query};
  show_tab(0);
  layout_dirty_ = true;
  focus_chat_after_build_ = true;
}

void App::show_tab(size_t i) {
  // Compact: whatever shows a tab, or opens a chat, goes to the main screen.
  if (compact_ && screen_ != Screen::Main && i < tabs_.size()) {
    screen_ = Screen::Main;
    layout_dirty_ = true;
    focus_chat_after_build_ = true;
  }
  if (i >= tabs_.size() || i == tab_) return;
  tab_ = i;
  layout_dirty_ = true;
  // The sidebar stays; the keys go to what the tab shows.
  focus_chat_after_build_ = true;
}

std::string App::usable_cwd(const std::string& want) const {
  const Project* p = current_project();
  return ws_.usable_cwd(want, p ? p->path : std::string());
}

bool App::spawn_agent(const std::string& agent, const std::string& cwd) {
  show_tab(0);
  const Project* pr = current_project();
  const Workspace::Started r = ws_.start_agent(agent, cwd, pr ? pr->path : std::string());
  if (!r.session) {
    set_status(r.error);
    return false;
  }
  focus_after_build_ = r.session;
  select_live(r.session);
  // Started from a sub-project: filed under it once its id is known.
  if (const SubProject* sp = current_sub(); sp && r.session->cwd() == sp->path && pr)
    ws_.file_under(r.session, pr->path, sp->name);
  layout_dirty_ = true;
  close_menu();
  const std::string& dir = r.session->cwd();
  set_status(!r.moved ? "started " + agent + " in " + dir
                      : "folder unavailable \xE2\x80\x94 started " + agent + " in " + dir);
  return true;
}

bool App::spawn_continuation(const std::string& agent, const std::string& session_id,
                             const std::string& cwd, bool fork) {
  if (session_id.empty()) return false;
  show_tab(0);
  const Project* pr = current_project();
  const Workspace::Started r = ws_.continue_session(agent, session_id, cwd, fork, pr ? pr->path : std::string());
  if (!r.note.empty()) set_status(r.note);
  using How = Workspace::Started::How;
  switch (r.how) {
    case How::Failed:
      if (!r.error.empty()) set_status(r.error);
      return false;
    case How::Running:
      select_live(r.session);
      focus_after_build_ = r.session;
      focus_session(r.session);
      close_menu();
      set_status("focused the running chat");
      return true;
    case How::Restarted:
      focus_after_build_ = r.session;
      select_live(r.session);
      layout_dirty_ = true;
      close_menu();
      set_status("Resuming " + session_title(*r.session));
      return true;
    case How::Started:
      focus_after_build_ = r.session;
      select_live(r.session);
      layout_dirty_ = true;
      close_menu();
      set_status((r.forked ? "Forked " : "Resuming ") + session_title(*r.session) +
                 (r.moved ? " (original folder unavailable)" : ""));
      return true;
  }
  return false;
}

bool App::spawn_raw(std::vector<std::string> argv, const std::string& cwd) {
  LiveSession* s = ws_.start_command(std::move(argv), cwd);
  if (!s) return false;
  focus_after_build_ = s;
  select_live(s);
  layout_dirty_ = true;
  return true;
}

std::vector<LiveSession*> App::live_sessions() const { return ws_.live_sessions(); }

void App::focus_session(LiveSession* s) {
  for (const auto& p : placed_)
    if (p.pane->session() == s) { focus_ = p.index; return; }
}

void App::reap_sessions() {
  const bool closed = ws_.reap([&](LiveSession* gone) {
    if (selected_live_ == gone) selected_live_ = nullptr;
    if (focus_after_build_ == gone) focus_after_build_ = nullptr;
    watch_.erase(gone);
  });
  if (closed) layout_dirty_ = true;
  if (layout_dirty_) {
    // A pane's menu goes with the pane; mico's own pickers belong to no pane
    // and stay open across the rebuild, so typing into one is not cut off by
    // an agent starting or stopping.
    if (menu_ && menu_->owner) close_menu();
    drag_.reset();
    mouse_capture_ = nullptr;
    dividers_.clear();
    build_layout();
    placed_.clear();
    // focus_after_build_ is consulted by render() once placed_ is filled.
  }
}

bool App::service() {
  x11clip::pump();
  const size_t before = ws_.live().size();
  const bool rebuilt = layout_dirty_;
  reap_sessions();
  bool changed = ws_.live().size() != before || rebuilt;
  const unsigned moved = ws_.service(tab_ == 1);
  // The activity index is shown only by Tools and Diff.
  if (moved & ~unsigned(Workspace::kActivity)) changed = true;
  if ((moved & Workspace::kActivity) && (tab_ == 3 || tab_ == 4 || tab_ == 5)) changed = true;
  // Pictures prepared off this thread: the frame that shows them.
  if (math::collect_prepared()) changed = true;

  // Relative times in the sidebar ("3m", "2h") drift on their own, so nudge a
  // repaint occasionally even when nothing else moved.
  static int64_t last_tick = 0;
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const int64_t now = int64_t(ts.tv_sec);
  if (now != last_tick) {
    last_tick = now;
    tick_++;
    changed = true;
  }

  // A working agent's spinner needs a faster beat than the clock. This only
  // runs while something is actually busy, so an idle daemon still costs
  // nothing.
  static int64_t last_anim = 0;
  const bool any_busy = ws_.any_busy();
  const int64_t now_ms = int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
  if (any_busy && now_ms - last_anim >= 120) {
    last_anim = now_ms;
    anim_++;
    changed = true;
  }

  watch_agents(now_ms);
  if (std::string w = ws_.take_memory_warning(); !w.empty()) set_status(std::move(w));

  // What git blame said: the chat behind the commit, when an agent made it.
  for (GitIndex::Blame b; ws_.git().take_blame(b);) {
    const std::string where = b.file + ":" + std::to_string(b.line);
    if (!b.error.empty()) set_status(where + ": " + b.error);
    else if (!open_commit(b.hash)) set_status(where + " is from " + b.hash.substr(0, 7) + ", not a commit by any chat mico knows");
    changed = true;
  }

  if (changed) dirty_ = true;
  return std::exchange(dirty_, false);
}

void App::watch_agents(int64_t now) {
  next_watch_ms_ = 0;
  for (const auto& owned : ws_.live()) {
    LiveSession* s = owned.get();
    // Only agents mico knows: a plain command's output says nothing about
    // whether it is done.
    if (!s->adapter() || !s->spawned() || s->exited()) {
      // Killed outright while it was being watched: nearly always for memory.
      if (s->exited() && watch_.erase(s) && s->pty().exit_signal() == SIGKILL)
        set_status(session_title(*s) + " was killed (SIGKILL), most likely for memory \xC2\xB7 see :log");
      watch_.erase(s);
      continue;
    }
    const bool waiting = s->needs_input() && !s->starting();
    const bool busy = s->busy();
    // The first look only learns the state: an agent found already idle, as
    // one resumed is, has finished nothing just now.
    const auto [it, first] = watch_.try_emplace(s);
    Watch& w = it->second;
    if (first) {
      w.waiting = waiting;
      w.busy_since = busy ? now : 0;
      continue;
    }
    std::string event;
    if (waiting && !w.waiting) event = "needs you";
    w.waiting = waiting;
    if (busy) {
      if (!w.busy_since) w.busy_since = now;
      w.done_at = 0;
    } else if (w.busy_since) {
      w.busy_since = 0;
      w.done_at = now + kSettleMs;
    }
    if (w.done_at && now >= w.done_at) {
      w.done_at = 0;
      // A question it stopped on has been announced as that.
      if (!waiting && event.empty()) event = "finished";
    }
    if (w.done_at && (!next_watch_ms_ || w.done_at < next_watch_ms_)) next_watch_ms_ = w.done_at;
    if (event.empty() || notify_mode() == NotifyMode::Off) continue;

    Notice n;
    n.title = session_title(*s);
    n.body = std::string(s->adapter()->label()) + " " + event;
    n.on_screen = tab_ == 0 && selected_live_ == s;
    // Said here too, for when the terminal is the one being looked at.
    if (!n.on_screen) set_status(n.title + " \xC2\xB7 " + event);
    MLOG("notice: %s: %s", n.title.c_str(), n.body.c_str());
    notices_.push_back(std::move(n));
  }
}

std::string App::notice_seq(const Notice& n, NotifyEscape how) {
  return notify_seq(notify_mode() == NotifyMode::Bell ? NotifyEscape::Bell : how, n.title, n.body);
}

bool App::open_commit(const std::string& hash) {
  // The chat printed the hash abbreviated; the one asked for may be longer or
  // shorter. Either begins the other.
  for (const ChatActivity* c : ws_.activity().chats())
    for (const ChatCommit& cm : c->commits) {
      const size_t n = std::min(cm.hash.size(), hash.size());
      if (n < 4 || cm.hash.compare(0, n, hash, 0, n) != 0) continue;
      open_at(c->path, cm.call_offset, {});
      set_status(cm.hash + " \xC2\xB7 " + cm.subject + " \xC2\xB7 made in " +
                 (c->title.empty() ? agent_label(c->agent) + " chat" : text::oneline(c->title, 60)));
      return true;
    }
  return false;
}

int App::idle_timeout_ms() const {
  int ms = ws_.idle_timeout_ms(tab_ == 1);
  // To call a turn finished on time when nothing else wakes the loop.
  if (next_watch_ms_) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    ms = std::min(ms, int(std::clamp<int64_t>(next_watch_ms_ - now, 1, 1000)));
  }
  return math::preparing() ? std::min(ms, 10) : ms;
}

void App::collect_session_fds(std::vector<int>& out) const {
  ws_.collect_fds(out);
  // A paste in another window is a request to us while we own the clipboard.
  if (const int x = x11clip::fd(); x >= 0) out.push_back(x);
}

void App::open_in_editor(const std::string& url) {
  std::string path = url.substr(7);
  int line = 0;
  if (const size_t h = path.find("#L"); h != std::string::npos) {
    line = std::atoi(path.c_str() + h + 2);
    path.resize(h);
  }
  // Only an absolute path: the link may come from what an agent wrote, and a
  // "path" like "+!cmd" or "-c..." would be read by the editor as an option.
  if (!path.starts_with("/")) return;
  const char* ed = getenv("VISUAL");
  if (!ed || !*ed) ed = getenv("EDITOR");
  if (!ed || !*ed) ed = "vi";
  // The editor as the user set it (it may carry its own arguments); the path
  // and line go in as arguments, never through the shell's parsing.
  std::vector<std::string> argv{"/bin/sh", "-c", std::string("exec ") + ed + " \"$@\"", "mico-edit"};
  if (line > 0) argv.push_back("+" + std::to_string(line));
  argv.push_back(path);
  const size_t slash = path.rfind('/');
  spawn_raw(std::move(argv), slash == std::string::npos ? std::string(".") : path.substr(0, slash));
}

void App::copy_to_clipboard(std::string text) {
  x11clip::set_text(text);
  clipboard_ = std::move(text);
}



int App::project_index() const {
  const auto& ps = ws_.store().projects();
  if (!project_path_.empty())
    for (size_t i = 0; i < ps.size(); i++)
      if (ps[i].path == project_path_) return int(i);
  return project_ >= 0 && size_t(project_) < ps.size() ? project_ : -1;
}

const Project* App::current_project() const {
  const auto& ps = ws_.store().projects();
  const int i = project_index();
  return i < 0 ? nullptr : &ps[size_t(i)];
}

const SubProject* App::current_sub() const {
  const Project* pr = current_project();
  if (!pr || sub_.empty()) return nullptr;
  for (const auto& sp : pr->subs)
    if (sp.name == sub_) return &sp;
  return nullptr;
}

void App::select_sub(const std::string& name) {
  if (name == sub_) return;
  sub_ = name;
  filter_version_++;
  // Like a folder, a sub-project opens on its own newest chat.
  selected_path_.clear();
  selected_live_ = nullptr;
  layout_dirty_ = true;
  notify_state_changed();
}

std::string App::live_sub(const LiveSession& s) const { return ws_.sub_of(s); }

std::string App::selected_cwd() const {
  if (const SubProject* sp = current_sub()) return sp->path;
  const Project* pr = current_project();
  return pr ? pr->path : std::string(".");
}

const SessionRef* App::current_session() const {
  const Project* p = current_project();
  if (!p || selected_path_.empty()) return nullptr;
  for (const auto& sr : p->sessions)
    if (sr.path == selected_path_) return &sr;
  return nullptr;
}

App::ViewFilter App::view_filter() const {
  ViewFilter f;
  if (all_folders_) {
    f.label = "all folders";
    return f;
  }
  f.project = current_project();
  f.label = f.project ? f.project->name : "all folders";
  f.sub = current_sub();
  if (f.sub) f.label += " \xE2\x80\xBA " + f.sub->name;  // ›
  if (!f.project || !chat_filter_) return f;
  if (selected_live_) {
    f.chat_path = selected_live_->transcript();
    f.label += " \xC2\xB7 " + session_title(*selected_live_);
  } else if (!selected_path_.empty()) {
    f.chat_path = selected_path_;
    for (const auto& s : f.project->sessions)
      if (s.path == selected_path_) {
        std::string t = s.title.empty() ? s.id : text::oneline(s.title, 60);
        if (const std::string* n = ws_.store().custom_name(s.agent, s.id)) t = *n;
        f.label += " \xC2\xB7 " + t;
      }
  }
  // A new chat with nothing written yet has no transcript to narrow to; it
  // filters to nothing rather than falling back to the folder.
  if (f.chat_path.empty()) f.chat_path = "\n(no transcript yet)";
  return f;
}

std::vector<Project> App::filtered_projects() const {
  const ViewFilter f = view_filter();
  if (!f.project) return ws_.store().projects();
  Project one = *f.project;
  if (!f.chat_path.empty())
    std::erase_if(one.sessions, [&](const SessionRef& s) { return s.path != f.chat_path; });
  else if (f.sub)
    std::erase_if(one.sessions, [&](const SessionRef& s) { return s.sub != f.sub->name; });
  return {std::move(one)};
}

bool App::in_filter(const std::string& project_name, const std::string& path) const {
  const ViewFilter f = view_filter();
  if (!f.project) return true;
  if (project_name != f.project->name) return false;
  if (!f.chat_path.empty()) return path == f.chat_path;
  if (f.sub) return ws_.store().sub_of_path(path) == f.sub->name;
  return true;
}

void App::select_project(int i) {
  if (i == project_index() && sub_.empty()) return;
  filter_version_++;
  project_ = i;
  const auto& ps = ws_.store().projects();
  project_path_ = i >= 0 && size_t(i) < ps.size() ? ps[size_t(i)].path : std::string();
  sub_.clear();
  selected_path_.clear();
  selected_live_ = nullptr;
  layout_dirty_ = true;
  notify_state_changed();
}

void App::select_live(LiveSession* s) {
  if (selected_live_ == s && selected_path_.empty()) return;
  filter_version_++;
  selected_live_ = s;
  selected_path_.clear();
  layout_dirty_ = true;
  notify_state_changed();
}

void App::select_stored(const std::string& transcript_path) {
  if (selected_live_ == nullptr && selected_path_ == transcript_path) return;
  filter_version_++;
  selected_live_ = nullptr;
  selected_path_ = transcript_path;
  layout_dirty_ = true;
  notify_state_changed();
}

void App::compact_show(Screen s) {
  if (s == screen_) return;
  screen_ = s;
  layout_dirty_ = true;
  focus_ = 0;
  mark_dirty();
}

void App::compact_back() {
  if (screen_ == Screen::Main) compact_show(Screen::Chats);
  else if (screen_ == Screen::Chats) compact_show(Screen::Folders);
}

bool App::open_selected_chat() {
  if (compact_ && (selected_live_ || current_session())) show_tab(0);
  if (selected_live_) {
    selected_live_->pty().poll_exit();
    if (selected_live_->exited())
      return spawn_continuation(selected_live_->agent(), selected_live_->session_id(),
                                selected_live_->cwd(), false);
    focus_after_build_ = selected_live_;
    focus_session(selected_live_);
    return true;
  }
  const SessionRef* s = current_session();
  if (!s) return false;
  return spawn_continuation(s->agent, s->id, s->cwd, false);
}

std::string App::session_title(const LiveSession& session) const { return ws_.title_of(session); }

void App::force_select(int p, int s) {
  project_ = p;
  {
    const auto& ps = ws_.store().projects();
    project_path_ = p >= 0 && size_t(p) < ps.size() ? ps[size_t(p)].path : std::string();
    sub_.clear();
  }
  selected_path_.clear();
  const Project* pr = current_project();
  if (pr && s >= 0 && size_t(s) < pr->sessions.size())
    selected_path_ = pr->sessions[size_t(s)].path;
  selected_live_ = nullptr;
  layout_dirty_ = true;
  notify_state_changed();
}

void App::notify_state_changed() {
  if (root_) root_->for_each_pane([](Pane& p) { p.on_state_changed(); });
}

void App::ask(std::string label, std::string action, std::string initial, std::string carry) {
  close_menu();
  prompt_ = Prompt{std::move(label), std::move(action), std::move(initial), std::move(carry)};
}

// Splits a command line on whitespace, honouring single and double quotes so a
// path with a space in it survives.
std::vector<std::string> split_command(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  char quote = 0;
  bool any = false;
  for (char c : s) {
    if (quote) {
      if (c == quote) quote = 0;
      else cur.push_back(c);
      continue;
    }
    if (c == '\'' || c == '"') { quote = c; any = true; continue; }
    if (c == ' ' || c == '\t') {
      if (any || !cur.empty()) { out.push_back(cur); cur.clear(); any = false; }
      continue;
    }
    cur.push_back(c);
  }
  if (any || !cur.empty()) out.push_back(cur);
  return out;
}

void App::prompt_submit() {
  if (!prompt_) return;
  Prompt p = *prompt_;
  prompt_.reset();

  if (p.action == "add_folder") {
    if (ws_.store().add_folder(p.text)) {
      set_status("tracking " + p.text);
      select_project(int(ws_.store().projects().size()) - 1);
    } else {
      set_status("could not add folder: " + p.text);
    }
    return;
  }
  if (p.action == "add_sub_name" || p.action == "rename_sub") {
    const size_t nl = p.carry.find('\n');
    if (nl == std::string::npos) return;
    const std::string project = p.carry.substr(0, nl), other = p.carry.substr(nl + 1);
    std::string name = p.text;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    if (name.empty()) return;
    const bool add = p.action == "add_sub_name";
    const bool ok = add ? ws_.store().add_subproject(project, name, other)
                        : ws_.store().rename_subproject(project, other, name);
    if (!ok) {
      set_status("could not " + std::string(add ? "add" : "rename") + " \xE2\x80\x9C" + name +
                 "\xE2\x80\x9D \xE2\x80\x94 is the name taken?");
      return;
    }
    // Land on it: the new or renamed sub-project is what is selected.
    const auto& ps = ws_.store().projects();
    for (size_t i = 0; i < ps.size(); i++)
      if (ps[i].path == project) select_project(int(i));
    select_sub(name);
    set_status(add ? "added sub-project " + name : "renamed to " + name);
    return;
  }
  if (p.action == "rename") {
    const size_t tab = p.carry.find('\t');
    if (tab != std::string::npos) {
      ws_.store().set_custom_name(p.carry.substr(0, tab), p.carry.substr(tab + 1), p.text);
      set_status(p.text.empty() ? "name cleared" : "renamed to " + p.text);
    }
    return;
  }
}

namespace {
// Backspace for a one-line field: a character, or with Ctrl/Alt (or Ctrl+W)
// the word before the end, trailing spaces included.
void erase_back(std::string& t, bool word) {
  const auto pop_char = [&] {
    while (!t.empty() && (uint8_t(t.back()) & 0xC0) == 0x80) t.pop_back();
    if (!t.empty()) t.pop_back();
  };
  if (!word) { pop_char(); return; }
  while (!t.empty() && t.back() == ' ') t.pop_back();
  while (!t.empty() && t.back() != ' ') pop_char();
}
}  // namespace

bool App::prompt_key(const KeyEvent& k) {
  if (!prompt_) return false;
  switch (k.key) {
    case Key::Escape: prompt_.reset(); return true;
    case Key::Enter: prompt_submit(); return true;
    case Key::Backspace: erase_back(prompt_->text, k.ctrl || k.alt); return true;
    case Key::Char:
      if (k.ctrl && k.ch == 'u') { prompt_->text.clear(); return true; }
      if (k.ctrl && k.ch == 'w') { erase_back(prompt_->text, true); return true; }
      if (!k.ctrl && !k.alt) text::encode(k.ch, prompt_->text);
      return true;
    default: return true;  // swallow everything: this is modal
  }
}

void App::render_prompt(Surface& s) {
  const Prompt& p = *prompt_;
  const int w = std::min(s.width() - 4, 72);
  const int x = (s.width() - w) / 2;
  const int y = std::max(0, s.height() / 3);
  Painter box(s, Rect{x, y, w, 4});
  box.clear(Style{theme().text, theme().menu_bg});
  box.box(Rect{0, 0, w, 4}, Style{theme().border_focus, theme().menu_bg});
  box.text(2, 0, " " + p.label + " ", Style{theme().border_focus, theme().menu_bg, attr::kBold});

  Style field{theme().text, theme().sel_bg};
  box.fill(Rect{2, 1, w - 4, 1}, field);
  int used = box.text_clipped(2, 1, p.text, field, w - 5);
  box.put(2 + used, 1, U'\u258f', Style{theme().accent, field.bg});
  box.text(2, 2, "enter to run · esc to cancel", Style{theme().dim, theme().menu_bg});
}

void App::open_menu(Pane* owner, Point pos, std::vector<MenuItem> items, std::string title) {
  if (items.empty()) return;
  Picker::Options o;
  o.frame = Picker::Frame::Popup;
  o.filter = true;
  o.title = std::move(title);
  // A menu opened by a click has no cursor until the pointer or a key puts
  // one there: a highlight under nothing reads as a choice already made.
  o.start_unselected = true;
  o.footer = false;
  std::vector<PickItem> picks;
  picks.reserve(items.size());
  for (auto& it : items) {
    PickItem p;
    p.label = std::move(it.label);
    p.detail = std::move(it.detail);
    p.id = std::move(it.action);
    p.enabled = it.enabled;
    p.separator = it.separator;
    p.checked = it.checked;
    picks.push_back(std::move(p));
  }
  Menu m;
  m.owner = owner;
  m.at = pos;
  m.picker = Picker(std::move(o));
  m.picker.set_items(std::move(picks));
  menu_ = std::move(m);
}

void App::menu_result(Picker::Result r) {
  if (!menu_) return;
  if (r == Picker::Result::Cancelled) {
    close_menu();
    return;
  }
  if (!menu_->flow.empty()) {
    // Copies: answering can replace the menu the item lives in.
    const auto item = [&] { return menu_->picker.items()[size_t(menu_->picker.index())]; };
    switch (r) {
      case Picker::Result::Chosen: flow_chosen(item()); break;
      case Picker::Result::Completed: flow_complete(item()); break;
      case Picker::Result::Back: flow_back(); break;
      case Picker::Result::QueryChanged:
        if (menu_->flow != "switch") {
          menu_->picker.set_items(flow_items(menu_->flow, menu_->picker.query()));
          menu_->picker.cursor_to_first();
        }
        break;
      default: break;
    }
    return;
  }
  if (r != Picker::Result::Chosen) return;
  const std::string action = menu_->picker.items()[size_t(menu_->picker.index())].id;
  Pane* owner = menu_->owner;
  close_menu();
  // A menu with no owning pane is mico's own palette.
  if (owner) owner->on_action(action);
  else run_command(action);
}

// ---------------------------------------------------------------- rendering

void App::render_chrome(Surface& s, const Node::Placed& p, bool focused) {
  Painter frame(s, p.rect);
  // No clear here: the surface was already cleared, box() paints every cell of
  // the border ring, and the pane clears its own interior. Filling the whole
  // pane as well meant writing most cells three times before any content.
  Style border{theme().border, theme().panel};
  frame.box(Rect{0, 0, p.rect.w, p.rect.h}, border);
  if (focused) frame.hline(1, 0, std::max(0, p.rect.w - 2), U'─',
                           Style{theme().border_focus, theme().panel});

  // The badge is placed first and the title takes what is left of the bar: a
  // long chat title is clipped before a state is hidden.
  int title_room = p.rect.w - 4;
  if (const Pane::Badge b = p.pane->badge(); !b.text.empty()) {
    const std::string bt = " " + b.text + " ";
    const int bw = text::str_width(bt);
    if (bw + 8 < p.rect.w) {
      const int bx = p.rect.w - 2 - bw;
      frame.text(bx, 0, bt, Style{b.color, theme().panel});
      title_room = bx - 3;
    }
  }
  std::string t = " " + p.pane->title() + " ";
  Style ts{focused ? theme().accent : theme().text, theme().panel, attr::kBold};
  frame.text_clipped(2, 0, t, ts, title_room);
}

// What is on screen, for a crash report: the tab, and on Sessions the chat.
const char* App::screen_crumb() {
  screen_crumb_ = tabs_[tab_].name + " tab";
  if (tab_ == 0 && selected_live_) screen_crumb_ += ", chat " + std::string(selected_live_->crumb());
  else if (tab_ == 0 && !selected_path_.empty()) screen_crumb_ += ", chat " + selected_path_;
  return screen_crumb_.c_str();
}

void App::render(Surface& s) {
  logs::Doing doing("drawing the", screen_crumb());
  viewport_w_ = s.width();
  viewport_h_ = s.height();
  // Compact on a narrow terminal, or as the setting says.
  const LayoutMode mode = layout_mode();
  const bool want = mode == LayoutMode::Compact || (mode == LayoutMode::Auto && s.width() < kCompactWidth);
  if (want != compact_) {
    compact_ = want;
    // Into compact: where the work is, the chat if one is open.
    screen_ = tab_ != 0 || selected_live_ || !selected_path_.empty() ? Screen::Main : Screen::Chats;
    layout_dirty_ = true;
    focus_ = 0;
  }
  reap_sessions();
  s.clear(Style{theme().text, theme().bg});

  // Chrome claims rows from the outside in, and gives them up on a screen too
  // small to spare them.
  tab_row_ = s.height() >= 6 ? 0 : -1;
  // The command line opens over the status bar rather than holding a row of
  // its own: idle, it was a permanent hint repeating the one in the tab bar.
  cmd_row_ = s.height() >= 4 ? s.height() - 1 : -1;
  const int top = tab_row_ >= 0 ? 1 : 0;
  const int bottom = 1;  // status bar, which the command line takes over
  Rect body{0, top, s.width(), std::max(1, s.height() - top - bottom)};
  placed_ = root_->place(body, &dividers_);
  if (focus_ >= placed_.size()) focus_ = 0;

  // A freshly spawned agent takes focus so you can type into its prompt at
  // once. Deferred to here because placed_ does not exist until now.
  if (focus_after_build_) {
    for (const auto& pl : placed_)
      if (pl.pane->session() == focus_after_build_) { focus_ = pl.index; break; }
    focus_after_build_ = nullptr;
  }
  // A chat opened from a search: the main area, placed after the sidebar.
  if (focus_chat_after_build_ && !placed_.empty()) {
    focus_ = placed_.back().index;
    focus_chat_after_build_ = false;
  }

  for (const auto& p : placed_) {
    bool focused = p.index == focus_;
    // Compact panes have no border: on a phone every column counts.
    if (!compact_) render_chrome(s, p, focused);
    Rect inner = inner_of(p);
    if (inner.empty()) continue;
    Painter pp(s, inner);
    p.pane->render(pp, focused);
    // The chat list can auto-select a row on its first paint. Finish that
    // transition before painting or accepting input for the main pane.
    if (layout_dirty_) { render(s); return; }
  }

  // The clipboard is queued from the composed surface: the text a selection
  // covers is only known once every pane has drawn it. Done before the status
  // line so "copied selection" is visible in this same frame.
  if (sel_copy_pending_) {
    sel_copy_pending_ = false;
    std::string t = selected_text(s);
    if (t.empty()) {
      set_status("nothing to copy");
    } else {
      copy_to_clipboard(std::move(t));
      set_status("copied selection");
    }
  }

  render_dividers(s);
  render_tabs(s);
  render_status(s);
  render_command(s);
  if (menu_) render_menu(s);
  if (prompt_) render_prompt(s);
  render_selection(s);
}

// Splits are draggable, but mouse mode 1002 reports motion only while a button
// is held, so there is no hover state to reveal them with. A permanent handle
// at each seam is the affordance instead.
void App::render_dividers(Surface& s) {
  for (const auto& d : dividers_) {
    const bool horiz = d.dir == SplitDir::Horizontal;
    const bool active = drag_ && drag_->node == d.owner && drag_->index == d.index;
    // Keep the grip solid in both states; dashed box characters make the idle
    // handle look broken. Colour alone indicates an active drag.
    Style st{active ? theme().border_focus : theme().dim, theme().panel,
             attr::kNone};

    // A small centred grip, not a line across the seam: a full-width bar reads
    // as content, especially next to an agent's startup screen.
    if (horiz) {
      // One cell: a stack of heavy bars beside a pane border reads as the
      // border glitching, not as something to grab.
      Painter p(s, Rect{d.hit.x + d.hit.w / 2, d.hit.y + d.hit.h / 2, 1, 1});
      p.put(0, 0, U'┃', st);
    } else {
      // Only the upper row of the seam: the lower one is the next pane's title
      // bar, and a handle there would eat the title text.
      const int cx = d.hit.x + d.hit.w / 2;
      for (int dx = -1; dx <= 1; dx++) {
        Painter p(s, Rect{cx + dx, d.hit.y, 1, 1});
        p.put(0, 0, U'━', st);
      }
    }
  }
}

void App::render_tabs(Surface& s) {
  tab_hit_.clear();
  back_hit_ = menu_hit_ = Rect{};
  if (tab_row_ < 0) return;
  if (compact_) return render_compact_bar(s);
  Painter p(s, Rect{0, tab_row_, s.width(), 1});
  p.clear(Style{theme().dim, theme().bg});

  int x = p.text(1, 0, "mico", Style{theme().accent, theme().bg, attr::kBold}) + 4;
  // The tabs head the column they switch; the sidebar is not theirs.
  if (placed_.size() >= 2) x = std::max(x, placed_.back().rect.x + 1);
  for (size_t i = 0; i < tabs_.size(); i++) {
    const bool on = i == tab_;
    const std::string label = " " + tabs_[i].name + " ";
    const int wide = text::str_width(label);
    Style st{on ? theme().text : theme().dim, on ? theme().panel : theme().bg,
             on ? attr::kBold : attr::kNone};
    p.fill(Rect{x, 0, wide, 1}, st);
    p.text(x, 0, label, st);
    tab_hit_.push_back(Rect{x, tab_row_, wide, 1});
    x += wide + 1;
  }

  // A hint at the right edge, where there is room for it.
  const char* hint = "F1 or : for commands";
  const int hw = text::str_width(hint);
  if (s.width() - hw - 2 > x) p.text(s.width() - hw - 1, 0, hint, Style{theme().dim, theme().bg});
}

// Compact's top row: back, where you are, and the menu. Tap targets, so wide.
void App::render_compact_bar(Surface& s) {
  const Theme& th = theme();
  Painter p(s, Rect{0, tab_row_, s.width(), 1});
  p.clear(Style{th.text, th.panel});
  const int W = s.width();
  // The menu, at the right edge: what the tab strip and F-keys did.
  const std::string menu = " \xE2\x89\xA1 ";  // ≡
  p.text(W - 3, 0, menu, Style{th.accent, th.panel, attr::kBold});
  menu_hit_ = Rect{W - 5, tab_row_, 5, 1};
  int x = 0;
  if (screen_ != Screen::Folders) {
    const std::string back = screen_ == Screen::Chats ? " \xE2\x80\xB9 Folders " : " \xE2\x80\xB9 Chats ";  // ‹
    x = p.text(0, 0, back, Style{th.accent, th.panel, attr::kBold});
    back_hit_ = Rect{0, tab_row_, x + 1, 1};
    p.put(x, 0, U'│', Style{th.border, th.panel});
    x += 2;
  } else {
    x = 1 + p.text(1, 0, "mico", Style{th.accent, th.panel, attr::kBold}) + 2;
  }
  // Where you are.
  std::string here;
  const Project* pr = current_project();
  if (screen_ == Screen::Folders) here = "Folders";
  else if (screen_ == Screen::Chats) here = pr ? (pr->name.empty() ? pr->path : pr->name) : "Chats";
  else if (tab_ != 0) here = tabs_[tab_].name;
  else if (selected_live_) here = session_title(*selected_live_);
  else if (const SessionRef* sr = current_session()) here = sr->title.empty() ? agent_label(sr->agent) + " chat" : sr->title;
  else here = "Sessions";
  p.text_clipped(x, 0, text::oneline(here, 200), Style{th.text, th.panel, attr::kBold}, std::max(0, W - x - 5));
}

// Compact's menu: the tabs, and what the function keys and right-click do.
void App::open_compact_menu() {
  std::vector<MenuItem> items = {
      MenuItem{"Go to a chat\xE2\x80\xA6", "go"},
      MenuItem{"New agent\xE2\x80\xA6", "new"},
      MenuItem{"Raw terminal \xE2\x86\x94 chat view", "view", tab_ == 0 && screen_ == Screen::Main},
      MenuItem{"This pane's menu\xE2\x80\xA6", "panemenu"},
      MenuItem::sep(),
  };
  for (size_t i = 0; i < tabs_.size(); i++) {
    MenuItem it{tabs_[i].name, "tab " + std::to_string(i)};
    it.checked = i == tab_ && screen_ == Screen::Main;
    items.push_back(std::move(it));
  }
  items.push_back(MenuItem::sep());
  items.push_back(MenuItem{"All commands\xE2\x80\xA6", "help"});
  items.push_back(MenuItem{"Detach (agents keep running)", "detach"});
  open_menu(nullptr, Point{std::max(0, viewport_w_ - 2), tab_row_ + 1}, std::move(items), "mico");
}

void App::render_command(Surface& s) {
  const int y = cmd_row_;
  if (y < 0) return;
  Painter p(s, Rect{0, y, s.width(), 1});
  if (!cmd_active_ && cmd_text_.empty()) {
    // Idle, only the colon stays: where to click, without a sentence saying so.
    p.put(0, 0, U':', Style{theme().dim, theme().panel, attr::kBold});
    return;
  }
  const Color bg = theme().menu_bg;
  p.clear(Style{theme().text, bg});
  p.put(0, 0, U':', Style{cmd_active_ ? theme().accent : theme().dim, bg, attr::kBold});
  int used = p.text_clipped(2, 0, cmd_text_, Style{theme().text, bg}, s.width() - 3);
  if (cmd_active_) p.put(2 + used, 0, U'▏', Style{theme().accent, bg});
}

// One place that knows every command, so :help cannot drift from what works.
namespace {
struct Command {
  const char* name;
  const char* help;
};
constexpr Command kCommands[] = {
    {"help", "show this list"},
    {"go", "jump to any chat: go [name]  (Ctrl+K, F12)"},
    {"new", "start an agent — :new, or :new <command>"},
    {"folder", "track another folder"},
    {"outline", "go to a message, edit or failure in this chat (Ctrl+G)"},
    {"fork", "fork the selected chat into a new one"},
    {"resume", "resume the selected chat"},
    {"density", "minimal | normal | full"},
    {"theme", "the colours: theme <name>, or theme to list them"},
    {"settings", "open the Settings tab: rendering, theme, agents"},
    {"select", "selection mode, so the terminal can copy"},
    {"usage", "show the usage tab"},
    {"search", "search every chat: search <text>"},
    {"tools", "show where the agents' time went"},
    {"diff", "show what the agents changed in files, and which chat did"},
    {"git", "show the repository: work trees, who works in each, changes and commits"},
    {"commit", "go to the chat that made a commit: commit <hash>"},
    {"blame", "go to the chat that last changed a line: blame <file>:<line>"},
    {"charts", "tell agents they can draw charts: charts on|off"},
    {"mcp", "give agents mico's tools (plot) over MCP: mcp on|off"},
    {"web", "the web view, served by the daemon: web on [port] | off | tailscale | host <name> | new-token, or web to copy its address"},
    {"sessions", "show the sessions tab"},
    {"redraw", "repaint everything"},
    {"log", "show where the log file is"},
    {"rescan", "re-read sessions from disk"},
    {"close", "close the focused pane"},
    {"detach", "leave; agents keep running"},
    {"quit", "stop every agent and quit"},
};
}  // namespace

void App::run_command(std::string line) {
  // Trim, and tolerate a leading colon so pasting ":fork" works.
  size_t a = line.find_first_not_of(" \t:");
  if (a == std::string::npos) return;
  size_t b = line.find_last_not_of(" \t");
  line = line.substr(a, b - a + 1);

  const size_t sp = line.find(' ');
  const std::string cmd = line.substr(0, sp);
  const std::string arg = sp == std::string::npos ? "" : line.substr(sp + 1);

  const Project* pr = current_project();
  const std::string cwd = pr ? pr->path : ".";
  const SessionRef* sess = current_session();

  if (cmd == "help") {
    std::vector<MenuItem> items;
    for (const auto& c : kCommands) {
      MenuItem it{c.name, c.name};
      it.detail = c.help;
      items.push_back(std::move(it));
      if (std::string_view(c.name) != "outline") continue;
      // Each agent is a command of its own, starting it.
      for (const Adapter* a : all_adapters()) {
        MenuItem ai{std::string(a->id()), std::string(a->id())};
        ai.detail = "start " + std::string(a->id()) + " in the selected project";
        items.push_back(std::move(ai));
      }
    }
    open_menu(nullptr, Point{2, std::max(0, 4)}, std::move(items), "Commands");
    if (menu_) {
      // A palette is for typing into: the field shows from the start and
      // Enter runs the first command without a key to reach it.
      Picker::Options& o = menu_->picker.options();
      o.show_query = true;
      o.footer = true;
      menu_->picker.set_cursor(0);
    }
    return;
  }
  if (cmd == "go") {
    open_switcher();
    if (menu_ && !arg.empty()) {
      menu_->picker.set_query(arg);
    }
    return;
  }
  if (cmd == "outline") {
    // The chat on screen: the focused pane if it shows one, else whichever
    // pane does. A pane with no chat ignores the action.
    close_menu();
    if (focus_ < placed_.size()) placed_[focus_].pane->on_action("outline");
    for (size_t i = 0; i < placed_.size() && !menu_; i++) placed_[i].pane->on_action("outline");
    return;
  }
  if (cmd == "folder") {
    pick_folder();
    return;
  }
  if (cmd == "new") {
    open_new_agent(arg);
    return;
  }
  if (adapter_for(cmd)) {
    spawn_agent(cmd, arg.empty() ? selected_cwd() : arg);
    return;
  }
  if (cmd == "fork" || cmd == "resume") {
    if (cmd == "resume") {
      if (!open_selected_chat()) set_status("no resumable chat selected");
      return;
    }
    if (selected_live_) {
      spawn_continuation(selected_live_->agent(), selected_live_->session_id(),
                         selected_live_->cwd(), true);
      return;
    }
    if (!sess) { set_status("no chat selected"); return; }
    spawn_continuation(sess->agent, sess->id, sess->cwd, cmd == "fork");
    return;
  }
  if (cmd == "settings") {
    show_tab(6);
    return;
  }
  if (cmd == "theme") {
    std::string names;
    for (const auto& t : themes()) {
      if (arg == t.name) {
        RenderSettings rs = render_settings();
        rs.theme = t.name;
        set_render_settings(rs);
        set_status(std::string("theme: ") + t.name + " \xE2\x80\x94 " + t.detail);
        return;
      }
      names += (names.empty() ? "" : " \xC2\xB7 ") + std::string(t.name);
    }
    set_status((arg.empty() ? "theme " + render_settings().theme : "no theme \"" + arg + "\"") + "  (" + names + ")");
    return;
  }
  if (cmd == "density") {
    if (arg.empty() || arg[0] == 'n') filters_.density = Density::Normal;
    else if (arg[0] == 'm') filters_.density = Density::Minimal;
    else if (arg[0] == 'f') filters_.density = Density::Full;
    else { set_status("density: minimal | normal | full"); return; }
    set_status(std::string("density: ") + density_name(filters_.density));
    return;
  }
  if (cmd == "select") { toggle_selection(); return; }
  if (cmd == "usage") { show_tab(1); return; }
  if (cmd == "tools") { show_tab(3); return; }
  if (cmd == "diff") { show_tab(4); return; }
  if (cmd == "git") { show_tab(5); return; }
  if (cmd == "tab") { show_tab(size_t(std::atoi(arg.c_str()))); return; }
  if (cmd == "back") { compact_back(); return; }
  if (cmd == "view") {
    // Raw terminal ↔ chat view for the focused agent, as F2.
    if (focus_ < placed_.size() && placed_[focus_].pane->session()) placed_[focus_].pane->on_action("toggle_view");
    return;
  }
  if (cmd == "panemenu") {
    // The focused pane's menu, as F9 or a right-click.
    if (focus_ < placed_.size()) {
      const Rect r = inner_of(placed_[focus_]);
      open_menu(placed_[focus_].pane, Point{r.x + 1, r.y + 1}, placed_[focus_].pane->context_menu(Point{-1, -1}));
    }
    return;
  }
  if (cmd == "search") { open_search(arg); return; }
  if (cmd == "commit") {
    if (arg.size() < 4 || arg.find_first_not_of("0123456789abcdef") != std::string::npos) {
      set_status("commit <hash>: the commit, by its hash");
      return;
    }
    if (!open_commit(arg))
      set_status(ws_.activity().complete() ? "no chat mico knows made " + arg
                                           : "no chat found to have made " + arg + " yet; still reading transcripts");
    return;
  }
  if (cmd == "blame") {
    // "file:line" or "file line", the file from the selected folder.
    std::string file = arg;
    int line_no = 0;
    const size_t cut = arg.find_last_of(": ");
    if (cut != std::string::npos) {
      file = arg.substr(0, cut);
      line_no = std::atoi(arg.c_str() + cut + 1);
    }
    while (!file.empty() && file.back() == ' ') file.pop_back();
    if (file.empty() || line_no <= 0) {
      set_status("blame <file>:<line>: which chat last changed that line");
      return;
    }
    ws_.git().blame(selected_cwd(), file, line_no);
    set_status("asking git who last changed " + file + ":" + std::to_string(line_no) + "\xE2\x80\xA6");
    return;
  }
  if (cmd == "web") {
    if (arg == "tailscale") {
      // This machine's name on the tailnet, kept; the address with it is
      // copied. mico does not start `tailscale serve` itself: what a machine
      // offers to its network is for its user to say.
      const std::string name = tailscale_name();
      if (name.empty()) {
        set_status("tailscale: no name found (is it installed and up? `tailscale status`)");
        return;
      }
      set_web(true);
      set_web_host(name);
      copy_to_clipboard(web_remote_url());
      set_status("tailnet address copied. Now run: tailscale serve --bg " + std::to_string(web_port()) +
                 "  (never funnel)");
      return;
    }
    if (arg == "new-token") {
      if (!new_web_token()) {
        set_status("web view: no new token could be made; the old one stands");
        return;
      }
      if (web_enabled()) copy_to_clipboard(web_remote_url().empty() ? web_url() : web_remote_url());
      set_status("web view: a new token; browsers with the old one are let go" +
                 std::string(web_enabled() ? ", and the new address is on the clipboard" : ""));
      return;
    }
    if (arg == "host off" || arg == "host") {
      set_web_host({});
      set_status("web view: no other host answered to");
      return;
    }
    if (arg.starts_with("host ")) {
      set_web_host(arg.substr(5));
      if (web_host().empty()) set_status("web view: not a host name");
      else {
        set_web(true);
        copy_to_clipboard(web_remote_url());
        set_status("web view also answers to " + web_host() + " (over https); its address is on the clipboard");
      }
      return;
    }
    if (arg == "off") set_web(false);
    else if (arg == "on" || arg.starts_with("on ")) set_web(true, arg.size() > 3 ? std::atoi(arg.c_str() + 3) : 0);
    if (!web_enabled()) {
      set_status("web view off  (:web on serves it on 127.0.0.1:" + std::to_string(web_port()) + ")");
      return;
    }
    // The address carries the token: copied rather than only shown, so it
    // need not be typed, and it stays out of the screen's scrollback.
    copy_to_clipboard(web_remote_url().empty() ? web_url() : web_remote_url());
    set_status("web view on 127.0.0.1:" + std::to_string(web_port()) +
               (web_host().empty() ? "" : ", and " + web_host() + " over https") +
               " \xE2\x80\x94 its address (with the token) is on the clipboard");
    return;
  }
  if (cmd == "mcp") {
    if (arg == "on" || arg == "off") set_mcp_tools(arg == "on");
    set_status(std::string("mcp: mico's tools (plot) are ") + (mcp_tools_enabled() ? "" : "not ") +
               "given to agents" + (arg.empty() ? "  (:mcp on|off)" : " — applies to agents started from now"));
    return;
  }
  if (cmd == "charts") {
    if (arg == "on" || arg == "off") set_agent_hints(arg == "on");
    set_status(std::string("charts: agents are ") + (agent_hints_enabled() ? "" : "not ") +
               "told about them at launch" + (arg.empty() ? "  (:charts on|off)" : " — applies to agents started from now"));
    return;
  }
  if (cmd == "sessions") { show_tab(0); return; }
  if (cmd == "redraw") { force_redraw(); return; }
  if (cmd == "log") { set_status("log: " + logs::path()); return; }
  if (cmd == "rescan") { ws_.store().scan(); set_status("rescanned"); return; }
  if (cmd == "close") {
    if (focus_ < placed_.size())
      if (LiveSession* ls = placed_[focus_].pane->session()) close_session(ls);
    return;
  }
  if (cmd == "detach") { detach(); return; }
  if (cmd == "quit" || cmd == "kill") { shutdown(); return; }

  set_status("unknown command: " + cmd + "  (try :help)");
}

bool App::command_key(const KeyEvent& k) {
  if (!cmd_active_) return false;
  switch (k.key) {
    case Key::Escape: cmd_active_ = false; cmd_text_.clear(); return true;
    case Key::Enter: {
      std::string line = cmd_text_;
      cmd_text_.clear();
      cmd_active_ = false;
      if (!line.empty()) {
        cmd_history_.push_back(line);
        run_command(std::move(line));
      }
      return true;
    }
    case Key::Backspace: erase_back(cmd_text_, k.ctrl || k.alt); return true;
    case Key::Up:
      if (!cmd_history_.empty()) {
        if (cmd_hist_ < 0) cmd_hist_ = int(cmd_history_.size());
        if (cmd_hist_ > 0) cmd_text_ = cmd_history_[size_t(--cmd_hist_)];
      }
      return true;
    case Key::Down:
      if (cmd_hist_ >= 0 && cmd_hist_ + 1 < int(cmd_history_.size()))
        cmd_text_ = cmd_history_[size_t(++cmd_hist_)];
      else { cmd_hist_ = -1; cmd_text_.clear(); }
      return true;
    case Key::Char:
      if (k.ctrl && k.ch == 'u') { cmd_text_.clear(); return true; }
      if (k.ctrl && k.ch == 'w') { erase_back(cmd_text_, true); return true; }
      if (!k.ctrl && !k.alt) text::encode(k.ch, cmd_text_);
      return true;
    default: return true;  // modal while active
  }
}

void App::render_status(Surface& s) {
  int y = s.height() - 1;
  Painter bar(s, Rect{0, y, s.width(), 1});
  bar.clear(Style{theme().dim, theme().panel});

  std::string left = " ";

  // A message the user just caused outranks the standing counts, which is the
  // opposite of what fits-if-there-is-room gives you on a narrow terminal.
  if (!status_.empty() && tick_ - status_at_ < 5) {
    bar.text(text::str_width(left), 0, " " + status_,
             Style{theme().attention, theme().panel, attr::kBold});
    return;
  }
  status_.clear();

  // The sidebar's states, counted: see chat_state.
  int working = 0, attention = 0, unread = 0;
  for (const auto& ls : ws_.live()) {
    const int rank = chat_state(ls.get(), theme()).rank;
    if (rank == 0) attention++;
    else if (rank == 1) unread++;
    else if (rank == 2) working++;
  }
  char buf[192];
  if (ws_.live().empty()) {
    snprintf(buf, sizeof buf, " %zu folders · %zu chats · density: %s",
             ws_.store().projects().size(), ws_.store().session_count(), density_name(filters_.density));
  } else {
    // Only mention what is true: a bar that always reads "0 need you" trains
    // you to stop reading it.
    int n = snprintf(buf, sizeof buf, " %zu agent%s", ws_.live().size(),
                     ws_.live().size() == 1 ? "" : "s");
    if (working) n += snprintf(buf + n, sizeof buf - size_t(n), " · %d working", working);
    if (attention)
      n += snprintf(buf + n, sizeof buf - size_t(n), " · %d need%s you", attention,
                    attention == 1 ? "s" : "");
    if (unread)
      n += snprintf(buf + n, sizeof buf - size_t(n), " · %d new repl%s", unread, unread == 1 ? "y" : "ies");
    snprintf(buf + n, sizeof buf - size_t(n), " · density: %s", density_name(filters_.density));
  }
  int x = bar.text(text::str_width(left), 0, buf, Style{theme().text, theme().panel});

  std::string help = selection_
                         ? "SELECT — drag to copy with your terminal · F8 to resume"
                         : status_.empty()
                             ? "alt-1/2/3 panes · alt-x command"
                             : status_;
  if (selection_) {
    bar.fill(Rect{0, 0, s.width(), 1}, Style{theme().bg, theme().attention});
    bar.text(1, 0, help, Style{theme().bg, theme().attention, attr::kBold});
    return;
  }
  int hw = text::str_width(help);
  if (s.width() - hw - 1 > text::str_width(left) + x)
    bar.text(s.width() - hw - 1, 0, help, Style{theme().dim, theme().panel});
}

void App::render_menu(Surface& s) {
  Menu& m = *menu_;
  if (m.centered) {
    // A palette: the same place and width every time, so it reads as a
    // place to go rather than a popup at wherever the pointer was.
    const int w = std::min(s.width() - 2, std::clamp(m.picker.natural_width(), 64, 100));
    const int y = std::max(1, s.height() / 6);
    const int h = m.picker.rows(w, std::max(3, s.height() - y - 1));
    m.rect = Rect{(s.width() - w) / 2, y, w, h};
    m.picker.render(Painter(s, m.rect), theme());
    return;
  }
  if (m.above) {
    // On top of the control that opened it: bottom edge on the row above the
    // anchor, left edge on the anchor, pulled in when the screen ends.
    const int w = std::min(m.picker.natural_width(), s.width());
    const int h = m.picker.rows(w, std::max(3, m.at.y));
    m.rect = Rect{std::clamp(m.at.x, 0, std::max(0, s.width() - w)), std::max(0, m.at.y - h), w, h};
    m.picker.render(Painter(s, m.rect), theme());
    return;
  }
  const int w = std::min(m.picker.natural_width(), s.width());
  const int h = m.picker.rows(w, std::max(3, s.height()));
  // Keep the popup on screen when opened near an edge. It is placed again
  // every frame, since typing into it changes its height.
  m.rect = Rect{std::min(m.at.x, std::max(0, s.width() - w)),
                std::min(m.at.y, std::max(0, s.height() - h)), w, h};
  m.picker.render(Painter(s, m.rect), theme());
}

// ------------------------------------------------------------------- input

bool App::menu_mouse(const MouseEvent& e) {
  if (!menu_) return false;
  Menu& m = *menu_;
  if (!m.rect.contains(e.pos)) {
    if (e.kind == MouseKind::Press) close_menu();
    return true;  // clicks outside a popup dismiss it, they never fall through
  }
  const Picker::Result r = m.picker.on_mouse(e, Point{e.pos.x - m.rect.x, e.pos.y - m.rect.y});
  // Hovering changes nothing else, so nothing else would mark the frame.
  if (r == Picker::Result::Moved) mark_dirty();
  menu_result(r);
  return true;
}

bool App::divider_press(Point pt) {
  // dividers_ is deepest-first, so a nested seam beats the outer one it sits in.
  for (const auto& d : dividers_) {
    if (!d.hit.contains(pt)) continue;
    drag_ = Drag{d.owner, d.index, d.area, d.hit};
    d.owner->drag_divider(d.index, d.area, pt);
    return true;
  }
  return false;
}

void App::handle_mouse(const MouseEvent& m) {
  if (drag_) {
    if (m.kind == MouseKind::Release) { drag_.reset(); save_layout(); return; }
    if (m.kind == MouseKind::Drag || m.kind == MouseKind::Move) {
      drag_->node->drag_divider(drag_->index, drag_->area, m.pos);
      return;
    }
    // Any other event ends the drag rather than leaving it stuck.
    drag_.reset();
    save_layout();
  }

  if (mouse_capture_) {
    // A pane that took the mouse (a scrollbar drag) owns it; text selection
    // must not fight it.
    cancel_selection();
    for (const auto& p : placed_)
      if (p.pane == mouse_capture_) {
        const Rect in = inner_of(p);
        p.pane->on_mouse(m, Point{m.pos.x - in.x, m.pos.y - in.y});
        break;
      }
    if (m.kind == MouseKind::Release) mouse_capture_ = nullptr;
    return;
  }

  if (menu_) {
    if (m.kind == MouseKind::Press) cancel_selection();
    if (menu_mouse(m)) return;
  }

  if (m.kind == MouseKind::Press && m.button == MouseButton::Left) {
    // The tab strip and the command line sit outside the pane tree.
    if (tab_row_ >= 0 && m.pos.y == tab_row_) {
      if (compact_) {
        if (back_hit_.contains(m.pos)) compact_back();
        else if (menu_hit_.contains(m.pos)) open_compact_menu();
        return;
      }
      for (size_t i = 0; i < tab_hit_.size(); i++)
        if (tab_hit_[i].contains(m.pos)) { show_tab(i); return; }
      return;
    }
    if (cmd_row_ >= 0 && m.pos.y == cmd_row_ && (cmd_active_ || m.pos.x <= 1)) {
      open_command_line();
      return;
    }
    if (divider_press(m.pos)) return;
  }

  // Which pane is under the cursor, if any, for both forwarding and selection.
  const Node::Placed* hit = nullptr;
  for (const auto& p : placed_)
    if (p.rect.contains(m.pos)) { hit = &p; break; }

  if (m.kind == MouseKind::Press && m.button == MouseButton::Left) {
    // A fresh press drops the old highlight and arms a new selection.
    sel_has_ = false;
    sel_copy_pending_ = false;
    sel_dragging_ = true;
    sel_anchor_ = sel_cursor_ = m.pos;
    sel_area_ = hit ? inner_of(*hit) : Rect{m.pos.x, m.pos.y, 1, 1};
  } else if (sel_dragging_ && (m.kind == MouseKind::Drag || m.kind == MouseKind::Move)) {
    // Extend the range, clamped to the pane it began in.
    Point c = m.pos;
    c.x = std::clamp(c.x, sel_area_.x, sel_area_.x + std::max(0, sel_area_.w - 1));
    c.y = std::clamp(c.y, sel_area_.y, sel_area_.y + std::max(0, sel_area_.h - 1));
    if (c.x != sel_anchor_.x || c.y != sel_anchor_.y) {
      sel_cursor_ = c;
      sel_has_ = true;
      mark_dirty();
    }
  } else if (m.kind == MouseKind::Release && m.button == MouseButton::Left && sel_dragging_) {
    sel_dragging_ = false;
    if (sel_has_) {
      // Copy what was dragged out. The release is swallowed so the pane does
      // not also read the gesture as a click.
      sel_copy_pending_ = true;
      mark_dirty();
      return;
    }
  }

  if (!hit) return;

  const Rect hit_inner = inner_of(*hit);
  Point local{m.pos.x - hit_inner.x, m.pos.y - hit_inner.y};
  if (m.kind == MouseKind::Press) {
    focus_ = hit->index;
    if (m.button == MouseButton::Right) {
      cancel_selection();
      auto items = hit->pane->context_menu(local);
      open_menu(hit->pane, m.pos, std::move(items));
      return;
    }
  }
  hit->pane->on_mouse(m, local);
  // A click on a link: opened where the user's terminal is, which only the
  // loop serving that terminal knows.
  if (std::string url = hit->pane->take_url(); !url.empty()) {
    // A file opens in the editor, beside the chat, on the machine the file is
    // on: the daemon's. A web link opens where the user is.
    if (url.starts_with("file://")) open_in_editor(url);
    else open_url_ = std::move(url);
  }

  // A pane that grabbed the mouse or opened a menu owns it now.
  if (mouse_capture_ || menu_) cancel_selection();
}

void App::cancel_selection() {
  sel_dragging_ = false;
  sel_has_ = false;
  sel_copy_pending_ = false;
}

bool App::normalized_selection(Point& top, Point& bot) const {
  if (!sel_has_) return false;
  normalize_selection(sel_anchor_, sel_cursor_, sel_area_, top, bot);
  return true;
}

namespace {
// What copying a picture gives: an equation's LaTeX, a chart's title.
std::string equation_text(uint32_t id) {
  const math::Image* im = math::find(id);
  return im ? im->copy : std::string();
}
}  // namespace

std::string App::selected_text(const Surface& s) const {
  Point top{}, bot{};
  if (!normalized_selection(top, bot)) return {};
  return selection_text(s, top, bot, sel_area_, equation_text);
}

void App::render_selection(Surface& s) {
  Point top{}, bot{};
  if (!normalized_selection(top, bot)) return;
  for (int y = std::max(0, top.y); y <= bot.y && y < s.height(); y++) {
    const int x0 = (y == top.y) ? top.x : sel_area_.x;
    const int x1 = (y == bot.y) ? bot.x : sel_area_.x + sel_area_.w - 1;
    for (int x = std::max(0, x0); x <= x1 && x < s.width(); x++) {
      Cell& c = s.at(x, y);
      c.st.a |= attr::kReverse;
      if (c.width == 2 && x + 1 < s.width()) s.at(x + 1, y).st.a |= attr::kReverse;
    }
  }
}

void App::focus_next(int delta) {
  if (placed_.empty()) return;
  int n = int(placed_.size());
  focus_ = size_t(((int(focus_) + delta) % n + n) % n);
}

void App::handle_key(const KeyEvent& k) {
  // Ctrl+C over highlighted text copies it, as in any terminal. It must not
  // go on to interrupt the agent, or detach mico, with a selection showing.
  if (sel_has_ && k.is_ctrl('c')) {
    sel_copy_pending_ = true;
    mark_dirty();
    return;
  }
  // A key press ends a mouse selection. A copy queued by the release is left
  // alone so it still lands in this frame's clipboard flush.
  if (sel_has_ && !sel_copy_pending_) cancel_selection();
  if (prompt_ && prompt_key(k)) return;
  if (command_key(k)) return;
  if (menu_) {
    // Modal: every key is the menu's, whether it uses it or not.
    menu_result(menu_->picker.on_key(k));
    return;
  }

  // Function keys are global even over a raw pty pane, which swallows
  // everything else. Without them there would be no way back out of an agent.
  // Ctrl+L redraws everything, the usual terminal escape hatch for a screen
  // left dirty by something outside the program.
  if (k.is_ctrl('l')) { force_redraw(); return; }

  // Alt+1, 2, 3 (or Ctrl+1, 2, 3, where the terminal can send them): the
  // folders, the chats, the chat itself (where its message box takes the
  // keys). Alt+X: the command line. Global like the function keys, so they
  // reach past a message being typed and out of a raw pane.
  if (k.key == Key::Char && (k.alt != k.ctrl) && k.ch >= '1' && k.ch <= '3') {
    if (compact_) {
      compact_show(k.ch == '1' ? Screen::Folders : k.ch == '2' ? Screen::Chats : Screen::Main);
      return;
    }
    if (!placed_.empty()) {
      const size_t i = std::min(size_t(k.ch - '1'), placed_.size() - 1);
      focus_ = (k.ch == '3' ? placed_.back() : placed_[i]).index;
      mark_dirty();
      return;
    }
  }
  if (k.key == Key::Char && k.alt && !k.ctrl) {
    if (k.ch == 'x') { open_command_line(); return; }
  }

  switch (k.key) {
    case Key::F3: focus_next(1); return;
    case Key::F4: {
      const Project* p = current_project();
      spawn_agent("claude", p ? selected_cwd() : ".");
      return;
    }
    case Key::F5: {
      const Project* p = current_project();
      spawn_agent("codex", p ? selected_cwd() : ".");
      return;
    }
    case Key::F6: {
      const SessionRef* s = current_session();
      if (s) spawn_continuation(s->agent, s->id, s->cwd, true);
      return;
    }
    // Detach is global: it has to work even over a raw pane, which swallows
    // every other key.
    // Global, because a raw pty pane swallows every ordinary key.
    case Key::F1: open_command_line(); return;
    case Key::F7: open_new_agent(); return;
    // Global like the others, so a raw pane can jump away too.
    case Key::F12: open_switcher(); return;
    // Opens the focused pane's menu without a mouse: some terminals keep
    // right-click for themselves and never forward it.
    case Key::F9: {
      if (focus_ < placed_.size()) {
        const Rect& r = placed_[focus_].rect;
        Point at{r.x + 2, r.y + 2};
        // Off the pane: a keyboard menu has no pointer, so panes act on
        // their selection rather than on whatever sits at some fixed row.
        open_menu(placed_[focus_].pane, at, placed_[focus_].pane->context_menu(Point{-1, -1}));
      }
      return;
    }
    case Key::F8: toggle_selection(); return;
    case Key::F10: detach(); return;
    default: break;
  }

  const bool captured = focus_ < placed_.size() && placed_[focus_].pane->captures_keys();
  if (!captured) {
    if (k.is('q') || k.is_ctrl('c')) { detach(); return; }
    if (k.is('m')) { handle_key(KeyEvent{Key::F9}); return; }
    if (k.is(':')) { open_command_line(); return; }
    if (k.is_ctrl('k')) { open_switcher(); return; }
    if (k.key == Key::Tab) { focus_next(1); return; }
    if (k.key == Key::BackTab) { focus_next(-1); return; }
    if (k.is('/') || k.is_ctrl('f')) { open_search({}); return; }
    if (k.is('[')) { show_tab((tab_ + tabs_.size() - 1) % tabs_.size()); return; }
    if (k.is(']')) { show_tab((tab_ + 1) % tabs_.size()); return; }
    if (k.is('d')) {
      filters_.density = Density(((int)filters_.density + 1) % 3);
      notify_state_changed();
      return;
    }
  }

  if (focus_ < placed_.size()) {
    const bool used = placed_[focus_].pane->on_key(k);
    // Compact, on the folders or the chats: ← or Backspace goes back, and
    // Esc does when the list had no use for it.
    if (compact_ && screen_ != Screen::Main &&
        (k.key == Key::Left || k.key == Key::Backspace || (k.key == Key::Escape && !used)))
      compact_back();
  }
}

void App::handle(const InputEvent& e) {
  logs::Doing doing("handling input on the", screen_crumb());
  switch (e.type) {
    case InputEvent::Type::Key: handle_key(e.key); break;
    case InputEvent::Type::Mouse: handle_mouse(e.mouse); break;
    case InputEvent::Type::Paste: handle_paste(e.paste); break;
    case InputEvent::Type::Focus:
      term_focused_ = e.focus_in;
      term_focus_known_ = true;
      break;
    default: break;
  }
}

// A bracketed paste is one event, not a run of keystrokes, so it is routed
// deliberately: mico's own modal fields take it as literal text, and the
// focused pane decides whether it wants it (the chat prompt inserts it, a raw
// pty view forwards it re-wrapped). It is never replayed as key events, which
// would turn a pasted "a" into a shortcut.
void App::handle_paste(const std::string& t) {
  if (t.empty()) return;
  // The popup is modal for input, paste included: it narrows the list.
  if (menu_) {
    menu_->picker.on_paste(t);
    return;
  }
  if (prompt_) {
    for (char c : t) prompt_->text.push_back(c == '\n' || c == '\r' ? ' ' : c);
    return;
  }
  if (cmd_active_) {
    for (char c : t)
      if (c != '\n' && c != '\r') cmd_text_.push_back(c);
    return;
  }
  if (focus_ < placed_.size()) placed_[focus_].pane->on_paste(t);
}

// -------------------------------------------------------------------- loops

int App::run() {
  if (!term_.start(theme().bg)) {
    fprintf(stderr, "mico: stdin/stdout is not a terminal (try --dump)\n");
    return 1;
  }
  Color shown_bg = theme().bg;

  Surface s;
  std::vector<pollfd> fds;
  math::KittyHeld images_sent;
  std::vector<uint32_t> evicted;
  while (running_) {
    bool dirty = service();
    save_view_if_changed();

    // A new theme: the terminal is given its background and repainted.
    if (const Color bg = theme().bg; bg != shown_bg) {
      shown_bg = bg;
      term_.queue(tty::background_seq(bg));
      force_redraw();
      dirty = true;
    }

    // Equations become images when this terminal can show them.
    math::Config mc = math::config();
    mc.enabled = term_.caps().any();
    mc.kitty = mc.enabled && term_.caps().kitty;
    if (term_.caps().cell_w > 0) {
      mc.cell_w = term_.caps().cell_w;
      mc.cell_h = term_.caps().cell_h;
    }
    mc.fg = theme().math;
    const uint64_t gen = math::generation();
    math::configure(mc);
    if (math::generation() != gen) dirty = true;
    const GfxCaps& tc = term_.caps();
    term_.set_images(!mc.enabled ? ImageMode::None : tc.kitty ? ImageMode::Kitty : ImageMode::Sixel,
                     math::sixel_pass);

    const bool resized = s.width() != term_.width() || s.height() != term_.height();
    s.resize(term_.width(), term_.height());
    if (dirty || redraw_ || resized) render(s);
    if (take_redraw()) term_.invalidate();
    term_.set_mouse(wants_mouse(), wants_motion());
    if (std::string clip = take_clipboard(); !clip.empty()) term_.queue(clipboard_seq(clip));
    // Local mode: the terminal is on this machine, so the link opens here.
    if (std::string url = take_open_url(); !url.empty()) open_url(url);
    for (const Notice& n : take_notices())
      if (announce(n, term_focus_known_, term_focused_)) term_.queue(notice_seq(n, term_.caps().notify));
    // In selection mode the screen is drawn once and then held still, so the
    // terminal's own selection survives.
    if (!selection_ || !sel_drawn_) {
      std::string pictures;
      evicted.clear();
      math::take_evicted(evicted);
      math::free_images(evicted, images_sent, pictures, tc.tmux);
      if (mc.enabled && tc.kitty) math::send_images(s, images_sent, pictures, tc.tmux);
      if (!pictures.empty()) term_.queue(pictures);
      term_.present(s);
      sel_drawn_ = true;
    }

    // Watch stdin and every agent at once, so a busy agent never blocks input
    // and an idle UI costs nothing.
    fds.clear();
    fds.push_back(pollfd{term_.input_fd(), POLLIN, 0});
    for (auto& live : ws_.live())
      fds.push_back(pollfd{live->pty().fd(), POLLIN, 0});
    if (const int x = x11clip::fd(); x >= 0) fds.push_back(pollfd{x, POLLIN, 0});

    int timeout = idle_timeout_ms();
    if (term_.pending_escape()) timeout = 25;
    int rc = ::poll(fds.data(), fds.size(), timeout);

    const short st_ev = rc > 0 ? fds[0].revents : 0;
    if (st_ev & (POLLERR | POLLNVAL)) break;

    if (st_ev & (POLLIN | POLLHUP)) {
      if (term_.ingest() == 0) {
        if (++stdin_silent_ > 3) break;  // stdin is gone; there is no user left
      } else {
        stdin_silent_ = 0;
      }
      // Local mode owns the agents, so there is nothing to detach from and
      // leaving means stopping.
      while (auto e = term_.next_event())
        if (feed(*e) != AppAction::None) running_ = false;
    } else {
      // Resize arrives as a signal, not readable input.
      while (auto e = term_.next_event())
        if (feed(*e) != AppAction::None) running_ = false;
      // Nothing followed a lone ESC within the timeout: it was the key.
      if (rc == 0 && term_.pending_escape())
        if (auto e = term_.flush_escape())
          if (feed(*e) != AppAction::None) running_ = false;
    }

  }

  ws_.terminate_all();
  term_.stop();
  return 0;
}

int App::dump(int w, int h) {
  Surface s;
  s.resize(w, h);
  render(s);

  std::string out;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const Cell& c = s.at(x, y);
      if (c.width == 0) continue;
      text::encode(c.cp ? c.cp : U' ', out);
    }
    out += '\n';
  }
  fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

}  // namespace mico
