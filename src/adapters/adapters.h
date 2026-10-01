#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "model/adapter.h"

namespace mico {

// The adapters mico ships with, one per agent. pi and omp share a parser but
// are separate agents, so each has its own entry.
const Adapter& claude_adapter();
const Adapter& codex_adapter();
const Adapter& pi_adapter();
const Adapter& omp_adapter();

// The adapter for an agent named as its command is ("claude", "codex", "pi",
// "omp"), or null for one mico has no adapter for: that agent still gets a
// pane, just no chat view.
const Adapter* adapter_for(std::string_view agent);

// Codex's reply to an optional question (see is_async_question_tool) is an
// envelope. One answer in it:
//   <send_user_message_question_reply>
//   [{"answer":…,"question":…,"questionItemId":"[\"request_user_input_async\",\"<call>\",0]"}]
//   </send_user_message_question_reply>
// Codex matches questionItemId byte for byte to clear its own pending
// question, so it is written exactly as Codex writes it.
struct AsyncReply {
  std::string call_id;
  int index = 0;  // which of the call's questions
  std::string question, answer;
};
std::string async_reply_envelope(const std::vector<AsyncReply>& replies);
// The replies in a message that is such an envelope; false for any other.
bool parse_async_reply(std::string_view text, std::vector<AsyncReply>& out);

}  // namespace mico
