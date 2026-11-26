#include "core/answers.h"

#include <algorithm>

#include "base/text.h"

namespace mico {

bool permission_takes_note(const PermissionPrompt& p, int i) {
  if (!p.amend) return false;
  if (i == 0) return true;
  return i == int(p.options.size()) - 1 && p.options[size_t(i)].starts_with("No");
}

bool permission_answer(const PermissionPrompt& p, int i, const std::string& note_in, PermissionAnswer& out) {
  if (i < 0 || i >= int(p.options.size())) return false;
  // Codex skips a disabled choice on the way, so it takes no key of its own.
  const auto off = [&](int k) { return k < int(p.disabled.size()) && p.disabled[size_t(k)]; };
  if (off(i)) return false;
  out = PermissionAnswer{};
  for (int cur = p.cursor; cur != i;) {
    cur += cur < i ? 1 : -1;
    if (!off(cur)) out.steps.emplace_back(cur > p.cursor ? "\x1b[B" : "\x1b[A");
  }
  const std::string note = text::oneline(note_in, 2000);
  const bool inline_note = !note.empty() && permission_takes_note(p, i);
  if (inline_note) {
    out.steps.emplace_back("\t");
    out.steps.push_back(note);
  }
  out.steps.emplace_back("\r");
  // A choice with no room for a note ("always allow …") still gets it, as a
  // message right after.
  if (!note.empty() && !inline_note) out.after = "Note on my answer to \"" + p.question + "\": " + note;
  return true;
}

// omp's form dialog (18.x, packages/tui/src/html-form/dialog.ts), as it takes
// keys: the cursor stops on each enabled option of a radio or checkbox group
// (and a multiple <select>), then on the group's "Other" row; once on a
// <select>; last on Submit, and it starts on the first stop. ↓ moves one stop.
// Enter on a radio selects it and goes to the next field; on a checkbox it
// only goes on, Space toggling one; ←/→ change a <select>. Enter on Submit
// sends the form.
std::vector<std::string> form_answer_steps(const QuestionCard& card, const std::vector<std::vector<uint8_t>>& chosen) {
  if (!card.form || card.terminal) return {};
  static const std::string kDown = "\x1b[B", kRight = "\x1b[C", kLeft = "\x1b[D";
  // Shift+Tab: on the first stop it moves nothing, and it wakes a form drawn
  // over several pages, which can take its first key for that alone.
  std::vector<std::string> keys{"\x1b[Z"};
  for (size_t qi = 0; qi < card.questions.size(); qi++) {
    const QuestionSpec& q = card.questions[qi];
    const int n = int(q.options.size());
    const auto want = [&](int i) {
      return qi < chosen.size() && size_t(i) < chosen[qi].size() && chosen[qi][size_t(i)];
    };
    const auto was = [&](int i) { return size_t(i) < q.checked.size() && q.checked[size_t(i)]; };
    int pick = -1;
    for (int i = 0; i < n && pick < 0; i++)
      if (want(i)) pick = i;
    if (q.control == "radio") {
      for (int i = 0; i < std::max(pick, 0); i++) keys.push_back(kDown);
    } else if (q.control == "select") {
      int from = 0;
      for (int i = 0; i < n; i++)
        if (was(i)) from = i;
      for (int i = from; i < pick; i++) keys.push_back(kRight);
      for (int i = from; i > pick && pick >= 0; i--) keys.push_back(kLeft);
    } else {  // checkbox, multiselect: toggle what differs from the start
      int at = 0;
      for (int i = 0; i < n; i++) {
        if (want(i) == was(i)) continue;
        for (; at < i; at++) keys.push_back(kDown);
        keys.emplace_back(" ");
      }
    }
    keys.emplace_back("\r");  // the next field, or Submit
  }
  keys.emplace_back("\r");  // Submit
  return keys;
}

}  // namespace mico
