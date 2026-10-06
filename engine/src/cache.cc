// cache.cc -- see cache.h.

#include "cache.h"

namespace epica {

std::shared_ptr<Block> BlockCache::Lookup(const Key& key) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = index_.find(key);
  if (it == index_.end()) {
    ++misses_;
    return nullptr;
  }
  ++hits_;
  // Move to front: it is now the most recently used.
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->block;
}

void BlockCache::Insert(const Key& key, std::shared_ptr<Block> block) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = index_.find(key);
  if (it != index_.end()) {  // already present: refresh
    lru_.splice(lru_.begin(), lru_, it->second);
    return;
  }
  const size_t charge = block->size() + sizeof(Entry);
  lru_.push_front(Entry{key, std::move(block), charge});
  index_[key] = lru_.begin();
  usage_ += charge;
  EvictIfNeeded();
}

void BlockCache::EraseFile(uint64_t file_number) {
  std::lock_guard<std::mutex> l(mu_);
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (it->key.file_number == file_number) {
      usage_ -= it->charge;
      index_.erase(it->key);
      it = lru_.erase(it);
    } else {
      ++it;
    }
  }
}

void BlockCache::EvictIfNeeded() {
  while (usage_ > capacity_ && !lru_.empty()) {
    Entry& victim = lru_.back();  // least recently used
    usage_ -= victim.charge;
    index_.erase(victim.key);
    lru_.pop_back();
  }
}

size_t BlockCache::usage() const {
  std::lock_guard<std::mutex> l(mu_);
  return usage_;
}

}  // namespace epica
