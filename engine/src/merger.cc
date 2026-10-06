// merger.cc -- see merger.h.

#include "merger.h"

namespace epica {
namespace {

class MergingIterator : public Iterator {
 public:
  MergingIterator(const InternalKeyComparator* cmp, std::vector<Iterator*> children)
      : cmp_(cmp) {
    for (Iterator* c : children) children_.emplace_back(c);
  }

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    FindSmallest();
  }

  void Seek(Slice target) override {
    for (auto& c : children_) c->Seek(target);
    FindSmallest();
  }

  void Next() override {
    current_->Next();
    FindSmallest();
  }

  Slice key() const override { return current_->key(); }
  Slice value() const override { return current_->value(); }

  Status status() const override {
    for (const auto& c : children_) {
      Status s = c->status();
      if (!s.ok()) return s;
    }
    return Status::Ok();
  }

 private:
  void FindSmallest() {
    Iterator* smallest = nullptr;
    for (auto& c : children_) {
      if (!c->Valid()) continue;
      if (smallest == nullptr || cmp_->Compare(c->key(), smallest->key()) < 0) smallest = c.get();
    }
    current_ = smallest;
  }

  const InternalKeyComparator* cmp_;
  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
};

}  // namespace

Iterator* NewMergingIterator(const InternalKeyComparator* cmp, std::vector<Iterator*> children) {
  if (children.empty()) return NewEmptyIterator();
  if (children.size() == 1) return children[0];
  return new MergingIterator(cmp, std::move(children));
}

}  // namespace epica
