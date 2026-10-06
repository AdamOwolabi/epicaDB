// bloom.cc -- see bloom.h.

#include "bloom.h"

#include "coding.h"

namespace epica {

// Same hash LevelDB uses (a simplified MurmurHash). Good mixing, fast, and
// deterministic across platforms so filters written on one machine read
// correctly on another.
uint32_t Hash(const char* data, size_t n, uint32_t seed) {
  const uint32_t m = 0xc6a4a793;
  const uint32_t r = 24;
  const char* limit = data + n;
  uint32_t h = seed ^ (static_cast<uint32_t>(n) * m);

  while (data + 4 <= limit) {
    uint32_t w = DecodeFixed32(data);
    data += 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }
  switch (limit - data) {
    case 3:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[0]));
      h *= m;
      h ^= (h >> r);
      break;
  }
  return h;
}

static uint32_t BloomHash(Slice key) { return Hash(key.data(), key.size(), 0xbc9f1d34); }

BloomFilterPolicy::BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
  // Optimal k = bits_per_key * ln(2). Clamp to a sane range.
  k_ = static_cast<int>(bits_per_key * 0.69);
  if (k_ < 1) k_ = 1;
  if (k_ > 30) k_ = 30;
}

void BloomFilterPolicy::CreateFilter(const std::vector<Slice>& keys, std::string* dst) const {
  size_t bits = keys.size() * static_cast<size_t>(bits_per_key_);
  if (bits < 64) bits = 64;  // tiny filters have high FP rates; pad them
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  const size_t init_size = dst->size();
  dst->resize(init_size + bytes, 0);
  dst->push_back(static_cast<char>(k_));  // remember k in the filter itself
  char* array = &(*dst)[init_size];

  for (const Slice& key : keys) {
    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // rotate right 17 bits
    for (int j = 0; j < k_; j++) {
      const uint32_t bitpos = h % static_cast<uint32_t>(bits);
      array[bitpos / 8] |= static_cast<char>(1 << (bitpos % 8));
      h += delta;
    }
  }
}

bool BloomFilterPolicy::KeyMayMatch(Slice key, Slice filter) const {
  const size_t len = filter.size();
  if (len < 2) return false;

  const char* array = filter.data();
  const size_t bits = (len - 1) * 8;
  const int k = static_cast<unsigned char>(array[len - 1]);
  if (k > 30) return true;  // reserved for future encodings: be conservative

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int j = 0; j < k; j++) {
    const uint32_t bitpos = h % static_cast<uint32_t>(bits);
    if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
    h += delta;
  }
  return true;
}

}  // namespace epica
