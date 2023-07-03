#ifndef CHRONOLOG_TYPE_DEFINITIONS_H
#define CHRONOLOG_TYPE_DEFINITIONS_H


namespace chronolog
{

typedef std::string StoryName;
typedef std::string ChronicleName;
typedef uint64_t StoryId;
typedef uint64_t ChronicleId;
typedef uint32_t ClientId;

typedef uint64_t chrono_time;
typedef uint32_t chrono_index;

class LogEvent
{
public:
	LogEvent() = default;

        LogEvent(StoryId const& story_id, chrono_time event_time, ClientId client_id, chrono_index index, std::string const& record)
                : storyId(story_id)
		, eventTime(event_time)
		, clientId(client_id)
	       	, eventIndex(index)
                , logRecord(record)     
        {}  
        StoryId  storyId;
	uint64_t eventTime;
        ClientId clientId;
	uint32_t eventIndex;
	std::string logRecord; //INNA: replace with size_t  length; & void * data; later on
 
  uint64_t const& time() const
  {  return eventTime; }

  uint32_t const& index() const
  {  return eventIndex; }

  // serialization function used by thallium RPC providers 
  
  template <typename SerArchiveT>
  void serialize( SerArchiveT & serT)
  {
      serT (storyId, eventTime, clientId, eventIndex, logRecord);
  }
};


}
#endif
