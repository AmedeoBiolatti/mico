#include "core/search.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "term/text.h"

namespace mico {

bool in_scope(EventKind k, const SearchScope& s) {
  switch (k) {
    case EventKind::User: case EventKind::Assistant: case EventKind::Question:
    case EventKind::Notice:
      return true;
    case EventKind::Thinking: return s.thinking;
    // A tool's output counts while tools are shown: opening the call shows it.
    case EventKind::ToolCall: case EventKind::ToolResult: case EventKind::TaskStatus:
      return s.tools;
    case EventKind::Meta: return s.meta;
    default: return false;
  }
}

ChatSearch::~ChatSearch() { close_map(); }

void ChatSearch::start(const std::vector<Project>& projects, const Store& store,
                       std::string query, SearchScope scope) {
  cancel();
  scope_ = scope;
  query_ = std::move(query);
  fold_ = text::fold(query_);
  hits_.clear();
  chats_ = total_ = 0;
  jobs_.clear();
  next_job_ = 0;
  if (fold_.empty()) return;
  for (const auto& p : projects)
    for (const auto& s : p.sessions) {
      const std::string* name = store.custom_name(s.agent, s.id);
      jobs_.push_back(Job{s, p.name, name ? *name : s.title});
    }
  std::stable_sort(jobs_.begin(), jobs_.end(),
                   [](const Job& a, const Job& b) { return a.s.mtime > b.s.mtime; });
}

void ChatSearch::cancel() {
  close_map();
  next_job_ = jobs_.size();
}

void ChatSearch::open_next() {
  const Job& j = jobs_[next_job_++];
  pos_ = 0;
  file_hits_ = 0;
  file_first_kept_ = false;
  const int fd = ::open(j.s.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  struct stat st{};
  if (fstat(fd, &st) == 0 && st.st_size > 0) {
    void* m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (m != MAP_FAILED) {
      madvise(m, size_t(st.st_size), MADV_SEQUENTIAL);
      map_ = static_cast<const char*>(m);
      size_ = size_t(st.st_size);
    }
  }
  ::close(fd);
}

void ChatSearch::close_map() {
  if (!map_) return;
  if (file_hits_ > 0) {
    chats_++;
    if (file_first_kept_) hits_[file_first_].chat_hits = file_hits_;
  }
  munmap(const_cast<char*>(map_), size_);
  map_ = nullptr;
  size_ = 0;
}

bool ChatSearch::step(int budget_ms) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto spent = [&] {
    return std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(budget_ms);
  };
  constexpr size_t kChunk = 4u << 20;
  while (!spent()) {
    if (!map_) {
      if (next_job_ >= jobs_.size()) return true;
      open_next();
      continue;
    }
    // One chunk at a time, so the clock is looked at every few megabytes. The
    // haystack runs a needle's length past the chunk, so a match that
    // straddles the boundary is still seen, from this side.
    const size_t chunk_end = std::min(size_, pos_ + kChunk);
    const std::string_view hay(map_, std::min(size_, chunk_end + fold_.size() - 1));
    const size_t m = text::find_folded(hay, fold_, pos_);
    if (m == std::string_view::npos || m >= chunk_end) {
      pos_ = chunk_end;
      if (pos_ >= size_) close_map();
      continue;
    }
    const void* nl_before = m ? memrchr(map_, '\n', m) : nullptr;
    const size_t a = nl_before ? size_t(static_cast<const char*>(nl_before) - map_) + 1 : 0;
    const void* nl_after = memchr(map_ + m, '\n', size_ - m);
    const size_t b = nl_after ? size_t(static_cast<const char*>(nl_after) - map_) : size_;
    confirm(jobs_[next_job_ - 1], a, b);
    pos_ = b + 1;  // one hit per line
    if (pos_ >= size_) close_map();
  }
  return complete();
}

bool ChatSearch::confirm(const Job& j, size_t a, size_t b) {
  const Adapter* adapter = Store::adapter_for(j.s);
  if (!adapter) return false;
  tmp_.clear();
  evs_.clear();
  adapter->parse(std::string_view(map_ + a, b - a), tmp_, evs_);
  for (const Event& e : evs_) {
    if (!in_scope(e.kind, scope_)) continue;
    for (Str field : {e.text, e.summary, e.name, e.detail}) {
      const std::string_view v = tmp_.view(field);
      const size_t p = text::find_folded(v, fold_);
      if (p == std::string_view::npos) continue;
      total_++;
      file_hits_++;
      if (file_hits_ > kSnippetsPerChat || hits_.size() >= kMaxHits) return true;
      // Context on both sides, cut at character boundaries, with line breaks
      // turned into spaces one for one so the match keeps its offset.
      size_t from = p > 48 ? p - 48 : 0;
      while (from > 0 && (uint8_t(v[from]) & 0xC0) == 0x80) from--;
      size_t to = std::min(v.size(), p + fold_.size() + 160);
      while (to < v.size() && (uint8_t(v[to]) & 0xC0) == 0x80) to++;
      SearchHit h;
      h.path = j.s.path;
      h.agent = j.s.agent;
      h.id = j.s.id;
      h.title = j.title;
      h.project = j.project;
      h.mtime = j.s.mtime;
      h.offset = a;
      if (from > 0) h.snippet = "\xE2\x80\xA6";  // …
      h.match_at = h.snippet.size() + (p - from);
      h.match_len = fold_.size();
      for (size_t i = from; i < to; i++) {
        const char c = v[i];
        h.snippet.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
      }
      if (to < v.size()) h.snippet += "\xE2\x80\xA6";
      h.first_in_chat = file_hits_ == 1;
      if (h.first_in_chat) {
        file_first_ = hits_.size();
        file_first_kept_ = true;
      }
      hits_.push_back(std::move(h));
      return true;
    }
  }
  return false;
}

}  // namespace mico
