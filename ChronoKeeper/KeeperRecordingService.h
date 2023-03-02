#ifndef KEEPER_RECORDING_SERVICE_H
#define KEEPER_RECORDING_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include "KeeperIdCard.h"
#include "chronolog_types.h"
#include "IngestionQueue.h"

namespace tl = thallium;


namespace chronolog
{


class KeeperRecordingService : public tl::provider<KeeperRecordingService> 
{
public:
    
// KeeperRecordingService should be created on the heap not the stack thus the constructor is private...

    static KeeperRecordingService * CreateKeeperRecordingService(tl::engine& tl_engine, uint16_t service_provider_id
		    , IngestionQueue & ingestion_queue)
    {
          return  new KeeperRecordingService( tl_engine, service_provider_id, ingestion_queue );
    }  

    ~KeeperRecordingService() 
    {
        get_engine().pop_finalize_callback(this);
    }

private:

    void record_event(tl::request const& request, LogEvent const & log_event) 
                   //    ClientId teller_id,  StoryId story_id, 
		     //  ChronoTick const& chrono_tick, std::string const& record) 
    {
        std::cout << "recording {"<< log_event.clientId<<":"<<log_event.storyId<<":"<< log_event.logRecord <<"}"<< std::endl;
	theIngestionQueue.ingestLogEvent(log_event);
    }


    KeeperRecordingService(tl::engine& tl_engine, uint16_t service_provider_id, IngestionQueue & ingestion_queue)
    	: tl::provider<KeeperRecordingService>(tl_engine, service_provider_id)
	, theIngestionQueue(ingestion_queue)  
    {
        define("record_event", &KeeperRecordingService::record_event, tl::ignore_return_value());
	//set up callback for the case when the engine is being finalized while this provider is still alive
	get_engine().push_finalize_callback(this, [p=this](){delete p;} );
    }

    KeeperRecordingService( KeeperRecordingService const&) = delete;
    KeeperRecordingService & operator= (KeeperRecordingService const&) = delete;

    IngestionQueue & theIngestionQueue;
};

}// namespace chronolog

#endif
