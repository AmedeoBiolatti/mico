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

// Codex's folder trust dialog, as 0.160 draws it in place of the input box:
//   Folder access / Trust this folder? … / › 1. Trust and continue /
//   2. Back to Agent Command Center / enter continue · esc back
// (older releases: "Do you trust the contents of this directory?", "1. Yes,
// continue"). Its key line is the screen's last, so a conversation quoting
// the dialog is not taken for it.
namespace {

constexpr std::string_view kTrustQuestions[] = {"Trust this folder?", "Do you trust the contents of this directory"};
constexpr std::string_view kTrustYes[] = {"Trust and continue", "Yes, continue"};

bool codex_trust_dialog(const Vt& vt) {
  int bottom = vt.total_rows() - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  if (bottom < 0) return false;
  const std::string last = row_text(vt.row(bottom));
  if (last.find("enter continue") == std::string::npos && last.find("Press enter") == std::string::npos)
    return false;
  for (int y = bottom; y >= std::max(0, bottom - 16); y--) {
    const std::string t = row_text(vt.row(y));
    for (std::string_view q : kTrustQuestions)
      if (t.find(q) != std::string::npos) return true;
  }
  return false;
}

// How far the cursor is from the "trust" choice, in rows; kNoMove when either
// is not on screen.
constexpr int kNoMove = -1000;
int codex_trust_moves(const Vt& vt) {
  // The dialog fills the screen from its top: every visible row.
  const int bottom = vt.total_rows() - 1;
  int cursor = -1, yes = -1, choice = 0;
  for (int y = std::max(0, bottom - vt.height() + 1); y <= bottom; y++) {
    const std::string t = row_text(vt.row(y));
    std::string_view v = trim_left(t);
    const bool focused = v.starts_with("\xE2\x80\xBA");  // ›
    if (focused) v = trim_left(v.substr(3));
    if (!is_numbered_choice(v)) continue;
    if (focused) cursor = choice;
    for (std::string_view y_label : kTrustYes)
      if (v.find(y_label) != std::string_view::npos) yes = choice;
    choice++;
  }
  if (cursor < 0 || yes < 0) return kNoMove;
  return yes - cursor;
}

}  // namespace

bool CodexAdapter::startup_prompt(const Vt& vt) const { return codex_trust_dialog(vt); }

// One step at a time, each judged from the screen, as claude's: an arrow
// while the cursor is elsewhere, Enter once it is on the trusting choice.
std::string CodexAdapter::startup_answer(const Vt& vt, bool* confirms) const {
  if (!codex_trust_dialog(vt)) return {};
  const int moves = codex_trust_moves(vt);
  if (moves == kNoMove) return {};
  *confirms = moves == 0;
  return moves > 0 ? "\x1b[B" : moves < 0 ? "\x1b[A" : "\r";
}

std::string CodexAdapter::screen_reply(const Vt& vt) const {
  return mico::screen_reply(vt, ReplyLayout::Codex);
}

std::string CodexAdapter::async_reply(const std::vector<AsyncReply>& replies) const {
  if (replies.empty() || replies[0].call_id.empty()) return Adapter::async_reply(replies);
  return async_reply_envelope(replies);
}

}  // namespace mico
