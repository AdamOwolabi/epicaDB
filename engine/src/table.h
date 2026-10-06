// table.h -- SSTable (Sorted String Table): the immutable on-disk file
// format that holds a memtable's contents, or the merged output of a
// compaction.
//
// File layout:
//
//   +---------------------+
//   | data block 0        |  sorted internal keys -> values, ~4 KiB each
//   | data block 1        |  each followed by | fixed32 num_entries | fixed32 crc32c |
//   | ...                 |
//   +---------------------+
//   | filter block        |  one bloom filter over every user key in the file
//   +---------------------+
//   | index block         |  one entry per data block:
//   |                     |    key   = last internal key in that block
//   |                     |    value = BlockHandle(offset, size) of that block
//   +---------------------+
//   | footer (48 bytes)   |  | filter handle | index handle | padding | fixed64 magic |
//   +---------------------+
//
// Reading a key:
//   1. Footer -> index block (read once at open, kept in memory).
//   2. Binary search the index for the first block whose last key >= target.
//   3. Ask the bloom filter; if "definitely absent", stop -- zero data I/O.
//   4. Fetch that one data block (block cache first, then pread), scan it.
//
// Why "last key" as the index key: a Seek in the index for target T lands on
// the first block whose last key is >= T, which is exactly the only block
// that can contain T.
//
// Files are never modified after Finish(); they are only ever created by a
// flush/compaction and deleted when no version references them.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "block.h"
#include "bloom.h"
#include "cache.h"
#include "env.h"
#include "epica/iterator.h"
#include "epica/options.h"
#include "epica/status.h"
#include "internal_key.h"

namespace epica {

constexpr uint64_t kTableMagicNumber = 0x45504943'41444200ull;  // "EPICADB\0"
constexpr size_t kFooterSize = 2 * BlockHandle::kMaxEncodedLength + 8;

class TableBuilder {
 public:
  // Takes ownership of nothing; `file` must outlive the builder.
  TableBuilder(const Options& options, WritableFile* file);
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // Keys must be internal keys in strictly increasing InternalKeyComparator
  // order.
  void Add(Slice internal_key, Slice value);

  // Writes filter block, index block and footer. Does NOT sync/close the
  // file; the caller does that so it can decide on fsync policy.
  Status Finish();

  uint64_t NumEntries() const { return num_entries_; }
  uint64_t FileSize() const { return offset_; }
  Status status() const { return status_; }

 private:
  void FlushDataBlock();
  // Appends block contents + crc trailer to the file; fills *handle.
  void WriteRawBlock(Slice contents, BlockHandle* handle);

  Options options_;
  WritableFile* file_;
  uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  uint64_t num_entries_ = 0;

  // Bloom filter inputs: user keys of every entry (copied; the file may have
  // millions of keys but this is bounded by memtable_size in practice).
  std::string filter_keys_flat_;
  std::vector<size_t> filter_key_starts_;
  std::unique_ptr<BloomFilterPolicy> filter_policy_;

  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;
};

class Table {
 public:
  // Opens the file, reads footer + index + filter. `cache` may be null.
  static Status Open(const Options& options, const std::string& path, uint64_t file_number,
                     std::shared_ptr<BlockCache> cache, std::unique_ptr<Table>* out);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Iterates over every (internal_key, value) in the file in order.
  Iterator* NewIterator() const;

  // Finds the first entry with internal key >= `internal_key` in the block
  // that could contain it, and calls handle(found_key, found_value) if such
  // an entry exists in that block. Returns Ok even when nothing is found;
  // the callback decides what a hit means (see Version::Get).
  Status InternalGet(Slice internal_key, void* arg,
                     void (*handle)(void* arg, Slice k, Slice v)) const;

  uint64_t file_number() const { return file_number_; }
  uint64_t file_size() const { return file_->size(); }

 private:
  friend class TableIterator;
  Table() = default;

  // Reads one data block, using the cache when possible.
  Status ReadDataBlock(const BlockHandle& handle, std::shared_ptr<Block>* out) const;

  uint64_t file_number_ = 0;
  std::unique_ptr<RandomAccessFile> file_;
  std::shared_ptr<BlockCache> cache_;
  std::unique_ptr<Block> index_block_;
  std::string filter_data_;
  std::unique_ptr<BloomFilterPolicy> filter_policy_;
  InternalKeyComparator icmp_;
};

// Reads a block plus its crc trailer at `handle`, verifies the crc, and
// returns the decoded block. Exposed for tests.
Status ReadBlockFromFile(const RandomAccessFile* file, const BlockHandle& handle,
                         std::unique_ptr<Block>* out);

}  // namespace epica
