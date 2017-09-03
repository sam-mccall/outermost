#ifndef BUFFERS_H_
#define BUFFERS_H_

#include "base.h"

#include <array>
#include <cstring>
#include <deque>

template <int N>
class History {
 public:
  History() {
    memset(data_, 0, N);
  }

  void Write(const u8* src, int count) {
    while (count >= 2*N) {
      src += N;
      count -= N;
    }
    CHECK(count >= 0);
    for (; count; --count) {
      data_[pos_++] = *src++;
      if (pos_ == N) pos_ = 0;
    }
  }

  void Dump() {
    constexpr static int kBlockSize = 32;
    static_assert(N % kBlockSize == 0);
    auto get = [this](int block, int i) {
      return data_[(pos_ + block * kBlockSize + i) % N];
    };
    for (int block = 0; block < N/kBlockSize; ++block) {
      for (int i = 0; i < kBlockSize; ++i) {
        u8 c = get(block, i);
        fprintf(stderr, "%c  ", isprint(c) ? c : ' ');
      }
      fprintf(stderr, "\n");
      for (int i = 0; i < kBlockSize; ++i) {
        u8 c = get(block, i);
        fprintf(stderr, "%02x ", c);
      }
      fprintf(stderr, "\n");
    }
  }

 private:
  u8 data_[N];
  int pos_ = 0;
};

template <int N>
class WriteQueue {
 public:
  WriteQueue() : blocks_(1) {}

  void Push(const u8* data, int n) {
    while (n > 0) {
      int count = std::min(n, N - limit_);
      memcpy(&blocks_.back()[limit_], data, count);
      limit_ += count;
      if (limit_ == N) {
        limit_ = 0;
        blocks_.emplace_back();
      }
      n -= count;
    }
  }

  void Shift(int n) {
    start_ += n;
    if (start_ == N) {
      start_ = 0;
      blocks_.pop_front();
    }
  }

  bool HasBlock() { return blocks_.size() > 1 || start_ != limit_; }
  int GetBlock(u8** data) {
    *data = &blocks_.front()[start_];
    return blocks_.size() == 1 ? limit_ - start_ : N - start_;
  }

 private:
  std::deque<std::array<u8, N>> blocks_;
  int start_ = 0; // in first block
  int limit_ = 0; // in last block
};

#endif // BUFFERS_H_
