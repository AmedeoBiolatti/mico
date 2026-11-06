#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "adapters/codex/codex.h"

#include "base/text.h"

#include "adapters/adapters.h"
#include "adapters/user_text.h"
#include "adapters/translation.h"

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
  while (!t.empty() && std::isspace(uint8_t(t.front()))) t.remove_prefix(1);
  static constexpr std::string_view kMarkers[] = {
      "# AGENTS.md instructions", "<skills_instructions>", "<user_instructions>",
      "<environment_context>",    "<recommended_plugins>", "# Instructions for",
      // A finished /review hands its findings back as a user-role message;
      // the reader sees them in the assistant message that follows.
      "<user_action>"};
  for (auto m : kMarkers)
    if (t.size() >= m.size() && t.compare(0, m.size(), m) == 0) return true;
  return false;
}

// Context and a user's text can share one message. Filter its blocks rather
// than demoting the whole message because its first block is context. Newer
// rollouts identify user.text explicitly, including pasted instruction text.
Str add_user_content(Arena& arena, const js::Value& content, const js::Value& metadata, bool& context_only) {
  std::vector<std::string_view> kinds;
  js::scan_object(metadata.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "content_item_kinds")
      js::scan_array(v.raw, [&](const js::Value& kind) { kinds.push_back(kind.body()); return true; });
    return true;
  });
  if (!content.is_array()) {
    Str text = add_content(arena, content);
    context_only = (kinds.empty() || kinds[0] != "user.text") && looks_like_preamble(arena.view(text));
    return text;
  }
  std::string shown;
  size_t index = 0;
  js::scan_array(content.raw, [&](const js::Value& b) {
    const bool human = index < kinds.size() && kinds[index] == "user.text";
    index++;
    js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
      if (k != "text" || !v.is_string()) return true;
      std::string text;
      js::unescape_append(v.body().substr(0, Arena::kMaxText), text);
      if (human || !looks_like_preamble(text)) {
        if (!shown.empty()) shown += '\n';
        shown += text;
      }
      return false;
    });
    return shown.size() < Arena::kMaxText;
  });
  context_only = shown.empty();
  return context_only ? add_content(arena, content) : arena.add(shown);
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

// A function call names its tool within a namespace: an MCP server's
// ("mcp__cua_repl" + "js") or a built-in group's ("clock" + "sleep"). Spelled
// the way codex spells the whole name, so mico's own plot is recognised.
Str add_tool_name(Arena& arena, const js::Value& name, const js::Value& ns) {
  if (!name.is_string()) return arena.add("?");
  if (!ns.is_string() || ns.body().empty() || ns.body() == "functions") return arena.add_json(name);
  const uint32_t at = arena.open();
  arena.put_json(ns);
  arena.put(ns.body().starts_with("mcp__") ? "__" : ".");
  arena.put_json(name);
  return arena.close(at);
}

// The names in a tool list, as `additional_tools` and `tool_search_output`
// carry it: tools and namespaces of tools, a namespace's on one line.
std::string tool_names(const js::Value& tools) {
  std::string out;
  const auto name_of = [](const js::Value& tool) {
    std::string name;
    js::scan_object(tool.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "name" && v.is_string()) { js::unescape_append(v.body(), name); return false; }
      return true;
    });
    return name;
  };
  js::scan_array(tools.raw, [&](const js::Value& tool) {
    if (!tool.is_object()) return true;
    js::Value nested{};
    js::scan_object(tool.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "tools" && v.is_array()) nested = v;
      return true;
    });
    if (!out.empty()) out += '\n';
    out += name_of(tool);
    if (nested.is_array()) {
      out += ": ";
      bool first = true;
      js::scan_array(nested.raw, [&](const js::Value& t) {
        if (!first) out += ", ";
        first = false;
        out += name_of(t);
        return out.size() < Arena::kMaxText;
      });
    }
    return out.size() < Arena::kMaxText;
  });
  return out;
}

// Generated pixels are a bare base64 `result`, not an image block, so the
// adapter points at them itself: an Image event's summary is where they sit
// in the line ("media offset length"), read back when the chat draws it.
bool add_generated_image(Arena& arena, std::string_view raw, const js::Value& result, Event& e) {
  if (!result.is_string() || result.body().empty()) return false;
  const std::string_view b64 = result.body();
  if (b64.data() < raw.data() || b64.data() + b64.size() > raw.data() + raw.size()) return false;
  const std::string_view media = b64.starts_with("/9j/")    ? "image/jpeg"
                                 : b64.starts_with("UklGR") ? "image/webp"
                                 : b64.starts_with("R0lGOD") ? "image/gif"
                                                             : "image/png";
  e.kind = EventKind::Image;
  e.summary = arena.add(std::string(media) + ' ' + std::to_string(b64.data() - raw.data()) + ' ' +
                        std::to_string(b64.size()));
  return true;
}

// A rollback's count of the turns it drops, from its event_msg payload; -1
// when it has none.
int64_t rollback_turns(std::string_view payload) {
  int64_t turns = -1;
  js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
    if (k == "num_turns" && v.type == js::Type::Number) turns = std::atoll(std::string(v.raw).c_str());
    return true;
  });
  return turns;
}

}  // namespace

void CodexAdapter::seed_state(SessionState& st) const {
  st.declare("model", "model");
  st.declare("effort", "effort");
  st.declare("approval", "permissions");
  st.declare("sandbox", "sandbox");
}

void CodexAdapter::observe(std::string_view raw, SessionState& st) const {
  std::string_view payload;
  std::string_view type;
  js::scan_keys(raw, [&](std::string_view k, std::string_view rest) {
    if (k == "type") {
      type = js::string_body(rest);
      return type == "turn_context" || type == "event_msg";
    }
    if (k == "payload") { payload = rest; return false; }
    return true;
  });
  if (payload.empty() || payload.front() != '{') return;
  if (type == "event_msg") {
    std::string_view settings;
    bool applied = false, goal = false;
    js::scan_keys(payload, [&](std::string_view k, std::string_view rest) {
      if (k == "type") {
        const std::string_view t = js::string_body(rest);
        applied = t == "thread_settings_applied";
        goal = t == "thread_goal_updated";
        return applied || goal;
      }
      if (k == (goal ? "goal" : "thread_settings")) { settings = rest; return false; }
      return true;
    });
    if (settings.empty() || settings.front() != '{') return;
    if (goal) {
      // A /goal is a standing setting, not a step: a chip, which each update
      // (its usage counts tick every turn) rewrites instead of repeating.
      std::string objective, status;
      js::scan_object(settings, [&](std::string_view k, const js::Value& v) {
        if (k == "objective" && v.is_string()) js::unescape_append(v.body(), objective);
        else if (k == "status" && v.is_string()) status = v.body();
        return true;
      });
      for (char& c : objective)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      objective = text::ellipsize(objective, 60);
      if (!status.empty() && status != "active") objective += " (" + status + ")";
      st.set("goal", "goal", objective);
      return;
    }
    if (!applied) return;
    payload = settings;
  } else if (type != "turn_context") return;

  js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
    if (k == "model") st.set("model", "model", v.body());
    else if (k == "effort" || k == "reasoning_effort") st.set("effort", "effort", v.body());
    else if (k == "approval_policy") st.set("approval", "permissions", v.body());
    else if (k == "personality") st.set("personality", "style", v.body());
    else if (k == "summary") st.set("summary", "summary", v.body());
    else if (k == "sandbox_policy") {
      js::scan_object(v.raw, [&](std::string_view sk, const js::Value& sv) {
        if (sk != "type") return true;
        st.set("sandbox", "sandbox", sv.body());
        return false;
      });
    } else if (k == "permission_profile") {
      std::string_view profile;
      bool write = false;
      js::scan_object(v.raw, [&](std::string_view pk, const js::Value& pv) {
        if (pk == "type") profile = pv.body();
        else if (pk == "file_system")
          js::scan_object(pv.raw, [&](std::string_view fk, const js::Value& fv) {
            if (fk == "entries") js::scan_array(fv.raw, [&](const js::Value& entry) {
              js::scan_object(entry.raw, [&](std::string_view ek, const js::Value& ev) {
                if (ek == "access" && ev.body() == "write") write = true;
                return true;
              });
              return true;
            });
            return true;
          });
        return true;
      });
      if (profile == "disabled") st.set("sandbox", "sandbox", "danger-full-access");
      else if (profile == "managed") st.set("sandbox", "sandbox", write ? "workspace-write" : "read-only");
    }
    return true;
  });
}

// Pictures in a compacted record are copies: the replacement history it
// carries is for the model, and the chat has already shown them where they
// were. An event_msg is a mirror of a response_item the same way.
bool CodexAdapter::owns_images(std::string_view raw) const {
  bool copy = false;
  js::scan_keys(raw, [&](std::string_view k, std::string_view rest) {
    if (k != "type") return true;
    const std::string_view type = js::string_body(rest);
    copy = type == "compacted" || type == "event_msg";
    return false;
  });
  return !copy;
}

// A rollback is an event_msg: {"type":"event_msg","payload":{"type":
// "thread_rolled_back","num_turns":2}}.
int CodexAdapter::rewound_turns(std::string_view raw) const {
  std::string_view type, payload;
  js::scan_keys(raw, [&](std::string_view k, std::string_view rest) {
    if (k == "type") { type = js::string_body(rest); return type == "event_msg"; }
    if (k == "payload") { payload = rest; return false; }
    return true;
  });
  if (type != "event_msg" || payload.empty() || payload.front() != '{') return 0;
  bool rollback = false;
  js::scan_keys(payload, [&](std::string_view k, std::string_view rest) {
    if (k == "type") rollback = js::string_body(rest) == "thread_rolled_back";
    return false;
  });
  if (!rollback) return 0;
  return int(std::clamp<int64_t>(rollback_turns(payload), 0, 1 << 30));
}

// What codex counts as a turn when it rolls back: a message of the user's
// (not context it injected), or a message from another agent.
bool CodexAdapter::starts_turn(std::string_view raw) const {
  // Told by the head's payload type when it names one; anything else is read.
  const std::string_view pt = codex_payload_type(raw);
  if (!pt.empty() && pt != "message" && pt != "agent_message") return false;
  thread_local Arena arena;
  thread_local std::vector<Event> events;
  arena.clear();
  events.clear();
  parse(raw, arena, events);
  return std::any_of(events.begin(), events.end(),
                     [](const Event& e) { return e.kind == EventKind::User || e.kind == EventKind::Peer; });
}

void CodexAdapter::parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const {
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
      return type == "response_item" || type == "event_msg" || type == "inter_agent_communication";
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
  if (type == "inter_agent_communication") {
    // A message between agents, written as its own record rather than as an
    // agent_message response item.
    if (payload.empty() || payload.front() != '{') {
      translation_warning(arena, out, "codex", "record payload", raw);
      return;
    }
    js::Value author{}, recipient{}, content{};
    js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
      if (k == "author") author = v;
      else if (k == "recipient") recipient = v;
      else if (k == "content") content = v;
      return true;
    });
    if (!content.is_string()) {
      translation_warning(arena, out, "codex", "inter-agent message", raw);
      return;
    }
    Event e;
    e.kind = EventKind::Peer;
    if (author.is_string()) e.name = arena.add_json(author);
    if (recipient.is_string()) e.summary = arena.add_json(recipient);
    e.text = arena.add_json(content);
    if (!e.text.empty()) out.push_back(e);
    return;
  }
  if (type != "response_item" && type != "event_msg") {
    // A line codex never finished writing has no type to name.
    if (type.empty()) translation_warning(arena, out, "codex", "malformed record", raw);
    // Bookkeeping for codex itself: tool lists, risk scores, usage, state.
    else if (type != "session_meta" && type != "turn_context" && type != "world_state" &&
             type != "token_usage_record" && type != "inter_agent_communication_metadata" &&
             type != "dynamic_tools_updated" && type != "security_risk_score")
      translation_warning(arena, out, "codex", "record " + std::string(type), raw);
    return;
  }
  if (payload.empty() || payload.front() != '{') {
    translation_warning(arena, out, "codex", "record payload", raw);
    return;
  }
  if (type == "event_msg") {
    bool item = false;
    std::string_view event_type;
    js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
      if (k != "type") return true;
      event_type = v.body();
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
    if (event_type == "thread_rolled_back") {
      // The turns stay in the file; the conversation leaves them out
      // (Conversation's rewinds) and this marks the place.
      const int64_t turns = rollback_turns(payload);
      if (turns < 0) {
        translation_warning(arena, out, "codex", "rollback", raw);
        return;
      }
      Event e;
      e.kind = EventKind::Notice;
      e.text = arena.add(turns == 1 ? std::string("Rolled back 1 turn") : "Rolled back " + std::to_string(turns) + " turns");
      out.push_back(e);
      return;
    }
    // A /goal is drawn as a chip (observe), not in the chat.
    if (!item && event_type != "task_complete" && event_type != "turn_complete" && event_type != "turn_aborted" &&
        event_type != "task_started" && event_type != "turn_started" && event_type != "token_count" &&
        event_type != "thread_settings_applied" && event_type != "agent_message" && event_type != "agent_reasoning" &&
        event_type != "agent_reasoning_raw_content" && event_type != "user_message" &&
        event_type != "thread_goal_updated")
      translation_warning(arena, out, "codex", "event " + std::string(event_type), raw);
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
      std::string_view item_type;
      js::scan_keys(it, [&](std::string_view k, std::string_view rest) {
        if (k == "type") { item_type = js::string_body(rest); mcp = item_type == "McpToolCall"; }
        return false;
      });
      if (!mcp) {
        if (item_type == "SubAgentActivity") {
          js::Value kind{}, path{}, thread{}, model{}, effort{};
          js::scan_object(it, [&](std::string_view k, const js::Value& v) {
            if (k == "kind") kind = v;
            else if (k == "agent_path") path = v;
            else if (k == "agent_thread_id") thread = v;
            else if (k == "model") model = v;
            else if (k == "reasoning_effort") effort = v;
            return true;
          });
          if (!kind.is_string() || (kind.body() != "started" && kind.body() != "interacted" && kind.body() != "completed") ||
              ((!path.is_string() || path.body().empty()) && (!thread.is_string() || thread.body().empty()))) {
            translation_warning(arena, out, "codex", "subagent activity", raw);
            return;
          }
          Event e;
          e.kind = EventKind::TaskStatus;
          const uint32_t at = arena.open();
          arena.put("Subagent ");
          arena.put_json(path.is_string() && !path.body().empty() ? path : thread);
          arena.put(" ");
          arena.put_json(kind);
          for (const auto& value : {model, effort}) {
            if (!value.is_string() || value.body().empty()) continue;
            arena.put(" \xC2\xB7 ");
            arena.put_json(value);
          }
          e.text = arena.close(at);
          out.push_back(e);
          return;
        }
        // Built-in extensions also write a completed-item mirror. Their
        // actual call and output are response_items, already rendered below.
        if (item_type == "Extension") {
          std::string_view kind;
          js::scan_keys(it, [&](std::string_view k, std::string_view rest) {
            if (k == "kind") { kind = js::string_body(rest); return false; }
            return true;
          });
          if (kind == "web.search" || kind == "clock.sleep") return;
          if (kind == "image_gen.generation") {
            // Generated by a tool inside an exec script, which shows its
            // call; the picture itself is saved to a file, named here.
            js::Value saved{};
            js::scan_object(it, [&](std::string_view k, const js::Value& v) {
              if (k == "savedPath" || k == "saved_path") saved = v;
              return true;
            });
            Event e;
            e.kind = EventKind::Notice;
            const uint32_t at = arena.open();
            arena.put("Generated an image");
            if (saved.is_string() && !saved.body().empty()) {
              arena.put(": ");
              arena.put_json(saved);
            }
            e.text = arena.close(at);
            out.push_back(e);
            return;
          }
          translation_warning(arena, out, "codex", "extension " + std::string(kind), raw);
          return;
        }
        if (item_type == "EnteredReviewMode") {
          js::Value hint{};
          js::scan_object(it, [&](std::string_view k, const js::Value& v) {
            if (k == "user_facing_hint") hint = v;
            return true;
          });
          Event e;
          e.kind = EventKind::Notice;
          const uint32_t at = arena.open();
          arena.put("Review started");
          if (hint.is_string() && !hint.body().empty()) {
            arena.put(": ");
            arena.put_json(hint);
          }
          e.text = arena.close(at);
          out.push_back(e);
          return;
        }
        // Mirrors of what other records already show: compaction by its
        // compacted record, an image view and a function's output by the
        // tool output, a plan and a review's findings by the assistant's
        // message, a subagent tool call and a generated image by their
        // response items, a dynamic tool by the exec script that called it.
        if (item_type != "CommandExecution" && item_type != "FileChange" && item_type != "AgentMessage" &&
            item_type != "Reasoning" && item_type != "UserMessage" && item_type != "WebSearch" &&
            item_type != "ContextCompaction" && item_type != "ImageView" && item_type != "CollabAgentToolCall" &&
            item_type != "Plan" && item_type != "DynamicToolCall" && item_type != "FunctionCallOutput" &&
            item_type != "ExitedReviewMode" && item_type != "ImageGeneration")
          translation_warning(arena, out, "codex", "completed item " + std::string(item_type), raw);
        return;
      }
      std::string_view server, tool, id;
      js::Value args{};
      js::scan_object(it, [&](std::string_view k, const js::Value& v) {
        if (k == "server") server = v.body();
        else if (k == "tool") tool = v.body();
        else if (k == "id") id = v.body();
        else if (k == "arguments") args = v;
        return true;
      });
      // A direct call shares its id with its function_call, which already
      // drew the chart; one made inside an exec script has no call of its own.
      if (server == "mico" && tool == "plot" && args.is_object() && !id.starts_with("call_")) {
        Event e;
        e.tool_id = hash_id(id);
        make_chart_event(e, arena, args.raw);
        out.push_back(e);
      }
    }
    return;
  }
  if (type != "response_item") return;

  std::string_view pt, role, channel, phase;
  js::Value content{}, summary{}, name{}, call_id{}, input{}, arguments{}, output{}, metadata{}, author{}, recipient{},
      ns{}, tools{}, action{}, result{};
  js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
    if (k == "type") pt = v.body();
    else if (k == "namespace") ns = v;
    else if (k == "tools") tools = v;
    else if (k == "action") action = v;
    else if (k == "result") result = v;
    else if (k == "role") role = v.body();
    else if (k == "channel") channel = v.body();
    else if (k == "phase") phase = v.body();
    else if (k == "internal_chat_message_metadata_passthrough") metadata = v;
    else if (k == "content") content = v;
    else if (k == "summary") summary = v;
    else if (k == "name") name = v;
    else if (k == "call_id") call_id = v;
    else if (k == "input") input = v;
    else if (k == "arguments") arguments = v;
    else if (k == "output") output = v;
    else if (k == "author") author = v;
    else if (k == "recipient") recipient = v;
    return true;
  });

  Event e;
  if (pt == "agent_message") {
    if (content.type == js::Type::Null) {
      translation_warning(arena, out, "codex", "missing agent message content", raw);
      return;
    }
    // Routing metadata is a separate record. The message itself belongs in
    // the peer view, without starting or ending a human conversation turn.
    e.kind = EventKind::Peer;
    if (author.is_string()) e.name = arena.add_json(author);
    if (recipient.is_string()) e.summary = arena.add_json(recipient);
    if (content.is_array()) {
      std::string shown;
      js::scan_array(content.raw, [&](const js::Value& block) {
        std::string_view kind;
        js::scan_object(block.raw, [&](std::string_view k, const js::Value& v) {
          if (k == "type") kind = v.body();
          return true;
        });
        if (!shown.empty()) shown += '\n';
        if (kind == "encrypted_content") {
          shown += "[Encrypted content unavailable]";
        } else {
          const std::string single = "[" + std::string(block.raw) + "]";
          const js::Value blocks{single, js::Type::Array};
          warn_content(arena, out, "codex", blocks);
          shown += arena.view(add_content(arena, blocks));
        }
        return shown.size() < Arena::kMaxText;
      });
      e.text = arena.add(shown);
    } else {
      warn_content(arena, out, "codex", content);
      e.text = add_content(arena, content);
    }
    if (e.text.empty()) return;
  } else if (pt == "message") {
    if (role != "user" && role != "assistant" && role != "developer" && role != "system") {
      translation_warning(arena, out, "codex", "message role " + std::string(role), raw);
      return;
    }
    if (content.type == js::Type::Null) {
      if (role == "user" || role == "assistant") translation_warning(arena, out, "codex", "missing message content", raw);
      return;
    }
    if (role == "user" || role == "assistant") warn_content(arena, out, "codex", content);
    bool context_only = false;
    e.text = role == "user" ? add_user_content(arena, content, metadata, context_only) : add_content(arena, content);
    if (e.text.empty()) return;
    if (role == "assistant") e.kind = EventKind::Assistant;
    else if (role == "user")
      e.kind = context_only ? EventKind::Meta : EventKind::User;
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
  } else if (pt == "configuration_update") {
    // Harness configuration is context, like system/developer messages.
    // Show all settings in the context view as new ones are introduced.
    e.kind = EventKind::Meta;
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k != "payload") return true;
      e.text = arena.add("Configuration updated\n" + std::string(v.raw));
      return false;
    });
  } else if (pt == "reasoning") {
    // summary is usually empty (content is encrypted server-side); only the
    // rare summarized reasoning is renderable.
    if (summary.type == js::Type::Null) return;
    warn_content(arena, out, "codex", summary);
    e.text = add_content(arena, summary);
    if (e.text.empty()) return;
    e.kind = EventKind::Thinking;
  } else if (pt == "custom_tool_call" || pt == "function_call") {
    e.kind = EventKind::ToolCall;
    e.name = add_tool_name(arena, name, ns);
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
      if (!args.is_string()) warn_content(arena, out, "codex", args);
      e.detail = narrow_to_patch(arena, e.summary);
      if (e.detail.empty() && arena.view(e.name) == "exec")
        e.summary = narrow_to_command(arena, e.summary);
    }
  } else if (pt == "custom_tool_call_output" || pt == "function_call_output") {
    e.kind = EventKind::ToolResult;
    e.tool_id = hash_id(call_id.body());
    if (output.type != js::Type::Null) e.text = add_tool_output(arena, output);
    warn_content(arena, out, "codex", output);
  } else if (pt == "additional_tools") {
    // The tools the model is offered from here on, in full schema: context,
    // like a developer message. The context view lists their names.
    if (!tools.is_array()) {
      translation_warning(arena, out, "codex", "tool list", raw);
      return;
    }
    e.kind = EventKind::Meta;
    e.text = arena.add("Tools available\n" + tool_names(tools));
  } else if (pt == "local_shell_call") {
    // The older shell tool: its command is an argv; its output comes back
    // as a function_call_output under the same call id.
    e.kind = EventKind::ToolCall;
    e.name = arena.add("shell");
    e.tool_id = hash_id(call_id.body());
    std::string command;
    js::scan_object(action.raw, [&](std::string_view k, const js::Value& v) {
      if (k != "command" || !v.is_array()) return true;
      js::scan_array(v.raw, [&](const js::Value& word) {
        if (!command.empty()) command += ' ';
        if (word.is_string()) js::unescape_append(word.body(), command);
        return command.size() < Arena::kMaxText;
      });
      return false;
    });
    if (command.empty()) {
      translation_warning(arena, out, "codex", "shell call", raw);
      return;
    }
    e.summary = arena.add(command);
  } else if (pt == "tool_search_call") {
    e.kind = EventKind::ToolCall;
    e.name = arena.add("tool_search");
    e.tool_id = hash_id(call_id.body());
    // Its arguments are an object ({"query": …}), not a JSON string.
    if (arguments.is_string()) e.summary = arena.add_json(arguments);
    else if (arguments.type != js::Type::Null) e.summary = arena.add(arguments.raw);
  } else if (pt == "tool_search_output") {
    e.kind = EventKind::ToolResult;
    e.tool_id = hash_id(call_id.body());
    const std::string found = tools.is_array() ? tool_names(tools) : std::string();
    e.text = arena.add(found.empty() ? std::string("No tools found") : "Found\n" + found);
  } else if (pt == "web_search_call") {
    // The search itself; codex keeps no results, so the call is the row.
    std::string what;
    js::Value queries{};
    std::string_view kind;
    js::scan_object(action.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") kind = v.body();
      else if (k == "queries" && v.is_array()) queries = v;
      else if ((k == "query" || k == "url") && v.is_string() && what.empty()) js::unescape_append(v.body(), what);
      else if (k == "pattern" && v.is_string()) {
        what += what.empty() ? "" : " \xC2\xB7 ";
        js::unescape_append(v.body(), what);
      }
      return true;
    });
    if (what.empty() && queries.is_array())
      js::scan_array(queries.raw, [&](const js::Value& q) {
        if (!q.is_string()) return true;
        if (!what.empty()) what += " \xC2\xB7 ";
        js::unescape_append(q.body(), what);
        return what.size() < Arena::kMaxText;
      });
    e.kind = EventKind::ToolCall;
    e.name = arena.add(kind == "open_page" ? "web_open" : kind == "find_in_page" ? "web_find" : "web_search");
    if (!what.empty()) e.summary = arena.add(what);
  } else if (pt == "image_generation_call") {
    // The picture is the answer, so it shows at every density.
    if (!add_generated_image(arena, raw, result, e)) {
      js::Value status{};
      js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
        if (k == "status") status = v;
        return true;
      });
      // Still generating, or failed: no pixels to show.
      if (status.is_string() && (status.body() == "in_progress" || status.body() == "generating")) return;
      translation_warning(arena, out, "codex", "image generation", raw);
      return;
    }
  } else if (pt == "compaction" || pt == "compaction_summary" || pt == "context_compaction") {
    // Encrypted history for the model; the compacted record marks the place.
    return;
  } else {
    translation_warning(arena, out, "codex", "response item " + std::string(pt), raw);
    return;
  }
  out.push_back(e);
  if (pt == "message" && role == "assistant" && (channel == "final" || phase == "final_answer")) {
    Event end;
    end.kind = EventKind::TurnEnd;
    out.push_back(end);
  }
}

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
