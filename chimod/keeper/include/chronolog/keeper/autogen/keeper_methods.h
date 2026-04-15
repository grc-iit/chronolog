#ifndef CHRONOLOG_KEEPER_AUTOGEN_METHODS_H_
#define CHRONOLOG_KEEPER_AUTOGEN_METHODS_H_

#include <chimaera/chimaera.h>
#include <string>
#include <vector>

namespace chronolog::keeper {

namespace Method {
// Inherited methods
GLOBAL_CROSS_CONST chi::u32 kCreate = 0;
GLOBAL_CROSS_CONST chi::u32 kDestroy = 1;
GLOBAL_CROSS_CONST chi::u32 kMonitor = 9;

// keeper-specific methods
GLOBAL_CROSS_CONST chi::u32 kRecordEvent = 10;
GLOBAL_CROSS_CONST chi::u32 kRecordEventBatch = 11;
GLOBAL_CROSS_CONST chi::u32 kStartStoryRecording = 12;
GLOBAL_CROSS_CONST chi::u32 kStopStoryRecording = 13;
GLOBAL_CROSS_CONST chi::u32 kCreateChronicle = 14;
GLOBAL_CROSS_CONST chi::u32 kDestroyChronicle = 15;
GLOBAL_CROSS_CONST chi::u32 kAcquireStory = 16;
GLOBAL_CROSS_CONST chi::u32 kReleaseStory = 17;
GLOBAL_CROSS_CONST chi::u32 kShowChronicles = 18;
GLOBAL_CROSS_CONST chi::u32 kShowStories = 19;

GLOBAL_CROSS_CONST chi::u32 kMaxMethodId = 20;

inline const std::vector<std::string>& GetMethodNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(kMaxMethodId);
    v[0] = "Create";
    v[1] = "Destroy";
    v[9] = "Monitor";
    v[10] = "RecordEvent";
    v[11] = "RecordEventBatch";
    v[12] = "StartStoryRecording";
    v[13] = "StopStoryRecording";
    v[14] = "CreateChronicle";
    v[15] = "DestroyChronicle";
    v[16] = "AcquireStory";
    v[17] = "ReleaseStory";
    v[18] = "ShowChronicles";
    v[19] = "ShowStories";
    return v;
  }();
  return names;
}
}  // namespace Method

}  // namespace chronolog::keeper

#endif  // CHRONOLOG_KEEPER_AUTOGEN_METHODS_H_
