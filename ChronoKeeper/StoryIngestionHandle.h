#ifndef STORY_INGESTION_HANDLE_H
#define STORY_INGESTION_HANDLE_H

#include <mutex>
#include <deque>

//
// IngestionQueue is a funnel into the KeeperDataStore
// std::deque guarantees O(1) time for addidng elements and resizing 
// (vector of vectors implementation)

namespace chronolog
{
typedef uint64_t StoryId;
typedef uint64_t ClientId;
typedef std::mutex ChronoMutex;	

struct LogEvent
{
	LogEvent(StoryId const& story_id, ClientId const& client_id, uint64_t time, std::string const& record)
		: storyId(story_id), clientId(client_id)
		, timestamp(time),logRecord(record)
	{}  
	StoryId  storyId;
	ClientId clientId;
	uint64_t timestamp;
	std::string logRecord;
};

typedef std::deque<LogEvent> EventDeque;

class StoryIngestionHandle
{

public:
	StoryIngestionHandle( ChronoMutex & a_mutex, EventDeque & event_deque)
       		: ingestionMutex(a_mutex)
	  	, activeDeque(&eventDeque)
			{}
	~StoryIngestionQueue() = default;

	void ingestLogEvent( LogEvent const& logEvent)
	{   // assume multiple service threads pushing events on ingestionQueue
	    std::lock_guard<ingestionMutex> lock(); 
	    activeDeque->push_back(logEvent); 
	}

	void swapActiveDeque( EventDeque * empty_deque, EventDeque * full_deque)
	{
	    if( activeDeque->is_empty())
	    {	    return ;       }
//INNA: check if atomic compare_and_swap will work here

	    std::lock_guard<ingestionMutex> lock_guard(ingestionMutex); 
	    if(  activeDeque->is_empty() )
	    {	    return ;       }

	    full_queue = activeDeque;
	    activeDeque = empty_deque;

	}
		
	     
private:

	std::mutex & ingestionMutex;   
	EventDeque * activeDeque;
};

}

#endif

