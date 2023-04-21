#ifndef CHRONOLOG_CLIENT_IMPL_H
#define CHRONOLOG_CLIENT_IMPL_H

#include "RPCClient.h"
#include "errcode.h"
#include "ConfigurationManager.h"
//#include "ClocksourceManager.h"

#include "client.h"
#include "chronolog_types.h"
//#include "StorytellerClient.h"


//ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;
//class ClocksourceManager;
//class StorytellerClient;

namespace chronolog
{

class ChronologClientImpl 
{
public:

    ChronologClientImpl(const ChronoLog::ConfigurationManager& conf_manager) {
        CHRONOLOG_CONF->SetConfiguration(conf_manager);
        init();
    }

    ChronologClientImpl(const ChronoLogRPCImplementation& protocol, const std::string& visor_ip, int visor_port) {
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
    }

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

    StoryHandle * AcquireStory(std::string const& chronicle_name, std::string const& story_name,
                     const std::unordered_map<std::string, std::string> &attrs, int &flags);
    int ReleaseStory(std::string const&chronicle_name, std::string const& story_name); //, int &flags);
    int DestroyStory(std::string const& chronicle_name, std::string const& story_name);

    int GetChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string &value);
    int EditChronicleAttr(std::string const& chronicle_name, const std::string &key, const std::string &value);
    
    std::vector<std::string> & ShowChronicles( std::vector<std::string> &); //std::string &client_id);
    std::vector<std::string> & ShowStories( const std::string &chronicle_name, std::vector<std::string> &);

private:
    std::string clientAccount;
    ClientId	clientId;
    std::shared_ptr<RPCClient> rpcClient_;
  //  StorytellerClient * storyteller;
  //  ClocksourceManager *pClocksourceManager_;


};
} //namespace chronolog

#endif //CHRONOLOG_CLIENT_H
