// version.cc -- see version.h.

#include "version.h"

#include <algorithm>
#include <cstdio>

#include "coding.h"
#include "crc32c.h"
#include "env.h"
#include "filenames.h"

namespace epica {

// ---------------------------------------------------------------------------
// FileMetaData

FileMetaData::FileMetaData(const Options& options, std::string dir, uint64_t number,
                           uint64_t file_size, std::string smallest, std::string largest,
                           std::shared_ptr<BlockCache> cache)
    : options_(options),
      path_(SstFileName(dir, number)),
      number_(number),
      file_size_(file_size),
      smallest_(std::move(smallest)),
      largest_(std::move(largest)),
      cache_(std::move(cache)) {}

// This is where physical file deletion happens. A file is unlinked only when
// (a) a newer Version no longer lists it AND (b) every reader that was using
// the old Version has finished. shared_ptr gives us (b) for free.
FileMetaData::~FileMetaData() {
  table_.reset();
  if (obsolete_) {
    if (cache_) cache_->EraseFile(number_);
    DeleteFile(path_);  // best effort; a leftover is cleaned on next Open
  }
}

Status FileMetaData::GetTable(Table** out) {
  std::lock_guard<std::mutex> l(open_mu_);
  if (!table_ && open_status_.ok()) {
    open_status_ = Table::Open(options_, path_, number_, cache_, &table_);
  }
  *out = table_.get();
  return open_status_;
}

Iterator* FileMetaData::NewIterator() {
  Table* t;
  Status s = GetTable(&t);
  if (!s.ok()) return NewErrorIterator(s);
  return t->NewIterator();
}

// ---------------------------------------------------------------------------
// Version

namespace {

struct Saver {
  enum State { kNotFound, kFound, kDeleted, kCorrupt } state = kNotFound;
  Slice user_key;
  std::string* value;
};

// Callback for Table::InternalGet. The table hands us the first entry at or
// after our lookup key; we decide whether it is actually our key.
void SaveValue(void* arg, Slice ikey, Slice v) {
  auto* s = static_cast<Saver*>(arg);
  ParsedInternalKey parsed;
  if (!ParseInternalKey(ikey, &parsed)) {
    s->state = Saver::kCorrupt;
    return;
  }
  if (parsed.user_key == s->user_key) {
    s->state = (parsed.type == kTypeValue) ? Saver::kFound : Saver::kDeleted;
    if (s->state == Saver::kFound) s->value->assign(v.data(), v.size());
  }
}

bool AfterFile(Slice user_key, const FileMetaData& f) {
  return user_key.compare(f.largest_user_key()) > 0;
}
bool BeforeFile(Slice user_key, const FileMetaData& f) {
  return user_key.compare(f.smallest_user_key()) < 0;
}

}  // namespace

Status Version::Get(const LookupKey& key, std::string* value) const {
  const Slice ikey = key.internal_key();
  const Slice user_key = key.user_key();
  InternalKeyComparator icmp;

  Saver saver;
  saver.user_key = user_key;
  saver.value = value;

  for (int level = 0; level < num_levels(); ++level) {
    const FileList& files = files_[static_cast<size_t>(level)];
    if (files.empty()) continue;

    // Which files could hold the key?
    FileList candidates;
    if (level == 0) {
      // L0 files overlap; check every one whose range covers the key. They
      // are already sorted newest-first.
      for (const auto& f : files) {
        if (!BeforeFile(user_key, *f) && !AfterFile(user_key, *f)) candidates.push_back(f);
      }
    } else {
      // Disjoint, sorted: binary search for the first file whose largest
      // key is >= ikey.
      auto it = std::lower_bound(files.begin(), files.end(), ikey,
                                 [&](const std::shared_ptr<FileMetaData>& f, Slice k) {
                                   return icmp.Compare(f->largest(), k) < 0;
                                 });
      if (it != files.end() && !BeforeFile(user_key, **it)) candidates.push_back(*it);
    }

    for (const auto& f : candidates) {
      Table* t;
      Status s = f->GetTable(&t);
      if (!s.ok()) return s;
      s = t->InternalGet(ikey, &saver, SaveValue);
      if (!s.ok()) return s;
      switch (saver.state) {
        case Saver::kNotFound: break;  // keep looking in older data
        case Saver::kFound: return Status::Ok();
        case Saver::kDeleted: return Status::NotFound();
        case Saver::kCorrupt: return Status::Corruption("corrupt key in table " + std::to_string(f->number()));
      }
    }
  }
  return Status::NotFound();
}

void Version::AddIterators(std::vector<Iterator*>* out) const {
  for (const auto& level : files_) {
    for (const auto& f : level) out->push_back(f->NewIterator());
  }
}

void Version::GetOverlappingInputs(int level, Slice begin, Slice end, FileList* out) const {
  out->clear();
  for (const auto& f : files_[static_cast<size_t>(level)]) {
    if (!begin.empty() && AfterFile(begin, *f)) continue;   // file entirely before range
    if (!end.empty() && BeforeFile(end, *f)) continue;      // file entirely after range
    out->push_back(f);
  }
}

int64_t Version::TotalBytes(int level) const {
  int64_t sum = 0;
  for (const auto& f : files_[static_cast<size_t>(level)]) sum += static_cast<int64_t>(f->file_size());
  return sum;
}

std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < num_levels(); ++level) {
    const auto& files = files_[static_cast<size_t>(level)];
    if (files.empty()) continue;
    r += "--- level " + std::to_string(level) + " ---\n";
    for (const auto& f : files) {
      r += "  #" + std::to_string(f->number()) + " " + std::to_string(f->file_size()) + " bytes [" +
           f->smallest_user_key().ToString() + " .. " + f->largest_user_key().ToString() + "]\n";
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// Compaction

bool Compaction::IsTrivialMove() const {
  return num_input_files(0) == 1 && num_input_files(1) == 0;
}

bool Compaction::IsBaseLevelForKey(Slice user_key) {
  for (int lvl = level_ + 2; lvl < input_version_->num_levels(); ++lvl) {
    const FileList& files = input_version_->files(lvl);
    size_t& ptr = level_ptrs_[static_cast<size_t>(lvl)];
    while (ptr < files.size()) {
      const FileMetaData& f = *files[ptr];
      if (user_key.compare(f.largest_user_key()) <= 0) {
        if (user_key.compare(f.smallest_user_key()) >= 0) return false;  // overlaps
        break;  // key is before this file; later files are even further right
      }
      ++ptr;  // key is after this file, and all later keys will be too
    }
  }
  return true;
}

void Compaction::AddInputDeletions(VersionEdit* edit) const {
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : inputs_[which]) edit->RemoveFile(level_ + which, f->number());
  }
}

// ---------------------------------------------------------------------------
// VersionSet

VersionSet::VersionSet(std::string dir, const Options& options, std::shared_ptr<BlockCache> cache)
    : dir_(std::move(dir)),
      options_(options),
      cache_(std::move(cache)),
      current_(std::make_shared<Version>(options.num_levels)),
      compact_pointer_(static_cast<size_t>(options.num_levels)) {}

uint64_t VersionSet::MaxBytesForLevel(int level) const {
  // L0 is governed by file count, not bytes. L1 = base; each level after
  // that is 10x the previous.
  uint64_t result = options_.level1_max_bytes;
  for (int l = 1; l < level; ++l) result *= 10;
  return result;
}

void VersionSet::SortLevel(FileList* files, int level) const {
  if (level == 0) {
    // Newest first, so Get() consults the freshest L0 file before older ones.
    std::sort(files->begin(), files->end(),
              [](const auto& a, const auto& b) { return a->number() > b->number(); });
  } else {
    std::sort(files->begin(), files->end(), [&](const auto& a, const auto& b) {
      return icmp_.Compare(a->smallest(), b->smallest()) < 0;
    });
  }
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  if (edit->has_log_number) log_number_ = edit->log_number;

  // Build the new Version = current - deleted + added.
  auto v = std::make_shared<Version>(options_.num_levels);
  std::vector<std::shared_ptr<FileMetaData>> removed;
  for (int level = 0; level < options_.num_levels; ++level) {
    FileList& dst = v->files_[static_cast<size_t>(level)];
    for (const auto& f : current_->files(level)) {
      if (edit->deleted_files.count({level, f->number()})) {
        removed.push_back(f);
      } else {
        dst.push_back(f);
      }
    }
    for (const auto& [lvl, f] : edit->new_files) {
      if (lvl == level) dst.push_back(f);
    }
    SortLevel(&dst, level);
  }

  // Persist first. If this fails the in-memory state is unchanged and the
  // new files are simply orphans that DeleteObsoleteFiles removes later.
  std::shared_ptr<Version> saved = current_;
  current_ = v;
  Status s = WriteManifest();
  if (!s.ok()) {
    current_ = saved;
    return s;
  }
  // Now that no future Version lists them, the removed files may be deleted
  // once their last reader goes away. Exception: a file that was "deleted"
  // from one level and "added" to another (trivial move) is still live.
  std::set<uint64_t> still_live;
  for (const auto& [lvl, f] : edit->new_files) still_live.insert(f->number());
  for (auto& f : removed) {
    if (!still_live.count(f->number())) f->MarkObsolete();
  }
  return Status::Ok();
}

Status VersionSet::WriteManifest() {
  std::string body;
  PutVarint64(&body, next_file_number_);
  PutVarint64(&body, last_sequence_);
  PutVarint64(&body, log_number_);
  PutVarint32(&body, static_cast<uint32_t>(options_.num_levels));
  for (int level = 0; level < options_.num_levels; ++level) {
    const FileList& files = current_->files(level);
    PutVarint32(&body, static_cast<uint32_t>(files.size()));
    for (const auto& f : files) {
      PutVarint64(&body, f->number());
      PutVarint64(&body, f->file_size());
      PutLengthPrefixedSlice(&body, f->smallest());
      PutLengthPrefixedSlice(&body, f->largest());
    }
  }
  std::string out;
  PutFixed32(&out, crc32c::Value(body.data(), body.size()));
  PutFixed32(&out, static_cast<uint32_t>(body.size()));
  out += body;
  return WriteFileAtomically(ManifestFileName(dir_), out);
}

Status VersionSet::ReadManifest(const std::string& contents) {
  if (contents.size() < 8) return Status::Corruption("MANIFEST too short");
  const uint32_t crc = DecodeFixed32(contents.data());
  const uint32_t len = DecodeFixed32(contents.data() + 4);
  if (contents.size() != 8 + len) return Status::Corruption("MANIFEST length mismatch");
  if (crc32c::Value(contents.data() + 8, len) != crc) return Status::Corruption("MANIFEST crc mismatch");

  Slice in(contents.data() + 8, len);
  uint32_t num_levels;
  if (!GetVarint64(&in, &next_file_number_) || !GetVarint64(&in, &last_sequence_) ||
      !GetVarint64(&in, &log_number_) || !GetVarint32(&in, &num_levels)) {
    return Status::Corruption("MANIFEST header");
  }
  if (static_cast<int>(num_levels) != options_.num_levels) {
    return Status::InvalidArgument("MANIFEST has " + std::to_string(num_levels) +
                                   " levels but options.num_levels is " +
                                   std::to_string(options_.num_levels));
  }
  auto v = std::make_shared<Version>(options_.num_levels);
  for (int level = 0; level < options_.num_levels; ++level) {
    uint32_t count;
    if (!GetVarint32(&in, &count)) return Status::Corruption("MANIFEST level count");
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t number, size;
      Slice smallest, largest;
      if (!GetVarint64(&in, &number) || !GetVarint64(&in, &size) ||
          !GetLengthPrefixedSlice(&in, &smallest) || !GetLengthPrefixedSlice(&in, &largest)) {
        return Status::Corruption("MANIFEST file entry");
      }
      v->files_[static_cast<size_t>(level)].push_back(std::make_shared<FileMetaData>(
          options_, dir_, number, size, smallest.ToString(), largest.ToString(), cache_));
    }
    SortLevel(&v->files_[static_cast<size_t>(level)], level);
  }
  current_ = v;
  return Status::Ok();
}

Status VersionSet::Recover(bool* fresh) {
  const std::string path = ManifestFileName(dir_);
  if (!FileExists(path)) {
    *fresh = true;
    return Status::Ok();
  }
  *fresh = false;
  std::string contents;
  Status s = ReadFileToString(path, &contents);
  if (!s.ok()) return s;
  return ReadManifest(contents);
}

int VersionSet::PickLevelToCompact() const {
  int level = -1;
  double best_score = -1;
  for (int l = 0; l + 1 < options_.num_levels; ++l) {  // the last level never compacts
    double score;
    if (l == 0) {
      // L0 is scored by file count: each file is a full overlapping run.
      score = static_cast<double>(current_->files(0).size()) /
              static_cast<double>(options_.l0_compaction_trigger);
    } else {
      // Deeper levels are scored by bytes vs. their budget.
      score = static_cast<double>(current_->TotalBytes(l)) /
              static_cast<double>(MaxBytesForLevel(l));
    }
    if (score > best_score) {
      best_score = score;
      level = l;
    }
  }
  return best_score >= 1.0 ? level : -1;
}

std::unique_ptr<Compaction> VersionSet::PickCompaction() { return PickCompactionForLevel(-1); }

// If `forced_level` >= 0, compacts that level regardless of score (used by
// CompactAll). Otherwise picks the level with the highest score >= 1.
std::unique_ptr<Compaction> VersionSet::PickCompactionForLevel(int forced_level) {
  int level = forced_level;
  if (level < 0) {
    level = PickLevelToCompact();
    if (level < 0) return nullptr;
  }
  if (level < 0 || level + 1 >= options_.num_levels) return nullptr;
  const FileList& files = current_->files(level);
  if (files.empty()) return nullptr;

  std::unique_ptr<Compaction> c(new Compaction(level, current_));
  c->level_ptrs_.assign(static_cast<size_t>(options_.num_levels), 0);

  if (level == 0) {
    // L0 files overlap, so take all of them; the result is one clean run.
    c->inputs_[0] = files;
  } else {
    // Round-robin: first file whose largest key is past the compact pointer.
    const std::string& ptr = compact_pointer_[static_cast<size_t>(level)];
    for (const auto& f : files) {
      if (ptr.empty() || icmp_.Compare(f->largest(), Slice(ptr)) > 0) {
        c->inputs_[0].push_back(f);
        break;
      }
    }
    if (c->inputs_[0].empty()) c->inputs_[0].push_back(files[0]);  // wrap around
  }
  SetupOtherInputs(c.get());
  return c;
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  // Key range covered by inputs[0].
  std::string smallest, largest;
  for (const auto& f : c->inputs_[0]) {
    if (smallest.empty() || icmp_.Compare(f->smallest(), Slice(smallest)) < 0) smallest = f->smallest().ToString();
    if (largest.empty() || icmp_.Compare(f->largest(), Slice(largest)) > 0) largest = f->largest().ToString();
  }
  current_->GetOverlappingInputs(c->level_ + 1, ExtractUserKey(Slice(smallest)),
                                 ExtractUserKey(Slice(largest)), &c->inputs_[1]);
  // Remember where to resume next time at this level.
  compact_pointer_[static_cast<size_t>(c->level_)] = largest;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) const {
  for (int level = 0; level < options_.num_levels; ++level) {
    for (const auto& f : current_->files(level)) live->insert(f->number());
  }
}

std::string VersionSet::LevelSummary() const {
  std::string r;
  for (int level = 0; level < options_.num_levels; ++level) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "  L%d: %3zu files, %10lld bytes", level,
                  current_->files(level).size(), static_cast<long long>(current_->TotalBytes(level)));
    r += buf;
    if (level > 0) {
      std::snprintf(buf, sizeof(buf), " (limit %lld)", static_cast<long long>(MaxBytesForLevel(level)));
      r += buf;
    }
    r += "\n";
  }
  return r;
}

// ---------------------------------------------------------------------------
// BuildTable

Status BuildTable(const Options& options, const std::string& dir, uint64_t number, Iterator* iter,
                  std::shared_ptr<BlockCache> cache, std::shared_ptr<FileMetaData>* meta) {
  meta->reset();
  iter->SeekToFirst();
  if (!iter->Valid()) return iter->status();

  const std::string path = SstFileName(dir, number);
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(path, &file);
  if (!s.ok()) return s;

  TableBuilder builder(options, file.get());
  std::string smallest = iter->key().ToString();
  std::string largest;
  for (; iter->Valid(); iter->Next()) {
    largest.assign(iter->key().data(), iter->key().size());
    builder.Add(iter->key(), iter->value());
  }
  s = iter->status();
  if (s.ok()) s = builder.Finish();
  if (s.ok()) s = file->Close();  // Sync + close: the table is durable
  if (s.ok()) s = SyncDirectory(SstDirName(dir));
  if (!s.ok()) {
    DeleteFile(path);
    return s;
  }
  *meta = std::make_shared<FileMetaData>(options, dir, number, builder.FileSize(), smallest, largest,
                                         std::move(cache));
  return Status::Ok();
}

}  // namespace epica
