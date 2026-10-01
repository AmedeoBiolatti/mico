#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

// Small filesystem helpers shared by session discovery and session launching.
// Both walk the same agent directories; neither needs a general-purpose
// filesystem layer.
namespace mico::fs {

inline std::string home() {
  const char* h = getenv("HOME");
  return h ? h : ".";
}

inline bool exists(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0;
}

inline int64_t mtime(const std::string& path, uint64_t* size = nullptr) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) return 0;
  if (size) *size = uint64_t(st.st_size);
  return int64_t(st.st_mtime);
}

inline bool has_suffix(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Calls fn(name) for each entry of `dir` that is (or is not) a directory.
template <class F>
void list_dir(const std::string& dir, bool want_dirs, F&& fn) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    bool is_dir = e->d_type == DT_DIR;
    if (e->d_type == DT_UNKNOWN) {
      struct stat st{};
      is_dir = stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    if (is_dir == want_dirs) fn(name);
  }
  closedir(d);
}

// Reads at most n bytes from the head of a file into a caller-owned buffer.
// Listing a few hundred sessions does a few hundred reads and no allocations.
inline std::string_view read_prefix(const std::string& path, size_t n, std::string& buf) {
  if (buf.size() < n) buf.resize(n);
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return {};
  ssize_t got = pread(fd, buf.data(), n, 0);
  ::close(fd);
  if (got <= 0) return {};
  return std::string_view(buf.data(), size_t(got));
}

// Metadata can include a large system prompt. Never silently parse a fixed
// 8 KB fragment as though it were the whole session header.
inline std::string_view read_first_line(const std::string& path, std::string& buf) {
  for (size_t n = 4096; n <= (16u << 20); n *= 2) {
    auto head = read_prefix(path, n, buf);
    const size_t nl = head.find('\n');
    if (nl != std::string_view::npos) return head.substr(0, nl);
    if (head.size() < n) break;
  }
  return {};
}

// Calls fn(line) for each complete line; a trailing partial line is ignored.
// Returning false stops the walk.
template <class F>
void for_each_line(std::string_view blob, F&& fn) {
  size_t pos = 0;
  while (pos < blob.size()) {
    size_t nl = blob.find('\n', pos);
    if (nl == std::string_view::npos) return;
    if (!fn(blob.substr(pos, nl - pos))) return;
    pos = nl + 1;
  }
}

}  // namespace mico::fs
