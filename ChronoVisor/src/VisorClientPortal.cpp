#include <string>
#include <vector>
#include <map>
#include <sys/types.h>
#include <unistd.h>

#include "ConfigurationManager.h"
#include "chronolog_types.h"
#include "VisorClientPortal.h"

#include "KeeperRegistry.h"
#include "ClientRegistryManager.h"
#include "ChronicleMetaDirectory.h"
#include "ClientPortalService.h"

namespace tl = thallium;
namespace chl = chronolog;

/////////////////
chronolog::VisorClientPortal::VisorClientPortal(): clientPortalState(chl::VisorClientPortal::UNKNOWN)
                                                   , clientPortalEngine(nullptr), clientPortalService(nullptr)
                                                   , theKeeperRegistry(nullptr)  //TODO: revisit later ...
{
    // TODO: revisit the construction of registries ....
    LOG_DEBUG("[VisorClientPortal] Constructor is called. Object created at {} in thread PID={}"
         , static_cast<const void*>(this), getpid());
    //clientManager = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
    //chronicleMetaDirectory = ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance();
    clientManager.setChronicleMetaDirectory(&chronicleMetaDirectory);
    chronicleMetaDirectory.set_client_registry_manager(&clientManager);
}

////////////////
int chronolog::VisorClientPortal::StartServices(ChronoLog::ConfigurationManager const &confManager
                                                , chl::KeeperRegistry*keeperRegistry)
{
    int return_status = chronolog::CL_ERR_UNKNOWN;

    // check if already started
    if(clientPortalState != UNKNOWN)
    { return chronolog::CL_SUCCESS; }

    // TODO : keeper registry can be a member ...
    theKeeperRegistry = keeperRegistry;

    try
    {
        // initialise thalium engine for KeeperRegistryService
        std::string CLIENT_PORTAL_SERVICE_NA_STRING =
                confManager.VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
                confManager.VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP + ":" +
                std::to_string(confManager.VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT);

        uint16_t provider_id = confManager.VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;
        margo_instance_id margo_id = margo_init(CLIENT_PORTAL_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 2);

        if(MARGO_INSTANCE_NULL == margo_id)
        {
            LOG_ERROR("[VisorClientPortal] Failed to initialize margo_instance.");
            return chronolog::CL_ERR_UNKNOWN;
        }

        LOG_INFO("[VisorClientPortal] margo_instance initialized with NA_STRING {}", CLIENT_PORTAL_SERVICE_NA_STRING);

        clientPortalEngine = new tl::engine(margo_id);
        clientPortalService = chl::ClientPortalService::CreateClientPortalService(*clientPortalEngine, provider_id
                                                                                  , *this);
        clientPortalState = INITIALIZED;
        return_status = chronolog::CL_SUCCESS;

    }
    catch(tl::exception const &ex)
    {
        LOG_ERROR("[VisorClientPortal] Failed to start ClientPortal services.");
    }

    return return_status;
}

void chronolog::VisorClientPortal::ShutdownServices()
{
    clientPortalState = SHUTTING_DOWN;
}

/////////////////
chronolog::VisorClientPortal::~VisorClientPortal()
{
    LOG_DEBUG("[VisorClientPortal] Destructor is called.");

    ShutdownServices();

    if(clientPortalService != nullptr)
    { delete clientPortalService; }

    if(clientPortalEngine != nullptr)
    { delete clientPortalEngine; }
}

/**
 * Admin APIs
 */
int chronolog::VisorClientPortal::ClientConnect(uint32_t client_euid, uint32_t client_host_id, uint32_t client_pid
                                                , chl::ClientId &client_id, uint64_t &clock_offset)
{
    LOG_INFO("New Client Connected. ClientEUID={}, ClientHostID={}, ClientPID={}", client_euid, client_host_id, client_pid);
    ClientInfo record;

    if(!is_client_authenticated(client_euid))
    {
        LOG_ERROR("client_euid={} is invalid", client_euid);
        //LOG_ERROR("client_euid=%u is invalid", client_euid);
        return chronolog::CL_ERR_INVALID_ARG;
    }
    //TODO: consider different hashing mechanism that takesproduces uint32 hash value
    std::string client_account_for_hash =
            std::to_string(client_euid) + std::to_string(client_host_id) + std::to_string(client_pid);
    uint64_t client_token = CityHash64(client_account_for_hash.c_str(), client_account_for_hash.size());
    client_id = client_token;    //INNA: change this API...
    LOG_INFO("[VisorClientPortal] Client arguments: account={} host_id={} pid={} -> client_token={}", client_euid
         , client_host_id, client_pid, client_token);
    return clientManager.add_client_record(client_token, record);
}

int chronolog::VisorClientPortal::ClientDisconnect(chronolog::ClientId const &client_id)
{
    LOG_INFO("Client Disconnected. ClientID={}", client_id);
    return clientManager.remove_client_record(client_id);
}

/**
 * Metadata APIs
 */
int chronolog::VisorClientPortal::CreateChronicle(chl::ClientId const &client_id, std::string const &chronicle_name
                                                  , const std::map <std::string, std::string> &attrs
                                                  , int &flags)
{
    if(chronicle_name.empty())
    { return chronolog::CL_ERR_INVALID_ARG; }

    if(!chronicle_action_is_authorized(client_id, chronicle_name))
    { return chronolog::CL_ERR_NOT_AUTHORIZED; }

    int return_code = chronicleMetaDirectory.create_chronicle(chronicle_name, attrs);
    if(return_code == chronolog::CL_SUCCESS)
    {
        LOG_INFO("[VisorClientPortal] Chronicle created: PID={}, ClientID={}, Name={}", getpid(), client_id
             , chronicle_name.c_str());
    }
    return (return_code);
}

int
chronolog::VisorClientPortal::DestroyChronicle(chl::ClientId const &client_id, chl::ChronicleName const &chronicle_name)
{
    if(chronicle_name.empty())
    { return chronolog::CL_ERR_INVALID_ARG; }

    if(!chronicle_action_is_authorized(client_id, chronicle_name))
    { return chronolog::CL_ERR_NOT_AUTHORIZED; }

    int return_code = chronicleMetaDirectory.destroy_chronicle(chronicle_name);
    if(return_code == chronolog::CL_SUCCESS)
    {
        LOG_DEBUG("[VisorClientPortal] Chronicle destroyed: ClientID={}, ChronicleName={}", client_id
             , chronicle_name.c_str());
    }
    return (return_code);
}


int chronolog::VisorClientPortal::DestroyStory(chl::ClientId const &client_id, std::string const &chronicle_name
                                               , std::string const &story_name)
{
    LOG_INFO("[VisorClientPortal] Story destroyed: PID={}, ChronicleName={}, StoryName={}", getpid(), chronicle_name.c_str()
         , story_name.c_str());
    if(!story_action_is_authorized(client_id, chronicle_name, story_name))
    { return chronolog::CL_ERR_NOT_AUTHORIZED; }

    if(!chronicle_name.empty() && !story_name.empty())
    {
        return chronicleMetaDirectory.destroy_story(chronicle_name, story_name);
    }
    else
    {
        return chronolog::CL_ERR_INVALID_ARG;
    }
}

///////////////////

chl::AcquireStoryResponseMsg
chronolog::VisorClientPortal::AcquireStory(chl::ClientId const &client_id, std::string const &chronicle_name
                                           , std::string const &story_name
                                           , const std::map <std::string, std::string> &attrs, int &flags)
{
    chronolog::StoryId story_id{0};
    std::vector <chronolog::KeeperIdCard> recording_keepers;
    chl::ServiceId player; //ServiceID of ChronoPlayer providing playback service for this story

    if(!theKeeperRegistry->is_running())
    { return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_NO_KEEPERS, story_id, recording_keepers); }

    if(chronicle_name.empty() || story_name.empty())
    { return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_INVALID_ARG, story_id, recording_keepers); }

    if(!story_action_is_authorized(client_id, chronicle_name, story_name))
    { return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_NOT_AUTHORIZED, story_id, recording_keepers); }

    int ret = chronolog::CL_ERR_UNKNOWN;

    ret = chronicleMetaDirectory.acquire_story(client_id, chronicle_name, story_name, attrs, flags, story_id);
                                              
    if(ret != chronolog::CL_SUCCESS)
    {
        // return the error with the empty recording_keepers vector
        return chronolog::AcquireStoryResponseMsg(ret, story_id, recording_keepers);
    }
    else
    {
        LOG_INFO("[VisorClientPortal] Story acquired: PID={}, ClientID={}, ChronicleName={}, StoryName={}, Flags={}"
             , getpid(), client_id, chronicle_name.c_str(), story_name.c_str(), flags);
    }

    // if this is the first client to acquire this story we need to choose an active recording group
    // for the new story and notify the recording Keepers & Graphers
    // so that they are ready to start recording this story

    if(chronolog::CL_SUCCESS != theKeeperRegistry->notifyRecordingGroupOfStoryRecordingStart(
                                        chronicle_name, story_name, story_id, recording_keepers, player))
    {
        // RPC notification to the keepers might have failed, release the newly acquired story
        chronicleMetaDirectory.release_story(client_id, chronicle_name, story_name, story_id);
        //we do know that there's no need notify keepers of the story ending in this case as it hasn't started...
        recording_keepers.clear();
        return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_NO_KEEPERS, story_id, recording_keepers);
    }

    return chronolog::AcquireStoryResponseMsg(chronolog::CL_SUCCESS, story_id, recording_keepers, player);
}


int chronolog::VisorClientPortal::ReleaseStory(chl::ClientId const &client_id, std::string const &chronicle_name
                                               , std::string const &story_name)
{
    LOG_INFO("[VisorClientPortal] Story released: PID={}, ChronicleName={}, StoryName={}", getpid(), chronicle_name.c_str()
         , story_name.c_str());

    if(!story_action_is_authorized(client_id, chronicle_name, story_name))
    { return chronolog::CL_ERR_NOT_AUTHORIZED; }

    StoryId story_id(0);
    auto return_code = chronicleMetaDirectory.release_story(client_id, chronicle_name, story_name, story_id);
    if(chronolog::CL_SUCCESS != return_code)
    { return return_code; }

    theKeeperRegistry->notifyRecordingGroupOfStoryRecordingStop(story_id);

    return chronolog::CL_SUCCESS;
}

//////////////

int chronolog::VisorClientPortal::GetChronicleAttr(chl::ClientId const &client_id, std::string const &chronicle_name
                                                   , const std::string &key, std::string &value)
{
    LOG_DEBUG("[VisorClientPortal] Get Chronicle Attributes: PID={}, Name={}, Key={}", getpid(), chronicle_name.c_str()
         , key.c_str());
    if(chronicle_name.empty() || key.empty())
    { return chronolog::CL_ERR_INVALID_ARG; }

    // TODO: add authorization check : if ( chronicle_action_is_authorized())
    return chronicleMetaDirectory.get_chronicle_attr(chronicle_name, key, value);
}

//////////////

int chronolog::VisorClientPortal::EditChronicleAttr(chl::ClientId const &client_id, std::string const &chronicle
                                                    , std::string const &key, std::string const &value)
{
    LOG_DEBUG("[VisorClientPortal] Edit Chronicle Attributes: PID={}, Name={}, Key={}, Value={}", getpid(), chronicle.c_str()
         , key.c_str(), value.c_str());
    if(chronicle.empty() || key.empty() || value.empty())
    { return chronolog::CL_ERR_INVALID_ARG; }

    // TODO: add authorization check : if ( chronicle_action_is_authorized())
    return chronicleMetaDirectory.edit_chronicle_attr(chronicle, key, value);
}

int chronolog::VisorClientPortal::ShowChronicles(chl::ClientId const &client_id, std::vector <std::string> &chronicles)
{
    LOG_DEBUG("[VisorClientPortal] Show Chronicles: PID={}, ClientID={}", getpid(), client_id);

    // TODO: add client_id authorization check : if ( chronicle_action_is_authorized())
    chronicleMetaDirectory.show_chronicles(chronicles);

    return chronolog::CL_SUCCESS;
}

int chronolog::VisorClientPortal::ShowStories(chl::ClientId const &client_id, std::string const &chronicle_name
                                              , std::vector <std::string> &stories)
{
    LOG_DEBUG("[VisorClientPortal] Show Stories: PID={}, ChronicleName={}", getpid(), chronicle_name.c_str());

    if(!chronicle_action_is_authorized(client_id, chronicle_name))
    { return chronolog::CL_ERR_UNKNOWN; }

    chronicleMetaDirectory.show_stories(chronicle_name, stories);

    return chronolog::CL_SUCCESS;
}


/*
/////////////////
int chronolog::VisorClientPortal::LocalDestroyStory(std::string const& chronicle_name, std::string const&story_name)
{    int return_code = chronolog::CL_SUCCESS;

    return return_code;
}


/////////////////
AcquireStoryResponseMsg const& chronolog::VisorClientPortal::LocalAcquireStory(
                              chronolog::ClientId const&client_id
                              , std::string const&chronicle_name,
                              std::string const&story_name,
                              const std::unordered_map<std::string, std::string> &attrs,
                              int &flags)
{
    int return_code = chronolog::CL_SUCCESS;

    return return_code;
}


/////////////////
int chronolog::VisorClientPortal::LocalReleaseStory( chronolog::ClientId const&client_id
            , std::string const&chronicle_name, std::string const&story_name)
{
    int return_code = chronolog::CL_SUCCESS;

    return return_code;
}


/////////////////
int chronolog::VisorClientPortal::LocalGetChronicleAttr( chronolog::ClientId const& client_id
            , std::string const&name, const std::string &key, std::string &value)
{
    int return_code = chronolog::CL_SUCCESS;

    return return_code;
}

/////////////////
int chronolog::VisorClientPortal::LocalEditChronicleAttr( chronolog::ClientId const& client_id
            , std::string const&name, const std::string &key, const std::string &value)
{
    int return_code = chronolog::CL_SUCCESS;

    return return_code;
}


*/
/////////////////

bool chronolog::VisorClientPortal::is_client_authenticated(uint32_t client_account)
{
    return true;
}

///////////////

bool
chronolog::VisorClientPortal::chronicle_action_is_authorized(chl::ClientId const &client_id, chl::ChronicleName const &)
{
    return true;
}
////////////////

bool chronolog::VisorClientPortal::story_action_is_authorized(chronolog::ClientId const &client_id
                                                              , chl::ChronicleName const &chronicle
                                                              , chl::StoryName const &story_name)
{
    return true;
}

