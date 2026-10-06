# Support reverse iteration (`Iterator::Prev`)

Iterators are forward-only today. Supporting `Prev()` enables `SCAN ... ORDER BY KEY DESC` and "last N keys with prefix" queries.

**Where:** `engine/include/epica/iterator.h`, `engine/src/skiplist.h` (needs backward links or a prev-search), `engine/src/block.cc` (use restart points to step back), `engine/src/merger.cc`, `engine/src/db_iter.cc`.

**Done when:** `Prev()` works across memtable + SSTables + merge iterator with tombstones and snapshots respected, with tests in `engine/tests/db_test.cc`.

See `DESIGN_DECISIONS.md` "Future work".
