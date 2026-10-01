#include "adapters/codex/codex.h"

#include "adapters/changes.h"
#include "base/path.h"

namespace mico {

using namespace changes;

bool CodexAdapter::may_have_changes(std::string_view raw) const {
  // Only a completed item (FileChange) or an apply_patch call holds changes:
  // the head's payload type says which a record is, when it has one.
  const std::string_view pt = codex_payload_type(raw);
  if (pt == "item_completed") return has(raw, "\"FileChange\"");
  if (pt == "custom_tool_call") return has(raw, "\"apply_patch\"");
  if (!pt.empty()) return false;
  return has(raw, "\"FileChange\"") || (has(raw, "\"apply_patch\"") && has(raw, "\"custom_tool_call\""));
}

void CodexAdapter::read_changes(std::string_view raw, std::string_view cwd, bool text, std::vector<LineChanges>& out) const {
  js::Value payload{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "payload") payload = v;
    return true;
  });
  if (!payload.is_object()) return;
  std::string_view ptype;
  std::string name, call_id, input;
  js::Value item{};
  js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "type") ptype = v.body();
    else if (k == "item") item = v;
    else if (k == "name") name = str(v);
    else if (k == "call_id") call_id = str(v);
    else if (k == "input") input = str(v);
    return true;
  });
  if (ptype == "item_completed" && item.is_object()) {
    std::string_view itype;
    std::string status, id;
    js::Value changes{};
    js::scan_object(item.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") itype = v.body();
      else if (k == "id") id = str(v);
      else if (k == "status") status = str(v);
      else if (k == "changes") changes = v;
      return true;
    });
    if (itype != "FileChange" || !changes.is_object()) return;
    LineChanges lc;
    // Named after the apply_patch call it records, when there is one.
    lc.result_of = id;
    lc.failed = status == "failed" || status == "declined";
    js::scan_object(changes.raw, [&](std::string_view path, const js::Value& c) {
      std::string type, diff, content, moved;
      js::scan_object(c.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "type") type = str(v);
        else if (k == "unified_diff") diff = str(v);
        else if (k == "content") content = str(v);
        else if (k == "move_path") moved = str(v);
        return true;
      });
      FileChange fc;
      std::string p;
      js::unescape_append(path, p);
      fc.file = resolve_path(cwd, p);
      if (!moved.empty()) fc.moved_to = resolve_path(cwd, moved);
      Sink sk{fc, text};
      if (type == "add") {
        fc.op = EditOp::Create;
        sk.all(content, '+');
      } else if (type == "delete") {
        fc.op = EditOp::Delete;
        sk.all(content, '-');
      } else {
        parse_unified(diff, sk);
      }
      lc.changes.push_back(std::move(fc));
      return true;
    });
    if (!lc.changes.empty()) out.push_back(std::move(lc));
    return;
  }
  // Older rollouts: the patch as the model wrote it, its result a separate line.
  if (ptype == "custom_tool_call" && name == "apply_patch" && !call_id.empty()) {
    LineChanges lc;
    lc.call_id = call_id;
    parse_apply_patch(input, cwd, text, lc.changes);
    if (!lc.changes.empty()) out.push_back(std::move(lc));
  }
}

}  // namespace mico
