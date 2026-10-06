// tests/memtable_test.cc -- versioned lookups and iteration in the memtable.
#include "memtable.h"

#include <gtest/gtest.h>

namespace epica {

class MemTableTest : public ::testing::Test {
 protected:
  InternalKeyComparator icmp_;
  std::shared_ptr<MemTable> mem_ = std::make_shared<MemTable>(icmp_);

  // Convenience: Get at a given snapshot sequence.
  //   returns "" + sets *found=false if memtable has no opinion.
  std::string Get(const std::string& key, SequenceNumber seq, bool* found, bool* deleted) {
    LookupKey lk(key, seq);
    std::string v;
    Status s;
    *found = mem_->Get(lk, &v, &s);
    *deleted = *found && s.IsNotFound();
    return v;
  }
};

TEST_F(MemTableTest, PutThenGet) {
  mem_->Add(1, kTypeValue, "a", "apple");
  bool found, deleted;
  EXPECT_EQ("apple", Get("a", 1, &found, &deleted));
  EXPECT_TRUE(found);
  EXPECT_FALSE(deleted);
  Get("b", 1, &found, &deleted);
  EXPECT_FALSE(found) << "unknown key: memtable should have no opinion";
}

TEST_F(MemTableTest, NewestVersionWinsAndSnapshotsSeeOld) {
  mem_->Add(1, kTypeValue, "k", "v1");
  mem_->Add(5, kTypeValue, "k", "v2");
  mem_->Add(9, kTypeDeletion, "k", "");
  bool found, deleted;
  // Latest: deleted.
  Get("k", 100, &found, &deleted);
  EXPECT_TRUE(found);
  EXPECT_TRUE(deleted);
  // Snapshot at 7 sees v2; at 3 sees v1; at 0 sees nothing.
  EXPECT_EQ("v2", Get("k", 7, &found, &deleted));
  EXPECT_EQ("v1", Get("k", 3, &found, &deleted));
  Get("k", 0, &found, &deleted);
  EXPECT_FALSE(found);
}

TEST_F(MemTableTest, IteratorYieldsInternalOrder) {
  mem_->Add(3, kTypeValue, "b", "b3");
  mem_->Add(1, kTypeValue, "a", "a1");
  mem_->Add(2, kTypeValue, "b", "b2");
  std::unique_ptr<Iterator> it(mem_->NewIterator());
  std::vector<std::pair<std::string, uint64_t>> seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ParsedInternalKey p;
    ASSERT_TRUE(ParseInternalKey(it->key(), &p));
    seen.emplace_back(p.user_key.ToString(), p.sequence);
  }
  // user key ascending, then sequence descending
  ASSERT_EQ(3u, seen.size());
  EXPECT_EQ(std::make_pair(std::string("a"), 1ull), seen[0]);
  EXPECT_EQ(std::make_pair(std::string("b"), 3ull), seen[1]);
  EXPECT_EQ(std::make_pair(std::string("b"), 2ull), seen[2]);
}

TEST_F(MemTableTest, MemoryUsageGrows) {
  const size_t before = mem_->ApproximateMemoryUsage();
  for (int i = 0; i < 1000; ++i) mem_->Add(static_cast<SequenceNumber>(i), kTypeValue, "key" + std::to_string(i), std::string(100, 'x'));
  EXPECT_GT(mem_->ApproximateMemoryUsage(), before + 100 * 1000);
  EXPECT_EQ(1000u, mem_->num_entries());
}

TEST(InternalKey, ComparatorOrdersSequenceDescending) {
  InternalKeyComparator c;
  std::string a, b;
  AppendInternalKey(&a, "k", 5, kTypeValue);
  AppendInternalKey(&b, "k", 9, kTypeValue);
  EXPECT_GT(c.Compare(a, b), 0) << "seq 9 must sort before seq 5";
  std::string z;
  AppendInternalKey(&z, "z", 1, kTypeValue);
  EXPECT_LT(c.Compare(a, z), 0) << "user key order comes first";
}

}  // namespace epica
