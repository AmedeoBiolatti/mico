#include "core/clipboard.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "base/log.h"

namespace mico::clip {
namespace {

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool on_path(const char* tool) {
  const char* path = getenv("PATH");
  if (!path) return false;
  for (const char* p = path;;) {
    const char* end = strchr(p, ':');
    std::string dir(p, end ? size_t(end - p) : strlen(p));
    if (!dir.empty() && access((dir + "/" + tool).c_str(), X_OK) == 0) return true;
    if (!end) return false;
    p = end + 1;
  }
}

// Runs argv and collects its stdout, killing it at the deadline. A clipboard
// owner can take its time answering, or never answer at all.
bool run(const std::vector<const char*>& argv, int timeout_ms, std::string& out) {
  constexpr size_t kMaxBytes = 64u << 20;  // a screenshot is a few MB
  // Made before the fork: the child only execs.
  std::vector<const char*> args = argv;
  args.push_back(nullptr);
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return false;
  const pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
  if (pid == 0) {
    const int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    dup2(null, STDERR_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    if (null > 2) close(null);
    execvp(args[0], const_cast<char* const*>(args.data()));
    _exit(127);
  }
  close(fds[1]);
  out.clear();
  const int64_t deadline = now_ms() + timeout_ms;
  bool timed_out = false;
  char buf[65536];
  for (;;) {
    const int left = int(deadline - now_ms());
    if (left <= 0) { timed_out = true; break; }
    pollfd pf{fds[0], POLLIN, 0};
    if (poll(&pf, 1, left) <= 0) continue;
    const ssize_t n = ::read(fds[0], buf, sizeof buf);
    if (n > 0) {
      out.append(buf, size_t(n));
      if (out.size() > kMaxBytes) { timed_out = true; break; }
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;  // EOF
  }
  close(fds[0]);
  if (timed_out) kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  return !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// The tool that reads this desktop's clipboard, and the arguments for each
// question asked of it.
struct Tool {
  std::vector<const char*> types, text;
  std::vector<const char*> image(const char* mime) const {
    std::vector<const char*> v = image_prefix;
    v.push_back(mime);
    return v;
  }
  std::vector<const char*> image_prefix;
};

bool find_tool(Tool* t) {
  if (getenv("WAYLAND_DISPLAY") && on_path("wl-paste")) {
    *t = Tool{{"wl-paste", "--list-types"}, {"wl-paste", "--no-newline"}, {"wl-paste", "--type"}};
    return true;
  }
  if (getenv("DISPLAY") && on_path("xclip")) {
    *t = Tool{{"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o"},
              {"xclip", "-selection", "clipboard", "-o"},
              {"xclip", "-selection", "clipboard", "-o", "-t"}};
    return true;
  }
  return false;
}

// The formats agents accept, best first.
struct ImageType {
  const char* mime;
  const char* ext;
};
constexpr ImageType kImageTypes[] = {
    {"image/png", "png"}, {"image/jpeg", "jpg"}, {"image/webp", "webp"}, {"image/gif", "gif"}};

// $XDG_RUNTIME_DIR/mico/images: private to the user, in RAM, and cleared at
// logout, so screenshots do not pile up on disk.
std::string image_dir() {
  const char* rt = getenv("XDG_RUNTIME_DIR");
  std::string dir = rt && *rt ? std::string(rt) + "/mico" : "/tmp/mico-" + std::to_string(getuid());
  mkdir(dir.c_str(), 0700);
  dir += "/images";
  mkdir(dir.c_str(), 0700);
  struct stat st{};
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
      (st.st_mode & 077) != 0)
    return {};
  return dir;
}

std::string save(const std::string& bytes, const char* ext) {
  const std::string dir = image_dir();
  if (dir.empty()) return {};
  static unsigned seq = 0;
  char name[96];
  snprintf(name, sizeof name, "/paste-%lld-%u.%s", (long long)time(nullptr), ++seq, ext);
  const std::string path = dir + name;
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return {};
  size_t off = 0;
  while (off < bytes.size()) {
    const ssize_t n = write(fd, bytes.data() + off, bytes.size() - off);
    if (n <= 0) { close(fd); unlink(path.c_str()); return {}; }
    off += size_t(n);
  }
  close(fd);
  return path;
}

bool is_image_file(const std::string& path) {
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  if (ext != "png" && ext != "jpg" && ext != "jpeg" && ext != "gif" && ext != "webp") return false;
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string percent_decode(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    const auto hex = [](char h) {
      return h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10
           : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
    };
    if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
      out.push_back(char(hex(s[i + 1]) * 16 + hex(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

}  // namespace

Content read(int timeout_ms) {
  Content c;
  Tool tool;
  if (!find_tool(&tool)) {
    c.kind = Content::Kind::NoTool;
    c.error = getenv("WAYLAND_DISPLAY") ? "install wl-clipboard to paste images"
                                        : "install xclip to paste images (sudo apt install xclip)";
    return c;
  }
  std::string types;
  run(tool.types, timeout_ms, types);
  for (const ImageType& t : kImageTypes) {
    if (types.find(t.mime) == std::string::npos) continue;
    std::string bytes;
    if (!run(tool.image(t.mime), timeout_ms, bytes) || bytes.empty()) break;
    c.image_path = save(bytes, t.ext);
    if (c.image_path.empty()) {
      c.error = "could not save the pasted image";
      return c;
    }
    MLOG("clipboard: saved %zu byte %s to %s", bytes.size(), t.mime, c.image_path.c_str());
    c.kind = Content::Kind::Image;
    return c;
  }
  if (run(tool.text, timeout_ms, c.text) && !c.text.empty()) c.kind = Content::Kind::Text;
  return c;
}

std::vector<std::string> image_paths(std::string_view pasted) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < pasted.size()) {
    while (i < pasted.size() && (pasted[i] == ' ' || pasted[i] == '\t' || pasted[i] == '\n' ||
                                 pasted[i] == '\r'))
      i++;
    if (i >= pasted.size()) break;
    std::string item;
    if (pasted[i] == '\'' || pasted[i] == '"') {
      const char q = pasted[i++];
      const size_t end = pasted.find(q, i);
      if (end == std::string_view::npos) return {};
      item = std::string(pasted.substr(i, end - i));
      i = end + 1;
    } else {
      const size_t start = i;
      while (i < pasted.size() && pasted[i] != ' ' && pasted[i] != '\t' && pasted[i] != '\n' &&
             pasted[i] != '\r')
        i++;
      item = std::string(pasted.substr(start, i - start));
    }
    if (item.starts_with("file://")) item = percent_decode(item.substr(7));
    if (item.empty() || item[0] != '/' || !is_image_file(item)) return {};
    out.push_back(std::move(item));
  }
  return out;
}

}  // namespace mico::clip
