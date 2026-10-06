# epicaDB — Design Decisions

Every major choice in the project, what the alternatives were, and why this
one won. Each entry points at the code that embodies it. Read this after
`BEGINNER_OVERVIEW.md` if the vocabulary is new.

---

## 1. Storage model: LSM tree, not B-tree

**Decision.** Writes go to an append-only write-ahead log (WAL) and an
in-memory sorted table (memtable). The memtable is periodically flushed to
immutable sorted files (SSTables), which are merged in the background
(compaction). This is a Log-Structured Merge tree.

**Alternative.** A B+tree that updates pages in place (SQLite, InnoDB).

**Why LSM.**
- Every write is sequential I/O: an append to the WAL plus a memory insert.
  B-trees do random page writes and need careful page-level crash recovery
  (double-write buffers, page checksums, torn-page handling).
- Immutable files make concurrency simpler: readers never see a file
  half-modified, so they need no locks on file contents.
- Compaction gives a natural place to reclaim deleted data and re-sort.

**Cost.** Reads may consult several places (memtable, immutable memtable,
several L0 files, one file per deeper level) — "read amplification". Bloom
filters (§7) and the level structure (§8) keep this small. Compaction
rewrites data several times over its life — "write amplification".

Code: `engine/src/db_impl.h` (architecture comment), `engine/src/db_impl.cc`.

---

## 2. Durability: WAL first, fsync before acknowledging

**Decision.** A write is (1) appended to the WAL, (2) fsync'd if
`WriteOptions::sync` (default true), (3) applied to the memtable, (4) then
the sequence number is published. Only after step 4 does `Write()` return.

**Alternative.** Apply to the memtable first, log later (or never).

**Why.** If the process dies between "applied in memory" and "written to
disk", an acknowledged write would vanish. Logging first means recovery can
always rebuild memory from disk. This is the classic ARIES-style "write-ahead"
rule and the definition of the D in ACID.

**macOS detail.** `fsync(2)` on macOS does not force the drive's write cache;
`fcntl(F_FULLFSYNC)` does. It costs ~4 ms per call on a laptop SSD, which is
why `db_driver` sustains only a few hundred synced writes per second per
thread. That is the honest price of durability; group commit (§3) amortises
it.

Code: `db_impl.cc` `DBImpl::Write` (lines ~296–355, comments "1. log it" …
"4. publish"), `env.cc` `FsyncFile`, `wal.cc` `WalWriter::Sync`.

---

## 3. Group commit for concurrent writers

**Decision.** Writers queue up; the one at the head ("leader") merges the
batches of everyone waiting behind it into one WAL record, does one
`write(2)` and one fsync, applies everything to the memtable, then wakes the
followers with the shared result.

**Alternative.** Each writer logs and fsyncs its own record under a lock
(one fsync per write), or a dedicated log-writer thread with a queue.

**Why.** Under contention the fsync dominates. N concurrent writers pay one
fsync instead of N, so throughput scales with concurrency instead of being
capped at ~250/s. No extra thread is needed: the leader is whichever writer
happened to arrive first, which keeps the design lock-step simple. Followers
never touch the WAL or memtable, so there is exactly one writer at any time
— which is what the skiplist requires (§5).

**Refinements.** A follower that requests `sync=true` will not ride on a
leader with `sync=false` (it would not get its fsync). Group size is capped
(1 MiB, or 128 KiB extra if the leader is small) so a tiny write does not wait
behind a huge one.

Code: `db_impl.cc` `DBImpl::Write` + `BuildBatchGroup` (line ~357).
Tests: `db_test.cc` `ConcurrentWritersAndReaders`.

---

## 4. WriteBatch as the unit of atomicity and the WAL record

**Decision.** Even a single `Put` becomes a one-op `WriteBatch`. A batch is
encoded once (`| seq | count | ops… |`) and that byte string IS the WAL
record payload (record type `kBatch`).

**Alternative.** One WAL record per operation, with a separate "commit"
marker for multi-op transactions.

**Why.** One record = one atomic unit falls out of the WAL's existing
guarantee (a record is either fully present or truncated away). No commit
markers, no partial-batch cleanup on recovery. The same bytes are also what
`Append()` concatenates during group commit, so grouping is a memcpy.

Code: `include/epica/write_batch.h` (format), `write_batch.cc`,
`db_impl.cc` `ApplyBatchToMemTable`.

---

## 5. Memtable: arena-backed skiplist with lock-free readers

**Decision.** A skiplist whose nodes live in a bump allocator (Arena), with
`std::atomic` next-pointers. One writer at a time (guaranteed by §3); any
number of concurrent readers with no locks.

**Alternatives.** `std::map` under a mutex; a concurrent B-tree; a hash map
(cannot do range scans).

**Why.**
- Range scans need sorted order, ruling out hash maps.
- Readers must not block on writers or each other: a Get should never wait
  for an unrelated Put's fsync. The skiplist supports this with a simple
  invariant: a node is fully initialised before its predecessor's pointer is
  published (release store), and nodes are never removed or moved.
- The arena makes allocation a pointer bump and frees everything at once
  when the memtable is dropped — exactly its lifetime pattern.
- Expected O(log n) with far less code than a balanced tree.

**Cost.** The memtable is append-only: overwriting a key adds a second entry
rather than replacing one. Compaction removes the duplicates later.

Code: `skiplist.h` (concurrency comment at top; `Insert` line ~171),
`arena.h`, `memtable.cc`. Tests: `skiplist_test.cc`
`ConcurrentReadersWithOneWriter`.

---

## 6. Internal keys and MVCC via sequence numbers

**Decision.** Every stored key is `user_key | seq<<8 | type`. Ordering is
user key ascending, then sequence **descending**. A read at sequence S seeks
to `(key, S)` and lands on the newest version ≤ S. A snapshot is just a
remembered sequence number. Deletes are entries of type `kTypeDeletion`
("tombstones").

**Alternative.** Store one value per key and overwrite; implement snapshots
by copying.

**Why.** Multi-version storage gives point-in-time snapshots and consistent
iterators for free, with no copying and no read locks. It also lets
immutable SSTables coexist: an older file can hold `k@5` while a newer one
holds `k@9`, and the merge order decides. Tombstones are required because a
delete must shadow older values that live in files we cannot modify.

**Cost.** Old versions and tombstones occupy space until compaction proves
nobody can observe them (§9).

Code: `internal_key.h` (worked example at top; `InternalKeyComparator` line
~82; `LookupKey` line ~102), `db_iter.cc` `FindNextUserEntry` (collapses
versions for users), `db_impl.cc` `GetSnapshot` / `OldestSnapshot`.

---

## 7. SSTable format: 4 KiB blocks, linear scan in-block, one bloom filter per file

**Decision.** Data blocks of ~4 KiB each end with a CRC-32C. An index block
maps "last key of block" → block handle. Seek = binary search over the index
(via `Seek` on the index block iterator), then linear scan within one block.
One bloom filter over all user keys in the file, checked before any data I/O.

**Alternatives.**
- LevelDB-style restart points + prefix compression inside blocks.
- Per-2-KiB filter partitions.
- Compression (Snappy/zstd).

**Why this shape.** Blocks are the unit of I/O and caching ("multi-block
storage"): a Get reads one block, never the whole file. Within a 4 KiB block
there are only tens of entries, so a linear scan is a few dozen memcmp calls
and not the bottleneck; restart points would add ~100 lines for a marginal
gain. One filter per file is simplest and a file's key set is bounded by the
memtable size, so the filter is at most a few hundred KiB. Compression is
deferred: it is orthogonal and easy to add as a byte in the block trailer.

**Bloom parameters.** 10 bits/key, k = 7 probes ⇒ ~1% false positives.
Double hashing (`h + i*rot(h)`) gives k probes from one hash computation.

Code: `table.h` (layout diagram), `table.cc` `TableBuilder::Add` /
`Finish` / `Table::InternalGet` (line ~197: filter first, then index, then
block), `block.h`, `bloom.cc`. Tests: `table_test.cc`, `bloom_test.cc`.

---

## 8. Leveled compaction (L0 overlapping, L1+ disjoint, 10× fan-out)

**Decision.** L0 receives flushed memtables (files may overlap). When L0 has
≥ 4 files, all of them are merged with the overlapping L1 files into new L1
files. Level n (n ≥ 1) has a byte budget of `10 MiB × 10^(n-1)`; when over
budget, one file is merged into level n+1. Files within L1+ never overlap, so
a Get touches at most one file per level.

**Alternative.** Size-tiered compaction (merge similarly sized runs; lower
write amplification, higher read/space amplification). The original plan
said "size-tiered first"; leveled was chosen instead because its invariants
(disjoint levels) make the read path and the tombstone rule (§9) much easier
to reason about and test.

**Details.**
- Score = L0 file count / trigger, or level bytes / budget; the highest
  score ≥ 1 is compacted. L0 gets priority when tied because L0 files hurt
  reads most.
- Round-robin within a level via `compact_pointer_` so compaction sweeps the
  key space instead of hammering one range.
- Trivial move: if nothing in level n+1 overlaps the chosen file, it is
  re-labelled instead of rewritten.
- Write stall: if L0 reaches 12 files, writers wait for compaction. Without
  it a fast writer outruns compaction forever and reads degrade unboundedly.

Code: `version.cc` `PickLevelToCompact` (~348), `SetupOtherInputs` (~405),
`Compaction::IsTrivialMove`; `db_impl.cc` `DoCompactionWork` (~564),
`MakeRoomForWrite` (~387, stall). Tests: `version_test.cc`, `db_test.cc`
`ManyWritesTriggerFlushAndCompactionAndStayCorrect`,
`BackgroundCompactionRunsOnItsOwn`.

---

## 9. When compaction may drop an entry

**Decision.** While merging, for each user key in newest-to-oldest order:
1. Drop an entry if a newer entry of the same key has sequence ≤ the oldest
   live snapshot (every reader can already see the newer one).
2. Drop a tombstone if its sequence ≤ the oldest live snapshot AND no level
   deeper than the output level contains that key (`IsBaseLevelForKey`).
   Otherwise the tombstone must stay to keep hiding the older value.

**Why.** Rule 1 is what makes snapshots safe: with a live snapshot at
sequence 5, `k@3` must survive even though `k@9` exists. Rule 2 prevents the
classic bug where deleting a tombstone "resurrects" an old value that still
sits in a deeper level.

Code: `db_impl.cc` `DoCompactionWork` (`drop = true` sites, ~620–624),
`version.cc` `Compaction::IsBaseLevelForKey` (~181). Tests: `db_test.cc`
`SnapshotIsolatesReads`, `TombstonesAreDroppedAtBottomAfterCompaction`,
`version_test.cc` `IsBaseLevelForKey`.

---

## 10. MANIFEST: atomic full rewrite instead of an edit log

**Decision.** The set of live files per level plus three counters is
serialised in full into `MANIFEST.tmp`, fsync'd, `rename`d over `MANIFEST`,
and the directory is fsync'd. CRC-32C over the body.

**Alternative.** LevelDB/RocksDB append `VersionEdit` records to a MANIFEST
log and periodically compact it, with a `CURRENT` pointer file.

**Why.** The MANIFEST changes only on flush/compaction — a few times per
second at most — and is a few KiB. Rewriting it is cheap, and "write tmp,
fsync, rename, fsync dir" is the standard recipe for an atomic file replace:
a crash at any instant leaves either the old or the new complete file. This
removes the edit-log replay code, the CURRENT indirection, and the
"MANIFEST grows forever" problem in one stroke.

Code: `env.cc` `WriteFileAtomically` (~75), `version.cc` `WriteManifest`
(~274) / `ReadManifest`, `version.h` (format comment). Tests:
`version_test.cc` `FreshThenPersistThenRecover`, `CorruptManifestIsDetected`.

---

## 11. Recovery: MANIFEST first, then WALs ≥ log_number, then flush and start clean

**Decision.** `Open` loads the MANIFEST, bumps the file-number counter past
every file it sees on disk, replays every WAL whose number ≥ the MANIFEST's
`log_number` into a memtable (flushing mid-way if it grows too big), writes
the result to L0, opens a fresh WAL, records the new `log_number`, and
deletes older WALs and orphan SSTables.

**Alternatives.** Keep replayed data in the memtable and reopen the last WAL
for appending (LevelDB's `reuse_logs`); replay every WAL regardless of the
MANIFEST.

**Why.**
- `log_number` is the durable statement "everything in logs below this is in
  an SSTable". Replaying older logs would re-insert data with *older*
  sequence numbers into the memtable, where it would shadow newer values on
  disk — a correctness bug, not just wasted work.
- Flushing the replayed memtable immediately means the steady state after
  `Open` is always "empty memtable + one empty WAL", so no code path has to
  handle a memtable whose contents span several logs.
- The torn tail of the last WAL (a crash mid-write) is truncated at the first
  bad record; a bad record in any *earlier* log is a hard error, because a
  fully written log should never be corrupt and silently dropping data is
  worse than refusing to open.

Code: `db_impl.cc` `Recover` (~78), `ReplayLogs` (~147); `wal.cc`
`RecoverWal` (~320, `min_seq` filter; `truncate` at ~368).
Tests: `db_test.cc` `SurvivesReopenViaWalOnly`,
`RecoveryReplaysOnlyLogsAfterLastFlush`, `SyncedWritesSurviveSimulatedCrash`;
`wal_test.cc` torn-tail tests; `tools/db_driver.cc` real `kill -9`.

---

## 12. File lifetime by reference counting, not a garbage-collection pass

**Decision.** `FileMetaData` is held by `shared_ptr` from every `Version`
that lists it. When a compaction removes a file from the current Version it
is marked obsolete; its destructor unlinks the file when the *last*
reference — possibly an iterator still reading the old Version — goes away.

**Alternative.** LevelDB's `DeleteObsoleteFiles`: list the directory,
compute live files across all Versions, delete the rest, run after every
compaction.

**Why.** Correctness by construction: it is impossible to delete a file that
some reader still holds, because holding it *is* the reference. No
"pending outputs" set, no directory listing on the hot path. A directory
sweep still runs once at `Open` to remove orphans from a crash mid-compaction
(no readers can exist then).

Code: `version.cc` `FileMetaData::~FileMetaData` (~32), `LogAndApply`
`MarkObsolete` (~269, with the trivial-move exception), `db_impl.cc`
`DeleteOrphanFiles`. Test: `version_test.cc`
`ObsoleteFileIsDeletedWhenLastRefDrops`.

---

## 13. One mutex, one background thread

**Decision.** A single `std::mutex mu_` protects all mutable DB state and is
never held during disk I/O. One background thread performs flushes and
compactions serially, flushes first.

**Alternatives.** Fine-grained locks; a thread pool with parallel
compactions (RocksDB); lock-free versions.

**Why.** Readers copy three `shared_ptr`s under the lock and then run
lock-free (§5, §6), and the write leader drops the lock before the WAL
write, so contention on `mu_` is short bookkeeping only. One background
thread means compactions never race each other, which removes an entire
class of bugs (two compactions picking overlapping inputs). `CompactAll`
borrows the same "one job at a time" slot via `bg_busy_`.

**Cost.** Compaction throughput is bounded by one core. That is the right
trade for this project's scale and can be relaxed later without changing
the file formats.

Code: `db_impl.h` (locking comment), `db_impl.cc` `BackgroundThread` (~497),
`CompactAll` (~696).

---

## 14. Block cache: single-mutex LRU of decoded blocks

**Decision.** `std::list` + `unordered_map` LRU keyed by (file number,
block offset), values are `shared_ptr<Block>`, one mutex, byte-budgeted.

**Alternative.** Sharded LRU (RocksDB), OS page cache only, caching
compressed blocks.

**Why.** Correct and ~80 lines. `shared_ptr` values mean eviction never
invalidates a block an iterator is reading. Sharding is a drop-in change if
the mutex ever shows up in a profile. The OS page cache still helps
underneath, but a decoded-block cache saves the CRC check and parse on hits.

Code: `cache.h/.cc`. Test: `cache_test.cc`.

---

## 15. Network protocol: length-prefixed binary frames, little-endian

**Decision.** `| u32 length | u8 opcode | body |` with length-prefixed byte
strings inside. Same framing for responses. Little-endian like every other
format in the engine.

**Alternatives.** Text protocol (Redis RESP); Protocol Buffers/gRPC; JSON.

**Why.** Keys and values are arbitrary bytes, so a text protocol would need
escaping. A length prefix tells the reader exactly how much to wait for, so
framing is a `readFully`. It is trivial to implement in any language (the
Java side is ~150 lines with no dependencies) and easy to inspect in a hex
dump. Protobuf/gRPC would add a code generator and a large dependency for a
seven-opcode API.

Code: `net/protocol.h` (spec), `server/.../client/Protocol.java`.
Tests: `net_test.cc` `GetFrameBytesMatchSpec`, `ProtocolTest.java`
`getFrameMatchesSpec` — the two sides are pinned to the same bytes.

---

## 16. Server threading: thread per connection

**Decision.** An acceptor thread spawns one thread per client connection;
each loops read-frame → dispatch → write-frame.

**Alternative.** Event loop (kqueue/epoll) with non-blocking sockets; a
fixed thread pool.

**Why.** Expected concurrency is a handful of clients (a query server, some
shells). Blocking I/O per connection is the simplest correct design and the
DB is already thread-safe, so no request queue is needed. The engine's group
commit turns many connections' PUTs into few fsyncs automatically.

Code: `net/server.cc` `AcceptLoop` / `HandleConnection` / `Dispatch`.
Test: `net_test.cc` `ManyConcurrentClients`.

---

## 17. Query layer in Java, separated by the socket protocol

**Decision.** Parsing, planning and execution live in a separate Java
process that talks to the C++ engine over TCP. The executor is programmed
against an `EngineClient` interface with two implementations: the real
socket client and an in-memory `TreeMap` fake.

**Alternative.** Put the query language in C++ inside the engine.

**Why.** It is the standard shape of real systems (storage engine vs. query
processor, e.g. MySQL's pluggable engines, TiDB/TiKV) and lets each layer
be tested and restarted independently. The interface seam means the whole
query package is unit-tested in milliseconds against the fake, and one
integration test (gated by `EPICA_ENGINE_PORT`) proves the wire contract.

Code: `server/src/main/java/epica/client/EngineClient.java`,
`InMemoryEngineClient.java`, `SocketEngineClient.java`.

---

## 18. Query planning choices

- **Prefix scans become bounded range scans.** `SCAN PREFIX user:` →
  `[user:, user;)` where the end is the prefix with its last byte
  incremented (carry over 0xFF). The engine does one range scan; no
  client-side prefix test. Code: `Bytes.prefixSuccessor`,
  `Planner.rangeScan`.
- **Limit pushdown only without a filter.** `SCAN LIMIT 5` sends limit=5 to
  the engine (one round trip, five rows). `SCAN WHERE … LIMIT 5` cannot,
  because the engine does not know the predicate; instead the scan streams
  pages of 128 and `Limit` stops pulling once satisfied. Code:
  `Planner.plan` (`pushdown`), `Executor.ScanIterator.fetchPage`.
- **Pull-based (Volcano) operators.** `Limit → Filter → RangeScan` are
  iterators; nothing materialises the full range. `COUNT` over a million
  keys uses constant memory. Code: `Executor.java`.
- **Pagination cursor = lastKey + 0x00.** The smallest key strictly greater
  than the last one seen, so no row is skipped or repeated between pages.
- **EXPLAIN** renders the plan tree, making these decisions visible.

---

## 19. Things deliberately left out (and where they would go)

| Feature | Why not now | Seam |
|---|---|---|
| Reverse iteration (`Prev`) | Needs backward links in skiplist + block restart points; forward-only covers all queries in the language | `Iterator` interface, `skiplist.h`, `block.cc` |
| Block compression | Orthogonal; add a type byte in the block trailer | `TableBuilder::WriteRawBlock`, `ReadBlockFromFile` |
| Parallel compactions | One thread is correct and simpler; would need input-range reservation | `BackgroundThread`, `PickCompaction` |
| Sharded block cache | Single mutex not yet a bottleneck | `cache.cc` |
| Multi-statement transactions with reads | `BATCH` gives atomic multi-write; read-modify-write needs a lock manager or OCC | `Executor`, new opcode |
| Replication | Out of scope; WAL is already the natural replication stream | `WalWriter` |
| Secondary indexes | Would be maintained as extra keys in the same batch | `Planner`, `AtomicBatch` |
