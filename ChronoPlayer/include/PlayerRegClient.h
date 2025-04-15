#ifndef PLAYER_REG_CLIENT_H
#define PLAYER_REG_CLIENT_H

#include <iostream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

#include "PlayerIdCard.h"
#include "PlayerRegistrationMsg.h"
#include "PlayerStatsMsg.h"
#include "chronolog_errcode.h"
#include "chrono_monitor.h"

namespace tl = thallium;


namespace chronolog
{

class PlayerRegistryClient
{

public:
    static PlayerRegistryClient*
    CreateRegistryClient(tl::engine &tl_engine, std::string const &registry_service_addr
                               , uint16_t registry_provider_id)
    {
        try
        {
            return new PlayerRegistryClient(tl_engine, registry_service_addr, registry_provider_id);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[PlayerRegistryClient] Failed to create PlayerRegistryClient");
            return nullptr;
        }
    }

    int send_register_msg(PlayerRegistrationMsg const & msg)
    {
        try
        {
            std::stringstream ss;
            ss << msg;
            LOG_DEBUG("[PlayerRegistryClient] Sending Registration Message: {}", ss.str());
            return register_player.on(reg_service_ph)(msg);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[PlayerRegistryClient] Failed Sending Registration Message.");
            return CL_ERR_UNKNOWN;
        }
    }

    int send_unregister_msg(PlayerIdCard const &playerIdCard)
    {
        try
        {
            std::stringstream ss;
            ss << playerIdCard;
            LOG_DEBUG("[PlayerRegistryClient] Sending Unregister Message: {}", ss.str());
            return unregister_player.on(reg_service_ph)(playerIdCard);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[PlayerRegistryClient] Failed Sending Unregistered Message.");
            return CL_ERR_UNKNOWN;
        }
    }

    void send_stats_msg(PlayerStatsMsg const & statsMsg)
    {
        try
        {
            std::stringstream stats;
            stats << statsMsg;
            LOG_DEBUG("[PlayerRegistryClient] Sending Stats Message: {}", stats.str());
            handle_player_stats_msg.on(reg_service_ph)(statsMsg);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[PlayerRegisterClient] Failed Sending Stats Message.");
        }
    }

    ~PlayerRegistryClient()
    {
        LOG_DEBUG("[PlayerRegistryClient] Destructor called. Cleaning up resources...");
        register_player.deregister();
        unregister_player.deregister();
        handle_player_stats_msg.deregister();
    }

private:
    std::string reg_service_addr;     // na address of Keeper Registry Service 
    uint16_t reg_service_provider_id;          // KeeperRegistryService provider id
    tl::provider_handle reg_service_ph;  //provider_handle for remote registry service
    tl::remote_procedure register_player;
    tl::remote_procedure unregister_player;
    tl::remote_procedure handle_player_stats_msg;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    PlayerRegistryClient(tl::engine &tl_engine, std::string const &registry_addr, uint16_t registry_provider_id)
            : reg_service_addr(registry_addr), reg_service_provider_id(registry_provider_id), reg_service_ph(
            tl_engine.lookup(registry_addr), registry_provider_id)
    {
        LOG_DEBUG("[PlayerRegistryClient] Initialized for RegistryService at {} with ProviderID={}", registry_addr
             , registry_provider_id);
        register_player = tl_engine.define("register_player");
        unregister_player = tl_engine.define("unregister_player");
        handle_player_stats_msg = tl_engine.define("handle_player_stats_msg").disable_response();
    }
};
}

#endif
