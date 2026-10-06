// write_batch.h -- a group of Puts/Deletes applied atomically.
//
// Every write to the DB, even a single Put, goes through a WriteBatch. The
// batch is the unit of atomicity: it becomes ONE write-ahead-log record, so a
// crash either persists all of its operations or none of them.
//
// Wire format (this exact byte string is what lands in the WAL):
//
//   | fixed64 sequence | fixed32 count | op | op | ... |
//   op (Put):    | 0x01 | varint32 klen | key | varint32 vlen | value |
//   op (Delete): | 0x00 | varint32 klen | key |
//
// `sequence` is the sequence number assigned to the FIRST op; op i gets
// sequence + i. The DB fills it in right before logging (SetSequence), so a
// user never sees or sets it.
//
// Example:
//   WriteBatch b;
//   b.Put("alice", "30");
//   b.Delete("bob");
//   db->Write(WriteOptions{}, &b);   // both or neither
//
// The same class is used on recovery: the WAL hands back the byte string,
// WriteBatch::Iterate() walks the ops and re-applies them to the memtable.
#pragma once

#include <cstdint>
#include <string>

#include "epica/slice.h"
#include "epica/status.h"

namespace epica {

class WriteBatch {
 public:
  WriteBatch();

  void Put(Slice key, Slice value);
  void Delete(Slice key);
  void Clear();

  // Number of operations in the batch.
  uint32_t Count() const;

  // Bytes of the encoded batch (what will hit the WAL).
  size_t ByteSize() const { return rep_.size(); }

  // Visitor over the encoded operations. Used by the DB to apply a batch to
  // the memtable and by tests.
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(Slice key, Slice value) = 0;
    virtual void Delete(Slice key) = 0;
  };
  // Returns Corruption if the encoding is malformed (can only happen when
  // replaying a damaged WAL record that nevertheless passed its CRC -- which
  // means a bug, not disk damage).
  Status Iterate(Handler* handler) const;

  // --- Below here is engine-internal plumbing, public for the DB layer. ---
  uint64_t Sequence() const;
  void SetSequence(uint64_t seq);
  const std::string& Contents() const { return rep_; }
  void SetContents(Slice contents) { rep_.assign(contents.data(), contents.size()); }
  // Appends every op of `src` onto this batch (used for group commit).
  void Append(const WriteBatch& src);

  static constexpr size_t kHeader = 8 + 4;

 private:
  void SetCount(uint32_t n);
  std::string rep_;  // encoded form, see file comment
};

}  // namespace epica
