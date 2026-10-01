#include "adapters/codex/codex.h"

#include <algorithm>
#include <cstdlib>

#include "adapters/adapters.h"
#include "adapters/screen.h"
#include "base/text.h"

namespace mico {

// The lowest row on screen led by Codex's "›", with what follows it; -1 when
// there is none. That is its input box, or a dialog's focused choice.
static int codex_cursor_row(const Vt& vt, std::string& rest) {
  const int top = std::max(0, vt.total_rows() - vt.height());
  for (int y = vt.total_rows() - 1; y >= top; --y) {
    const std::string t = row_text(vt.row(y));
    const std::string_view v = trim_left(t);
    if (!v.starts_with("\xE2\x80\xBA")) continue;  // ›
    rest = std::string(trim_left(v.substr(3)));
    return y;
  }
  return -1;
}

static bool is_numbered_choice(std::string_view v) {
  size_t i = 0;
  while (i < v.size() && v[i] >= '0' && v[i] <= '9') i++;
  return i > 0 && i < v.size() && v[i] == '.';
}

bool screen_shows_codex_activity(const Vt& vt) {
  std::string rest;
  const int box = codex_cursor_row(vt, rest);
  if (box < 0 || is_numbered_choice(rest)) return false;
  // Queued messages can sit between the status line and the box.
  const int top = std::max(0, vt.total_rows() - vt.height());
  for (int y = box - 1; y >= std::max(top, box - 8); --y) {
    const VtRow& r = vt.row(y);
    if (row_text(r).find("esc to interrupt") != std::string::npos && row_is_status(r)) return true;
  }
  return false;
}

bool screen_awaits_codex_input(const Vt& vt) {
  std::string rest;
  return codex_cursor_row(vt, rest) >= 0 && is_numbered_choice(rest);
}

bool parse_codex_permission_prompt(const Vt& vt, PermissionPrompt& out) {
  out = PermissionPrompt{};
  int bottom = vt.total_rows() - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  if (bottom < 0) return false;
  // "Press enter to confirm or esc to cancel" under an approval; the plan's
  // choice says "enter select · esc back" or "… to confirm or esc to go back".
  int footer = -1;
  for (int y = bottom; y >= std::max(0, bottom - 2); y--) {
    const std::string t = row_text(vt.row(y));
    if (t.find("enter to confirm") != std::string::npos || t.find("enter select") != std::string::npos) {
      footer = y;
      break;
    }
  }
  if (footer < 0) return false;

  std::vector<std::string> options;
  const int y = read_choices(vt, footer, "\xE2\x80\xBA", options, out.cursor);  // ›
  if (y < 0) return false;

  // Codex's question heads the dialog; what is asked about (the reason, the
  // command, a patch) sits between it and the choices. Above it is the
  // conversation, which ends at the prompt you sent, led by "›".
  int q = -1;
  for (int t = y; t >= std::max(0, y - 80); t--) {
    const std::string s(trim_left(row_text(vt.row(t))));
    if (s.starts_with("\xE2\x80\xBA")) break;
    if (s == "Implement this plan?") out.plan = true;
    else if (!((s.starts_with("Would you like") || s.starts_with("Do you want")) && s.ends_with('?'))) continue;
    q = t;
    out.question = s;
    break;
  }
  if (q < 0) return false;

  for (auto& o : options) {
    std::string detail;
    if (out.plan) {
      // The plan's choices are a label and, past a gap, what it does.
      const size_t gap = o.find("  ");
      if (gap != std::string::npos) {
        detail = std::string(trim_left(std::string_view(o).substr(gap)));
        o.resize(gap);
      }
    } else {
      // The key each choice also answers to, "(y)", "(esc)", is not its label.
      const size_t open = o.rfind(" (");
      if (open != std::string::npos && o.back() == ')' && o.size() - open <= 6) o.resize(open);
    }
    static constexpr std::string_view kDisabled = " (disabled)";
    const bool off = o.ends_with(kDisabled);
    if (off) o.resize(o.size() - kDisabled.size());
    out.details.push_back(std::move(detail));
    out.disabled.push_back(off);
  }
  for (int t = q + 1; t <= y && out.title.size() < 4; t++) {
    const VtRow& r = vt.row(t);
    if (row_is_blank(r)) continue;
    std::string s(trim_left(row_text(r)));
    if (s.starts_with("Environment: ")) continue;  // where it runs, not what
    out.title.push_back(std::move(s));
  }
  out.options = std::move(options);
  return true;
}

bool CodexAdapter::permission_prompt(const Vt& vt, PermissionPrompt& out) const {
  return parse_codex_permission_prompt(vt, out);
}

std::string CodexAdapter::screen_reply(const Vt& vt) const {
  return mico::screen_reply(vt, ReplyLayout::Codex);
}

std::string CodexAdapter::async_reply(const std::vector<AsyncReply>& replies) const {
  if (replies.empty() || replies[0].call_id.empty()) return Adapter::async_reply(replies);
  return async_reply_envelope(replies);
}

}  // namespace mico
