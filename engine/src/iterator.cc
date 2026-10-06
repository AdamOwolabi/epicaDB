// iterator.cc -- the two trivial iterators: empty and error.

#include "epica/iterator.h"

namespace epica {
namespace {

class EmptyIterator : public Iterator {
 public:
  explicit EmptyIterator(Status s) : status_(std::move(s)) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(Slice) override {}
  void Next() override {}
  Slice key() const override { return Slice(); }
  Slice value() const override { return Slice(); }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

Iterator* NewEmptyIterator() { return new EmptyIterator(Status::Ok()); }
Iterator* NewErrorIterator(const Status& s) { return new EmptyIterator(s); }

}  // namespace epica
