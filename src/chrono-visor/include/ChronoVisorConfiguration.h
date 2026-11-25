#ifndef CHRONO_VISOR_CONFIGURATION_H
#define CHRONO_VISOR_CONFIGURATION_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <json-c/json.h>
#include <sstream>
#include <string>

#include <ConfigurationBlocks.h>

namespace chronolog
{

struct VisorConfiguration
{
    RpcTimeoutPolicy RPC_POLICY;
    RPCProviderConf VISOR_CLIENT_PORTAL_SERVICE_CONF;
    RPCProviderConf VISOR_KEEPER_REGISTRY_SERVICE_CONF;
    LogConf VISOR_LOG_CONF;
    size_t DELAYED_DATA_ADMIN_EXIT_IN_SECS{};
    // Slots of the visor's one RPC runtime, each with its own worker thread:
    // client requests and the visor's calls to graphers, keepers and players
    // all hold one while they run. 1-255.
    size_t RPC_SLOTS = 32;
    // SQLite catalog file. A relative path is resolved against the visor's
    // working directory.
    std::string CATALOG_DB_PATH = "visor/catalog.sqlite";

    VisorConfiguration()
    {
        /* Visor-related configurations */
        VISOR_CLIENT_PORTAL_SERVICE_CONF.PROTO_CONF = "ofi+tcp";
        VISOR_CLIENT_PORTAL_SERVICE_CONF.IP = "127.0.0.1";
        VISOR_CLIENT_PORTAL_SERVICE_CONF.BASE_PORT = 5555;
        VISOR_CLIENT_PORTAL_SERVICE_CONF.SERVICE_PROVIDER_ID = 55;

        VISOR_KEEPER_REGISTRY_SERVICE_CONF.PROTO_CONF = "ofi+tcp";
        VISOR_KEEPER_REGISTRY_SERVICE_CONF.IP = "127.0.0.1";
        VISOR_KEEPER_REGISTRY_SERVICE_CONF.BASE_PORT = 8888;
        VISOR_KEEPER_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID = 88;

        DELAYED_DATA_ADMIN_EXIT_IN_SECS = 3;
    }

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[VISOR_CLIENT_PORTAL_SERVICE_CONF: " + VISOR_CLIENT_PORTAL_SERVICE_CONF.to_String() +
               ", VISOR_KEEPER_REGISTRY_SERVICE_CONF: " + VISOR_KEEPER_REGISTRY_SERVICE_CONF.to_String() +
               ", VISOR_LOG: " + VISOR_LOG_CONF.to_String() +
               ", DELAYED_DATA_ADMIN_EXIT_IN_SECS: " + std::to_string(DELAYED_DATA_ADMIN_EXIT_IN_SECS) +
               ", RPC_SLOTS: " + std::to_string(RPC_SLOTS) + ", CATALOG_DB_PATH: " + CATALOG_DB_PATH + "]";
    }
};

} // namespace chronolog

#endif //CHRONOVISOR_CONFIGURATION_H
