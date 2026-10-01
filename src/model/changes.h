#pragma once
#include <cstdint>
#include <string>
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
inline const char* edit_op_name(EditOp op) {
  switch (op) {
    case EditOp::Edit: return "edited";
    case EditOp::Create: return "created";
    case EditOp::Write: return "rewrote";
    case EditOp::Delete: return "deleted";
  }
  return "edited";
}

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

}  // namespace mico
