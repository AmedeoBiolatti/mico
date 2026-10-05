#include "adapters/pi/pi.h"

#include <algorithm>

#include "adapters/screen.h"
#include "vt/vt.h"

// Reading what pi and omp draw: the reply they are writing, before their
// transcript has it.
namespace mico {
namespace {

bool default_bg(const VtRow& r) { return r.empty() || r[0].st.bg == kDefaultColor; }

// A rule across the screen, as pi draws its input box: "────", or the upper
// one with its spinner, "── ⠹ Working ──".
bool is_rule(const VtRow& r) { return default_bg(r) && row_indent(r) == 0 && row_lead(r) == U'─'; }

// The first glyph of a row, or null for a blank one.
const Cell* first_glyph(const VtRow& r) {
  for (const Cell& c : r)
    if (c.width && c.cp && c.cp != U' ') return &c;
  return nullptr;
}

// A row of the reply: on the terminal's own background (a user's message
// and a tool's box have one of their own), not the grey italic of thinking,
// not a box's border.
bool reply_row(const VtRow& r) {
  if (!default_bg(r)) return false;
  const Cell* c = first_glyph(r);
  if (!c) return true;  // a blank row between paragraphs
  if ((c->st.a & attr::kItalic) && c->st.fg != kDefaultColor) return false;
  if (row_indent(r) == 0) return false;  // rules, borders, the agent's chrome
  return true;
}

}  // namespace

// Both draw the reply one column in, under whatever came before it in the
// turn; pi's input box is two rules at the bottom, omp's a status bar in a
// background of its own with "Working…" or the session's title above it.
std::string PiFamilyAdapter::screen_reply(const Vt& vt) const {
  const int total = vt.total_rows();
  int bottom = total - 1;
  while (bottom >= 0 && row_is_blank(vt.row(bottom))) bottom--;
  if (bottom < 0) return {};
  const int lowest = std::max(0, bottom - 12);

  int floor = -1;  // the first row below the chat
  if (id() == "omp") {
    for (int y = bottom; y >= lowest && floor < 0; y--)
      if (!default_bg(vt.row(y)) && !row_is_blank(vt.row(y))) floor = y;
    // "⎋ Working…" and the title, right of it, sit just above the bar.
    while (floor > 0 && !row_is_blank(vt.row(floor - 1)) && row_indent(vt.row(floor - 1)) >= 2) floor--;
  } else {
    int rules = 0;
    for (int y = bottom; y >= lowest && floor < 0; y--)
      if (is_rule(vt.row(y)) && ++rules == 2) floor = y;
  }
  if (floor <= 0) return {};

  int end = floor;
  while (end > 0 && row_is_blank(vt.row(end - 1))) end--;
  int start = end;
  while (start > 0 && reply_row(vt.row(start - 1))) start--;
  while (start < end && row_is_blank(vt.row(start))) start++;
  if (start >= end) return {};
  return rows_markdown(vt, start, end, 1, vt.width() - 1);
}

}  // namespace mico
