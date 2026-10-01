#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "adapters/adapter.h"
#include "base/front_vec.h"
#include "base/jsonl.h"

namespace mico {

// A question an agent asks with a multiple-choice tool, parsed from a Question
// event's `questions` JSON.
struct QuestionOption {
  std::string label, description;
};
struct QuestionSpec {
  std::string header, text;
  bool multi = false;
  int recommended = 0;
  std::vector<QuestionOption> options;
};
struct QuestionCard {
  uint64_t tool_id = 0;
  std::string tool;
  std::vector<QuestionSpec> questions;
  // Optional (codex's request_user_input_async): answered with a message,
  // not through the agent's menu, and offers a reply in the user's own words.
  bool async = false;
  std::string call_id;  // an optional question's, which its answer names
};
void parse_question_card(std::string_view json, QuestionCard& card);

// A landmark of a transcript: one of your messages, a file the agent edited, a
// tool call that failed, a question, a notice such as a compaction.
struct OutlineEntry {
  uint64_t offset = 0;  // byte offset of its line
  char kind = 'u';      // 'u' you, 'e' edit, 'x' failed, 'q' question, 'n' notice
  std::string label;
  std::string detail;
};

// One chat, read through its agent's adapter: a lazy window of events over a
// transcript that can be hundreds of megabytes, and what those events say
// about where the conversation stands — the questions waiting on an answer,
// the tool calls still running, the messages the agent is holding. What any
// front end needs to show a chat, and nothing about how it is shown.
//
// The window is read forwards as the file grows and backwards on request, and
// can be dropped and started again anywhere. Nothing in it owns text: events
// index into `arena()`.
class Conversation {
 public:
  Conversation() = default;
  Conversation(const Conversation&) = delete;
  Conversation& operator=(const Conversation&) = delete;

  // Opens `path`, read through `adapter`, with an empty window at its end.
  bool open(const std::string& path, const Adapter* adapter);
  // Forgets the file's contents: window, facts, outline and session state.
  void clear();
  bool is_open() const { return file_.is_open(); }
  const std::string& path() const { return path_; }
  const Adapter* adapter() const { return adapter_; }

  Jsonl& file() { return file_; }
  const Jsonl& file() const { return file_; }
  Arena& arena() { return arena_; }
  const Arena& arena() const { return arena_; }
  FrontVec<Event>& events() { return events_; }
  const FrontVec<Event>& events() const { return events_; }
  // Lines [parsed_from, parsed_to) of the file are in the window.
  size_t parsed_from() const { return parsed_from_; }
  size_t parsed_to() const { return parsed_to_; }
  // True when the window reaches the end of the file as last indexed: what
  // the facts below say is about now.
  bool at_tail() const { return parsed_to_ == file_.line_count(); }
  // What the transcript says about the session itself: model, effort, mode.
  // Describes the session, not the window, so it survives a reanchor.
  SessionState& state() { return state_; }
  const SessionState& state() const { return state_; }

  // Called whenever older history is indexed in front of the window: every
  // line number held anywhere moves up by the count. The conversation's own
  // (its events, its window, its state) have moved already.
  std::function<void(size_t)> on_shift;

  // --- Reading -------------------------------------------------------------

  // True when the file grew since the last look.
  bool refresh() { return file_.refresh(); }
  // Parses forward, at most `max_lines`: live tailing wants everything that
  // arrived, filling a screen after a seek a chunk, not the rest of a
  // three-hundred-megabyte file. Returns the events appended.
  size_t grow_forwards(size_t max_lines = size_t(-1));
  // Parses up to `max_lines` older lines in front of the window, indexing an
  // older slab of the file first when the window starts at the index's start.
  // Returns the events prepended; `mark` gets the arena size before them, so
  // the caller can drop text it will not show. The facts are not brought up
  // to date until replay_facts(): the caller may strip events first.
  size_t grow_backwards(size_t max_lines, size_t* mark = nullptr);
  // Works the facts out again from the whole window, in transcript order: a
  // result already in the newer window resolves its newly loaded older call.
  void replay_facts();
  // Indexes the file back to `byte` (0: the whole file).
  void index_back_to(size_t byte);
  // Drops the window and starts again, empty, at line `line`. The session
  // state and the outline are kept: they describe the file.
  void reanchor(size_t line);

  // --- Where the conversation stands ------------------------------------------

  // Messages the agent is holding: sent while it worked, not yet handed to the
  // model. Oldest first.
  const std::vector<Str>& agent_queue() const { return agent_queue_; }
  // Blocking questions with no answer yet, oldest first: the agent is stopped
  // on the newest.
  const std::vector<uint64_t>& pending_questions() const { return pending_; }
  // Every question in the window.
  const std::vector<uint64_t>& questions() const { return questions_all_; }
  // Tool calls with no result yet.
  const std::vector<uint64_t>& pending_tools() const { return pending_tools_; }
  // Calls of mico's plot tool: their results say nothing worth showing.
  const std::vector<uint64_t>& chart_tools() const { return chart_tools_; }
  bool tool_pending(uint64_t id) const;

  // Optional questions in the window, oldest first. One is Open until a user
  // message quotes it (Answered) or the user sends anything else (Skipped).
  enum class AsyncStatus : uint8_t { Open, Answered, Skipped };
  struct AsyncSlot {
    uint64_t id;
    Str title;   // the first question, as quoted back by the reply
    Str answer;  // the reply below the quote, once Answered
    AsyncStatus status;
  };
  const std::vector<AsyncSlot>& async_questions() const { return async_; }
  const AsyncSlot* async_slot(uint64_t id) const;
  int open_async_questions() const;
  // True while question `id` still wants an answer: blocking and pending, or
  // optional and Open.
  bool question_open(uint64_t id) const;
  // A blocking question whose result is in: the answer to each of its
  // questions, read out of the result text. False while there is no result,
  // when it failed, or when any answer cannot be found.
  bool question_answers(uint64_t id, std::vector<std::string>& out) const;
  // Moves whenever a question's state changed in a way a card already shown
  // has to follow: a newer optional question took the keys from an older one,
  // one was answered or skipped, a blocking one got its result.
  uint64_t cards_gen() const { return cards_gen_; }

  // --- Outline ------------------------------------------------------------

  // The landmarks of the whole transcript, oldest first. Read from the file,
  // not from the window, and kept: each call reads only what is new since the
  // last, then goes on backwards into older history for at most `budget_ms`.
  // False when older history is still unread.
  bool outline(std::vector<OutlineEntry>& out, int budget_ms);
  // The full text of the user message whose line starts at byte `offset`.
  // False when the line holds no user message.
  bool user_text_at(uint64_t offset, std::string* out);

 private:
  void parse_lines(size_t from, size_t to);
  void add_images(std::string_view line, size_t before);
  void note_event(const Event& e);
  void clear_facts();
  void shift_lines(size_t count);
  void outline_scan(size_t a, size_t b, std::vector<OutlineEntry>& into);

  Jsonl file_;
  const Adapter* adapter_ = nullptr;
  std::string path_;
  Arena arena_;
  FrontVec<Event> events_;
  std::vector<Event> batch_;  // reused parse output
  size_t parsed_from_ = 0;
  size_t parsed_to_ = 0;
  SessionState state_;

  std::vector<Str> agent_queue_;
  std::vector<uint64_t> pending_;
  std::vector<uint64_t> questions_all_;
  std::vector<uint64_t> pending_tools_;
  std::vector<uint64_t> chart_tools_;
  std::vector<AsyncSlot> async_;
  uint64_t cards_gen_ = 0;

  std::vector<OutlineEntry> outline_;
  uint64_t outline_lo_ = UINT64_MAX;  // first byte read; 0 once the start is reached
  uint64_t outline_hi_ = 0;           // end of the last line read
  // Calls seen, by id: a failed result names the call it failed. Bounded.
  std::unordered_map<uint64_t, std::string> outline_calls_;
  // Failed results whose call is in history not read yet: the offset of
  // their entry, filled in when the call turns up.
  std::unordered_map<uint64_t, uint64_t> outline_orphans_;
};

}  // namespace mico
