#include <cstdlib>

#include "adapters/codex/codex.h"
#include "adapters/tool_calls.h"
#include "base/text.h"
#include "base/time.h"

// Codex's background work, as its rollout records it. Two kinds outlive the
// call that started them:
//   - a code cell its `exec` tool yields ("Script running with cell ID 3"),
//     until a `wait` on that cell says "Script completed" or "Script failed";
//   - a command started with exec_command that is still running when the
//     call returns: its output chunk names a session_id and no exit_code,
//     until the command's CommandExecution item completes with that
//     process_id (what codex's /ps lists as background terminals).
namespace mico {

using namespace tools;

namespace {

// The first command an exec script runs, `cmd:"…"`, for the list.
std::string first_command(std::string_view script) {
  static constexpr std::string_view kKey = "cmd:\"";
  size_t at = script.find(kKey);
  if (at == std::string_view::npos) return one_line(script, 160);
  at += kKey.size();
  std::string cmd;
  for (size_t i = at; i < script.size() && script[i] != '"'; i++) {
    if (script[i] == '\\' && i + 1 < script.size()) {
      const char c = script[++i];
      cmd += c == 'n' ? ' ' : c;
    } else {
      cmd += script[i];
    }
  }
  return one_line(cmd, 160);
}

// Each JSON object in an output's text that names a session_id: the chunks
// exec_command returns, one per command a script ran.
template <class F>
void each_chunk(std::string_view text, F&& fn) {
  for (size_t at = text.find("{\"chunk_id\""); at != std::string_view::npos;
       at = text.find("{\"chunk_id\"", at + 1)) {
    std::string session;
    bool exited = false;
    js::scan_object(text.substr(at), [&](std::string_view k, const js::Value& v) {
      if (k == "session_id") session = std::string(v.raw);
      else if (k == "exit_code") exited = true;
      return true;
    });
    if (!session.empty()) fn(session, exited);
  }
}

}  // namespace

void CodexAdapter::read_background(std::string_view raw, uint64_t offset, BackgroundTasks& t) const {
  const std::string_view head = codex_payload_type(raw);
  const bool call = head == "custom_tool_call" || head == "function_call";
  const bool output = head == "custom_tool_call_output" || head == "function_call_output";
  const bool item = head == "item_completed" || (head.empty() && text::contains(raw, "\"item_completed\""));
  if (!call && !output && !item && !head.empty()) return;

  int64_t at = 0;
  js::Value payload{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "timestamp") at = parse_time(v.body());
    else if (k == "payload") payload = v;
    return true;
  });
  if (!payload.is_object()) return;
  std::string type, name, call_id;
  js::Value input{}, arguments{}, out{}, it{};
  js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "type") type = text_of(v);
    else if (k == "name") name = text_of(v);
    else if (k == "call_id") call_id = text_of(v);
    else if (k == "input") input = v;
    else if (k == "arguments") arguments = v;
    else if (k == "output") out = v;
    else if (k == "item") it = v;
    return true;
  });

  if (type == "custom_tool_call" || type == "function_call") {
    // Kept until the output says whether anything is left running.
    BackgroundTasks::Call c;
    c.kind = "shell";
    c.at_ms = at;
    c.offset = offset;
    if (name == "exec" && input.is_string()) {
      c.what = first_command(text_of(input));
    } else if (name == "wait") {
      std::string args = arguments.is_string() ? text_of(arguments) : std::string(arguments.raw);
      js::scan_object(args, [&](std::string_view k, const js::Value& v) {
        if (k == "cell_id") c.what = "cell " + (v.is_string() ? text_of(v) : std::string(v.raw));
        return true;
      });
      c.kind = "wait";
    } else {
      return;
    }
    if (!call_id.empty()) t.calls[call_id] = std::move(c);
    return;
  }

  if (type == "custom_tool_call_output" || type == "function_call_output") {
    const auto found = t.calls.find(call_id);
    if (found == t.calls.end()) return;
    const BackgroundTasks::Call c = std::move(found->second);
    t.calls.erase(found);
    const std::string text = result_text(out);
    if (c.kind == "wait") {
      if (text.starts_with("Script completed") || text.starts_with("Script failed")) t.end(c.what);
    } else {
      constexpr std::string_view kRunning = "Script running with cell ID ";
      if (text.starts_with(kRunning)) {
        const size_t from = kRunning.size();
        const std::string cell = text.substr(from, text.find_first_of(" \n", from) - from);
        t.start("cell " + cell, c);
      }
    }
    each_chunk(text, [&](const std::string& session, bool exited) {
      const std::string id = "process " + session;
      // Codex can record the command's end before the output that saw it
      // still running.
      if (exited || t.calls.count("ended " + id)) {
        t.end(id);
      } else {
        BackgroundTasks::Call p = c;
        p.kind = "shell";
        if (c.kind == "wait") p.what = id;
        t.start(id, p);
      }
    });
    return;
  }

  if (it.is_object()) {
    std::string itype, process;
    js::scan_object(it.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") itype = text_of(v);
      else if (k == "process_id") process = text_of(v);
      return true;
    });
    if (itype == "CommandExecution" && !process.empty()) {
      t.end("process " + process);
      t.calls["ended process " + process].kind = "ended";
    }
  }
}

}  // namespace mico
