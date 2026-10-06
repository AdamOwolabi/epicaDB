// tests/version_test.cc -- MANIFEST persistence and compaction picking.
#include "version.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "filenames.h"

namespace epica {
namespace fs = std::filesystem;

class VersionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/epica_version_test_XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(tmpl));
    dir_ = tmpl;
    fs::create_directories(SstDirName(dir_));
    cache_ = std::make_shared<BlockCache>(1 << 20);
  }
  void TearDown() override { fs::remove_all(dir_); }

  static std::string IKey(const std::string& u, SequenceNumber s = 1) {
    std::string k;
    AppendInternalKey(&k, u, s, kTypeValue);
    return k;
  }
  std::shared_ptr<FileMetaData> Meta(uint64_t number, uint64_t size, const std::string& lo,
                                     const std::string& hi) {
    return std::make_shared<FileMetaData>(opts_, dir_, number, size, IKey(lo), IKey(hi), cache_);
  }

  std::string dir_;
  Options opts_;
  std::shared_ptr<BlockCache> cache_;
};

TEST_F(VersionTest, FreshThenPersistThenRecover) {
  {
    VersionSet vs(dir_, opts_, cache_);
    bool fresh;
    ASSERT_TRUE(vs.Recover(&fresh).ok());
    EXPECT_TRUE(fresh);
    VersionEdit e;
    e.AddFile(0, Meta(5, 1000, "a", "m"));
    e.AddFile(1, Meta(6, 2000, "b", "c"));
    e.AddFile(1, Meta(7, 2000, "x", "z"));
    e.SetLogNumber(8);
    vs.SetLastSequence(1234);
    ASSERT_TRUE(vs.LogAndApply(&e).ok());
    EXPECT_TRUE(fs::exists(ManifestFileName(dir_)));
  }
  VersionSet vs(dir_, opts_, cache_);
  bool fresh;
  ASSERT_TRUE(vs.Recover(&fresh).ok());
  EXPECT_FALSE(fresh);
  EXPECT_EQ(1234u, vs.LastSequence());
  EXPECT_EQ(8u, vs.LogNumber());
  EXPECT_EQ(1, vs.NumLevelFiles(0));
  EXPECT_EQ(2, vs.NumLevelFiles(1));
  // L1 sorted by smallest key.
  EXPECT_EQ("b", vs.current()->files(1)[0]->smallest_user_key().ToString());
  EXPECT_EQ("x", vs.current()->files(1)[1]->smallest_user_key().ToString());
}

TEST_F(VersionTest, CorruptManifestIsDetected) {
  {
    VersionSet vs(dir_, opts_, cache_);
    bool fresh;
    ASSERT_TRUE(vs.Recover(&fresh).ok());
    VersionEdit e;
    ASSERT_TRUE(vs.LogAndApply(&e).ok());
  }
  // Flip a byte in the body.
  const std::string path = ManifestFileName(dir_);
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  f.seekp(9);
  f.put('\xff');
  f.close();
  VersionSet vs(dir_, opts_, cache_);
  bool fresh;
  EXPECT_TRUE(vs.Recover(&fresh).IsCorruption());
}

TEST_F(VersionTest, L0CompactionTriggersOnFileCount) {
  VersionSet vs(dir_, opts_, cache_);
  bool fresh;
  ASSERT_TRUE(vs.Recover(&fresh).ok());
  VersionEdit e;
  for (int i = 0; i < opts_.l0_compaction_trigger - 1; ++i) e.AddFile(0, Meta(10 + i, 100, "a", "z"));
  ASSERT_TRUE(vs.LogAndApply(&e).ok());
  EXPECT_FALSE(vs.NeedsCompaction());
  VersionEdit e2;
  e2.AddFile(0, Meta(99, 100, "a", "z"));
  e2.AddFile(1, Meta(50, 100, "m", "n"));  // overlaps -> becomes inputs[1]
  e2.AddFile(1, Meta(51, 100, "zz", "zzz"));  // does not overlap
  ASSERT_TRUE(vs.LogAndApply(&e2).ok());
  EXPECT_TRUE(vs.NeedsCompaction());
  auto c = vs.PickCompaction();
  ASSERT_NE(nullptr, c);
  EXPECT_EQ(0, c->level());
  EXPECT_EQ(opts_.l0_compaction_trigger, c->num_input_files(0));
  EXPECT_EQ(1, c->num_input_files(1));
  EXPECT_FALSE(c->IsTrivialMove());
}

TEST_F(VersionTest, LevelCompactionTriggersOnBytesAndTrivialMove) {
  opts_.level1_max_bytes = 1000;
  VersionSet vs(dir_, opts_, cache_);
  bool fresh;
  ASSERT_TRUE(vs.Recover(&fresh).ok());
  VersionEdit e;
  e.AddFile(1, Meta(10, 600, "a", "b"));
  e.AddFile(1, Meta(11, 600, "c", "d"));  // total 1200 > 1000
  ASSERT_TRUE(vs.LogAndApply(&e).ok());
  ASSERT_TRUE(vs.NeedsCompaction());
  auto c = vs.PickCompaction();
  ASSERT_NE(nullptr, c);
  EXPECT_EQ(1, c->level());
  EXPECT_EQ(1, c->num_input_files(0));
  EXPECT_EQ(0, c->num_input_files(1));
  EXPECT_TRUE(c->IsTrivialMove()) << "empty L2: file can just move";
  EXPECT_EQ(10000u, vs.MaxBytesForLevel(2));
}

TEST_F(VersionTest, IsBaseLevelForKey) {
  VersionSet vs(dir_, opts_, cache_);
  bool fresh;
  ASSERT_TRUE(vs.Recover(&fresh).ok());
  VersionEdit e;
  for (int i = 0; i < opts_.l0_compaction_trigger; ++i) e.AddFile(0, Meta(10 + i, 100, "a", "z"));
  e.AddFile(3, Meta(40, 100, "m", "p"));  // deeper level holds m..p
  ASSERT_TRUE(vs.LogAndApply(&e).ok());
  auto c = vs.PickCompaction();
  ASSERT_NE(nullptr, c);
  EXPECT_TRUE(c->IsBaseLevelForKey("b")) << "no deeper file has b: tombstone can go";
  EXPECT_FALSE(c->IsBaseLevelForKey("n")) << "L3 has n: tombstone must stay";
  EXPECT_TRUE(c->IsBaseLevelForKey("q"));
}

TEST_F(VersionTest, ObsoleteFileIsDeletedWhenLastRefDrops) {
  const std::string path = SstFileName(dir_, 77);
  {
    std::ofstream f(path);
    f << "pretend sstable";
  }
  ASSERT_TRUE(fs::exists(path));
  auto meta = Meta(77, 15, "a", "b");
  meta->MarkObsolete();
  auto extra_ref = meta;  // an "iterator" still using it
  meta.reset();
  EXPECT_TRUE(fs::exists(path)) << "still referenced";
  extra_ref.reset();
  EXPECT_FALSE(fs::exists(path)) << "last reference gone: unlinked";
}

}  // namespace epica
