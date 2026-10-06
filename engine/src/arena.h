// arena.h -- bump-pointer memory allocator used by the memtable's skiplist.
//
// Why not plain `new`? A memtable holds hundreds of thousands of tiny nodes
// and key/value copies that all die at the same moment (when the memtable is
// flushed to disk). Allocating each with malloc costs a header per object,
// scatters them across the heap, and forces a free() per object at the end.
//
// The arena instead grabs 4 KiB slabs and hands out slices with a pointer
// bump. Freeing is "delete the arena". Individual frees are impossible by
// design -- that is exactly the memtable's lifetime pattern.
//
//   Arena a;
//   char* p = a.Allocate(17);        // 17 bytes, no alignment promise
//   char* q = a.AllocateAligned(24); // 8-byte aligned, safe for pointers
//
// MemoryUsage() is what the engine compares against Options::memtable_size
// to decide when to flush.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace epica {

class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() {
    for (char* b : blocks_) delete[] b;
  }

  // Returns `bytes` bytes of storage. Never returns nullptr (throws on OOM
  // like operator new).
  char* Allocate(size_t bytes) {
    if (bytes <= remaining_) {
      char* r = ptr_;
      ptr_ += bytes;
      remaining_ -= bytes;
      return r;
    }
    return AllocateFallback(bytes);
  }

  // Like Allocate, but the result is aligned to sizeof(void*). Skiplist nodes
  // contain atomic pointers, which must be naturally aligned.
  char* AllocateAligned(size_t bytes) {
    constexpr size_t kAlign = sizeof(void*);
    const size_t mod = reinterpret_cast<uintptr_t>(ptr_) & (kAlign - 1);
    const size_t slop = (mod == 0 ? 0 : kAlign - mod);
    const size_t needed = bytes + slop;
    if (needed <= remaining_) {
      char* r = ptr_ + slop;
      ptr_ += needed;
      remaining_ -= needed;
      return r;
    }
    // Fallback blocks are freshly new[]'d and therefore already aligned.
    return AllocateFallback(bytes);
  }

  // Approximate total bytes owned by this arena. Readers may call this while
  // a writer is allocating, hence the atomic.
  size_t MemoryUsage() const { return usage_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) {
      // Large object: give it its own block so we do not waste the rest of
      // the current slab.
      return NewBlock(bytes);
    }
    ptr_ = NewBlock(kBlockSize);
    remaining_ = kBlockSize;
    char* r = ptr_;
    ptr_ += bytes;
    remaining_ -= bytes;
    return r;
  }

  char* NewBlock(size_t n) {
    char* b = new char[n];
    blocks_.push_back(b);
    usage_.fetch_add(n + sizeof(char*), std::memory_order_relaxed);
    return b;
  }

  char* ptr_ = nullptr;
  size_t remaining_ = 0;
  std::vector<char*> blocks_;
  std::atomic<size_t> usage_{0};
};

}  // namespace epica
