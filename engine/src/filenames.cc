// filenames.cc -- see filenames.h.

#include "filenames.h"

#include <cstdio>

namespace epica {

std::string ManifestFileName(const std::string& dir) { return dir + "/MANIFEST"; }
std::string LockFileName(const std::string& dir) { return dir + "/LOCK"; }
std::string SstDirName(const std::string& dir) { return dir + "/sst"; }

std::string SstFileName(const std::string& dir, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%012llu.sst", static_cast<unsigned long long>(number));
  return SstDirName(dir) + "/" + buf;
}

bool ParseSstFileName(const std::string& name, uint64_t* number) {
  const std::string suffix = ".sst";
  if (name.size() <= suffix.size()) return false;
  if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
  uint64_t v = 0;
  for (size_t i = 0; i + suffix.size() < name.size(); ++i) {
    const char c = name[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = v;
  return true;
}

}  // namespace epica
