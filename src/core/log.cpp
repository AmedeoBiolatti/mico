#include "core/log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mico::logs {
namespace {

int g_fd = -1;
std::string g_path;
std::string g_tag = "?";

std::string state_dir() {
  if (const char* x = getenv("XDG_STATE_HOME"); x && *x) return std::string(x) + "/mico";
  const char* h = getenv("HOME");
  return (h ? std::string(h) : ".") + "/.local/state/mico";
}

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
                        ? (O_WRONLY | O_CREAT | O_TRUNC)
                        : (O_WRONLY | O_CREAT | O_APPEND);
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

}  // namespace mico::logs
