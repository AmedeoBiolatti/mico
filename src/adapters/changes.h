#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "model/changes.h"

// Reading file changes out of transcripts: the diff and patch formats several
// agents write. Each adapter's read_changes() picks its records and hands
// their payloads to these.
namespace mico::changes {

// A JSON string's text, unescaped; empty for anything else.
std::string str(const js::Value& v);
// A JSON number as an int; 0 for anything else.
int num(const js::Value& v);

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
bool hunk_header(std::string_view l, int& o, int& n, std::string_view& rest);
// A unified diff. The "--- a" / "+++ b" header, when there is one, names the
// file into `header_file`.
void parse_unified(std::string_view diff, Sink& sk, std::string* header_file = nullptr);
// pi's and omp's display diffs: each line marked, then numbered — " 17 text",
// "+ 21 text", "-135 text" (pi) or "+45|text" (omp) — with "..." or an empty
// line where lines were left out.
void parse_numbered(std::string_view diff, Sink& sk);
// An apply_patch envelope: "*** Update File: a" and its hunks, "*** Add
// File: b" and its lines, "*** Delete File: c", each file one change.
void parse_apply_patch(std::string_view patch, std::string_view cwd, bool text, std::vector<FileChange>& out);
// True when `raw` contains `needle`.
bool has(std::string_view raw, std::string_view needle);

}  // namespace mico::changes
