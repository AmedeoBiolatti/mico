#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mico {

// Runs independent jobs on worker threads while the calling thread goes on
// with its own work, handing back each result as it finishes. For index
// passes over every transcript: each file is read on its own, and the UI
// thread only merges what is done, between frames.
//
// A slot is a job's input and its output both: work(slot) runs on a worker
// and touches only that slot; collect() hands finished slots back on the
// calling thread. Nothing else is shared.
template <class Slot>
class Batch {
 public:
  Batch() = default;
  Batch(const Batch&) = delete;
  Batch& operator=(const Batch&) = delete;
  ~Batch() { stop(); }

  // Starts work(slot) for every slot, on up to `threads` threads (at least
  // one). A batch still running is stopped first.
  void start(std::vector<Slot> slots, std::function<void(Slot&)> work, unsigned threads) {
    stop();
    slots_ = std::move(slots);
    work_ = std::move(work);
    done_ = std::make_unique<std::atomic<bool>[]>(slots_.size());
    taken_.assign(slots_.size(), false);
    left_ = slots_.size();
    finished_ = 0;
    next_ = 0;
    stopping_ = false;
    threads = std::max(1u, std::min<unsigned>(threads, unsigned(slots_.size())));
    for (unsigned t = 0; t < threads && !slots_.empty(); t++)
      threads_.emplace_back([this] {
        for (;;) {
          const size_t i = next_.fetch_add(1);
          if (i >= slots_.size() || stopping_.load()) return;
          work_(slots_[i]);
          done_[i].store(true, std::memory_order_release);
          {
            std::lock_guard<std::mutex> lock(mu_);
            finished_++;
          }
          cv_.notify_all();
        }
      });
  }

  // Hands each slot finished since the last call to take(slot).
  template <class F>
  void collect(F&& take) {
    for (size_t i = 0; i < slots_.size() && left_ > 0; i++) {
      if (taken_[i] || !done_[i].load(std::memory_order_acquire)) continue;
      take(slots_[i]);
      taken_[i] = true;
      left_--;
    }
    if (left_ == 0 && !threads_.empty()) join();
  }

  // Waits until a slot finishes that collect() has not seen, or `ms` passes.
  void wait(int ms) {
    std::unique_lock<std::mutex> lock(mu_);
    const size_t seen = slots_.size() - left_;
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return finished_ > seen; });
  }

  // Slots not yet handed back.
  size_t pending() const { return left_; }
  bool running() const { return left_ > 0; }

  // Abandons what has not started and waits for what has; results not yet
  // collected are dropped.
  void stop() {
    stopping_ = true;
    join();
    slots_.clear();
    taken_.clear();
    left_ = 0;
  }

  // Threads worth using for a batch: the machine's cores, less one for the UI,
  // capped where reading files from memory stops getting quicker.
  static unsigned default_threads() {
    const unsigned n = std::thread::hardware_concurrency();
    return std::clamp(n > 1 ? n - 1 : 1u, 1u, 8u);
  }

 private:
  void join() {
    for (auto& t : threads_) t.join();
    threads_.clear();
  }

  std::vector<Slot> slots_;
  std::function<void(Slot&)> work_;
  std::unique_ptr<std::atomic<bool>[]> done_;
  std::vector<bool> taken_;
  size_t left_ = 0;
  std::atomic<size_t> next_{0};
  std::atomic<bool> stopping_{false};
  std::mutex mu_;
  std::condition_variable cv_;
  size_t finished_ = 0;  // under mu_
  std::vector<std::thread> threads_;
};

}  // namespace mico
