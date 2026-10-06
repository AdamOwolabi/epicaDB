// crc32c.h -- CRC-32C checksum used to detect corruption in WAL records,
// SSTable blocks, and the MANIFEST. A flipped bit anywhere in a protected
// region changes the checksum, so the reader reports Corruption instead of
// returning wrong data.
#pragma once

#include <cstddef>
#include <cstdint>

namespace epica::crc32c {

// CRC-32C (Castagnoli, polynomial 0x1EDC6F41), as used by iSCSI, ext4, and
// LevelDB/RocksDB. Table-driven; no hardware acceleration yet.

// Extend a running CRC with more bytes. Start with `crc = 0`.
uint32_t Extend(uint32_t crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// Masking as in LevelDB: a CRC stored inside data that is itself CRC'd would
// otherwise yield degenerate results. Not used by the WAL today (the CRC is
// stored outside the covered range) but cheap to have for later formats.
inline uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + 0xa282ead8u; }
inline uint32_t Unmask(uint32_t masked) {
  uint32_t rot = masked - 0xa282ead8u;
  return (rot >> 17) | (rot << 15);
}

}  // namespace epica::crc32c
