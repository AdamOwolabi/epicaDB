// internal_key.h -- how the engine tags every key with a version.
//
// A user writes Put("k", "v1"), then Put("k", "v2"), then Delete("k"). The
// engine must keep all three for a while: an old snapshot may still need
// "v1", and the delete must shadow "v2" until compaction physically removes
// both. So the engine never stores bare user keys. It stores *internal keys*:
//
//   internal_key = user_key | fixed64( sequence << 8 | type )
//
//   sequence: 56-bit counter, incremented for every write. Bigger = newer.
//   type:     kTypeDeletion (0) or kTypeValue (1).
//
// Ordering (InternalKeyComparator): user key ascending, then sequence
// DESCENDING. Newest version of a key sorts first. That single choice makes
// "find the newest version visible at snapshot S" a plain Seek:
//
//   entries for "k":   k@7:Delete   k@5:Put(v2)   k@3:Put(v1)
//   Seek(k@5) lands on k@5 -> visible value at snapshot 5 is v2.
//   Seek(k@9) lands on k@7 -> at snapshot 9, k is deleted.
//
// LookupKey packages the memtable form (varint32 length prefix + internal
// key) and the plain internal key form for one Get call.
#pragma once

#include <cstdint>
#include <string>

#include "coding.h"
#include "epica/slice.h"

namespace epica {

using SequenceNumber = uint64_t;

// 56 bits for sequence, leaving 8 for the type byte.
constexpr SequenceNumber kMaxSequenceNumber = ((1ull << 56) - 1);

enum ValueType : uint8_t {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
};

// When seeking for "everything at or before sequence S", we build the tag
// with the highest type so that the entry with exactly sequence S sorts
// after our probe... in descending-seq order this means the probe finds the
// entry with sequence S itself if it exists. LevelDB uses the same trick.
constexpr ValueType kValueTypeForSeek = kTypeValue;

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
  return (seq << 8) | t;
}

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;
};

// Splits an internal key. Returns false if it is too short or has a bogus
// type byte -- which only happens on corruption.
inline bool ParseInternalKey(Slice internal_key, ParsedInternalKey* out) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  const uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  const auto c = static_cast<uint8_t>(num & 0xff);
  out->sequence = num >> 8;
  out->type = static_cast<ValueType>(c);
  out->user_key = Slice(internal_key.data(), n - 8);
  return c <= kTypeValue;
}

inline Slice ExtractUserKey(Slice internal_key) {
  return Slice(internal_key.data(), internal_key.size() - 8);
}

inline void AppendInternalKey(std::string* dst, Slice user_key, SequenceNumber seq, ValueType t) {
  dst->append(user_key.data(), user_key.size());
  PutFixed64(dst, PackSequenceAndType(seq, t));
}

// Orders internal keys: user key ascending, sequence descending.
class InternalKeyComparator {
 public:
  int Compare(Slice a, Slice b) const {
    int r = ExtractUserKey(a).compare(ExtractUserKey(b));
    if (r == 0) {
      const uint64_t anum = DecodeFixed64(a.data() + a.size() - 8);
      const uint64_t bnum = DecodeFixed64(b.data() + b.size() - 8);
      if (anum > bnum) r = -1;       // higher sequence sorts first
      else if (anum < bnum) r = +1;
    }
    return r;
  }
  int operator()(Slice a, Slice b) const { return Compare(a, b); }
};

// A LookupKey is what Get() hands to the memtable and tables:
//
//   | varint32 internal_key_len | user_key | fixed64(seq<<8 | kValueTypeForSeek) |
//   ^ memtable_key()            ^ internal_key()
//                               ^ user_key() ------------^
class LookupKey {
 public:
  LookupKey(Slice user_key, SequenceNumber seq) {
    PutVarint32(&buf_, static_cast<uint32_t>(user_key.size() + 8));
    kstart_ = buf_.size();
    AppendInternalKey(&buf_, user_key, seq, kValueTypeForSeek);
  }
  Slice memtable_key() const { return Slice(buf_); }
  Slice internal_key() const { return Slice(buf_.data() + kstart_, buf_.size() - kstart_); }
  Slice user_key() const { return Slice(buf_.data() + kstart_, buf_.size() - kstart_ - 8); }

 private:
  std::string buf_;
  size_t kstart_;
};

}  // namespace epica
