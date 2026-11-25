#ifndef CHRONOLOG_IN_MEMORY_CATALOG_STORE_H
#define CHRONOLOG_IN_MEMORY_CATALOG_STORE_H

#include <map>
#include <mutex>
#include <string>

#include "VisorCatalogStore.h"
#include "chronolog_errcode.h"

namespace chronolog
{

// A VisorCatalogStore that keeps its rows in memory, for unit tests. It follows
// the SQLite store's status codes: a story names its chronicle by id, and load
// reports the chronicle's stored name.
class InMemoryCatalogStore: public VisorCatalogStore
{
public:
    int open() override { return CL_SUCCESS; }

    int load(CatalogSnapshot& out) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = CatalogSnapshot();
        CatalogSnapshot snapshot;
        for(auto const& [id, row]: chronicles_)
        {
            CatalogChronicle chronicle{id, row.name, row.destroyPending};
            if(!catalogIdsMatch(chronicle))
                return CL_ERR_INVALID_CONF;
            snapshot.chronicles.push_back(chronicle);
        }
        for(auto const& [id, row]: stories_)
        {
            auto chronicle = chronicles_.find(row.chronicleId);
            if(chronicle == chronicles_.end())
                return CL_ERR_INVALID_CONF;
            CatalogStory story{id, chronicle->second.name, row.name, row.destroyPending};
            if(!catalogIdsMatch(story))
                return CL_ERR_INVALID_CONF;
            snapshot.stories.push_back(story);
        }
        out = std::move(snapshot);
        return CL_SUCCESS;
    }

    int insertChronicle(ChronicleId id, std::string const& name) override
    {
        if(!catalogIdsMatch(CatalogChronicle{id, name}))
            return CL_ERR_INVALID_ARG;
        std::lock_guard<std::mutex> lock(mutex_);
        if(chronicles_.count(id) != 0)
            return CL_ERR_CHRONICLE_EXISTS;
        for(auto const& entry: chronicles_)
            if(entry.second.name == name)
                return CL_ERR_CHRONICLE_EXISTS;
        chronicles_[id] = ChronicleRow{name, false};
        return CL_SUCCESS;
    }

    int
    insertStory(StoryId id, ChronicleId chronicle_id, std::string const& chronicle, std::string const& story) override
    {
        if(!catalogIdsMatch(chronicle_id, CatalogStory{id, chronicle, story}))
            return CL_ERR_INVALID_ARG;
        std::lock_guard<std::mutex> lock(mutex_);
        if(stories_.count(id) != 0)
            return CL_ERR_STORY_EXISTS;
        for(auto const& entry: stories_)
            if(entry.second.chronicleId == chronicle_id && entry.second.name == story)
                return CL_ERR_STORY_EXISTS;
        if(chronicles_.count(chronicle_id) == 0)
            return CL_ERR_NOT_EXIST;
        stories_[id] = StoryRow{chronicle_id, story, false};
        return CL_SUCCESS;
    }

    int deleteStory(StoryId id) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stories_.erase(id) != 0 ? CL_SUCCESS : CL_ERR_NOT_EXIST;
    }

    int deleteChronicle(ChronicleId id) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(chronicles_.erase(id) == 0)
            return CL_ERR_NOT_EXIST;
        for(auto it = stories_.begin(); it != stories_.end();)
            it = it->second.chronicleId == id ? stories_.erase(it) : std::next(it);
        return CL_SUCCESS;
    }

    int setStoryDestroyPending(StoryId id, bool pending) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = stories_.find(id);
        if(it == stories_.end())
            return CL_ERR_NOT_EXIST;
        it->second.destroyPending = pending;
        return CL_SUCCESS;
    }

    int setChronicleDestroyPending(ChronicleId id, bool pending) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = chronicles_.find(id);
        if(it == chronicles_.end())
            return CL_ERR_NOT_EXIST;
        it->second.destroyPending = pending;
        return CL_SUCCESS;
    }

private:
    struct ChronicleRow
    {
        std::string name;
        bool destroyPending;
    };
    struct StoryRow
    {
        ChronicleId chronicleId;
        std::string name;
        bool destroyPending;
    };

    std::mutex mutex_;
    std::map<ChronicleId, ChronicleRow> chronicles_;
    std::map<StoryId, StoryRow> stories_;
};

} // namespace chronolog

#endif // CHRONOLOG_IN_MEMORY_CATALOG_STORE_H
