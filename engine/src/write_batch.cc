// write_batch.cc -- encoding/decoding of atomic write groups.
// Format is documented in include/epica/write_batch.h.

#include "epica/write_batch.h"

#include "coding.h"

namespace epica {

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);  // zeroed sequence + count
}

uint32_t WriteBatch::Count() const { return DecodeFixed32(rep_.data() + 8); }
void WriteBatch::SetCount(uint32_t n) { EncodeFixed32(&rep_[8], n); }

uint64_t WriteBatch::Sequence() const { return DecodeFixed64(rep_.data()); }
void WriteBatch::SetSequence(uint64_t seq) { EncodeFixed64(&rep_[0], seq); }

void WriteBatch::Put(Slice key, Slice value) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(0x01));  // kTypeValue
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(Slice key) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(0x00));  // kTypeDeletion
  PutLengthPrefixedSlice(&rep_, key);
}

// Group commit: the leader writer glues the followers' batches onto its own
// so a single WAL record carries all of them.
void WriteBatch::Append(const WriteBatch& src) {
  SetCount(Count() + src.Count());
  rep_.append(src.rep_.data() + kHeader, src.rep_.size() - kHeader);
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) return Status::Corruption("malformed WriteBatch (too small)");
  input.remove_prefix(kHeader);

  uint32_t found = 0;
  while (!input.empty()) {
    found++;
    const char tag = input[0];
    input.remove_prefix(1);
    Slice key, value;
    switch (tag) {
      case 0x01:
        if (GetLengthPrefixedSlice(&input, &key) && GetLengthPrefixedSlice(&input, &value)) {
          handler->Put(key, value);
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case 0x00:
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->Delete(key);
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != Count()) return Status::Corruption("WriteBatch has wrong count");
  return Status::Ok();
}

}  // namespace epica
