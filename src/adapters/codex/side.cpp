#include "adapters/codex/codex.h"
#include "base/json.h"

namespace mico {
namespace {
std::string text_of(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}
}  // namespace

// A fork that is never written down (--ephemeral), from the prompt on stdin
// ("-"). Read-only, no MCP servers, and low effort: a side question wants a
// quick answer, not the main task's depth.
std::vector<std::string> CodexAdapter::side_question_argv(std::string_view id) const {
  return {"codex", "exec", "-c", "sandbox_mode=\"read-only\"", "-c", "approval_policy=\"never\"",
          "-c", "mcp_servers={}", "-c", "model_reasoning_effort=\"low\"",
          "fork", std::string(id), "--ephemeral", "--json", "--skip-git-repo-check", "-"};
}

// --json: each finished item, then turn.completed (or turn.failed).
bool CodexAdapter::read_side_answer(std::string_view line, std::string& answer, std::string& error) const {
  std::string_view type;
  js::Value item{}, err{}, message{};
  js::scan_object(line, [&](std::string_view k, const js::Value& v) {
    if (k == "type") type = v.body();
    else if (k == "item") item = v;
    else if (k == "error") err = v;
    else if (k == "message") message = v;
    return true;
  });
  if ((type == "item.completed" || type == "item.updated") && item.is_object()) {
    std::string_view itype;
    js::Value text{};
    js::scan_object(item.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") itype = v.body();
      else if (k == "text") text = v;
      return true;
    });
    if (itype == "agent_message" && text.is_string()) answer = text_of(text);
    return false;
  }
  if (type == "turn.completed") {
    if (!answer.empty()) error.clear();
    return true;
  }
  if (type == "turn.failed" || type == "error") {
    std::string why = text_of(message);
    if (why.empty() && err.is_object())
      js::scan_object(err.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "message") why = text_of(v);
        return true;
      });
    // A passing error (a retried connection) is only kept in case it ends it.
    if (answer.empty() || type == "turn.failed") error = why.empty() ? "codex could not answer" : why;
    return type == "turn.failed";
  }
  return false;
}

}  // namespace mico
