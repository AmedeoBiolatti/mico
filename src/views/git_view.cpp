#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/text.h"
#include "core/activity.h"
#include "core/git.h"
#include "core/session.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

std::string ago(int64_t unix_s) {
  const int64_t d = int64_t(time(nullptr)) - unix_s;
  char b[24];
  if (d < 60) return "just now";
  if (d < 3600) snprintf(b, sizeof b, "%lldm", (long long)(d / 60));
  else if (d < 86400) snprintf(b, sizeof b, "%lldh", (long long)(d / 3600));
  else snprintf(b, sizeof b, "%lldd", (long long)(d / 86400));
  return b;
}

std::string home_path(const std::string& p) {
  const char* home = getenv("HOME");
  const size_t n = home ? strlen(home) : 0;
  if (n && p.size() > n && p.compare(0, n, home) == 0 && p[n] == '/') return "~" + p.substr(n);
  return p;
}

bool under(const std::string& path, const std::string& dir) {
  return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/');
}

std::string chat_label(const ChatActivity& c) {
  return agent_label(c.agent) + " \xC2\xB7 " + (c.title.empty() ? std::string("chat") : text::oneline(c.title, 80));
}

// A commit a chat made, and the chat.
struct Made {
  const ChatActivity* chat = nullptr;
  const ChatCommit* commit = nullptr;
};

// The repository holding the selected folder, as git and the agents see it:
// its work trees and who is working in each, its other branches and which
// chat last committed to each, what the focused work tree has not
// committed and which chat changed it, and the focused branch's commits,
// each with the chat that made it. Beside the list, the diff of whatever is
// selected. Nothing here writes to the repository.
class GitView final : public Pane {
 public:
  explicit GitView(GitTabState& st)
      : scope_(st.scope), wt_(st.wt), ref_(st.ref), sel_key_(st.sel_key) {}
  std::string title() const override { return "Git"; }

  void render(Painter& p, bool focused) override {
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.panel});
    const int W = p.width(), H = p.height();
    gather();

    // Heading.
    {
      int x = p.text(1, 0, "Git", Style{th.text, th.panel, attr::kBold}) + 3;
      x += p.text_clipped(x, 0, app_->view_filter().label + (top_.empty() ? "" : " \xC2\xB7 " + home_path(top_)),
                          Style{th.accent, th.panel}, std::max(0, W - x - 1));
      p.text_clipped(x, 0, "    \xE2\x86\x91\xE2\x86\x93 choose \xC2\xB7 enter go \xC2\xB7 r read again \xC2\xB7 a " +
                               std::string(app_->all_folders() ? "this folder" : "all folders"),
                     Style{th.dim, th.panel}, std::max(0, W - x - 1));
    }
    if (H < 3) return;
    p.hline(1, 1, std::max(0, W - 2), U'─', Style{th.border, th.panel});

    // The list, and the detail beside it or, on a narrow pane, below it.
    const bool side = W >= 110;
    Rect list{0, 2, side ? std::clamp(W * 48 / 100, 50, 96) : W, H - 2};
    Rect detail{};
    if (side) {
      detail = Rect{list.w + 1, 2, W - list.w - 1, H - 2};
      for (int y = 2; y < H; y++) p.put(list.w, y, U'│', Style{th.border, th.panel});
    } else if (H > 14) {
      list.h = (H - 2) / 2;
      detail = Rect{0, 2 + list.h + 1, W, H - 3 - list.h};
      p.hline(1, 2 + list.h, std::max(0, W - 2), U'─', Style{th.border, th.panel});
    }
    detail_rect_ = detail;
    list_rect_ = list;
    draw_list(p.sub(list), focused);
    if (detail.w > 4 && detail.h > 1) draw_detail(p.sub(detail));
  }

  bool on_key(const KeyEvent& k) override {
    switch (k.key) {
      case Key::Up: move(-1); return true;
      case Key::Down: move(+1); return true;
      case Key::PageUp: detail_scroll_ = std::max(0, detail_scroll_ - std::max(1, detail_rect_.h - 2)); return true;
      case Key::PageDown: detail_scroll_ += std::max(1, detail_rect_.h - 2); return true;
      case Key::Home: detail_scroll_ = 0; return true;
      case Key::Enter: act(); return true;
      default: break;
    }
    if (k.is('k')) { move(-1); return true; }
    if (k.is('j')) { move(+1); return true; }
    if (k.is(' ')) { detail_scroll_ += std::max(1, detail_rect_.h - 2); return true; }
    if (k.is('r')) { app_->workspace().git().refresh_all(); return true; }
    if (k.is('a')) { app_->set_all_folders(!app_->all_folders()); return true; }
    return false;
  }

  bool on_mouse(const MouseEvent& m, Point local) override {
    const bool in_detail = detail_rect_.w > 0 && local.x >= detail_rect_.x && local.y >= detail_rect_.y &&
                           local.y < detail_rect_.y + detail_rect_.h;
    if (m.kind == MouseKind::WheelUp) {
      if (in_detail) detail_scroll_ = std::max(0, detail_scroll_ - 3);
      else move(-1);
      return true;
    }
    if (m.kind == MouseKind::WheelDown) {
      if (in_detail) detail_scroll_ += 3;
      else move(+1);
      return true;
    }
    if (m.kind != MouseKind::Press || m.button != MouseButton::Left || in_detail) return false;
    for (const auto& [y, i] : shown_)
      if (y == local.y) {
        // A second click on the selected row goes where Enter would.
        if (i == sel_) act();
        else select(i);
        return true;
      }
    return false;
  }

  std::vector<MenuItem> context_menu(Point) override {
    return {MenuItem{"Read git again", "refresh"},
            MenuItem{app_->all_folders() ? "Only this folder" : "All folders", "all"}};
  }
  void on_action(const std::string& a) override {
    if (a == "refresh") app_->workspace().git().refresh_all();
    else if (a == "all") app_->set_all_folders(!app_->all_folders());
  }

 private:
  enum class Kind { Heading, Blank, Note, Folder, Worktree, Agent, Branch, File, Commit };
  struct Line {
    Kind kind;
    int index = -1;    // into the vector its kind is kept in
    std::string text;  // a heading's or a note's
    std::string key;   // what keeps the selection on it as the list changes
    bool selectable() const { return kind != Kind::Heading && kind != Kind::Blank && kind != Kind::Note; }
  };
  struct AgentRow {
    LiveSession* live;
    int worktree;
  };
  struct FolderRow {
    int project;
    const GitStatus* st;
  };
  struct FileRow {
    GitStatus::Entry e;
    GitNumstat n;
    bool counted = false;
    const ChatActivity* chat = nullptr;  // the chat that last changed it since HEAD
    const FileEdit* edit = nullptr;
  };

  GitIndex& git() { return app_->workspace().git(); }

  // The work tree holding `path`: the deepest whose folder contains it, so a
  // work tree inside the main one wins.
  int worktree_of(const std::string& path) const {
    int best = -1;
    for (size_t i = 0; i < worktrees_.size(); i++)
      if (under(path, worktrees_[i].path) && (best < 0 || worktrees_[i].path.size() > worktrees_[size_t(best)].path.size()))
        best = int(i);
    return best;
  }

  void gather() {
    lines_.clear();
    worktrees_.clear();
    agents_.clear();
    branches_.clear();
    files_.clear();
    log_.clear();
    folders_.clear();
    top_.clear();
    const App::ViewFilter vf = app_->view_filter();
    if (!vf.project) return gather_all();
    const std::string dir = vf.sub ? vf.sub->path : vf.project->path;
    const GitStatus* st = git().status(dir);
    if (!st) return heading_note("reading git\xE2\x80\xA6");
    if (!st->repo) return heading_note(home_path(dir) + " is not in a git repository");
    top_ = st->top;

    // Work trees: what git lists, or this one alone until it has answered.
    if (const auto* q = git().query(dir, {"worktree", "list", "--porcelain"}); q && q->ok)
      worktrees_ = parse_worktrees(q->out);
    if (worktrees_.empty())
      worktrees_.push_back(GitWorktree{st->top.empty() ? dir : st->top, st->head, st->branch, false, false, false});
    std::erase_if(worktrees_, [](const GitWorktree& w) { return w.bare; });

    // The focused work tree: the one chosen, else the one holding the chat
    // the sidebar selects, else the selected folder's.
    std::string want = dir;
    if (!vf.chat_path.empty())
      for (const ChatActivity* c : app_->workspace().activity().chats())
        if (c->path == vf.chat_path) want = c->cwd;
    if (scope_ != dir) {
      scope_ = dir;
      wt_.clear();
      ref_.clear();
      sel_key_.clear();
    }
    int focus = -1;
    for (size_t i = 0; i < worktrees_.size(); i++)
      if (worktrees_[i].path == wt_) focus = int(i);
    if (focus < 0) focus = std::max(0, worktree_of(want));
    wt_ = worktrees_[size_t(focus)].path;

    // Who is working where.
    for (LiveSession* s : app_->live_sessions()) {
      if (s->exited()) continue;
      const int w = worktree_of(s->cwd());
      if (w >= 0) agents_.push_back(AgentRow{s, w});
    }

    // Which chats committed what, by the hash they printed.
    made_.clear();
    std::map<std::string, Made> last_on_branch;
    for (const ChatActivity* c : app_->workspace().activity().chats()) {
      if (c->commits.empty() || worktree_of(c->cwd) < 0) continue;
      for (const ChatCommit& cm : c->commits) {
        made_[cm.hash.substr(0, 7)] = Made{c, &cm};
        if (cm.branch.empty()) continue;
        Made& m = last_on_branch[cm.branch];
        if (!m.commit || m.commit->at_ms < cm.at_ms) m = Made{c, &cm};
      }
    }

    // Branches not checked out in any work tree.
    if (const auto* q = git().query(dir, {"for-each-ref", kBranchFormat, "refs/heads"}); q && q->ok)
      for (GitBranch& b : parse_branches(q->out))
        if (std::none_of(worktrees_.begin(), worktrees_.end(), [&](const GitWorktree& w) { return w.branch == b.name; }))
          branches_.push_back(std::move(b));
    branch_made_.clear();
    for (const GitBranch& b : branches_) {
      auto it = last_on_branch.find(b.name);
      branch_made_.push_back(it == last_on_branch.end() ? Made{} : it->second);
    }
    for (size_t i = 0; i < worktrees_.size(); i++) {
      auto it = last_on_branch.find(worktrees_[i].branch);
      worktree_made_.resize(worktrees_.size());
      worktree_made_[i] = it == last_on_branch.end() ? Made{} : it->second;
    }

    // What the focused work tree has not committed, and which chat changed
    // each file since HEAD's commit.
    const GitStatus* wst = git().status(wt_);
    std::string wtop = wst && !wst->top.empty() ? wst->top : wt_;
    int64_t head_time = 0;
    if (const auto* q = git().query(wt_, {"log", kLogFormat, "-n", "1", "HEAD", "--"}); q && q->ok) {
      const auto l = parse_log(q->out);
      if (!l.empty()) head_time = l[0].time;
    }
    if (wst)
      for (const auto& e : wst->entries) files_.push_back(FileRow{e, {}, false, nullptr, nullptr});
    if (const auto* q = git().query(wt_, {"diff", "--numstat", "-z", "HEAD"}); q && q->ok)
      for (const auto& [path, n] : parse_numstat(q->out))
        for (FileRow& f : files_)
          if (f.e.path == path) {
            f.n = n;
            f.counted = true;
          }
    if (!files_.empty()) {
      std::unordered_map<std::string, FileRow*> by_path;
      for (FileRow& f : files_) by_path[wtop + "/" + f.e.path] = &f;
      for (const ChatActivity* c : app_->workspace().activity().chats())
        for (const FileEdit& e : c->edits) {
          if (e.at_ms / 1000 < head_time) continue;
          auto it = by_path.find(e.file);
          if (it == by_path.end()) continue;
          FileRow& f = *it->second;
          if (!f.edit || f.edit->at_ms < e.at_ms) {
            f.edit = &e;
            f.chat = c;
          }
        }
    }

    files_who_ = std::any_of(files_.begin(), files_.end(), [](const FileRow& f) { return f.chat != nullptr; });
    files_name_w_ = 0;
    for (const FileRow& f : files_)
      files_name_w_ = std::max(files_name_w_, text::str_width(f.e.path) + (f.e.orig.empty() ? 0 : text::str_width(f.e.orig) + 3));

    // The focused branch's commits.
    const std::string ref = ref_.empty() ? "HEAD" : ref_;
    if (const auto* q = git().query(wt_, {"log", kLogFormat, "-n", "200", ref, "--"}); q && q->ok)
      log_ = parse_log(q->out);

    build_lines();
  }

  // With every folder in the filter: each tracked folder's branch and state,
  // and who is working there.
  void gather_all() {
    const auto& projects = app_->store().projects();
    lines_.push_back(Line{Kind::Heading, -1, "Folders", {}});
    for (size_t i = 0; i < projects.size(); i++) {
      folders_.push_back(FolderRow{int(i), git().status(projects[i].path)});
      lines_.push_back(Line{Kind::Folder, int(folders_.size()) - 1, {}, "f" + projects[i].path});
    }
    lines_.push_back(Line{Kind::Blank, -1, {}, {}});
    lines_.push_back(Line{Kind::Note, -1, "enter narrows to a folder: its work trees, changes and commits", {}});
    fix_selection();
  }

  void heading_note(std::string text) {
    lines_.push_back(Line{Kind::Note, -1, std::move(text), {}});
    fix_selection();
  }

  void build_lines() {
    lines_.push_back(Line{Kind::Heading, -1, worktrees_.size() == 1 ? "Work tree" : "Work trees", {}});
    for (size_t i = 0; i < worktrees_.size(); i++) {
      lines_.push_back(Line{Kind::Worktree, int(i), {}, "w" + worktrees_[i].path});
      for (size_t a = 0; a < agents_.size(); a++)
        if (agents_[a].worktree == int(i))
          lines_.push_back(Line{Kind::Agent, int(a), {}, "a" + std::to_string(agents_[a].live->serial())});
    }
    if (!branches_.empty()) {
      lines_.push_back(Line{Kind::Blank, -1, {}, {}});
      lines_.push_back(Line{Kind::Heading, -1, "Other branches", {}});
      for (size_t i = 0; i < branches_.size(); i++)
        lines_.push_back(Line{Kind::Branch, int(i), {}, "b" + branches_[i].name});
    }
    lines_.push_back(Line{Kind::Blank, -1, {}, {}});
    const std::string wname = wt_.substr(wt_.rfind('/') + 1);
    lines_.push_back(Line{Kind::Heading, -1,
                          "Changes \xC2\xB7 " + wname + (files_.empty() ? "" : " \xC2\xB7 " + std::to_string(files_.size()) +
                                                                          (files_.size() == 1 ? " file" : " files")),
                          {}});
    if (files_.empty()) lines_.push_back(Line{Kind::Note, -1, "nothing uncommitted", {}});
    for (size_t i = 0; i < files_.size(); i++) lines_.push_back(Line{Kind::File, int(i), {}, "c" + files_[i].e.path});
    lines_.push_back(Line{Kind::Blank, -1, {}, {}});
    lines_.push_back(Line{Kind::Heading, -1, "Commits \xC2\xB7 " + (ref_.empty() ? wname + " (HEAD)" : ref_), {}});
    if (log_.empty()) lines_.push_back(Line{Kind::Note, -1, "no commits", {}});
    for (size_t i = 0; i < log_.size(); i++) lines_.push_back(Line{Kind::Commit, int(i), {}, "h" + log_[i].hash});
    fix_selection();
  }

  // The selection stays on its row as rows come and go; until one is
  // chosen, it rests on the first that can be.
  void fix_selection() {
    sel_ = -1;
    for (size_t i = 0; i < lines_.size(); i++)
      if (lines_[i].selectable() && lines_[i].key == sel_key_) sel_ = int(i);
    if (sel_ < 0)
      for (size_t i = 0; i < lines_.size(); i++)
        if (lines_[i].selectable()) {
          sel_ = int(i);
          break;
        }
  }

  void select(int i) {
    if (i < 0 || i >= int(lines_.size()) || !lines_[size_t(i)].selectable() || i == sel_) return;
    sel_ = i;
    sel_key_ = lines_[size_t(i)].key;
    detail_scroll_ = 0;
  }

  void move(int d) {
    for (int i = sel_ + d; i >= 0 && i < int(lines_.size()); i += d)
      if (lines_[size_t(i)].selectable()) {
        select(i);
        return;
      }
  }

  void act() {
    if (sel_ < 0 || sel_ >= int(lines_.size())) return;
    const Line& l = lines_[size_t(sel_)];
    switch (l.kind) {
      case Kind::Folder:
        app_->select_project(folders_[size_t(l.index)].project);
        app_->set_all_folders(false);
        break;
      case Kind::Worktree:
        wt_ = worktrees_[size_t(l.index)].path;
        ref_.clear();
        break;
      case Kind::Agent:
        app_->open_live(agents_[size_t(l.index)].live);
        break;
      case Kind::Branch:
        ref_ = branches_[size_t(l.index)].name;
        break;
      case Kind::File: {
        const FileRow& f = files_[size_t(l.index)];
        if (f.chat) app_->open_at(f.chat->path, f.edit->call_offset, {});
        else app_->set_status(f.e.path + ": no chat mico knows changed it since the last commit");
        break;
      }
      case Kind::Commit: {
        const GitLogEntry& e = log_[size_t(l.index)];
        if (!app_->open_commit(e.hash))
          app_->set_status(e.hash.substr(0, 7) + " by " + e.author + ": not a commit any chat mico knows made");
        break;
      }
      default: break;
    }
  }

  // ---------------------------------------------------------------- list

  void draw_list(Painter p, bool focused) {
    const Theme& th = app_->theme();
    const int W = p.width(), H = p.height();
    shown_.clear();
    if (sel_ >= 0) {
      if (sel_ < scroll_ + 1) scroll_ = std::max(0, sel_ - 1);
      if (sel_ >= scroll_ + H - 1) scroll_ = sel_ - H + 2;
    }
    scroll_ = std::clamp(scroll_, 0, std::max(0, int(lines_.size()) - H));
    for (int r = 0; r < H && scroll_ + r < int(lines_.size()); r++) {
      const int i = scroll_ + r;
      const Line& l = lines_[size_t(i)];
      const bool sel = i == sel_;
      const Color bg = sel ? (focused ? th.sel_bg : th.sel_inactive) : th.panel;
      if (sel) p.fill(Rect{0, r, W, 1}, Style{th.text, bg});
      if (sel) p.put(0, r, U'▌', Style{th.accent, bg});
      if (l.selectable()) shown_.push_back({r + list_rect_.y, i});
      const Style text{th.text, bg, sel ? attr::kBold : uint16_t(0)}, dim{th.dim, bg};
      switch (l.kind) {
        case Kind::Heading:
          p.text_clipped(1, r, l.text, Style{th.heading, bg, attr::kBold}, W - 2);
          break;
        case Kind::Note:
          p.text_clipped(3, r, l.text, dim, W - 4);
          break;
        case Kind::Blank: break;
        case Kind::Folder: draw_folder(p, r, folders_[size_t(l.index)], text, dim, bg); break;
        case Kind::Worktree: draw_worktree(p, r, l.index, text, dim, bg); break;
        case Kind::Agent: draw_agent(p, r, agents_[size_t(l.index)], text, dim, bg); break;
        case Kind::Branch: draw_branch(p, r, l.index, text, dim, bg); break;
        case Kind::File: draw_file(p, r, files_[size_t(l.index)], text, dim, bg); break;
        case Kind::Commit: draw_commit(p, r, log_[size_t(l.index)], text, dim, bg); break;
      }
    }
  }

  // "main ±3 ↑1 ↓2", or "clean"; returns the columns used.
  int draw_state(Painter& p, int x, int y, const GitStatus* st, Color bg, int w) {
    const Theme& th = app_->theme();
    if (!st) return p.text_clipped(x, y, "\xE2\x80\xA6", Style{th.dim, bg}, w);
    int used = 0;
    const auto part = [&](const std::string& s, Color c) {
      if (used + text::str_width(s) + 1 > w) return;
      used += p.text(x + used, y, s, Style{c, bg}) + 1;
    };
    if (st->files) part("\xC2\xB1" + std::to_string(st->files), th.warn);
    else part("clean", th.dim);
    if (st->conflicts) part("!" + std::to_string(st->conflicts), th.err);
    if (st->ahead) part("\xE2\x86\x91" + std::to_string(st->ahead), th.dim);
    if (st->behind) part("\xE2\x86\x93" + std::to_string(st->behind), th.dim);
    return used;
  }

  void draw_folder(Painter& p, int r, const FolderRow& f, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const Project& pr = app_->store().projects()[size_t(f.project)];
    const int W = p.width();
    int x = 3 + p.text_clipped(3, r, pr.name.empty() ? pr.path : pr.name, text, 20) + 2;
    if (f.st && f.st->repo) {
      x += p.text_clipped(x, r, f.st->branch.empty() ? f.st->head : f.st->branch, Style{th.accent, bg}, 24) + 1;
      x += draw_state(p, x, r, f.st, bg, 20) + 1;
    } else {
      x += p.text(x, r, f.st ? "no git" : "\xE2\x80\xA6", dim) + 2;
    }
    int working = 0;
    for (LiveSession* s : app_->live_sessions())
      if (!s->exited() && under(s->cwd(), pr.path)) working++;
    if (working)
      p.text_clipped(x, r, std::to_string(working) + (working == 1 ? " agent" : " agents"), dim, std::max(0, W - x - 1));
  }

  void draw_worktree(Painter& p, int r, int i, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const GitWorktree& w = worktrees_[size_t(i)];
    const int W = p.width();
    const bool focus = w.path == wt_;
    p.put(2, r, focus ? U'▸' : U' ', Style{th.accent, bg});
    const std::string name = !w.branch.empty() ? w.branch : "detached " + w.head;
    int x = 4 + p.text_clipped(4, r, name, Style{th.accent, bg, text.a}, std::max(8, W / 3)) + 2;
    x += draw_state(p, x, r, git().status(w.path), bg, 22) + 1;
    std::string flags = w.locked ? "locked " : "";
    if (w.prunable) flags += "gone ";
    if (!flags.empty()) x += p.text(x, r, flags, Style{th.err, bg});
    p.text_clipped(x + 1, r, home_path(w.path), dim, std::max(0, W - x - 2));
  }

  void draw_agent(Painter& p, int r, const AgentRow& a, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const ChatState cs = chat_state(a.live, th, app_->anim());
    const int W = p.width();
    p.put(6, r, cs.glyph, Style{cs.color, bg, attr::kBold});
    int x = 8 + p.text(8, r, agent_label(a.live->agent()) + " ", Style{th.text, bg, text.a});
    const std::string word = cs.word;
    const int ww = text::str_width(word);
    x += p.text_clipped(x, r, app_->session_title(*a.live), dim, std::max(0, W - x - ww - 3));
    p.text(W - ww - 1, r, word, Style{cs.color, bg});
  }

  void draw_branch(Painter& p, int r, int i, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const GitBranch& b = branches_[size_t(i)];
    const Made& m = branch_made_[size_t(i)];
    const int W = p.width();
    p.put(2, r, b.name == ref_ ? U'▸' : U' ', Style{th.accent, bg});
    int x = 4 + p.text_clipped(4, r, b.name, Style{th.text, bg, text.a}, std::max(8, W / 3)) + 2;
    if (!b.track.empty()) x += p.text(x, r, b.track + " ", Style{b.track == "[gone]" ? th.err : th.dim, bg});
    const std::string when = ago(b.time);
    const std::string who = m.chat ? chat_label(*m.chat) : b.subject;
    p.text_clipped(x, r, who, Style{m.chat ? th.accent : th.dim, bg}, std::max(0, W - x - int(when.size()) - 2));
    p.text(W - int(when.size()) - 1, r, when, dim);
  }

  void draw_file(Painter& p, int r, const FileRow& f, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const int W = p.width();
    // git's two letters, as `git status --short` shows them.
    const char x0 = f.e.x == '.' ? ' ' : f.e.x, y0 = f.e.y == '.' ? ' ' : f.e.y;
    const char c = f.e.x == '?' ? '?' : f.e.x == 'U' ? 'U' : f.e.y != '.' ? f.e.y : f.e.x;
    const Color col = c == '?' || c == 'A' ? th.added : c == 'D' ? th.removed : c == 'U' ? th.err : c == 'R' ? th.accent : th.warn;
    p.text(3, r, std::string{x0, y0}, Style{col, bg, attr::kBold});
    std::string counts;
    if (f.counted)
      counts = f.n.added < 0 ? "binary" : "+" + std::to_string(f.n.added) + " \xE2\x88\x92" + std::to_string(f.n.removed);
    else if (f.e.x == '?')
      counts = "new";
    // Columns shared by every file: the name, its counts, the chat.
    const int who_w = files_who_ ? std::clamp(W / 3, 12, 40) : 0;
    constexpr int kCounts = 13;
    const int name_w = std::clamp(files_name_w_ + 2, 8, std::max(8, W - 6 - kCounts - who_w - 2));
    std::string name = f.e.orig.empty() ? f.e.path : f.e.orig + " \xE2\x86\x92 " + f.e.path;
    p.text_clipped(6, r, name, text, name_w - 1);
    int x = 6 + name_w;
    if (counts == "binary" || counts == "new") p.text(x, r, counts, dim);
    else if (!counts.empty()) {
      const size_t sp = counts.find(' ');
      x += p.text(x, r, counts.substr(0, sp), Style{th.added, bg}) + 1;
      p.text(x, r, counts.substr(sp + 1), Style{th.removed, bg});
    }
    if (f.chat) p.text_clipped(W - who_w - 1, r, chat_label(*f.chat), Style{th.accent, bg}, who_w);
  }

  void draw_commit(Painter& p, int r, const GitLogEntry& e, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const int W = p.width();
    const auto it = made_.find(e.hash.substr(0, 7));
    const bool agent = it != made_.end();
    p.put(2, r, agent ? U'◆' : U'·', Style{agent ? th.accent : th.border, bg});
    int x = 4 + p.text(4, r, e.hash.substr(0, 7), Style{th.hunk, bg}) + 1;
    const std::string when = ago(e.time);
    const std::string who = agent ? chat_label(*it->second.chat) : e.author;
    const int who_w = std::min(text::str_width(who), std::max(8, W / 3));
    // The subject first; where the branches point, after it, as room allows.
    const int room = std::max(0, W - x - who_w - int(when.size()) - 4);
    x += p.text_clipped(x, r, e.subject, text, room);
    const std::string refs = e.refs.empty() ? "" : " (" + e.refs + ")";
    const int left = room - (x - 4 - 8);
    if (!refs.empty() && left > 6) p.text_clipped(x, r, refs, Style{th.accent, bg}, left);
    p.text_clipped(W - who_w - int(when.size()) - 2, r, who, Style{agent ? th.accent : th.dim, bg}, who_w);
    p.text(W - int(when.size()) - 1, r, when, dim);
  }

  // -------------------------------------------------------------- detail

  void draw_detail(Painter p) {
    const Theme& th = app_->theme();
    const int W = p.width();
    if (sel_ < 0 || sel_ >= int(lines_.size())) return;
    const Line& l = lines_[size_t(sel_)];
    std::vector<std::pair<std::string, Style>> head;  // lines above the diff
    const std::vector<FileChange>* files = nullptr;
    std::vector<FileChange> one;
    std::string root = top_;
    const auto note = [&](std::string s) { head.push_back({std::move(s), Style{th.dim, th.panel}}); };
    switch (l.kind) {
      case Kind::File: {
        const FileRow& f = files_[size_t(l.index)];
        const GitStatus* wst = git().status(wt_);
        if (wst && !wst->top.empty()) root = wst->top;
        head.push_back({f.e.path, Style{th.text, th.panel, attr::kBold}});
        if (f.chat) note("last changed by " + chat_label(*f.chat) + " \xC2\xB7 enter opens the chat there");
        else note("no chat mico knows changed it since the last commit");
        const GitIndex::Query* q =
            f.e.x == '?' ? git().query(wt_, {"diff", "--no-color", "--no-index", "--", "/dev/null", f.e.path}, 30000, true)
                         : git().query(wt_, {"diff", "--no-color", "--find-renames", "HEAD", "--", f.e.path});
        if (!q) note("reading the diff\xE2\x80\xA6");
        else {
          parse_patch(q->out, root, one);
          files = &one;
          if (one.empty()) note("no text difference to show");
        }
        break;
      }
      case Kind::Commit: {
        const GitLogEntry& e = log_[size_t(l.index)];
        head.push_back({e.hash.substr(0, 10) + "  " + e.subject, Style{th.text, th.panel, attr::kBold}});
        const auto it = made_.find(e.hash.substr(0, 7));
        if (it != made_.end()) note("made by " + chat_label(*it->second.chat) + " \xC2\xB7 enter opens the chat there");
        else note(e.author + " \xC2\xB7 not made by any chat mico knows");
        const GitCommit* c = git().commit(wt_, e.hash);
        if (!c) note("reading the commit\xE2\x80\xA6");
        else if (c->found) files = &c->files;
        break;
      }
      case Kind::Worktree: {
        const GitWorktree& w = worktrees_[size_t(l.index)];
        const GitStatus* st = git().status(w.path);
        head.push_back({home_path(w.path), Style{th.text, th.panel, attr::kBold}});
        note(w.branch.empty() ? "detached at " + w.head : "on " + w.branch +
                                                          (st && !st->upstream.empty() ? ", tracking " + st->upstream : ""));
        if (st)
          note(std::to_string(st->staged) + " staged \xC2\xB7 " + std::to_string(st->changed) + " changed \xC2\xB7 " +
               std::to_string(st->untracked) + " untracked" +
               (st->conflicts ? " \xC2\xB7 " + std::to_string(st->conflicts) + " in conflict" : ""));
        int n = 0;
        for (const AgentRow& a : agents_) n += a.worktree == l.index;
        note(n ? std::to_string(n) + (n == 1 ? " agent running here" : " agents running here") : "no agent running here");
        if (l.index < int(worktree_made_.size()) && worktree_made_[size_t(l.index)].chat)
          note("last agent commit: " + worktree_made_[size_t(l.index)].commit->subject + " \xC2\xB7 " +
               chat_label(*worktree_made_[size_t(l.index)].chat));
        note(w.path == wt_ ? "its changes and commits are listed" : "enter lists its changes and commits");
        break;
      }
      case Kind::Branch: {
        const GitBranch& b = branches_[size_t(l.index)];
        head.push_back({b.name, Style{th.text, th.panel, attr::kBold}});
        note("last commit " + ago(b.time) + " ago: " + b.subject);
        if (!b.upstream.empty()) note("tracks " + b.upstream + (b.track.empty() ? "" : " " + b.track));
        if (const Made& m = branch_made_[size_t(l.index)]; m.chat)
          note("last agent commit: " + m.commit->subject + " \xC2\xB7 " + chat_label(*m.chat));
        note(b.name == ref_ ? "its commits are listed" : "enter lists its commits");
        break;
      }
      case Kind::Agent: {
        LiveSession* s = agents_[size_t(l.index)].live;
        head.push_back({agent_label(s->agent()) + " \xC2\xB7 " + app_->session_title(*s), Style{th.text, th.panel, attr::kBold}});
        note(home_path(s->cwd()));
        note("enter opens the chat");
        break;
      }
      case Kind::Folder: {
        const Project& pr = app_->store().projects()[size_t(folders_[size_t(l.index)].project)];
        head.push_back({home_path(pr.path), Style{th.text, th.panel, attr::kBold}});
        note("enter shows its repository");
        break;
      }
      default: break;
    }
    // The heading lines, then the diff, scrolled together.
    std::vector<DRow> rows;
    for (size_t i = 0; i < head.size(); i++) rows.push_back(DRow{-1, int(i)});
    if (files) {
      rows.push_back(DRow{-2, 0});
      for (size_t f = 0; f < files->size(); f++) {
        rows.push_back(DRow{int(f), -1});
        const int n = int((*files)[f].lines.size());
        for (int k = 0; k < std::min(n, kMaxLines); k++) rows.push_back(DRow{int(f), k});
        if (n > kMaxLines) rows.push_back(DRow{int(f), -3});
        rows.push_back(DRow{-2, 0});
      }
    }
    detail_scroll_ = std::clamp(detail_scroll_, 0, std::max(0, int(rows.size()) - p.height()));
    const int num_w = 5;
    for (int y = 0; y < p.height() && detail_scroll_ + y < int(rows.size()); y++) {
      const DRow& d = rows[size_t(detail_scroll_ + y)];
      if (d.file == -1) {
        p.text_clipped(1, y, head[size_t(d.line)].first, head[size_t(d.line)].second, W - 2);
        continue;
      }
      if (d.file == -2) continue;
      const FileChange& fc = (*files)[size_t(d.file)];
      if (d.line == -1) {
        p.fill(Rect{0, y, W, 1}, Style{th.text, th.strip_bg});
        std::string name = fc.file;
        if (!root.empty() && under(name, root)) name = name.substr(root.size() + 1);
        int x = 1 + p.text_clipped(1, y, name, Style{th.text, th.strip_bg, attr::kBold}, std::max(0, W - 20));
        if (fc.op != EditOp::Edit) x += p.text(x, y, std::string(" \xC2\xB7 ") + edit_op_name(fc.op), Style{th.dim, th.strip_bg});
        const std::string plus = "+" + std::to_string(fc.added), minus = " \xE2\x88\x92" + std::to_string(fc.removed);
        const int rx = W - 1 - text::str_width(plus) - text::str_width(minus);
        if (rx > x + 1) {
          p.text(rx, y, plus, Style{th.added, th.strip_bg});
          p.text(rx + text::str_width(plus), y, minus, Style{th.removed, th.strip_bg});
        }
        continue;
      }
      if (d.line == -3) {
        p.text_clipped(num_w + 2, y, "\xE2\x80\xA6 " + std::to_string(int(fc.lines.size()) - kMaxLines) + " more lines",
                       Style{th.dim, th.panel}, W - num_w - 3);
        continue;
      }
      const DiffLine& dl = fc.lines[size_t(d.line)];
      if (dl.kind == '@') {
        p.hline(0, y, W, U'┄', Style{th.border, th.panel});
        if (dl.new_no || dl.old_no)
          p.text(num_w, y, " line " + std::to_string(dl.new_no ? dl.new_no : dl.old_no) + " ", Style{th.hunk, th.panel});
        continue;
      }
      const int no = dl.kind == '-' ? dl.old_no : dl.new_no;
      const Color fg = dl.kind == '+' ? th.added : dl.kind == '-' ? th.removed : th.text;
      const Color bg = dl.kind == '+' ? th.added_bg : dl.kind == '-' ? th.removed_bg : th.panel;
      if (bg != th.panel) p.fill(Rect{num_w, y, std::max(0, W - num_w), 1}, Style{th.text, bg});
      if (no) {
        const std::string n = std::to_string(no);
        p.text(std::max(0, num_w - 1 - int(n.size())), y, n, Style{th.dim, th.panel});
      }
      if (dl.kind != ' ') p.text(num_w, y, dl.kind == '-' ? "\xE2\x88\x92" : "+", Style{fg, bg, attr::kBold});
      p.text_clipped(num_w + 2, y, dl.text, Style{dl.kind == ' ' ? th.dim : fg, bg}, std::max(0, W - num_w - 3));
    }
  }

  struct DRow {
    int file;  // -1 a heading line, -2 a gap, else the file
    int line;  // the heading's, or the file's line; -1 its header, -3 "more"
  };
  static constexpr int kMaxLines = 400;

  // Kept by the App: the pane is made again whenever the sidebar moves.
  std::string& scope_;  // the folder the tab was last showing
  std::string& wt_;     // the focused work tree's path
  std::string& ref_;    // the branch whose commits are listed; empty: the work tree's HEAD
  std::string& sel_key_;
  std::string top_;
  std::vector<GitWorktree> worktrees_;
  std::vector<Made> worktree_made_;
  std::vector<AgentRow> agents_;
  std::vector<GitBranch> branches_;
  std::vector<Made> branch_made_;
  std::vector<FileRow> files_;
  bool files_who_ = false;  // some file has a chat to name
  int files_name_w_ = 0;     // the widest file's name
  std::vector<GitLogEntry> log_;
  std::vector<FolderRow> folders_;
  std::map<std::string, Made> made_;  // by abbreviated hash
  std::vector<Line> lines_;
  int sel_ = -1, scroll_ = 0, detail_scroll_ = 0;
  Rect list_rect_{}, detail_rect_{};
  std::vector<std::pair<int, int>> shown_;  // (row in the pane, line)
};

}  // namespace

PanePtr make_git_view(GitTabState& state) { return std::make_unique<GitView>(state); }

}  // namespace mico
