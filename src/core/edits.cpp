#include "core/edits.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "base/json.h"

namespace mico {

const char* edit_op_name(EditOp op) {
  switch (op) {
    case EditOp::Edit: return "edited";
    case EditOp::Create: return "created";
    case EditOp::Write: return "rewrote";
    case EditOp::Delete: return "deleted";
  }
  return "edited";
}

std::string resolve_path(std::string_view cwd, std::string_view p) {
  if (p.empty()) return {};
  std::string full;
  if (p[0] == '/') {
    full = p;
  } else if (p.starts_with("~/")) {
    const char* h = getenv("HOME");
    full = std::string(h ? h : "") + std::string(p.substr(1));
  } else {
    full = std::string(cwd) + "/" + std::string(p);
  }
  std::vector<std::string_view> parts;
  std::string_view s = full;
  for (size_t i = 0; i <= s.size();) {
    size_t e = s.find('/', i);
    if (e == std::string_view::npos) e = s.size();
    const std::string_view part = s.substr(i, e - i);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    i = e + 1;
  }
  std::string out;
  for (std::string_view part : parts) out += "/" + std::string(part);
  return out.empty() ? "/" : out;
}

namespace {

std::string str(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}

int num(const js::Value& v) { return v.type == js::Type::Number ? std::atoi(std::string(v.raw).c_str()) : 0; }

// Calls fn(line) for each line of `s`; a final newline ends the last line
// rather than starting an empty one.
template <class F>
void each_line(std::string_view s, F&& fn) {
  size_t i = 0;
  while (i < s.size()) {
    size_t e = s.find('\n', i);
    if (e == std::string_view::npos) e = s.size();
    std::string_view l = s.substr(i, e - i);
    if (l.ends_with('\r')) l.remove_suffix(1);
    fn(l);
    i = e + 1;
  }
}

// Collects a change's lines, numbering them as it goes.
struct Sink {
  FileChange& fc;
  bool text;
  int old_no = 0, new_no = 0;

  void hunk(int o, int n, std::string_view rest) {
    old_no = o;
    new_no = n;
    if (text) fc.lines.push_back(DiffLine{'@', o, n, std::string(rest)});
  }
  // A gap in a diff that shows only part of the file.
  void gap() {
    old_no = new_no = 0;
    if (text && !fc.lines.empty() && fc.lines.back().kind != '@') fc.lines.push_back(DiffLine{'@', 0, 0, {}});
  }
  void line(char k, std::string_view t, int o = -1, int n = -1) {
    if (k == '+') fc.added++;
    else if (k == '-') fc.removed++;
    if (o >= 0) old_no = o;
    if (n >= 0) new_no = n;
    if (text) {
      DiffLine d{k, k == '+' ? 0 : old_no, k == '-' ? 0 : new_no, {}};
      // Tabs would be drawn as one cell; four spaces keep the indentation.
      d.text.reserve(t.size());
      for (char c : t) {
        if (c == '\t') d.text += "    ";
        else d.text += c;
      }
      fc.lines.push_back(std::move(d));
    }
    if (k != '+' && old_no) old_no++;
    if (k != '-' && new_no) new_no++;
  }
  // A whole file's content, every line added (or removed).
  void all(std::string_view content, char k) {
    old_no = k == '-' ? 1 : 0;
    new_no = k == '+' ? 1 : 0;
    each_line(content, [&](std::string_view l) { line(k, l); });
  }
};

// "@@ -12,5 +12,7 @@ rest": the numbers and what follows them. False when
// the header has no numbers (apply_patch writes "@@" or "@@ context").
bool hunk_header(std::string_view l, int& o, int& n, std::string_view& rest) {
  const size_t minus = l.find('-'), plus = l.find(" +");
  const size_t close = l.find("@@", 2);
  if (minus == std::string_view::npos || plus == std::string_view::npos || close == std::string_view::npos ||
      minus > plus || plus > close)
    return false;
  o = std::atoi(std::string(l.substr(minus + 1, plus - minus - 1)).c_str());
  n = std::atoi(std::string(l.substr(plus + 2, close - plus - 2)).c_str());
  rest = l.substr(close + 2);
  while (!rest.empty() && rest[0] == ' ') rest.remove_prefix(1);
  return true;
}

// A unified diff. The "--- a" / "+++ b" header, when there is one, names the
// file into `header_file`.
void parse_unified(std::string_view diff, Sink& sk, std::string* header_file = nullptr) {
  bool in_hunk = false;
  each_line(diff, [&](std::string_view l) {
    if (l.starts_with("@@")) {
      int o = 0, n = 0;
      std::string_view rest;
      if (hunk_header(l, o, n, rest)) sk.hunk(o, n, rest);
      else {
        rest = l.substr(2);
        while (!rest.empty() && rest[0] == ' ') rest.remove_prefix(1);
        sk.hunk(0, 0, rest);
      }
      in_hunk = true;
      return;
    }
    if (!in_hunk && (l.starts_with("--- ") || l.starts_with("+++ ") || l.starts_with("diff ") ||
                     l.starts_with("index "))) {
      if (header_file && l.starts_with("+++ ")) {
        std::string_view f = l.substr(4);
        if (f.starts_with("b/")) f.remove_prefix(2);
        const size_t tab = f.find('\t');
        if (tab != std::string_view::npos) f = f.substr(0, tab);
        if (f != "/dev/null") *header_file = f;
      } else if (header_file && header_file->empty() && l.starts_with("--- ")) {
        std::string_view f = l.substr(4);
        if (f.starts_with("a/")) f.remove_prefix(2);
        if (f != "/dev/null") *header_file = f;
      }
      return;
    }
    if (l.starts_with("\\ ") || l == "*** End of File") return;
    if (l.empty()) {
      sk.line(' ', "");
      return;
    }
    const char k = l[0];
    if (k == '+' || k == '-' || k == ' ') sk.line(k, l.substr(1));
  });
}

// pi's and omp's display diffs: each line marked, then numbered — " 17 text",
// "+ 21 text", "-135 text" (pi) or "+45|text" (omp) — with "..." or an empty
// line where lines were left out.
void parse_numbered(std::string_view diff, Sink& sk) {
  bool any = false;
  each_line(diff, [&](std::string_view l) {
    if (l.empty()) {
      if (any) sk.gap();
      return;
    }
    const char k = l[0];
    if (k != '+' && k != '-' && k != ' ') return;
    std::string_view rest = l.substr(1);
    size_t i = 0;
    while (i < rest.size() && rest[i] == ' ') i++;
    int no = 0;
    const size_t digits = i;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') no = no * 10 + (rest[i++] - '0');
    if (i == digits) {
      if (rest.substr(i).starts_with("...") && any) sk.gap();
      return;
    }
    if (i < rest.size() && (rest[i] == '|' || rest[i] == ' ')) i++;
    // The number shown is the old line's for a removal, the new one's else.
    sk.line(k, rest.substr(i), k == '-' ? no : 0, k == '-' ? 0 : no);
    any = true;
  });
}

// An apply_patch envelope: "*** Update File: a" and its hunks, "*** Add
// File: b" and its lines, "*** Delete File: c", each file one change.
void parse_apply_patch(std::string_view patch, std::string_view cwd, bool text, std::vector<FileChange>& out) {
  std::vector<FileChange>& v = out;
  size_t cur = size_t(-1);
  int old_no = 0, new_no = 0;
  const auto start = [&](EditOp op, std::string_view path) {
    FileChange fc;
    fc.op = op;
    fc.file = resolve_path(cwd, path);
    v.push_back(std::move(fc));
    cur = v.size() - 1;
    old_no = new_no = 0;
    if (op == EditOp::Create) new_no = 1;
  };
  each_line(patch, [&](std::string_view l) {
    if (l.starts_with("*** ")) {
      if (l.starts_with("*** Update File: ")) start(EditOp::Edit, l.substr(17));
      else if (l.starts_with("*** Add File: ")) start(EditOp::Create, l.substr(14));
      else if (l.starts_with("*** Delete File: ")) start(EditOp::Delete, l.substr(17));
      else if (l.starts_with("*** Move to: ") && cur < v.size()) v[cur].moved_to = resolve_path(cwd, l.substr(13));
      else if (l.starts_with("*** End Patch")) cur = size_t(-1);
      return;
    }
    if (cur >= v.size()) return;
    Sink s{v[cur], text, old_no, new_no};
    if (l.starts_with("@@")) {
      std::string_view rest = l.substr(2);
      while (!rest.empty() && rest[0] == ' ') rest.remove_prefix(1);
      s.hunk(0, 0, rest);
    } else if (l.empty()) {
      s.line(' ', "");
    } else if (l[0] == '+' || l[0] == '-' || l[0] == ' ') {
      s.line(l[0], l.substr(1));
    }
    old_no = s.old_no;
    new_no = s.new_no;
  });
}

// claude: a tool result whose record carries toolUseResult with the file
// and its structuredPatch (Edit, MultiEdit, Write over a file) or the
// content of a file Write created.
void claude(std::string_view raw, bool text, std::vector<LineChanges>& out) {
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

void codex(std::string_view raw, std::string_view cwd, bool text, std::vector<LineChanges>& out) {
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

// pi and omp: an edit's result carries its diff; a write is read from the
// call, since its result says only that it worked.
void pi(std::string_view raw, std::string_view cwd, bool text, std::vector<LineChanges>& out) {
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

// memmem: two-way search, far quicker than string_view::find on lines where
// the needle's first character, a quote, is on every other byte.
bool has(std::string_view raw, std::string_view needle) {
  return memmem(raw.data(), raw.size(), needle.data(), needle.size()) != nullptr;
}

}  // namespace

bool may_have_changes(std::string_view agent, std::string_view raw) {
  if (agent == "claude") return has(raw, "\"toolUseResult\":{") && has(raw, "\"filePath\"");
  if (agent == "codex")
    return has(raw, "\"FileChange\"") || (has(raw, "\"apply_patch\"") && has(raw, "\"custom_tool_call\""));
  if (agent == "pi" || agent == "omp")
    return (has(raw, "\"toolName\":\"edit\"") && has(raw, "\"details\"")) ||
           (has(raw, "\"name\":\"write\"") && has(raw, "\"toolCall\""));
  return false;
}

std::vector<LineChanges> read_changes(std::string_view agent, std::string_view raw, std::string_view cwd, bool text) {
  std::vector<LineChanges> out;
  if (!may_have_changes(agent, raw)) return out;
  if (agent == "claude") claude(raw, text, out);
  else if (agent == "codex") codex(raw, cwd, text, out);
  else if (agent == "pi" || agent == "omp") pi(raw, cwd, text, out);
  return out;
}

bool load_change(const std::string& transcript, std::string_view agent, std::string_view cwd, uint64_t offset,
                 const std::string& file, FileChange& out) {
  const int fd = open(transcript.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::string line;
  char buf[65536];
  uint64_t at = offset;
  for (;;) {
    const ssize_t n = pread(fd, buf, sizeof buf, off_t(at));
    if (n <= 0) break;
    const void* nl = memchr(buf, '\n', size_t(n));
    if (nl) {
      line.append(buf, size_t(static_cast<const char*>(nl) - buf));
      break;
    }
    line.append(buf, size_t(n));
    at += uint64_t(n);
    if (line.size() > (256u << 20)) break;
  }
  close(fd);
  for (LineChanges& lc : read_changes(agent, line, cwd, true))
    for (FileChange& c : lc.changes)
      if (c.file == file || c.file.empty()) {
        out = std::move(c);
        if (out.file.empty()) out.file = file;
        return true;
      }
  return false;
}

}  // namespace mico
