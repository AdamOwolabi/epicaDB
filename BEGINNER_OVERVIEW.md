# epicaDB — The Complete Beginner's Overview

This document explains every part of epicaDB: what it is, why it is shaped
this way, how each piece works at the byte level, and how to talk about it
in an interview. It assumes you can read C++ and Java but have never built a
database. Read it top to bottom once, then use the **Code map** (§12) and
**Interview Q&A** (§15–16) as references.

Line numbers below refer to the files as they are in this repository. If a
number drifts after an edit, search for the function name.

---

## 1. The elevator pitch (memorise this)

> epicaDB is a persistent key-value database. The storage engine is C++20
> and follows the LSM-tree design used by LevelDB and RocksDB: writes go to a
> write-ahead log and an in-memory skiplist, get flushed to immutable sorted
> files with bloom filters, and are merged by a background leveled
> compaction. It supports atomic batches, snapshot reads, range scans,
> concurrent readers that never block, and group commit for concurrent
> writers. On top sits a Java query layer with a lexer, parser, planner and
> pull-based executor that talks to the engine over a length-prefixed binary
> protocol. It survives `kill -9` at any instant, which the test suite proves.

Sizes: ~5,000 lines of C++ (engine + net + tools), ~1,400 lines of Java,
~3,200 lines of tests. 88 C++ tests, 22 Java tests, clean under
AddressSanitizer/UBSan.

---

## 2. Vocabulary

| Term | Meaning in this project |
|---|---|
| **Key-value store** | A database whose only operations are put(k,v), get(k), delete(k), scan(range). No tables, no SQL. |
| **Durability** | A write that has been acknowledged survives a crash or power loss. |
| **Atomicity** | A group of writes either all happen or none do. |
| **WAL** (write-ahead log) | An append-only file every write goes to *before* anything else. Replayed on restart. |
| **fsync** | The system call that forces file data from OS/drive caches onto the physical medium. Slow (~4 ms on a laptop SSD with `F_FULLFSYNC`). |
| **Memtable** | The in-memory sorted structure holding recent writes. Here: a skiplist. |
| **Immutable memtable** | A memtable that has been frozen (no more writes) and is waiting to be flushed to disk. |
| **Flush** | Writing a memtable out as an SSTable. |
| **SSTable** (Sorted String Table) | An immutable file of sorted key/value entries, split into blocks, with an index and a bloom filter. |
| **Block** | ~4 KiB slice of an SSTable; the unit of disk read and of caching. |
| **Bloom filter** | A compact bit array that answers "definitely not present" or "maybe present". |
| **Level** | A tier of SSTables. L0 files may overlap each other; L1+ files within a level do not. Each level is ~10× the previous. |
| **Compaction** | Merging SSTables from one level into the next, dropping overwritten values and expired tombstones. |
| **Tombstone** | A stored "this key is deleted" marker. Needed because files are immutable. |
| **Sequence number** | A 56-bit counter incremented on every operation. Larger = newer. |
| **Internal key** | `user_key + 8 bytes (sequence, type)`; what the engine actually stores and sorts. |
| **Snapshot** | A remembered sequence number. Reading at it shows exactly the state after that operation. |
| **MVCC** | Multi-Version Concurrency Control: keeping several versions so readers and writers never block each other. |
| **MANIFEST** | The file that records which SSTables exist at which level, plus counters. |
| **Version** | An immutable in-memory snapshot of the MANIFEST contents. A new one is installed after each flush/compaction. |
| **Group commit** | Merging several concurrent writers' batches into one log write and one fsync. |
| **Write stall** | Making writers wait when L0 has too many files so compaction can catch up. |
| **Read/write/space amplification** | How many places a read checks / how many times a byte is rewritten / how much extra disk is used, relative to the ideal. LSM trees trade these against each other. |
| **Iterator** | A forward cursor: `Seek`, `Valid`, `key`, `value`, `Next`. Every storage layer exposes the same one. |
| **Merging iterator** | Combines N sorted iterators into one sorted stream. |
| **Planner / executor** | The query layer parts that turn a parsed statement into operators and run them. |

---

## 3. The big picture

```
                     ┌──────────────────────────────────────────────┐
  nc / scripts ───►  │  Java QueryServer (text lines, port 7380)     │
                     │  Java Shell (REPL)                            │
                     │      │  Lexer → Parser → Planner → Executor   │
                     │      ▼                                        │
                     │  SocketEngineClient  (binary protocol)        │
                     └──────────────┬───────────────────────────────┘
                                    │ TCP, length-prefixed frames
                     ┌──────────────▼───────────────────────────────┐
                     │  C++ epica_server  (thread per connection)   │
                     │      │                                        │
                     │      ▼            DB (db_impl.cc)             │
   writers ─────►  Write() ─► group commit ─► WAL append+fsync ─► MemTable
                     │                                        │       │
   readers ─────►  Get()/NewIterator() ─► mem ─► imm ─► Version ─────┤
                     │                                                │ flush (bg thread)
                     │        ┌───────────────────────────────────────▼─────┐
                     │        │ L0: [sst][sst][sst]   (overlapping)          │
                     │        │ L1: [sst][sst][sst][sst]  (disjoint, 10 MiB) │
                     │        │ L2: [...]                 (100 MiB)           │
                     │        │ ...                         compaction ↓      │
                     │        └───────────────────────────────────────────────┘
                     │   MANIFEST: which sst files are in which level        │
                     └──────────────────────────────────────────────────────┘
```

Three layers:

1. **Storage engine** (`engine/src`) — everything about bytes on disk and in
   memory. Exposes `DB` (`engine/include/epica/db.h`).
2. **Network** (`engine/net`) — a thin server that maps seven opcodes onto
   `DB` calls, and a client.
3. **Query layer** (`server/`) — understands the language `PUT`, `GET`,
   `SCAN PREFIX ... WHERE ... LIMIT ...`, `BATCH ... END`, `EXPLAIN`, turns
   it into engine calls.

---

## 4. The life of a PUT

`db->Put(WriteOptions{sync=true}, "user:1", "Ada")` — follow it in
`engine/src/db_impl.cc`:

1. **Wrap in a WriteBatch.** `Put` (line ~257) creates a one-op batch. The
   batch is bytes: `| seq(8) | count(4) | 0x01 | klen | "user:1" | vlen | "Ada" |`
   (`write_batch.cc`).
2. **Queue as a Writer.** `Write` (line ~296) pushes a `Writer` struct onto
   `writers_` and waits until it is at the front. If another thread's leader
   already committed us, we return its status — that's group commit from
   the follower's side.
3. **Leader: make room.** `MakeRoomForWrite` (~387). If the memtable is over
   `memtable_size` (4 MiB), open a new WAL file, freeze the memtable as
   `imm_`, create a fresh one, wake the background thread. If a previous
   `imm_` is still flushing, or L0 has ≥ 12 files, wait (write stall).
4. **Leader: build the group.** `BuildBatchGroup` (~357) appends the batches
   of every waiting writer (up to a size cap) onto one batch.
5. **Assign sequence numbers.** `updates->SetSequence(last_sequence + 1)`;
   each op gets the next number. With 3 followers of 1 op each, the group
   takes sequences 42..45.
6. **Unlock and do I/O.** Line ~320: release the mutex.
   `log_->Append(kBatch, bytes)` (~321) writes `| crc | len | type=3 | batch |`
   into the WAL buffer; `log_->Sync()` (~324) does `write(2)` + `F_FULLFSYNC`.
   **This is the moment the write becomes durable.**
7. **Apply to memtable.** `ApplyBatchToMemTable` (~327) walks the batch and
   calls `MemTable::Add(seq, type, key, value)` for each op
   (`memtable.cc` ~30), which allocates one arena entry and inserts into the
   skiplist.
8. **Relock and publish.** `versions_->SetLastSequence(45)`. From this
   instant readers taking a fresh snapshot see the new data.
9. **Wake followers.** Each follower's `done=true`, `status` set, its
   condition variable notified. The next queued writer becomes leader.

Cost: one `write(2)`, one fsync, N skiplist inserts. With 8 threads writing
concurrently you still pay about one fsync per group, not per write.

---

## 5. The life of a GET

`db->Get(ReadOptions{}, "user:1", &v)` — `DBImpl::Get` (~421):

1. **Take references under the lock**: current `last_sequence` (or the
   snapshot's), `mem_`, `imm_`, current `Version`. Release the lock. From here
   on nothing blocks the reader, and the `shared_ptr`s keep those structures
   alive even if a flush replaces them mid-read.
2. **Build a LookupKey**: `"user:1" + (seq<<8 | 1)` (`internal_key.h` ~102).
3. **Memtable** (`MemTable::Get`, `memtable.cc` ~62): skiplist `Seek` lands
   on the newest version of `user:1` with sequence ≤ ours. If it's a Put,
   return the value. If it's a tombstone, return NotFound. If the user key
   differs, the memtable has no opinion — fall through.
4. **Immutable memtable**: same.
5. **Version** (`Version::Get`, `version.cc` ~91):
   - **L0**: every file whose [smallest, largest] range covers the key,
     newest first.
   - **L1+**: binary search (`std::lower_bound`, ~115) for the single file
     whose range covers the key.
   - For each candidate file, `Table::InternalGet` (`table.cc` ~197):
     a. **Bloom filter** (~201): if it says "no", skip — zero disk I/O.
     b. **Index block**: `Seek` finds the one data block whose last key ≥ ours.
     c. **Block cache** (`cache.cc`): hit → reuse decoded block; miss →
        `pread` ~4 KiB, verify CRC, decode, insert.
     d. Linear scan inside the block; the callback `SaveValue` checks the user
        key matches and records Found/Deleted.
   - First Found or Deleted wins (newer levels are checked first).
6. Not in any file → NotFound.

Typical hot read: memtable miss + L0 bloom miss + one L1 block from cache =
no disk I/O at all.

---

## 6. The life of a crash

Kill the process at any instant. On the next `DB::Open` → `DBImpl::Recover`
(`db_impl.cc` ~78):

1. **Lock** the directory with `flock` on `<dir>/LOCK` (~92). Two processes
   on one directory would corrupt it; fail fast instead.
2. **MANIFEST** (`VersionSet::Recover`): read, verify CRC, rebuild the
   per-level file lists and the counters `next_file_number`,
   `last_sequence`, `log_number`.
3. **Bump the file counter** past every WAL/SST file actually on disk. A WAL
   opened after the last MANIFEST write has a number the MANIFEST never saw.
4. **Replay WALs** (`ReplayLogs` ~147 → `RecoverWal` in `wal.cc` ~320):
   only logs with number ≥ `log_number` — older ones are already in SSTables.
   Each record is a WriteBatch; apply it to the memtable with its *original*
   sequence numbers (they're inside the batch). If the memtable fills up
   mid-replay, flush it to L0 and continue.
   - A corrupt record at the tail of the **last** log = a torn write from the
     crash: truncate the file there (`truncate`, `wal.cc` ~368), fsync, done.
   - A corrupt record in an **earlier** log = real damage: refuse to open.
5. **Fresh WAL** and **flush the replayed memtable to L0**, then record the
   new `log_number` in the MANIFEST. Steady state after Open is always
   "empty memtable + one empty WAL".
6. **Delete** WAL files below `log_number` and SSTables the MANIFEST doesn't
   list (half-written outputs of an interrupted compaction).
7. Start the background thread.

What each on-disk piece guarantees:
- **WAL fsync before ack** ⇒ acknowledged writes exist in some WAL or SSTable.
- **CRC on every record/block/manifest** ⇒ a torn or bit-flipped write is
  detected, never returned as data.
- **SSTable fsync + dir fsync before MANIFEST update** ⇒ a listed file is
  complete.
- **MANIFEST written as tmp + fsync + rename + dir fsync** ⇒ it is either the
  old or the new one, never half.

`tools/db_driver.cc` proves this with real `kill -9`: the writer stores
`__max = i` in the *same batch* as `key-i`, so the verifier knows exactly
which keys must exist.

---

## 7. The life of a flush and a compaction

**Flush** (`CompactMemTable` ~517, on the background thread):
1. Take `imm_`. Unlock. `BuildTable` (`version.cc` ~443) iterates the
   memtable in order and feeds a `TableBuilder`; the result is
   `sst/<n>.sst`, fsync'd, directory fsync'd.
2. Relock. `VersionEdit`: add the file to L0, set `log_number` = the current
   WAL's number. `LogAndApply` writes the MANIFEST atomically and installs a
   new `Version`.
3. `imm_ = nullptr`, delete WALs older than `log_number`, wake anyone waiting.

**Compaction** — worked example with `l0_compaction_trigger = 4`:

```
Before:                                  L0 has 4 files -> score 1.0 -> compact
  L0: #12 [a..m]  #11 [c..z]  #10 [a..k]  #9 [m..t]      (overlapping, newest first)
  L1: #5 [a..f]   #6 [g..p]   #7 [q..z]                   (disjoint)

Pick (version.cc PickCompactionForLevel ~370):
  inputs[0] = all L0 files                     range [a..z]
  inputs[1] = L1 files overlapping [a..z] = #5 #6 #7

Merge (db_impl.cc DoCompactionWork ~564):
  MergingIterator over 7 table iterators yields internal keys in order:
    a@40:Put  a@31:Put  a@7:Put  b@12:Del  b@3:Put  c@41:Put ...
  For each user key, newest first, with oldest live snapshot S:
    a@40 keep (first version)
    a@31 drop if 40 <= S   (everyone can already see a@40)
    a@7  drop likewise
    b@12 (tombstone) drop only if 12 <= S AND no L2+ file has "b"
    b@3  drop if 12 <= S
  Output rolls to a new file every target_file_size (2 MiB).

After:
  L0: (empty)
  L1: #13 [a..h]  #14 [i..p]  #15 [q..z]      new, disjoint
  #5..#12 marked obsolete; unlinked when the last reader releases them.
```

Level n ≥ 1 is compacted when its bytes exceed `10 MiB × 10^(n-1)`; one file
(round-robin by key) merges into n+1. If nothing in n+1 overlaps it, the file
is just re-labelled (trivial move).

---

## 8. Component deep dives (byte level)

### 8.1 WAL — `engine/include/epica/wal.h`, `engine/src/wal.cc`

Record: `| crc32c(4) | length(4) | type(1) | payload |`. CRC covers type +
payload. Type 3 = WriteBatch (the engine); 1/2 = raw put/delete (tests).
`Append` (~162) buffers up to 64 KiB, `Sync` (~190) = flush + fsync.
`ReadRecord` (~241) validates length ≤ 64 MiB (so a corrupt length can't
trigger a giant allocation), type, and CRC; on the first failure it becomes
"sticky failed" and reports the offset for truncation. Files:
`<dir>/wal/000000000007.log`; a new file per memtable generation.

### 8.2 WriteBatch — `engine/include/epica/write_batch.h`

```
| fixed64 sequence | fixed32 count | 01 | varint klen | key | varint vlen | value | 00 | varint klen | key |
                                     └── Put ─────────────────────────────────┘ └── Delete ─────────┘
```
The DB fills in `sequence` right before logging. `Append` concatenates ops
for group commit. `Iterate(handler)` is used both to apply to the memtable
and to replay from the WAL — the same bytes, the same code.

### 8.3 Internal keys — `engine/src/internal_key.h`

`internal = user_key | fixed64(seq << 8 | type)`, type 1 = value, 0 = delete.
Order: user key ascending, then **sequence descending**. Example for key `k`:

```
k@9:Del   k@5:Put(v2)   k@3:Put(v1)        (storage order)
Seek(k@100) -> k@9  : latest state is "deleted"
Seek(k@6)   -> k@5  : snapshot 6 sees v2
Seek(k@4)   -> k@3  : snapshot 4 sees v1
```
`LookupKey` (~102) packages `varint len | internal key` for the memtable and
plain internal key for tables.

### 8.4 Arena + SkipList + MemTable — `arena.h`, `skiplist.h`, `memtable.cc`

- **Arena**: 4 KiB slabs, pointer-bump allocation, free-all-at-once. Large
  objects get their own slab. `MemoryUsage()` drives the flush decision.
- **SkipList**: levels with branching 4 (≈ every 4th node gets an express
  link; max 12 levels). `FindGreaterOrEqual` walks right while the next key is
  smaller, then drops a level. `Insert` (~171) links bottom-up: set the new
  node's next pointers first (nobody can see it), then publish by pointing
  each predecessor at it with a release store. Readers use acquire loads.
  Nodes are never freed or moved ⇒ readers need no locks.
- **MemTable entry** in the arena:
  `| varint ikey_len | user_key | fixed64 tag | varint vlen | value |`.
  The skiplist key is a `const char*` to the entry start; the comparator
  decodes the length prefix. `Get` (~62) seeks and checks the user key and
  type.

### 8.5 SSTable — `engine/src/table.h`, `table.cc`, `block.h`, `bloom.h`

```
[data block][data block]...[filter block][index block][footer 48 B]
data block  = entries (| varint klen | varint vlen | key | value |)* | fixed32 n | fixed32 crc32c
index block = one entry per data block: key = last internal key of the block, value = (offset, size)
filter      = bloom bit array + 1 byte k
footer      = filter handle | index handle | padding | fixed64 magic "EPICADB\0"
```
`TableBuilder::Add` (~20) appends to the current block; at ≥ 4 KiB
`FlushDataBlock` writes it with a CRC and remembers an index entry.
`Finish` (~63) writes filter, index, footer. `Table::Open` (~129) reads the
footer, then index and filter into memory (a few KiB), so a table costs one
fd + small RAM. `TableIterator` (~228) is a two-level iterator: index cursor
picks a block, block cursor walks it.

**Bloom math**: m = 10 bits/key, k = ⌊10 × 0.69⌉ = 7 probes,
false-positive rate ≈ (1 − e^(−kn/m))^k ≈ 0.8%. Probe i uses
`h + i × rotate(h, 17)` — "double hashing" gives k positions from one hash.

### 8.6 Block cache — `engine/src/cache.h`

LRU keyed by (file number, block offset). `std::list` (recency order) +
`unordered_map` (key → list iterator) ⇒ O(1) lookup, insert, evict. Budget in
bytes (8 MiB default). Values are `shared_ptr<Block>` so an iterator reading a
block is unaffected by eviction. `STATS` shows hits/misses.

### 8.7 Versions and MANIFEST — `engine/src/version.h`, `version.cc`

`FileMetaData` = number, size, smallest/largest internal key, lazily opened
`Table`, `obsolete` flag. `Version` = vector of per-level `FileMetaData`
lists (L0 newest-first, L1+ by key). `VersionEdit` = files added/removed +
new `log_number`. `LogAndApply` (~233) builds the new Version, writes the
MANIFEST atomically, installs it, then marks removed files obsolete — they
are unlinked in `~FileMetaData` (~32) when the last `shared_ptr` drops.

MANIFEST bytes: `| crc32c | len | varint next_file | varint last_seq | varint log_number | varint num_levels | per level: varint count, per file: varint number, varint size, lenprefixed smallest, lenprefixed largest |`.

### 8.8 Snapshots — `db_impl.cc` `GetSnapshot` (~472)

A `SnapshotImpl` is just a sequence number in a list. `Get`/`NewIterator`
with a snapshot use that number as the visibility bound. Compaction asks
`OldestSnapshot()` and never drops a version some live snapshot could see.
Release removes it from the list.

### 8.9 Iterators — `iterator.h`, `merger.cc`, `db_iter.cc`

Every layer speaks `Valid/SeekToFirst/Seek/Next/key/value/status`.
`NewInternalIterator` builds one iterator per source (mem, imm, each SST
file) and wraps them in a `MergingIterator` (`FindSmallest` ~44: linear min
over children; fine for ≤ ~10). `DBIter::FindNextUserEntry` (~50) then
collapses versions for the user: first visible entry of a key decides — Put
⇒ emit, Delete ⇒ hide the key — and older versions are skipped. The DBIter
holds `shared_ptr`s to everything it reads, so a scan sees a frozen snapshot
no matter what writers do meanwhile.

### 8.10 Concurrency — `db_impl.h` (locking comment), `db_impl.cc`

- One mutex `mu_`. Held only for bookkeeping; **never during disk I/O**
  (`l.unlock()` at ~320, ~216, ~566).
- Writers: group commit (§4). Exactly one thread inserts into the skiplist
  at a time, satisfying its contract.
- Readers: copy `shared_ptr`s under the lock, then lock-free.
- One background thread (`BackgroundThread` ~497): waits on
  `bg_work_cv_` until `imm_` exists or a level needs compaction; flushes
  first. Signals `bg_cv_` when done, waking stalled writers and
  `Flush()`/`CompactAll()`.
- Backpressure: `MakeRoomForWrite` waits if `imm_` is still flushing or
  L0 ≥ `l0_stop_writes_trigger` (12).
- A failed fsync sets a sticky `bg_error_`: the DB refuses further writes
  rather than let disk and memory diverge.

---

## 9. The network layer — `engine/net/protocol.h`, `server.cc`, `client.cc`

Frame: `| u32 length | u8 opcode | body |`, little-endian, strings as
`| u32 len | bytes |`. `GET "abc"` on the wire:

```
08 00 00 00   length = 8
02            GET
03 00 00 00   key length 3
61 62 63      "abc"
```
Response: `| u32 length | u8 status | body |`; status 0 OK, 1 NOT_FOUND,
2 ERROR(msg), 3 BAD_REQUEST(msg). Opcodes: PING 1, GET 2, PUT 3, DEL 4,
SCAN 5 (start, end, limit), BATCH 6, STATS 7.

Server: `AcceptLoop` spawns a thread per connection; `HandleConnection`
(~145) loops `ReadFrame → Dispatch → WriteFrame`; `Dispatch` (~158) maps
opcodes to `DB` calls. SCAN uses a DB iterator, so it is a consistent
snapshot even under concurrent writes. Frames over 64 MiB or with unknown
opcodes get BAD_REQUEST, never a crash (`net_test.cc`).

Both sides pin the exact bytes: `net_test.cc GetFrameBytesMatchSpec` and
`ProtocolTest.java getFrameMatchesSpec`.

---

## 10. The query layer — `server/src/main/java/epica/query`

Follow `SCAN PREFIX user: WHERE VALUE CONTAINS Alan LIMIT 1`:

1. **Lexer** (`Lexer.java`): `WORD(SCAN) WORD(PREFIX) WORD(user:) WORD(WHERE)
   WORD(VALUE) WORD(CONTAINS) WORD(Alan) WORD(LIMIT) NUMBER(1) EOF`. Quoted
   strings become `STRING` with escapes resolved.
2. **Parser** (`Parser.java` `statement()` ~29, `range()` ~74): recursive
   descent → `Scan(Range(prefix="user:"), Predicate("Alan"), limit=1)` — a
   record in the sealed `Statement` AST.
3. **Planner** (`Planner.java`):
   - prefix → range `[user:, user;)` via `Bytes.prefixSuccessor` (last byte
     +1, with 0xFF carry; all-0xFF ⇒ unbounded);
   - a WHERE is present, so the LIMIT **cannot** be pushed to the engine
     (`pushdown = 0`, line ~34) — the engine doesn't know the predicate;
   - plan: `Limit(1) → Filter("Alan") → RangeScan(start, end, pageSize=128)`.
   `EXPLAIN` prints exactly this tree.
4. **Executor** (`Executor.java`): Volcano/pull model. `LimitIterator.hasNext`
   asks `FilterIterator`, which pulls from `ScanIterator`, which calls
   `engine.scan(cursor, end, 128)` (`fetchPage` ~106). After a full page the
   cursor becomes `lastKey + 0x00` (smallest key strictly greater). Limit
   stops pulling after one row; the engine was asked for at most one page.
   `COUNT` over a million keys runs in constant memory.
5. **Client** (`SocketEngineClient.java`): encodes SCAN with `Protocol.Writer`
   (little-endian!), `readFully` the reply, decodes rows.
6. **Rendering**: the Shell prints `user:2 = "Alan Turing"`; the QueryServer
   prints `ROW user:2 "Alan Turing"` then `END 1`.

Without WHERE, `SCAN LIMIT 5` becomes `Limit(5) → RangeScan(engineLimit=5)`:
one round trip, five rows (`ExecutorTest.scanPaginatesAndStopsEarlyOnLimit`
counts the round trips).

`BATCH PUT a 1; DEL b END` → `AtomicBatch` → one BATCH frame → one
`WriteBatch` → one WAL record. Atomic end to end.

The executor is written against `EngineClient`; `InMemoryEngineClient`
(TreeMap with unsigned byte order) makes the unit tests instant and powers
`java -jar ... memory` for learning the language without a server.

---

## 11. Numbers worth knowing

| Thing | Value | Where / why |
|---|---|---|
| memtable flush threshold | 4 MiB | `Options::memtable_size`; bigger = fewer files, longer replay |
| block size | 4 KiB | matches an FS page; unit of read + cache |
| bloom | 10 bits/key, 7 probes, ~1% FP | `bloom.cc`; `Bloom.FalsePositiveRateIsLow` allows < 2% |
| block cache | 8 MiB | `Options::block_cache_size` |
| L0 compaction trigger / stall | 4 / 12 files | `l0_compaction_trigger`, `l0_stop_writes_trigger` |
| level budgets | L1 10 MiB, ×10 per level, 7 levels | `MaxBytesForLevel` |
| output file size | 2 MiB | `target_file_size` |
| WAL buffer | 64 KiB | `wal_buffer_size` |
| max WAL payload / frame | 64 MiB | guards against corrupt lengths |
| sequence number | 56 bits (~7×10¹⁶ ops) | 8 bits left for type |
| fsync (F_FULLFSYNC) cost | ~4 ms on a laptop SSD | ⇒ ~250 synced writes/s single-threaded; group commit amortises |
| skiplist branching / max height | 4 / 12 | ≈ 4¹² ≈ 16M entries before degradation |
| group commit cap | 1 MiB (128 KiB extra if leader ≤ 128 KiB) | latency vs throughput |
| scan page size (Java) | 128 rows | `Planner.PAGE_SIZE` |

---

## 12. Code map

### C++ engine — `engine/include/epica/` (public)

| File | Purpose |
|---|---|
| `db.h` | The `DB` interface: Open, Put, Delete, Write, Get, NewIterator, GetSnapshot, Flush, CompactAll, GetStats. Usage example at top. |
| `options.h` | `Options` (sizes, thresholds), `WriteOptions{sync}`, `ReadOptions{snapshot}`; each field's trade-off documented. |
| `iterator.h` | The universal cursor interface. |
| `write_batch.h` | Atomic write group + its byte format. |
| `wal.h` | WAL record format, writer/reader/recovery API. |
| `status.h` | Error type (`Ok/NotFound/Corruption/IOError/InvalidArgument`). |
| `slice.h` | Non-owning byte view; defines the one key order (memcmp). |

### C++ engine — `engine/src/`

| File | Purpose | Key functions (line) |
|---|---|---|
| `db_impl.h/.cc` | The DB: recovery, write path, read path, background work | `Recover` 78, `ReplayLogs` 147, `Write` 296, `BuildBatchGroup` 357, `MakeRoomForWrite` 387, `Get` 421, `NewIterator` 464, `GetSnapshot` 472, `BackgroundThread` 497, `CompactMemTable` 517, `DoCompactionWork` 564, `Flush` 678, `CompactAll` 696 |
| `version.h/.cc` | Files per level, MANIFEST, compaction picking | `~FileMetaData` 32, `Version::Get` 91, `IsBaseLevelForKey` 181, `LogAndApply` 233, `WriteManifest` 274, `PickLevelToCompact` 348, `SetupOtherInputs` 405, `BuildTable` 443 |
| `table.h/.cc` | SSTable builder + reader, two-level iterator | `TableBuilder::Add` 20, `Finish` 63, `ReadBlockFromFile` 111, `Table::Open` 129, `ReadDataBlock` 176, `InternalGet` 197, `TableIterator` 228 |
| `block.h/.cc` | Block format, builder, in-block iterator | `BlockBuilder::Add`, `BlockIter::Seek` |
| `bloom.h/.cc` | Bloom filter + hash | `CreateFilter` 50, `KeyMayMatch` 72 |
| `cache.h/.cc` | LRU block cache | `Lookup` 7, `Insert` 20, `EvictIfNeeded` 47 |
| `memtable.h/.cc` | Arena skiplist memtable | `Add` 30, `Get` 62, `MemTableIterator` 91 |
| `skiplist.h` | Lock-free-reader skiplist | `FindGreaterOrEqual`, `RandomHeight` 136, `Insert` 171 |
| `arena.h` | Bump allocator | `Allocate`, `AllocateAligned` |
| `internal_key.h` | Sequence/type tagging, comparator, LookupKey | `AppendInternalKey` 76, `InternalKeyComparator` 82, `LookupKey` 102 |
| `db_iter.h/.cc` | Collapses versions for users | `Seek` 26, `FindNextUserEntry` 50 |
| `merger.h/.cc` | K-way merge | `FindSmallest` 44 |
| `write_batch.cc` | Batch encode/decode | `Append` 38, `Iterate` 43 |
| `wal.cc` | WAL writer/reader/recovery | `Append` 162, `Sync` 190, `ReadRecord` 241, `RecoverWal` 320 |
| `env.h/.cc` | POSIX file helpers, fsync discipline | `FsyncFile` 17, `WriteFileAtomically` 75, `WritableFile`, `RandomAccessFile` |
| `filenames.h/.cc` | Directory layout | `SstFileName`, `ManifestFileName`, `LockFileName` |
| `coding.h` | Fixed/varint encoding | `PutVarint32`, `GetVarint64` |
| `crc32c.h/.cc` | Checksum | `Extend`, `Value` |
| `iterator.cc` | Empty/error iterators | |

### C++ network — `engine/net/`

| File | Purpose | Key |
|---|---|---|
| `protocol.h/.cc` | Wire format spec + encoders/decoders | `DecodeRequest`, `Encode*` |
| `server.h/.cc` | Thread-per-connection TCP server | `ReadFrame` 49, `HandleConnection` 145, `Dispatch` 158 |
| `client.h/.cc` | Blocking C++ client | `RoundTrip`, `Get`, `Scan` |

### Tools — `engine/tools/`

`epica_server.cc` (serve a dir), `epica_shell.cc` (local or `--remote`
REPL with `flush`/`compact`/`stats`), `db_driver.cc` (kill -9 crash test),
`wal_driver.cc` (WAL-only crash test).

### Java — `server/src/main/java/epica/`

| File | Purpose | Key |
|---|---|---|
| `client/EngineClient.java` | Interface the executor uses | |
| `client/SocketEngineClient.java` | Real engine over TCP | `roundTrip`, `scan` |
| `client/InMemoryEngineClient.java` | TreeMap fake, unsigned byte order | |
| `client/Protocol.java` | Little-endian frames | `Writer`, `Reader` |
| `query/Lexer.java`, `Token.java` | Tokens, quoting | `next`, `quoted` |
| `query/Parser.java`, `Statement.java` | Grammar → sealed AST | `statement` 29, `range` 74, `batch` 109 |
| `query/Planner.java`, `PlanNode.java` | AST → physical plan | `plan` (pushdown 34), `rangeScan` 48 |
| `query/Bytes.java` | `prefixSuccessor` 38, `immediateSuccessor`, `contains` | |
| `query/Executor.java` | Pull-based operators | `ScanIterator` 79, `fetchPage` 106, `FilterIterator` 129, `LimitIterator` 157 |
| `shell/Shell.java` | REPL rendering | |
| `server/QueryServer.java` | Line protocol server | `handle`, `render` |

### Tests

`engine/tests/*.cc` (88) and `server/src/test/java/**` (22): see
`TESTING.md` for the table of what each proves.

---

## 13. How to explain it in 60 seconds and in 5 minutes

**60 seconds.** "It's a key-value database in the LevelDB family. Writes hit
a write-ahead log and an in-memory skiplist; when that fills, it's flushed to
an immutable sorted file with a bloom filter; a background thread merges
files level by level. Reads check memory first, then each level, skipping
files via bloom filters and caching blocks. Every key carries a sequence
number, which gives snapshots and consistent scans for free. Concurrent
writers are batched into one fsync. On top I built a Java query layer with a
parser, planner and executor that talks to the engine over a binary
protocol. I crash-tested it with kill -9 and it recovers every acknowledged
write."

**5 minutes.** Walk §4 (a PUT), §5 (a GET), §6 (a crash), then §7
(compaction), then one design decision you like (group commit, or
reference-counted file deletion, or limit pushdown) from
`DESIGN_DECISIONS.md`.

---

## 14. Common misconceptions to avoid

- "The WAL is the database." No — the WAL is a *redo log* for the memtable
  only. Once a memtable is flushed, its WAL is deleted. Long-term data lives
  in SSTables listed by the MANIFEST.
- "Delete removes the key." No — it writes a tombstone. The key's bytes are
  physically gone only after a compaction that reaches the level where the
  old value lives.
- "A read locks the database." No — a read holds the mutex for a few
  pointer copies, then reads lock-free. Writers never block readers and
  readers never block writers.
- "fsync means the data is on disk." On macOS `fsync` alone only pushes to
  the drive's cache; `F_FULLFSYNC` forces the medium. Linux `fdatasync`
  does force it.
- "L0 works like the other levels." No — L0 files overlap each other (each is
  one memtable), so a Get may consult all of them. That is why L0 has a file
  count trigger and a stall limit while deeper levels have byte budgets.
- "The sequence number is per key." No — it is one global counter; every
  operation in the whole DB gets a unique number. That is what makes a single
  integer a valid snapshot.
- "The Java layer stores data." No — it is stateless; it parses, plans, and
  forwards. You can restart it any time.

---

## 15. Interview questions and strong answers

Group A — fundamentals

**Q1. What is a write-ahead log and why do you need one?**
A log where every change is appended and fsync'd *before* it is applied to
the in-memory structure. If the process dies, restart replays the log to
rebuild memory, so no acknowledged write is lost. In epicaDB `DBImpl::Write`
does Append → Sync → apply → publish, in that order
(`db_impl.cc` ~321–333). Without it, a Put acknowledged from RAM would vanish
in a crash.

**Q2. What is an LSM tree and how does it compare to a B-tree?**
An LSM tree buffers writes in memory and periodically writes sorted,
immutable runs to disk, merging them in the background. Writes are
sequential and cheap; reads may consult several runs (read amplification),
mitigated with bloom filters and leveling. A B-tree updates pages in place:
reads are one path down the tree, but writes are random I/O and crash
recovery of half-written pages is harder. LSM suits write-heavy and
append-heavy workloads; B-trees suit read-heavy point lookups with in-place
updates.

**Q3. Walk me through a Put.**
§4 above. Key beats: batch → queue → leader → make room → group → sequence
→ WAL append + fsync (unlocked) → memtable insert → publish sequence → wake
followers.

**Q4. Walk me through a Get.**
§5. Key beats: snapshot sequence → LookupKey → memtable → immutable
memtable → L0 files newest-first → one file per deeper level → bloom →
index → block cache → block scan.

**Q5. How does a delete work if files are immutable?**
Write a tombstone (an entry of type deletion with a new sequence). Reads
stop at the newest version of the key, so the tombstone hides older values
in lower levels. Compaction removes both the tombstone and the hidden
values once no snapshot can see them and no deeper level still holds the key
(`IsBaseLevelForKey`, `version.cc` ~181).

**Q6. What happens if the process is killed mid-write?**
The WAL record is torn. On the next open, `RecoverWal` hits a CRC/length
mismatch at the tail of the last log, truncates there, and continues
(`wal.cc` ~368). The torn write was never acknowledged (ack happens after
fsync), so nothing the client was promised is lost. `db_driver` demonstrates
this with real `kill -9`.

Group B — design

**Q7. Why a skiplist for the memtable and not `std::map`?**
Sorted order (for scans) with a simple lock-free story: nodes are published
with a release store after being fully built and are never freed or moved,
so readers need no lock while the single writer inserts. `std::map` would
need a reader-writer lock, and its rebalancing moves nodes. The arena makes
allocation a pointer bump and frees everything when the memtable is dropped.

**Q8. Why sequence descending in the internal key order?**
So the newest version of a key sorts first. A Seek to `(key, snapshot_seq)`
lands directly on the newest version visible at that snapshot — one seek,
no scanning backwards. It also makes compaction's "first occurrence is the
newest" rule trivial.

**Q9. How do snapshots work and what do they cost?**
A snapshot is a sequence number kept in a list. Reads at it ignore entries
with a higher sequence. Cost: compaction cannot drop versions the oldest
live snapshot could see, so long-lived snapshots retain space. No copying,
no locks.

**Q10. How does group commit work and why does it help?**
Concurrent writers queue; the head merges everyone's batches into one WAL
record and does one fsync (~4 ms) for all of them. Throughput scales with
concurrency instead of being capped at one fsync per write. The leader
role rotates naturally; no dedicated thread (`BuildBatchGroup`, ~357).

**Q11. Why does the mutex never cover disk I/O?**
Because I/O is milliseconds and everything else is microseconds. Holding
the lock during fsync would serialise readers behind writers. The leader
copies what it needs, unlocks, does the I/O, relocks to publish
(`l.unlock()` at ~320; also ~216 for flush, ~566 for compaction).

**Q12. What is a bloom filter and where do you use it?**
A bit array with k hash probes per key; answers "definitely absent" or
"maybe present". One per SSTable over its user keys; checked before reading
any data block (`Table::InternalGet` ~201). At 10 bits/key, ~1% of absent
lookups pay an unnecessary block read; every present key is always found.

**Q13. Why blocks? Why 4 KiB?**
Reading a whole SSTable per Get would be absurd; blocks make the unit of
I/O and caching small. 4 KiB matches a filesystem page and holds tens of
entries, so a linear scan inside a block is cheap once the index has picked
the block.

**Q14. Why is L0 special?**
L0 files are flushed memtables and overlap each other, so a Get may check
all of them. Deeper levels are disjoint, one file per level per key. L0 is
therefore governed by file *count* (compact at 4, stall writers at 12) while
deeper levels use byte budgets.

**Q15. What triggers compaction and how are inputs chosen?**
Score = L0 files / 4, or level bytes / budget (10 MiB × 10^(n−1)). Highest
score ≥ 1 wins. L0 → all L0 files + overlapping L1 files. Ln → one file
(round-robin by key via `compact_pointer_`) + overlapping Ln+1 files. If
nothing overlaps, the file is moved without rewriting.

**Q16. When can compaction drop an entry?**
(1) An older version when a newer version of the same key has sequence ≤
the oldest live snapshot. (2) A tombstone when its sequence ≤ the oldest
snapshot and no deeper level contains the key. Both in `DoCompactionWork`
(~615–626).

**Q17. How do you know which SSTables exist after a crash?**
The MANIFEST lists files per level. It is replaced atomically: write
`MANIFEST.tmp`, fsync, `rename`, fsync the directory. SSTables are fsync'd
(plus directory fsync) *before* the MANIFEST references them, so a listed
file is always complete. Unlisted files are orphans and are deleted at open.

**Q18. How are old SSTables deleted safely while readers might use them?**
Reference counting. Each `Version` holds `shared_ptr<FileMetaData>`; an
iterator holds its Version. When a compaction removes a file from the
current Version it is marked obsolete; the destructor unlinks it when the
last reference drops (`version.cc` ~32). It is impossible to delete a file
someone is reading.

**Q19. Why full-rewrite MANIFEST instead of an edit log like LevelDB?**
It changes a few times per second at most and is a few KiB; rewriting is
cheap and removes edit replay, CURRENT files, and unbounded growth.

**Q20. What does recovery do with logs older than the last flush?**
Skips them (`RecoverWal`'s `min_seq` = MANIFEST `log_number`) and deletes
them. Replaying them would re-insert *older* sequences into the memtable,
which sits above the SSTables in read order and would shadow newer values —
a correctness bug, not just wasted time.

Group C — concurrency & failure

**Q21. Can two readers and a writer run truly in parallel?**
Yes. Readers copy three `shared_ptr`s under the mutex then read lock-free
(skiplist with atomics, immutable SSTables, immutable Version). The writer
holds the mutex only for bookkeeping. `DBTest.ConcurrentWritersAndReaders`
runs 8 writer threads and a scanner.

**Q22. What if fsync fails?**
The WAL may or may not contain the record and the memtable does not. The
DB sets a sticky `bg_error_` and refuses further writes rather than let disk
and memory diverge. Same for a failed flush or compaction.

**Q23. How do you prevent two processes opening the same directory?**
`flock(LOCK_EX | LOCK_NB)` on `<dir>/LOCK` at open; the second open fails
with "database is locked". The lock vanishes with the process on a crash.

**Q24. What is a write stall and why is it necessary?**
If writers outrun compaction, L0 accumulates overlapping files and every
read slows. At 12 L0 files `MakeRoomForWrite` blocks writers until the
background thread reduces L0. It trades write latency for bounded read
latency.

**Q25. Why one background thread?**
Serial flush/compaction can never race with itself (no two compactions
choosing overlapping inputs). Flushes take priority because writers may be
waiting on them. Throughput is bounded by one core, which is fine here and
is a contained change later.

Group D — network & query layer

**Q26. Why a binary length-prefixed protocol instead of text or gRPC?**
Keys/values are arbitrary bytes (text would need escaping). A length prefix
makes framing a `readFully`. Trivial in any language (Java side ~150 lines,
zero deps). gRPC would add a code generator for a 7-opcode API.

**Q27. What does the planner actually decide?**
Prefix → bounded range `[p, successor(p))` computed with byte carry;
LIMIT pushed to the engine only when no WHERE filter intervenes (the engine
cannot evaluate the predicate); pull-based operator tree
`Limit → Filter → RangeScan`; `EXPLAIN` prints it.

**Q28. How does a filtered scan over a huge range avoid loading everything?**
Volcano-style iterators: `Limit` pulls from `Filter` pulls from `RangeScan`,
which fetches 128-row pages from the engine and advances a cursor
(`lastKey + 0x00`). Limit stops pulling when satisfied; nothing materialises
the whole range. `COUNT` runs in constant memory.

**Q29. How did you test that the Java client and C++ server agree on bytes?**
Both test suites pin the identical GET frame
(`08 00 00 00 02 03 00 00 00 61 62 63`), plus a gated integration test runs
the Java executor against a live engine.

**Q30. How does a `BATCH` stay atomic across the network?**
It becomes one BATCH frame → one `WriteBatch` in the server → one WAL record.
The WAL guarantees a record is entirely present or truncated away.

Group E — meta

**Q31. What was the hardest bug / most subtle invariant?**
Candidates: (a) replaying stale WALs would resurrect old values (Q20);
(b) deleting a tombstone too early resurrects a deeper old value (Q5);
(c) a trivial move must not mark the moved file obsolete (`LogAndApply`
~265–270); (d) a `"\x00abc"` C++ hex escape swallowing following hex digits
— caught by AddressSanitizer in a test literal.

**Q32. What would you do next?**
Reverse iteration, block compression (a trailer byte), parallel compactions,
a sharded block cache, replication using the WAL as the stream. Each has a
clear seam listed in `DESIGN_DECISIONS.md` §19.

**Q33. How would you measure read/write/space amplification?**
Write amp = bytes written by compaction ÷ bytes written by users
(`STATS` shows `bytes written by compaction`). Read amp = files/blocks
touched per Get (add counters in `Version::Get`). Space amp = total SST bytes
÷ live bytes (count after `CompactAll`).

---

## 16. The question-and-answer game

Cover the answer, say yours out loud, compare. Then flip it: ask yourself
the question a different way ("what breaks if we don't?").

**How do databases handle logging commands before execution in a write-ahead log?**
They serialise each change into a record, append it to a sequential log
file, and call fsync so the bytes are on stable storage, *then* apply the
change to in-memory structures and acknowledge the client. On restart the
log is replayed to rebuild memory. epicaDB: `WalWriter::Append` + `Sync`
then `ApplyBatchToMemTable` in `DBImpl::Write`. What breaks without it: an
acknowledged write lost on crash.

**How do databases detect a write that was cut in half by a crash?**
A checksum per record (CRC-32C here) plus a length field. A torn record
fails the CRC or claims more bytes than exist; recovery truncates at that
offset. epicaDB: `WalReader::ReadRecord`, `RecoverWal`.

**How do databases make a file replacement atomic?**
Write the new content to a temporary file, fsync it, `rename` it over the
old name (atomic on POSIX), fsync the directory. epicaDB: `WriteFileAtomically`
for the MANIFEST.

**How do databases delete a key when the files are read-only?**
A tombstone: a versioned "deleted" marker newer than the value. Reads stop
at the newest version; compaction later removes both.

**How do databases give a reader a consistent view while writers keep going?**
Multi-versioning: every write gets a sequence number; a reader fixes a
sequence at start and ignores anything newer. Old versions are kept until
no reader needs them. epicaDB: internal keys, `GetSnapshot`, `DBIter`.

**How do databases avoid reading a file that cannot contain the key?**
Key-range metadata per file (skip if outside [smallest, largest]) and a
bloom filter (skip if "definitely absent"). epicaDB: `Version::Get`,
`Table::InternalGet`.

**How do databases find a key inside a large sorted file without reading it all?**
An index of (last key per block → block offset), binary-searched; then one
block is read and scanned. epicaDB: index block + `TableIterator`.

**How do databases keep the number of files a read must touch bounded?**
Leveling: files in a level are disjoint, so one file per level; the number of
levels is logarithmic in data size; L0 is capped by count. Compaction
maintains these invariants.

**How do databases make many concurrent writers cheap when each needs fsync?**
Group commit: one thread writes and syncs a combined record for all waiting
writers. epicaDB: `BuildBatchGroup`.

**How do databases make sure a background merge does not delete a file a query is still reading?**
Reference counting or epoch-based reclamation. epicaDB: `shared_ptr` to
`FileMetaData`; unlink in the destructor if obsolete.

**How do databases recover the list of live files after a crash?**
A manifest/catalog written durably before the files are considered live;
anything on disk but not in the manifest is garbage. epicaDB: `MANIFEST`,
`DeleteOrphanFiles`.

**How do databases know which log records are already reflected in data files?**
A checkpoint marker — here the MANIFEST's `log_number`: every log below it
is fully contained in SSTables. Recovery replays only logs at or above it.

**How do databases stop a burst of writes from degrading reads forever?**
Backpressure: slow or stall writers when the pending-merge backlog (L0 file
count) crosses a threshold. epicaDB: `l0_stop_writes_trigger`.

**How does a query engine avoid loading a whole table to return the first ten matching rows?**
Pull-based (Volcano) operators: the consumer asks for one row at a time;
the limit stops asking when satisfied; the scan fetches in pages. epicaDB:
`Executor` iterators.

**How does a query planner turn a prefix search into something a key-value store can do?**
A range scan `[prefix, prefix-with-last-byte-incremented)`, handling 0xFF
carry. epicaDB: `Bytes.prefixSuccessor`.

**How do two programs in different languages agree on a binary protocol?**
Fix byte order (little-endian here), fix framing (u32 length prefix), fix
every field's width, and pin one identical byte sequence in both test
suites. epicaDB: `net_test.cc` and `ProtocolTest.java`.

**How do you prove a database survives crashes rather than argue it?**
Kill the process with SIGKILL at random instants while it writes with
sync=true, then verify every acknowledged key on restart. Store the
"highest acknowledged" marker in the same atomic batch as the data so the
verifier knows the exact expected set. epicaDB: `tools/db_driver.cc`.

**How do you catch memory bugs that tests pass over silently?**
Run the whole suite under AddressSanitizer and UndefinedBehaviorSanitizer
(`-DEPICA_SANITIZE=ON`). It caught a hex-escape bug in this project.

---

## 17. A study plan for three days

**Day 1 — read the code in this order**, running the matching test after each:
1. `coding.h`, `crc32c.*`, `slice.h`, `status.h`.
2. `wal.h/.cc` → `engine_tests --gtest_filter='WalTest.*'`; run the
   `wal_driver` kill test.
3. `write_batch.*`, `internal_key.h`, `arena.h`, `skiplist.h`, `memtable.*`
   → `SkipList.*`, `MemTableTest.*`.
4. `block.*`, `bloom.*`, `cache.*`, `table.*` → `TableTest.*`, `Bloom.*`.

**Day 2 — the DB:**
5. `version.h/.cc` → `VersionTest.*`.
6. `db_impl.h/.cc`, `db_iter.*`, `merger.*` → `DBTest.*`; run `db_driver`.
7. Use `epica_shell` on a local dir: `put`, `stats`, `flush`, `stats`,
   `del`, `compact`, `stats`. Watch levels change. `xxd` the files.
8. Read `DESIGN_DECISIONS.md` end to end.

**Day 3 — the outer layers and rehearsal:**
9. `net/protocol.h`, `server.cc`; then `Protocol.java`, `SocketEngineClient`.
10. `Lexer` → `Parser` → `Planner` → `Executor`; run `mvn test`; use
    `EXPLAIN` in the shell for several queries.
11. Run `scripts/run_tests.sh` and be able to say what each stage proves.
12. Say the 60-second pitch (§13) five times. Do the Q&A game (§16) twice.
    Pick three design decisions you can defend and one you would change.
