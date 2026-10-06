// status.h -- the engine's error type. Every fallible call returns a Status
// instead of throwing: callers check s.ok() and branch on the code. This
// mirrors LevelDB/RocksDB and keeps error paths explicit in C++ code that
// also runs on a background thread, where an uncaught exception would
// terminate the process.
//
//   Status s = db->Get(opts, key, &value);
//   if (s.IsNotFound()) ...      // key absent (or deleted)
//   else if (!s.ok())  ...       // s.ToString() e.g. "Corruption: crc mismatch"
#pragma once

#include <string>
#include <utility>

namespace epica {

// Result type for operations that can fail. Cheap to copy; carries a code and
// an optional human-readable message.
class Status {
 public:
  enum class Code { kOk = 0, kNotFound, kCorruption, kIOError, kInvalidArgument };

  Status() = default;

  static Status Ok() { return Status(); }
  static Status NotFound(std::string msg = "") { return Status(Code::kNotFound, std::move(msg)); }
  static Status Corruption(std::string msg = "") { return Status(Code::kCorruption, std::move(msg)); }
  static Status IOError(std::string msg = "") { return Status(Code::kIOError, std::move(msg)); }
  static Status InvalidArgument(std::string msg = "") {
    return Status(Code::kInvalidArgument, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

  std::string ToString() const {
    if (ok()) return "OK";
    std::string s;
    switch (code_) {
      case Code::kNotFound: s = "NotFound"; break;
      case Code::kCorruption: s = "Corruption"; break;
      case Code::kIOError: s = "IOError"; break;
      case Code::kInvalidArgument: s = "InvalidArgument"; break;
      case Code::kOk: s = "OK"; break;
    }
    if (!msg_.empty()) {
      s += ": ";
      s += msg_;
    }
    return s;
  }

 private:
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace epica
