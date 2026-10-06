// iterator.h -- the one cursor abstraction every layer of the engine speaks.
//
// Memtables, SSTable blocks, whole SSTables, merged multi-source views, and
// the user-facing DB scan all expose this same interface. That uniformity is
// what lets a MergingIterator combine any mix of children without knowing
// what they are, and lets compaction reuse the exact same code path as a
// user's range scan.
//
// Protocol:
//   it->Seek("m");                 // position at first key >= "m"
//   while (it->Valid()) {          // false once past the end
//     use(it->key(), it->value()); // Slices valid until the next move
//     it->Next();
//   }
//   check(it->status());           // I/O or corruption error, if any
//
// Iterators are forward-only. Reverse scans (Prev) were left out to keep the
// block format and skiplist simpler; see DESIGN_DECISIONS.md.
#pragma once

#include "epica/slice.h"
#include "epica/status.h"

namespace epica {

class Iterator {
 public:
  Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator() = default;

  // True if the cursor points at an entry. key()/value() may only be called
  // when Valid().
  virtual bool Valid() const = 0;

  virtual void SeekToFirst() = 0;

  // Position at the first entry whose key is >= target.
  virtual void Seek(Slice target) = 0;

  // Advance. REQUIRES: Valid().
  virtual void Next() = 0;

  // The returned Slices point into iterator-owned storage and are invalidated
  // by the next Seek/Next/destruction.
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;

  // Ok unless an error occurred while iterating (corrupt block, read error).
  // An iterator with a bad status is also !Valid().
  virtual Status status() const = 0;
};

// An iterator over nothing. Handy for empty tables and error cases.
Iterator* NewEmptyIterator();
Iterator* NewErrorIterator(const Status& s);

}  // namespace epica
