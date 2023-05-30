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

class RPCVisor {
public:
    RPCVisor() {
        LOGD("%s constructor is called", typeid(*this).name());
        rpc = std::make_shared<ChronoLogRPC>();
        set_prefix("ChronoLog");
        LOGD("%s constructor finishes, object created@%p in thread PID=%d",
             typeid(*this).name(), this, getpid());
        clientManager = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
        chronicleMetaDirectory = ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance();
    }

    ~RPCVisor()
    {
    }

    void Visor_start() {
        rpc->start();
    }
    /**
     * Admin APIs
     */
    int LocalConnect(const std::string &uri, std::string &client_id, int &flags, uint64_t &clock_offset) {
        LOGD("%s in ChronoLogAdminRPCProxy@%p called in PID=%d, with args: uri=%s",
             __FUNCTION__, this, getpid(), uri.c_str());
        ClientInfo record;
        record.addr_ = "127.0.0.1";
        if (std::strtol(client_id.c_str(), nullptr, 10) < 0) {
            LOGE("client id is invalid");
            return CL_ERR_INVALID_ARG;
        }
        return clientManager->add_client_record(client_id,record);
    }

    int LocalDisconnect(const std::string &client_id, int &flags) {
        LOGD("%s is called in PID=%d, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        if (std::strtol(client_id.c_str(), nullptr, 10) < 0) {
            LOGE("client id is invalid");
            return CL_ERR_INVALID_ARG;
        }
        return clientManager->remove_client_record(client_id,flags);
    }

    /**
     * Metadata APIs
     */
    int LocalCreateChronicle(std::string &name,
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

    int LocalDestroyChronicle(std::string &name, int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        if (!name.empty()) {
            return chronicleMetaDirectory->destroy_chronicle(name, flags);
        } else {
            LOGE("name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalDestroyStory(std::string &chronicle_name, std::string &story_name, int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        if (!chronicle_name.empty() && !story_name.empty()) {
            return chronicleMetaDirectory->destroy_story(chronicle_name,story_name,flags);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalAcquireStory(std::string &client_id,
                          std::string& chronicle_name,
                          std::string& story_name,
                          const std::unordered_map<std::string, std::string> &attrs,
                          int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
        if (!chronicle_name.empty() && !story_name.empty()) {
            int ret = chronicleMetaDirectory->create_story(chronicle_name, story_name, attrs);
            if (ret != CL_SUCCESS) return ret;
            return chronicleMetaDirectory->acquire_story(client_id, chronicle_name, story_name, flags);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalReleaseStory(std::string &client_id, std::string& chronicle_name, std::string& story_name, int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        if (!chronicle_name.empty() && !story_name.empty()) {
            return chronicleMetaDirectory->release_story(client_id, chronicle_name, story_name, flags);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalGetChronicleAttr(std::string &name, const std::string &key, std::string &value) {
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

    int LocalEditChronicleAttr(std::string &name, const std::string &key, const std::string &value) {
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
                                   std::string &,
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
                                   std::string &,
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
                                   std::string &,
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
                                   std::string &,
                                   int &)> destroyChronicleFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3) {
                            ThalliumLocalDestroyChronicle(std::forward<decltype(PH1)>(PH1),
                                                          std::forward<decltype(PH2)>(PH2),
                                                          std::forward<decltype(PH3)>(PH3));
                        }
                );

                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   int &)> destroyStoryFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4) {
                            ThalliumLocalDestroyStory(std::forward<decltype(PH1)>(PH1),
                                                      std::forward<decltype(PH2)>(PH2),
                                                      std::forward<decltype(PH3)>(PH3),
                                                      std::forward<decltype(PH4)>(PH4));
                        }
                );
                std::function<void(const tl::request &,
				                   std::string &,
                                   std::string &,
                                   std::string &,
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
				                   std::string &,
                                   std::string &,
                                   std::string &,
                                   int &)> releaseStoryFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4,
                               auto && PH5) {
                            ThalliumLocalReleaseStory(std::forward<decltype(PH1)>(PH1),
                                                      std::forward<decltype(PH2)>(PH2),
                                                      std::forward<decltype(PH3)>(PH3),
                                                      std::forward<decltype(PH4)>(PH4),
                                                      std::forward<decltype(PH5)>(PH5));
                        }
                );

                std::function<void(const tl::request &,
                                   std::string &name,
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
                                   std::string &name,
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
                              const std::string &uri, std::string &client_id, int &flags, uint64_t &clock_offset)

    CHRONOLOG_THALLIUM_DEFINE(LocalDisconnect, (client_id, flags), std::string &client_id, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalCreateChronicle, (name, attrs, flags),
                              std::string &name, const std::unordered_map<std::string, std::string> &attrs, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyChronicle, (name, flags), std::string &name, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyStory, (chronicle_name, story_name, flags),
                              std::string &chronicle_name, std::string &story_name, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireStory, (client_id, chronicle_name, story_name, attrs, flags),
                              std::string &client_id,
                              std::string &chronicle_name,
                              std::string &story_name,
                              const std::unordered_map<std::string, std::string> &attrs,
                              int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseStory, (client_id, chronicle_name, story_name, flags),
                              std::string &client_id, std::string &chronicle_name, std::string &story_name, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalGetChronicleAttr, (name, key, value),
                              std::string &name, const std::string &key, std::string &value)

    CHRONOLOG_THALLIUM_DEFINE(LocalEditChronicleAttr, (name, key, value),
                              std::string &name, const std::string &key, const std::string &value)

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
    std::shared_ptr<ClientRegistryManager> clientManager;
    std::shared_ptr<ChronicleMetaDirectory> chronicleMetaDirectory;
};


#endif //CHRONOLOG_RPCVISOR_H
