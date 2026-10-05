#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "model/background.h"
#include "model/changes.h"
#include "model/commands.h"
#include "model/event.h"
#include "model/launch.h"
#include "model/session_ref.h"
#include "model/tools.h"
#include "model/usage.h"
#include "model/state.h"

namespace mico {

// mico's own plot tool, as each agent names an MCP server's tool: claude
// "mcp__mico__plot", codex "mico.plot" or "mcp__mico__plot".
inline bool is_mico_plot(std::string_view name) {
  return name == "mcp__mico__plot" || name == "mico__plot" || name == "mico.plot";
}
// Its call as an event: the arguments, as a ```chart block to draw.
inline void make_chart_event(Event& e, Arena& arena, std::string_view args_json) {
  e.kind = EventKind::Chart;
  const uint32_t at = arena.open();
  arena.put("```chart\n");
  arena.put(args_json);
  arena.put("\n```");
  e.text = arena.close(at);
}


class Vt;
class LineReader;
struct PermissionPrompt;
struct BtwPanel;

// How an agent's multiple-choice menu answers to keys.
struct MenuKeys {
  // The cursor starts on the recommended option rather than the first.
  bool starts_at_recommended = true;
  // A multi-select ends in Next/Submit buttons, below the options and the
  // Other field, and has a review screen even when it is the only question.
  bool multi_buttons = false;
  // Each key waits until the menu has redrawn from the one before: a React
  // form drops keys that arrive while it renders.
  bool paced = false;
};

// What is known when looking for a running session's transcript.
struct TranscriptQuery {
  std::string session_id;  // known up front, or empty
  std::string cwd;
  std::string origin;      // the session a fork came from, or the one resumed
  bool forked = false;
  int64_t started_at = 0;  // unix seconds
  int pid = -1;            // the agent's pty session leader
  // What snapshot_transcripts() saw before the launch, sorted.
  const std::vector<std::string>* preexisting = nullptr;
  // True for a transcript another running session has already claimed.
  std::function<bool(const std::string&)> claimed;
  bool resuming() const { return !session_id.empty() && !forked; }
  bool existed(const std::string& path) const {
    return preexisting && std::binary_search(preexisting->begin(), preexisting->end(), path);
  }
};

struct FoundTranscript {
  std::string path;
  std::string session_id;  // as the transcript names it
};

// What there is to judge a running agent's activity by.
struct Liveness {
  const Vt& vt;
  int64_t quiet_ms = 0;    // since the agent last wrote to its terminal
  bool turn_open = false;  // see Adapter::tracks_turns()
};

// The answer to one question an agent asked without waiting for it (codex's
// request_user_input_async): it goes back as an ordinary message.
struct AsyncReply {
  std::string call_id;
  int index = 0;  // which of the call's questions
  std::string question, answer;
};

// Everything mico knows about one kind of agent: how to start it, where it
// keeps its transcripts, how to read them, and how to tell from its screen
// what it is doing. The rest of mico asks its adapter rather than checking
// which agent it is, so a new agent is a new adapter and nothing else.
//
// Adapters are stateless: one instance serves every session of its agent, and
// everything per-session is passed in. An agent with no adapter still gets a
// pane, it just has no chat view.
class Adapter {
 public:
  virtual ~Adapter() = default;

  // --- Identity ------------------------------------------------------------

  // The agent as its command is named: "claude", "codex", "pi", "omp".
  virtual std::string_view id() const = 0;
  // As menus name it: "Claude Code", "Oh My Pi".
  virtual std::string_view name() const { return id(); }
  // As a title or a chip names it, short: "Claude", "OMP".
  virtual std::string_view label() const { return id(); }

  // --- Transcript ----------------------------------------------------------

  // Appends 0..N events for one line. A single assistant record routinely holds
  // thinking + prose + several tool calls, so this is not one-to-one. Lines
  // carrying nothing renderable (token counts, world state, event mirrors)
  // append nothing.
  virtual void parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const = 0;

  // Reads whatever the line says about the session itself — model, effort,
  // mode. Separate from parse() because these records carry no conversation
  // and would otherwise be rejected before anyone looked at them.
  virtual void observe(std::string_view raw, SessionState& st) const {}

  // Fills `st` with the fields this agent reports, values left empty. Lets the
  // chip bar appear before the first turn, so model/effort can be set on a
  // fresh session the way omp's harness does it.
  virtual void seed_state(SessionState& st) const {}

  // --- Tool runs -----------------------------------------------------------

  // Reports the tool calls and results one transcript line records, the line
  // starting at byte `offset`. The activity index times and classifies them.
  virtual void read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const {}

  // What one transcript line, starting at byte `offset`, says of work the
  // agent runs in the background: a call that may start or stop some, its
  // result, a notice of an event or an end. A live session feeds it the lines
  // its agent writes, and shows what is still running.
  virtual void read_background(std::string_view raw, uint64_t offset, BackgroundTasks& t) const {}

  // --- Usage ---------------------------------------------------------------

  // Adds the tokens (and, where the agent writes them, the dollars and the
  // rate limit) a transcript records to `e`. An agent that resumes_usage()
  // reads only from `r.offset` on and advances it; others read it all.
  virtual void read_usage(LineReader& j, UsageEntry& e, UsageResume& r) const {}
  // Whether read_usage() can pick up where an earlier read of a growing
  // transcript stopped, so a running session costs only its new lines.
  virtual bool resumes_usage() const { return false; }
  // Whether the agent's dollars come from a PriceBook fitted to the price
  // samples it records, rather than from the transcript itself.
  virtual bool prices_from_samples() const { return false; }
  // The account's usage limits as the agent last reported them outside its
  // transcripts (claude, through the status line mico gives it). An agent
  // that writes them into its transcripts reports them through read_usage().
  virtual void plan_limits(std::vector<PlanLimit>& out) const {}

  // --- File changes --------------------------------------------------------

  // A cheap test, before any parsing: false when `raw` cannot hold a change.
  virtual bool may_have_changes(std::string_view raw) const { return false; }
  // The file changes one transcript line records, a group per call. `cwd`
  // resolves the relative paths some agents write; `text` false counts lines
  // without keeping them, which is what an index over every chat wants.
  virtual void read_changes(std::string_view raw, std::string_view cwd, bool text,
                            std::vector<LineChanges>& out) const {}

  // --- Stored sessions -----------------------------------------------------

  // Every session the agent has stored, read off the head of each transcript:
  // what the chat list shows. Read-only: mico never writes into an agent's
  // store.
  virtual void list_sessions(const std::function<void(SessionRef&&)>& add) const {}

  // Tells the agent that `path`, a folder the user just tracked, is trusted,
  // so it does not ask. The one place mico writes into an agent's own config,
  // and only for an agent that would otherwise stop at a dialog there.
  virtual void trust_folder(const std::string& path) const {}

  // --- Launching -----------------------------------------------------------

  // Settles `l.argv`: the agent's own command when it is empty, the session id
  // when the agent can be told one (which makes finding its transcript exact),
  // and whatever of `x` the agent can take. Never replaces what the command
  // line already says.
  virtual void prepare(Launch& l, const LaunchExtras& x) const;

  // Fills `l` to continue session `id`: resuming it, or with `fork`, branching
  // it into a new one. False when the agent can do neither. An agent that can
  // resume but not fork resumes, clears `l.forked` and says so in `note`.
  virtual bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const {
    return false;
  }

  // --- Finding a running session's transcript ------------------------------

  // The transcripts that exist before a launch, for an agent whose new one can
  // only be told apart from them by being new. Sorted by the caller.
  virtual void snapshot_transcripts(std::vector<std::string>& out) const {}

  // Looks once for the transcript of the session `q` describes. Called every
  // half second until it succeeds; false while there is none yet.
  virtual bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const { return false; }

  // --- Watching a running session ------------------------------------------

  // True while the agent is working.
  virtual bool busy(const Liveness& l) const;
  // True while the agent shows a prompt that wants a keypress: a trust dialog,
  // a y/n. Not gated on busy(): a dialog with a countdown keeps redrawing.
  virtual bool awaits_input(const Vt& vt) const;

  // Whether busy() wants Liveness::turn_open, read from the transcript as it
  // grows. Most agents' screens are enough; reading costs a stat each poll.
  virtual bool tracks_turns() const { return false; }
  // +1 for a record that opens a turn, -1 for one that closes it, else 0,
  // judged from the record's first 200 bytes.
  virtual int turn_marker(std::string_view head) const { return 0; }

  // A dialog the agent opens at startup that mico answers for the user (the
  // folder is on their tracked list, which is trust enough): the next key to
  // send, or empty when the screen shows none. `confirms` is set when that key
  // accepts it. Sent one at a time, each after the screen has settled.
  virtual std::string startup_answer(const Vt& vt, bool* confirms) const { return {}; }
  // True while the screen shows such a dialog, answerable or not yet.
  virtual bool startup_prompt(const Vt& vt) const { return false; }

  // --- Reading a running session's screen ----------------------------------

  // The permission dialog the agent shows now, read off its screen. False when
  // it shows none.
  virtual bool permission_prompt(const Vt& vt, PermissionPrompt& out) const { return false; }
  // A panel whose content never reaches the transcript (claude's /btw), so the
  // screen is the only place to read it. False when none is open.
  virtual bool side_panel(const Vt& vt, BtwPanel& out) const { return false; }
  // True while the agent compacts its context: a wait of its own, which can
  // run for minutes.
  virtual bool compacting(const Vt& vt) const { return false; }
  // Up to `max` rows of work in progress to splice under the chat; see
  // mico::live_rows(), which is the default.
  virtual void live_rows(const Vt& vt, std::vector<int>& out, int max) const;
  // The reply the agent is writing, read off its screen as markdown, before
  // its transcript has it. Empty when there is none or it cannot be read.
  virtual std::string screen_reply(const Vt& vt) const { return {}; }

  // --- Commands, models and settings -----------------------------------------

  // The commands built into the agent, as its own "/" menu lists them.
  virtual std::vector<SlashCommand> builtin_commands() const { return {}; }
  // Commands the user or the project defined in files (prompt templates,
  // skills). `home` is passed so tests can point it elsewhere.
  virtual void file_commands(const std::string& cwd, const std::string& home,
                             std::vector<SlashCommand>& out) const {}

  // A command line that answers command_probe_request(), written to its
  // stdin, with what the agent offers now: commands, and perhaps models.
  // Empty when the agent cannot be asked.
  virtual std::vector<std::string> command_probe_argv() const { return {}; }
  virtual std::string command_probe_request() const { return {}; }
  // Reads the probe's answer out of what it has written so far. False until
  // a complete one is there.
  virtual bool read_command_probe(std::string_view output, CommandProbeAnswer& out) const { return false; }

  // Keys that open the agent's model picker in a fresh session, for an agent
  // that has no other way to say which models it offers; empty for none.
  virtual std::string model_picker_command() const { return {}; }
  // The models that picker shows, read off the screen.
  virtual std::vector<ModelOption> read_model_picker(const Vt& vt) const { return {}; }

  // How mico sets the state field `key` (see seed_state()).
  virtual ChipControl chip_control(std::string_view key) const;

  // --- Answering ------------------------------------------------------------

  // How the agent's question menus take keys.
  virtual MenuKeys menu_keys() const { return {}; }
  // The message that answers questions the agent asked without waiting. By
  // default each question quoted, then its answer.
  virtual std::string async_reply(const std::vector<AsyncReply>& replies) const;
};

// Codex's optional question: the call returns `{"accepted":true}` at once and
// the agent keeps working. The reply, whenever it comes, is an ordinary user
// message. Codex 0.159 writes it as an envelope naming the question by its
// call and index (see AsyncReply); older ones quoted it: "> question\n\nanswer".
inline bool is_async_question_tool(std::string_view n) { return n == "request_user_input_async"; }

// True for the tool names that ask the user a multiple-choice question.
inline bool is_question_tool(std::string_view n) {
  return n == "AskUserQuestion" || n == "request_user_input" || n == "ask" ||
         is_async_question_tool(n);
}

// Fills a Question event from a tool call's input. `input` is either the
// arguments object or a JSON string holding one (codex encodes its arguments as
// a string). The raw `questions` array goes into `detail` for the card renderer
// to re-scan, and the first question's text into `text` so the collapsed line
// says something useful. Returns false when there is no usable array, leaving
// the caller to fall back to a normal ToolCall.
inline bool build_question(Event& e, Arena& arena, const js::Value& input) {
  thread_local std::string buf;
  std::string_view src;
  if (input.is_object()) {
    src = input.raw;
  } else if (input.is_string()) {
    buf.clear();
    js::unescape_append(input.body(), buf);
    src = buf;
  } else {
    return false;
  }

  js::Value questions{}, first{};
  js::scan_object(src, [&](std::string_view k, const js::Value& v) {
    if (k != "questions") return true;
    questions = v;
    return false;
  });
  if (!questions.is_array()) return false;

  js::scan_array(questions.raw, [&](const js::Value& q) {
    if (!q.is_object()) return true;
    js::scan_object(q.raw, [&](std::string_view k, const js::Value& v) {
      // The async form calls it a title.
      if ((k != "question" && k != "title") || !v.is_string()) return true;
      first = v;
      return false;
    });
    return false;  // only the first question is needed for the preview line
  });

  e.kind = EventKind::Question;
  if (first.is_string()) e.text = arena.add_json(first);
  e.detail = arena.add(questions.raw);
  return true;
}

// Keystrokes that answer a question card by driving the agent's own menu:
// for each question, walk its cursor to the chosen option (Space-toggling each
// one for a multi-select) and confirm with Enter, which advances to the next
// question. Keep key events separate so a paced menu can render between them.
std::vector<std::string> question_answer_steps(
    const Adapter& agent, const std::vector<bool>& multi,
    const std::vector<int>& recommended, const std::vector<int>& options,
    const std::vector<std::vector<uint8_t>>& chosen);

std::string question_answer_keys(
    const Adapter& agent, const std::vector<bool>& multi,
    const std::vector<int>& recommended, const std::vector<int>& options,
    const std::vector<std::vector<uint8_t>>& chosen);

}  // namespace mico
