// memtable.h -- the in-memory, sorted write buffer at the top of the LSM tree.
//
// Every write lands here first (right after it is logged to the WAL). Reads
// check here first because it holds the newest data. When it grows past
// Options::memtable_size it is frozen ("immutable memtable"), a fresh one
// takes its place, and a background thread writes the frozen one out as an
// SSTable.
//
// Storage: an Arena-backed SkipList whose keys are `const char*` pointing at
// entries laid out like this inside the arena:
//
//   | varint32 internal_key_len | user_key | fixed64 seq<<8|type | varint32 vlen | value |
//   ^ skiplist key points here
//
// Encoding the length in front lets the comparator find the internal key
// without any side table, and keeps each entry a single contiguous copy.
//
// Deletes are stored as entries too ("tombstones", type kTypeDeletion). The
// memtable never removes anything; that is compaction's job.
//
// Thread safety: one writer at a time (the DB's writer lock), any number of
// concurrent readers with no locks (see skiplist.h). Lifetime is managed by
// std::shared_ptr so a reader holding a reference keeps a flushed memtable
// alive until it is done.
#pragma once

#include <memory>
#include <string>

#include "arena.h"
#include "epica/iterator.h"
#include "epica/status.h"
#include "internal_key.h"
#include "skiplist.h"

namespace epica {

class MemTable {
 public:
  explicit MemTable(const InternalKeyComparator& cmp);
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Approximate bytes held (arena usage). Compared against memtable_size.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  // Adds an entry. For deletes, `value` is ignored (empty).
  void Add(SequenceNumber seq, ValueType type, Slice key, Slice value);

  // Looks up key.user_key() at or before key's sequence number.
  //   returns true, s=Ok        -> found, *value filled
  //   returns true, s=NotFound  -> found a tombstone (key is deleted)
  //   returns false             -> memtable has no opinion; check older data
  bool Get(const LookupKey& key, std::string* value, Status* s);

  // Iterates over internal keys in sorted order. key() is the internal key,
  // value() the user value. Caller owns the iterator; it must not outlive
  // the memtable (hold the shared_ptr).
  Iterator* NewIterator();

  uint64_t num_entries() const { return num_entries_; }

 private:
  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    // Decodes the length prefix of each entry and compares internal keys.
    int operator()(const char* a, const char* b) const;
  };
  friend class MemTableIterator;

  using Table = SkipList<const char*, KeyComparator>;

  KeyComparator comparator_;
  Arena arena_;
  Table table_;
  uint64_t num_entries_ = 0;
};

}  // namespace epica
