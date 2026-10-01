#include "adapters/adapters.h"

#include "adapters/claude/claude.h"
#include "adapters/codex/codex.h"
#include "adapters/pi/pi.h"

namespace mico {
namespace {

const ClaudeAdapter g_claude;
const CodexAdapter g_codex;
const PiAdapter g_pi;
const OmpAdapter g_omp;

// For a command mico has no adapter for: it runs in a pane, and is judged by
// the defaults.
class PlainAdapter final : public Adapter {
 public:
  std::string_view id() const override { return {}; }
  void parse(std::string_view, Arena&, std::vector<Event>&) const override {}
};
const PlainAdapter g_plain;

}  // namespace

const Adapter& claude_adapter() { return g_claude; }
const Adapter& codex_adapter() { return g_codex; }
const Adapter& pi_adapter() { return g_pi; }
const Adapter& omp_adapter() { return g_omp; }
const Adapter& plain_adapter() { return g_plain; }

const std::vector<const Adapter*>& all_adapters() {
  static const std::vector<const Adapter*> all{&g_claude, &g_codex, &g_pi, &g_omp};
  return all;
}

const Adapter* adapter_for(std::string_view agent) {
  for (const Adapter* a : all_adapters())
    if (a->id() == agent) return a;
  return nullptr;
}

}  // namespace mico
