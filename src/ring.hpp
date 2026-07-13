// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

// FIFO over a contiguous buffer. Realtime callers reserve their maximum size
// up front and use try_push() so an unexpected input can fail open without
// touching the allocator.
template <typename T> class Ring {
  static_assert(std::is_trivially_copyable_v<T>,
                "Ring requires a trivially copyable element type");

public:
  void reserve(size_t cap) {
    if (cap <= buf_.size())
      return;
    std::vector<T> next(cap);
    if (count_) {
      const size_t cur = buf_.size();
      const size_t first = std::min(count_, cur - head_);
      std::memcpy(next.data(), &buf_[head_], first * sizeof(T));
      if (count_ > first)
        std::memcpy(next.data() + first, buf_.data(),
                    (count_ - first) * sizeof(T));
    }
    buf_.swap(next);
    head_ = 0;
  }

  void clear() {
    head_ = 0;
    count_ = 0;
  }

  size_t size() const { return count_; }
  size_t capacity() const { return buf_.size(); }
  bool empty() const { return count_ == 0; }
  const T &front() const { return buf_[head_]; }

  void push(const T *src, size_t n) {
    if (!n)
      return;
    ensure(count_ + n);
    push_unchecked(src, n);
  }

  bool try_push(const T *src, size_t n) {
    if (n > buf_.size() - count_)
      return false;
    if (n)
      push_unchecked(src, n);
    return true;
  }

  bool try_push(const T &value) { return try_push(&value, 1); }

  void push(const T &value) { push(&value, 1); }

  void peek(T *dst, size_t n) const {
    const size_t cap = buf_.size();
    const size_t first = std::min(n, cap - head_);
    std::memcpy(dst, &buf_[head_], first * sizeof(T));
    if (n > first)
      std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(T));
  }

  void pop(size_t n) {
    head_ = (head_ + n) % buf_.size();
    count_ -= n;
  }

private:
  void push_unchecked(const T *src, size_t n) {
    const size_t cap = buf_.size();
    const size_t tail = (head_ + count_) % cap;
    const size_t first = std::min(n, cap - tail);
    std::memcpy(&buf_[tail], src, first * sizeof(T));
    if (n > first)
      std::memcpy(buf_.data(), src + first, (n - first) * sizeof(T));
    count_ += n;
  }
  void ensure(size_t need) {
    if (need > buf_.size())
      reserve(std::max(need, buf_.size() * 2 + 64));
  }

  std::vector<T> buf_;
  size_t head_ = 0;
  size_t count_ = 0;
};
