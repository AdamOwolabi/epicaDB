// wal.cc -- write-ahead log writer, reader and recovery. See wal.h for the
// record format and the durability contract. Milestone 1 of the engine.

#include "epica/wal.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "crc32c.h"

namespace epica {
namespace {

std::string Errno(const std::string& what) { return what + ": " + std::strerror(errno); }

void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  buf[0] = static_cast<char>(v & 0xff);
  buf[1] = static_cast<char>((v >> 8) & 0xff);
  buf[2] = static_cast<char>((v >> 16) & 0xff);
  buf[3] = static_cast<char>((v >> 24) & 0xff);
  dst->append(buf, 4);
}

uint32_t DecodeFixed32(const char* p) {
  const auto* b = reinterpret_cast<const unsigned char*>(p);
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

bool IsValidType(uint8_t t) {
  return t == static_cast<uint8_t>(RecordType::kPut) ||
         t == static_cast<uint8_t>(RecordType::kDelete) ||
         t == static_cast<uint8_t>(RecordType::kBatch);
}

Status FsyncFd(int fd) {
#ifdef __APPLE__
  // On macOS fsync() alone does not force the drive cache; F_FULLFSYNC does.
  if (::fcntl(fd, F_FULLFSYNC) == 0) return Status::Ok();
  // Fall back to fsync on filesystems that reject F_FULLFSYNC (e.g. some
  // network mounts).
  if (::fsync(fd) != 0) return Status::IOError(Errno("fsync"));
  return Status::Ok();
#else
  if (::fdatasync(fd) != 0) return Status::IOError(Errno("fdatasync"));
  return Status::Ok();
#endif
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0) return Status::IOError(Errno("open dir " + dir));
  Status s = Status::Ok();
  if (::fsync(fd) != 0) s = Status::IOError(Errno("fsync dir " + dir));
  ::close(fd);
  return s;
}

std::string WalDir(const std::string& dir) { return dir + "/wal"; }

Status EnsureDir(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) == 0) return Status::Ok();
  if (errno == EEXIST) return Status::Ok();
  return Status::IOError(Errno("mkdir " + path));
}

}  // namespace

// ---------------------------------------------------------------------------
// File names

std::string WalFileName(const std::string& dir, uint64_t seq) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%012llu.log", static_cast<unsigned long long>(seq));
  return WalDir(dir) + "/" + buf;
}

bool ParseWalFileName(const std::string& name, uint64_t* seq) {
  const std::string suffix = ".log";
  if (name.size() <= suffix.size()) return false;
  if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
  uint64_t v = 0;
  for (size_t i = 0; i + suffix.size() < name.size(); ++i) {
    char c = name[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  *seq = v;
  return true;
}

// ---------------------------------------------------------------------------
// WalWriter

WalWriter::WalWriter(std::string path, uint64_t seq, int fd, uint64_t initial_size, WalOptions opts)
    : path_(std::move(path)), seq_(seq), fd_(fd), file_size_(initial_size), opts_(std::move(opts)) {
  buffer_.reserve(opts_.buffer_size);
}

WalWriter::~WalWriter() { Close(); }

Status WalWriter::Open(const std::string& dir, uint64_t seq, const WalOptions& opts,
                       std::unique_ptr<WalWriter>* out) {
  Status s = EnsureDir(dir);
  if (!s.ok()) return s;
  s = EnsureDir(WalDir(dir));
  if (!s.ok()) return s;

  const std::string path = WalFileName(dir, seq);
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) {
    return Status::InvalidArgument("WAL file already exists and is non-empty: " + path);
  }

  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return Status::IOError(Errno("open " + path));

  // Make the new file name durable.
  s = SyncDir(WalDir(dir));
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  out->reset(new WalWriter(path, seq, fd, 0, opts));
  return Status::Ok();
}

Status WalWriter::Reopen(const std::string& dir, uint64_t seq, const WalOptions& opts,
                         std::unique_ptr<WalWriter>* out) {
  const std::string path = WalFileName(dir, seq);
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return Status::IOError(Errno("stat " + path));
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd < 0) return Status::IOError(Errno("open " + path));
  out->reset(new WalWriter(path, seq, fd, static_cast<uint64_t>(st.st_size), opts));
  return Status::Ok();
}

Status WalWriter::WriteAll(const char* data, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd_, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return Status::IOError(Errno("write " + path_));
    }
    data += w;
    n -= static_cast<size_t>(w);
    file_size_ += static_cast<uint64_t>(w);
  }
  return Status::Ok();
}

Status WalWriter::Append(RecordType type, Slice payload) {
  if (fd_ < 0) return Status::IOError("WAL writer is closed: " + path_);
  if (payload.size() > kWalMaxPayload) {
    return Status::InvalidArgument("WAL payload exceeds kWalMaxPayload");
  }

  const char type_byte = static_cast<char>(type);
  uint32_t crc = crc32c::Extend(0, &type_byte, 1);
  crc = crc32c::Extend(crc, payload.data(), payload.size());

  PutFixed32(&buffer_, crc);
  PutFixed32(&buffer_, static_cast<uint32_t>(payload.size()));
  buffer_.push_back(type_byte);
  buffer_.append(payload.data(), payload.size());

  if (opts_.sync_on_append) return Sync();
  if (buffer_.size() >= opts_.buffer_size) return Flush();
  return Status::Ok();
}

Status WalWriter::Flush() {
  if (fd_ < 0) return Status::IOError("WAL writer is closed: " + path_);
  if (buffer_.empty()) return Status::Ok();
  Status s = WriteAll(buffer_.data(), buffer_.size());
  buffer_.clear();
  return s;
}

Status WalWriter::Sync() {
  Status s = Flush();
  if (!s.ok()) return s;
  return FsyncFd(fd_);
}

Status WalWriter::Close() {
  if (fd_ < 0) return Status::Ok();
  Status s = Sync();
  if (::close(fd_) != 0 && s.ok()) s = Status::IOError(Errno("close " + path_));
  fd_ = -1;
  return s;
}

// ---------------------------------------------------------------------------
// WalReader

WalReader::WalReader(std::string path, int fd, uint64_t file_size)
    : path_(std::move(path)), fd_(fd), file_size_(file_size) {}

WalReader::~WalReader() {
  if (fd_ >= 0) ::close(fd_);
}

Status WalReader::Open(const std::string& path, std::unique_ptr<WalReader>* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Status::IOError(Errno("open " + path));
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    Status s = Status::IOError(Errno("fstat " + path));
    ::close(fd);
    return s;
  }
  out->reset(new WalReader(path, fd, static_cast<uint64_t>(st.st_size)));
  return Status::Ok();
}

bool WalReader::ReadFully(char* dst, size_t n, size_t* got) {
  *got = 0;
  while (*got < n) {
    ssize_t r = ::pread(fd_, dst + *got, n - *got, static_cast<off_t>(offset_ + *got));
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) break;  // EOF
    *got += static_cast<size_t>(r);
  }
  return true;
}

bool WalReader::ReadRecord(RecordType* type, std::string* payload, Status* status) {
  *status = Status::Ok();
  if (failed_) {
    *status = Status::Corruption("reader already hit corruption in " + path_);
    return false;
  }
  if (offset_ >= file_size_) return false;  // clean EOF

  auto fail = [&](const std::string& why) {
    failed_ = true;
    *status = Status::Corruption(path_ + " @" + std::to_string(offset_) + ": " + why);
    return false;
  };

  const uint64_t remaining = file_size_ - offset_;
  if (remaining < kWalHeaderSize) return fail("truncated header");

  char hdr[kWalHeaderSize];
  size_t got = 0;
  if (!ReadFully(hdr, kWalHeaderSize, &got)) {
    *status = Status::IOError(Errno("pread " + path_));
    failed_ = true;
    return false;
  }
  if (got != kWalHeaderSize) return fail("short header read");

  const uint32_t expected_crc = DecodeFixed32(hdr);
  const uint32_t length = DecodeFixed32(hdr + 4);
  const uint8_t type_byte = static_cast<uint8_t>(hdr[8]);

  if (length > kWalMaxPayload) return fail("payload length " + std::to_string(length) + " too large");
  if (remaining - kWalHeaderSize < length) return fail("truncated payload");
  if (!IsValidType(type_byte)) return fail("unknown record type " + std::to_string(type_byte));

  payload->resize(length);
  if (length > 0) {
    // Temporarily advance offset_ for ReadFully's pread positioning.
    const uint64_t saved = offset_;
    offset_ += kWalHeaderSize;
    bool ok = ReadFully(payload->data(), length, &got);
    offset_ = saved;
    if (!ok) {
      *status = Status::IOError(Errno("pread " + path_));
      failed_ = true;
      return false;
    }
    if (got != length) return fail("short payload read");
  }

  const char tb = static_cast<char>(type_byte);
  uint32_t actual = crc32c::Extend(0, &tb, 1);
  actual = crc32c::Extend(actual, payload->data(), payload->size());
  if (actual != expected_crc) return fail("crc mismatch");

  *type = static_cast<RecordType>(type_byte);
  offset_ += kWalHeaderSize + length;
  return true;
}

// ---------------------------------------------------------------------------
// Recovery

Status ListWalFiles(const std::string& dir, std::vector<uint64_t>* seqs) {
  seqs->clear();
  const std::string wal_dir = WalDir(dir);
  DIR* d = ::opendir(wal_dir.c_str());
  if (d == nullptr) {
    if (errno == ENOENT) return Status::Ok();  // no wal/ directory yet
    return Status::IOError(Errno("opendir " + wal_dir));
  }
  while (struct dirent* e = ::readdir(d)) {
    uint64_t seq;
    if (ParseWalFileName(e->d_name, &seq)) seqs->push_back(seq);
  }
  ::closedir(d);
  std::sort(seqs->begin(), seqs->end());
  return Status::Ok();
}

Status RecoverWal(const std::string& dir, const WalRecordFn& fn, WalRecoveryInfo* info,
                  uint64_t min_seq) {
  WalRecoveryInfo local;
  if (info == nullptr) info = &local;
  *info = WalRecoveryInfo{};

  std::vector<uint64_t> all;
  Status ls = ListWalFiles(dir, &all);
  if (!ls.ok()) return ls;
  // Drop logs the MANIFEST already accounts for (their memtables were flushed).
  std::vector<uint64_t> seqs;
  for (uint64_t s : all) {
    if (s >= min_seq) seqs.push_back(s);
  }

  for (size_t i = 0; i < seqs.size(); ++i) {
    const bool is_last = (i + 1 == seqs.size());
    const std::string path = WalFileName(dir, seqs[i]);

    std::unique_ptr<WalReader> reader;
    Status s = WalReader::Open(path, &reader);
    if (!s.ok()) return s;

    RecordType type;
    std::string payload;
    Status rs;
    while (reader->ReadRecord(&type, &payload, &rs)) {
      fn(type, Slice(payload));
      info->records_replayed++;
    }
    info->files_replayed++;
    info->last_seq = seqs[i];

    if (rs.ok()) continue;  // clean EOF
    if (rs.IsIOError()) return rs;
    if (!is_last) {
      // A corrupt record in a fully-written, non-final log is not a torn
      // write. Refuse to silently drop data.
      return rs;
    }

    // Torn tail of the last log: truncate at the first bad record.
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return Status::IOError(Errno("stat " + path));
    const uint64_t keep = reader->offset();
    reader.reset();  // close the read fd before truncating
    if (static_cast<uint64_t>(st.st_size) > keep) {
      info->truncated_bytes = static_cast<uint64_t>(st.st_size) - keep;
      if (::truncate(path.c_str(), static_cast<off_t>(keep)) != 0) {
        return Status::IOError(Errno("truncate " + path));
      }
      int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
      if (fd >= 0) {
        FsyncFd(fd);
        ::close(fd);
      }
      std::fprintf(stderr, "epica: WAL recovery truncated %llu bytes from %s (%s)\n",
                   static_cast<unsigned long long>(info->truncated_bytes), path.c_str(),
                   rs.message().c_str());
    }
  }
  return Status::Ok();
}

}  // namespace epica
