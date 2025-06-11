#ifndef RPC_VISOR_PORTAL_CLIENT_H
#define RPC_VISOR_PORTAL_CLIENT_H

#include <string>
#include <map>
#include <iostream>
#include <sys/types.h>
#include <unistd.h>
#include <thallium.hpp>
#include <sys/types.h>
#include <unistd.h>
#include <thallium/serialization/serialize.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/map.hpp>

#include "chrono_monitor.h"
#include "chronolog_types.h"
#include "ConnectResponseMsg.h"
#include "AcquireStoryResponseMsg.h"

namespace tl = thallium;


namespace chronolog
{


class RpcVisorClient
{

public:
    static RpcVisorClient*
    CreateRpcVisorClient(tl::engine &tl_engine, std::string const &service_addr, uint16_t provider_id)
    {
        try
        {
            return new RpcVisorClient(tl_engine, service_addr, provider_id);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RpcVisorClient] Failed to create client instance due to a Thallium exception.");
        }
        return nullptr;
    }


    ConnectResponseMsg Connect(uint32_t client_euid, uint32_t client_host_ip, uint32_t client_pid)
    {
        LOG_DEBUG("[RpcVisorClient] Initiating connection for Account={}, HostID={}, PID={}", client_euid, client_host_ip
             , client_pid);
        try
        {
            ConnectResponseMsg response = visor_connect.on(service_ph)(client_euid, client_host_ip, client_pid);
            LOG_INFO("[RpcVisorClient] Connection successful for Account={}, HostID={}, PID={}", client_euid, client_host_ip
                 , client_pid);
            return response;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RpcVisorClient] Connection attempt failed.");
        }
        return (ConnectResponseMsg(chronolog::CL_ERR_UNKNOWN, ClientId{0}));
    }

    int Disconnect(ClientId const &client_id)
    {
        LOG_INFO("[RPCVisorClient] Initiating disconnection for ClientID={}", client_id);
        try
        {
            int result = visor_disconnect.on(service_ph)(client_id);
            if(result == chronolog::CL_SUCCESS)
            {
                LOG_INFO("[RPCVisorClient] Disconnection successful for ClientID={}", client_id);
            }
            else
            {
                LOG_ERROR("[RPCVisorClient] Failed to disconnect ClientID={}. Unexpected return code: {}", client_id
                     , chronolog::to_string_client(result));
            }
            return result;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to disconnect. Thallium exception encountered.");
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    int CreateChronicle(ClientId const &client_id, std::string const &name
                        , const std::map <std::string, std::string> &attrs, int &flags)
    {
        LOG_INFO("[RPCVisorClient] Initiating creation of chronicle: Name={}, Flags={}", name.c_str()
             , flags);
        try
        {
            int result = create_chronicle.on(service_ph)(client_id, name, attrs, flags);

            if(result == chronolog::CL_SUCCESS)
            {
                LOG_INFO("[RPCVisorClient] Successfully created chronicle with Name={}, Flags={}", name.c_str(), flags);
            }
            else
            {
                LOG_ERROR("[RPCVisorClient] Failed to create chronicle with Name={}, Flags={}. Unexpected return code: {}"
                     , name.c_str(), flags, chronolog::to_string_client(result));
            }
            return result;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to create chronicle {}. Thallium exception encountered.", name.c_str());
        }
        return (chronolog::CL_ERR_UNKNOWN);

    }

    int DestroyChronicle(ClientId const &client_id, std::string const &name)
    {
        LOG_INFO("[RPCVisorClient] Initiating destruction of chronicle: Name={}", name.c_str());
        try
        {
            int result = destroy_chronicle.on(service_ph)(client_id, name);

            if(result == chronolog::CL_SUCCESS)
            {
                LOG_INFO("[RPCVisorClient] Successfully destroyed chronicle: Name={}", name.c_str());
            }
            else
            {
                LOG_ERROR("[RPCVisorClient] Failed to destroy chronicle: Name={}, Error Code={}", name.c_str(), chronolog::to_string_client(result));
            }
            return result;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to destroy chronicle {}. Thallium exception encountered.", name.c_str());
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    chronolog::AcquireStoryResponseMsg
    AcquireStory(ClientId const &client_id, std::string const &chronicle_name, std::string const &story_name
                 , const std::map <std::string, std::string> &attrs, const int &flags)
    {
        LOG_INFO("[RPCVisorClient] Initiating story acquisition: ChronicleName={}, StoryName={}", chronicle_name.c_str()
             , story_name.c_str());
        try
        {
            chronolog::AcquireStoryResponseMsg response = acquire_story.on(service_ph)(client_id, chronicle_name
                                                                                       , story_name, attrs, flags);

            if(response.getErrorCode() == chronolog::CL_SUCCESS)
            {
                LOG_INFO("[RPCVisorClient] Successfully acquired story: ChronicleName={}, StoryName={}"
                     , chronicle_name.c_str(), story_name.c_str());
            }
            else
            {
                LOG_ERROR("[RPCVisorClient] Failed to acquire story: ChronicleName={}, StoryName={}, Error Code={}"
                     , chronicle_name.c_str(), story_name.c_str(), chronolog::to_string_client(response.getErrorCode()));
            }

            return response;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to acquire story {} from chronicle {}. Thallium exception encountered."
                 , story_name.c_str(), chronicle_name.c_str());
        }
        return (AcquireStoryResponseMsg(chronolog::CL_ERR_UNKNOWN, 0, std::vector <KeeperIdCard>{}));
    }

    int ReleaseStory(ClientId const &client_id, std::string const &chronicle_name, std::string const &story_name)
    {
        LOG_INFO("[RPCVisorClient] Initiating story release: ChronicleName={}, StoryName={}", chronicle_name.c_str()
             , story_name.c_str());
        try
        {
            int resultCode = release_story.on(service_ph)(client_id, chronicle_name, story_name);

            if(resultCode == chronolog::CL_SUCCESS)
            {
                LOG_INFO("[RPCVisorClient] Successfully released story: ChronicleName={}, StoryName={}"
                     , chronicle_name.c_str(), story_name.c_str());
            }
            else
            {
                LOG_ERROR("[RPCVisorClient] Failed to release story: ChronicleName={}, StoryName={}, Error Code={}"
                     , chronicle_name.c_str(), story_name.c_str(), chronolog::to_string_client(resultCode));
            }

            return resultCode;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to release story {} from chronicle {}. Thallium exception encountered."
                 , story_name.c_str(), chronicle_name.c_str());
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    int DestroyStory(ClientId const &client_id, std::string const &chronicle_name, std::string const &story_name)
    {
        LOG_INFO("[RPCVisorClient] Initiating story destruction: ChronicleName={}, StoryName={}", chronicle_name.c_str()
             , story_name.c_str());
        try
        {
            int resultCode = destroy_story.on(service_ph)(client_id, chronicle_name, story_name);

            if(resultCode == chronolog::CL_SUCCESS)
            {
                LOG_INFO("[RPCVisorClient] Successfully destroyed story: ChronicleName={}, StoryName={}"
                     , chronicle_name.c_str(), story_name.c_str());
            }
            else
            {
                LOG_ERROR("[RPCVisorClient] Failed to destroy story: ChronicleName={}, StoryName={}, Error Code={}"
                     , chronicle_name.c_str(), story_name.c_str(), chronolog::to_string_client(resultCode));
            }

            return resultCode;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to destroy story {} from chronicle {}. Thallium exception encountered."
                 , story_name.c_str(), chronicle_name.c_str());
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }


    int GetChronicleAttr(ClientId const &client_id, std::string const &name, const std::string &key, std::string &value)
    {
        LOG_INFO("[RPCVisorClient] Retrieving attribute: ChronicleName={}, Key={}", name.c_str(), key.c_str());
        try
        {
            int resultCode = get_chronicle_attr.on(service_ph)(client_id, name, key, value);

            if(resultCode == chronolog::CL_SUCCESS)
            {
                LOG_INFO("[RPCVisorClient] Successfully retrieved attribute: ChronicleName={}, Key={}, Value={}"
                     , name.c_str(), key.c_str(), value.c_str());
            }
            else
            {
                LOG_ERROR("[RPCVisorClient] Failed to retrieve attribute: ChronicleName={}, Key={}, Error Code={}"
                     , name.c_str(), key.c_str(), chronolog::to_string_client(resultCode));
            }

            return resultCode;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to retrieve attribute {} from chronicle {}. Thallium exception encountered."
                 , key.c_str(), name.c_str());
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    int EditChronicleAttr(ClientId const &client_id, std::string const &name, const std::string &key
                          , const std::string &value)
    {
        LOG_INFO("[RPCVisorClient] Modifying attribute: ChronicleName={}, Key={}, NewValue={}", name.c_str(), key.c_str()
             , value.c_str());
        try
        {
            int resultCode = edit_chronicle_attr.on(service_ph)(client_id, name, key, value);

            if(resultCode == chronolog::CL_SUCCESS)
            {
                LOG_INFO("[RPCVisorClient] Successfully modified attribute: ChronicleName={}, Key={}, NewValue={}"
                     , name.c_str(), key.c_str(), value.c_str());
            }
            else
            {
                LOG_ERROR("[RPCVisorClient] Failed to modify attribute: ChronicleName={}, Key={}, NewValue={}, Error Code={}"
                     , name.c_str(), key.c_str(), value.c_str(), chronolog::to_string_client(resultCode));
            }

            return resultCode;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to modify attribute {} of chronicle {}. Thallium exception encountered."
                 , key.c_str(), name.c_str());
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    std::vector <std::string> ShowChronicles(ClientId const &client_id) //, std::vector<std::string> & chronicles)
    {
        LOG_INFO("[RPCVisorClient] Attempting to retrieve list of chronicles for ClientID={}", client_id);
        try
        {
            std::vector <std::string> chronicleList = show_chronicles.on(service_ph)(client_id);

            if(!chronicleList.empty())
            {
                LOG_INFO("[RPCVisorClient] Successfully fetched {} chronicles for ClientID={}", chronicleList.size()
                     , client_id);
            }
            else
            {
                LOG_WARNING("[RPCVisorClient] Retrieved an empty list of chronicles for ClientID={}", client_id);
            }
            return chronicleList;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to fetch chronicles for ClientID {}. Thallium exception encountered."
                 , client_id);
        }
        return (std::vector <std::string>{});
    }

    std::vector <std::string>
    ShowStories(ClientId const &client_id, std::string const &chronicle_name) //, std::vector<std::string> & stories )
    {
        LOG_INFO("[RPCVisorClient] Attempting to retrieve stories for ClientID={}, ChronicleName={}", client_id
             , chronicle_name);
        try
        {
            std::vector <std::string> storyList = show_stories.on(service_ph)(client_id, chronicle_name);

            if(!storyList.empty())
            {
                LOG_INFO("[RPCVisorClient] Successfully fetched {} stories for ClientID={} from ChronicleName={}"
                     , storyList.size(), client_id, chronicle_name);
            }
            else
            {
                LOG_WARNING("[RPCVisorClient] Retrieved an empty list of stories for ClientID={} from ChronicleName={}"
                     , client_id, chronicle_name);
            }

            return storyList;
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[RPCVisorClient] Failed to fetch stories for ClientID {} from ChronicleName {}. Thallium exception encountered."
                 , client_id, chronicle_name);
        }
        return (std::vector <std::string>{});
    }

    ~RpcVisorClient()
    {
        visor_connect.deregister();
        visor_disconnect.deregister();
        create_chronicle.deregister();
        destroy_chronicle.deregister();
        get_chronicle_attr.deregister();
        edit_chronicle_attr.deregister();
        acquire_story.deregister();
        release_story.deregister();
        destroy_story.deregister();
        show_chronicles.deregister();
        show_stories.deregister();
    }

private:
    std::string service_addr;     // na address of ChronoVisor ClientService  
    uint16_t service_provider_id;          // ChronoVisor ClientService provider_id id
    tl::provider_handle service_ph;  //provider_handle for client registry service
    tl::remote_procedure visor_connect;
    tl::remote_procedure visor_disconnect;
    tl::remote_procedure create_chronicle;
    tl::remote_procedure destroy_chronicle;
    tl::remote_procedure get_chronicle_attr;
    tl::remote_procedure edit_chronicle_attr;
    tl::remote_procedure acquire_story;
    tl::remote_procedure release_story;
    tl::remote_procedure destroy_story;
    tl::remote_procedure show_chronicles;
    tl::remote_procedure show_stories;

    RpcVisorClient(RpcVisorClient const &) = delete;

    RpcVisorClient &operator=(RpcVisorClient const &) = delete;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    RpcVisorClient(tl::engine &tl_engine, std::string const &service_addr, uint16_t provider_id): service_addr(
            service_addr), service_provider_id(provider_id), service_ph(tl_engine.lookup(service_addr), provider_id)
    {
        LOG_DEBUG("[RpcVisorClient] Initialized for Visor Service at {} with ProviderID={}", service_addr
             , service_provider_id);
        visor_connect = tl_engine.define("Connect");
        visor_disconnect = tl_engine.define("Disconnect");
        create_chronicle = tl_engine.define("CreateChronicle");
        destroy_chronicle = tl_engine.define("DestroyChronicle");
        get_chronicle_attr = tl_engine.define("GetChronicleAttr");
        edit_chronicle_attr = tl_engine.define("EditChronicleAttr");
        acquire_story = tl_engine.define("AcquireStory");
        release_story = tl_engine.define("ReleaseStory");
        destroy_story = tl_engine.define("DestroyStory");
        show_chronicles = tl_engine.define("ShowChronicles");
        show_stories = tl_engine.define("ShowStories");
    }
};

}

#endif
