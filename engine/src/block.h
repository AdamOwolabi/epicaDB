// block.h -- the unit of storage inside an SSTable ("multi-block storage").
//
// An SSTable is a sequence of blocks, each about Options::block_size (4 KiB)
// of sorted key/value entries. Reads never load a whole table: the index
// says which block might hold the key, and only that block is fetched (and
// cached). Writes never rewrite a block: SSTables are immutable once built.
//
// Block layout on disk:
//
//   | entry | entry | ... | entry | fixed32 num_entries | fixed32 crc32c |
//   entry = | varint32 klen | varint32 vlen | key bytes | value bytes |
//
// The crc32c covers everything before it. A flipped bit anywhere in the
// block is detected on read and surfaces as Status::Corruption instead of a
// wrong answer.
//
// Entries within a block are scanned linearly on Seek. With 4 KiB blocks
// that is at most a few dozen entries, so the binary search over the index
// block (one entry per data block) does the heavy lifting. LevelDB adds
// "restart points" for in-block binary search plus prefix compression; we
// left both out on purpose (see DESIGN_DECISIONS.md).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "epica/iterator.h"
#include "epica/slice.h"
#include "epica/status.h"
#include "internal_key.h"

namespace epica {

// A BlockHandle says where a block lives in the file: | varint64 offset | varint64 size |.
struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;  // excludes the 8-byte trailer (num_entries + crc)

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);
  static constexpr size_t kMaxEncodedLength = 10 + 10;
};

// Every data block ends with this many bytes after the entries.
constexpr size_t kBlockTrailerSize = 4 + 4;

class BlockBuilder {
 public:
  BlockBuilder() = default;
  void Reset();
  // Keys must be added in strictly increasing order.
  void Add(Slice key, Slice value);
  // Appends the trailer (num_entries) and returns the block contents. The CRC
  // is added by TableBuilder because it also covers nothing else here.
  Slice Finish();
  size_t CurrentSizeEstimate() const { return buffer_.size() + 4; }
  bool empty() const { return counter_ == 0; }
  const std::string& last_key() const { return last_key_; }

 private:
  std::string buffer_;
  std::string last_key_;
  uint32_t counter_ = 0;
  bool finished_ = false;
};

// A decoded, immutable block in memory. Shared between the block cache and
// any live iterators via shared_ptr.
class Block {
 public:
  // `contents` excludes the crc (it has already been verified) but includes
  // the num_entries trailer.
  explicit Block(std::string contents);
  size_t size() const { return data_.size(); }
  uint32_t num_entries() const { return num_entries_; }

  // Iterator over (key, value) pairs; the comparator orders keys.
  Iterator* NewIterator(const InternalKeyComparator* cmp) const;

 private:
  friend class BlockIter;
  std::string data_;
  uint32_t num_entries_;
  size_t entries_end_;  // offset where the trailer begins
};

}  // namespace epica
