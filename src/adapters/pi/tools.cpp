#include "adapters/pi/pi.h"

#include <algorithm>
#include <cstdlib>

#include "adapters/tool_calls.h"
#include "base/time.h"

namespace mico {

using namespace tools;

// toolCall blocks in assistant messages, and toolResult
// messages that name the call.
void PiFamilyAdapter::read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const {
  if (raw.find("\"toolCall\"") == std::string_view::npos && raw.find("\"toolResult\"") == std::string_view::npos)
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
    sink.result(call_id, at, error);
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
    if (type == "toolCall" && !id.empty()) sink.call(id, at, offset, name, subject(arguments));
    return true;
  });
}

}  // namespace mico
