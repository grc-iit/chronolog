#include <unistd.h>

#include <thallium.hpp>

#include "StoryChunkExtractor.h"


namespace tl = thallium;

//////////////////////////////

void chronolog::StoryChunkExtractorBase::startExtractionThreads( int stream_count)
{
    std::lock_guard lock(extractorMutex);

    if(extractorState == RUNNING)
    {   return; }

    extractorState = RUNNING;
    std::cout <<"ExtractionModule: startExtractionThreads"<<std::endl;

    for(int i=0; i < stream_count; ++i)
    {
        tl::managed<tl::xstream> es = tl::xstream::create();
        extractionStreams.push_back(std::move(es));
    }

    for(int i=0; i < 2*stream_count; ++i)
    {
        tl::managed<tl::thread> th = extractionStreams[i % extractionStreams.size()]->make_thread([p=this](){ p->drainExtractionQueue();});
        extractionThreads.push_back(std::move(th));
    }

}
//////////////////////////////

void chronolog::StoryChunkExtractorBase::shutdownExtractionThreads()
{
    std::lock_guard lock(extractorMutex);

    if(extractorState == SHUTTING_DOWN)
    {   return; }

    extractorState = SHUTTING_DOWN;
    std::cout <<"ExtractionModule: shutdown : chunkExtractionQueue.size="<<chunkExtractionQueue.size()<<std::endl;

    // join threads & executionstreams while holding stateMutex

    for( auto& eth : extractionThreads)
    {
        eth->join();
    }
    std::cout <<"ExtractorBase: shutdown : extractionThreads exitted"<<std::endl;
    for( auto& es : extractionStreams)
    {
        es->join();
    }
    std::cout <<"ExtractorBase: shutdown : chunkStreamsExited"<<std::endl;

}

//////////////////////
chronolog::StoryChunkExtractorBase::~StoryChunkExtractorBase()
{
    std::cout <<"ExtractorBase::~ExtractorBase"<<std::endl;
    shutdownExtractionThreads();

    extractionThreads.clear();
    extractionStreams.clear();
}

//////////////////////

void chronolog::StoryChunkExtractorBase::drainExtractionQueue()
{

    thallium::xstream es = thallium::xstream::self();

    // extraction threads will be running as long as the state doesn't change
    // and untill the extractionQueue is drained in shutdown mode
    while( (extractorState == RUNNING) || !chunkExtractionQueue.empty() )
    {
        std::cout << "ExtractorBase:drainExtractionQueue: from ES "
            << es.get_rank() << ", ULT " << thallium::thread::self_id()
        <<" extractionQueue.size="<<chunkExtractionQueue.size() << std::endl;
        while( !chunkExtractionQueue.empty())
        {
            StoryChunk * storyChunk = chunkExtractionQueue.ejectStoryChunk();
            if(storyChunk == nullptr)  //the queue might have been drained by another thread before the current thread acquired extractionQueue mutex
            {   break;  }
            processStoryChunk(storyChunk);  // INNA: should add return type and handle the failure properly
            // free the memory or reset the startTime and return to the pool of prealocated chunks
            delete storyChunk;
        }

        sleep(30);
    }
}

