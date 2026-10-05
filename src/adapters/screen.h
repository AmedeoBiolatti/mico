#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "vt/vt.h"

// Reading an agent's terminal screen: what agents' TUIs have in common. Each
// adapter adds what is particular to its own agent's screen.
namespace mico {

// True when a row carries nothing.
bool row_is_blank(const VtRow& r);
// True when a row is only the application's own furniture — box borders,
// rules, block shading — and no actual text. The bottom of an agent's screen
// is its input box, and splicing those borders into a chat view just reads as
// stray horizontal lines.
bool row_is_chrome(const VtRow& r);

// A row led by a completed-message bullet (Claude's ●). Rows above it are
// already in the transcript.
bool row_is_committed_bullet(const VtRow& r);

// A status / spinner line judged by its text ("esc to interrupt", a trailing
// "(12s)" clock, "N startup issues"), so it is filtered whatever glyph it
// leads with.
bool row_is_status(const VtRow& r);



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


// Picks up to `max` rows of work-in-progress, oldest first, for splicing under
// a chat. Every row of the agent's own input box and status line is dropped,
// individually — an agent that renders inline can briefly hold two copies of
// its box mid-repaint, so cutting at the first border it finds is not enough.
void live_rows(const Vt& vt, std::vector<int>& out, int max);

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

// The reply the agent is writing, read off its screen, as markdown: its last
// text block after the user's latest prompt, with the lines it wrapped joined
// again and its bold and italic marked. Transcripts get a block only once it
// is complete, seconds after the screen shows it being written, so this is
// what a chat can draw meanwhile. Claude and Codex only; empty when there is
// none.
enum class ReplyLayout {
  Claude,  // input box under a rule, a reply led by a bullet
  Codex,   // input line at the bottom, tool output in "└" cells
};
std::string screen_reply(const Vt& vt, ReplyLayout layout);

// Leading spaces before the first visible glyph; -1 for a blank row.
int row_indent(const VtRow& r);
// The first visible glyph of a row, or 0 for a blank row.
char32_t row_lead(const VtRow& r);
// A row as UTF-8 with the trailing blanks dropped.
std::string row_text(const VtRow& r);
std::string_view trim_left(std::string_view s);

// Upwards from a dialog's footer: its choices, each "N. label" (the focused
// one led by `cursor`) and the deeper-indented rows a long label wraps onto.
// Returns the first row above the choices, or -1 when they are not one
// numbered menu of two or more; `focus` is the choice the cursor is on.
int read_choices(const Vt& vt, int footer, std::string_view cursor, std::vector<std::string>& options,
                 int& focus);

// Rows [start, end) from column `from` as markdown, wrapped lines joined when
// they ran to `wrap_at`, bold and italic marked from the cells' attributes.
// Columns from `to` on are not read (-1: to the end of each row).
std::string rows_markdown(const Vt& vt, int start, int end, int from, int wrap_at, int to = -1);

// Where a panel drawn beside the agent's conversation (Claude's file diff)
// starts in rows [start, end): the column of its left edge, a vertical rule
// down most of the rows (two at least) with the conversation's own text left
// of it on one or more. A table's rule is not one: its rows start with a rule
// of their own. Claude 2.1.287 and later draw no rule: the panel is a column
// in a background of its own beside every row, so its edge is where that
// background starts, at the same column on nearly every row. -1 when there is
// no such panel.
int side_panel_edge(const Vt& vt, int start, int end, int from);

}  // namespace mico
