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
#include "errcode.h"
#include "RPCFactory.h"
#include "ChronicleMetaDirectory.h"
#include "ClientRegistryInfo.h"
#include "ClientRegistryManager.h"

extern std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory;

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

    int LocalCreateChronicle(std::string& name,
                              const std::unordered_map<std::string, std::string>& attrs,
                              int& flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, attrs=", __FUNCTION__, getpid(), name.c_str());
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }

        if (!name.empty()) {
            return g_chronicleMetaDirectory->create_chronicle(name);
        } else {
            LOGE("name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalDestroyChronicle(std::string& name, int& flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        if (!name.empty()) {
            return g_chronicleMetaDirectory->destroy_chronicle(name, flags);
        } else {
            LOGE("name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalAcquireChronicle(std::string& name, int& flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        if (!name.empty()) {
            return g_chronicleMetaDirectory->acquire_chronicle(name, flags);
        } else {
            LOGE("name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalReleaseChronicle(std::string& name, int& flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        if (!name.empty()) {
            return g_chronicleMetaDirectory->release_chronicle(name, flags);
        } else {
            LOGE("name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalCreateStory(std::string& chronicle_name,
                          std::string& story_name,
                          const std::unordered_map<std::string, std::string>& attrs,
                          int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, ,attrs=",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
        if (!chronicle_name.empty() && !story_name.empty()) {
            return g_chronicleMetaDirectory->create_story(chronicle_name, story_name, attrs);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalDestroyStory(std::string& chronicle_name, std::string& story_name, int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        if (!chronicle_name.empty() && !story_name.empty()) {
            return g_chronicleMetaDirectory->destroy_story(chronicle_name, story_name, flags);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalAcquireStory(std::string& chronicle_name, std::string& story_name, int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        if (!chronicle_name.empty() && !story_name.empty()) {
            return g_chronicleMetaDirectory->acquire_story(chronicle_name, story_name, flags);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalReleaseStory(std::string& chronicle_name, std::string& story_name, int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        if (!chronicle_name.empty() && !story_name.empty()) {
            return g_chronicleMetaDirectory->release_story(chronicle_name, story_name, flags);
        } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalGetChronicleAttr(std::string& name, const std::string& key, std::string& value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s", __FUNCTION__, getpid(), name.c_str(), key.c_str());
        if (!name.empty() && !key.empty()) {
            g_chronicleMetaDirectory->get_chronicle_attr(name, key, value);
            return CL_SUCCESS;
        } else {
            if (name.empty())
                LOGE("name is empty");
            if (key.empty())
                LOGE("key is empty");
            return CL_ERR_INVALID_ARG;
        }
    }

    int LocalEditChronicleAttr(std::string& name, const std::string& key, const std::string& value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s, value=%s",
             __FUNCTION__, getpid(), name.c_str(), key.c_str(), value.c_str());
        if (!name.empty() && !key.empty() && !value.empty()) {
            return g_chronicleMetaDirectory->edit_chronicle_attr(name, key, value);
        } else {
            if (name.empty())
                LOGE("name is empty");
            if (key.empty())
                LOGE("key is empty");
            return CL_ERR_INVALID_ARG;
        }
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
                                   int &)> destroyChronicleFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalDestroyChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   int &)> acquireChronicleFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalAcquireChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   int &)> releaseChronicleFunc(
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
                                   int &)> destroyStoryFunc(
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
                                   int &)> acquireStoryFunc(
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
                                   int &)> releaseStoryFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalReleaseStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );

                std::function<void(const tl::request &,
                                   std::string &name,
                                   const std::string &,
                                   std::string &)> getChronicleAttrFunc(
                        std::bind(&ChronicleMetadataRPCVisor::ThalliumLocalGetChronicleAttr,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );
                std::function<void(const tl::request &,
                                   std::string &name,
                                   const std::string &,
                                   const std::string &)> editChronicleAttrFunc(
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
    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyChronicle, (name, flags), std::string &name, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireChronicle, (name, flags), std::string &name, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseChronicle, (name, flags), std::string &name, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalCreateStory, (chronicle_name, story_name, attrs, flags),
                    std::string &chronicle_name,
                    std::string &story_name,
                    const std::unordered_map<std::string, std::string> &attrs,
                    int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyStory, (chronicle_name, story_name, flags),
                    std::string &chronicle_name, std::string &story_name, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireStory, (chronicle_name, story_name, flags),
                    std::string &chronicle_name, std::string &story_name, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseStory, (chronicle_name, story_name, flags),
                    std::string &chronicle_name, std::string &story_name, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalGetChronicleAttr, (name, key, value),
                              std::string &name, const std::string &key, std::string &value)
    CHRONOLOG_THALLIUM_DEFINE(LocalEditChronicleAttr, (name, key, value),
                    std::string &name, const std::string &key, const std::string &value)

//private:
    std::shared_ptr<ChronicleMetaDirectory> chronicleMetaDirectory;
    ChronoLogCharStruct func_prefix;
    std::shared_ptr<ChronoLogRPC> rpc;
};

#endif //CHRONOLOG_CHRONICLEMETADATARPCVISOR_H
