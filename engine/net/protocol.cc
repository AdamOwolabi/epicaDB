// net/protocol.cc -- see protocol.h.

#include "net/protocol.h"

namespace epica::net {

void PutU32(std::string* dst, uint32_t v) {
  char b[4];
  b[0] = static_cast<char>(v & 0xff);
  b[1] = static_cast<char>((v >> 8) & 0xff);
  b[2] = static_cast<char>((v >> 16) & 0xff);
  b[3] = static_cast<char>((v >> 24) & 0xff);
  dst->append(b, 4);
}
void PutU8(std::string* dst, uint8_t v) { dst->push_back(static_cast<char>(v)); }
void PutStr(std::string* dst, Slice s) {
  PutU32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

std::string Frame(Slice payload) {
  std::string out;
  PutU32(&out, static_cast<uint32_t>(payload.size()));
  out.append(payload.data(), payload.size());
  return out;
}

bool GetU32(Slice* in, uint32_t* v) {
  if (in->size() < 4) return false;
  const auto* b = reinterpret_cast<const unsigned char*>(in->data());
  *v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
       (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  in->remove_prefix(4);
  return true;
}
bool GetU8(Slice* in, uint8_t* v) {
  if (in->empty()) return false;
  *v = static_cast<uint8_t>((*in)[0]);
  in->remove_prefix(1);
  return true;
}
bool GetStr(Slice* in, Slice* s) {
  uint32_t n;
  if (!GetU32(in, &n) || in->size() < n) return false;
  *s = Slice(in->data(), n);
  in->remove_prefix(n);
  return true;
}

Status DecodeRequest(Slice in, Request* r) {
  uint8_t op;
  if (!GetU8(&in, &op)) return Status::InvalidArgument("empty request");
  r->op = static_cast<Op>(op);
  Slice a, b;
  switch (r->op) {
    case Op::kPing:
    case Op::kStats:
      break;
    case Op::kGet:
    case Op::kDel:
      if (!GetStr(&in, &a)) return Status::InvalidArgument("truncated key");
      r->key = a.ToString();
      break;
    case Op::kPut:
      if (!GetStr(&in, &a) || !GetStr(&in, &b)) return Status::InvalidArgument("truncated put");
      r->key = a.ToString();
      r->value = b.ToString();
      break;
    case Op::kScan:
      if (!GetStr(&in, &a) || !GetStr(&in, &b) || !GetU32(&in, &r->scan_limit)) {
        return Status::InvalidArgument("truncated scan");
      }
      r->scan_start = a.ToString();
      r->scan_end = b.ToString();
      break;
    case Op::kBatch: {
      uint32_t n;
      if (!GetU32(&in, &n)) return Status::InvalidArgument("truncated batch");
      for (uint32_t i = 0; i < n; ++i) {
        uint8_t kind;
        if (!GetU8(&in, &kind) || !GetStr(&in, &a)) return Status::InvalidArgument("truncated batch op");
        BatchOp bo;
        bo.is_put = (kind == 1);
        bo.key = a.ToString();
        if (bo.is_put) {
          if (!GetStr(&in, &b)) return Status::InvalidArgument("truncated batch value");
          bo.value = b.ToString();
        }
        r->batch.push_back(std::move(bo));
      }
      break;
    }
    default:
      return Status::InvalidArgument("unknown opcode " + std::to_string(op));
  }
  if (!in.empty()) return Status::InvalidArgument("trailing bytes in request");
  return Status::Ok();
}

// --- request encoders ---
std::string EncodePing() {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(Op::kPing));
  return s;
}
std::string EncodeStats() {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(Op::kStats));
  return s;
}
std::string EncodeGet(Slice key) {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(Op::kGet));
  PutStr(&s, key);
  return s;
}
std::string EncodeDel(Slice key) {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(Op::kDel));
  PutStr(&s, key);
  return s;
}
std::string EncodePut(Slice key, Slice value) {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(Op::kPut));
  PutStr(&s, key);
  PutStr(&s, value);
  return s;
}
std::string EncodeScan(Slice start, Slice end, uint32_t limit) {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(Op::kScan));
  PutStr(&s, start);
  PutStr(&s, end);
  PutU32(&s, limit);
  return s;
}
std::string EncodeBatch(const std::vector<BatchOp>& ops) {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(Op::kBatch));
  PutU32(&s, static_cast<uint32_t>(ops.size()));
  for (const auto& op : ops) {
    PutU8(&s, op.is_put ? 1 : 0);
    PutStr(&s, op.key);
    if (op.is_put) PutStr(&s, op.value);
  }
  return s;
}

// --- response encoders ---
std::string EncodeOk() {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(RespStatus::kOk));
  return s;
}
std::string EncodeOkStr(Slice v) {
  std::string s = EncodeOk();
  PutStr(&s, v);
  return s;
}
std::string EncodeNotFound() {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(RespStatus::kNotFound));
  return s;
}
std::string EncodeError(RespStatus st, Slice message) {
  std::string s;
  PutU8(&s, static_cast<uint8_t>(st));
  PutStr(&s, message);
  return s;
}
std::string EncodeScanResult(const std::vector<std::pair<std::string, std::string>>& kvs) {
  std::string s = EncodeOk();
  PutU32(&s, static_cast<uint32_t>(kvs.size()));
  for (const auto& [k, v] : kvs) {
    PutStr(&s, k);
    PutStr(&s, v);
  }
  return s;
}

}  // namespace epica::net
