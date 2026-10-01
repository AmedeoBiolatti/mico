#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/message.h"
#include "term/text.h"

namespace mico {

// The text state behind the chat prompt box: a flat UTF-8 string, a cursor
// byte offset (always on a codepoint boundary), and an optional selection.
// Multi-line by nature — Ctrl+J inserts a real newline — but otherwise a very
// ordinary single-field text editor. Kept separate from SessionPane so the
// cursor/selection arithmetic (the part most likely to have an off-by-one)
// can be exercised without a live pty.
class PromptEditor {
 public:
  const std::string& text() const { return text_; }
  size_t cursor() const { return cursor_; }
  bool empty() const { return text_.empty(); }

  bool has_selection() const { return sel_anchor_ != std::string::npos; }
  size_t sel_lo() const { return std::min(cursor_, sel_anchor_); }
  size_t sel_hi() const { return std::max(cursor_, sel_anchor_); }

  // Empties the box as an edit: Ctrl+Z brings the text back.
  void clear() {
    if (text_.empty()) return;
    checkpoint(Edit::Other);
    text_.clear();
    cursor_ = 0;
    drop_selection();
  }
  // Empties the box and forgets its history: the text went somewhere (sent,
  // queued, saved as a note), so undoing into it again would duplicate it.
  void reset() {
    text_.clear();
    cursor_ = 0;
    drop_selection();
    attachments_.clear();
    undo_.clear();
    redo_.clear();
    last_ = Edit::None;
  }

  // Replaces bytes [from, to) with `with` as one undoable edit and puts the
  // cursor after it: a completion taking the place of the word typed.
  void replace(size_t from, size_t to, std::string_view with) {
    from = std::min(from, text_.size());
    to = std::clamp(to, from, text_.size());
    checkpoint(Edit::Other);
    drop_selection();
    text_.replace(from, to - from, with);
    cursor_ = from + with.size();
  }

  // --- selection and history -----------------------------------------------
  void select_all() {
    last_ = Edit::None;
    sel_anchor_ = 0;
    cursor_ = text_.size();
    if (text_.empty()) drop_selection();
  }
  // The selection as the message would send it: pastes and images as their
  // contents, not their labels.
  std::string selected_text() const {
    if (!has_selection()) return {};
    return expand(sel_lo(), sel_hi());
  }
  // Removes the selection and returns it, for Ctrl+X.
  std::string cut_selection() {
    std::string out = selected_text();
    if (!out.empty() || has_selection()) {
      checkpoint(Edit::Other);
      erase_selection();
    }
    return out;
  }
  // Ctrl+Z / Ctrl+Y. Typing and deleting are undone a word at a time, not a
  // keystroke at a time; anything else is a step of its own.
  bool undo() { return step(undo_, redo_); }
  bool redo() { return step(redo_, undo_); }
  bool can_undo() const { return !undo_.empty(); }

  // --- attachments ---------------------------------------------------------
  // A long paste or an image stays out of the text. The text holds a single
  // private-use codepoint in its place, so the cursor steps over it, one
  // Backspace removes it, and wrapping measures it as the label it is drawn
  // as. The content goes back in only when the message is sent.
  struct Attachment {
    bool image = false;
    std::string data;   // the pasted text, or the image's file path
    std::string label;  // what the box shows: "[Pasted 1,234 characters]"
  };
  // Plane 15 private use: never produced by a keyboard, and far from anything
  // an agent's output or a paste would plausibly carry.
  static constexpr char32_t kTokenBase = 0xF0000;

  void insert_attachment(Attachment a) {
    checkpoint(Edit::Other);
    attachments_.push_back(std::move(a));
    insert_char(kTokenBase + char32_t(attachments_.size() - 1));
  }
  const Attachment* attachment(char32_t cp) const {
    if (cp < kTokenBase || cp - kTokenBase >= attachments_.size()) return nullptr;
    return &attachments_[cp - kTokenBase];
  }
  int image_count() const {
    return int(std::count_if(attachments_.begin(), attachments_.end(),
                             [](const Attachment& a) { return a.image; }));
  }
  // Display width, with an attachment counted as its label.
  int glyph_width(char32_t cp) const {
    if (const Attachment* a = attachment(cp)) return text::str_width(a->label);
    return std::max(0, text::cp_width(cp));
  }

  // The message as it goes to the agent: text with every paste put back, and
  // each image as a part of its own, in order.
  using Part = MessagePart;
  std::vector<Part> parts() const {
    std::vector<Part> out;
    std::string run;
    for (size_t i = 0; i < text_.size();) {
      const size_t start = i;
      const char32_t cp = text::decode(text_, i);
      const Attachment* a = attachment(cp);
      if (!a) { run.append(text_, start, i - start); continue; }
      if (!a->image) { run += a->data; continue; }
      if (!run.empty()) out.push_back(Part{false, std::move(run)});
      run.clear();
      out.push_back(Part{true, a->data});
    }
    if (!run.empty()) out.push_back(Part{false, std::move(run)});
    return out;
  }

  // A bare arrow with a selection active collapses to the edge it points
  // toward and stops there — it does not also take its usual one-codepoint
  // step, which is what every other text box does.
  void move_left(bool extend) {
    if (!extend && has_selection()) { cursor_ = sel_lo(); drop_selection(); return; }
    move_cursor(prev_boundary(cursor_), extend);
  }
  void move_right(bool extend) {
    if (!extend && has_selection()) { cursor_ = sel_hi(); drop_selection(); return; }
    move_cursor(next_boundary(cursor_), extend);
  }
  void move_home(bool extend) { move_cursor(line_start(cursor_), extend); }
  void move_end(bool extend) { move_cursor(line_end(cursor_), extend); }
  // Ctrl+Home / Ctrl+End: the whole message, not just the line.
  void move_doc_start(bool extend) { move_cursor(0, extend); }
  void move_doc_end(bool extend) { move_cursor(text_.size(), extend); }

  // Word-wise movement, the classical Ctrl+Left/Right (or Alt+B/F): skip any
  // whitespace in the direction of travel, then the run of non-space.
  void move_word_left(bool extend) {
    if (!extend && has_selection()) { cursor_ = sel_lo(); drop_selection(); return; }
    move_cursor(word_left(cursor_), extend);
  }
  void move_word_right(bool extend) {
    if (!extend && has_selection()) { cursor_ = sel_hi(); drop_selection(); return; }
    move_cursor(word_right(cursor_), extend);
  }

  void delete_word_before() {
    checkpoint(Edit::Other);
    if (erase_selection()) return;
    size_t to = word_left(cursor_);
    text_.erase(to, cursor_ - to);
    cursor_ = to;
  }
  void delete_word_after() {
    checkpoint(Edit::Other);
    if (erase_selection()) return;
    size_t to = word_right(cursor_);
    text_.erase(cursor_, to - cursor_);
  }

  // Readline's Ctrl+U / Ctrl+K, within the current line.
  void kill_to_start() {
    checkpoint(Edit::Other);
    if (erase_selection()) return;
    size_t to = line_start(cursor_);
    text_.erase(to, cursor_ - to);
    cursor_ = to;
  }
  void kill_to_end() {
    checkpoint(Edit::Other);
    if (erase_selection()) return;
    size_t to = line_end(cursor_);
    text_.erase(cursor_, to - cursor_);
  }

  // Moves the cursor up (dir<0) or down (dir>0) a line, keeping roughly the
  // same byte offset into the line. False at the top/bottom line: the caller
  // decides what an arrow with nowhere further to go should do instead.
  bool move_line(int dir, bool extend) {
    size_t ls = line_start(cursor_);
    size_t col = cursor_ - ls;
    size_t to;
    if (dir < 0) {
      if (ls == 0) return false;
      size_t prev_start = line_start(ls - 1);
      to = std::min(prev_start + col, ls - 1);
    } else {
      size_t le = line_end(cursor_);
      if (le == text_.size()) return false;
      size_t next_start = le + 1;
      to = std::min(next_start + col, line_end(next_start));
    }
    while (to > 0 && to < text_.size() && (uint8_t(text_[to]) & 0xC0) == 0x80) to--;
    move_cursor(to, extend);
    return true;
  }

  void backspace() {
    if (text_.empty()) return;
    checkpoint(Edit::Delete);
    if (erase_selection()) return;
    size_t prev = prev_boundary(cursor_);
    text_.erase(prev, cursor_ - prev);
    cursor_ = prev;
  }

  void del() {
    if (cursor_ >= text_.size() && !has_selection()) return;
    checkpoint(Edit::Delete);
    if (erase_selection()) return;
    text_.erase(cursor_, next_boundary(cursor_) - cursor_);
  }

  void insert_char(char32_t cp) {
    checkpoint(Edit::Type);
    erase_selection();
    std::string ins;
    text::encode(cp, ins);
    text_.insert(cursor_, ins);
    cursor_ += ins.size();
    // A space ends the word: the next one is an undo step of its own.
    if (cp == U' ' || cp == U'\t') last_ = Edit::None;
  }

  void insert_newline() {
    checkpoint(Edit::Other);
    erase_selection();
    text_.insert(cursor_, 1, '\n');
    cursor_++;
  }

  // [start,end) byte ranges of each line, '\n' excluded. Always at least one.
  std::vector<std::pair<size_t, size_t>> lines() const {
    std::vector<std::pair<size_t, size_t>> out;
    size_t start = 0;
    for (size_t i = 0; i <= text_.size(); i++)
      if (i == text_.size() || text_[i] == '\n') { out.push_back({start, i}); start = i + 1; }
    return out;
  }

  // Display rows for a `cols`-wide box: logical lines hard-wrapped, and rows
  // that tile the bytes exactly (an editor must not drop the spaces a prose
  // wrapper skips at a break). At least one row, so an empty box still draws.
  std::vector<std::pair<size_t, size_t>> wrap(int cols) const {
    std::vector<std::pair<size_t, size_t>> out;
    const int limit = std::max(1, cols);
    size_t ls = 0;
    for (;;) {
      size_t le = text_.find('\n', ls);
      const bool last = le == std::string::npos;
      if (last) le = text_.size();
      size_t p = ls;
      if (p == le) out.push_back({p, p});
      while (p < le) {
        const size_t row_start = p;
        int w = 0;
        while (p < le) {
          size_t j = p;
          const char32_t cp = text::decode(text_, j);
          const int cw = glyph_width(cp);
          if (w > 0 && w + cw > limit) break;
          p = j;
          w += cw;
          if (w >= limit) break;
        }
        out.push_back({row_start, p});
      }
      if (last) break;
      ls = le + 1;
    }
    return out;
  }

  // Up/Down across display rows (wrapped), keeping the visual column. The
  // logical move_line() below is what the unit tests exercise; the pane uses
  // this one so a wrapped line behaves like a line.
  bool move_display_line(int dir, int cols, bool extend) {
    const auto rows = wrap(cols);
    if (rows.empty()) return false;
    size_t cr = rows.size() - 1;
    for (size_t i = 0; i < rows.size(); i++)
      if (cursor_ <= rows[i].second) { cr = i; break; }
    const size_t ls = rows[cr].first;
    int col = 0;
    for (size_t i = ls; i < cursor_;) col += glyph_width(text::decode(text_, i));
    if (dir < 0) {
      if (cr == 0) return false;
      move_cursor(offset_at_col(rows[cr - 1].first, rows[cr - 1].second, col), extend);
      return true;
    }
    if (cr + 1 >= rows.size()) return false;
    move_cursor(offset_at_col(rows[cr + 1].first, rows[cr + 1].second, col), extend);
    return true;
  }

 private:
  enum class Edit : uint8_t { None, Type, Delete, Other };
  struct Snapshot {
    std::string text;
    size_t cursor, anchor;
    std::vector<Attachment> attachments;
  };
  static constexpr size_t kMaxUndo = 200;

  // Records the state before an edit. A run of the same kind of edit (typing,
  // or deleting a character at a time) shares one record; moving the cursor
  // or starting from a selection ends the run.
  void checkpoint(Edit e) {
    if (e != Edit::Other && e == last_ && !has_selection()) return;
    undo_.push_back(Snapshot{text_, cursor_, sel_anchor_, attachments_});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    last_ = e;
  }
  bool step(std::vector<Snapshot>& from, std::vector<Snapshot>& to) {
    if (from.empty()) return false;
    to.push_back(Snapshot{text_, cursor_, sel_anchor_, attachments_});
    Snapshot& s = from.back();
    text_ = std::move(s.text);
    cursor_ = s.cursor;
    sel_anchor_ = s.anchor;
    attachments_ = std::move(s.attachments);
    from.pop_back();
    last_ = Edit::None;
    return true;
  }
  std::string expand(size_t lo, size_t hi) const {
    std::string out;
    for (size_t i = lo; i < hi && i < text_.size();) {
      const size_t start = i;
      const char32_t cp = text::decode(text_, i);
      if (const Attachment* a = attachment(cp)) out += a->data;
      else out.append(text_, start, i - start);
    }
    return out;
  }

  size_t prev_boundary(size_t i) const {
    if (i == 0) return 0;
    i--;
    while (i > 0 && (uint8_t(text_[i]) & 0xC0) == 0x80) i--;
    return i;
  }
  size_t next_boundary(size_t i) const {
    if (i >= text_.size()) return text_.size();
    size_t j = i;
    text::decode(text_, j);
    return j;
  }
  bool is_space_at(size_t i) const {
    if (i >= text_.size()) return true;
    size_t j = i;
    const char32_t cp = text::decode(text_, j);
    return cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r';
  }
  size_t word_left(size_t from) const {
    size_t i = from;
    while (i > 0 && is_space_at(prev_boundary(i))) i = prev_boundary(i);
    while (i > 0 && !is_space_at(prev_boundary(i))) i = prev_boundary(i);
    return i;
  }
  size_t word_right(size_t from) const {
    size_t i = from;
    while (i < text_.size() && is_space_at(i)) i = next_boundary(i);
    while (i < text_.size() && !is_space_at(i)) i = next_boundary(i);
    return i;
  }
  // Byte offset in [start,end) where the display column first reaches `col`.
  size_t offset_at_col(size_t start, size_t end, int col) const {
    if (col <= 0) return start;
    int w = 0;
    size_t p = start;
    while (p < end) {
      size_t j = p;
      const char32_t cp = text::decode(text_, j);
      const int cw = glyph_width(cp);
      if (w > 0 && w + cw > col) break;
      p = j;
      w += cw;
      if (w >= col) break;
    }
    return p;
  }

  size_t line_start(size_t at) const {
    if (at == 0) return 0;
    size_t nl = text_.rfind('\n', at - 1);
    return nl == std::string::npos ? 0 : nl + 1;
  }
  size_t line_end(size_t at) const {
    size_t nl = text_.find('\n', at);
    return nl == std::string::npos ? text_.size() : nl;
  }

  void drop_selection() { sel_anchor_ = std::string::npos; }

  // Erases the selected range, if any. Returns whether it did.
  bool erase_selection() {
    if (sel_anchor_ == std::string::npos) return false;
    size_t lo = sel_lo(), hi = sel_hi();
    text_.erase(lo, hi - lo);
    cursor_ = lo;
    drop_selection();
    return true;
  }

  // Moves the cursor to `to`. Shift held extends the selection (starting one
  // if there wasn't one); otherwise any selection collapses, the way every
  // other text box does when an arrow is tapped with a selection active.
  void move_cursor(size_t to, bool extend) {
    last_ = Edit::None;
    if (extend) {
      if (sel_anchor_ == std::string::npos) sel_anchor_ = cursor_;
    } else {
      drop_selection();
    }
    cursor_ = to;
  }

  std::string text_;
  std::vector<Attachment> attachments_;  // indexed by token - kTokenBase
  size_t cursor_ = 0;
  size_t sel_anchor_ = std::string::npos;  // npos: no selection; else the other end
  std::vector<Snapshot> undo_, redo_;
  Edit last_ = Edit::None;
};

}  // namespace mico
