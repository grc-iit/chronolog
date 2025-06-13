#ifndef CHRONOLOG_CONFIGURATIONMANAGER_H
#define CHRONOLOG_CONFIGURATIONMANAGER_H

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cassert>
#include <unordered_map>
#include <json-c/json.h>
#include <sstream>
#include <spdlog/common.h>
#include "enum.h"
#include "chronolog_errcode.h"

namespace ChronoLog
{
typedef struct ClockConf_
{
    ClocksourceType CLOCKSOURCE_TYPE;
    uint64_t DRIFT_CAL_SLEEP_SEC;
    uint64_t DRIFT_CAL_SLEEP_NSEC;

    [[nodiscard]] std::string to_String() const
    {
        return "CLOCKSOURCE_TYPE: " + std::string(getClocksourceTypeString(CLOCKSOURCE_TYPE)) +
               ", DRIFT_CAL_SLEEP_SEC: " + std::to_string(DRIFT_CAL_SLEEP_SEC) + ", DRIFT_CAL_SLEEP_NSEC: " +
               std::to_string(DRIFT_CAL_SLEEP_NSEC);
    }
} ClockConf;

typedef struct AuthConf_
{
    std::string AUTH_TYPE;
    std::string MODULE_PATH;

    [[nodiscard]] std::string to_String() const
    {
        return "AUTH_TYPE: " + AUTH_TYPE + ", MODULE_PATH: " + MODULE_PATH;
    }
} AuthConf;

typedef struct RPCProviderConf_
{
    std::string PROTO_CONF;
    std::string IP;
    uint16_t BASE_PORT{};
    uint16_t SERVICE_PROVIDER_ID{};

    [[nodiscard]] std::string to_String() const
    {
        return "[PROTO_CONF: " +
               PROTO_CONF + ", IP: " + IP + ", BASE_PORT: " + std::to_string(BASE_PORT) + ", SERVICE_PROVIDER_ID: " +
               std::to_string(SERVICE_PROVIDER_ID) + ", PORTS: " + "]";
    }
} RPCProviderConf;

typedef struct LogConf_
{
    std::string LOGTYPE;
    std::string LOGFILE;
    spdlog::level::level_enum LOGLEVEL{};
    std::string LOGNAME;
    size_t LOGFILESIZE{};
    size_t LOGFILENUM{};
    spdlog::level::level_enum FLUSHLEVEL{};

    // Helper function to convert spdlog::level::level_enum to string
    static std::string LevelToString(spdlog::level::level_enum level)
    {
        switch(level)
        {
            case spdlog::level::trace:
                return "TRACE";
            case spdlog::level::debug:
                return "DEBUG";
            case spdlog::level::info:
                return "INFO";
            case spdlog::level::warn:
                return "WARN";
            case spdlog::level::err:
                return "ERROR";
            case spdlog::level::critical:
                return "CRITICAL";
            case spdlog::level::off:
                return "OFF";
            default:
                return "UNKNOWN";
        }
    }

    [[nodiscard]] std::string to_String() const
    {
        return "[TYPE: " + LOGTYPE + ", FILE: " + LOGFILE + ", LEVEL: " + LevelToString(LOGLEVEL) + ", NAME: " +
               LOGNAME + ", LOGFILESIZE: " + std::to_string(LOGFILESIZE) + ", LOGFILENUM: " +
               std::to_string(LOGFILENUM) + ", FLUSH LEVEL: " + LevelToString(FLUSHLEVEL) + "]";
    }
} LogConf;

typedef struct VisorClientPortalServiceConf_
{
    RPCProviderConf RPC_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[RPC_CONF: " + RPC_CONF.to_String() + "]";
    }
} VisorClientPortalServiceConf;

typedef struct VisorKeeperRegistryServiceConf_
{
    RPCProviderConf RPC_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[RPC_CONF: " + RPC_CONF.to_String() + "]";
    }
} VisorKeeperRegistryServiceConf;

typedef struct KeeperRecordingServiceConf_
{
    RPCProviderConf RPC_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[RPC_CONF: " + RPC_CONF.to_String() + "]";
    }
} KeeperRecordingServiceConf;

typedef struct KeeperDataStoreAdminServiceConf_
{
    RPCProviderConf RPC_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[RPC_CONF: " + RPC_CONF.to_String() + "]";
    }
} KeeperDataStoreAdminServiceConf;

typedef struct KeeperGrapherDrainServiceConf_
{
    RPCProviderConf RPC_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[RPC_CONF: " + RPC_CONF.to_String() + "]";
    }
} KeeperGrapherDrainServiceConf;

typedef struct DataStoreConf_
{
    int max_story_chunk_size;

    [[nodiscard]] std::string to_String() const
    {
        return  "[DATA_STORE_CONF: max_story_chunk_size: " + std::to_string(max_story_chunk_size) +
                "]";
    }
} DataStoreConf;

typedef struct ArchiveExtractorReaderConf_
{
    std::string story_files_dir;

    [[nodiscard]] std::string to_String() const
    {
        return  "[EXTRACTOR_READER_CONF: STORY_FILES_DIR: " + story_files_dir +
                "]";
    }
} ExtractorReaderConf;

typedef struct VisorConf_
{
    VisorClientPortalServiceConf VISOR_CLIENT_PORTAL_SERVICE_CONF;
    VisorKeeperRegistryServiceConf VISOR_KEEPER_REGISTRY_SERVICE_CONF;
    LogConf VISOR_LOG_CONF;
    size_t DELAYED_DATA_ADMIN_EXIT_IN_SECS{};

    [[nodiscard]] std::string to_String() const
    {
        return "[VISOR_CLIENT_PORTAL_SERVICE_CONF: " + VISOR_CLIENT_PORTAL_SERVICE_CONF.to_String() +
               ", VISOR_KEEPER_REGISTRY_SERVICE_CONF: " + VISOR_KEEPER_REGISTRY_SERVICE_CONF.to_String() +
               ", VISOR_LOG: " + VISOR_LOG_CONF.to_String() + ", DELAYED_DATA_ADMIN_EXIT_IN_SECS: " +
               std::to_string(DELAYED_DATA_ADMIN_EXIT_IN_SECS) + "]";
    }
} VisorConf;

typedef struct KeeperConf_
{
    uint32_t RECORDING_GROUP;
    KeeperRecordingServiceConf KEEPER_RECORDING_SERVICE_CONF;
    KeeperDataStoreAdminServiceConf KEEPER_DATA_STORE_ADMIN_SERVICE_CONF;
    VisorKeeperRegistryServiceConf VISOR_KEEPER_REGISTRY_SERVICE_CONF;
    KeeperGrapherDrainServiceConf KEEPER_GRAPHER_DRAIN_SERVICE_CONF;
    DataStoreConf DATA_STORE_CONF{};
    ExtractorReaderConf EXTRACTOR_CONF;
    LogConf KEEPER_LOG_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[CHRONO_KEEPER_CONFIGURATION: RECORDING_GROUP: "+ std::to_string(RECORDING_GROUP) +
               ", KEEPER_RECORDING_SERVICE_CONF: " + KEEPER_RECORDING_SERVICE_CONF.to_String() +
               ", KEEPER_DATA_STORE_ADMIN_SERVICE_CONF: " + KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.to_String() +
               ", VISOR_KEEPER_REGISTRY_SERVICE_CONF: " + VISOR_KEEPER_REGISTRY_SERVICE_CONF.to_String() +
               ", KEEPER_GRAPHER_DRAIN_SERVICE_CONF: " + KEEPER_GRAPHER_DRAIN_SERVICE_CONF.to_String() +
               ", DATA_STORE_CONF: " + DATA_STORE_CONF.to_String() +
               ", EXTRACTOR_CONF: " + EXTRACTOR_CONF.to_String() +
               ", KEEPER_LOG_CONF: " + KEEPER_LOG_CONF.to_String() + "]";
    }
} KeeperConf;

typedef struct GrapherConf_
{
    uint32_t RECORDING_GROUP{};
    RPCProviderConf KEEPER_GRAPHER_DRAIN_SERVICE_CONF;
    RPCProviderConf DATA_STORE_ADMIN_SERVICE_CONF;
    RPCProviderConf VISOR_REGISTRY_SERVICE_CONF;
    LogConf LOG_CONF;
    DataStoreConf DATA_STORE_CONF{};
    ExtractorReaderConf EXTRACTOR_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[CHRONO_GRAPHER_CONFIGURATION: RECORDING_GROUP: "+ std::to_string(RECORDING_GROUP) +
               ", KEEPER_GRAPHER_DRAIN_SERVICE_CONF: " + KEEPER_GRAPHER_DRAIN_SERVICE_CONF.to_String() +
               ", DATA_STORE_ADMIN_SERVICE_CONF: " + DATA_STORE_ADMIN_SERVICE_CONF.to_String() +
               ", VISOR_REGISTRY_SERVICE_CONF: " + VISOR_REGISTRY_SERVICE_CONF.to_String() +
               ", LOG_CONF: " + LOG_CONF.to_String() +
               ", DATA_STORE_CONF: " + DATA_STORE_CONF.to_String() +
               ", EXTRACTOR_CONF: " + EXTRACTOR_CONF.to_String() +
               "]";
    }
} GrapherConf;

typedef struct DataStoreAdminServiceConf_
{
    RPCProviderConf RPC_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[RPC_CONF: " + RPC_CONF.to_String() + "]";
    }
} DataStoreAdminServiceConf;


typedef struct PlayerConf_
{
    uint32_t RECORDING_GROUP;
    RPCProviderConf DATA_STORE_ADMIN_SERVICE_CONF;
    RPCProviderConf PLAYBACK_SERVICE_CONF;
    RPCProviderConf VISOR_REGISTRY_SERVICE_CONF;
    LogConf LOG_CONF;
    DataStoreConf DATA_STORE_CONF{};
    ExtractorReaderConf READER_CONF;

    PlayerConf_()
    {
        /* Grapher-related configurations */
        RECORDING_GROUP = 0;
        DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        DATA_STORE_ADMIN_SERVICE_CONF.IP = "127.0.0.1";
        DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT = 2222;
        DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID = 22;

        PLAYBACK_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        PLAYBACK_SERVICE_CONF.IP = "127.0.0.1";
        PLAYBACK_SERVICE_CONF.BASE_PORT = 2225;
        PLAYBACK_SERVICE_CONF.SERVICE_PROVIDER_ID = 25;

        VISOR_REGISTRY_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_REGISTRY_SERVICE_CONF.IP = "127.0.0.1";
        VISOR_REGISTRY_SERVICE_CONF.BASE_PORT = 8888;
        VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID = 88;
    }

    [[nodiscard]] std::string to_String() const
    {
        return "[CHRONO_PLAYER_CONFIGURATION: RECORDING_GROUP: " + std::to_string(RECORDING_GROUP) +
               ", DATA_STORE_ADMIN_SERVICE_CONF: " + DATA_STORE_ADMIN_SERVICE_CONF.to_String() +
               ", PLAYBACK_SERVICE_CONF: " + PLAYBACK_SERVICE_CONF.to_String() +
               ", VISOR_REGISTRY_SERVICE_CONF: " + VISOR_REGISTRY_SERVICE_CONF.to_String() +
               ", LOG_CONF: " + LOG_CONF.to_String() +
               ", DATA_STORE_CONF: " + DATA_STORE_CONF.to_String() +
               ", READER_CONF: " + READER_CONF.to_String() +
               "]";
    }
} PlayerConf;

typedef struct ClientConf_
{
    RPCProviderConf CLIENT_QUERY_SERVICE_CONF;
    VisorClientPortalServiceConf VISOR_CLIENT_PORTAL_SERVICE_CONF;
    LogConf CLIENT_LOG_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[CLIENT_QUERY_SERVICE_CONF: " + CLIENT_QUERY_SERVICE_CONF.to_String() +
               ", [VISOR_CLIENT_PORTAL_SERVICE_CONF: " + VISOR_CLIENT_PORTAL_SERVICE_CONF.to_String() +
               ", CLIENT_LOG_CONF:" + CLIENT_LOG_CONF.to_String() + "]";
    }
} ClientConf;

class ConfigurationManager
{
public:
    ChronoLogServiceRole ROLE{};
    ClockConf CLOCK_CONF{};
    ClocksourceType CLOCKSOURCE_TYPE{};
    AuthConf AUTH_CONF{};
    VisorConf VISOR_CONF{};
    ClientConf CLIENT_CONF{};
    KeeperConf KEEPER_CONF{};
    GrapherConf GRAPHER_CONF{};
    PlayerConf PLAYER_CONF{};

    ConfigurationManager()
    {
        std::cout << "[ConfigurationManager] Initializing configuration with default settings." << std::endl;
        ROLE = CHRONOLOG_UNKNOWN;

        /* Clock-related configurations */
        CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;
        CLOCK_CONF.DRIFT_CAL_SLEEP_SEC = 10;
        CLOCK_CONF.DRIFT_CAL_SLEEP_NSEC = 0;
        CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;

        /* Authentication-related configurations */
        AUTH_CONF.AUTH_TYPE = "RBAC";
        AUTH_CONF.MODULE_PATH = "";

        /* Visor-related configurations */
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT = 5555;
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 55;

        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.BASE_PORT = 8888;
        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 88;

        VISOR_CONF.DELAYED_DATA_ADMIN_EXIT_IN_SECS = 3;

        /* Keeper-related configurations */
        KEEPER_CONF.RECORDING_GROUP = 0;
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT = 6666;
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 66;

        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.BASE_PORT = 7777;
        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 77;

        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.BASE_PORT = 8888;
        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 88;

        KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.BASE_PORT = 3333;
        KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 33;

        KEEPER_CONF.DATA_STORE_CONF.max_story_chunk_size = 4096;

        KEEPER_CONF.EXTRACTOR_CONF.story_files_dir = "/tmp/";

        /* Grapher-related configurations */
        GRAPHER_CONF.RECORDING_GROUP = 0;
        GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.IP = "127.0.0.1";
        GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.BASE_PORT = 9999;
        GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.SERVICE_PROVIDER_ID = 99;

        GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.IP = "127.0.0.1";
        GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT = 4444;
        GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID = 44;

        GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF.IP = "127.0.0.1";
        GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF.BASE_PORT = 8888;
        GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID = 88;

        GRAPHER_CONF.DATA_STORE_CONF.max_story_chunk_size = 4096;

        GRAPHER_CONF.EXTRACTOR_CONF.story_files_dir = "/tmp/";

        /* Player-related configurations */
        PLAYER_CONF.RECORDING_GROUP = 0;
        PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.IP = "127.0.0.1";
        PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT = 2222;
        PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID = 22;

        PLAYER_CONF.PLAYBACK_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        PLAYER_CONF.PLAYBACK_SERVICE_CONF.IP = "127.0.0.1";
        PLAYER_CONF.PLAYBACK_SERVICE_CONF.BASE_PORT = 2225;
        PLAYER_CONF.PLAYBACK_SERVICE_CONF.SERVICE_PROVIDER_ID = 25;

        PLAYER_CONF.VISOR_REGISTRY_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        PLAYER_CONF.VISOR_REGISTRY_SERVICE_CONF.IP = "127.0.0.1";
        PLAYER_CONF.VISOR_REGISTRY_SERVICE_CONF.BASE_PORT = 8888;
        PLAYER_CONF.VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID = 88;

        PLAYER_CONF.DATA_STORE_CONF.max_story_chunk_size = 4096;

        PLAYER_CONF.READER_CONF.story_files_dir = "/tmp/";

        /* Client-related configurations */
        CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT = 5555;
        CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 55;

        CLIENT_CONF.CLIENT_QUERY_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        CLIENT_CONF.CLIENT_QUERY_SERVICE_CONF.IP = "127.0.0.1";
        CLIENT_CONF.CLIENT_QUERY_SERVICE_CONF.BASE_PORT = 5557;
        CLIENT_CONF.CLIENT_QUERY_SERVICE_CONF.SERVICE_PROVIDER_ID = 57;

        PrintConf();
    }

    explicit ConfigurationManager(const std::string &conf_file_path)
    {
        std::cout << "[ConfigurationManager] Loading configuration from file: " << conf_file_path.c_str() << std::endl;
        LoadConfFromJSONFile(conf_file_path);
    }

    void PrintConf() const
    {
        std::cout << "******** Start of configuration output ********" << std::endl;
        std::cout << "AUTH_CONF: " << AUTH_CONF.to_String().c_str() << std::endl;
        std::cout << "VISOR_CONF: " << VISOR_CONF.to_String().c_str() << std::endl;
        std::cout << "KEEPER_CONF: " << KEEPER_CONF.to_String().c_str() << std::endl;
        std::cout << "GRAPHER_CONF: " << GRAPHER_CONF.to_String().c_str() << std::endl;
        std::cout << "PLAYER_CONF: " << PLAYER_CONF.to_String().c_str() << std::endl;
        std::cout << "CLIENT_CONF: " << CLIENT_CONF.to_String().c_str() << std::endl;
        std::cout << "******** End of configuration output ********" << std::endl;
    }

    void LoadConfFromJSONFile(const std::string &conf_file_path)
    {
        json_object*root = json_object_from_file(conf_file_path.c_str());
        if(root == nullptr)
        {
            std::cerr << "[ConfigurationManager] Failed to open configuration file at path: " << conf_file_path.c_str()
                      << ". Exiting..." << std::endl;
            exit(chronolog::CL_ERR_NOT_EXIST);
        }

        json_object_object_foreach(root, key, val)
        {
            if(strcmp(key, "clock") == 0)
            {
                json_object*clock_conf = json_object_object_get(root, "clock");
                if(clock_conf == nullptr || !json_object_is_type(clock_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str() << ". Clock configuration is not found or is not an object."
                              << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                parseClockConf(clock_conf);
            }
            else if(strcmp(key, "authentication") == 0)
            {
                json_object*auth_conf = json_object_object_get(root, "authentication");
                if(auth_conf == nullptr || !json_object_is_type(auth_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str()
                              << ". Authentication configuration is not found or is not an object." << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                parseAuthConf(auth_conf);
            }
            else if(strcmp(key, "chrono_visor") == 0)
            {
                json_object*chrono_visor_conf = json_object_object_get(root, "chrono_visor");
                if(chrono_visor_conf == nullptr || !json_object_is_type(chrono_visor_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str()
                              << ". ChronoVisor configuration is not found or is not an object." << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                parseVisorConf(chrono_visor_conf);
            }
            else if(strcmp(key, "chrono_keeper") == 0)
            {
                json_object*chrono_keeper_conf = json_object_object_get(root, "chrono_keeper");
                if(chrono_keeper_conf == nullptr || !json_object_is_type(chrono_keeper_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str()
                              << ". ChronoKeeper configuration is not found or is not an object." << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                parseKeeperConf(chrono_keeper_conf);
            }
            else if(strcmp(key, "chrono_grapher") == 0)
            {
                json_object*chrono_grapher_conf = json_object_object_get(root, "chrono_grapher");
                if(chrono_grapher_conf == nullptr || !json_object_is_type(chrono_grapher_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str()
                              << ". ChronoGrapher configuration is not found or is not an object." << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                parseGrapherConf(chrono_grapher_conf);
            }
            else if(strcmp(key, "chrono_player") == 0)
            {
                json_object*chrono_player_conf = json_object_object_get(root, "chrono_player");
                if(chrono_player_conf == nullptr || !json_object_is_type(chrono_player_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str()
                              << ". ChronoPlayer configuration is not found or is not an object." << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                parsePlayerConf(chrono_player_conf);
            }
            else if(strcmp(key, "chrono_client") == 0)
            {
                json_object*chrono_client_conf = json_object_object_get(root, "chrono_client");
                if(chrono_client_conf == nullptr || !json_object_is_type(chrono_client_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str()
                              << ". ChronoClient configuration is not found or is not an object." << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                parseClientConf(chrono_client_conf);
            }
            else
            {
                std::cerr << "[ConfigurationManager] Unknown configuration item: " << key << std::endl;
            }
        }
        json_object_put(root);
        PrintConf();
    }

private:
  /*  void parseRPCImplConf(json_object*json_conf, ChronoLogRPCImplementation &rpc_impl)
    {
        if(json_object_is_type(json_conf, json_type_string))
        {
            const char*conf_str = json_object_get_string(json_conf);
            if(strcmp(conf_str, "Thallium_sockets") == 0)
            {
                rpc_impl = CHRONOLOG_THALLIUM_SOCKETS;
            }
            else if(strcmp(conf_str, "Thallium_tcp") == 0)
            {
                rpc_impl = CHRONOLOG_THALLIUM_TCP;
            }
            else if(strcmp(conf_str, "Thallium_roce") == 0)
            {
                rpc_impl = CHRONOLOG_THALLIUM_ROCE;
            }
            else
            {
                std::cout << "[ConfigurationManager] Unknown rpc implementation: " << conf_str << std::endl;
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager] Invalid rpc implementation configuration" << std::endl;
        }
    }
*/
    void parselogLevelConf(json_object*json_conf, spdlog::level::level_enum &log_level)
    {
        if(json_object_is_type(json_conf, json_type_string))
        {
            const char*conf_str = json_object_get_string(json_conf);
            if(strcmp(conf_str, "trace") == 0)
            {
                log_level = spdlog::level::trace;
            }
            else if(strcmp(conf_str, "info") == 0)
            {
                log_level = spdlog::level::info;
            }
            else if(strcmp(conf_str, "debug") == 0)
            {
                log_level = spdlog::level::debug;
            }
            else if(strcmp(conf_str, "warning") == 0)
            {
                log_level = spdlog::level::warn;
            }
            else if(strcmp(conf_str, "error") == 0)
            {
                log_level = spdlog::level::err;
            }
            else if(strcmp(conf_str, "critical") == 0)
            {
                log_level = spdlog::level::critical;
            }
            else if(strcmp(conf_str, "off") == 0)
            {
                log_level = spdlog::level::off;
            }
            else
            {
                std::cout << "[ConfigurationManager] Unknown log level: " << conf_str << std::endl;
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager] Invalid Log Level implementation configuration" << std::endl;
        }
    }

    void parseFlushLevelConf(json_object*json_conf, spdlog::level::level_enum &flush_level)
    {
        if(json_object_is_type(json_conf, json_type_string))
        {
            const char*conf_str = json_object_get_string(json_conf);
            if(strcmp(conf_str, "trace") == 0)
            {
                flush_level = spdlog::level::trace;
            }
            else if(strcmp(conf_str, "info") == 0)
            {
                flush_level = spdlog::level::info;
            }
            else if(strcmp(conf_str, "debug") == 0)
            {
                flush_level = spdlog::level::debug;
            }
            else if(strcmp(conf_str, "warning") == 0)
            {
                flush_level = spdlog::level::warn;
            }
            else if(strcmp(conf_str, "error") == 0)
            {
                flush_level = spdlog::level::err;
            }
            else if(strcmp(conf_str, "critical") == 0)
            {
                flush_level = spdlog::level::critical;
            }
            else if(strcmp(conf_str, "off") == 0)
            {
                flush_level = spdlog::level::off;
            }
            else
            {
                std::cout << "[ConfigurationManager] Unknown flush level: " << conf_str << "Set it to default value: "
                                                                                           "Warning" << std::endl;
                flush_level = spdlog::level::warn;
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager] Invalid Flush Level implementation configuration" << std::endl;
        }
    }

    void parseClockConf(json_object*clock_conf)
    {
        json_object_object_foreach(clock_conf, key, val)
        {
            if(strcmp(key, "clocksource_type") == 0)
            {
                if(json_object_is_type(val, json_type_string))
                {
                    const char*clocksource_type = json_object_get_string(val);
                    if(strcmp(clocksource_type, "C_STYLE") == 0)
                        CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;
                    else if(strcmp(clocksource_type, "CPP_STYLE") == 0)
                        CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::CPP_STYLE;
                    else if(strcmp(clocksource_type, "TSC") == 0)
                        CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::TSC;
                    else
                        std::cout << "[ConfigurationManager] Unknown clocksource type: " << clocksource_type
                                  << std::endl;
                }
                else
                {
                    std::cerr
                            << "[ConfigurationManager] Failed to parse configuration file: clocksource_type is not a string"
                            << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
            }
            else if(strcmp(key, "drift_cal_sleep_sec") == 0)
            {
                if(json_object_is_type(val, json_type_int))
                {
                    CLOCK_CONF.DRIFT_CAL_SLEEP_SEC = json_object_get_int(val);
                }
                else
                {
                    std::cerr
                            << "[ConfigurationManager] Failed to parse configuration file: drift_cal_sleep_sec is not an integer"
                            << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
            }
            else if(strcmp(key, "drift_cal_sleep_nsec") == 0)
            {
                if(json_object_is_type(val, json_type_int))
                {
                    CLOCK_CONF.DRIFT_CAL_SLEEP_NSEC = json_object_get_int(val);
                }
                else
                {
                    std::cerr
                            << "[ConfigurationManager] Failed to parse configuration file: drift_cal_sleep_nsec is not an integer"
                            << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
            }
        }
    }

    void parseAuthConf(json_object*auth_conf)
    {
        if(auth_conf == nullptr || !json_object_is_type(auth_conf, json_type_object))
        {
            std::cerr
                    << "[ConfigurationManager] Error while parsing configuration file. Authentication configuration is not found or is not an object."
                    << std::endl;
            exit(chronolog::CL_ERR_INVALID_CONF);
        }
        json_object_object_foreach(auth_conf, key, val)
        {
            if(strcmp(key, "auth_type") == 0)
            {
                if(json_object_is_type(val, json_type_string))
                {
                    AUTH_CONF.AUTH_TYPE = json_object_get_string(val);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] Failed to parse configuration file: auth_type is not a string"
                              << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
            }
            else if(strcmp(key, "module_location") == 0)
            {
                if(json_object_is_type(val, json_type_string))
                {
                    AUTH_CONF.MODULE_PATH = json_object_get_string(val);
                }
                else
                {
                    std::cerr
                            << "[ConfigurationManager] Failed to parse configuration file: module_location is not a string"
                            << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
            }
        }
    }

    void parseRPCProviderConf(json_object*json_conf, RPCProviderConf &rpc_provider_conf)
    {
        json_object_object_foreach(json_conf, key, val)
        {
            if(strcmp(key, "protocol_conf") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                rpc_provider_conf.PROTO_CONF = json_object_get_string(val);
            }
            else if(strcmp(key, "service_ip") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                rpc_provider_conf.IP = json_object_get_string(val);
            }
            else if(strcmp(key, "service_base_port") == 0)
            {
                assert(json_object_is_type(val, json_type_int));
                rpc_provider_conf.BASE_PORT = json_object_get_int(val);
            }
            else if(strcmp(key, "service_provider_id") == 0)
            {
                assert(json_object_is_type(val, json_type_int));
                rpc_provider_conf.SERVICE_PROVIDER_ID = json_object_get_int(val);
            }
            else
            {
                std::cerr << "[ConfigurationManager] Unknown client end configuration: " << key << std::endl;
            }
        }
    }

    void parseLogConf(json_object*json_conf, LogConf &log_conf)
    {
        json_object_object_foreach(json_conf, key, val)
        {
            if(strcmp(key, "type") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                log_conf.LOGTYPE = json_object_get_string(val);
            }
            else if(strcmp(key, "file") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                log_conf.LOGFILE = json_object_get_string(val);
            }
            else if(strcmp(key, "level") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                parselogLevelConf(val, log_conf.LOGLEVEL);
            }
            else if(strcmp(key, "name") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                log_conf.LOGNAME = json_object_get_string(val);
            }
            else if(strcmp(key, "filesize") == 0)
            {
                assert(json_object_is_type(val, json_type_int));
                log_conf.LOGFILESIZE = json_object_get_int(val);
            }
            else if(strcmp(key, "filenum") == 0)
            {
                assert(json_object_is_type(val, json_type_int));
                log_conf.LOGFILENUM = json_object_get_int(val);
            }
            else if(strcmp(key, "flushlevel") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                parseFlushLevelConf(val, log_conf.FLUSHLEVEL);
            }
            else
            {
                std::cerr << "[ConfigurationManager] Unknown log configuration: " << key << std::endl;
            }
        }
    }

    void parseVisorConf(json_object*json_conf)
    {
        json_object_object_foreach(json_conf, key, val)
        {
            if(strcmp(key, "VisorClientPortalService") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*visor_client_portal_service_conf = json_object_object_get(json_conf
                                                                                      , "VisorClientPortalService");
                json_object_object_foreach(visor_client_portal_service_conf, key, val)
                {
                    if(strcmp(key, "rpc") == 0)
                    {
                        parseRPCProviderConf(val, VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_visor] Unknown VisorClientPortalService "
                                     "configuration: " << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "VisorKeeperRegistryService") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*visor_keeper_registry_service_conf = json_object_object_get(json_conf
                                                                                        , "VisorKeeperRegistryService");
                json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
                {
                    if(strcmp(key, "rpc") == 0)
                    {
                        parseRPCProviderConf(val, VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_visor] Unknown VisorKeeperRegistryService "
                                     "configuration: " << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "Monitoring") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*chronovisor_log = json_object_object_get(json_conf, "Monitoring");
                json_object_object_foreach(chronovisor_log, key, val)
                {
                    if(strcmp(key, "monitor") == 0)
                    {
                        parseLogConf(val, VISOR_CONF.VISOR_LOG_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_visor] Unknown Monitoring configuration: "
                                  << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "delayed_data_admin_exit_in_secs") == 0)
            {
                assert(json_object_is_type(val, json_type_int));
                int delayed_exit_value = json_object_get_int(val);
                VISOR_CONF.DELAYED_DATA_ADMIN_EXIT_IN_SECS = ((0 < delayed_exit_value && delayed_exit_value < 60)
                                                              ? delayed_exit_value : 5);
            }
            else
            {
                std::cerr << "[ConfigurationManager] [chrono_visor] Unknown Visor configuration: " << key << std::endl;
            }
        }
    }

    void parseKeeperConf(json_object*json_conf)
    {
        json_object_object_foreach(json_conf, key, val)
        {
            if(strcmp(key, "RecordingGroup") == 0)
            {
                assert(json_object_is_type(val, json_type_int));
                int value = json_object_get_int(val);
                KEEPER_CONF.RECORDING_GROUP = (value >= 0 ? value : 0);
            }
            else if(strcmp(key, "KeeperRecordingService") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*keeper_recording_service_conf = json_object_object_get(json_conf, "KeeperRecordingService");
                json_object_object_foreach(keeper_recording_service_conf, key, val)
                {
                    if(strcmp(key, "rpc") == 0)
                    {
                        parseRPCProviderConf(val, KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown KeeperRecordingService "
                                     "configuration: " << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "KeeperDataStoreAdminService") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*keeper_data_store_admin_service_conf = json_object_object_get(json_conf
                                                                                          , "KeeperDataStoreAdminService");
                json_object_object_foreach(keeper_data_store_admin_service_conf, key, val)
                {
                    if(strcmp(key, "rpc") == 0)
                    {
                        parseRPCProviderConf(val, KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown KeeperDataStoreAdminService "
                                     "configuration: " << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "VisorKeeperRegistryService") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*visor_keeper_registry_service_conf = json_object_object_get(json_conf
                                                                                        , "VisorKeeperRegistryService");
                json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
                {
                    if(strcmp(key, "rpc") == 0)
                    {
                        parseRPCProviderConf(val, KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown VisorKeeperRegistryService configuration: "
                                  << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "KeeperGrapherDrainService") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*keeper_grapher_drain_service_conf = json_object_object_get(json_conf
                                                                                       , "KeeperGrapherDrainService");
                json_object_object_foreach(keeper_grapher_drain_service_conf, key, val)
                {
                    if(strcmp(key, "rpc") == 0)
                    {
                        parseRPCProviderConf(val, KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown KeeperGrapherDrainService configuration: "
                                  << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "Monitoring") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*chronokeeper_log = json_object_object_get(json_conf, "Monitoring");
                json_object_object_foreach(chronokeeper_log, key, val)
                {
                    if(strcmp(key, "monitor") == 0)
                    {
                        parseLogConf(val, KEEPER_CONF.KEEPER_LOG_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown Monitoring configuration: "
                                  << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "DataStoreInternals") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*data_store_conf = json_object_object_get(json_conf, "DataStoreInternals");
                json_object_object_foreach(data_store_conf, key, val)
                {
                    if(strcmp(key, "max_story_chunk_size") == 0)
                    {
                        assert(json_object_is_type(val, json_type_int));
                        KEEPER_CONF.DATA_STORE_CONF.max_story_chunk_size = json_object_get_int(val);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown DataStoreInternals configuration: "
                                  << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "Extractors") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*extractors = json_object_object_get(json_conf, "Extractors");
                json_object_object_foreach(extractors, key, val)
                {
                    if(strcmp(key, "story_files_dir") == 0)
                    {
//                    std::cerr << "reading story_files_dir" << std::endl;
                        assert(json_object_is_type(val, json_type_string));
                        KEEPER_CONF.EXTRACTOR_CONF.story_files_dir = json_object_get_string(val);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown Extractors configuration "
                                  << key << std::endl;
                    }
                }
            }
            else
            {
                std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown Keeper configuration: "
                          << key << std::endl;
            }
        }
    }

    void parseGrapherConf(json_object*json_conf);
    void parsePlayerConf(json_object*json_conf);

    void parseClientConf(json_object*json_conf)
    {
        json_object_object_foreach(json_conf, key, val)
        {
            if(strcmp(key, "ClientQueryService") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*visor_client_portal_service_conf = json_object_object_get(json_conf
                                                                                      , "ClientQueryService");
                json_object_object_foreach(visor_client_portal_service_conf, key, val)
                {
                    if(strcmp(key, "rpc") == 0)
                    {
                        parseRPCProviderConf(val, CLIENT_CONF.CLIENT_QUERY_SERVICE_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_client] Unknown ClientQueryService configuration: "
                                  << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "VisorClientPortalService") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*visor_client_portal_service_conf = json_object_object_get(json_conf
                                                                                      , "VisorClientPortalService");
                json_object_object_foreach(visor_client_portal_service_conf, key, val)
                {
                    if(strcmp(key, "rpc") == 0)
                    {
                        parseRPCProviderConf(val, CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_client] Unknown VisorClientPortalService configuration: "
                                  << key << std::endl;
                    }
                }
            }
            else if(strcmp(key, "Monitoring") == 0)
            {
                assert(json_object_is_type(val, json_type_object));
                json_object*chronoclient_log = json_object_object_get(json_conf, "Monitoring");
                json_object_object_foreach(chronoclient_log, key, val)
                {
                    if(strcmp(key, "monitor") == 0)
                    {
                        parseLogConf(val, CLIENT_CONF.CLIENT_LOG_CONF);
                    }
                    else
                    {
                        std::cerr << "[ConfigurationManager] [chrono_client] Unknown Monitoring configuration: "
                                  << key << std::endl;
                    }
                }
            }
            else
            {
                std::cerr << "[ConfigurationManager] [chrono_client] Unknown Client configuration: "
                          << key << std::endl;
            }
        }
    }
};
}

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
