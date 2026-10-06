// net/server.cc -- see server.h.

#include "net/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>

#include "net/protocol.h"

namespace epica::net {

// ---------------------------------------------------------------------------
// Framing helpers

bool ReadExact(int fd, char* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::recv(fd, buf + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;  // peer closed
    got += static_cast<size_t>(r);
  }
  return true;
}

bool WriteAll(int fd, const char* buf, size_t n) {
  while (n > 0) {
    ssize_t w = ::send(fd, buf, n, 0);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    buf += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

bool ReadFrame(int fd, std::string* payload) {
  char hdr[4];
  if (!ReadExact(fd, hdr, 4)) return false;
  Slice in(hdr, 4);
  uint32_t len;
  GetU32(&in, &len);
  if (len == 0 || len > kMaxFrameSize) return false;
  payload->resize(len);
  return ReadExact(fd, payload->data(), len);
}

bool WriteFrame(int fd, Slice payload) {
  std::string f = Frame(payload);
  return WriteAll(fd, f.data(), f.size());
}

// ---------------------------------------------------------------------------
// Server

Server::Server(DB* db, uint16_t port, std::string bind_addr)
    : db_(db), port_(port), bind_addr_(std::move(bind_addr)) {}

Server::~Server() { Stop(); }

Status Server::Start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return Status::IOError(std::string("socket: ") + std::strerror(errno));
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
    return Status::InvalidArgument("bad bind address " + bind_addr_);
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    Status s = Status::IOError(std::string("bind: ") + std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return s;
  }
  if (::listen(listen_fd_, 128) != 0) {
    Status s = Status::IOError(std::string("listen: ") + std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return s;
  }
  // Discover the port if the OS chose it.
  socklen_t len = sizeof(addr);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    port_ = ntohs(addr.sin_port);
  }
  stopping_ = false;
  accept_thread_ = std::thread(&Server::AcceptLoop, this);
  return Status::Ok();
}

void Server::Stop() {
  if (stopping_.exchange(true)) return;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);  // unblocks accept()
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (int fd : conn_fds_) ::shutdown(fd, SHUT_RDWR);  // unblocks recv()
    threads.swap(conn_threads_);
  }
  for (auto& t : threads) t.join();
}

void Server::AcceptLoop() {
  while (!stopping_) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
    if (fd < 0) {
      if (errno == EINTR) continue;
      break;  // listener closed by Stop()
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_) {
      ::close(fd);
      break;
    }
    conn_fds_.insert(fd);
    conn_threads_.emplace_back(&Server::HandleConnection, this, fd);
  }
}

void Server::HandleConnection(int fd) {
  std::string payload;
  while (!stopping_ && ReadFrame(fd, &payload)) {
    std::string response = Dispatch(payload);
    ++requests_;
    if (!WriteFrame(fd, response)) break;
  }
  ::close(fd);
  std::lock_guard<std::mutex> l(mu_);
  conn_fds_.erase(fd);
}

// Translates one request into DB calls and one response payload.
std::string Server::Dispatch(Slice payload) {
  Request req;
  Status s = DecodeRequest(payload, &req);
  if (!s.ok()) return EncodeError(RespStatus::kBadRequest, s.ToString());

  switch (req.op) {
    case Op::kPing:
      return EncodeOk();

    case Op::kGet: {
      std::string value;
      s = db_->Get(ReadOptions{}, req.key, &value);
      if (s.ok()) return EncodeOkStr(value);
      if (s.IsNotFound()) return EncodeNotFound();
      return EncodeError(RespStatus::kError, s.ToString());
    }

    case Op::kPut:
      s = db_->Put(WriteOptions{}, req.key, req.value);
      return s.ok() ? EncodeOk() : EncodeError(RespStatus::kError, s.ToString());

    case Op::kDel:
      s = db_->Delete(WriteOptions{}, req.key);
      return s.ok() ? EncodeOk() : EncodeError(RespStatus::kError, s.ToString());

    case Op::kBatch: {
      WriteBatch batch;
      for (const auto& op : req.batch) {
        if (op.is_put) batch.Put(op.key, op.value);
        else batch.Delete(op.key);
      }
      s = db_->Write(WriteOptions{}, &batch);
      return s.ok() ? EncodeOk() : EncodeError(RespStatus::kError, s.ToString());
    }

    case Op::kScan: {
      // The iterator captures a snapshot, so a concurrent writer cannot make
      // the scan see a half-applied batch.
      std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions{}));
      std::vector<std::pair<std::string, std::string>> kvs;
      const Slice end(req.scan_end);
      for (it->Seek(req.scan_start); it->Valid(); it->Next()) {
        if (!end.empty() && it->key().compare(end) >= 0) break;
        kvs.emplace_back(it->key().ToString(), it->value().ToString());
        if (req.scan_limit != 0 && kvs.size() >= req.scan_limit) break;
      }
      if (!it->status().ok()) return EncodeError(RespStatus::kError, it->status().ToString());
      return EncodeScanResult(kvs);
    }

    case Op::kStats:
      return EncodeOkStr(db_->GetStats() + "  server requests:    " +
                         std::to_string(requests_.load() + 1) + "\n");
  }
  return EncodeError(RespStatus::kBadRequest, "unhandled opcode");
}

}  // namespace epica::net
