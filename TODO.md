# Project Criteria

## Requirements

- [x] support concurrency, multi programming, file system, multi block storage
      (group commit + lock-free readers + background thread: `engine/src/db_impl.cc`;
       thread-per-connection TCP server: `engine/net/server.cc`;
       directory/file layout with fsync discipline: `engine/src/env.cc`, `filenames.h`;
       4 KiB blocks with CRC + LRU block cache: `engine/src/block.cc`, `table.cc`, `cache.cc`)
- [x] Write ahead Log (milestone 1: `engine/src/wal.cc`, 22 tests, crash-tested)
- [x] key value storage engine via LSM engine
      (memtable `memtable.cc`/`skiplist.h`, SSTables `table.cc`, leveled compaction
       `version.cc` + `db_impl.cc`, MANIFEST, snapshots, tombstone GC)
- [x] support query engine
      (Java: `server/src/main/java/epica/query` — lexer, parser, planner with prefix
       bounds and limit pushdown, pull-based executor with pagination, EXPLAIN)

## Features

- [x] PUT / GET / DEL / atomic BATCH
- [x] range scans: FROM/TO, PREFIX, WHERE VALUE CONTAINS, LIMIT, COUNT
- [x] snapshots (point-in-time reads) and consistent iterators
- [x] EXPLAIN shows the physical plan
- [x] STATS shows memtable, levels, cache hit rate, WAL, sequence

## Technical Requirements

- [x] durability: WAL fsync (`F_FULLFSYNC` on macOS) before acknowledging
- [x] atomicity: WriteBatch = one WAL record
- [x] crash recovery: MANIFEST + WAL replay + torn-tail truncation + orphan cleanup
- [x] corruption detection: CRC-32C on WAL records, SSTable blocks, MANIFEST
- [x] bounded memory: memtable size limit, streaming scans with pagination
- [x] single-process safety: flock on `<dir>/LOCK`

## User Experience

- [x] `epica_shell` (C++ local/remote), Java `shell`, Java `memory` mode, `nc`-friendly text server

## Data and Validation

- [x] binary-safe keys and values end to end
- [x] malformed frames/queries produce errors, never crashes

## Testing

- [x] ensure all changes are logged before being applied (`DBImpl::Write` steps 1–4)
- [x] balance reads and writes (bloom filters, block cache, L0 trigger 4 / stall 12,
      level budgets; `DBTest.BackgroundCompactionRunsOnItsOwn`)
- [x] 88 C++ tests, 22 Java tests, ASan/UBSan clean, two kill -9 drivers, `scripts/run_tests.sh`

## Documentation

- [x] header comment on every file; comments on key functions/lines
- [x] `DESIGN_DECISIONS.md`, `BEGINNER_OVERVIEW.md`, `TESTING.md`, `README.md`

## Definition of Done

- [x] `scripts/run_tests.sh` passes end to end

## Possible next steps (not required)

- [ ] reverse iteration (`Prev`)
- [ ] block compression
- [ ] parallel compactions / sharded block cache
- [ ] replication using the WAL as the stream
