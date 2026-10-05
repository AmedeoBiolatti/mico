#include <algorithm>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/activity.h"
#include "core/git.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/code.h"
#include "views/views.h"

namespace mico {
namespace {

// A path cut from the left, keeping its file name: "…/views/diff.cpp".
std::string fit_path(std::string_view s, int w) {
  if (w <= 0) return {};
  const int sw = text::str_width(s);
  if (sw <= w) return std::string(s);
  size_t i = 0;
  int drop = sw - (w - 1);
  while (i < s.size() && drop > 0) {
    int cw;
    i = text::glyph_end(s, i, &cw);
    drop -= std::max(1, cw);
  }
  return "\xE2\x80\xA6" + std::string(s.substr(i));
}

// `s` with its first `cols` columns taken off, for scrolling sideways.
std::string_view skip_cols(std::string_view s, int cols) {
  size_t i = 0;
  while (i < s.size() && cols > 0) {
    int cw;
    i = text::glyph_end(s, i, &cw);
    cols -= std::max(1, cw);
  }
  return s.substr(i);
}

std::string chat_name(const ChatActivity& c) {
  return c.title.empty() ? agent_label(c.agent) + " chat" : text::oneline(c.title, 80);
}

// A line of a change as drawn: tabs set out, coloured runs, and the part
// that changed when it pairs with a line on the other side.
struct Shown {
  std::string text;
  std::vector<code::Run> runs;
  uint32_t lo = 0, hi = 0;
};
struct Loaded {
  FileChange fc;
  std::vector<Shown> shown;
  const code::Lang* lang = nullptr;
  bool ok = false;
};

struct Ref {
  const ChatActivity* chat;
  const FileEdit* edit;
  // A commit's change, read from git rather than from the transcript.
  const Loaded* from_git = nullptr;
};

// A row of the list: a file and every change made to it, or a chat and
// every change it made.
struct Group {
  std::string key;  // the file, or the chat's transcript
  std::string label;
  long added = 0, removed = 0;
  int64_t last = 0;
  bool created = false, deleted = false;
  std::vector<Ref> edits;  // newest first
  std::vector<const ChatActivity*> chats;
  std::vector<std::string> agents, files;
  // By commit: the commit the chat announced, and git's record of it, null
  // while git is read.
  const ChatCommit* commit = nullptr;
  const GitCommit* git = nullptr;
};

// What the agents changed in files: every change they made, by file or by
// chat, with who made it and its diff, for whatever the sidebar selects.
// The diffs are the agents' own records, read from the transcripts; mico
// never compares files itself.
class DiffView final : public Pane {
 public:
  DiffView(ActivityIndex& index, DiffSettings& settings)
      : index_(index), span_(settings.span), group_(settings.group), sel_key_(settings.sel_key) {
    group_ = std::clamp(group_, 0, kGroups - 1);
  }
  std::string title() const override { return "Diff"; }

  void render(Painter& p, bool focused) override {
    poll_activity(*app_, index_, next_pass_);
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.panel});
    gather();
    const int W = p.width(), H = p.height();
    list_rows_.clear();
    header_rows_.clear();

    // Heading: what is shown, and how to change it.
    {
      int x = p.text(1, 0, "Diff", Style{th.text, th.panel, attr::kBold}) + 3;
      x += p.text_clipped(x, 0, app_->view_filter().label + " \xC2\xB7 " + kPeriods[span_].label,
                          Style{th.accent, th.panel}, std::max(0, W - x - 1));
      const std::string state =
          index_.complete() ? "" : "  reading " + std::to_string(index_.done()) + "/" + std::to_string(index_.total());
      p.text_clipped(x, 0,
                     state + "    t time range \xC2\xB7 g by " + kGroupNames[(group_ + 1) % kGroups] + " \xC2\xB7 a " +
                         (app_->all_folders() ? "this folder" : "all folders"),
                     Style{th.dim, th.panel}, std::max(0, W - x - 1));
    }
    if (H < 2) return;
    p.hline(1, 1, std::max(0, W - 2), U'─', Style{th.border, th.panel});
    if (H < 3) return;

    // The headline.
    if (group_ == kByCommit) {
      std::string s = thousands(groups_.size()) + (groups_.size() == 1 ? " commit" : " commits");
      int x = p.text_clipped(1, 2, s, Style{th.text, th.panel, attr::kBold}, W - 2) + 1;
      std::string who;
      if (chats_) {
        who = " \xC2\xB7 by " + thousands(chats_) + (chats_ == 1 ? " chat" : " chats");
        std::string names;
        for (const auto& [agent, n] : agents_) names += (names.empty() ? "" : ", ") + agent_label(agent) + " " + std::to_string(n);
        who += " (" + names + ")";
      }
      p.text_clipped(x, 2, who, Style{th.dim, th.panel}, std::max(0, W - x - 1));
    } else {
      std::string s = thousands(files_) + (files_ == 1 ? " file" : " files") + " changed";
      int x = p.text_clipped(1, 2, s, Style{th.text, th.panel, attr::kBold}, W - 2) + 1;
      x += p.text(x + 1, 2, "+" + thousands(added_), Style{th.added, th.panel, attr::kBold}) + 1;
      x += p.text(x + 1, 2, "\xE2\x88\x92" + thousands(removed_), Style{th.removed, th.panel, attr::kBold}) + 1;
      std::string who;
      if (chats_) {
        who = " \xC2\xB7 by " + thousands(chats_) + (chats_ == 1 ? " chat" : " chats");
        std::string names;
        for (const auto& [agent, n] : agents_) names += (names.empty() ? "" : ", ") + agent_label(agent) + " " + std::to_string(n);
        who += " (" + names + ")";
      }
      p.text_clipped(x + 1, 2, who, Style{th.dim, th.panel}, std::max(0, W - x - 2));
    }
    if (groups_.empty()) {
      if (H > 4)
        p.text(3, 4,
               !index_.complete()      ? "reading transcripts\xE2\x80\xA6"
               : group_ == kByCommit ? "no commits by agents in this span"
                                     : "no file changes in this span",
               Style{th.dim, th.panel});
      view_h_ = 1;
      return;
    }

    // The list: at most two fifths of the room, the rest for the diff.
    const int top = 3;
    const int room = H - top;
    const int list_h = std::clamp(int(groups_.size()), 1, std::max(3, room * 2 / 5));
    list_h_ = list_h;
    if (sel_ < list_scroll_) list_scroll_ = sel_;
    if (sel_ >= list_scroll_ + list_h) list_scroll_ = sel_ - list_h + 1;
    list_scroll_ = std::clamp(list_scroll_, 0, std::max(0, int(groups_.size()) - list_h));
    const int count_w = 7;
    const int when_w = 9;
    const int by_w = std::clamp(W / 3, 16, 48);
    const int name_w = std::max(10, W - 3 - 2 * count_w - 2 - by_w - 2 - when_w - 2);
    for (int r = 0; r < list_h && list_scroll_ + r < int(groups_.size()); r++) {
      const int i = list_scroll_ + r;
      Group& g = groups_[size_t(i)];
      const int y = top + r;
      const bool sel = i == sel_;
      // A commit's counts are git's: asked for as its row comes into view.
      if (group_ == kByCommit && !g.git) {
        g.git = app_->workspace().git().commit(g.chats[0]->cwd, g.commit->hash);
        if (g.git) {
          g.added = g.git->added;
          g.removed = g.git->removed;
        }
      }
      const Color bg = sel ? (focused ? th.sel_bg : th.sel_inactive) : i % 2 ? th.panel_alt : th.panel;
      if (bg != th.panel) p.fill(Rect{1, y, W - 2, 1}, Style{th.text, bg});
      if (sel) p.put(1, y, U'❯', Style{th.accent, bg, attr::kBold});
      int x = 3;
      const bool counted = group_ != kByCommit || (g.git && g.git->found);
      x += p.text(x, y, pad(counted ? "+" + thousands(g.added) : "", count_w - 1, true) + " ", Style{th.added, bg});
      x += p.text(x, y, pad(counted ? "\xE2\x88\x92" + thousands(g.removed) : "", count_w - 1, true) + "  ",
                  Style{th.removed, bg});
      const std::string tag = group_ == kByCommit ? (!g.git || g.git->found ? "" : g.git->repo ? " not in git" : " no repo")
                              : g.deleted         ? " deleted"
                              : g.created         ? " new"
                                                  : "";
      const int lw = name_w - text::str_width(tag);
      if (group_ == kByCommit) {
        const std::string hash = g.commit->hash.substr(0, 7) + " ";
        x += p.text(x, y, hash, Style{th.hunk, bg});
        x += p.text(x, y, text::ellipsize(g.label, lw - text::str_width(hash)), Style{th.text, bg, sel ? attr::kBold : uint16_t(0)});
      } else {
        const std::string shown = group_ == kByChat ? text::ellipsize(g.label, lw) : fit_path(g.label, lw);
        x += p.text(x, y, shown, Style{th.text, bg, sel ? attr::kBold : uint16_t(0)});
      }
      p.text(x, y, tag, Style{g.deleted || group_ == kByCommit ? th.removed : th.added, bg});
      x = 3 + 2 * count_w + 1 + name_w + 2;
      std::string by;
      if (group_ == kByCommit) {
        by = agent_label(g.chats[0]->agent) + " \xC2\xB7 " + chat_name(*g.chats[0]);
      } else if (group_ == kByChat) {
        by = agent_label(g.chats.empty() ? "" : g.chats[0]->agent) + " \xC2\xB7 " + thousands(g.files.size()) +
             (g.files.size() == 1 ? " file" : " files");
      } else if (g.chats.size() == 1) {
        by = agent_label(g.chats[0]->agent) + " \xC2\xB7 " + chat_name(*g.chats[0]);
      } else {
        std::string names;
        for (const auto& a : g.agents) names += (names.empty() ? "" : ", ") + agent_label(a);
        by = std::to_string(g.chats.size()) + " chats \xC2\xB7 " + names;
      }
      p.text(x, y, text::ellipsize(by, by_w), Style{th.dim, bg});
      p.text(W - 2 - when_w, y, pad(ago(g.last / 1000), when_w, true), Style{th.dim, bg});
      list_rows_.push_back({y, i});
    }

    // The rule between: what the diff below is of.
    const int rule = top + list_h;
    if (rule >= H) return;
    Group& g = groups_[size_t(sel_)];
    if (group_ == kByCommit && !commit_detail(g, p, rule)) return;
    sync_detail(g);
    if (group_ == kByCommit) {
      // The commit as git has it: which, by whom, and how many files.
      p.hline(1, rule, std::max(0, W - 2), U'─', Style{th.border, th.panel});
      std::string what = " " + g.git->hash.substr(0, 10) + " \xC2\xB7 " + g.git->author + " \xC2\xB7 " +
                         thousands(g.edits.size()) + (g.edits.size() == 1 ? " file " : " files ");
      int x = 2 + p.text_clipped(2, rule, what, Style{th.text, th.panel, attr::kBold}, std::max(0, W - 4));
      p.text_clipped(x + 1, rule, " n/p file \xC2\xB7 enter open the chat that made it \xC2\xB7 pgup/pgdn scroll ",
                     Style{th.dim, th.panel}, std::max(0, W - x - 3));
    } else {
      p.hline(1, rule, std::max(0, W - 2), U'─', Style{th.border, th.panel});
      std::string what = " " + g.label + " \xC2\xB7 " + thousands(g.edits.size()) +
                         (g.edits.size() == 1 ? " change " : " changes ");
      int x = 2 + p.text_clipped(2, rule, what, Style{th.text, th.panel, attr::kBold}, std::max(0, W - 4));
      p.text_clipped(x + 1, rule, " n/p change \xC2\xB7 enter open the chat there \xC2\xB7 pgup/pgdn scroll \xC2\xB7 \xE2\x86\x90/\xE2\x86\x92 ",
                     Style{th.dim, th.panel}, std::max(0, W - x - 3));
    }
    const int dtop = rule + 1;
    const int dh = H - dtop;
    view_h_ = std::max(1, dh);
    detail_top_ = dtop;
    if (dh <= 0) return;
    extend(g, scroll_ + dh + 1);
    if (built_ == g.edits.size()) scroll_ = std::min(scroll_, std::max(0, int(rows_.size()) - dh));
    scroll_ = std::max(0, scroll_);
    const int num_w = 5;
    for (int r = 0; r < dh; r++) {
      const int i = scroll_ + r;
      if (i >= int(rows_.size())) break;
      const Row& row = rows_[size_t(i)];
      const int y = dtop + r;
      const Ref& ref = g.edits[size_t(row.edit)];
      const FileEdit& e = *ref.edit;
      if (row.line == kHeader) {
        p.fill(Rect{1, y, W - 2, 1}, Style{th.text, th.strip_bg});
        int x = 2;
        if (group_ != kByFile) {
          x += p.text_clipped(x, y, show_path(e.file, ref.chat->project), Style{th.text, th.strip_bg, attr::kBold},
                              std::max(0, W - 30 - x));
        } else {
          x += p.text(x, y, agent_label(ref.chat->agent), Style{th.accent, th.strip_bg, attr::kBold});
          x += p.text_clipped(x, y, " \xC2\xB7 " + chat_name(*ref.chat), Style{th.text, th.strip_bg, attr::kBold},
                              std::max(0, W - 34 - x));
        }
        std::string op = std::string(" \xC2\xB7 ") + edit_op_name(e.op);
        if (!e.moved_to.empty()) op += " \xE2\x86\x92 " + show_path(e.moved_to, ref.chat->project);
        x += p.text_clipped(x, y, op + " \xC2\xB7 " + ago(e.at_ms / 1000), Style{th.dim, th.strip_bg}, std::max(0, W - 20 - x));
        const std::string plus = "+" + thousands(e.added), minus = " \xE2\x88\x92" + thousands(e.removed);
        const int rx = W - 3 - text::str_width(plus) - text::str_width(minus);
        if (rx > x + 1) {
          p.text(rx, y, plus, Style{th.added, th.strip_bg});
          p.text(rx + text::str_width(plus), y, minus, Style{th.removed, th.strip_bg});
        }
        header_rows_.push_back({y, row.edit});
        continue;
      }
      if (row.line == kSpacer) continue;
      if (row.line == kMissing) {
        p.text_clipped(2 + num_w, y, "the diff could not be read again from the transcript", Style{th.dim, th.panel},
                       W - num_w - 4);
        continue;
      }
      const Loaded* l = loaded(ref);
      if (!l) continue;
      if (row.line == kMore) {
        p.text_clipped(2 + num_w, y,
                       "\xE2\x80\xA6 " + thousands(long(l->fc.lines.size()) - kMaxLines) + " more lines \xC2\xB7 enter opens the chat",
                       Style{th.dim, th.panel}, W - num_w - 4);
        continue;
      }
      const DiffLine& d = l->fc.lines[size_t(row.line)];
      // A hunk's start: a rule saying where in the file it is.
      if (d.kind == '@') {
        p.hline(1, y, std::max(0, W - 2), U'\u2504', Style{th.border, th.panel});
        int x = 1 + num_w;
        if (d.new_no || d.old_no)
          x += p.text(x, y, " line " + std::to_string(d.new_no ? d.new_no : d.old_no) + " ", Style{th.hunk, th.panel});
        if (!d.text.empty())
          p.text_clipped(x, y, " " + text::oneline(d.text, 200) + " ", Style{th.dim, th.panel},
                         std::max(0, W - 4 - x));
        continue;
      }
      const int no = d.kind == '-' ? d.old_no : d.new_no;
      const Color fg = d.kind == '+' ? th.added : d.kind == '-' ? th.removed : th.dim;
      const Color bg = d.kind == '+' ? th.added_bg : d.kind == '-' ? th.removed_bg : th.panel;
      if (bg != th.panel) p.fill(Rect{1 + num_w, y, std::max(0, W - 2 - num_w), 1}, Style{th.text, bg});
      if (no) p.text(1, y, pad(std::to_string(no), num_w, true), Style{d.kind == ' ' ? th.dim : fg, th.panel});
      if (d.kind != ' ') p.text(2 + num_w, y, d.kind == '-' ? "\xE2\x88\x92" : "+", Style{fg, bg, attr::kBold});
      draw_code(p, 4 + num_w, y, std::max(0, W - num_w - 5), l->shown[size_t(row.line)], d.kind, l->lang != nullptr,
                bg);
    }
  }

  bool on_key(const KeyEvent& k) override {
    switch (k.key) {
      case Key::Up: select(sel_ - 1); return true;
      case Key::Down: select(sel_ + 1); return true;
      case Key::PageUp: scroll_ = std::max(0, scroll_ - std::max(1, view_h_ - 2)); return true;
      case Key::PageDown: scroll_ += std::max(1, view_h_ - 2); return true;
      case Key::Home: scroll_ = 0; hscroll_ = 0; return true;
      case Key::End: end_ = true; return true;
      case Key::Left: hscroll_ = std::max(0, hscroll_ - 8); return true;
      case Key::Right: hscroll_ += 8; return true;
      case Key::Enter: open_at_top(); return true;
      default: break;
    }
    if (k.is(' ')) { scroll_ += std::max(1, view_h_ - 2); return true; }
    if (k.is('n')) { jump(+1); return true; }
    if (k.is('p')) { jump(-1); return true; }
    if (k.is('t')) { span_ = (span_ + 1) % kPeriodCount; reset_detail(); return true; }
    if (k.is('g')) { regroup((group_ + 1) % kGroups); return true; }
    if (k.is('a')) { app_->set_all_folders(!app_->all_folders()); return true; }
    if (k.is('r')) { index_.start(app_->store().projects(), app_->store()); return true; }
    return false;
  }
  bool on_mouse(const MouseEvent& m, Point local) override {
    const bool in_list = local.y < detail_top_;
    if (m.kind == MouseKind::WheelUp) {
      if (in_list) select(sel_ - 1);
      else scroll_ = std::max(0, scroll_ - 3);
      return true;
    }
    if (m.kind == MouseKind::WheelDown) {
      if (in_list) select(sel_ + 1);
      else scroll_ += 3;
      return true;
    }
    if (m.kind != MouseKind::Press || m.button != MouseButton::Left) return false;
    for (const auto& [y, i] : list_rows_)
      if (y == local.y) {
        select(i);
        return true;
      }
    for (const auto& [y, e] : header_rows_)
      if (y == local.y) {
        open_edit(e);
        return true;
      }
    return false;
  }
  std::vector<MenuItem> context_menu(Point) override {
    std::vector<MenuItem> items;
    for (int i = 0; i < kGroups; i++)
      if (i != group_) items.push_back(MenuItem{std::string("Group by ") + kGroupNames[i], "group" + std::to_string(i)});
    items.push_back(MenuItem{"Next time range", "span"});
    items.push_back(MenuItem{app_->all_folders() ? "Only this folder" : "All folders", "all"});
    items.push_back(MenuItem{"Reread transcripts", "rescan"});
    return items;
  }
  void on_action(const std::string& a) override {
    if (a.starts_with("group")) regroup(std::atoi(a.c_str() + 5));
    else if (a == "span") { span_ = (span_ + 1) % kPeriodCount; reset_detail(); }
    else if (a == "all") app_->set_all_folders(!app_->all_folders());
    else if (a == "rescan") index_.start(app_->store().projects(), app_->store());
  }

 private:
  static constexpr int kByFile = 0, kByChat = 1, kByCommit = 2, kGroups = 3;
  static constexpr const char* kGroupNames[kGroups] = {"file", "chat", "commit"};

  void regroup(int to) {
    group_ = std::clamp(to, 0, kGroups - 1);
    sel_ = 0;
    sel_key_.clear();
    reset_detail();
  }

  // Below a commit's row: its diff, once git has it, made into changes the
  // rest of the view draws like any other. False, with a note drawn in its
  // place, while there is none to draw.
  bool commit_detail(Group& g, Painter& p, int rule) {
    const Theme& th = app_->theme();
    const int W = p.width();
    if (!g.git) g.git = app_->workspace().git().commit(g.chats[0]->cwd, g.commit->hash);
    std::string note;
    if (!g.git) note = "reading " + g.commit->hash + " from git\xE2\x80\xA6";
    else if (!g.git->repo)
      note = g.chats[0]->cwd + " is not a git repository now, or is gone";
    else if (!g.git->found)
      note = "git has no " + g.commit->hash + " in " + g.chats[0]->cwd +
             ": rewritten since (an amend or a rebase), or made in another repository";
    if (note.empty()) {
      CommitDiff& d = commit_diffs_[g.key];
      if (d.from != g.git) {
        d.from = g.git;
        d.edits.clear();
        d.loaded.clear();
        d.edits.reserve(g.git->files.size());
        d.loaded.reserve(g.git->files.size());
        for (const FileChange& fc : g.git->files) {
          FileEdit e;
          e.at_ms = g.commit->at_ms;
          e.offset = e.call_offset = g.commit->call_offset;
          e.file = fc.file;
          e.moved_to = fc.moved_to;
          e.op = fc.op;
          e.added = fc.added;
          e.removed = fc.removed;
          d.edits.push_back(std::move(e));
          Loaded l;
          l.fc = fc;
          l.ok = true;
          prepare(l, fc.file);
          d.loaded.push_back(std::move(l));
        }
      }
      g.edits.clear();
      for (size_t i = 0; i < d.edits.size(); i++) g.edits.push_back(Ref{g.chats[0], &d.edits[i], &d.loaded[i]});
      if (!g.edits.empty()) return true;
      note = "the commit changed no file git shows as text";
    }
    p.hline(1, rule, std::max(0, W - 2), U'─', Style{th.border, th.panel});
    p.text_clipped(2, rule, " " + g.commit->hash + " ", Style{th.text, th.panel, attr::kBold}, std::max(0, W - 4));
    if (rule + 2 < p.height()) p.text_clipped(3, rule + 2, note, Style{th.dim, th.panel}, std::max(0, W - 6));
    detail_top_ = rule + 1;
    rows_.clear();
    built_ = 0;
    detail_sig_.clear();
    return false;
  }

  // A row of the diff below the list: an edit's header, one of its lines, or
  // a note in their place.
  static constexpr int kHeader = -1, kSpacer = -2, kMissing = -3, kMore = -4;
  // A new file of thousands of lines would bury the rest.
  static constexpr int kMaxLines = 400;
  struct Row {
    int edit;
    int line;
  };
  static std::string expand_tabs(std::string_view s) {
    std::string out;
    int col = 0;
    for (size_t i = 0; i < s.size();) {
      if (s[i] == '\t') {
        do out += ' ';
        while (++col % 4);
        i++;
        continue;
      }
      const size_t at = i;
      int cw;
      i = text::glyph_end(s, i, &cw);
      col += std::max(0, cw);
      out.append(s.substr(at, i - at));
    }
    return out;
  }

  // Colours a change's lines as the code they are, the old side and the new
  // each carried on its own, and finds the words a changed line changed.
  static void prepare(Loaded& l, const std::string& file) {
    l.lang = code::lang_of_path(file);
    const auto& lines = l.fc.lines;
    l.shown.assign(lines.size(), Shown{});
    code::State old_st, new_st;
    std::vector<code::Run> scratch;
    for (size_t i = 0; i < lines.size(); i++) {
      const DiffLine& d = lines[i];
      Shown& sh = l.shown[i];
      if (d.kind == '@') {
        old_st = new_st = {};
        continue;
      }
      sh.text = expand_tabs(d.text);
      if (d.kind == '-') code::highlight(sh.text, l.lang, old_st, sh.runs);
      else code::highlight(sh.text, l.lang, new_st, sh.runs);
      if (d.kind == ' ') code::highlight(sh.text, l.lang, old_st, scratch);
    }
    // Removed lines and the added ones right after them pair up in order; in
    // each pair, what lies between the common start and end is the change.
    for (size_t i = 0; i < lines.size();) {
      if (lines[i].kind != '-') { i++; continue; }
      size_t d = i;
      while (d < lines.size() && lines[d].kind == '-') d++;
      size_t a = d;
      while (a < lines.size() && lines[a].kind == '+') a++;
      for (size_t k = 0; k < std::min(d - i, a - d); k++) {
        Shown& x = l.shown[i + k];
        Shown& y = l.shown[d + k];
        const std::string_view sx = x.text, sy = y.text;
        size_t pre = 0;
        while (pre < sx.size() && pre < sy.size() && sx[pre] == sy[pre]) pre++;
        while (pre > 0 && pre < sx.size() && (uint8_t(sx[pre]) & 0xC0) == 0x80) pre--;
        size_t suf = 0;
        while (suf < sx.size() - pre && suf < sy.size() - pre && sx[sx.size() - 1 - suf] == sy[sy.size() - 1 - suf]) suf++;
        while (suf > 0 && (uint8_t(sx[sx.size() - suf]) & 0xC0) == 0x80) suf--;
        // A line changed nearly throughout is just a different line.
        const size_t cx = sx.size() - pre - suf, cy = sy.size() - pre - suf;
        if (cx + cy == 0 || (cx + cy) * 10 > (sx.size() + sy.size()) * 8) continue;
        x.lo = uint32_t(pre), x.hi = uint32_t(pre + cx);
        y.lo = uint32_t(pre), y.hi = uint32_t(pre + cy);
      }
      i = a;
    }
  }

  Color tok_color(code::Tok t, char kind, bool coloured) const {
    const Theme& th = app_->theme();
    switch (t) {
      case code::Tok::Keyword: return th.code_keyword;
      case code::Tok::String: return th.code_string;
      case code::Tok::Comment: return th.code_comment;
      case code::Tok::Number: return th.code_number;
      case code::Tok::Type: return th.code_type;
      case code::Tok::Func: return th.code_func;
      default: break;
    }
    if (coloured) return th.code_text;
    return kind == '+' ? th.added : kind == '-' ? th.removed : th.text;
  }

  // One line of code from column `x`, `w` wide, scrolled sideways by
  // hscroll_; the words that changed on a stronger tint.
  void draw_code(Painter& p, int x0, int y, int w, const Shown& sh, char kind, bool coloured, Color bg) const {
    const Theme& th = app_->theme();
    const Color strong = kind == '+' ? th.added_strong : th.removed_strong;
    int col = 0;
    for (const code::Run& r : sh.runs) {
      // Split where the changed part starts and ends.
      uint32_t cuts[4] = {r.off, r.off + r.len, r.off + r.len, r.off + r.len};
      int n = 1;
      if (sh.hi > sh.lo) {
        if (sh.lo > r.off && sh.lo < r.off + r.len) cuts[n++] = sh.lo;
        if (sh.hi > r.off && sh.hi < r.off + r.len) cuts[n++] = sh.hi;
        cuts[n++] = r.off + r.len;
        std::sort(cuts + 1, cuts + n);
      } else {
        cuts[1] = r.off + r.len, n = 2;
      }
      Style st{tok_color(r.tok, kind, coloured), bg};
      if (r.tok == code::Tok::Comment) st.a |= attr::kItalic;
      for (int k = 0; k + 1 < n; k++) {
        if (cuts[k + 1] <= cuts[k]) continue;
        std::string_view t = std::string_view(sh.text).substr(cuts[k], cuts[k + 1] - cuts[k]);
        const bool changed = sh.hi > sh.lo && cuts[k] >= sh.lo && cuts[k] < sh.hi;
        Style ps = st;
        if (changed) ps.bg = strong;
        const int tw = text::str_width(t);
        if (col + tw <= hscroll_) { col += tw; continue; }
        int x = x0 + col - hscroll_;
        if (col < hscroll_) {
          t = skip_cols(t, hscroll_ - col);
          x = x0;
        }
        col += tw;
        if (x >= x0 + w) return;
        p.text_clipped(x, y, t, ps, x0 + w - x);
      }
    }
  }

  // A file as the list shows it: inside its folder, from there; with every
  // folder shown, under the folder's name; elsewhere, from home.
  std::string show_path(const std::string& file, const std::string& project) const {
    auto it = project_paths_.find(project);
    if (it != project_paths_.end()) {
      const std::string& root = it->second;
      if (file.size() > root.size() && file.starts_with(root) && file[root.size()] == '/') {
        std::string rel = file.substr(root.size() + 1);
        return app_->all_folders() ? project + "/" + rel : rel;
      }
    }
    const char* home = getenv("HOME");
    if (home && *home && file.starts_with(home) && file.size() > strlen(home) && file[strlen(home)] == '/')
      return "~" + file.substr(strlen(home));
    return file;
  }

  // By commit: a row for each commit a chat announced in the span, newest
  // first. What it changed is git's to say, read as the row is shown.
  void gather_commits(int64_t from) {
    std::vector<Group> groups;
    std::map<std::string, long> agents;
    chats_ = 0;
    for (const ChatActivity* c : index_.chats()) {
      if (c->commits.empty() || !app_->in_filter(c->project, c->path)) continue;
      bool counted = false;
      for (const ChatCommit& cm : c->commits) {
        if (cm.at_ms < from) continue;
        if (!counted) {
          counted = true;
          chats_++;
          agents[c->agent]++;
        }
        Group g;
        g.key = c->cwd + "\n" + cm.hash;
        g.label = cm.subject;
        g.last = cm.at_ms;
        g.chats = {c};
        g.agents = {c->agent};
        g.commit = &cm;
        groups.push_back(std::move(g));
      }
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.last > b.last; });
    groups_ = std::move(groups);
    files_ = 0;
    added_ = removed_ = 0;
    agents_.assign(agents.begin(), agents.end());
    std::sort(agents_.begin(), agents_.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    sel_ = 0;
    for (size_t i = 0; i < groups_.size(); i++)
      if (groups_[i].key == sel_key_) sel_ = int(i);
    if (commit_diffs_.size() > 64) commit_diffs_.clear();
  }

  void gather() {
    project_paths_.clear();
    for (const auto& pr : app_->store().projects()) project_paths_[pr.name] = pr.path;
    const int64_t from = period_start_ms(span_);
    if (group_ == kByCommit) return gather_commits(from);
    std::unordered_map<std::string, size_t> at;
    std::vector<Group> groups;
    std::map<std::string, long> agents;
    std::unordered_map<std::string, int> files;
    added_ = removed_ = 0;
    chats_ = 0;
    for (const ChatActivity* c : index_.chats()) {
      if (c->edits.empty() || !app_->in_filter(c->project, c->path)) continue;
      bool counted = false;
      for (const FileEdit& e : c->edits) {
        if (e.at_ms < from) continue;
        if (!counted) {
          counted = true;
          chats_++;
          agents[c->agent]++;
        }
        added_ += e.added;
        removed_ += e.removed;
        files[e.file]++;
        const std::string& key = group_ == kByChat ? c->path : e.file;
        auto [it, fresh] = at.try_emplace(key, groups.size());
        if (fresh) {
          Group g;
          g.key = key;
          g.label = group_ == kByChat ? chat_name(*c) : show_path(e.file, c->project);
          groups.push_back(std::move(g));
        }
        Group& g = groups[it->second];
        g.added += e.added;
        g.removed += e.removed;
        g.edits.push_back(Ref{c, &e});
        if (std::find(g.chats.begin(), g.chats.end(), c) == g.chats.end()) g.chats.push_back(c);
        if (std::find(g.agents.begin(), g.agents.end(), c->agent) == g.agents.end()) g.agents.push_back(c->agent);
        if (group_ == kByChat && std::find(g.files.begin(), g.files.end(), e.file) == g.files.end())
          g.files.push_back(e.file);
      }
    }
    for (Group& g : groups) {
      std::stable_sort(g.edits.begin(), g.edits.end(),
                       [](const Ref& a, const Ref& b) { return a.edit->at_ms > b.edit->at_ms; });
      g.last = g.edits.front().edit->at_ms;
      // Over the span: made here, or gone by its end.
      g.created = g.edits.back().edit->op == EditOp::Create;
      g.deleted = g.edits.front().edit->op == EditOp::Delete;
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.last > b.last; });
    groups_ = std::move(groups);
    files_ = files.size();
    agents_.assign(agents.begin(), agents.end());
    std::sort(agents_.begin(), agents_.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    // A row chosen follows its file or chat as the list re-sorts; until one
    // is, the newest is.
    sel_ = 0;
    for (size_t i = 0; i < groups_.size(); i++)
      if (groups_[i].key == sel_key_) sel_ = int(i);
  }

  void select(int i) {
    if (groups_.empty()) return;
    i = std::clamp(i, 0, int(groups_.size()) - 1);
    if (i == sel_) return;
    sel_ = i;
    sel_key_ = groups_[size_t(i)].key;
    reset_detail();
  }

  void reset_detail() {
    rows_.clear();
    built_ = 0;
    scroll_ = 0;
    hscroll_ = 0;
    detail_sig_.clear();
  }

  // Starts the diff over when what it is of changed: another row, or new
  // changes to this one.
  void sync_detail(const Group& g) {
    const Ref& newest = g.edits.front();
    const std::string sig = g.key + "\n" + std::to_string(g.edits.size()) + "\n" + newest.chat->path + "\n" +
                            std::to_string(newest.edit->offset) + char('0' + group_);
    if (sig == detail_sig_) return;
    const bool same_row = !detail_sig_.empty() && detail_sig_.starts_with(g.key + "\n");
    const int keep = scroll_;
    rows_.clear();
    built_ = 0;
    detail_sig_ = sig;
    // New changes on top of the one being read: stay where the reader is.
    if (!same_row) scroll_ = 0;
    else scroll_ = keep;
  }

  static std::string cache_key(const Ref& r) {
    return r.chat->path + "\n" + std::to_string(r.edit->offset) + "\n" + r.edit->file;
  }

  const Loaded* loaded(const Ref& r) {
    if (r.from_git) return r.from_git;
    const std::string key = cache_key(r);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.ok ? &it->second : nullptr;
    if (cache_.size() > 600) cache_.clear();
    Loaded& l = cache_[key];
    l.ok = load_change(r.chat->path, r.chat->agent, r.chat->cwd, r.edit->offset, r.edit->file, l.fc);
    if (l.ok) prepare(l, r.edit->file);
    return l.ok ? &l : nullptr;
  }

  // Lays out more changes until there are `want` rows, or all of them: each
  // is read from its transcript only when it comes into view.
  void extend(const Group& g, int want) {
    if (end_) want = 1 << 30;
    while (built_ < g.edits.size() && int(rows_.size()) < want) {
      const int e = int(built_++);
      rows_.push_back(Row{e, kHeader});
      const Loaded* l = loaded(g.edits[size_t(e)]);
      if (!l) {
        rows_.push_back(Row{e, kMissing});
      } else {
        const int n = int(l->fc.lines.size());
        for (int i = 0; i < std::min(n, kMaxLines); i++) rows_.push_back(Row{e, i});
        if (n > kMaxLines) rows_.push_back(Row{e, kMore});
      }
      rows_.push_back(Row{e, kSpacer});
    }
    if (end_) {
      end_ = false;
      scroll_ = std::max(0, int(rows_.size()) - view_h_);
    }
  }

  // Moves the diff to the next or previous change's header.
  void jump(int dir) {
    if (groups_.empty()) return;
    extend(groups_[size_t(sel_)], scroll_ + view_h_ * 2 + 64);
    if (dir > 0) {
      for (size_t i = size_t(scroll_) + 1; i < rows_.size(); i++)
        if (rows_[i].line == kHeader) {
          scroll_ = int(i);
          return;
        }
    } else {
      for (int i = std::min(scroll_, int(rows_.size())) - 1; i >= 0; i--)
        if (rows_[size_t(i)].line == kHeader) {
          scroll_ = i;
          return;
        }
    }
  }

  void open_edit(int e) {
    if (groups_.empty()) return;
    const Group& g = groups_[size_t(sel_)];
    if (e < 0 || e >= int(g.edits.size())) return;
    const Ref& r = g.edits[size_t(e)];
    app_->open_at(r.chat->path, r.edit->call_offset, {});
  }

  // Enter: the change at the top of the diff, the newest before any scrolling.
  void open_at_top() {
    if (groups_.empty()) return;
    if (const Group& g = groups_[size_t(sel_)]; g.commit && g.edits.empty()) {
      app_->open_at(g.chats[0]->path, g.commit->call_offset, {});
      return;
    }
    int e = 0;
    if (scroll_ < int(rows_.size())) e = rows_[size_t(scroll_)].edit;
    open_edit(e);
  }

  ActivityIndex& index_;
  int64_t next_pass_ = 0;
  int& span_;
  int& group_;
  // Each commit's changes as git gave them, kept while it is looked at: the
  // rows of its diff point into them.
  struct CommitDiff {
    const GitCommit* from = nullptr;
    std::vector<FileEdit> edits;
    std::vector<Loaded> loaded;
  };
  std::map<std::string, CommitDiff> commit_diffs_;
  std::vector<Group> groups_;
  std::map<std::string, std::string> project_paths_;
  std::vector<std::pair<std::string, long>> agents_;
  size_t files_ = 0, chats_ = 0;
  long added_ = 0, removed_ = 0;
  int sel_ = 0, list_scroll_ = 0, list_h_ = 1;
  std::string& sel_key_;
  // The diff of the selected row.
  std::vector<Row> rows_;
  size_t built_ = 0;  // changes laid out so far
  std::string detail_sig_;
  int scroll_ = 0, hscroll_ = 0, view_h_ = 1, detail_top_ = 1 << 30;
  bool end_ = false;
  std::unordered_map<std::string, Loaded> cache_;
  std::vector<std::pair<int, int>> list_rows_;    // (screen row, group)
  std::vector<std::pair<int, int>> header_rows_;  // (screen row, change)
};

}  // namespace

PanePtr make_diff_view(ActivityIndex& index, DiffSettings& settings) {
  return std::make_unique<DiffView>(index, settings);
}

}  // namespace mico
