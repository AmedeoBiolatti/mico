#include "adapters/claude/claude.h"

#include "adapters/changes.h"
#include "base/path.h"

namespace mico {

using namespace changes;

bool ClaudeAdapter::may_have_changes(std::string_view raw) const {
  return has(raw, "\"toolUseResult\":{") && has(raw, "\"filePath\"");
}

// A tool result whose record carries toolUseResult with the file and its
// structuredPatch (Edit, MultiEdit, Write over a file) or the content of a file
// Write created.
void ClaudeAdapter::read_changes(std::string_view raw, std::string_view cwd, bool text, std::vector<LineChanges>& out) const {
  js::Value result{}, message{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "toolUseResult") result = v;
    else if (k == "message") message = v;
    return true;
  });
  if (!result.is_object()) return;
  std::string file, type, content;
  js::Value patch{};
  js::scan_object(result.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "filePath") file = str(v);
    else if (k == "type") type = str(v);
    else if (k == "structuredPatch") patch = v;
    else if (k == "content" && v.is_string()) content = str(v);
    return true;
  });
  if (file.empty() || (!patch.is_array() && type != "create")) return;
  LineChanges lc;
  // The call it answers, for where to open the chat.
  js::Value content_v{};
  if (message.is_object())
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "content") content_v = v;
      return true;
    });
  js::scan_array(content_v.raw, [&](const js::Value& b) {
    js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "tool_use_id") lc.result_of = str(v);
      else if (k == "is_error") lc.failed = v.is_true();
      return true;
    });
    return lc.result_of.empty();
  });
  FileChange fc;
  fc.file = resolve_path("/", file);
  fc.op = type == "create" ? EditOp::Create : type == "update" ? EditOp::Write : EditOp::Edit;
  Sink sk{fc, text};
  bool hunks = false;
  js::scan_array(patch.raw, [&](const js::Value& h) {
    int o = 0, n = 0;
    js::Value lines{};
    js::scan_object(h.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "oldStart") o = num(v);
      else if (k == "newStart") n = num(v);
      else if (k == "lines") lines = v;
      return true;
    });
    sk.hunk(o, n, "");
    js::scan_array(lines.raw, [&](const js::Value& lv) {
      const std::string l = str(lv);
      if (l.empty()) sk.line(' ', "");
      else if (l[0] == '+' || l[0] == '-' || l[0] == ' ') sk.line(l[0], std::string_view(l).substr(1));
      return true;
    });
    hunks = true;
    return true;
  });
  if (!hunks && type == "create") sk.all(content, '+');
  lc.changes.push_back(std::move(fc));
  out.push_back(std::move(lc));
}

}  // namespace mico
