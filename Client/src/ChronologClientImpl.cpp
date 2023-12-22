#include <unistd.h>
#include <string>
#include "ChronologClientImpl.h"
#include "StorytellerClient.h"
#include "city.h"

namespace chl = chronolog;

std::mutex chronolog::ChronologClientImpl::chronologClientMutex;
chronolog::ChronologClientImpl*chronolog::ChronologClientImpl::chronologClientImplInstance{nullptr};


chronolog::ChronologClientImpl*
chronolog::ChronologClientImpl::GetClientImplInstance(ChronoLog::ConfigurationManager const &confManager)
{
    std::lock_guard <std::mutex> lock_client(chronologClientMutex);
    if(chronologClientImplInstance == nullptr)
    {
        chronologClientImplInstance = new ChronologClientImpl(confManager);
    }

    return chronologClientImplInstance;
}

////////
chronolog::ChronologClientImpl::ChronologClientImpl(const ChronoLog::ConfigurationManager &confManager): clientState(
        UNKNOWN), clientLogin(""), hostId(0), pid(0), clientId(0), tlEngine(nullptr), rpcVisorClient(nullptr)
                                                                                                         , storyteller(
                nullptr)
{
    //pClocksourceManager_ = ClocksourceManager::getInstance();
    //pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);

    defineClientIdentity();
    tlEngine = new thallium::engine(confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF
                                    , THALLIUM_CLIENT_MODE, true, 1);

    std::string CLIENT_VISOR_NA_STRING =
            confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
            confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP + ":" +
            std::to_string(confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT);
    rpcVisorClient = chl::RpcVisorClient::CreateRpcVisorClient(*tlEngine, CLIENT_VISOR_NA_STRING
                                                               , confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID);
}

////////

chronolog::ChronologClientImpl::ChronologClientImpl(const std::string &protocol_string, const std::string &visor_ip
                                                    , int visor_port, uint16_t visor_portal_service_provider)
        : clientState(UNKNOWN), clientLogin(""), hostId(0), pid(0), clientId(0), tlEngine(nullptr), rpcVisorClient(
        nullptr), storyteller(nullptr)
{
    //pClocksourceManager_ = ClocksourceManager::getInstance();
    //pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);

    defineClientIdentity();
    tlEngine = new thallium::engine(protocol_string, THALLIUM_CLIENT_MODE, true, 1);

    std::string client_visor_na_string = protocol_string + "://" + visor_ip + ":" + std::to_string(visor_port);

    rpcVisorClient = chl::RpcVisorClient::CreateRpcVisorClient(*tlEngine, client_visor_na_string
                                                               , visor_portal_service_provider);
}

void chronolog::ChronologClientImpl::defineClientIdentity()
{
    euid = geteuid(); //TODO: effective uid might be a better choice than login name ...

    char login_name[LOGIN_NAME_MAX];
    getlogin_r(login_name, LOGIN_NAME_MAX);

    clientLogin = login_name; //this truncates login_name to the actual strlen...

    //32bit host identifier
    hostId = gethostid();
    //32bit process id
    pid = getpid();
    Logger::getLogger()->info("[ChronologClientImpl] Client Identity - Login: {}, EUID: {}, HostID: {}, PID: {}"
                              , clientLogin, euid, hostId, pid);
}

chronolog::ChronologClientImpl::~ChronologClientImpl()
{
    if(storyteller != nullptr)
    { delete storyteller; }

    if(rpcVisorClient != nullptr)
    { delete rpcVisorClient; }

    if(tlEngine != nullptr)
    {
        tlEngine->finalize();
        delete tlEngine;
    }

}

int chronolog::ChronologClientImpl::Connect()
{
    std::lock_guard <std::mutex> lock_client(chronologClientMutex);
    // if already connected return success
    // if disconencting return failure....
    if((clientState != UNKNOWN) && (clientState != SHUTTING_DOWN))
    {
        Logger::getLogger()->info(
                "[ChronoLogClientImpl] Already connected or in the process of shutting down. No further action taken.");
        return chronolog::CL_SUCCESS;
    }


    auto connectResponseMsg = rpcVisorClient->Connect(euid, hostId, pid);

    std::stringstream ss;
    ss << connectResponseMsg;
    Logger::getLogger()->debug("[ChronoLogClientImpl] Connection attempt to Visor completed. Response received: {}"
                               , ss.str());

    int return_code = connectResponseMsg.getErrorCode();
    if(return_code == chronolog::CL_SUCCESS)
    {
        clientState = CONNECTED;
        clientId = connectResponseMsg.getClientId();
        if(storyteller == nullptr)
        {
            storyteller = new StorytellerClient(clockProxy, *tlEngine, clientId);
        }
        //TODO: if we ever change the connection hashing algorithm we'd need to handle reconnection case with the new client_id 
    }
    else
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Connection attempt to Visor failed with error code: {}"
                                   , return_code);
    }
    return return_code;
}

int chronolog::ChronologClientImpl::Disconnect()
{
    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->debug(
                "[ChronoLogClientImpl] Already in a disconnected or shutting down state. No action taken.");
        return chronolog::CL_SUCCESS;
    }

    auto return_code = rpcVisorClient->Disconnect(clientId);
    if(return_code == chronolog::CL_SUCCESS)
    {
        clientState = SHUTTING_DOWN;
        Logger::getLogger()->info("[ChronoLogClientImpl] Successfully disconnected from Visor.");
    }
    else
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Failed to disconnect from Visor. Error code: {}"
                                   , return_code);
    }
    return return_code;

}

int chronolog::ChronologClientImpl::CreateChronicle(std::string const &chronicle_name
                                                    , const std::unordered_map <std::string, std::string> &attrs
                                                    , int &flags)
{
    if(chronicle_name.empty())
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Failed to create chronicle: No valid name provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to create chronicle '{}': Client is not connected or is shutting down."
                , chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to create the chronicle using the Visor client.
    int result = rpcVisorClient->CreateChronicle(clientId, chronicle_name, attrs, flags);

    // Log the outcome of the create operation.
    if(result == chronolog::CL_SUCCESS)
    {
        Logger::getLogger()->info("[ChronoLogClientImpl] Successfully created chronicle '{}'.", chronicle_name);
    }
    else
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Failed to create chronicle '{}'. Error code: {}"
                                   , chronicle_name, result);
    }
    return result;
}

int chronolog::ChronologClientImpl::DestroyChronicle(std::string const &chronicle_name)
{
    if(chronicle_name.empty())
    { return chronolog::CL_ERR_INVALID_ARG; }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    { return chronolog::CL_ERR_NO_CONNECTION; }

    return rpcVisorClient->DestroyChronicle(clientId, chronicle_name);
}

int chronolog::ChronologClientImpl::DestroyStory(std::string const &chronicle_name, std::string const &story_name)
{
    if(chronicle_name.empty() || story_name.empty())
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Failed to destroy chronicle: No valid name provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to destroy chronicle '{}': Client is not connected or is shutting down."
                , chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to destroy the chronicle using the Visor client.
    int result = rpcVisorClient->DestroyChronicle(clientId, chronicle_name);

    // Log the outcome of the destroy operation.
    if(result == chronolog::CL_SUCCESS)
    {
        Logger::getLogger()->info("[ChronoLogClientImpl] Successfully destroyed chronicle '{}'.", chronicle_name);
    }
    else
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Failed to destroy chronicle '{}'. Error code: {}"
                                   , chronicle_name, result);
    }
    return result;
}

std::pair <int, chronolog::StoryHandle*>
chronolog::ChronologClientImpl::AcquireStory(std::string const &chronicle_name, std::string const &story_name
                                             , const std::unordered_map <std::string, std::string> &attrs, int &flags)
{
    // Log the attempt to acquire a story with specific details.
    Logger::getLogger()->debug("[ChronoLogClientImpl] Attempting to acquire story. ChronicleName={}, StoryName={}"
                               , chronicle_name, story_name);

    if(chronicle_name.empty() || story_name.empty())
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Failed to acquire story: Missing essential parameters.");
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_ERR_INVALID_ARG, nullptr);
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to acquire story '{}' from chronicle '{}': Client is not connected or is shutting down."
                , story_name, chronicle_name);
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_ERR_NO_CONNECTION, nullptr);
    }

    // this function can be called from any client thread, so before sending an rpc request to the Visor
    // we check if the story acquisition request has been already granted to the process on some other thread
    // 
    chronolog::StoryHandle*storyHandle = storyteller->findStoryWritingHandle(chronicle_name, story_name);
    if(storyHandle != nullptr)
    {
        Logger::getLogger()->info("[ChronoLogClientImpl] Story '{}' from chronicle '{}' is already acquired."
                                  , story_name, chronicle_name);
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_SUCCESS, storyHandle);
    }

    // issue rpc request to the Visor
    auto acquireStoryResponse = rpcVisorClient->AcquireStory(clientId, chronicle_name, story_name, attrs, flags);

    std::stringstream ss;
    ss << acquireStoryResponse;
    Logger::getLogger()->debug("[ChronoLogClientImpl] Response from AcquireStory RPC call: {}", ss.str());
    if(acquireStoryResponse.getErrorCode() != chronolog::CL_SUCCESS)
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to acquire story '{}' from chronicle '{}'. Error code: {}", story_name
                , chronicle_name, acquireStoryResponse.getErrorCode());
        return std::pair <int, chronolog::StoryHandle*>(acquireStoryResponse.getErrorCode(), nullptr);
    }

    //successfull AcquireStoryResponse carries Visor generated StoryId & vector<KeeperIdCard> 
    // for the Keepers assigned to record the acquired story

    storyHandle = storyteller->initializeStoryWritingHandle(chronicle_name, story_name
                                                            , acquireStoryResponse.getStoryId()
                                                            , acquireStoryResponse.getKeepers());

    if(storyHandle == nullptr)
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Failed to initialize story handle for '{}' in chronicle '{}'."
                                   , story_name, chronicle_name);
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_ERR_UNKNOWN, nullptr);
    }
    else
    {
        Logger::getLogger()->info("[ChronoLogClientImpl] Successfully acquired story '{}' in chronicle '{}'."
                                  , story_name, chronicle_name);
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_SUCCESS, storyHandle);
    }
}

///////

int chronolog::ChronologClientImpl::ReleaseStory(std::string const &chronicle_name, std::string const &story_name)
{
    // there's no reason to waste an rpc call on empty strings...
    if(chronicle_name.empty() || story_name.empty())
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to release story: Both chronicle_name and story_name must be provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    // if we storyteller has active WritingHandle for this story
    // it should be cleared regardless of the Visor connection state

    if(nullptr == storyteller || nullptr == storyteller->findStoryWritingHandle(chronicle_name, story_name))
    {
        Logger::getLogger()->warn(
                "[ChronoLogClientImpl] No active writing handle found for story '{}' in chronicle '{}'.", story_name
                , chronicle_name);
        return chronolog::CL_ERR_NOT_EXIST;
    }

    storyteller->removeAcquiredStoryHandle(chronicle_name, story_name);
    Logger::getLogger()->info("[ChronoLogClientImpl] Successfully released the story '{}' from chronicle '{}'."
                              , story_name, chronicle_name);
    // if the client is still connected to the Visor
    // send ReleaseStory request 
    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Cannot release story '{}' from chronicle '{}' due to client being in an unknown or shutting down state."
                , story_name, chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to release the story by sending a request to the Visor.
    auto releaseStatus = rpcVisorClient->ReleaseStory(clientId, chronicle_name, story_name);
    if(releaseStatus != chronolog::CL_SUCCESS)
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to release story '{}' from chronicle '{}'. Error code: {}", story_name
                , chronicle_name, releaseStatus);
    }
    else
    {
        Logger::getLogger()->info("[ChronoLogClientImpl] Successfully released story '{}' from chronicle '{}'."
                                  , story_name, chronicle_name);
    }

    return releaseStatus;
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::GetChronicleAttr(std::string const &chronicle_name, const std::string &key
                                                     , std::string &value)
{
    value.clear();  // in case the error is returned , make sure value is an empty string

    if(chronicle_name.empty() || key.empty())
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to get attribute: Both chronicle_name and key must be provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Cannot fetch attribute for chronicle '{}': Client is in an unknown or shutting down state."
                , chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to fetch the attribute from the Visor using the RPC call.
    int fetchStatus = rpcVisorClient->GetChronicleAttr(clientId, chronicle_name, key, value);
    if(fetchStatus != chronolog::CL_SUCCESS)
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to fetch attribute '{}' for chronicle '{}'. Error code: {}", key
                , chronicle_name, fetchStatus);
    }
    else
    {
        Logger::getLogger()->info(
                "[ChronoLogClientImpl] Successfully fetched attribute '{}' for chronicle '{}'. Value: '{}'", key
                , chronicle_name, value);
    }
    return fetchStatus;
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::EditChronicleAttr(std::string const &chronicle_name, const std::string &key
                                                      , std::string const &value)
{
    if(chronicle_name.empty() || key.empty() || value.empty())
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to edit attribute: chronicle_name, key, and value must all be provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Cannot edit attribute for chronicle '{}': Client is in an unknown or shutting down state."
                , chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to edit the attribute in the Visor using the RPC call.
    int editStatus = rpcVisorClient->EditChronicleAttr(clientId, chronicle_name, key, value);
    if(editStatus != chronolog::CL_SUCCESS)
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to edit attribute '{}' for chronicle '{}'. Error code: {}", key
                , chronicle_name, editStatus);
    }
    else
    {
        Logger::getLogger()->info(
                "[ChronoLogClientImpl] Successfully edited attribute '{}' for chronicle '{}'. New value: '{}'", key
                , chronicle_name, value);
    }
    return editStatus;
}

std::vector <std::string> &chronolog::ChronologClientImpl::ShowChronicles(std::vector <std::string> &chronicles)
{
    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to fetch chronicles: Client is in an unknown or shutting down state.");
        return chronicles;
    }

    // Fetch the list of chronicles from the Visor using the RPC call.
    chronicles = rpcVisorClient->ShowChronicles(clientId);

    // Log the number of chronicles fetched and return the list.
    if(!chronicles.empty())
    {
        Logger::getLogger()->info("[ChronoLogClientImpl] Successfully fetched {} chronicles.", chronicles.size());
    }
    else
    {
        Logger::getLogger()->warn("[ChronoLogClientImpl] No chronicles found for the client.");
    }
    return chronicles;
}

std::vector <std::string> &
chronolog::ChronologClientImpl::ShowStories(std::string const &chronicle_name, std::vector <std::string> &stories)
{
    if(chronicle_name.empty())
    {
        Logger::getLogger()->error("[ChronoLogClientImpl] Failed to fetch stories: Empty chronicle name provided.");
        return stories;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        Logger::getLogger()->error(
                "[ChronoLogClientImpl] Failed to fetch stories for chronicle '{}': Client is in an unknown or shutting down state."
                , chronicle_name);
        return stories;
    }

    // Fetch stories for the given chronicle name using the RPC call.
    stories = rpcVisorClient->ShowStories(clientId, chronicle_name);

    // Log the number of stories fetched and return the list.
    if(!stories.empty())
    {
        Logger::getLogger()->info("[ChronoLogClientImpl] Successfully fetched {} stories for chronicle '{}'."
                                  , stories.size(), chronicle_name);
    }
    else
    {
        Logger::getLogger()->warn("[ChronoLogClientImpl] No stories found for chronicle '{}'.", chronicle_name);
    }
    return stories;
}
