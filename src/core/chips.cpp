#include "core/chips.h"

#include <algorithm>
#include <string>
#include <vector>

#include "adapters/adapters.h"
#include "core/models.h"
#include "core/session.h"

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
  // A selection made through mico is already provider-qualified. Match it
  // before consulting the provider last reported by the transcript.
  for (const auto& m : known_models(agent))
    if (m.value == *model) return &m;
  const std::string* provider = st.find("provider");
  for (const auto& m : known_models(agent))
    if ((m.resolved == *model || m.label == *model) &&
        (!provider || provider->empty() || m.value == *provider + "/" + m.resolved)) return &m;
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

std::vector<ChipChoice> chip_choices(const SessionState& st, const std::string& key, bool live,
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

  std::vector<ChipChoice> items;
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
    else if (name == "model_selector") {
      const std::string* m = st.find("model");
      const std::string* provider = st.find("provider");
      if (model) got = model->value;
      else if (m && !m->empty() && m->find('/') != std::string::npos) got = *m;
      else if (m && !m->empty() && provider && !provider->empty()) got = *provider + "/" + *m;
    } else if (name.ends_with("_label")) {
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
      ChipChoice it;
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
        it.action = "chipsteps:" + key + "|" + v + "|" + steps;
      } else {
        it.action = "chipset:" + spec.cmd_prefix + v;
      }
      // The transcript writes the model in its own way — the alias, the
      // shown name, or the concrete id, "claude-" sometimes shed.
      const std::string_view res = opt ? std::string_view(opt->resolved) : std::string_view();
      const std::string_view res_short =
          res.starts_with("claude-") ? res.substr(7) : std::string_view();
      it.checked = value && (*value == v || *value == shown ||
                             (!res.empty() && *value == res) ||
                             (!res_short.empty() && *value == res_short));
      if (opt && model && control.source == ChipControl::Source::Models) it.checked = opt == model;
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
      ChipChoice it;
      it.label = control.ring[size_t(i)].second;
      it.action = "chipmode:" + key + "|" + control.ring[size_t(i)].first + "|" + keys;
      it.enabled = live;
      it.checked = i == cur;
      if (it.checked && cursor) *cursor = int(items.size());
      items.push_back(std::move(it));
    }
  } else if (const std::string& cmd = control.picker; !cmd.empty()) {
    ChipChoice it;
    it.label = "open " + cmd + " in the agent";
    it.action = "chipcmd:" + cmd;
    it.enabled = live;
    items.push_back(std::move(it));
  } else {
    ChipChoice it;
    it.label = "(set this in the agent)";
    it.enabled = false;
    items.push_back(std::move(it));
  }

  if (value && !value->empty()) {
    // Whatever is typed into the picker, copying stays offered.
    ChipChoice it;
    it.label = "copy value";
    it.detail = *value;
    it.action = "chipcopy:" + key;
    it.pinned = true;
    items.push_back(std::move(it));
  }
  return items;
}

ChipApplied apply_chip_choice(LiveSession& s, std::string_view action, std::string* key, std::string* value) {
  key->clear();
  value->clear();
  if (s.answer_sending()) return ChipApplied::Busy;
  // "<key>|<value>|<rest>", as chipsteps and chipmode carry it.
  const auto split = [&](std::string_view rest, std::string& tail) {
    const size_t b1 = rest.find('|');
    const size_t b2 = b1 == std::string_view::npos ? b1 : rest.find('|', b1 + 1);
    if (b2 == std::string_view::npos) return false;
    *key = rest.substr(0, b1);
    *value = rest.substr(b1 + 1, b2 - b1 - 1);
    tail = rest.substr(b2 + 1);
    return true;
  };
  if (action.starts_with("chipset:")) {
    // The agent's own set command, then Enter once its completion has
    // settled: both in one PTY read are taken for pasted text and go to the
    // model as a message.
    const std::string line(action.substr(8));
    if (!s.send_answer({line, "\r"}, true)) return ChipApplied::Busy;
    const size_t sp = line.find(' ');
    if (sp != std::string::npos) {
      const std::string cmd = line.substr(0, sp);
      const char* k = cmd == "/model"         ? "model"
                      : cmd == "/effort"      ? "effort"
                      : cmd == "/permissions" ? "perm"
                                              : nullptr;
      if (k) {
        *key = k;
        *value = line.substr(sp + 1);
      }
    }
    return ChipApplied::Done;
  }
  if (action.starts_with("chipsteps:")) {
    // Steps apart by \x1f, taken one at a time as the screen settles
    // (ChipControl::steps).
    std::string body;
    if (!split(action.substr(10), body)) return ChipApplied::Unsupported;
    std::vector<std::string> keys;
    for (size_t at = 0; at <= body.size();) {
      size_t end = body.find('\x1f', at);
      if (end == std::string::npos) end = body.size();
      keys.push_back(body.substr(at, end - at));
      at = end + 1;
    }
    if (!s.send_answer(std::move(keys), true)) {
      key->clear();
      value->clear();
      return ChipApplied::Busy;
    }
    return ChipApplied::Done;
  }
  if (action.starts_with("chipmode:")) {
    // Walk the agent's ring (claude's permission modes) to a named stop.
    std::string keys;
    if (!split(action.substr(9), keys)) return ChipApplied::Unsupported;
    if (!keys.empty()) s.pty().write(keys);
    return ChipApplied::Done;
  }
  return ChipApplied::Unsupported;
}

}  // namespace mico
