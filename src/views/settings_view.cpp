#include <algorithm>
#include <string>
#include <vector>

#include "core/session.h"
#include "core/settings.h"
#include "term/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

// One switch: its label, what it does, and how to read and flip it.
struct Toggle {
  const char* label;
  const char* detail;
  bool RenderSettings::*render = nullptr;  // a rendering switch, or…
  bool (*get)() = nullptr;                 // …an agent one, kept in its own file
  void (*set)(bool) = nullptr;
};

struct Section {
  const char* title;
  const char* note;
  std::vector<Toggle> toggles;
};

const std::vector<Section>& sections() {
  static const std::vector<Section> s = {
      {"Rendering", "how the chat draws what agents write; applies at once",
       {
           {"Pictures", "screenshots, image files and plots, at every density", &RenderSettings::pictures},
           {"LaTeX", "equations typeset as pictures (off: Unicode text)", &RenderSettings::math},
           {"Charts", "```chart blocks and the plot tool drawn (off: their JSON)", &RenderSettings::charts},
           {"Diagrams", "```mermaid flowcharts, sequences, states (off: the source)", &RenderSettings::diagrams},
           {"Code colours", "code coloured by its language", &RenderSettings::highlight},
           {"JSON results", "tool results that are JSON laid out, foldable", &RenderSettings::json},
           {"Notebooks", "notebooks read as cells, outputs, tables and plots", &RenderSettings::notebooks},
           {"Output colours", "command output keeps its ANSI colours", &RenderSettings::ansi},
           {"Links", "URLs and file paths found in text, clickable", &RenderSettings::links},
           {"Progress bars", "a running command's progress drawn in the activity row", &RenderSettings::progress},
       }},
      {"Agents", "what agents are told and given; applies to agents started from now on",
       {
           {"Agent hints", "a short note in their system prompt on the charts and diagrams mico draws", nullptr,
            agent_hints_enabled, set_agent_hints},
           {"Plot tool", "mico's MCP server, giving agents a tool that draws charts", nullptr, mcp_tools_enabled,
            set_mcp_tools},
       }},
  };
  return s;
}

class SettingsView final : public Pane {
 public:
  std::string title() const override { return "Settings"; }

  void render(Painter& p, bool focused) override {
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.panel});
    rows_.clear();
    const int W = p.width();
    int y = -scroll_, index = 0, label_w = 0;
    for (const auto& sec : sections())
      for (const auto& t : sec.toggles) label_w = std::max(label_w, text::str_width(t.label));
    const auto row = [&](auto&& draw) {
      if (y >= 0 && y < p.height()) draw(y);
      y++;
    };
    row([&](int r) {
      const int x = p.text(1, r, "Settings", Style{th.text, th.panel, attr::kBold}) + 3;
      p.text_clipped(x, r, "\xE2\x86\x91\xE2\x86\x93 choose \xC2\xB7 Space or click to switch", Style{th.dim, th.panel},
                     std::max(0, W - x - 1));
    });
    row([&](int r) { p.hline(1, r, std::max(0, W - 2), U'─', Style{th.border, th.panel}); });
    for (const auto& sec : sections()) {
      row([](int) {});
      row([&](int r) {
        const int x = p.text(1, r, sec.title, Style{th.heading, th.panel, attr::kBold}) + 2;
        p.text_clipped(x, r, sec.note, Style{th.dim, th.panel}, std::max(0, W - x - 1));
      });
      for (const auto& t : sec.toggles) {
        const bool on = value(t), sel = index == sel_;
        const int me = index++;
        row([&](int r) {
          rows_.push_back({r, me});
          const Style base{th.text, sel && focused ? th.sel_bg : th.panel};
          if (sel && focused) p.hline(0, r, W, U' ', base);
          int x = 3;
          x += p.text(x, r, on ? "\xE2\x97\x89 " : "\xE2\x97\x8B ", Style{on ? th.ok : th.dim, base.bg, attr::kBold});  // ◉ ○
          x += p.text(x, r, t.label, Style{on ? th.text : th.dim, base.bg, sel ? attr::kBold : uint16_t(0)});
          const int col = std::max(x + 2, 3 + 2 + label_w + 3);
          if (col < W - 2) p.text_clipped(col, r, t.detail, Style{th.dim, base.bg}, W - col - 1);
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
      case Key::Enter: flip(sel_); return true;
      default: break;
    }
    if (k.is(' ')) { flip(sel_); return true; }
    if (k.is('k')) { sel_ = std::max(0, sel_ - 1); return true; }
    if (k.is('j')) { sel_ = std::min(count_ - 1, sel_ + 1); return true; }
    return false;
  }

  bool on_mouse(const MouseEvent& m, Point local) override {
    if (m.kind == MouseKind::WheelUp) { scroll_ = std::max(0, scroll_ - 3); return true; }
    if (m.kind == MouseKind::WheelDown) { scroll_ = std::min(std::max(0, content_h_ - view_h_), scroll_ + 3); return true; }
    if (m.kind != MouseKind::Press || m.button != MouseButton::Left) return false;
    for (const auto& [y, i] : rows_)
      if (y == local.y) {
        sel_ = i;
        flip(i);
        return true;
      }
    return false;
  }

 private:
  static bool value(const Toggle& t) { return t.render ? render_settings().*t.render : t.get(); }

  void flip(int index) {
    int i = 0;
    for (const auto& sec : sections())
      for (const auto& t : sec.toggles) {
        if (i++ != index) continue;
        if (t.render) {
          RenderSettings s = render_settings();
          s.*t.render = !(s.*t.render);
          set_render_settings(s);
        } else {
          t.set(!t.get());
        }
        app_->set_status(std::string(t.label) + (value(t) ? ": on" : ": off"));
        return;
      }
  }

  int sel_ = 0, count_ = 0, scroll_ = 0, view_h_ = 0, content_h_ = 0;
  std::vector<std::pair<int, int>> rows_;  // screen row, toggle index
};

}  // namespace

PanePtr make_settings_view() { return std::make_unique<SettingsView>(); }

}  // namespace mico
