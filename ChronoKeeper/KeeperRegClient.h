#ifndef KEEPER_REG_CLIENT_H
#define KEEPER_REG_CLIENT_H

#include <iostream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

#include "chrono_common/KeeperIdCard.h"
#include "chrono_common/KeeperRegistrationMsg.h"
#include "chrono_common/KeeperStatsMsg.h"
#include "chronolog_errcode.h"

namespace tl = thallium;


namespace chronolog
{

class KeeperRegistryClient
{

public:
    static KeeperRegistryClient*
    CreateKeeperRegistryClient(tl::engine &tl_engine, std::string const &registry_service_addr
                               , uint16_t registry_provider_id)
    {
        try
        {
            return new KeeperRegistryClient(tl_engine, registry_service_addr, registry_provider_id);
        }
        catch(tl::exception const &)
        {
            Logger::getLogger()->error("[KeeperRegistryClient] Failed to create KeeperRegistryClient");
            return nullptr;
        }
    }

    int send_register_msg(KeeperRegistrationMsg const &keeperMsg)
    {
        try
        {
            std::stringstream ss;
            ss << keeperMsg;
            Logger::getLogger()->debug("[KeeperRegisterClient] Sending Register Message: {}", ss.str());
            return register_keeper.on(reg_service_ph)(keeperMsg);
        }
        catch(tl::exception const &)
        {
            Logger::getLogger()->error("[KeeperRegisterClient] Failed Sending Register Message.");
            return CL_ERR_UNKNOWN;
        }
    }

    int send_unregister_msg(KeeperIdCard const &keeperIdCard)
    {
        try
        {
            std::stringstream ss;
            ss << keeperIdCard;
            Logger::getLogger()->debug("[KeeperRegisterClient] Sending Unregister Message: {}", ss.str());
            return unregister_keeper.on(reg_service_ph)(keeperIdCard);
        }
        catch(tl::exception const &)
        {
            Logger::getLogger()->error("[KeeperRegisterClient] Failed Sending Unregistered Message.");
            return CL_ERR_UNKNOWN;
        }
    }

    void send_stats_msg(KeeperStatsMsg const &keeperStatsMsg)
    {
        try
        {
            std::stringstream ss;
            ss << keeperStatsMsg;
            Logger::getLogger()->debug("[KeeperRegisterClient] Sending Stats Message: {}", ss.str());
            handle_stats_msg.on(reg_service_ph)(keeperStatsMsg);
        }
        catch(tl::exception const &)
        {
            Logger::getLogger()->error("[KeeperRegisterClient] Failed Sending Stats Message.");
        }
    }

    ~KeeperRegistryClient()
    {
        Logger::getLogger()->debug("[KeeperRegistryClient] Destructor called. Cleaning up resources...");
        register_keeper.deregister();
        unregister_keeper.deregister();
        handle_stats_msg.deregister();
    }

private:
    std::string reg_service_addr;     // na address of Keeper Registry Service 
    uint16_t reg_service_provider_id;          // KeeperRegistryService provider id
    tl::provider_handle reg_service_ph;  //provider_handle for remote registry service
    tl::remote_procedure register_keeper;
    tl::remote_procedure unregister_keeper;
    tl::remote_procedure handle_stats_msg;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRegistryClient(tl::engine &tl_engine, std::string const &registry_addr, uint16_t registry_provider_id)
            : reg_service_addr(registry_addr), reg_service_provider_id(registry_provider_id), reg_service_ph(
            tl_engine.lookup(registry_addr), registry_provider_id)
    {
        Logger::getLogger()->debug("[KeeperRegistryClient] Initialized for RegistryService at {} with ProviderID={}"
                                   , registry_addr, registry_provider_id);
        register_keeper = tl_engine.define("register_keeper");
        unregister_keeper = tl_engine.define("unregister_keeper");
        handle_stats_msg = tl_engine.define("handle_stats_msg").disable_response();
    }
};
}

#endif
