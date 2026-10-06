// cache.h -- a thread-safe LRU cache of decoded SSTable blocks.
//
// Disk reads are the slow part of a Get that misses the memtable. Most
// workloads have hot keys, so the blocks holding them are read again and
// again. The block cache keeps recently used decoded blocks in memory, keyed
// by (file number, block offset), and evicts the least recently used block
// when the byte budget (Options::block_cache_size) is exceeded.
//
// Implementation: the textbook doubly-linked list + hash map. The list is
// ordered by recency (front = most recent). A lookup moves the hit to the
// front; an insert pushes to the front and pops from the back until under
// budget. Both are O(1). One mutex guards everything -- simple, and fine at
// this scale (see DESIGN_DECISIONS.md for the sharded alternative).
//
// Values are std::shared_ptr<Block>, so an iterator can keep using a block
// after the cache evicts it.
#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "block.h"

namespace epica {

class BlockCache {
 public:
  explicit BlockCache(size_t capacity_bytes) : capacity_(capacity_bytes) {}

  struct Key {
    uint64_t file_number;
    uint64_t offset;
    bool operator==(const Key& o) const {
      return file_number == o.file_number && offset == o.offset;
    }
  };

  // Returns nullptr on miss.
  std::shared_ptr<Block> Lookup(const Key& key);
  void Insert(const Key& key, std::shared_ptr<Block> block);

  // Drops every block belonging to a file that has been deleted.
  void EraseFile(uint64_t file_number);

  size_t capacity() const { return capacity_; }
  size_t usage() const;
  uint64_t hits() const { return hits_; }
  uint64_t misses() const { return misses_; }

 private:
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return std::hash<uint64_t>()(k.file_number * 1000003u ^ k.offset);
    }
  };
  struct Entry {
    Key key;
    std::shared_ptr<Block> block;
    size_t charge;
  };
  using List = std::list<Entry>;

  void EvictIfNeeded();  // REQUIRES: mu_ held

  mutable std::mutex mu_;
  size_t capacity_;
  size_t usage_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  List lru_;  // front = most recently used
  std::unordered_map<Key, List::iterator, KeyHash> index_;
};

}  // namespace epica
