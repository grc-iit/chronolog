#ifndef CHRONO_PLAYER_CONFIGURATION_H
#define CHRONO_PLAYER_CONFIGURATION_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <json-c/json.h>

#include <ConfigurationBlocks.h>

namespace chronolog
{

struct PlaybackServiceConf
{
    RPCProviderConf RPC_CONF;
    uint32_t INGESTION_THREAD_COUNT = 1;
};

struct PlayerConfiguration
{
    uint32_t RECORDING_GROUP;
    RPCProviderConf DATA_STORE_ADMIN_SERVICE_CONF;
    PlaybackServiceConf PLAYBACK_SERVICE_CONF;
    RPCProviderConf VISOR_REGISTRY_SERVICE_CONF;
    LogConf LOG_CONF;
    DataStoreConf DATA_STORE_CONF{};
    ExtractorReaderConf READER_CONF;

    PlayerConfiguration()
    {
        RECORDING_GROUP = 0;
        DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        DATA_STORE_ADMIN_SERVICE_CONF.IP = "127.0.0.1";
        DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT = 2222;
        DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID = 22;

        PLAYBACK_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        PLAYBACK_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        PLAYBACK_SERVICE_CONF.RPC_CONF.BASE_PORT = 2225;
        PLAYBACK_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 25;

        VISOR_REGISTRY_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_REGISTRY_SERVICE_CONF.IP = "127.0.0.1";
        VISOR_REGISTRY_SERVICE_CONF.BASE_PORT = 8888;
        VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID = 88;

        DATA_STORE_CONF.max_story_chunk_size = 4096;
        DATA_STORE_CONF.story_chunk_duration_secs = 60;
        DATA_STORE_CONF.acceptance_window_secs = 180;
        DATA_STORE_CONF.inactive_story_delay_secs = 300;

        READER_CONF.story_files_dir = "/tmp/";
    }
    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[CHRONO_PLAYER_CONFIGURATION: RECORDING_GROUP: " + std::to_string(RECORDING_GROUP) +
               ", DATA_STORE_ADMIN_SERVICE_CONF: " + DATA_STORE_ADMIN_SERVICE_CONF.to_String() +
               ", PLAYBACK_SERVICE_CONF: " + PLAYBACK_SERVICE_CONF.RPC_CONF.to_String() +
               ", VISOR_REGISTRY_SERVICE_CONF: " + VISOR_REGISTRY_SERVICE_CONF.to_String() +
               ", LOG_CONF: " + LOG_CONF.to_String() + ", DATA_STORE_CONF: " + DATA_STORE_CONF.to_String() +
               ", READER_CONF: " + READER_CONF.to_String() + "]";
    }
};

} // namespace chronolog

#endif
