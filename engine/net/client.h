// net/client.h -- minimal blocking C++ client for the engine server.
//
// Used by the integration tests and handy for quick experiments. The Java
// query layer has its own client (server/src/main/java/epica/client) that
// speaks the identical wire format.
//
//   epica::net::Client c;
//   c.Connect("127.0.0.1", 7379);
//   c.Put("k", "v");
//   std::string v; c.Get("k", &v);       // Ok, v == "v"
//   c.Get("missing", &v);                // NotFound
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "epica/status.h"
#include "net/protocol.h"

namespace epica::net {

class Client {
 public:
  Client() = default;
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Status Connect(const std::string& host, uint16_t port);
  void Close();
  bool connected() const { return fd_ >= 0; }

  Status Ping();
  Status Get(Slice key, std::string* value);
  Status Put(Slice key, Slice value);
  Status Delete(Slice key);
  Status Scan(Slice start, Slice end, uint32_t limit,
              std::vector<std::pair<std::string, std::string>>* out);
  Status Batch(const std::vector<BatchOp>& ops);
  Status Stats(std::string* text);

 private:
  // Sends one request and parses the status byte + body of the reply.
  Status RoundTrip(const std::string& request, RespStatus* st, Slice* body, std::string* storage);
  int fd_ = -1;
};

}  // namespace epica::net
