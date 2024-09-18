#ifndef GRAPHER_REG_CLIENT_H
#define GRAPHER_REG_CLIENT_H

#include <iostream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

#include "GrapherIdCard.h"
#include "GrapherRegistrationMsg.h"
#include "GrapherStatsMsg.h"
#include "chronolog_errcode.h"

namespace tl = thallium;


namespace chronolog
{

class GrapherRegistryClient
{

public:
    static GrapherRegistryClient*
    CreateRegistryClient(tl::engine &tl_engine, std::string const &registry_service_addr
                               , uint16_t registry_provider_id)
    {
        try
        {
            return new GrapherRegistryClient(tl_engine, registry_service_addr, registry_provider_id);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[GrapherRegistryClient] Failed to create GrapherRegistryClient");
            return nullptr;
        }
    }

    int send_register_msg(GrapherRegistrationMsg const & grapherMsg)
    {
        try
        {
            std::stringstream ss;
            ss << grapherMsg;
            LOG_DEBUG("[GrapherRegistryClient] Sending Registration Message: {}", ss.str());
            return register_grapher.on(reg_service_ph)(grapherMsg);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[GrapherRegistryClient] Failed Sending Registration Message.");
            return CL_ERR_UNKNOWN;
        }
    }

    int send_unregister_msg(GrapherIdCard const &grapherIdCard)
    {
        try
        {
            std::stringstream ss;
            ss << grapherIdCard;
            LOG_DEBUG("[GrapherRegistryClient] Sending Unregister Message: {}", ss.str());
            return unregister_grapher.on(reg_service_ph)(grapherIdCard);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[GrapherRegistryClient] Failed Sending Unregistered Message.");
            return CL_ERR_UNKNOWN;
        }
    }

    void send_stats_msg(GrapherStatsMsg const & statsMsg)
    {
        try
        {
            std::stringstream stats;
            stats << statsMsg;
            LOG_DEBUG("[GrapherRegistryClient] Sending Stats Message: {}", stats.str());
            handle_grapher_stats_msg.on(reg_service_ph)(statsMsg);
        }
        catch(tl::exception const &)
        {
            LOG_ERROR("[GrapherRegisterClient] Failed Sending Stats Message.");
        }
    }

    ~GrapherRegistryClient()
    {
        LOG_DEBUG("[GrapherRegistryClient] Destructor called. Cleaning up resources...");
        register_grapher.deregister();
        unregister_grapher.deregister();
        handle_grapher_stats_msg.deregister();
    }

private:
    std::string reg_service_addr;     // na address of Keeper Registry Service 
    uint16_t reg_service_provider_id;          // KeeperRegistryService provider id
    tl::provider_handle reg_service_ph;  //provider_handle for remote registry service
    tl::remote_procedure register_grapher;
    tl::remote_procedure unregister_grapher;
    tl::remote_procedure handle_grapher_stats_msg;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    GrapherRegistryClient(tl::engine &tl_engine, std::string const &registry_addr, uint16_t registry_provider_id)
            : reg_service_addr(registry_addr), reg_service_provider_id(registry_provider_id), reg_service_ph(
            tl_engine.lookup(registry_addr), registry_provider_id)
    {
        LOG_DEBUG("[GrapherRegistryClient] Initialized for RegistryService at {} with ProviderID={}", registry_addr
             , registry_provider_id);
        register_grapher = tl_engine.define("register_grapher");
        unregister_grapher = tl_engine.define("unregister_grapher");
        handle_grapher_stats_msg = tl_engine.define("handle_grapher_stats_msg").disable_response();
    }
};
}

#endif
