#pragma once
#include <string>
#include <vector>

#include "adapters/screen.h"

// How an agent's own dialogs are answered from outside its terminal: the keys
// that walk its cursor to a choice and confirm it. Shared by the TUI's panel
// and the state protocol, so a phone answers exactly as the terminal does.
namespace mico {

// Where claude offers "Tab to amend", the first choice ("Yes") and a last
// "No" take a line for claude: an instruction, or the reason for refusing.
bool permission_takes_note(const PermissionPrompt& p, int choice);

struct PermissionAnswer {
  std::vector<std::string> steps;  // keys, one per step, paced by the session
  std::string after;               // a note that had no room in the dialog, sent as a message after
};
// The keys for choice `i` of `p`, with `note` typed into the choice when it
// takes one. False when `i` is no choice, or one the dialog shows as off.
bool permission_answer(const PermissionPrompt& p, int i, const std::string& note, PermissionAnswer& out);

}  // namespace mico
