#include "adapters/adapters.h"

namespace mico {

const Adapter* adapter_for(std::string_view agent) {
  if (agent == "claude") return &claude_adapter();
  if (agent == "codex") return &codex_adapter();
  if (agent == "pi") return &pi_adapter();
  if (agent == "omp") return &omp_adapter();
  return nullptr;
}

}  // namespace mico
