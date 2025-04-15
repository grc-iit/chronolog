
#include <chrono>
#include <iostream>
#include <signal.h>

#include "chrono_monitor.h"
#include "ServiceId.h"
#include "StoryChunk.h"
#include "StoryChunkTransferAgent.h"
#include "ArchiveReadingRequestQueue.h"
#include "PlaybackService.h"

namespace tl = thallium;
namespace chl = chronolog;

bool keep_running = true;
void sigterm_handler(int)
{
    std::cout<< "Received SIGTERM signal. Initiating shutdown procedure."<<std::endl;
    keep_running = false;
    return;
}

int main()
{

    signal(SIGTERM, sigterm_handler);

    int result = chronolog::chrono_monitor::initialize( "file" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
                                                    ,"/tmp/TransferAgent.log"  //, confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
                                                    , spdlog::level::debug// confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
                                                    , "CronoPlayer"//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
                                                    , 10000//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
                                                    , 2//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
                                                    , spdlog::level::debug//confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
                                                    );


    chl::ServiceId localServiceId("ofi+sockets", "127.0.0.1",5555,55);

    std::string LOCAL_SERVICE_NA_STRING;
    localServiceId.get_service_as_string(LOCAL_SERVICE_NA_STRING);

    tl::engine * localEngine = nullptr;
    chl::PlaybackService * playbackService = nullptr;

    chl::ServiceId queryServiceId("ofi+sockets", "127.0.0.1",5557,57);

    chl::ArchiveReadingRequestQueue readingRequestQueue;

    try
    {
        margo_instance_id margo_id = margo_init(LOCAL_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);
        localEngine = new tl::engine(margo_id);

        std::cout <<"started localEngine at "<< localEngine->self()<<std::endl;


        chl::PlaybackService * playbackService = chl::PlaybackService::CreatePlaybackService( *localEngine, localServiceId.getProviderId(),readingRequestQueue);
    }
    catch(tl::exception const &)
    {
        playbackService = nullptr;
    }

    if(nullptr == playbackService)
    {
        return(-1);
    }

 while( true ==  keep_running)
    {
/*
        std::cout<<"TransferAgent is running"<<std::endl;
        transferAgent->is_receiver_available();
        
        chl::StoryChunk storyChunk("Chronicle","Story", 1, std::chrono::high_resolution_clock::now().time_since_epoch().count()
                  ,  std::chrono::high_resolution_clock::now().time_since_epoch().count()+5000);
   
        for(int i =0; i< 5; ++i) 
        { 
            storyChunk.insertEvent(chl::LogEvent{ 1, storyChunk.getStartTime()+i, 2968,1,
                             "line "+std::to_string(i)});
        }
        std::cout<<"TransferAgent is sending storyChunk for story "<<storyChunk.getChronicleName()<<"-"<< storyChunk.getStoryName()<<"-"<<storyChunk.getStartTime()<<"-"<<storyChunk.getEndTime()<<" eventCount:"<<storyChunk.getEventCount()<<std::endl;
        transferAgent->processStoryChunk(&storyChunk);

  */      sleep(3);
    }

    std::cout << "Shutting down  TransferAgent for "<<chl::to_string(queryServiceId)<<std::endl;

    delete playbackService;
    delete localEngine;
return 1;
}
