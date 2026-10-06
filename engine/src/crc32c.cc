// crc32c.cc -- table-driven CRC-32C. The 256-entry table is computed at
// compile time (constexpr), so there is no start-up cost and no
// initialisation-order hazard.
#include "crc32c.h"

#include <array>

namespace epica::crc32c {
namespace {

constexpr uint32_t kPoly = 0x82F63B78u;  // reflected form of 0x1EDC6F41

constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint32_t c = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

}  // namespace epica::crc32c
