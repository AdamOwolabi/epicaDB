// db_iter.h -- turns the internal multi-version stream into what a user
// expects: each key once, newest visible value only, no tombstones.
//
// Input (from MergingIterator over memtables + all SSTables), internal keys
// sorted by user key asc then sequence desc:
//
//   a@9:Put(v3)  a@7:Put(v2)  a@2:Put(v1)  b@8:Del  b@5:Put(x)  c@4:Put(y)
//
// Output for a reader at sequence 10:   a=v3, c=y
//   - a: first visible entry is a@9, a Put -> emit; skip the older a's.
//   - b: first visible entry is b@8, a Delete -> hide every b; emit nothing.
//   - c: c@4 Put -> emit.
//
// Output for a reader at sequence 6 (an older snapshot):  a=v1... no: a=v2
//   - a@9 and a@7 are... a@9 is invisible (9 > 6), a@7 visible -> emit v2.
//   - b@8 invisible, b@5 visible Put -> emit b=x. The delete "hasn't
//     happened yet" from this snapshot's point of view.
//
// DBIter also owns the shared_ptrs that keep the memtables and Version alive
// for as long as the user holds the iterator.
#pragma once

#include <functional>
#include <memory>

#include "epica/iterator.h"
#include "internal_key.h"

namespace epica {

// `internal_iter` yields internal keys; `sequence` is the visibility bound;
// `cleanup` runs in the destructor (used to release memtable/version refs).
Iterator* NewDBIterator(Iterator* internal_iter, SequenceNumber sequence,
                        std::function<void()> cleanup);

}  // namespace epica
