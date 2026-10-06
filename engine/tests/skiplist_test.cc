// tests/skiplist_test.cc -- ordering, seek semantics, and the
// single-writer/multi-reader concurrency contract of the skiplist.
#include "skiplist.h"

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>

namespace epica {

struct IntCmp {
  int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};
using List = SkipList<uint64_t, IntCmp>;

TEST(SkipList, EmptyList) {
  Arena arena;
  List list(IntCmp{}, &arena);
  EXPECT_FALSE(list.Contains(10));
  List::Iterator it(&list);
  EXPECT_FALSE(it.Valid());
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek(100);
  EXPECT_FALSE(it.Valid());
}

TEST(SkipList, InsertAndIterateInOrder) {
  Arena arena;
  List list(IntCmp{}, &arena);
  std::set<uint64_t> expected;
  // Insert in a scrambled order; iteration must come out sorted.
  for (uint64_t i = 0; i < 2000; ++i) {
    uint64_t k = (i * 7919) % 2003;
    if (expected.insert(k).second) list.Insert(k);
  }
  List::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t k : expected) {
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(k, it.key());
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
}

TEST(SkipList, SeekFindsFirstGreaterOrEqual) {
  Arena arena;
  List list(IntCmp{}, &arena);
  for (uint64_t k : {10u, 20u, 30u, 40u}) list.Insert(k);
  List::Iterator it(&list);
  it.Seek(20);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(20u, it.key());
  it.Seek(25);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(30u, it.key());
  it.Seek(41);
  EXPECT_FALSE(it.Valid());
  it.Seek(0);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(10u, it.key());
}

// One writer inserts increasing keys while readers keep scanning. Readers
// must only ever see a sorted prefix of what was inserted -- never a
// half-linked node, never a key out of order.
TEST(SkipList, ConcurrentReadersWithOneWriter) {
  Arena arena;
  List list(IntCmp{}, &arena);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> written{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        const uint64_t snapshot = written.load(std::memory_order_acquire);
        List::Iterator it(&list);
        it.SeekToFirst();
        uint64_t prev = 0, count = 0;
        while (it.Valid()) {
          const uint64_t k = it.key();
          ASSERT_TRUE(count == 0 || k > prev) << "out of order";
          prev = k;
          ++count;
          it.Next();
        }
        // Everything written before we started must be visible.
        ASSERT_GE(count, snapshot);
      }
    });
  }
  for (uint64_t k = 1; k <= 20000; ++k) {
    list.Insert(k);
    written.store(k, std::memory_order_release);
  }
  stop = true;
  for (auto& t : readers) t.join();
}

}  // namespace epica
