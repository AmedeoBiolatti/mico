#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace mico {

// Reads a file's lines front to back, once: what an index pass over every
// transcript does. Plain reads into one buffer, a few megabytes at a time.
// Jsonl maps a file for a view that jumps around in it; for a single pass the
// mapping costs a page fault every 4 KB and more than the scanning itself.
// Each reader owns its buffer, so readers on different threads share nothing.
class LineReader {
 public:
  explicit LineReader(std::string path) : path_(std::move(path)) {}

  // Calls fn(line, offset) for each complete line starting at byte `from`,
  // which must be the start of a line, the newline left off. A last line
  // without its newline is still being written: it is left for next time.
  // Returns the offset just past the last line handed out, `from` when there
  // was none; false from `ok()` when the file could not be read.
  template <class F>
  uint64_t each(uint64_t from, F&& fn) {
    const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      ok_ = false;
      return from;
    }
    ok_ = true;
    uint64_t done = from;     // file offset of buf_[0]
    size_t have = 0;          // bytes in buf_
    if (buf_.size() < kSlab) buf_.resize(kSlab);
    for (;;) {
      if (have == buf_.size()) buf_.resize(buf_.size() * 2);  // one line longer than the buffer
      const ssize_t n = pread(fd, buf_.data() + have, buf_.size() - have, off_t(done + have));
      if (n <= 0) break;
      have += size_t(n);
      size_t start = 0;
      for (;;) {
        const void* nl = memchr(buf_.data() + start, '\n', have - start);
        if (!nl) break;
        const size_t end = size_t(static_cast<const char*>(nl) - buf_.data());
        fn(std::string_view(buf_.data() + start, end - start), done + start);
        start = end + 1;
      }
      // Keep the unfinished line at the front for the next read.
      if (start > 0) {
        memmove(buf_.data(), buf_.data() + start, have - start);
        have -= start;
        done += start;
      }
    }
    ::close(fd);
    return done;
  }
  bool ok() const { return ok_; }

 private:
  static constexpr size_t kSlab = 4u << 20;
  std::string path_;
  std::string buf_;
  bool ok_ = false;
};

}  // namespace mico
