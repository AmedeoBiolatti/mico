#include "adapters/changes.h"

#include <cstdlib>
#include <cstring>

#include "base/path.h"
#include "base/text.h"

namespace mico::changes {

std::string str(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}

int num(const js::Value& v) { return v.type == js::Type::Number ? std::atoi(std::string(v.raw).c_str()) : 0; }


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
void parse_unified(std::string_view diff, Sink& sk, std::string* header_file) {
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

bool has(std::string_view raw, std::string_view needle) { return text::contains(raw, needle); }

}  // namespace mico::changes
