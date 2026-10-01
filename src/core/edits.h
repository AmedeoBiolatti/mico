#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mico {

// What the agents changed in files, as their transcripts record it: Claude's
// structuredPatch, Codex's FileChange items and apply_patch calls, pi's and
// omp's edit results and write calls. Each already carries the diff, so mico
// never has to compare files itself.

enum class EditOp : uint8_t {
  Edit,    // lines changed in place
  Create,  // a new file
  Write,   // a whole file written over
  Delete,
};
const char* edit_op_name(EditOp op);

struct DiffLine {
  char kind = ' ';             // ' ' context, '+' added, '-' removed, '@' a hunk's start
  int old_no = 0, new_no = 0;  // 1-based; 0 when the agent did not say
  std::string text;            // for '@', what the hunk header said after its numbers
};

struct FileChange {
  std::string file;      // absolute; empty when the line does not name it
  std::string moved_to;  // a rename's new name
  EditOp op = EditOp::Edit;
  int added = 0, removed = 0;
  std::vector<DiffLine> lines;  // only when read with text
};

struct LineChanges {
  std::vector<FileChange> changes;
  // A call's changes: they happened only if its result says so.
  std::string call_id;
  // A result's changes: the call they answer, which may name the file, and
  // whose own record of them they replace.
  std::string result_of;
  bool failed = false;
};

// The file changes one transcript line records, if any, a group per call.
// `cwd` resolves the relative paths some agents write; `text` false counts
// lines without keeping them, which is what an index over every chat wants.
std::vector<LineChanges> read_changes(std::string_view agent, std::string_view raw, std::string_view cwd, bool text);

// Cheap test for a line that may hold changes, before any parsing.
bool may_have_changes(std::string_view agent, std::string_view raw);

// Reads the line at `offset` of a transcript again and returns its change to
// `file` with its lines. False when the file moved on or the line is gone.
bool load_change(const std::string& transcript, std::string_view agent, std::string_view cwd, uint64_t offset,
                 const std::string& file, FileChange& out);

// `p` made absolute against `cwd`, with "./" and doubled slashes taken out.
std::string resolve_path(std::string_view cwd, std::string_view p);

}  // namespace mico
