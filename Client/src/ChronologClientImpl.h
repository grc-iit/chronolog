#ifndef CHRONOLOG_CLIENT_IMPL_H
#define CHRONOLOG_CLIENT_IMPL_H

#include "RPCClient.h"
#include "errcode.h"
#include "ConfigurationManager.h"
//#include "ClocksourceManager.h"

#include "chronolog_types.h"
//#include "StorytellerClient.h"


//ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;
//class ClocksourceManager;
//class StorytellerClient;

#include "chronolog_client.h"
#include "rpcVisorClient.h"
#include "StorytellerClient.h"

namespace chronolog
{

enum ChronologClientState
{
	UNKNOWN	 = 0,
	CONNECTED = 1,
	READING	  = 2,
	WRITING	  = 3,
	SHUTTING_DOWN = 4
};

class ChronologClientImpl 
{
public:

   // static mutex ensures that there'd be the single instance 
   // of ChronologClientImpl ever created regardless of the
   // thread(s) GetClientImplInstance() is called from
    static std::mutex chronologClientMutex;
    static ChronologClientImpl * chronologClientImplInstance;
    static ChronologClientImpl * GetClientImplInstance(ChronoLog::ConfigurationManager const &);
    
    // the classs is non-copyable
    ChronologClientImpl( ChronologClientImpl const&) = delete;
    ChronologClientImpl & operator=(ChronologClientImpl const&) = delete;

    ~ChronologClientImpl();

    int Connect(const std::string &server_uri,
                std::string const& client_id,
                int &flags);
                //uint64_t &clock_offset);
    int Disconnect( ); //const std::string &client_account, int &flags);

    int CreateChronicle(std::string const& chronicle_name,
                        const std::unordered_map<std::string, std::string> &attrs,
                        int &flags);
    int DestroyChronicle(std::string const& chronicle_name); //, int &flags);

    std::pair<int,StoryHandle*> AcquireStory(std::string const& chronicle_name, std::string const& story_name,
                     const std::unordered_map<std::string, std::string> &attrs, int &flags);
    int ReleaseStory(std::string const&chronicle_name, std::string const& story_name); //, int &flags);
    int DestroyStory(std::string const& chronicle_name, std::string const& story_name);

    int GetChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string &value);
    int EditChronicleAttr(std::string const& chronicle_name, const std::string &key, const std::string &value);
    
    std::vector<std::string> & ShowChronicles( std::vector<std::string> &); //std::string &client_id);
    std::vector<std::string> & ShowStories( const std::string &chronicle_name, std::vector<std::string> &);

private:

    ChronologClientState    clientState;
    std::string clientAccount;
    ClientId	clientId;
    ChronologTimer clockProxy;
    std::shared_ptr<RPCClient> rpcClient_;
    RpcVisorClient * rpcVisorClient;
    StorytellerClient * storyteller;
   // ClocksourceManager *pClocksourceManager_;

    //TODO : client_account & client_ip will be acquired from the cleitn process itself ....
    //    for now they can be passed in....
    ChronologClientImpl(const ChronoLog::ConfigurationManager& conf_manager)
	    : clientState(UNKNOWN)
	    , clientAccount("")
	    , clientId(0)
	    , rpcVisorClient(nullptr)
	    , storyteller(nullptr) 
    {
        CHRONOLOG_CONF->SetConfiguration(conf_manager);
        init();
    }

    ChronologClientImpl(const ChronoLogRPCImplementation& protocol, const std::string& visor_ip, int visor_port) 
	    : clientState(UNKNOWN)
	    , clientAccount("")
	    , clientId(0)
	    , rpcVisorClient(nullptr)
	    , storyteller(nullptr) 
    {
        CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION = protocol;
        CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP = visor_ip;
        CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT = visor_port;
        init();
    }

    void init() {
        //pClocksourceManager_ = ClocksourceManager::getInstance();
        //pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);
        CHRONOLOG_CONF->ROLE = CHRONOLOG_CLIENT;
        rpcClient_ = ChronoLog::Singleton<RPCClient>::GetInstance();
        storyteller =  new StorytellerClient( clockProxy,  rpcClient_->get_tl_client_engine(), 0 ); //clientId field will be accessable later...
    }


};
} //namespace chronolog

#endif 
