// env.h -- thin wrappers over POSIX file I/O used by the SSTable, MANIFEST
// and DB layers. (The WAL has its own equivalents inside wal.cc because it
// predates this file; both use the same fsync discipline.)
//
// The two durability primitives the rest of the engine leans on:
//
//   * FsyncFile(fd): F_FULLFSYNC on macOS (fsync alone does not force the
//     drive's write cache), fdatasync on Linux.
//   * SyncDirectory(dir): fsync the *directory* so a newly created or
//     renamed file name is itself durable. Creating a file and fsyncing its
//     contents is not enough -- the directory entry lives in a different
//     block.
//
// WriteFileAtomically implements the classic "write tmp, fsync, rename,
// fsync dir" recipe, which is how the MANIFEST is replaced without a window
// where it is half-written.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "epica/slice.h"
#include "epica/status.h"

namespace epica {

std::string ErrnoMessage(const std::string& what);

Status FsyncFile(int fd);
Status SyncDirectory(const std::string& dir);
Status CreateDirIfMissing(const std::string& dir);
bool FileExists(const std::string& path);
Status GetFileSize(const std::string& path, uint64_t* size);
Status DeleteFile(const std::string& path);
Status ListDirectory(const std::string& dir, std::vector<std::string>* names);
Status ReadFileToString(const std::string& path, std::string* out);

// Writes `data` to `path` such that a crash at any point leaves either the
// old file or the complete new file, never a mix.
Status WriteFileAtomically(const std::string& path, Slice data);

// Sequential writer for building an SSTable. Buffers in user space and
// issues write(2) when the buffer fills or on Flush().
class WritableFile {
 public:
  static Status Open(const std::string& path, std::unique_ptr<WritableFile>* out);
  ~WritableFile();
  Status Append(Slice data);
  Status Flush();
  Status Sync();   // Flush + FsyncFile
  Status Close();  // Sync + close(2). Idempotent.
  uint64_t size() const { return file_size_ + buffer_.size(); }

 private:
  WritableFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  std::string path_;
  int fd_;
  uint64_t file_size_ = 0;
  std::string buffer_;
};

// Positional reader (pread) for SSTables. Many threads may Read at once;
// pread carries its own offset so there is no shared file position to race
// on.
class RandomAccessFile {
 public:
  static Status Open(const std::string& path, std::unique_ptr<RandomAccessFile>* out);
  ~RandomAccessFile();
  // Reads exactly n bytes at offset into *out (resized). Short reads at EOF
  // return Corruption because every read is for a region the footer/index
  // told us exists.
  Status Read(uint64_t offset, size_t n, std::string* out) const;
  uint64_t size() const { return size_; }

 private:
  RandomAccessFile(std::string path, int fd, uint64_t size)
      : path_(std::move(path)), fd_(fd), size_(size) {}
  std::string path_;
  int fd_;
  uint64_t size_;
};

}  // namespace epica
