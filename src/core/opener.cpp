#include "core/opener.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace mico {

namespace {

// Twice forked, so what runs is nobody's child: no zombie to reap, and it
// outlives mico. Its output would land on the screen mico draws; it goes
// nowhere instead. Nothing passes through a shell.
bool spawn_detached(const std::vector<std::string>& argv) {
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    if (fork() != 0) _exit(0);
    setsid();
    const int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2) close(null);
    }
    execvp(args[0], args.data());
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return true;
}

}  // namespace

bool open_url(std::string_view url) {
  if (!(url.starts_with("https://") || url.starts_with("http://") || url.starts_with("file://") ||
        url.starts_with("mailto:")))
    return false;
  for (unsigned char c : url)
    if (c < 0x21 || c == 0x7F) return false;
#ifdef __APPLE__
  return spawn_detached({"open", std::string(url)});
#else
  return spawn_detached({"xdg-open", std::string(url)});
#endif
}

bool notify_desktop(std::string_view title, std::string_view body) {
#ifdef __APPLE__
  // AppleScript string literals: only the quote and the backslash need escaping.
  const auto quoted = [](std::string_view s) {
    std::string q = "\"";
    for (const char c : s) {
      if (c == '"' || c == '\\') q.push_back('\\');
      q.push_back(c == '\n' ? ' ' : c);
    }
    return q + "\"";
  };
  return spawn_detached({"osascript", "-e",
                         "display notification " + quoted(body) + " with title " + quoted(title)});
#else
  if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) return false;
  return spawn_detached({"notify-send", "--app-name=mico", "--", std::string(title), std::string(body)});
#endif
}

}  // namespace mico
