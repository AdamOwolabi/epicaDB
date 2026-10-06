// env.cc -- POSIX implementations for env.h.

#include "env.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace epica {

std::string ErrnoMessage(const std::string& what) { return what + ": " + std::strerror(errno); }

Status FsyncFile(int fd) {
#ifdef __APPLE__
  if (::fcntl(fd, F_FULLFSYNC) == 0) return Status::Ok();
  if (::fsync(fd) != 0) return Status::IOError(ErrnoMessage("fsync"));
  return Status::Ok();
#else
  if (::fdatasync(fd) != 0) return Status::IOError(ErrnoMessage("fdatasync"));
  return Status::Ok();
#endif
}

Status SyncDirectory(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0) return Status::IOError(ErrnoMessage("open dir " + dir));
  Status s = Status::Ok();
  if (::fsync(fd) != 0) s = Status::IOError(ErrnoMessage("fsync dir " + dir));
  ::close(fd);
  return s;
}

Status CreateDirIfMissing(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST) return Status::Ok();
  return Status::IOError(ErrnoMessage("mkdir " + dir));
}

bool FileExists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

Status GetFileSize(const std::string& path, uint64_t* size) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return Status::IOError(ErrnoMessage("stat " + path));
  *size = static_cast<uint64_t>(st.st_size);
  return Status::Ok();
}

Status DeleteFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0) return Status::IOError(ErrnoMessage("unlink " + path));
  return Status::Ok();
}

Status ListDirectory(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return Status::IOError(ErrnoMessage("opendir " + dir));
  while (struct dirent* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
    names->emplace_back(e->d_name);
  }
  ::closedir(d);
  return Status::Ok();
}

Status ReadFileToString(const std::string& path, std::string* out) {
  std::unique_ptr<RandomAccessFile> f;
  Status s = RandomAccessFile::Open(path, &f);
  if (!s.ok()) return s;
  return f->Read(0, static_cast<size_t>(f->size()), out);
}

Status WriteFileAtomically(const std::string& path, Slice data) {
  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return Status::IOError(ErrnoMessage("open " + tmp));

  const char* p = data.data();
  size_t n = data.size();
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      Status s = Status::IOError(ErrnoMessage("write " + tmp));
      ::close(fd);
      return s;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  Status s = FsyncFile(fd);  // 1. contents durable
  ::close(fd);
  if (!s.ok()) return s;

  if (::rename(tmp.c_str(), path.c_str()) != 0) {  // 2. atomic swap of names
    return Status::IOError(ErrnoMessage("rename " + tmp));
  }
  // 3. the directory entry pointing at the new inode must be durable too
  const size_t slash = path.find_last_of('/');
  return SyncDirectory(slash == std::string::npos ? "." : path.substr(0, slash));
}

// ---------------------------------------------------------------------------

Status WritableFile::Open(const std::string& path, std::unique_ptr<WritableFile>* out) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return Status::IOError(ErrnoMessage("open " + path));
  out->reset(new WritableFile(path, fd));
  (*out)->buffer_.reserve(64 * 1024);
  return Status::Ok();
}

WritableFile::~WritableFile() { Close(); }

Status WritableFile::Append(Slice data) {
  buffer_.append(data.data(), data.size());
  if (buffer_.size() >= 64 * 1024) return Flush();
  return Status::Ok();
}

Status WritableFile::Flush() {
  if (fd_ < 0) return Status::IOError("file closed: " + path_);
  const char* p = buffer_.data();
  size_t n = buffer_.size();
  while (n > 0) {
    ssize_t w = ::write(fd_, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return Status::IOError(ErrnoMessage("write " + path_));
    }
    p += w;
    n -= static_cast<size_t>(w);
    file_size_ += static_cast<uint64_t>(w);
  }
  buffer_.clear();
  return Status::Ok();
}

Status WritableFile::Sync() {
  Status s = Flush();
  if (!s.ok()) return s;
  return FsyncFile(fd_);
}

Status WritableFile::Close() {
  if (fd_ < 0) return Status::Ok();
  Status s = Sync();
  if (::close(fd_) != 0 && s.ok()) s = Status::IOError(ErrnoMessage("close " + path_));
  fd_ = -1;
  return s;
}

// ---------------------------------------------------------------------------

Status RandomAccessFile::Open(const std::string& path, std::unique_ptr<RandomAccessFile>* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Status::IOError(ErrnoMessage("open " + path));
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    Status s = Status::IOError(ErrnoMessage("fstat " + path));
    ::close(fd);
    return s;
  }
  out->reset(new RandomAccessFile(path, fd, static_cast<uint64_t>(st.st_size)));
  return Status::Ok();
}

RandomAccessFile::~RandomAccessFile() {
  if (fd_ >= 0) ::close(fd_);
}

Status RandomAccessFile::Read(uint64_t offset, size_t n, std::string* out) const {
  out->resize(n);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::pread(fd_, out->data() + got, n - got, static_cast<off_t>(offset + got));
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::IOError(ErrnoMessage("pread " + path_));
    }
    if (r == 0) return Status::Corruption("short read in " + path_ + " (truncated file?)");
    got += static_cast<size_t>(r);
  }
  return Status::Ok();
}

}  // namespace epica
