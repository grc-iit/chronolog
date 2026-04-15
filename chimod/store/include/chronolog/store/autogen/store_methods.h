#ifndef CHRONOLOG_STORE_AUTOGEN_METHODS_H_
#define CHRONOLOG_STORE_AUTOGEN_METHODS_H_

#include <chimaera/chimaera.h>
#include <string>
#include <vector>

namespace chronolog::store {

namespace Method {
// Inherited methods
GLOBAL_CROSS_CONST chi::u32 kCreate = 0;
GLOBAL_CROSS_CONST chi::u32 kDestroy = 1;
GLOBAL_CROSS_CONST chi::u32 kMonitor = 9;

// store-specific methods
GLOBAL_CROSS_CONST chi::u32 kArchiveChunk = 10;
GLOBAL_CROSS_CONST chi::u32 kReadChunk = 11;
GLOBAL_CROSS_CONST chi::u32 kListArchives = 12;
GLOBAL_CROSS_CONST chi::u32 kDeleteArchive = 13;
GLOBAL_CROSS_CONST chi::u32 kCompactArchive = 14;

GLOBAL_CROSS_CONST chi::u32 kMaxMethodId = 15;

inline const std::vector<std::string>& GetMethodNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(kMaxMethodId);
    v[0] = "Create";
    v[1] = "Destroy";
    v[9] = "Monitor";
    v[10] = "ArchiveChunk";
    v[11] = "ReadChunk";
    v[12] = "ListArchives";
    v[13] = "DeleteArchive";
    v[14] = "CompactArchive";
    return v;
  }();
  return names;
}
}  // namespace Method

}  // namespace chronolog::store

#endif  // CHRONOLOG_STORE_AUTOGEN_METHODS_H_
