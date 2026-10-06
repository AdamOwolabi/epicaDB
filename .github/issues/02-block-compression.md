# Block compression for SSTables

Data blocks are written uncompressed. Add an optional compression type byte in the block trailer (none / snappy or lz4) and transparently decompress on read.

**Where:** `TableBuilder::WriteRawBlock` and `ReadBlockFromFile` in `engine/src/table.cc`, `Options` in `engine/include/epica/options.h`.

**Done when:** `Options::compression` is honoured, old uncompressed tables still open, CRC still covers the compressed bytes, round-trip test added in `engine/tests/table_test.cc`.
