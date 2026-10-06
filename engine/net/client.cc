// net/client.cc -- see client.h.

#include "net/client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "net/server.h"  // for ReadFrame / WriteFrame

namespace epica::net {

Client::~Client() { Close(); }

Status Client::Connect(const std::string& host, uint16_t port) {
  Close();
  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) return Status::IOError(std::string("socket: ") + std::strerror(errno));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    Close();
    return Status::InvalidArgument("bad host " + host);
  }
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    Status s = Status::IOError(std::string("connect: ") + std::strerror(errno));
    Close();
    return s;
  }
  return Status::Ok();
}

void Client::Close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

Status Client::RoundTrip(const std::string& request, RespStatus* st, Slice* body,
                         std::string* storage) {
  if (fd_ < 0) return Status::IOError("not connected");
  if (!WriteFrame(fd_, request)) return Status::IOError("send failed");
  if (!ReadFrame(fd_, storage)) return Status::IOError("connection closed by server");
  Slice in(*storage);
  uint8_t code;
  if (!GetU8(&in, &code)) return Status::Corruption("empty response");
  *st = static_cast<RespStatus>(code);
  *body = in;
  if (*st == RespStatus::kError || *st == RespStatus::kBadRequest) {
    Slice msg;
    GetStr(&in, &msg);
    return *st == RespStatus::kError ? Status::IOError("server: " + msg.ToString())
                                     : Status::InvalidArgument("server: " + msg.ToString());
  }
  return Status::Ok();
}

Status Client::Ping() {
  RespStatus st;
  Slice body;
  std::string storage;
  return RoundTrip(EncodePing(), &st, &body, &storage);
}

Status Client::Get(Slice key, std::string* value) {
  RespStatus st;
  Slice body;
  std::string storage;
  Status s = RoundTrip(EncodeGet(key), &st, &body, &storage);
  if (!s.ok()) return s;
  if (st == RespStatus::kNotFound) return Status::NotFound();
  Slice v;
  if (!GetStr(&body, &v)) return Status::Corruption("bad GET response");
  value->assign(v.data(), v.size());
  return Status::Ok();
}

Status Client::Put(Slice key, Slice value) {
  RespStatus st;
  Slice body;
  std::string storage;
  return RoundTrip(EncodePut(key, value), &st, &body, &storage);
}

Status Client::Delete(Slice key) {
  RespStatus st;
  Slice body;
  std::string storage;
  return RoundTrip(EncodeDel(key), &st, &body, &storage);
}

Status Client::Batch(const std::vector<BatchOp>& ops) {
  RespStatus st;
  Slice body;
  std::string storage;
  return RoundTrip(EncodeBatch(ops), &st, &body, &storage);
}

Status Client::Scan(Slice start, Slice end, uint32_t limit,
                    std::vector<std::pair<std::string, std::string>>* out) {
  RespStatus st;
  Slice body;
  std::string storage;
  Status s = RoundTrip(EncodeScan(start, end, limit), &st, &body, &storage);
  if (!s.ok()) return s;
  uint32_t n;
  if (!GetU32(&body, &n)) return Status::Corruption("bad SCAN response");
  out->clear();
  for (uint32_t i = 0; i < n; ++i) {
    Slice k, v;
    if (!GetStr(&body, &k) || !GetStr(&body, &v)) return Status::Corruption("bad SCAN entry");
    out->emplace_back(k.ToString(), v.ToString());
  }
  return Status::Ok();
}

Status Client::Stats(std::string* text) {
  RespStatus st;
  Slice body;
  std::string storage;
  Status s = RoundTrip(EncodeStats(), &st, &body, &storage);
  if (!s.ok()) return s;
  Slice t;
  if (!GetStr(&body, &t)) return Status::Corruption("bad STATS response");
  *text = t.ToString();
  return Status::Ok();
}

}  // namespace epica::net
