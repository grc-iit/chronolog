#ifndef CHRONOLOG_TYPE_DEFINITIONS_H
#define CHRONOLOG_TYPE_DEFINITIONS_H


namespace chronolog
{

typedef std::string StoryName;
typedef std::string ChronicleName;
typedef uint64_t StoryId;
typedef uint64_t ClientId;
typedef uint64_t ChronicleId;

// ChronoTick contains epoch timestamp & integer index
typedef std::tuple<uint64_t,uint64_t> ChronoTick; 


/*bool operator < (ChronoTick const& tick1, ChronoTick const& tick2)
{
	return ( (tick1.first  == tick2.first) ?
		 (tick1.second < tick2.second) : (tick1.first < tick2.first) );
}
*/

class LogEvent
{
public:
	LogEvent() = default;

        LogEvent(StoryId const& story_id, ClientId const& client_id, ChronoTick const& chrono_tick, std::string const& record)
                : storyId(story_id), clientId(client_id)
                , chronoTick(chrono_tick),logRecord(record)     
        {}  
        StoryId  storyId;
        ClientId clientId;
        ChronoTick chronoTick;
	std::string logRecord; //INNA: replace with size_t  length; & void * data; later on
 
  uint64_t const& time() const
  {  return std::get<0>(chronoTick); }

  // serialization function used by thallium RPC providers 
  // to serialize/deserialize KeeperIdCard 
  
  template <typename SerArchiveT>
  void serialize( SerArchiveT & serT)
  {
      serT (storyId, clientId, chronoTick, logRecord);
  }
};


}
#endif
