#include <string>
#include <vector>

#include "core/models.h"
#include "model/state.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

// Values mico offers directly for a chip, and the line it sends the agent to
// select one. Empty list -> fall back to opening the agent's own picker.
struct ChipSpec {
  std::vector<std::string> values;
  std::string cmd_prefix;  // e.g. "/model " ; the chosen value is appended
};

ChipSpec spec_for(const std::string& agent, const std::string& key) {
  if (agent == "claude") {
    // The model list is whatever the probe read out of claude's own picker —
    // never a list written down here, which would go stale the day a model
    // ships. Empty until a probe lands, and then the menu offers claude's
    // picker instead.
    if (key == "model") {
      ChipSpec spec;
      spec.cmd_prefix = "/model ";
      for (const auto& m : known_models(agent)) spec.values.push_back(m.value);
      return spec;
    }
    if (key == "effort") {
      // The levels claude's own initialize answer reported, when it has.
      ChipSpec spec;
      spec.cmd_prefix = "/effort ";
      spec.values = known_efforts(agent);
      if (spec.values.empty()) spec.values = {"low", "medium", "high"};
      return spec;
    }
  } else if (agent == "codex") {
    if (key == "effort") return {{"low", "medium", "high"}, "/model "};
    if (key == "approval")
      return {{"untrusted", "on-request", "on-failure", "never"}, "/approvals "};
  }
  return {};
}

// The bare picker command, for chips mico does not enumerate itself.
std::string picker_cmd(const std::string& agent, const std::string& key) {
  if (agent == "pi" || agent == "omp") {
    // Both open a fuzzy-search picker rather than taking a value inline, so
    // mico cannot offer a one-shot set the way it does for claude/codex.
    if (key == "model") return "/model";
    if (key == "effort") return "/thinking";
    return {};
  }
  if (key == "model" || key == "effort") return "/model";
  if (key == "mode" || key == "perm") return "/permissions";
  if (key == "approval") return "/approvals";
  return {};
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
  const ChipSpec spec = spec_for(agent, key);
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
  } else if (agent == "claude" && (key == "mode" || key == "perm")) {
    // Claude has no one-shot command for the permission mode; Shift+Tab walks a
    // fixed ring. We know the current state, so each entry sends exactly enough
    // presses to land on it — and "plan" is reachable directly.
    static const char* kRing[] = {"default", "acceptEdits", "plan"};
    static const char* kNice[] = {"default", "accept edits", "plan mode"};
    int cur = 0;
    if (value)
      for (int i = 0; i < 3; i++)
        if (*value == kRing[i]) cur = i;
    for (int i = 0; i < 3; i++) {
      std::string keys;
      for (int step = (i - cur + 3) % 3; step > 0; step--) keys += "\x1b[Z";
      PickItem it;
      it.label = kNice[i];
      it.id = "chipmode:" + key + "|" + kRing[i] + "|" + keys;
      it.enabled = live;
      it.checked = i == cur;
      if (it.checked && cursor) *cursor = int(items.size());
      items.push_back(std::move(it));
    }
  } else if (const std::string cmd = picker_cmd(agent, key); !cmd.empty()) {
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
