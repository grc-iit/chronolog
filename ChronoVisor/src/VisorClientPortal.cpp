
#include <string>
#include <vector>
#include <map>
#include <thallium.hpp>  

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
chronolog::VisorClientPortal::VisorClientPortal() // ChronoLog::ConfigurationManager & configuration)
        : clientPortalState(chl::VisorClientPortal::UNKNOWN)
        , clientPortalEngine(nullptr)
        , clientPortalService(nullptr)
        , theKeeperRegistry(nullptr)  //TODO: revisit later ...
{

    // TODO: revisit the construction of registries ....

            LOGD("%s constructor is called", typeid(*this).name());
    //clientManager = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
    //chronicleMetaDirectory = ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance();
    clientManager.setChronicleMetaDirectory( &chronicleMetaDirectory);
    chronicleMetaDirectory.set_client_registry_manager(&clientManager);


}

////////////////
int chronolog::VisorClientPortal::StartServices(ChronoLog::ConfigurationManager const& confManager
        ,  chl::KeeperRegistry* keeperRegistry)
{
    // TODO : keeper registry can be a member ...
    theKeeperRegistry= keeperRegistry;
    
   
    // if(visorPortalState == UNKNOWN)
    // {
     //INNA: TODO: add exception handling ...    
     // initialise thalium engine for KeeperRegistryService

         std::string CLIENT_PORTAL_SERVICE_NA_STRING=
         confManager.RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string()
         +"://" + confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string()
         +":" + std::to_string(confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT);
 
         uint16_t provider_id =  confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.SERVICE_PROVIDER_ID;
 
         margo_instance_id margo_id=margo_init(CLIENT_PORTAL_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 2);
 
         if(MARGO_INSTANCE_NULL == margo_id)
         {
              std::cout<<"VisorClientPortal : Failed to initialize margo_instance"<<std::endl;
              return -1;
          }
          std::cout<<"VisorClientPortal : :margo_instance initialized with NA_STRING"
              << "{"<<CLIENT_PORTAL_SERVICE_NA_STRING<<"}" <<std::endl;
 
         clientPortalEngine =  new tl::engine(margo_id);
 
          clientPortalService =
          chl::ClientPortalService::CreateClientPortalService(*clientPortalEngine, provider_id, *this);
  
      //    visorPortalState = INITIALIZED;
      //}
        clientPortalEngine->wait_for_finalize();
 
    return CL_SUCCESS;
}

/////////////////
chronolog::VisorClientPortal::~VisorClientPortal()
{
    std::cout << "VisorClientPortal::~VisorClientPortal"<<std::endl;

    //ShutdownServices();
    // TODO : check that everything is kosher here ... 
    if(clientPortalService != nullptr)
    {   delete clientPortalService; }
    
    if(clientPortalEngine != nullptr)
    {   delete clientPortalEngine;   }

}

    /**
     * Admin APIs
     */
int chronolog::VisorClientPortal::ClientConnect( std::string const& client_account, uint32_t client_host_ip
            ,  chl::ClientId & client_id, uint64_t &clock_offset) 
{
        LOGD("%s in ChronoLogAdminRPCProxy@%p called in PID=%d, with args: account=%s",
             __FUNCTION__, this, getpid(), client_account.c_str());
        ClientInfo record;
        record.addr_ = "127.0.0.1";   
        //TODO: process real ip + account instead and generate client_id
        if (std::strtol(client_account.c_str(), nullptr, 10) < 0) {
            LOGE("client_id=%s is invalid", client_account.c_str());
            return CL_ERR_INVALID_ARG;
        }
        return clientManager.add_client_record(client_account,record);
    }

int chronolog::VisorClientPortal::ClientDisconnect(std::string const& client_id) {
        LOGD("%s is called in PID=%d, with args: client_id=%s",
             __FUNCTION__, getpid(), client_id.c_str());
        if (std::strtol(client_id.c_str(), nullptr, 10) < 0) {
            LOGE("client_id=%s is invalid", client_id.c_str());
            return CL_ERR_INVALID_ARG;
        }

        return clientManager.remove_client_record(client_id);
    }

    /**
     * Metadata APIs
     */
int chronolog::VisorClientPortal::CreateChronicle(chl::ClientId const& client_id
            , std::string const& name, const std::unordered_map<std::string, std::string> &attrs, int &flags) 
{
    LOGD("%s is called in PID=%d - START, with args: name=%s, attrs=", __FUNCTION__, getpid(), name.c_str());

    if (name.empty()) 
    {   return CL_ERR_INVALID_ARG; }
        
    // TODO: add authorization check : if ( chronicle_action_is_authorized())
            
    int return_code=chronicleMetaDirectory.create_chronicle(name);
        LOGD("%s is called in PID=%d - END, with args: name=%s, return_code=%d", __FUNCTION__, getpid(), name.c_str(), return_code);

    return(return_code);
}

int chronolog::VisorClientPortal::DestroyChronicle(chl::ClientId const&, chl::ChronicleName const& name) 
{
    LOGD("%s is called in PID=%d, with args: name=%s", __FUNCTION__, getpid(), name.c_str());
    if (name.empty()) 
    {   return CL_ERR_INVALID_ARG; }

    // TODO: add authorization check : if ( chronicle_action_is_authorized())
            
    return chronicleMetaDirectory.destroy_chronicle(name);
}


int chronolog::VisorClientPortal::DestroyStory( chl::ClientId const& client_id, std::string const& chronicle_name, std::string const& story_name) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());
    // TODO: add authorization check : if ( chronicle_action_is_authorized())
        if (!chronicle_name.empty() && !story_name.empty()) {
            return chronicleMetaDirectory.destroy_story(chronicle_name,story_name);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

///////////////////

chl::AcquireStoryResponseMsg chronolog::VisorClientPortal::AcquireStory(std::string const& client_id,
                          std::string const& chronicle_name,
                          std::string const& story_name,
                          const std::unordered_map<std::string, std::string> &attrs,
                          int& flags
                       // , chl::AcquireStoryResponseMsg &
            ) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
	
	chronolog::StoryId story_id{0};
	std::vector<chronolog::KeeperIdCard> recording_keepers;
	
	if( !theKeeperRegistry->is_running())
	//{ return CL_ERR_NO_KEEPERS; }
        {return chronolog::AcquireStoryResponseMsg (CL_ERR_NO_KEEPERS, story_id, recording_keepers); }

        if (chronicle_name.empty() || story_name.empty()) 
	//{ //TODO : add this check on the client side, 
	  //there's no need to waste the RPC on empty strings...
	  //return CL_ERR_INVALID_ARG;
	//}
        {return chronolog::AcquireStoryResponseMsg (CL_ERR_INVALID_ARG, story_id, recording_keepers); }

	// TODO : create_stroy should be part of acquire_story 
            int ret = chronicleMetaDirectory.create_story(chronicle_name, story_name, attrs);
            if (ret != CL_SUCCESS)
	    //{ return ret; }
        {return chronolog::AcquireStoryResponseMsg (ret, story_id, recording_keepers); }

	// TODO : StoryId token and recordingKeepers vector need to be returned to the client 
	// when the client side RPC is updated to receive them
        bool notify_keepers = false;
        ret = chronicleMetaDirectory.acquire_story(client_id, chronicle_name, story_name, flags, story_id,notify_keepers);
	if(ret != CL_SUCCESS)
	//{ return ret; }
        {return chronolog::AcquireStoryResponseMsg (ret, story_id, recording_keepers); }

	recording_keepers = theKeeperRegistry->getActiveKeepers(recording_keepers);
	// if this is the first client to acquire this story we need to notify the recording Keepers 
	// so that they are ready to start recording this story
	if(notify_keepers)
	{
	    if( 0 != theKeeperRegistry->notifyKeepersOfStoryRecordingStart(recording_keepers, chronicle_name, story_name,story_id))
	    {  // RPC notification to the keepers might have failed, release the newly acquired story 
	       chronicleMetaDirectory.release_story(client_id, chronicle_name,story_name,story_id, notify_keepers);
	       //TODO: chronicleMetaDirectory->release_story(client_id, story_id, notify_keepers); 
	       //we do know that there's no need notify keepers of the story ending in this case as it hasn't started...
	       //return CL_ERR_NO_KEEPERS;
               return chronolog::AcquireStoryResponseMsg (CL_ERR_NO_KEEPERS, story_id, recording_keepers); 
	    }
	    
	}
	
	//chronolog::AcquireStoryResponseMsg (CL_SUCCESS, story_id, recording_keepers);

        LOGD("%s finished  in PID=%d, with args: chronicle_name=%s, story_name=%s",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());
   // return CL_SUCCESS; 
    return chronolog::AcquireStoryResponseMsg (CL_SUCCESS, story_id, recording_keepers);
    }


int chronolog::VisorClientPortal::ReleaseStory(std::string const& client_id, std::string const& chronicle_name, std::string const& story_name) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());

	//TODO: add this check on the client side so we dont' waste RPC call on empty strings...
        if (chronicle_name.empty() || story_name.empty()) 
	{ return CL_ERR_INVALID_ARG; }

	StoryId story_id(0);
	bool notify_keepers = false;
        auto return_code = chronicleMetaDirectory.release_story(client_id, chronicle_name, story_name, story_id, notify_keepers);
	if(CL_SUCCESS != return_code)
	{  return return_code; }

	if( notify_keepers && theKeeperRegistry->is_running() )
	{  
	  std::vector<chronolog::KeeperIdCard> recording_keepers;
	  theKeeperRegistry->notifyKeepersOfStoryRecordingStop( theKeeperRegistry->getActiveKeepers(recording_keepers), story_id);
	}
        LOGD("%s finished in PID=%d, with args: chronicle_name=%s, story_name=%s", 
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str() );
    return CL_SUCCESS;
    }

//////////////

int chronolog::VisorClientPortal::GetChronicleAttr( chl::ClientId const& client_id
                , std::string const& chronicle_name, const std::string &key, std::string &value) 
{
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s", __FUNCTION__, getpid(), chronicle_name.c_str(), key.c_str());
    if ( chronicle_name.empty() || key.empty()) 
    {   return CL_ERR_INVALID_ARG;  }


        // TODO: add authorization check : if ( chronicle_action_is_authorized())

    return chronicleMetaDirectory.get_chronicle_attr( chronicle_name, key, value);
}

//////////////

int chronolog::VisorClientPortal::EditChronicleAttr(chl::ClientId const& client_id
                , std::string const& chronicle, std::string const& key, std::string const& value) 
{
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s, value=%s",
             __FUNCTION__, getpid(), chronicle.c_str(), key.c_str(), value.c_str());
    if (chronicle.empty() || key.empty() || value.empty()) 
    {   return CL_ERR_INVALID_ARG; }


        // TODO: add authorization check : if ( chronicle_action_is_authorized())

    return chronicleMetaDirectory.edit_chronicle_attr(chronicle, key, value);
}

int chronolog::VisorClientPortal::ShowChronicles(chl::ClientId const& client_id, std::vector<std::string> & chronicles )
{
        // TODO: add client_id authorization check : if ( chronicle_action_is_authorized())

    chronicleMetaDirectory.show_chronicles(chronicles);

    return CL_SUCCESS;
}

int chronolog::VisorClientPortal::ShowStories(chl::ClientId const& client_id, std::string const& chronicle_name, std::vector<std::string>& stories) 
{
        LOGD("%s is called in PID=%d, chronicle_name=%s",
             __FUNCTION__, getpid(),  chronicle_name.c_str());

        // TODO: add client_id authorization check : if ( chronicle_action_is_authorized())

    chronicleMetaDirectory.show_stories(chronicle_name, stories);

    return CL_SUCCESS;
}


/*


/////////////////
int chronolog::VisorClientPortal::LocalDestroyStory(std::string const& chronicle_name, std::string const&story_name)
{    int return_code = CL_SUCCESS;

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
    int return_code = CL_SUCCESS;

    return return_code;
}


/////////////////
int chronolog::VisorClientPortal::LocalReleaseStory( chronolog::ClientId const&client_id
            , std::string const&chronicle_name, std::string const&story_name)
{
    int return_code = CL_SUCCESS;

    return return_code;
}


/////////////////
int chronolog::VisorClientPortal::LocalGetChronicleAttr( chronolog::ClientId const& client_id
            , std::string const&name, const std::string &key, std::string &value)
{
    int return_code = CL_SUCCESS;

    return return_code;
}

/////////////////
int chronolog::VisorClientPortal::LocalEditChronicleAttr( chronolog::ClientId const& client_id
            , std::string const&name, const std::string &key, const std::string &value)
{
    int return_code = CL_SUCCESS;

    return return_code;
}


*/
/////////////////

bool chronolog::VisorClientPortal::authenticate_client( std::string const& client_account, chl::ClientId&)
{  

    return true; 
}

///////////////

bool chronolog::VisorClientPortal::chronicle_action_is_authorized( chl::ClientId const& client_id, chl::ChronicleName const&)
{

    return true;
}
////////////////

bool chronolog::VisorClientPortal::story_action_is_authorized( chronolog::ClientId const& client_id
            , chl::ChronicleName const& chronicle, chl::StoryName const& story_name)
{

    return true;
}

