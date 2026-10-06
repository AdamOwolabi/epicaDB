// memtable.cc -- see memtable.h for the entry layout.

#include "memtable.h"

#include <cstring>

#include "coding.h"

namespace epica {

namespace {

// Reads a varint32 length prefix and returns the bytes that follow it.
Slice GetLengthPrefixed(const char* p) {
  Slice in(p, 5);  // a varint32 is at most 5 bytes
  uint32_t len = 0;
  GetVarint32(&in, &len);
  return Slice(in.data(), len);
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return comparator.Compare(GetLengthPrefixed(a), GetLengthPrefixed(b));
}

MemTable::MemTable(const InternalKeyComparator& cmp)
    : comparator_(cmp), table_(comparator_, &arena_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, Slice key, Slice value) {
  // Layout: | varint32 ikey_len | user_key | fixed64 tag | varint32 vlen | value |
  const size_t key_size = key.size();
  const size_t val_size = value.size();
  const size_t internal_key_size = key_size + 8;
  const size_t encoded_len = static_cast<size_t>(VarintLength(internal_key_size)) +
                             internal_key_size +
                             static_cast<size_t>(VarintLength(val_size)) + val_size;

  char* buf = arena_.Allocate(encoded_len);
  char* p = buf;

  std::string tmp;
  PutVarint32(&tmp, static_cast<uint32_t>(internal_key_size));
  std::memcpy(p, tmp.data(), tmp.size());
  p += tmp.size();

  std::memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += 8;

  tmp.clear();
  PutVarint32(&tmp, static_cast<uint32_t>(val_size));
  std::memcpy(p, tmp.data(), tmp.size());
  p += tmp.size();
  std::memcpy(p, value.data(), val_size);

  table_.Insert(buf);
  ++num_entries_;
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  Table::Iterator iter(&table_);
  // Seek lands on the first entry >= (user_key, seq). Because sequence sorts
  // descending, that is the newest version of user_key at or below seq -- or
  // some other user key entirely if none exists.
  iter.Seek(key.memtable_key().data());
  if (!iter.Valid()) return false;

  const char* entry = iter.key();
  Slice ikey = GetLengthPrefixed(entry);
  if (ExtractUserKey(ikey) != key.user_key()) return false;  // different key

  const uint64_t tag = DecodeFixed64(ikey.data() + ikey.size() - 8);
  switch (static_cast<ValueType>(tag & 0xff)) {
    case kTypeValue: {
      Slice v = GetLengthPrefixed(ikey.data() + ikey.size());
      value->assign(v.data(), v.size());
      *s = Status::Ok();
      return true;
    }
    case kTypeDeletion:
      *s = Status::NotFound();
      return true;  // definitive: the newest version is a tombstone
  }
  return false;
}

// ---------------------------------------------------------------------------

class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void Seek(Slice target) override {
    // The skiplist wants a memtable-encoded key (length prefix first).
    tmp_.clear();
    PutVarint32(&tmp_, static_cast<uint32_t>(target.size()));
    tmp_.append(target.data(), target.size());
    iter_.Seek(tmp_.data());
  }
  void Next() override { iter_.Next(); }
  Slice key() const override { return GetLengthPrefixed(iter_.key()); }
  Slice value() const override {
    Slice k = GetLengthPrefixed(iter_.key());
    return GetLengthPrefixed(k.data() + k.size());
  }
  Status status() const override { return Status::Ok(); }

 private:
  MemTable::Table::Iterator iter_;
  std::string tmp_;
};

Iterator* MemTable::NewIterator() { return new MemTableIterator(&table_); }

}  // namespace epica
