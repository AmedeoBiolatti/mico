#include "adapters/claude/claude.h"
#include "base/text.h"

#include <algorithm>
#include <cstdlib>

#include "adapters/tool_calls.h"
#include "base/time.h"

namespace mico {

using namespace tools;

namespace {

// A block of a message's content as Claude writes it: a tool_use (id, name,
// input) or a tool_result (tool_use_id, is_error, content).
struct ToolBlock {
  std::string_view type;
  std::string id, name, use_id;
  js::Value input{}, content{};
  bool error = false;
};

ToolBlock read_block(std::string_view raw) {
  ToolBlock b;
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "type") b.type = v.body();
    else if (k == "id") b.id = text_of(v);
    else if (k == "name") b.name = text_of(v);
    else if (k == "input") b.input = v;
    else if (k == "tool_use_id") b.use_id = text_of(v);
    else if (k == "is_error") b.error = v.is_true();
    else if (k == "content") b.content = v;
    return true;
  });
  return b;
}

}  // namespace

// tool_use blocks in assistant messages, tool_result blocks in
// user messages, each record stamped.
void ClaudeAdapter::read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const {
  // "tool_use" or "tool_result", in one scan.
  if (!text::contains(raw, "\"tool_"))
    return;
  // Stops once it has both: what follows them, toolUseResult above all,
  // often repeats the whole of a tool's output.
  int64_t at = 0;
  bool stamped = false;
  std::string_view message;
  js::scan_keys(raw, [&](std::string_view k, std::string_view rest) {
    if (k == "timestamp") {
      at = parse_time(js::string_body(rest));
      stamped = true;
    } else if (k == "message") {
      message = rest;
    }
    return !(stamped && !message.empty());
  });
  if (!message.starts_with('{')) return;
  js::Value content{};
  js::scan_object(message, [&](std::string_view k, const js::Value& v) {
    if (k == "content") { content = v; return false; }
    return true;
  });
  if (!content.is_array()) return;
  js::scan_array(content.raw, [&](const js::Value& v) {
    if (!v.is_object()) return true;
    const ToolBlock b = read_block(v.raw);
    if (b.type == "tool_use" && !b.id.empty()) sink.call(b.id, at, offset, b.name, subject(b.input));
    else if (b.type == "tool_result" && !b.use_id.empty())
      sink.result(b.use_id, at, b.error, -1, sink.wants_output(b.use_id, {}) ? result_text(b.content) : std::string());
    return true;
  });
}

namespace {

// "&quot;" and its kind, as Claude writes them inside a notice.
std::string unentity(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '&') {
      static constexpr std::pair<std::string_view, char> kEnt[] = {
          {"&quot;", '"'}, {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&#39;", '\''}, {"&apos;", '\''}};
      bool done = false;
      for (const auto& [e, c] : kEnt)
        if (s.compare(i, e.size(), e) == 0) {
          out.push_back(c);
          i += e.size() - 1;
          done = true;
          break;
        }
      if (done) continue;
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string tag(std::string_view env, std::string_view name) {
  const std::string open = "<" + std::string(name) + ">", close = "</" + std::string(name) + ">";
  const size_t a = env.find(open);
  if (a == std::string_view::npos) return {};
  const size_t b = env.find(close, a + open.size());
  if (b == std::string_view::npos) return {};
  return unentity(env.substr(a + open.size(), b - a - open.size()));
}

}  // namespace

// Calls that start background work (Monitor; Bash, which may be moved to the
// background or run there from the start; Agent, launched to run there) or stop it (TaskStop, and the
// KillShell before it); the results that say a task began (toolUseResult's
// taskId or backgroundTaskId); and <task-notification> envelopes, wherever
// the line holds one — a user turn, a queued command, the queue itself —
// which carry a monitor's events and every task's end (a <status>, or a
// monitor's "expired" event).
void ClaudeAdapter::read_background(std::string_view raw, uint64_t offset, BackgroundTasks& t) const {
  const bool tools = text::contains(raw, "\"tool_");
  const bool notices = text::contains(raw, "task-notification>");
  if (!tools && !notices) return;
  int64_t at = 0;
  js::Value message{}, result{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "timestamp") at = parse_time(v.body());
    else if (k == "message") message = v;
    else if (k == "toolUseResult") result = v;
    return true;
  });

  if (tools && message.is_object()) {
    js::Value content{};
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "content") content = v;
      return true;
    });
    if (content.is_array())
      js::scan_array(content.raw, [&](const js::Value& block) {
        if (!block.is_object()) return true;
        const auto [type, id, name, use_id, input, said, error] = read_block(block.raw);
        if (type == "tool_use" && !id.empty() &&
            (name == "Monitor" || name == "Bash" || name == "TaskStop" || name == "KillShell" || name == "KillBash" ||
             name == "Agent" || name == "Task")) {
          BackgroundTasks::Call c;
          c.at_ms = at;
          c.offset = offset;
          std::string description, command, stops;
          bool persistent = false;
          int64_t timeout = 0;
          js::scan_object(input.raw, [&](std::string_view k, const js::Value& v) {
            if (k == "description") description = text_of(v);
            else if (k == "command") command = text_of(v);
            else if (k == "task_id" || k == "shell_id" || k == "bash_id") stops = text_of(v);
            else if (k == "timeout_ms" && v.type == js::Type::Number) timeout = std::atoll(std::string(v.raw).c_str());
            else if (k == "persistent") persistent = v.is_true();
            return true;
          });
          if (name == "Monitor") {
            c.kind = "monitor";
            // Claude's own default when the call names none.
            c.timeout_ms = persistent ? 0 : timeout > 0 ? timeout : 300000;
          } else if (name == "Bash") {
            c.kind = "shell";
          } else if (name == "Agent" || name == "Task") {
            c.kind = "agent";
          } else {
            c.kind = "stop";
          }
          c.what = c.kind == "stop" ? stops : one_line(description.empty() ? command : description, 160);
          t.calls[id] = std::move(c);
        } else if (type == "tool_result" && !use_id.empty()) {
          const auto it = t.calls.find(use_id);
          if (it == t.calls.end()) return true;
          const BackgroundTasks::Call c = std::move(it->second);
          t.calls.erase(it);
          if (c.kind == "stop") {
            if (!error && !c.what.empty()) t.end(c.what);
            return true;
          }
          std::string task;
          bool async = false;
          if (result.is_object())
            js::scan_object(result.raw, [&](std::string_view k, const js::Value& v) {
              if ((k == "taskId" || k == "backgroundTaskId") && v.is_string()) task = text_of(v);
              else if (k == "agentId" && v.is_string() && c.kind == "agent") task = text_of(v);
              else if (k == "isAsync") async = v.is_true();
              return true;
            });
          // A subagent runs in the background only when launched to: one that
          // ran in the foreground has finished by the time its result is in.
          if (c.kind == "agent" && !async) return true;
          // "… Output is being written to: /tmp/claude-1000/…/tasks/<id>.output."
          std::string output;
          if (!task.empty() && !error) {
            const std::string text = result_text(said);
            constexpr std::string_view kTo = "Output is being written to: ";
            if (const size_t at = text.find(kTo); at != std::string::npos) {
              const size_t from = at + kTo.size();
              size_t end = text.find_first_of(" \n\t", from);
              if (end == std::string::npos) end = text.size();
              output = text.substr(from, end - from);
              while (!output.empty() && (output.back() == '.' || output.back() == ',')) output.pop_back();
            }
            t.start(task, c, output);
          }
        }
        return true;
      });
  }

  if (notices) {
    // Each envelope, as escaped inside its JSON string.
    constexpr std::string_view kOpen = "<task-notification>", kClose = "</task-notification>";
    for (size_t p = raw.find(kOpen); p != std::string_view::npos; p = raw.find(kOpen, p + kOpen.size())) {
      const size_t e = raw.find(kClose, p);
      if (e == std::string_view::npos) break;
      std::string env;
      js::unescape_append(raw.substr(p, e + kClose.size() - p), env);
      const std::string id = tag(env, "task-id");
      if (id.empty()) continue;
      const std::string status = tag(env, "status"), event = tag(env, "event");
      if (!status.empty() || event.starts_with("[Monitor expired")) t.end(id);
      else if (!event.empty()) t.event(id, one_line(event, 200), at);
    }
  }
}

}  // namespace mico
