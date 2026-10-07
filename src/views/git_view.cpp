#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/text.h"
#include "core/activity.h"
#include "core/git.h"
#include "core/session.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

using View = GitTabState::View;

std::string ago(int64_t unix_s) {
  const int64_t d = int64_t(time(nullptr)) - unix_s;
  char b[24];
  if (d < 60) return "now";
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

std::string_view last_part(std::string_view p) {
  while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
  const size_t slash = p.rfind('/');
  return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

std::string chat_label(const ChatActivity& c) {
  return agent_label(c.agent) + " \xC2\xB7 " + (c.title.empty() ? std::string("chat") : text::oneline(c.title, 80));
}

// A commit a chat made, and the chat.
struct Made {
  const ChatActivity* chat = nullptr;
  const ChatCommit* commit = nullptr;
};

const char* const kViewNames[GitTabState::kViews] = {"Changes", "Log", "Branches", "Trees", "Repos"};

// Where git has a file, in the order an editor lists them.
enum Section : int { kConflicts, kStaged, kUnstaged, kUntracked, kSections };
const char* const kSectionNames[kSections] = {"Conflicts", "Staged", "Changes", "Untracked"};
const char* const kSectionSays[kSections] = {"in conflict", "staged", "changed, not staged", "untracked"};

// "[ahead 2, behind 1]" as "↑2 ↓1", and "[gone]" as "gone".
std::string track_marks(const std::string& t) {
  if (t.empty()) return {};
  if (t == "[gone]") return "gone";
  int a = 0, b = 0;
  if (const size_t i = t.find("ahead "); i != std::string::npos) a = std::atoi(t.c_str() + i + 6);
  if (const size_t i = t.find("behind "); i != std::string::npos) b = std::atoi(t.c_str() + i + 7);
  std::string s;
  if (a) s += "\xE2\x86\x91" + std::to_string(a);
  if (b) s += (s.empty() ? "" : " ") + std::string("\xE2\x86\x93") + std::to_string(b);
  return s;
}

// "+12 −3", leaving out a side that is nought.
std::string plus_minus(int added, int removed) {
  std::string s;
  if (added) s += "+" + std::to_string(added);
  if (removed) s += (s.empty() ? "" : " ") + std::string("\xE2\x88\x92") + std::to_string(removed);
  return s;
}

// Names split at '/' into a tree: files under their folders, branches under
// their prefixes. A leaf is an index into whatever is listed.
struct Trie {
  std::map<std::string, Trie> dirs;
  std::vector<std::pair<std::string, int>> leaves;  // the name's last part, and the index
  void add(std::string_view path, int index) {
    Trie* t = this;
    size_t at = 0;
    for (size_t slash; (slash = path.find('/', at)) != std::string_view::npos; at = slash + 1)
      t = &t->dirs[std::string(path.substr(at, slash - at))];
    t->leaves.push_back({std::string(path.substr(at)), index});
  }
  void all(std::vector<int>& out) const {
    for (const auto& [name, d] : dirs) d.all(out);
    for (const auto& [name, i] : leaves) out.push_back(i);
  }
};

// The repositories of the selected folder, as git and the agents see them,
// in five views switched with 1-5: Changes (what is not committed, by
// section and folder, each file with the chat that last changed it), Log
// (the focused branch's commits on their graph, the agents' marked),
// Branches (local and remote, grouped by prefix), Trees (the work trees and
// the agents at work in each) and, when there are several, Repos. Beside the
// list, whatever is selected: a diff, a commit, what is known of it. Nothing
// here writes to a repository.
class GitView final : public Pane {
 public:
  explicit GitView(GitTabState& st) : st_(st) {}
  std::string title() const override { return "Git"; }

  void render(Painter& p, bool focused) override {
    // The edits and commits by chat, and where each agent works, come from
    // the activity index: kept reading while the tab is shown.
    poll_activity(*app_, app_->workspace().activity(), next_pass_);
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.panel});
    const int W = p.width(), H = p.height();
    gather();
    draw_heading(p);
    int top = 1;
    strip_y_ = -1;
    if (!folders_mode_ && H > 4) {
      strip_y_ = 1;
      draw_strip(p, strip_y_);
      top = 2;
    }
    if (H < top + 2) return;
    p.hline(1, top, std::max(0, W - 2), U'─', Style{th.border, th.panel});
    top++;

    // The list, and the detail beside it or, on a narrow pane, below it. The
    // log's lines are long; the others' short.
    const bool side = W >= 110;
    const int share = view_ == View::Log ? 60 : 48;
    Rect list{0, top, side ? std::clamp(W * share / 100, 50, std::max(50, W - 40)) : W, H - top};
    Rect detail{};
    if (side) {
      detail = Rect{list.w + 1, top, W - list.w - 1, H - top};
      for (int y = top; y < H; y++) p.put(list.w, y, U'│', Style{th.border, th.panel, attr::kDecor});
    } else if (H - top > 12) {
      list.h = (H - top) / 2;
      detail = Rect{0, top + list.h + 1, W, H - top - 1 - list.h};
      p.hline(1, top + list.h, std::max(0, W - 2), U'─', Style{th.border, th.panel});
    }
    detail_rect_ = detail;
    list_rect_ = list;
    draw_list(p.sub(list), focused);
    if (detail.w > 4 && detail.h > 1) draw_detail(p.sub(detail));
  }

  // The rows are rebuilt at once, so the next key, typed or pasted ahead of
  // a frame, acts on what this one left.
  bool on_key(const KeyEvent& k) override {
    if (!key(k)) return false;
    gather();
    return true;
  }
  bool on_mouse(const MouseEvent& m, Point local) override {
    if (!click(m, local)) return false;
    gather();
    return true;
  }

  bool key(const KeyEvent& k) {
    if (k.key == Key::Char && !k.ctrl && !k.alt && k.ch >= '1' && k.ch <= '5' && !folders_mode_) {
      set_view(int(k.ch - '1'));
      return true;
    }
    switch (k.key) {
      case Key::Up: move(-1); return true;
      case Key::Down: move(+1); return true;
      case Key::Left: fold_or_up(); return true;
      case Key::Right: unfold_or_down(); return true;
      case Key::PageUp: detail_scroll_ = std::max(0, detail_scroll_ - std::max(1, detail_rect_.h - 2)); return true;
      case Key::PageDown: detail_scroll_ += std::max(1, detail_rect_.h - 2); return true;
      case Key::Home: detail_scroll_ = 0; return true;
      case Key::Enter: act(); return true;
      case Key::Escape:
        // Out of a branch's log, back to the work tree's own.
        if (view_ == View::Log && !st_.ref.empty()) {
          st_.ref.clear();
          st_.sel[View::Log].clear();
          return true;
        }
        return false;
      default: break;
    }
    if (k.is('k')) { move(-1); return true; }
    if (k.is('j')) { move(+1); return true; }
    if (k.is(' ')) { detail_scroll_ += std::max(1, detail_rect_.h - 2); return true; }
    if (k.is('r')) { app_->workspace().git().refresh_all(); return true; }
    if (k.is('a')) { app_->set_all_folders(!app_->all_folders()); return true; }
    if (k.is('g') && repos_.size() > 1) { st_.all_changes = !st_.all_changes; return true; }
    return false;
  }

  bool click(const MouseEvent& m, Point local) {
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
    if (local.y == strip_y_) {
      for (const auto& [x0, x1, v] : tab_spans_)
        if (local.x >= x0 && local.x < x1) set_view(v);
      return true;
    }
    for (const auto& [y, i] : shown_)
      if (y == local.y) {
        const Line& l = lines_[size_t(i)];
        // The fold mark folds at once; a second click on the selected row
        // goes where Enter would.
        if (l.foldable() && local.x <= 2 + l.depth * 2) {
          select(i);
          toggle(l.key);
        } else if (i == sel_) {
          act();
        } else {
          select(i);
        }
        return true;
      }
    return false;
  }

  std::vector<MenuItem> context_menu(Point) override {
    std::vector<MenuItem> items{MenuItem{"Read git again", "refresh"},
                                MenuItem{app_->all_folders() ? "Only this folder" : "All folders", "all"}};
    if (repos_.size() > 1)
      items.push_back(MenuItem{st_.all_changes ? "Changes of the focused repository" : "Changes of every repository", "changes"});
    return items;
  }
  void on_action(const std::string& a) override {
    if (a == "refresh") app_->workspace().git().refresh_all();
    else if (a == "all") app_->set_all_folders(!app_->all_folders());
    else if (a == "changes") st_.all_changes = !st_.all_changes;
  }

 private:
  enum class Kind {
    Heading, Note, Graph,            // not to be selected
    Section, Group, Dir,             // fold
    Folder, Repo, Worktree, Agent, Branch, Remote, Item, Commit,
  };
  struct Line {
    Kind kind;
    int index = -1;    // into the vector its kind is kept in; a fold's into members_
    int depth = 0;
    std::string text{};  // a heading's, note's or fold's; a leaf's name
    std::string key{};   // keeps the selection on it; names a fold in st_.toggled
    bool open = true;  // a fold: unfolded
    bool selectable() const { return kind != Kind::Heading && kind != Kind::Note && kind != Kind::Graph; }
    bool foldable() const { return kind == Kind::Section || kind == Kind::Group || kind == Kind::Dir; }
  };
  struct RepoRow {
    std::string top;
    int parent = -1;  // the repository it is inside of
    int depth = 0;    // how many it is inside of
    std::vector<LiveSession*> live;  // the agents at work in it
  };
  struct AgentRow {
    LiveSession* live;
    int worktree;
    bool away = false;  // works in this work tree, but runs outside it
  };
  struct FolderRow {
    int project;
    const GitStatus* st;
  };
  struct FileRow {
    GitStatus::Entry e;
    std::string wt;          // the work tree its path is from
    std::string root;        // that work tree's top, to make the path absolute
    int64_t head_time = 0;   // when that work tree's HEAD was committed
    int owner = -1;          // the repository it is in
    int repo = -1;           // a repository of its own, under Repos
    const ChatActivity* chat = nullptr;  // the chat that last changed it since HEAD
    const FileEdit* edit = nullptr;
  };
  // A file as one section lists it: staged and changed again, it is in two.
  struct FileItem {
    int file;
    int section;
    char letter;  // M, A, D, R, U (untracked), ! (in conflict)
    GitNumstat n{};
    bool counted = false;
  };

  GitIndex& git() { return app_->workspace().git(); }

  // ------------------------------------------------------------ the model

  // The work tree holding `path`: the deepest whose folder contains it, so a
  // work tree inside the main one wins.
  int worktree_of(const std::string& path) const {
    int best = -1;
    for (size_t i = 0; i < worktrees_.size(); i++)
      if (under(path, worktrees_[i].path) && (best < 0 || worktrees_[i].path.size() > worktrees_[size_t(best)].path.size()))
        best = int(i);
    return best;
  }

  // The repository holding `path`: the one whose work tree it is in, or
  // whose work tree that is (one `git worktree add` made, beside the main
  // one), else the deepest whose top contains it.
  int repo_of(const std::string& path) {
    if (const std::string& t = git().top_of(path); !t.empty()) {
      for (size_t i = 0; i < repos_.size(); i++)
        if (repos_[i].top == t) return int(i);
      const std::string& m = git().main_of(t);
      for (size_t i = 0; i < repos_.size(); i++)
        if (repos_[i].top == m) return int(i);
    }
    int best = -1;
    for (size_t i = 0; i < repos_.size(); i++)
      if (under(path, repos_[i].top) && (best < 0 || repos_[i].top.size() > repos_[size_t(best)].top.size()))
        best = int(i);
    return best;
  }

  // Where agent `s` works: the newest folder it worked in that is in a
  // repository or work tree listed, else where it runs. An agent started in
  // the main checkout is often at work in another work tree.
  std::string place_of(LiveSession* s) {
    for (const std::string& d : app_->workspace().work_dirs(*s))
      if (repo_of(d) >= 0 || worktree_of(d) >= 0) return d;
    return s->cwd();
  }

  // `path` as seen from folder `from`: "web/api", or the folder's own name,
  // or, outside it, the whole path.
  static std::string rel(const std::string& path, const std::string& from) {
    if (path == from) return path.substr(path.rfind('/') + 1);
    if (under(path, from)) return path.substr(from.size() + 1);
    return home_path(path);
  }
  // A repository's name: its path from the one it is inside of, or with
  // `whole`, from the folder shown.
  std::string repo_label(int i, bool whole) const {
    const RepoRow& r = repos_[size_t(i)];
    return rel(r.top, !whole && r.parent >= 0 ? repos_[size_t(r.parent)].top : dir_);
  }
  // The branch the focused work tree has checked out; empty when detached.
  const std::string& wt_branch() const {
    static const std::string none;
    for (const GitWorktree& w : worktrees_)
      if (w.path == st_.wt) return w.branch;
    return none;
  }

  void gather() {
    lines_.clear();
    members_.clear();
    repos_.clear();
    worktrees_.clear();
    worktree_made_.clear();
    agents_.clear();
    branches_.clear();
    branch_made_.clear();
    remotes_.clear();
    files_.clear();
    items_.clear();
    item_groups_.clear();
    graph_.clear();
    log_.clear();
    folders_.clear();
    made_.clear();
    top_.clear();
    state_note_.clear();
    wt_status_ = nullptr;
    log_read_ = false;
    focus_ = -1;
    folders_mode_ = false;
    view_ = st_.view;
    const App::ViewFilter vf = app_->view_filter();
    if (!vf.project) {
      folders_mode_ = true;
      return gather_all();
    }
    const std::string dir = vf.sub ? vf.sub->path : vf.project->path;
    if (st_.scope != dir) {
      st_.scope = dir;
      st_.repo.clear();
      st_.wt.clear();
      st_.ref.clear();
      for (std::string& s : st_.sel) s.clear();
    }
    dir_ = dir;
    const GitRepos& found = app_->workspace().repos_in(dir);
    if (found.tops.empty())
      return note_only(found.looking ? "looking for repositories in " + home_path(dir) + "\xE2\x80\xA6"
                                     : home_path(dir) + " is in no git repository, and holds none");

    // The repositories: each with the one it is inside of, and the agents
    // at work in it.
    for (const std::string& t : found.tops) repos_.push_back(RepoRow{t, -1, 0, {}});
    for (size_t i = 0; i < repos_.size(); i++)
      for (size_t j = 0; j < repos_.size(); j++)
        if (j != i && under(repos_[i].top, repos_[j].top)) {
          RepoRow& r = repos_[i];
          r.depth++;
          if (r.parent < 0 || repos_[j].top.size() > repos_[size_t(r.parent)].top.size()) r.parent = int(j);
        }
    if (view_ == View::Repos && repos_.size() < 2) view_ = View::Changes;
    const std::vector<LiveSession*> live = app_->live_sessions();
    for (LiveSession* s : live)
      if (!s->exited())
        if (const int i = repo_of(place_of(s)); i >= 0) repos_[size_t(i)].live.push_back(s);

    // The focused repository: the one chosen, else the one holding the chat
    // the sidebar selects, else the selected folder's, else the first with
    // something uncommitted.
    std::string want = dir;
    if (!vf.chat_path.empty())
      for (const ChatActivity* c : app_->workspace().activity().chats())
        if (c->path == vf.chat_path) want = c->cwd;
    for (size_t i = 0; i < repos_.size(); i++)
      if (repos_[i].top == st_.repo) focus_ = int(i);
    if (focus_ < 0) focus_ = repo_of(want);
    for (size_t i = 0; focus_ < 0 && i < repos_.size(); i++)
      if (const GitStatus* s = git().status(repos_[i].top); s && s->files) focus_ = int(i);
    if (focus_ < 0) focus_ = 0;
    const std::string rtop = repos_[size_t(focus_)].top;
    top_ = rtop;
    const GitStatus* st = git().status(rtop);
    if (!st) state_note_ = "reading git\xE2\x80\xA6";
    else if (!st->repo) state_note_ = home_path(rtop) + " is no git work tree now";
    if (!state_note_.empty()) return build_lines();

    // Work trees: what git lists, or this one alone until it has answered.
    if (const auto* q = git().query(rtop, {"worktree", "list", "--porcelain"}); q && q->ok)
      worktrees_ = parse_worktrees(q->out);
    std::erase_if(worktrees_, [](const GitWorktree& w) { return w.bare; });
    if (worktrees_.empty())
      worktrees_.push_back(GitWorktree{st->top.empty() ? rtop : st->top, st->head, st->branch, false, false, false});

    // The focused work tree: the one chosen, else the one holding the chat
    // the sidebar selects, else the selected folder's.
    int focus = -1;
    for (size_t i = 0; i < worktrees_.size(); i++)
      if (worktrees_[i].path == st_.wt) focus = int(i);
    if (focus < 0) focus = std::max(0, worktree_of(want));
    st_.wt = worktrees_[size_t(focus)].path;
    wt_status_ = git().status(st_.wt);

    // Who is working where: each agent where it last worked, when that is a
    // work tree of this repository and not a repository inside one.
    for (LiveSession* s : live) {
      if (s->exited()) continue;
      const std::string place = place_of(s);
      const int w = worktree_of(place);
      if (w < 0) continue;
      if (const int r = repo_of(place); r >= 0 && r != focus_) continue;
      agents_.push_back(AgentRow{s, w, !under(s->cwd(), worktrees_[size_t(w)].path)});
    }

    // Which chats committed what, by the hash they printed: those that ran
    // in the folder, in one above it, or in a work tree of the repository,
    // and only the commits git finds in it.
    std::vector<Made> announced;
    std::vector<std::string> args{"rev-list", "--no-walk", "--ignore-missing"};
    for (const ChatActivity* c : app_->workspace().activity().chats()) {
      if (c->commits.empty() || (!under(c->cwd, dir) && !under(dir, c->cwd) && worktree_of(c->cwd) < 0)) continue;
      for (const ChatCommit& cm : c->commits) {
        announced.push_back(Made{c, &cm});
        args.push_back(cm.hash);
      }
    }
    std::unordered_set<std::string> present;  // abbreviated
    if (!announced.empty())
      if (const auto* q = git().query(rtop, args); q && q->ok)
        for (size_t i = 0; i < q->out.size();) {
          size_t e = q->out.find('\n', i);
          if (e == std::string::npos) e = q->out.size();
          if (e - i >= 7) present.insert(q->out.substr(i, 7));
          i = e + 1;
        }
    std::map<std::string, Made> last_on_branch;
    for (const Made& m : announced) {
      if (!present.count(m.commit->hash.substr(0, 7))) continue;
      made_[m.commit->hash.substr(0, 7)] = m;
      if (m.commit->branch.empty()) continue;
      Made& last = last_on_branch[m.commit->branch];
      if (!last.commit || last.commit->at_ms < m.commit->at_ms) last = m;
    }

    // Branches: the local ones always (the strip counts them), the remote
    // ones while they are shown.
    if (const auto* q = git().query(rtop, {"for-each-ref", kBranchFormat, "refs/heads"}); q && q->ok)
      branches_ = parse_branches(q->out);
    for (const GitBranch& b : branches_) {
      auto it = last_on_branch.find(b.name);
      branch_made_.push_back(it == last_on_branch.end() ? Made{} : it->second);
    }
    worktree_made_.resize(worktrees_.size());
    for (size_t i = 0; i < worktrees_.size(); i++) {
      auto it = last_on_branch.find(worktrees_[i].branch);
      worktree_made_[i] = it == last_on_branch.end() ? Made{} : it->second;
    }
    if (view_ == View::Branches)
      if (const auto* q = git().query(rtop, {"for-each-ref", kBranchFormat, "refs/remotes"}); q && q->ok)
        for (GitBranch& b : parse_branches(q->out))
          // "origin" alone is origin/HEAD, a pointer to one of the others.
          if (b.name.find('/') != std::string::npos && !b.name.ends_with("/HEAD")) remotes_.push_back(std::move(b));

    // What is not committed: in the focused work tree, or in every
    // repository's, and which chat changed each file since its HEAD's commit.
    if (st_.all_changes && repos_.size() > 1) {
      for (size_t i = 0; i < repos_.size(); i++) add_files(int(i) == focus_ ? st_.wt : repos_[i].top, int(i));
    } else {
      add_files(st_.wt, focus_);
    }
    credit_files();

    // The log, on its graph, while it is shown.
    if (view_ == View::Log) {
      const std::string ref = st_.ref.empty() ? "HEAD" : st_.ref;
      if (const auto* q = git().query(st_.wt, {"log", "--graph", kGraphFormat, "-n", "200", ref, "--"}); q) {
        log_read_ = true;
        if (q->ok) parse_graph_log(q->out, graph_, log_);
      }
      graph_w_ = 0;
      for (const GitGraphRow& g : graph_) graph_w_ = std::max(graph_w_, int(g.graph.size()));
      graph_w_ = std::min(graph_w_, 24);
    }
    build_lines();
  }

  // What work tree `wt`, of repository `owner`, has not committed: each file
  // in the sections git has it in, with the lines each section changes.
  void add_files(const std::string& wt, int owner) {
    item_groups_.push_back({owner, items_.size()});
    const GitStatus* wst = git().status(wt);
    if (!wst) return;
    const std::string root = wst->top.empty() ? wt : wst->top;
    int64_t head_time = 0;
    if (const auto* q = git().query(wt, {"log", kLogFormat, "-n", "1", "HEAD", "--"}); q && q->ok) {
      const auto l = parse_log(q->out);
      if (!l.empty()) head_time = l[0].time;
    }
    const size_t from = items_.size();
    for (const GitStatus::Entry& e : wst->entries) {
      FileRow f{e, wt, root, head_time, owner};
      // A submodule, or a repository git sees only as an untracked folder:
      // one of the repositories listed.
      if (!e.sub.empty() || (e.x == '?' && e.path.ends_with('/'))) {
        std::string path = root + "/" + e.path;
        if (path.ends_with('/')) path.pop_back();
        for (size_t i = 0; i < repos_.size(); i++)
          if (repos_[i].top == path) f.repo = int(i);
      }
      files_.push_back(std::move(f));
      const int fi = int(files_.size()) - 1;
      if (e.x == 'U' && e.y == 'U') {
        items_.push_back(FileItem{fi, kConflicts, '!'});
      } else if (e.x == '?') {
        items_.push_back(FileItem{fi, kUntracked, 'U'});
      } else {
        if (e.x != '.') items_.push_back(FileItem{fi, kStaged, e.x});
        if (e.y != '.') items_.push_back(FileItem{fi, kUnstaged, e.y});
      }
    }
    const auto count = [&](std::vector<std::string> args, int section) {
      if (const auto* q = git().query(wt, args); q && q->ok)
        for (const auto& [path, n] : parse_numstat(q->out))
          for (size_t i = from; i < items_.size(); i++)
            if (items_[i].section == section && files_[size_t(items_[i].file)].e.path == path) {
              items_[i].n = n;
              items_[i].counted = true;
            }
    };
    count({"diff", "--no-ext-diff", "--no-textconv", "--numstat", "-z", "--cached"}, kStaged);
    count({"diff", "--no-ext-diff", "--no-textconv", "--numstat", "-z"}, kUnstaged);
  }

  // Which chat last changed each file since its HEAD's commit, from the
  // agents' own edit records.
  void credit_files() {
    files_who_ = false;
    if (files_.empty()) return;
    std::unordered_map<std::string, FileRow*> by_path;
    for (FileRow& f : files_) by_path[f.root + "/" + f.e.path] = &f;
    for (const ChatActivity* c : app_->workspace().activity().chats())
      for (const FileEdit& e : c->edits) {
        auto it = by_path.find(e.file);
        if (it == by_path.end()) continue;
        FileRow& f = *it->second;
        if (e.at_ms / 1000 < f.head_time) continue;
        if (!f.edit || f.edit->at_ms < e.at_ms) {
          f.edit = &e;
          f.chat = c;
        }
      }
    files_who_ = std::any_of(files_.begin(), files_.end(), [](const FileRow& f) { return f.chat != nullptr; });
  }

  // With every folder in the filter: each tracked folder's branch and state,
  // and who is working there.
  void gather_all() {
    const auto& projects = app_->store().projects();
    lines_.push_back(Line{Kind::Heading, -1, 0, "Folders"});
    for (size_t i = 0; i < projects.size(); i++) {
      folders_.push_back(FolderRow{int(i), git().status(projects[i].path)});
      lines_.push_back(Line{Kind::Folder, int(folders_.size()) - 1, 0, {}, "f" + projects[i].path});
    }
    lines_.push_back(Line{Kind::Note, -1, 0, {}});
    lines_.push_back(Line{Kind::Note, -1, 0, "enter narrows to a folder: its changes, log, branches and work trees"});
    fix_selection();
  }

  void note_only(std::string text) {
    lines_.push_back(Line{Kind::Note, -1, 0, std::move(text)});
    fix_selection();
  }

  // ------------------------------------------------------------ the lines

  bool is_open(const std::string& key, bool by_default) const { return by_default != (st_.toggled.count(key) > 0); }
  void toggle(const std::string& key) {
    if (!st_.toggled.erase(key)) st_.toggled.insert(key);
  }

  // A fold: a section, a repository's group or a folder, and what is in it
  // (indices into whatever it lists). True when it is unfolded.
  bool fold(Kind kind, int depth, std::string text, std::string key, std::vector<int> members, bool by_default = true) {
    const bool open = is_open(key, by_default);
    members_.push_back(std::move(members));
    lines_.push_back(Line{kind, int(members_.size()) - 1, depth, std::move(text), std::move(key), open});
    return open;
  }

  // The lines of tree `t`, folders first, a folder holding only one folder
  // shown as one ("src/core"), then its leaves by name.
  template <class Leaf>
  void tree(const Trie& t, int depth, const std::string& key, const std::string& path, const Leaf& leaf) {
    for (const auto& [name, sub] : t.dirs) {
      std::string label = name;
      const Trie* d = &sub;
      while (d->leaves.empty() && d->dirs.size() == 1) {
        label += "/" + d->dirs.begin()->first;
        d = &d->dirs.begin()->second;
      }
      const std::string full = path.empty() ? label : path + "/" + label;
      std::vector<int> in;
      d->all(in);
      if (fold(Kind::Dir, depth, label, key + full, std::move(in))) tree(*d, depth + 1, key, full, leaf);
    }
    std::vector<const std::pair<std::string, int>*> leaves;
    for (const auto& l : t.leaves) leaves.push_back(&l);
    std::sort(leaves.begin(), leaves.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
    for (const auto* l : leaves) leaf(l->second, depth, l->first);
  }

  void build_lines() {
    if (view_ == View::Repos) build_repos();  // even while the focused one is being read
    else if (!state_note_.empty()) lines_.push_back(Line{Kind::Note, -1, 0, state_note_});
    else if (view_ == View::Changes) build_changes();
    else if (view_ == View::Log) build_log();
    else if (view_ == View::Branches) build_branches();
    else build_trees();
    fix_selection();
  }

  // Conflicts, Staged, Changes, Untracked: each a tree of folders. With
  // every repository's, each under its own.
  void build_changes() {
    if (items_.empty()) {
      lines_.push_back(Line{Kind::Note, -1, 0, wt_status_ ? "nothing uncommitted" : "reading git\xE2\x80\xA6"});
      return;
    }
    const bool every = st_.all_changes && repos_.size() > 1;
    for (size_t g = 0; g < item_groups_.size(); g++) {
      const int owner = item_groups_[g].first;
      const size_t from = item_groups_[g].second;
      const size_t to = g + 1 < item_groups_.size() ? item_groups_[g + 1].second : items_.size();
      if (from == to) continue;
      const std::string& top = repos_[size_t(owner)].top;
      int depth = 0;
      if (every) {
        std::vector<int> in;
        for (size_t i = from; i < to; i++) in.push_back(int(i));
        if (!fold(Kind::Group, 0, repo_label(owner, true), "g" + top, std::move(in))) continue;
        depth = 1;
      }
      for (int sec = 0; sec < kSections; sec++) {
        Trie t;
        std::vector<int> in;
        for (size_t i = from; i < to; i++)
          if (items_[i].section == sec) {
            std::string path = files_[size_t(items_[i].file)].e.path;
            if (path.ends_with('/')) path.pop_back();
            t.add(path, int(i));
            in.push_back(int(i));
          }
        if (in.empty()) continue;
        const std::string skey = top + "\n" + std::to_string(sec);
        if (!fold(Kind::Section, depth, kSectionNames[sec], "s" + skey, std::move(in))) continue;
        tree(t, depth + 1, "d" + skey + "\n", "", [&](int i, int d, const std::string& name) {
          const FileRow& f = files_[size_t(items_[size_t(i)].file)];
          lines_.push_back(Line{Kind::Item, i, d, name, "i" + skey + "\n" + f.e.path});
        });
      }
    }
  }

  // The commits on their graph, under what they are the commits of.
  void build_log() {
    const std::string what = st_.ref.empty() ? (wt_branch().empty() ? std::string("HEAD") : wt_branch() + " \xC2\xB7 HEAD")
                                             : st_.ref + " \xC2\xB7 esc: back to HEAD";
    lines_.push_back(Line{Kind::Heading, -1, 0, what});
    if (log_.empty()) {
      lines_.push_back(Line{Kind::Note, -1, 0, log_read_ ? "no commits" : "reading the log\xE2\x80\xA6"});
      return;
    }
    for (size_t i = 0; i < graph_.size(); i++) {
      const GitGraphRow& g = graph_[i];
      if (g.commit >= 0) lines_.push_back(Line{Kind::Commit, int(i), 0, {}, "h" + log_[size_t(g.commit)].hash});
      else lines_.push_back(Line{Kind::Graph, int(i), 0});
    }
  }

  // Local branches and remote ones, each grouped by its prefixes.
  void build_branches() {
    Trie local;
    std::vector<int> all;
    for (size_t i = 0; i < branches_.size(); i++) {
      local.add(branches_[i].name, int(i));
      all.push_back(int(i));
    }
    if (fold(Kind::Section, 0, "Local", "bL", std::move(all)))
      tree(local, 1, "bl", "", [&](int i, int d, const std::string& name) {
        lines_.push_back(Line{Kind::Branch, i, d, name, "b" + branches_[size_t(i)].name});
      });
    if (remotes_.empty()) return;
    Trie remote;
    all.clear();
    for (size_t i = 0; i < remotes_.size(); i++) {
      remote.add(remotes_[i].name, int(i));
      all.push_back(int(i));
    }
    if (fold(Kind::Section, 0, "Remote", "bR", std::move(all), false))
      tree(remote, 1, "br", "", [&](int i, int d, const std::string& name) {
        lines_.push_back(Line{Kind::Remote, i, d, name, "R" + remotes_[size_t(i)].name});
      });
  }

  // The work trees, each with the agents at work in it.
  void build_trees() {
    for (size_t i = 0; i < worktrees_.size(); i++) {
      lines_.push_back(Line{Kind::Worktree, int(i), 0, {}, "w" + worktrees_[i].path});
      for (size_t a = 0; a < agents_.size(); a++)
        if (agents_[a].worktree == int(i))
          lines_.push_back(Line{Kind::Agent, int(a), 1, {}, "a" + std::to_string(agents_[a].live->serial())});
    }
  }

  void build_repos() {
    for (size_t i = 0; i < repos_.size(); i++)
      lines_.push_back(Line{Kind::Repo, int(i), repos_[i].depth, {}, "r" + repos_[i].top});
  }

  // ------------------------------------------------------------ selection

  std::string& sel_key() { return st_.sel[folders_mode_ ? int(View::Changes) : view_]; }

  // The selection stays on its row as rows come and go; until one is
  // chosen, it rests on the first that can be.
  void fix_selection() {
    sel_ = -1;
    const std::string& key = sel_key();
    for (size_t i = 0; i < lines_.size(); i++)
      if (lines_[i].selectable() && lines_[i].key == key) sel_ = int(i);
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
    sel_key() = lines_[size_t(i)].key;
    detail_scroll_ = 0;
  }

  void move(int d) {
    for (int i = sel_ + d; i >= 0 && i < int(lines_.size()); i += d)
      if (lines_[size_t(i)].selectable()) {
        select(i);
        return;
      }
  }

  void set_view(int v) {
    if (v < 0 || v >= GitTabState::kViews || (v == View::Repos && repos_.size() < 2)) return;
    st_.view = v;
    detail_scroll_ = 0;
    scroll_ = 0;
  }

  // ←: fold what is unfolded, else up to the fold it is in.
  void fold_or_up() {
    if (sel_ < 0) return;
    const Line& l = lines_[size_t(sel_)];
    if (l.foldable() && l.open) return toggle(l.key);
    for (int i = sel_ - 1; i >= 0; i--)
      if (lines_[size_t(i)].foldable() && lines_[size_t(i)].depth < l.depth) return select(i);
  }
  // →: unfold what is folded, else down into it.
  void unfold_or_down() {
    if (sel_ < 0) return;
    const Line& l = lines_[size_t(sel_)];
    if (!l.foldable()) return;
    if (!l.open) toggle(l.key);
    else move(+1);
  }

  void focus_repo(int i) {
    st_.repo = repos_[size_t(i)].top;
    st_.wt.clear();
    st_.ref.clear();
  }

  void act() {
    if (sel_ < 0 || sel_ >= int(lines_.size())) return;
    const Line& l = lines_[size_t(sel_)];
    if (l.foldable()) return toggle(l.key);
    switch (l.kind) {
      case Kind::Folder:
        app_->select_project(folders_[size_t(l.index)].project);
        app_->set_all_folders(false);
        break;
      case Kind::Repo:
        focus_repo(l.index);
        set_view(View::Changes);
        break;
      case Kind::Worktree:
        st_.wt = worktrees_[size_t(l.index)].path;
        st_.ref.clear();
        set_view(View::Changes);
        break;
      case Kind::Agent:
        app_->open_live(agents_[size_t(l.index)].live);
        break;
      case Kind::Branch:
      case Kind::Remote: {
        // Its log; the checked-out branch's is the work tree's own.
        const std::string& name = (l.kind == Kind::Branch ? branches_ : remotes_)[size_t(l.index)].name;
        st_.ref = name == wt_branch() ? std::string() : name;
        st_.sel[View::Log].clear();
        set_view(View::Log);
        break;
      }
      case Kind::Item: {
        const FileRow& f = files_[size_t(items_[size_t(l.index)].file)];
        if (f.repo >= 0) {
          // A repository of its own: focused, its changes listed.
          focus_repo(f.repo);
          st_.sel[View::Changes].clear();
        } else if (f.chat) {
          app_->open_at(f.chat->path, f.edit->call_offset, {});
        } else {
          app_->set_status(f.e.path + ": no chat mico knows changed it since the last commit");
        }
        break;
      }
      case Kind::Commit: {
        const GitLogEntry& e = log_[size_t(graph_[size_t(l.index)].commit)];
        if (!app_->open_commit(e.hash))
          app_->set_status(e.hash.substr(0, 7) + " by " + e.author + ": not a commit any chat mico knows made");
        break;
      }
      default: break;
    }
  }

  // --------------------------------------------------------- heading, strip

  void draw_heading(Painter& p) {
    const Theme& th = app_->theme();
    const int W = p.width();
    int x = 1 + p.text(1, 0, "Git", Style{th.text, th.panel, attr::kBold}) + 2;
    std::string where = app_->view_filter().label;
    if (repos_.size() > 1 && focus_ >= 0) where += " \xE2\x80\xBA " + repo_label(focus_, true);
    x += p.text_clipped(x, 0, where, Style{th.accent, th.panel}, std::max(0, W - x - 1));
    // The focused work tree: its branch, its state, the agents in it.
    if (wt_status_ && wt_status_->repo) {
      x += p.text(x, 0, " \xC2\xB7 ", Style{th.dim, th.panel});
      const std::string& b = !wt_status_->branch.empty() ? wt_status_->branch : wt_status_->head;
      x += p.text_clipped(x, 0, b.empty() ? "no commits" : b, Style{th.text, th.panel, attr::kBold}, std::max(0, W - x - 1)) + 1;
      x += draw_state(p, x, 0, wt_status_, th.panel, std::max(0, std::min(24, W - x - 1)));
      if (!agents_.empty()) {
        std::optional<ChatState> best;
        for (const AgentRow& a : agents_) {
          const ChatState cs = chat_state(a.live, th, app_->anim());
          if (!best || cs.rank < best->rank) best = cs;
        }
        if (x + 4 < W) {
          p.put(x + 1, 0, best->glyph, Style{best->color, th.panel, attr::kBold});
          x += 3 + p.text(x + 3, 0, std::to_string(agents_.size()) + (agents_.size() == 1 ? " agent" : " agents"),
                          Style{th.dim, th.panel});
        }
      }
    }
    // The keys, right-aligned, as far as there is room.
    std::string keys = folders_mode_ ? "enter go \xC2\xB7 r read again \xC2\xB7 a this folder"
                                     : "1-" + std::to_string(repos_.size() > 1 ? 5 : 4) +
                                           " views \xC2\xB7 \xE2\x86\x90\xE2\x86\x92 fold \xC2\xB7 enter go \xC2\xB7 r read again";
    if (!folders_mode_ && repos_.size() > 1) keys += st_.all_changes ? " \xC2\xB7 g one repo" : " \xC2\xB7 g every repo";
    const int kw = text::str_width(keys);
    if (W - kw - 1 > x + 2) p.text(W - kw - 1, 0, keys, Style{th.dim, th.panel});
  }

  // The views, the one shown lit: "1 Changes 18  2 Log  3 Branches 10 …".
  void draw_strip(Painter& p, int y) {
    const Theme& th = app_->theme();
    tab_spans_.clear();
    int x = 1;
    for (int v = 0; v < GitTabState::kViews; v++) {
      if (v == View::Repos && repos_.size() < 2) continue;
      const size_t n = v == View::Changes ? items_.size() : v == View::Branches ? branches_.size()
                       : v == View::Trees ? worktrees_.size() : v == View::Repos ? repos_.size() : 0;
      const bool on = v == view_;
      const Color bg = on ? th.sel_bg : th.panel;
      const int x0 = x;
      x += p.text(x, y, " ", Style{th.text, bg});
      x += p.text(x, y, std::to_string(v + 1), Style{on ? th.accent : th.border, bg, attr::kBold}) + 1;
      x += p.text(x, y, kViewNames[v], Style{on ? th.text : th.dim, bg, on ? attr::kBold : uint16_t(0)});
      if (n) x += p.text(x, y, " " + std::to_string(n), Style{on ? th.accent : th.dim, bg});
      x += p.text(x, y, " ", Style{th.text, bg});
      tab_spans_.push_back({x0, x, v});
      x += 1;
    }
  }

  // --------------------------------------------------------------- the list

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
      if (sel) p.put(0, r, U'▌', Style{th.accent, bg, attr::kDecor});
      if (l.selectable()) shown_.push_back({r + list_rect_.y, i});
      const Style text{th.text, bg, sel ? attr::kBold : uint16_t(0)}, dim{th.dim, bg};
      switch (l.kind) {
        case Kind::Heading: p.text_clipped(1, r, l.text, Style{th.heading, bg, attr::kBold}, W - 2); break;
        case Kind::Note: p.text_clipped(3, r, l.text, dim, W - 4); break;
        case Kind::Section:
        case Kind::Group:
        case Kind::Dir: draw_fold(p, r, l, sel, bg); break;
        case Kind::Folder: draw_folder(p, r, folders_[size_t(l.index)], text, dim, bg); break;
        case Kind::Repo: draw_repo(p, r, l.index, text, dim, bg); break;
        case Kind::Worktree: draw_worktree(p, r, l.index, text, dim, bg); break;
        case Kind::Agent: draw_agent(p, r, agents_[size_t(l.index)], text, dim, bg); break;
        case Kind::Branch: draw_branch(p, r, l, text, dim, bg); break;
        case Kind::Remote: draw_remote(p, r, l, text, dim, bg); break;
        case Kind::Item: draw_item(p, r, l, text, dim, bg); break;
        case Kind::Graph:
        case Kind::Commit: draw_commit(p, r, l, text, dim, bg); break;
      }
    }
  }

  // "±3 !1 ↑1 ↓2", or "clean"; returns the columns used.
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

  // A section, a repository's group or a folder: its mark, name and count.
  void draw_fold(Painter& p, int r, const Line& l, bool sel, Color bg) {
    const Theme& th = app_->theme();
    const int W = p.width();
    const int x0 = 1 + l.depth * 2;
    p.put(x0, r, l.open ? U'▾' : U'▸', Style{th.dim, bg});
    const Style name = l.kind == Kind::Section ? Style{th.heading, bg, attr::kBold}
                       : l.kind == Kind::Group ? Style{th.accent, bg, attr::kBold}
                                               : Style{th.text, bg, sel ? attr::kBold : uint16_t(0)};
    int x = x0 + 2 + p.text_clipped(x0 + 2, r, l.text, name, std::max(0, W - x0 - 10));
    x += 1 + p.text(x + 1, r, std::to_string(members_[size_t(l.index)].size()), Style{th.dim, bg});
    if (l.kind == Kind::Group) {
      const int first = items_[size_t(members_[size_t(l.index)].front())].file;
      const GitStatus* st = git().status(files_[size_t(first)].wt);
      if (st && st->repo) {
        x += 2;
        x += p.text_clipped(x, r, st->branch.empty() ? st->head : st->branch, Style{th.dim, bg}, std::max(0, W - x - 1));
      }
    }
  }

  // A file in a section: git's letter, its name, the lines it changes and
  // the chat that changed it last.
  void draw_item(Painter& p, int r, const Line& l, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const int W = p.width();
    const FileItem& it = items_[size_t(l.index)];
    const FileRow& f = files_[size_t(it.file)];
    const int x0 = 3 + l.depth * 2;  // in line with the names of the folders beside it
    const char c = it.letter;
    const Color col = c == 'U' || c == 'A' ? th.added : c == 'D' ? th.removed : c == '!' ? th.err
                      : c == 'R' || c == 'C' ? th.accent : th.warn;
    p.text(x0, r, std::string(1, c), Style{col, bg, attr::kBold});
    std::string counts;
    if (f.repo >= 0) counts = f.e.sub.empty() ? "repository" : "submodule";
    else if (it.counted) counts = it.n.added < 0 ? "binary" : plus_minus(it.n.added, it.n.removed);
    else if (it.section == kUntracked) counts = "new";
    // Right to left: the chat, the counts, then the name in what is left.
    const int who_w = files_who_ ? std::clamp(W / 3, 12, 36) : 0;
    const int counts_end = W - 1 - (who_w ? who_w + 2 : 0);
    const int cw = text::str_width(counts);
    const int name_room = std::max(0, counts_end - cw - 2 - (x0 + 2));
    std::string name = l.text + (f.e.path.ends_with('/') ? "/" : "");
    int x = x0 + 2 + p.text_clipped(x0 + 2, r, name, text, name_room);
    if (!f.e.orig.empty())
      p.text_clipped(x + 1, r, "\xE2\x86\x90 " + f.e.orig, dim, std::max(0, x0 + 2 + name_room - x - 1));
    if (!counts.empty()) {
      const int cx = counts_end - cw;
      const size_t sp = counts.find(' ');
      if (counts[0] == '+' || counts.starts_with("\xE2\x88\x92")) {
        const std::string first = counts.substr(0, sp);
        const int fw = p.text(cx, r, first, Style{first[0] == '+' ? th.added : th.removed, bg});
        if (sp != std::string::npos) p.text(cx + fw + 1, r, counts.substr(sp + 1), Style{th.removed, bg});
      } else {
        p.text(cx, r, counts, dim);
      }
    }
    if (f.chat) p.text_clipped(W - who_w - 1, r, chat_label(*f.chat), Style{th.accent, bg}, who_w);
  }

  // The graph, one lane colour per column; a commit's dot, the agent's a ◆.
  void draw_graph(Painter& p, int r, const std::string& g, bool agent, Color bg) {
    const Theme& th = app_->theme();
    const Color lanes[] = {th.ansi[4], th.ansi[5], th.ansi[6], th.ansi[3], th.ansi[2], th.ansi[1]};
    for (int i = 0; i < int(g.size()) && i < graph_w_; i++) {
      const char c = g[size_t(i)];
      const Color lane = lanes[size_t(i / 2) % std::size(lanes)];
      char32_t cp = 0;
      switch (c) {
        case '*': cp = agent ? U'◆' : U'●'; break;
        case '|': cp = U'│'; break;
        case '/': cp = U'╱'; break;
        case '\\': cp = U'╲'; break;
        case '_': cp = U'_'; break;
        case '-': cp = U'─'; break;
        case '.': cp = U'·'; break;
        default: continue;
      }
      p.put(1 + i, r, cp, Style{c == '*' && agent ? th.accent : lane, bg, c == '*' ? attr::kBold : uint16_t(0)});
    }
  }

  // A commit: its graph, hash, subject, where branches point, who and when.
  void draw_commit(Painter& p, int r, const Line& l, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const int W = p.width();
    const GitGraphRow& g = graph_[size_t(l.index)];
    const GitLogEntry* e = g.commit >= 0 ? &log_[size_t(g.commit)] : nullptr;
    const auto made = e ? made_.find(e->hash.substr(0, 7)) : made_.end();
    const bool agent = made != made_.end();
    draw_graph(p, r, g.graph, agent, bg);
    if (!e) return;
    int x = 2 + graph_w_;
    x += p.text(x, r, e->hash.substr(0, 7), Style{th.hunk, bg}) + 1;
    const std::string when = ago(e->time);
    const std::string who = agent ? chat_label(*made->second.chat) : e->author;
    // Who and when in columns of their own, so the subjects line up.
    const int who_w = std::clamp(W / 4, 12, 28);
    const int end = W - 1 - kWhen - 1 - who_w - 2;  // where the subject and badges stop

    // Where branches and tags point, as badges after the subject.
    struct Badge {
      std::string name;
      Style style;
    };
    std::vector<Badge> badges;
    int badges_w = 0;
    for (size_t at = 0; at < e->refs.size();) {
      size_t comma = e->refs.find(", ", at);
      if (comma == std::string::npos) comma = e->refs.size();
      std::string ref = e->refs.substr(at, comma - at);
      at = comma + 2;
      Style s{th.accent, th.strip_bg};
      if (ref.starts_with("HEAD -> ")) {
        ref = ref.substr(8);
        s = Style{th.panel, th.accent, attr::kBold};
      } else if (ref == "HEAD") {
        s = Style{th.panel, th.accent, attr::kBold};
      } else if (ref.starts_with("tag: ")) {
        ref = ref.substr(5);
        s = Style{th.warn, th.strip_bg};
      } else if (std::none_of(branches_.begin(), branches_.end(), [&](const GitBranch& b) { return b.name == ref; })) {
        s = Style{th.dim, th.strip_bg};  // a remote's
      }
      badges_w += text::str_width(ref) + 3;
      badges.push_back(Badge{std::move(ref), s});
    }
    // The subject keeps at least half its room; badges that do not fit go.
    const int room = std::max(0, end - x);
    while (!badges.empty() && badges_w > room / 2) {
      badges_w -= text::str_width(badges.back().name) + 3;
      badges.pop_back();
    }
    x += p.text_clipped(x, r, e->subject, text, std::max(0, room - badges_w));
    for (const Badge& b : badges) {
      x += 1;
      x += p.text(x, r, " " + b.name + " ", b.style);
    }
    p.text_clipped(W - 1 - kWhen - 1 - who_w, r, who, Style{agent ? th.accent : th.dim, bg}, who_w);
    p.text(W - 1 - text::str_width(when), r, when, dim);
  }
  static constexpr int kWhen = 4;  // "now", "59m", "23h", "120d"

  // A local branch: checked out here (●) or in another work tree (○), how
  // far from its upstream, and the chat that last committed to it.
  void draw_branch(Painter& p, int r, const Line& l, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const int W = p.width();
    const GitBranch& b = branches_[size_t(l.index)];
    const Made& m = branch_made_[size_t(l.index)];
    const int x0 = 3 + l.depth * 2;
    const GitWorktree* in = nullptr;
    for (const GitWorktree& w : worktrees_)
      if (w.branch == b.name) in = &w;
    const bool here = in && in->path == st_.wt;
    if (in) p.put(x0, r, here ? U'●' : U'○', Style{th.accent, bg, attr::kBold});
    const std::string when = ago(b.time);
    const std::string track = track_marks(b.track);
    int x = x0 + 2 + p.text_clipped(x0 + 2, r, l.text, Style{here ? th.accent : th.text, bg, uint16_t(text.a | (here ? attr::kBold : 0))},
                                    std::max(8, W / 2));
    if (!track.empty()) x += 1 + p.text(x + 1, r, track, Style{track == "gone" ? th.err : th.dim, bg});
    if (in && !here) x += 1 + p.text_clipped(x + 1, r, "in " + std::string(last_part(in->path)), dim, std::max(0, W - x - 8));
    if (b.name == st_.ref) x += 1 + p.text(x + 1, r, "\xE2\x97\x82 log", Style{th.accent, bg});
    const std::string who = m.chat ? chat_label(*m.chat) : b.subject;
    const int room = W - x - int(when.size()) - 4;
    if (room > 6) p.text_clipped(x + 2, r, who, Style{m.chat ? th.accent : th.dim, bg}, room);
    p.text(W - int(when.size()) - 1, r, when, dim);
  }

  void draw_remote(Painter& p, int r, const Line& l, Style text, Style dim, Color bg) {
    const int W = p.width();
    const GitBranch& b = remotes_[size_t(l.index)];
    const int x0 = 3 + l.depth * 2;
    const std::string when = ago(b.time);
    int x = x0 + 2 + p.text_clipped(x0 + 2, r, l.text, text, std::max(8, W / 2));
    const int room = W - x - int(when.size()) - 4;
    if (room > 6) p.text_clipped(x + 2, r, b.subject, dim, room);
    p.text(W - int(when.size()) - 1, r, when, dim);
  }

  void draw_folder(Painter& p, int r, const FolderRow& f, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const Project& pr = app_->store().projects()[size_t(f.project)];
    const int W = p.width();
    int x = 3 + p.text_clipped(3, r, pr.name.empty() ? pr.path : pr.name, text, 20) + 2;
    if (f.st && f.st->repo) {
      x += p.text_clipped(x, r, f.st->branch.empty() ? f.st->head : f.st->branch, Style{th.accent, bg}, 24) + 1;
      x += draw_state(p, x, r, f.st, bg, 20) + 1;
    } else if (const GitRepos& rs = app_->workspace().repos_in(pr.path); f.st && !rs.tops.empty()) {
      // In no repository itself, but holding some.
      const size_t n = rs.tops.size();
      x += p.text(x, r, std::to_string(n) + (n == 1 ? " repo" : " repos"), Style{th.accent, bg}) + 1;
      const GitStatus sum = git().total(rs.tops);
      x += draw_state(p, x, r, &sum, bg, 20) + 1;
    } else {
      x += p.text(x, r, f.st ? "no git" : "\xE2\x80\xA6", dim) + 2;
    }
    int working = 0;
    for (LiveSession* s : app_->live_sessions())
      if (!s->exited() && under(s->cwd(), pr.path)) working++;
    if (working)
      p.text_clipped(x, r, std::to_string(working) + (working == 1 ? " agent" : " agents"), dim, std::max(0, W - x - 1));
  }

  // A repository: its name, branch and state, and its agents' mark.
  void draw_repo(Painter& p, int r, int i, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const RepoRow& rr = repos_[size_t(i)];
    const int W = p.width();
    if (i == focus_) p.put(1, r, U'●', Style{th.accent, bg, attr::kBold});
    // One inside another is drawn under it, a step in for each level.
    int x = 3 + 2 * std::max(0, rr.depth - 1);
    if (rr.depth > 0) x += p.text(x, r, "\xE2\x94\x94 ", Style{th.border, bg});
    x += p.text_clipped(x, r, repo_label(i, false), Style{text.fg, bg, uint16_t(text.a | (i == focus_ ? attr::kBold : 0))},
                        std::max(8, W / 4)) + 2;
    std::string agents;
    std::optional<ChatState> best;
    for (LiveSession* s : rr.live) {
      const ChatState cs = chat_state(s, th, app_->anim());
      if (!best || cs.rank < best->rank) best = cs;
    }
    if (best) agents = std::to_string(rr.live.size());
    const int right = agents.empty() ? 1 : int(agents.size()) + 3;
    const GitStatus* st = git().status(rr.top);
    if (st && st->repo) {
      const std::string name = !st->branch.empty() ? st->branch : !st->head.empty() ? st->head : "no commits";
      x += p.text_clipped(x, r, name, Style{th.accent, bg}, std::max(0, std::min(24, W - x - right))) + 1;
      x += draw_state(p, x, r, st, bg, std::max(0, std::min(20, W - x - right))) + 1;
    } else {
      x += p.text_clipped(x, r, st ? "no git" : "\xE2\x80\xA6", dim, std::max(0, W - x - right)) + 1;
    }
    if (const std::string sub = submodule_marks(i); !sub.empty())
      p.text_clipped(x, r, sub, dim, std::max(0, W - x - right));
    if (best) {
      p.put(W - int(agents.size()) - 3, r, best->glyph, Style{best->color, bg, attr::kBold});
      p.text(W - int(agents.size()) - 1, r, agents, dim);
    }
  }

  // What the repository a submodule is in says of it: "submodule", and what
  // moved in it. Empty for one that is not a submodule, or is clean.
  std::string submodule_marks(int i) {
    const RepoRow& rr = repos_[size_t(i)];
    if (rr.parent < 0) return {};
    const std::string& ptop = repos_[size_t(rr.parent)].top;
    const GitStatus* pst = git().status(ptop);
    if (!pst) return {};
    const std::string path = rr.top.substr(ptop.size() + 1);
    for (const GitStatus::Entry& e : pst->entries)
      if (e.path == path && !e.sub.empty()) {
        std::string s = "submodule";
        if (e.sub[0] == 'C') s += " \xC2\xB7 new commits";
        if (e.sub[1] == 'M') s += " \xC2\xB7 changed";
        if (e.sub[2] == 'U') s += " \xC2\xB7 untracked";
        return s;
      }
    return {};
  }

  // A work tree: focused (●) or not, its branch, state and folder.
  void draw_worktree(Painter& p, int r, int i, Style text, Style dim, Color bg) {
    const Theme& th = app_->theme();
    const GitWorktree& w = worktrees_[size_t(i)];
    const int W = p.width();
    const bool focus = w.path == st_.wt;
    if (focus) p.put(1, r, U'●', Style{th.accent, bg, attr::kBold});
    const std::string name = !w.branch.empty() ? w.branch : "detached " + w.head;
    int x = 3 + p.text_clipped(3, r, name, Style{focus ? th.accent : th.text, bg, uint16_t(text.a | (focus ? attr::kBold : 0))},
                               std::max(8, W / 3)) + 2;
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
    p.put(5, r, cs.glyph, Style{cs.color, bg, attr::kBold});
    int x = 7 + p.text(7, r, agent_label(a.live->agent()) + " ", Style{th.text, bg, text.a});
    const std::string word = cs.word;
    const int ww = text::str_width(word);
    // One at work here that runs elsewhere says where it runs.
    const std::string where = a.away ? "  runs in " + runs_in(a.live->cwd()) : "";
    const int where_w = std::min(text::str_width(where), std::max(0, (W - x - ww - 3) / 2));
    x += p.text_clipped(x, r, app_->session_title(*a.live), dim, std::max(0, W - x - ww - 3 - where_w));
    if (where_w) p.text_clipped(x, r, where, dim, where_w);
    p.text(W - ww - 1, r, word, Style{cs.color, bg});
  }

  // Where an agent runs, from the folder shown: "./", "./tools", or the path.
  std::string runs_in(const std::string& cwd) const {
    if (cwd == dir_) return "./";
    if (under(cwd, dir_)) return "./" + cwd.substr(dir_.size() + 1);
    return home_path(cwd);
  }

  // ------------------------------------------------------------ the detail

  // What is known of repository `i`: its branch, what differs, who works in it.
  void repo_notes(int i, const auto& note) {
    const RepoRow& rr = repos_[size_t(i)];
    const GitStatus* st = git().status(rr.top);
    if (!st) return note("reading git\xE2\x80\xA6");
    if (!st->repo) return note("no git work tree now");
    note(st->branch.empty() ? "detached at " + st->head
                            : "on " + st->branch + (st->upstream.empty() ? "" : ", tracking " + st->upstream));
    note(std::to_string(st->staged) + " staged \xC2\xB7 " + std::to_string(st->changed) + " changed \xC2\xB7 " +
         std::to_string(st->untracked) + " untracked" +
         (st->conflicts ? " \xC2\xB7 " + std::to_string(st->conflicts) + " in conflict" : ""));
    const size_t n = rr.live.size();
    note(n ? std::to_string(n) + (n == 1 ? " agent at work in it" : " agents at work in it") : "no agent at work in it");
  }

  void draw_detail(Painter p) {
    const Theme& th = app_->theme();
    const int W = p.width();
    if (sel_ < 0 || sel_ >= int(lines_.size())) return;
    const Line& l = lines_[size_t(sel_)];
    std::vector<std::pair<std::string, Style>> head;  // lines above the diff
    const std::vector<FileChange>* files = nullptr;
    std::vector<FileChange> one;
    std::string root = top_;
    const auto title = [&](std::string s) { head.push_back({std::move(s), Style{th.text, th.panel, attr::kBold}}); };
    const auto note = [&](std::string s) { head.push_back({std::move(s), Style{th.dim, th.panel}}); };
    switch (l.kind) {
      case Kind::Item: {
        const FileItem& it = items_[size_t(l.index)];
        const FileRow& f = files_[size_t(it.file)];
        title(f.e.orig.empty() ? f.e.path : f.e.orig + " \xE2\x86\x92 " + f.e.path);
        if (f.repo >= 0) {
          note(f.e.sub.empty() ? "a repository of its own, inside this one" : "a submodule of this repository");
          repo_notes(f.repo, note);
          note("enter lists its changes");
          break;
        }
        root = f.root;
        note(kSectionSays[it.section] + std::string(f.chat ? " \xC2\xB7 last changed by " + chat_label(*f.chat) + " \xC2\xB7 enter opens the chat there"
                                                            : " \xC2\xB7 no chat mico knows changed it since the last commit"));
        std::vector<std::string> args{"diff", "--no-color", "--no-ext-diff", "--no-textconv"};
        bool any_exit = false;
        if (it.section == kUntracked) {
          args.insert(args.end(), {"--no-index", "--", "/dev/null", f.e.path});
          any_exit = true;
        } else {
          if (it.section == kStaged) args.insert(args.end(), {"--find-renames", "--cached"});
          args.push_back("--");
          if (!f.e.orig.empty()) args.push_back(f.e.orig);
          args.push_back(f.e.path);
        }
        const GitIndex::Query* q = git().query(f.wt, args, 30000, any_exit);
        if (!q) note("reading the diff\xE2\x80\xA6");
        else {
          parse_patch(q->out, root, one);
          files = &one;
          if (one.empty()) note("no text difference to show");
        }
        break;
      }
      case Kind::Section:
      case Kind::Group:
      case Kind::Dir: {
        const std::vector<int>& in = members_[size_t(l.index)];
        // A folder's whole path, or a branch group's prefix.
        title(l.kind != Kind::Dir ? l.text : view_ == View::Branches ? l.key.substr(2) : l.key.substr(l.key.rfind('\n') + 1));
        if (view_ == View::Branches) {
          note(std::to_string(in.size()) + (in.size() == 1 ? " branch" : " branches"));
        } else {
          int added = 0, removed = 0;
          std::vector<const ChatActivity*> chats;
          for (int i : in) {
            const FileItem& it = items_[size_t(i)];
            if (it.counted && it.n.added > 0) added += it.n.added;
            if (it.counted && it.n.removed > 0) removed += it.n.removed;
            if (const ChatActivity* c = files_[size_t(it.file)].chat; c && std::find(chats.begin(), chats.end(), c) == chats.end())
              chats.push_back(c);
          }
          note(std::to_string(in.size()) + (in.size() == 1 ? " file" : " files") +
               (added || removed ? " \xC2\xB7 " + plus_minus(added, removed) : ""));
          for (size_t c = 0; c < chats.size() && c < 4; c++) note("changed by " + chat_label(*chats[c]));
          if (chats.size() > 4) note("and " + std::to_string(chats.size() - 4) + " more chats");
        }
        note(l.open ? "enter or \xE2\x86\x90 folds it" : "enter or \xE2\x86\x92 unfolds it");
        break;
      }
      case Kind::Repo: {
        const RepoRow& rr = repos_[size_t(l.index)];
        title(home_path(rr.top));
        if (rr.parent >= 0) {
          const std::string sub = submodule_marks(l.index);
          note((sub.empty() ? std::string("inside ") : "a submodule of ") + repo_label(rr.parent, true));
        }
        repo_notes(l.index, note);
        note(l.index == focus_ ? "focused: the other views are its" : "enter focuses it");
        break;
      }
      case Kind::Commit: {
        const GitLogEntry& e = log_[size_t(graph_[size_t(l.index)].commit)];
        title(e.subject);
        note(e.hash.substr(0, 10) + " \xC2\xB7 " + e.author + " \xC2\xB7 " + ago(e.time) + (e.refs.empty() ? "" : " \xC2\xB7 " + e.refs));
        const auto it = made_.find(e.hash.substr(0, 7));
        if (it != made_.end()) note("made by " + chat_label(*it->second.chat) + " \xC2\xB7 enter opens the chat there");
        else note("not made by any chat mico knows");
        const GitCommit* c = git().commit(st_.wt, e.hash);
        if (!c) note("reading the commit\xE2\x80\xA6");
        else if (c->found) files = &c->files;
        break;
      }
      case Kind::Worktree: {
        const GitWorktree& w = worktrees_[size_t(l.index)];
        const GitStatus* st = git().status(w.path);
        title(home_path(w.path));
        note(w.branch.empty() ? "detached at " + w.head : "on " + w.branch +
                                                          (st && !st->upstream.empty() ? ", tracking " + st->upstream : ""));
        if (st)
          note(std::to_string(st->staged) + " staged \xC2\xB7 " + std::to_string(st->changed) + " changed \xC2\xB7 " +
               std::to_string(st->untracked) + " untracked" +
               (st->conflicts ? " \xC2\xB7 " + std::to_string(st->conflicts) + " in conflict" : ""));
        int n = 0;
        for (const AgentRow& a : agents_) n += a.worktree == l.index;
        note(n ? std::to_string(n) + (n == 1 ? " agent at work here" : " agents at work here") : "no agent at work here");
        if (l.index < int(worktree_made_.size()) && worktree_made_[size_t(l.index)].chat)
          note("last agent commit: " + worktree_made_[size_t(l.index)].commit->subject + " \xC2\xB7 " +
               chat_label(*worktree_made_[size_t(l.index)].chat));
        note(w.path == st_.wt ? "focused: Changes and Log are its" : "enter focuses it");
        break;
      }
      case Kind::Branch:
      case Kind::Remote: {
        const bool local = l.kind == Kind::Branch;
        const GitBranch& b = (local ? branches_ : remotes_)[size_t(l.index)];
        title(b.name);
        note("last commit " + ago(b.time) + ": " + b.subject);
        if (!b.upstream.empty()) note("tracks " + b.upstream + (b.track.empty() ? "" : " " + b.track));
        for (const GitWorktree& w : worktrees_)
          if (local && w.branch == b.name) note("checked out in " + home_path(w.path));
        if (local)
          if (const Made& m = branch_made_[size_t(l.index)]; m.chat)
            note("last agent commit: " + m.commit->subject + " \xC2\xB7 " + chat_label(*m.chat));
        note("enter lists its commits");
        break;
      }
      case Kind::Agent: {
        const AgentRow& a = agents_[size_t(l.index)];
        LiveSession* s = a.live;
        title(agent_label(s->agent()) + " \xC2\xB7 " + app_->session_title(*s));
        note(a.away ? "works here, and runs in " + home_path(s->cwd()) : home_path(s->cwd()));
        note("enter opens the chat");
        break;
      }
      case Kind::Folder: {
        const Project& pr = app_->store().projects()[size_t(folders_[size_t(l.index)].project)];
        title(home_path(pr.path));
        note("enter shows its repositories");
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
  GitTabState& st_;
  int view_ = View::Changes;  // the view shown: st_.view, unless that one is not there
  bool folders_mode_ = false;  // every folder in the filter: one list of them
  std::string dir_;     // the folder shown
  std::string top_;     // the focused repository's top
  std::vector<RepoRow> repos_;
  int focus_ = -1;      // into repos_
  std::string state_note_;  // said in place of the focused repository's views
  int64_t next_pass_ = 0;   // when the activity index is next read again
  const GitStatus* wt_status_ = nullptr;  // the focused work tree's
  std::vector<GitWorktree> worktrees_;
  std::vector<Made> worktree_made_;
  std::vector<AgentRow> agents_;
  std::vector<GitBranch> branches_, remotes_;
  std::vector<Made> branch_made_;
  std::vector<FileRow> files_;
  std::vector<FileItem> items_;
  std::vector<std::pair<int, size_t>> item_groups_;  // each repository's first item
  bool files_who_ = false;  // some file has a chat to name
  std::vector<GitGraphRow> graph_;
  std::vector<GitLogEntry> log_;
  int graph_w_ = 0;
  bool log_read_ = false;   // git has answered for the log
  std::vector<FolderRow> folders_;
  std::map<std::string, Made> made_;  // by abbreviated hash
  std::vector<Line> lines_;
  std::vector<std::vector<int>> members_;  // what each fold holds
  int sel_ = -1, scroll_ = 0, detail_scroll_ = 0;
  Rect list_rect_{}, detail_rect_{};
  int strip_y_ = -1;
  struct TabSpan {
    int x0, x1, view;
  };
  std::vector<TabSpan> tab_spans_;
  std::vector<std::pair<int, int>> shown_;  // (row in the pane, line)
};

}  // namespace

PanePtr make_git_view(GitTabState& state) { return std::make_unique<GitView>(state); }

}  // namespace mico
