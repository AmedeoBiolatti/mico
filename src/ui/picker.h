#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "vt/surface.h"
#include "term/term.h"
#include "ui/theme.h"

namespace mico {

// One choice in a Picker.
struct PickItem {
  std::string label;
  std::string detail;  // dim text after the label, or under it (Options::detail_rows)
  std::string hint;    // dim, right-aligned: a key, a count, a time
  std::string id;      // the caller's key for this item; the picker never reads it
  std::string group;   // a heading row is drawn where the group changes
  std::string lead;    // a glyph before the label ("●", "▲"), in lead_color
  Color lead_color = kDefaultColor;
  bool enabled = true;
  bool separator = false;
  bool checked = false;  // multi-select state; a ✓ in a single-select menu
  bool pinned = false;   // shown whatever the query, after the matches: "Use “…”"
};

// Case-insensitive fuzzy match of `query` against `text`: the query as a run,
// or its characters in order, each continuing the run before it or starting a
// word: "tc" matches "tool calls", and "co" does not match "doctor".
// Returns -1 for no match, else a score where higher is better — a run at a
// word start, then a run anywhere, then word starts; earlier beats later. `marks` gets the byte offsets in `text`
// of the matched characters, for highlighting.
int fuzzy_match(std::string_view text, std::string_view query, std::vector<uint32_t>* marks = nullptr);

// A list to choose from: a popup menu, a command palette, a panel of choices
// over the prompt box, a question card. One implementation of the cursor,
// multi-selection, filtering, scrolling, the mouse and the drawing, so every
// list-shaped choice in mico behaves the same way and a new one is a list of
// items plus what to do with the answer.
//
// The picker owns no screen position: the caller hands render() a painter and
// passes on_mouse() coordinates local to it, the same as a Pane.
class Picker {
 public:
  enum class Frame {
    Popup,  // a bordered box floating over everything; title in the border
    Panel,  // a borderless strip on the footer surface; title and subtitle rows
  };
  struct Options {
    Frame frame = Frame::Popup;
    std::string title;
    std::string title_lead;              // a glyph before a Panel's title
    Color title_color = kDefaultColor;   // default: the theme's accent
    std::string subtitle;                // a Panel's second heading row
    bool multi = false;     // Space (Tab while filtering) toggles; Enter confirms
    bool filter = false;    // typed characters narrow the list
    bool ranked = true;     // a query sorts by match quality; false keeps item order
    bool match_detail = true;  // a query found in the detail keeps an item too
    bool tab_completes = false;  // Tab reports Completed instead of moving
    bool back_on_empty = false;  // Backspace with no query reports Back
    bool show_query = false;  // the query row even while it is empty
    std::string query_placeholder = "type to filter";
    bool numbers = false;   // 1-9 pick an item directly, and are drawn
    bool wrap = true;       // moving past either end comes round the other
    bool start_unselected = false;  // no cursor until a key or the mouse puts one
    int label_rows = 1;     // rows a long label may wrap onto
    int detail_rows = 0;    // 0: the detail follows the label on its row
    bool footer = true;
    std::string footer_text;  // replaces the generated key hint
    std::string note;         // "✎ note" under the items
    std::string empty_text = "no matches";
    Color bg = kDefaultColor;  // default: menu_bg for a popup, strip_bg for a panel
  };

  // What a key or a click did.
  enum class Result {
    Ignored,       // not the picker's; the caller may use it
    Handled,       // consumed with nothing to report (a stray key in a modal)
    Moved,         // the cursor moved
    QueryChanged,
    Toggled,       // multi: an item's checked flag flipped (index())
    Chosen,        // single: an item was picked (index())
    Confirmed,     // multi: Enter; checked() has the answer
    Cancelled,     // Escape with an empty query
    Completed,     // Tab, where it completes: take index() into the query
    Back,          // Backspace on an empty query, where a flow has a step before
  };

  Picker() = default;
  explicit Picker(Options o) : opt_(std::move(o)) {}

  Options& options() { return opt_; }
  const Options& options() const { return opt_; }

  // Replaces the items. The query stays; the cursor stays on the same
  // position when it still exists, so a list refreshed every frame (a dialog
  // read off the screen) does not jump.
  void set_items(std::vector<PickItem> items);
  const std::vector<PickItem>& items() const { return items_; }
  PickItem& item(int i) { return items_[size_t(i)]; }

  // The query. The picker edits it itself when Options::filter is on; a
  // caller that owns a text field (the prompt box after a "/") sets it.
  void set_query(std::string q);
  const std::string& query() const { return query_; }

  // The item under the cursor, as an index into items(); -1 for none.
  int cursor() const;
  void set_cursor(int item);
  // Back to the first choice shown: for a caller that replaced the items
  // because the query changed, where the old position means nothing.
  void cursor_to_first() {
    cur_ = -1;
    move(1);
  }
  // The item the last Chosen or Toggled result was about.
  int index() const { return index_; }
  // Checked items, in item order.
  std::vector<int> checked() const;
  // Items that pass the filter, in the order shown (headings excluded).
  int visible_count() const;

  Result on_key(const KeyEvent& k);
  Result on_paste(std::string_view s);
  // `local` is relative to the painter last passed to render().
  Result on_mouse(const MouseEvent& m, Point local);

  // Rows this picker wants at `width`, all of its items shown, capped at
  // `max_rows`. For a popup this includes the border.
  int rows(int width, int max_rows) const;
  // Columns a popup wants: its widest row, and its title.
  int natural_width() const;

  void render(Painter p, const Theme& th);

 private:
  struct Entry {
    int item;
    bool heading;  // the row naming item's group
  };
  struct Geometry {
    int mark_x = -1, check_x = -1, num_x = -1, label_x = 0, right = 0;
  };

  void refilter(bool query_changed);
  bool selectable(size_t e) const;
  int entry_of(int item) const;
  int entry_rows(size_t e, const Geometry& g) const;
  Geometry geometry(int width) const;
  int head_rows() const;  // title, subtitle and query rows inside the frame
  int tail_rows() const;  // note and footer rows inside the frame
  Result move(int delta);
  Result step_to(int e);
  Result choose(size_t e);
  Result toggle(size_t e);
  std::string footer_hint() const;
  void draw_label(Painter& p, int x, int y, std::string_view s, size_t base,
                  const std::vector<uint32_t>& marks, Style st, Style hi, int max_w) const;

  Options opt_;
  std::vector<PickItem> items_;
  std::string query_;
  std::vector<Entry> view_;
  std::vector<std::vector<uint32_t>> marks_;  // per item, while a query is set
  int cur_ = -1;       // an index into view_
  int top_ = 0;        // the first entry drawn
  bool follow_ = true; // keep the cursor in view; the wheel turns it off
  int page_ = 1;       // entries the last frame showed, for PageUp/PageDown
  int index_ = -1;
  std::vector<int> hits_;  // per painter row: the entry drawn there, or -1
  int query_row_ = -1;
};

}  // namespace mico
