#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "core/activity.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

std::string dur(int64_t ms) {
  char b[32];
  if (ms < 0) return "—";
  if (ms < 60000) snprintf(b, sizeof b, "%.1fs", double(ms) / 1000.0);
  else if (ms < 3600000) snprintf(b, sizeof b, "%lldm %02llds", (long long)(ms / 60000), (long long)(ms / 1000 % 60));
  else snprintf(b, sizeof b, "%lldh %02lldm", (long long)(ms / 3600000), (long long)(ms / 60000 % 60));
  return b;
}

std::string count(size_t n) {
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
    int cw;
    i = text::glyph_end(s, i, &cw);
    cw = std::max(1, cw);
    if (used + cw > w - 1) break;
    out.append(s.substr(at, i - at));
    used += cw;
  }
  return out + "\xE2\x80\xA6";
}

std::string pad(const std::string& s, int w, bool right) {
  const int n = w - text::str_width(s);
  if (n <= 0) return s;
  return right ? std::string(size_t(n), ' ') + s : s + std::string(size_t(n), ' ');
}

// A share of the whole, in eighths of a cell.
std::string bar(double frac, int w) {
  static const char* kEighths[] = {"", "\xE2\x96\x8F", "\xE2\x96\x8E", "\xE2\x96\x8D", "\xE2\x96\x8C",
                                   "\xE2\x96\x8B", "\xE2\x96\x8A", "\xE2\x96\x89"};
  int e = int(std::clamp(frac, 0.0, 1.0) * w * 8 + 0.5);
  std::string out;
  for (; e >= 8; e -= 8) out += "\xE2\x96\x88";
  out += kEighths[e];
  return out;
}

struct Bucket {
  std::string name;
  size_t calls = 0, failed = 0;
  int64_t total = 0, slowest = 0;
  std::vector<int64_t> durs;
  int64_t median() {
    if (durs.empty()) return -1;
    std::nth_element(durs.begin(), durs.begin() + long(durs.size() / 2), durs.end());
    return durs[durs.size() / 2];
  }
  void add(int64_t d, bool fail) {
    calls++;
    failed += fail;
    total += d;
    slowest = std::max(slowest, d);
    durs.push_back(d);
  }
};

struct Slow {
  int64_t dur;
  const ChatActivity* chat;
  const ToolRun* run;
};

// Where the agents' time went: tool calls by kind and by command, and the
// slowest of them, for this folder or all of them, over a chosen span.
class ToolsView final : public Pane {
 public:
  explicit ToolsView(ActivityIndex& index) : index_(index) {}
  std::string title() const override { return "Tools"; }

  void render(Painter& p, bool focused) override {
    poll();
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.panel});
    gather();

    int y = -scroll_;
    const auto row = [&](auto&& draw) {
      if (y >= 0 && y < p.height()) draw(y);
      y++;
    };
    const int W = p.width();

    // Heading: what is shown, and how to change it.
    row([&](int r) {
      int x = p.text(1, r, "Tools", Style{th.text, th.panel, attr::kBold}) + 3;
      // What it covers is what the sidebar has selected.
      const std::string scope = app_->view_filter().label;
      x += p.text_clipped(x, r, scope + " \xC2\xB7 " + kSpans[span_].label, Style{th.accent, th.panel},
                          std::max(0, W - x - 1));
      std::string state = index_.complete() ? "" : "  reading " + std::to_string(index_.done()) + "/" + std::to_string(index_.total());
      p.text_clipped(x, r, state + "    t time range \xC2\xB7 a " + (app_->all_folders() ? "this folder" : "all folders"),
                     Style{th.dim, th.panel}, std::max(0, W - x - 1));
    });
    row([&](int r) { p.hline(1, r, std::max(0, W - 2), U'─', Style{th.border, th.panel}); });

    // The headline.
    row([&](int r) {
      std::string s = dur(work_total_) + " running tools \xC2\xB7 " + dur(wait_total_) + " waiting for you \xC2\xB7 " +
                      count(calls_) + " calls";
      if (calls_) {
        char b[32];
        snprintf(b, sizeof b, " \xC2\xB7 %.1f%% failed", 100.0 * double(failed_) / double(calls_));
        s += b;
      }
      p.text_clipped(1, r, s, Style{th.text, th.panel, attr::kBold}, std::max(0, W - 2));
    });
    row([](int) {});

    // By kind.
    const int name_w = 16, num_w = 8, time_w = 9, bar_w = std::max(6, std::min(28, W - 1 - name_w - num_w - 3 * time_w - 7 * 2 - 6));
    row([&](int r) {
      p.text_clipped(1, r,
                     pad("kind", name_w, false) + "  " + pad("calls", num_w, true) + "  " + pad("time", time_w, true) + "  " +
                         pad("share", bar_w, false) + "  " + pad("median", time_w, true) + "  " + pad("slowest", time_w, true) +
                         "  " + pad("failed", 6, true),
                     Style{th.dim, th.panel, attr::kBold}, std::max(0, W - 2));
    });
    const int64_t share_of = std::max<int64_t>(1, work_total_);
    for (auto& b : kinds_) {
      if (!b.calls) continue;
      const bool wait = b.name == tool_kind_name(ToolKind::Wait);
      row([&](int r) {
        int x = 1;
        x += p.text(x, r, pad(b.name, name_w, false) + "  " + pad(count(b.calls), num_w, true) + "  " + pad(dur(b.total), time_w, true) + "  ",
                    Style{wait ? th.dim : th.text, th.panel});
        if (!wait) p.text(x, r, bar(double(b.total) / double(share_of), bar_w), Style{th.accent, th.panel});
        x += bar_w + 2;
        x += p.text(x, r, pad(dur(b.median()), time_w, true) + "  " + pad(dur(b.slowest), time_w, true) + "  ",
                    Style{wait ? th.dim : th.text, th.panel});
        p.text(x, r, pad(b.failed ? count(b.failed) : "", 6, true), Style{th.err, th.panel});
      });
    }
    row([](int) {});

    // By command.
    const int cmd_w = std::max(16, std::min(40, W - 2 - num_w - 3 * time_w - 6 - 5 * 2));
    row([&](int r) {
      p.text_clipped(1, r,
                     pad("command", cmd_w, false) + "  " + pad("calls", num_w, true) + "  " + pad("time", time_w, true) + "  " +
                         pad("median", time_w, true) + "  " + pad("slowest", time_w, true) + "  " + pad("failed", 6, true),
                     Style{th.dim, th.panel, attr::kBold}, std::max(0, W - 2));
    });
    for (size_t i = 0; i < groups_.size() && i < 15; i++) {
      Bucket& b = groups_[i];
      row([&](int r) {
        int x = 1;
        x += p.text(x, r, pad(fit(b.name, cmd_w), cmd_w, false) + "  " + pad(count(b.calls), num_w, true) + "  " +
                              pad(dur(b.total), time_w, true) + "  " + pad(dur(b.median()), time_w, true) + "  " +
                              pad(dur(b.slowest), time_w, true) + "  ",
                    Style{th.text, th.panel});
        p.text(x, r, pad(b.failed ? count(b.failed) : "", 6, true), Style{th.err, th.panel});
      });
    }
    row([](int) {});

    // The slowest calls: choose one to open its chat there.
    row([&](int r) {
      int x = p.text(1, r, "slowest calls", Style{th.dim, th.panel, attr::kBold});
      p.text_clipped(x + 3, r, "\xE2\x86\x91/\xE2\x86\x93 choose \xC2\xB7 enter opens the chat at the call",
                     Style{th.dim, th.panel}, std::max(0, W - x - 4));
    });
    sel_ = std::clamp(sel_, 0, std::max(0, int(slow_.size()) - 1));
    slow_rows_.clear();
    const int list_top = y;
    for (int i = 0; i < int(slow_.size()); i++) {
      const Slow& s = slow_[size_t(i)];
      const bool sel = i == sel_;
      const int at = y;
      row([&](int r) {
        const Color bg = sel ? th.sel_bg : th.panel;
        if (sel) p.fill(Rect{1, r, W - 2, 1}, Style{th.text, bg});
        if (sel) p.put(1, r, U'❯', Style{th.accent, bg, attr::kBold});
        int x = 3;
        x += p.text(x, r, pad(dur(s.dur), time_w, true) + "  ", Style{th.text, bg, attr::kBold});
        x += p.text(x, r, pad(tool_kind_name(s.run->kind), 13, false) + " ", Style{th.accent, bg});
        const std::string where = "  " + fit(s.chat->title.empty() ? agent_label(s.chat->agent) + " chat" : s.chat->title, 30) +
                                  " \xC2\xB7 " + ago(s.run->start_ms);
        const int room = std::max(0, W - x - 2 - text::str_width(where));
        x += p.text(x, r, fit(s.run->command.empty() ? s.run->tool : s.run->command, room),
                    Style{s.run->failed ? th.err : th.text, bg});
        p.text_clipped(std::max(x, W - 2 - text::str_width(where)), r, where, Style{th.dim, bg}, W - 2 - x);
      });
      slow_rows_.push_back({at, i});
    }
    if (slow_.empty()) row([&](int r) { p.text(3, r, index_.complete() ? "no tool calls in this span" : "reading transcripts\xE2\x80\xA6", Style{th.dim, th.panel}); });
    content_h_ = y + scroll_;
    // Keep the chosen call in view as the arrows move it.
    if (keep_sel_visible_) {
      keep_sel_visible_ = false;
      const int sel_y = list_top + scroll_ + sel_;
      if (sel_y - scroll_ >= p.height()) scroll_ = sel_y - p.height() + 2;
      if (sel_y - scroll_ < 0) scroll_ = sel_y;
    }
    view_h_ = p.height();
    (void)focused;
  }

  bool on_key(const KeyEvent& k) override {
    switch (k.key) {
      case Key::Up: sel_ = std::max(0, sel_ - 1); keep_sel_visible_ = true; return true;
      case Key::Down: sel_ = std::min(std::max(0, int(slow_.size()) - 1), sel_ + 1); keep_sel_visible_ = true; return true;
      case Key::PageUp: scroll_ = std::max(0, scroll_ - std::max(1, view_h_ - 2)); return true;
      case Key::PageDown: scroll_ = std::min(std::max(0, content_h_ - view_h_), scroll_ + std::max(1, view_h_ - 2)); return true;
      case Key::Home: scroll_ = 0; sel_ = 0; return true;
      case Key::Enter: open_selected(); return true;
      default: break;
    }
    if (k.is('t')) { span_ = (span_ + 1) % kSpanCount; sel_ = 0; return true; }
    if (k.is('a')) { app_->set_all_folders(!app_->all_folders()); sel_ = 0; return true; }
    if (k.is('r')) { index_.start(app_->store().projects(), app_->store()); return true; }
    return false;
  }
  bool on_mouse(const MouseEvent& m, Point local) override {
    if (m.kind == MouseKind::WheelUp) { scroll_ = std::max(0, scroll_ - 3); return true; }
    if (m.kind == MouseKind::WheelDown) { scroll_ = std::min(std::max(0, content_h_ - view_h_), scroll_ + 3); return true; }
    if (m.kind != MouseKind::Press || m.button != MouseButton::Left) return false;
    for (const auto& [y, i] : slow_rows_)
      if (y == local.y) {
        sel_ = i;
        open_selected();
        return true;
      }
    return false;
  }
  std::vector<MenuItem> context_menu(Point) override {
    return {MenuItem{"Next time range", "span"},
            MenuItem{app_->all_folders() ? "Only this folder" : "All folders", "all"},
            MenuItem{"Reread transcripts", "rescan"}};
  }
  void on_action(const std::string& a) override {
    if (a == "span") span_ = (span_ + 1) % kSpanCount;
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

  // A pass is restarted every few seconds; unchanged files come from the
  // cache, so a quiet refresh is a stat() per chat.
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

  void gather() {
    kinds_.assign(size_t(kToolKinds), Bucket{});
    for (int k = 0; k < kToolKinds; k++) kinds_[size_t(k)].name = tool_kind_name(ToolKind(k));
    std::map<std::string, Bucket> groups;
    slow_.clear();
    work_total_ = wait_total_ = 0;
    calls_ = failed_ = 0;
    const int64_t from = span_start_ms();
    const App::ViewFilter f = app_->view_filter();
    for (const ChatActivity* c : index_.chats()) {
      if (!app_->in_filter(c->project, c->path)) continue;
      for (const ToolRun& r : c->runs) {
        if (r.dur_ms < 0 || r.start_ms < from) continue;
        const bool wait = r.waited || r.kind == ToolKind::Wait;
        calls_++;
        failed_ += r.failed;
        kinds_[size_t(wait ? int(ToolKind::Wait) : int(r.kind))].add(r.dur_ms, r.failed);
        if (wait) {
          wait_total_ += r.dur_ms;
          continue;
        }
        work_total_ += r.dur_ms;
        Bucket& g = groups[r.group];
        g.name = r.group;
        g.add(r.dur_ms, r.failed);
        slow_.push_back(Slow{r.dur_ms, c, &r});
      }
    }
    // Biggest first; waiting last, as the odd one out.
    std::stable_sort(kinds_.begin(), kinds_.end(), [](const Bucket& a, const Bucket& b) {
      const bool wa = a.name == tool_kind_name(ToolKind::Wait), wb = b.name == tool_kind_name(ToolKind::Wait);
      if (wa != wb) return wb;
      return a.total > b.total;
    });
    groups_.clear();
    for (auto& [name, b] : groups) groups_.push_back(std::move(b));
    std::sort(groups_.begin(), groups_.end(), [](const Bucket& a, const Bucket& b) { return a.total > b.total; });
    const size_t keep = std::min<size_t>(slow_.size(), 50);
    std::partial_sort(slow_.begin(), slow_.begin() + long(keep), slow_.end(),
                      [](const Slow& a, const Slow& b) { return a.dur > b.dur; });
    slow_.resize(keep);
  }

  void open_selected() {
    if (sel_ < 0 || sel_ >= int(slow_.size())) return;
    const Slow& s = slow_[size_t(sel_)];
    app_->open_at(s.chat->path, s.run->offset, {});
  }

  ActivityIndex& index_;
  int64_t next_pass_ = 0;
  int span_ = 1;  // last 7 days
  int sel_ = 0, scroll_ = 0, content_h_ = 0, view_h_ = 1;
  bool keep_sel_visible_ = false;
  std::vector<Bucket> kinds_, groups_;
  std::vector<Slow> slow_;
  std::vector<std::pair<int, int>> slow_rows_;  // (screen row, index in slow_)
  int64_t work_total_ = 0, wait_total_ = 0;
  size_t calls_ = 0, failed_ = 0;
};

}  // namespace

PanePtr make_tools_view(ActivityIndex& index) { return std::make_unique<ToolsView>(index); }

}  // namespace mico
