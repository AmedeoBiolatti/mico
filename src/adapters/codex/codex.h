#pragma once
#include "adapters/adapter.h"

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

  // session.cpp
  void prepare(Launch& l, const LaunchExtras& x) const override;
  bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const override;
  void snapshot_transcripts(std::vector<std::string>& out) const override;
  bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const override;
  bool busy(const Liveness& l) const override;
  bool awaits_input(const Vt& vt) const override;
  bool tracks_turns() const override { return true; }
  int turn_marker(std::string_view head) const override;
};

}  // namespace mico
