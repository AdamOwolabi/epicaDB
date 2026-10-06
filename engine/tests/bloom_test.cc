// tests/bloom_test.cc -- no false negatives, and a sane false-positive rate.
#include "bloom.h"

#include <gtest/gtest.h>

namespace epica {

static std::string Key(int i) { return "key-" + std::to_string(i); }

TEST(Bloom, EmptyFilterMatchesNothing) {
  BloomFilterPolicy p(10);
  std::string filter;
  p.CreateFilter({}, &filter);
  EXPECT_FALSE(p.KeyMayMatch("anything", filter));
}

TEST(Bloom, NoFalseNegatives) {
  BloomFilterPolicy p(10);
  std::vector<std::string> keys;
  for (int i = 0; i < 10000; ++i) keys.push_back(Key(i));
  std::vector<Slice> slices(keys.begin(), keys.end());
  std::string filter;
  p.CreateFilter(slices, &filter);
  for (const auto& k : keys) EXPECT_TRUE(p.KeyMayMatch(k, filter)) << k;
}

TEST(Bloom, FalsePositiveRateIsLow) {
  BloomFilterPolicy p(10);
  std::vector<std::string> keys;
  for (int i = 0; i < 10000; ++i) keys.push_back(Key(i));
  std::vector<Slice> slices(keys.begin(), keys.end());
  std::string filter;
  p.CreateFilter(slices, &filter);
  // Roughly 10 bits/key + 1 byte for k.
  EXPECT_NEAR(10000 * 10 / 8, static_cast<int>(filter.size()), 16);

  int fp = 0;
  for (int i = 0; i < 10000; ++i) {
    if (p.KeyMayMatch("absent-" + std::to_string(i), filter)) ++fp;
  }
  // Theory says ~0.8-1% at 10 bits/key; allow up to 2%.
  EXPECT_LT(fp, 200) << "false positive rate too high: " << fp / 100.0 << "%";
}

TEST(Bloom, HashIsDeterministic) {
  EXPECT_EQ(Hash("abc", 3, 1), Hash("abc", 3, 1));
  EXPECT_NE(Hash("abc", 3, 1), Hash("abd", 3, 1));
}

}  // namespace epica
