#include "adapters/codex/codex.h"
#include "base/text.h"

#include <algorithm>
#include <cstdlib>

#include "adapters/tool_calls.h"
#include "base/time.h"

namespace mico {

using namespace tools;

// Newer rollouts record each command with its duration and exit
// code; older ones a function call, and an output headed "Exit code: N /
// Wall time: X seconds".
std::string_view codex_payload_type(std::string_view raw) {
  const std::string_view head = raw.substr(0, 256);
  constexpr std::string_view kKey = "\"payload\":{\"type\":\"";
  const size_t at = head.find(kKey);
  if (at == std::string_view::npos) return {};
  const size_t from = at + kKey.size();
  const size_t end = head.find('"', from);
  return end == std::string_view::npos ? std::string_view() : head.substr(from, end - from);
}

void CodexAdapter::read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const {
  // Most records are no tool call. Told by the head's payload type when it
  // has one, rather than by scanning a line that can run to megabytes.
  if (const std::string_view pt = codex_payload_type(raw); !pt.empty()) {
    if (pt != "item_completed" && pt != "function_call" && pt != "function_call_output" &&
        pt != "custom_tool_call" && pt != "custom_tool_call_output")
      return;
  } else if (!text::contains(raw, "\"item_completed\"") && !text::contains(raw, "function_call") &&
             !text::contains(raw, "custom_tool_call")) {
    return;
  }
  // The payload is most of the record: stop at it rather than measure it, and
  // read it once, below.
  int64_t at = 0;
  std::string_view payload;
  js::scan_keys(raw, [&](std::string_view k, std::string_view rest) {
    if (k == "timestamp") at = parse_time(js::string_body(rest));
    else if (k == "payload") { payload = rest; return false; }
    return true;
  });
  if (!payload.starts_with('{')) return;
  std::string_view ptype;
  std::string name, call_id, output, input_text;
  js::Value it{}, args{};
  js::scan_object(payload, [&](std::string_view k, const js::Value& v) {
    if (k == "type") ptype = v.body();
    else if (k == "item") it = v;
    else if (k == "name") name = text_of(v);
    else if (k == "call_id") call_id = text_of(v);
    else if (k == "arguments") args = v;
    // Only its head is read, for an exit code and a time: not the whole of
    // what the command printed.
    else if (k == "output") output = v.is_string() ? js::string_prefix(v.raw, 1024) : std::string(v.raw.substr(0, 1024));
    else if (k == "input") input_text = text_of(v);
    return true;
  });

  if (ptype == "item_completed" && it.is_object()) {
    std::string_view itype;
    js::Value command{}, duration{}, parsed{}, changes{};
    int exit_code = 0;
    std::string status, server, tool;
    js::scan_object(it.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") itype = v.body();
      else if (k == "command") command = v;
      else if (k == "duration") duration = v;
      else if (k == "exit_code" && v.type == js::Type::Number) exit_code = std::atoi(std::string(v.raw).c_str());
      else if (k == "status") status = text_of(v);
      else if (k == "parsed_cmd") parsed = v;
      else if (k == "changes") changes = v;
      else if (k == "server") server = text_of(v);
      else if (k == "tool") tool = text_of(v);
      return true;
    });
    int64_t dur = -1;
    if (duration.is_object()) {
      int64_t secs = 0, nanos = 0;
      js::scan_object(duration.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "secs") secs = std::atoll(std::string(v.raw).c_str());
        else if (k == "nanos") nanos = std::atoll(std::string(v.raw).c_str());
        return true;
      });
      dur = secs * 1000 + nanos / 1000000;
    }
    const int64_t start = dur > 0 ? at - dur : at;
    if (itype == "CommandExecution") {
      std::string cmd;
      if (command.is_array()) {
        std::string obj = "{\"command\":" + std::string(command.raw) + "}";
        cmd = subject(js::Value{obj, js::Type::Object});
      } else {
        cmd = text_of(command);
      }
      // codex's own reading of the command: read / search / list_files.
      std::string hint;
      js::scan_array(parsed.raw, [&](const js::Value& p) {
        js::scan_object(p.raw, [&](std::string_view k, const js::Value& v) {
          if (k == "type") hint = text_of(v);
          return true;
        });
        return false;
      });
      sink.run(start, std::max<int64_t>(0, dur), offset, "exec", cmd, exit_code != 0 || status == "failed",
               hint == "read" || hint == "search" || hint == "list_files");
    } else if (itype == "McpToolCall") {
      sink.run(start, std::max<int64_t>(0, dur), offset, server + "." + tool, "", status == "failed");
    } else if (itype == "FileChange") {
      std::string files;
      int n = 0;
      js::scan_object(changes.raw, [&](std::string_view k, const js::Value&) {
        if (n++ < 3) files += (files.empty() ? "" : ", ") + std::string(basename(k));
        return true;
      });
      if (n > 3) files += ", +" + std::to_string(n - 3) + " more";
      sink.run(at, 0, offset, "FileChange", files, status == "failed");
    }
    return;
  }
  if (ptype == "function_call" && !call_id.empty()) {
    // Arguments arrive as a JSON string.
    std::string a = text_of(args);
    std::string cmd = a.empty() ? std::string() : subject(js::Value{a, js::Type::Object});
    if (name == "request_user_input_async") return;  // the agent does not wait on it
    sink.call(call_id, at, offset, name, cmd);
  } else if (ptype == "custom_tool_call" && !call_id.empty() && name == "apply_patch") {
    std::string files;
    for (size_t p = input_text.find("*** "); p != std::string::npos; p = input_text.find("*** ", p + 4)) {
      for (std::string_view tag : {"*** Update File: ", "*** Add File: ", "*** Delete File: "})
        if (input_text.compare(p, tag.size(), tag) == 0) {
          const size_t e = input_text.find('\n', p);
          const std::string f(basename(std::string_view(input_text).substr(p + tag.size(), e - p - tag.size())));
          if (files.find(f) == std::string::npos) files += (files.empty() ? "" : ", ") + f;
        }
    }
    sink.call(call_id, at, offset, "apply_patch", one_line(files));
  } else if ((ptype == "function_call_output" || ptype == "custom_tool_call_output") && !call_id.empty()) {
    // "Exit code: 1\nWall time: 2.5 seconds", or JSON with a metadata block.
    int64_t exact = -1;
    bool failed = false;
    if (const size_t e = output.find("Exit code: "); e != std::string::npos && e < 64)
      failed = std::atoi(output.c_str() + e + 11) != 0;
    if (const size_t w = output.find("Wall time: "); w != std::string::npos && w < 128)
      exact = int64_t(std::strtod(output.c_str() + w + 11, nullptr) * 1000);
    if (const size_t m = output.find("\"exit_code\":"); m != std::string::npos && m < 512)
      failed = std::atoi(output.c_str() + m + 12) != 0;
    if (const size_t d = output.find("\"duration_seconds\":"); d != std::string::npos && d < 512)
      exact = int64_t(std::strtod(output.c_str() + d + 19, nullptr) * 1000);
    // A patch that did not apply says so in words, with no exit code.
    if (output.find("verification failed") < 256 || output.starts_with("Failed to"))
      failed = true;
    sink.result(call_id, at, failed, exact);
  }
}

}  // namespace mico
