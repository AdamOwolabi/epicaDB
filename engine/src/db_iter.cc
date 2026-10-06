// db_iter.cc -- see db_iter.h.

#include "db_iter.h"

#include <string>

namespace epica {
namespace {

class DBIter : public Iterator {
 public:
  DBIter(Iterator* iter, SequenceNumber sequence, std::function<void()> cleanup)
      : iter_(iter), sequence_(sequence), cleanup_(std::move(cleanup)) {}
  ~DBIter() override {
    iter_.reset();  // drop table/memtable iterators before releasing refs
    if (cleanup_) cleanup_();
  }

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(false);
  }

  void Seek(Slice target) override {
    // Probe at (target, sequence_): lands on target's newest visible version
    // if target exists, otherwise on the next user key.
    std::string probe;
    AppendInternalKey(&probe, target, sequence_, kValueTypeForSeek);
    iter_->Seek(probe);
    FindNextUserEntry(false);
  }

  void Next() override {
    // Skip every remaining (older) version of the key we just returned.
    saved_key_.assign(key_.data(), key_.size());
    iter_->Next();
    FindNextUserEntry(true);
  }

  Slice key() const override { return key_; }
  Slice value() const override { return value_; }
  Status status() const override { return status_.ok() ? iter_->status() : status_; }

 private:
  // Scans forward to the next entry the user should see. If `skipping`, every
  // entry whose user key == saved_key_ is ignored first (older versions of a
  // key already emitted, or of a key hidden by a tombstone).
  void FindNextUserEntry(bool skipping) {
    valid_ = false;
    while (iter_->Valid()) {
      ParsedInternalKey p;
      if (!ParseInternalKey(iter_->key(), &p)) {
        status_ = Status::Corruption("corrupt internal key in iterator");
        return;
      }
      if (p.sequence <= sequence_) {  // visible at this snapshot?
        if (skipping && p.user_key == Slice(saved_key_)) {
          // older version of something already decided; fall through to Next
        } else if (p.type == kTypeDeletion) {
          // Newest visible version is a tombstone: hide all older versions.
          saved_key_.assign(p.user_key.data(), p.user_key.size());
          skipping = true;
        } else {
          key_ = p.user_key;
          value_ = iter_->value();
          valid_ = true;
          return;
        }
      }
      iter_->Next();
    }
  }

  std::unique_ptr<Iterator> iter_;
  SequenceNumber sequence_;
  std::function<void()> cleanup_;
  std::string saved_key_;
  Slice key_, value_;
  bool valid_ = false;
  Status status_;
};

}  // namespace

Iterator* NewDBIterator(Iterator* internal_iter, SequenceNumber sequence,
                        std::function<void()> cleanup) {
  return new DBIter(internal_iter, sequence, std::move(cleanup));
}

}  // namespace epica
