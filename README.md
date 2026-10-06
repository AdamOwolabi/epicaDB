# epicaDB

A key-value database built from the disk up. The storage engine is C++20
(write-ahead log, skiplist memtable, block-based SSTables with bloom filters,
leveled compaction, snapshots, group commit); the query layer is Java
(lexer → parser → planner → pull-based executor) and talks to the engine
over a small binary socket protocol.

| Document | Read it for |
|---|---|
| [`BEGINNER_OVERVIEW.md`](BEGINNER_OVERVIEW.md) | how everything works, high level to byte level, with worked examples, a code map, and interview Q&A |
| [`DESIGN_DECISIONS.md`](DESIGN_DECISIONS.md) | every major choice, the alternatives, and why |
| [`TESTING.md`](TESTING.md) | every test and demo, what each proves, and how to run it by hand |
| `scripts/run_tests.sh` | runs all of the above, staged |

## Status

| Milestone | State |
|---|---|
| 1. Write-ahead log | done — 22 tests, kill -9 tested |
| 2. Memtable (arena skiplist) + WAL integration + WriteBatch | done |
| 3. SSTable flush (blocks, index, bloom filter, CRC) | done |
| 4. Multi-level reads, bloom filters, LRU block cache | done |
| 5. Leveled compaction, MANIFEST, tombstone GC, snapshots | done |
| 6. Range scans, binary socket protocol, TCP server, Java query layer | done |
| 7. Concurrency: group commit, lock-free readers, background thread, write stalls | done |

88 C++ tests (GoogleTest, also clean under ASan/UBSan) + 22 Java tests
(JUnit 5) + two `kill -9` crash drivers.

## Build and run

Requires CMake ≥ 3.20, a C++20 compiler, JDK 17 and Maven for the Java layer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8
./build/engine/engine_tests                          # C++ tests

export JAVA_HOME=$(/usr/libexec/java_home -v 17)     # macOS; pick your JDK 17 path otherwise
(cd server && mvn -q package)                        # Java tests + jar

./build/engine/epica_server /tmp/mydb 7379 &         # storage engine
java -jar server/target/epica-query.jar shell        # query shell -> engine
```

```
epica> PUT user:1 "Ada Lovelace"
OK (4.31 ms)
epica> SCAN PREFIX user: WHERE VALUE CONTAINS Ada LIMIT 5
user:1 = "Ada Lovelace"
(1 row, 0.62 ms)
epica> EXPLAIN SCAN PREFIX user: LIMIT 5
Limit(5)
  RangeScan(start="user:", end="user;", engineLimit=5, pageSize=128)
```

## Layout

```
engine/                     C++ storage engine
  include/epica/            public API: db.h, options.h, iterator.h, write_batch.h, wal.h, status.h, slice.h
  src/                      implementation (see BEGINNER_OVERVIEW.md "Code map")
  net/                      binary protocol, TCP server, C++ client
  tests/                    GoogleTest suites (88 tests)
  tools/                    epica_server, epica_shell, db_driver, wal_driver
server/                     Java query layer (Maven)
  src/main/java/epica/client   EngineClient interface, socket + in-memory implementations, Protocol
  src/main/java/epica/query    Lexer, Parser, Statement (AST), Planner, PlanNode, Executor
  src/main/java/epica/shell    interactive REPL
  src/main/java/epica/server   line-oriented TCP query server
scripts/run_tests.sh        staged test runner
cmake/                      build helpers (macOS libc++ header fallback)
```

## On-disk layout of a database

```
<dir>/LOCK                 advisory lock (flock) — one process at a time
<dir>/MANIFEST             live SSTables per level + counters, rewritten atomically
<dir>/wal/<num>.log        write-ahead logs; one per memtable generation
<dir>/sst/<num>.sst        SSTables; immutable once written
```

## Query language

```
PUT k v      GET k      DEL k
SCAN  [PREFIX p | [FROM a] [TO b]] [WHERE VALUE CONTAINS s] [LIMIT n]
COUNT [PREFIX p | [FROM a] [TO b]] [WHERE VALUE CONTAINS s]
BATCH PUT k v; DEL k2; ... END          atomic
EXPLAIN <statement>                     show the plan
STATS
```
Values with spaces are double-quoted; `\n \t \0 \" \\` escapes work.

## Contributing

Open issues are seeded from `.github/issues/` (reverse iteration, block
compression, sharded block cache). See [`CONTRIBUTING.md`](CONTRIBUTING.md).
First push: `scripts/github_bootstrap.sh` creates the repo, pushes, and opens
those issues via `gh`. MIT licensed.
