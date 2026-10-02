#include "adapters/adapter.h"

#include <algorithm>

#include "adapters/screen.h"

namespace mico {

void Adapter::prepare(Launch& l, const LaunchExtras& x) const {
  if (l.argv.empty()) l.argv = {l.agent};
}

bool Adapter::busy(const Liveness& l) const {
  // A turn produces output continuously; the gap only exceeds this when the
  // agent is sitting at its prompt.
  return l.quiet_ms < 1200;
}

bool Adapter::awaits_input(const Vt& vt) const { return screen_awaits_input(vt); }

ChipControl Adapter::chip_control(std::string_view key) const {
  // The agents' own pickers, by the names most of them use.
  ChipControl c;
  if (key == "model" || key == "effort") c.picker = "/model";
  // Codex's approval policy is set from its /permissions picker: /approvals is
  // gone, and the picker's presets take no argument.
  else if (key == "mode" || key == "perm" || key == "approval") c.picker = "/permissions";
  return c;
}

std::string Adapter::async_reply(const std::vector<AsyncReply>& replies) const {
  std::string msg;
  for (const AsyncReply& r : replies) {
    if (!msg.empty()) msg += "\n\n";
    // "> " before each line of the question.
    for (size_t at = 0; at <= r.question.size();) {
      size_t end = r.question.find('\n', at);
      if (end == std::string::npos) end = r.question.size();
      if (at) msg += '\n';
      msg += "> " + r.question.substr(at, end - at);
      at = end + 1;
    }
    msg += "\n\n" + r.answer;
  }
  return msg;
}

void Adapter::live_rows(const Vt& vt, std::vector<int>& out, int max) const {
  mico::live_rows(vt, out, max);
}

std::vector<std::string> question_answer_steps(
    const Adapter& agent, const std::vector<bool>& multi,
    const std::vector<int>& recommended, const std::vector<int>& options,
    const std::vector<std::vector<uint8_t>>& chosen) {
  const MenuKeys menu = agent.menu_keys();
  const size_t nq = std::min({multi.size(), recommended.size(), options.size(), chosen.size()});
  std::vector<std::string> keys;
  for (size_t qi = 0; qi < nq; qi++) {
    int cur = menu.starts_at_recommended
                  ? std::clamp(recommended[qi], 0, std::max(0, options[qi] - 1))
                  : 0;
    auto move_to = [&](int to) {
      while (cur < to) { keys.emplace_back("\x1b[B"); cur++; }
      while (cur > to) { keys.emplace_back("\x1b[A"); cur--; }
    };
    if (multi[qi]) {
      for (int i = 0; i < options[qi] && i < int(chosen[qi].size()); i++)
        if (chosen[qi][size_t(i)]) { move_to(i); keys.emplace_back(" "); }
      // Enter on an option would toggle it: go to the Next/Submit button,
      // past the Other field.
      if (menu.multi_buttons) move_to(options[qi] + 1);
    } else {
      for (int i = 0; i < options[qi] && i < int(chosen[qi].size()); i++)
        if (chosen[qi][size_t(i)]) { move_to(i); break; }
    }
    keys.emplace_back("\r");  // confirm and advance
  }
  // Several questions end on a review screen, and so does a multi-select with
  // buttons. A single single-select is submitted by its own Enter: a second
  // one would leak into the next prompt.
  if (nq > 1 || (menu.multi_buttons && nq == 1 && multi[0])) keys.emplace_back("\r");
  return keys;
}

std::string question_answer_keys(
    const Adapter& agent, const std::vector<bool>& multi,
    const std::vector<int>& recommended, const std::vector<int>& options,
    const std::vector<std::vector<uint8_t>>& chosen) {
  std::string keys;
  for (const auto& step : question_answer_steps(agent, multi, recommended, options, chosen)) keys += step;
  return keys;
}

}  // namespace mico
