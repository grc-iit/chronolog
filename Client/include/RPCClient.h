//
// Created by kfeng on 12/26/22.
//

#ifndef CHRONOLOG_RPCCLIENT_H
#define CHRONOLOG_RPCCLIENT_H

#include <iostream>
#include <unordered_map>
#include <functional>
#include <thallium.hpp>
#include <utility>
#include <sys/types.h>
#include <unistd.h>
#include "macro.h"
#include "RPCFactory.h"

class RPCClient {
public:
    RPCClient() {
        LOGD("%s constructor is called", typeid(*this).name());
        rpc = new ChronoLogRPC(); //ChronoLog::Singleton<ChronoLogRPCFactory>::GetInstance()
                //->GetRPC(CHRONOLOG_CONF->RPC_BASE_SERVER_PORT);
        set_prefix("ChronoLog");
        LOGD("%s constructor finishes, object created@%p in thread PID=%d",
             typeid(*this).name(), this, getpid());
    }

    ~RPCClient()
    {
	delete rpc;
    }

    int Connect(const std::string &uri, std::string &client_id, int &flags, uint64_t &clock_offset) {
        LOGD("%s in ChronoLogAdminRPCProxy at addresss %p called in PID=%d, with args: uri=%s, client_id=%s",
             __FUNCTION__, this, getpid(), uri.c_str(), client_id.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("Connect", 0, int, uri, client_id, flags, clock_offset);
    }

    int Disconnect(const std::string &client_id, int &flags) {
        LOGD("%s is called in PID=%d, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("Disconnect", 0, int, client_id, flags);
    }

    int CreateChronicle(std::string &name,
                        const std::unordered_map<std::string, std::string> &attrs,
                        int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d, attrs=",
             __FUNCTION__, getpid(), name.c_str(), flags);
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
        return CHRONOLOG_RPC_CALL_WRAPPER("CreateChronicle", 0, int, name, attrs, flags);
    }

    int DestroyChronicle(std::string &name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("DestroyChronicle", 0, int, name, flags);
    }

    int AcquireChronicle(std::string &client_id, std::string &name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("AcquireChronicle", 0, int, client_id,name, flags);
    }

    int ReleaseChronicle(std::string &client_id, std::string &name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("ReleaseChronicle", 0, int, client_id, name, flags);
    }

    int CreateStory(std::string &chronicle_name,
                    std::string &story_name,
                    const std::unordered_map<std::string, std::string> &attrs,
                    int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d ,attrs=",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
        return CHRONOLOG_RPC_CALL_WRAPPER("CreateStory", 0, int, chronicle_name, story_name, attrs, flags);
    }

    int DestroyStory(std::string &chronicle_name, std::string &story_name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("DestroyStory", 0, int, chronicle_name, story_name, flags);
    }

    int AcquireStory(std::string &client_id, std::string &chronicle_name, std::string &story_name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("AcquireStory", 0, int, client_id, chronicle_name, story_name, flags);
    }

    int ReleaseStory(std::string &client_id, std::string &chronicle_name, std::string &story_name, const int &flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("ReleaseStory", 0, int, client_id, chronicle_name, story_name, flags);
    }

    int GetChronicleAttr(std::string &name, const std::string &key, std::string &value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s", __FUNCTION__, getpid(), name.c_str(), key.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("GetChronicleAttr", 0, int, name, key, value);
    }

    int EditChronicleAttr(std::string &name, const std::string &key, const std::string &value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s, value=%s",
             __FUNCTION__, getpid(), name.c_str(), key.c_str(), value.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("EditChronicleAttr", 0, int, name, key, value);
    }


private:
    void set_prefix(std::string prefix) {
        func_prefix = std::move(prefix);
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
    }

    ChronoLogCharStruct func_prefix;
    ChronoLogRPC *rpc;
    //std::shared_ptr<ChronoLogRPC> rpc;
};


#endif //CHRONOLOG_RPCCLIENT_H
