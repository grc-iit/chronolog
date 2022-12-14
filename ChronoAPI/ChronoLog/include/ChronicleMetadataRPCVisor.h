//
// Created by kfeng on 4/4/22.
//

#ifndef CHRONOLOG_CHRONICLEMETADATARPCVISOR_H
#define CHRONOLOG_CHRONICLEMETADATARPCVISOR_H

#include <iostream>
#include <unordered_map>
#include <functional>
#include <thallium.hpp>
#include <sys/types.h>
#include <unistd.h>
#include "macro.h"
#include "RPCFactory.h"
#include "ChronicleMetaDirectory.h"
#include "ClientRegistryInfo.h"
#include "ClientRegistryManager.h"

class ChronicleMetadataRPCVisor {
public:
    ChronicleMetadataRPCVisor() : rpc(ChronoLog::Singleton<ChronoLogRPCFactory>::GetInstance()->
                                      GetRPC(CHRONOLOG_CONF->RPC_BASE_SERVER_PORT)) {
        func_prefix = "ChronoLog";
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
        if (CHRONOLOG_CONF->IS_SERVER) {
            chronicleMetaDirectory = ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance();
        }
        LOGD("%s constructor finishes, object created@%p in thread PID=%d", typeid(*this).name(), this, getpid());
    }

    ~ChronicleMetadataRPCVisor() = default;

    bool LocalCreateChronicle(std::string &name,
                              const std::unordered_map<std::string, std::string> &attrs,
                              int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, attrs=", __FUNCTION__, getpid(), name.c_str());
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->create_chronicle(name);
    }

    bool LocalDestroyChronicle(std::string &name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->destroy_chronicle(name, flags);
    }

    bool LocalAcquireChronicle(std::string &name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->acquire_chronicle(name, flags);
    }

    bool LocalReleaseChronicle(std::string &name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->release_chronicle(name, flags);
    }

    bool LocalCreateStory(std::string &chronicle_name,
                          std::string &story_name,
                          const std::unordered_map<std::string, std::string> &attrs,
                          int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, ,attrs=",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->create_story(chronicle_name, story_name, attrs);
    }

    bool LocalDestroyStory(std::string &chronicle_name, std::string &story_name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->destroy_story(chronicle_name, story_name, flags);
    }

    bool LocalAcquireStory(std::string &chronicle_name, std::string &story_name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->acquire_story(chronicle_name, story_name, flags);
    }

    bool LocalReleaseStory(std::string &chronicle_name, std::string &story_name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->release_story(chronicle_name, story_name, flags);
    }

    std::string LocalGetChronicleAttr(std::string &name, const std::string &key) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s", __FUNCTION__, getpid(), name.c_str(), key.c_str());
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->get_chronicle_attr(name, key);
    }

    bool LocalEditChronicleAttr(std::string &name, const std::string &key, const std::string &value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s, value=%s",
             __FUNCTION__, getpid(), name.c_str(), key.c_str(), value.c_str());
        extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;
        return g_chronicleMetaDirectory->edit_chronicle_attr(name, key, value);
    }

    void bind_functions() {
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE: {
                std::function<void(const tl::request &,
                                   std::string &,
                                   const std::unordered_map<std::string, std::string> &,
                                   int &)> createChronicleFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalCreateChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   const int &)> destroyChronicleFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalDestroyChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   const int &)> acquireChronicleFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalAcquireChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   const int &)> releaseChronicleFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalReleaseChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );

                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   const std::unordered_map<std::string, std::string> &,
                                   int &)> createStoryFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalCreateStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4,
                                  std::placeholders::_5)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   const int &)> destroyStoryFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalDestroyStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   const int &)> acquireStoryFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalAcquireStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   const int &)> releaseStoryFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalReleaseStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );

                std::function<void(const tl::request &,
                                   std::string &name,
                                   const std::string &key)> getChronicleAttrFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalGetChronicleAttr,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   std::string &name,
                                   const std::string &key,
                                   const std::string &value)> editChronicleAttrFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalEditChronicleAttr,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );

                rpc->bind("ChronoLogThalliumCreateChronicle", createChronicleFunc);
                rpc->bind("ChronoLogThalliumDestroyChronicle", destroyChronicleFunc);
                rpc->bind("ChronoLogThalliumAcquireChronicle", acquireChronicleFunc);
                rpc->bind("ChronoLogThalliumReleaseChronicle", releaseChronicleFunc);

                rpc->bind("ChronoLogThalliumCreateStory", createStoryFunc);
                rpc->bind("ChronoLogThalliumDestroyStory", destroyStoryFunc);
                rpc->bind("ChronoLogThalliumAcquireStory", acquireStoryFunc);
                rpc->bind("ChronoLogThalliumReleaseStory", releaseStoryFunc);

                rpc->bind("ChronoLogThalliumGetChronicleAttr", getChronicleAttrFunc);
                rpc->bind("ChronoLogThalliumEditChronicleAttr", editChronicleAttrFunc);
            }
        }
    }

    CHRONOLOG_THALLIUM_DEFINE(LocalCreateChronicle, (name, attrs, flags),
                    std::string &name, const std::unordered_map<std::string, std::string> &attrs, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyChronicle, (name, flags), std::string &name, const int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireChronicle, (name, flags), std::string &name, const int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseChronicle, (name, flags), std::string &name, const int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalCreateStory, (chronicle_name, story_name, attrs, flags),
                    std::string &chronicle_name,
                    std::string &story_name,
                    const std::unordered_map<std::string, std::string> &attrs,
                    int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyStory, (chronicle_name, story_name, flags),
                    std::string &chronicle_name, std::string &story_name, const int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireStory, (chronicle_name, story_name, flags),
                    std::string &chronicle_name, std::string &story_name, const int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseStory, (chronicle_name, story_name, flags),
                    std::string &chronicle_name, std::string &story_name, const int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalGetChronicleAttr, (name, key), std::string &name, const std::string &key)
    CHRONOLOG_THALLIUM_DEFINE(LocalEditChronicleAttr, (name, key, value),
                    std::string &name, const std::string &key, const std::string &value)

//private:
    std::shared_ptr<ChronicleMetaDirectory> chronicleMetaDirectory;
    ChronoLogCharStruct func_prefix;
    std::shared_ptr<ChronoLogRPC> rpc;
};

#endif //CHRONOLOG_CHRONICLEMETADATARPCVISOR_H
