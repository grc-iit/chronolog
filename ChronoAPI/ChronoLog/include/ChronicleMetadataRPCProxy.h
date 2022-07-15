//
// Created by kfeng on 4/4/22.
//

#ifndef CHRONOLOG_CHRONICLEMETADATARPCPROXY_H
#define CHRONOLOG_CHRONICLEMETADATARPCPROXY_H

#include <iostream>
#include <unordered_map>
#include <functional>
#include <thallium.hpp>
#include <sys/types.h>
#include <unistd.h>
#include "macro.h"
#include "RPCFactory.h"
#include "ChronicleMetaDirectory.h"
#include "ClientRegistryRecord.h"
#include "ClientRegistryManager.h"

class ChronicleMetadataRPCProxy {
public:
    ChronicleMetadataRPCProxy() : chronicleMetaDirectory(ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance()),
                                  rpc(ChronoLog::Singleton<RPCFactory>::GetInstance()->GetRPC(CHRONOLOG_CONF->RPC_PORT)) {
        func_prefix = "ChronoLog";
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case THALLIUM_SOCKETS:
            case THALLIUM_TCP:
            case THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
        if (CHRONOLOG_CONF->IS_SERVER) {
            bind_functions();
            rpc->start();
        }
    }

    ~ChronicleMetadataRPCProxy() = default;

    bool LocalCreateChronicle(std::string &name, const std::unordered_map<std::string, std::string> &attrs) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: name=" << name << ", attrs=<";
//        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            //std::cout << iter->first << "=" << iter->second;
//            if (iter != attrs.end()) //std::cout << ", ";
//        }
        //std::cout << ">" << std::endl;
        return chronicleMetaDirectory->create_chronicle(name);
    }

    bool CreateChronicle(std::string &name, const std::unordered_map<std::string, std::string> &attrs) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("CreateChronicle", 0, bool, name, attrs);
    }

    bool LocalDestroyChronicle(std::string &name, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: name=" << name << ", flags=" << flags << std::endl;
        return chronicleMetaDirectory->destroy_chronicle(name, flags);
    }

    bool DestroyChronicle(std::string &name, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("DestroyChronicle", 0, bool, name, flags);
    }

    bool LocalAcquireChronicle(std::string &name, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: name=" << name << ", flags=" << flags << std::endl;
        return chronicleMetaDirectory->acquire_chronicle(name, flags);
    }

    bool AcquireChronicle(std::string &name, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("AcquireChronicle", 0, bool, name, flags);
    }

    bool LocalReleaseChronicle(uint64_t &cid, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: cid=" << cid << ", flags=" << flags << std::endl;
        return chronicleMetaDirectory->release_chronicle(cid, flags);
    }

    bool ReleaseChronicle(uint64_t &cid, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("ReleaseChronicle", 0, bool, cid, flags);
    }

    bool LocalCreateStory(uint64_t &cid, std::string &name, const std::unordered_map<std::string, std::string> &attrs) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: name=" << name << ", attrs=<";
//        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            //std::cout << iter->first << "=" << iter->second;
//            if (iter != attrs.end()) //std::cout << ", ";
//        }
        //std::cout << ">" << std::endl;
        return chronicleMetaDirectory->create_story(cid, name, attrs);
    }

    bool CreateStory(uint64_t &cid, std::string &name, const std::unordered_map<std::string, std::string> &attrs) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("CreateStory", 0, bool, cid, name, attrs);
    }

    bool LocalDestroyStory(uint64_t &cid, std::string &name, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: name=" << name << ", flags=" << flags << std::endl;
        return chronicleMetaDirectory->destroy_story(cid, name, flags);
    }

    bool DestroyStory(uint64_t &cid, std::string &name, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("DestroyStory", 0, bool, cid, name, flags);
    }

    bool LocalAcquireStory(uint64_t &cid, std::string &name, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: name=" << name << ", flags=" << flags << std::endl;
        return chronicleMetaDirectory->acquire_story(cid, name, flags);
    }

    bool AcquireStory(uint64_t &cid, std::string &name, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("AcquireStory", 0, bool, cid, name, flags);
    }

    bool LocalReleaseStory(uint64_t &sid, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: sid=" << sid << ", flags=" << flags << std::endl;
        return chronicleMetaDirectory->release_story(sid, flags);
    }

    bool ReleaseStory(uint64_t &sid, const int &flags) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("ReleaseStory", 0, bool, sid, flags);
    }

    std::string LocalGetChronicleAttr(uint64_t &cid, const std::string &key) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: cid=" << cid << ", key=" << key << std::endl;
        return chronicleMetaDirectory->get_chronicle_attr(cid, key);
    }

    std::string GetChronicleAttr(uint64_t &cid, const std::string &key) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("GetChronicleAttr", 0, std::string, cid, key);
    }

    bool LocalEditChronicleAttr(uint64_t &cid, const std::string &key, const std::string &value) {
        //std::cout << __func__ << " is called in PID=" << getpid()
//                  << " with args: cid=" << cid << ", key=" << key << ", value=" << value << std::endl;
        return chronicleMetaDirectory->edit_chronicle_attr(cid, key, value);
    }

    bool EditChronicleAttr(uint64_t &cid, const std::string &key, const std::string &value) {
        //std::cout << __func__ << " is called in PID=" << getpid() << std::endl;
        return RPC_CALL_WRAPPER("EditChronicleAttr", 0, bool, cid, key, value);
    }
    
    void bind_functions() {
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case THALLIUM_SOCKETS:
            case THALLIUM_TCP:
            case THALLIUM_ROCE: {
                std::function<void(const tl::request &,
                                   std::string &,
                                   const std::unordered_map<std::string, std::string> &)> createChronicleFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalCreateChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   const int &)> destroyChronicleFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalDestroyChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   const int &)> acquireChronicleFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalAcquireChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   uint64_t &,
                                   const int &)> releaseChronicleFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalReleaseChronicle,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );

                std::function<void(const tl::request &,
                                   uint64_t &,
                                   std::string &,
                                   const std::unordered_map<std::string, std::string> &)> createStoryFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalCreateStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );
                std::function<void(const tl::request &,
                                   uint64_t &,
                                   std::string &,
                                   const int &)> destroyStoryFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalDestroyStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );
                std::function<void(const tl::request &,
                                   uint64_t &,
                                   std::string &,
                                   const int &)> acquireStoryFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalAcquireStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4)
                );
                std::function<void(const tl::request &,
                                   uint64_t &,
                                   const int &)> releaseStoryFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalReleaseStory,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );

                std::function<void(const tl::request &,
                                   uint64_t &,
                                   const std::string &)> getChronicleAttrFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalGetChronicleAttr,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   uint64_t &,
                                   const std::string &key,
                                   const std::string &value)> editChronicleAttrFunc(
                        std::bind(&ChronicleMetadataRPCProxy::ThalliumLocalEditChronicleAttr,
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

    THALLIUM_DEFINE(LocalCreateChronicle, (name, attrs),
                    std::string &name, const std::unordered_map<std::string, std::string> &attrs)
    THALLIUM_DEFINE(LocalDestroyChronicle, (name, flags), std::string &name, const int &flags)
    THALLIUM_DEFINE(LocalAcquireChronicle, (name, flags), std::string &name, const int &flags)
    THALLIUM_DEFINE(LocalReleaseChronicle, (cid, flags), uint64_t &cid, const int &flags)

    THALLIUM_DEFINE(LocalCreateStory, (cid, name, attrs),
                    uint64_t &cid, std::string &name, const std::unordered_map<std::string, std::string> &attrs)
    THALLIUM_DEFINE(LocalDestroyStory, (cid, name, flags), uint64_t &cid, std::string &name, const int &flags)
    THALLIUM_DEFINE(LocalAcquireStory, (cid, name, flags), uint64_t &cid, std::string &name, const int &flags)
    THALLIUM_DEFINE(LocalReleaseStory, (sid, flags), uint64_t &sid, const int &flags)

    THALLIUM_DEFINE(LocalGetChronicleAttr, (cid, key), uint64_t &cid, const std::string &key)
    THALLIUM_DEFINE(LocalEditChronicleAttr, (cid, key, value),
                    uint64_t &cid, const std::string &key, const std::string &value)

private:
    std::shared_ptr<ChronicleMetaDirectory> chronicleMetaDirectory;
    CharStruct func_prefix;
    std::shared_ptr<RPC> rpc;
};

#endif //CHRONOLOG_CHRONICLEMETADATARPCPROXY_H
