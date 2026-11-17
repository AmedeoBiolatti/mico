#include <cstdlib>
#include <ctime>
#include <optional>

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

bool draw_list_scrollbar(Painter& p, const Theme& th, const TallList& tl) {
  if (!tl.scrolls() || p.width() < 8) return false;
  const int track = tl.list_h();
  const int span = tl.count - tl.visible();
  const int thumb = std::clamp(track * tl.visible() / std::max(1, tl.count), 1, track);
  const int at = (track - thumb) * std::clamp(tl.list.top, 0, span) / std::max(1, span);
  for (int y = 0; y < track; y++) {
    const bool on = y >= at && y < at + thumb;
    p.put(p.width() - 1, y, on ? U'\u2590' : U'\u2502', Style{on ? th.dim : th.border, th.panel, attr::kDecor});  // ▐ │
  }
  return true;
}

bool list_scrollbar_mouse(TallList& tl, const MouseEvent& m, Point local, int width, bool& held) {
  if (held && m.kind == MouseKind::Release) {
    held = false;
    return true;
  }
  const bool press = m.kind == MouseKind::Press && m.button == MouseButton::Left;
  if (!tl.scrolls() || width < 8) return held = false;
  if (press) {
    if (local.x != width - 1 || local.y < 0 || local.y >= tl.list_h()) return false;
    held = true;
  } else if (!(held && (m.kind == MouseKind::Drag || m.kind == MouseKind::Move))) {
    return false;
  }
  const int span = tl.count - tl.visible();
  const int y = std::clamp(local.y, 0, tl.list_h() - 1);
  tl.list.top = std::clamp((y * span + (tl.list_h() - 1) / 2) / std::max(1, tl.list_h() - 1), 0, span);
  return true;
}

void draw_filter_header(Painter& p, const Theme& th, std::string_view label, bool on, bool focused) {
  const Style st{on ? th.text : th.dim, on ? (focused ? th.sel_bg : th.sel_inactive) : th.panel,
                 on ? attr::kBold : attr::kNone};
  p.fill(Rect{0, 0, p.width(), 1}, st);
  p.put(0, 0, on ? U'\u258C' : U' ', Style{th.accent, st.bg, attr::kDecor});  // ▌
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
    int sub = -1;     // index into the project's subs; -1 for the folder itself
    int kids = 0;     // sub-projects directly in it: it can be folded
    int hidden = 0;   // sub-projects under it not listed, while it is folded
  };

  // The fold key of a row: its folder's, or its sub-project's.
  std::string fold_key(const Item& it) const {
    const Project& pr = app_->store().projects()[size_t(it.project)];
    return App::fold_key(pr.path, it.sub < 0 ? std::string() : pr.subs[size_t(it.sub)].name);
  }

  // The rows: every folder, and its sub-projects but those inside one that
  // is folded away. A sub-project is listed after the one it is in.
  void rebuild() {
    items_.clear();
    const auto& projects = app_->store().projects();
    for (size_t i = 0; i < projects.size(); i++) {
      const Project& pr = projects[i];
      Item head{int(i), -1};
      for (const auto& sp : pr.subs) head.kids += sp.parent().empty();
      const bool folded = head.kids && app_->folded(App::fold_key(pr.path));
      if (folded) head.hidden = int(pr.subs.size());
      items_.push_back(head);
      if (folded) continue;
      std::string skip;  // a folded sub-project, while in its nested ones
      for (size_t j = 0; j < pr.subs.size(); j++) {
        const SubProject& sp = pr.subs[j];
        if (!skip.empty() && sub_within(sp.name, skip) && sp.name != skip) { items_.back().hidden++; continue; }
        skip.clear();
        Item it{int(i), int(j)};
        for (const auto& o : pr.subs) it.kids += o.parent() == sp.name;
        if (it.kids && app_->folded(App::fold_key(pr.path, sp.name))) skip = sp.name;
        items_.push_back(it);
      }
    }
  }

  // The row the app has selected: the folder, or its sub-project — or, when
  // that is folded away, the row it is folded into.
  int app_row() const {
    const int p = app_->project_index();
    const SubProject* sp = app_->current_sub();
    if (p < 0 || size_t(p) >= app_->store().projects().size()) return 0;
    const Project& pr = app_->store().projects()[size_t(p)];
    int best = 0;
    size_t depth = 0;  // how much of the selected sub-project's name the best row covers
    for (size_t r = 0; r < items_.size(); r++) {
      if (items_[r].project != p) continue;
      if (items_[r].sub < 0) {
        if (!sp) return int(r);
        if (depth == 0) best = int(r);
        continue;
      }
      const std::string& name = pr.subs[size_t(items_[r].sub)].name;
      if (sp && sub_within(sp->name, name) && name.size() + 1 > depth) {
        best = int(r);
        depth = name.size() + 1;
      }
    }
    return best;
  }

  // Folds a row away or opens it. Folding the one the chats are filtered to
  // from inside it moves the selection onto it, so what is lit is what shows.
  void set_fold(int row, bool fold) {
    if (row < 0 || row >= add_row() || !items_[size_t(row)].kids) return;
    const Item it = items_[size_t(row)];
    if (fold && it.project == app_->project_index() && app_->current_sub()) {
      const Project& pr = app_->store().projects()[size_t(it.project)];
      if (it.sub < 0 || (sub_within(app_->current_sub()->name, pr.subs[size_t(it.sub)].name) &&
                         app_->current_sub()->name != pr.subs[size_t(it.sub)].name))
        set_sel(row);
    }
    app_->set_folded(fold_key(it), fold);
    rebuild();
    for (size_t r = 0; r < items_.size(); r++)
      if (items_[r].project == it.project && items_[r].sub == it.sub) { sel_ = last_ext_ = int(r); break; }
    list_.list.sel = sel_;
  }
  bool folded_row(int row) const {
    return row >= 0 && row < add_row() && items_[size_t(row)].kids && app_->folded(fold_key(items_[size_t(row)]));
  }
  // The column of a row's ▸/▾, where a click folds or opens it.
  int chevron_x(const Item& it, int width) const {
    if (it.sub < 0) return 1;
    const Project& pr = app_->store().projects()[size_t(it.project)];
    return sub_x(pr.subs[size_t(it.sub)], width);
  }
  static int sub_x(const SubProject& sp, int width) {
    return 4 + std::min(2 * (sp.depth() - 1), std::max(0, (width - 16) / 2));
  }
  // ▾ open, ▸ folded; lit when something folded away needs you.
  void draw_chevron(Painter& p, int x, int y, Color bg, bool folded) {
    const Theme& th = app_->theme();
    p.put(x, y, folded ? U'\u25B8' : U'\u25BE', Style{folded ? th.accent : th.dim, bg, attr::kBold});
  }

  void render(Painter& whole, bool focused) override {
    const auto& projects = app_->store().projects();
    const Theme& th = app_->theme();
    whole.clear(Style{th.text, th.panel});
    rebuild();
    // "All folders" heads the list on every tab: on Sessions it lists the
    // chats of every folder, elsewhere it is what the tab covers.
    head_ = whole.height() > kFilterHeaderH + 3 ? kFilterHeaderH : 0;
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
    width_ = p.width();
    // A list longer than the pane says where it is, in its last column.
    Painter rows = draw_list_scrollbar(p, th, list_) ? p.sub(Rect{0, 0, p.width() - 1, p.height()}) : p;

    // With every folder in the filter, no one folder is lit.
    const bool lit = !(head_ && app_->all_folders());
    for (int i = list_.list.top, y = 0; i < add && y < list_.list_h(); i++, y += TallList::kItemH) {
      const Item& it = items_[size_t(i)];
      const Project& pr = projects[size_t(it.project)];
      const Color bg = i % 2 ? th.panel_alt : th.panel;  // by index: the stripes scroll with the rows
      if (it.sub < 0) draw_item(rows, pr, it, y, lit && i == sel_, focused, bg);
      else draw_sub(rows, pr, it, y, lit && i == sel_, focused, bg);
    }
    draw_list_button(p, th, list_, "+ Add folder", sel_ == add, focused);
  }

  // A folder shows the mark of its chat that wants you most, the same marks
  // the chat list shows: needs you, then a reply not yet read, then working,
  // then ready. Nothing live is no mark at all.
  void draw_dot(Painter& p, int x, int y, Color bg, auto&& in) {
    const Theme& th = app_->theme();
    std::optional<ChatState> best;
    for (LiveSession* s : app_->live_sessions()) {
      if (s->exited() || !in(*s)) continue;
      const ChatState cs = chat_state(s, th, app_->anim());
      if (!best || cs.rank < best->rank) best = cs;
    }
    if (best && best->rank < 4) p.put(x, y, best->glyph, Style{best->color, bg, attr::kBold});
  }

  // How many of a row's chats are in each state that wants a look — "!2"
  // needing you, "●1" with a reply not yet read, "⠋3" working — drawn so
  // they end at column `right`. Ready chats are left out: nothing to do
  // there. Returns the columns used, 0 for none.
  int draw_states(Painter& p, int right, int y, Color bg, int max_w, auto&& in) {
    const Theme& th = app_->theme();
    int count[3] = {0, 0, 0};
    ChatState look[3]{};
    for (LiveSession* s : app_->live_sessions()) {
      if (s->exited() || !in(*s)) continue;
      const ChatState cs = chat_state(s, th, app_->anim());
      if (cs.rank > 2) continue;
      if (!count[cs.rank]++) look[cs.rank] = cs;
    }
    std::string part[3];
    int w = 0;
    for (int r = 0; r < 3; r++)
      if (count[r]) {
        part[r] = std::to_string(count[r]);
        const int pw = 1 + text::str_width(part[r]);
        if (w + (w ? 1 : 0) + pw > max_w) { count[r] = 0; continue; }  // the most urgent first
        w += (w ? 1 : 0) + pw;
      }
    int x = right - w;
    for (int r = 0; r < 3; r++) {
      if (!count[r]) continue;
      p.put(x, y, look[r].glyph, Style{look[r].color, bg, attr::kBold});
      x += 1 + p.text(x + 1, y, part[r], Style{look[r].color, bg, attr::kBold}) + 1;
    }
    return w;
  }

  // What git says of `dir`, in at most `w` columns from x: the branch, then
  // how many files differ, commits ahead and behind, and conflicts, as room
  // allows. A folder in no repository that holds some: how many, and their
  // states added up. Returns the columns used.
  int draw_git(Painter& p, int x, int y, int w, const std::string& dir, Color bg) {
    GitIndex& git = app_->workspace().git();
    const GitStatus* g = git.status(dir);
    if (!g || w < 4) return 0;
    GitStatus sum;
    std::string name;
    if (g->repo) {
      name = !g->branch.empty() ? g->branch : !g->head.empty() ? g->head : "no commits";
    } else {
      const GitRepos& rs = app_->workspace().repos_in(dir);
      if (rs.tops.empty()) return 0;
      name = std::to_string(rs.tops.size()) + (rs.tops.size() == 1 ? " repo" : " repos");
      sum = git.total(rs.tops);
      g = &sum;
    }
    const Theme& th = app_->theme();
    int used = p.text_clipped(x, y, name, Style{th.accent, bg}, w);
    const auto part = [&](const std::string& s, Color c) {
      if (used + 1 + text::str_width(s) > w) return;
      used += 1 + p.text(x + used + 1, y, s, Style{c, bg});
    };
    if (g->files) part("\xC2\xB1" + std::to_string(g->files), th.warn);
    if (g->conflicts) part("!" + std::to_string(g->conflicts), th.err);
    if (g->ahead) part("\xE2\x86\x91" + std::to_string(g->ahead), th.dim);
    if (g->behind) part("\xE2\x86\x93" + std::to_string(g->behind), th.dim);
    return used;
  }

  static bool in_project(const Project& pr, const std::string& cwd) {
    if (cwd == pr.path) return true;
    for (const auto& sp : pr.subs)
      if (sp.path != pr.path && (cwd == sp.path || cwd.starts_with(sp.path + "/"))) return true;
    return false;
  }

  // The open chats a folder's row stands for: all of those running in it and
  // its sub-projects, or, on a sub-project's row, that sub-project's and
  // those of the ones nested in it.
  std::vector<LiveSession*> open_chats(const Project& pr, const SubProject* sp) const {
    std::vector<LiveSession*> out;
    for (LiveSession* s : app_->live_sessions())
      if (in_project(pr, s->cwd()) && (!sp || sub_within(app_->live_sub(*s), sp->name))) out.push_back(s);
    return out;
  }

  // The menu's "Stop all agents": what it will stop, said before it is chosen.
  MenuItem stop_all_item(const Project& pr, const SubProject* sp) const {
    const auto chats = open_chats(pr, sp);
    if (chats.empty()) return MenuItem{"Stop all agents", "stop_all", false, false, false, "none open"};
    size_t working = 0;
    for (const LiveSession* s : chats) working += s->busy();
    std::string detail = std::to_string(chats.size()) + " open";
    if (working) detail += " \xC2\xB7 " + std::to_string(working) + " working";
    return MenuItem{"Stop all agents", "stop_all", true, false, false, detail};
  }

  // Two lines: the name, then its path with how many chats and how recent.
  void draw_item(Painter& p, const Project& pr, const Item& it, int y, bool sel, bool focused, Color bg) {
    const Theme& th = app_->theme();
    Style st{sel ? th.text : th.dim, sel ? (focused ? th.sel_bg : th.sel_inactive) : bg,
             sel ? attr::kBold : attr::kNone};
    const Style meta{th.dim, st.bg};
    p.fill(Rect{0, y, p.width(), TallList::kItemH}, st);
    for (int dy = 0; dy < TallList::kItemH; dy++)
      p.put(0, y + dy, sel ? U'▌' : U' ', Style{th.accent, st.bg, attr::kDecor});
    // ▸/▾ for a folder with sub-projects, then the mark of its chats, those
    // folded away included: one that needs you still shows on a folded row.
    if (it.kids) draw_chevron(p, 1, y, st.bg, it.hidden > 0);
    draw_dot(p, 2, y, st.bg, [&](const LiveSession& s) { return in_project(pr, s.cwd()); });
    // At the right: its chats' states, then how many sub-projects it hides.
    // Open, it counts only the chats no listed sub-project row shows; folded,
    // everything inside it.
    const std::string more = it.hidden ? "+" + std::to_string(it.hidden) : "";
    const int mw = more.empty() ? 0 : text::str_width(more) + 1;
    if (mw && p.width() > 12) p.text(p.width() - mw, y, more, Style{th.dim, st.bg});
    const int sw = p.width() > 16 ? draw_states(p, p.width() - mw - (mw ? 1 : 0), y, st.bg, (p.width() - 5) / 2,
        [&](const LiveSession& s) {
          return in_project(pr, s.cwd()) && (it.hidden || app_->live_sub(s).empty());
        }) : 0;
    const int right = mw + (sw ? sw + 1 + (mw ? 1 : 0) : 0);
    p.text_clipped(4, y, pr.name.empty() ? pr.path : pr.name, st, std::max(0, p.width() - 5 - right));

    // Second line: git's branch and state, then the path, on the left;
    // "12 chats · 3h" on the right. The path gives way first on a narrow
    // pane, then the git state.
    // Archived chats and subagents' runs are not in the chat list, so they
    // don't count.
    size_t n = 0;
    int64_t newest = 0;
    for (const auto& s : pr.sessions)
      if (s.parent.empty() && !app_->store().archived(s.agent, s.id)) {
        n++;
        newest = std::max(newest, s.mtime);
      }
    std::string info = n == 0 ? "no chats" : std::to_string(n) + (n == 1 ? " chat" : " chats");
    const std::string when = " \xC2\xB7 " + rel_time(newest);
    if (n > 0 && text::str_width(info) + text::str_width(when) <= p.width() - 4) info += when;
    const int iw = text::str_width(info);
    const int room = p.width() - 4 - iw - 2;
    if (room >= 6) {
      const int g = draw_git(p, 4, y + 1, room, pr.path, st.bg);
      if (room - g - 2 >= 4) p.text_clipped(4 + (g ? g + 2 : 0), y + 1, abbreviate(pr.path), meta, room - (g ? g + 2 : 0));
      p.text(p.width() - iw, y + 1, info, meta);
    } else {
      p.text_clipped(4, y + 1, info, meta, std::max(0, p.width() - 5));
    }
  }

  // The folder sub-project `sp` sits in: its parent's, or the project's.
  static const std::string& parent_dir(const Project& pr, const SubProject& sp) {
    if (!sp.parent().empty())
      for (const auto& up : pr.subs)
        if (up.name == sp.parent()) return up.path;
    return pr.path;
  }

  // A sub-project, indented under its folder or the sub-project it is in:
  // its name, then where it runs — "same folder" as what it is in, or the
  // path under the project — and its chats, those nested in it included.
  void draw_sub(Painter& p, const Project& pr, const Item& it, int y, bool sel, bool focused, Color bg) {
    const SubProject& sp = pr.subs[size_t(it.sub)];
    const Theme& th = app_->theme();
    Style st{sel ? th.text : th.dim, sel ? (focused ? th.sel_bg : th.sel_inactive) : bg,
             sel ? attr::kBold : attr::kNone};
    const Style meta{th.dim, st.bg};
    p.fill(Rect{0, y, p.width(), TallList::kItemH}, st);
    for (int dy = 0; dy < TallList::kItemH; dy++)
      p.put(0, y + dy, sel ? U'▌' : U' ', Style{th.accent, st.bg, attr::kDecor});
    // Two columns more for each level down, while the pane leaves room.
    // One that holds others has its ▸/▾ where the others have their └.
    const int x = sub_x(sp, p.width());
    if (it.kids) draw_chevron(p, x, y, st.bg, it.hidden > 0);
    else p.put(x, y, U'└', Style{th.border, st.bg});  // └
    draw_dot(p, x + 1, y, st.bg, [&](const LiveSession& s) {
      return in_project(pr, s.cwd()) && sub_within(app_->live_sub(s), sp.name);
    });
    const int tx = x + 3;
    const std::string more = it.hidden ? "+" + std::to_string(it.hidden) : "";
    const int mw = more.empty() ? 0 : text::str_width(more) + 1;
    if (mw && p.width() - tx > 8) p.text(p.width() - mw, y, more, Style{th.dim, st.bg});
    const int sw = p.width() - tx > 12 ? draw_states(p, p.width() - mw - (mw ? 1 : 0), y, st.bg, (p.width() - tx) / 2,
        [&](const LiveSession& s) {
          if (!in_project(pr, s.cwd())) return false;
          const std::string sub = app_->live_sub(s);
          return it.hidden || it.kids == 0 ? sub_within(sub, sp.name) : sub == sp.name;
        }) : 0;
    const int right = mw + (sw ? sw + 1 + (mw ? 1 : 0) : 0);
    p.text_clipped(tx, y, sp.leaf(), st, std::max(0, p.width() - tx - 1 - right));
    size_t n = 0;
    int64_t newest = 0;
    for (const auto& s : pr.sessions)
      if (sub_within(s.sub, sp.name) && s.parent.empty() && !app_->store().archived(s.agent, s.id)) {
        n++;
        newest = std::max(newest, s.mtime);
      }
    std::string info = n == 0 ? "no chats" : std::to_string(n) + (n == 1 ? " chat" : " chats");
    if (n > 0 && p.width() > 30) info += " \xC2\xB7 " + rel_time(newest);
    const bool same = sp.path == parent_dir(pr, sp);
    const std::string where = same ? "same folder" : "." + sp.path.substr(pr.path.size());
    const int iw = text::str_width(info);
    const int room = p.width() - tx - iw - 2;
    if (room >= 6) {
      // A sub-project in a folder of its own may be on another branch, or
      // another repository: it says so. One in its parent's folder would
      // only repeat the parent's line.
      const int g = same ? 0 : draw_git(p, tx, y + 1, room, sp.path, st.bg);
      if (room - g - 2 >= 4) p.text_clipped(tx + (g ? g + 2 : 0), y + 1, where, meta, room - (g ? g + 2 : 0));
      p.text(p.width() - iw, y + 1, info, meta);
    } else {
      p.text_clipped(tx, y + 1, info, meta, std::max(0, p.width() - tx - 1));
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
    // Folding: Space either way; ← folds (or goes up to what it is in) and
    // → opens, but not in the compact layout, where ← goes back.
    if (sel_ < add_row() && !k.ctrl && !k.alt) {
      if (k.is(' ') && items_[size_t(sel_)].kids) { set_fold(sel_, !folded_row(sel_)); return true; }
      if (!app_->compact() && k.key == Key::Right && items_[size_t(sel_)].kids && folded_row(sel_)) {
        set_fold(sel_, false);
        return true;
      }
      if (!app_->compact() && k.key == Key::Left) {
        if (items_[size_t(sel_)].kids && !folded_row(sel_)) set_fold(sel_, true);
        else if (items_[size_t(sel_)].sub >= 0) {
          // Up to the row it is in.
          const Item it = items_[size_t(sel_)];
          const std::string up(app_->store().projects()[size_t(it.project)].subs[size_t(it.sub)].parent());
          for (int r = sel_ - 1; r >= 0; r--) {
            const Item& o = items_[size_t(r)];
            if (o.project != it.project) break;
            const bool is_up = up.empty() ? o.sub < 0
                : o.sub >= 0 && app_->store().projects()[size_t(o.project)].subs[size_t(o.sub)].name == up;
            if (is_up) { list_.list.sel = r; set_sel(r); break; }
          }
        }
        return true;
      }
    }
    if (!list_.on_key(k)) return false;
    set_sel(list_.list.sel);
    // Compact: a folder chosen goes on to its chats.
    if (k.key == Key::Enter && app_->compact() && sel_ < add_row()) app_->compact_show(App::Screen::Chats);
    return true;
  }

  bool on_mouse(const MouseEvent& m, Point at) override {
    if (head_ && at.y < head_ && !bar_held_) {
      if (m.kind == MouseKind::Press && m.button == MouseButton::Left) app_->set_all_folders(true);
      return true;
    }
    const Point local{at.x, at.y - head_};
    if (list_scrollbar_mouse(list_, m, local, width_, bar_held_)) {
      if (bar_held_) app_->capture_mouse(this);
      return true;
    }
    // A press on a row's ▸/▾ folds or opens it, and selects nothing.
    if (m.kind == MouseKind::Press && m.button == MouseButton::Left) {
      const int row = list_.index_at(local.y);
      if (row >= 0 && row < add_row() && items_[size_t(row)].kids &&
          local.y % TallList::kItemH == 0 && std::abs(local.x - chevron_x(items_[size_t(row)], width_)) <= 1) {
        set_fold(row, !folded_row(row));
        return true;
      }
    }
    const int hit = list_.on_mouse(m, local);
    if (hit == add_row()) app_->pick_folder();
    else if (hit >= 0) {
      set_sel(hit);
      // Compact: a tap on a folder goes on to its chats.
      if (app_->compact() && m.kind == MouseKind::Press) app_->compact_show(App::Screen::Chats);
    }
    return true;
  }

  std::vector<MenuItem> context_menu(Point local) override {
    // A right-click acts on the row under the pointer, without selecting it:
    // a new selection rebuilds the layout, which would close this very menu.
    // From the keyboard (no pointer) it acts on the selection.
    int row = sel_;
    if (local.y >= 0) row = local.y >= head_ ? list_.index_at(local.y - head_) : -1;
    const bool on_row = row >= 0 && row < add_row();
    menu_target_ = on_row ? items_[size_t(row)] : Item{-1, -1};
    // Off a folder: what is about the list. Detaching and quitting are F10,
    // :detach and :quit, never one slip of the mouse away.
    if (!on_row) return {MenuItem{"Add folder\xE2\x80\xA6", "add_folder_menu"}, MenuItem{"Rescan", "rescan"}};
    const auto& ps = app_->store().projects();
    if (size_t(menu_target_.project) >= ps.size()) return {};
    const Project& pr = ps[size_t(menu_target_.project)];
    if (menu_target_.sub >= 0 && size_t(menu_target_.sub) >= pr.subs.size()) return {};
    if (menu_target_.sub >= 0)
      return {MenuItem{"New chat here\xE2\x80\xA6", "new_agent"}, MenuItem{"Add sub-project\xE2\x80\xA6", "add_sub"},
              MenuItem{"Rename sub-project\xE2\x80\xA6", "rename_sub"}, MenuItem{"Copy path", "copy_path"}, MenuItem::sep(),
              stop_all_item(pr, &pr.subs[size_t(menu_target_.sub)]), MenuItem{"Remove sub-project", "remove_sub"}};
    return {MenuItem{"New chat here\xE2\x80\xA6", "new_agent"}, MenuItem{"Add sub-project\xE2\x80\xA6", "add_sub"},
            MenuItem{"Copy path", "copy_path"}, MenuItem::sep(),
            stop_all_item(pr, nullptr), MenuItem{"Remove folder", "remove_folder"}};
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
    } else if (a == "stop_all") {
      // Every open chat of it goes, a working one too: the chats stay in the
      // list, from their transcripts, to be resumed.
      if (pr) {
        size_t running = 0, exited = 0;
        for (LiveSession* s : open_chats(*pr, sp)) {
          (s->exited() ? exited : running)++;
          app_->close_session(s);
        }
        const std::string where = sp ? sub_label(sp->name) : pr->name.empty() ? pr->path : pr->name;
        std::string msg = running ? "stopped " + std::to_string(running) + (running == 1 ? " agent" : " agents") : "no agent was running";
        if (exited) msg += " \xC2\xB7 closed " + std::to_string(exited) + " that had exited";
        app_->set_status(msg + " in " + where);
      }
    } else if (a == "add_sub") {
      if (pr) app_->pick_subproject_folder(pr->path, sp ? sp->name : std::string());
    } else if (a == "rename_sub") {
      if (pr && sp) app_->ask("Rename sub-project", "rename_sub", std::string(sp->leaf()), pr->path + "\n" + sp->name);
    } else if (a == "remove_sub") {
      if (pr && sp) {
        const std::string name = sp->name, project = pr->path, up(sp->parent());
        size_t nested = 0;
        for (const auto& o : pr->subs) nested += o.name != name && sub_within(o.name, name);
        // Back to what it was in, before it goes.
        if (app_->current_sub() && sub_within(app_->current_sub()->name, name) && app_->current_project() == pr) {
          app_->select_project(menu_target_.project);
          if (!up.empty()) app_->select_sub(up);
        }
        if (app_->store().remove_subproject(project, name)) {
          std::string msg = "removed sub-project " + sub_label(name);
          if (nested) msg += " and " + std::to_string(nested) + " nested in it";
          app_->set_status(msg + " \xE2\x80\x94 its chats are back in " +
                           (up.empty() ? std::string("the folder") : sub_label(up)));
        }
      }
    } else if (a == "add_folder_menu") {
      app_->pick_folder();
    } else if (a == "remove_folder") {
      if (pr && app_->store().remove_folder(pr->path)) app_->set_status("removed " + pr->path);
    } else if (a == "copy_path") {
      if (pr) {
        const std::string path = sp ? sp->path : pr->path;
        app_->copy_to_clipboard(path);
        app_->set_status("copied: " + path);
      }
    } else if (a == "rescan") {
      app_->store().scan();
      app_->set_status("rescanned");
    }
  }

 private:
  TallList list_;
  std::vector<Item> items_;
  Item menu_target_{-1, -1};  // the row the last menu was opened on
  int sel_ = 0;
  int last_ext_ = -1;  // last app row we synced from
  int width_ = 0;      // the list's, as last drawn
  bool bar_held_ = false;  // the scrollbar is being dragged
};

}  // namespace

PanePtr make_project_list() { return std::make_unique<ProjectList>(); }

}  // namespace mico
