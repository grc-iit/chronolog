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

namespace chronolog
{
struct ClockConf
{
    ClocksourceType CLOCKSOURCE_TYPE;
    uint64_t DRIFT_CAL_SLEEP_SEC;
    uint64_t DRIFT_CAL_SLEEP_NSEC;

    ClockConf()
    {
        /* Clock-related configurations */
        CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;
        DRIFT_CAL_SLEEP_SEC = 10;
        DRIFT_CAL_SLEEP_NSEC = 0;
    }

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "CLOCKSOURCE_TYPE: " + std::string(getClocksourceTypeString(CLOCKSOURCE_TYPE)) +
               ", DRIFT_CAL_SLEEP_SEC: " + std::to_string(DRIFT_CAL_SLEEP_SEC) + ", DRIFT_CAL_SLEEP_NSEC: " +
               std::to_string(DRIFT_CAL_SLEEP_NSEC);
    }
};

struct AuthConf
{
    std::string AUTH_TYPE;
    std::string MODULE_PATH;

    AuthConf()
    {
        /* Authentication-related configurations */
        AUTH_TYPE = "RBAC";
        MODULE_PATH = "";

    }

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "AUTH_TYPE: " + AUTH_TYPE + ", MODULE_PATH: " + MODULE_PATH;
    }
};

struct RPCProviderConf
{
    std::string PROTO_CONF;
    std::string IP;
    uint16_t BASE_PORT{};
    uint16_t SERVICE_PROVIDER_ID{};

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[PROTO_CONF: " +
               PROTO_CONF + ", IP: " + IP + ", BASE_PORT: " + std::to_string(BASE_PORT) + ", SERVICE_PROVIDER_ID: " +
               std::to_string(SERVICE_PROVIDER_ID) + ", PORTS: " + "]";
    }
};

struct LogConf
{
    std::string LOGTYPE;
    std::string LOGFILE;
    spdlog::level::level_enum LOGLEVEL{};
    std::string LOGNAME;
    size_t LOGFILESIZE{};
    size_t LOGFILENUM{};
    spdlog::level::level_enum FLUSHLEVEL{};

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

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[TYPE: " + LOGTYPE + ", FILE: " + LOGFILE + ", LEVEL: " + LevelToString(LOGLEVEL) + ", NAME: " +
               LOGNAME + ", LOGFILESIZE: " + std::to_string(LOGFILESIZE) + ", LOGFILENUM: " +
               std::to_string(LOGFILENUM) + ", FLUSH LEVEL: " + LevelToString(FLUSHLEVEL) + "]";
    }
};

struct DataStoreConf
{
    int max_story_chunk_size = 4096;
    int story_chunk_duration_secs = 30;
    int acceptance_window_secs = 60;
    int inactive_story_delay_secs = 180;

    DataStoreConf()
    { }

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return  "[DATA_STORE_CONF: max_story_chunk_size: " + std::to_string(max_story_chunk_size) +
                " story_chunk_duration_secs: " + std::to_string(story_chunk_duration_secs) +
                " acceptance_window_secs: " + std::to_string(acceptance_window_secs) +
                " inactive_story_delay_secs: " + std::to_string(inactive_story_delay_secs) +
                "]";
    }
};

struct ExtractorReaderConf
{
    std::string story_files_dir;

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return  "[EXTRACTOR_READER_CONF: STORY_FILES_DIR: " + story_files_dir +
                "]";
    }
};


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
               ", VISOR_LOG: " + VISOR_LOG_CONF.to_String() + ", DELAYED_DATA_ADMIN_EXIT_IN_SECS: " +
               std::to_string(DELAYED_DATA_ADMIN_EXIT_IN_SECS) + "]";
    }
};

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
        return "[CHRONO_GRAPHER_CONFIGURATION: RECORDING_GROUP: "+ std::to_string(RECORDING_GROUP) +
               ", KEEPER_GRAPHER_DRAIN_SERVICE_CONF: " + KEEPER_GRAPHER_DRAIN_SERVICE_CONF.to_String() +
               ", DATA_STORE_ADMIN_SERVICE_CONF: " + DATA_STORE_ADMIN_SERVICE_CONF.to_String() +
               ", VISOR_REGISTRY_SERVICE_CONF: " + VISOR_REGISTRY_SERVICE_CONF.to_String() +
               ", LOG_CONF: " + LOG_CONF.to_String() +
               ", DATA_STORE_CONF: " + DATA_STORE_CONF.to_String() +
               ", EXTRACTOR_CONF: " + EXTRACTOR_CONF.to_String() +
               "]";
    }
};

struct GrapherConfiguration
{
    uint32_t RECORDING_GROUP{};
    RPCProviderConf KEEPER_GRAPHER_DRAIN_SERVICE_CONF;
    RPCProviderConf DATA_STORE_ADMIN_SERVICE_CONF;
    RPCProviderConf VISOR_REGISTRY_SERVICE_CONF;
    LogConf LOG_CONF;
    DataStoreConf DATA_STORE_CONF{};
    ExtractorReaderConf EXTRACTOR_CONF;

    GrapherConfiguration()
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
        DATA_STORE_CONF.story_chunk_duration_secs = 60;
        DATA_STORE_CONF.acceptance_window_secs = 180;
        DATA_STORE_CONF.inactive_story_delay_secs = 300;

        EXTRACTOR_CONF.story_files_dir = "/tmp/";
    }

    int parseJsonConf(json_object*);
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
};

struct PlayerConfiguration
{
    uint32_t RECORDING_GROUP;
    RPCProviderConf DATA_STORE_ADMIN_SERVICE_CONF;
    RPCProviderConf PLAYBACK_SERVICE_CONF;
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

        PLAYBACK_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        PLAYBACK_SERVICE_CONF.IP = "127.0.0.1";
        PLAYBACK_SERVICE_CONF.BASE_PORT = 2225;
        PLAYBACK_SERVICE_CONF.SERVICE_PROVIDER_ID = 25;

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
               ", PLAYBACK_SERVICE_CONF: " + PLAYBACK_SERVICE_CONF.to_String() +
               ", VISOR_REGISTRY_SERVICE_CONF: " + VISOR_REGISTRY_SERVICE_CONF.to_String() +
               ", LOG_CONF: " + LOG_CONF.to_String() +
               ", DATA_STORE_CONF: " + DATA_STORE_CONF.to_String() +
               ", READER_CONF: " + READER_CONF.to_String() +
               "]";
    }
};

class ConfigurationManager
{
public:
    ClockConf CLOCK_CONF{};
    AuthConf AUTH_CONF{};
    VisorConfiguration VISOR_CONF{};
    KeeperConfiguration KEEPER_CONF{};
    GrapherConfiguration GRAPHER_CONF{};
    PlayerConfiguration PLAYER_CONF{};

    ConfigurationManager()
    : CLOCK_CONF{}
    , AUTH_CONF{}
    , VISOR_CONF{}
    , KEEPER_CONF{}
    , GRAPHER_CONF{}
    , PLAYER_CONF{}
    {

    }

    explicit ConfigurationManager(const std::string &conf_file_path)
    {
        LoadConfFromJSONFile(conf_file_path);
    }

    void LoadConfFromJSONFile(const std::string &conf_file_path)
    {
        json_object*root = json_object_from_file(conf_file_path.c_str());
        if(root == nullptr)
        {
            std::cerr << "[ConfigurationManager] Failed to open configuration file at path: " << conf_file_path.c_str()
                      << ". Exiting..." << std::endl;
            exit(chronolog::CL_ERR_UNKNOWN);
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
                CLOCK_CONF.parseJsonConf(clock_conf);
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
                AUTH_CONF.parseJsonConf(auth_conf);
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
                VISOR_CONF.parseJsonConf(chrono_visor_conf);
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
                KEEPER_CONF.parseJsonConf(chrono_keeper_conf);
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
                GRAPHER_CONF.parseJsonConf(chrono_grapher_conf);
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
                PLAYER_CONF.parseJsonConf(chrono_player_conf);
            }
        }
        json_object_put(root);
    }
};
}

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
