#include "vt/vt.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "base/text.h"

namespace mico {
namespace {

constexpr size_t kMaxScrollback = 10000;

// xterm's 256-colour cube, so SGR 38;5;n renders the same colour the agent meant.
Color xterm256(int n) {
  static constexpr Color kBase[16] = {
      0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
      0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
  if (n < 0) return kDefaultColor;
  if (n < 16) return kBase[n];
  if (n < 232) {
    int i = n - 16;
    static constexpr int kSteps[6] = {0, 95, 135, 175, 215, 255};
    return (kSteps[i / 36] << 16) | (kSteps[(i / 6) % 6] << 8) | kSteps[i % 6];
  }
  int g = 8 + (n - 232) * 10;
  return (g << 16) | (g << 8) | g;
}

}  // namespace

bool row_is_blank(const VtRow& r) {
  for (const Cell& c : r)
    if (c.width != 0 && c.cp != U' ' && c.cp != 0) return false;
  return true;
}

bool row_is_chrome(const VtRow& r) {
  bool any = false;
  for (const Cell& c : r) {
    if (c.width == 0 || c.cp == U' ' || c.cp == 0) continue;
    any = true;
    const bool box = c.cp >= 0x2500 && c.cp <= 0x257F;
    const bool block = c.cp >= 0x2580 && c.cp <= 0x259F;
    if (!box && !block) return false;
  }
  return any;
}

bool row_starts_furniture(const VtRow& r) {
  for (const Cell& c : r) {
    if (c.width == 0 || c.cp == U' ' || c.cp == 0) continue;
    if (c.cp >= 0x2500 && c.cp <= 0x257F) return true;  // a box border
    if (c.cp >= 0x2580 && c.cp <= 0x259F) return true;  // block shading
    if (c.cp == 0x276F || c.cp == 0x203A || c.cp == '>') return true;  // a prompt
    if (c.cp == 0x23F5 || c.cp == 0x25B6 || c.cp == 0x25BA) return true;  // play/status
    // Working-spinner frames and the bullets both agents lead their status
    // lines with. In the strip zone a line starting with any of these is the
    // agent's own chrome, not conversation.
    if (c.cp == '*' || c.cp == 0x00B7 || c.cp == 0x2022 || c.cp == 0x2219) return true;
    if ((c.cp >= 0x2722 && c.cp <= 0x273D) || c.cp == 0x2217) return true;
    if (c.cp == 0x26A0 || c.cp == 0x26A1 || c.cp == 0x2139) return true;  // warn / info
    return false;
  }
  return false;
}

// A status or spinner line, judged by content: "Working (5s · esc to
// interrupt)", "1 startup issue · ctrl + t for details", and the like. These
// are the agent talking about itself, and they change every second.
bool row_is_status(const VtRow& r) {
  std::string t;
  for (const Cell& c : r) {
    if (c.width == 0) continue;
    if (c.cp < 0x80) t.push_back(char(c.cp));
    else t.push_back('.');
    if (t.size() > 200) break;
  }
  auto has = [&](const char* n) { return t.find(n) != std::string::npos; };
  if (has("esc to interrupt") || has("ctrl + t") || has("ctrl+t")) return true;
  if (has("startup issue") || has("startup issues")) return true;
  if (has(" tokens)") || has(" tokens ")) return true;
  // A trailing "(12s)" spinner clock with nothing else substantial on the line.
  size_t op = t.rfind('(');
  if (op != std::string::npos && op + 2 < t.size()) {
    size_t i = op + 1;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') i++;
    if (i > op + 1 && i < t.size() && (t[i] == 's' || t[i] == 'm')) return true;
  }
  return false;
}

// A row whose first glyph is a completed-message bullet. Everything from here
// up the screen is already in the transcript; only what is below it is work
// still in flight.
bool row_is_committed_bullet(const VtRow& r) {
  for (const Cell& c : r) {
    if (c.width == 0 || c.cp == U' ' || c.cp == 0) continue;
    return c.cp == 0x25CF || c.cp == 0x23FA;  // ● or ⏺
  }
  return false;
}

// Leading spaces before the first visible glyph.
static int row_indent(const VtRow& r) {
  int n = 0;
  for (const Cell& c : r) {
    if (c.width == 0) continue;
    if (c.cp == U' ' || c.cp == 0) { n++; continue; }
    return n;
  }
  return -1;  // blank
}

// The first visible glyph of a row, or 0 for a blank row.
static char32_t row_lead(const VtRow& r) {
  for (const Cell& c : r) {
    if (c.width == 0 || c.cp == U' ' || c.cp == 0) continue;
    return c.cp;
  }
  return 0;
}

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

// A line that could plausibly be the agent narrating an action it is taking:
// it starts with a word, a path, a shell token, or a tool-output marker.
// Anything led by a symbol, a star, a bullet or a box glyph is the agent's own
// chrome. Whitelisting the shape is robust against spinner frames changing
// between releases, which blacklisting glyph by glyph is not.
static bool looks_like_activity(const VtRow& r) {
  const char32_t c = row_lead(r);
  if (c == 0) return false;
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  if (c == '$' || c == '/' || c == '.' || c == '_' || c == '~' || c == '\'') return true;
  if (c == 0x23BF || c == 0x2514 || c == 0x2570) return true;  // tool-output corner
  return false;
}

bool screen_awaits_input(const Vt& vt) {
  // Claude's side-question panel waits for nothing: its answer is on screen
  // (and in mico's chat), and the conversation goes on whether or not it is
  // closed.
  if (BtwPanel btw; parse_btw_panel(vt, btw)) return false;
  int bottom = vt.total_rows() - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  if (bottom < 0) return false;

  std::string text;
  const int top = std::max(0, bottom - 18);
  for (int y = top; y <= bottom; y++) {
    // Claude's own input box is a ❯ row directly under a rule. That is its
    // prompt, not a menu: counting it made every idle Claude read as waiting
    // for an answer, so it showed "needs you" and a queued message never went.
    const bool input_box = y > 0 && row_is_chrome(vt.row(y - 1));
    // And its dialogs take the input box's place: with the box on screen,
    // "Do you want to" above it is Claude's reply asking, not a dialog.
    if (input_box && row_lead(vt.row(y)) == 0x276F) return false;
    for (const Cell& c : vt.row(y)) {
      if (c.width == 0) continue;
      text.push_back(c.cp >= 0x20 && c.cp < 0x7f ? char(c.cp) : ' ');
      // A selection cursor (❯ / ▶) directly before an option is a live menu.
      if (!input_box && (c.cp == 0x276F || c.cp == 0x25B6 || c.cp == 0x25BA)) text += "<CURSOR>";
    }
    text.push_back('\n');
  }

  static const char* kMarks[] = {
      "<CURSOR>",           "trust this folder", "Quick safety check",
      "Do you want to",     "Enter to confirm",  "(y/n)",
      "[Y/n]",              "[y/N]",             "Yes, proceed",
  };
  for (const char* m : kMarks)
    if (text.find(m) != std::string::npos) return true;
  return false;
}

// A row as UTF-8 with the trailing blanks dropped.
static std::string row_text(const VtRow& r) {
  std::string out;
  for (const Cell& c : r) {
    if (c.width == 0) continue;
    text::encode(c.cp ? c.cp : U' ', out);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

static std::string_view trim_left(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return s;
}

// Upwards from a dialog's footer: its choices, each "N. label" (the focused
// one led by `cursor`) and the deeper-indented rows a long label wraps onto.
// Going up, a choice's wrapped rows come before the choice. Returns the first
// row above the choices, or -1 when they are not one numbered menu of two or
// more; `focus` is the choice the cursor is on.
static int read_choices(const Vt& vt, int footer, std::string_view cursor, std::vector<std::string>& options,
                        int& focus) {
  options.clear();
  focus = 0;
  std::vector<int> numbers;
  std::string wrapped;
  int y = footer - 1;
  for (; y >= std::max(0, footer - 30); y--) {
    const VtRow& r = vt.row(y);
    if (row_is_blank(r)) continue;
    const std::string t = row_text(r);
    std::string_view v = trim_left(t);
    bool focused = false;
    if (v.starts_with(cursor)) {
      focused = true;
      v = trim_left(v.substr(cursor.size()));
    }
    size_t digits = 0;
    while (digits < v.size() && v[digits] >= '0' && v[digits] <= '9') digits++;
    if (digits > 0 && digits + 1 < v.size() && v[digits] == '.' && v[digits + 1] == ' ') {
      std::string label(trim_left(v.substr(digits + 2)));
      // A path or a hyphenated word wraps at its '/' or '-', not at a space.
      if (!wrapped.empty())
        label += (label.back() == '-' || label.back() == '/' ? "" : " ") + wrapped;
      wrapped.clear();
      numbers.push_back(std::atoi(std::string(v.substr(0, digits)).c_str()));
      options.push_back(std::move(label));
      if (focused) focus = -int(options.size());  // fixed up below
      continue;
    }
    if (options.empty() || row_indent(r) < 4) break;  // the question
    if (wrapped.empty()) wrapped = std::string(v);
    else wrapped = std::string(v) + (v.back() == '-' || v.back() == '/' ? "" : " ") + wrapped;
  }
  if (y < 0 || options.size() < 2) return -1;
  std::reverse(options.begin(), options.end());
  std::reverse(numbers.begin(), numbers.end());
  for (size_t i = 0; i < numbers.size(); i++)
    if (numbers[i] != int(i) + 1) return -1;  // not one numbered menu
  focus = focus < 0 ? int(options.size()) + focus : 0;
  return y;
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
  const bool confirm = text.find("Enter to confirm") != std::string::npos ||
                       text.find("Yes, I trust") != std::string::npos;
  return q && confirm;
}

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

// The generic scan: from the bottom of the screen, keep the flush-left lines
// that read like work in progress, dropping the agent's chrome row by row.
static void live_rows_generic(const Vt& vt, std::vector<int>& out, int max) {
  out.clear();
  if (max <= 0) return;
  if (max > 2) max = 2;  // a hint at what the agent is doing, not a transcript

  int bottom = vt.total_rows() - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  if (bottom < 0) return;

  const int scan_stop = std::max(0, bottom - 24);
  bool collected = false;

  for (int y = bottom; y >= scan_stop && int(out.size()) < max; y--) {
    const VtRow& r = vt.row(y);
    // At or above a completed-message bullet is transcript territory.
    if (row_is_committed_bullet(r)) break;
    if (row_is_blank(r)) {
      // A blank only ends the scan once real activity has been picked up: the
      // gap between the in-progress area and the settled history. Below the
      // first activity line it is just spacing inside the chrome block.
      if (collected) break;
      continue;
    }
    // Indented rows are wrapped continuations or nested detail the transcript
    // keeps; symbol-led rows are the agent's own chrome. Neither ends the scan
    // and neither is shown — only a flush-left line that reads like an action.
    if (row_indent(r) >= 2 || !looks_like_activity(r)) continue;
    out.push_back(y);
    collected = true;
  }
  std::reverse(out.begin(), out.end());
}

void live_rows(const Vt& vt, std::vector<int>& out, int max, std::string_view agent) {
  // pi and omp render a full chat of their own into the terminal. Splicing its
  // tail shows that rendering verbatim — a streaming reply, thinking, tool
  // output — beside mico's own rendering of the same turn, which is exactly
  // the duplication the chat view exists to avoid. Neither leads a completed
  // message with a bullet and both park their footer below the editor, so no
  // row is reliably "work in flight". Their chat view is transcript-only; the
  // pane title and state chips already say the agent is working.
  if (agent == "pi" || agent == "omp") { out.clear(); return; }
  live_rows_generic(vt, out, max);
}

namespace {

bool is_prompt_glyph(char32_t c) { return c == 0x276F || c == 0x203A; }  // ❯ ›

// A bullet in the colour of the text: how both agents lead a reply. A tool
// call's bullet has a colour of its own (grey while it runs, then green or
// red), and Codex's "Working" line is bold.
bool is_text_bullet(const Cell& c) {
  if (!(c.cp == 0x25CF || c.cp == 0x23FA || c.cp == 0x2022)) return false;  // ● ⏺ •
  if (c.st.a & attr::kBold) return false;
  if (c.st.fg == kDefaultColor) return true;
  const int r = (c.st.fg >> 16) & 0xFF, g = (c.st.fg >> 8) & 0xFF, b = c.st.fg & 0xFF;
  return std::min({r, g, b}) >= 0xE0 || std::max({r, g, b}) <= 0x20;  // white or black
}

// A row of a reply, from column `from`: the text with bold and italic marked
// as markdown, and the plain text, whose columns decide whether the agent
// wrapped the row.
struct ReplyRow {
  std::string marked, plain;
  int width = 0;  // columns up to the last glyph, from the row's start
  // Every glyph bold: a heading. Every glyph coloured and none bold: a line
  // of highlighted code, as Codex draws a code block.
  bool all_bold = false, all_colored = false;
};

ReplyRow reply_row(const VtRow& r, int from) {
  ReplyRow out;
  int last = -1;
  for (int x = 0; x < int(r.size()); x++)
    if (r[size_t(x)].width && r[size_t(x)].cp != U' ' && r[size_t(x)].cp) last = x;
  out.width = last + 1;
  // A heading the agent shows with its hashes is markdown already.
  bool heading = false;
  for (int x = from; x <= last; x++) {
    const char32_t c = r[size_t(x)].cp;
    if (c == U' ') continue;
    heading = c == U'#';
    break;
  }
  uint16_t on = 0;  // the emphasis the marked text is inside
  std::string pending_space;
  // Bold wins over italic: "***" is more than the chat's markdown reads.
  auto marker = [](uint16_t a) { return a & attr::kBold ? "**" : a & attr::kItalic ? "*" : ""; };
  bool any = false, bold = true, colored = true;
  for (int x = from; x <= last; x++) {
    const Cell& c = r[size_t(x)];
    if (c.width == 0) continue;
    std::string glyph;
    text::encode(c.cp ? c.cp : U' ', glyph);
    out.plain += glyph;
    if (glyph == " ") {
      // Spaces sit outside the markers: "**a b**", never "**a **b".
      pending_space += glyph;
      continue;
    }
    any = true;
    bold &= (c.st.a & attr::kBold) != 0;
    colored &= c.st.fg != kDefaultColor && !(c.st.a & attr::kBold);
    uint16_t want = heading ? 0 : uint16_t(c.st.a & (attr::kBold | attr::kItalic));
    if (want & attr::kBold) want = attr::kBold;
    if (want != on) {
      out.marked += marker(on);
      out.marked += pending_space;
      out.marked += marker(want);
      on = want;
    } else {
      out.marked += pending_space;
    }
    pending_space.clear();
    out.marked += glyph;
  }
  out.marked += marker(on);
  out.all_bold = any && bold && !heading;
  out.all_colored = any && colored;
  return out;
}

// Where a row's text starts a block of its own rather than going on with the
// line above: a list item, a heading, a table or a fence.
bool starts_block(std::string_view t) {
  while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
  if (t.empty()) return true;
  for (std::string_view m : {"- ", "* ", "+ ", "\xE2\x80\xA2 ", "\xE2\x97\xA6 ", "\xE2\x96\xAA "})
    if (t.starts_with(m)) return true;
  if (t[0] == '#' || t[0] == '|' || t[0] == '>' || t.starts_with("```")) return true;
  if (uint8_t(t[0]) == 0xE2 && t.size() > 2 && uint8_t(t[1]) == 0x94) return true;  // a box table
  size_t i = 0;
  while (i < t.size() && t[i] >= '0' && t[i] <= '9') i++;
  return i > 0 && i + 1 < t.size() && (t[i] == '.' || t[i] == ')') && t[i + 1] == ' ';
}

int first_word_width(std::string_view t) {
  while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
  const size_t sp = t.find(' ');
  return text::str_width(t.substr(0, sp == std::string_view::npos ? t.size() : sp));
}

}  // namespace

// Rows [start, end) of an agent's rendered markdown, as markdown again: the
// lines it wrapped joined, bold and italic marked, highlighted code fenced.
// Text starts at column `from`; a row wraps onto the next when that row's
// first word would have run past column `wrap_at`.
std::string rows_markdown(const Vt& vt, int start, int end, int from, int wrap_at) {
  std::vector<std::string> lines;
  int prev_width = 0;
  bool in_code = false;
  for (int y = start; y < end; y++) {
    ReplyRow row = reply_row(vt.row(y), from);
    // Highlighted code goes in a fence, as it was written: no markup read
    // into it, and no line of it joined to another.
    // A blank line inside the code does not end it.
    const bool code = row.plain.empty() ? in_code : row.all_colored;
    if (code != in_code) {
      if (in_code) {
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        lines.emplace_back("```");
      } else {
        lines.emplace_back("```");
      }
      in_code = code;
    }
    if (in_code) {
      lines.push_back(std::move(row.plain));
      prev_width = 0;
      continue;
    }
    if (row.plain.empty()) {
      lines.emplace_back();
      prev_width = 0;
      continue;
    }
    // The agent wrapped here if the row's first word did not fit on the one
    // above it.
    const bool wrapped = !lines.empty() && !lines.back().empty() && !starts_block(row.plain) &&
                         prev_width + 1 + first_word_width(row.plain) > wrap_at;
    if (wrapped) {
      std::string_view rest = row.marked;
      while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
      std::string& line = lines.back();
      // A word broken at its hyphen joins again without a space.
      const bool hyphen = line.size() > 1 && line.back() == '-' &&
                          std::isalnum(uint8_t(line[line.size() - 2]));
      if (!hyphen) line += ' ';
      line += rest;
    } else if (row.all_bold) {
      std::string_view title = row.plain;
      while (!title.empty() && title.front() == ' ') title.remove_prefix(1);
      lines.push_back("## " + std::string(title));
    } else {
      std::string line = std::move(row.marked);
      // Codex draws list items with its own bullets; markdown's is a dash.
      const size_t at = line.find_first_not_of(' ');
      for (std::string_view b : {"\xE2\x80\xA2 ", "\xE2\x97\xA6 ", "\xE2\x96\xAA "})
        if (at != std::string::npos && std::string_view(line).substr(at).starts_with(b))
          line.replace(at, b.size(), "- ");
      lines.push_back(std::move(line));
    }
    prev_width = row.width;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  if (in_code) lines.emplace_back("```");
  std::string out;
  for (const std::string& l : lines) {
    out += l;
    out += '\n';
  }
  if (!out.empty()) out.pop_back();
  return out;
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

// Codex draws its tool cells ("• Explored", "• Ran cargo test") with the same
// dim bullet as a reply. What gives them away is the bold title after it, or
// the "└" its details hang from on the row below.
static bool is_codex_tool_cell(const Vt& vt, int y) {
  const VtRow& r = vt.row(y);
  std::string title;
  for (size_t x = 2; x < r.size() && (r[x].st.a & attr::kBold); x++)
    if (r[x].width) text::encode(r[x].cp ? r[x].cp : U' ', title);
  while (!title.empty() && title.back() == ' ') title.pop_back();
  static constexpr std::string_view kTitles[] = {
      "Explored", "Exploring", "Ran", "Running", "Edited", "Added", "Deleted", "Called", "Calling",
      "Waited", "Waiting", "Interacted", "Searched", "Searching", "Viewed image", "Proposed Plan",
      "Updated Plan", "Questions"};
  for (std::string_view t : kTitles)
    if (title == t) return true;
  for (int k = y + 1; k < std::min(vt.total_rows(), y + 3); k++) {
    const VtRow& d = vt.row(k);
    if (row_is_blank(d)) continue;
    return row_indent(d) == 2 && row_lead(d) == U'\u2514';  // └
  }
  return false;
}

std::string screen_reply(const Vt& vt, std::string_view agent) {
  if (agent != "claude" && agent != "codex") return {};
  const int total = vt.total_rows();
  int bottom = total - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  auto lead = [&](int y) -> const Cell* {
    const VtRow& r = vt.row(y);
    return r.empty() || !r[0].width ? nullptr : &r[0];
  };
  // The agent's input line: the lowest prompt glyph in the first column.
  // Claude's sits under the rule its box opens with, which tells it from the
  // user's own message when a dialog has taken the box's place. Codex's is at
  // the bottom, under its reply or over the options of an approval.
  const bool claude = agent == "claude";
  int input = -1;
  for (int y = bottom; y >= std::max(0, bottom - (claude ? 40 : 10)) && input < 0; y--) {
    const Cell* c = lead(y);
    if (!c || !is_prompt_glyph(c->cp)) continue;
    if (claude && !(y > 0 && row_is_chrome(vt.row(y - 1)))) continue;
    input = y;
  }
  if (input < 0) return {};
  // Up from there to the last reply block, unless the user's own message
  // comes first: then nothing has been written since.
  int start = -1;
  for (int y = input - 1; y >= std::max(0, input - 3000) && start < 0; y--) {
    const Cell* c = lead(y);
    if (!c) continue;
    if (is_prompt_glyph(c->cp)) return {};
    if (is_text_bullet(*c) && (claude || !is_codex_tool_cell(vt, y))) start = y;
  }
  if (start < 0) return {};
  // The block's rows are indented under the bullet; anything in the first
  // column (a tool call, a spinner, the input box) ends it. So does a blank
  // there in a colour: Claude blinks a running tool's bullet, and while it is
  // off the cell is a space still coloured like the bullet, not a
  // continuation row, whose first cell has no colour of its own.
  int end = start + 1;
  while (end < input) {
    const Cell* c = lead(end);
    if (c && ((c->cp != U' ' && c->cp) ||
              (c->st.fg != kDefaultColor && c->st.bg == kDefaultColor)))
      break;
    // A tool's output ("⎿ …") ends it too, with the tool's own lines above:
    // back to the blank row that separates the two.
    if (row_indent(vt.row(end)) == 2 && row_lead(vt.row(end)) == 0x23BF) {
      while (end > start + 1 && !row_is_blank(vt.row(end - 1))) end--;
      break;
    }
    end++;
  }

  return rows_markdown(vt, start, end, 2, vt.width());
}

void Vt::blank(VtRow& r) const { r.assign(size_t(w_), Cell{U' ', Style{}, 1}); }

void Vt::resize(int w, int h) {
  w = std::max(1, w);
  h = std::max(1, h);
  if (w == w_ && h == h_ && !screen_.empty()) return;
  w_ = w;
  h_ = h;
  screen_.resize(size_t(h_));
  alt_buf_.resize(size_t(h_));
  for (auto& r : screen_) r.resize(size_t(w_), Cell{U' ', Style{}, 1});
  for (auto& r : alt_buf_) r.resize(size_t(w_), Cell{U' ', Style{}, 1});
  top_ = 0;
  bot_ = h_ - 1;
  cx_ = std::min(cx_, w_ - 1);
  cy_ = std::min(cy_, h_ - 1);
}

const VtRow& Vt::row(int i) const {
  static const VtRow kEmpty;
  int sb = int(scrollback_.size());
  if (i < 0) return kEmpty;
  if (i < sb) return scrollback_[size_t(i)];
  int y = i - sb;
  const auto& buf = alt_ ? alt_buf_ : screen_;
  if (y >= int(buf.size())) return kEmpty;
  return buf[size_t(y)];
}

VtRow& Vt::line(int y) {
  auto& buf = alt_ ? alt_buf_ : screen_;
  y = std::clamp(y, 0, int(buf.size()) - 1);
  return buf[size_t(y)];
}

void Vt::set_cursor(int x, int y) {
  cx_ = std::clamp(x, 0, w_ - 1);
  cy_ = std::clamp(y, 0, h_ - 1);
  wrap_pending_ = false;
}

void Vt::scroll_up(int n) {
  auto& buf = alt_ ? alt_buf_ : screen_;
  // Only lines leaving the top of a full-height region are real history;
  // a scrolling region is the app repainting, not the conversation moving.
  const bool archive = !alt_ && top_ == 0 && bot_ == h_ - 1;
  for (int i = 0; i < n; i++) {
    // The row about to leave the top: either it carries its allocation down to
    // become the new blank bottom row, or it is archived and the buffer we
    // recycle comes from the scrollback line that just aged out. Steady-state
    // scrolling then costs no allocation at all.
    VtRow fresh;
    if (archive) {
      scrollback_.push_back(std::move(buf[size_t(top_)]));
      if (scrollback_.size() > kMaxScrollback) {
        fresh = std::move(scrollback_.front());
        scrollback_.pop_front();
      }
    } else {
      fresh = std::move(buf[size_t(top_)]);
    }
    for (int y = top_; y < bot_; y++) buf[size_t(y)] = std::move(buf[size_t(y) + 1]);
    blank(fresh);
    buf[size_t(bot_)] = std::move(fresh);
  }
}

void Vt::scroll_down(int n) {
  auto& buf = alt_ ? alt_buf_ : screen_;
  for (int i = 0; i < n; i++) {
    VtRow fresh = std::move(buf[size_t(bot_)]);  // reuse the row falling off the bottom
    for (int y = bot_; y > top_; y--) buf[size_t(y)] = std::move(buf[size_t(y) - 1]);
    blank(fresh);
    buf[size_t(top_)] = std::move(fresh);
  }
}

void Vt::index() {
  if (cy_ == bot_) scroll_up(1);
  else if (cy_ < h_ - 1) cy_++;
  wrap_pending_ = false;
}

void Vt::reverse_index() {
  if (cy_ == top_) scroll_down(1);
  else if (cy_ > 0) cy_--;
  wrap_pending_ = false;
}

void Vt::newline() {
  index();
}

void Vt::put(char32_t cp, int w) {
  if (w == 0) return;
  if (wrap_pending_ && autowrap_) {
    carriage_return();
    index();
  }
  if (cx_ + w > w_) {
    if (!autowrap_) { cx_ = w_ - w; }
    else { carriage_return(); index(); }
  }
  VtRow& r = line(cy_);
  if (size_t(cx_) >= r.size()) return;

  // Overwriting half of a wide glyph must clear its partner.
  if (r[size_t(cx_)].width == 0 && cx_ > 0) r[size_t(cx_ - 1)] = Cell{U' ', cur_, 1};
  if (r[size_t(cx_)].width == 2 && cx_ + 1 < w_) r[size_t(cx_ + 1)] = Cell{U' ', cur_, 1};

  r[size_t(cx_)] = Cell{cp, cur_, uint8_t(w)};
  if (w == 2 && cx_ + 1 < w_) r[size_t(cx_ + 1)] = Cell{U' ', cur_, 0};

  cx_ += w;
  if (cx_ >= w_) { cx_ = w_ - 1; wrap_pending_ = true; }
}

// Writes a run of single-width ASCII without going through put() per byte.
// Agent output is overwhelmingly plain text, and the per-character path spends
// most of its time re-deriving state that cannot have changed mid-run.
void Vt::put_ascii_run(std::string_view s) {
  size_t k = 0;
  while (k < s.size()) {
    if (wrap_pending_ && autowrap_) {
      carriage_return();
      index();
    }
    if (cx_ >= w_) {
      if (!autowrap_) return;
      carriage_return();
      index();
    }

    VtRow& r = line(cy_);
    const size_t room = size_t(w_ - cx_);
    const size_t n = std::min(room, s.size() - k);

    // Landing on the tail of a double-width glyph must blank its head.
    if (r[size_t(cx_)].width == 0 && cx_ > 0) r[size_t(cx_ - 1)] = Cell{U' ', cur_, 1};

    for (size_t t = 0; t < n; t++) {
      Cell& dst = r[size_t(cx_) + t];
      // Only the rare wide-glyph case needs the slow fixup.
      if (dst.width == 2 && size_t(cx_) + t + 1 < r.size())
        r[size_t(cx_) + t + 1] = Cell{U' ', cur_, 1};
      dst = Cell{char32_t(uint8_t(s[k + t])), cur_, 1};
    }

    cx_ += int(n);
    k += n;
    if (cx_ >= w_) {
      cx_ = w_ - 1;
      wrap_pending_ = true;
    }
  }
}

void Vt::erase_in_line(int mode) {
  VtRow& r = line(cy_);
  int from = mode == 0 ? cx_ : 0;
  int to = mode == 1 ? cx_ + 1 : w_;
  if (mode == 2) { from = 0; to = w_; }
  for (int x = from; x < to && x < int(r.size()); x++) r[size_t(x)] = Cell{U' ', cur_, 1};
}

void Vt::erase_in_display(int mode) {
  if (mode == 3) { scrollback_.clear(); return; }
  int from = mode == 0 ? cy_ + 1 : 0;
  int to = mode == 1 ? cy_ : h_;
  if (mode == 2) { from = 0; to = h_; }
  for (int y = from; y < to; y++) blank(line(y));
  if (mode == 0) erase_in_line(0);
  else if (mode == 1) erase_in_line(1);
}

void Vt::insert_lines(int n) {
  if (cy_ < top_ || cy_ > bot_) return;
  auto& buf = alt_ ? alt_buf_ : screen_;
  for (int i = 0; i < n; i++) {
    buf.erase(buf.begin() + bot_);
    VtRow fresh;
    blank(fresh);
    buf.insert(buf.begin() + cy_, std::move(fresh));
  }
}

void Vt::delete_lines(int n) {
  if (cy_ < top_ || cy_ > bot_) return;
  auto& buf = alt_ ? alt_buf_ : screen_;
  for (int i = 0; i < n; i++) {
    buf.erase(buf.begin() + cy_);
    VtRow fresh;
    blank(fresh);
    buf.insert(buf.begin() + bot_, std::move(fresh));
  }
}

void Vt::insert_chars(int n) {
  VtRow& r = line(cy_);
  for (int i = 0; i < n; i++) {
    r.pop_back();
    r.insert(r.begin() + cx_, Cell{U' ', cur_, 1});
  }
}

void Vt::delete_chars(int n) {
  VtRow& r = line(cy_);
  for (int i = 0; i < n; i++) {
    if (size_t(cx_) < r.size()) r.erase(r.begin() + cx_);
    r.push_back(Cell{U' ', cur_, 1});
  }
}

void Vt::erase_chars(int n) {
  VtRow& r = line(cy_);
  for (int i = 0; i < n && cx_ + i < int(r.size()); i++) r[size_t(cx_ + i)] = Cell{U' ', cur_, 1};
}

int Vt::param(size_t i, int fallback) const {
  return i < nums_.size() && nums_[i] >= 0 ? nums_[i] : fallback;
}

void Vt::exec_sgr() {
  if (nums_.empty()) { cur_ = Style{}; return; }
  for (size_t i = 0; i < nums_.size(); i++) {
    int n = nums_[i] < 0 ? 0 : nums_[i];
    switch (n) {
      case 0: cur_ = Style{}; break;
      case 1: cur_.a |= attr::kBold; break;
      case 2: cur_.a |= attr::kDim; break;
      case 3: cur_.a |= attr::kItalic; break;
      case 4: cur_.a |= attr::kUnderline; break;
      case 7: cur_.a |= attr::kReverse; break;
      case 22: cur_.a &= ~(attr::kBold | attr::kDim); break;
      case 23: cur_.a &= ~attr::kItalic; break;
      case 24: cur_.a &= ~attr::kUnderline; break;
      case 27: cur_.a &= ~attr::kReverse; break;
      case 39: cur_.fg = kDefaultColor; break;
      case 49: cur_.bg = kDefaultColor; break;
      case 38:
      case 48: {
        bool fg = n == 38;
        int kind = param(i + 1, 0);
        if (kind == 2) {
          Color c = (param(i + 2, 0) << 16) | (param(i + 3, 0) << 8) | param(i + 4, 0);
          (fg ? cur_.fg : cur_.bg) = c;
          i += 4;
        } else if (kind == 5) {
          (fg ? cur_.fg : cur_.bg) = xterm256(param(i + 2, 0));
          i += 2;
        }
        break;
      }
      default:
        if (n >= 30 && n <= 37) cur_.fg = xterm256(n - 30);
        else if (n >= 40 && n <= 47) cur_.bg = xterm256(n - 40);
        else if (n >= 90 && n <= 97) cur_.fg = xterm256(n - 90 + 8);
        else if (n >= 100 && n <= 107) cur_.bg = xterm256(n - 100 + 8);
        break;
    }
  }
}

void Vt::set_mode(bool on) {
  bool priv = !intermediates_.empty() && intermediates_[0] == '?';
  for (size_t i = 0; i < nums_.size(); i++) {
    int n = nums_[i];
    if (!priv) continue;
    switch (n) {
      case 1: app_cursor_ = on; break;
      case 7: autowrap_ = on; break;
      case 25: cursor_visible_ = on; break;
      case 1000: case 1002: case 1003: mouse_mode_ = on ? n : 0; break;
      case 1006: sgr_mouse_ = on; break;
      case 2004: bracketed_paste_ = on; break;
      case 1047: case 1049: case 47:
        if (on != alt_) {
          alt_ = on;
          if (on) {
            for (auto& r : alt_buf_) blank(r);
            saved_cx_ = cx_;
            saved_cy_ = cy_;
            set_cursor(0, 0);
          } else {
            set_cursor(saved_cx_, saved_cy_);
          }
        }
        break;
      default: break;  // 2026 sync, 1004 focus, 2031 theme: nothing to model
    }
  }
}

void Vt::exec_csi(char final) {
  // A private marker (?, >, <, =) makes a different command of the same final
  // byte: "CSI ? u" asks for the kitty keyboard flags, "CSI > 0 q" for the
  // terminal's name. Run as plain CSI u / CSI s they restored or saved the
  // cursor, and claude asks exactly that after drawing its first frame: the
  // cursor jumped to the top-left and every relative redraw after it (a menu
  // moving its ❯) landed on the wrong rows. Only modes (h/l) and selective
  // erase (? J, ? K) keep their meaning.
  if (!intermediates_.empty() && final != 'h' && final != 'l' && final != 'J' && final != 'K')
    return;
  switch (final) {
    case 'A': set_cursor(cx_, cy_ - std::max(1, param(0, 1))); break;
    case 'B': set_cursor(cx_, cy_ + std::max(1, param(0, 1))); break;
    case 'C': set_cursor(cx_ + std::max(1, param(0, 1)), cy_); break;
    case 'D': set_cursor(cx_ - std::max(1, param(0, 1)), cy_); break;
    case 'E': set_cursor(0, cy_ + std::max(1, param(0, 1))); break;
    case 'F': set_cursor(0, cy_ - std::max(1, param(0, 1))); break;
    case 'G': case '`': set_cursor(param(0, 1) - 1, cy_); break;
    case 'd': set_cursor(cx_, param(0, 1) - 1); break;
    case 'H': case 'f': set_cursor(param(1, 1) - 1, param(0, 1) - 1); break;
    case 'J': erase_in_display(param(0, 0)); break;
    case 'K': erase_in_line(param(0, 0)); break;
    case 'L': insert_lines(std::max(1, param(0, 1))); break;
    case 'M': delete_lines(std::max(1, param(0, 1))); break;
    case 'P': delete_chars(std::max(1, param(0, 1))); break;
    case '@': insert_chars(std::max(1, param(0, 1))); break;
    case 'X': erase_chars(std::max(1, param(0, 1))); break;
    case 'S': scroll_up(std::max(1, param(0, 1))); break;
    case 'T': scroll_down(std::max(1, param(0, 1))); break;
    case 'm': exec_sgr(); break;
    case 'h': set_mode(true); break;
    case 'l': set_mode(false); break;
    case 'r':
      top_ = std::clamp(param(0, 1) - 1, 0, h_ - 1);
      bot_ = std::clamp(param(1, h_) - 1, top_, h_ - 1);
      set_cursor(0, top_);
      break;
    case 's': saved_cx_ = cx_; saved_cy_ = cy_; break;
    case 'u': set_cursor(saved_cx_, saved_cy_); break;
    default: break;
  }
}

void Vt::exec_esc(char b) {
  switch (b) {
    case '7': saved_cx_ = cx_; saved_cy_ = cy_; saved_style_ = cur_; break;
    case '8': set_cursor(saved_cx_, saved_cy_); cur_ = saved_style_; break;
    case 'D': index(); break;
    case 'M': reverse_index(); break;
    case 'E': carriage_return(); index(); break;
    case 'c':
      cur_ = Style{};
      for (auto& r : screen_) blank(r);
      set_cursor(0, 0);
      break;
    default: break;
  }
}

void Vt::write(std::string_view bytes) {
  if (screen_.empty()) resize(w_, h_);

  for (size_t i = 0; i < bytes.size(); i++) {
    unsigned char b = (unsigned char)bytes[i];

    switch (state_) {
      case State::Ground: {
        if (b == 0x1b) { state_ = State::Esc; params_.clear(); intermediates_.clear(); nums_.clear(); break; }
        if (b == '\n' || b == 0x0b || b == 0x0c) { newline(); break; }
        if (b == '\r') { carriage_return(); break; }
        if (b == '\t') { set_cursor((cx_ / 8 + 1) * 8, cy_); break; }
        if (b == 0x08) { set_cursor(cx_ - 1, cy_); break; }
        if (b == 0x07 || b == 0x0e || b == 0x0f) break;
        if (b < 0x20) break;

        // Fast path: consume the whole run of printable ASCII at once.
        if (utf8_.empty() && b < 0x7F) {
          size_t j = i;
          while (j < bytes.size()) {
            const unsigned char x = (unsigned char)bytes[j];
            if (x < 0x20 || x >= 0x7F) break;
            j++;
          }
          put_ascii_run(bytes.substr(i, j - i));
          i = j - 1;  // the for-loop increment lands on the terminator
          break;
        }

        // Multi-byte characters can straddle a read boundary.
        utf8_.push_back(char(b));
        size_t need = 1;
        unsigned char c0 = (unsigned char)utf8_[0];
        if ((c0 & 0xE0) == 0xC0) need = 2;
        else if ((c0 & 0xF0) == 0xE0) need = 3;
        else if ((c0 & 0xF8) == 0xF0) need = 4;
        if (utf8_.size() < need) break;
        size_t k = 0;
        char32_t cp = text::decode(utf8_, k);
        utf8_.clear();
        put(cp, std::max(1, text::cp_width(cp)));
        break;
      }

      case State::Esc: {
        if (b == '[') { state_ = State::Csi; break; }
        if (b == ']' || b == 'P' || b == '^' || b == '_') { state_ = b == ']' ? State::Osc : State::StringIgnore; params_.clear(); break; }
        // Charset designators take one more byte. Consuming it by advancing the
        // index breaks the moment the pair straddles a read.
        if (b == '(' || b == ')' || b == '*' || b == '+' || b == '%' || b == '#') {
          state_ = State::EscFinal;
          break;
        }
        exec_esc(char(b));
        state_ = State::Ground;
        break;
      }

      case State::Csi: {
        if ((b >= '0' && b <= '9') || b == ';' || b == ':') { params_.push_back(char(b)); break; }
        if (b == '?' || b == '>' || b == '<' || b == '!' || b == '=') { intermediates_.push_back(char(b)); break; }
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back(char(b)); break; }
        if (b >= 0x40 && b <= 0x7E) {
          nums_.clear();
          int acc = -1;
          for (char c : params_) {
            if (c >= '0' && c <= '9') acc = (acc < 0 ? 0 : acc) * 10 + (c - '0');
            else { nums_.push_back(acc); acc = -1; }
          }
          nums_.push_back(acc);
          exec_csi(char(b));
          state_ = State::Ground;
          break;
        }
        state_ = State::Ground;
        break;
      }

      case State::EscFinal:
        state_ = State::Ground;
        break;

      case State::Osc:
        // OSC 8 hyperlinks and title sets carry no cell content; skip to ST.
        if (b == 0x07) { state_ = State::Ground; break; }
        if (b == 0x1b) state_ = State::OscEsc;
        break;

      case State::OscEsc:
        // ESC inside a string terminates it only when followed by a backslash;
        // anything else means the string was abandoned mid-way.
        state_ = (b == '\\') ? State::Ground : State::Osc;
        if (b != '\\' && b == 0x1b) state_ = State::OscEsc;
        break;

      case State::StringIgnore:
        if (b == 0x07) { state_ = State::Ground; break; }
        if (b == 0x1b) state_ = State::StringEsc;
        break;

      case State::StringEsc:
        state_ = (b == '\\') ? State::Ground : State::StringIgnore;
        if (b != '\\' && b == 0x1b) state_ = State::StringEsc;
        break;
    }
  }
}

}  // namespace mico
