#include <cstdlib>
#include <ctime>

#include "core/store.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/list.h"
#include "views/views.h"

namespace mico {

std::string rel_time(int64_t mtime) {
  int64_t d = int64_t(time(nullptr)) - mtime;
  if (d < 60) return "now";
  char buf[24];
  if (d < 3600) snprintf(buf, sizeof buf, "%lldm", (long long)(d / 60));
  else if (d < 86400) snprintf(buf, sizeof buf, "%lldh", (long long)(d / 3600));
  else snprintf(buf, sizeof buf, "%lldd", (long long)(d / 86400));
  return buf;
}

void draw_list_button(Painter& p, const Theme& th, const TallList& tl, std::string_view label,
                      bool sel, bool focused) {
  const int y = tl.footer_y(), h = tl.footer_h();
  const Style st{sel ? th.accent : th.dim, sel ? (focused ? th.sel_bg : th.sel_inactive) : th.strip_bg,
                 sel ? attr::kBold : attr::kNone};
  p.fill(Rect{0, y, p.width(), h}, st);
  const int w = text::str_width(label);
  p.text_clipped(std::max(0, (p.width() - w) / 2), y + h / 2, label, st, p.width());
}

void draw_filter_header(Painter& p, const Theme& th, std::string_view label, bool on, bool focused) {
  const Style st{on ? th.text : th.dim, on ? (focused ? th.sel_bg : th.sel_inactive) : th.panel,
                 on ? attr::kBold : attr::kNone};
  p.fill(Rect{0, 0, p.width(), 1}, st);
  p.put(0, 0, on ? U'\u258C' : U' ', Style{th.accent, st.bg});  // ▌
  p.put(1, 0, U'\u25C6', Style{on ? th.accent : th.dim, st.bg, attr::kBold});  // ◆
  p.text_clipped(3, 0, label, st, std::max(0, p.width() - 4));
  p.hline(0, 1, p.width(), U'\u2500', Style{th.border, th.panel});
}

namespace {

// "/home/me/src" -> "~/src": shorter, and how a person reads it.
std::string abbreviate(const std::string& path) {
  const char* e = getenv("HOME");
  std::string h = e ? e : "";
  while (h.size() > 1 && h.back() == '/') h.pop_back();
  if (!h.empty() && path.starts_with(h) && (path.size() == h.size() || path[h.size()] == '/'))
    return "~" + path.substr(h.size());
  return path;
}

class ProjectList final : public Pane {
 public:
  std::string title() const override { return "Projects"; }

  // One row of the tree: a tracked folder, or one of its sub-projects.
  struct Item {
    int project = 0;
    int sub = -1;  // index into the project's subs; -1 for the folder itself
  };

  void rebuild() {
    items_.clear();
    const auto& projects = app_->store().projects();
    for (size_t i = 0; i < projects.size(); i++) {
      items_.push_back({int(i), -1});
      for (size_t j = 0; j < projects[i].subs.size(); j++) items_.push_back({int(i), int(j)});
    }
  }

  // The row the app has selected: the folder, or its sub-project.
  int app_row() const {
    const int p = app_->project_index();
    const SubProject* sp = app_->current_sub();
    for (size_t r = 0; r < items_.size(); r++) {
      if (items_[r].project != p) continue;
      if (!sp && items_[r].sub < 0) return int(r);
      if (sp && items_[r].sub >= 0 &&
          app_->store().projects()[size_t(p)].subs[size_t(items_[r].sub)].name == sp->name)
        return int(r);
    }
    return 0;
  }

  void render(Painter& whole, bool focused) override {
    const auto& projects = app_->store().projects();
    const Theme& th = app_->theme();
    whole.clear(Style{th.text, th.panel});
    rebuild();
    // Off the Sessions tab the list filters, and "All folders" heads it.
    head_ = app_->filtering() && whole.height() > kFilterHeaderH + 3 ? kFilterHeaderH : 0;
    if (head_) draw_filter_header(whole, th, "All folders", app_->all_folders(), focused);
    Painter p = whole.sub(Rect{0, head_, whole.width(), whole.height() - head_});
    const int add = int(items_.size());  // index of the "+ add folder" button

    // Own the selection so it can rest on the "+" button, which has no project
    // behind it. Follow an external change of folder or sub-project.
    const int ext = app_row();
    if (ext != last_ext_) sel_ = last_ext_ = ext;
    list_.list.sel = sel_;
    list_.fit(add, p.height());
    sel_ = list_.list.sel;

    // With every folder in the filter, no one folder is lit.
    const bool lit = !(head_ && app_->all_folders());
    for (int i = list_.list.top, y = 0; i < add && y < list_.list_h(); i++, y += TallList::kItemH) {
      const Item& it = items_[size_t(i)];
      const Project& pr = projects[size_t(it.project)];
      if (it.sub < 0) draw_item(p, pr, y, lit && i == sel_, focused);
      else draw_sub(p, pr, pr.subs[size_t(it.sub)], y, lit && i == sel_, focused);
    }
    draw_list_button(p, th, list_, "+ Add folder", sel_ == add, focused);
  }

  // The dot a set of live chats earns: needing input outranks running, which
  // outranks idle. Nothing running is no dot at all.
  void draw_dot(Painter& p, int x, int y, Color bg, auto&& in) {
    const Theme& th = app_->theme();
    int rank = -1;
    for (LiveSession* s : app_->live_sessions()) {
      if (s->exited() || !in(*s)) continue;
      const int r = s->needs_input() ? 2 : s->busy() ? 1 : s->unseen() ? 2 : 0;
      if (r > rank) rank = r;
    }
    if (rank >= 0) {
      const Color c = rank == 2 ? th.attention : rank == 1 ? th.working : th.ok;
      p.put(x, y, U'●', Style{c, bg, attr::kBold});
    }
  }

  static bool in_project(const Project& pr, const std::string& cwd) {
    if (cwd == pr.path) return true;
    for (const auto& sp : pr.subs)
      if (sp.path != pr.path && (cwd == sp.path || cwd.starts_with(sp.path + "/"))) return true;
    return false;
  }

  // Two lines: the name, then its path with how many chats and how recent.
  void draw_item(Painter& p, const Project& pr, int y, bool sel, bool focused) {
    const Theme& th = app_->theme();
    Style st{sel ? th.text : th.dim, sel ? (focused ? th.sel_bg : th.sel_inactive) : th.panel,
             sel ? attr::kBold : attr::kNone};
    const Style meta{th.dim, st.bg};
    p.fill(Rect{0, y, p.width(), TallList::kItemH}, st);
    for (int dy = 0; dy < TallList::kItemH; dy++)
      p.put(0, y + dy, sel ? U'▌' : U' ', Style{th.accent, st.bg});
    draw_dot(p, 1, y, st.bg, [&](const LiveSession& s) { return in_project(pr, s.cwd()); });
    p.text_clipped(3, y, pr.name.empty() ? pr.path : pr.name, st, std::max(0, p.width() - 4));

    // Second line: path on the left, "12 chats · 3h" on the right. The path
    // gives way first on a narrow pane.
    const size_t n = pr.sessions.size();
    std::string info = n == 0 ? "no chats" : std::to_string(n) + (n == 1 ? " chat" : " chats");
    const std::string when = " \xC2\xB7 " + rel_time(pr.mtime);
    if (n > 0 && text::str_width(info) + text::str_width(when) <= p.width() - 4) info += when;
    const int iw = text::str_width(info);
    const int room = p.width() - 3 - iw - 2;
    if (room >= 6) {
      p.text_clipped(3, y + 1, abbreviate(pr.path), meta, room);
      p.text(p.width() - iw, y + 1, info, meta);
    } else {
      p.text_clipped(3, y + 1, info, meta, std::max(0, p.width() - 4));
    }
  }

  // A sub-project, indented under its folder: its name, then where it runs —
  // "same folder" or the path under the project — and its chats.
  void draw_sub(Painter& p, const Project& pr, const SubProject& sp, int y, bool sel, bool focused) {
    const Theme& th = app_->theme();
    Style st{sel ? th.text : th.dim, sel ? (focused ? th.sel_bg : th.sel_inactive) : th.panel,
             sel ? attr::kBold : attr::kNone};
    const Style meta{th.dim, st.bg};
    p.fill(Rect{0, y, p.width(), TallList::kItemH}, st);
    for (int dy = 0; dy < TallList::kItemH; dy++)
      p.put(0, y + dy, sel ? U'▌' : U' ', Style{th.accent, st.bg});
    p.put(3, y, U'└', Style{th.border, st.bg});  // └
    draw_dot(p, 4, y, st.bg, [&](const LiveSession& s) {
      return in_project(pr, s.cwd()) && app_->live_sub(s) == sp.name;
    });
    p.text_clipped(6, y, sp.name, st, std::max(0, p.width() - 7));
    size_t n = 0;
    int64_t newest = 0;
    for (const auto& s : pr.sessions)
      if (s.sub == sp.name) {
        n++;
        newest = std::max(newest, s.mtime);
      }
    std::string info = n == 0 ? "no chats" : std::to_string(n) + (n == 1 ? " chat" : " chats");
    if (n > 0 && p.width() > 30) info += " \xC2\xB7 " + rel_time(newest);
    const std::string where = sp.path == pr.path ? "same folder" : "." + sp.path.substr(pr.path.size());
    const int iw = text::str_width(info);
    const int room = p.width() - 6 - iw - 2;
    if (room >= 6) {
      p.text_clipped(6, y + 1, where, meta, room);
      p.text(p.width() - iw, y + 1, info, meta);
    } else {
      p.text_clipped(6, y + 1, info, meta, std::max(0, p.width() - 7));
    }
  }

  int add_row() const { return int(items_.size()); }
  int head_ = 0;  // lines the "All folders" row takes, 0 on the Sessions tab

  void set_sel(int v) {
    sel_ = std::clamp(v, 0, add_row());
    if (sel_ >= add_row()) return;
    const Item it = items_[size_t(sel_)];
    app_->select_project(it.project);
    if (it.sub >= 0) app_->select_sub(app_->store().projects()[size_t(it.project)].subs[size_t(it.sub)].name);
    last_ext_ = sel_;
    if (head_) app_->set_all_folders(false);
  }

  bool on_key(const KeyEvent& k) override {
    if (k.key == Key::Enter && sel_ == add_row()) {
      app_->pick_folder();
      return true;
    }
    // "All folders" sits above the first folder: ↑ reaches it, ↓ leaves it.
    if (head_ && !k.shift && !k.ctrl && !k.alt) {
      if (app_->all_folders() && (k.key == Key::Down || k.key == Key::Enter)) {
        app_->set_all_folders(false);
        return true;
      }
      if (!app_->all_folders() && k.key == Key::Up && sel_ == 0) {
        app_->set_all_folders(true);
        return true;
      }
    }
    if (!list_.on_key(k)) return false;
    set_sel(list_.list.sel);
    return true;
  }

  bool on_mouse(const MouseEvent& m, Point at) override {
    if (head_ && at.y < head_) {
      if (m.kind == MouseKind::Press && m.button == MouseButton::Left) app_->set_all_folders(true);
      return true;
    }
    const Point local{at.x, at.y - head_};
    const int hit = list_.on_mouse(m, local);
    if (hit == add_row()) app_->pick_folder();
    else if (hit >= 0) set_sel(hit);
    return true;
  }

  std::vector<MenuItem> context_menu(Point local) override {
    // A right-click acts on the row under the pointer, without selecting it:
    // a new selection rebuilds the layout, which would close this very menu.
    // From the keyboard (no pointer) it acts on the selection.
    int row = sel_;
    if (local.y >= head_) {
      const int hit = list_.index_at(local.y - head_);
      if (hit >= 0) row = hit;
    }
    const bool on_row = row >= 0 && row < add_row();
    menu_target_ = on_row ? items_[size_t(row)] : Item{-1, -1};
    const bool on_sub = on_row && menu_target_.sub >= 0;
    std::vector<MenuItem> items;
    if (on_sub) {
      items = {
          MenuItem{"New agent here…", "new_agent"},
          MenuItem{"Rename sub-project…", "rename_sub"},
          MenuItem{"Remove sub-project", "remove_sub"},
          MenuItem{"Add sub-project…", "add_sub"},
          MenuItem{"Copy path", "copy_path"},
      };
    } else {
      items = {
          MenuItem{"Add sub-project…", "add_sub", on_row},
          MenuItem{"New agent here…", "new_agent", on_row},
          MenuItem{"Open newest session", "open_newest", on_row},
          MenuItem{"Copy path", "copy_path", on_row},
          MenuItem::sep(),
          MenuItem{"Add folder…", "add_folder_menu"},
          MenuItem{"Remove this folder", "remove_folder", on_row},
      };
    }
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem{"Rescan", "rescan"});
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem{"Detach (agents keep running)", "detach"});
    items.push_back(MenuItem{"Stop all agents and quit", "shutdown"});
    return items;
  }

  void on_action(const std::string& a) override {
    // What the menu was opened on.
    const auto& ps = app_->store().projects();
    const Project* pr = menu_target_.project >= 0 && size_t(menu_target_.project) < ps.size()
                            ? &ps[size_t(menu_target_.project)]
                            : nullptr;
    const SubProject* sp = pr && menu_target_.sub >= 0 && size_t(menu_target_.sub) < pr->subs.size()
                               ? &pr->subs[size_t(menu_target_.sub)]
                               : nullptr;
    // Acting on it selects it: "new agent here" means there.
    const auto select_target = [&] {
      if (!pr) return;
      app_->select_project(menu_target_.project);
      if (sp) app_->select_sub(sp->name);
    };
    if (a == "new_agent") {
      select_target();
      app_->open_new_agent();
    } else if (a == "add_sub") {
      if (pr) app_->pick_subproject_folder(pr->path);
    } else if (a == "rename_sub") {
      if (pr && sp) app_->ask("Rename sub-project", "rename_sub", sp->name, pr->path + "\n" + sp->name);
    } else if (a == "remove_sub") {
      if (pr && sp) {
        const std::string name = sp->name, project = pr->path;
        if (app_->current_sub() && app_->current_sub()->name == name && app_->current_project() == pr)
          app_->select_project(menu_target_.project);  // back to the folder, before it goes
        if (app_->store().remove_subproject(project, name))
          app_->set_status("removed sub-project " + name + " \xE2\x80\x94 its chats are back in the folder");
      }
    } else if (a == "add_folder_menu") {
      app_->pick_folder();
    } else if (a == "remove_folder") {
      if (pr && app_->store().remove_folder(pr->path)) app_->set_status("removed " + pr->path);
    } else if (a == "open_newest") {
      if (pr && !pr->sessions.empty()) {
        select_target();
        app_->select_stored(pr->sessions.front().path);
        app_->open_selected_chat();
      }
    } else if (a == "copy_path") {
      if (pr) app_->set_status("path: " + (sp ? sp->path : pr->path));
    } else if (a == "rescan") {
      app_->store().scan();
      app_->set_status("rescanned");
    } else if (a == "detach") {
      app_->detach();
    } else if (a == "shutdown") {
      app_->shutdown();
    }
  }

 private:
  TallList list_;
  std::vector<Item> items_;
  Item menu_target_{-1, -1};  // the row the last menu was opened on
  int sel_ = 0;
  int last_ext_ = -1;  // last app row we synced from
};

}  // namespace

PanePtr make_project_list() { return std::make_unique<ProjectList>(); }

}  // namespace mico
