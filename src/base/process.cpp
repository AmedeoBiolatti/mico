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
  if (argv.empty()) return r;
  const std::string bin = find_program(argv[0]);
  if (bin.empty()) return r;
  // Made before the fork: the child only execs.
  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return r;
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return r;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    const int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      dup2(null, STDERR_FILENO);
    }
    execv(bin.c_str(), args.data());
    _exit(127);
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

}  // namespace mico::proc
