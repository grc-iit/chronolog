#ifndef STORY_INGESTION_HANDLE_H
#define STORY_INGESTION_HANDLE_H

#include <mutex>
#include <deque>

//
// IngestionQueue is a funnel into the KeeperDataStore
// std::deque guarantees O(1) time for addidng elements and resizing 
// (vector of vectors implementation)

namespace chronolog
{

typedef std::deque <LogEvent> EventDeque;

class StoryIngestionHandle
{

public:
    StoryIngestionHandle(std::mutex &a_mutex, EventDeque*active, EventDeque*passive): ingestionMutex(a_mutex)
                                                                                      , activeDeque(active)
                                                                                      , passiveDeque(passive)
    {}

    ~StoryIngestionHandle() = default;

    EventDeque &getActiveDeque() const
    { return *activeDeque; }

    EventDeque &getPassiveDeque() const
    { return *passiveDeque; }

    void ingestEvent(LogEvent const &logEvent)
    {   // assume multiple service threads pushing events on ingestionQueue
        std::lock_guard <std::mutex> lock(ingestionMutex);
        activeDeque->push_back(logEvent);
    }

    void swapActiveDeque() //EventDeque * empty_deque, EventDeque * full_deque)
    {
        if(!passiveDeque->empty() || activeDeque->empty())
        { return; }

//INNA: check if atomic compare_and_swap will work here

        std::lock_guard <std::mutex> lock_guard(ingestionMutex);
        if(!passiveDeque->empty() || activeDeque->empty())
        { return; }

        EventDeque*full_deque = activeDeque;
        activeDeque = passiveDeque;
        passiveDeque = full_deque;
    }


private:
    std::mutex &ingestionMutex;
    EventDeque*activeDeque;
    EventDeque*passiveDeque;
};

}

#endif

