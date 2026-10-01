#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/path.h"
#include "model/changes.h"

namespace mico {

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


}  // namespace mico
