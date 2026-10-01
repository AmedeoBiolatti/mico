#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "core/event.h"
#include "core/state.h"

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


// Per-agent translation from a transcript line to normalized Events.
// Adapters are pure and stateless: enrichment only, never a gate. An agent with
// no adapter still gets a pane, it just has no chat view.
class Adapter {
 public:
  virtual ~Adapter() = default;
  virtual std::string_view id() const = 0;

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
};

const Adapter& claude_adapter();
const Adapter& codex_adapter();
const Adapter& pi_adapter();
const Adapter& omp_adapter();

// Codex's optional question: the call returns `{"accepted":true}` at once and
// the agent keeps working. The reply, whenever it comes, is an ordinary user
// message. Codex 0.159 writes it as an envelope naming the question by its
// call and index (see AsyncReply); older ones quoted it: "> question\n\nanswer".
inline bool is_async_question_tool(std::string_view n) { return n == "request_user_input_async"; }

// One answer in that envelope:
//   <send_user_message_question_reply>
//   [{"answer":…,"question":…,"questionItemId":"[\"request_user_input_async\",\"<call>\",0]"}]
//   </send_user_message_question_reply>
// Codex matches questionItemId byte for byte to clear its own pending
// question, so it is written exactly as Codex writes it.
struct AsyncReply {
  std::string call_id;
  int index = 0;  // which of the call's questions
  std::string question, answer;
};
std::string async_reply_envelope(const std::vector<AsyncReply>& replies);
// The replies in a message that is such an envelope; false for any other.
bool parse_async_reply(std::string_view text, std::vector<AsyncReply>& out);

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
