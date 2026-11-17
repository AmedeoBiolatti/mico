#include "adapters/claude/claude.h"
#include "base/json.h"

namespace mico {
namespace {
std::string text_of(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}
}  // namespace

// Print mode, resuming the chat into a fork that is never written down. It
// may look things up but not change them, and starts no MCP server: a side
// question wants its answer, not a second session's setup.
std::vector<std::string> ClaudeAdapter::side_question_argv(std::string_view id) const {
  return {"claude", "-p", "--resume", std::string(id), "--fork-session", "--no-session-persistence",
          "--output-format", "stream-json", "--verbose", "--include-partial-messages",
          "--strict-mcp-config", "--effort", "low", "--tools", "Read,Grep,Glob"};
}

// stream-json: the reply arrives as text deltas, message by message (a
// message that ends in a tool call is followed by another), then a result.
bool ClaudeAdapter::read_side_answer(std::string_view line, std::string& answer, std::string& error) const {
  std::string_view type, subtype;
  js::Value event{}, result{};
  bool is_error = false;
  js::scan_object(line, [&](std::string_view k, const js::Value& v) {
    if (k == "type") type = v.body();
    else if (k == "subtype") subtype = v.body();
    else if (k == "event") event = v;
    else if (k == "result") result = v;
    else if (k == "is_error") is_error = v.is_true();
    return true;
  });
  if (type == "stream_event" && event.is_object()) {
    std::string_view etype;
    js::Value delta{};
    js::scan_object(event.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") etype = v.body();
      else if (k == "delta") delta = v;
      return true;
    });
    if (etype == "message_start") answer.clear();
    if (etype != "content_block_delta" || !delta.is_object()) return false;
    std::string_view dtype;
    js::Value text{};
    js::scan_object(delta.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") dtype = v.body();
      else if (k == "text") text = v;
      return true;
    });
    if (dtype == "text_delta" && text.is_string()) js::unescape_append(text.body(), answer);
    return false;
  }
  if (type != "result") return false;
  if (is_error || subtype != "success") {
    error = text_of(result);
    if (error.empty()) error = subtype.empty() ? "claude could not answer" : "claude: " + std::string(subtype);
  } else if (std::string final = text_of(result); !final.empty()) {
    answer = std::move(final);
  }
  return true;
}

std::string ClaudeAdapter::tool_description(std::string_view line, uint64_t tool_id) const {
  std::string out;
  js::Value message{};
  js::scan_object(line, [&](std::string_view k, const js::Value& v) {
    if (k == "message") { message = v; return false; }
    return true;
  });
  if (!message.is_object()) return out;
  js::Value content{};
  js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "content") { content = v; return false; }
    return true;
  });
  if (!content.is_array()) return out;
  js::scan_array(content.raw, [&](const js::Value& b) {
    if (!b.is_object()) return true;
    js::Value id{}, input{};
    js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "id") id = v;
      else if (k == "input") input = v;
      return true;
    });
    if (!id.is_string() || hash_id(id.body()) != tool_id || !input.is_object()) return true;
    js::scan_object(input.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "description") { out = text_of(v); return false; }
      return true;
    });
    return false;
  });
  return out;
}

}  // namespace mico
