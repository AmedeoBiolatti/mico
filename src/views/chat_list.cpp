#include <algorithm>
#include <chrono>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "adapters/adapters.h"
#include "core/session.h"
#include "core/store.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/list.h"
#include "views/views.h"

namespace mico {
namespace {

// One row of the unified list: a running session, or a stored transcript that
// has no live session.
struct Row {
  LiveSession* live = nullptr;  // null -> stored
  const SessionRef* stored = nullptr;
  std::string name;
  std::string key;  // agent\tid, stable across rebuilds and used for marks
  // When you last used it: a live chat's last message or (re)start, a stored
  // one's last write. The list is newest first.
  int64_t updated = 0;
  bool archived = false;
  // Who the row is while it is laid out: its key, or for a new chat that has
  // no id yet, its session.
  std::string id() const { return key.empty() ? "live:" + std::to_string(uintptr_t(live)) : key; }
};

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

enum State { Running, Waiting, Active, Old };

State state_of(const Row& r) {
  if (!r.live) return Old;
  if (r.live->exited()) return Old;
  if (r.live->needs_input()) return Waiting;
  if (r.live->busy()) return Running;
  if (r.live->unseen()) return Waiting;
  return Active;
}

std::string agent_of(const Row& r) { return r.live ? r.live->agent() : r.stored->agent; }

std::string key_of(const std::string& agent, const std::string& id) {
  return id.empty() ? std::string() : agent + "\t" + id;
}

void split_key(const std::string& key, std::string* agent, std::string* id) {
  const size_t t = key.find('\t');
  if (t == std::string::npos) return;
  *agent = key.substr(0, t);
  *id = key.substr(t + 1);
}

// Every chat for the selected project: live sessions in it, plus stored
// transcripts that are not already open live. Archived chats are hidden unless
// the list is showing them, and never dropped from the store.
void build_rows(App* app, std::vector<Row>& out, bool show_archived, bool held) {
  out.clear();
  const Project* pr = app->current_project();
  if (!pr) return;

  Store& store = app->store();
  // A sub-project narrows the list to its own chats.
  const SubProject* sub = app->current_sub();
  // The project's chats run in its folder, or in a sub-project folder of it.
  const auto in_project = [&](const std::string& cwd) {
    if (cwd == pr->path) return true;
    for (const auto& sp : pr->subs)
      if (sp.path != pr->path && (cwd == sp.path || cwd.starts_with(sp.path + "/"))) return true;
    return false;
  };
  std::vector<std::string> live_ids;
  for (LiveSession* s : app->live_sessions()) {
    if (!in_project(s->cwd())) continue;
    if (sub && app->live_sub(*s) != sub->name) {
      live_ids.push_back(key_of(s->agent(), s->session_id()));  // nor its stored row
      continue;
    }
    // A running agent is never hidden by archive: the pane still exists, and
    // hiding the row would orphan it. Archiving stops an idle agent for that
    // reason, and one that has exited is as good as a stored transcript.
    const bool archived = store.archived(s->agent(), s->session_id());
    if (archived && !show_archived && s->exited()) {
      live_ids.push_back(key_of(s->agent(), s->session_id()));  // hide its stored row too
      continue;
    }
    Row r;
    r.live = s;
    r.key = key_of(s->agent(), s->session_id());
    r.archived = archived;
    r.name = app->session_title(*s);
    // Not its state, which changes with every turn: rows would move under
    // the pointer all the time.
    r.updated = s->used_at();
    live_ids.push_back(key_of(s->agent(), s->session_id()));
    out.push_back(std::move(r));
  }
  for (const auto& sr : pr->sessions) {
    if (std::find(live_ids.begin(), live_ids.end(), key_of(sr.agent, sr.id)) != live_ids.end()) continue;
    if (sub && sr.sub != sub->name) continue;
    const bool archived = store.archived(sr.agent, sr.id);
    if (archived && !show_archived) continue;
    Row r;
    r.stored = &sr;
    r.key = key_of(sr.agent, sr.id);
    r.archived = archived;
    r.name = sr.title.empty() ? sr.id : text::oneline(sr.title, 0);
    if (const std::string* n = store.custom_name(sr.agent, sr.id)) r.name = *n;
    r.updated = sr.mtime;
    out.push_back(std::move(r));
  }
  std::stable_sort(out.begin(), out.end(), [](const Row& a, const Row& b) { return a.updated > b.updated; });

  // While the list is in use its rows stay where they were; a chat that was
  // not there before, one just started, goes on top.
  App::ChatOrder& order = app->chat_order();
  const std::string scope =
      pr->path + "\n" + (sub ? sub->name : std::string()) + "\n" + (show_archived ? "a" : "");
  if (held && order.scope == scope) {
    std::unordered_map<std::string, size_t> was;
    for (size_t i = 0; i < order.ids.size(); i++) was.emplace(order.ids[i], i);
    std::vector<size_t> place(out.size());
    for (size_t i = 0; i < out.size(); i++) {
      auto it = was.find(out[i].id());
      place[i] = it == was.end() ? 0 : it->second + 1;
    }
    std::vector<size_t> idx(out.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return place[a] < place[b]; });
    std::vector<Row> held_rows;
    held_rows.reserve(out.size());
    for (size_t i : idx) held_rows.push_back(std::move(out[i]));
    out = std::move(held_rows);
  }
  order.scope = scope;
  order.ids.clear();
  for (const Row& r : out) order.ids.push_back(r.id());
}

class ChatList final : public Pane {
 public:
  std::string title() const override {
    const Project* p = app_->current_project();
    std::string t = p ? "Chats · " + p->name : "Chats";
    if (const SubProject* sp = app_->current_sub()) t += " \xE2\x80\xBA " + sp->name;  // ›
    if (marked_shown_ > 0) t += " · " + std::to_string(marked_shown_) + " selected";
    else if (show_archived_) t += " · archived";
    return t;
  }

  void render(Painter& whole, bool focused) override {
    const Theme& th = app_->theme();
    whole.clear(Style{th.text, th.panel});
    // Off the Sessions tab the list filters, and "All chats" heads it.
    head_ = app_->filtering() && whole.height() > kFilterHeaderH + 3 ? kFilterHeaderH : 0;
    if (head_) draw_filter_header(whole, th, "All chats", !app_->chat_filter(), focused);
    Painter p = whole.sub(Rect{0, head_, whole.width(), whole.height() - head_});
    // In use: focused, or touched a moment ago (a click moves the focus to
    // the chat it opens, and the pointer is still over the list).
    build_rows(app_, rows_, show_archived_, focused || now_ms() < app_->chat_order().held_until_ms);
    prune_marks();
    const int add = int(rows_.size());  // "+ new chat" button

    // On first paint, or after the selection was cleared, open whatever sits
    // at the top of the list so the main area is never blank.
    if (!app_->selected_live() && app_->selected_path().empty() && !rows_.empty())
      choose(0);

    // Keep the visible selection pointed at whatever the app has selected.
    resync();
    list_.list.sel = sel_;
    list_.fit(add, p.height());
    sel_ = list_.list.sel;
    p.clear(Style{th.text, th.panel});

    // Filtering the whole folder, no one chat is lit.
    const bool lit = !head_ || app_->chat_filter();
    for (int i = list_.list.top, y = 0; i < add && y < list_.list_h(); i++, y += TallList::kItemH)
      draw_item(p, rows_[size_t(i)], y, lit && i == sel_, focused);
    draw_list_button(p, th, list_, "+ New chat", sel_ == add, focused);
  }

  // Two lines: the title, then state · agent with the time on the right.
  void draw_item(Painter& p, const Row& r, int y, bool cursor, bool focused) {
    const Theme& th = app_->theme();
    const bool marked = !r.key.empty() && marked_.count(r.key) != 0;
    Style base{th.text, (cursor || marked) ? (focused ? th.sel_bg : th.sel_inactive) : th.panel,
               cursor ? attr::kBold : attr::kNone};
    const Style meta{th.dim, base.bg};
    p.fill(Rect{0, y, p.width(), TallList::kItemH}, base);
    for (int dy = 0; dy < TallList::kItemH; dy++)
      p.put(0, y + dy, cursor ? U'▌' : U' ', Style{th.accent, base.bg});

    const State st = state_of(r);
    const ChatState cs = chat_state(r.live, th);
    Color c = r.archived ? th.dim : cs.color;
    p.put(1, y, cs.glyph, Style{c, base.bg, attr::kBold});

    // First line: the title, and a check on a gathered row.
    int right = p.width();
    if (marked) {
      right -= 2;
      p.put(right, y, U'✓', Style{th.accent, base.bg, attr::kBold});
    }
    p.text_clipped(3, y, r.name, base, std::max(0, right - 4));

    // Second line: what it is doing and which agent, then when a stored chat
    // last moved. A live one is happening now, so it has no time.
    const std::string word = r.archived ? "archived" : r.live && r.live->exited() ? "stopped" : cs.word;
    const std::string when = st == Old && !r.live ? rel_time(r.updated) : "";
    const int ww = text::str_width(when);
    int x = 3;
    const int limit = p.width() - (ww ? ww + 2 : 1);
    x += p.text_clipped(x, y + 1, word, Style{c, base.bg}, std::max(0, limit - x));
    const std::string agent = " · " + agent_label(agent_of(r));
    if (x + text::str_width(agent) <= limit) p.text(x, y + 1, agent, meta);
    if (ww && p.width() - ww > x) p.text(p.width() - ww, y + 1, when, meta);
  }

  // Holds the order for a few seconds after the list was last touched.
  void hold() { app_->chat_order().held_until_ms = now_ms() + kHoldMs; }
  static constexpr int64_t kHoldMs = 4000;

  bool on_key(const KeyEvent& k) override {
    hold();
    const int add = int(rows_.size());

    // Multi-select: Space gathers a row and steps down (as in a file manager);
    // Shift+arrows extend a run; Ctrl+A takes every named row; Esc lets go.
    if (k.is(' ')) { toggle_mark(sel_); if (sel_ + 1 <= add) { sel_++; list_.list.sel = sel_; } return true; }
    if (k.is_ctrl('a')) { mark_all(); return true; }
    if (k.key == Key::Escape) { marked_.clear(); anchor_ = -1; return true; }
    if (k.key == Key::Down && k.shift) { extend_to(sel_ + 1); return true; }
    if (k.key == Key::Up && k.shift) { extend_to(sel_ - 1); return true; }
    if (k.is('a')) { show_archived_ = !show_archived_; anchor_ = -1; return true; }

    // Filtering: "All chats" sits above the first chat; Enter opens the chat
    // in Sessions, as the list does there.
    if (head_ && !k.shift && !k.ctrl && !k.alt) {
      if (!app_->chat_filter() && k.key == Key::Down && sel_ < add) {
        narrow_to(sel_);
        return true;
      }
      if (app_->chat_filter() && k.key == Key::Up && sel_ == 0) {
        app_->set_chat_filter(false);
        return true;
      }
    }
    if (k.key == Key::Enter) {
      activate(sel_);
      if (head_ && sel_ < add) app_->open_tab(0);
      return true;
    }
    if (!list_.on_key(k)) return false;
    if (head_ && list_.list.sel < add) narrow_to(list_.list.sel);
    else choose(list_.list.sel);
    return true;
  }

  // Off the Sessions tab, picking a chat narrows the tab to it rather than
  // opening it: nothing is started for a look at its usage.
  void narrow_to(int i) {
    choose(i);
    app_->set_all_folders(false);
    app_->set_chat_filter(true);
  }

  bool on_mouse(const MouseEvent& m, Point at) override {
    hold();
    if (head_ && at.y >= 0 && at.y < head_) {
      if (m.kind == MouseKind::Press && m.button == MouseButton::Left) app_->set_chat_filter(false);
      return true;
    }
    const Point local{at.x, at.y - head_};
    if (local.y < 0 || local.y >= list_.height) return false;
    const int hit = list_.on_mouse(m, local);
    if (hit < 0) return true;
    if (m.kind == MouseKind::Press) {
      if (m.ctrl) {
        toggle_mark(hit);
        sel_ = hit;
        return true;
      }
      if (m.shift) { extend_to(hit); return true; }
      if (head_ && hit < int(rows_.size())) narrow_to(hit);
      else activate(hit);
    }
    return true;
  }

  std::vector<MenuItem> context_menu(Point local) override {
    // Long enough to read the menu and pick from it.
    app_->chat_order().held_until_ms = now_ms() + 30000;
    // A right-click acts on the row under the pointer, not on the selection:
    // selecting a chat opens it and resumes its agent, so "click it, then
    // archive it" used to start the agent it was meant to put away. A menu
    // opened from the keyboard has no pointer (y < 0) and takes the selection.
    const int at = local.y < 0 ? sel_ : local.y < head_ ? -1 : list_.index_at(local.y - head_);
    const bool on_row = at >= 0 && at < int(rows_.size());
    const Row* r = on_row ? &rows_[size_t(at)] : nullptr;
    menu_live_ = r ? r->live : nullptr;
    menu_path_ = r && r->stored ? r->stored->path : std::string();
    const bool stored = r && r->stored;
    const bool live = r && r->live && !r->live->exited();
    const bool named = on_row && !r->key.empty();

    if (marked_.size() > 1) {
      bool all_archived = true;
      for (const auto& key : marked_) {
        std::string a, i;
        split_key(key, &a, &i);
        if (!app_->store().archived(a, i)) { all_archived = false; break; }
      }
      const std::string n = std::to_string(marked_.size());
      return {
          MenuItem{n + " chats selected", "noop", false},
          MenuItem::sep(),
          MenuItem{all_archived ? "Unarchive " + n : "Archive " + n,
                   all_archived ? "unarchive_selected" : "archive_selected"},
          MenuItem{"Clear selection", "clear_sel"},
      };
    }

    std::vector<MenuItem> items;
    for (const Adapter* ag : all_adapters())
      items.push_back(MenuItem{"New " + std::string(ag->id()) + " here", "new:" + std::string(ag->id())});
    items.insert(items.end(), {
        MenuItem::sep(),
        MenuItem{"Open", "open", on_row},
        MenuItem{"Select (space)", "mark", named},
        MenuItem{"Rename…", "rename", named},
        MenuItem{"Move to sub-project…", "movesub_menu",
                 named && app_->current_project() && !app_->current_project()->subs.empty()},
        MenuItem{r && r->archived ? "Unarchive" : "Archive", r && r->archived ? "unarchive" : "archive",
                 named},
        MenuItem::sep(),
        MenuItem{"Resume chat", "resume", bool(stored || (r && r->live && r->live->exited() && named))},
        MenuItem{"Fork into a new run", "fork", bool(stored || live)},
        MenuItem{show_archived_ ? "Hide archived" : "Show archived", "toggle_archived"},
        MenuItem::sep(),
        MenuItem{"Stop this agent", "stop", live},
    });
    return items;
  }

  void on_action(const std::string& a) override {
    hold();
    const std::string cwd = app_->selected_cwd();
    const int at = menu_index();
    const Row* r = at >= 0 ? &rows_[size_t(at)] : nullptr;

    if (a.starts_with("new:")) { app_->spawn_agent(a.substr(4), cwd); return; }
    if (a == "open" && r) { activate(at); return; }
    if (a == "toggle_archived") { show_archived_ = !show_archived_; return; }
    if (a == "clear_sel") { marked_.clear(); anchor_ = -1; return; }
    if (a == "mark" && r) { toggle_mark(at); return; }
    if (a == "rename" && r && !r->key.empty()) {
      app_->ask("Rename chat", "rename", r->name, r->key);
      return;
    }
    if (a == "movesub_menu" && r && !r->key.empty()) {
      const Project* pr = app_->current_project();
      if (!pr) return;
      // Which sub-project it is in now, so the picker opens on it.
      std::string agent, id;
      split_key(r->key, &agent, &id);
      const std::string now = r->live ? app_->live_sub(*r->live) : r->stored ? r->stored->sub : std::string();
      std::vector<PickItem> items;
      int cursor = 0;
      for (const auto& sp : pr->subs) {
        PickItem it;
        it.label = sp.name;
        it.detail = sp.path == pr->path ? "same folder" : "." + sp.path.substr(pr->path.size());
        it.id = "movesub:" + r->key + "\n" + sp.name;
        it.checked = sp.name == now;
        if (it.checked) cursor = int(items.size());
        items.push_back(std::move(it));
      }
      PickItem none;
      none.label = "No sub-project";
      none.detail = "just " + pr->name;
      none.id = "movesub:" + r->key + "\n";
      none.checked = now.empty();
      if (none.checked) cursor = int(items.size());
      items.push_back(std::move(none));
      app_->open_picker(this, "Move \xE2\x80\x9C" + r->name + "\xE2\x80\x9D to", std::move(items), cursor);
      return;
    }
    if (a.starts_with("movesub:")) {
      const Project* pr = app_->current_project();
      const std::string rest = a.substr(8);
      const size_t nl = rest.find('\n');
      std::string agent, id;
      split_key(rest.substr(0, nl), &agent, &id);
      if (!pr || id.empty() || nl == std::string::npos) return;
      const std::string sub = rest.substr(nl + 1);
      app_->store().assign_sub(pr->path, agent, id, sub);
      app_->set_status(sub.empty() ? "moved out of its sub-project" : "moved to " + sub);
      return;
    }
    if (a == "archive" && r && !r->key.empty()) { set_archived_for({r->key}, true); return; }
    if (a == "unarchive" && r) { set_archived_for({r->key}, false); return; }
    if (a == "archive_selected") { set_archived_for(marked_, true); return; }
    if (a == "unarchive_selected") { set_archived_for(marked_, false); return; }
    if (a == "resume" && r) {
      if (r->stored)
        app_->spawn_continuation(r->stored->agent, r->stored->id, r->stored->cwd, false);
      else if (r->live)
        app_->spawn_continuation(r->live->agent(), r->live->session_id(), r->live->cwd(), false);
      return;
    }
    if (a == "fork" && r) {
      if (r->stored)
        app_->spawn_continuation(r->stored->agent, r->stored->id, r->stored->cwd, true);
      else if (r->live)
        app_->spawn_continuation(r->live->agent(), r->live->session_id(), r->live->cwd(), true);
      return;
    }
    if (a == "stop" && r && r->live) { app_->close_session(r->live); return; }
  }

 private:
  // Forget marks for rows that no longer exist, so a rename or archive of a
  // vanished chat does not linger in the title count.
  void prune_marks() {
    if (marked_.empty()) { marked_shown_ = 0; return; }
    std::set<std::string> live;
    for (const auto& r : rows_)
      if (!r.key.empty()) live.insert(r.key);
    for (auto it = marked_.begin(); it != marked_.end();)
      it = live.count(*it) ? std::next(it) : marked_.erase(it);
    marked_shown_ = int(marked_.size());
  }

  void toggle_mark(int i) {
    if (i < 0 || i >= int(rows_.size()) || rows_[i].key.empty()) return;
    anchor_ = i;
    const std::string& key = rows_[i].key;
    if (!marked_.insert(key).second) marked_.erase(key);
  }

  void mark_all() {
    marked_.clear();
    for (const auto& r : rows_)
      if (!r.key.empty()) marked_.insert(r.key);
    anchor_ = 0;
  }

  void extend_to(int to) {
    const int add = int(rows_.size());
    if (rows_.empty()) return;
    if (anchor_ < 0) anchor_ = sel_;
    to = std::clamp(to, 0, add);  // allow stepping onto the "+" row as an edge
    if (to >= add) to = add - 1;
    if (to < 0) return;
    const int lo = std::min(anchor_, to), hi = std::max(anchor_, to);
    marked_.clear();
    for (int i = lo; i <= hi && i < add; i++)
      if (!rows_[size_t(i)].key.empty()) marked_.insert(rows_[size_t(i)].key);
    sel_ = to;
    list_.list.sel = sel_;
  }

  // The row the open menu was opened on, found again by identity: rows are
  // rebuilt, re-sorted and re-synced to the selection every frame, so an index
  // taken when the menu opened can name another chat by the time an item is
  // picked. -1 when that chat is gone.
  int menu_index() const {
    for (size_t i = 0; i < rows_.size(); i++) {
      const Row& r = rows_[i];
      if (menu_live_ ? r.live == menu_live_ : (r.stored && r.stored->path == menu_path_))
        return int(i);
    }
    return -1;
  }

  void set_archived_for(const std::set<std::string>& keys, bool on) {
    int n = 0, working = 0;
    for (const auto& key : keys) {
      std::string agent, id;
      split_key(key, &agent, &id);
      if (id.empty()) continue;
      if (on) {
        // An archived chat is put away, so its agent is stopped; the chat is
        // in its transcript and resuming brings it back. Not mid-turn,
        // though: stopping it there would throw the turn away.
        LiveSession* live = nullptr;
        for (const auto& r : rows_)
          if (r.key == key && r.live && !r.live->exited()) live = r.live;
        if (live && live->busy()) { working++; continue; }
        if (live) app_->close_session(live);
      }
      app_->store().set_archived(agent, id, on);
      n++;
    }
    if (n > 0) {
      marked_.clear();
      anchor_ = -1;
    }
    std::string msg;
    if (n > 0) msg = (on ? "archived " : "unarchived ") + std::to_string(n) + " chat" + (n == 1 ? "" : "s");
    if (working > 0)
      msg += (msg.empty() ? "" : " · ") + std::to_string(working) +
             (working == 1 ? " chat is" : " chats are") + " still working — wait, or stop it first";
    if (!msg.empty()) app_->set_status(msg);
  }

  // Point the visible cursor at the app's current selection.
  void resync() {
    for (size_t i = 0; i < rows_.size(); i++) {
      const Row& r = rows_[i];
      if (r.live && r.live == app_->selected_live()) { sel_ = int(i); return; }
      if (r.stored && r.stored->path == app_->selected_path()) { sel_ = int(i); return; }
    }
  }

  // Select a row (preview / focus), no spawning.
  void choose(int i) {
    sel_ = i;
    if (i < 0 || i >= int(rows_.size())) return;
    const Row& r = rows_[i];
    if (r.live) app_->select_live(r.live);
    else if (r.stored) app_->select_stored(r.stored->path);
  }

  // Enter / single-click opens and resumes. Arrow navigation only previews.
  void activate(int i) {
    const Project* pr = app_->current_project();
    if (i == int(rows_.size())) {
      app_->spawn_agent("claude", pr ? app_->selected_cwd() : ".");
      return;
    }
    if (i < 0 || i >= int(rows_.size())) return;
    choose(i);
    app_->open_selected_chat();
  }

  std::vector<Row> rows_;
  std::set<std::string> marked_;  // row keys
  TallList list_;
  int sel_ = 0;
  LiveSession* menu_live_ = nullptr;  // the row the menu was opened on: live,
  std::string menu_path_;             // or stored (see menu_index)
  int anchor_ = -1;       // where a Shift range started
  int marked_shown_ = 0;  // marks still present in the list, for the title
  bool show_archived_ = false;
  int head_ = 0;  // lines the "All chats" row takes, 0 on the Sessions tab
};

}  // namespace

ChatState chat_state(const LiveSession* live, const Theme& th) {
  Row r;
  r.live = const_cast<LiveSession*>(live);
  switch (state_of(r)) {
    case Waiting: return {U'\u25CF', th.attention, "needs you", 0};  // ●
    case Running: return {U'\u25CD', th.working, "working", 1};      // ◍
    case Active: return {U'\u25CB', th.accent, "ready", 2};          // ○
    default: return {U'\u00B7', th.dim, "saved", 3};                 // ·
  }
}

PanePtr make_chat_list() { return std::make_unique<ChatList>(); }

}  // namespace mico
