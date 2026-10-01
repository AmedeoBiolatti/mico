#pragma once
#include "adapters/adapter.h"
#include "adapters/screen.h"

namespace mico {

// OpenAI Codex. Rollouts are ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl.
// Codex cannot be told an id, so a new session's rollout is found after the
// fact: one this agent's processes hold open, that did not exist before.
class CodexAdapter final : public Adapter {
 public:
  std::string_view id() const override { return "codex"; }
  std::string_view name() const override { return "Codex"; }
  std::string_view label() const override { return "Codex"; }

  // transcript.cpp
  void parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const override;
  void observe(std::string_view raw, SessionState& st) const override;
  void seed_state(SessionState& st) const override;

  // tools.cpp
  void read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const override;

  // usage.cpp
  void read_usage(Jsonl& j, UsageEntry& e, UsageResume& r) const override;
  bool resumes_usage() const override { return true; }

  // changes.cpp
  bool may_have_changes(std::string_view raw) const override;
  void read_changes(std::string_view raw, std::string_view cwd, bool text,
                    std::vector<LineChanges>& out) const override;

  // list.cpp
  void list_sessions(const std::function<void(SessionRef&&)>& add) const override;

  // session.cpp
  void prepare(Launch& l, const LaunchExtras& x) const override;
  bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const override;
  void snapshot_transcripts(std::vector<std::string>& out) const override;
  bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const override;
  bool busy(const Liveness& l) const override;
  bool awaits_input(const Vt& vt) const override;
  bool tracks_turns() const override { return true; }
  int turn_marker(std::string_view head) const override;

  // screen.cpp
  bool permission_prompt(const Vt& vt, PermissionPrompt& out) const override;
  std::string screen_reply(const Vt& vt) const override;

  // commands.cpp
  std::vector<SlashCommand> builtin_commands() const override;
  ChipControl chip_control(std::string_view key) const override;
};

// Reading its screen (screen.cpp).

// Codex's "Working (12s • esc to interrupt)" line, just above its input box.
// Only there: the conversation higher up can quote the same words.
bool screen_shows_codex_activity(const Vt& vt);
// Codex's dialogs (a command to approve, the folder trust question) take the
// place of its input box and lead the focused choice with its "›" and a
// number. The input box leads with the same glyph and no number.
bool screen_awaits_codex_input(const Vt& vt);

// Codex's approval dialog in the same terms: "Would you like to run the
// following command?" as the question, what it is about (the reason, "$ cmd")
// as the heading, and the choices without the key each answers to ("(y)").
// Codex offers no Tab to amend: a note goes as a message after the answer.
// Also its "Implement this plan?" (plan = true), each choice with a
// description beside it and possibly disabled.
bool parse_codex_permission_prompt(const Vt& vt, PermissionPrompt& out);

}  // namespace mico
