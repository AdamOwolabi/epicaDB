// skiplist.h -- the ordered in-memory index behind the memtable.
//
// A skiplist is a sorted linked list with "express lanes". Level 0 links every
// node; level 1 links roughly every 4th node; level 2 every 16th, and so on
// (kBranching = 4). Search starts at the top lane, runs right while the next
// key is still smaller than the target, drops one lane, and repeats. Expected
// cost is O(log n) for search and insert, like a balanced tree, but with far
// simpler code and -- crucially -- a lock-free story for readers.
//
//   level 2:  head ---------------------> 30 -------------------> nil
//   level 1:  head -------> 10 ---------> 30 -------> 50 -------> nil
//   level 0:  head -> 5 -> 10 -> 20 -> 30 -> 40 -> 50 -> 60 -> nil
//
//   Seek(40): start at head level 2 -> 30 (<40, advance) -> nil (stop, drop)
//             level 1 from 30 -> 50 (>=40, drop)
//             level 0 from 30 -> 40 (found)
//
// Concurrency contract (same as LevelDB):
//   * Exactly one thread may Insert at a time; the DB serialises writers.
//   * Any number of threads may read concurrently with that writer, without
//     locks. This works because a node is fully built before it is published,
//     the `next` pointers are std::atomic, and we never delete or move nodes
//     (the Arena keeps them alive until the whole list is dropped).
//
// Template parameters: Key is a trivially-copyable handle (the memtable uses
// `const char*` pointing into the arena) and Comparator is a functor
// `int operator()(const Key&, const Key&)` returning <0, 0, >0.
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <random>

#include "arena.h"

namespace epica {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  // The arena must outlive the skiplist; every node is allocated from it.
  SkipList(Comparator cmp, Arena* arena);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Inserts key. REQUIRES: nothing equal to key is already in the list.
  // (The memtable guarantees this: every internal key carries a unique
  // sequence number.)
  void Insert(const Key& key);

  bool Contains(const Key& key) const;

  // Forward-only cursor. Thread-safe against a concurrent Insert.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    // Positions at the first node with key >= target.
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  static constexpr int kMaxHeight = 12;
  static constexpr int kBranching = 4;

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }
  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return n != nullptr && compare_(n->key, key) < 0;
  }

  // Returns the first node >= key. If prev != nullptr, fills prev[level] with
  // the last node < key at every level (the insertion points).
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
  std::mt19937 rnd_;
};

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  // Readers use acquire so they see a fully-initialised node once they see
  // the pointer; the writer publishes with release.
  Node* Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_release);
  }
  // Used while building a node nobody else can see yet.
  void NoBarrier_SetNext(int n, Node* x) { next_[n].store(x, std::memory_order_relaxed); }

 private:
  // Flexible array trick: NewNode allocates extra space so next_[1..height-1]
  // exists past the end of the struct.
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key,
                                                                             int height) {
  char* mem = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  return new (mem) Node(key);
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // Geometric distribution: each extra level with probability 1/kBranching.
  int height = 1;
  while (height < kMaxHeight && (rnd_() % kBranching) == 0) height++;
  return height;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // keep moving right in this lane
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      level--;  // drop to a slower, denser lane
    }
  }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(Key(), kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int i = 0; i < kMaxHeight; i++) head_->SetNext(i, nullptr);
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);
  assert(x == nullptr || !Equal(key, x->key));  // no duplicates

  const int height = RandomHeight();
  if (height > GetMaxHeight()) {
    for (int i = GetMaxHeight(); i < height; i++) prev[i] = head_;
    // Publishing the new height before the new node is fine: a reader that
    // sees the taller height simply finds nullptr in the new lanes and drops.
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; i++) {
    // Link new node to its successor first (nobody can see x yet), then
    // publish x by pointing the predecessor at it (release store).
    x->NoBarrier_SetNext(i, prev[i]->Next(i));
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Equal(key, x->key);
}

}  // namespace epica
