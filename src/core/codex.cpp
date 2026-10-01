#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "core/adapter.h"
#include "core/user_text.h"

namespace mico {
namespace {

Str add_content(Arena& arena, const js::Value& content) {
  if (content.is_string()) return arena.add_json(content);
  uint32_t off = arena.open();
  if (content.is_array()) {
    bool first = true;
    js::scan_array(content.raw, [&](const js::Value& b) {
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

// Codex replays the whole system preamble as user-role messages. They are
// machinery, not conversation, so they get demoted to Meta and hidden.
bool looks_like_preamble(std::string_view t) {
  static constexpr std::string_view kMarkers[] = {
      "# AGENTS.md instructions", "<skills_instructions>", "<user_instructions>",
      "<environment_context>",    "<recommended_plugins>", "# Instructions for"};
  for (auto m : kMarkers)
    if (t.size() >= m.size() && t.compare(0, m.size(), m) == 0) return true;
  return false;
}

// Codex's `exec` tool takes a JavaScript snippet, so the raw input reads as
// `text(await tools.exec_command({cmd:"ls", ...}))`. The command is the part a
// human wants. The text is already in the arena, so this narrows the slice
// rather than building a new string.
Str narrow_to_command(const Arena& arena, Str full) {
  std::string_view s = arena.view(full);
  static constexpr std::string_view kKey = "cmd:\"";
  size_t at = s.find(kKey);
  if (at == std::string_view::npos) return full;
  at += kKey.size();
  size_t end = at;
  while (end < s.size()) {
    if (s[end] == '\\' && end + 1 < s.size()) { end += 2; continue; }
    if (s[end] == '"') break;
    end++;
  }
  if (end <= at) return full;
  return Str{uint32_t(full.off + at), uint32_t(end - at)};
}

// Codex writes tool output as a *string* holding a Python-style repr of a list
// of {'type': 'input_text', 'text': '...'} dicts. Pull the text out so the chat
// shows the output instead of the wrapper. Detected on the still-escaped body,
// so ordinary output never pays for the check.
Str add_tool_output(Arena& arena, const js::Value& v) {
  std::string_view body = v.body();
  const bool py = v.is_string() && body.size() > 3 && body.compare(0, 3, "[{'") == 0 &&
                  body.find("'text':") != std::string_view::npos;
  if (!py) return add_content(arena, v);

  // Reused across calls: this fires on most tool outputs in a rollout, and a
  // fresh buffer each time would be an allocation per event.
  thread_local std::string scratch;
  scratch.clear();
  js::unescape_append(body.substr(0, std::min(body.size(), Arena::kMaxText)), scratch);

  uint32_t off = arena.open();
  static constexpr std::string_view kKey = "'text': '";
  size_t pos = 0;
  bool first = true;
  while ((pos = scratch.find(kKey, pos)) != std::string::npos) {
    pos += kKey.size();
    if (!first) arena.put("\n");
    first = false;
    size_t start = pos;
    while (pos < scratch.size()) {
      if (scratch[pos] == '\\' && pos + 1 < scratch.size()) {
        arena.put(std::string_view(scratch).substr(start, pos - start));
        char c = scratch[pos + 1];
        arena.put(c == 'n' ? "\n" : c == 't' ? "\t" : std::string_view(&c, 1));
        pos += 2;
        start = pos;
        continue;
      }
      if (scratch[pos] == '\'') break;
      pos++;
    }
    arena.put(std::string_view(scratch).substr(start, pos - start));
    if (pos < scratch.size()) pos++;
  }
  Str result = arena.close(off);
  return result.empty() ? add_content(arena, v) : result;
}

// codex applies edits through `apply_patch`, whose argument already *is* a
// patch. Narrow the slice to the patch body so the chat can show it as a diff.
Str narrow_to_patch(const Arena& arena, Str full) {
  std::string_view s = arena.view(full);
  size_t begin = s.find("*** Begin Patch");
  if (begin == std::string_view::npos) return {};
  size_t end = s.find("*** End Patch", begin);
  if (end == std::string_view::npos) end = s.size();
  else end += 13;
  return Str{uint32_t(full.off + begin), uint32_t(end - begin)};
}

class CodexAdapter final : public Adapter {
 public:
  std::string_view id() const override { return "codex"; }

  void seed_state(SessionState& st) const override {
    st.declare("model", "model");
    st.declare("effort", "effort");
    st.declare("approval", "approvals");
    st.declare("sandbox", "sandbox");
  }

  void observe(std::string_view raw, SessionState& st) const override {
    js::Value payload{};
    bool wanted = false;
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") {
        wanted = v.body() == "turn_context";
        return wanted;
      }
      if (k == "payload") { payload = v; return false; }
      return true;
    });
    if (!wanted || !payload.is_object()) return;

    js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "model") st.set("model", "model", v.body());
      else if (k == "effort") st.set("effort", "effort", v.body());
      else if (k == "approval_policy") st.set("approval", "approvals", v.body());
      else if (k == "personality") st.set("personality", "style", v.body());
      else if (k == "summary") st.set("summary", "summary", v.body());
      else if (k == "sandbox_policy") {
        js::scan_object(v.raw, [&](std::string_view sk, const js::Value& sv) {
          if (sk != "type") return true;
          st.set("sandbox", "sandbox", sv.body());
          return false;
        });
      }
      return true;
    });
  }

  void parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const override {
    // Codex writes "type" before "payload", so the 60% of a rollout that is
    // token counts, world state and event mirrors is rejected after three
    // members — the payload, which can be a hundred kilobytes, is never read.
    // The payload is not measured either: `payload` runs to the end of the
    // line, and each scan below stops at the payload's own closing brace. An
    // event_msg (over half of a rollout's bytes) is then settled by its
    // payload's first member instead of by skipping all of it.
    std::string_view payload;
    std::string_view type;

    js::scan_keys(raw, [&](std::string_view k, std::string_view rest) {
      if (k == "type") {
        type = js::string_body(rest);
        return type == "response_item" || type == "event_msg";
      }
      if (k == "payload") { payload = rest; return false; }
      return true;
    });
    if (type == "compacted") {
      // The replacement history it carries is for the model; the reader only
      // needs to know the conversation was compacted here.
      Event e;
      e.kind = EventKind::Notice;
      e.text = arena.add("Conversation compacted");
      out.push_back(e);
      return;
    }
    if (payload.empty() || payload.front() != '{') return;
    if (type == "event_msg") {
      bool item = false;
      js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
        if (k != "type") return true;
        if (v.body() == "task_complete" || v.body() == "turn_complete" ||
            v.body() == "turn_aborted") {
          Event e;
          e.kind = EventKind::TurnEnd;
          // Cancelled: whatever it said last was not its answer.
          e.ok = v.body() != "turn_aborted";
          out.push_back(e);
        }
        item = v.body() == "item_completed";
        return false;
      });
      // A finished MCP call, and mico's plot among them: drawn as its chart.
      // The item's type is its first member, so other items (command output
      // runs long) are turned away after one key.
      if (item) {
        std::string_view it;
        js::scan_keys(payload, [&](std::string_view k, std::string_view rest) {
          if (k == "item") { it = rest; return false; }
          return true;
        });
        bool mcp = false;
        js::scan_keys(it, [&](std::string_view k, std::string_view rest) {
          if (k == "type") mcp = js::string_body(rest) == "McpToolCall";
          return false;
        });
        if (!mcp) return;
        std::string_view server, tool, id;
        js::Value args{};
        js::scan_object(it, [&](std::string_view k, const js::Value& v) {
          if (k == "server") server = v.body();
          else if (k == "tool") tool = v.body();
          else if (k == "id") id = v.body();
          else if (k == "arguments") args = v;
          return true;
        });
        if (server == "mico" && tool == "plot" && args.is_object()) {
          Event e;
          e.tool_id = hash_id(id);
          make_chart_event(e, arena, args.raw);
          out.push_back(e);
        }
      }
      return;
    }
    if (type != "response_item") return;

    std::string_view pt, role, channel;
    js::Value content{}, summary{}, name{}, call_id{}, input{}, arguments{}, output{};
    js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
      if (k == "type") pt = v.body();
      else if (k == "role") role = v.body();
      else if (k == "channel") channel = v.body();
      else if (k == "content") content = v;
      else if (k == "summary") summary = v;
      else if (k == "name") name = v;
      else if (k == "call_id") call_id = v;
      else if (k == "input") input = v;
      else if (k == "arguments") arguments = v;
      else if (k == "output") output = v;
      return true;
    });

    Event e;
    if (pt == "message") {
      if (content.type == js::Type::Null) return;
      e.text = add_content(arena, content);
      if (e.text.empty()) return;
      if (role == "assistant") e.kind = EventKind::Assistant;
      else if (role == "user")
        e.kind = looks_like_preamble(arena.view(e.text)) ? EventKind::Meta : EventKind::User;
      else e.kind = EventKind::Meta;  // developer
      if (e.kind == EventKind::User) e.text = unwrap_pasted_content(arena, e.text);
      // An answer to an optional question reads the way Codex shows it, and
      // names its question by the call that asked it.
      std::vector<AsyncReply> replies;
      if (e.kind == EventKind::User && parse_async_reply(arena.view(e.text), replies)) {
        std::string shown;
        for (const auto& r : replies) {
          if (!shown.empty()) shown += "\n\n";
          shown += "> " + r.question + "\n\n" + r.answer;
        }
        e.text = arena.add(shown);
        e.tool_id = hash_id(replies[0].call_id);
      }
    } else if (pt == "reasoning") {
      // summary is usually empty (content is encrypted server-side); only the
      // rare summarized reasoning is renderable.
      if (summary.type == js::Type::Null) return;
      e.text = add_content(arena, summary);
      if (e.text.empty()) return;
      e.kind = EventKind::Thinking;
    } else if (pt == "custom_tool_call" || pt == "function_call") {
      e.kind = EventKind::ToolCall;
      e.name = name.is_string() ? arena.add_json(name) : arena.add("?");
      e.tool_id = hash_id(call_id.body());
      const js::Value& args = input.type != js::Type::Null ? input : arguments;
      if (is_mico_plot(arena.view(e.name)) && args.type != js::Type::Null) {
        // Arguments arrive as a JSON string in a function call.
        std::string json;
        if (args.is_string()) js::unescape_append(args.body(), json);
        else json = std::string(args.raw);
        make_chart_event(e, arena, json);
      } else if (is_question_tool(arena.view(e.name)) && build_question(e, arena, args)) {
        // A Question event, not a generic tool call. An optional one keeps its
        // call id, which its answer has to name.
        if (is_async_question_tool(arena.view(e.name))) e.summary = arena.add(call_id.body());
      } else if (args.type != js::Type::Null) {
        e.summary = add_content(arena, args);
        e.detail = narrow_to_patch(arena, e.summary);
        if (e.detail.empty() && arena.view(e.name) == "exec")
          e.summary = narrow_to_command(arena, e.summary);
      }
    } else if (pt == "custom_tool_call_output" || pt == "function_call_output") {
      e.kind = EventKind::ToolResult;
      e.tool_id = hash_id(call_id.body());
      if (output.type != js::Type::Null) e.text = add_tool_output(arena, output);
    } else {
      return;
    }
    out.push_back(e);
    if (pt == "message" && role == "assistant" && channel == "final") {
      Event end;
      end.kind = EventKind::TurnEnd;
      out.push_back(end);
    }
  }
};

const CodexAdapter g_codex;

}  // namespace

const Adapter& codex_adapter() { return g_codex; }

static constexpr std::string_view kReplyOpen = "<send_user_message_question_reply>";
static constexpr std::string_view kReplyClose = "</send_user_message_question_reply>";

std::string async_reply_envelope(const std::vector<AsyncReply>& replies) {
  std::string out = std::string(kReplyOpen) + "\n[";
  for (size_t i = 0; i < replies.size(); i++) {
    const AsyncReply& r = replies[i];
    // Codex keeps the question on one line, as its own replies do.
    std::string question = r.question;
    std::replace(question.begin(), question.end(), '\n', ' ');
    std::replace(question.begin(), question.end(), '\r', ' ');
    const std::string id = "[\"request_user_input_async\"," + js::quote(r.call_id) + "," + std::to_string(r.index) + "]";
    if (i) out += ",";
    out += "{\"answer\":" + js::quote(r.answer) + ",\"question\":" + js::quote(question) +
           ",\"questionItemId\":" + js::quote(id) + "}";
  }
  return out + "]\n" + std::string(kReplyClose);
}

bool parse_async_reply(std::string_view text, std::vector<AsyncReply>& out) {
  out.clear();
  while (!text.empty() && std::isspace(uint8_t(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(uint8_t(text.back()))) text.remove_suffix(1);
  if (!text.starts_with(kReplyOpen) || !text.ends_with(kReplyClose)) return false;
  text = text.substr(kReplyOpen.size(), text.size() - kReplyOpen.size() - kReplyClose.size());
  while (!text.empty() && std::isspace(uint8_t(text.front()))) text.remove_prefix(1);
  const auto one = [&](const js::Value& v) {
    if (!v.is_object()) return true;
    AsyncReply r;
    std::string id;
    js::scan_object(v.raw, [&](std::string_view k, const js::Value& f) {
      if (!f.is_string()) return true;
      if (k == "answer") js::unescape_append(f.body(), r.answer);
      else if (k == "question") js::unescape_append(f.body(), r.question);
      else if (k == "questionItemId") js::unescape_append(f.body(), id);
      return true;
    });
    // ["request_user_input_async","<call>",<index>]
    int at = 0;
    js::scan_array(id, [&](const js::Value& e) {
      if (at == 1 && e.is_string()) js::unescape_append(e.body(), r.call_id);
      else if (at == 2) r.index = std::atoi(std::string(e.raw).c_str());
      at++;
      return true;
    });
    out.push_back(std::move(r));
    return true;
  };
  if (text.starts_with('[')) js::scan_array(text, one);
  else one(js::Value{text, text.starts_with('{') ? js::Type::Object : js::Type::Null});
  return !out.empty();
}

}  // namespace mico
