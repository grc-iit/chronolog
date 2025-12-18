#ifndef CHRONO_VISOR_CONFIGURATION_H
#define CHRONO_VISOR_CONFIGURATION_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cassert>
#include <unordered_map>
#include <json-c/json.h>
#include <sstream>
#include <spdlog/common.h>

#include <ConfigurationManager.h>

namespace chronolog
{

struct VisorConfiguration
{
    RPCProviderConf VISOR_CLIENT_PORTAL_SERVICE_CONF;
    RPCProviderConf VISOR_KEEPER_REGISTRY_SERVICE_CONF;
    LogConf VISOR_LOG_CONF;
    size_t DELAYED_DATA_ADMIN_EXIT_IN_SECS{};

    VisorConfiguration()
    {
        /* Visor-related configurations */
        VISOR_CLIENT_PORTAL_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_CLIENT_PORTAL_SERVICE_CONF.IP = "127.0.0.1";
        VISOR_CLIENT_PORTAL_SERVICE_CONF.BASE_PORT = 5555;
        VISOR_CLIENT_PORTAL_SERVICE_CONF.SERVICE_PROVIDER_ID = 55;

        VISOR_KEEPER_REGISTRY_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
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
               ", DELAYED_DATA_ADMIN_EXIT_IN_SECS: " + std::to_string(DELAYED_DATA_ADMIN_EXIT_IN_SECS) + "]";
    }
};

} // namespace chronolog

#endif //CHRONOVISOR_CONFIGURATION_H
