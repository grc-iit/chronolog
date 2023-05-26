//
// Created by kfeng on 12/26/22.
//

#ifndef CHRONOLOG_RPCVISOR_H
#define CHRONOLOG_RPCVISOR_H

#include <iostream>
#include <unordered_map>
#include <functional>
#include <thallium.hpp>
#include <sys/types.h>
#include <unistd.h>
#include "macro.h"
#include "errcode.h"
#include "RPCFactory.h"
#include "ClientRegistryManager.h"
#include "ChronicleMetaDirectory.h"
#include "KeeperRegistry.h"


class RPCVisor {
public:
    RPCVisor(chronolog::KeeperRegistry * keeper_registry)
   : keeperRegistry(keeper_registry)
    {
        LOGD("%s constructor is called", typeid(*this).name());
        clientManager = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
        chronicleMetaDirectory = ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance();
        clientManager->setChronicleMetaDirectory(chronicleMetaDirectory.get());
        chronicleMetaDirectory->set_client_registry_manager(clientManager.get());
        
	rpc = std::make_shared<ChronoLogRPC>();
        set_prefix("ChronoLog");
        LOGD("%s constructor finishes, object created@%p in thread PID=%d",
             typeid(*this).name(), this, getpid());
    }

    ~RPCVisor()
    {
    }

    void Visor_start( ) { //chronolog::KeeperRegistry * keeper_registry) {
        rpc->start();
    }
    /**
     * Admin APIs
     */
    int LocalConnect(const std::string &uri, std::string const& client_id, int &flags, uint64_t &clock_offset) {
        LOGD("%s in ChronoLogAdminRPCProxy@%p called in PID=%d, with args: uri=%s",
             __FUNCTION__, this, getpid(), uri.c_str());
        ClientInfo record;
        record.addr_ = "127.0.0.1";
        if (std::strtol(client_id.c_str(), nullptr, 10) < 0) {
            LOGE("client_id=%s is invalid", client_id.c_str());
            return CL_ERR_INVALID_ARG;
        }
        return clientManager->add_client_record(client_id,record);
    }

    int LocalDisconnect(std::string const& client_id, int &flags) {
        LOGD("%s is called in PID=%d, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        if (std::strtol(client_id.c_str(), nullptr, 10) < 0) {
            LOGE("client_id=%s is invalid", client_id.c_str());
            return CL_ERR_INVALID_ARG;
        }
        return clientManager->remove_client_record(client_id,flags);
    }

    /**
     * Metadata APIs
     */
    int LocalCreateChronicle(std::string const& name,
                             const std::unordered_map<std::string, std::string> &attrs,
                             int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, attrs=", __FUNCTION__, getpid(), name.c_str());
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }

        if (!name.empty()) {
            return chronicleMetaDirectory->create_chronicle(name);
        } else {
            LOGE("name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalDestroyChronicle(std::string const& name) {
        LOGD("%s is called in PID=%d, with args: name=%s", __FUNCTION__, getpid(), name.c_str());
        if (!name.empty()) {
            return chronicleMetaDirectory->destroy_chronicle(name);
        } else {
            LOGE("name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalDestroyStory(std::string const& chronicle_name, std::string const& story_name) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());
        if (!chronicle_name.empty() && !story_name.empty()) {
            return chronicleMetaDirectory->destroy_story(chronicle_name,story_name);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

///////////////////

    int LocalAcquireStory(std::string const& client_id,
                          std::string const& chronicle_name,
                          std::string const& story_name,
                          const std::unordered_map<std::string, std::string> &attrs,
                          int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);

	if( !keeperRegistry->is_running())
	{ return CL_ERR_NO_KEEPERS; }

        /*for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }*/

        if (chronicle_name.empty() || story_name.empty()) 
	{ //TODO : add this check on the client side, 
	  //there's no need to waste the RPC on empty strings...
	  return CL_ERR_INVALID_ARG;
	}
	// TODO : create_stroy should be part of acquire_story 
            int ret = chronicleMetaDirectory->create_story(chronicle_name, story_name, attrs);
            if (ret != CL_SUCCESS) {
                return ret;
            }

	// TODO : StoryId token and recordingKeepers vector need to be returned to the client 
	// when the client side RPC is updated to receive them
	StoryId story_id(0);
	std::vector<chronolog::KeeperIdCard> recording_keepers;
        bool notify_keepers = false;
        ret = chronicleMetaDirectory->acquire_story(client_id, chronicle_name, story_name, flags, story_id,notify_keepers);
	if(ret != CL_SUCCESS)
	{ return ret; }

	// if this is the first client to acquire this story we need to notify the recording Keepers 
	// so that they are ready to start recording this story
	if(notify_keepers)
	{
	    recording_keepers = keeperRegistry->getActiveKeepers(recording_keepers);
	    if( 0 != keeperRegistry->notifyKeepersOfStoryRecordingStart(recording_keepers, chronicle_name, story_name,story_id))
	    {  // RPC notification to the keepers might have failed, release the newly acquired story 
	       chronicleMetaDirectory->release_story(client_id, chronicle_name,story_name,story_id, notify_keepers);
	       //TODO: chronicleMetaDirectory->release_story(client_id, story_id, notify_keepers); 
	       //we do know that there's no need notify keepers of the story ending in this case as it hasn't started...
	       return CL_ERR_NO_KEEPERS;
	    }
	    
	}

        LOGD("%s finished  in PID=%d, with args: chronicle_name=%s, story_name=%s",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());
    return CL_SUCCESS; 
    }
//TODO: check if flags are ever needed to release the story...

    int LocalReleaseStory(std::string const& client_id, std::string const& chronicle_name, std::string const& story_name) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());

	//TODO: add this check on the client side so we dont' waste RPC call on empty strings...
        if (chronicle_name.empty() || story_name.empty()) 
	{ return CL_ERR_INVALID_ARG; }

	StoryId story_id(0);
	bool notify_keepers = false;
        auto return_code = chronicleMetaDirectory->release_story(client_id, chronicle_name, story_name, story_id, notify_keepers);
	if(CL_SUCCESS != return_code)
	{  return return_code; }

	if( notify_keepers && keeperRegistry->is_running() )
	{  
	  std::vector<chronolog::KeeperIdCard> recording_keepers;
	  keeperRegistry->notifyKeepersOfStoryRecordingStop( keeperRegistry->getActiveKeepers(recording_keepers), story_id);
	}
        LOGD("%s finished in PID=%d, with args: chronicle_name=%s, story_name=%s", 
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str() );
    return CL_SUCCESS;
    }
//////////////

    int LocalGetChronicleAttr(std::string const& name, const std::string &key, std::string &value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s", __FUNCTION__, getpid(), name.c_str(), key.c_str());
        if (!name.empty() && !key.empty()) {
            chronicleMetaDirectory->get_chronicle_attr(name, key, value);
            return CL_SUCCESS;
        } else {
            if (name.empty())
                LOGE("name is empty");
            if (key.empty())
                LOGE("key is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalEditChronicleAttr(std::string const& name, const std::string &key, const std::string &value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s, value=%s",
             __FUNCTION__, getpid(), name.c_str(), key.c_str(), value.c_str());
        if (!name.empty() && !key.empty() && !value.empty()) {
            return chronicleMetaDirectory->edit_chronicle_attr(name, key, value);
        } else {
            if (name.empty())
                LOGE("name is empty");
            if (key.empty())
                LOGE("key is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    std::vector<std::string> LocalShowChronicles(std::string &client_id) {
        LOGD("%s is called in PID=%d, with args: client_id=%s", __FUNCTION__, getpid(), client_id.c_str());
        return chronicleMetaDirectory->show_chronicles(client_id);
    }

    std::vector<std::string> LocalShowStories(std::string &client_id, const std::string &chronicle_name) {
        LOGD("%s is called in PID=%d, with args: client_id=%s, chronicle_name=%s",
             __FUNCTION__, getpid(), client_id.c_str(), chronicle_name.c_str());
        return chronicleMetaDirectory->show_stories(client_id, chronicle_name);
    }

    void bind_functions() {
        switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE: {
                std::function<void(const tl::request &,
                                   const std::string &,
                                   std::string const&,
                                   int &,
                                   uint64_t &)> connectFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4,
                               auto && PH5) {
                            ThalliumLocalConnect(std::forward<decltype(PH1)>(PH1),
                                                 std::forward<decltype(PH2)>(PH2),
                                                 std::forward<decltype(PH3)>(PH3),
                                                 std::forward<decltype(PH4)>(PH4),
                                                 std::forward<decltype(PH5)>(PH5));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string const&,
                                   int &)> disconnectFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3) {
                            ThalliumLocalDisconnect(std::forward<decltype(PH1)>(PH1),
                                                    std::forward<decltype(PH2)>(PH2),
                                                    std::forward<decltype(PH3)>(PH3));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string const&,
                                   const std::unordered_map<std::string, std::string> &,
                                   int &)> createChronicleFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4) {
                            ThalliumLocalCreateChronicle(std::forward<decltype(PH1)>(PH1), \
                                                         std::forward<decltype(PH2)>(PH2),
                                                         std::forward<decltype(PH3)>(PH3),
                                                         std::forward<decltype(PH4)>(PH4));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string const&
                                   )> destroyChronicleFunc(
                        [this](auto && PH1,
                               auto && PH2
                               ) {
                            ThalliumLocalDestroyChronicle(std::forward<decltype(PH1)>(PH1),
                                                          std::forward<decltype(PH2)>(PH2)
                                                          );
                        }
                );

                std::function<void(const tl::request &,
                                   std::string const&,
                                   std::string const&
                                   )> destroyStoryFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3
                               ) {
                            ThalliumLocalDestroyStory(std::forward<decltype(PH1)>(PH1),
                                                      std::forward<decltype(PH2)>(PH2),
                                                      std::forward<decltype(PH3)>(PH3)
                                                      );
                        }
                );
                std::function<void(const tl::request &,
				                   std::string const&,
                                   std::string const&,
                                   std::string const&,
                                   const std::unordered_map<std::string, std::string> &,
                                   int &)> acquireStoryFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4,
                               auto && PH5,
                               auto && PH6) {
                            ThalliumLocalAcquireStory(std::forward<decltype(PH1)>(PH1),
                                                      std::forward<decltype(PH2)>(PH2),
                                                      std::forward<decltype(PH3)>(PH3),
                                                      std::forward<decltype(PH4)>(PH4),
                                                      std::forward<decltype(PH5)>(PH5),
                                                      std::forward<decltype(PH6)>(PH6));
                        }
                );
                std::function<void(const tl::request &,
				                   std::string const&,
                                   std::string const&,
                                   std::string const&
                                   )> releaseStoryFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4
                               ) {
                            ThalliumLocalReleaseStory(std::forward<decltype(PH1)>(PH1),
                                                      std::forward<decltype(PH2)>(PH2),
                                                      std::forward<decltype(PH3)>(PH3),
                                                      std::forward<decltype(PH4)>(PH4)
                                                      );
                        }
                );

                std::function<void(const tl::request &,
                                   std::string const&name,
                                   const std::string &,
                                   std::string &)> getChronicleAttrFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4) {
                            ThalliumLocalGetChronicleAttr(std::forward<decltype(PH1)>(PH1),
                                                          std::forward<decltype(PH2)>(PH2),
                                                          std::forward<decltype(PH3)>(PH3),
                                                          std::forward<decltype(PH4)>(PH4));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string const&name,
                                   const std::string &,
                                   const std::string &)> editChronicleAttrFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4) {
                            ThalliumLocalEditChronicleAttr(std::forward<decltype(PH1)>(PH1),
                                                           std::forward<decltype(PH2)>(PH2),
                                                           std::forward<decltype(PH3)>(PH3),
                                                           std::forward<decltype(PH4)>(PH4));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string &client_id)> showChroniclesFunc(
                        [this](auto && PH1,
                               auto && PH2) {
                            ThalliumLocalShowChronicles(std::forward<decltype(PH1)>(PH1),
                                                        std::forward<decltype(PH2)>(PH2));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string &client_id,
                                   const std::string &chroicle_name)> showStoriesFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3) {
                            ThalliumLocalShowStories(std::forward<decltype(PH1)>(PH1),
                                                     std::forward<decltype(PH2)>(PH2),
                                                     std::forward<decltype(PH3)>(PH3));
                        }
                );

                rpc->bind("ChronoLogThalliumConnect", connectFunc);
                rpc->bind("ChronoLogThalliumDisconnect", disconnectFunc);

                rpc->bind("ChronoLogThalliumCreateChronicle", createChronicleFunc);
                rpc->bind("ChronoLogThalliumDestroyChronicle", destroyChronicleFunc);

                rpc->bind("ChronoLogThalliumDestroyStory", destroyStoryFunc);
                rpc->bind("ChronoLogThalliumAcquireStory", acquireStoryFunc);
                rpc->bind("ChronoLogThalliumReleaseStory", releaseStoryFunc);

                rpc->bind("ChronoLogThalliumGetChronicleAttr", getChronicleAttrFunc);
                rpc->bind("ChronoLogThalliumEditChronicleAttr", editChronicleAttrFunc);

                rpc->bind("ChronoLogThalliumShowChronicles", showChroniclesFunc);
                rpc->bind("ChronoLogThalliumShowStories", showStoriesFunc);
            }
        }
    }

    CHRONOLOG_THALLIUM_DEFINE(LocalConnect, (uri, client_id, flags, clock_offset),
                              const std::string &uri, std::string const &client_id, int &flags, uint64_t &clock_offset)

    CHRONOLOG_THALLIUM_DEFINE(LocalDisconnect, (client_id, flags), std::string const& client_id, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalCreateChronicle, (name, attrs, flags),
                              std::string const&name, const std::unordered_map<std::string, std::string> &attrs, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyChronicle, (name), std::string const&name)

    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyStory, (chronicle_name, story_name),
                              std::string const& chronicle_name, std::string const&story_name)

    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireStory, (client_id, chronicle_name, story_name, attrs, flags),
                              std::string const&client_id,
                              std::string const&chronicle_name,
                              std::string const&story_name,
                              const std::unordered_map<std::string, std::string> &attrs,
                              int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseStory, (client_id, chronicle_name, story_name),
                              std::string const&client_id, std::string const&chronicle_name, std::string const&story_name)

    CHRONOLOG_THALLIUM_DEFINE(LocalGetChronicleAttr, (name, key, value),
                              std::string const&name, const std::string &key, std::string &value)

    CHRONOLOG_THALLIUM_DEFINE(LocalEditChronicleAttr, (name, key, value),
                              std::string const&name, const std::string &key, const std::string &value)

    CHRONOLOG_THALLIUM_DEFINE(LocalShowChronicles, (client_id), std::string &client_id)

    CHRONOLOG_THALLIUM_DEFINE(LocalShowStories, (client_id, chronicle_name),
                              std::string &client_id, const std::string &chronicle_name)

private:
    void set_prefix(std::string prefix) {
        func_prefix = prefix;
        switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
    }

    ChronoLogCharStruct func_prefix;
    std::shared_ptr<ChronoLogRPC> rpc;
    chronolog::KeeperRegistry * keeperRegistry;
    std::shared_ptr<ClientRegistryManager> clientManager;
    std::shared_ptr<ChronicleMetaDirectory> chronicleMetaDirectory;
};


#endif //CHRONOLOG_RPCVISOR_H
