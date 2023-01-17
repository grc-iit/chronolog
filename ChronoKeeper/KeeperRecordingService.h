#ifndef KEEPER_RECORDING_SERVICE_H
#define KEEPER_RECORDING_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include "KeeperIdCard.h"
#include "LogIngestionQueue.h"

namespace tl = thallium;


namespace chronolog
{

//#INNA :  for now assume all identifiers are uint64_t , revisit later 
typedef uint64_t StoryId;
typedef uint64_t StorytellerId;


class KeeperRecordingService : public tl::provider<KeeperRecordingService> 
{
public:
    
// KeeperRecordingService should be created on the heap not the stack thus the constructor is private...

    static KeeperRecordingService * CreateKeeperRecordingService(tl::engine& tl_engine, uint16_t service_provider_id
		    , LogIngestionQueue & ingestion_queue)
    {
          return  new KeeperRecordingService( tl_engine, service_provider_id, ingestion_queue );
    }  

    //INNA: TODO: finalize provider through callback
    ~KeeperRecordingService() 
    {
        std::cout<<"KeeperRecordingService::destructor"<<std::endl;
        get_engine().wait_for_finalize();
    }

private:

    void record_event(tl::request const& request, 
                       StorytellerId teller_id,  StoryId story_id, 
		       uint64_t timeStamp, const std::string& record) 
    {
        std::cout << "recording {"<< teller_id<<":"<<story_id<<":"<< record <<"}"<< std::endl;
	theIngestionQueue.ingestLogEvent(LogEvent(story_id,teller_id,timeStamp,record));
        request.respond( static_cast<int>(record.size()) );
    }

    void record_event_no_response( StorytellerId teller_id,  StoryId story_id, 
		       uint64_t timeStamp, const std::string& record) 
    {
        std::cout << "recording {"<< teller_id<<":"<<story_id<<":"<< record <<"}"<< std::endl;
	theIngestionQueue.ingestLogEvent(LogEvent(story_id,teller_id,timeStamp,record));
    }


    KeeperRecordingService(tl::engine& tl_engine, uint16_t service_provider_id, LogIngestionQueue & ingestion_queue)
    	: tl::provider<KeeperRecordingService>(tl_engine, service_provider_id)
	, theIngestionQueue(ingestion_queue)  
    {
        define("record_event", &KeeperRecordingService::record_event);
        define("record_event_no_response", &KeeperRecordingService::record_event_no_response,tl::ignore_return_value() );
    }

    KeeperRecordingService( KeeperRecordingService const&) = delete;
    KeeperRecordingService & operator= (KeeperRecordingService const&) = delete;

    LogIngestionQueue & theIngestionQueue;
};

}// namespace chronolog

#endif
