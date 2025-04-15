#include <unistd.h>
#include <string>
#include "ChronologClientImpl.h"
#include "StorytellerClient.h"
#include "city.h"

namespace chl = chronolog;

std::mutex chronolog::ChronologClientImpl::chronologClientMutex;
chronolog::ChronologClientImpl*chronolog::ChronologClientImpl::chronologClientImplInstance{nullptr};

chronolog::ChronologClientImpl*chronolog::ChronologClientImpl::GetClientImplInstance(
        chronolog::ClientPortalServiceConf const &visorClientPortalServiceConf)
{
    chrono_monitor::initialize("file", "/tmp/chrono_client.log", spdlog::level::info, "chrono_client", 1024000, 3
                               , spdlog::level::warn);
        
    chronolog::ClientQueryServiceConf clientQueryServiceConf;

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if(chronologClientImplInstance == nullptr)
    {
        chronologClientImplInstance = new ChronologClientImpl(clientQueryServiceConf, visorClientPortalServiceConf);
    }

    return chronologClientImplInstance;
}

chronolog::ChronologClientImpl::ChronologClientImpl(
    chronolog::ClientQueryServiceConf const& clientQueryServiceConf,
    chronolog::ClientPortalServiceConf const& clientPortalServiceConf)
        : clientState(UNKNOWN)
        , clientLogin("")
        , hostId(0), pid(0), clientId(0)
        , tlEngine(nullptr)
        , rpcVisorClient(nullptr)
        , storyteller(nullptr)
        , storyReaderService(nullptr)
{

    defineClientIdentity();

    tlEngine = new thallium::engine(clientQueryServiceConf.proto_conf(), THALLIUM_SERVER_MODE, true, 1);
    
    storyReaderService= chl::ClientQueryService::CreateClientQueryService(*tlEngine, chronolog::ServiceId(clientQueryServiceConf.proto_conf(), 
                                        hostId, clientQueryServiceConf.port(), clientQueryServiceConf.provider_id()));

    std::string CLIENT_VISOR_NA_STRING =
            clientPortalServiceConf.proto_conf() + "://" + clientPortalServiceConf.ip() + ":" +
            std::to_string(clientPortalServiceConf.port());

    rpcVisorClient = chl::RpcVisorClient::CreateRpcVisorClient(*tlEngine, CLIENT_VISOR_NA_STRING
                                                               , clientPortalServiceConf.provider_id());
}

////////

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
    LOG_INFO("[ChronologClientImpl] Client Identity - Login: {}, EUID: {}, HostID: {}, PID: {}", clientLogin, euid
             , hostId, pid);
}

chronolog::ChronologClientImpl::~ChronologClientImpl()
{
    if(storyteller != nullptr)
    { delete storyteller; }

    if(rpcVisorClient != nullptr)
    { delete rpcVisorClient; }

    if(storyReaderService)
    { delete storyReaderService; }

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
        LOG_INFO(
                "[ChronoLogClientImpl] Already connected or in the process of shutting down. No further action taken.");
        return chronolog::CL_SUCCESS;
    }


    auto connectResponseMsg = rpcVisorClient->Connect(euid, hostId, pid);

    std::stringstream ss;
    ss << connectResponseMsg;
    LOG_DEBUG("[ChronoLogClientImpl] Connection attempt to Visor completed. Response received: {}", ss.str());

    int return_code = connectResponseMsg.getErrorCode();
    if(return_code == chronolog::CL_SUCCESS)
    {
        clientState = CONNECTED;
        clientId = connectResponseMsg.getClientId();
        if(storyteller == nullptr)
        {
            storyteller = new StorytellerClient(clockProxy, *storyReaderService, clientId);
        }
        //TODO: if we ever change the connection hashing algorithm we'd need to handle reconnection case with the new client_id 
    }
    else
    {
        LOG_ERROR("[ChronoLogClientImpl] Connection attempt to Visor failed with error code: {}", return_code);
    }
    return return_code;
}

int chronolog::ChronologClientImpl::Disconnect()
{
    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_DEBUG("[ChronoLogClientImpl] Already in a disconnected or shutting down state. No action taken.");
        return chronolog::CL_SUCCESS;
    }

    auto return_code = rpcVisorClient->Disconnect(clientId);
    if(return_code == chronolog::CL_SUCCESS)
    {
        clientState = SHUTTING_DOWN;
        LOG_INFO("[ChronoLogClientImpl] Successfully disconnected from Visor.");
    }
    else
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to disconnect from Visor. Error code: {}", return_code);
    }
    return return_code;

}

int chronolog::ChronologClientImpl::CreateChronicle(std::string const &chronicle_name
                                                    , const std::map <std::string, std::string> &attrs, int &flags)
{
    if(chronicle_name.empty())
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to create chronicle: No valid name provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to create chronicle '{}': Client is not connected or is shutting down."
                  , chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to create the chronicle using the Visor client.
    int result = rpcVisorClient->CreateChronicle(clientId, chronicle_name, attrs, flags);

    // Log the outcome of the create operation.
    if(result == chronolog::CL_SUCCESS)
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully created chronicle '{}'.", chronicle_name);
    }
    else
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to create chronicle '{}'. Error code: {}", chronicle_name, result);
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

    int result = rpcVisorClient->DestroyChronicle(clientId, chronicle_name);

    // Log the outcome of the destroy operation.
    if(result == chronolog::CL_SUCCESS)
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully destroyed chronicle '{}'.", chronicle_name);
    }
    else
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to destroy chronicle '{}'. Error code: {}", chronicle_name, result);
    }

    return result;
}

int chronolog::ChronologClientImpl::DestroyStory(std::string const &chronicle_name, std::string const &story_name)
{
    if(chronicle_name.empty() || story_name.empty())
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to destroy chronicle: No valid name provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to destroy chronicle '{}': Client is not connected or is shutting down."
                  , chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to destroy the chronicle using the Visor client.
    int result = rpcVisorClient->DestroyStory(clientId, chronicle_name, story_name);

    // Log the outcome of the destroy operation.
    if(result == chronolog::CL_SUCCESS)
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully destroyed Story '{}' from Chronicle '{}'.", story_name
                 , chronicle_name);
    }
    else
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to destroy Story '{}' from Chronicle '{}'. Error code: {}", story_name
                  , chronicle_name, result);
    }
    return result;
}

std::pair <int, chronolog::StoryHandle*>
chronolog::ChronologClientImpl::AcquireStory(std::string const &chronicle_name, std::string const &story_name
                                             , const std::map <std::string, std::string> &attrs, int &flags)
{
    // Log the attempt to acquire a story with specific details.
    LOG_DEBUG("[ChronoLogClientImpl] Attempting to acquire story. ChronicleName={}, StoryName={}", chronicle_name
              , story_name);

    if(chronicle_name.empty() || story_name.empty())
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to acquire story: Missing essential parameters.");
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_ERR_INVALID_ARG, nullptr);
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_ERROR(
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
        LOG_INFO("[ChronoLogClientImpl] Story '{}' from chronicle '{}' is already acquired.", story_name
                 , chronicle_name);
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_SUCCESS, storyHandle);
    }

    // issue rpc request to the Visor
    auto acquireStoryResponse = rpcVisorClient->AcquireStory(clientId, chronicle_name, story_name, attrs, flags);

    std::stringstream ss;
    ss << acquireStoryResponse;
    LOG_DEBUG("[ChronoLogClientImpl] Response from AcquireStory RPC call: {}", ss.str());
    if(acquireStoryResponse.getErrorCode() != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to acquire story '{}' from chronicle '{}'. Error code: {}", story_name
                  , chronicle_name, acquireStoryResponse.getErrorCode());
        return std::pair <int, chronolog::StoryHandle*>(acquireStoryResponse.getErrorCode(), nullptr);
    }

    //successfull AcquireStoryResponse carries Visor generated StoryId & vector<KeeperIdCard> 
    // for the Keepers assigned to record the acquired story

    storyHandle = storyteller->initializeStoryWritingHandle(chronicle_name, story_name
                                                            , acquireStoryResponse.getStoryId()
                                                            , acquireStoryResponse.getKeepers()
                    , acquireStoryResponse.getPlayer());

    if(storyHandle == nullptr)
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to initialize story handle for '{}' in chronicle '{}'.", story_name
                  , chronicle_name);
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_ERR_UNKNOWN, nullptr);
    }
    else
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully acquired story '{}' in chronicle '{}'.", story_name
                 , chronicle_name);
        return std::pair <int, chronolog::StoryHandle*>(chronolog::CL_SUCCESS, storyHandle);
    }
}

///////

int chronolog::ChronologClientImpl::ReleaseStory(std::string const &chronicle_name, std::string const &story_name)
{
    // there's no reason to waste an rpc call on empty strings...
    if(chronicle_name.empty() || story_name.empty())
    {
        LOG_ERROR(
                "[ChronoLogClientImpl] Failed to release story: Both chronicle_name and story_name must be provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    // if we storyteller has active WritingHandle for this story
    // it should be cleared regardless of the Visor connection state

    if(nullptr == storyteller || nullptr == storyteller->findStoryWritingHandle(chronicle_name, story_name))
    {
        LOG_WARNING("[ChronoLogClientImpl] No active writing handle found for story '{}' in chronicle '{}'.", story_name
                    , chronicle_name);
        return chronolog::CL_ERR_NOT_ACQUIRED;
    }

    storyteller->removeAcquiredStoryHandle(chronicle_name, story_name);
    LOG_INFO("[ChronoLogClientImpl] Successfully released the story '{}' from chronicle '{}'.", story_name
             , chronicle_name);
    // if the client is still connected to the Visor
    // send ReleaseStory request 
    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_ERROR(
                "[ChronoLogClientImpl] Cannot release story '{}' from chronicle '{}' due to client being in an unknown or shutting down state."
                , story_name, chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to release the story by sending a request to the Visor.
    auto releaseStatus = rpcVisorClient->ReleaseStory(clientId, chronicle_name, story_name);
    if(releaseStatus != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to release story '{}' from chronicle '{}'. Error code: {}", story_name
                  , chronicle_name, releaseStatus);
    }
    else
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully released story '{}' from chronicle '{}'.", story_name
                 , chronicle_name);
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
        LOG_ERROR("[ChronoLogClientImpl] Failed to get attribute: Both chronicle_name and key must be provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_ERROR(
                "[ChronoLogClientImpl] Cannot fetch attribute for chronicle '{}': Client is in an unknown or shutting down state."
                , chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to fetch the attribute from the Visor using the RPC call.
    int fetchStatus = rpcVisorClient->GetChronicleAttr(clientId, chronicle_name, key, value);
    if(fetchStatus != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to fetch attribute '{}' for chronicle '{}'. Error code: {}", key
                  , chronicle_name, fetchStatus);
    }
    else
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully fetched attribute '{}' for chronicle '{}'. Value: '{}'", key
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
        LOG_ERROR(
                "[ChronoLogClientImpl] Failed to edit attribute: chronicle_name, key, and value must all be provided.");
        return chronolog::CL_ERR_INVALID_ARG;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_ERROR(
                "[ChronoLogClientImpl] Cannot edit attribute for chronicle '{}': Client is in an unknown or shutting down state."
                , chronicle_name);
        return chronolog::CL_ERR_NO_CONNECTION;
    }

    // Attempt to edit the attribute in the Visor using the RPC call.
    int editStatus = rpcVisorClient->EditChronicleAttr(clientId, chronicle_name, key, value);
    if(editStatus != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to edit attribute '{}' for chronicle '{}'. Error code: {}", key
                  , chronicle_name, editStatus);
    }
    else
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully edited attribute '{}' for chronicle '{}'. New value: '{}'", key
                 , chronicle_name, value);
    }
    return editStatus;
}

std::vector <std::string> &chronolog::ChronologClientImpl::ShowChronicles(std::vector <std::string> &chronicles)
{
    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to fetch chronicles: Client is in an unknown or shutting down state.");
        return chronicles;
    }

    // Fetch the list of chronicles from the Visor using the RPC call.
    chronicles = rpcVisorClient->ShowChronicles(clientId);

    // Log the number of chronicles fetched and return the list.
    if(!chronicles.empty())
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully fetched {} chronicles.", chronicles.size());
    }
    else
    {
        LOG_WARNING("[ChronoLogClientImpl] No chronicles found for the client.");
    }
    return chronicles;
}

std::vector <std::string> &
chronolog::ChronologClientImpl::ShowStories(std::string const &chronicle_name, std::vector <std::string> &stories)
{
    if(chronicle_name.empty())
    {
        LOG_ERROR("[ChronoLogClientImpl] Failed to fetch stories: Empty chronicle name provided.");
        return stories;
    }

    std::lock_guard <std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {
        LOG_ERROR(
                "[ChronoLogClientImpl] Failed to fetch stories for chronicle '{}': Client is in an unknown or shutting down state."
                , chronicle_name);
        return stories;
    }

    // Fetch stories for the given chronicle name using the RPC call.
    stories = rpcVisorClient->ShowStories(clientId, chronicle_name);

    // Log the number of stories fetched and return the list.
    if(!stories.empty())
    {
        LOG_INFO("[ChronoLogClientImpl] Successfully fetched {} stories for chronicle '{}'.", stories.size()
                 , chronicle_name);
    }
    else
    {
        LOG_WARNING("[ChronoLogClientImpl] No stories found for chronicle '{}'.", chronicle_name);
    }
    return stories;
}

//////////////////////////////

