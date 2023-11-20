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
#include "enum.h"
#include "log.h"
#include "errcode.h"

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
    ChronoLogRPCImplementation RPC_IMPLEMENTATION;
    std::string PROTO_CONF;
    std::string IP;
    uint16_t BASE_PORT;
    uint16_t SERVICE_PROVIDER_ID;

    [[nodiscard]] std::string to_String() const
    {
        return "[RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION)) + ", PROTO_CONF: " +
               PROTO_CONF + ", IP: " + IP + ", BASE_PORT: " + std::to_string(BASE_PORT) + ", SERVICE_PROVIDER_ID: " +
               std::to_string(SERVICE_PROVIDER_ID) + ", PORTS: " + "]";
    }
} RPCProviderConf;

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

typedef struct VisorConf_
{
    VisorClientPortalServiceConf VISOR_CLIENT_PORTAL_SERVICE_CONF;
    VisorKeeperRegistryServiceConf VISOR_KEEPER_REGISTRY_SERVICE_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[VISOR_CLIENT_PORTAL_SERVICE_CONF: " + VISOR_CLIENT_PORTAL_SERVICE_CONF.to_String() +
               ", VISOR_KEEPER_REGISTRY_SERVICE_CONF: " + VISOR_KEEPER_REGISTRY_SERVICE_CONF.to_String() + "]";
    }
} VisorConf;

typedef struct KeeperConf_
{
    KeeperRecordingServiceConf KEEPER_RECORDING_SERVICE_CONF;
    KeeperDataStoreAdminServiceConf KEEPER_DATA_STORE_ADMIN_SERVICE_CONF;
    VisorKeeperRegistryServiceConf VISOR_KEEPER_REGISTRY_SERVICE_CONF;
    std::string STORY_FILES_DIR;

    [[nodiscard]] std::string to_String() const
    {
        return "[KEEPER_RECORDING_SERVICE_CONF: " + KEEPER_RECORDING_SERVICE_CONF.to_String() +
               ", KEEPER_DATA_STORE_ADMIN_SERVICE_CONF: " + KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.to_String() +
               ", VISOR_KEEPER_REGISTRY_SERVICE_CONF: " + VISOR_KEEPER_REGISTRY_SERVICE_CONF.to_String() +
               ", STORY_FILES_DIR:" + STORY_FILES_DIR + "]";
    }
} KeeperConf;

typedef struct ClientConf_
{
    VisorClientPortalServiceConf VISOR_CLIENT_PORTAL_SERVICE_CONF;

    [[nodiscard]] std::string to_String() const
    {
        return "[VISOR_CLIENT_PORTAL_SERVICE_CONF: " + VISOR_CLIENT_PORTAL_SERVICE_CONF.to_String() + "]";
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

    ConfigurationManager()
    {
        LOGI("constructing configuration with all default values: ");
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
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT = 5555;
        VISOR_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 55;

        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.BASE_PORT = 8888;
        VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 88;

        /* Keeper-related configurations */
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT = 6666;
        KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 66;

        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.BASE_PORT = 7777;
        KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 77;

        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.IP = "127.0.0.1";
        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.BASE_PORT = 8888;
        KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 88;

        KEEPER_CONF.STORY_FILES_DIR = "/tmp/";

        /* Client-related configurations */
        CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
        CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF = "ofi+sockets";
        CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT = 5555;
        CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID = 55;

        PrintConf();
    }

    explicit ConfigurationManager(const std::string &conf_file_path)
    {
        LOGI("constructing configuration from a configuration file: %s", conf_file_path.c_str());
        LoadConfFromJSONFile(conf_file_path);
    }

    void PrintConf() const
    {
        LOGI("******** Start of configuration output ********");
        LOGI("ROLE: %s", getServiceRoleString(ROLE));
        LOGI("CLOCK_CONF: %s", CLOCK_CONF.to_String().c_str());
        LOGI("AUTH_CONF: %s", AUTH_CONF.to_String().c_str());
        LOGI("VISOR_CONF: %s", VISOR_CONF.to_String().c_str());
        LOGI("KEEPER_CONF: %s", KEEPER_CONF.to_String().c_str());
        LOGI("CLIENT_CONF: %s", CLIENT_CONF.to_String().c_str());
        LOGI("******** End of configuration output ********");
    }

    void LoadConfFromJSONFile(const std::string &conf_file_path)
    {
        json_object*root = json_object_from_file(conf_file_path.c_str());
        if(root == nullptr)
        {
            LOGE("Unable to open file %s, exiting ...", conf_file_path.c_str());
            exit(CL_ERR_NOT_EXIST);
        }

        json_object_object_foreach(root, key, val)
        {
            if(strcmp(key, "clock") == 0)
            {
                json_object*clock_conf = json_object_object_get(root, "clock");
                if(clock_conf == nullptr || !json_object_is_type(clock_conf, json_type_object))
                {
                    LOGE("Error during parsing configuration file %s\n"
                         "Error: %s\n", conf_file_path.c_str(), "clock configuration is not an object");
                    exit(CL_ERR_INVALID_CONF);
                }
                parseClockConf(clock_conf);
            }
            else if(strcmp(key, "authentication") == 0)
            {
                json_object*auth_conf = json_object_object_get(root, "authentication");
                if(auth_conf == nullptr || !json_object_is_type(auth_conf, json_type_object))
                {
                    LOGE("Error during parsing configuration file %s\n"
                         "Error: %s\n", conf_file_path.c_str(), "authentication configuration is not an "
                                                                "object");
                    exit(CL_ERR_INVALID_CONF);
                }
                parseAuthConf(auth_conf);
            }
            else if(strcmp(key, "chrono_visor") == 0)
            {
                json_object*chrono_visor_conf = json_object_object_get(root, "chrono_visor");
                if(chrono_visor_conf == nullptr || !json_object_is_type(chrono_visor_conf, json_type_object))
                {
                    LOGE("Error during parsing configuration file %s\n"
                         "Error: %s\n", conf_file_path.c_str(), "chrono_visor configuration is not an object");
                    exit(CL_ERR_INVALID_CONF);
                }
                parseVisorConf(chrono_visor_conf);
            }
            else if(strcmp(key, "chrono_keeper") == 0)
            {
                json_object*chrono_keeper_conf = json_object_object_get(root, "chrono_keeper");
                if(chrono_keeper_conf == nullptr || !json_object_is_type(chrono_keeper_conf, json_type_object))
                {
                    LOGE("Error during parsing configuration file %s\n"
                         "Error: %s\n", conf_file_path.c_str(), "chrono_keeper configuration is not an object");
                    exit(CL_ERR_INVALID_CONF);
                }
                parseKeeperConf(chrono_keeper_conf);
            }
            else if(strcmp(key, "chrono_client") == 0)
            {
                json_object*chrono_client_conf = json_object_object_get(root, "chrono_client");
                if(chrono_client_conf == nullptr || !json_object_is_type(chrono_client_conf, json_type_object))
                {
                    LOGE("Error during parsing configuration file %s\n"
                         "Error: %s\n", conf_file_path.c_str(), "chrono_client configuration is not an object");
                    exit(CL_ERR_INVALID_CONF);
                }
                parseClientConf(chrono_client_conf);
            }
            else
            {
                LOGE("Unknown configuration item: %s", key);
            }
        }

        json_object_put(root);
        PrintConf();
    }

private:
    void parseRPCImplConf(json_object*json_conf, ChronoLogRPCImplementation &rpc_impl)
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
                LOGE("Unknown rpc implementation: %s", conf_str);
            }
        }
        else
        {
            LOGE("Invalid rpc implementation configuration");
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
                        LOGI("Unknown clocksource type: %s", clocksource_type);
                }
                else
                {
                    LOGE("clocksource_type is not a string");
                    exit(CL_ERR_INVALID_CONF);
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
                    LOGE("drift_cal_sleep_sec is not an integer");
                    exit(CL_ERR_INVALID_CONF);
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
                    LOGE("drift_cal_sleep_nsec is not an integer");
                    exit(CL_ERR_INVALID_CONF);
                }
            }
        }
    }

    void parseAuthConf(json_object*auth_conf)
    {
        if(auth_conf == nullptr || !json_object_is_type(auth_conf, json_type_object))
        {
            LOGE("authentication configuration is not an object");
            exit(CL_ERR_INVALID_CONF);
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
                    LOGE("auth_type is not a string");
                    exit(CL_ERR_INVALID_CONF);
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
                    LOGE("module_location is not a string");
                    exit(CL_ERR_INVALID_CONF);
                }
            }
        }
    }

    void parseRPCProviderConf(json_object*json_conf, RPCProviderConf &rpc_provider_conf)
    {
        json_object_object_foreach(json_conf, key, val)
        {
            if(strcmp(key, "rpc_implementation") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                parseRPCImplConf(val, rpc_provider_conf.RPC_IMPLEMENTATION);
            }
            else if(strcmp(key, "protocol_conf") == 0)
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
                LOGE("Unknown client end configuration: %s", key);
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
                        LOGE("Unknown VisorClientPortalService configuration: %s", key);
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
                        LOGE("Unknown VisorKeeperRegistryService configuration: %s", key);
                    }
                }
            }
            else
            {
                LOGE("Unknown visor configuration: %s", key);
            }
        }
    }

    void parseKeeperConf(json_object*json_conf)
    {
        json_object_object_foreach(json_conf, key, val)
        {
            if(strcmp(key, "KeeperRecordingService") == 0)
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
                        LOGE("Unknown KeeperRecordingService configuration: %s", key);
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
                        LOGE("Unknown KeeperDataStoreAdminService configuration: %s", key);
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
                        LOGE("Unknown VisorKeeperRegistryService configuration: %s", key);
                    }
                }
            }
            else if(strcmp(key, "story_files_dir") == 0)
            {
                assert(json_object_is_type(val, json_type_string));
                KEEPER_CONF.STORY_FILES_DIR = json_object_get_string(val);
            }
            else
            {
                LOGE("Unknown keeper configuration: %s", key);
            }
        }
    }

    void parseClientConf(json_object*json_conf)
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
                        parseRPCProviderConf(val, CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF);
                    }
                    else
                    {
                        LOGE("Unknown VisorClientPortalService configuration: %s", key);
                    }
                }
            }
            else
            {
                LOGE("Unknown client configuration: %s", key);
            }
        }
    }

};
}

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
