// coding.h -- byte-level encoding helpers shared by every on-disk format in
// the engine (WAL, SSTable blocks, MANIFEST, write batches).
//
// Two ideas live here:
//
//   1. Fixed-width little-endian integers (PutFixed32/64, DecodeFixed32/64).
//      Used where the size must be known before reading (headers, footers).
//
//   2. Varints (PutVarint32/64, GetVarint32/64). A varint stores an integer
//      in 1..10 bytes: 7 bits of payload per byte, the high bit set on every
//      byte except the last. Small numbers (key lengths, block sizes) take
//      1-2 bytes instead of 4-8, which matters when there are millions of
//      them in SSTable blocks.
//
//      Example: 300 = 0b1_0010_1100
//        byte0 = 0b1_0101100  (low 7 bits 0101100, continuation bit set)
//        byte1 = 0b0_0000010  (next 7 bits 0000010, no continuation)
//
// All functions are header-only and inline. Everything is little-endian
// regardless of host byte order, so files are portable.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "epica/slice.h"

namespace epica {

// ---------------------------------------------------------------------------
// Fixed-width integers

inline void EncodeFixed32(char* dst, uint32_t v) {
  dst[0] = static_cast<char>(v & 0xff);
  dst[1] = static_cast<char>((v >> 8) & 0xff);
  dst[2] = static_cast<char>((v >> 16) & 0xff);
  dst[3] = static_cast<char>((v >> 24) & 0xff);
}

inline void EncodeFixed64(char* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  EncodeFixed32(buf, v);
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  EncodeFixed64(buf, v);
  dst->append(buf, 8);
}

inline uint32_t DecodeFixed32(const char* p) {
  const auto* b = reinterpret_cast<const unsigned char*>(p);
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* p) {
  const auto* b = reinterpret_cast<const unsigned char*>(p);
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
  return v;
}

// ---------------------------------------------------------------------------
// Varints

inline void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  int n = 0;
  while (v >= 0x80) {
    buf[n++] = static_cast<char>((v & 0x7f) | 0x80);  // 7 bits + continuation
    v >>= 7;
  }
  buf[n++] = static_cast<char>(v);
  dst->append(buf, static_cast<size_t>(n));
}

inline void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  int n = 0;
  while (v >= 0x80) {
    buf[n++] = static_cast<char>((v & 0x7f) | 0x80);
    v >>= 7;
  }
  buf[n++] = static_cast<char>(v);
  dst->append(buf, static_cast<size_t>(n));
}

// Parses a varint from the front of *in, advancing it. Returns false if the
// input ends before the varint does (truncated/corrupt data).
inline bool GetVarint64(Slice* in, uint64_t* out) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && !in->empty(); shift += 7) {
    const auto byte = static_cast<unsigned char>((*in)[0]);
    in->remove_prefix(1);
    result |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
  }
  return false;
}

inline bool GetVarint32(Slice* in, uint32_t* out) {
  uint64_t v;
  if (!GetVarint64(in, &v) || v > 0xffffffffu) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

// Length-prefixed byte strings: varint32 length followed by the bytes.
inline void PutLengthPrefixedSlice(std::string* dst, Slice s) {
  PutVarint32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

inline bool GetLengthPrefixedSlice(Slice* in, Slice* out) {
  uint32_t len;
  if (!GetVarint32(in, &len)) return false;
  if (in->size() < len) return false;
  *out = Slice(in->data(), len);
  in->remove_prefix(len);
  return true;
}

inline int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++len;
  }
  return len;
}

}  // namespace epica
