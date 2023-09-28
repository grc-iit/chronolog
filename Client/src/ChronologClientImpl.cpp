
#include <unistd.h>
#include <string>
#include "ChronologClientImpl.h"
#include "StorytellerClient.h"
#include "city.h"

namespace chl = chronolog;

std::mutex chronolog::ChronologClientImpl::chronologClientMutex;
chronolog::ChronologClientImpl *chronolog::ChronologClientImpl::chronologClientImplInstance{nullptr};


chronolog::ChronologClientImpl *
chronolog::ChronologClientImpl::GetClientImplInstance(ChronoLog::ConfigurationManager const &confManager)
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
    if (chronologClientImplInstance == nullptr)
    {
        chronologClientImplInstance = new ChronologClientImpl(confManager);
    }

    return chronologClientImplInstance;
}

////////
chronolog::ChronologClientImpl::ChronologClientImpl(const ChronoLog::ConfigurationManager &confManager)
        : clientState(UNKNOWN), clientLogin(""), hostId(0), pid(0), clientId(0), tlEngine(nullptr), rpcVisorClient(nullptr),
          storyteller(nullptr)
{
    //pClocksourceManager_ = ClocksourceManager::getInstance();
    //pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);

    defineClientIdentity();
    tlEngine = new thallium::engine(confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF,
                                    THALLIUM_CLIENT_MODE, true, 1);

    std::string CLIENT_VISOR_NA_STRING = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF
                                         + "://" + confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP
                                         + ":" + std::to_string(
            confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT);
    rpcVisorClient = chl::RpcVisorClient::CreateRpcVisorClient(*tlEngine, CLIENT_VISOR_NA_STRING,
                                                               confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID);
}

////////

chronolog::ChronologClientImpl::ChronologClientImpl(const std::string &protocol_string, const std::string &visor_ip,
                                                    int visor_port, uint16_t visor_portal_service_provider)
        : clientState(UNKNOWN), clientLogin(""),hostId(0),pid(0), clientId(0), tlEngine(nullptr), rpcVisorClient(nullptr),
          storyteller(nullptr)
{
    //pClocksourceManager_ = ClocksourceManager::getInstance();
    //pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);

    defineClientIdentity();
    tlEngine = new thallium::engine(protocol_string, THALLIUM_CLIENT_MODE, true, 1);

    std::string client_visor_na_string = protocol_string
                                         + "://" + visor_ip
                                         + ":" + std::to_string(visor_port);

    rpcVisorClient = chl::RpcVisorClient::CreateRpcVisorClient(*tlEngine, client_visor_na_string,
                                                               visor_portal_service_provider);
}

void chronolog::ChronologClientImpl::defineClientIdentity()
{
    euid = geteuid(); //TODO: effective uid might be a better choice than login name ...

    char login_name[LOGIN_NAME_MAX];
    getlogin_r(login_name, LOGIN_NAME_MAX);

    clientLogin=login_name; //this truncates login_name to the actual strlen...

    //32bit host identifier
    hostId= gethostid();
    //32bit process id
    pid=getpid();
    std::cout <<"Client_identity {"<<clientLogin<<":"<<euid<<":"<<hostId<<":"<<pid<<"}"<<std::endl;

}

chronolog::ChronologClientImpl::~ChronologClientImpl()
{
    if (storyteller != nullptr)
    { delete storyteller; }

    if (rpcVisorClient != nullptr)
    { delete rpcVisorClient; }

    if (tlEngine != nullptr)
    {
        tlEngine->finalize();
        delete tlEngine;
    }

}

int chronolog::ChronologClientImpl::Connect()
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
    // if already connected return success
    // if disconencting return failure....
    if ((clientState != UNKNOWN) && (clientState != SHUTTING_DOWN))
    { return CL_SUCCESS; }


    auto connectResponseMsg = rpcVisorClient->Connect(euid, hostId, pid);

    std::cout<< connectResponseMsg<<std::endl;

    int return_code = connectResponseMsg.getErrorCode();
    if (return_code == CL_SUCCESS)
    {
        clientState = CONNECTED;
        clientId = connectResponseMsg.getClientId();
        if (storyteller == nullptr) 
        { storyteller = new StorytellerClient(clockProxy, *tlEngine, clientId); }
        //TODO: if we ever change the connection hashing algorithm we'd need to handle reconnection case with the new client_id 
    }
    return return_code;
}

int chronolog::ChronologClientImpl::Disconnect() 
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return CL_SUCCESS; }

    auto return_code = rpcVisorClient->Disconnect(clientId);
    if (return_code == CL_SUCCESS)
    {
        clientState = SHUTTING_DOWN;
    }
    return return_code;

}

int chronolog::ChronologClientImpl::CreateChronicle(std::string const &chronicle_name,
                                                    const std::unordered_map<std::string, std::string> &attrs,
                                                    int &flags)
{
    if (chronicle_name.empty())
    { return CL_ERR_INVALID_ARG; }

    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return CL_ERR_NO_CONNECTION; }

    return rpcVisorClient->CreateChronicle(clientId, chronicle_name, attrs, flags);
}

int chronolog::ChronologClientImpl::DestroyChronicle(std::string const &chronicle_name)
{
    if (chronicle_name.empty())
    { return CL_ERR_INVALID_ARG; }

    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return CL_ERR_NO_CONNECTION; }

    return rpcVisorClient->DestroyChronicle(clientId, chronicle_name);
}

int chronolog::ChronologClientImpl::DestroyStory(std::string const &chronicle_name, std::string const &story_name)
{
    if (chronicle_name.empty() || story_name.empty())
    { return CL_ERR_INVALID_ARG; }

    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return CL_ERR_NO_CONNECTION; }

    return rpcVisorClient->DestroyStory(clientId, chronicle_name, story_name);
}

std::pair<int, chronolog::StoryHandle *>
chronolog::ChronologClientImpl::AcquireStory(std::string const &chronicle_name, std::string const &story_name,
                                             const std::unordered_map<std::string, std::string> &attrs, int &flags)
{
    std::cout << "ChronologClientImpl::AcquireStory call : " << chronicle_name << ":" << story_name << std::endl;
    if (chronicle_name.empty() || story_name.empty())
    { return std::pair<int, chronolog::StoryHandle *>(CL_ERR_INVALID_ARG, nullptr); }

    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return std::pair<int, chronolog::StoryHandle *>(CL_ERR_NO_CONNECTION, nullptr); }

    // this function can be called from any client thread, so before sending an rpc request to the Visor
    // we check if the story acquisition request has been already granted to the process on some other thread
    // 
    chronolog::StoryHandle *storyHandle = storyteller->findStoryWritingHandle(chronicle_name, story_name);
    if (storyHandle != nullptr)
    { return std::pair<int, chronolog::StoryHandle *>(CL_SUCCESS, storyHandle); }

    // issue rpc request to the Visor
    auto acquireStoryResponse = rpcVisorClient->AcquireStory(clientId, chronicle_name, story_name, attrs, flags);

    std::cout << "AcquireStoryResponseMsg : " << acquireStoryResponse << std::endl;
    if (acquireStoryResponse.getErrorCode() != CL_SUCCESS)
    {
        return std::pair<int, chronolog::StoryHandle *>(acquireStoryResponse.getErrorCode(), nullptr);
    }

    //successfull AcquireStoryResponse carries Visor generated StoryId & vector<KeeperIdCard> 
    // for the Keepers assigned to record the acquired story

    storyHandle = storyteller->initializeStoryWritingHandle(chronicle_name, story_name,
                                                            acquireStoryResponse.getStoryId(),
                                                            acquireStoryResponse.getKeepers());


    if (storyHandle == nullptr)
    { return std::pair<int, chronolog::StoryHandle *>(CL_ERR_UNKNOWN, nullptr); }
    else
    { return std::pair<int, chronolog::StoryHandle *>(CL_SUCCESS, storyHandle); }
}

///////

int chronolog::ChronologClientImpl::ReleaseStory(std::string const &chronicle_name, std::string const &story_name)
{
    // there's no reason to waste an rpc call on empty strings...
    if (chronicle_name.empty() || story_name.empty())
    { return CL_ERR_INVALID_ARG; }

    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    // if we storyteller has active WritingHandle for this story
    // it should be cleared regardless of the Visor connection state

    if (nullptr == storyteller
        || nullptr == storyteller->findStoryWritingHandle(chronicle_name, story_name))
    { return CL_ERR_NOT_EXIST; }

    storyteller->removeAcquiredStoryHandle(chronicle_name, story_name);

    // if the client is still connected to the Visor
    // send ReleaseStory request 
    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return CL_ERR_NO_CONNECTION; }

    auto ret = rpcVisorClient->ReleaseStory(clientId, chronicle_name, story_name);
    return ret;
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::GetChronicleAttr(std::string const &chronicle_name, const std::string &key,
                                                     std::string &value)
{
    value.clear();  // in case the error is returned , make sure value is an empty string

    if (chronicle_name.empty() || key.empty())
    { return CL_ERR_INVALID_ARG; }

    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return CL_ERR_NO_CONNECTION; }

    return rpcVisorClient->GetChronicleAttr(clientId, chronicle_name, key, value);
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::EditChronicleAttr(std::string const &chronicle_name, const std::string &key,
                                                      std::string const &value)
{
    if (chronicle_name.empty() || key.empty() || value.empty())
    { return CL_ERR_INVALID_ARG; }

    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return CL_ERR_NO_CONNECTION; }

    return rpcVisorClient->EditChronicleAttr(clientId, chronicle_name, key, value);
}

std::vector<std::string> &chronolog::ChronologClientImpl::ShowChronicles(std::vector<std::string> &chronicles)
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return chronicles; }

    chronicles = rpcVisorClient->ShowChronicles(clientId);
    return chronicles;
}

std::vector<std::string> &
chronolog::ChronologClientImpl::ShowStories(std::string const &chronicle_name, std::vector<std::string> &stories)
{
    if (chronicle_name.empty())
    { return stories; }

    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if ((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return stories; }

    stories = rpcVisorClient->ShowStories(clientId, chronicle_name);
    return stories;
}
