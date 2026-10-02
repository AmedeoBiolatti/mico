#include "adapters/screen.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "base/text.h"

namespace mico {

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
int row_indent(const VtRow& r) {
  int n = 0;
  for (const Cell& c : r) {
    if (c.width == 0) continue;
    if (c.cp == U' ' || c.cp == 0) { n++; continue; }
    return n;
  }
  return -1;  // blank
}

// The first visible glyph of a row, or 0 for a blank row.
char32_t row_lead(const VtRow& r) {
  for (const Cell& c : r) {
    if (c.width == 0 || c.cp == U' ' || c.cp == 0) continue;
    return c.cp;
  }
  return 0;
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
std::string row_text(const VtRow& r) {
  std::string out;
  for (const Cell& c : r) {
    if (c.width == 0) continue;
    text::encode(c.cp ? c.cp : U' ', out);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string_view trim_left(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return s;
}

// Upwards from a dialog's footer: its choices, each "N. label" (the focused
// one led by `cursor`) and the deeper-indented rows a long label wraps onto.
// Going up, a choice's wrapped rows come before the choice. Returns the first
// row above the choices, or -1 when they are not one numbered menu of two or
// more; `focus` is the choice the cursor is on.
int read_choices(const Vt& vt, int footer, std::string_view cursor, std::vector<std::string>& options,
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

// The generic scan: from the bottom of the screen, keep the flush-left lines
// that read like work in progress, dropping the agent's chrome row by row.
void live_rows(const Vt& vt, std::vector<int>& out, int max) {
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

ReplyRow reply_row(const VtRow& r, int from, int to) {
  ReplyRow out;
  int last = -1;
  const int n = to < 0 ? int(r.size()) : std::min(to, int(r.size()));
  for (int x = 0; x < n; x++)
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
std::string rows_markdown(const Vt& vt, int start, int end, int from, int wrap_at, int to) {
  std::vector<std::string> lines;
  int prev_width = 0;
  bool in_code = false;
  for (int y = start; y < end; y++) {
    ReplyRow row = reply_row(vt.row(y), from, to);
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

std::string screen_reply(const Vt& vt, ReplyLayout layout) {
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
  const bool claude = layout == ReplyLayout::Claude;
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

  // A panel beside the conversation (Claude's diff of a file) shares the
  // reply's rows: only what is left of it is the reply, wrapped at its edge.
  const int edge = side_panel_edge(vt, start, input, 2);
  return rows_markdown(vt, start, end, 2, edge < 0 ? vt.width() : edge, edge);
}

int side_panel_edge(const Vt& vt, int start, int end, int from) {
  const auto rule = [](char32_t c) { return c == U'\u2502' || c == U'\u2503' || c == U'\u2551'; };
  const auto box = [](char32_t c) { return c >= 0x2500 && c <= 0x257F; };
  const int w = vt.width();
  std::vector<int> rows(size_t(std::max(0, w)), 0), beside_text(size_t(std::max(0, w)), 0);
  for (int y = std::max(0, start); y < end; y++) {
    const VtRow& r = vt.row(y);
    char32_t first = 0;  // the row's first glyph left of the column
    for (int x = from; x < std::min(w, int(r.size())); x++) {
      const Cell& c = r[size_t(x)];
      if (!c.width) continue;
      if (rule(c.cp) && !box(first)) {
        rows[size_t(x)]++;
        if (first) beside_text[size_t(x)]++;
      }
      if (!first && c.cp && c.cp != U' ') first = c.cp;
    }
  }
  // A panel runs down beside all of it (the rule above the input box aside);
  // a diagram's rule in a code block runs down a few of its rows.
  const int need = std::max(2, (end - std::max(0, start) - 1) * 3 / 4);
  for (int x = from + 1; x < w; x++)
    if (rows[size_t(x)] >= need && beside_text[size_t(x)] >= 1) return x;

  // No rule: a panel in a background of its own. Its colour is the one most
  // rows end in; its edge, the column where most rows change into it, padding
  // first, so a diff line's own background inside it does not move the edge.
  // It sits right of the conversation, which keeps a third of the width at
  // least, so an edit's diff in the conversation, coloured out to the edge
  // from just after its line numbers, is never taken for one.
  std::vector<std::pair<Color, int>> ends;
  for (int y = std::max(0, start); y < end; y++) {
    const VtRow& r = vt.row(y);
    if (int(r.size()) < w || w < 1) continue;
    const Color bg = r[size_t(w - 1)].st.bg;
    if (bg == kDefaultColor) continue;
    auto it = std::find_if(ends.begin(), ends.end(), [&](const auto& e) { return e.first == bg; });
    if (it == ends.end()) ends.emplace_back(bg, 1);
    else it->second++;
  }
  if (ends.empty()) return -1;
  const Color panel =
      std::max_element(ends.begin(), ends.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
  std::fill(rows.begin(), rows.end(), 0);
  std::fill(beside_text.begin(), beside_text.end(), 0);
  const int lo = std::max(from + 1, w / 3);
  for (int y = std::max(0, start); y < end; y++) {
    const VtRow& r = vt.row(y);
    if (int(r.size()) < w) continue;
    bool text = false;
    for (int x = from; x < w; x++) {
      const Cell& c = r[size_t(x)];
      if (x >= lo && c.st.bg == panel && r[size_t(x - 1)].st.bg != panel) {
        rows[size_t(x)]++;
        if (text) beside_text[size_t(x)]++;
      }
      text |= c.width && c.cp && c.cp != U' ' && c.st.bg != panel;
    }
  }
  // Half the rows will do: the column, the side and the colour are the test,
  // and the rows under the reply need not all have the panel beside them.
  const int half = std::max(2, (end - std::max(0, start)) / 2);
  int best = -1;
  for (int x = lo; x < w; x++)
    if (rows[size_t(x)] >= half && beside_text[size_t(x)] >= 1 && (best < 0 || rows[size_t(x)] > rows[size_t(best)]))
      best = x;
  return best;
}

void Vt::blank(VtRow& r) const { r.assign(size_t(w_), Cell{U' ', Style{}, 1}); }

}  // namespace mico
