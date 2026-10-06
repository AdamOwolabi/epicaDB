// table.cc -- SSTable build and read paths. See table.h for the layout.

#include "table.h"

#include "coding.h"
#include "crc32c.h"

namespace epica {

// ---------------------------------------------------------------------------
// TableBuilder

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : options_(options), file_(file) {
  if (options.bloom_bits_per_key > 0) {
    filter_policy_ = std::make_unique<BloomFilterPolicy>(options.bloom_bits_per_key);
  }
}

void TableBuilder::Add(Slice internal_key, Slice value) {
  if (!status_.ok()) return;

  // The index entry for the previous block is written lazily, once we know
  // the block is complete. Its key is that block's last key.
  if (pending_index_entry_) {
    std::string handle_enc;
    pending_handle_.EncodeTo(&handle_enc);
    index_block_.Add(last_key_, handle_enc);
    pending_index_entry_ = false;
  }

  if (filter_policy_) {
    Slice user_key = ExtractUserKey(internal_key);
    filter_key_starts_.push_back(filter_keys_flat_.size());
    filter_keys_flat_.append(user_key.data(), user_key.size());
  }

  last_key_.assign(internal_key.data(), internal_key.size());
  ++num_entries_;
  data_block_.Add(internal_key, value);

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) FlushDataBlock();
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) return;
  WriteRawBlock(data_block_.Finish(), &pending_handle_);
  pending_index_entry_ = true;
  data_block_.Reset();
}

void TableBuilder::WriteRawBlock(Slice contents, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = contents.size();
  status_ = file_->Append(contents);
  if (!status_.ok()) return;
  char trailer[4];
  EncodeFixed32(trailer, crc32c::Value(contents.data(), contents.size()));
  status_ = file_->Append(Slice(trailer, 4));
  if (status_.ok()) offset_ += contents.size() + 4;
}

Status TableBuilder::Finish() {
  FlushDataBlock();
  if (!status_.ok()) return status_;

  // Filter block.
  BlockHandle filter_handle;
  {
    std::string filter;
    if (filter_policy_) {
      std::vector<Slice> keys;
      keys.reserve(filter_key_starts_.size());
      for (size_t i = 0; i < filter_key_starts_.size(); ++i) {
        const size_t start = filter_key_starts_[i];
        const size_t end =
            (i + 1 < filter_key_starts_.size()) ? filter_key_starts_[i + 1] : filter_keys_flat_.size();
        keys.emplace_back(filter_keys_flat_.data() + start, end - start);
      }
      filter_policy_->CreateFilter(keys, &filter);
    }
    WriteRawBlock(filter, &filter_handle);
    if (!status_.ok()) return status_;
  }

  // Index block (flush the last pending entry first).
  if (pending_index_entry_) {
    std::string handle_enc;
    pending_handle_.EncodeTo(&handle_enc);
    index_block_.Add(last_key_, handle_enc);
    pending_index_entry_ = false;
  }
  BlockHandle index_handle;
  WriteRawBlock(index_block_.Finish(), &index_handle);
  if (!status_.ok()) return status_;

  // Footer: fixed size so a reader can find it at (file_size - kFooterSize).
  std::string footer;
  filter_handle.EncodeTo(&footer);
  index_handle.EncodeTo(&footer);
  footer.resize(2 * BlockHandle::kMaxEncodedLength);  // pad
  PutFixed64(&footer, kTableMagicNumber);
  status_ = file_->Append(footer);
  if (status_.ok()) offset_ += footer.size();
  return status_;
}

// ---------------------------------------------------------------------------
// Reading blocks

Status ReadBlockFromFile(const RandomAccessFile* file, const BlockHandle& handle,
                         std::unique_ptr<Block>* out) {
  std::string buf;
  Status s = file->Read(handle.offset, static_cast<size_t>(handle.size) + 4, &buf);
  if (!s.ok()) return s;
  const uint32_t expected = DecodeFixed32(buf.data() + handle.size);
  const uint32_t actual = crc32c::Value(buf.data(), static_cast<size_t>(handle.size));
  if (expected != actual) {
    return Status::Corruption("block checksum mismatch at offset " + std::to_string(handle.offset));
  }
  buf.resize(static_cast<size_t>(handle.size));
  out->reset(new Block(std::move(buf)));
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// Table

Status Table::Open(const Options& options, const std::string& path, uint64_t file_number,
                   std::shared_ptr<BlockCache> cache, std::unique_ptr<Table>* out) {
  std::unique_ptr<RandomAccessFile> file;
  Status s = RandomAccessFile::Open(path, &file);
  if (!s.ok()) return s;
  if (file->size() < kFooterSize) return Status::Corruption("file too short for footer: " + path);

  // Footer.
  std::string footer;
  s = file->Read(file->size() - kFooterSize, kFooterSize, &footer);
  if (!s.ok()) return s;
  if (DecodeFixed64(footer.data() + kFooterSize - 8) != kTableMagicNumber) {
    return Status::Corruption("bad table magic number: " + path);
  }
  Slice in(footer.data(), kFooterSize - 8);
  BlockHandle filter_handle, index_handle;
  s = filter_handle.DecodeFrom(&in);
  if (s.ok()) s = index_handle.DecodeFrom(&in);
  if (!s.ok()) return s;

  std::unique_ptr<Table> t(new Table());
  t->file_number_ = file_number;
  t->cache_ = std::move(cache);

  s = ReadBlockFromFile(file.get(), index_handle, &t->index_block_);
  if (!s.ok()) return s;

  // Filter block is raw bytes (not an entry block); read + verify manually.
  if (filter_handle.size > 0) {
    std::string buf;
    s = file->Read(filter_handle.offset, static_cast<size_t>(filter_handle.size) + 4, &buf);
    if (!s.ok()) return s;
    if (DecodeFixed32(buf.data() + filter_handle.size) !=
        crc32c::Value(buf.data(), static_cast<size_t>(filter_handle.size))) {
      return Status::Corruption("filter block checksum mismatch: " + path);
    }
    buf.resize(static_cast<size_t>(filter_handle.size));
    t->filter_data_ = std::move(buf);
    t->filter_policy_ = std::make_unique<BloomFilterPolicy>(options.bloom_bits_per_key > 0
                                                                ? options.bloom_bits_per_key
                                                                : 10);
  }
  t->file_ = std::move(file);
  *out = std::move(t);
  return Status::Ok();
}

Status Table::ReadDataBlock(const BlockHandle& handle, std::shared_ptr<Block>* out) const {
  if (cache_) {
    BlockCache::Key key{file_number_, handle.offset};
    if (auto hit = cache_->Lookup(key)) {
      *out = std::move(hit);
      return Status::Ok();
    }
    std::unique_ptr<Block> b;
    Status s = ReadBlockFromFile(file_.get(), handle, &b);
    if (!s.ok()) return s;
    *out = std::shared_ptr<Block>(std::move(b));
    cache_->Insert(key, *out);
    return Status::Ok();
  }
  std::unique_ptr<Block> b;
  Status s = ReadBlockFromFile(file_.get(), handle, &b);
  if (!s.ok()) return s;
  *out = std::shared_ptr<Block>(std::move(b));
  return Status::Ok();
}

Status Table::InternalGet(Slice internal_key, void* arg,
                          void (*handle)(void* arg, Slice k, Slice v)) const {
  // Step 1: bloom filter -- may let us skip the file entirely.
  if (filter_policy_ && !filter_data_.empty() &&
      !filter_policy_->KeyMayMatch(ExtractUserKey(internal_key), Slice(filter_data_))) {
    return Status::Ok();  // definitely not here
  }

  // Step 2: index block -> which data block could hold the key.
  std::unique_ptr<Iterator> index_iter(index_block_->NewIterator(&icmp_));
  index_iter->Seek(internal_key);
  if (!index_iter->Valid()) return index_iter->status();  // past the last block

  Slice handle_bytes = index_iter->value();
  BlockHandle bh;
  Status s = bh.DecodeFrom(&handle_bytes);
  if (!s.ok()) return s;

  // Step 3: fetch that block and scan.
  std::shared_ptr<Block> block;
  s = ReadDataBlock(bh, &block);
  if (!s.ok()) return s;
  std::unique_ptr<Iterator> it(block->NewIterator(&icmp_));
  it->Seek(internal_key);
  if (it->Valid()) (*handle)(arg, it->key(), it->value());
  return it->status();
}

// Two-level iterator: an outer cursor over the index block picks a data
// block; an inner cursor walks that block; when the inner one is exhausted
// we advance the outer one and load the next block.
class TableIterator : public Iterator {
 public:
  explicit TableIterator(const Table* table)
      : table_(table), index_iter_(table->index_block_->NewIterator(&table->icmp_)) {}

  bool Valid() const override { return data_iter_ && data_iter_->Valid(); }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_) data_iter_->SeekToFirst();
    SkipEmptyBlocksForward();
  }

  void Seek(Slice target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_) data_iter_->Seek(target);
    SkipEmptyBlocksForward();
  }

  void Next() override {
    data_iter_->Next();
    SkipEmptyBlocksForward();
  }

  Slice key() const override { return data_iter_->key(); }
  Slice value() const override { return data_iter_->value(); }
  Status status() const override {
    if (!status_.ok()) return status_;
    if (!index_iter_->status().ok()) return index_iter_->status();
    if (data_iter_ && !data_iter_->status().ok()) return data_iter_->status();
    return Status::Ok();
  }

 private:
  void InitDataBlock() {
    data_iter_.reset();
    block_.reset();
    if (!index_iter_->Valid()) return;
    Slice handle_bytes = index_iter_->value();
    BlockHandle bh;
    status_ = bh.DecodeFrom(&handle_bytes);
    if (!status_.ok()) return;
    status_ = table_->ReadDataBlock(bh, &block_);
    if (!status_.ok()) return;
    data_iter_.reset(block_->NewIterator(&table_->icmp_));
  }

  void SkipEmptyBlocksForward() {
    while (status_.ok() && (!data_iter_ || !data_iter_->Valid())) {
      if (!index_iter_->Valid()) {  // exhausted the whole table
        data_iter_.reset();
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_) data_iter_->SeekToFirst();
    }
  }

  const Table* table_;
  std::unique_ptr<Iterator> index_iter_;
  std::shared_ptr<Block> block_;  // keeps the block alive while data_iter_ points into it
  std::unique_ptr<Iterator> data_iter_;
  Status status_;
};

Iterator* Table::NewIterator() const { return new TableIterator(this); }

}  // namespace epica
