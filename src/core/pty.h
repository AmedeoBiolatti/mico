#pragma once
#include <sys/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace mico {

// Removes the variables that would make a spawned agent think it is a nested
// session of whatever launched mico. Call in the child, before exec.
void scrub_agent_env();

// A child process on a pseudo-terminal. mico spawns agents; it never tries to
// attach to one already running, which would need ptrace and would break the
// "visibility layer, not a harness" line.
class Pty {
 public:
  ~Pty();
  Pty() = default;
  Pty(const Pty&) = delete;
  Pty& operator=(const Pty&) = delete;

  bool spawn(const std::vector<std::string>& argv, const std::string& cwd, int w, int h);
  // Non-empty after a spawn that could not even exec (binary not on PATH, or
  // the working directory is gone). Distinct from a process that ran and then
  // exited on its own.
  const std::string& spawn_error() const { return spawn_error_; }
  int fd() const { return fd_; }
  pid_t pid() const { return pid_; }

  // Appends whatever is readable without blocking. Returns false at EOF.
  bool read_available(std::string& out);
  void write(std::string_view bytes);
  void flush_input();
  // Input the child has not taken yet (a large paste into a full pty).
  bool has_pending_input() const { return !pending_input_.empty(); }
  void resize(int w, int h);

  // Reaps the child if it has exited. Cheap; safe to call every frame.
  void poll_exit();
  bool exited() const { return exited_; }
  int exit_status() const { return status_; }
  void terminate();
  // Release a reaped child before restarting it. Never stops a running child.
  bool reset_exited();

 private:
  int fd_ = -1;
  pid_t pid_ = -1;
  bool exited_ = false;
  int status_ = 0;
  std::string spawn_error_;
  std::string pending_input_;
};

}  // namespace mico
