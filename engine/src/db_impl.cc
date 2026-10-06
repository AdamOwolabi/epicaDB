// db_impl.cc -- see db_impl.h for the architecture overview.
//
// Reading order suggestion:
//   1. DB::Open / DBImpl::Recover / ReplayLogs      (how state is rebuilt)
//   2. DBImpl::Write / BuildBatchGroup / MakeRoomForWrite   (the write path)
//   3. DBImpl::Get / NewIterator                    (the read path)
//   4. BackgroundThread / CompactMemTable / DoCompactionWork (maintenance)

#include "db_impl.h"

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "db_iter.h"
#include "env.h"
#include "filenames.h"
#include "merger.h"

namespace epica {

// One queued writer. Lives on the caller's stack for the duration of Write().
struct DBImpl::Writer {
  WriteBatch* batch = nullptr;
  bool sync = false;
  bool done = false;
  Status status;
  std::condition_variable cv;
};

// ---------------------------------------------------------------------------
// Open / close

Status DB::Open(const Options& options, const std::string& dir, DB** out) {
  *out = nullptr;
  auto* impl = new DBImpl(options, dir);
  Status s = impl->Recover();
  if (!s.ok()) {
    delete impl;
    return s;
  }
  *out = impl;
  return Status::Ok();
}

DBImpl::DBImpl(const Options& options, std::string dir)
    : options_(options),
      dir_(std::move(dir)),
      icmp_(),
      block_cache_(std::make_shared<BlockCache>(options.block_cache_size)),
      mem_(std::make_shared<MemTable>(icmp_)),
      versions_(std::make_unique<VersionSet>(dir_, options_, block_cache_)) {}

DBImpl::~DBImpl() {
  {
    Lock l(mu_);
    shutting_down_ = true;
    bg_work_cv_.notify_all();
  }
  if (bg_thread_.joinable()) bg_thread_.join();

  Lock l(mu_);
  if (log_) log_->Close();  // Sync + close; everything acknowledged is on disk
  for (SnapshotImpl* s : snapshots_) delete s;  // caller leaked them; tidy up
  snapshots_.clear();
  if (lock_fd_ >= 0) {
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
  }
}

// ---------------------------------------------------------------------------
// Recovery

Status DBImpl::Recover() {
  Lock l(mu_);

  if (!FileExists(dir_)) {
    if (!options_.create_if_missing) return Status::InvalidArgument(dir_ + " does not exist");
  }
  Status s = CreateDirIfMissing(dir_);
  if (s.ok()) s = CreateDirIfMissing(SstDirName(dir_));
  if (!s.ok()) return s;

  // Advisory lock: a second process opening the same directory would
  // corrupt it (two WAL writers, two MANIFEST writers). Fail fast instead.
  lock_fd_ = ::open(LockFileName(dir_).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ < 0) return Status::IOError(ErrnoMessage("open LOCK"));
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock_fd_);
    lock_fd_ = -1;
    return Status::IOError("database is locked by another process: " + dir_);
  }

  // 1. MANIFEST: which SSTables exist and where the WAL/sequence counters were.
  bool fresh = false;
  s = versions_->Recover(&fresh);
  if (!s.ok()) return s;

  // Bump the file-number counter past anything already on disk.
  {
    std::vector<uint64_t> wal_seqs;
    if (ListWalFiles(dir_, &wal_seqs).ok()) {
      for (uint64_t n : wal_seqs) versions_->MarkFileNumberUsed(n);
    }
    std::vector<std::string> names;
    if (ListDirectory(SstDirName(dir_), &names).ok()) {
      for (const auto& n : names) {
        uint64_t number;
        if (ParseSstFileName(n, &number)) versions_->MarkFileNumberUsed(number);
      }
    }
  }

  // 2. WAL: everything written after the last flush lives only in the logs.
  s = ReplayLogs(l);
  if (!s.ok()) return s;

  // 3. Fresh WAL for new writes.
  s = NewLogFile();
  if (!s.ok()) return s;

  // 4. Whatever the replay produced goes straight to L0, so the old logs can
  //    be dropped and startup state is "empty memtable + one new log".
  VersionEdit edit;
  if (mem_->num_entries() > 0) {
    s = WriteLevel0Table(mem_, &edit, l);
    if (!s.ok()) return s;
    mem_ = std::make_shared<MemTable>(icmp_);
  }
  edit.SetLogNumber(logfile_number_);
  s = versions_->LogAndApply(&edit);  // also creates the MANIFEST on a fresh DB
  if (!s.ok()) return s;

  DeleteObsoleteWalFiles();
  DeleteOrphanFiles();

  bg_thread_ = std::thread(&DBImpl::BackgroundThread, this);
  MaybeScheduleWork();
  return Status::Ok();
}

// Feeds every WriteBatch record from the surviving logs into the memtable.
Status DBImpl::ReplayLogs(Lock& l) {
  const uint64_t min_log = versions_->LogNumber();
  SequenceNumber max_seq = versions_->LastSequence();
  Status replay_error;
  uint64_t batches = 0;

  auto on_record = [&](RecordType type, Slice payload) {
    if (!replay_error.ok()) return;
    if (type != RecordType::kBatch) {
      replay_error = Status::Corruption("unexpected non-batch WAL record");
      return;
    }
    WriteBatch batch;
    batch.SetContents(payload);
    Status s = ApplyBatchToMemTable(batch, mem_.get());
    if (!s.ok()) {
      replay_error = s;
      return;
    }
    ++batches;
    const SequenceNumber last = batch.Sequence() + batch.Count() - 1;
    if (last > max_seq) max_seq = last;

    // Bound memory during a long replay by flushing as we go.
    if (mem_->ApproximateMemoryUsage() > options_.memtable_size) {
      VersionEdit edit;
      s = WriteLevel0Table(mem_, &edit, l);
      if (s.ok()) s = versions_->LogAndApply(&edit);
      if (!s.ok()) {
        replay_error = s;
        return;
      }
      mem_ = std::make_shared<MemTable>(icmp_);
    }
  };

  WalRecoveryInfo info;
  Status s = RecoverWal(dir_, on_record, &info, min_log);
  if (!s.ok()) return s;
  if (!replay_error.ok()) return replay_error;
  versions_->SetLastSequence(max_seq);
  if (info.records_replayed > 0 || info.truncated_bytes > 0) {
    std::fprintf(stderr, "epica: replayed %llu batches from %llu log(s)%s\n",
                 static_cast<unsigned long long>(batches),
                 static_cast<unsigned long long>(info.files_replayed),
                 info.truncated_bytes > 0 ? " (torn tail truncated)" : "");
  }
  return Status::Ok();
}

Status DBImpl::NewLogFile() {
  const uint64_t number = versions_->NewFileNumber();
  WalOptions wo;
  wo.buffer_size = options_.wal_buffer_size;
  std::unique_ptr<WalWriter> w;
  Status s = WalWriter::Open(dir_, number, wo, &w);
  if (!s.ok()) return s;
  if (log_) {
    s = log_->Close();  // the old log belongs to the frozen memtable now
    if (!s.ok()) return s;
  }
  log_ = std::move(w);
  logfile_number_ = number;
  return Status::Ok();
}

// Builds one SSTable from a memtable and records it in *edit at level 0.
Status DBImpl::WriteLevel0Table(std::shared_ptr<MemTable> mem, VersionEdit* edit, Lock& l) {
  const uint64_t number = versions_->NewFileNumber();
  l.unlock();  // building the table is pure I/O; let readers/writers proceed
  std::unique_ptr<Iterator> it(mem->NewIterator());
  std::shared_ptr<FileMetaData> meta;
  Status s = BuildTable(options_, dir_, number, it.get(), block_cache_, &meta);
  l.lock();
  if (s.ok() && meta) edit->AddFile(0, meta);
  return s;
}

// Logs older than the MANIFEST's log_number only contain data that is
// already in SSTables. Recovery would skip them anyway; reclaim the space.
void DBImpl::DeleteObsoleteWalFiles() {
  std::vector<uint64_t> seqs;
  if (!ListWalFiles(dir_, &seqs).ok()) return;
  for (uint64_t seq : seqs) {
    if (seq < versions_->LogNumber()) DeleteFile(WalFileName(dir_, seq));
  }
}

// Startup only: no iterators exist yet, so any SSTable the MANIFEST does not
// list is garbage from a crash mid-flush/compaction.
void DBImpl::DeleteOrphanFiles() {
  std::set<uint64_t> live;
  versions_->AddLiveFiles(&live);
  std::vector<std::string> names;
  if (!ListDirectory(SstDirName(dir_), &names).ok()) return;
  for (const auto& n : names) {
    uint64_t number;
    if (ParseSstFileName(n, &number) && !live.count(number)) {
      DeleteFile(SstFileName(dir_, number));
    }
  }
  DeleteFile(ManifestFileName(dir_) + ".tmp");  // harmless if absent
}

// ---------------------------------------------------------------------------
// Write path

Status DBImpl::Put(const WriteOptions& opts, Slice key, Slice value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(opts, &batch);
}

Status DBImpl::Delete(const WriteOptions& opts, Slice key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opts, &batch);
}

namespace {
// Applies each op in a batch to the memtable with consecutive sequence
// numbers starting at the batch's sequence.
class MemTableInserter : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber seq, MemTable* mem) : seq_(seq), mem_(mem) {}
  void Put(Slice key, Slice value) override {
    mem_->Add(seq_, kTypeValue, key, value);
    seq_++;
  }
  void Delete(Slice key) override {
    mem_->Add(seq_, kTypeDeletion, key, Slice());
    seq_++;
  }

 private:
  SequenceNumber seq_;
  MemTable* mem_;
};
}  // namespace

Status DBImpl::ApplyBatchToMemTable(const WriteBatch& batch, MemTable* mem) {
  MemTableInserter inserter(batch.Sequence(), mem);
  return batch.Iterate(&inserter);
}

// Group commit. Every caller queues a Writer; the one at the head of the
// queue ("leader") gathers as many followers as fit, writes them as ONE WAL
// record with ONE fsync, applies them to the memtable, then wakes everyone.
// Under contention this amortises the ~5 ms fsync across many writes.
Status DBImpl::Write(const WriteOptions& opts, WriteBatch* batch) {
  if (batch->Count() == 0) return Status::Ok();

  Writer w;
  w.batch = batch;
  w.sync = opts.sync;

  Lock l(mu_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) w.cv.wait(l);
  if (w.done) return w.status;  // a leader already committed us

  // We are the leader.
  Status status = MakeRoomForWrite(l);
  SequenceNumber last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (status.ok()) {
    WriteBatch* updates = BuildBatchGroup(&last_writer);
    updates->SetSequence(last_sequence + 1);
    last_sequence += updates->Count();
    MemTable* mem = mem_.get();  // only the leader may swap mem_, and that's us

    // Disk I/O without the mutex: other threads can read, and more writers
    // can queue up behind us (they will form the next group).
    l.unlock();
    status = log_->Append(RecordType::kBatch, updates->Contents());  // 1. log it
    bool sync_error = false;
    if (status.ok() && w.sync) {
      status = log_->Sync();                                          // 2. make it durable
      if (!status.ok()) sync_error = true;
    }
    if (status.ok()) status = ApplyBatchToMemTable(*updates, mem);    // 3. then apply
    l.lock();

    if (sync_error) {
      // The WAL may or may not contain the record; the memtable does not.
      // Refuse all further writes so the discrepancy cannot grow.
      bg_error_ = status;
    }
    if (updates == &tmp_batch_) tmp_batch_.Clear();
    if (status.ok()) versions_->SetLastSequence(last_sequence);  // 4. publish
  }

  // Wake every writer in the group we just committed.
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last_writer) break;
  }
  // The next queued writer becomes the new leader.
  if (!writers_.empty()) writers_.front()->cv.notify_one();
  return status;
}

// Merges the leader's batch with as many queued followers as fit under a
// size cap. Returns the leader's own batch if it is alone.
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  size_t size = result->ByteSize();

  // Small writes should not wait behind a huge group; cap the group size,
  // and cap it lower when the leader itself is small (latency matters more
  // than throughput for small writes).
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) max_size = size + (128 << 10);

  *last_writer = first;
  for (auto it = writers_.begin() + 1; it != writers_.end(); ++it) {
    Writer* w = *it;
    // A follower that wants fsync must not ride on a leader that skips it.
    if (w->sync && !first->sync) break;
    size += w->batch->ByteSize();
    if (size > max_size) break;
    if (result == first->batch) {
      result = &tmp_batch_;
      result->Append(*first->batch);
    }
    result->Append(*w->batch);
    *last_writer = w;
  }
  return result;
}

// Ensures the active memtable has room. May rotate the memtable (freeze it
// as imm_, open a new WAL) or block the writer when the engine is behind.
Status DBImpl::MakeRoomForWrite(Lock& l) {
  while (true) {
    if (!bg_error_.ok()) return bg_error_;

    if (mem_->ApproximateMemoryUsage() <= options_.memtable_size) {
      if (versions_->NumLevelFiles(0) >= options_.l0_stop_writes_trigger) {
        // Write stall: too many L0 files. Let compaction catch up.
        MaybeScheduleWork();
        bg_cv_.wait(l);
        continue;
      }
      return Status::Ok();
    }

    if (imm_ != nullptr) {
      // The previous memtable is still being flushed; wait for it.
      MaybeScheduleWork();
      bg_cv_.wait(l);
      continue;
    }

    // Rotate: new WAL first (so the frozen memtable's log is closed and
    // complete), then swap memtables. The flush happens in the background.
    Status s = NewLogFile();
    if (!s.ok()) return s;
    imm_ = mem_;
    mem_ = std::make_shared<MemTable>(icmp_);
    MaybeScheduleWork();
  }
}

// ---------------------------------------------------------------------------
// Read path

Status DBImpl::Get(const ReadOptions& opts, Slice key, std::string* value) {
  SequenceNumber snapshot;
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<Version> current;
  {
    // Grab references under the lock, then read without it. The shared_ptrs
    // keep these structures alive even if a flush/compaction replaces them
    // mid-read.
    Lock l(mu_);
    snapshot = opts.snapshot ? static_cast<const SnapshotImpl*>(opts.snapshot)->sequence
                             : versions_->LastSequence();
    mem = mem_;
    imm = imm_;
    current = versions_->current();
  }

  LookupKey lkey(key, snapshot);
  Status s;
  if (mem->Get(lkey, value, &s)) return s;          // newest data
  if (imm && imm->Get(lkey, value, &s)) return s;   // being flushed
  return current->Get(lkey, value);                 // on disk, L0 then L1..Ln
}

Iterator* DBImpl::NewInternalIterator(SequenceNumber* latest_sequence,
                                      std::function<void()>* cleanup) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<Version> current;
  {
    Lock l(mu_);
    *latest_sequence = versions_->LastSequence();
    mem = mem_;
    imm = imm_;
    current = versions_->current();
  }
  std::vector<Iterator*> children;
  children.push_back(mem->NewIterator());
  if (imm) children.push_back(imm->NewIterator());
  current->AddIterators(&children);
  // The lambda owns the shared_ptrs; it runs when the DBIter is destroyed.
  *cleanup = [mem, imm, current]() {};
  return NewMergingIterator(&icmp_, std::move(children));
}

Iterator* DBImpl::NewIterator(const ReadOptions& opts) {
  SequenceNumber seq;
  std::function<void()> cleanup;
  Iterator* internal = NewInternalIterator(&seq, &cleanup);
  if (opts.snapshot) seq = static_cast<const SnapshotImpl*>(opts.snapshot)->sequence;
  return NewDBIterator(internal, seq, std::move(cleanup));
}

const Snapshot* DBImpl::GetSnapshot() {
  Lock l(mu_);
  auto* s = new SnapshotImpl(versions_->LastSequence());
  snapshots_.push_back(s);
  s->pos = std::prev(snapshots_.end());
  return s;
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) return;
  Lock l(mu_);
  auto* s = const_cast<SnapshotImpl*>(static_cast<const SnapshotImpl*>(snapshot));
  snapshots_.erase(s->pos);
  delete s;
}

SequenceNumber DBImpl::OldestSnapshot() {
  return snapshots_.empty() ? versions_->LastSequence() : snapshots_.front()->sequence;
}

// ---------------------------------------------------------------------------
// Background work

void DBImpl::MaybeScheduleWork() { bg_work_cv_.notify_one(); }

void DBImpl::BackgroundThread() {
  Lock l(mu_);
  while (true) {
    while (!shutting_down_ &&
           (bg_busy_ || (imm_ == nullptr && !versions_->NeedsCompaction()))) {
      bg_work_cv_.wait(l);
    }
    if (shutting_down_) break;

    bg_busy_ = true;
    if (imm_ != nullptr) {
      CompactMemTable(l);  // flushes take priority: writers may be waiting
    } else if (auto c = versions_->PickCompaction()) {
      BackgroundCompaction(std::move(c), l);
    }
    bg_busy_ = false;
    bg_cv_.notify_all();  // wake stalled writers / Flush() / CompactAll()
  }
}

Status DBImpl::CompactMemTable(Lock& l) {
  std::shared_ptr<MemTable> imm = imm_;
  VersionEdit edit;
  Status s = WriteLevel0Table(imm, &edit, l);
  if (s.ok()) {
    // Once this edit is durable, every record in logs older than the current
    // one is covered by an SSTable.
    edit.SetLogNumber(logfile_number_);
    s = versions_->LogAndApply(&edit);
  }
  if (s.ok()) {
    imm_ = nullptr;
    ++memtable_flushes_;
    DeleteObsoleteWalFiles();
  } else {
    bg_error_ = s;
  }
  return s;
}

Status DBImpl::BackgroundCompaction(std::unique_ptr<Compaction> c, Lock& l) {
  Status s;
  VersionEdit edit;
  if (c->IsTrivialMove()) {
    // Nothing in the next level overlaps: just re-label the file.
    const auto& f = c->inputs(0)[0];
    edit.RemoveFile(c->level(), f->number());
    edit.AddFile(c->level() + 1, f);
    s = versions_->LogAndApply(&edit);
  } else {
    s = DoCompactionWork(c.get(), &edit, l);
    if (s.ok()) s = versions_->LogAndApply(&edit);
  }
  if (s.ok()) {
    ++compactions_run_;
  } else {
    bg_error_ = s;
  }
  return s;
}

// Merges inputs from level L and L+1 into new L+1 files, dropping entries
// that no reader could ever observe again:
//   * an older version of a key when a newer version is visible to every
//     live snapshot;
//   * a tombstone, once it is visible to every snapshot AND no deeper level
//     holds an older value it would need to hide.
Status DBImpl::DoCompactionWork(Compaction* c, VersionEdit* edit, Lock& l) {
  const SequenceNumber smallest_snapshot = OldestSnapshot();
  l.unlock();

  std::vector<Iterator*> children;
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : c->inputs(which)) children.push_back(f->NewIterator());
  }
  std::unique_ptr<Iterator> input(NewMergingIterator(&icmp_, std::move(children)));
  input->SeekToFirst();

  struct Output {
    uint64_t number;
    std::unique_ptr<WritableFile> file;
    std::unique_ptr<TableBuilder> builder;
    std::string smallest, largest;
  };
  std::unique_ptr<Output> out;
  std::vector<std::shared_ptr<FileMetaData>> outputs;
  Status s;

  auto finish_output = [&]() {
    s = out->builder->Finish();
    if (s.ok()) s = out->file->Close();
    if (s.ok()) {
      outputs.push_back(std::make_shared<FileMetaData>(options_, dir_, out->number,
                                                       out->builder->FileSize(), out->smallest,
                                                       out->largest, block_cache_));
    }
    out.reset();
  };

  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  uint64_t dropped = 0, kept = 0;

  while (s.ok() && input->Valid()) {
    Slice key = input->key();
    ParsedInternalKey ikey;
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Keep corrupt entries rather than silently losing them.
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key || ikey.user_key != Slice(current_user_key)) {
        // First (= newest) occurrence of this user key.
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }
      if (last_sequence_for_key <= smallest_snapshot) {
        // A newer version of this key is visible to every snapshot, so this
        // older one is unreachable.
        drop = true;
      } else if (ikey.type == kTypeDeletion && ikey.sequence <= smallest_snapshot &&
                 c->IsBaseLevelForKey(ikey.user_key)) {
        // The tombstone has done its job: nothing older exists anywhere.
        drop = true;
      }
      last_sequence_for_key = ikey.sequence;
    }

    if (drop) {
      ++dropped;
    } else {
      ++kept;
      if (!out) {
        l.lock();
        const uint64_t number = versions_->NewFileNumber();
        l.unlock();
        out = std::make_unique<Output>();
        out->number = number;
        s = WritableFile::Open(SstFileName(dir_, number), &out->file);
        if (!s.ok()) break;
        out->builder = std::make_unique<TableBuilder>(options_, out->file.get());
        out->smallest = key.ToString();
      }
      out->builder->Add(key, input->value());
      out->largest.assign(key.data(), key.size());
      if (out->builder->FileSize() >= options_.target_file_size) finish_output();
    }
    input->Next();
  }
  if (s.ok() && out) finish_output();
  if (s.ok()) s = input->status();
  if (s.ok()) s = SyncDirectory(SstDirName(dir_));

  l.lock();
  if (s.ok()) {
    c->AddInputDeletions(edit);
    for (const auto& f : outputs) {
      edit->AddFile(c->level() + 1, f);
      bytes_compacted_ += f->file_size();
    }
    std::fprintf(stderr, "epica: compacted L%d (%d files) + L%d (%d files) -> %zu files, kept %llu, dropped %llu\n",
                 c->level(), c->num_input_files(0), c->level() + 1, c->num_input_files(1),
                 outputs.size(), static_cast<unsigned long long>(kept),
                 static_cast<unsigned long long>(dropped));
  } else {
    if (out) {
      out->file.reset();
      DeleteFile(SstFileName(dir_, out->number));
    }
    for (const auto& f : outputs) DeleteFile(SstFileName(dir_, f->number()));
  }
  return s;
}

// ---------------------------------------------------------------------------
// Maintenance entry points

Status DBImpl::Flush() {
  Lock l(mu_);
  if (mem_->num_entries() > 0) {
    while (imm_ != nullptr && bg_error_.ok()) {
      MaybeScheduleWork();
      bg_cv_.wait(l);
    }
    if (!bg_error_.ok()) return bg_error_;
    Status s = NewLogFile();
    if (!s.ok()) return s;
    imm_ = mem_;
    mem_ = std::make_shared<MemTable>(icmp_);
    MaybeScheduleWork();
  }
  while (imm_ != nullptr && bg_error_.ok()) bg_cv_.wait(l);
  return bg_error_;
}

Status DBImpl::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  Lock l(mu_);
  while (bg_busy_) bg_cv_.wait(l);  // take over the "one job at a time" slot
  bg_busy_ = true;
  for (int level = 0; s.ok() && level + 1 < options_.num_levels; ++level) {
    while (s.ok() && versions_->NumLevelFiles(level) > 0) {
      auto c = versions_->PickCompactionForLevel(level);
      if (!c) break;
      s = BackgroundCompaction(std::move(c), l);
    }
  }
  bg_busy_ = false;
  bg_cv_.notify_all();
  bg_work_cv_.notify_all();
  return s;
}

std::string DBImpl::GetStats() {
  Lock l(mu_);
  std::string r = "epicaDB stats for " + dir_ + "\n";
  r += "  last_sequence:      " + std::to_string(versions_->LastSequence()) + "\n";
  r += "  memtable:           " + std::to_string(mem_->ApproximateMemoryUsage()) + " bytes, " +
       std::to_string(mem_->num_entries()) + " entries\n";
  r += "  immutable memtable: " + std::string(imm_ ? "flushing" : "none") + "\n";
  r += "  wal file:           #" + std::to_string(logfile_number_) + " (" +
       std::to_string(log_ ? log_->size() : 0) + " bytes)\n";
  r += "  live snapshots:     " + std::to_string(snapshots_.size()) + "\n";
  r += "  flushes/compactions:" + std::to_string(memtable_flushes_) + "/" +
       std::to_string(compactions_run_) + " (" + std::to_string(bytes_compacted_) +
       " bytes written by compaction)\n";
  r += "  block cache:        " + std::to_string(block_cache_->usage()) + "/" +
       std::to_string(block_cache_->capacity()) + " bytes, hits " +
       std::to_string(block_cache_->hits()) + ", misses " +
       std::to_string(block_cache_->misses()) + "\n";
  r += "  levels:\n" + versions_->LevelSummary();
  r += versions_->current()->DebugString();
  if (!bg_error_.ok()) r += "  BACKGROUND ERROR: " + bg_error_.ToString() + "\n";
  return r;
}

int DBImpl::NumLevelFiles(int level) {
  Lock l(mu_);
  return versions_->NumLevelFiles(level);
}

uint64_t DBImpl::LastSequence() {
  Lock l(mu_);
  return versions_->LastSequence();
}

}  // namespace epica
