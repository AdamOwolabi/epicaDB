// db_impl.h -- the engine's brain: ties WAL, memtables, versions, compaction
// and threading together behind the DB interface.
//
// Data flow for a write:
//
//   Put() ---> Write() ---> [writer queue, one leader] ---> WAL Append+Sync
//                                                          ---> MemTable Add
//                                                          ---> wake followers
//
// Data flow for a read (newest source wins):
//
//   Get() ---> mem_ ---> imm_ ---> Version (L0 files newest-first, then L1..Ln)
//
// Background thread (exactly one):
//
//   loop:
//     if imm_ != null           -> CompactMemTable():  imm_ -> new L0 SSTable
//     else if NeedsCompaction   -> BackgroundCompaction(): merge Ln + Ln+1
//     else                      -> sleep on bg_cv_ until signalled
//
// Locking: a single mutex `mu_` protects every mutable field below. It is
// held only for short bookkeeping sections and NEVER during disk I/O: the
// write leader drops it before appending to the WAL; the background thread
// drops it while building tables. Readers hold it just long enough to copy
// shared_ptrs to the memtables and current Version, then read lock-free.
//
// Sequence numbers: every operation gets a unique, increasing 56-bit
// sequence. `versions_->LastSequence()` is the newest committed one. A
// snapshot is simply a remembered sequence number; a read at snapshot S sees
// exactly the operations with sequence <= S.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cache.h"
#include "epica/db.h"
#include "epica/wal.h"
#include "internal_key.h"
#include "memtable.h"
#include "version.h"

namespace epica {

class SnapshotImpl : public Snapshot {
 public:
  explicit SnapshotImpl(SequenceNumber s) : sequence(s) {}
  const SequenceNumber sequence;
  std::list<SnapshotImpl*>::iterator pos;  // for O(1) removal
};

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, std::string dir);
  ~DBImpl() override;

  Status Put(const WriteOptions& opts, Slice key, Slice value) override;
  Status Delete(const WriteOptions& opts, Slice key) override;
  Status Write(const WriteOptions& opts, WriteBatch* batch) override;
  Status Get(const ReadOptions& opts, Slice key, std::string* value) override;
  Iterator* NewIterator(const ReadOptions& opts) override;
  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;
  Status Flush() override;
  Status CompactAll() override;
  std::string GetStats() override;

  // Called by DB::Open. Locks the directory, loads MANIFEST, replays WALs.
  Status Recover();

  // Test hooks.
  int NumLevelFiles(int level);
  uint64_t LastSequence();

 private:
  struct Writer;

  using Lock = std::unique_lock<std::mutex>;

  // --- write path ---
  Status MakeRoomForWrite(Lock& l);                   // REQUIRES mu_ (may wait)
  WriteBatch* BuildBatchGroup(Writer** last_writer);  // REQUIRES mu_
  static Status ApplyBatchToMemTable(const WriteBatch& batch, MemTable* mem);
  Status NewLogFile();                                // REQUIRES mu_

  // --- recovery ---
  Status ReplayLogs(Lock& l);
  Status WriteLevel0Table(std::shared_ptr<MemTable> mem, VersionEdit* edit, Lock& l);
  void DeleteObsoleteWalFiles();                      // REQUIRES mu_
  void DeleteOrphanFiles();                           // startup only

  // --- background work ---
  void BackgroundThread();
  void MaybeScheduleWork();                           // REQUIRES mu_
  Status CompactMemTable(Lock& l);                    // REQUIRES mu_ (releases during I/O)
  Status BackgroundCompaction(std::unique_ptr<Compaction> c, Lock& l);  // same
  Status DoCompactionWork(Compaction* c, VersionEdit* edit, Lock& l);   // same
  SequenceNumber OldestSnapshot();                    // REQUIRES mu_

  Iterator* NewInternalIterator(SequenceNumber* latest_sequence,
                                std::function<void()>* cleanup);

  const Options options_;
  const std::string dir_;
  const InternalKeyComparator icmp_;
  std::shared_ptr<BlockCache> block_cache_;
  int lock_fd_ = -1;

  std::mutex mu_;
  std::condition_variable bg_cv_;    // signalled when background work completes
  std::condition_variable bg_work_cv_;  // signalled when there is work to do

  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;    // frozen, waiting to be flushed; may be null
  std::unique_ptr<WalWriter> log_;
  uint64_t logfile_number_ = 0;
  std::unique_ptr<VersionSet> versions_;

  std::deque<Writer*> writers_;
  WriteBatch tmp_batch_;                 // scratch space for group commit
  std::list<SnapshotImpl*> snapshots_;   // oldest first

  std::thread bg_thread_;
  bool shutting_down_ = false;
  bool bg_busy_ = false;
  Status bg_error_;                      // sticky: a failed flush poisons the DB
  uint64_t compactions_run_ = 0;
  uint64_t memtable_flushes_ = 0;
  uint64_t bytes_compacted_ = 0;
};

}  // namespace epica
