// filenames.h -- the on-disk directory layout of a database.
//
//   <dir>/
//     LOCK                     advisory lock so two processes cannot open it
//     MANIFEST                 which SSTables are live at each level
//     MANIFEST.tmp             transient during an atomic MANIFEST rewrite
//     wal/000000000007.log     write-ahead logs (see wal.h)
//     sst/000000000009.sst     SSTables (see table.h)
//
// WAL logs and SSTables draw numbers from the same counter (the MANIFEST's
// next_file_number), so a number identifies exactly one file of either kind
// and their relative age is obvious.
#pragma once

#include <cstdint>
#include <string>

namespace epica {

std::string ManifestFileName(const std::string& dir);
std::string LockFileName(const std::string& dir);
std::string SstDirName(const std::string& dir);
std::string SstFileName(const std::string& dir, uint64_t number);
// Parses "<number>.sst". Returns false for anything else.
bool ParseSstFileName(const std::string& name, uint64_t* number);

}  // namespace epica
