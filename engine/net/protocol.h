// net/protocol.h -- the binary wire format between clients (the Java query
// layer, the C++ test client, anything else) and the engine server.
//
// Design goals: trivially parseable in any language, length-prefixed so a
// reader always knows how many bytes to wait for, and binary-safe (keys and
// values can contain any byte, including '\n' and '\0').
//
// Every message is one frame. All integers little-endian, like the rest of
// the engine's formats.
//
//   Request:   | u32 length | u8 opcode | body (length-1 bytes) |
//   Response:  | u32 length | u8 status | body (length-1 bytes) |
//
//   opcode      body
//   PING  0x01  (empty)                             -> OK, empty
//   GET   0x02  str key                             -> OK, str value | NOT_FOUND
//   PUT   0x03  str key, str value                  -> OK
//   DEL   0x04  str key                             -> OK
//   SCAN  0x05  str start, str end, u32 limit       -> OK, u32 n, n x (str key, str value)
//               end == "" means unbounded; end is exclusive; limit 0 = none
//   BATCH 0x06  u32 n, n x (u8 kind, str key[, str value])   kind 1=put 0=del
//                                                   -> OK
//   STATS 0x07  (empty)                             -> OK, str text
//
//   str = | u32 len | bytes |
//
//   status: 0 OK, 1 NOT_FOUND, 2 ERROR (body = str message), 3 BAD_REQUEST
//
// Example: GET "abc" on the wire is
//   08 00 00 00   length = 8 (1 opcode + 4 key-length + 3 key bytes)
//   02            opcode GET
//   03 00 00 00   key length 3
//   61 62 63      "abc"
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epica/slice.h"
#include "epica/status.h"

namespace epica::net {

enum class Op : uint8_t {
  kPing = 0x01,
  kGet = 0x02,
  kPut = 0x03,
  kDel = 0x04,
  kScan = 0x05,
  kBatch = 0x06,
  kStats = 0x07,
};

enum class RespStatus : uint8_t {
  kOk = 0,
  kNotFound = 1,
  kError = 2,
  kBadRequest = 3,
};

constexpr uint32_t kMaxFrameSize = 64u * 1024u * 1024u;

// --- building ---
void PutU32(std::string* dst, uint32_t v);
void PutU8(std::string* dst, uint8_t v);
void PutStr(std::string* dst, Slice s);

// Wraps `payload` (opcode/status + body) in a length prefix.
std::string Frame(Slice payload);

// --- parsing ---
// Each returns false on truncated input and leaves *in unspecified.
bool GetU32(Slice* in, uint32_t* v);
bool GetU8(Slice* in, uint8_t* v);
bool GetStr(Slice* in, Slice* s);

struct BatchOp {
  bool is_put;
  std::string key;
  std::string value;
};

// Fully decoded request.
struct Request {
  Op op;
  std::string key;
  std::string value;
  std::string scan_start, scan_end;
  uint32_t scan_limit = 0;
  std::vector<BatchOp> batch;
};

// Decodes a request payload (everything after the length prefix).
Status DecodeRequest(Slice payload, Request* out);

// Encoders for request payloads (client side).
std::string EncodePing();
std::string EncodeGet(Slice key);
std::string EncodePut(Slice key, Slice value);
std::string EncodeDel(Slice key);
std::string EncodeScan(Slice start, Slice end, uint32_t limit);
std::string EncodeBatch(const std::vector<BatchOp>& ops);
std::string EncodeStats();

// Encoders for response payloads (server side).
std::string EncodeOk();
std::string EncodeOkStr(Slice s);
std::string EncodeNotFound();
std::string EncodeError(RespStatus st, Slice message);
std::string EncodeScanResult(const std::vector<std::pair<std::string, std::string>>& kvs);

}  // namespace epica::net
