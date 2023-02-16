#ifndef KEEPER_DATA_STORE_H
#define KEEPER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <mutex>

#include "IngestionQueue.h"

namespace chronolog
{


typedef uint64_t StoryId;
typedef uint64_t StorytellerId;
typedef std::string StoryName;
typedef std::string ChronicleName;

// Pool of Sequencing worker threads 
// Pool of Sequencing mutexes 
// Pool of indestion mutexes 
//
// Story Pipeline
// 1. ingestion Queue + hashed ingestion mutex 
// 2. map of 5 secs maps<timestamp,arrival index> + hashed sequencing index
// 
// on acquire story allocate StoryPipeLine object for the story, assign ingestion & sequencing mutex from the mutex pools
//
// Hashmap of story pipelines
//
//
//
//
enum Storyttate
{
	UNKNOWN = 0,
	UNFOLDING =1, // being written to
	PAUSED	=2,   // has been released 
	ARCHIVED =3
};

// StorycChunk contains all the events for the single story
// for the duration [startTime, endTime[
// startTime included, endTime excluded
// startTime/endTime are invariant
class StoryChunk
{
public:
   int mergeEvents(std::deque<LogEvent> const&);
   int mergeEvents(std::vector<LogEvent> const&);

private:
StoryId storyId;          
uint64_t startTime;
uint64_t endTime;   
uint64_t lastRevisionTime;
std::map<uint64_t, std::list<LogEvent>> logEvents;

};

class StoryPipeline
{
public:
	StoryPipeline( StoryId const&, std::mutex &, std::mutex &);
	
	~StoryPipeline();

int mergeEvents(std::vector<LogEvent> const&);
int mergeEvents(StoryChunk const&);

private:

StoryId 	storyId;
ChronicleName	chronicleName;
StoryName	storyName;
uint64_t	revisionTime; //timestamp of the most recent merge 
uint64_t	tailTime; // timestamp of the most recent recorded event
uint64_t	acquisitionTime; //timestamp of the most recent aquisition

// this mutex will be assigned to the story from the DataStore IngestionMutexPool
// based on the StoryId, it is used to protect the IngestionQueue from concurrent access
// by RecordingService threads
std::mutex & ingestionMutex;
// two ingestion queues so that they can take turns playing 
// active/passive ingestion duty
// 
std::deque<LogEvent> eventQueue1;
std::deque<LogEvent> eventQueue2;

StoryIngestionHandle * ingestionQueueHandle;

// this mutex will be assigned to the story from the DataStore SequencingMutexPool
// based  on the StoryId; it is used to protect Story sequencing operations 
// from concurrent access by the DataStore Sequencing threads
std::mutex & sequencingMutex;

// map of storyChunks ordered by inclusive startTime
std::map<uint64_t, StoryChunk > storyTimeline;

};

class KeeperDataStore
{
public:

bool is_shutting_down() const
{
	return (false); // INNA: implement state_management
}

int startStoryRecording(ChronicleName const&, StoryName const&, StoryId const&, uint32_t timeGranularity=30 ); //INNA: 30 seconds?
int stopStoryRecording(StoryId const&);

void shutdown();

private:
std::unordered_map<StoryId, StoryPipeline> StoryPipelines;
std::mutex theStoryPipelinesMutex;

std::unordered_map<StoryId, std::mutex> ingestionMutexes;
};

}
#endif
