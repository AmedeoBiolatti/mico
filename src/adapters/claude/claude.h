#pragma once
#include "adapters/adapter.h"

namespace mico {

// Claude Code. Transcripts are ~/.claude/projects/<cwd-slug>/<session-id>.jsonl,
// and claude takes the id it is to use, so a running session's transcript is
// found by name.
class ClaudeAdapter final : public Adapter {
 public:
  std::string_view id() const override { return "claude"; }
  std::string_view name() const override { return "Claude Code"; }
  std::string_view label() const override { return "Claude"; }

  // transcript.cpp
  void parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const override;
  void observe(std::string_view raw, SessionState& st) const override;
  void seed_state(SessionState& st) const override;

  // session.cpp
  void prepare(Launch& l, const LaunchExtras& x) const override;
  bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const override;
  bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const override;
  bool busy(const Liveness& l) const override;
  std::string startup_answer(const Vt& vt, bool* confirms) const override;
};

}  // namespace mico
