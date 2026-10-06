// tests/wal_test.cc -- write-ahead log: round trips, torn-tail truncation,
// corruption detection, multi-file replay order, buffering vs. sync.
#include "epica/wal.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace epica {
namespace {

namespace fs = std::filesystem;

struct Rec {
  RecordType type;
  std::string payload;
  bool operator==(const Rec& o) const { return type == o.type && payload == o.payload; }
};

class WalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/epica_wal_test_XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(tmpl));
    dir_ = tmpl;
  }
  void TearDown() override { fs::remove_all(dir_); }

  std::vector<Rec> Replay(Status* st, WalRecoveryInfo* info = nullptr) {
    std::vector<Rec> out;
    *st = RecoverWal(
        dir_, [&](RecordType t, Slice p) { out.push_back({t, p.ToString()}); }, info);
    return out;
  }

  std::vector<Rec> ReplayOk(WalRecoveryInfo* info = nullptr) {
    Status st;
    auto out = Replay(&st, info);
    EXPECT_TRUE(st.ok()) << st.ToString();
    return out;
  }

  uint64_t FileSize(uint64_t seq) { return fs::file_size(WalFileName(dir_, seq)); }

  void AppendRaw(uint64_t seq, const std::string& bytes) {
    int fd = ::open(WalFileName(dir_, seq).c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(static_cast<ssize_t>(bytes.size()), ::write(fd, bytes.data(), bytes.size()));
    ::close(fd);
  }

  void FlipByte(uint64_t seq, uint64_t off) {
    int fd = ::open(WalFileName(dir_, seq).c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    char c;
    ASSERT_EQ(1, ::pread(fd, &c, 1, static_cast<off_t>(off)));
    c = static_cast<char>(c ^ 0xFF);
    ASSERT_EQ(1, ::pwrite(fd, &c, 1, static_cast<off_t>(off)));
    ::close(fd);
  }

  std::vector<Rec> WriteRecords(uint64_t seq, int n, const WalOptions& opts = WalOptions{}) {
    std::unique_ptr<WalWriter> w;
    Status s = WalWriter::Open(dir_, seq, opts, &w);
    EXPECT_TRUE(s.ok()) << s.ToString();
    std::vector<Rec> recs;
    for (int i = 0; i < n; ++i) {
      Rec r;
      r.type = (i % 3 == 0) ? RecordType::kDelete : RecordType::kPut;
      r.payload = "seq" + std::to_string(seq) + "-rec" + std::to_string(i) +
                  std::string(static_cast<size_t>(i % 7), 'x');
      EXPECT_TRUE(w->Append(r.type, r.payload).ok());
      recs.push_back(std::move(r));
    }
    EXPECT_TRUE(w->Close().ok());
    return recs;
  }

  std::string dir_;
};

TEST_F(WalTest, FileNameRoundTrip) {
  EXPECT_EQ(dir_ + "/wal/000000000042.log", WalFileName(dir_, 42));
  uint64_t seq = 0;
  EXPECT_TRUE(ParseWalFileName("000000000042.log", &seq));
  EXPECT_EQ(42u, seq);
  EXPECT_FALSE(ParseWalFileName("foo.log", &seq));
  EXPECT_FALSE(ParseWalFileName("42.txt", &seq));
  EXPECT_FALSE(ParseWalFileName(".log", &seq));
}

TEST_F(WalTest, RecoverWithNoWalDirIsOk) {
  WalRecoveryInfo info;
  auto recs = ReplayOk(&info);
  EXPECT_TRUE(recs.empty());
  EXPECT_EQ(0u, info.files_replayed);
  EXPECT_EQ(0u, info.last_seq);
}

TEST_F(WalTest, EmptyFileRecoversToZeroRecords) {
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, &w).ok());
  ASSERT_TRUE(w->Close().ok());
  WalRecoveryInfo info;
  auto recs = ReplayOk(&info);
  EXPECT_TRUE(recs.empty());
  EXPECT_EQ(1u, info.files_replayed);
  EXPECT_EQ(1u, info.last_seq);
}

TEST_F(WalTest, RoundTrip) {
  auto expected = WriteRecords(1, 200);
  WalRecoveryInfo info;
  auto got = ReplayOk(&info);
  EXPECT_EQ(expected, got);
  EXPECT_EQ(200u, info.records_replayed);
  EXPECT_EQ(0u, info.truncated_bytes);
}

TEST_F(WalTest, EmptyPayloadRoundTrip) {
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, &w).ok());
  ASSERT_TRUE(w->Append(RecordType::kPut, Slice()).ok());
  ASSERT_TRUE(w->Close().ok());
  auto got = ReplayOk();
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(RecordType::kPut, got[0].type);
  EXPECT_TRUE(got[0].payload.empty());
}

TEST_F(WalTest, OpenRefusesNonEmptyExistingFile) {
  WriteRecords(1, 3);
  std::unique_ptr<WalWriter> w;
  Status s = WalWriter::Open(dir_, 1, &w);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(WalTest, ReopenAppends) {
  auto first = WriteRecords(1, 5);
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Reopen(dir_, 1, WalOptions{}, &w).ok());
  ASSERT_TRUE(w->Append(RecordType::kPut, "after-reopen").ok());
  ASSERT_TRUE(w->Close().ok());
  auto got = ReplayOk();
  ASSERT_EQ(6u, got.size());
  for (size_t i = 0; i < 5; ++i) EXPECT_EQ(first[i], got[i]);
  EXPECT_EQ("after-reopen", got[5].payload);
}

TEST_F(WalTest, TornTailGarbageIsTruncated) {
  auto expected = WriteRecords(1, 10);
  const uint64_t good_size = FileSize(1);
  // A partial header: fewer bytes than kWalHeaderSize.
  AppendRaw(1, std::string("\x01\x02\x03", 3));
  ASSERT_GT(FileSize(1), good_size);

  WalRecoveryInfo info;
  auto got = ReplayOk(&info);
  EXPECT_EQ(expected, got);
  EXPECT_EQ(3u, info.truncated_bytes);
  EXPECT_EQ(good_size, FileSize(1));
}

TEST_F(WalTest, TornTailPartialPayloadIsTruncated) {
  auto expected = WriteRecords(1, 10);
  const uint64_t good_size = FileSize(1);
  // A full header claiming 100 bytes of payload, but only 10 present.
  std::string hdr;
  hdr.append("\0\0\0\0", 4);              // crc (wrong, but never checked)
  hdr.append("\x64\0\0\0", 4);            // length = 100
  hdr.push_back(1);                       // kPut
  hdr.append(10, 'z');
  AppendRaw(1, hdr);

  auto got = ReplayOk();
  EXPECT_EQ(expected, got);
  EXPECT_EQ(good_size, FileSize(1));
}

TEST_F(WalTest, FlippedByteInLastFileTruncatesFromThatRecord) {
  auto expected = WriteRecords(1, 10);
  // Compute the offset of record 7 by replaying with a reader.
  std::unique_ptr<WalReader> r;
  ASSERT_TRUE(WalReader::Open(WalFileName(dir_, 1), &r).ok());
  RecordType t;
  std::string p;
  Status st;
  for (int i = 0; i < 7; ++i) ASSERT_TRUE(r->ReadRecord(&t, &p, &st));
  const uint64_t off7 = r->offset();
  r.reset();

  // Corrupt a payload byte of record 7.
  FlipByte(1, off7 + kWalHeaderSize + 2);

  WalRecoveryInfo info;
  auto got = ReplayOk(&info);
  ASSERT_EQ(7u, got.size());
  for (size_t i = 0; i < 7; ++i) EXPECT_EQ(expected[i], got[i]);
  EXPECT_EQ(off7, FileSize(1));
  EXPECT_GT(info.truncated_bytes, 0u);

  // Recovery is idempotent: a second pass sees a clean file.
  WalRecoveryInfo info2;
  auto again = ReplayOk(&info2);
  EXPECT_EQ(got, again);
  EXPECT_EQ(0u, info2.truncated_bytes);
}

TEST_F(WalTest, FlippedByteInNonLastFileIsHardCorruption) {
  WriteRecords(1, 10);
  WriteRecords(2, 10);
  const uint64_t size1 = FileSize(1);
  FlipByte(1, kWalHeaderSize + 1);  // inside the first record's payload
  Status st;
  Replay(&st);
  EXPECT_TRUE(st.IsCorruption()) << st.ToString();
  EXPECT_EQ(size1, FileSize(1)) << "non-last file must not be truncated";
}

TEST_F(WalTest, MultipleFilesReplayInSeqOrder) {
  // Create out of numeric order to make sure we sort, not rely on readdir.
  auto r3 = WriteRecords(3, 4);
  auto r1 = WriteRecords(1, 4);
  auto r10 = WriteRecords(10, 4);
  std::vector<Rec> expected;
  expected.insert(expected.end(), r1.begin(), r1.end());
  expected.insert(expected.end(), r3.begin(), r3.end());
  expected.insert(expected.end(), r10.begin(), r10.end());

  WalRecoveryInfo info;
  auto got = ReplayOk(&info);
  EXPECT_EQ(expected, got);
  EXPECT_EQ(3u, info.files_replayed);
  EXPECT_EQ(10u, info.last_seq);
}

TEST_F(WalTest, LargePayloadRoundTrip) {
  std::string big(3 * 1024 * 1024, '\0');
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, &w).ok());
  ASSERT_TRUE(w->Append(RecordType::kPut, big).ok());
  ASSERT_TRUE(w->Append(RecordType::kDelete, "tiny").ok());
  ASSERT_TRUE(w->Close().ok());
  auto got = ReplayOk();
  ASSERT_EQ(2u, got.size());
  EXPECT_EQ(big, got[0].payload);
  EXPECT_EQ("tiny", got[1].payload);
}

TEST_F(WalTest, PayloadOverMaxIsRejected) {
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, &w).ok());
  std::string too_big(kWalMaxPayload + 1, 'a');
  EXPECT_TRUE(w->Append(RecordType::kPut, too_big).IsInvalidArgument());
}

TEST_F(WalTest, SyncOnAppendIsVisibleWithoutClose) {
  WalOptions opts;
  opts.sync_on_append = true;
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, opts, &w).ok());
  ASSERT_TRUE(w->Append(RecordType::kPut, "one").ok());
  ASSERT_TRUE(w->Append(RecordType::kPut, "two").ok());

  // Do not close. A fresh reader must already see both records.
  std::unique_ptr<WalReader> r;
  ASSERT_TRUE(WalReader::Open(WalFileName(dir_, 1), &r).ok());
  RecordType t;
  std::string p;
  Status st;
  ASSERT_TRUE(r->ReadRecord(&t, &p, &st));
  EXPECT_EQ("one", p);
  ASSERT_TRUE(r->ReadRecord(&t, &p, &st));
  EXPECT_EQ("two", p);
  EXPECT_FALSE(r->ReadRecord(&t, &p, &st));
  EXPECT_TRUE(st.ok());
}

TEST_F(WalTest, BufferedAppendIsNotVisibleUntilFlush) {
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, &w).ok());
  ASSERT_TRUE(w->Append(RecordType::kPut, "buffered").ok());
  EXPECT_EQ(0u, FileSize(1));
  ASSERT_TRUE(w->Flush().ok());
  EXPECT_EQ(kWalHeaderSize + 8, FileSize(1));
  EXPECT_EQ(kWalHeaderSize + 8, w->size());
}

TEST_F(WalTest, BufferFlushesAutomaticallyAtThreshold) {
  WalOptions opts;
  opts.buffer_size = 1024;
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, opts, &w).ok());
  std::string payload(300, 'p');
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(w->Append(RecordType::kPut, payload).ok());
  // 4 * (9 + 300) = 1236 >= 1024, so at least one flush happened.
  EXPECT_GT(FileSize(1), 0u);
  ASSERT_TRUE(w->Close().ok());
  EXPECT_EQ(4u * (kWalHeaderSize + 300), FileSize(1));
}

TEST_F(WalTest, ReaderReportsUnknownType) {
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, &w).ok());
  ASSERT_TRUE(w->Append(RecordType::kPut, "ok").ok());
  ASSERT_TRUE(w->Close().ok());
  FlipByte(1, 8);  // type byte of the first (only) record
  std::unique_ptr<WalReader> r;
  ASSERT_TRUE(WalReader::Open(WalFileName(dir_, 1), &r).ok());
  RecordType t;
  std::string p;
  Status st;
  EXPECT_FALSE(r->ReadRecord(&t, &p, &st));
  EXPECT_TRUE(st.IsCorruption()) << st.ToString();
  EXPECT_EQ(0u, r->offset());
  // Sticky failure.
  EXPECT_FALSE(r->ReadRecord(&t, &p, &st));
  EXPECT_TRUE(st.IsCorruption());
}

TEST_F(WalTest, WritesToClosedWriterFail) {
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(dir_, 1, &w).ok());
  ASSERT_TRUE(w->Close().ok());
  EXPECT_TRUE(w->Append(RecordType::kPut, "x").IsIOError());
  EXPECT_TRUE(w->Close().ok());  // idempotent
}

}  // namespace
}  // namespace epica
