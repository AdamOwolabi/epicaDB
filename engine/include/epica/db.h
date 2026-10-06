// db.h -- the public face of the epicaDB storage engine.
//
// Usage:
//
//   epica::DB* db;
//   epica::Status s = epica::DB::Open(epica::Options{}, "/tmp/mydb", &db);
//   s = db->Put(epica::WriteOptions{}, "k", "v");
//   std::string v;
//   s = db->Get(epica::ReadOptions{}, "k", &v);     // v == "v"
//   s = db->Delete(epica::WriteOptions{}, "k");
//   s = db->Get(epica::ReadOptions{}, "k", &v);     // s.IsNotFound()
//   delete db;                                     // flushes WAL, joins threads
//
// Everything is thread-safe: any number of threads may call any method on
// the same DB concurrently. Writers are batched into a single WAL append
// (group commit); readers never block on writers.
//
// Snapshots give a consistent point-in-time view:
//
//   const Snapshot* snap = db->GetSnapshot();
//   db->Put(wo, "k", "new");
//   ReadOptions ro; ro.snapshot = snap;
//   db->Get(ro, "k", &v);   // still sees the value from before the Put
//   db->ReleaseSnapshot(snap);
//
// Range scans:
//
//   std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions{}));
//   for (it->Seek("user:"); it->Valid() && it->key().view().starts_with("user:"); it->Next())
//     ...
//
// The implementation lives in src/db_impl.{h,cc}; this header is the stable
// interface that the network server and tools compile against.
#pragma once

#include <memory>
#include <string>

#include "epica/iterator.h"
#include "epica/options.h"
#include "epica/slice.h"
#include "epica/status.h"
#include "epica/write_batch.h"

namespace epica {

// Opaque handle to a point-in-time view. Obtain with DB::GetSnapshot, release
// with DB::ReleaseSnapshot. While a snapshot is alive, compaction keeps every
// version of every key that the snapshot could observe.
class Snapshot {
 public:
  virtual ~Snapshot() = default;
};

class DB {
 public:
  // Opens (or creates) the database at `dir`. On success *out is a heap DB
  // the caller owns. Recovery -- MANIFEST load and WAL replay -- happens
  // inside Open, so a returned DB is fully consistent.
  static Status Open(const Options& options, const std::string& dir, DB** out);

  DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  virtual ~DB() = default;

  virtual Status Put(const WriteOptions& opts, Slice key, Slice value) = 0;
  virtual Status Delete(const WriteOptions& opts, Slice key) = 0;

  // Applies the batch atomically.
  virtual Status Write(const WriteOptions& opts, WriteBatch* batch) = 0;

  // Returns NotFound if the key does not exist (or was deleted).
  virtual Status Get(const ReadOptions& opts, Slice key, std::string* value) = 0;

  // Caller owns the iterator and must destroy it before the DB. The iterator
  // observes an implicit snapshot taken at creation time.
  virtual Iterator* NewIterator(const ReadOptions& opts) = 0;

  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // Forces the active memtable to disk and waits for it. Mostly for tests
  // and tools; the engine flushes on its own when memtable_size is reached.
  virtual Status Flush() = 0;

  // Runs compactions until no level is over its threshold, then returns.
  // Test/tool helper.
  virtual Status CompactAll() = 0;

  // Human-readable summary: memtable bytes, files and bytes per level,
  // sequence number, cache stats.
  virtual std::string GetStats() = 0;
};

}  // namespace epica
