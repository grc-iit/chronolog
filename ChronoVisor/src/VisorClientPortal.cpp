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

    LOGD("%s constructor is called", typeid(*this).name());
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
            std::cout << "VisorClientPortal : Failed to initialize margo_instance" << std::endl;
            return chronolog::CL_ERR_UNKNOWN;
        }

        std::cout << "VisorClientPortal : :margo_instance initialized with NA_STRING" << "{"
                  << CLIENT_PORTAL_SERVICE_NA_STRING << "}" << std::endl;

        clientPortalEngine = new tl::engine(margo_id);

        clientPortalService = chl::ClientPortalService::CreateClientPortalService(*clientPortalEngine, provider_id
                                                                                  , *this);

        clientPortalState = INITIALIZED;

        return_status = chronolog::CL_SUCCESS;

    }
    catch(tl::exception const &ex)
    {
        std::cout << "VisorClientPortal : Failed to start ClientPortal services" << std::endl;
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
    std::cout << "VisorClientPortal::~VisorClientPortal" << std::endl;

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
    LOGD("%s called with args: account=%u host_id=%u pid=%u", __FUNCTION__, client_euid, client_host_id, client_pid);
    ClientInfo record;

    if(!is_client_authenticated(client_euid))
    {
        LOGE("client_euid=%u is invalid", client_euid);
        return CL_ERR_INVALID_ARG;
    }
    //TODO: consider different hashing mechanism that takesproduces uint32 hash value
    std::string client_account_for_hash =
            std::to_string(client_euid) + std::to_string(client_host_id) + std::to_string(client_pid);
    uint64_t client_token = CityHash64(client_account_for_hash.c_str(), client_account_for_hash.size());
    client_id = client_token;    //INNA: change this API...
    LOGD("%s  with args: account=%u host_id=%u pid=%u -> client_token=%lu", __FUNCTION__, client_euid, client_host_id
         , client_pid, client_token);
    return clientManager.add_client_record(client_token, record);
}

int chronolog::VisorClientPortal::ClientDisconnect(chronolog::ClientId const &client_id)
{
    LOGD("%s is called with args: client_id=%lu", __FUNCTION__, client_id);

    return clientManager.remove_client_record(client_id);
}

/**
 * Metadata APIs
 */
int chronolog::VisorClientPortal::CreateChronicle(chl::ClientId const &client_id, std::string const &chronicle_name
                                                  , const std::unordered_map <std::string, std::string> &attrs
                                                  , int &flags)
{
    LOGD("%s is called in PID=%d - START, with args: clientId:%lu name=%s ", __FUNCTION__, getpid(), client_id
         , chronicle_name.c_str());

    if(chronicle_name.empty())
    { return chronolog::CL_ERR_INVALID_ARG; }

    if(!chronicle_action_is_authorized(client_id, chronicle_name))
    { return CL_ERR_NOT_AUTHORIZED; }

    int return_code = chronicleMetaDirectory.create_chronicle(chronicle_name);

    return (return_code);
}

int
chronolog::VisorClientPortal::DestroyChronicle(chl::ClientId const &client_id, chl::ChronicleName const &chronicle_name)
{
    LOGD("%s is called with args: client_id=%lu chronicle_name=%s", __FUNCTION__, client_id, chronicle_name.c_str());
    if(chronicle_name.empty())
    { return chronolog::CL_ERR_INVALID_ARG; }

    if(!chronicle_action_is_authorized(client_id, chronicle_name))
    { return CL_ERR_NOT_AUTHORIZED; }

    return chronicleMetaDirectory.destroy_chronicle(chronicle_name);
}


int chronolog::VisorClientPortal::DestroyStory(chl::ClientId const &client_id, std::string const &chronicle_name
                                               , std::string const &story_name)
{
    LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s", __FUNCTION__, getpid()
         , chronicle_name.c_str(), story_name.c_str());

    if(!story_action_is_authorized(client_id, chronicle_name, story_name))
    { return CL_ERR_NOT_AUTHORIZED; }

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
                                           , const std::unordered_map <std::string, std::string> &attrs, int &flags)
{
    LOGD("%s is called in PID=%d, with args: client_id:%lu chronicle_name=%s, story_name=%s, flags=%d", __FUNCTION__
         , getpid(), client_id, chronicle_name.c_str(), story_name.c_str(), flags);

    chronolog::StoryId story_id{0};
    std::vector <chronolog::KeeperIdCard> recording_keepers;

    if(!theKeeperRegistry->is_running())
    { return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_NO_KEEPERS, story_id, recording_keepers); }

    if(chronicle_name.empty() || story_name.empty())
    { return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_INVALID_ARG, story_id, recording_keepers); }

    if(!story_action_is_authorized(client_id, chronicle_name, story_name))
    { return chronolog::AcquireStoryResponseMsg(CL_ERR_NOT_AUTHORIZED, story_id, recording_keepers); }

    int ret = CL_ERR_UNKNOWN;
    //ret = chronicleMetaDirectory.create_story(chronicle_name, story_name, attrs);

    //if (ret != CL_SUCCESS && ret != CL_ERR_STORY_EXISTS)
    //{ return chronolog::AcquireStoryResponseMsg(ret, story_id, recording_keepers); }

    bool notify_keepers = false;
    ret = chronicleMetaDirectory.acquire_story(client_id, chronicle_name, story_name, attrs, flags, story_id
                                               , notify_keepers);
    if(ret != chronolog::CL_SUCCESS)
    { return chronolog::AcquireStoryResponseMsg(ret, story_id, recording_keepers); }

    recording_keepers = theKeeperRegistry->getActiveKeepers(recording_keepers);
    // if this is the first client to acquire this story we need to notify the recording Keepers
    // so that they are ready to start recording this story
    if(notify_keepers)
    {
        if(chronolog::CL_SUCCESS !=
           theKeeperRegistry->notifyKeepersOfStoryRecordingStart(recording_keepers, chronicle_name, story_name
                                                                 , story_id))
        {  // RPC notification to the keepers might have failed, release the newly acquired story
            chronicleMetaDirectory.release_story(client_id, chronicle_name, story_name, story_id, notify_keepers);
            //we do know that there's no need notify keepers of the story ending in this case as it hasn't started...
            //return CL_ERR_NO_KEEPERS;
            return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_NO_KEEPERS, story_id, recording_keepers);
        }

    }

    return chronolog::AcquireStoryResponseMsg(chronolog::CL_SUCCESS, story_id, recording_keepers);
}


int chronolog::VisorClientPortal::ReleaseStory(chl::ClientId const &client_id, std::string const &chronicle_name
                                               , std::string const &story_name)
{
    LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s", __FUNCTION__, getpid()
         , chronicle_name.c_str(), story_name.c_str());

    if(!story_action_is_authorized(client_id, chronicle_name, story_name))
    { return CL_ERR_NOT_AUTHORIZED; }

    StoryId story_id(0);
    bool notify_keepers = false;
    auto return_code = chronicleMetaDirectory.release_story(client_id, chronicle_name, story_name, story_id
                                                            , notify_keepers);
    if(chronolog::CL_SUCCESS != return_code)
    { return return_code; }

    if(notify_keepers && theKeeperRegistry->is_running())
    {
        std::vector <chronolog::KeeperIdCard> recording_keepers;
        theKeeperRegistry->notifyKeepersOfStoryRecordingStop(theKeeperRegistry->getActiveKeepers(recording_keepers)
                                                             , story_id);
    }
    LOGD("%s finished in PID=%d, with args: chronicle_name=%s, story_name=%s", __FUNCTION__, getpid()
         , chronicle_name.c_str(), story_name.c_str());
    return chronolog::CL_SUCCESS;
}

//////////////

int chronolog::VisorClientPortal::GetChronicleAttr(chl::ClientId const &client_id, std::string const &chronicle_name
                                                   , const std::string &key, std::string &value)
{
    LOGD("%s is called in PID=%d, with args: name=%s, key=%s", __FUNCTION__, getpid(), chronicle_name.c_str()
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
    LOGD("%s is called in PID=%d, with args: name=%s, key=%s, value=%s", __FUNCTION__, getpid(), chronicle.c_str()
         , key.c_str(), value.c_str());
    if(chronicle.empty() || key.empty() || value.empty())
    { return chronolog::CL_ERR_INVALID_ARG; }


    // TODO: add authorization check : if ( chronicle_action_is_authorized())

    return chronicleMetaDirectory.edit_chronicle_attr(chronicle, key, value);
}

int chronolog::VisorClientPortal::ShowChronicles(chl::ClientId const &client_id, std::vector <std::string> &chronicles)
{
    // TODO: add client_id authorization check : if ( chronicle_action_is_authorized())

    chronicleMetaDirectory.show_chronicles(chronicles);

    return chronolog::CL_SUCCESS;
}

int chronolog::VisorClientPortal::ShowStories(chl::ClientId const &client_id, std::string const &chronicle_name
                                              , std::vector <std::string> &stories)
{
    LOGD("%s is called in PID=%d, chronicle_name=%s", __FUNCTION__, getpid(), chronicle_name.c_str());

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

