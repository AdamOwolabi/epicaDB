// tests/write_batch_test.cc -- encoding of atomic write groups.
#include "epica/write_batch.h"

#include <gtest/gtest.h>

namespace epica {

namespace {
struct Recorder : WriteBatch::Handler {
  std::vector<std::string> ops;
  void Put(Slice k, Slice v) override { ops.push_back("Put(" + k.ToString() + "," + v.ToString() + ")"); }
  void Delete(Slice k) override { ops.push_back("Delete(" + k.ToString() + ")"); }
};
}  // namespace

TEST(WriteBatch, Empty) {
  WriteBatch b;
  EXPECT_EQ(0u, b.Count());
  EXPECT_EQ(WriteBatch::kHeader, b.ByteSize());
  Recorder r;
  EXPECT_TRUE(b.Iterate(&r).ok());
  EXPECT_TRUE(r.ops.empty());
}

TEST(WriteBatch, IterateInOrder) {
  WriteBatch b;
  b.Put("a", "1");
  b.Delete("b");
  b.Put("c", "");
  b.SetSequence(42);
  EXPECT_EQ(3u, b.Count());
  EXPECT_EQ(42u, b.Sequence());
  Recorder r;
  ASSERT_TRUE(b.Iterate(&r).ok());
  ASSERT_EQ(3u, r.ops.size());
  EXPECT_EQ("Put(a,1)", r.ops[0]);
  EXPECT_EQ("Delete(b)", r.ops[1]);
  EXPECT_EQ("Put(c,)", r.ops[2]);
}

TEST(WriteBatch, ContentsRoundTripThroughBytes) {
  // This is what happens on WAL replay: raw bytes -> batch -> ops.
  WriteBatch b;
  b.Put("k", std::string("bin\0ary", 7));
  WriteBatch copy;
  copy.SetContents(b.Contents());
  Recorder r;
  ASSERT_TRUE(copy.Iterate(&r).ok());
  EXPECT_EQ(1u, copy.Count());
  EXPECT_EQ(std::string("Put(k,bin\0ary)", 14), r.ops[0]);
}

TEST(WriteBatch, AppendMergesForGroupCommit) {
  WriteBatch a, b;
  a.Put("x", "1");
  b.Delete("y");
  b.Put("z", "3");
  a.Append(b);
  EXPECT_EQ(3u, a.Count());
  Recorder r;
  ASSERT_TRUE(a.Iterate(&r).ok());
  EXPECT_EQ("Delete(y)", r.ops[1]);
}

TEST(WriteBatch, CorruptCountIsDetected) {
  WriteBatch b;
  b.Put("a", "1");
  std::string bytes = b.Contents();
  bytes[8] = 5;  // claim 5 ops
  WriteBatch bad;
  bad.SetContents(bytes);
  Recorder r;
  EXPECT_TRUE(bad.Iterate(&r).IsCorruption());
}

}  // namespace epica
