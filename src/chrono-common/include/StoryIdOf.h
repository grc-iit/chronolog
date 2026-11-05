#ifndef CHRONOLOG_STORY_ID_OF_H
#define CHRONOLOG_STORY_ID_OF_H

#include <cstdint>
#include <string>

#include <city.h>

namespace chronolog
{

// The StoryId of a chronicle's story. Server-internal: clients name stories,
// and only ever hand back the id the visor gave them. The chronicle name's
// length goes first, so the hashed string tells where the chronicle ends:
// plain concatenation gave ("ab", "c") and ("a", "bc") one id.
//
// The visor assigns ids with this function and the keeper recomputes them from
// names during WAL recovery, so both must call this one.
inline uint64_t storyIdOf(std::string const& chronicle_name, std::string const& story_name)
{
    std::string const story_name_for_hash = std::to_string(chronicle_name.length()) + ":" + chronicle_name + story_name;
    return CityHash64(story_name_for_hash.c_str(), story_name_for_hash.length());
}

} // namespace chronolog

#endif // CHRONOLOG_STORY_ID_OF_H
