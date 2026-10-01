#include <string>
#include <vector>

#include "adapters/adapters.h"
#include "core/models.h"
#include "model/state.h"
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

}  // namespace

std::string chip_command(const std::string& key) {
  // Kept for the tests / callers that only ask "is this chip actionable".
  if (key == "model" || key == "effort") return "/model";
  if (key == "mode" || key == "perm") return "/permissions";
  if (key == "approval") return "/approvals";
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
  const ChipSpec spec = spec_for(agent, control);
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
      PickItem it;
      it.label = shown;
      if (opt && !opt->detail.empty()) it.detail = opt->detail;
      if (shown != v) it.hint = v;
      it.id = "chipset:" + spec.cmd_prefix + v;
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
