#include <algorithm>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/activity.h"
#include "term/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

std::string count(long n) {
  std::string s = std::to_string(n);
  for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
  return s;
}

std::string ago(int64_t ms) {
  const int64_t d = int64_t(time(nullptr)) - ms / 1000;
  char b[24];
  if (d < 60) return "just now";
  if (d < 3600) snprintf(b, sizeof b, "%lldm ago", (long long)(d / 60));
  else if (d < 86400) snprintf(b, sizeof b, "%lldh ago", (long long)(d / 3600));
  else snprintf(b, sizeof b, "%lldd ago", (long long)(d / 86400));
  return b;
}

std::string fit(std::string_view s, int w) {
  if (w <= 0) return {};
  if (text::str_width(s) <= w) return std::string(s);
  std::string out;
  int used = 0;
  for (size_t i = 0; i < s.size();) {
    const size_t at = i;
    const char32_t cp = text::decode(s, i);
    const int cw = std::max(1, text::cp_width(cp));
    if (used + cw > w - 1) break;
    out.append(s.substr(at, i - at));
    used += cw;
  }
  return out + "\xE2\x80\xA6";
}

// A path cut from the left, keeping its file name: "…/views/diff.cpp".
std::string fit_path(std::string_view s, int w) {
  if (w <= 0) return {};
  const int sw = text::str_width(s);
  if (sw <= w) return std::string(s);
  size_t i = 0;
  int drop = sw - (w - 1);
  while (i < s.size() && drop > 0) {
    const char32_t cp = text::decode(s, i);
    drop -= std::max(1, text::cp_width(cp));
  }
  return "\xE2\x80\xA6" + std::string(s.substr(i));
}

// `s` with its first `cols` columns taken off, for scrolling sideways.
std::string_view skip_cols(std::string_view s, int cols) {
  size_t i = 0;
  while (i < s.size() && cols > 0) {
    const char32_t cp = text::decode(s, i);
    cols -= std::max(1, text::cp_width(cp));
  }
  return s.substr(i);
}

std::string pad(const std::string& s, int w, bool right) {
  const int n = w - text::str_width(s);
  if (n <= 0) return s;
  return right ? std::string(size_t(n), ' ') + s : s + std::string(size_t(n), ' ');
}

std::string chat_name(const ChatActivity& c) {
  return c.title.empty() ? agent_label(c.agent) + " chat" : text::oneline(c.title, 80);
}

struct Ref {
  const ChatActivity* chat;
  const FileEdit* edit;
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
};

// What the agents changed in files: every change they made, by file or by
// chat, with who made it and its diff, for whatever the sidebar selects.
// The diffs are the agents' own records, read from the transcripts; mico
// never compares files itself.
class DiffView final : public Pane {
 public:
  DiffView(ActivityIndex& index, DiffSettings& settings)
      : index_(index), span_(settings.span), by_chat_(settings.by_chat), sel_key_(settings.sel_key) {}
  std::string title() const override { return "Diff"; }

  void render(Painter& p, bool focused) override {
    poll();
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.panel});
    gather();
    const int W = p.width(), H = p.height();
    list_rows_.clear();
    header_rows_.clear();

    // Heading: what is shown, and how to change it.
    {
      int x = p.text(1, 0, "Diff", Style{th.text, th.panel, attr::kBold}) + 3;
      x += p.text_clipped(x, 0, app_->view_filter().label + " \xC2\xB7 " + kSpans[span_].label,
                          Style{th.accent, th.panel}, std::max(0, W - x - 1));
      const std::string state =
          index_.complete() ? "" : "  reading " + std::to_string(index_.done()) + "/" + std::to_string(index_.total());
      p.text_clipped(x, 0,
                     state + "    t time range \xC2\xB7 g by " + (by_chat_ ? "file" : "chat") + " \xC2\xB7 a " +
                         (app_->all_folders() ? "this folder" : "all folders"),
                     Style{th.dim, th.panel}, std::max(0, W - x - 1));
    }
    if (H < 2) return;
    p.hline(1, 1, std::max(0, W - 2), U'─', Style{th.border, th.panel});
    if (H < 3) return;

    // The headline.
    {
      std::string s = count(long(files_)) + (files_ == 1 ? " file" : " files") + " changed";
      int x = p.text_clipped(1, 2, s, Style{th.text, th.panel, attr::kBold}, W - 2) + 1;
      x += p.text(x + 1, 2, "+" + count(added_), Style{th.added, th.panel, attr::kBold}) + 1;
      x += p.text(x + 1, 2, "\xE2\x88\x92" + count(removed_), Style{th.removed, th.panel, attr::kBold}) + 1;
      std::string who;
      if (chats_) {
        who = " \xC2\xB7 by " + count(long(chats_)) + (chats_ == 1 ? " chat" : " chats");
        std::string names;
        for (const auto& [agent, n] : agents_) names += (names.empty() ? "" : ", ") + agent_label(agent) + " " + std::to_string(n);
        who += " (" + names + ")";
      }
      p.text_clipped(x + 1, 2, who, Style{th.dim, th.panel}, std::max(0, W - x - 2));
    }
    if (groups_.empty()) {
      if (H > 4)
        p.text(3, 4, index_.complete() ? "no file changes in this span" : "reading transcripts\xE2\x80\xA6",
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
      const Group& g = groups_[size_t(i)];
      const int y = top + r;
      const bool sel = i == sel_;
      const Color bg = sel ? (focused ? th.sel_bg : th.sel_inactive) : th.panel;
      if (sel) p.fill(Rect{1, y, W - 2, 1}, Style{th.text, bg});
      if (sel) p.put(1, y, U'❯', Style{th.accent, bg, attr::kBold});
      int x = 3;
      x += p.text(x, y, pad("+" + count(g.added), count_w - 1, true) + " ", Style{th.added, bg});
      x += p.text(x, y, pad("\xE2\x88\x92" + count(g.removed), count_w - 1, true) + "  ", Style{th.removed, bg});
      std::string tag = g.deleted ? " deleted" : g.created ? " new" : "";
      const int lw = name_w - text::str_width(tag);
      const std::string shown = by_chat_ ? fit(g.label, lw) : fit_path(g.label, lw);
      x += p.text(x, y, shown, Style{th.text, bg, sel ? attr::kBold : uint16_t(0)});
      p.text(x, y, tag, Style{g.deleted ? th.removed : th.added, bg});
      x = 3 + 2 * count_w + 1 + name_w + 2;
      std::string by;
      if (by_chat_) {
        by = agent_label(g.chats.empty() ? "" : g.chats[0]->agent) + " \xC2\xB7 " + count(long(g.files.size())) +
             (g.files.size() == 1 ? " file" : " files");
      } else if (g.chats.size() == 1) {
        by = agent_label(g.chats[0]->agent) + " \xC2\xB7 " + chat_name(*g.chats[0]);
      } else {
        std::string names;
        for (const auto& a : g.agents) names += (names.empty() ? "" : ", ") + agent_label(a);
        by = std::to_string(g.chats.size()) + " chats \xC2\xB7 " + names;
      }
      p.text(x, y, fit(by, by_w), Style{th.dim, bg});
      p.text(W - 2 - when_w, y, pad(ago(g.last), when_w, true), Style{th.dim, bg});
      list_rows_.push_back({y, i});
    }

    // The rule between: what the diff below is of.
    const int rule = top + list_h;
    if (rule >= H) return;
    const Group& g = groups_[size_t(sel_)];
    sync_detail(g);
    {
      p.hline(1, rule, std::max(0, W - 2), U'─', Style{th.border, th.panel});
      std::string what = " " + g.label + " \xC2\xB7 " + count(long(g.edits.size())) +
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
        if (by_chat_) {
          x += p.text_clipped(x, y, show_path(e.file, ref.chat->project), Style{th.text, th.strip_bg, attr::kBold},
                              std::max(0, W - 30 - x));
        } else {
          x += p.text(x, y, agent_label(ref.chat->agent), Style{th.accent, th.strip_bg, attr::kBold});
          x += p.text_clipped(x, y, " \xC2\xB7 " + chat_name(*ref.chat), Style{th.text, th.strip_bg, attr::kBold},
                              std::max(0, W - 34 - x));
        }
        std::string op = std::string(" \xC2\xB7 ") + edit_op_name(e.op);
        if (!e.moved_to.empty()) op += " \xE2\x86\x92 " + show_path(e.moved_to, ref.chat->project);
        x += p.text_clipped(x, y, op + " \xC2\xB7 " + ago(e.at_ms), Style{th.dim, th.strip_bg}, std::max(0, W - 20 - x));
        const std::string plus = "+" + count(e.added), minus = " \xE2\x88\x92" + count(e.removed);
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
      const FileChange* fc = loaded(ref);
      if (!fc) continue;
      if (row.line == kMore) {
        p.text_clipped(2 + num_w, y,
                       "\xE2\x80\xA6 " + count(long(fc->lines.size()) - kMaxLines) + " more lines \xC2\xB7 enter opens the chat",
                       Style{th.dim, th.panel}, W - num_w - 4);
        continue;
      }
      const DiffLine& d = fc->lines[size_t(row.line)];
      if (d.kind == '@') {
        std::string h;
        if (d.old_no || d.new_no) h = "@@ -" + std::to_string(d.old_no) + " +" + std::to_string(d.new_no) + " @@";
        else h = "\xE2\x8B\xAF";
        if (!d.text.empty()) h += " " + d.text;
        p.text_clipped(2 + num_w, y, h, Style{th.hunk, th.panel}, std::max(0, W - num_w - 4));
        continue;
      }
      const int no = d.kind == '-' ? d.old_no : d.new_no;
      if (no) p.text(1, y, pad(std::to_string(no), num_w, true), Style{th.dim, th.panel});
      const Color fg = d.kind == '+' ? th.added : d.kind == '-' ? th.removed : th.dim;
      const char mark[2] = {d.kind == ' ' ? ' ' : d.kind, 0};
      p.text(2 + num_w, y, d.kind == '-' ? "\xE2\x88\x92" : mark, Style{fg, th.panel, attr::kBold});
      p.text_clipped(4 + num_w, y, skip_cols(d.text, hscroll_), Style{d.kind == ' ' ? th.text : fg, th.panel},
                     std::max(0, W - num_w - 5));
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
    if (k.is('t')) { span_ = (span_ + 1) % kSpanCount; reset_detail(); return true; }
    if (k.is('g')) { by_chat_ = !by_chat_; sel_ = 0; sel_key_.clear(); reset_detail(); return true; }
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
    return {MenuItem{by_chat_ ? "Group by file" : "Group by chat", "group"}, MenuItem{"Next time range", "span"},
            MenuItem{app_->all_folders() ? "Only this folder" : "All folders", "all"},
            MenuItem{"Reread transcripts", "rescan"}};
  }
  void on_action(const std::string& a) override {
    if (a == "group") { by_chat_ = !by_chat_; sel_ = 0; sel_key_.clear(); reset_detail(); }
    else if (a == "span") { span_ = (span_ + 1) % kSpanCount; reset_detail(); }
    else if (a == "all") app_->set_all_folders(!app_->all_folders());
    else if (a == "rescan") index_.start(app_->store().projects(), app_->store());
  }

 private:
  struct Span {
    const char* label;
    int64_t seconds;  // 0: since local midnight; -1: all time
  };
  static constexpr int kSpanCount = 4;
  static constexpr Span kSpans[kSpanCount] = {
      {"today", 0}, {"last 7 days", 7 * 86400}, {"last 30 days", 30 * 86400}, {"all time", -1}};
  // A row of the diff below the list: an edit's header, one of its lines, or
  // a note in their place.
  static constexpr int kHeader = -1, kSpacer = -2, kMissing = -3, kMore = -4;
  // A new file of thousands of lines would bury the rest.
  static constexpr int kMaxLines = 400;
  struct Row {
    int edit;
    int line;
  };

  // The same index as Tools: a pass every few seconds, unchanged files from
  // its cache.
  void poll() {
    const int64_t now = int64_t(time(nullptr));
    if (index_.complete()) {
      if (now < next_pass_) return;
      index_.start(app_->store().projects(), app_->store());
      next_pass_ = now + 4;
    }
    index_.step(index_.done() == 0 ? 300 : App::kIndexSliceMs);
  }

  int64_t span_start_ms() const {
    const Span& s = kSpans[span_];
    const time_t now = time(nullptr);
    if (s.seconds < 0) return 0;
    if (s.seconds > 0) return (int64_t(now) - s.seconds) * 1000;
    tm local{};
    localtime_r(&now, &local);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    return int64_t(mktime(&local)) * 1000;
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

  void gather() {
    project_paths_.clear();
    for (const auto& pr : app_->store().projects()) project_paths_[pr.name] = pr.path;
    const int64_t from = span_start_ms();
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
        const std::string& key = by_chat_ ? c->path : e.file;
        auto [it, fresh] = at.try_emplace(key, groups.size());
        if (fresh) {
          Group g;
          g.key = key;
          g.label = by_chat_ ? chat_name(*c) : show_path(e.file, c->project);
          groups.push_back(std::move(g));
        }
        Group& g = groups[it->second];
        g.added += e.added;
        g.removed += e.removed;
        g.edits.push_back(Ref{c, &e});
        if (std::find(g.chats.begin(), g.chats.end(), c) == g.chats.end()) g.chats.push_back(c);
        if (std::find(g.agents.begin(), g.agents.end(), c->agent) == g.agents.end()) g.agents.push_back(c->agent);
        if (by_chat_ && std::find(g.files.begin(), g.files.end(), e.file) == g.files.end()) g.files.push_back(e.file);
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
                            std::to_string(newest.edit->offset) + (by_chat_ ? "c" : "f");
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

  const FileChange* loaded(const Ref& r) {
    const std::string key = cache_key(r);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.ok ? &it->second.fc : nullptr;
    if (cache_.size() > 600) cache_.clear();
    Loaded& l = cache_[key];
    l.ok = load_change(r.chat->path, r.chat->agent, r.chat->cwd, r.edit->offset, r.edit->file, l.fc);
    return l.ok ? &l.fc : nullptr;
  }

  // Lays out more changes until there are `want` rows, or all of them: each
  // is read from its transcript only when it comes into view.
  void extend(const Group& g, int want) {
    if (end_) want = 1 << 30;
    while (built_ < g.edits.size() && int(rows_.size()) < want) {
      const int e = int(built_++);
      rows_.push_back(Row{e, kHeader});
      const FileChange* fc = loaded(g.edits[size_t(e)]);
      if (!fc) {
        rows_.push_back(Row{e, kMissing});
      } else {
        const int n = int(fc->lines.size());
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
    int e = 0;
    if (scroll_ < int(rows_.size())) e = rows_[size_t(scroll_)].edit;
    open_edit(e);
  }

  ActivityIndex& index_;
  int64_t next_pass_ = 0;
  int& span_;
  bool& by_chat_;
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
  struct Loaded {
    FileChange fc;
    bool ok = false;
  };
  std::unordered_map<std::string, Loaded> cache_;
  std::vector<std::pair<int, int>> list_rows_;    // (screen row, group)
  std::vector<std::pair<int, int>> header_rows_;  // (screen row, change)
};

}  // namespace

PanePtr make_diff_view(ActivityIndex& index, DiffSettings& settings) {
  return std::make_unique<DiffView>(index, settings);
}

}  // namespace mico
