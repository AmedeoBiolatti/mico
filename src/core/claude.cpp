#include <iterator>

#include "core/adapter.h"
#include "core/user_text.h"

namespace mico {
namespace {

// Joins a `content` field, which is either a string or an array of blocks, into
// one arena run. Only text-bearing blocks contribute.
Str add_content(Arena& arena, const js::Value& content) {
  if (content.is_string()) return arena.add_json(content);
  uint32_t off = arena.open();
  if (content.is_array()) {
    bool first = true;
    js::scan_array(content.raw, [&](const js::Value& b) {
      if (b.is_string()) {
        if (!first) arena.put("\n");
        arena.put_json(b);
        first = false;
        return true;
      }
      if (!b.is_object()) return true;
      js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
        if (k != "text" || !v.is_string()) return true;
        if (!first) arena.put("\n");
        arena.put_json(v);
        first = false;
        return false;
      });
      return arena.since(off) < Arena::kMaxText;
    });
  }
  return arena.close(off);
}

// Preferred argument to show for a tool call, in priority order.
Str tool_arg_summary(Arena& arena, const js::Value& input) {
  if (!input.is_object()) return {};
  static constexpr std::string_view kByName[] = {"command", "file_path", "pattern",
                                                 "description", "path", "prompt", "url"};
  js::Value best{};
  int best_rank = 99;
  js::Value fallback{};

  js::scan_object(input.raw, [&](std::string_view k, const js::Value& v) {
    if (!v.is_string()) return true;
    for (int i = 0; i < int(std::size(kByName)); i++) {
      if (k == kByName[i]) {
        if (i < best_rank) { best_rank = i; best = v; }
        return best_rank != 0;  // "command" wins outright; stop scanning
      }
    }
    if (fallback.type == js::Type::Null) fallback = v;
    return true;
  });
  const js::Value& pick = best_rank < 99 ? best : fallback;
  return pick.is_string() ? arena.add_json(pick) : Str{};
}

// Writes `text` into the arena with every line prefixed, so the chat can show
// an edit as a diff instead of as two opaque blobs.
void put_prefixed(Arena& a, std::string_view text, char prefix, size_t max_lines) {
  size_t pos = 0, n = 0;
  while (pos <= text.size() && n < max_lines) {
    size_t nl = text.find('\n', pos);
    size_t end = nl == std::string_view::npos ? text.size() : nl;
    a.put(std::string_view(&prefix, 1));
    a.put(text.substr(pos, end - pos));
    a.put("\n");
    n++;
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
}

// Builds the expanded payload for edit-shaped tools. Returns an empty slice for
// tools that have no natural diff, which leaves them showing their arguments.
Str build_detail(Arena& arena, std::string_view name, const js::Value& input) {
  if (!input.is_object()) return {};
  const bool is_edit = name == "Edit" || name == "NotebookEdit";
  const bool is_write = name == "Write";
  if (!is_edit && !is_write) return {};

  js::Value old_s{}, new_s{}, content{};
  js::scan_object(input.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "old_string") old_s = v;
    else if (k == "new_string") new_s = v;
    else if (k == "content") content = v;
    return true;
  });

  thread_local std::string scratch;
  auto unesc = [&](const js::Value& v) -> std::string_view {
    scratch.clear();
    if (v.is_string()) js::unescape_append(v.body(), scratch);
    return scratch;
  };

  constexpr size_t kMaxDiffLines = 400;
  uint32_t off = arena.open();
  if (is_write) {
    put_prefixed(arena, unesc(content), '+', kMaxDiffLines);
  } else {
    put_prefixed(arena, unesc(old_s), '-', kMaxDiffLines / 2);
    put_prefixed(arena, unesc(new_s), '+', kMaxDiffLines / 2);
  }
  return arena.close(off);
}

// Claude records slash-commands, their output, and injected reminders as user
// messages. They are not things the user said, and left alone they outnumber
// the real turns. Rewrites `text` in place and returns what it really is.
EventKind classify_user_text(Arena& arena, Str& text) {
  std::string_view v = arena.view(text);
  auto starts = [&](std::string_view m) {
    return v.size() >= m.size() && v.compare(0, m.size(), m) == 0;
  };

  // The output of a slash command is machinery.
  if (starts("<local-command-stdout>") || starts("<local-command-caveat>")) return EventKind::Meta;

  // A slash command the user ran (/model, /clear, …) is control, not
  // conversation. It is hidden entirely; its effect shows up as a state
  // change, not as a chat line.
  if (starts("<command-name>")) return EventKind::Meta;

  // Reminders are injected into otherwise real turns; strip them and keep the
  // rest. If nothing survives, the whole record was machinery.
  static constexpr std::string_view kOpen = "<system-reminder>";
  static constexpr std::string_view kClose = "</system-reminder>";
  if (v.find(kOpen) == std::string_view::npos) return EventKind::User;

  uint32_t off = arena.open();
  size_t pos = 0;
  while (pos < v.size()) {
    size_t a = v.find(kOpen, pos);
    if (a == std::string_view::npos) { arena.put(v.substr(pos)); break; }
    arena.put(v.substr(pos, a - pos));
    size_t b = v.find(kClose, a);
    if (b == std::string_view::npos) break;
    pos = b + kClose.size();
  }
  Str stripped = arena.close(off);

  std::string_view out = arena.view(stripped);
  size_t lo = 0, hi = out.size();
  while (lo < hi && (out[lo] == ' ' || out[lo] == '\n' || out[lo] == '\t')) lo++;
  while (hi > lo && (out[hi - 1] == ' ' || out[hi - 1] == '\n' || out[hi - 1] == '\t')) hi--;
  text = Str{stripped.off + uint32_t(lo), uint32_t(hi - lo)};
  return text.empty() ? EventKind::Meta : EventKind::User;
}

// Background commands are reported as synthetic user messages. Render their
// summary as task activity instead of attributing the XML envelope to the user.
bool task_notification(Arena& arena, Event& e) {
  auto source = arena.view(e.text);
  while (!source.empty() && (source.front() == ' ' || source.front() == '\n' ||
                            source.front() == '\r' || source.front() == '\t')) source.remove_prefix(1);
  constexpr std::string_view open = "<task-notification>", close = "</task-notification>";
  if (!source.starts_with(open)) return false;
  const size_t end = source.find(close, open.size());
  if (end == std::string_view::npos) return false;
  // Only a whole notification is synthetic. Quoted examples and messages
  // with additional human text retain their original content.
  if (source.substr(end + close.size()).find_first_not_of(" \t\r\n") != std::string_view::npos) return false;
  source = source.substr(open.size(), end - open.size());
  const auto field = [&](std::string_view name) {
    const std::string a = "<" + std::string(name) + ">";
    const std::string b = "</" + std::string(name) + ">";
    const size_t start = source.find(a);
    if (start == std::string_view::npos) return std::string_view{};
    const size_t finish = source.find(b, start + a.size());
    if (finish == std::string_view::npos) return std::string_view{};
    return source.substr(start + a.size(), finish - start - a.size());
  };
  // Any whole envelope naming a task is Claude's: completions, failures,
  // "stopped" orphans left by a previous session, and Monitor events (which
  // carry an <event> and no <status> at all).
  if (field("task-id").empty()) return false;
  const auto status = field("status");
  const auto id = field("tool-use-id");
  e.tool_id = id.empty() ? 0 : hash_id(id);
  e.ok = status.empty() || status == "completed";
  std::string summary(field("summary"));
  if (summary.empty()) summary = status.empty() ? "Background task update" : "Background task " + std::string(status);
  if (const auto event = field("event"); !event.empty()) summary += " \xE2\x80\x94 " + std::string(event);  // —
  // Decode XML's named entities in a single pass (never interpret markup in
  // the summary as another notification).
  std::string display;
  for (size_t i = 0; i < summary.size();) {
    bool decoded = false;
    for (const auto& [entity, value] : {std::pair{"&amp;", '&'}, {"&lt;", '<'},
                                      {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}}) {
      if (std::string_view(summary).substr(i).starts_with(entity)) {
        display += value;
        i += std::char_traits<char>::length(entity);
        decoded = true;
        break;
      }
    }
    if (!decoded) display += summary[i++];
  }
  e.text = arena.add(display);
  e.kind = EventKind::TaskStatus;
  return true;
}

class ClaudeAdapter final : public Adapter {
 public:
  std::string_view id() const override { return "claude"; }

  void seed_state(SessionState& st) const override {
    st.declare("model", "model");
    st.declare("effort", "effort");
    st.declare("mode", "mode");
    st.declare("perm", "permissions");
  }

  void observe(std::string_view raw, SessionState& st) const override {
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") {
        // Mode records lead with their type, so they resolve immediately.
        std::string_view t = v.body();
        return t == "mode" || t == "permission-mode" || t == "assistant";
      }
      if (k == "mode") st.set("mode", "mode", v.body());
      else if (k == "permissionMode") st.set("perm", "permissions", v.body());
      else if (k == "effort") st.set("effort", "effort", v.body());
      else if (k == "message") {
        js::scan_object(v.raw, [&](std::string_view mk, const js::Value& mv) {
          if (mk != "model") return true;
          st.set("model", "model", mv.body());
          return false;
        });
      }
      return true;
    });
  }

  void parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const override {
    // Metadata records (mode, permission-mode, ai-title, …) write "type" as
    // their first key, so they are rejected after one member. User and
    // assistant records lead with parentUuid, so their type is found later —
    // but skipping the message object is a depth scan, not a parse.
    js::Value message{};
    bool is_user = false, is_assistant = false, is_meta = false;
    bool is_system = false, compact_summary = false;
    bool is_queue = false, is_attachment = false;
    std::string_view queue_op;
    js::Value queue_text{}, attachment{};

    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      // An attachment record writes its object before its type; take it
      // whichever comes first and stop once both are in.
      if (k == "attachment") { attachment = v; return !is_attachment; }
      if (k == "type") {
        std::string_view t = v.body();
        is_user = t == "user";
        is_assistant = t == "assistant";
        is_system = t == "system";
        is_queue = t == "queue-operation";
        is_attachment = t == "attachment";
        if (is_attachment) return attachment.type == js::Type::Null;
        return is_user || is_assistant || is_system || is_queue;
      }
      if (is_queue) {
        if (k == "operation") { queue_op = v.body(); return true; }
        if (k == "content") { queue_text = v; return false; }
        return true;
      }
      if (is_system) {
        // Only a compaction boundary is of interest; its subtype follows type.
        if (k == "subtype") {
          if (v.body() == "compact_boundary") {
            Event e;
            e.kind = EventKind::Notice;
            e.text = arena.add("Conversation compacted");
            out.push_back(e);
          }
          return false;
        }
        return true;
      }
      if (k == "message") { message = v; return true; }
      if (k == "isMeta") { is_meta = v.is_true(); return true; }
      // The summary a compaction hands the model: pages of recap written for
      // it, not for the reader. The boundary's notice already marks the spot.
      if (k == "isCompactSummary") { compact_summary = v.is_true(); return !compact_summary; }
      return true;
    });

    if (is_attachment) {
      // Of attachments only one kind is conversation: a message the user sent
      // mid-turn, handed to the model at a tool boundary (steering).
      js::Value prompt{};
      bool queued = false;
      js::scan_object(attachment.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "type") { queued = v.body() == "queued_command"; return queued; }
        if (k == "prompt") { prompt = v; return false; }
        return true;
      });
      // Background-task notices ride the same queue; they are not the user.
      if (queued && prompt.is_string() && !prompt.body().starts_with("<")) {
        Event e;
        e.kind = EventKind::User;
        e.text = add_content(arena, prompt);
        if (!e.text.empty()) out.push_back(e);
      }
      return;
    }
    if (is_queue) {
      // What the user sent while the agent worked, until the agent takes it.
      const bool add = queue_op == "enqueue";
      if (!add && queue_op != "dequeue" && queue_op != "remove") return;
      if (queue_text.is_string() && queue_text.body().starts_with("<")) return;  // a task notice
      if (add && !queue_text.is_string()) return;
      Event e;
      e.kind = add ? EventKind::QueueAdd : EventKind::QueueTake;
      if (queue_text.is_string()) e.text = arena.add_json(queue_text);
      out.push_back(e);
      return;
    }
    if ((!is_user && !is_assistant) || !message.is_object() || compact_summary) return;

    js::Value content{};
    std::string_view stop_reason;
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "content") content = v;
      else if (k == "stop_reason") stop_reason = v.body();
      return true;
    });
    if (content.type == js::Type::Null) return;

    if (is_user) {
      // A user record carrying tool_result blocks is the tool's output, not a
      // human turn; it must not render as something the user said.
      bool had_result = false;
      if (content.is_array()) {
        js::scan_array(content.raw, [&](const js::Value& b) {
          if (!b.is_object()) return true;
          js::Value inner{};
          uint64_t id = 0;
          bool is_result = false, err = false;
          js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
            if (k == "type") is_result = v.body() == "tool_result";
            else if (k == "tool_use_id") id = hash_id(v.body());
            else if (k == "is_error") err = v.is_true();
            else if (k == "content") inner = v;
            return true;
          });
          if (!is_result) return true;
          Event e;
          e.kind = EventKind::ToolResult;
          e.tool_id = id;
          e.ok = !err;
          if (inner.type != js::Type::Null) e.text = add_content(arena, inner);
          out.push_back(e);
          had_result = true;
          return true;
        });
      }
      if (had_result) return;

      Event e;
      e.text = add_content(arena, content);
      if (e.text.empty()) return;
      e.kind = is_meta ? EventKind::Meta : classify_user_text(arena, e.text);
      if (e.kind == EventKind::User) task_notification(arena, e);
      if (e.kind == EventKind::User) e.text = unwrap_pasted_content(arena, e.text);
      if (!e.text.empty()) out.push_back(e);
      return;
    }

    // Assistant: one record holds thinking + prose + tool calls, in order.
    if (!content.is_array()) return;
    js::scan_array(content.raw, [&](const js::Value& b) {
      if (!b.is_object()) return true;
      js::Value text{}, thinking{}, name{}, id{}, input{};
      std::string_view bt;
      js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "type") bt = v.body();
        else if (k == "text") text = v;
        else if (k == "thinking") thinking = v;
        else if (k == "name") name = v;
        else if (k == "id") id = v;
        else if (k == "input") input = v;
        return true;
      });

      Event e;
      if (bt == "text" && text.is_string() && !text.body().empty()) {
        e.kind = EventKind::Assistant;
        e.text = arena.add_json(text);
      } else if (bt == "thinking" && thinking.is_string() && !thinking.body().empty()) {
        e.kind = EventKind::Thinking;
        e.text = arena.add_json(thinking);
      } else if (bt == "tool_use") {
        e.kind = EventKind::ToolCall;
        e.name = name.is_string() ? arena.add_json(name) : arena.add("?");
        e.tool_id = hash_id(id.body());
        if (is_mico_plot(arena.view(e.name)) && input.is_object()) {
          make_chart_event(e, arena, input.raw);
        } else if (!(is_question_tool(arena.view(e.name)) && build_question(e, arena, input))) {
          e.summary = tool_arg_summary(arena, input);
          e.detail = build_detail(arena, arena.view(e.name), input);
        }
      } else {
        return true;
      }
      out.push_back(e);
      return true;
    });
    if (stop_reason == "end_turn" || stop_reason == "stop_sequence") {
      Event end;
      end.kind = EventKind::TurnEnd;
      out.push_back(end);
    }
  }
};

const ClaudeAdapter g_claude;

}  // namespace

const Adapter& claude_adapter() { return g_claude; }

}  // namespace mico
