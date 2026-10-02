#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "adapters/adapters.h"
#include "core/usage.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

std::string tok(uint64_t n) {
  if (n == 0) return "—";
  char b[24];
  if (n >= 1000000) snprintf(b, sizeof b, "%.1fM", double(n) / 1e6);
  else if (n >= 1000) snprintf(b, sizeof b, "%.0fk", double(n) / 1e3);
  else snprintf(b, sizeof b, "%llu", (unsigned long long)n);
  return b;
}

std::string money(const UsageStat& s) {
  if (!s.has_cost) return "—";
  char b[24];
  snprintf(b, sizeof b, "$%.2f", s.cost_usd);
  return b;
}

std::string pct(double v) {
  char b[16];
  snprintf(b, sizeof b, "%.0f%%", v);
  return b;
}

std::string ago(int64_t sec) {
  if (sec <= 0) return "never";
  const int64_t d = int64_t(time(nullptr)) - sec;
  if (d < 2) return "just now";
  char b[24];
  if (d < 60) snprintf(b, sizeof b, "%llds ago", (long long)d);
  else if (d < 3600) snprintf(b, sizeof b, "%lldm ago", (long long)(d / 60));
  else snprintf(b, sizeof b, "%lldh ago", (long long)(d / 3600));
  return b;
}

std::string reset_in(int64_t when) {
  const int64_t d = when - int64_t(time(nullptr));
  if (d <= 0) return "now";
  char b[32];
  if (d >= 86400)
    snprintf(b, sizeof b, "%lldd %lldh", (long long)(d / 86400), (long long)((d % 86400) / 3600));
  else if (d >= 3600)
    snprintf(b, sizeof b, "%lldh %lldm", (long long)(d / 3600), (long long)((d % 3600) / 60));
  else
    snprintf(b, sizeof b, "%lldm", (long long)(d / 60));
  return b;
}

std::string trunc(std::string_view s, int w) {
  std::string out;
  int used = 0;
  for (size_t i = 0; i < s.size();) {
    size_t j = i;
    const char32_t cp = text::decode(s, j);
    const int cw = std::max(1, text::cp_width(cp));
    if (used + cw > w) break;
    out.append(s.substr(i, j - i));
    used += cw;
    i = j;
  }
  return out;
}

std::string pad_left(const std::string& s, int w) {
  const int n = w - text::str_width(s);  // columns, not bytes: "—" is three

  return n > 0 ? std::string(size_t(n), ' ') + s : s;
}

// A table row: name on the left, then fixed right-aligned numeric columns.
std::string row(const std::string& name, int name_w, const std::string* sessions,
                const std::string& cost, const std::string& prompt,
                const std::string& output, const std::string& cached) {
  std::string out = trunc(name, name_w);
  out += std::string(size_t(std::max(0, name_w - int(text::str_width(out)))), ' ');
  if (sessions) out += "  " + pad_left(*sessions, 8);
  out += "  " + pad_left(cost, 9);
  out += "  " + pad_left(prompt, 9);
  out += "  " + pad_left(output, 9);
  out += "  " + pad_left(cached, 7);
  return out;
}

struct Group {
  std::string name;
  UsageStat stat;
  int sessions = 0;
};

void add_group(std::vector<Group>& gs, const std::string& name, const UsageStat& s) {
  for (auto& g : gs)
    if (g.name == name) { g.stat.add(s); g.sessions++; return; }
  gs.push_back(Group{name, s, 1});
}

// Cost first when any of it is known, then raw volume. A project with no
// dollars (codex-only) still sorts by tokens below the ones that have them.
void sort_groups(std::vector<Group>& gs) {
  std::stable_sort(gs.begin(), gs.end(), [](const Group& a, const Group& b) {
    if (a.stat.has_cost != b.stat.has_cost) return a.stat.has_cost;
    if (a.stat.has_cost && a.stat.cost_usd != b.stat.cost_usd)
      return a.stat.cost_usd > b.stat.cost_usd;
    return a.stat.total > b.stat.total;
  });
}

class UsageView final : public Pane {
 public:
  explicit UsageView(UsageIndex& index) : index_(index) {}
  std::string title() const override { return "Usage"; }

  void render(Painter& p, bool focused) override {
    poll();
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.bg});

    const auto& es = index_.entries();
    UsageStat total;
    std::vector<Group> projects, models;
    const UsageEntry* quota = nullptr;
    const int64_t now = int64_t(time(nullptr));
    // What the sidebar selects: every folder, one, or one chat in it. The
    // rate limit is the account's, whichever chat reported it.
    const App::ViewFilter f = app_->view_filter();
    int counted = 0;
    for (const auto& e : es) {
      if (e.has_quota && e.quota_resets_at > now &&
          (!quota || e.quota_resets_at > quota->quota_resets_at))
        quota = &e;
      if (!app_->in_filter(e.project, e.path)) continue;
      counted++;
      total.add(e.stat);
      add_group(projects, e.project.empty() ? "(unknown)" : e.project, e.stat);
      // A model named but never billed for is only noise in the table.
      for (const auto& m : e.models)
        if (m.stat.prompt_tokens() + m.stat.output > 0 || m.stat.cost_usd > 0)
          add_group(models, m.model, m.stat);
    }
    // The account's rolling limit comes from the newest still-open window: a
    // stored session can carry a record whose window already reset, and
    // "0%, resets now" would be worse than showing nothing.
    sort_groups(projects);
    sort_groups(models);

    int y = 0;
    int hx = p.text(1, y, "Usage", Style{th.text, th.bg, attr::kBold}) + 3;
    hx += p.text_clipped(hx, y, f.label, Style{th.accent, th.bg}, std::max(0, p.width() - hx - 12));
    const std::string state =
        index_.complete()
            ? (index_.total() == 0 ? "no sessions" : ago(last_complete_))
            : "scanning " + std::to_string(index_.done()) + "/" + std::to_string(index_.total());
    p.text_clipped(hx + 3, y, state, Style{th.dim, th.bg}, std::max(0, p.width() - hx - 4));
    y++;
    p.hline(1, y, std::max(0, p.width() - 2), U'─', Style{th.border, th.bg});
    y += 1;

    // The accounts' limits: claude's as its status line last saw them, and
    // codex's from its newest window. One whose window has started over since
    // is not shown: what it holds now nobody has reported.
    std::vector<PlanLimit> limits;
    for (const Adapter* a : all_adapters()) a->plan_limits(limits);
    if (quota)
      limits.push_back(PlanLimit{quota->agent, window_name(quota->quota_window_minutes), quota->quota_used_pct,
                                 quota->quota_resets_at, quota->mtime});
    std::erase_if(limits, [&](const PlanLimit& l) { return l.resets_at <= now; });
    y = render_limits(p, th, y, limits);

    if (y < p.height()) {
      char line[192];
      snprintf(line, sizeof line, "total    %s   ·   %s in / %s out   ·   %s cached   ·   %d sessions",
               money(total).c_str(), tok(total.prompt_tokens()).c_str(),
               tok(total.output).c_str(), pct(total.cached_pct()).c_str(), counted);
      p.text_clipped(1, y, line, Style{th.text, th.bg, attr::kBold}, std::max(0, p.width() - 2));
      y++;
    }

    const int name_w = std::max(10, p.width() - 2 - 2 - (8 + 9 + 9 + 9 + 7) - 5 * 2);
    y = render_table(p, th, y, projects, name_w, true, "project");
    y = render_table(p, th, y, models, name_w, false, "model");

    if (projects.empty() && index_.complete())
      p.text(1, std::min(y + 1, p.height() - 1),
             es.empty() ? "no usage found — start an agent, or press r to rescan"
                        : "no usage in " + f.label + " — pick another folder or chat on the left",
             Style{th.dim, th.bg});
  }

  bool on_key(const KeyEvent& k) override {
    if (k.is('r')) { rescan(); return true; }
    return false;
  }

  std::vector<MenuItem> context_menu(Point) override {
    return {MenuItem{"Rescan usage now", "rescan"}};
  }
  void on_action(const std::string& a) override {
    if (a == "rescan") rescan();
  }

 private:
  // A pass is restarted every few seconds. start() drops the job list and fills
  // the visible entries from the mtime cache, so a refresh after nothing moved
  // is a stat() per session; only files the agents changed are read again.
  void poll() {
    const int64_t now = int64_t(time(nullptr));
    if (index_.complete()) {
      if (now < next_pass_) return;
      index_.start(app_->store().projects());
      next_pass_ = now + kRefreshSeconds;
      if (index_.complete()) { last_complete_ = now; return; }  // all cached
    }
    // The first frame gets a larger slice: opening the tab should show real
    // numbers, not an empty "scanning 0/N". After that a frame's worth keeps
    // the UI responsive while the rest of the pass lands.
    if (index_.step(index_.done() == 0 ? 300 : App::kIndexSliceMs)) last_complete_ = now;
  }

  void rescan() {
    index_.start(app_->store().projects());
    next_pass_ = int64_t(time(nullptr)) + kRefreshSeconds;
    if (index_.complete()) last_complete_ = int64_t(time(nullptr));
  }

  // One row per limit: the agent and the window it spans, a bar, how much
  // is used, when it starts over, and how old the reading is once that
  // matters (claude's comes with its requests: none lately, none newer).
  int render_limits(Painter& p, const Theme& th, int y, const std::vector<PlanLimit>& limits) {
    if (limits.empty()) return y;
    int agent_w = 0, window_w = 0;
    for (const auto& l : limits) {
      agent_w = std::max(agent_w, text::str_width(l.agent));
      window_w = std::max(window_w, text::str_width(l.window));
    }
    const int bar_w = std::min(24, std::max(8, p.width() / 4));
    const int64_t now = int64_t(time(nullptr));
    for (const auto& l : limits) {
      if (y >= p.height()) return y;
      const double frac = std::clamp(l.used_pct / 100.0, 0.0, 1.0);
      const int filled = int(frac * bar_w + 0.5);
      p.text(1, y, l.agent, Style{th.accent, th.bg, attr::kBold});
      int x = 1 + agent_w + 2;
      p.text(x, y, l.window, Style{th.dim, th.bg});
      x += window_w + 2;
      for (int i = 0; i < bar_w; i++)
        p.put(x + i, y, i < filled ? U'█' : U'░',
              Style{i < filled ? (frac > 0.85 ? th.err : th.accent) : th.border, th.bg});
      x += bar_w + 2;
      char buf[40];
      snprintf(buf, sizeof buf, "%5.1f%%", l.used_pct);
      x += p.text(x, y, buf, Style{th.text, th.bg, attr::kBold}) + 2;
      std::string when = "resets in " + reset_in(l.resets_at);
      if (now - l.as_of >= 600) when += " \xC2\xB7 as of " + ago(l.as_of);
      p.text_clipped(x, y, when, Style{th.dim, th.bg}, std::max(0, p.width() - x - 1));
      y++;
    }
    return y + 1;
  }

  // A codex window, by its length.
  static std::string window_name(int64_t minutes) {
    if (minutes == 10080) return "week";
    if (minutes > 0 && minutes % 1440 == 0) return std::to_string(minutes / 1440) + " days";
    if (minutes > 0 && minutes % 60 == 0) return std::to_string(minutes / 60) + " hours";
    return minutes > 0 ? std::to_string(minutes) + " minutes" : "window";
  }

  int render_table(Painter& p, const Theme& th, int y, const std::vector<Group>& gs,
                   int name_w, bool with_sessions, const char* label) {
    if (gs.empty() || y + 1 >= p.height()) return y;
    const std::string sessions = with_sessions ? "sessions" : std::string();
    p.text_clipped(1, y, row(label, name_w, with_sessions ? &sessions : nullptr, "cost",
                             "in", "out", "cached"),
                   Style{th.dim, th.bg, attr::kBold}, std::max(0, p.width() - 2));
    y++;
    for (const auto& g : gs) {
      if (y >= p.height()) break;
      std::string sc = with_sessions ? std::to_string(g.sessions) : std::string();
      p.text_clipped(1, y,
                     row(g.name, name_w, with_sessions ? &sc : nullptr, money(g.stat),
                         tok(g.stat.prompt_tokens()), tok(g.stat.output),
                         pct(g.stat.cached_pct())),
                     Style{th.text, th.bg}, std::max(0, p.width() - 2));
      y++;
    }
    return y + 1;
  }

  static constexpr int kRefreshSeconds = 4;
  UsageIndex& index_;
  int64_t next_pass_ = 0;
  int64_t last_complete_ = 0;
};

}  // namespace

PanePtr make_usage_view(UsageIndex& index) { return std::make_unique<UsageView>(index); }

}  // namespace mico
