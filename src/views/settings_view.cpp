#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "core/away.h"
#include "core/scope.h"
#include "core/session.h"
#include "core/settings.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

// One setting: its label, the ways it can be, and how to read and set it.
// Every row is a choice among a few named ways; a switch is just "off · on".
struct Row {
  std::string label;
  std::vector<std::string> names;    // the ways, as shown
  std::vector<std::string> details;  // what each way does
  std::function<int()> get;
  std::function<void(int)> set;
};

struct Section {
  const char* title;
  const char* note;
  std::vector<Row> rows;
};

// A rendering choice, read from and written to the render settings.
Row render_row(size_t part) {
  const RenderChoice& c = render_choices()[part];
  Row r;
  r.label = c.label;
  for (const auto& v : c.variants) {
    r.names.emplace_back(v.name);
    r.details.emplace_back(v.detail);
  }
  r.get = [part] { return int(render_settings().way[part]); };
  r.set = [part](int i) {
    RenderSettings s = render_settings();
    s.way[part] = uint8_t(i);
    set_render_settings(s);
  };
  return r;
}

// An agent switch, kept in its own file.
Row switch_row(const char* label, const char* off, const char* on, bool (*get)(), void (*set)(bool)) {
  Row r;
  r.label = label;
  r.names = {"off", "on"};
  r.details = {off, on};
  r.get = [get] { return get() ? 1 : 0; };
  r.set = [set](int i) { set(i == 1); };
  return r;
}

std::vector<Section> sections() {
  std::vector<Section> s;
  Row theme;
  theme.label = "Theme";
  for (const auto& t : themes()) {
    theme.names.emplace_back(t.name);
    theme.details.emplace_back(t.detail);
  }
  theme.get = [] {
    const auto& all = themes();
    for (size_t i = 0; i < all.size(); i++)
      if (render_settings().theme == all[i].name) return int(i);
    return 0;
  };
  theme.set = [](int i) {
    RenderSettings rs = render_settings();
    rs.theme = themes()[size_t(i)].name;
    set_render_settings(rs);
  };
  s.push_back({"Appearance", "the colours everything is drawn in; applies at once", {}});
  s.back().rows.push_back(std::move(theme));
  Row layout;
  layout.label = "Layout";
  layout.names = {"auto", "wide", "compact"};
  layout.details = {"compact below 80 columns, as on a phone; wide above",
                    "the sidebar and the chat side by side, whatever the width",
                    "one pane at a time, with a bar to go back and a menu (\xE2\x89\xA1)"};
  layout.get = [] { return int(layout_mode()); };
  layout.set = [](int i) { set_layout_mode(LayoutMode(i)); };
  s.back().rows.push_back(std::move(layout));

  s.push_back({"Rendering", "how the chat draws what agents write; applies at once", {}});
  for (size_t part = 0; part < render_choices().size(); part++) s.back().rows.push_back(render_row(part));

  s.push_back({"Agents", "what agents are told and given; applies to agents started from now on", {}});
  s.back().rows.push_back(switch_row("Agent hints", "not told", "a short note in their system prompt on the charts and diagrams mico draws",
                                     agent_hints_enabled, set_agent_hints));
  s.back().rows.push_back(switch_row("Plot tool", "not given", "mico's MCP server, giving agents a tool that draws charts",
                                     mcp_tools_enabled, set_mcp_tools));
  s.back().rows.push_back(switch_row("Usage limits", "not read",
                                     "claude's subscription limits, through its status line, for the Usage tab",
                                     plan_limits_enabled, set_plan_limits));

  s.push_back({"Away", "what mico does while you look elsewhere, or after it stops", {}});
  Row notify;
  notify.label = "Notifications";
  notify.names = {"off", "bell", "desktop"};
  notify.details = {"an agent that finished or needs you shows only in the sidebar and the status bar",
                    "the terminal's bell, when its window is not the one in front",
                    "a desktop notification through the terminal (kitty, Ghostty, WezTerm, foot, iTerm2; "
                    "the bell elsewhere), or notify-send with no terminal attached"};
  notify.get = [] { return int(notify_mode()); };
  notify.set = [](int i) { set_notify_mode(NotifyMode(i)); };
  s.back().rows.push_back(std::move(notify));
  s.back().rows.push_back(switch_row("Resume agents", "agents end with the daemon",
                                     "agents still running when the daemon went (a reboot, a crash, mico kill) "
                                     "are resumed when it starts again; :quit ends them for good",
                                     restore_agents_enabled, set_restore_agents));
  s.back().rows.push_back(switch_row("Own scopes", "agents share the daemon's cgroup, and die with it",
                                     "each agent, and the daemon, in a systemd scope of its own: killed for memory, "
                                     "an agent's work takes only that agent (agents started from now on)",
                                     scope::enabled, scope::set_enabled));
  return s;
}

class SettingsView final : public Pane {
 public:
  std::string title() const override { return "Settings"; }

  void render(Painter& p, bool focused) override {
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.panel});
    hits_.clear();
    rows_.clear();
    sections_ = sections();
    const int W = p.width();
    int y = -scroll_, index = 0, label_w = 0, ways_w = 0;
    for (const auto& sec : sections_)
      for (const auto& r : sec.rows) {
        label_w = std::max(label_w, text::str_width(r.label));
        int w = 0;
        for (const auto& n : r.names) w += text::str_width(n) + 3;
        ways_w = std::max(ways_w, w);
      }
    // What each way does is set in one column, to the right of the widest row of ways.
    const int detail_x = 3 + label_w + 3 + ways_w + 1;
    const auto line = [&](auto&& draw) {
      if (y >= 0 && y < p.height()) draw(y);
      y++;
    };
    line([&](int r) {
      const int x = p.text(1, r, "Settings", Style{th.text, th.panel, attr::kBold}) + 3;
      p.text_clipped(x, r, "\xE2\x86\x91\xE2\x86\x93 choose \xC2\xB7 \xE2\x86\x90\xE2\x86\x92 or click a way \xC2\xB7 Space next",
                     Style{th.dim, th.panel}, std::max(0, W - x - 1));
    });
    line([&](int r) { p.hline(1, r, std::max(0, W - 2), U'─', Style{th.border, th.panel}); });
    for (const auto& sec : sections_) {
      line([](int) {});
      line([&](int r) {
        const int x = p.text(1, r, sec.title, Style{th.heading, th.panel, attr::kBold}) + 2;
        p.text_clipped(x, r, sec.note, Style{th.dim, th.panel}, std::max(0, W - x - 1));
      });
      for (const auto& row : sec.rows) {
        const int me = index++;
        const bool sel = me == sel_;
        const int cur = std::clamp(row.get(), 0, int(row.names.size()) - 1);
        line([&](int r) {
          rows_.push_back({r, me});
          const Color bg = sel && focused ? th.sel_bg : th.panel;
          if (sel && focused) p.hline(0, r, W, U' ', Style{th.text, bg});
          int x = 3;
          x += p.text(x, r, row.label, Style{th.text, bg, sel ? attr::kBold : uint16_t(0)});
          x = std::max(x + 2, 3 + label_w + 3);
          // The ways side by side, the one in force filled in.
          for (size_t i = 0; i < row.names.size() && x < W - 2; i++) {
            const std::string pill = " " + row.names[i] + " ";
            const bool on = int(i) == cur;
            const Style st = on ? Style{th.panel, th.accent, attr::kBold} : Style{th.dim, bg};
            const int w = p.text_clipped(x, r, pill, st, W - x - 1);
            hits_.push_back({r, x, x + w, me, int(i)});
            x += w + 1;
          }
          if (detail_x + 3 < W - 2)
            p.text_clipped(detail_x, r, row.details[size_t(cur)], Style{th.code_comment, bg}, W - detail_x - 1);
        });
      }
    }
    count_ = index;
    view_h_ = p.height();
    content_h_ = y + scroll_;
  }

  bool on_key(const KeyEvent& k) override {
    switch (k.key) {
      case Key::Up: sel_ = std::max(0, sel_ - 1); return true;
      case Key::Down: sel_ = std::min(count_ - 1, sel_ + 1); return true;
      case Key::Home: sel_ = 0; return true;
      case Key::End: sel_ = count_ - 1; return true;
      case Key::Left: step(sel_, -1, false); return true;
      case Key::Right: step(sel_, +1, false); return true;
      case Key::Enter: step(sel_, +1, true); return true;
      default: break;
    }
    if (k.is(' ')) { step(sel_, +1, true); return true; }
    if (k.is('k')) { sel_ = std::max(0, sel_ - 1); return true; }
    if (k.is('j')) { sel_ = std::min(count_ - 1, sel_ + 1); return true; }
    if (k.is('h')) { step(sel_, -1, false); return true; }
    if (k.is('l')) { step(sel_, +1, false); return true; }
    return false;
  }

  bool on_mouse(const MouseEvent& m, Point local) override {
    if (m.kind == MouseKind::WheelUp) { scroll_ = std::max(0, scroll_ - 3); return true; }
    if (m.kind == MouseKind::WheelDown) { scroll_ = std::min(std::max(0, content_h_ - view_h_), scroll_ + 3); return true; }
    if (m.kind != MouseKind::Press || m.button != MouseButton::Left) return false;
    for (const auto& h : hits_)
      if (h.y == local.y && local.x >= h.x0 && local.x < h.x1) {
        sel_ = h.row;
        choose(h.row, h.way);
        return true;
      }
    for (const auto& [y, i] : rows_)
      if (y == local.y) {
        sel_ = i;
        step(i, +1, true);
        return true;
      }
    return false;
  }

 private:
  Row* row_at(int index) {
    int i = 0;
    for (auto& sec : sections_)
      for (auto& r : sec.rows)
        if (i++ == index) return &r;
    return nullptr;
  }

  // Moves `index`'s choice by `delta`, around the ends when `wrap`.
  void step(int index, int delta, bool wrap) {
    Row* r = row_at(index);
    if (!r) return;
    const int n = int(r->names.size());
    int i = r->get() + delta;
    if (wrap) i = (i % n + n) % n;
    else i = std::clamp(i, 0, n - 1);
    choose(index, i);
  }

  void choose(int index, int way) {
    Row* r = row_at(index);
    if (!r || way < 0 || way >= int(r->names.size())) return;
    r->set(way);
    app_->set_status(r->label + ": " + r->names[size_t(way)] + " \xE2\x80\x94 " + r->details[size_t(way)]);
  }

  struct Hit {
    int y, x0, x1, row, way;
  };
  std::vector<Section> sections_;
  int sel_ = 0, count_ = 0, scroll_ = 0, view_h_ = 0, content_h_ = 0;
  std::vector<std::pair<int, int>> rows_;  // screen row, setting index
  std::vector<Hit> hits_;                  // where each way was drawn
};

}  // namespace

PanePtr make_settings_view() { return std::make_unique<SettingsView>(); }

}  // namespace mico
