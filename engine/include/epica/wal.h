#pragma once

// Write-ahead log for the epicaDB storage engine.
//
// Every mutation is appended here and fsync'd BEFORE it touches the memtable.
// If the process dies, Open() replays the log and nothing acknowledged is
// lost. This is the "durability" in ACID.
//
// On-disk record format (all integers little-endian):
//
//   | crc32c (4) | length (4) | type (1) | payload (length bytes) |
//
// crc32c covers `type` + `payload`. Records are laid down contiguously in
// files named <dir>/wal/<seq>.log, where seq is a zero-padded, monotonically
// increasing number. Block-aligned framing (for multi-block storage) is a
// later milestone; WalWriter is the seam where that changes.
//
// Threading: WalWriter and WalReader are single-threaded. The engine layer
// owns any locking. This will be revisited in the concurrency milestone.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "epica/slice.h"
#include "epica/status.h"

namespace epica {

enum class RecordType : uint8_t {
  kPut = 1,     // payload = one raw put (used by tests/tools)
  kDelete = 2,  // payload = one raw delete (used by tests/tools)
  kBatch = 3,   // payload = an encoded WriteBatch (what the engine writes)
};

// Size of the fixed header preceding every record.
constexpr size_t kWalHeaderSize = 4 + 4 + 1;

// Upper bound on a single record's payload. Protects the reader from
// allocating based on a corrupt length field.
constexpr uint32_t kWalMaxPayload = 64u * 1024u * 1024u;  // 64 MiB

struct WalOptions {
  // If true, every Append flushes and fdatasyncs before returning. Slow, but
  // the strongest guarantee. The engine will normally leave this false and
  // call Sync() itself (group commit).
  bool sync_on_append = false;

  // Bytes buffered in user space before a write(2) is issued.
  size_t buffer_size = 64 * 1024;
};

// Returns the path of the log file with the given sequence number.
std::string WalFileName(const std::string& dir, uint64_t seq);

// Parses "<seq>.log". Returns false if the name is not a WAL file name.
bool ParseWalFileName(const std::string& name, uint64_t* seq);

class WalWriter {
 public:
  // Creates <dir>/wal/ if needed and opens a fresh log file with the given
  // sequence number. Fails with InvalidArgument if the file already exists
  // and is non-empty; use Reopen for that.
  static Status Open(const std::string& dir, uint64_t seq, const WalOptions& opts,
                     std::unique_ptr<WalWriter>* out);
  static Status Open(const std::string& dir, uint64_t seq, std::unique_ptr<WalWriter>* out) {
    return Open(dir, seq, WalOptions{}, out);
  }

  // Opens an existing log file for appending. Used by recovery after the
  // torn tail of the last log has been truncated.
  static Status Reopen(const std::string& dir, uint64_t seq, const WalOptions& opts,
                       std::unique_ptr<WalWriter>* out);

  ~WalWriter();
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Appends one record. Buffered unless sync_on_append is set.
  Status Append(RecordType type, Slice payload);

  // Flushes the user-space buffer to the kernel. No durability guarantee.
  Status Flush();

  // Flushes, then fdatasync/fsync. After this returns Ok, every record
  // appended so far survives a crash or power loss.
  Status Sync();

  // Flushes, syncs, and closes the file descriptor. Idempotent.
  Status Close();

  uint64_t seq() const { return seq_; }
  // Bytes written to the file so far, including buffered bytes.
  uint64_t size() const { return file_size_ + buffer_.size(); }

 private:
  WalWriter(std::string path, uint64_t seq, int fd, uint64_t initial_size, WalOptions opts);
  Status WriteAll(const char* data, size_t n);

  std::string path_;
  uint64_t seq_;
  int fd_;
  uint64_t file_size_;
  WalOptions opts_;
  std::string buffer_;
};

class WalReader {
 public:
  static Status Open(const std::string& path, std::unique_ptr<WalReader>* out);

  ~WalReader();
  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;

  // Reads the next record into *type / *payload.
  //   returns true          -> a record was read; *status is Ok.
  //   returns false, Ok     -> clean end of file.
  //   returns false, !Ok    -> corruption (bad CRC, truncated record, bad
  //                            type, absurd length). offset() points at the
  //                            start of the bad record so the caller can
  //                            truncate there. Further calls keep failing.
  bool ReadRecord(RecordType* type, std::string* payload, Status* status);

  // Byte offset at which the next record would start.
  uint64_t offset() const { return offset_; }

 private:
  WalReader(std::string path, int fd, uint64_t file_size);
  bool ReadFully(char* dst, size_t n, size_t* got);

  std::string path_;
  int fd_;
  uint64_t file_size_;
  uint64_t offset_ = 0;
  bool failed_ = false;
};

using WalRecordFn = std::function<void(RecordType type, Slice payload)>;

struct WalRecoveryInfo {
  uint64_t records_replayed = 0;
  uint64_t files_replayed = 0;
  // Highest sequence number seen; 0 if no logs existed. The engine should
  // open its next writer at last_seq + 1 (or Reopen last_seq).
  uint64_t last_seq = 0;
  // Non-zero if the tail of the last log was truncated during recovery.
  uint64_t truncated_bytes = 0;
};

// Replays every log under <dir>/wal/ with sequence >= min_seq, in ascending
// sequence order, invoking fn for each intact record. Logs below min_seq are
// ones the MANIFEST says were already flushed to SSTables; they are skipped
// (the engine deletes them afterwards). A corrupt record in any replayed file
// except the last is a hard Corruption error. A corrupt record in the last
// file is treated as a torn write: the file is truncated at that record and
// recovery succeeds.
Status RecoverWal(const std::string& dir, const WalRecordFn& fn, WalRecoveryInfo* info = nullptr,
                  uint64_t min_seq = 0);

// Lists the sequence numbers of every log file under <dir>/wal/, sorted.
Status ListWalFiles(const std::string& dir, std::vector<uint64_t>* seqs);

}  // namespace epica
