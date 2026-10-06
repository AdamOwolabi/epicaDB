// tools/wal_driver.cc -- manual crash-test driver for the WAL alone
// (milestone 1). For the whole-database equivalent see db_driver.cc.
//
//   wal_driver <dir>              append records forever with sync_on_append,
//                                 printing a progress line every 1000 records.
//                                 kill -9 it whenever you like.
//   wal_driver --recover <dir>    replay the log, report record count and
//                                 how many bytes (if any) were truncated.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "epica/wal.h"

using epica::RecordType;
using epica::Slice;
using epica::Status;

static int Recover(const std::string& dir) {
  uint64_t n = 0;
  uint64_t last_i = 0;
  bool gap = false;
  epica::WalRecoveryInfo info;
  Status s = epica::RecoverWal(
      dir,
      [&](RecordType type, Slice payload) {
        (void)type;
        // Payload is "rec-<i>"; check the sequence is contiguous.
        uint64_t i = std::strtoull(payload.data() + 4, nullptr, 10);
        if (n > 0 && i != last_i + 1) gap = true;
        last_i = i;
        ++n;
      },
      &info);
  if (!s.ok()) {
    std::fprintf(stderr, "recovery failed: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("recovered %llu records from %llu file(s); last_seq=%llu; truncated=%llu bytes; %s\n",
              static_cast<unsigned long long>(n),
              static_cast<unsigned long long>(info.files_replayed),
              static_cast<unsigned long long>(info.last_seq),
              static_cast<unsigned long long>(info.truncated_bytes),
              gap ? "SEQUENCE GAP DETECTED" : "sequence contiguous");
  return gap ? 2 : 0;
}

static int Append(const std::string& dir) {
  // Recover first so we know where to continue numbering and which seq to use.
  uint64_t next_i = 0;
  epica::WalRecoveryInfo info;
  Status s = epica::RecoverWal(
      dir, [&](RecordType, Slice p) { next_i = std::strtoull(p.data() + 4, nullptr, 10) + 1; },
      &info);
  if (!s.ok()) {
    std::fprintf(stderr, "recovery failed: %s\n", s.ToString().c_str());
    return 1;
  }

  epica::WalOptions opts;
  opts.sync_on_append = true;
  std::unique_ptr<epica::WalWriter> w;
  const uint64_t seq = info.last_seq + 1;
  s = epica::WalWriter::Open(dir, seq, opts, &w);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("appending to %s starting at rec-%llu (pid %d). kill -9 me any time.\n",
              epica::WalFileName(dir, seq).c_str(), static_cast<unsigned long long>(next_i),
              static_cast<int>(getpid()));
  for (uint64_t i = next_i;; ++i) {
    std::string payload = "rec-" + std::to_string(i);
    s = w->Append(RecordType::kPut, payload);
    if (!s.ok()) {
      std::fprintf(stderr, "append failed: %s\n", s.ToString().c_str());
      return 1;
    }
    if (i % 1000 == 0) {
      std::printf("  rec-%llu written (file %llu bytes)\n", static_cast<unsigned long long>(i),
                  static_cast<unsigned long long>(w->size()));
      std::fflush(stdout);
    }
  }
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--recover") == 0) return Recover(argv[2]);
  if (argc == 2) return Append(argv[1]);
  std::fprintf(stderr, "usage: %s <dir> | %s --recover <dir>\n", argv[0], argv[0]);
  return 64;
}
