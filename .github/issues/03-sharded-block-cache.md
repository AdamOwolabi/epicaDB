# Shard the LRU block cache to reduce lock contention

The block cache (`engine/src/cache.cc`) is a single LRU under one mutex. Under many concurrent readers this is a hot spot.

**Task:** split into N shards keyed by hash of the cache key, each with its own mutex and LRU; keep total capacity the same.

**Done when:** existing `engine/tests/cache_test.cc` passes, a multi-threaded test shows no regression, and `STATS` still reports a combined hit rate.
