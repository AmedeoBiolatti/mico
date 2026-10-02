#pragma once
#include <cstdarg>
#include <string>
#include <string_view>

// A tiny append-only log to a file, because mico owns the terminal and cannot
// use stderr. One line per event, timestamped, with a short tag for who wrote
// it ("daemon", "client", "local"). Path: $XDG_STATE_HOME/mico/mico.log, or
// ~/.local/state/mico/mico.log.
namespace mico::logs {

void init(const char* tag);          // safe to call more than once
void line(std::string_view text);
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
const std::string& path();

// What this thread is working on, for a crash to name: "reading", and a
// transcript's path. Both must outlive the work (a literal, a path held by
// the job); nothing is copied, so it is cheap enough to set around every
// session's turn of the loop. Null clears it.
void doing(const char* what, const char* detail = nullptr);
// Sets doing() for the life of the object, and puts back what was there.
class Doing {
 public:
  Doing(const char* what, const char* detail = nullptr);
  ~Doing();
  Doing(const Doing&) = delete;
  Doing& operator=(const Doing&) = delete;

 private:
  const char *what_, *detail_;
};

// From here on a fatal signal (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT) or
// an uncaught exception writes to the log what it was, what the crashing
// thread was doing, and a backtrace, before the process goes down as it
// would have. A SIGKILL leaves no such line: nothing can catch it.
void install_crash_handler();

}  // namespace mico::logs

#define MLOG(...) ::mico::logs::logf(__VA_ARGS__)
