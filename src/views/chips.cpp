#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include "adapters/adapters.h"
#include "core/models.h"
#include "core/session.h"
#include "model/state.h"
#include "base/progress.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

// Values mico offers directly for a chip, and the line it sends the agent to
// select one. Empty list -> fall back to the ring or the agent's own picker.
struct ChipSpec {
  std::vector<std::string> values;
  std::string cmd_prefix;  // e.g. "/model " ; the chosen value is appended
};

const Adapter& adapter_of(const std::string& agent) {
  const Adapter* a = adapter_for(agent);
  return a ? *a : plain_adapter();
}

ChipSpec spec_for(const std::string& agent, const ChipControl& c) {
  ChipSpec spec;
  spec.cmd_prefix = c.set_prefix;
  switch (c.source) {
    case ChipControl::Source::Fixed: spec.values = c.values; break;
    case ChipControl::Source::Models:
      for (const auto& m : known_models(agent)) spec.values.push_back(m.value);
      break;
    case ChipControl::Source::Efforts:
      spec.values = known_efforts(agent);
      if (spec.values.empty()) spec.values = c.values;
      break;
  }
  return spec;
}

// The known model the state names, by its value, its concrete id or its
// name: each agent writes the model its own way.
const ModelOption* current_model(const std::string& agent, const SessionState& st) {
  const std::string* model = st.find("model");
  if (!model || model->empty()) return nullptr;
  for (const auto& m : known_models(agent))
    if (m.value == *model || m.resolved == *model || m.label == *model) return &m;
  return nullptr;
}

// `tmpl` with each "{name}" filled by `lookup`; false when one names nothing.
template <class F>
bool fill(std::string_view tmpl, F&& lookup, std::string& out) {
  out.clear();
  for (size_t i = 0; i < tmpl.size();) {
    const size_t open = tmpl.find('{', i);
    if (open == std::string_view::npos) {
      out += tmpl.substr(i);
      break;
    }
    const size_t close = tmpl.find('}', open);
    if (close == std::string_view::npos) {
      out += tmpl.substr(i);
      break;
    }
    out += tmpl.substr(i, open - i);
    std::string got;
    if (!lookup(tmpl.substr(open + 1, close - open - 1), got) || got.empty()) return false;
    out += got;
    i = close + 1;
  }
  return true;
}

}  // namespace

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
  const std::string* value = st.find(key);
  std::string label = key;
  for (const auto& f : st.fields)
    if (f.key == key) label = f.label;
  if (title) {
    *title = label;
    if (value && !value->empty()) *title += " \xC2\xB7 " + *value;
  }
  if (cursor) *cursor = 0;

  std::vector<PickItem> items;
  const ChipControl control = adapter_of(agent).chip_control(key);
  ChipSpec spec = spec_for(agent, control);
  const ModelOption* model = current_model(agent, st);
  // The levels the model in use takes, where the agent says per model.
  if (control.source == ChipControl::Source::Efforts && model && !model->efforts.empty())
    spec.values = model->efforts;
  const auto label_of = [&](const std::string& v) {
    for (const auto& [val, name] : control.labels)
      if (val == v) return name;
    return v;
  };
  // "{value}" and "{label}" are the choice; any other name a state field,
  // "_label" after it for the agent's name for its value.
  std::string chosen, chosen_label;
  const auto lookup = [&](std::string_view name, std::string& got) {
    if (name == "value") got = chosen;
    else if (name == "label") got = chosen_label;
    else if (name == "model_label") got = model ? model->label : st.find("model") ? *st.find("model") : "";
    else if (name.ends_with("_label")) {
      const std::string* f = st.find(name.substr(0, name.size() - 6));
      got = f ? label_of(*f) : "";
    } else {
      const std::string* f = st.find(name);
      got = f ? *f : "";
    }
    return !got.empty();
  };
  // Steps needing a field the state lacks cannot be sent: the agent's own
  // picker is offered instead.
  for (const auto& s : control.steps)
    if (std::string probe; !s.starts_with(kPickStep) &&
                           !fill(s, [&](std::string_view n, std::string& g) {
                             return n == "value" || n == "label" ? (g = "x", true) : lookup(n, g);
                           }, probe))
      spec.values.clear();
  if (!spec.values.empty() && live) {
    for (const auto& v : spec.values) {
      // Show the readable form; the action still carries the exact value. A
      // probed model brings the agent's own name and description ("Fable 5.1
      // — For your toughest challenges"); the value stays as the dim hint.
      const ModelOption* opt = nullptr;
      for (const auto& m : known_models(agent))
        if (m.value == v) { opt = &m; break; }
      std::string shown = opt && !opt->label.empty() ? opt->label : v;
      if (shown == v)
        for (std::string_view pfx : {"claude-", "anthropic/", "openai/"})
          if (shown.size() > pfx.size() && shown.compare(0, pfx.size(), pfx) == 0)
            shown = shown.substr(pfx.size());
      if (!opt && shown == v) shown = label_of(v);
      PickItem it;
      it.label = shown;
      if (opt && !opt->detail.empty()) it.detail = opt->detail;
      if (shown != v) it.hint = v;
      chosen = v;
      chosen_label = shown;
      if (!control.steps.empty()) {
        // Keys through the agent's own menus, a step a field cannot fill
        // taken as Enter: the menu's own default.
        std::string steps, step;
        for (const auto& s : control.steps) {
          if (!fill(s, lookup, step)) step = "\r";
          steps += (steps.empty() ? "" : "\x1f") + step;
        }
        it.id = "chipsteps:" + key + "|" + v + "|" + steps;
      } else {
        it.id = "chipset:" + spec.cmd_prefix + v;
      }
      // The transcript writes the model in its own way — the alias, the
      // shown name, or the concrete id, "claude-" sometimes shed.
      const std::string_view res = opt ? std::string_view(opt->resolved) : std::string_view();
      const std::string_view res_short =
          res.starts_with("claude-") ? res.substr(7) : std::string_view();
      it.checked = value && (*value == v || *value == shown ||
                             (!res.empty() && *value == res) ||
                             (!res_short.empty() && *value == res_short));
      if (it.checked && cursor) *cursor = int(items.size());
      items.push_back(std::move(it));
    }
  } else if (!control.ring.empty()) {
    // A ring walked with one key: knowing the current state, each entry sends
    // exactly enough presses to land on it.
    const int n = int(control.ring.size());
    int cur = 0;
    if (value)
      for (int i = 0; i < n; i++)
        if (*value == control.ring[size_t(i)].first) cur = i;
    for (int i = 0; i < n; i++) {
      std::string keys;
      for (int step = (i - cur + n) % n; step > 0; step--) keys += control.ring_key;
      PickItem it;
      it.label = control.ring[size_t(i)].second;
      it.id = "chipmode:" + key + "|" + control.ring[size_t(i)].first + "|" + keys;
      it.enabled = live;
      it.checked = i == cur;
      if (it.checked && cursor) *cursor = int(items.size());
      items.push_back(std::move(it));
    }
  } else if (const std::string& cmd = control.picker; !cmd.empty()) {
    PickItem it;
    it.label = "open " + cmd + " in the agent";
    it.id = "chipcmd:" + cmd;
    it.enabled = live;
    items.push_back(std::move(it));
  } else {
    PickItem it;
    it.label = "(set this in the agent)";
    it.enabled = false;
    items.push_back(std::move(it));
  }

  if (value && !value->empty()) {
    // Whatever is typed into the picker, copying stays offered.
    PickItem it;
    it.label = "copy value";
    it.detail = *value;
    it.id = "chipcopy:" + key;
    it.pinned = true;
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
