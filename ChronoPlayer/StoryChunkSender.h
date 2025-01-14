#ifndef CHRONOLOG_STORYCHUNK_SENDER_H
#define CHRONOLOG_STORYCHUNK_SENDER_H

#include <thallium.hpp>

#include "chrono_monitor.h"
#include "chronolog_types.h"

#include "StoryChunkExtractor.h"
#include "ServiceId.h"

namespace tl = thallium;

namespace chronolog
{

class StoryChunkSender: public StoryChunkExtractorBase
{
public:

    static StoryChunkSender *
    CreateStoryChunkSender(tl::engine &tl_engine , ServiceId const & receiver_service_id)
    {
        StoryChunkSender * storyChunkSender = nullptr;
        try
        {
            storyChunkSender = new StoryChunkSender(tl_engine, receiver_service_id);
        }
        catch(tl::exception const &ex)
        {
            LOG_ERROR("[StoryChunkSender] Failed to create StoryChunkSender for service");
        }
        return storyChunkSender;
    }


   ~StoryChunkSender();

    int processStoryChunk(StoryChunk*story_chunk) override;

private:
    tl::engine & service_engine;          // local tl::engine
    ServiceId   receiver_service_id;              // remote receiver service ServiceId
    tl::provider_handle receiver_service_handle;  // tl::provider_handle for remote receiver service
    tl::remote_procedure receiver_is_available;
    tl::remote_procedure receive_story_chunk;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    StoryChunkSender(tl::engine &tl_engine, ServiceId const& receiver_service_id);
};


}


#endif //CHRONOLOG_STORYCHUNK_SENDER_H
