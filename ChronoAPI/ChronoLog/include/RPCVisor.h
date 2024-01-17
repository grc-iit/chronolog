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
#include "chronolog_errcode.h"
#include "RPCFactory.h"
#include "ClientRegistryManager.h"
#include "ChronicleMetaDirectory.h"
#include "KeeperRegistry.h"
#include "AcquireStoryResponseMsg.h"

class RPCVisor
{
public:
    RPCVisor(chronolog::KeeperRegistry*keeper_registry): keeperRegistry(keeper_registry)
    {
        LOGD("[RPCVisor] Constructor is called");
        clientManager = ChronoLog::Singleton <ClientRegistryManager>::GetInstance();
        chronicleMetaDirectory = ChronoLog::Singleton <ChronicleMetaDirectory>::GetInstance();
        clientManager->setChronicleMetaDirectory(chronicleMetaDirectory.get());
        chronicleMetaDirectory->set_client_registry_manager(clientManager.get());

        rpc = std::make_shared <ChronoLogRPC>();
        set_prefix("ChronoLog");
        LOGD("[RPCVisor] Constructor completed. Created at {} in thread PID={}", static_cast<void*>(this), getpid());
    }

    ~RPCVisor()
    {
    }

    void Visor_start()
    { //chronolog::KeeperRegistry * keeper_registry) {
        LOGI("[RPCVisor] Start visor is called.");
        rpc->start();
    }

    /**
     * Admin APIs
     */
    int LocalConnect(const std::string &uri, std::string const &client_id, int &flags, uint64_t &clock_offset)
    {
        LOGD("[RPCVisor] LocalConnect initiated. URI: {}, PID: {}", uri, getpid());

        ClientInfo record;
        record.addr_ = "127.0.0.1";

        long clientIdValue = std::strtol(client_id.c_str(), nullptr, 10);
        if(clientIdValue < 0)
        {
            LOGE("[RPCVisor] LocalConnect failed: Invalid client_id detected. ClientID={}", client_id);
            return chronolog::CL_ERR_INVALID_ARG;
        }
        else
        {
            LOGD("[RPCVisor] Valid client_id detected. ClientID={}", client_id);
        }
        int result = clientManager->add_client_record(client_id, record);
        if(result != chronolog::CL_SUCCESS)
        {
            LOGE("[RPCVisor] Failed to add client record. ClientID={}, ErrorCode={}", client_id, result);
        }
        else
        {
            LOGD("[RPCVisor] Successfully added client record. ClientID={}", client_id);
        }
    }

    int LocalDisconnect(std::string const &client_id, int &flags)
    {
        LOGD("[RPCVisor] Initiating Local Disconnect. Process ID: {}, Client ID: {}, Flags: {}", getpid(), client_id
             , flags);

        long clientIdValue = std::strtol(client_id.c_str(), nullptr, 10);
        if(clientIdValue < 0)
        {
            LOGE("[RPCVisor] Failed: Detected an invalid Client ID during disconnection. Client ID: {}", client_id);
            return chronolog::CL_ERR_INVALID_ARG;
        }
        else
        {
            LOGD("[RPCVisor] Valid Client ID confirmed for disconnection process. Client ID: {}", client_id);
        }

        int result = clientManager->remove_client_record(client_id, flags);
        if(result != chronolog::CL_SUCCESS)
        {
            LOGE("[RPCVisor] Disconnection failed: Unable to remove client record. Client ID: {}, Error Code: {}"
                 , client_id, result);
        }
        else
        {
            LOGD("[RPCVisor] Disconnection succeeded: Client record successfully removed. Client ID: {}", client_id);
        }
        return result;
    }


    /**
     * Metadata APIs
     */
    int LocalCreateChronicle(std::string const &name, const std::unordered_map <std::string, std::string> &attrs
                             , int &flags)
    {
        LOGD("[RPCVisor] Initiating Local Chronicle Creation. Process ID: {}, Chronicle Name: {}", getpid(), name);

        // Log the attributes associated with the Chronicle creation
        LOGD("[RPCVisor] Attributes for Chronicle '{}':", name);
        for(const auto &attr: attrs)
        {
            LOGD("[RPCVisor] - {} : {}", attr.first, attr.second);
        }

        if(name.empty())
        {
            LOGE("[RPCVisor] Chronicle creation failed: Provided name is empty.");
            return chronolog::CL_ERR_INVALID_ARG;
        }

        int result = chronicleMetaDirectory->create_chronicle(name);
        if(result != chronolog::CL_SUCCESS)
        {
            LOGE("[RPCVisor] Chronicle creation failed: Error encountered while creating Chronicle '{}'. Error Code: {}"
                 , name, result);
        }
        else
        {
            LOGD("[RPCVisor] Chronicle creation succeeded: Chronicle '{}' successfully created.", name);
        }

        return result;
    }

    int LocalDestroyChronicle(std::string const &name)
    {
        LOGD("[RPCVisor] Initiating Local Chronicle Destruction. Process ID: {}, Chronicle Name: {}", getpid(), name);

        if(name.empty())
        {
            LOGE("[RPCVisor] Chronicle destruction failed: Provided name is empty.");
            return chronolog::CL_ERR_INVALID_ARG;
        }

        int result = chronicleMetaDirectory->destroy_chronicle(name);
        if(result != chronolog::CL_SUCCESS)
        {
            LOGE("[RPCVisor] Chronicle destruction failed: Error encountered while destroying Chronicle '{}'. Error Code: {}"
                 , name, result);
        }
        else
        {
            LOGD("[RPCVisor] Chronicle destruction succeeded: Chronicle '{}' successfully destroyed.", name);
        }

        return result;
    }

    int LocalDestroyStory(std::string const &chronicle_name, std::string const &story_name)
    {
        LOGD("[RPCVisor] Initiating Local Story Destruction. Process ID: {}, Chronicle Name: {}, Story Name: {}"
             , getpid(), chronicle_name, story_name);

        if(chronicle_name.empty() && story_name.empty())
        {
            LOGE("[RPCVisor] Story destruction failed: Both Chronicle name and Story name are empty.");
            return chronolog::CL_ERR_INVALID_ARG;
        }
        else if(chronicle_name.empty())
        {
            LOGE("[RPCVisor] Story destruction failed: Chronicle name is empty for Story '{}'.", story_name);
            return chronolog::CL_ERR_INVALID_ARG;
        }
        else if(story_name.empty())
        {
            LOGE("[RPCVisor] Story destruction failed: Story name is empty for Chronicle '{}'.", chronicle_name);
            return chronolog::CL_ERR_INVALID_ARG;
        }

        int result = chronicleMetaDirectory->destroy_story(chronicle_name, story_name);
        if(result != chronolog::CL_SUCCESS)
        {
            LOGE("[RPCVisor] Story destruction failed: Error encountered while destroying Story '{}' in Chronicle '{}'. Error Code: {}"
                 , story_name, chronicle_name, result);
        }
        else
        {
            LOGD("[RPCVisor] Story destruction succeeded: Story '{}' in Chronicle '{}' successfully destroyed."
                 , story_name, chronicle_name);
        }

        return result;
    }


///////////////////

    chronolog::AcquireStoryResponseMsg
    LocalAcquireStory(std::string const &client_id, std::string const &chronicle_name, std::string const &story_name
                      , const std::unordered_map <std::string, std::string> &attrs, int &flags)
    {
        LOGD("[RPCVisor] Acquiring Story. PID: {}, Chronicle Name: {}, Story Name: {}, Flags: {}", getpid()
             , chronicle_name, story_name, flags);

        chronolog::StoryId story_id{0};
        std::vector <chronolog::KeeperIdCard> recording_keepers;

        if(!keeperRegistry->is_running())
        {
            //{ return CL_ERR_NO_KEEPERS; }
            LOGE("[RPCVisor] Keeper registry is not running. Cannot acquire story.");
            return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_NO_KEEPERS, story_id, recording_keepers);
        }

        if(chronicle_name.empty() || story_name.empty())
        {
            //{ //TODO : add this check on the client side,
            //there's no need to waste the RPC on empty strings...
            //return CL_ERR_INVALID_ARG;
            //}
            LOGE("[RPCVisor] Invalid arguments: Chronicle name or Story name is empty.");
            return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_INVALID_ARG, story_id, recording_keepers);
        }

        // TODO : create_stroy should be part of acquire_story
        int ret = chronicleMetaDirectory->create_story(chronicle_name, story_name, attrs);
        if(ret != chronolog::CL_SUCCESS)
        {
            //{ return ret; }
            LOGE("[RPCVisor] Failed to create story: {}. Error Code: {}", story_name, ret);
            return chronolog::AcquireStoryResponseMsg(ret, story_id, recording_keepers);
        }

        // TODO : StoryId token and recordingKeepers vector need to be returned to the client
        // when the client side RPC is updated to receive them
        bool notify_keepers = false;
        ret = chronicleMetaDirectory->acquire_story(client_id, chronicle_name, story_name, flags, story_id
                                                    , notify_keepers);
        if(ret != chronolog::CL_SUCCESS)
        {
            //{ return ret; }
            LOGE("[RPCVisor] Failed to acquire story: {}. Error Code: {}", story_name, ret);
            return chronolog::AcquireStoryResponseMsg(ret, story_id, recording_keepers);
        }

        recording_keepers = keeperRegistry->getActiveKeepers(recording_keepers);
        // if this is the first client to acquire this story we need to notify the recording Keepers
        // so that they are ready to start recording this story
        if(notify_keepers)
        {
            if(0 != keeperRegistry->notifyKeepersOfStoryRecordingStart(recording_keepers, chronicle_name, story_name
                                                                       , story_id))
            {
                LOGE("[RPCVisor] Failed to notify keepers of story recording start for story: {}", story_name);
                // RPC notification to the keepers might have failed, release the newly acquired story
                chronicleMetaDirectory->release_story(client_id, chronicle_name, story_name, story_id, notify_keepers);
                //TODO: chronicleMetaDirectory->release_story(client_id, story_id, notify_keepers);
                //we do know that there's no need notify keepers of the story ending in this case as it hasn't started...
                //return CL_ERR_NO_KEEPERS;
                return chronolog::AcquireStoryResponseMsg(chronolog::CL_ERR_NO_KEEPERS, story_id, recording_keepers);
            }

        }

        //chronolog::AcquireStoryResponseMsg (CL_SUCCESS, story_id, recording_keepers);

        LOGD("[RPCVisor] Successfully acquired story: {} in Chronicle: {}", story_name, chronicle_name);
        // return CL_SUCCESS;
        return chronolog::AcquireStoryResponseMsg(chronolog::CL_SUCCESS, story_id, recording_keepers);
    }
    //TODO: check if flags are ever needed to release the story...

    int
    LocalReleaseStory(std::string const &client_id, std::string const &chronicle_name, std::string const &story_name)
    {
        LOGD("[RPCVisor] Releasing Story. PID: {}, Chronicle Name: {}, Story Name: {}", getpid(), chronicle_name
             , story_name);

        //TODO: add this check on the client side so we dont' waste RPC call on empty strings...
        if(chronicle_name.empty() || story_name.empty())
        {
            LOGE("[RPCVisor] Invalid arguments: Chronicle name or Story name is empty.");
            return chronolog::CL_ERR_INVALID_ARG;
        }

        chronolog::StoryId story_id(0);
        bool notify_keepers = false;
        auto return_code = chronicleMetaDirectory->release_story(client_id, chronicle_name, story_name, story_id
                                                                 , notify_keepers);
        if(chronolog::CL_SUCCESS != return_code)
        {
            LOGE("[RPCVisor] Failed to release story: {}. Error Code: {}", story_name, return_code);
            return return_code;
        }

        if(notify_keepers && keeperRegistry->is_running())
        {
            std::vector <chronolog::KeeperIdCard> recording_keepers;
            keeperRegistry->notifyKeepersOfStoryRecordingStop(keeperRegistry->getActiveKeepers(recording_keepers)
                                                              , story_id);
            LOGD("[RPCVisor] Notified keepers of story recording stop for: {}", story_name);
        }

        LOGD("[RPCVisor] Successfully released story: {} from Chronicle: {}", story_name, chronicle_name);
        return chronolog::CL_SUCCESS;
    }
//////////////

    int LocalGetChronicleAttr(std::string const &name, const std::string &key, std::string &value)
    {
        LOGD("[RPCVisor] Retrieving Chronicle Attribute. PID: {}, Chronicle Name: {}, Key: {}", getpid(), name, key);
        if(name.empty() || key.empty())
        {
            if(name.empty())
            {
                LOGE("[RPCVisor] Empty Chronicle Name provided while fetching attribute.");
            }
            if(key.empty())
            {
                LOGE("[RPCVisor] Empty Key provided while fetching attribute for Chronicle: {}", name);
            }
            return chronolog::CL_ERR_INVALID_ARG;
        }
        chronicleMetaDirectory->get_chronicle_attr(name, key, value);

        LOGD("[RPCVisor] Successfully retrieved attribute for Chronicle: {}. Key: {}, Value: {}", name, key, value);
        return chronolog::CL_SUCCESS;
    }

    int LocalEditChronicleAttr(const std::string &name, const std::string &key, const std::string &value)
    {
        LOGD("[RPCVisor] Editing Chronicle Attribute. PID: {}, Chronicle Name: {}, Key: {}, New Value: {}", getpid()
             , name, key, value);

        if(name.empty() || key.empty() || value.empty())
        {
            if(name.empty())
            {
                LOGE("[RPCVisor] Empty Chronicle Name provided while editing attribute.");
            }
            if(key.empty())
            {
                LOGE("[RPCVisor] Empty Key provided while editing attribute for Chronicle: {}", name);
            }
            if(value.empty())
            {
                LOGE("[RPCVisor] Empty Value provided while editing attribute for Key: {} in Chronicle: {}", key, name);
            }
            return chronolog::CL_ERR_INVALID_ARG;
        }

        int result = chronicleMetaDirectory->edit_chronicle_attr(name, key, value);
        if(result != chronolog::CL_SUCCESS)
        {
            LOGE("[RPCVisor] Failed to edit attribute for Chronicle: {}. Key: {}. Error Code: {}", name, key, result);
        }
        else
        {
            LOGD("[RPCVisor] Successfully edited attribute for Chronicle: {}. Key: {}. New Value: {}", name, key
                 , value);
        }

        return result;
    }


    std::vector <std::string> LocalShowChronicles(const std::string &client_id)
    {
        LOGD("[RPCVisor] Displaying Chronicles for Client ID: {}. PID: {}", client_id, getpid());
        std::vector <std::string> chronicleList = chronicleMetaDirectory->show_chronicles(client_id);
        if(chronicleList.empty())
        {
            LOGW("[RPCVisor] No chronicles found for Client ID: {}", client_id);
        }
        else
        {
            LOGI("[RPCVisor] Retrieved {} chronicles for Client ID: {}", chronicleList.size(), client_id);
        }
        return chronicleList;
    }

    std::vector <std::string> LocalShowStories(const std::string &client_id, const std::string &chronicle_name)
    {
        LOGD("[RPCVisor] Retrieving Stories for Client ID: {} from Chronicle: {}. PID: {}", client_id, chronicle_name
             , getpid());
        std::vector <std::string> storyList = chronicleMetaDirectory->show_stories(client_id, chronicle_name);
        if(storyList.empty())
        {
            LOGW("[RPCVisor] No stories found for Client ID: {} in Chronicle: {}", client_id, chronicle_name);
        }
        else
        {
            LOGI("[RPCVisor] Retrieved {} stories for Client ID: {} from Chronicle: {}", storyList.size(), client_id
                 , chronicle_name);
        }
        return storyList;
    }

    void bind_functions()
    {
        switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
        {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
            {
                std::function <void(const tl::request &, const std::string &, std::string const &, int &
                                    , uint64_t &)> connectFunc(
                        [this](auto &&PH1, auto &&PH2, auto &&PH3, auto &&PH4, auto &&PH5)
                        {
                            ThalliumLocalConnect(std::forward <decltype(PH1)>(PH1), std::forward <decltype(PH2)>(PH2)
                                                 , std::forward <decltype(PH3)>(PH3), std::forward <decltype(PH4)>(PH4)
                                                 , std::forward <decltype(PH5)>(PH5));
                        });
                std::function <void(const tl::request &, std::string const &, int &)> disconnectFunc(
                        [this](auto &&PH1, auto &&PH2, auto &&PH3)
                        {
                            ThalliumLocalDisconnect(std::forward <decltype(PH1)>(PH1), std::forward <decltype(PH2)>(PH2)
                                                    , std::forward <decltype(PH3)>(PH3));
                        });
                std::function <void(const tl::request &, std::string const &
                                    , const std::unordered_map <std::string, std::string> &
                                    , int &)> createChronicleFunc([this](auto &&PH1, auto &&PH2, auto &&PH3, auto &&PH4)
                                                                  {
                                                                      ThalliumLocalCreateChronicle(
                                                                              std::forward <decltype(PH1)>(PH1), \
                                                         std::forward <decltype(PH2)>(PH2)
                                                                              , std::forward <decltype(PH3)>(PH3)
                                                                              , std::forward <decltype(PH4)>(PH4));
                                                                  });
                std::function <void(const tl::request &, std::string const &)> destroyChronicleFunc(
                        [this](auto &&PH1, auto &&PH2)
                        {
                            ThalliumLocalDestroyChronicle(std::forward <decltype(PH1)>(PH1)
                                                          , std::forward <decltype(PH2)>(PH2));
                        });

                std::function <void(const tl::request &, std::string const &, std::string const &)> destroyStoryFunc(
                        [this](auto &&PH1, auto &&PH2, auto &&PH3)
                        {
                            ThalliumLocalDestroyStory(std::forward <decltype(PH1)>(PH1), std::forward <decltype(PH2)>(
                                    PH2), std::forward <decltype(PH3)>(PH3));
                        });
                std::function <void(const tl::request &, std::string const &, std::string const &, std::string const &
                                    , const std::unordered_map <std::string, std::string> &, int &)> acquireStoryFunc(
                        [this](auto &&PH1, auto &&PH2, auto &&PH3, auto &&PH4, auto &&PH5, auto &&PH6)
                        {
                            ThalliumLocalAcquireStory(std::forward <decltype(PH1)>(PH1), std::forward <decltype(PH2)>(
                                            PH2), std::forward <decltype(PH3)>(PH3), std::forward <decltype(PH4)>(PH4)
                                                      , std::forward <decltype(PH5)>(PH5), std::forward <decltype(PH6)>(
                                            PH6));
                        });
                std::function <void(const tl::request &, std::string const &, std::string const &
                                    , std::string const &)> releaseStoryFunc(
                        [this](auto &&PH1, auto &&PH2, auto &&PH3, auto &&PH4)
                        {
                            ThalliumLocalReleaseStory(std::forward <decltype(PH1)>(PH1), std::forward <decltype(PH2)>(
                                    PH2), std::forward <decltype(PH3)>(PH3), std::forward <decltype(PH4)>(PH4));
                        });

                std::function <void(const tl::request &, std::string const &name, const std::string &
                                    , std::string &)> getChronicleAttrFunc(
                        [this](auto &&PH1, auto &&PH2, auto &&PH3, auto &&PH4)
                        {
                            ThalliumLocalGetChronicleAttr(std::forward <decltype(PH1)>(PH1)
                                                          , std::forward <decltype(PH2)>(PH2)
                                                          , std::forward <decltype(PH3)>(PH3)
                                                          , std::forward <decltype(PH4)>(PH4));
                        });
                std::function <void(const tl::request &, std::string const &name, const std::string &
                                    , const std::string &)> editChronicleAttrFunc(
                        [this](auto &&PH1, auto &&PH2, auto &&PH3, auto &&PH4)
                        {
                            ThalliumLocalEditChronicleAttr(std::forward <decltype(PH1)>(PH1)
                                                           , std::forward <decltype(PH2)>(PH2)
                                                           , std::forward <decltype(PH3)>(PH3)
                                                           , std::forward <decltype(PH4)>(PH4));
                        });
                std::function <void(const tl::request &, std::string &client_id)> showChroniclesFunc(
                        [this](auto &&PH1, auto &&PH2)
                        {
                            ThalliumLocalShowChronicles(std::forward <decltype(PH1)>(PH1), std::forward <decltype(PH2)>(
                                    PH2));
                        });
                std::function <void(const tl::request &, std::string &client_id
                                    , const std::string &chroicle_name)> showStoriesFunc(
                        [this](auto &&PH1, auto &&PH2, auto &&PH3)
                        {
                            ThalliumLocalShowStories(std::forward <decltype(PH1)>(PH1), std::forward <decltype(PH2)>(
                                    PH2), std::forward <decltype(PH3)>(PH3));
                        });

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

    CHRONOLOG_THALLIUM_DEFINE(LocalConnect, (uri, client_id, flags, clock_offset), const std::string &uri
                              , std::string const &client_id, int &flags, uint64_t &clock_offset)

    CHRONOLOG_THALLIUM_DEFINE(LocalDisconnect, (client_id, flags), std::string const &client_id, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalCreateChronicle, (name, attrs, flags), std::string const &name
                              , const std::unordered_map <std::string, std::string> &attrs, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyChronicle, (name), std::string const &name)

    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyStory, (chronicle_name, story_name), std::string const &chronicle_name
                              , std::string const &story_name)

    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireStory, (client_id, chronicle_name, story_name, attrs, flags)
                              , std::string const &client_id, std::string const &chronicle_name
                              , std::string const &story_name
                              , const std::unordered_map <std::string, std::string> &attrs, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseStory, (client_id, chronicle_name, story_name), std::string const &client_id
                              , std::string const &chronicle_name, std::string const &story_name)

    CHRONOLOG_THALLIUM_DEFINE(LocalGetChronicleAttr, (name, key, value), std::string const &name, const std::string &key
                              , std::string &value)

    CHRONOLOG_THALLIUM_DEFINE(LocalEditChronicleAttr, (name, key, value), std::string const &name
                              , const std::string &key, const std::string &value)

    CHRONOLOG_THALLIUM_DEFINE(LocalShowChronicles, (client_id), std::string &client_id)

    CHRONOLOG_THALLIUM_DEFINE(LocalShowStories, (client_id, chronicle_name), std::string&client_id
                              , const std::string &chronicle_name)

private:
    void set_prefix(std::string prefix)
    {
        func_prefix = prefix;
        switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
        {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
    }

    std::string func_prefix;
    std::shared_ptr <ChronoLogRPC> rpc;
    chronolog::KeeperRegistry*keeperRegistry;
    std::shared_ptr <ClientRegistryManager> clientManager;
    std::shared_ptr <ChronicleMetaDirectory> chronicleMetaDirectory;
};


#endif //CHRONOLOG_RPCVISOR_H
