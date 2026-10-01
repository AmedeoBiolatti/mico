#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "model/event.h"
#include "model/launch.h"
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
// question. Claude's multi-select Enter toggles the current option: its
// separate Next/Submit button comes after the options and the Other field.
// Keep key events separate so a React form can render between state changes.
inline std::vector<std::string> question_answer_steps(
    std::string_view agent, const std::vector<bool>& multi,
    const std::vector<int>& recommended, const std::vector<int>& options,
    const std::vector<std::vector<uint8_t>>& chosen) {
  const size_t nq = std::min({multi.size(), recommended.size(), options.size(), chosen.size()});
  std::vector<std::string> keys;
  for (size_t qi = 0; qi < nq; qi++) {
    int cur = agent == "claude" ? 0 : std::clamp(recommended[qi], 0, std::max(0, options[qi] - 1));
    auto move_to = [&](int to) {
      while (cur < to) { keys.emplace_back("\x1b[B"); cur++; }
      while (cur > to) { keys.emplace_back("\x1b[A"); cur--; }
    };
    if (multi[qi]) {
      for (int i = 0; i < options[qi] && i < int(chosen[qi].size()); i++)
        if (chosen[qi][size_t(i)]) { move_to(i); keys.emplace_back(" "); }
      if (agent == "claude") move_to(options[qi] + 1);
    } else {
      for (int i = 0; i < options[qi] && i < int(chosen[qi].size()); i++)
        if (chosen[qi][size_t(i)]) { move_to(i); break; }
    }
    keys.emplace_back("\r");  // confirm and advance
  }
  // Claude now submits a single single-select immediately. Sending a second
  // Enter there leaks into the next prompt. Multi-select still has review.
  if (nq > 1 || (agent == "claude" && nq == 1 && multi[0])) keys.emplace_back("\r");
  return keys;
}

inline std::string question_answer_keys(
    std::string_view agent, const std::vector<bool>& multi,
    const std::vector<int>& recommended, const std::vector<int>& options,
    const std::vector<std::vector<uint8_t>>& chosen) {
  std::string keys;
  for (const auto& step : question_answer_steps(agent, multi, recommended, options, chosen)) keys += step;
  return keys;
}

}  // namespace mico
