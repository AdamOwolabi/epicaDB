// net/server.h -- TCP front door for the storage engine.
//
// One acceptor thread accepts connections; each connection gets its own
// thread that loops "read frame -> dispatch to DB -> write frame" until the
// client hangs up. The DB itself is fully thread-safe, so connection threads
// call it directly with no extra locking. Concurrent PUTs from different
// connections are merged by the engine's group commit; concurrent GETs run
// in parallel without blocking each other.
//
// Thread-per-connection was chosen over an event loop (epoll/kqueue) because
// the expected client count is small (a query server plus a few shells) and
// the code is a fraction of the size. See DESIGN_DECISIONS.md.
//
//   epica::net::Server server(db, 7379);
//   server.Start();      // returns immediately; threads run in background
//   ...
//   server.Stop();       // closes listener and all connections, joins threads
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "epica/db.h"
#include "epica/status.h"

namespace epica::net {

class Server {
 public:
  // `port` 0 = let the OS pick (see port() after Start).
  Server(DB* db, uint16_t port, std::string bind_addr = "127.0.0.1");
  ~Server();

  Status Start();
  void Stop();
  uint16_t port() const { return port_; }
  uint64_t requests_served() const { return requests_.load(); }

 private:
  void AcceptLoop();
  void HandleConnection(int fd);
  std::string Dispatch(Slice request_payload);

  DB* db_;
  uint16_t port_;
  std::string bind_addr_;
  int listen_fd_ = -1;
  std::atomic<bool> stopping_{false};
  std::atomic<uint64_t> requests_{0};
  std::thread accept_thread_;

  std::mutex mu_;
  std::set<int> conn_fds_;
  std::vector<std::thread> conn_threads_;
};

// Blocking helpers shared by server and client.
bool ReadExact(int fd, char* buf, size_t n);
bool WriteAll(int fd, const char* buf, size_t n);
// Reads one length-prefixed frame; returns false on EOF/error/oversize.
bool ReadFrame(int fd, std::string* payload);
bool WriteFrame(int fd, Slice payload);

}  // namespace epica::net
