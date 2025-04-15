#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include <string>
#include <vector>
#include <map>

#include "ConfigurationManager.h" 

#include "ClientConfiguration.h"

namespace chronolog
{

typedef std::string StoryName;
typedef std::string ChronicleName;
typedef uint64_t ClientId;
typedef uint64_t chrono_time;
typedef uint32_t chrono_index;

class Event
{
public:

    Event(chrono_time event_time = 0, ClientId client_id = 0, chrono_index index = 0, std::string const &record = std::string())
        : eventTime(event_time)
        , clientId(client_id)
        , eventIndex(index)
        , logRecord(record)
    { }

    uint64_t time() const
    { return eventTime; }

    ClientId const & client_id() const
    { return clientId; }

    uint32_t index() const
    { return eventIndex; }

    std::string const& log_record() const
    { return logRecord; }

    Event( Event const& other)
        : eventTime(other.time())
        , clientId(other.client_id())
        , eventIndex(other.index())
        , logRecord(other.log_record())
    { }

    Event& operator= (const Event & other) 
    {
        if (this != &other) 
        {
            eventTime = other.time();
            clientId = other.client_id();
            eventIndex = other.index();
            logRecord = other.log_record();
        }
        return *this;
    }

    bool operator== (const Event &other) const
    {
        return (eventTime == other.eventTime && clientId == other.clientId && eventIndex == other.eventIndex );
    }

    bool operator!= (const Event &other) const
    { return !(*this == other); }

    bool operator< (const Event &other) const
    {
        if( ( eventTime < other.time() ) 
           || (eventTime == other.time() && clientId < other.client_id()) 
           || (eventTime == other.time() && clientId == other.client_id() && eventIndex < other.index()) 
          )
        { 
            return true; 
        }
        else
        {
            return false; 
        }
    }


    inline std::string toString() const
    {
        return  "{Event: " + std::to_string(eventTime) + ":" + std::to_string(clientId) + ":" + std::to_string(eventIndex) +
                          ":" + logRecord +"}";
    }

private:
    
    uint64_t eventTime;
    ClientId clientId;
    uint32_t eventIndex;
    std::string logRecord;

};

class StoryHandle
{
public:
    virtual  ~StoryHandle();

    virtual int log_event(std::string const &) = 0;

    virtual int playback_story(uint64_t start, uint64_t end, std::vector<Event> & playback_events) = 0;
};

class ChronologClientImpl;

// top level Chronolog Client...
// implementation details are in the ChronologClientImpl class 
class Client
{
public:
    Client(ChronoLog::ConfigurationManager const &);
    
    Client(ClientPortalServiceConf const &);

    ~Client();

    int Connect();

    int Disconnect();

    int CreateChronicle(std::string const &chronicle_name, std::map <std::string, std::string> const &attrs , int &flags);

    int DestroyChronicle(std::string const &chronicle_name);

    std::pair <int, StoryHandle*> AcquireStory(std::string const &chronicle_name, std::string const &story_name
                                               , const std::map <std::string, std::string> &attrs
                                               , int &flags);

    int ReleaseStory(std::string const &chronicle_name, std::string const &story_name);

    int DestroyStory(std::string const &chronicle_name, std::string const &story_name);

    int GetChronicleAttr(std::string const &chronicle_name, const std::string &key, std::string &value);

    int EditChronicleAttr(std::string const &chronicle_name, const std::string &key, const std::string &value);

    std::vector <std::string> &ShowChronicles(std::vector <std::string> &);

    std::vector <std::string> &ShowStories(std::string const &chronicle_name, std::vector <std::string> &);

private:
    ChronologClientImpl*chronologClientImpl;
};

} //namespace chronolog

#endif 
