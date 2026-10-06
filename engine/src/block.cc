// block.cc -- see block.h for the layout.

#include "block.h"

#include "coding.h"

namespace epica {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset) && GetVarint64(input, &size)) return Status::Ok();
  return Status::Corruption("bad block handle");
}

// ---------------------------------------------------------------------------
// BlockBuilder

void BlockBuilder::Reset() {
  buffer_.clear();
  last_key_.clear();
  counter_ = 0;
  finished_ = false;
}

void BlockBuilder::Add(Slice key, Slice value) {
  PutVarint32(&buffer_, static_cast<uint32_t>(key.size()));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data(), key.size());
  buffer_.append(value.data(), value.size());
  last_key_.assign(key.data(), key.size());
  ++counter_;
}

Slice BlockBuilder::Finish() {
  PutFixed32(&buffer_, counter_);
  finished_ = true;
  return Slice(buffer_);
}

// ---------------------------------------------------------------------------
// Block + iterator

Block::Block(std::string contents) : data_(std::move(contents)) {
  if (data_.size() < 4) {
    num_entries_ = 0;
    entries_end_ = 0;
    return;
  }
  entries_end_ = data_.size() - 4;
  num_entries_ = DecodeFixed32(data_.data() + entries_end_);
}

class BlockIter : public Iterator {
 public:
  BlockIter(const Block* block, const InternalKeyComparator* cmp)
      : block_(block), cmp_(cmp), current_(block->entries_end_) {}

  bool Valid() const override { return status_.ok() && current_ < block_->entries_end_; }

  void SeekToFirst() override {
    current_ = 0;
    ParseCurrent();
  }

  // Linear scan: blocks are small (a few KiB), so this is a handful of
  // comparisons. The table-level index already narrowed us to this block.
  void Seek(Slice target) override {
    SeekToFirst();
    while (Valid() && cmp_->Compare(key_, target) < 0) Next();
  }

  void Next() override {
    current_ = next_;
    ParseCurrent();
  }

  Slice key() const override { return key_; }
  Slice value() const override { return value_; }
  Status status() const override { return status_; }

 private:
  void ParseCurrent() {
    if (current_ >= block_->entries_end_) return;  // at end
    Slice in(block_->data_.data() + current_, block_->entries_end_ - current_);
    uint32_t klen, vlen;
    if (!GetVarint32(&in, &klen) || !GetVarint32(&in, &vlen) || in.size() < klen + vlen) {
      status_ = Status::Corruption("bad entry in block");
      current_ = block_->entries_end_;
      return;
    }
    key_ = Slice(in.data(), klen);
    value_ = Slice(in.data() + klen, vlen);
    next_ = static_cast<size_t>((in.data() + klen + vlen) - block_->data_.data());
  }

  const Block* block_;
  const InternalKeyComparator* cmp_;
  size_t current_;
  size_t next_ = 0;
  Slice key_;
  Slice value_;
  Status status_;
};

Iterator* Block::NewIterator(const InternalKeyComparator* cmp) const {
  return new BlockIter(this, cmp);
}

}  // namespace epica
