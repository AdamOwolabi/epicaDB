// tests/crc32c_test.cc -- known-answer vectors for CRC-32C so the checksum
// matches every other implementation (iSCSI, LevelDB, Java's CRC32C).
#include "crc32c.h"

#include <gtest/gtest.h>

#include <string>

namespace epica::crc32c {

TEST(Crc32c, KnownVectors) {
  EXPECT_EQ(0u, Value("", 0));
  // Standard CRC-32C check value.
  EXPECT_EQ(0xE3069283u, Value("123456789", 9));
  // From the LevelDB test suite.
  char buf[32];
  std::memset(buf, 0, sizeof(buf));
  EXPECT_EQ(0x8a9136aau, Value(buf, sizeof(buf)));
  std::memset(buf, 0xff, sizeof(buf));
  EXPECT_EQ(0x62a8ab43u, Value(buf, sizeof(buf)));
}

TEST(Crc32c, ExtendIsIncremental) {
  const std::string s = "hello world";
  uint32_t whole = Value(s.data(), s.size());
  uint32_t part = Extend(0, s.data(), 5);
  part = Extend(part, s.data() + 5, s.size() - 5);
  EXPECT_EQ(whole, part);
}

TEST(Crc32c, MaskRoundTrip) {
  uint32_t crc = Value("foo", 3);
  EXPECT_NE(crc, Mask(crc));
  EXPECT_EQ(crc, Unmask(Mask(crc)));
}

}  // namespace epica::crc32c
