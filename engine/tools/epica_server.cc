// tools/epica_server.cc -- runs the storage engine as a network service.
//
//   epica_server <db-dir> [port]        default port 7379
//
// Opens (or creates) the DB, listens on 127.0.0.1:<port>, and serves the
// binary protocol in net/protocol.h until SIGINT/SIGTERM. The Java query
// layer (server/) and `epica_shell --remote` connect to this.
//
// On shutdown the DB destructor syncs the WAL and joins the background
// thread, so Ctrl-C never loses acknowledged writes.

#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "epica/db.h"
#include "net/server.h"

static volatile std::sig_atomic_t g_stop = 0;
static void OnSignal(int) { g_stop = 1; }

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s <db-dir> [port]\n", argv[0]);
    return 64;
  }
  const std::string dir = argv[1];
  const int port = argc == 3 ? std::atoi(argv[2]) : 7379;

  epica::DB* db = nullptr;
  epica::Status s = epica::DB::Open(epica::Options{}, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }
  std::unique_ptr<epica::DB> db_owner(db);

  epica::net::Server server(db, static_cast<uint16_t>(port));
  s = server.Start();
  if (!s.ok()) {
    std::fprintf(stderr, "server start failed: %s\n", s.ToString().c_str());
    return 1;
  }
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  std::printf("epica_server: serving %s on 127.0.0.1:%u (pid %d). Ctrl-C to stop.\n", dir.c_str(),
              server.port(), static_cast<int>(getpid()));
  std::fflush(stdout);

  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::printf("\nepica_server: stopping after %llu requests\n",
              static_cast<unsigned long long>(server.requests_served()));
  server.Stop();
  return 0;  // db_owner's destructor flushes the WAL and joins the bg thread
}
