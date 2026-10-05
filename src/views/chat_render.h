#pragma once
#include <utility>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/front_vec.h"
#include "adapters/adapter.h"
#include "core/conversation.h"
#include "vt/surface.h"
#include "term/term.h"
#include "base/text.h"
#include "ui/pane.h"
#include "ui/theme.h"
#include "views/markdown.h"

namespace mico {

// Renders a transcript as a chat: the Conversation's window of events, laid
// out into rows, with the per-tool expansion state, scrolling, finding and
// the question cards' keys. Used both by the session browser and by a live
// agent pane, so the two cannot drift apart in appearance or behaviour.
//
// Nothing here owns text. Events index into `arena_`, rows index into either
// `arena_` or `scratch_`, and a steady frame allocates nothing.
class ChatRenderer {
 public:
  ChatRenderer();
  bool open(const std::string& path, const Adapter* adapter);
  // The chat itself, apart from how it is drawn.
  const Conversation& conversation() const { return conv_; }
  // The folder the agent works in: a ```chart block's data file is read
  // relative to it.
  void set_base_dir(const std::string& dir) { chart_env_.base_dir = dir; }
  bool has_adapter() const { return conv_.adapter() != nullptr; }
  const std::string& path() const { return conv_.path(); }

  void poll_growth();
  void render(Painter& p, const Theme& th, const Filters& f);
  bool on_key(const KeyEvent& k);
  bool on_mouse(const MouseEvent& m, Point local);
  // The URL a click just landed on, once.
  std::string take_url() { return std::exchange(open_url_, {}); }
  std::vector<MenuItem> context_menu(Point local);
  // `copy_out`, when given, receives text the action asked to put on the
  // clipboard.
  bool on_action(const std::string& a, Filters& f, std::string* copy_out = nullptr);
  // What an action did that the status line should say ("image copied"), once.
  std::string take_notice() { return std::exchange(notice_, {}); }

  // A picture, chart or display equation clicked is zoomed into: laid out
  // again as large as the pane allows, where it is. Clicked again, or another
  // one zoomed, it is back to its size.
  void toggle_zoom(uint32_t image_id);
  const std::string& zoomed() const { return zoom_; }

  // --- model questions ---------------------------------------------------
  // A question card is parsed from a Question event's `questions` JSON and
  // rendered inline. For a live single-question card the user can pick a
  // button with the keyboard or the mouse; the pane turns the complete form
  // into the keys the agent's own menu understands. Stored transcripts are
  // read-only (the answer is the tool result beneath).
  using QuestionOption = mico::QuestionOption;
  using QuestionSpec = mico::QuestionSpec;
  using QuestionCard = mico::QuestionCard;
  // A committed choice, handed to the pane to translate into agent keys.
  struct Answer {
    uint64_t tool_id = 0;
    std::string tool;
    QuestionCard card;
    std::vector<std::vector<uint8_t>> chosen;  // per question: selected flags
    // The user picked "answer in your own words" on question `free_q`: the
    // pane opens a quoted reply in the prompt box instead of sending anything.
    bool free_text = false;
    int free_q = 0;
    std::vector<std::string> notes;  // per question: the user's note, often empty
  };

  // True when the newest blocking question still has no answer: the agent is
  // stopped on it. Optional questions never count; the agent keeps working.
  bool has_pending_question() const {
    return conv_.at_tail() && !conv_.pending_questions().empty();
  }
  // Optional questions the user has neither answered nor moved past.
  int open_async_questions() const { return conv_.open_async_questions(); }
  // Messages the agent is holding: sent while it worked, not yet handed to the
  // model. Claude records these; the pane lists them above the prompt.
  std::vector<std::string_view> agent_queue() const {
    std::vector<std::string_view> out;
    for (const Str& s : conv_.agent_queue()) out.push_back(conv_.arena().view(s));
    return out;
  }
  bool question_active() const;
  bool question_submitted() const { return has_pending_question() && !sent_.empty(); }
  // Enables the interactive card. Stored views leave this off.
  void set_questions_interactive(bool on) { q_interactive_ = on; }

  // --- work in flight ----------------------------------------------------
  // While the agent is working, the pending tool row (a call with no result
  // yet) draws the spinner instead of its bullet, and in_flight_tool() names
  // the current action for the live status line. Stored views leave it off.
  void set_working(bool on, char32_t spin) { working_ = on; spin_ = spin; }
  // A live pane with a reserved activity row already displays the current
  // command there. Stored views retain every tool in the transcript.
  void set_activity_bar(bool on) { activity_bar_ = on; }
  bool in_flight_tool(std::string_view* name, std::string_view* summary, uint64_t* id = nullptr) const;
  // The reply the agent is still writing, as markdown (screen_reply()). It is
  // drawn under the chat as it grows, until the transcript holds a message
  // that reads the same; empty clears it.
  void set_draft(std::string_view text) {
    if (text != draft_) { draft_.assign(text); draft_checked_ = false; }
  }
  // True while the draft is drawn: set, and not in the transcript yet.
  bool draft_shown();
  // A side question and its answer (claude's /btw), drawn under everything
  // else, draft included, until cleared: it never reaches the transcript, so
  // the screen is where it comes from. `status` is the line under it
  // ("answering…", the keys). An empty question clears it.
  void set_aside(std::string_view question, std::string_view answer, std::string_view status) {
    aside_q_.assign(question);
    aside_a_.assign(answer);
    aside_status_.assign(status);
  }
  bool aside_shown() const { return !aside_q_.empty(); }
  // A keypress while a question card is active; false leaves it for the pane.
  bool question_key(const KeyEvent& k);
  // Notes on the live card: the question the cursor is on, its note, and
  // setting it (empty clears). A note goes out with the answer. False/empty
  // when no card takes notes (an optional question is answered in the prompt
  // box, where anything can be said already).
  int question_note_target(std::string* header = nullptr) const;
  const std::string& question_note(int qi) const;
  bool set_question_note(int qi, std::string text);
  // A committed answer waiting to be delivered. Consumes it.
  bool take_answer(Answer& out);

  // --- finding -----------------------------------------------------------
  // Search covers the whole transcript, not just the loaded window: each line
  // whose raw bytes hold the query (ASCII case folded) is parsed, and counts
  // only if the query is in text the chat shows at the current density. The
  // moves are carried out by the next render(), which knows width and filters.
  void set_find_query(std::string q);
  const std::string& find_query() const { return find_q_; }
  // Next match: dir < 0 older, dir > 0 newer, from the current match or else
  // from what is on screen. Wraps around at either end.
  void find_next(int dir) { if (!find_q_.empty()) pending_find_ = dir < 0 ? -1 : 1; }
  // Matches found, or -1 before the first search; the current one, 1-based.
  int find_count() const { return find_done_ ? int(find_lines_.size()) : -1; }
  int find_index() const { return find_cur_ + 1; }
  void clear_find();
  // Opens at the line holding byte `offset` of the file, with `query` lit:
  // where a search across chats found it.
  void reveal(uint64_t offset, std::string query);
  // Moves to the previous (dir < 0) or next user message, loading older
  // history as needed. At the newest, next goes to the bottom.
  void jump_user(int dir) { pending_user_ = dir < 0 ? -1 : 1; }

  // --- outline -----------------------------------------------------------
  // The landmarks of the whole transcript, oldest first: your messages, the
  // files the agent edited, tool calls that failed, questions, notices such
  // as a compaction. Read from the file, not from the loaded window, and kept:
  // each call reads only what is new since the last, then goes on backwards
  // into older history for at most `budget_ms`. False when older history is
  // still unread.
  using OutlineEntry = mico::OutlineEntry;
  bool outline(std::vector<OutlineEntry>& out, int budget_ms) { return conv_.outline(out, budget_ms); }
  // The full text of the user message whose line starts at byte `offset` (an
  // outline 'u' entry) — the outline's own label is one line and truncated,
  // this is what ↑ puts back in the prompt box. False when the line holds no
  // user message.
  bool user_text_at(uint64_t offset, std::string* out) { return conv_.user_text_at(offset, out); }
  // Byte offset of the line at the top of the view: where an outline opens.
  uint64_t view_offset() const;
  // Scrolls to the line holding byte `offset`, loading older history as needed.
  void go_to(uint64_t offset) { pending_reveal_ = offset; }

  void to_bottom() { scroll_ = 0; }
  // Moves the view to a position in the file, 0 being the start. The index is
  // extended backwards first: it only covers the tail until someone asks.
  void seek_fraction(double f);
  // True while the scrollbar thumb is held. The pane uses it to ask the app
  // for the mouse, so the drag survives the pointer leaving the pane.
  bool grabbing() const { return dragging_; }
  // Where the thumb sits, 0..1, as the last frame computed it.
  double thumb_fraction() const { return thumb_frac_; }
  void set_scroll(int s) { scroll_ = s; }
  // Transcript text currently retained. Bounded by the window budget.
  size_t retained_bytes() const { return conv_.arena().bytes(); }
  size_t window_resets() const { return trims_; }

  const SessionState& state() const { return conv_.state(); }
  // Declares the chip fields for `a` if none are known yet, so the bar shows
  // before the first turn. Safe to call every frame.
  void seed_agent(const Adapter* a) {
    if (a && conv_.state().empty()) a->seed_state(conv_.state());
  }
  // Optimistic update after the user picks a value from a chip menu, before
  // the agent has written the change into the transcript.
  void set_state(std::string_view key, std::string_view value) { conv_.state().set(key, key, value); }
  // A clickable chip in the status strip under the chat.
  struct Chip {
    Rect rect;
    std::string key;
  };
  // Draws the strip and reports where each chip landed. Empty when the
  // transcript has told us nothing about the session yet.
  void render_chips(Painter& p, const Theme& th, std::vector<Chip>& hits) const;

 private:
  // Where the card being answered is: which question is current (or its size,
  // meaning the Submit step), the option cursor on each question, and the
  // selected flags per question. Questions are rare, so a linear scan beats a
  // map's include and node churn; none of this is touched on a steady frame.
  struct QState {
    int q = 0;
    std::vector<int> cursor;
    std::vector<std::vector<uint8_t>> chosen;
    std::vector<std::string> notes;
  };

  // Semantic role, resolved to colours only at draw time. Keeping the theme out
  // of the layout means recolouring never invalidates a single row.
  enum class RowStyle : uint8_t {
    Gap, User, Assistant, Thinking, Tool, Result, ResultErr, Dim,
    Question, QText, QOption, QSubmit, QHint,
    Work,  // what the agent says between steps, as opposed to its answer
    Fold,  // a finished turn's steps, folded into one line
  };
  static constexpr int kRowStyles = int(RowStyle::Fold) + 1;

  // A row is a run of styled segments. Most rows hold exactly one, but inline
  // markup means the count is not fixed, so segments live in one flat array and
  // the row just names a range of it.
  // A marker drawn in the left margin, spanning every line of a turn. Colour
  // alone does not survive a long scroll — a gutter gives the eye an edge to
  // follow, and still reads without colour at all.
  enum class Gutter : uint8_t { None, User, Answer };

  struct Row {
    uint64_t tool_id;  // 0 when the row is not a clickable tool row
    uint32_t seg_first;
    uint32_t src_line;  // line this row came from: true position, and re-anchoring
    uint16_t seg_count;
    uint8_t indent;
    RowStyle base;
    Gutter gutter;
    uint8_t q = 0;      // question index, when this row is part of a card
    uint8_t opt = 0xFF;  // option index; 0xFE is the multi-select Submit row
    bool cont = false;   // a wrapped option's later line: no cursor or check
    bool code = false;   // a row of a fenced code block: on the code background
    uint8_t tint = 0;    // a diff's row: 1 added, 2 removed
    uint16_t node = 0;   // a JSON result's row that opens a container: its id + 1
  };
  static constexpr uint32_t kScratch = md::kScratchBit;

  void reset();
  // Carries out a pending find, reveal or user-message jump.
  void resolve_moves(int w, int h, const Filters& f);
  void compute_matches(const Filters& f);
  // Puts the first row of line `line` a third of the way down the viewport.
  void show_line(uint32_t line, int w, int h, const Filters& f);
  // The line at the middle of the viewport, as last drawn.
  uint32_t viewport_line(int h) const;
  void grow_backwards();
  // Parses forward, at most `max_lines`. Live tailing wants everything that
  // arrived; filling a screen after a seek wants a chunk, not the rest of a
  // three-hundred-megabyte file.
  void grow_forwards(size_t max_lines = size_t(-1));
  void invalidate_rows();
  void ensure_rows(int w, size_t needed, const Filters& f);
  // Drops the window and re-seeds it at the current viewport once the arena
  // grows past its budget, so scrolling through a huge transcript cannot
  // retain the whole thing.
  void trim_window();
  // Drops the window and starts again so it ends at `line`.
  void reanchor(size_t line);
  void seek_from_bar(int thumb_top);
  // Lays out events appended since the last pass.
  void layout_appended(int w, const Filters& f);
  const code::Lang* result_lang(size_t index) const;
  const code::Lang* output_lang_ = nullptr;  // the tool output being laid out, when it is a file
  void layout_image(const Event& e, int w);
  bool visible(size_t i, const Filters& f) const;
  // visible(), or the first of a folded turn's steps, which carries the fold.
  bool laid_out(size_t i, const Filters& f) const;

  // --- turns -------------------------------------------------------------
  // What each event is to its turn. A turn that ended with text after its
  // last step has an answer: that text. Everything the agent said before it
  // is work, and at Minimal and Normal density a finished turn's work folds
  // into one line above the answer, opened by a click.
  enum : uint8_t { kWork = 1, kAnswer = 2, kFolded = 4, kFoldHead = 8 };
  static constexpr uint64_t kFoldBit = 1ull << 63;
  FrontVec<uint8_t> roles_;     // per event of the window
  FrontVec<uint64_t> fold_of_;  // per event: the fold it is in, 0 for none
  size_t open_from_ = 0;        // where the last turn starts, still open to new events
  std::vector<uint8_t> zeros_;
  std::vector<uint64_t> fold_zeros_;
  // Works the roles out again after the window grew by `prepended` events at
  // the front (and any at the back). Events already laid out whose role
  // changed — a turn that just ended — are laid out again.
  void classify(size_t prepended);
  static bool folding(const Filters& f) { return f.density != Density::Full; }
  bool folded_away(size_t i, const Filters& f) const {
    return folding(f) && (roles_[i] & kFolded) && !expanded(fold_of_[i]);
  }
  void layout_fold(size_t index, int w);
  // Drops the text of the first `count` events, just loaded from older
  // history, that are steps of a folded turn: they are not drawn, and a turn
  // longer than the window's budget would otherwise fill it and never let
  // scrolling reach the message above. `mark` is where their text starts.
  void strip_folded(size_t count, size_t mark);
  // A fold opened whose steps were loaded bare: read again with their text.
  bool reload_ = false;
  // Opens the fold holding line `line`, so a search or a jump can show it.
  bool unfold_line(uint32_t line);
  // The line at the top of a view `h` rows tall, and how many rows into it.
  struct Anchor {
    uint32_t line = UINT32_MAX;
    int into = 0;
  };
  Anchor top_anchor(int h) const;
  // After a relayout, puts `line` back at the top of the view, `into` rows
  // into it.
  void restore_anchor(uint32_t line, int into, int w, int h, const Filters& f);
  void layout_event(size_t index, int w, const Filters& f);
  void layout_question(const Event& e, int w);
  // Renumbers the rows after older history was indexed in front of them.
  void shift_rows(size_t count);
  // Drops answers in progress, and answers sent, to questions no longer open.
  void prune_answers();
  // Re-lays out the window when the conversation says a card changed.
  void sync_cards();
  uint64_t cards_seen_ = 0;
  QState* state_for(uint64_t id, const QuestionCard& card);
  // The card that keys and clicks answer, 0 if none: the blocking question
  // when there is one, since it holds the agent, else the newest open optional
  // one.
  uint64_t live_question() const;
  // Options a cursor walks on question `q`: an optional card adds the
  // own-words row after them.
  int cursor_stops(const QuestionCard& card, int q) const;
  const QState* qstate_of(uint64_t id) const;
  const QuestionCard* card(uint64_t id) const;
  // True when the card for `id` is the one being answered here right now.
  bool question_live(uint64_t id) const;
  // A card needs a Submit step when it has more than one question, or a single
  // multi-select one (Space toggles, Enter confirms).
  bool card_needs_submit(const QuestionCard& card) const;
  // Moves to the next question, or to the Submit step, or commits.
  void question_advance(QState& st, const QuestionCard& card, uint64_t id);
  // A click/keystroke on question `qi`'s option `oi`. `confirm` advances after
  // selecting (Enter); otherwise it just toggles (Space/click on multi).
  bool question_choose(uint64_t id, int qi, int oi, bool confirm);
  bool question_switch_to(uint64_t id, int qi);
  bool question_submit(uint64_t id);
  // "Answer in your own words" on an optional card.
  bool question_free_text(uint64_t id, int qi);
  void commit_answer(uint64_t id, const QuestionCard& card,
                     const std::vector<std::vector<uint8_t>>& chosen);
  void emit_scratch(Str s, RowStyle base, int indent, uint64_t tool_id, uint8_t q, uint8_t opt,
                    bool cont = false);
  // Renders `text` into rows, capped at `cap` lines, appending a "… N more"
  // marker when it truncates. `markdown` selects prose rendering over verbatim.
  void emit_text(Str text, RowStyle base, int indent, int w, size_t cap, uint64_t tool_id,
                 bool markdown, bool diff, Gutter gutter = Gutter::None);
  void flush_lines(RowStyle base, int extra_indent, uint64_t tool_id, Gutter gutter);
  std::string_view seg_text(const md::Seg& s) const;

  bool expanded(uint64_t id) const;
  void toggle(uint64_t id);
  bool tool_pending(uint64_t id) const { return conv_.tool_pending(id); }
  // Tool id on the row at screen row `y` of the last frame, 0 if none.
  uint64_t tool_at(int y) const;

  struct QStateSlot {
    uint64_t id;
    QState st;
  };
  using AsyncStatus = Conversation::AsyncStatus;
  using AsyncSlot = Conversation::AsyncSlot;
  const AsyncSlot* async_slot(uint64_t id) const { return conv_.async_slot(id); }
  // An optional card changed state after it was laid out; the next frame
  // re-lays out the window rather than leave a stale card on screen.
  bool relayout_ = false;
  std::vector<uint64_t> sent_;           // answered here, result not in yet
  // The draft: its text, whether the transcript already holds it (worked out
  // again when either side changes), and what its rows added to the layout on
  // the last frame, taken off again before the next one.
  std::string draft_;
  bool draft_checked_ = false, draft_committed_ = false;
  size_t draft_events_ = 0;
  size_t draft_rows_ = 0, draft_segs_ = 0, draft_scratch_ = 0;
  void drop_draft_rows();
  void layout_draft(int w);
  std::string aside_q_, aside_a_, aside_status_;
  bool working_ = false;
  bool activity_bar_ = false;
  uint64_t activity_tool_ = 0;
  char32_t spin_ = U'\u280B';
  std::vector<QuestionCard> cards_;
  std::vector<QStateSlot> qstate_;
  // Option rows of the last frame, in pane-local coordinates, for hit tests.
  struct QHit {
    Rect rect;
    uint64_t tool_id;
    uint8_t q, opt;
  };
  std::vector<QHit> question_hits_;
  // Where links were drawn this frame, so a click can find one.
  struct LinkHit {
    Rect rect;
    uint16_t link;
  };
  std::vector<LinkHit> link_hits_;
  uint16_t press_link_ = 0;
  // Where images were drawn this frame: a click zooms one, a right-click
  // offers to copy it.
  struct ImageHit {
    Rect rect;
    uint32_t id;
  };
  std::vector<ImageHit> image_hits_;
  uint32_t image_at(Point pt) const;
  uint32_t press_image_ = 0;
  uint32_t menu_image_ = 0;
  // The image zoomed into, by md::image_source(); and the one just zoomed or
  // let go, brought into view once it is laid out again.
  std::string zoom_;
  std::string zoom_reveal_;
  Anchor zoom_back_;  // where the view was before the zoom
  void reveal_zoomed(int h);
  std::string notice_;
  std::string open_url_;
  uint64_t rows_links_gen_ = 0;
  uint64_t rows_settings_gen_ = 0;  // the render settings it was laid out under
  bool q_interactive_ = false;
  Answer answer_;
  bool answer_ready_ = false;

  Conversation conv_;
  Arena scratch_;  // synthesized row text; discarded whenever layout is rebuilt

  FrontVec<Row> rows_;
  md::Work md_work_;  // md::render's buffers, reused across messages
  std::vector<Row> moved_;  // reused when moving a laid-out event to the front
  std::vector<md::Seg> segs_;      // flat segment pool addressed by rows_
  std::vector<md::Line> mdlines_;  // reused markdown output
  std::vector<md::Seg> inline_;    // reused inline-run scratch
  std::vector<uint64_t> expanded_;      // sorted; tool calls the user opened
  // Tool results drawn as notebooks: their images are drawn inside them, in
  // place, not again after them.
  std::unordered_set<uint64_t> notebook_tools_;
  // JSON containers the user opened or closed, by tool result; and, while
  // one is laid out, the container each of its rows opens.
  std::unordered_map<uint64_t, std::unordered_set<int>> json_flips_;
  std::vector<uint16_t> mdline_nodes_;
  std::vector<text::Span> spans_;       // reused wrap output


  std::string find_q_, find_fold_;
  std::vector<uint32_t> find_lines_;  // matching lines, oldest first
  std::vector<uint64_t> find_tools_;  // per match: a tool to open to show it, or 0
  bool find_done_ = false;
  size_t find_upto_ = 0;               // line_count() the matches cover
  int find_cur_ = -1;
  uint32_t find_line_ = UINT32_MAX;    // the current match's line, lit harder
  int pending_find_ = 0;
  int pending_user_ = 0;
  uint64_t pending_reveal_ = UINT64_MAX;
  std::string row_text_;               // reused when lighting matches

  // Data files the laid-out charts read, with the times they had then. A
  // change redraws them: a training run's metrics plot live.
  md::ChartEnv chart_env_;
  std::vector<std::pair<std::string, int64_t>> chart_files_;
  int64_t chart_check_ms_ = 0;
  void check_chart_files();

  size_t rows_from_event_ = 0;
  size_t rows_to_event_ = 0;
  int rows_w_ = -1;
  Density rows_density_ = Density::Normal;
  uint64_t expand_gen_ = 0;
  uint64_t rows_gen_ = 0;
  uint64_t rows_math_gen_ = 0;

  size_t trims_ = 0;
  uint32_t cur_line_ = 0;
  uint32_t menu_line_ = 0;  // src_line under the last right-click  // src_line of the event currently being laid out
  int scroll_ = 0;
  int last_h_ = 20;
  int last_pad_ = 0;  // blank rows above a short conversation, for hit-testing
  // Scrollbar geometry from the last frame, so a click can be tested against
  // what the user actually saw.
  int bar_col_ = -1;
  int bar_y_ = 0;
  int bar_h_ = 0;
  double thumb_frac_ = 0;
  bool dragging_ = false;
  int grab_dy_ = 0;
  // Tool row under the last left press; the toggle happens on release so a
  // drag that turns into a text selection does not also open the call.
  uint64_t press_tool_ = 0;
  int press_y_ = -1;
};

}  // namespace mico
