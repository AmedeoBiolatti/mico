#pragma once
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "vt/surface.h"

namespace mico {

using VtRow = std::vector<Cell>;

// True when a row carries nothing.
bool row_is_blank(const VtRow& r);
// True when a row is only the application's own furniture — box borders,
// rules, block shading — and no actual text. The bottom of an agent's screen
// is its input box, and splicing those borders into a chat view just reads as
// stray horizontal lines.
bool row_is_chrome(const VtRow& r);


// A terminal emulator: bytes in, cell grid out. Both agents render inline
// rather than on the alternate screen, so scrollback is ours to keep and is
// part of the model, not an afterthought.
class Vt {
 public:
  void resize(int w, int h);
  void write(std::string_view bytes);

  int width() const { return w_; }
  int height() const { return h_; }

  // Rows addressed across scrollback + screen: 0 is the oldest retained line.
  int total_rows() const { return int(scrollback_.size()) + h_; }
  const VtRow& row(int i) const;

  Point cursor() const { return {cx_, cy_}; }
  bool cursor_visible() const { return cursor_visible_; }
  bool alt_screen() const { return alt_; }

  // Which input protocols the application asked for; the raw pane forwards
  // mouse events only when the agent actually wants them.
  bool wants_mouse() const { return mouse_mode_ != 0; }
  bool sgr_mouse() const { return sgr_mouse_; }
  bool app_cursor_keys() const { return app_cursor_; }
  bool bracketed_paste() const { return bracketed_paste_; }

 private:
  // Sequences arrive split across reads, so every multi-byte form needs a
  // state rather than a look-ahead into the current buffer.
  enum class State { Ground, Esc, EscFinal, Csi, Osc, OscEsc, StringIgnore, StringEsc };

  VtRow& line(int y);
  void put(char32_t cp, int w);
  // Bulk path for a run of printable ASCII, which is nearly all agent output.
  void put_ascii_run(std::string_view s);
  void newline();
  void index();          // move down, scrolling within the margins
  void reverse_index();
  void carriage_return() { cx_ = 0; wrap_pending_ = false; }
  void scroll_up(int n);
  void scroll_down(int n);
  void erase_in_display(int mode);
  void erase_in_line(int mode);
  void insert_lines(int n);
  void delete_lines(int n);
  void insert_chars(int n);
  void delete_chars(int n);
  void erase_chars(int n);
  void set_cursor(int x, int y);
  void blank(VtRow& r) const;

  void exec_csi(char final);
  void exec_sgr();
  void set_mode(bool on);
  void exec_esc(char b);
  int param(size_t i, int fallback) const;

  int w_ = 80, h_ = 24;
  int cx_ = 0, cy_ = 0;
  int saved_cx_ = 0, saved_cy_ = 0;
  int top_ = 0, bot_ = 23;  // scrolling region, inclusive
  bool wrap_pending_ = false;
  bool autowrap_ = true;
  bool cursor_visible_ = true;
  bool alt_ = false;
  bool app_cursor_ = false;
  bool sgr_mouse_ = false;
  bool bracketed_paste_ = false;
  int mouse_mode_ = 0;

  Style cur_{};
  Style saved_style_{};

  std::vector<VtRow> screen_;
  std::vector<VtRow> alt_buf_;
  std::deque<VtRow> scrollback_;

  State state_ = State::Ground;
  std::string params_;
  std::string intermediates_;
  std::vector<int> nums_;
  std::string utf8_;  // partial multi-byte character across write() calls
};

// True when a row *begins* with an application's own furniture: a box border,
// a prompt marker, a status glyph. Judged on the first visible glyph, so a
// bordered input line counts even though it also carries text.
bool row_starts_furniture(const VtRow& r);

// A row led by a completed-message bullet (Claude's ●). Rows above it are
// already in the transcript.
bool row_is_committed_bullet(const VtRow& r);

// A status / spinner line judged by its text ("esc to interrupt", a trailing
// "(12s)" clock, "N startup issues"), so it is filtered whatever glyph it
// leads with.
bool row_is_status(const VtRow& r);

// Claude's current working footer, excluding ordinary terminal redraws,
// completion summaries and old status lines in scrollback.
bool screen_shows_claude_activity(const Vt& vt);
// The text of Claude's live spinner row ("✻ Compacting conversation… (12s)"),
// or empty when it shows none. What Claude says it is doing, in its words.
std::string claude_activity_line(const Vt& vt);

// Codex's "Working (12s • esc to interrupt)" line, just above its input box.
// Only there: the conversation higher up can quote the same words.
bool screen_shows_codex_activity(const Vt& vt);
// Codex's dialogs (a command to approve, the folder trust question) take the
// place of its input box and lead the focused choice with its "›" and a
// number. The input box leads with the same glyph and no number.
bool screen_awaits_codex_input(const Vt& vt);

// True when the emulator screen is showing an interactive prompt that is
// waiting for a keypress — a trust dialog, a yes/no question, a selection
// menu. Used to pull a fresh session into raw view so the prompt can be
// answered.
bool screen_awaits_input(const Vt& vt);

// Claude's permission dialog, as the screen shows it: what a tool wants to do
// ("Bash command", "touch x"), the question ("Do you want to proceed?"), and
// the numbered choices. Read off the screen rather than predicted, because
// the choices differ by tool and by what can be remembered ("always allow
// access to <dir>", "switch to accept edits"), and change between releases.
struct PermissionPrompt {
  std::vector<std::string> title;    // the dialog's heading rows, at most a few
  std::string question;
  std::vector<std::string> options;  // labels, "N. " removed, wrapped rows joined
  std::vector<std::string> details;  // a description beside each label, when shown
  std::vector<bool> disabled;        // choices on screen that cannot be picked
  int cursor = 0;                    // the option claude's ❯ is on
  // "Tab to amend": Tab on the first choice ("Yes, and …") or on the last
  // "No" ("No, and tell Claude what to do differently") opens a line of text
  // for claude, sent with the answer.
  bool amend = false;
  // Codex's "Implement this plan?" at the end of a plan, which takes the same
  // panel: a choice of how to go on rather than a permission.
  bool plan = false;
};
bool parse_permission_prompt(const Vt& vt, PermissionPrompt& out);
// Codex's approval dialog in the same terms: "Would you like to run the
// following command?" as the question, what it is about (the reason, "$ cmd")
// as the heading, and the choices without the key each answers to ("(y)").
// Codex offers no Tab to amend: a note goes as a message after the answer.
// Also its "Implement this plan?" (plan = true), each choice with a
// description beside it and possibly disabled.
bool parse_codex_permission_prompt(const Vt& vt, PermissionPrompt& out);

// True when the screen is Claude's first-run "do you trust this folder"
// dialog specifically — a prompt mico can answer on the user's behalf, since
// they explicitly added the folder to the tracked list.
bool screen_is_trust_prompt(const Vt& vt);
// Down-arrow presses (negative: up) from the dialog's cursor to its "Yes, I
// trust" choice, or kNoTrustMove when either cannot be found on screen.
constexpr int kNoTrustMove = -1000;
int trust_prompt_moves(const Vt& vt);

// Picks up to `max` rows of work-in-progress, oldest first, for splicing under
// a chat. Every row of the agent's own input box and status line is dropped,
// individually — an agent that renders inline can briefly hold two copies of
// its box mid-repaint, so cutting at the first border it finds is not enough.
//
// `agent` names the agent whose screen this is. pi and omp render a complete
// chat of their own, so their views are transcript-only and this returns
// nothing: splicing their tail would duplicate mico's own rendering with the
// agent's. Claude and Codex keep the generic bottom-up scan.
void live_rows(const Vt& vt, std::vector<int>& out, int max, std::string_view agent = {});

// Claude's side-question panel (/btw), read off its screen. The panel takes
// the input box's place until Esc, and nothing of it reaches the transcript:
// the screen is the only place the answer exists. It lists every side
// question of the session, the one whose answer shows in bold; the answer is
// the part the panel shows, which for a long one is a window it scrolls.
struct BtwPanel {
  std::vector<std::string> questions;  // oldest first, "/btw " removed
  int current = -1;                    // the question the answer is for
  std::string answer;                  // markdown; empty while answering
  bool answering = false;
  std::string hint;                    // claude's key line, as drawn
};
bool parse_btw_panel(const Vt& vt, BtwPanel& out);

// The reply the agent is writing, read off its screen, as markdown: its last
// text block after the user's latest prompt, with the lines it wrapped joined
// again and its bold and italic marked. Transcripts get a block only once it
// is complete, seconds after the screen shows it being written, so this is
// what a chat can draw meanwhile. Claude and Codex only; empty when there is
// none.
std::string screen_reply(const Vt& vt, std::string_view agent);

}  // namespace mico
