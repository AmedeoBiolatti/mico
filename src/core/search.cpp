#include "base/log.h"
#include "core/search.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "base/text.h"

namespace mico {

bool in_scope(EventKind k, const SearchScope& s) {
  switch (k) {
    case EventKind::User: case EventKind::Assistant: case EventKind::Question:
    case EventKind::Notice: case EventKind::Peer:
      return true;
    case EventKind::Thinking: return s.thinking;
    // A tool's output counts while tools are shown: opening the call shows it.
    case EventKind::ToolCall: case EventKind::ToolResult: case EventKind::TaskStatus:
      return s.tools;
    case EventKind::Meta: return s.meta;
    default: return false;
  }
}

ChatSearch::~ChatSearch() { cancel(); }

void ChatSearch::start(const std::vector<Project>& projects, const Store& store,
                       std::string query, SearchScope scope) {
  cancel();
  scope_ = scope;
  query_ = std::move(query);
  fold_ = text::fold(query_);
  hits_.clear();
  chats_ = total_ = 0;
  jobs_.clear();
  merged_ = 0;
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
  batch_.stop();
  done_.clear();
  ready_.clear();
  merged_ = jobs_.size();
}

bool ChatSearch::step(int budget_ms) {
  if (complete()) return true;
  if (!batch_.running() && ready_.empty()) {
    std::vector<Work> work(jobs_.size());
    for (size_t i = 0; i < jobs_.size(); i++) {
      work[i].job = &jobs_[i];
      work[i].fold = fold_;
      work[i].scope = scope_;
    }
    ready_.assign(jobs_.size(), false);
    done_.assign(jobs_.size(), Work{});
    batch_.start(std::move(work), &ChatSearch::search_file, Batch<Work>::default_threads());
  }
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  for (;;) {
    batch_.collect([&](Work& w) {
      const size_t i = size_t(w.job - jobs_.data());
      done_[i] = std::move(w);
      ready_[i] = true;
    });
    // Newest first, as a single thread found them: a file waits for those
    // before it.
    while (merged_ < jobs_.size() && ready_[merged_]) {
      merge(done_[merged_]);
      done_[merged_] = Work{};
      merged_++;
    }
    if (complete() || std::chrono::steady_clock::now() >= until) break;
    batch_.wait(int(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count()) + 1);
  }
  if (complete()) {
    done_.clear();
    ready_.clear();
  }
  return complete();
}

void ChatSearch::merge(Work& w) {
  total_ += size_t(w.count);
  if (w.count == 0) return;
  chats_++;
  bool first = true;
  for (SearchHit& h : w.hits) {
    if (hits_.size() >= kMaxHits) break;
    h.first_in_chat = first;
    if (first) h.chat_hits = w.count;
    first = false;
    hits_.push_back(std::move(h));
  }
}

// One file, on a worker thread: everything here is the job's own.
void ChatSearch::search_file(Work& w) {
  const Job& j = *w.job;
  logs::Doing doing("searching", j.s.path.c_str());
  const Adapter* adapter = Store::adapter_for(j.s);
  if (!adapter) return;
  const int fd = ::open(j.s.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  struct stat st{};
  const char* map = nullptr;
  size_t size = 0;
  if (fstat(fd, &st) == 0 && st.st_size > 0) {
    void* m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (m != MAP_FAILED) {
      madvise(m, size_t(st.st_size), MADV_SEQUENTIAL);
      map = static_cast<const char*>(m);
      size = size_t(st.st_size);
    }
  }
  ::close(fd);
  if (!map) return;
  const std::string& fold = w.fold;
  const std::string_view all(map, size);
  Arena tmp;
  std::vector<Event> evs;
  // Confirms a raw hit on the line [a, b): the query has to be in text the
  // chat shows. One hit per line.
  const auto confirm = [&](size_t a, size_t b) {
    tmp.clear();
    evs.clear();
    adapter->parse(all.substr(a, b - a), tmp, evs);
    for (const Event& e : evs) {
      if (!in_scope(e.kind, w.scope)) continue;
      for (Str field : {e.text, e.summary, e.name, e.detail}) {
        const std::string_view v = tmp.view(field);
        const size_t p = text::find_folded(v, fold);
        if (p == std::string_view::npos) continue;
        w.count++;
        if (w.hits.size() >= size_t(kSnippetsPerChat)) return;
        // Context on both sides, cut at character boundaries, with line breaks
        // turned into spaces one for one so the match keeps its offset.
        size_t from = p > 48 ? p - 48 : 0;
        while (from > 0 && (uint8_t(v[from]) & 0xC0) == 0x80) from--;
        size_t to = std::min(v.size(), p + fold.size() + 160);
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
        h.match_len = fold.size();
        for (size_t i = from; i < to; i++) {
          const char c = v[i];
          h.snippet.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
        }
        if (to < v.size()) h.snippet += "\xE2\x80\xA6";
        w.hits.push_back(std::move(h));
        return;
      }
    }
  };
  for (size_t pos = 0; pos < size;) {
    const size_t m = text::find_folded(all, fold, pos);
    if (m == std::string_view::npos) break;
    const void* nl_before = m ? memrchr(map, '\n', m) : nullptr;
    const size_t a = nl_before ? size_t(static_cast<const char*>(nl_before) - map) + 1 : 0;
    const void* nl_after = memchr(map + m, '\n', size - m);
    const size_t b = nl_after ? size_t(static_cast<const char*>(nl_after) - map) : size;
    confirm(a, b);
    pos = b + 1;
  }
  munmap(const_cast<char*>(map), size);
}

}  // namespace mico
