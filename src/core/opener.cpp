#include "core/opener.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace mico {

bool open_url(std::string_view url) {
  if (!(url.starts_with("https://") || url.starts_with("http://") || url.starts_with("file://") ||
        url.starts_with("mailto:")))
    return false;
  for (unsigned char c : url)
    if (c < 0x21 || c == 0x7F) return false;
  const std::string arg(url);
#ifdef __APPLE__
  const char* tool = "open";
#else
  const char* tool = "xdg-open";
#endif
  // Twice forked, so the browser is nobody's child: no zombie to reap, and it
  // outlives mico. Its output would land on the screen mico draws; it goes
  // nowhere instead.
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
    }
    execlp(tool, tool, arg.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return true;
}

}  // namespace mico
