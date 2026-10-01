#include "adapters/claude/claude.h"
#include "base/text.h"

#include <algorithm>
#include <cstdlib>

#include "adapters/tool_calls.h"
#include "base/time.h"

namespace mico {

using namespace tools;

// tool_use blocks in assistant messages, tool_result blocks in
// user messages, each record stamped.
void ClaudeAdapter::read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const {
  // "tool_use" or "tool_result", in one scan.
  if (!text::contains(raw, "\"tool_"))
    return;
  int64_t at = 0;
  js::Value message{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "timestamp") at = parse_time(v.body());
    else if (k == "message") message = v;
    return true;
  });
  if (!message.is_object()) return;
  js::Value content{};
  js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "content") { content = v; return false; }
    return true;
  });
  if (!content.is_array()) return;
  js::scan_array(content.raw, [&](const js::Value& b) {
    if (!b.is_object()) return true;
    std::string_view type;
    std::string id, name, use_id;
    js::Value input{};
    bool error = false;
    js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") type = v.body();
      else if (k == "id") id = text_of(v);
      else if (k == "name") name = text_of(v);
      else if (k == "input") input = v;
      else if (k == "tool_use_id") use_id = text_of(v);
      else if (k == "is_error") error = v.is_true();
      return true;
    });
    if (type == "tool_use" && !id.empty()) sink.call(id, at, offset, name, subject(input));
    else if (type == "tool_result" && !use_id.empty()) sink.result(use_id, at, error);
    return true;
  });
}

}  // namespace mico
