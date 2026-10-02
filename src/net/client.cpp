#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/opener.h"
#include "core/scope.h"
#include "net/proto.h"
#include "base/log.h"
#include "term/term.h"
#include "ui/theme.h"

namespace mico {
namespace {

volatile sig_atomic_t g_resized = 0;
void on_winch(int) { g_resized = 1; }

int dial(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un addr;
  // A socket in a directory someone else controls could be anyone's daemon.
  if (!proto::socket_addr(path, &addr) || !proto::private_dir(proto::socket_dir(), false) ||
      connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  // Must be non-blocking: the receive path drains until EAGAIN, and on a
  // blocking socket that second read never returns, so a frame that has
  // already arrived is never drawn.
  if (int fl = fcntl(fd, F_GETFL, 0); fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  return fd;
}

// Double-forks a detached daemon so it outlives this client and any terminal.
// With `scoped`, in a systemd scope of its own: in this terminal's, it would
// go down whenever systemd-oomd kills the terminal for memory.
bool spawn_daemon(bool scoped) {
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n <= 0) return false;
  self[n] = '\0';
  // Made before the fork: the child only execs.
  std::vector<std::string> argv = {self, "--daemon"};
  if (scoped) argv = scope::wrap(argv, scope::unit_name("daemon-" + std::to_string(getpid())), "mico daemon");
  std::vector<char*> args;
  for (auto& a : argv) args.push_back(a.data());
  args.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    if (fork() != 0) _exit(0);  // orphan the grandchild onto init
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      dup2(null, STDOUT_FILENO);
      dup2(null, STDERR_FILENO);
      if (null > 2) close(null);
    }
    execv(args[0], args.data());
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);  // reap the intermediate fork immediately
  return true;
}

int connect_or_spawn(const std::string& path, bool allow_spawn) {
  if (int fd = dial(path); fd >= 0) return fd;
  if (!allow_spawn) return -1;
  MLOG("no daemon at %s, spawning one", path.c_str());
  // In a scope of its own when it can be; if that daemon never answers, one
  // started as before.
  const bool scoped = scope::available();
  for (int attempt = 0; attempt < (scoped ? 2 : 1); attempt++) {
    const bool in_scope = scoped && attempt == 0;
    if (!spawn_daemon(in_scope)) { MLOG("spawn_daemon failed"); return -1; }
    // The daemon has to bind before it can be reached; poll briefly rather
    // than guessing a fixed sleep.
    for (int i = 0; i < 200; i++) {
      usleep(15000);
      if (int fd = dial(path); fd >= 0) return fd;
    }
    if (in_scope) MLOG("the daemon started in a scope did not answer; starting one without");
  }
  return -1;
}

}  // namespace

int run_client(bool allow_spawn) {
  const std::string path = proto::socket_path();
  int fd = connect_or_spawn(path, allow_spawn);
  if (fd < 0) {
    fprintf(stderr, "mico: could not reach a daemon at %s\n", path.c_str());
    return 1;
  }

  if (!tty::enter_raw()) {
    fprintf(stderr, "mico: stdin/stdout is not a terminal (try --dump)\n");
    close(fd);
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);  // a daemon that dies must not kill us by signal
  tty::install_winch(on_winch);
  // The daemon's theme is compiled in, so the client can colour the margins.
  tty::write_all(STDOUT_FILENO, tty::init_seq(active_theme().bg));

  int w = 80, h = 24;
  tty::query_size(&w, &h);
  // The daemon draws for this terminal, so it needs to know what it can draw.
  std::string typed;
  GfxCaps caps = tty::probe_graphics(&typed);
  std::string out;
  proto::encode_size(proto::Type::Hello, w, h, caps, out);
  if (!typed.empty()) proto::encode(proto::Type::Input, typed, out);

  std::string in;
  bool done = false;
  // With VMIN=0 a zero-length read is indistinguishable from end-of-input on a
  // single call, so a real EOF is recognised by repetition. Treating the first
  // empty read as EOF makes the client exit before it has painted anything.
  int silent = 0;

  while (!done) {
    if (g_resized) {
      g_resized = 0;
      if (tty::query_size(&w, &h)) {
        tty::cell_pixels(&caps.cell_w, &caps.cell_h);
        proto::encode_size(proto::Type::Resize, w, h, caps, out);
      }
    }

    // Flush whatever is queued for the daemon before sleeping.
    while (!out.empty()) {
      ssize_t n = write(fd, out.data(), out.size());
      if (n > 0) { out.erase(0, size_t(n)); continue; }
      if (n < 0 && errno == EINTR) continue;
      break;
    }

    pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0},
                     {fd, short(POLLIN | (out.empty() ? 0 : POLLOUT)), 0}};
    // No timeout: stdin and the daemon wake this, and SIGWINCH interrupts it
    // (poll is never restarted after a signal, SA_RESTART or not).
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (fds[0].revents & POLLIN) {
      char buf[8192];
      ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
      if (n > 0) {
        // No decoding here: the terminal's bytes go to the daemon untouched.
        proto::encode(proto::Type::Input, std::string_view(buf, size_t(n)), out);
        silent = 0;
      } else if (n == 0) {
        if (++silent > 3) done = true;  // stdin really is gone
      }
    }

    if (fds[1].revents & (POLLIN | POLLHUP)) {
      char buf[65536];
      bool eof = false;
      for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) { in.append(buf, size_t(n)); continue; }
        if (n == 0) eof = true;
        else if (errno == EINTR) continue;
        break;
      }

      proto::Type t;
      std::string payload;
      while (proto::decode(in, &t, &payload)) {
        if (t == proto::Type::Frame) tty::write_all(STDOUT_FILENO, payload);
        else if (t == proto::Type::OpenUrl) open_url(payload);
        else if (t == proto::Type::Detach) done = true;
      }
      if (eof) done = true;
    }
    if (fds[1].revents & (POLLERR | POLLNVAL)) done = true;
  }

  proto::encode(proto::Type::Bye, {}, out);
  while (!out.empty()) {
    ssize_t n = write(fd, out.data(), out.size());
    if (n <= 0) break;
    out.erase(0, size_t(n));
  }

  tty::write_all(STDOUT_FILENO, tty::kFini);
  tty::leave_raw();
  close(fd);
  return 0;
}

// Asks a running daemon to exit. Agents it owns go with it, so this is the
// deliberate teardown, not something a detaching client ever does.
int kill_daemon() {
  const std::string path = proto::socket_path();
  int fd = dial(path);
  if (fd < 0) {
    fprintf(stderr, "mico: no daemon running\n");
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);
  std::string out;
  proto::encode(proto::Type::Kill, {}, out);
  tty::write_all(fd, out);
  // Wait for the far end to close, so the caller knows it is actually gone
  // rather than racing a still-shutting-down daemon.
  char buf[64];
  while (read(fd, buf, sizeof buf) > 0) {}
  close(fd);
  printf("mico: daemon stopped\n");
  return 0;
}

}  // namespace mico
