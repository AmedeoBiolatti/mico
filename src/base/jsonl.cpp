#include "base/jsonl.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace mico {

Jsonl::~Jsonl() { close(); }

Jsonl& Jsonl::operator=(Jsonl&& o) noexcept {
  if (this != &o) {
    close();
    path_ = std::move(o.path_);
    data_ = std::exchange(o.data_, nullptr);
    len_ = std::exchange(o.len_, 0);
    offsets_ = std::move(o.offsets_);
    indexed_from_ = std::exchange(o.indexed_from_, 0);
    scanned_since_release_ = std::exchange(o.scanned_since_release_, 0);
  }
  return *this;
}

void Jsonl::close() {
  if (data_) munmap(const_cast<char*>(data_), len_);
  data_ = nullptr;
  len_ = 0;
  offsets_.clear();
  indexed_from_ = 0;
  scanned_since_release_ = 0;
  path_.clear();
}

bool Jsonl::map_file(const std::string& path, size_t* out_len) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
  void* m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (m == MAP_FAILED) return false;
  data_ = static_cast<const char*>(m);
  *out_len = size_t(st.st_size);
  return true;
}

bool Jsonl::open(const std::string& path) {
  close();
  size_t mapped = 0;
  if (!map_file(path, &mapped)) return false;
  len_ = mapped;
  path_ = path;
  void* m = const_cast<char*>(data_);
  // Sequential: we scan front-to-back once to index, then jump around.
  madvise(const_cast<void*>(m), len_, MADV_SEQUENTIAL);
  build_index();
  madvise(const_cast<void*>(m), len_, MADV_RANDOM);
  return true;
}

bool Jsonl::refresh() {
  if (path_.empty() || !data_) return false;
  struct stat st{};
  if (stat(path_.c_str(), &st) != 0) return false;
  if (size_t(st.st_size) <= len_) return false;

  const size_t old_len = len_;
  // The last offset is where the next line begins: end-of-file when the file
  // ended on a newline, or the start of a partial line when it did not. Either
  // way it is exactly where the new scan resumes.
  const size_t scan_from = offsets_.empty() ? 0 : offsets_.back();
  const size_t keep_from = indexed_from_;

  FrontVec<size_t> keep = std::move(offsets_);
  const char* old_data = data_;
  size_t map_len = 0;
  if (!map_file(path_, &map_len)) {
    offsets_ = std::move(keep);
    data_ = old_data;
    return false;
  }
  munmap(const_cast<char*>(old_data), old_len);
  offsets_ = std::move(keep);
  len_ = map_len;
  indexed_from_ = keep_from;

  const char* p = data_ + scan_from;
  const char* end = data_ + len_;
  while (p < end) {
    const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
    if (!nl) break;
    offsets_.push_back(size_t(nl - data_) + 1);
    p = nl + 1;
  }
  return true;
}

namespace {
// Indexed on open. A screenful of chat needs a few hundred lines; this covers
// far more than that while touching a fraction of a large rollout.
constexpr size_t kInitialIndexBytes = 1u << 20;
// Each subsequent backward step. Larger than the initial chunk because a reader
// scrolling this far back is going somewhere.
constexpr size_t kExtendBytes = 4u << 20;
}  // namespace

// Scans [from, to) for line starts and prepends them. `from` must already sit
// on a line boundary.
void Jsonl::index_range(size_t from, size_t to) {
  std::vector<size_t> found;
  found.reserve((to - from) / 512 + 8);
  const char* p = data_ + from;
  const char* end = data_ + to;
  while (p < end) {
    const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
    if (!nl) break;
    found.push_back(size_t(nl - data_) + 1);
    p = nl + 1;
  }
  // The last boundary found equals `to` when the ranges abut, which the
  // existing index already provides; drop it to avoid a duplicate offset.
  if (!found.empty() && found.back() == to) found.pop_back();
  // One range insert, not one insert per line: inserting individually is
  // quadratic and a 4 MB slab holds thousands of lines.
  offsets_.prepend(found.data(), found.data() + found.size());
  offsets_.push_front(from);
  indexed_from_ = from;
}

void Jsonl::build_index() {
  offsets_.clear();

  // Index the tail only: the newest turns are what opens on screen.
  size_t start = len_ > kInitialIndexBytes ? len_ - kInitialIndexBytes : 0;
  if (start > 0) {
    // Move to the next real line boundary so we never index a partial line.
    const char* nl = static_cast<const char*>(memchr(data_ + start, '\n', len_ - start));
    start = nl ? size_t(nl - data_) + 1 : 0;
  }
  // A trailing partial line is not a line. Exposing one lets a reader parse a
  // half-written record and show a truncated message until the writer finishes
  // it; the offset that ends the last complete line is kept as the resume
  // point for refresh().
  indexed_from_ = start;
  offsets_.push_back(start);
  const char* p = data_ + start;
  const char* end = data_ + len_;
  while (p < end) {
    const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
    if (!nl) break;
    offsets_.push_back(size_t(nl - data_) + 1);
    p = nl + 1;
  }
}

namespace {
// Pages read through the mapping are dropped after this many bytes. The text
// worth keeping was copied into the arena during parsing, and anything touched
// again comes back from the page cache, so dropping is cheap.
constexpr size_t kReleaseEvery = 24u << 20;
}  // namespace

void Jsonl::will_read(size_t first, size_t last) {
  if (!data_ || first >= last || last > line_count()) return;
  const size_t from = line_offset(first), to = line_offset(last);
  scanned_since_release_ += to - from;
  if (scanned_since_release_ >= kReleaseEvery) release_pages();
  prefetch(from, to);
}

size_t Jsonl::extend_back() {
  if (indexed_from_ == 0 || !data_) return 0;

  // Drop what has already been scanned before pulling in the next slab.
  if (scanned_since_release_ >= kReleaseEvery) release_pages();

  size_t before = line_count();

  size_t start = indexed_from_ > kExtendBytes ? indexed_from_ - kExtendBytes : 0;
  prefetch(start, indexed_from_);
  // Include a line larger than the slab instead of falsely declaring that
  // the index reached the beginning and losing all earlier history. Searched
  // a slab at a time, not a byte at a time: a byte-wise walk backwards through
  // a huge line faults it in page by page.
  while (start > 0 && data_[start - 1] != '\n') {
    const size_t lo = start > kExtendBytes ? start - kExtendBytes : 0;
    prefetch(lo, start);
    const void* nl = memrchr(data_ + lo, '\n', start - lo);
    start = nl ? size_t(static_cast<const char*>(nl) - data_) + 1 : lo;
  }
  scanned_since_release_ += indexed_from_ - start;
  index_range(start, indexed_from_);
  return line_count() - before;
}

// Measured on a cold 249 MB rollout, scanning 85 MB backwards: page faults
// through the mapping took 1.0 s (21k major faults); the same bytes read with
// pread took 0.05 s. madvise(MADV_WILLNEED), posix_fadvise(WILLNEED) and
// readahead(2) were all tried and left the fault count unchanged, so the
// slab is simply read. The copy costs a few milliseconds per 4 MB and leaves
// the pages cached, so the mapping then hits memory instead of the disk.
void Jsonl::prefetch(size_t from, size_t to) const {
  if (path_.empty() || from >= to) return;
  const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  static char buf[1u << 20];
  while (from < to) {
    const ssize_t n = pread(fd, buf, std::min(sizeof buf, to - from), off_t(from));
    if (n <= 0) break;
    from += size_t(n);
  }
  ::close(fd);
}

void Jsonl::release_pages() {
  if (data_ && len_) madvise(const_cast<char*>(data_), len_, MADV_DONTNEED);
  scanned_since_release_ = 0;
}

size_t Jsonl::line_at_byte(size_t byte) const {
  if (offsets_.size() < 2) return 0;
  // offsets_ is sorted; the line is the last one starting at or before `byte`.
  size_t lo = 0, hi = line_count();
  while (lo + 1 < hi) {
    const size_t mid = (lo + hi) / 2;
    if (offsets_[mid] <= byte) lo = mid;
    else hi = mid;
  }
  return lo;
}

size_t Jsonl::line_offset(size_t i) const {
  if (offsets_.empty()) return 0;
  if (i >= offsets_.size()) return offsets_.back();
  return offsets_[i];
}

std::string_view Jsonl::line(size_t i) const {
  if (i + 1 >= offsets_.size()) return {};
  size_t a = offsets_[i], b = offsets_[i + 1];
  if (b > a && data_[b - 1] == '\n') b--;
  if (b > a && data_[b - 1] == '\r') b--;
  return {data_ + a, b - a};
}

}  // namespace mico
