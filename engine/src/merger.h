// merger.h -- k-way merge of sorted iterators into one sorted stream.
//
// The LSM tree stores the same key space in many places at once: the active
// memtable, the immutable memtable, several L0 files, and one run per deeper
// level. A range scan (and a compaction) needs all of them as a single
// sorted sequence. MergingIterator does exactly that: it holds N child
// iterators and always exposes the child with the smallest current key.
//
//   child A:  a  d  g
//   child B:  b  d  h        -> merged: a b c d d g h
//   child C:  c
//
// Duplicate user keys with different sequence numbers are both emitted, in
// InternalKeyComparator order (newest first). It is DBIter's job to collapse
// them for the user, and compaction's job to decide which to keep.
//
// Implementation: linear scan over children to find the minimum on each
// step. With N <= ~10 children this beats a heap in practice and is simpler
// to reason about. See DESIGN_DECISIONS.md.
#pragma once

#include <memory>
#include <vector>

#include "epica/iterator.h"
#include "internal_key.h"

namespace epica {

// Takes ownership of every child.
Iterator* NewMergingIterator(const InternalKeyComparator* cmp, std::vector<Iterator*> children);

}  // namespace epica
