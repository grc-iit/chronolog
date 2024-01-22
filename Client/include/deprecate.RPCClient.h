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

#include "AcquireStoryResponseMsg.h"

class RPCClient
{
public:
    RPCClient()
    {
        LOG_DEBUG("%s constructor is called", typeid(*this).name());
        rpc = std::make_shared <ChronoLogRPC>();
        set_prefix("ChronoLog");
        LOG_DEBUG("%s constructor finishes, object created@%p in thread PID=%d", typeid(*this).name(), this, getpid());
    }

    ~RPCClient()
    {
    }

    int Connect(const std::string &uri, std::string const &client_id, int &flags, uint64_t &clock_offset)
    {
        LOG_DEBUG("%s in ChronoLogAdminRPCProxy at addresss %p called in PID=%d, with args: uri=%s, client_id=%s"
             , __FUNCTION__, this, getpid(), uri.c_str(), client_id.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("Connect", 0, int, uri, client_id, flags, clock_offset);
    }

    int Disconnect(std::string const &client_id, int &flags)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: client_id=%s, flags=%d", __FUNCTION__, getpid(), client_id.c_str()
             , flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("Disconnect", 0, int, client_id, flags);
    }

    int CreateChronicle(std::string const &name, const std::unordered_map <std::string, std::string> &attrs, int &flags)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: name=%s, flags=%d, attrs=", __FUNCTION__, getpid(), name.c_str()
             , flags);
        for(auto iter = attrs.begin(); iter != attrs.end(); ++iter)
        {
            LOG_DEBUG("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
        return CHRONOLOG_RPC_CALL_WRAPPER("CreateChronicle", 0, int, name, attrs, flags);
    }

    int DestroyChronicle(std::string const &name)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: name=%s", __FUNCTION__, getpid(), name.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("DestroyChronicle", 0, int, name);
    }

    int DestroyStory(std::string const &chronicle_name, std::string const &story_name)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s", __FUNCTION__, getpid()
             , chronicle_name.c_str(), story_name.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("DestroyStory", 0, int, chronicle_name, story_name);
    }

    chronolog::AcquireStoryResponseMsg
    AcquireStory(std::string &client_id, std::string const &chronicle_name, std::string const &story_name
                 , const std::unordered_map <std::string, std::string> &attrs, const int &flags)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: client_id=%s, chronicle_name=%s, story_name=%s, flags=%d", __FUNCTION__
             , getpid(), client_id.c_str(), chronicle_name.c_str(), story_name.c_str(), flags);
        for(auto iter = attrs.begin(); iter != attrs.end(); ++iter)
        {
            LOG_DEBUG("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
        return CHRONOLOG_RPC_CALL_WRAPPER("AcquireStory", 0, chronolog::AcquireStoryResponseMsg, client_id
                                          , chronicle_name, story_name, attrs, flags);
    }

    int ReleaseStory(std::string &client_id, std::string const &chronicle_name, std::string const &story_name)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: client_id=%s, chronicle_name=%s, story_name=%s", __FUNCTION__, getpid()
             , client_id.c_str(), chronicle_name.c_str(), story_name.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("ReleaseStory", 0, int, client_id, chronicle_name, story_name);
    }

    int GetChronicleAttr(std::string const &name, const std::string &key, std::string &value)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: name=%s, key=%s", __FUNCTION__, getpid(), name.c_str(), key.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("GetChronicleAttr", 0, int, name, key, value);
    }

    int EditChronicleAttr(std::string const &name, const std::string &key, const std::string &value)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: name=%s, key=%s, value=%s", __FUNCTION__, getpid(), name.c_str()
             , key.c_str(), value.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("EditChronicleAttr", 0, int, name, key, value);
    }

    std::vector <std::string> ShowChronicles(std::string const &client_id)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: client_id=%s", __FUNCTION__, getpid(), client_id.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("ShowChronicles", 0, std::vector <std::string>, client_id);
    }

    std::vector <std::string> ShowStories(std::string const &client_id, const std::string &chronicle_name)
    {
        LOG_DEBUG("%s is called in PID=%d, with args: client_id=%s, chronicle_name=%s", __FUNCTION__, getpid()
             , client_id.c_str(), chronicle_name.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("ShowStories", 0, std::vector <std::string>, client_id, chronicle_name);
    }

    tl::engine &get_tl_client_engine()
    { return rpc->get_tl_client_engine(); }

private:
    void set_prefix(std::string prefix)
    {
        func_prefix = std::move(prefix);
        switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
        {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
    }

    ChronoLogCharStruct func_prefix;
    //ChronoLogRPC *rpc;
    std::shared_ptr <ChronoLogRPC> rpc;
};


#endif //CHRONOLOG_RPCCLIENT_H
