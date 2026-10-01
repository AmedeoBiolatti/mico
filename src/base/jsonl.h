#pragma once
#include <cstddef>
#include <deque>

#include "base/front_vec.h"
#include <string>
#include <string_view>
#include <vector>

namespace mico {

// A JSONL file mapped into memory with a newline offset index. Nothing is
// parsed on open: transcripts run to hundreds of megabytes, so lines are handed
// out as views and parsed only when they are about to be drawn.
class Jsonl {
 public:
  Jsonl() = default;
  ~Jsonl();
  Jsonl(const Jsonl&) = delete;
  Jsonl& operator=(const Jsonl&) = delete;
  Jsonl(Jsonl&& o) noexcept { *this = std::move(o); }
  Jsonl& operator=(Jsonl&& o) noexcept;

  bool open(const std::string& path);
  // Re-maps if the file grew and extends the index over the new bytes only.
  // Live transcripts are append-only, so nothing already indexed can move.
  bool refresh();
  void close();
  bool is_open() const { return data_ != nullptr; }

  size_t size_bytes() const { return len_; }
  // Lines currently indexed. Indexing starts at the tail and walks backwards,
  // so this grows as the reader asks for older content. Index 0 is the oldest
  // line indexed so far, not necessarily the first line of the file.
  size_t line_count() const { return offsets_.empty() ? 0 : offsets_.size() - 1; }
  std::string_view line(size_t i) const;
  // Byte offset of a line within the file, for reporting true position.
  size_t line_offset(size_t i) const;

  // Indexes another chunk towards the start of the file. Returns how many lines
  // were prepended; 0 once the whole file is indexed. Existing line indices
  // shift up by the returned amount.
  size_t extend_back();
  bool complete() const { return indexed_from_ == 0; }
  // First byte the index covers. Everything before it is unscanned.
  size_t indexed_from() const { return indexed_from_; }
  // Line whose span contains `byte`, clamped to the indexed range. Only
  // meaningful once the index reaches that far back.
  size_t line_at_byte(size_t byte) const;
  // Tells the kernel the mapped pages are no longer needed. The index survives;
  // any line touched later is simply faulted back in. Without this, walking a
  // 200 MB transcript leaves 200 MB of page cache charged to the process.
  //
  // Reading is what costs memory here, not keeping: most of a rollout is
  // rejected without contributing a byte of retained text, so reclamation is
  // driven by how much has been scanned rather than how much was kept.
  void release_pages();
  // About to read lines [first, last): pulls their bytes in with large reads,
  // and every so often lets go of pages read before. Reading without it is
  // correct, only slower on a cold file and heavier on resident memory. The
  // release has to be counted here, per byte parsed, not only when the index
  // grows: a reader re-walking an already indexed stretch never grows it.
  void will_read(size_t first, size_t last);
  const std::string& path() const { return path_; }


 private:
  void build_index();
  bool map_file(const std::string& path, size_t* out_len);
  void index_range(size_t from, size_t to);
  // Pulls [from, to) into the page cache with large reads before the mapping
  // touches it. The mapping is MADV_RANDOM, so a backward scan otherwise
  // faults the file in one 4 KB page at a time.
  void prefetch(size_t from, size_t to) const;

  std::string path_;
  const char* data_ = nullptr;
  size_t len_ = 0;
  FrontVec<size_t> offsets_;  // line i spans [offsets_[i], offsets_[i+1])
  // First byte covered by the index; everything before it is unscanned.
  size_t indexed_from_ = 0;
  size_t scanned_since_release_ = 0;
};

}  // namespace mico
