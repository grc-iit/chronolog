#ifndef CHRONO_KEEPER_CONFIGURATION_H
#define CHRONO_KEEPER_CONFIGURATION_H

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <map>
#include <string>
#include <json-c/json.h>

#include "chronolog_errcode.h"

#include <ConfigurationBlocks.h>
#include <ExtractionModuleConfiguration.h>

namespace chronolog
{
struct KeeperRecordingServiceConf
{
    RPCProviderConf RPC_CONF;
    uint32_t INGESTION_THREAD_COUNT = 1;

    [[nodiscard]] std::string to_String() const
    {
        return "[RPC_CONF: " + RPC_CONF.to_String() +
               ", INGESTION_THREAD_COUNT: " + std::to_string(INGESTION_THREAD_COUNT) + "]";
    }
};

// The keeper's write-ahead log, read from the "WAL" object of the keeper section.
// A missing object leaves the defaults, which keep the WAL off. parseJsonConf
// refuses a block the WAL could not run with, so a bad value stops the keeper
// at startup instead of failing a durable write later.
struct WalConf
{
    bool enabled = false;
    std::string dir;                   // absolute when enabled
    uint64_t segment_bytes = 67108864; // >= 1 MiB
    uint64_t max_bytes = 1073741824;
    uint64_t group_commit_max_bytes = 4194304;
    uint32_t group_commit_window_us = 0; // <= 10000
    uint64_t reserve_min_bytes = 268435456;
    uint32_t reserve_percent = 10; // 0..100
    uint64_t durable_cap_mb = 4096;

    static constexpr uint64_t MIN_SEGMENT_BYTES = 1048576;
    static constexpr uint32_t MAX_GROUP_COMMIT_WINDOW_US = 10000;

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[WAL_CONF: enabled: " + std::string(enabled ? "true" : "false") + ", dir: " + dir +
               ", segment_bytes: " + std::to_string(segment_bytes) + ", max_bytes: " + std::to_string(max_bytes) +
               ", group_commit_max_bytes: " + std::to_string(group_commit_max_bytes) +
               ", group_commit_window_us: " + std::to_string(group_commit_window_us) +
               ", reserve_min_bytes: " + std::to_string(reserve_min_bytes) +
               ", reserve_percent: " + std::to_string(reserve_percent) +
               ", durable_cap_mb: " + std::to_string(durable_cap_mb) + "]";
    }
};

struct KeeperConfiguration
{
    uint32_t RECORDING_GROUP;
    KeeperRecordingServiceConf KEEPER_RECORDING_SERVICE_CONF;
    RPCProviderConf DATA_STORE_ADMIN_SERVICE_CONF;
    RPCProviderConf VISOR_REGISTRY_SERVICE_CONF;
    RPCProviderConf KEEPER_GRAPHER_DRAIN_SERVICE_CONF;
    DataStoreConf DATA_STORE_CONF{};
    LogConf LOG_CONF;

    ExtractionModuleConfiguration EXTRACTION_MODULE_CONF;
    WalConf WAL_CONF;

    KeeperConfiguration()
    {
        RECORDING_GROUP = 0;

        KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT = 6666;
        KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 66;
        KEEPER_RECORDING_SERVICE_CONF.INGESTION_THREAD_COUNT = 4;

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

        EXTRACTION_MODULE_CONF.extraction_stream_count = 1;
    }

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        std::string a_string = "[CHRONO_KEEPER_CONFIGURATION: RECORDING_GROUP: " + std::to_string(RECORDING_GROUP) +
                               ", KEEPER_RECORDING_SERVICE_CONF: " + KEEPER_RECORDING_SERVICE_CONF.to_String() +
                               ", KEEPER_GRAPHER_DRAIN_SERVICE_CONF: " + KEEPER_GRAPHER_DRAIN_SERVICE_CONF.to_String() +
                               ", DATA_STORE_ADMIN_SERVICE_CONF: " + DATA_STORE_ADMIN_SERVICE_CONF.to_String() +
                               ", VISOR_REGISTRY_SERVICE_CONF: " + VISOR_REGISTRY_SERVICE_CONF.to_String() +
                               ", LOG_CONF: " + LOG_CONF.to_String() +
                               ", DATA_STORE_CONF: " + DATA_STORE_CONF.to_String() +
                               ", WAL_CONF: " + WAL_CONF.to_String() + ", ";

        EXTRACTION_MODULE_CONF.to_string(a_string);
        a_string += "]";
        return a_string;
    }
};

} // namespace chronolog

#endif
