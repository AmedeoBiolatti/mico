#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "model/state.h"

namespace mico {

class LiveSession;

// One choice a state chip offers: what the terminal's chip picker lists and
// what a protocol client is sent. `action` says what picking it does:
//   chipset:<line>                 the agent's set command ("/model fable")
//   chipsteps:<key>|<value>|<keys> keys through the agent's menus, \x1f apart
//   chipmode:<key>|<value>|<keys>  a ring walked with one key (Shift+Tab)
//   chipcmd:<command>              open the agent's own picker
//   chipcopy:<key>                 copy the value
// Empty for a row that only says something.
struct ChipChoice {
  std::string label;
  std::string detail;  // the agent's description of it
  std::string hint;    // the exact value, when the label is a readable name
  std::string action;
  bool enabled = true;
  bool checked = false;  // the value in use
  bool pinned = false;   // offered whatever is typed: "copy value"
};

// The choices for state field `key` of a chat of `agent`: the values mico can
// set (the current one checked), or the agent's own picker, plus "copy value".
// `live` enables the actions that talk to a running agent; a stored transcript
// can only be read from. `cursor` gets the current value's index, `title` the
// chip's label and value.
std::vector<ChipChoice> chip_choices(const SessionState& st, const std::string& key, bool live,
                                     const std::string& agent, int* cursor = nullptr,
                                     std::string* title = nullptr);

enum class ChipApplied { Done, Busy, Unsupported };
// Sends a chipset, chipsteps or chipmode action to `s`. On Done, `key` and
// `value` name the field it set, for showing the choice before the agent
// writes it into its transcript (empty when the action names none). Busy when
// keys are still being delivered; Unsupported for any other action, which
// only a terminal can carry out.
ChipApplied apply_chip_choice(LiveSession& s, std::string_view action, std::string* key, std::string* value);

}  // namespace mico
