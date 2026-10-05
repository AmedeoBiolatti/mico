#include "core/answers.h"

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

}  // namespace mico
