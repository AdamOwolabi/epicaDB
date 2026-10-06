# epicaDB — Testing Guide

Everything here can be run by hand, one command at a time, or all at once
with `scripts/run_tests.sh`. Each section says **what it proves** so you can
explain the result, not just observe it.

Paths assume you are in the repository root. Commands were verified on macOS
(Apple Silicon) with Apple clang 16, CMake 4.4, Temurin JDK 17, Maven 3.9.

```bash
# one-time: make sure Maven uses JDK 17 (Homebrew's mvn defaults to a newer JDK)
export JAVA_HOME=$(/usr/libexec/java_home -v 17)
```

---

## 0. The fast path

```bash
scripts/run_tests.sh          # stages 1–8, stops at first failure (~4 min incl. sanitizers)
scripts/run_tests.sh 1 7      # skip the slow sanitizer stage
scripts/run_tests.sh 4        # just the whole-DB crash test
```

---

## 1. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
ls build/engine/            # engine_tests  epica_server  epica_shell  db_driver  wal_driver
```

**Proves:** the C++20 engine, network layer, tools and tests compile with
`-Wall -Wextra -Wpedantic -Werror` (zero warnings tolerated in library code).

macOS note: if you see `'cstddef' file not found`, the Command Line Tools'
libc++ headers are broken; `cmake/AppleLibcxxFallback.cmake` detects that
and points the compiler at the SDK copy automatically.

---

## 2. C++ unit and integration tests (88 tests)

```bash
./build/engine/engine_tests                       # everything
./build/engine/engine_tests --gtest_brief=1       # only failures + summary
ctest --test-dir build --output-on-failure        # same, via CTest
./build/engine/engine_tests --gtest_filter='DBTest.*'         # one suite
./build/engine/engine_tests --gtest_filter='*Snapshot*'       # by name
./build/engine/engine_tests --gtest_list_tests                # see names
```

| Suite (file) | What it proves |
|---|---|
| `Crc32c`, `Coding` (`crc32c_test.cc`, `coding_test.cc`) | checksums match known vectors; varints/fixed ints round-trip, little-endian on disk |
| `WalTest` (`wal_test.cc`, 22 tests) | records round-trip; torn tails are truncated; corruption in an earlier log is a hard error; multi-file replay order; buffering vs sync |
| `SkipList` (`skiplist_test.cc`) | sorted iteration, seek semantics, **4 readers racing 1 writer see only sorted prefixes** |
| `MemTableTest`, `InternalKey` | newest version wins; snapshot sequence sees the right older version; tombstones |
| `WriteBatch` | encoding, group-commit `Append`, corrupt count detected |
| `Bloom` | zero false negatives on 10k keys; false-positive rate < 2% at 10 bits/key |
| `BlockCache` | LRU eviction order; per-file erase; evicted block stays valid for holders |
| `TableTest` | SSTable build/read; seeks across block boundaries; bloom + cache used; flipped bit ⇒ `Corruption`; truncated file refuses to open |
| `VersionTest` | MANIFEST persist/recover; corrupt MANIFEST detected; L0 and byte-based compaction triggers; trivial move; `IsBaseLevelForKey`; obsolete file unlinked only when last ref drops |
| `DBTest` | put/get/delete; binary keys; atomic batch; **reopen with WAL only**; flush deletes old WAL; reads span memtable + SSTables; 6000 writes ⇒ flush + compaction ⇒ still correct vs. a `std::map` model; background compaction drains L0 on its own; bounded scans; **snapshots isolate reads across compaction**; iterator sees a consistent snapshot; tombstones vanish after bottom-level compaction; 8 writer threads + 1 scanner; simulated crash; second `Open` on same dir fails with "locked"; recovery replays only logs after the last flush |
| `Protocol`, `ServerTest` (`net_test.cc`) | GET frame is byte-exact to the spec; garbage is rejected; TCP round trips; batch/scan/stats; binary safety; 8 concurrent clients × 200 ops |

Run a single test with extra output:

```bash
./build/engine/engine_tests --gtest_filter=DBTest.ManyWritesTriggerFlushAndCompactionAndStayCorrect
# stderr shows lines like:  epica: compacted L0 (4 files) + L1 (0 files) -> 3 files, kept 1500, dropped 4500
```

---

## 3. WAL crash test (real `kill -9`)

Terminal A:
```bash
./build/engine/wal_driver /tmp/epica-wal       # appends forever with sync_on_append, prints pid
```
Terminal B (any time):
```bash
kill -9 <pid>
./build/engine/wal_driver --recover /tmp/epica-wal
# recovered 1837 records from 1 file(s); last_seq=1; truncated=0 bytes; sequence contiguous
```
Run A again — it resumes numbering where it left off — and kill it again.
`--recover` must always say `sequence contiguous`.

**Proves:** no acknowledged record is lost and none is duplicated or
reordered, even when the process dies mid-write. If the kill lands inside a
record you will see `truncated=N bytes`: the torn tail was detected by CRC
and cut off (`wal.cc` `RecoverWal`).

---

## 4. Whole-database crash test

```bash
rm -rf /tmp/epica-crash
./build/engine/db_driver /tmp/epica-crash          # writes key-N + "__max"=N in one batch, sync=true
# ... in another terminal:
kill -9 <pid>
./build/engine/db_driver --verify /tmp/epica-crash
# epica: replayed 723 batches from 1 log(s)
# verified keys 0..722: 0 bad, scan saw 723 keys (OK)
# <stats dump>
```

Repeat run/kill/verify several times. The driver uses a tiny memtable
(256 KiB) so flushes and compactions happen every few seconds; kills will
land during them.

**Proves:** recovery correctly combines the MANIFEST (which SSTables exist),
WAL replay (writes after the last flush), torn-tail truncation, and orphan
cleanup (an SSTable half-written by a compaction that never committed).
Because `__max` is written in the *same batch* as the data, the verifier
knows exactly which keys must exist: atomicity of `WriteBatch` is what makes
the test decidable.

Watch throughput: ~250 writes/s. That is `F_FULLFSYNC` on a laptop SSD,
the honest cost of `sync=true`. Try the same with several concurrent writers
via the server (section 7) and watch group commit raise it.

---

## 5. Java unit tests (no engine required)

```bash
cd server
mvn -q test
grep -h "Tests run" target/surefire-reports/*.txt
cd ..
```

| Test class | What it proves |
|---|---|
| `LexerTest` | words, numbers, quoted strings with escapes, case-insensitive keywords |
| `ParserTest` | every grammar production; positions in error messages |
| `PlannerTest` | prefix ⇒ bounded range (`user:` ⇒ `[user:, user;)`), 0xFF carry; **limit pushdown only without WHERE**; EXPLAIN tree |
| `ExecutorTest` | full queries against the in-memory engine; `SCAN LIMIT 5` = 1 round trip, filtered limit stops after first page, `COUNT` over 1000 keys = 8 pages |
| `ProtocolTest` | Java encoder emits the exact bytes the C++ test pins (`08 00 00 00 02 03 00 00 00 61 62 63`) |
| `EngineIntegrationTest` | skipped unless `EPICA_ENGINE_PORT` is set (section 6) |

---

## 6. Java ↔ C++ integration test

Terminal A:
```bash
./build/engine/epica_server /tmp/epica-itest 7379
```
Terminal B:
```bash
cd server && EPICA_ENGINE_PORT=7379 mvn -q test -Dtest=EngineIntegrationTest && cd ..
```

**Proves:** the two independently written protocol implementations agree
byte-for-byte; PUT/GET/COUNT/filtered COUNT/LIMIT/BATCH/STATS all work
through the real engine.

---

## 7. Drive the whole stack by hand

Three terminals.

```bash
# A: storage engine
./build/engine/epica_server /tmp/epica-demo 7379

# B: Java query server (text protocol on 7380 -> binary protocol on 7379)
java -jar server/target/epica-query.jar server 7380 127.0.0.1:7379
#    (build the jar once with: cd server && mvn -q package -DskipTests)

# C: talk to it
printf 'PUT user:1 "Ada Lovelace"\nGET user:1\nSCAN PREFIX user:\nquit\n' | nc 127.0.0.1 7380
```

Or use the interactive Java shell (same query language, prettier output):

```bash
java -jar server/target/epica-query.jar shell 127.0.0.1:7379
epica> PUT user:1 "Ada Lovelace"
epica> PUT user:2 "Alan Turing"
epica> EXPLAIN SCAN PREFIX user: WHERE VALUE CONTAINS Alan LIMIT 1
epica> SCAN PREFIX user: WHERE VALUE CONTAINS Alan LIMIT 1
epica> BATCH DEL user:1; PUT user:3 "Grace Hopper" END
epica> COUNT PREFIX user:
epica> STATS
```

Or the C++ shell, which can also force flushes/compactions on a local dir:

```bash
./build/engine/epica_shell /tmp/epica-demo2
epica> put a 1
epica> put b 2
epica> stats            # note: memtable has 2 entries, L0: 0 files
epica> flush
epica> stats            # L0: 1 files; wal file is new and empty
epica> del a
epica> compact
epica> stats            # tombstone gone; only b remains in an L1 file
epica> scan
```

And without any engine at all (in-memory fake, good for learning the query
language):

```bash
java -jar server/target/epica-query.jar memory
```

**Proves:** the whole pipeline — text query → lexer → parser → planner →
executor → binary protocol → TCP → C++ server → group commit → WAL →
memtable → SSTables — and back.

Try concurrency: run two `nc` loops writing at once and read `STATS`;
`flushes/compactions` and `server requests` climb while every GET stays
correct.

---

## 8. Sanitizers

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DEPICA_SANITIZE=ON
cmake --build build-asan -j8
./build-asan/engine/engine_tests
```

**Proves:** no heap/stack/global buffer overflows, no use-after-free, no
undefined behaviour across all 88 tests including the multi-threaded ones.
(During development this build caught an out-of-range hex escape in a test
literal that the normal build silently accepted.)

---

## 9. Inspecting what is on disk

```bash
ls -la /tmp/epica-demo /tmp/epica-demo/wal /tmp/epica-demo/sst
xxd /tmp/epica-demo/wal/000000000003.log | head        # crc | len | type=03 | batch bytes
xxd /tmp/epica-demo/sst/000000000004.sst | tail -4     # footer ends with magic "EPICADB\0" (reversed: LE)
xxd /tmp/epica-demo/MANIFEST | head                    # crc | len | varints
```

Decode a WAL record by hand: bytes 0–3 CRC-32C, 4–7 payload length, byte 8
type (`03` = WriteBatch), then the batch: 8-byte sequence, 4-byte op count,
then `01 <klen> key <vlen> value` per Put or `00 <klen> key` per Delete.

---

## 10. Cleaning up

```bash
rm -rf /tmp/epica-* /tmp/epica_*
rm -rf build build-asan server/target
```
