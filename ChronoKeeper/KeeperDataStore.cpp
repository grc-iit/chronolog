
#include <map>
#include <mutex>
#include <chrono>

#include "KeeperDataStore.h"

namespace chl = chronolog;

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

    //check it the pipeline was put on the waitingForExit list by the previous aquisition
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
      // removed from ,memory at time now+acceptance_window...
      // unless there's a new story aqiusition request comes before that moment
      
	std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {  
         //INNA:set this later (*pipeline_iter).second->scheduleForExit(revisionTime + acceptanceWindow);
	 
	    pipelinesWaitingForExit.insert(std::pair<chl::StoryId,chl::StoryPipeline*>( (*pipeline_iter).first, (*pipeline_iter).second) );
    }

return 1;
}

////////////////////////

void chronolog::KeeperDataStore::collectIngestedEvents()
{

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

    std::cout<<"KeeperDataStore::extractDecayedStoryChunks mapOfStoryPipelines.size()={"<<theMapOfStoryPipelines.size()<<"}"<< std::endl;
    uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    for( auto pipeline_iter = theMapOfStoryPipelines.begin(); 
            pipeline_iter != theMapOfStoryPipelines.end(); ++pipeline_iter)
    {
//INNA: this can be delegated to different threads handling individual storylines...
            (*pipeline_iter).second->extractDecayedStoryChunks(current_time);
    }


}
////////////////////////


void  chronolog::KeeperDataStore::startDataCollection()
{
    std::lock_guard storeLock(dataStoreMutex);
    if( is_shutting_down() )
    {   return;   }
    
    state = RUNNING;

/*  
   for(int i=0; i < 4; i++) {
        tl::managed<tl::xstream> es = tl::xstream::create();
        dataStoreStreams.push_back(std::move(es));
    }

    std::vector<tl::managed<tl::thread>> dataStoreThreads;
    for(int i=0; i < 6; i++)
    {
        tl::managed<tl::thread> th = dataStoreStreams[i % (dataStoreStreams.size()-1)]->make_thread(
                    [p=&theDataStore](){
                        tl::xstream es = tl::xstream::self();
                        while( !p->is_shutting_down())
                        {
                            std::cout<< "DataStore ES "<<es.get_rank() << ", ULT "
                                << tl::thread::self_id() << std::endl;

                            for(int i =0; i<6; ++i)
                            {
                                p->collectIngestedEvents();
                                sleep(10);
                            }
                            p->extractDecayedStoryChunks();
                            p->retireDecayedPipelines();
                        }
                        std::cout << "Exiting thread "<< tl::thread::self_id() << std::endl;
                    });
        dataStoreThreads.push_back(std::move(th));
    }
*/
}
//////////////////////////////

void chronolog::KeeperDataStore::shutdownDataCollection()
{
    std::cout<<"KeeperDataStore: shutdownDataCollection" << std::endl;
      
 // switch the state to shuttingDown
    std::lock_guard storeLock(dataStoreMutex);
    if( is_shutting_down() )
    {   return;   }

    state = SHUTTING_DOWN;

   // Unregister the ChronoKeeper  
   //
   //stop Recording service for all the active stories
   // ? should we notify the Visor ???
   // 
   // Finalize and persist all the story lines to disk
   // ? we should  probably disregard acceptance window in this case
   //
    // join threads & execution streams would be here while holding stateMutex
   //
}

///////////////////////

 //
chronolog::KeeperDataStore::~KeeperDataStore()
{
    std::cout<<"KeeperDataStore::~KeeperDataStore()"<<std::endl;
    shutdownDataCollection();
  
}
