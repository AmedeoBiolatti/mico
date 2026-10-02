#include "adapters/claude/claude.h"

#include <algorithm>
#include <cstdlib>

#include "adapters/screen.h"
#include "base/text.h"

namespace mico {

bool screen_shows_claude_activity(const Vt& vt) {
  if (!claude_activity_line(vt).empty()) return true;
  // While a reply streams Claude drops its spinner line, but the footer under
  // the input box still offers to interrupt.
  const int visible_top = std::max(0, vt.total_rows() - vt.height());
  int bottom = vt.total_rows() - 1;
  while (bottom >= visible_top && row_is_blank(vt.row(bottom))) --bottom;
  // Only in the footer, under the box's lower rule: the conversation can
  // quote the words too.
  bool says = false;
  for (int y = bottom; y >= std::max(visible_top, bottom - 3); --y) {
    const VtRow& r = vt.row(y);
    if (row_is_chrome(r)) return says;
    std::string line;
    for (const Cell& c : r)
      if (c.width && c.cp < 0x80) line.push_back(char(c.cp ? c.cp : ' '));
    says |= line.find("esc to interrupt") != std::string::npos;
  }
  return false;
}

std::string claude_activity_line(const Vt& vt) {
  const int visible_top = std::max(0, vt.total_rows() - vt.height());
  int bottom = vt.total_rows() - 1;
  while (bottom >= visible_top && row_is_blank(vt.row(bottom))) --bottom;
  for (int y = bottom; y >= std::max(visible_top, bottom - 12); --y) {
    const auto& row = vt.row(y);
    if (row_is_committed_bullet(row)) break;
    const char32_t lead = row_lead(row);
    // Claude cycles these star-shaped spinner frames. Its completed
    // "Worked for ..." line can use the same glyph, but has no ellipsis.
    const bool spinner = lead == U'*' || lead == U'·' || lead == U'∗' ||
                         (lead >= 0x2722 && lead <= 0x273D);
    if (!spinner) continue;
    std::string line;
    for (const Cell& c : row)
      if (c.width) text::encode(c.cp ? c.cp : U' ', line);
    if (line.find("…") != std::string::npos || line.find("...") != std::string::npos ||
        line.find("esc to interrupt") != std::string::npos) return line;
  }
  return {};
}

bool parse_permission_prompt(const Vt& vt, PermissionPrompt& out) {
  out = PermissionPrompt{};
  int bottom = vt.total_rows() - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  if (bottom < 0) return false;

  // The footer: "Esc to cancel", possibly with "Tab to amend", on one of the
  // last rows.
  int footer = -1;
  for (int y = bottom; y >= std::max(0, bottom - 2); y--) {
    const std::string t = row_text(vt.row(y));
    if (t.find("Esc to cancel") != std::string::npos) {
      footer = y;
      out.amend = t.find("Tab to amend") != std::string::npos;
      break;
    }
  }
  if (footer < 0) return false;

  std::vector<std::string> options;
  const int y = read_choices(vt, footer, "\xE2\x9D\xAF", options, out.cursor);  // ❯
  if (y < 0) return false;

  // The question: what makes this a permission dialog and not some other
  // numbered menu (the model picker, a resume list).
  const std::string q(trim_left(row_text(vt.row(y))));
  if (!(q.starts_with("Do you want") || q.starts_with("Would you like")) || q.back() != '?')
    return false;
  out.question = q;
  out.options = std::move(options);

  // The heading: rows between the dialog's top rule and the question, down
  // to the first dashed rule (which opens a file's contents or a plan).
  int top = y - 1;
  // The dialog opens under a solid rule; the dashed ones (╌) inside it frame
  // a file's contents.
  while (top >= 0 && y - top < 60 &&
         !(row_lead(vt.row(top)) == U'\u2500' && row_is_chrome(vt.row(top))))
    top--;
  for (int t = top + 1; t < y && out.title.size() < 4; t++) {
    const VtRow& r = vt.row(t);
    if (row_is_blank(r)) continue;
    if (row_lead(r) == U'╌') break;  // ╌
    out.title.emplace_back(trim_left(row_text(r)));
  }
  return true;
}

int trust_prompt_moves(const Vt& vt) {
  int bottom = vt.total_rows() - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  int cursor = -1, yes = -1, choice = 0;
  for (int y = std::max(0, bottom - 18); y <= bottom; y++) {
    const std::string t = row_text(vt.row(y));
    const std::string_view v = trim_left(t);
    const bool focused = v.starts_with("\xE2\x9D\xAF");  // ❯
    // A choice row: the cursor, or the two spaces it leaves when elsewhere.
    const bool is_choice = focused || (t.size() > 3 && t.starts_with(" ") &&
                                       (v.starts_with("Yes") || v.starts_with("No")));
    if (!is_choice) continue;
    if (focused) cursor = choice;
    if (v.find("Yes, I trust") != std::string_view::npos) yes = choice;
    choice++;
  }
  if (cursor < 0 || yes < 0) return kNoTrustMove;
  return yes - cursor;
}

bool screen_is_trust_prompt(const Vt& vt) {
  int bottom = vt.total_rows() - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  std::string text;
  for (int y = std::max(0, bottom - 18); y <= bottom; y++) {
    for (const Cell& c : vt.row(y)) {
      if (c.width == 0) continue;
      text.push_back(c.cp >= 0x20 && c.cp < 0x7f ? char(c.cp) : ' ');
    }
    text.push_back(' ');
  }
  const bool q = text.find("trust this folder") != std::string::npos ||
                 text.find("Quick safety check") != std::string::npos;
  // Its key line is the screen's last: a resumed chat reprints old messages,
  // and one may quote the dialog.
  const std::string last = row_text(vt.row(std::max(0, bottom)));
  const bool confirm = last.find("Enter to confirm") != std::string::npos ||
                       last.find("Esc to cancel") != std::string::npos;
  return q && confirm;
}

bool parse_btw_panel(const Vt& vt, BtwPanel& out) {
  out = BtwPanel{};
  const int total = vt.total_rows();
  const int visible_top = std::max(0, total - vt.height());
  int bottom = total - 1;
  while (bottom >= visible_top && row_is_blank(vt.row(bottom))) bottom--;
  if (bottom < visible_top) return false;
  // The key line closes the panel: indented, and it always offers Esc.
  const std::string hint = row_text(vt.row(bottom));
  if (row_indent(vt.row(bottom)) != 4 || hint.find("Esc to close") == std::string::npos) return false;
  // Up from the key line: the answer (indented six, or blank), then the
  // questions (indented four, "/btw …"). The rule of upper blocks that opens
  // the panel is not always drawn — not in a chat with nothing in it yet —
  // so it is where the scan stops, not what it looks for.
  int answer_from = -1, y = bottom - 1;
  for (; y >= visible_top; y--) {
    const VtRow& r = vt.row(y);
    if (row_is_blank(r)) continue;
    if (row_indent(r) >= 6) {
      answer_from = y;
      continue;
    }
    break;
  }
  std::vector<std::pair<std::string, bool>> asked;  // newest first
  for (; y >= visible_top; y--) {
    const VtRow& r = vt.row(y);
    if (row_is_blank(r)) continue;
    const std::string t = row_text(r);
    const std::string_view body = trim_left(t);
    if (row_indent(r) != 4 || !body.starts_with("/btw ")) break;
    // The question being shown is drawn in bold, the others dim.
    asked.push_back({std::string(body.substr(5)), (r[4].st.a & attr::kBold) != 0});
  }
  for (size_t i = asked.size(); i-- > 0;) {
    out.questions.push_back(std::move(asked[i].first));
    if (asked[i].second) out.current = int(out.questions.size()) - 1;
  }
  if (out.questions.empty()) return false;
  if (out.current < 0) out.current = int(out.questions.size()) - 1;
  out.hint = std::string(trim_left(hint));
  if (answer_from < 0) return true;
  int end = bottom;
  while (end > answer_from && row_is_blank(vt.row(end - 1))) end--;
  // "✽ Answering…" alone, a spinner glyph before it, while it thinks.
  if (end - answer_from == 1) {
    const std::string t = row_text(vt.row(answer_from));
    if (t.find("Answering") != std::string::npos && t.find("\xE2\x80\xA6") != std::string::npos) {
      out.answering = true;
      return true;
    }
  }
  // Claude wraps the answer two columns short of the edge.
  out.answer = rows_markdown(vt, answer_from, end, 6, vt.width() - 2);
  return true;
}

bool ClaudeAdapter::permission_prompt(const Vt& vt, PermissionPrompt& out) const {
  return parse_permission_prompt(vt, out);
}

bool ClaudeAdapter::awaits_input(const Vt& vt) const {
  // The side-question panel waits for nothing: its answer is on screen (and in
  // mico's chat), and the conversation goes on whether or not it is closed.
  if (BtwPanel btw; parse_btw_panel(vt, btw)) return false;
  return screen_awaits_input(vt);
}

bool ClaudeAdapter::side_panel(const Vt& vt, BtwPanel& out) const { return parse_btw_panel(vt, out); }

// Read from Claude's spinner row only; the rest of its screen is conversation
// and may mention compaction for other reasons.
bool ClaudeAdapter::compacting(const Vt& vt) const {
  return claude_activity_line(vt).find("Compacting") != std::string::npos;
}

std::string ClaudeAdapter::screen_reply(const Vt& vt) const {
  return mico::screen_reply(vt, ReplyLayout::Claude);
}

MenuKeys ClaudeAdapter::menu_keys() const {
  MenuKeys k;
  k.starts_at_recommended = false;
  k.multi_buttons = true;
  k.paced = true;
  return k;
}

}  // namespace mico
