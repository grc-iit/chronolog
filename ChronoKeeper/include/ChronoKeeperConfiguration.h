#ifndef CHRONO_KEEPER_CONFIGURATION_H
#define CHRONO_KEEPER_CONFIGURATION_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unordered_map>
#include <json-c/json.h>
#include <spdlog/common.h>

#include "chronolog_errcode.h"

#include <ConfigurationBlocks.h>

namespace chronolog
{
struct KeeperConfiguration
{
    uint32_t RECORDING_GROUP;
    RPCProviderConf KEEPER_RECORDING_SERVICE_CONF;
    RPCProviderConf DATA_STORE_ADMIN_SERVICE_CONF;
    RPCProviderConf VISOR_REGISTRY_SERVICE_CONF;
    RPCProviderConf KEEPER_GRAPHER_DRAIN_SERVICE_CONF;
    DataStoreConf DATA_STORE_CONF{};
    ExtractorReaderConf EXTRACTOR_CONF;
    LogConf LOG_CONF;

    KeeperConfiguration()
    {
        RECORDING_GROUP = 0;
        KEEPER_GRAPHER_DRAIN_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_GRAPHER_DRAIN_SERVICE_CONF.IP = "127.0.0.1";
        KEEPER_GRAPHER_DRAIN_SERVICE_CONF.BASE_PORT = 9999;
        KEEPER_GRAPHER_DRAIN_SERVICE_CONF.SERVICE_PROVIDER_ID = 99;

        DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        DATA_STORE_ADMIN_SERVICE_CONF.IP = "127.0.0.1";
        DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT = 4444;
        DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID = 44;

        VISOR_REGISTRY_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_REGISTRY_SERVICE_CONF.IP = "127.0.0.1";
        VISOR_REGISTRY_SERVICE_CONF.BASE_PORT = 8888;
        VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID = 88;

        DATA_STORE_CONF.max_story_chunk_size = 4096;
        DATA_STORE_CONF.story_chunk_duration_secs = 30;
        DATA_STORE_CONF.acceptance_window_secs = 10;
        DATA_STORE_CONF.inactive_story_delay_secs = 180;

        EXTRACTOR_CONF.story_files_dir = "/tmp/";
    }

    int parseJsonConf(json_object*);
    [[nodiscard]] std::string to_String() const
    {
        return "[CHRONO_GRAPHER_CONFIGURATION: RECORDING_GROUP: " + std::to_string(RECORDING_GROUP) +
               ", KEEPER_GRAPHER_DRAIN_SERVICE_CONF: " + KEEPER_GRAPHER_DRAIN_SERVICE_CONF.to_String() +
               ", DATA_STORE_ADMIN_SERVICE_CONF: " + DATA_STORE_ADMIN_SERVICE_CONF.to_String() +
               ", VISOR_REGISTRY_SERVICE_CONF: " + VISOR_REGISTRY_SERVICE_CONF.to_String() +
               ", LOG_CONF: " + LOG_CONF.to_String() + ", DATA_STORE_CONF: " + DATA_STORE_CONF.to_String() +
               ", EXTRACTOR_CONF: " + EXTRACTOR_CONF.to_String() + "]";
    }
};

} // namespace chronolog

#endif 
