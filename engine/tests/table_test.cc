// tests/table_test.cc -- SSTable build/read round trips, block boundaries,
// bloom skipping, and corruption detection.
#include "table.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>

#include "coding.h"

namespace epica {
namespace fs = std::filesystem;

class TableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/epica_table_test_XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(tmpl));
    dir_ = tmpl;
    path_ = dir_ + "/t.sst";
    opts_.block_size = 256;  // tiny blocks so a small table spans many
  }
  void TearDown() override { fs::remove_all(dir_); }

  static std::string IKey(const std::string& user, SequenceNumber seq, ValueType t = kTypeValue) {
    std::string s;
    AppendInternalKey(&s, user, seq, t);
    return s;
  }

  // Builds a table with n keys "k00000".."k(n-1)" at sequence 1..n.
  void Build(int n, std::map<std::string, std::string>* kv = nullptr) {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(WritableFile::Open(path_, &f).ok());
    TableBuilder b(opts_, f.get());
    for (int i = 0; i < n; ++i) {
      char k[16];
      std::snprintf(k, sizeof(k), "k%05d", i);
      std::string v = "value-" + std::to_string(i);
      b.Add(IKey(k, static_cast<SequenceNumber>(i + 1)), v);
      if (kv) (*kv)[k] = v;
    }
    ASSERT_TRUE(b.Finish().ok()) << b.status().ToString();
    ASSERT_TRUE(f->Close().ok());
    entries_ = b.NumEntries();
  }

  std::unique_ptr<Table> Open(std::shared_ptr<BlockCache> cache = nullptr) {
    std::unique_ptr<Table> t;
    Status s = Table::Open(opts_, path_, 1, cache, &t);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return t;
  }

  struct Found {
    bool hit = false;
    std::string key, value;
  };
  static void Save(void* arg, Slice k, Slice v) {
    auto* f = static_cast<Found*>(arg);
    f->hit = true;
    f->key = k.ToString();
    f->value = v.ToString();
  }

  std::string dir_, path_;
  Options opts_;
  uint64_t entries_ = 0;
};

TEST_F(TableTest, RoundTripIteration) {
  std::map<std::string, std::string> kv;
  Build(1000, &kv);
  auto t = Open();
  std::unique_ptr<Iterator> it(t->NewIterator());
  auto expect = kv.begin();
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++expect, ++n) {
    ASSERT_NE(kv.end(), expect);
    EXPECT_EQ(expect->first, ExtractUserKey(it->key()).ToString());
    EXPECT_EQ(expect->second, it->value().ToString());
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(1000, n);
}

TEST_F(TableTest, SeekAcrossBlockBoundaries) {
  Build(1000);
  auto t = Open();
  std::unique_ptr<Iterator> it(t->NewIterator());
  for (int probe : {0, 1, 17, 255, 256, 257, 500, 998, 999}) {
    char k[16];
    std::snprintf(k, sizeof(k), "k%05d", probe);
    it->Seek(IKey(k, kMaxSequenceNumber));
    ASSERT_TRUE(it->Valid()) << k;
    EXPECT_EQ(k, ExtractUserKey(it->key()).ToString());
  }
  it->Seek(IKey("k00500x", kMaxSequenceNumber));  // between keys
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("k00501", ExtractUserKey(it->key()).ToString());
  it->Seek(IKey("zzz", kMaxSequenceNumber));  // past the end
  EXPECT_FALSE(it->Valid());
}

TEST_F(TableTest, InternalGetFindsAndMisses) {
  Build(300);
  auto t = Open();
  Found f;
  ASSERT_TRUE(t->InternalGet(IKey("k00150", kMaxSequenceNumber), &f, Save).ok());
  ASSERT_TRUE(f.hit);
  EXPECT_EQ("value-150", f.value);

  // A key that is absent: the bloom filter should usually stop us, and even
  // if it lets us through, the callback sees a different user key.
  Found g;
  ASSERT_TRUE(t->InternalGet(IKey("nope", kMaxSequenceNumber), &g, Save).ok());
  if (g.hit) EXPECT_NE("nope", ExtractUserKey(g.key).ToString());
}

TEST_F(TableTest, BlockCacheIsUsed) {
  Build(1000);
  auto cache = std::make_shared<BlockCache>(1 << 20);
  auto t = Open(cache);
  Found f;
  ASSERT_TRUE(t->InternalGet(IKey("k00010", kMaxSequenceNumber), &f, Save).ok());
  ASSERT_TRUE(t->InternalGet(IKey("k00010", kMaxSequenceNumber), &f, Save).ok());
  EXPECT_EQ(1u, cache->misses());
  EXPECT_EQ(1u, cache->hits());
}

TEST_F(TableTest, FlippedBitInDataBlockIsCorruption) {
  Build(200);
  // Corrupt a byte in the first data block (offset 10 is inside entry data).
  {
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(10);
    char c;
    f.get(c);
    f.seekp(10);
    f.put(static_cast<char>(c ^ 0xff));
  }
  auto t = Open();  // footer/index are intact so open succeeds
  std::unique_ptr<Iterator> it(t->NewIterator());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
}

TEST_F(TableTest, TruncatedFileFailsToOpen) {
  Build(50);
  fs::resize_file(path_, 20);
  std::unique_ptr<Table> t;
  EXPECT_TRUE(Table::Open(opts_, path_, 1, nullptr, &t).IsCorruption());
}

TEST_F(TableTest, TombstonesAndVersionsSurvive) {
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(WritableFile::Open(path_, &f).ok());
  TableBuilder b(opts_, f.get());
  b.Add(IKey("a", 9, kTypeDeletion), "");
  b.Add(IKey("a", 5), "old");
  b.Add(IKey("b", 7), "bee");
  ASSERT_TRUE(b.Finish().ok());
  ASSERT_TRUE(f->Close().ok());
  auto t = Open();
  Found found;
  ASSERT_TRUE(t->InternalGet(IKey("a", 6), &found, Save).ok());  // snapshot 6 -> old
  ASSERT_TRUE(found.hit);
  ParsedInternalKey p;
  ASSERT_TRUE(ParseInternalKey(found.key, &p));
  EXPECT_EQ(5u, p.sequence);
  EXPECT_EQ("old", found.value);
  Found latest;
  ASSERT_TRUE(t->InternalGet(IKey("a", 100), &latest, Save).ok());
  ASSERT_TRUE(ParseInternalKey(latest.key, &p));
  EXPECT_EQ(kTypeDeletion, p.type);
}

}  // namespace epica
