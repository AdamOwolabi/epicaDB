// tools/epica_shell.cc -- interactive shell for poking at a database.
//
//   epica_shell <db-dir>                 open the directory in-process
//   epica_shell --remote [host:]port     talk to a running epica_server
//
// Commands (one per line):
//   put <key> <value...>     get <key>     del <key>
//   scan [start] [end]       scan keys in [start, end)
//   batch                    start collecting put/del lines; 'end' commits
//   flush                    force memtable -> SSTable   (local mode only)
//   compact                  compact every level         (local mode only)
//   stats                    engine statistics
//   help, quit
//
// The shell is intentionally dumb -- it splits on whitespace and treats the
// rest of the line as the value -- so you can see raw engine behaviour. The
// Java query layer has the real parser.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "epica/db.h"
#include "net/client.h"

namespace {

// Abstracts local vs. remote so the command loop is written once.
class Backend {
 public:
  virtual ~Backend() = default;
  virtual epica::Status Get(const std::string& k, std::string* v) = 0;
  virtual epica::Status Put(const std::string& k, const std::string& v) = 0;
  virtual epica::Status Del(const std::string& k) = 0;
  virtual epica::Status Scan(const std::string& a, const std::string& b,
                             std::vector<std::pair<std::string, std::string>>* out) = 0;
  virtual epica::Status Batch(const std::vector<epica::net::BatchOp>& ops) = 0;
  virtual epica::Status Stats(std::string* s) = 0;
  virtual epica::Status Flush() { return epica::Status::InvalidArgument("local mode only"); }
  virtual epica::Status Compact() { return epica::Status::InvalidArgument("local mode only"); }
};

class LocalBackend : public Backend {
 public:
  explicit LocalBackend(epica::DB* db) : db_(db) {}
  epica::Status Get(const std::string& k, std::string* v) override {
    return db_->Get(epica::ReadOptions{}, k, v);
  }
  epica::Status Put(const std::string& k, const std::string& v) override {
    return db_->Put(epica::WriteOptions{}, k, v);
  }
  epica::Status Del(const std::string& k) override { return db_->Delete(epica::WriteOptions{}, k); }
  epica::Status Scan(const std::string& a, const std::string& b,
                     std::vector<std::pair<std::string, std::string>>* out) override {
    std::unique_ptr<epica::Iterator> it(db_->NewIterator(epica::ReadOptions{}));
    for (it->Seek(a); it->Valid(); it->Next()) {
      if (!b.empty() && it->key().compare(b) >= 0) break;
      out->emplace_back(it->key().ToString(), it->value().ToString());
    }
    return it->status();
  }
  epica::Status Batch(const std::vector<epica::net::BatchOp>& ops) override {
    epica::WriteBatch wb;
    for (const auto& op : ops) {
      if (op.is_put) wb.Put(op.key, op.value);
      else wb.Delete(op.key);
    }
    return db_->Write(epica::WriteOptions{}, &wb);
  }
  epica::Status Stats(std::string* s) override {
    *s = db_->GetStats();
    return epica::Status::Ok();
  }
  epica::Status Flush() override { return db_->Flush(); }
  epica::Status Compact() override { return db_->CompactAll(); }

 private:
  epica::DB* db_;
};

class RemoteBackend : public Backend {
 public:
  epica::Status Connect(const std::string& host, uint16_t port) { return c_.Connect(host, port); }
  epica::Status Get(const std::string& k, std::string* v) override { return c_.Get(k, v); }
  epica::Status Put(const std::string& k, const std::string& v) override { return c_.Put(k, v); }
  epica::Status Del(const std::string& k) override { return c_.Delete(k); }
  epica::Status Scan(const std::string& a, const std::string& b,
                     std::vector<std::pair<std::string, std::string>>* out) override {
    return c_.Scan(a, b, 0, out);
  }
  epica::Status Batch(const std::vector<epica::net::BatchOp>& ops) override { return c_.Batch(ops); }
  epica::Status Stats(std::string* s) override { return c_.Stats(s); }

 private:
  epica::net::Client c_;
};

void PrintHelp() {
  std::puts(
      "  put <key> <value...>   get <key>   del <key>\n"
      "  scan [start] [end]     keys in [start, end)\n"
      "  batch ... end          atomic group of put/del lines\n"
      "  flush | compact        (local mode) force flush / full compaction\n"
      "  stats | help | quit");
}

int Repl(Backend* be) {
  std::string line;
  std::vector<epica::net::BatchOp> batch;
  bool in_batch = false;
  std::printf("epicaDB shell. 'help' for commands.\n");
  while (true) {
    std::printf(in_batch ? "batch> " : "epica> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd.empty()) continue;

    if (cmd == "quit" || cmd == "exit") break;
    if (cmd == "help") {
      PrintHelp();
      continue;
    }
    epica::Status s;
    if (cmd == "put") {
      std::string k, v;
      ss >> k;
      std::getline(ss, v);
      if (!v.empty() && v[0] == ' ') v.erase(0, 1);
      if (in_batch) {
        batch.push_back({true, k, v});
        std::printf("  queued put %s\n", k.c_str());
        continue;
      }
      s = be->Put(k, v);
      std::printf("%s\n", s.ToString().c_str());
    } else if (cmd == "del") {
      std::string k;
      ss >> k;
      if (in_batch) {
        batch.push_back({false, k, ""});
        std::printf("  queued del %s\n", k.c_str());
        continue;
      }
      s = be->Del(k);
      std::printf("%s\n", s.ToString().c_str());
    } else if (cmd == "get") {
      std::string k, v;
      ss >> k;
      s = be->Get(k, &v);
      if (s.ok()) std::printf("\"%s\"\n", v.c_str());
      else std::printf("%s\n", s.ToString().c_str());
    } else if (cmd == "scan") {
      std::string a, b;
      ss >> a >> b;
      std::vector<std::pair<std::string, std::string>> out;
      s = be->Scan(a, b, &out);
      for (const auto& [k, v] : out) std::printf("  %s = \"%s\"\n", k.c_str(), v.c_str());
      std::printf("%zu row(s) %s\n", out.size(), s.ok() ? "" : s.ToString().c_str());
    } else if (cmd == "batch") {
      in_batch = true;
      batch.clear();
      std::printf("  collecting; type 'end' to commit atomically\n");
    } else if (cmd == "end") {
      if (!in_batch) {
        std::printf("not in a batch\n");
        continue;
      }
      in_batch = false;
      s = be->Batch(batch);
      std::printf("%s (%zu ops committed atomically)\n", s.ToString().c_str(), batch.size());
    } else if (cmd == "flush") {
      std::printf("%s\n", be->Flush().ToString().c_str());
    } else if (cmd == "compact") {
      std::printf("%s\n", be->Compact().ToString().c_str());
    } else if (cmd == "stats") {
      std::string st;
      s = be->Stats(&st);
      std::printf("%s", s.ok() ? st.c_str() : (s.ToString() + "\n").c_str());
    } else {
      std::printf("unknown command '%s'; try help\n", cmd.c_str());
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--remote") {
    std::string target = argv[2], host = "127.0.0.1";
    size_t colon = target.find(':');
    if (colon != std::string::npos) {
      host = target.substr(0, colon);
      target = target.substr(colon + 1);
    }
    RemoteBackend be;
    epica::Status s = be.Connect(host, static_cast<uint16_t>(std::atoi(target.c_str())));
    if (!s.ok()) {
      std::fprintf(stderr, "connect failed: %s\n", s.ToString().c_str());
      return 1;
    }
    return Repl(&be);
  }
  if (argc == 2) {
    epica::DB* db;
    epica::Status s = epica::DB::Open(epica::Options{}, argv[1], &db);
    if (!s.ok()) {
      std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
      return 1;
    }
    std::unique_ptr<epica::DB> owner(db);
    LocalBackend be(db);
    return Repl(&be);
  }
  std::fprintf(stderr, "usage: %s <db-dir> | %s --remote [host:]port\n", argv[0], argv[0]);
  return 64;
}
