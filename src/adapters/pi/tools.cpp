#include "adapters/pi/pi.h"
#include "base/text.h"

#include <algorithm>
#include <cstdlib>

#include "adapters/tool_calls.h"
#include "base/time.h"

namespace mico {

using namespace tools;

namespace {

// What a call was about, for the index: the shared pick, else the files an
// omp edit script names, else its eval code, else the intent ("i") omp states
// on every call.
std::string call_subject(const js::Value& args) {
  std::string s = subject(args);
  if (!s.empty() || !args.is_object()) return s;
  std::string intent;
  js::scan_object(args.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "input" && v.is_string()) s = edit_script_paths(text_of(v));
    else if (k == "code" && v.is_string()) s = text_of(v);
    else if (k == "i" && v.is_string()) intent = text_of(v);
    return s.empty();
  });
  if (s.empty()) s = std::move(intent);
  if (s.size() > 4096) s.resize(4096);
  return s;
}

}  // namespace

// toolCall blocks in assistant messages, and toolResult
// messages that name the call.
void PiFamilyAdapter::read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const {
  // "toolCall" or "toolResult", in one scan.
  if (!text::contains(raw, "\"tool"))
    return;
  int64_t at = 0;
  js::Value message{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "timestamp") at = parse_time(v.body());
    else if (k == "message") message = v;
    return true;
  });
  if (!message.is_object()) return;
  std::string role, call_id;
  bool error = false;
  js::Value content{};
  js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "role") role = text_of(v);
    else if (k == "toolCallId") call_id = text_of(v);
    else if (k == "isError") error = v.is_true();
    else if (k == "content") content = v;
    return true;
  });
  if (role == "toolResult") {
    sink.result(call_id, at, error, -1, sink.wants_output(call_id, {}) ? result_text(content) : std::string());
    return;
  }
  if (!content.is_array()) return;
  js::scan_array(content.raw, [&](const js::Value& b) {
    std::string_view type;
    std::string id, name;
    js::Value arguments{};
    js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") type = v.body();
      else if (k == "id") id = text_of(v);
      else if (k == "name") name = text_of(v);
      else if (k == "arguments") arguments = v;
      return true;
    });
    if (type == "toolCall" && !id.empty()) sink.call(id, at, offset, name, call_subject(arguments));
    return true;
  });
}

}  // namespace mico
