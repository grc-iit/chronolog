#ifndef INGESTION_QUEUE_H
#define INGESTION_QUEUE_H


#include <deque>

//INNA: TODO : this is just a draft implementation 
//
// IngestionQueue is a funnel into the MemoryDataStore
// stl deque guarantees O(1) time for addidng elements and resizing 
// (vector of vectors implementation)
// I also intend to have only one ingestion thread adding time chunks and elements to this dequeue
// so no mutex is needed to protect log_event ingestion 

namespace chronolog
{
typedef uint64_t StoryId;
typedef uint64_t ClientId;

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

class LogIngestionQueue
{

public:
	LogIngestionQueue() {}
	~LogIngestionQueue() = default;

	void ingestLogEvent( LogEvent const& logEvent)
	{ eventDeque.push_back(logEvent); }

private:

	std::deque<LogEvent> eventDeque;
};

}

#endif

