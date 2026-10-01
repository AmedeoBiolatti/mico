#include "adapters/adapter.h"

#include "vt/vt.h"

namespace mico {

void Adapter::prepare(Launch& l, const LaunchExtras& x) const {
  if (l.argv.empty()) l.argv = {l.agent};
}

bool Adapter::busy(const Liveness& l) const {
  // A turn produces output continuously; the gap only exceeds this when the
  // agent is sitting at its prompt.
  return l.quiet_ms < 1200;
}

bool Adapter::awaits_input(const Vt& vt) const { return screen_awaits_input(vt); }

}  // namespace mico
