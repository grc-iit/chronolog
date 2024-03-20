#ifndef GRAPHER_RECORDING_SERVICE_H
#define GRAPHER_RECORDING_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chronolog_errcode.h"
#include "KeeperIdCard.h"
#include "chronolog_types.h"
#include "ChunkIngestionQueue.h"

namespace tl = thallium;

namespace chronolog
{
class GrapherRecordingService: public tl::provider <GrapherRecordingService>
{
public:
    // RecordingService should be created on the heap not the stack thus the constructor is private...
    static GrapherRecordingService*
    CreateRecordingService(tl::engine &tl_engine, uint16_t service_provider_id, ChunkIngestionQueue &ingestion_queue)
    {
        return new GrapherRecordingService(tl_engine, service_provider_id, ingestion_queue);
    }

    ~GrapherRecordingService()
    {
        LOG_DEBUG("[GrapherRecordingService] Destructor called. Cleaning up...");
        get_engine().pop_finalize_callback(this);
    }
/*
 INN: replace this method with chunk receptor method
    void on_chunk_received(tl::request const &request, LogEvent const &log_event)
    {
        //  ClientId teller_id,  StoryId story_id,
        //  ChronoTick const& chrono_tick, std::string const& record)
        std::stringstream ss;
        ss << log_event;
        LOG_DEBUG("[KeeperRecordingService] Recording event: {}", ss.str());
        theIngestionQueue.ingestLogEvent(log_event);
        request.respond(chronolog::CL_SUCCESS);
    }
*/
private:
    GrapherRecordingService(tl::engine &tl_engine, uint16_t service_provider_id, ChunkIngestionQueue &ingestion_queue)
            : tl::provider <GrapherRecordingService>(tl_engine, service_provider_id), theIngestionQueue(ingestion_queue)
    {
        //define("", &RecordingService::record_event, tl::ignore_return_value());
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });
    }

    GrapherRecordingService(GrapherRecordingService const &) = delete;

    GrapherRecordingService &operator=(GrapherRecordingService const &) = delete;

    ChunkIngestionQueue &theIngestionQueue;
};

}// namespace chronolog

#endif
