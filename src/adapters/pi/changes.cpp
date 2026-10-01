#include "adapters/pi/pi.h"

#include "adapters/changes.h"
#include "base/path.h"

namespace mico {

using namespace changes;

bool PiFamilyAdapter::may_have_changes(std::string_view raw) const {
  return (has(raw, "\"toolName\":\"edit\"") && has(raw, "\"details\"")) ||
         (has(raw, "\"name\":\"write\"") && has(raw, "\"toolCall\""));
}

// An edit's result carries its diff; a write is read from the call, since its
// result says only that it worked.
void PiFamilyAdapter::read_changes(std::string_view raw, std::string_view cwd, bool text, std::vector<LineChanges>& out) const {
  js::Value message{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "message") message = v;
    return true;
  });
  if (!message.is_object()) return;
  std::string role, call_id, tool;
  bool error = false;
  js::Value details{}, content{};
  js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "role") role = str(v);
    else if (k == "toolCallId") call_id = str(v);
    else if (k == "toolName") tool = str(v);
    else if (k == "isError") error = v.is_true();
    else if (k == "details") details = v;
    else if (k == "content") content = v;
    return true;
  });
  if (role == "toolResult") {
    if (tool != "edit" || !details.is_object()) return;
    std::string patch, diff, path, op;
    js::scan_object(details.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "patch") patch = str(v);
      else if (k == "diff") diff = str(v);
      else if (k == "path") path = str(v);
      else if (k == "op") op = str(v);
      return true;
    });
    if (patch.empty() && diff.empty()) return;
    LineChanges lc;
    lc.result_of = call_id;
    lc.failed = error;
    FileChange fc;
    fc.op = op == "create" ? EditOp::Create : op == "delete" ? EditOp::Delete : EditOp::Edit;
    Sink sk{fc, text};
    std::string header;
    if (!patch.empty()) parse_unified(patch, sk, &header);
    else parse_numbered(diff, sk);
    fc.file = resolve_path(cwd, path.empty() ? header : path);
    lc.changes.push_back(std::move(fc));
    out.push_back(std::move(lc));
    return;
  }
  if (!content.is_array()) return;
  js::scan_array(content.raw, [&](const js::Value& b) {
    std::string_view type;
    std::string id, name;
    js::Value args{};
    js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") type = v.body();
      else if (k == "id") id = str(v);
      else if (k == "name") name = str(v);
      else if (k == "arguments") args = v;
      return true;
    });
    if (type != "toolCall" || name != "write" || id.empty() || !args.is_object()) return true;
    std::string path, body;
    js::scan_object(args.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "path" || k == "file_path") path = str(v);
      else if (k == "content") body = str(v);
      return true;
    });
    if (path.empty()) return true;
    LineChanges lc;
    lc.call_id = id;
    FileChange fc;
    fc.op = EditOp::Write;
    fc.file = resolve_path(cwd, path);
    Sink sk{fc, text};
    sk.all(body, '+');
    lc.changes.push_back(std::move(fc));
    out.push_back(std::move(lc));
    return true;
  });
}

}  // namespace mico
