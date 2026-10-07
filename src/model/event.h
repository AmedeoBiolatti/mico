#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "base/json.h"

namespace mico {

// A slice of the arena. Events own no memory of their own: a transcript can
// yield hundreds of thousands of them, and four std::strings apiece meant four
// allocations apiece.
struct Str {
  uint32_t off = 0;
  uint32_t len = 0;
  bool empty() const { return len == 0; }
};

enum class EventKind : uint8_t {
  Meta,  // session/mode records; hidden by default
  User,
  Assistant,
  Thinking,
  ToolCall,
  ToolResult,
  // Agent-generated completion notice for a background command or task.
  TaskStatus,
  // A tool call that asks the user a multiple-choice question
  // (AskUserQuestion / request_user_input / ask). `detail` holds the raw
  // `questions` JSON array; the chat renders it as a card and, for a live
  // session, answers it by driving the agent's own menu.
  Question,
  TurnEnd,  // completion or cancellation; never rendered
  // A one-line marker for something that happened to the conversation rather
  // than in it ("Conversation compacted"). Shown at every density.
  Notice,
  // The agent took a message the user sent while it was working and is
  // holding it (QueueAdd, `text` = the message), or has now handed it to the
  // model (QueueTake, `text` = which one, empty for the oldest). Never drawn
  // as rows: the pane lists what is still held, above the prompt.
  QueueAdd,
  QueueTake,
  // A chart the agent drew with mico's plot tool. `text` is a ```chart block
  // holding the call's arguments, so it renders as any chart in a message.
  Chart,
  // An image in the transcript (a screenshot, an image the agent read, one
  // the user pasted). Its pixels stay in the transcript: `summary` holds which
  // image of its line it is, and the chat reads them from the line when it
  // draws it. A tool's image carries the tool's id.
  Image,
  // A message between agents: one a subagent's session sent it, or one it
  // sent its session. `name` is who sent it, `summary` who it went to when
  // the transcript says (empty: this chat's agent), `text` the message. Not a
  // turn of yours, and said in the open: shown at every density, never
  // folded away with the steps around it.
  Peer,
};

// One normalized turn element: 40 bytes, trivially copyable, no owned memory.
// Both agents' wire formats collapse onto this, and every view renders from it
// rather than from agent-specific JSON.
struct Event {
  Str text;      // message body / result payload
  Str name;      // tool name
  Str summary;   // one-line argument preview, e.g. the bash command
  Str detail;    // expanded payload: a diff for edit-shaped tool calls
  uint64_t tool_id = 0;  // hash of the call id; links a result to its call
  uint32_t src_line = 0;
  EventKind kind = EventKind::Meta;
  uint8_t ok = 1;
  // A step of a folded turn loaded without its text, which it gets back when
  // the fold is opened. Its detail keeps only whether there was one.
  uint8_t bare = 0;
};
static_assert(sizeof(Event) <= 48, "Event must stay small; it is stored by the million");
static_assert(std::is_trivially_copyable_v<Event>);

// Append-only backing store for every event's text. Grows monotonically, so
// offsets handed out stay valid no matter which end new events arrive at.
class Arena {
 public:
  std::string_view view(Str s) const { return {buf_.data() + s.off, s.len}; }

  Str add(std::string_view v) {
    Str s{uint32_t(buf_.size()), uint32_t(v.size())};
    buf_.append(v);
    return s;
  }

  // Copies a JSON string body, unescaping only when it actually needs it.
  // Most transcript strings are escape-free and become a plain memcpy.
  // Text beyond what a view could ever show is never worth copying: a chat
  // caps one message at a couple of hundred wrapped rows, and a tool output
  // can be a hundred kilobytes. Copying it costs the unescape pass, the
  // memcpy, and the memory to hold it, for bytes nothing will read.
  // A chat shows at most a couple of hundred wrapped rows of any one message,
  // so at a wide pane that is about 24 KB. Bytes past it cannot be reached by
  // any view, and copying them costs the unescape pass, the memcpy, and the
  // window budget that decides how often the parse window has to be thrown
  // away and rebuilt.
  static constexpr size_t kMaxText = 24u << 10;

  void put_json(const js::Value& v, size_t cap = kMaxText) {
    std::string_view body = v.body();
    if (body.size() > cap) body = body.substr(0, cap);
    if (v.literal()) buf_.append(body);
    else js::unescape_append(body, buf_);
  }
  Str add_json(const js::Value& v, size_t cap = kMaxText) {
    uint32_t off = uint32_t(buf_.size());
    put_json(v, cap);
    return Str{off, uint32_t(buf_.size() - off)};
  }
  // Bytes written so far into the run opened at `off`.
  size_t since(uint32_t off) const { return buf_.size() - off; }

  // Begin/end pair for building a value from several pieces without a
  // temporary string.
  uint32_t open() { return uint32_t(buf_.size()); }
  void put(std::string_view v) { buf_.append(v); }
  Str close(uint32_t off) { return Str{off, uint32_t(buf_.size() - off)}; }

  void clear() { buf_.clear(); }
  // Drops everything from byte `n` on: text laid out for one frame only.
  void truncate(size_t n) { if (n < buf_.size()) buf_.resize(n); }
  size_t bytes() const { return buf_.size(); }
  void reserve(size_t n) { buf_.reserve(n); }

 private:
  std::string buf_;
};

// FNV-1a. Tool call ids are only ever compared, never displayed, so they are
// hashed at parse time instead of being stored as strings.
inline uint64_t hash_id(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= uint8_t(c);
    h *= 1099511628211ull;
  }
  return h ? h : 1;
}

}  // namespace mico
