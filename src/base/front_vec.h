#pragma once
#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace mico {

// A contiguous array that grows at both ends. Scrolling back prepends: parsed
// events, laid-out rows, and line offsets all arrive oldest-last. std::deque
// does that by allocating and freeing a small block every few elements, which
// made it the largest source of allocations while paging through a long chat. This keeps
// one buffer with room on either side, and keeps it across clear(), so a
// window sliding through a file reuses the same memory throughout.
//
// Only for trivially copyable T: elements are moved as plain bytes and slots
// outside [head, end) hold whatever was last there.
template <class T>
class FrontVec {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  size_t size() const { return end_ - head_; }
  bool empty() const { return end_ == head_; }

  T& operator[](size_t i) { return buf_[head_ + i]; }
  const T& operator[](size_t i) const { return buf_[head_ + i]; }
  T* begin() { return buf_.data() + head_; }
  T* end() { return buf_.data() + end_; }
  const T* begin() const { return buf_.data() + head_; }
  const T* end() const { return buf_.data() + end_; }
  T& back() { return buf_[end_ - 1]; }
  const T& back() const { return buf_[end_ - 1]; }

  void push_back(const T& v) {
    if (end_ == buf_.size()) make_room(0, 1);
    buf_[end_++] = v;
  }

  void append(const T* first, const T* last) {
    const size_t n = size_t(last - first);
    if (buf_.size() - end_ < n) make_room(0, n);
    std::copy(first, last, buf_.data() + end_);
    end_ += n;
  }

  void push_front(const T& v) { prepend(&v, &v + 1); }
  // Drops the first `n` elements.
  void pop_front(size_t n) { head_ += std::min(n, size()); }

  void prepend(const T* first, const T* last) {
    const size_t n = size_t(last - first);
    if (head_ < n) make_room(n, 0);
    head_ -= n;
    std::copy(first, last, buf_.data() + head_);
  }

  // Shrinks only; the callers never grow through resize.
  void resize(size_t n) {
    if (n < size()) end_ = head_ + n;
  }

  T* erase(T* it) {
    std::copy(it + 1, end(), it);
    --end_;
    return it;
  }

  // Keeps the buffer, centred, so the next window grows either way for free.
  void clear() { head_ = end_ = buf_.size() / 2; }

 private:
  // Recentres the contents so there are at least `front` free slots before
  // them and `back` after. Moves within the buffer when it is big enough and
  // only reallocates when it is not.
  void make_room(size_t front, size_t back) {
    const size_t n = size();
    const size_t need = n + front + back;
    if (buf_.size() < need + need / 2 + 16) {
      std::vector<T> grown(2 * need + 64);
      const size_t head = front + (grown.size() - need) / 2;
      std::copy(begin(), end(), grown.data() + head);
      buf_.swap(grown);
      head_ = head;
    } else {
      const size_t head = front + (buf_.size() - need) / 2;
      // Overlapping ranges: copy in the direction that does not clobber.
      if (head < head_) std::copy(begin(), end(), buf_.data() + head);
      else std::copy_backward(begin(), end(), buf_.data() + head + n);
      head_ = head;
    }
    end_ = head_ + n;
  }

  std::vector<T> buf_;
  size_t head_ = 0, end_ = 0;
};

}  // namespace mico
