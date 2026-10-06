// slice.h -- a non-owning (pointer, length) view of bytes.
//
// Keys and values are arbitrary bytes, not text, so the engine's APIs take
// Slice rather than std::string. Passing a Slice copies 16 bytes, not the
// data. The caller must keep the underlying buffer alive for as long as the
// Slice is used -- the same rule as std::string_view.
//
// compare() is unsigned lexicographic byte order (memcmp), which is the ONE
// key order the whole engine agrees on: memtable, SSTables, and the Java
// client's InMemoryEngineClient all sort keys this way.
#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace epica {

// Non-owning view over a run of bytes. The caller guarantees the underlying
// storage outlives the Slice. Same shape as std::string_view, kept as its own
// type so engine APIs read as "bytes", not "text".
class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}  // NOLINT: implicit by design
  Slice(std::string_view s) : data_(s.data()), size_(s.size()) {}    // NOLINT
  Slice(const char* s) : data_(s), size_(std::strlen(s)) {}           // NOLINT

  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  char operator[](size_t i) const { return data_[i]; }

  void remove_prefix(size_t n) {
    data_ += n;
    size_ -= n;
  }

  std::string ToString() const { return std::string(data_, size_); }
  std::string_view view() const { return std::string_view(data_, size_); }

  int compare(const Slice& b) const {
    const size_t min_len = size_ < b.size_ ? size_ : b.size_;
    int r = std::memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) r = -1;
      else if (size_ > b.size_) r = +1;
    }
    return r;
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
inline bool operator!=(const Slice& a, const Slice& b) { return !(a == b); }
inline bool operator<(const Slice& a, const Slice& b) { return a.compare(b) < 0; }

}  // namespace epica
