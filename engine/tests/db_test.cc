// tests/db_test.cc -- end-to-end behaviour of the engine: durability across
// reopen, flush and compaction correctness, snapshots, scans, concurrency,
// and crash recovery from the combination of MANIFEST + WAL.
#include "epica/db.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <thread>

#include "db_impl.h"
#include "filenames.h"

namespace epica {
namespace fs = std::filesystem;

class DBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/epica_db_test_XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(tmpl));
    dir_ = tmpl;
    // Small thresholds so flush + compaction happen within a test.
    opts_.memtable_size = 64 * 1024;
    opts_.target_file_size = 32 * 1024;
    opts_.level1_max_bytes = 256 * 1024;
    opts_.block_size = 1024;
    Reopen();
  }
  void TearDown() override {
    db_.reset();
    fs::remove_all(dir_);
  }

  void Reopen() {
    db_.reset();
    DB* raw;
    Status s = DB::Open(opts_, dir_, &raw);
    ASSERT_TRUE(s.ok()) << s.ToString();
    db_.reset(raw);
  }
  DBImpl* impl() { return static_cast<DBImpl*>(db_.get()); }

  Status Put(const std::string& k, const std::string& v, bool sync = false) {
    WriteOptions wo;
    wo.sync = sync;
    return db_->Put(wo, k, v);
  }
  Status Del(const std::string& k) { return db_->Delete(WriteOptions{.sync = false}, k); }
  // Returns the value, "NOT_FOUND", or the error string.
  std::string Get(const std::string& k, const Snapshot* snap = nullptr) {
    ReadOptions ro;
    ro.snapshot = snap;
    std::string v;
    Status s = db_->Get(ro, k, &v);
    if (s.ok()) return v;
    if (s.IsNotFound()) return "NOT_FOUND";
    return s.ToString();
  }
  std::vector<std::pair<std::string, std::string>> Scan(const std::string& from = "",
                                                        const std::string& to = "",
                                                        const Snapshot* snap = nullptr) {
    ReadOptions ro;
    ro.snapshot = snap;
    std::unique_ptr<Iterator> it(db_->NewIterator(ro));
    std::vector<std::pair<std::string, std::string>> out;
    for (from.empty() ? it->SeekToFirst() : it->Seek(from); it->Valid(); it->Next()) {
      if (!to.empty() && it->key().compare(to) >= 0) break;
      out.emplace_back(it->key().ToString(), it->value().ToString());
    }
    EXPECT_TRUE(it->status().ok()) << it->status().ToString();
    return out;
  }
  static std::string Key(int i) {
    char b[32];
    std::snprintf(b, sizeof(b), "key%06d", i);
    return b;
  }
  int TotalFiles() {
    int n = 0;
    for (int l = 0; l < opts_.num_levels; ++l) n += impl()->NumLevelFiles(l);
    return n;
  }

  std::string dir_;
  Options opts_;
  std::unique_ptr<DB> db_;
};

TEST_F(DBTest, PutGetDelete) {
  EXPECT_EQ("NOT_FOUND", Get("a"));
  ASSERT_TRUE(Put("a", "1").ok());
  EXPECT_EQ("1", Get("a"));
  ASSERT_TRUE(Put("a", "2").ok());
  EXPECT_EQ("2", Get("a"));
  ASSERT_TRUE(Del("a").ok());
  EXPECT_EQ("NOT_FOUND", Get("a"));
  ASSERT_TRUE(Put("a", "3").ok());
  EXPECT_EQ("3", Get("a"));
}

TEST_F(DBTest, EmptyKeyAndValueAndBinary) {
  ASSERT_TRUE(Put("", "empty-key").ok());
  ASSERT_TRUE(Put("empty-value", "").ok());
  const std::string bin("\x00\x01\xff\n\r", 5);
  ASSERT_TRUE(Put(bin, bin).ok());
  EXPECT_EQ("empty-key", Get(""));
  EXPECT_EQ("", Get("empty-value"));
  EXPECT_EQ(bin, Get(bin));
}

TEST_F(DBTest, WriteBatchIsAtomicAndOrdered) {
  WriteBatch b;
  b.Put("x", "1");
  b.Put("y", "2");
  b.Delete("x");
  b.Put("z", "3");
  ASSERT_TRUE(db_->Write(WriteOptions{}, &b).ok());
  EXPECT_EQ("NOT_FOUND", Get("x")) << "delete after put in the same batch wins";
  EXPECT_EQ("2", Get("y"));
  EXPECT_EQ("3", Get("z"));
  EXPECT_EQ(4u, impl()->LastSequence()) << "one sequence per op";
}

TEST_F(DBTest, SurvivesReopenViaWalOnly) {
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(Put(Key(i), "v" + std::to_string(i)).ok());
  ASSERT_TRUE(Del(Key(50)).ok());
  EXPECT_EQ(0, TotalFiles()) << "everything still in the memtable";
  Reopen();  // no flush happened: recovery must come from the WAL
  for (int i = 0; i < 100; ++i) {
    if (i == 50) EXPECT_EQ("NOT_FOUND", Get(Key(i)));
    else EXPECT_EQ("v" + std::to_string(i), Get(Key(i)));
  }
  EXPECT_EQ(101u, impl()->LastSequence()) << "sequence counter restored from WAL";
}

TEST_F(DBTest, FlushCreatesL0FileAndDeletesOldWal) {
  for (int i = 0; i < 50; ++i) ASSERT_TRUE(Put(Key(i), "v").ok());
  std::vector<uint64_t> before;
  ASSERT_TRUE(ListWalFiles(dir_, &before).ok());
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(1, impl()->NumLevelFiles(0));
  std::vector<uint64_t> after;
  ASSERT_TRUE(ListWalFiles(dir_, &after).ok());
  ASSERT_EQ(1u, after.size());
  EXPECT_GT(after[0], before.back()) << "old WAL removed, fresh one in place";
  for (int i = 0; i < 50; ++i) EXPECT_EQ("v", Get(Key(i)));
  Reopen();
  for (int i = 0; i < 50; ++i) EXPECT_EQ("v", Get(Key(i)));
}

TEST_F(DBTest, ReadsSpanMemtableAndSSTables) {
  ASSERT_TRUE(Put("a", "disk").ok());
  ASSERT_TRUE(Put("b", "disk").ok());
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(Put("b", "mem").ok());  // newer version in memtable
  ASSERT_TRUE(Del("a").ok());          // tombstone in memtable over disk value
  ASSERT_TRUE(Put("c", "mem").ok());
  EXPECT_EQ("NOT_FOUND", Get("a"));
  EXPECT_EQ("mem", Get("b"));
  EXPECT_EQ("mem", Get("c"));
  auto rows = Scan();
  ASSERT_EQ(2u, rows.size());
  EXPECT_EQ("b", rows[0].first);
  EXPECT_EQ("c", rows[1].first);
}

TEST_F(DBTest, ManyWritesTriggerFlushAndCompactionAndStayCorrect) {
  std::map<std::string, std::string> model;
  const std::string big(200, 'x');
  for (int i = 0; i < 6000; ++i) {
    const std::string k = Key(i % 1500);  // overwrite each key 4 times
    const std::string v = big + std::to_string(i);
    ASSERT_TRUE(Put(k, v).ok());
    model[k] = v;
    if (i % 7 == 0) {
      ASSERT_TRUE(Del(Key((i * 13) % 1500)).ok());
      model.erase(Key((i * 13) % 1500));
    }
  }
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ(0, impl()->NumLevelFiles(0)) << "CompactAll pushes everything down";
  EXPECT_GT(TotalFiles(), 0);

  for (const auto& [k, v] : model) EXPECT_EQ(v, Get(k)) << k;
  for (int i = 0; i < 1500; ++i) {
    if (!model.count(Key(i))) EXPECT_EQ("NOT_FOUND", Get(Key(i)));
  }
  auto rows = Scan();
  ASSERT_EQ(model.size(), rows.size());
  auto m = model.begin();
  for (const auto& [k, v] : rows) {
    EXPECT_EQ(m->first, k);
    EXPECT_EQ(m->second, v);
    ++m;
  }
  // And again after a reopen (MANIFEST + tables).
  Reopen();
  for (const auto& [k, v] : model) EXPECT_EQ(v, Get(k)) << k;
}

TEST_F(DBTest, BackgroundCompactionRunsOnItsOwn) {
  opts_.l0_compaction_trigger = 2;
  Reopen();
  const std::string big(500, 'y');
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(Put(Key(i), big).ok());
  ASSERT_TRUE(db_->Flush().ok());
  // Wait (bounded) for the background thread to drain L0 below the trigger.
  for (int tries = 0; tries < 200 && impl()->NumLevelFiles(0) >= 2; ++tries) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_LT(impl()->NumLevelFiles(0), 2);
  for (int i = 0; i < 2000; i += 97) EXPECT_EQ(big, Get(Key(i)));
}

TEST_F(DBTest, RangeScanWithBounds) {
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(Put(Key(i), std::to_string(i)).ok());
  ASSERT_TRUE(db_->Flush().ok());
  for (int i = 100; i < 200; ++i) ASSERT_TRUE(Put(Key(i), std::to_string(i)).ok());
  auto rows = Scan(Key(95), Key(105));
  ASSERT_EQ(10u, rows.size()) << "half from SSTable, half from memtable";
  for (int i = 0; i < 10; ++i) EXPECT_EQ(Key(95 + i), rows[static_cast<size_t>(i)].first);
  auto tail = Scan(Key(198));
  ASSERT_EQ(2u, tail.size());
  EXPECT_EQ(Key(199), tail[1].first);
  EXPECT_TRUE(Scan("zzz").empty());
}

TEST_F(DBTest, SnapshotIsolatesReads) {
  ASSERT_TRUE(Put("k", "v1").ok());
  const Snapshot* s1 = db_->GetSnapshot();
  ASSERT_TRUE(Put("k", "v2").ok());
  ASSERT_TRUE(Put("new", "n").ok());
  const Snapshot* s2 = db_->GetSnapshot();
  ASSERT_TRUE(Del("k").ok());

  EXPECT_EQ("v1", Get("k", s1));
  EXPECT_EQ("NOT_FOUND", Get("new", s1));
  EXPECT_EQ("v2", Get("k", s2));
  EXPECT_EQ("n", Get("new", s2));
  EXPECT_EQ("NOT_FOUND", Get("k"));

  // Snapshots hold across flush and compaction: the old versions must not
  // be compacted away while s1 is alive.
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ("v1", Get("k", s1));
  EXPECT_EQ("v2", Get("k", s2));
  auto rows = Scan("", "", s1);
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ("v1", rows[0].second);

  db_->ReleaseSnapshot(s1);
  db_->ReleaseSnapshot(s2);
  // Now compaction may drop them; the latest view is unchanged.
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ("NOT_FOUND", Get("k"));
  EXPECT_EQ("n", Get("new"));
}

TEST_F(DBTest, IteratorSeesConsistentSnapshotDespiteWrites) {
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(Put(Key(i), "old").ok());
  std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions{}));
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(Put(Key(i), "new").ok());
  ASSERT_TRUE(Put(Key(99), "extra").ok());
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++n) EXPECT_EQ("old", it->value().ToString());
  EXPECT_EQ(10, n);
}

TEST_F(DBTest, TombstonesAreDroppedAtBottomAfterCompaction) {
  for (int i = 0; i < 500; ++i) ASSERT_TRUE(Put(Key(i), std::string(100, 'a')).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  for (int i = 0; i < 500; ++i) ASSERT_TRUE(Del(Key(i)).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  // Everything deleted and compacted: no files should remain at all.
  EXPECT_EQ(0, TotalFiles()) << db_->GetStats();
  EXPECT_TRUE(Scan().empty());
}

TEST_F(DBTest, ConcurrentWritersAndReaders) {
  constexpr int kThreads = 8, kPerThread = 500;
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string k = "t" + std::to_string(t) + "-" + std::to_string(i);
        if (!Put(k, k + "-v").ok()) ++errors;
        std::string v;
        if (!db_->Get(ReadOptions{}, k, &v).ok() || v != k + "-v") ++errors;  // read-your-writes
      }
    });
  }
  // A reader thread scanning throughout.
  std::atomic<bool> stop{false};
  std::thread scanner([&] {
    while (!stop) {
      auto rows = Scan();
      for (size_t i = 1; i < rows.size(); ++i) {
        if (!(rows[i - 1].first < rows[i].first)) ++errors;
      }
    }
  });
  for (auto& th : threads) th.join();
  stop = true;
  scanner.join();
  EXPECT_EQ(0, errors.load());
  EXPECT_EQ(static_cast<uint64_t>(kThreads * kPerThread), impl()->LastSequence());
  EXPECT_EQ(static_cast<size_t>(kThreads * kPerThread), Scan().size());
  Reopen();
  EXPECT_EQ(static_cast<size_t>(kThreads * kPerThread), Scan().size());
}

TEST_F(DBTest, SyncedWritesSurviveSimulatedCrash) {
  // "Crash" = abandon the DB object without running its destructor (so no
  // final Sync/Close) by leaking it, then open the directory again. The
  // engine's LOCK is released by closing the fd directly.
  for (int i = 0; i < 20; ++i) ASSERT_TRUE(Put(Key(i), "durable", /*sync=*/true).ok());
  ASSERT_TRUE(Put("unsynced", "maybe", /*sync=*/false).ok());
  DB* leaked = db_.release();
  (void)leaked;  // intentionally never deleted: simulates SIGKILL
  // The leaked DB still holds the flock on LOCK. Removing the file lets a new
  // Open create (and lock) a fresh one -- exactly what happens after a real
  // crash, where the dead process's lock vanished with it.
  fs::remove(LockFileName(dir_));
  Reopen();
  for (int i = 0; i < 20; ++i) EXPECT_EQ("durable", Get(Key(i)));
  // The unsynced write was still write(2)'d into the OS buffer (no crash of
  // the OS here), so it is normally present too -- but nothing guarantees it.
}

TEST_F(DBTest, TwoOpensOfSameDirFail) {
  DB* second;
  Status s = DB::Open(opts_, dir_, &second);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(std::string::npos, s.ToString().find("locked"));
}

TEST_F(DBTest, StatsMentionLevels) {
  ASSERT_TRUE(Put("a", "b").ok());
  ASSERT_TRUE(db_->Flush().ok());
  std::string st = db_->GetStats();
  EXPECT_NE(std::string::npos, st.find("L0:   1 files"));
  EXPECT_NE(std::string::npos, st.find("last_sequence:      1"));
}

TEST_F(DBTest, RecoveryReplaysOnlyLogsAfterLastFlush) {
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(Put(Key(i), "flushed").ok());
  ASSERT_TRUE(db_->Flush().ok());
  for (int i = 10; i < 20; ++i) ASSERT_TRUE(Put(Key(i), "logged").ok());
  Reopen();
  for (int i = 0; i < 10; ++i) EXPECT_EQ("flushed", Get(Key(i)));
  for (int i = 10; i < 20; ++i) EXPECT_EQ("logged", Get(Key(i)));
  EXPECT_EQ(20u, impl()->LastSequence());
  // Recovery flushes replayed data to L0 and starts a fresh empty WAL.
  std::vector<uint64_t> wals;
  ASSERT_TRUE(ListWalFiles(dir_, &wals).ok());
  EXPECT_EQ(1u, wals.size());
  EXPECT_EQ(0u, fs::file_size(WalFileName(dir_, wals[0])));
}

}  // namespace epica
