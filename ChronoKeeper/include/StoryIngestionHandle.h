#ifndef STORY_INGESTION_HANDLE_H
#define STORY_INGESTION_HANDLE_H

#include <mutex>
#include <deque>

#include <chronolog_types.h>
#include <chronolog_profile.h>

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
        {
            CL_PROFILE_REGION("keeper_ingestion_handle_lock_wait");
            ingestionMutex.lock();
        }
        std::lock_guard<std::mutex> lock(ingestionMutex, std::adopt_lock);
        CL_PROFILE_REGION("keeper_ingestion_handle_lock_hold");
        CL_PROFILE_REGION("keeper_queue_push");
        activeDeque->push_back(logEvent);
        CL_PROFILE_COUNTER("keeper_active_queue_depth", activeDeque->size());
    }

    void swapActiveDeque() //EventDeque * empty_deque, EventDeque * full_deque)
    {
        if(!passiveDeque->empty() || activeDeque->empty())
        { return; }

//INNA: check if atomic compare_and_swap will work here

        {
            CL_PROFILE_REGION("keeper_ingestion_handle_lock_wait");
            ingestionMutex.lock();
        }
        std::lock_guard<std::mutex> lock_guard(ingestionMutex, std::adopt_lock);
        CL_PROFILE_REGION("keeper_ingestion_handle_lock_hold");
        if(!passiveDeque->empty() || activeDeque->empty())
        { return; }

        CL_PROFILE_REGION("keeper_queue_swap");
        CL_PROFILE_COUNTER("keeper_queue_swap_events", activeDeque->size());
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
