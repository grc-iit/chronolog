#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include <string>
#include <vector>
#include <unordered_map>   

#include "ConfigurationManager.h"  //TODO: not sure this is a good idea , but will keep it for now ...

namespace chronolog
{

//Abstract StoryHandle will be returnrned to the cleitn in AcquireStory() API call
class StoryHandle
{
public:
    virtual  ~StoryHandle();

    virtual int log_event( std::string const&) = 0;

    // to be implemented with libfabric/thallium bulk transfer...
    //virtual int log_event( size_t , void*) = 0;
};

class ChronologClientImpl;

// top level Chronolog Client...
// implementation details are in the ChronologClientImpl class 
class Client
{
public:
    Client( ChronoLog::ConfigurationManager const&);
    ~Client();

    int Connect(const std::string &server_uri,
                std::string const& client_account,
                int &flags);
    int Disconnect( ); //std::string const& client_account);

    int CreateChronicle( std::string const& chronicle_name,
                         std::unordered_map<std::string, std::string> const& attrs,
                         int &flags);
    int DestroyChronicle(std::string const& chronicle_name);

//TODO: unordered_map? how manyu attributes do we expect ???
    StoryHandle * AcquireStory(std::string const& chronicle_name, std::string const& story_name,
                     const std::unordered_map<std::string, std::string> &attrs, int &flags);

    int ReleaseStory(std::string const& chronicle_name, std::string const& story_name);
    int DestroyStory(std::string const& chronicle_name, std::string const& story_name);

    int GetChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string &value);
    int EditChronicleAttr(std::string const& chronicle_name, const std::string &key, const std::string &value);

    std::vector<std::string> & ShowChronicles( std::vector<std::string> &);
    std::vector<std::string> & ShowStories( std::string const& chronicle_name, std::vector<std::string> & );

private:
    ChronologClientImpl * chronologClientImpl;
};

} //namespace chronolog

#endif 
