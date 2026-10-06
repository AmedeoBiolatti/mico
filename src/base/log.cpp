#include "base/log.h"
#include "base/path.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

namespace mico::logs {
namespace {

int g_fd = -1;
std::string g_path;
std::string g_tag = "?";


void mkdirs(const std::string& d) {
  std::string acc;
  for (size_t i = 1; i <= d.size(); i++) {
    if (i == d.size() || d[i] == '/') {
      acc = d.substr(0, i);
      mkdir(acc.c_str(), 0700);
    }
  }
}

}  // namespace

void init(const char* tag) {
  if (tag && *tag) g_tag = tag;
  if (g_fd >= 0) return;

  const std::string dir = state_dir();
  mkdirs(dir);
  g_path = dir + "/mico.log";

  // Keep the file from growing without bound: start fresh once it passes ~4 MB.
  struct stat st{};
  const int flags = (stat(g_path.c_str(), &st) == 0 && st.st_size > (4 << 20))
                        ? (O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC)
                        : (O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC);
  g_fd = open(g_path.c_str(), flags, 0600);

  logf("---- %s started (pid %d) ----", g_tag.c_str(), int(getpid()));
}

void line(std::string_view text) {
  if (g_fd < 0) return;
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  tm tmv{};
  localtime_r(&ts.tv_sec, &tmv);

  char head[64];
  int n = snprintf(head, sizeof head, "%02d:%02d:%02d.%03d [%s] ", tmv.tm_hour, tmv.tm_min,
                   tmv.tm_sec, int(ts.tv_nsec / 1000000), g_tag.c_str());
  std::string out(head, size_t(n));
  out.append(text);
  out.push_back('\n');
  ssize_t w = write(g_fd, out.data(), out.size());
  (void)w;
}

void logf(const char* fmt, ...) {
  if (g_fd < 0) return;
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  line(std::string_view(buf, size_t(n) < sizeof buf ? size_t(n) : sizeof buf - 1));
}

const std::string& path() { return g_path; }

namespace {

thread_local const char* t_what = nullptr;
thread_local const char* t_detail = nullptr;
char g_exe[1024];  // this binary, for addr2line

// Only what a signal handler may call: write(2) and hand-made numbers.
void raw(const char* s) {
  if (g_fd >= 0 && s) {
    ssize_t w = write(g_fd, s, strlen(s));
    (void)w;
  }
}
void raw_num(unsigned long long v, int base = 10) {
  char b[32];
  int i = sizeof b;
  b[--i] = '\0';
  do {
    b[--i] = "0123456789abcdef"[v % unsigned(base)];
    v /= unsigned(base);
  } while (v && i > 0);
  raw(b + i);
}

const char* signal_name(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (a bad memory access)";
    case SIGBUS: return "SIGBUS (a bad memory access)";
    case SIGFPE: return "SIGFPE (an arithmetic fault)";
    case SIGILL: return "SIGILL (an illegal instruction)";
    case SIGABRT: return "SIGABRT (aborted)";
    default: return "a fatal signal";
  }
}

void on_fatal(int sig, siginfo_t* info, void*) {
  raw("---- CRASH [");
  raw(g_tag.c_str());
  raw("] pid ");
  raw_num(unsigned(getpid()));
  raw(" thread ");
  raw_num(unsigned(syscall(SYS_gettid)));
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);  // unix seconds: localtime() is no handler's to call
  raw(" at unix time ");
  raw_num(static_cast<unsigned long long>(ts.tv_sec));
  raw(": ");
  raw(signal_name(sig));
  if (info && info->si_code <= 0) {
    // Sent, not caused: kill(2) from another process.
    raw(", sent by pid ");
    raw_num(unsigned(info->si_pid));
  } else if (sig == SIGSEGV || sig == SIGBUS) {
    raw(" at 0x");
    raw_num(reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr), 16);
  }
  raw("\n    while: ");
  if (t_what) {
    raw(t_what);
    if (t_detail) {
      raw(" ");
      raw(t_detail);
    }
  } else {
    raw("(nothing named)");
  }
  raw("\n    backtrace (addr2line -Cfie ");
  raw(g_exe);
  raw(" <offset> names a frame, from debug info: the build's own mico, or mico.debug beside a release):\n");
  void* frames[64];
  const int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, g_fd);
  raw("---- end of crash ----\n");
  // Down as it would have gone: the handler was reset, so this is the default.
  raise(sig);
}

void on_terminate() {
  // An exception nobody caught: say what it was, then abort, whose handler
  // adds the backtrace (the stack is still that of the throw).
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      logf("---- uncaught exception %s: %s", typeid(x).name(), x.what());
    } catch (...) {
      logf("---- uncaught exception of an unknown type");
    }
  } else {
    logf("---- std::terminate called");
  }
  abort();
}

}  // namespace

void doing(const char* what, const char* detail) {
  t_what = what;
  t_detail = detail;
}

Doing::Doing(const char* what, const char* detail) : what_(t_what), detail_(t_detail) { doing(what, detail); }
Doing::~Doing() { doing(what_, detail_); }

void install_crash_handler() {
  const ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
  g_exe[n > 0 ? n : 0] = '\0';
  // backtrace() loads libgcc on its first call, which allocates: done here,
  // not first in a handler whose heap may be what broke.
  void* warm[1];
  backtrace(warm, 1);
  // A stack of its own, so a stack overflow can still be reported.
  static char alt[1 << 16];
  stack_t ss{};
  ss.ss_sp = alt;
  ss.ss_size = sizeof alt;
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_sigaction = on_fatal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(sig, &sa, nullptr);
  std::set_terminate(on_terminate);
}

}  // namespace mico::logs
