#pragma once
#include <sys/types.h>

#include <string>
#include <vector>

// Each agent, and the daemon, in a systemd scope of its own.
//
// Without them everything mico starts shares the cgroup of the terminal that
// first started the daemon, and under memory pressure systemd-oomd kills a
// whole cgroup: one agent's runaway build took the daemon and every other
// agent with it. In scopes of their own, it takes only the agent whose work
// it was. Where there is no systemd user manager, or the setting is off,
// everything runs as before.
namespace mico::scope {

// Whether processes are put in scopes: the setting allows it, MICO_SCOPES is
// not 0, systemd-run is installed and a systemd user manager answered a
// trial run. The trial runs once, the first time it is asked.
bool available();

// `argv` run in a transient scope named `unit` (a ".scope" is added), or
// `argv` unchanged when scopes are not available. The command keeps its
// process: systemd-run moves itself into the scope, then becomes `argv`.
std::vector<std::string> wrap(const std::vector<std::string>& argv, const std::string& unit,
                              const std::string& description);

// A unit name from free text: what systemd allows ([A-Za-z0-9:_.-]), every
// other byte a '-'. "mico-" first.
std::string unit_name(const std::string& text);

// The setting: ~/.config/mico/scopes; on unless turned off.
bool enabled();
void set_enabled(bool on);

}  // namespace mico::scope
