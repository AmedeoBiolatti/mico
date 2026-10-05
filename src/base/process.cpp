#include "base/process.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace mico::proc {
namespace {

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Runs `argv` with `in` as its stdin and `out` as its stdout, each /dev/null
// when -1, and stderr to /dev/null. -1 when it could not be started.
pid_t spawn(const std::vector<std::string>& argv, int in, int out) {
  if (argv.empty()) return -1;
  const std::string bin = find_program(argv[0]);
  if (bin.empty()) return -1;
  // Made before the fork: the child only execs.
  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  const pid_t pid = fork();
  if (pid != 0) return pid;
  const int null = open("/dev/null", O_RDWR);
  dup2(in >= 0 ? in : null, STDIN_FILENO);
  dup2(out >= 0 ? out : null, STDOUT_FILENO);
  dup2(null, STDERR_FILENO);
  execv(bin.c_str(), args.data());
  _exit(127);
}

}  // namespace

std::string find_program(std::string_view name) {
  if (name.empty()) return {};
  if (name.find('/') != std::string_view::npos) {
    const std::string p(name);
    return access(p.c_str(), X_OK) == 0 ? p : std::string();
  }
  const char* env = getenv("PATH");
  std::string_view dirs = env ? env : "/usr/local/bin:/usr/bin:/bin";
  while (true) {
    const size_t colon = dirs.find(':');
    const std::string_view dir = dirs.substr(0, colon);
    const std::string cand = (dir.empty() ? std::string(".") : std::string(dir)) + "/" + std::string(name);
    if (access(cand.c_str(), X_OK) == 0) return cand;
    if (colon == std::string_view::npos) return {};
    dirs.remove_prefix(colon + 1);
  }
}

Result capture(const std::vector<std::string>& argv, const Options& opt) {
  Result r;
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return r;
  const pid_t pid = spawn(argv, -1, fds[1]);
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return r;
  }
  close(fds[1]);
  r.ran = true;
  const int64_t deadline = now_ms() + opt.timeout_ms;
  char chunk[65536];
  for (;;) {
    const int64_t left = deadline - now_ms();
    pollfd p{fds[0], POLLIN, 0};
    const int ready = left <= 0 ? 0 : ::poll(&p, 1, int(left));
    if (ready < 0 && errno == EINTR) continue;
    if (ready == 0) {
      r.timed_out = true;
      break;
    }
    const ssize_t n = read(fds[0], chunk, sizeof chunk);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    r.out.append(chunk, size_t(n));
    if (r.out.size() >= opt.cap || (opt.enough && opt.enough(r.out))) {
      if (r.out.size() > opt.cap) r.out.resize(opt.cap);
      r.cut = true;
      break;
    }
  }
  close(fds[0]);
  if (r.cut || r.timed_out) kill(pid, opt.stop_signal);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
  return r;
}

int feed(const std::vector<std::string>& argv, std::string_view input, int timeout_ms) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return -1;
  const pid_t pid = spawn(argv, fds[0], -1);
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  close(fds[0]);
  // Written whole, then closed: the end of the input is what it waits for.
  // A reader that stops early (SIGPIPE is ignored by the daemon) ends this.
  const int64_t deadline = now_ms() + timeout_ms;
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
  bool late = false;
  for (size_t at = 0; at < input.size();) {
    const ssize_t n = write(fds[1], input.data() + at, input.size() - at);
    if (n > 0) {
      at += size_t(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) {
      const int64_t left = deadline - now_ms();
      pollfd p{fds[1], POLLOUT, 0};
      if (left > 0 && ::poll(&p, 1, int(left)) >= 0) continue;
      late = true;
    }
    break;
  }
  close(fds[1]);
  int status = 0;
  for (;;) {
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) break;
    if (got < 0 && errno != EINTR) return -1;
    if (late || now_ms() > deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      return -1;
    }
    timespec ts{0, 10'000'000};
    nanosleep(&ts, nullptr);
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace mico::proc
