#ifndef CHRONOLOG_GRAPHER_AUTOGEN_METHODS_H_
#define CHRONOLOG_GRAPHER_AUTOGEN_METHODS_H_

#include <chimaera/chimaera.h>
#include <string>
#include <vector>

namespace chronolog::grapher {

namespace Method {
// Inherited methods
GLOBAL_CROSS_CONST chi::u32 kCreate = 0;
GLOBAL_CROSS_CONST chi::u32 kDestroy = 1;
GLOBAL_CROSS_CONST chi::u32 kMonitor = 9;

// grapher-specific methods
GLOBAL_CROSS_CONST chi::u32 kMergeChunks = 10;
GLOBAL_CROSS_CONST chi::u32 kFlushStory = 11;
GLOBAL_CROSS_CONST chi::u32 kGetMergeStatus = 12;

GLOBAL_CROSS_CONST chi::u32 kMaxMethodId = 13;

inline const std::vector<std::string>& GetMethodNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(kMaxMethodId);
    v[0] = "Create";
    v[1] = "Destroy";
    v[9] = "Monitor";
    v[10] = "MergeChunks";
    v[11] = "FlushStory";
    v[12] = "GetMergeStatus";
    return v;
  }();
  return names;
}
}  // namespace Method

}  // namespace chronolog::grapher

#endif  // CHRONOLOG_GRAPHER_AUTOGEN_METHODS_H_
