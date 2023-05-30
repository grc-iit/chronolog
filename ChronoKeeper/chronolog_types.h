#ifndef CHRONOLOG_TYPE_DEFINITIONS_H
#define CHRONOLOG_TYPE_DEFINITIONS_H


namespace chronolog
{

typedef uint64_t StoryId;
typedef uint64_t StorytellerId;
typedef uint64_t ClientId;
typedef std::string StoryName;
typedef std::string ChronicleName;
typedef uint64_t ChronoTick;
//typedef std::pair<uint64_t, uint64_t> ChronoTick; //INNA : use a pair throughout


struct LogEvent
{
        LogEvent(StoryId const& story_id, ClientId const& client_id, uint64_t time, std::string const& record)
                : storyId(story_id), clientId(client_id)
                , timestamp(time),logRecord(record)     //INNA: keep both tick integers
        {}  
        StoryId  storyId;
        ClientId clientId;
        uint64_t timestamp;
        std::string logRecord;
};


}
#endif
