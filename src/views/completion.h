#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/commands.h"
#include "ui/picker.h"

namespace mico {

// The word being typed that a menu completes: a "/command" opening the
// message, or an "@path" anywhere in it.
struct Trigger {
  char kind = 0;     // '/', '@', or 0 for none
  size_t from = 0;   // byte offset of the '/' or '@'
  size_t end = 0;    // the end of the word, which may run past the cursor
  std::string query; // what follows the trigger, up to the cursor
};
Trigger find_trigger(std::string_view text, size_t cursor);

// Indices into `paths` matching `query`, best first, at most `max`: a match
// in the file's own name outranks one spread over its folders, and the
// shorter path wins a tie. An empty query lists the top level, folders first.
std::vector<int> rank_paths(const std::vector<std::string>& paths, std::string_view query, size_t max);

// The "/" and "@" menus of a prompt box: which one is open, what it lists,
// and what taking an item puts in the box. The box itself stays the pane's;
// this reads it after every change and never edits it.
class Completion {
 public:
  Completion();

  // Re-reads the box. `allowed` is false while the box holds something other
  // than a message (a note) or the pane is not showing chat.
  void update(std::string_view text, size_t cursor, bool allowed);
  // The lists, set by the pane before update() when a menu of that kind is
  // open. Each comes with a version, so a list is rebuilt only when it moved.
  void set_commands(const std::vector<SlashCommand>* cmds, uint64_t version);
  void set_files(const std::vector<std::string>* paths, uint64_t version);

  const Trigger& trigger() const { return trig_; }
  bool visible() const;
  Picker& picker() { return pick_; }
  // Escape: closed until the word it was for is gone.
  void dismiss();

  struct Take {
    std::string text;       // what replaces the word
    bool send = false;      // a command that needs nothing more: run it
    bool keep_open = false; // a folder: the menu goes on inside it
  };
  // Taking the item under the cursor; empty text when there is none.
  Take take() const;

 private:
  void rebuild();

  Picker pick_;
  Trigger trig_;
  std::string dismissed_;  // kind + offset of the word Escape closed
  const std::vector<SlashCommand>* cmds_ = nullptr;
  const std::vector<std::string>* files_ = nullptr;
  uint64_t cmds_version_ = 0, files_version_ = 0;
  // What the current items were built from, so a keystroke that changes
  // nothing does not rebuild them.
  char built_kind_ = 0;
  uint64_t built_version_ = 0;
  std::string built_query_;
};

}  // namespace mico
