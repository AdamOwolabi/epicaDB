// bloom.h -- probabilistic "is this key definitely absent?" filter.
//
// Each SSTable carries one bloom filter over its user keys. Before doing any
// disk I/O for a Get, the engine asks the filter. A "no" is always correct,
// so the table is skipped. A "yes" is only probably correct (about 1% false
// positives at 10 bits/key), so the engine goes on to read the block.
//
// Construction: a bit array of (bits_per_key * n) bits, rounded up to whole
// bytes, with k = bits_per_key * ln2 (~0.69) hash probes per key. Each probe
// sets one bit. Lookup checks the same k bits; if any is 0 the key was never
// inserted.
//
// Hashing: one 32-bit hash h, then "double hashing" -- probe i uses
// h + i*delta where delta is h rotated by 17 bits. This is LevelDB's
// approach and is nearly as good as k independent hashes at a fraction of
// the cost (one hash computation per key).
//
// Format: | bit array bytes ... | k (1 byte) |
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epica/slice.h"

namespace epica {

class BloomFilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key);

  // Appends the filter for `keys` onto *dst.
  void CreateFilter(const std::vector<Slice>& keys, std::string* dst) const;

  // Returns false only if key is definitely not in the set the filter was
  // built from.
  bool KeyMayMatch(Slice key, Slice filter) const;

  int bits_per_key() const { return bits_per_key_; }

 private:
  int bits_per_key_;
  int k_;  // number of probes
};

// Murmur-style 32-bit hash used by the bloom filter and the block cache.
uint32_t Hash(const char* data, size_t n, uint32_t seed);

}  // namespace epica
