#include <unistd.h>
#include <map>
#include <mutex>
#include <chrono>

#include <thallium.hpp>

#include "KeeperDataStore.h"

namespace chl = chronolog;
namespace tl = thallium;

///////////////////////
class ClocksourceCPPStyle
{
public:
    uint64_t getTimestamp() {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }
};

////////////////////////

int chronolog::KeeperDataStore::startStoryRecording(std::string const& chronicle, std::string const& story, chronolog::StoryId const& story_id
		           , uint64_t start_time, uint32_t time_chunk_duration, uint32_t access_window)
{
    std::cout<<"KeeperDataStore: received startStoryRecording {"<< chronicle<<", "<<story<<", "<<story_id<< "}"<<std::endl;

    //get dataStoreMutex, check for story_id_presense & add new StoryPipeline if needed
    std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter == theMapOfStoryPipelines.end())
    {
        auto result = theMapOfStoryPipelines.emplace(std::pair<chl::StoryId, chl::StoryPipeline*> (story_id, new chl::StoryPipeline(theExtractionQueue,                         chronicle, story, story_id, start_time, time_chunk_duration)));
	    if( result.second)
	    { pipeline_iter = result.first; }
	    else
        { return 0; }
    }

    //check it the pipeline was put on the waitingForExit list by the previous acquisition
    // and remove it from there
    auto waiting_iter = pipelinesWaitingForExit.find(story_id);
    if(waiting_iter != pipelinesWaitingForExit.end())
    {
        pipelinesWaitingForExit.erase(waiting_iter);
        return 1;
    }

    //engage StoryPipeline with the IngestionQueue

    StoryIngestionHandle * ingestionHandle = (*pipeline_iter).second->getActiveIngestionHandle();

    theIngestionQueue.addStoryIngestionHandle( story_id, ingestionHandle);

return 1;
}
////////////////////////

int chronolog::KeeperDataStore::stopStoryRecording(chronolog::StoryId const& story_id)
{
    std::cout<<"KeeperDataStore: received stopStoryRecording {"<<story_id<<"}"<< std::endl;

      // we do not yet disengage the StoryPipeline from the IngestionQueue right away
      // but put it on the WaitingForExit list to be finalized, persisted to disk , and
      // removed from memory at exit_time = now+acceptance_window...
      // unless there's a new story acqiusition request comes before that moment

	std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {
        uint64_t exit_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                + (*pipeline_iter).second->getAcceptanceWindow();
        pipelinesWaitingForExit[(*pipeline_iter).first] =(std::pair<chl::StoryPipeline*, uint64_t>((*pipeline_iter).second, exit_time) );
    }

return 1;
}

////////////////////////

void chronolog::KeeperDataStore::collectIngestedEvents()
{
    std::cout<<"KeeperDataStore::collectIngestedEvents state {"<<state
            <<"} mapOfStoryPipelines {"<<theMapOfStoryPipelines.size()
            <<"} pipelinesWaitingForExit {"<< pipelinesWaitingForExit.size()
            <<"} start , ULT " << tl::thread::self_id() << std::endl;

    theIngestionQueue.drainOrphanEvents();

	std::lock_guard storeLock(dataStoreMutex);
    for( auto pipeline_iter = theMapOfStoryPipelines.begin();
            pipeline_iter != theMapOfStoryPipelines.end(); ++pipeline_iter)
    {
//INNA: this can be delegated to different threads handling individual storylines...
            (*pipeline_iter).second->collectIngestedEvents();
    }


}
////////////////////////
void chronolog::KeeperDataStore::extractDecayedStoryChunks()
{

    std::cout<<"KeeperDataStore::extractDecayedStoryChunks state {"<<state
            <<"} mapOfStoryPipelines {"<<theMapOfStoryPipelines.size()
            <<"} pipelinesWaitingForExit {"<< pipelinesWaitingForExit.size()
            <<"} start , ULT " << tl::thread::self_id() << std::endl;

    uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	std::lock_guard storeLock(dataStoreMutex);
    for( auto pipeline_iter = theMapOfStoryPipelines.begin();
            pipeline_iter != theMapOfStoryPipelines.end(); ++pipeline_iter)
    {

            (*pipeline_iter).second->extractDecayedStoryChunks(current_time);
    }


}
////////////////////////

void chronolog::KeeperDataStore::retireDecayedPipelines()
{
    std::cout<<"KeeperDataStore::retireDecayedPipelines state {"<<state
            <<"} mapOfStoryPipelines {"<<theMapOfStoryPipelines.size()
            <<"} pipelinesWaitingForExit {"<< pipelinesWaitingForExit.size()
            <<"} START , ULT " << tl::thread::self_id() << std::endl;

if( !theMapOfStoryPipelines.empty())
{
	std::lock_guard storeLock(dataStoreMutex);

    uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for( auto pipeline_iter = pipelinesWaitingForExit.begin();
            pipeline_iter != pipelinesWaitingForExit.end(); )
    {

        if ( current_time >= (*pipeline_iter).second.second)
        { //current_time >= pipeline exit_time
                StoryPipeline * pipeline = (*pipeline_iter).second.first;

            theMapOfStoryPipelines.erase(pipeline->getStoryId());
            theIngestionQueue.removeIngestionHandle(pipeline->getStoryId());
            pipeline_iter = pipelinesWaitingForExit.erase(pipeline_iter); //pipeline->getStoryId());
            delete pipeline;
        }
        else
        { pipeline_iter++; }

    }
}

    //swipe through pipelineswaiting and remove all those with nullptr
    std::cout<<"KeeperDataStore::retireDecayedPipelines state {"<<state
            <<"} mapOfStoryPipelines {"<<theMapOfStoryPipelines.size()
            <<"} pipelinesWaitingForExit {"<< pipelinesWaitingForExit.size()
            <<"} END , ULT " << tl::thread::self_id() << std::endl;

}

void chronolog::KeeperDataStore::dataCollectionTask()
{
          //run dataCollectionTask as long as the state == RUNNING
                        // or there're still events left to collect and
                        // storyPipelines left to retire...
        tl::xstream es = tl::xstream::self();
        std::cout<< "DataStoreCollectionTask ES "<<es.get_rank() << ", ULT " << tl::thread::self_id() << std::endl;
        while( !is_shutting_down()
                || !theIngestionQueue.is_empty()
                || !theMapOfStoryPipelines.empty())
        {
            std::cout<< "DataStoreCollectionTask ES "<<es.get_rank() << ", ULT " << tl::thread::self_id() << std::endl;
            for(int i =0; i<6; ++i)
            {
                collectIngestedEvents();
                sleep(10);
            }
            extractDecayedStoryChunks();
            retireDecayedPipelines();
        }
        std::cout << "Exiting DatacollectionTask thread "<< tl::thread::self_id() << std::endl;
}

////////////////////////
void  chronolog::KeeperDataStore::startDataCollection( int stream_count)
{
    std::lock_guard storeLock(dataStoreStateMutex);
    if( is_running() || is_shutting_down() )
    {   return;   }

    state = RUNNING;


    for(int i=0; i < stream_count; ++i)
    {
        tl::managed<tl::xstream> es = tl::xstream::create();
        dataStoreStreams.push_back(std::move(es));
    }

    for(int i=0; i < 2*stream_count; ++i)
    {
        tl::managed<tl::thread> th = dataStoreStreams[i % (dataStoreStreams.size())]->make_thread(
                    [p=this](){ p->dataCollectionTask(); });
        dataStoreThreads.push_back(std::move(th));
    }
}
//////////////////////////////

void chronolog::KeeperDataStore::shutdownDataCollection()
{
    std::cout<<"KeeperDataStore: shutdownDataCollection : state {"<< state
            <<"} mapOfStoryPipelines {"<<theMapOfStoryPipelines.size()
            <<"} pipelinesWaitingForExit {"<< pipelinesWaitingForExit.size()<<"}"<< std::endl;

 // switch the state to shuttingDown
    std::lock_guard storeLock(dataStoreStateMutex);
    if( is_shutting_down() )
    {   return;   }

    state = SHUTTING_DOWN;

    if( !theMapOfStoryPipelines.empty())
    {
        // label all existing Pipelines as waiting to exit
	    std::lock_guard storeLock(dataStoreMutex);
        uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

        for( auto pipeline_iter = theMapOfStoryPipelines.begin();
            pipeline_iter != theMapOfStoryPipelines.end(); ++pipeline_iter)
        {
            if(pipelinesWaitingForExit.find((*pipeline_iter).first) == pipelinesWaitingForExit.end())
	        {
                uint64_t exit_time = current_time + (*pipeline_iter).second->getAcceptanceWindow();
                pipelinesWaitingForExit[(*pipeline_iter).first] =(std::pair<chl::StoryPipeline*, uint64_t>((*pipeline_iter).second, exit_time) );
            }
        }
    }

    // join threads & execution streams while holding stateMutex
    // and just wait until all the events are collected and
    // all the storyPipelines decay and retire

    for(auto& th : dataStoreThreads)
    {
        th->join();
    }

    std::cout<<"KeeperDataStore: shutdownDataCollection : threads exitted" << std::endl;
    for(auto & es : dataStoreStreams)
    {
        es->join();
    }
    std::cout<<"KeeperDataStore: shutdownDataCollection : streams exitted" << std::endl;


}

///////////////////////

 //
chronolog::KeeperDataStore::~KeeperDataStore()
{
    std::cout<<"KeeperDataStore::~KeeperDataStore()"<<std::endl;
    //std::cout<<"KeeperDataStore:: before shutdown mapOfStryPipelines.size()={"<<theMapOfStoryPipelines.size()<<"}"<< std::endl;
    shutdownDataCollection();
    //std::cout<<"KeeperDataStore::after shutdown  mapOfStoryPipelines.size()={"<<theMapOfStoryPipelines.size()<<"}"<< std::endl;

}
