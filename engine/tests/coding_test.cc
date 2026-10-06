// tests/coding_test.cc -- fixed-width and varint encoding round trips.
#include "coding.h"

#include <gtest/gtest.h>

namespace epica {

TEST(Coding, Fixed32And64RoundTrip) {
  std::string s;
  PutFixed32(&s, 0xdeadbeef);
  PutFixed64(&s, 0x0123456789abcdefull);
  ASSERT_EQ(12u, s.size());
  EXPECT_EQ(0xdeadbeefu, DecodeFixed32(s.data()));
  EXPECT_EQ(0x0123456789abcdefull, DecodeFixed64(s.data() + 4));
  // Little-endian on disk regardless of host.
  EXPECT_EQ('\xef', s[0]);
}

TEST(Coding, VarintSizes) {
  // 7 bits per byte: boundaries at 128, 16384, ...
  EXPECT_EQ(1, VarintLength(0));
  EXPECT_EQ(1, VarintLength(127));
  EXPECT_EQ(2, VarintLength(128));
  EXPECT_EQ(2, VarintLength(16383));
  EXPECT_EQ(3, VarintLength(16384));
  EXPECT_EQ(10, VarintLength(~0ull));
}

TEST(Coding, VarintRoundTrip) {
  std::string s;
  std::vector<uint64_t> values = {0, 1, 127, 128, 300, 16383, 16384, 1ull << 32, ~0ull};
  for (uint64_t v : values) PutVarint64(&s, v);
  Slice in(s);
  for (uint64_t v : values) {
    uint64_t got;
    ASSERT_TRUE(GetVarint64(&in, &got));
    EXPECT_EQ(v, got);
  }
  EXPECT_TRUE(in.empty());
}

TEST(Coding, Varint300Bytes) {
  // Worked example from coding.h: 300 = 0xAC 0x02.
  std::string s;
  PutVarint32(&s, 300);
  ASSERT_EQ(2u, s.size());
  EXPECT_EQ('\xac', s[0]);
  EXPECT_EQ('\x02', s[1]);
}

TEST(Coding, TruncatedVarintFails) {
  std::string s("\x80", 1);  // continuation bit set, nothing follows
  Slice in(s);
  uint64_t v;
  EXPECT_FALSE(GetVarint64(&in, &v));
}

TEST(Coding, LengthPrefixedSlice) {
  std::string s;
  PutLengthPrefixedSlice(&s, "hello");
  PutLengthPrefixedSlice(&s, "");
  Slice in(s), a, b;
  ASSERT_TRUE(GetLengthPrefixedSlice(&in, &a));
  ASSERT_TRUE(GetLengthPrefixedSlice(&in, &b));
  EXPECT_EQ("hello", a.ToString());
  EXPECT_TRUE(b.empty());
  EXPECT_FALSE(GetLengthPrefixedSlice(&in, &a));
}

}  // namespace epica
