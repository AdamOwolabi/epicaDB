// options.h -- every tunable knob in the engine, in one place.
//
// Three structs:
//   Options       -- fixed at DB::Open; sizes, thresholds, feature toggles.
//   WriteOptions  -- per write; today just `sync`.
//   ReadOptions   -- per read; today just which snapshot to read at.
//
// Defaults are chosen for a laptop SSD and for tests that finish quickly. The
// comment on each field says what it trades off, because "why this number?"
// is a common interview follow-up.
#pragma once

#include <cstddef>
#include <cstdint>

namespace epica {

class Snapshot;

struct Options {
  // Create the directory and empty DB if none exists. If false, Open fails
  // with InvalidArgument on a missing DB.
  bool create_if_missing = true;

  // Bytes of arena memory the active memtable may reach before it is frozen
  // and flushed to an SSTable. Bigger = fewer, larger L0 files and more RAM
  // at risk in a crash (the WAL replays it, but replay time grows).
  size_t memtable_size = 4 * 1024 * 1024;

  // Target uncompressed size of one SSTable data block. Reads fetch a whole
  // block, so this is the unit of I/O. 4 KiB matches a filesystem page.
  size_t block_size = 4 * 1024;

  // Bits per key in each SSTable's bloom filter. 10 bits/key gives about a
  // 1% false-positive rate with 7 hash probes. 0 disables filters.
  int bloom_bits_per_key = 10;

  // Bytes of decoded data blocks kept in the shared LRU block cache.
  size_t block_cache_size = 8 * 1024 * 1024;

  // Compaction writes output files of about this size, then starts a new one.
  uint64_t target_file_size = 2 * 1024 * 1024;

  // Number of L0 files that triggers an L0->L1 compaction. L0 files overlap
  // each other, so every Get may have to consult all of them; keep it small.
  int l0_compaction_trigger = 4;

  // When L0 reaches this many files, writers stall until a compaction
  // reduces it. Without this a fast writer could outrun compaction forever
  // and every Get would have to consult dozens of overlapping L0 files.
  int l0_stop_writes_trigger = 12;

  // Total bytes allowed in L1. Level n allows level1_max_bytes * 10^(n-1).
  uint64_t level1_max_bytes = 10 * 1024 * 1024;

  // Number of levels (L0 .. L(num_levels-1)).
  int num_levels = 7;

  // Bytes buffered in user space by the WAL writer before write(2).
  size_t wal_buffer_size = 64 * 1024;
};

struct WriteOptions {
  // If true, the WAL is fsync'd before the write returns; the write survives
  // power loss. If false, it survives a process crash but not power loss.
  // With group commit the cost of `sync` is shared across concurrent writers.
  bool sync = true;
};

struct ReadOptions {
  // If non-null, read the state as of this snapshot instead of the latest.
  const Snapshot* snapshot = nullptr;
};

}  // namespace epica
