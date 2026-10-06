// tools/db_driver.cc -- whole-database crash test.
//
//   db_driver <dir>              writes key-<i> = value-<i> forever with
//                                sync=true, printing progress. kill -9 it.
//   db_driver --verify <dir>     reopens the DB (running recovery) and checks
//                                that every key from 0 to the highest one
//                                the writer acknowledged is present with
//                                the right value, and that the count matches.
//
// This exercises the full stack, not just the WAL: recovery has to combine
// SSTables listed in the MANIFEST with WAL records written after the last
// flush, and compaction may have been mid-flight at the moment of the kill.
//
// The writer keeps the highest acknowledged i in key "__max" inside the same
// batch as the data, so the verifier knows exactly what must be present.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "epica/db.h"

using epica::ReadOptions;
using epica::Status;
using epica::WriteOptions;

static std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key-%010llu", static_cast<unsigned long long>(i));
  return buf;
}
static std::string Value(uint64_t i) { return "value-" + std::to_string(i * 7919); }

static int Verify(const std::string& dir) {
  epica::DB* db;
  Status s = epica::DB::Open(epica::Options{}, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }
  std::unique_ptr<epica::DB> owner(db);

  std::string max_str;
  s = db->Get(ReadOptions{}, "__max", &max_str);
  if (s.IsNotFound()) {
    std::printf("empty database; nothing was ever acknowledged\n");
    return 0;
  }
  const uint64_t max = std::strtoull(max_str.c_str(), nullptr, 10);
  uint64_t bad = 0;
  for (uint64_t i = 0; i <= max; ++i) {
    std::string v;
    s = db->Get(ReadOptions{}, Key(i), &v);
    if (!s.ok() || v != Value(i)) {
      if (bad < 10) std::fprintf(stderr, "MISSING/WRONG %s: %s\n", Key(i).c_str(), s.ToString().c_str());
      ++bad;
    }
  }
  // Count via scan, too, so the iterator path is exercised.
  uint64_t scanned = 0;
  std::unique_ptr<epica::Iterator> it(db->NewIterator(ReadOptions{}));
  for (it->Seek("key-"); it->Valid() && it->key().view().starts_with("key-"); it->Next()) ++scanned;

  std::printf("verified keys 0..%llu: %llu bad, scan saw %llu keys (%s)\n",
              static_cast<unsigned long long>(max), static_cast<unsigned long long>(bad),
              static_cast<unsigned long long>(scanned),
              (bad == 0 && scanned >= max + 1) ? "OK" : "FAIL");
  std::printf("%s", db->GetStats().c_str());
  return bad == 0 ? 0 : 2;
}

static int Run(const std::string& dir) {
  epica::Options opts;
  opts.memtable_size = 256 * 1024;  // small, so flushes and compactions happen often
  opts.target_file_size = 128 * 1024;
  opts.level1_max_bytes = 1024 * 1024;
  epica::DB* db;
  Status s = epica::DB::Open(opts, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }
  std::unique_ptr<epica::DB> owner(db);

  uint64_t next = 0;
  std::string max_str;
  if (db->Get(ReadOptions{}, "__max", &max_str).ok()) next = std::strtoull(max_str.c_str(), nullptr, 10) + 1;

  std::printf("db_driver: writing from key %llu (pid %d). kill -9 me any time.\n",
              static_cast<unsigned long long>(next), static_cast<int>(getpid()));
  WriteOptions wo;
  wo.sync = true;
  for (uint64_t i = next;; ++i) {
    epica::WriteBatch b;
    b.Put(Key(i), Value(i));
    b.Put("__max", std::to_string(i));  // same batch => atomic with the data
    s = db->Write(wo, &b);
    if (!s.ok()) {
      std::fprintf(stderr, "write failed: %s\n", s.ToString().c_str());
      return 1;
    }
    if (i % 500 == 0) {
      std::printf("  wrote key %llu\n", static_cast<unsigned long long>(i));
      std::fflush(stdout);
    }
  }
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--verify") == 0) return Verify(argv[2]);
  if (argc == 2) return Run(argv[1]);
  std::fprintf(stderr, "usage: %s <dir> | %s --verify <dir>\n", argv[0], argv[0]);
  return 64;
}
