#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include <string>
#include <vector>
#include <map>

#include "ConfigurationManager.h"  //TODO: not sure this is a good idea , but will keep it for now ...

#include "ClientConfiguration.h"

//TODO: rename Client to be ChronologClient 
//TODO: remove ConfigurationManager from chronolog_client include fiels , should only be in the implementation files 
// if needed at all


namespace chronolog
{

class ChronologClientImpl;
class StoryWritingHandle;

class StoryHandle
{
public:
    friend ChronologClientImpl;

    StoryHandle();  //constructs invalid StoryHandle
                     
    StoryHandle(const StoryHandle&) = default;                    
    StoryHandle(StoryHandle&&) = default;                         
    StoryHandle& operator=(const StoryHandle&) = default;         
    StoryHandle& operator=(StoryHandle&&) = default;              

     ~StoryHandle() = default;

    bool is_valid() const;

    int log_event(std::string const &);

    // to be implemented with libfabric/thallium bulk transfer...
    //virtual int log_event( size_t , void*) = 0;

private:
    StoryHandle(const std::shared_ptr<StoryWritingHandle>& impl);

    std::shared_ptr<StoryWritingHandle> storyWritingHandle;
};

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

    std::pair <int, StoryHandle> AcquireStory(std::string const &chronicle_name, std::string const &story_name
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
