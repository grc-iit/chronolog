#ifndef CHRONOLOG_STORYCHUNK_TRANSFER_AGENT_H
#define CHRONOLOG_STORYCHUNK_TRANSFER_AGENT_H

#include <thallium.hpp>

#include "chrono_monitor.h"
#include "chronolog_types.h"

#include "StoryChunkExtractor.h"
#include "ServiceId.h"

namespace tl = thallium;

namespace chronolog
{

class StoryChunkTransferAgent: public StoryChunkExtractorBase
{
public:

    static StoryChunkTransferAgent *
    CreateStoryChunkTransferAgent(tl::engine &tl_engine , ServiceId const & receiver_service_id)
    {
        StoryChunkTransferAgent * storyChunkTransferAgent = nullptr;
        try
        {
            storyChunkTransferAgent = new StoryChunkTransferAgent(tl_engine, receiver_service_id);
        }
        catch(tl::exception const &ex)
        {
            LOG_ERROR("[StoryChunkTransferAgent] Failed to create StoryChunkTransferAgent for service");
        }
        return storyChunkTransferAgent;
    }


   ~StoryChunkTransferAgent();

    int processStoryChunk(StoryChunk*story_chunk) override;

private:
    tl::engine & service_engine;          // local tl::engine
    ServiceId   receiver_service_id;              // remote receiver service ServiceId
    tl::provider_handle receiver_service_handle;  // tl::provider_handle for remote receiver service
    tl::remote_procedure receiver_is_available;
    tl::remote_procedure receive_story_chunk;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    StoryChunkTransferAgent(tl::engine &tl_engine, ServiceId const& receiver_service_id);
};


}


#endif //CHRONOLOG_STORYCHUNK_SENDER_H
