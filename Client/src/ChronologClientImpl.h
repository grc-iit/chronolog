#ifndef CHRONOLOG_CLIENT_IMPL_H
#define CHRONOLOG_CLIENT_IMPL_H

#include "chronolog_errcode.h"
#include "ConfigurationManager.h"
#include "ClientConfiguration.h"

#include "chronolog_types.h"


#include "chronolog_client.h"
#include "rpcVisorClient.h"
#include "StorytellerClient.h"
#include "ClientQueryService.h"

namespace chronolog
{

enum ChronologClientState
{
    UNKNOWN = 0, CONNECTED = 1, READING = 2, WRITING = 3, SHUTTING_DOWN = 4
};

class ChronologClientImpl
{
public:

    // static mutex ensures that there'd be the single instance
    // of ChronologClientImpl ever created regardless of the
    // thread(s) GetClientImplInstance() is called from
    static std::mutex chronologClientMutex;
    static ChronologClientImpl*chronologClientImplInstance;

    static ChronologClientImpl*
    GetClientImplInstance(ChronoLog::ConfigurationManager const &);
    static ChronologClientImpl*
    GetClientImplInstance(chronolog::ClientPortalServiceConf const &);

    // the classs is non-copyable
    ChronologClientImpl(ChronologClientImpl const &) = delete;

    ChronologClientImpl &operator=(ChronologClientImpl const &) = delete;

    ~ChronologClientImpl();

    int Connect();

    int Disconnect(); 

    int CreateChronicle(std::string const &chronicle_name, const std::map <std::string, std::string> &attrs
                        , int &flags);

    int DestroyChronicle(std::string const &chronicle_name); 

    std::pair <int, StoryHandle*> AcquireStory(std::string const &chronicle_name, std::string const &story_name
                                               , const std::map <std::string, std::string> &attrs
                                               , int &flags);

    int ReleaseStory(std::string const &chronicle_name, std::string const &story_name); 
    int DestroyStory(std::string const &chronicle_name, std::string const &story_name);

    int GetChronicleAttr(std::string const &chronicle_name, const std::string &key, std::string &value);

    int EditChronicleAttr(std::string const &chronicle_name, const std::string &key, const std::string &value);

    std::vector <std::string> &ShowChronicles(std::vector <std::string> &);
    std::vector <std::string> &ShowStories(const std::string &chronicle_name, std::vector <std::string> &);

private:

    ChronologClientState clientState;
    std::string clientLogin;
    uint32_t euid;
    uint32_t hostId;
    uint32_t pid;
    ClientId clientId;
    ChronologTimer clockProxy;
    thallium::engine*tlEngine;
    RpcVisorClient*rpcVisorClient;
    StorytellerClient*storyteller;
    ClientQueryService * storyReaderService;
    
    ChronologClientImpl(const ChronoLog::ConfigurationManager &conf_manager);
    ChronologClientImpl( ClientQueryServiceConf const& , ClientPortalServiceConf const&);

    void defineClientIdentity();

};
} //namespace chronolog

#endif 
