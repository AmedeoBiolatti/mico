#pragma once
#include <signal.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Running a short-lived program and reading what it prints: git, a clipboard
// tool, tailscale. The daemon has threads, so everything a child needs is made
// before the fork, and the child only execs.
namespace mico::proc {

// Where `name` runs from: itself when it holds a slash and is executable,
// otherwise the first executable of that name on PATH. Empty when none is.
std::string find_program(std::string_view name);

struct Options {
  size_t cap = 16u << 20;   // bytes of output kept; more cuts the program short
  int timeout_ms = 10'000;  // then it is stopped, and timed_out set
  int stop_signal = SIGTERM;
  // Called after each read: true when what has arrived is enough, and the
  // program can be let go (cut is then set, as for the cap).
  std::function<bool(const std::string&)> enough;
};

struct Result {
  std::string out;         // its standard output, up to the cap
  bool ran = false;        // it was found and started
  bool cut = false;        // stopped early: the cap, or enough
  bool timed_out = false;  // stopped at the deadline
  int exit_code = -1;      // when it exited by itself; -1 when killed by a signal
  // Ran, finished by itself in time, and said so with 0.
  bool ok() const { return ran && !cut && !timed_out && exit_code == 0; }
};

// Runs argv (argv[0] found with find_program) with stdin and stderr on
// /dev/null, and collects its standard output.
Result capture(const std::vector<std::string>& argv, const Options& opt = {});

}  // namespace mico::proc
