#ifndef CHRONOLOG_PLAYER_AUTOGEN_METHODS_H_
#define CHRONOLOG_PLAYER_AUTOGEN_METHODS_H_

#include <chimaera/chimaera.h>
#include <string>
#include <vector>

namespace chronolog::player {

namespace Method {
// Inherited methods
GLOBAL_CROSS_CONST chi::u32 kCreate = 0;
GLOBAL_CROSS_CONST chi::u32 kDestroy = 1;
GLOBAL_CROSS_CONST chi::u32 kMonitor = 9;

// player-specific methods
GLOBAL_CROSS_CONST chi::u32 kReplayStory = 10;
GLOBAL_CROSS_CONST chi::u32 kReplayChronicle = 11;
GLOBAL_CROSS_CONST chi::u32 kGetStoryInfo = 12;

GLOBAL_CROSS_CONST chi::u32 kMaxMethodId = 13;

inline const std::vector<std::string>& GetMethodNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(kMaxMethodId);
    v[0] = "Create";
    v[1] = "Destroy";
    v[9] = "Monitor";
    v[10] = "ReplayStory";
    v[11] = "ReplayChronicle";
    v[12] = "GetStoryInfo";
    return v;
  }();
  return names;
}
}  // namespace Method

}  // namespace chronolog::player

#endif  // CHRONOLOG_PLAYER_AUTOGEN_METHODS_H_
