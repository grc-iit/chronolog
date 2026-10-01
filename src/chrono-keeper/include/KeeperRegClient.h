#ifndef KEEPER_REG_CLIENT_H
#define KEEPER_REG_CLIENT_H

#include <iostream>
#include <string>
#include <sstream>
#include <cstdint>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

#include <KeeperIdCard.h>
#include <KeeperRegistrationMsg.h>
#include <KeeperStatsMsg.h>
#include <ChronoClock.h>
#include <ClockState.h>
#include <chronolog_errcode.h>

namespace tl = thallium;


namespace chronolog
{

class KeeperRegistryClient
{

public:
    static KeeperRegistryClient* CreateKeeperRegistryClient(tl::engine& tl_engine,
                                                            std::string const& registry_service_addr,
                                                            uint16_t registry_provider_id)
    {
        try
        {
            return new KeeperRegistryClient(tl_engine, registry_service_addr, registry_provider_id);
        }
        catch(tl::exception const&)
        {
            LOG_ERROR("[KeeperRegistryClient] Failed to create KeeperRegistryClient");
            return nullptr;
        }
    }

    int send_register_msg(KeeperRegistrationMsg const& keeperMsg)
    {
        try
        {
            std::stringstream ss;
            ss << keeperMsg;
            LOG_DEBUG("[KeeperRegisterClient] Sending Register Message: {}", ss.str());
            return register_keeper.on(reg_service_ph)(keeperMsg);
        }
        catch(tl::exception const&)
        {
            LOG_ERROR("[KeeperRegisterClient] Failed Sending Register Message.");
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    int send_unregister_msg(KeeperIdCard const& keeperIdCard)
    {
        try
        {
            std::stringstream ss;
            ss << keeperIdCard;
            LOG_DEBUG("[KeeperRegisterClient] Sending Unregister Message: {}", ss.str());
            return unregister_keeper.on(reg_service_ph)(keeperIdCard);
        }
        catch(tl::exception const&)
        {
            LOG_ERROR("[KeeperRegisterClient] Failed Sending Unregistered Message.");
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    // The periodic stats heartbeat doubles as this keeper's clock exchange with
    // the Visor: bracket the round trip with local timestamps and fold the Visor
    // authority tick from the reply into the process clock offset.
    void send_stats_msg(KeeperStatsMsg const& keeperStatsMsg)
    {
        try
        {
            std::stringstream ss;
            ss << keeperStatsMsg;
            LOG_DEBUG("[KeeperRegisterClient] Sending Stats Message: {}", ss.str());
            // Timed so an unresponsive Visor cannot wedge the daemon's heartbeat
            // loop (the RPC is now request/response, not fire-and-forget).
            uint64_t t0 = ProcessClock().local_reading();
            ClockState visor_state = handle_stats_msg.on(reg_service_ph).timed(std::chrono::seconds(5), keeperStatsMsg);
            uint64_t t1 = ProcessClock().local_reading();
            ProcessClock().applySync(t0, visor_state.visor_time, t1);
            LOG_DEBUG("[KeeperRegisterClient] Clock synced via heartbeat: offset={}, uncertainty={}",
                      ProcessClock().offset(),
                      ProcessClock().state().uncertainty);
        }
        catch(tl::timeout const&)
        {
            // tl::timeout derives from std::exception, not tl::exception.
            LOG_WARNING("[KeeperRegisterClient] Stats/clock-sync RPC timed out.");
        }
        catch(tl::exception const&)
        {
            LOG_ERROR("[KeeperRegisterClient] Failed Sending Stats Message.");
        }
    }

    ~KeeperRegistryClient()
    {
        LOG_DEBUG("[KeeperRegistryClient] Destructor called. Cleaning up resources...");
        register_keeper.deregister();
        unregister_keeper.deregister();
        handle_stats_msg.deregister();
    }

private:
    std::string reg_service_addr;       // na address of Keeper Registry Service
    uint16_t reg_service_provider_id;   // KeeperRegistryService provider id
    tl::provider_handle reg_service_ph; //provider_handle for remote registry service
    tl::remote_procedure register_keeper;
    tl::remote_procedure unregister_keeper;
    tl::remote_procedure handle_stats_msg;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRegistryClient(tl::engine& tl_engine, std::string const& registry_addr, uint16_t registry_provider_id)
        : reg_service_addr(registry_addr)
        , reg_service_provider_id(registry_provider_id)
        , reg_service_ph(tl_engine.lookup(registry_addr), registry_provider_id)
    {
        LOG_DEBUG("[KeeperRegistryClient] Initialized for RegistryService at {} with ProviderID={}",
                  registry_addr,
                  registry_provider_id);
        register_keeper = tl_engine.define("register_keeper");
        unregister_keeper = tl_engine.define("unregister_keeper");
        handle_stats_msg = tl_engine.define("handle_stats_msg");
    }
};
} // namespace chronolog

#endif
