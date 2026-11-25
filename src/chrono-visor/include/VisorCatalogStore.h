#ifndef CHRONOLOG_VISOR_CATALOG_STORE_H
#define CHRONOLOG_VISOR_CATALOG_STORE_H

#include <string>
#include <vector>

#include <city.h>

#include "StoryIdOf.h"
#include "chronolog_types.h"

namespace chronolog
{

struct CatalogStory
{
    StoryId id;
    std::string chronicle;
    std::string story;
    bool destroyPending = false;
};

struct CatalogChronicle
{
    ChronicleId id;
    std::string name;
    bool destroyPending = false;
};

struct CatalogSnapshot
{
    std::vector<CatalogChronicle> chronicles;
    std::vector<CatalogStory> stories;
};

// The visor's persistent list of chronicles and stories. Every mutating call
// commits before it returns. A non-OK status means nothing changed.
//
// Status codes, the same for every implementation:
//   insertChronicle: CL_ERR_INVALID_ARG when the id is not catalogChronicleIdOf
//                    the name, CL_ERR_CHRONICLE_EXISTS for an id or name already
//                    stored;
//   insertStory:     CL_ERR_INVALID_ARG when the id is not storyIdOf the names
//                    or the chronicle id is not catalogChronicleIdOf the
//                    chronicle's name, CL_ERR_STORY_EXISTS for an id or
//                    (chronicle, name) already stored, CL_ERR_NOT_EXIST when the
//                    chronicle is not stored;
//   delete*, set*:   CL_ERR_NOT_EXIST when the row is not stored;
//   open, load:      CL_ERR_INVALID_CONF with the reason logged;
//   any other failure, such as a failed commit: CL_ERR_UNKNOWN.
class VisorCatalogStore
{
public:
    virtual ~VisorCatalogStore() = default;
    virtual int open() = 0;
    // Refuses the whole catalog when a stored id does not match its names or a
    // story's chronicle is not stored. On any failure out is left empty.
    virtual int load(CatalogSnapshot& out) = 0;
    virtual int insertChronicle(ChronicleId, std::string const& name) = 0;
    virtual int insertStory(StoryId, ChronicleId, std::string const& chronicle, std::string const& story) = 0;
    virtual int deleteStory(StoryId) = 0;
    // Deletes the chronicle and its stories in one transaction.
    virtual int deleteChronicle(ChronicleId) = 0;
    // The destroy was sent to the graphers and not every one confirmed it
    // (plan Amendment P6). Cleared by the delete.
    virtual int setStoryDestroyPending(StoryId, bool) = 0;
    virtual int setChronicleDestroyPending(ChronicleId, bool) = 0;
};

// Ids are the name hashes, as ChronicleMetaDirectory computes them.
inline ChronicleId catalogChronicleIdOf(std::string const& name) { return CityHash64(name.c_str(), name.length()); }

inline bool catalogIdsMatch(CatalogChronicle const& chronicle)
{
    return chronicle.id == catalogChronicleIdOf(chronicle.name);
}

inline bool catalogIdsMatch(CatalogStory const& story) { return story.id == storyIdOf(story.chronicle, story.story); }

// A story to insert under the chronicle with id chronicle_id.
inline bool catalogIdsMatch(ChronicleId chronicle_id, CatalogStory const& story)
{
    return chronicle_id == catalogChronicleIdOf(story.chronicle) && catalogIdsMatch(story);
}

} // namespace chronolog

#endif // CHRONOLOG_VISOR_CATALOG_STORE_H
