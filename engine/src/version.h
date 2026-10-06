// version.h -- "which SSTable files exist, at which level?" and how that
// answer is persisted (the MANIFEST) and changed (flush and compaction).
//
// Vocabulary:
//   FileMetaData  one SSTable: number, size, smallest/largest internal key.
//                 Also lazily owns the opened Table and deletes the file
//                 from disk when the last reference drops after it became
//                 obsolete (shared_ptr does the reference counting).
//   Version       an immutable snapshot of the per-level file lists. Readers
//                 grab a shared_ptr<Version> and read without locks; a new
//                 Version is installed atomically after every flush or
//                 compaction. An iterator holding an old Version keeps its
//                 files alive even after they are compacted away.
//   VersionEdit   a delta: files added, files removed, new log number.
//   VersionSet    owns the current Version, the counters (next file number,
//                 last sequence, log number), MANIFEST I/O, and the logic
//                 that decides what to compact next.
//
// Level invariants:
//   L0  files may overlap each other (each is one flushed memtable). Sorted
//       newest-first so Get checks the freshest data first.
//   L1+ files within a level have disjoint key ranges, sorted by key. A Get
//       binary-searches for the single file that could contain the key.
//
// MANIFEST format (rewritten atomically on every change, see env.h):
//   | fixed32 crc32c | fixed32 body_len | body |
//   body = | varint64 next_file_number | varint64 last_sequence | varint64 log_number
//          | varint32 num_levels | per level: varint32 count, then per file:
//            varint64 number | varint64 size | lenprefixed smallest | lenprefixed largest |
#pragma once

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cache.h"
#include "epica/iterator.h"
#include "epica/options.h"
#include "epica/status.h"
#include "internal_key.h"
#include "table.h"

namespace epica {

class FileMetaData {
 public:
  FileMetaData(const Options& options, std::string dir, uint64_t number, uint64_t file_size,
               std::string smallest, std::string largest, std::shared_ptr<BlockCache> cache);
  ~FileMetaData();  // unlinks the file if obsolete_

  uint64_t number() const { return number_; }
  uint64_t file_size() const { return file_size_; }
  // Internal keys.
  Slice smallest() const { return Slice(smallest_); }
  Slice largest() const { return Slice(largest_); }
  Slice smallest_user_key() const { return ExtractUserKey(Slice(smallest_)); }
  Slice largest_user_key() const { return ExtractUserKey(Slice(largest_)); }

  // Opens the table on first use (thread-safe). Tables stay open while the
  // FileMetaData lives; the OS fd is the only resource.
  Status GetTable(Table** out);
  Iterator* NewIterator();

  void MarkObsolete() { obsolete_ = true; }

 private:
  Options options_;
  std::string path_;
  uint64_t number_;
  uint64_t file_size_;
  std::string smallest_, largest_;
  std::shared_ptr<BlockCache> cache_;
  std::mutex open_mu_;
  std::unique_ptr<Table> table_;
  Status open_status_;
  bool obsolete_ = false;
};

using FileList = std::vector<std::shared_ptr<FileMetaData>>;

class Version {
 public:
  explicit Version(int num_levels) : files_(static_cast<size_t>(num_levels)) {}

  const FileList& files(int level) const { return files_[static_cast<size_t>(level)]; }
  int num_levels() const { return static_cast<int>(files_.size()); }

  // Point lookup across all levels. Same contract as MemTable::Get:
  //   Ok       -> found, *value set
  //   NotFound -> tombstone found, or key in no file
  Status Get(const LookupKey& key, std::string* value) const;

  // One iterator per L0 file, one per file in deeper levels (they do not
  // overlap, but a per-file iterator keeps the code uniform).
  void AddIterators(std::vector<Iterator*>* out) const;

  // Files in `level` whose user-key range intersects [begin, end].
  void GetOverlappingInputs(int level, Slice begin_user_key, Slice end_user_key,
                            FileList* out) const;

  int64_t TotalBytes(int level) const;
  std::string DebugString() const;

 private:
  friend class VersionSet;
  std::vector<FileList> files_;
};

struct VersionEdit {
  bool has_log_number = false;
  uint64_t log_number = 0;
  std::vector<std::pair<int, std::shared_ptr<FileMetaData>>> new_files;
  std::set<std::pair<int, uint64_t>> deleted_files;  // (level, number)

  void SetLogNumber(uint64_t n) {
    has_log_number = true;
    log_number = n;
  }
  void AddFile(int level, std::shared_ptr<FileMetaData> f) { new_files.emplace_back(level, std::move(f)); }
  void RemoveFile(int level, uint64_t number) { deleted_files.insert({level, number}); }
};

// Describes one compaction job picked by VersionSet::PickCompaction.
class Compaction {
 public:
  int level() const { return level_; }
  // inputs[0] = files from level(), inputs[1] = overlapping files from level()+1.
  const FileList& inputs(int which) const { return inputs_[which]; }
  int num_input_files(int which) const { return static_cast<int>(inputs_[which].size()); }

  // True if the single input file can simply be moved to the next level
  // because nothing there overlaps it. Saves a full rewrite.
  bool IsTrivialMove() const;

  // True if no level deeper than level()+1 contains user_key. Then a
  // tombstone for that key can be dropped: there is no older value it needs
  // to keep hiding.
  bool IsBaseLevelForKey(Slice user_key);

  // Records the deletion of every input file into *edit.
  void AddInputDeletions(VersionEdit* edit) const;

 private:
  friend class VersionSet;
  Compaction(int level, std::shared_ptr<Version> input_version)
      : level_(level), input_version_(std::move(input_version)) {}

  int level_;
  std::shared_ptr<Version> input_version_;
  FileList inputs_[2];
  // Per deeper level, index of the first file not yet ruled out for
  // IsBaseLevelForKey (keys arrive in sorted order, so this only advances).
  std::vector<size_t> level_ptrs_;
};

class VersionSet {
 public:
  VersionSet(std::string dir, const Options& options, std::shared_ptr<BlockCache> cache);

  // Loads MANIFEST if present. *fresh is set to true if there was none.
  Status Recover(bool* fresh);

  // Applies edit to current(), writes the new MANIFEST atomically, installs
  // the new Version, and marks removed files obsolete. REQUIRES: the DB
  // mutex is held (only one LogAndApply at a time).
  Status LogAndApply(VersionEdit* edit);

  std::shared_ptr<Version> current() const { return current_; }

  uint64_t NewFileNumber() { return next_file_number_++; }
  // After a crash the MANIFEST's counter may lag behind file numbers that
  // were handed out but never recorded (e.g. a WAL opened after the last
  // flush). Recovery calls this for every file it sees on disk.
  void MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) next_file_number_ = number + 1;
  }
  uint64_t LastSequence() const { return last_sequence_; }
  void SetLastSequence(uint64_t s) { last_sequence_ = s; }
  uint64_t LogNumber() const { return log_number_; }
  uint64_t NextFileNumber() const { return next_file_number_; }

  // Returns nullptr if no level is over its threshold.
  std::unique_ptr<Compaction> PickCompaction();
  // Level with the highest score >= 1, or -1 if nothing needs compacting.
  int PickLevelToCompact() const;
  bool NeedsCompaction() const { return PickLevelToCompact() >= 0; }
  // Compaction of every level with any data, used by DB::CompactAll.
  std::unique_ptr<Compaction> PickCompactionForLevel(int level);

  uint64_t MaxBytesForLevel(int level) const;
  int NumLevelFiles(int level) const { return static_cast<int>(current_->files(level).size()); }

  // Every file number referenced by the current version.
  void AddLiveFiles(std::set<uint64_t>* live) const;

  std::string LevelSummary() const;

 private:
  Status WriteManifest();
  Status ReadManifest(const std::string& contents);
  void SetupOtherInputs(Compaction* c);
  void SortLevel(FileList* files, int level) const;

  std::string dir_;
  Options options_;
  std::shared_ptr<BlockCache> cache_;
  InternalKeyComparator icmp_;

  std::shared_ptr<Version> current_;
  uint64_t next_file_number_ = 2;  // 1 is reserved for the first WAL
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;

  // Per level, the largest user key compacted so far. The next compaction at
  // that level starts after it, so compaction sweeps the key space instead
  // of hammering one hot range.
  std::vector<std::string> compact_pointer_;
};

// Reads every (internal_key, value) from `iter`, writes an SSTable numbered
// `number`, fsyncs it, and returns its metadata. If the iterator is empty no
// file is created and *meta is left null.
Status BuildTable(const Options& options, const std::string& dir, uint64_t number, Iterator* iter,
                  std::shared_ptr<BlockCache> cache, std::shared_ptr<FileMetaData>* meta);

}  // namespace epica
