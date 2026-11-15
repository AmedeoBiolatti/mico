#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include "adapters/adapters.h"
#include "core/chips.h"
#include "core/models.h"
#include "core/session.h"
#include "model/state.h"
#include "base/progress.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {

std::string background_summary(const std::vector<BackgroundTask>& tasks) {
  int monitors = 0, commands = 0, agents = 0;
  for (const auto& t : tasks) (t.kind == "monitor" ? monitors : t.kind == "agent" ? agents : commands)++;
  std::string s;
  if (monitors) s = std::to_string(monitors) + (monitors == 1 ? " monitor" : " monitors");
  if (commands)
    s += (s.empty() ? "" : " \xC2\xB7 ") + std::to_string(commands) + (commands == 1 ? " command" : " commands");
  if (agents)
    s += (s.empty() ? "" : " \xC2\xB7 ") + std::to_string(agents) + (agents == 1 ? " agent" : " agents");
  return s;
}

std::string background_progress(const BackgroundTask& t, int cols) {
  if (t.fraction < 0) return {};
  const double f = std::clamp(t.fraction, 0.0, 1.0);
  std::string s;
  if (cols > 0) s = "\xE2\x96\x95" + progress::bar(f, cols) + "\xE2\x96\x8F ";
  s += std::to_string(int(f * 100 + 0.5)) + "%";
  if (t.done >= 0 && t.total > 0) s += " \xC2\xB7 " + std::to_string(t.done) + "/" + std::to_string(t.total);
  int left = t.eta_s;
  if (left < 0 && t.since_ms > 0 && f > t.since_fraction && f < 1) {
    const double secs = double(int64_t(time(nullptr)) * 1000 - t.since_ms) / 1000.0;
    if (secs >= 3) left = int((1 - f) * secs / (f - t.since_fraction));
  }
  if (left >= 0 && f < 1) s += " \xC2\xB7 " + progress::duration(left) + " left";
  return s;
}

std::string background_percent(const std::vector<BackgroundTask>& tasks) {
  for (const auto& t : tasks)
    if (t.fraction >= 0) return " " + std::to_string(int(std::clamp(t.fraction, 0.0, 1.0) * 100 + 0.5)) + "%";
  return {};
}

std::vector<PickItem> background_items(const std::vector<BackgroundTask>& tasks, const Theme& th) {
  const int64_t now = int64_t(time(nullptr)) * 1000;
  const auto span = [](int64_t ms) {
    const int64_t s = std::max<int64_t>(0, ms / 1000);
    return s < 60 ? std::to_string(s) + "s" : s < 3600 ? std::to_string(s / 60) + "m" : std::to_string(s / 3600) + "h";
  };
  std::vector<PickItem> items;
  for (const auto& t : tasks) {
    PickItem it;
    it.label = t.what.empty() ? t.id : t.what;
    // ◉ a monitor, ◆ an agent, ▶ a command
    it.lead = t.kind == "monitor" ? "\xE2\x97\x89" : t.kind == "agent" ? "\xE2\x97\x86" : "\xE2\x96\xB6";
    it.lead_color = th.working;
    it.group = t.kind == "monitor" ? "Monitors" : t.kind == "agent" ? "Agents" : "Commands";
    std::string d = background_progress(t, 10);
    d += (d.empty() ? "" : " \xC2\xB7 ") + (t.started_ms ? "running " + span(now - t.started_ms) : std::string("running"));
    if (t.expires_ms) d += ", ends in " + span(t.expires_ms - now);
    if (t.events)
      d += " \xC2\xB7 " + std::to_string(t.events) + (t.events == 1 ? " event" : " events") +
           (t.last_event.empty() ? "" : ", last " + span(now - t.last_event_ms) + " ago: " + t.last_event);
    it.detail = d;
    it.hint = t.id;
    it.id = "bg:" + std::to_string(t.offset);
    items.push_back(std::move(it));
  }
  return items;
}

std::string chip_command(const std::string& key) {
  // Kept for the tests / callers that only ask "is this chip actionable".
  if (key == "model" || key == "effort") return "/model";
  if (key == "mode" || key == "perm" || key == "approval") return "/permissions";
  return {};
}

std::vector<MenuItem> chip_menu(const SessionState& st, const std::string& key, bool live) {
  return chip_menu(st, key, live, "");
}

std::vector<PickItem> chip_pick_items(const SessionState& st, const std::string& key, bool live,
                                      const std::string& agent, int* cursor, std::string* title) {
  std::vector<PickItem> items;
  for (auto& c : chip_choices(st, key, live, agent, cursor, title)) {
    PickItem it;
    it.label = std::move(c.label);
    it.detail = std::move(c.detail);
    it.hint = std::move(c.hint);
    it.id = std::move(c.action);
    it.enabled = c.enabled;
    it.checked = c.checked;
    it.pinned = c.pinned;
    items.push_back(std::move(it));
  }
  return items;
}

void open_chip_picker(App* app, Pane* owner, Point above, const SessionState& st,
                      const std::string& key, bool live, const std::string& agent) {
  int cursor = 0;
  std::string title;
  auto items = chip_pick_items(st, key, live, agent, &cursor, &title);
  app->open_picker_above(owner, above, std::move(title), std::move(items), cursor);
}

std::vector<MenuItem> chip_menu(const SessionState& st, const std::string& key, bool live,
                                const std::string& agent) {
  // The same choices, shaped for a popup at the pointer: the title becomes a
  // heading row, a pinned item goes after a rule.
  int cursor = 0;
  std::string title;
  auto picks = chip_pick_items(st, key, live, agent, &cursor, &title);
  std::vector<MenuItem> items;
  items.push_back(MenuItem{std::move(title), "", false});
  items.push_back(MenuItem::sep());
  for (auto& p : picks) {
    if (p.pinned) items.push_back(MenuItem::sep());
    MenuItem it{std::move(p.label), std::move(p.id), p.enabled};
    it.checked = p.checked;
    it.detail = std::move(p.detail);
    items.push_back(std::move(it));
  }
  return items;
}

}  // namespace mico
