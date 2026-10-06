// tests/cache_test.cc -- LRU eviction order and file-scoped erase.
#include "cache.h"

#include <gtest/gtest.h>

namespace epica {

static std::shared_ptr<Block> MakeBlock(size_t bytes) {
  return std::make_shared<Block>(std::string(bytes, 'x'));
}

TEST(BlockCache, HitAndMiss) {
  BlockCache c(1 << 20);
  EXPECT_EQ(nullptr, c.Lookup({1, 0}));
  c.Insert({1, 0}, MakeBlock(100));
  auto b = c.Lookup({1, 0});
  ASSERT_NE(nullptr, b);
  EXPECT_EQ(100u, b->size());
  EXPECT_EQ(1u, c.hits());
  EXPECT_EQ(1u, c.misses());
}

TEST(BlockCache, EvictsLeastRecentlyUsed) {
  // Each entry charges size + bookkeeping; capacity fits about 3 x 1000.
  BlockCache c(3 * 1000 + 3 * 200);
  c.Insert({1, 0}, MakeBlock(1000));
  c.Insert({1, 1}, MakeBlock(1000));
  c.Insert({1, 2}, MakeBlock(1000));
  ASSERT_NE(nullptr, c.Lookup({1, 0}));  // touch 0: now 1 is the LRU
  c.Insert({1, 3}, MakeBlock(1000));     // over budget: evict 1
  EXPECT_EQ(nullptr, c.Lookup({1, 1}));
  EXPECT_NE(nullptr, c.Lookup({1, 0}));
  EXPECT_NE(nullptr, c.Lookup({1, 2}));
  EXPECT_NE(nullptr, c.Lookup({1, 3}));
}

TEST(BlockCache, EraseFileDropsOnlyThatFile) {
  BlockCache c(1 << 20);
  c.Insert({7, 0}, MakeBlock(10));
  c.Insert({7, 1}, MakeBlock(10));
  c.Insert({8, 0}, MakeBlock(10));
  c.EraseFile(7);
  EXPECT_EQ(nullptr, c.Lookup({7, 0}));
  EXPECT_EQ(nullptr, c.Lookup({7, 1}));
  EXPECT_NE(nullptr, c.Lookup({8, 0}));
}

TEST(BlockCache, EvictedBlockStaysAliveForHolders) {
  BlockCache c(500);
  auto b = MakeBlock(400);
  c.Insert({1, 0}, b);
  c.Insert({1, 1}, MakeBlock(400));  // evicts the first
  EXPECT_EQ(nullptr, c.Lookup({1, 0}));
  EXPECT_EQ(400u, b->size()) << "our shared_ptr keeps it valid";
}

}  // namespace epica
