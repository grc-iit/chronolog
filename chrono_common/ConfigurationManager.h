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
            return "CLOCKSOURCE_TYPE: " + std::string(getClocksourceTypeString(CLOCKSOURCE_TYPE))
                   + ", DRIFT_CAL_SLEEP_SEC: " + std::to_string(DRIFT_CAL_SLEEP_SEC)
                   + ", DRIFT_CAL_SLEEP_NSEC: " + std::to_string(DRIFT_CAL_SLEEP_NSEC);
        }
    } ClockConf;

    typedef struct RPCProviderEndConf_
    {
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        std::string PROTO_CONF;
        std::string IP;
        uint16_t BASE_PORT;
        uint16_t SERVICE_PROVIDER_ID;
        uint8_t PORTS;
        uint8_t SERVICE_THREADS;

        [[nodiscard]] std::string to_String() const
        {
            return "[RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION))
                   + ", PROTO_CONF: " + PROTO_CONF
                   + ", IP: " + IP
                   + ", BASE_PORT: " + std::to_string(BASE_PORT)
                   + ", SERVICE_PROVIDER_ID: " + std::to_string(SERVICE_PROVIDER_ID)
                   + ", PORTS: " + std::to_string(PORTS)
                   + ", SERVICE_THREADS: " + std::to_string(SERVICE_THREADS) + "]";
        }
    } RPCProviderEndConf;

    typedef struct RPCConsumerEndConf_
    {
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        std::string PROTO_CONF;
        uint16_t PORT;
        uint8_t SERVICE_THREADS;

        [[nodiscard]] std::string to_String() const
        {
            return "[RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION))
                   + ", PROTO_CONF: " + PROTO_CONF
                   + ", PORT: " + std::to_string(PORT)
                   + ", SERVICE_THREADS: " + std::to_string(SERVICE_THREADS) + "]";
        }
    } RPCConsumerEndConf;

    typedef struct VisorConf_
    {
        RPCProviderEndConf RPC_FROM_CLIENT_CONF;
        RPCProviderEndConf RPC_FROM_KEEPER_CONF;
        RPCConsumerEndConf RPC_TO_KEEPER_CONF;

        [[nodiscard]] std::string to_String() const
        {
            return "[RPC_FROM_CLIENT_CONF: " + RPC_FROM_CLIENT_CONF.to_String()
                   + ", RPC_FROM_KEEPER_CONF: " + RPC_FROM_KEEPER_CONF.to_String()
                   + ", RPC_TO_KEEPER_CONF: " + RPC_TO_KEEPER_CONF.to_String() + "]";
        }
    } VisorConf;

    typedef struct KeeperConf_
    {
        RPCProviderEndConf RPC_FROM_CLIENT_CONF;
        RPCProviderEndConf RPC_FROM_VISOR_CONF;
        RPCConsumerEndConf RPC_TO_VISOR_CONF;

        [[nodiscard]] std::string to_String() const
        {
            return "[RPC_FROM_CLIENT_CONF: " + RPC_FROM_CLIENT_CONF.to_String()
                   + ", RPC_FROM_VISOR_CONF: " + RPC_FROM_VISOR_CONF.to_String()
                   + ", RPC_TO_VISOR_CONF: " + RPC_TO_VISOR_CONF.to_String() + "]";
        }
    } KeeperConf;

    typedef struct ClientConf_
    {
        RPCConsumerEndConf RPC_TO_VISOR_CONF;
        RPCConsumerEndConf RPC_TO_KEEPER_CONF;

        [[nodiscard]] std::string to_String() const
        {
            return "[RPC_TO_VISOR_CONF: " + RPC_TO_VISOR_CONF.to_String()
                   + ", RPC_TO_KEEPER_CONF: " + RPC_TO_KEEPER_CONF.to_String() + "]";
        }
    } ClientConf;

    typedef struct AuthConf_
    {
        std::string AUTH_TYPE;
        std::string MODULE_PATH;

        [[nodiscard]] std::string to_String() const
        {
            return "AUTH_TYPE: " + AUTH_TYPE
                   + ", MODULE_PATH: " + MODULE_PATH;
        }
    } AuthConf;

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

            /* Visor-related configurations */
            VISOR_CONF.RPC_FROM_CLIENT_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            VISOR_CONF.RPC_FROM_CLIENT_CONF.PROTO_CONF = "ofi+sockets";
            VISOR_CONF.RPC_FROM_CLIENT_CONF.IP = "127.0.0.1";
            VISOR_CONF.RPC_FROM_CLIENT_CONF.BASE_PORT = 5555;
            VISOR_CONF.RPC_FROM_CLIENT_CONF.SERVICE_PROVIDER_ID = 55;
            VISOR_CONF.RPC_FROM_CLIENT_CONF.PORTS = 1;
            VISOR_CONF.RPC_FROM_CLIENT_CONF.SERVICE_THREADS = 1;

            VISOR_CONF.RPC_FROM_KEEPER_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            VISOR_CONF.RPC_FROM_KEEPER_CONF.PROTO_CONF = "ofi+sockets";
            VISOR_CONF.RPC_FROM_KEEPER_CONF.IP = "127.0.0.1";
            VISOR_CONF.RPC_FROM_KEEPER_CONF.BASE_PORT = 7777;
            VISOR_CONF.RPC_FROM_KEEPER_CONF.SERVICE_PROVIDER_ID = 77;
            VISOR_CONF.RPC_FROM_KEEPER_CONF.PORTS = 1;
            VISOR_CONF.RPC_FROM_KEEPER_CONF.SERVICE_THREADS = 1;

            VISOR_CONF.RPC_TO_KEEPER_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            VISOR_CONF.RPC_TO_KEEPER_CONF.PROTO_CONF = "ofi+sockets";
            VISOR_CONF.RPC_TO_KEEPER_CONF.PORT = 2222;
            VISOR_CONF.RPC_TO_KEEPER_CONF.SERVICE_THREADS = 1;

            /* Keeper-related configurations */
            KEEPER_CONF.RPC_FROM_VISOR_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            KEEPER_CONF.RPC_FROM_VISOR_CONF.PROTO_CONF = "ofi+sockets";
            KEEPER_CONF.RPC_FROM_VISOR_CONF.IP = "127.0.0.1";
            KEEPER_CONF.RPC_FROM_VISOR_CONF.BASE_PORT = 6666;
            KEEPER_CONF.RPC_FROM_VISOR_CONF.SERVICE_PROVIDER_ID = 66;
            KEEPER_CONF.RPC_FROM_VISOR_CONF.PORTS = 1;
            KEEPER_CONF.RPC_FROM_VISOR_CONF.SERVICE_THREADS = 1;

            KEEPER_CONF.RPC_FROM_CLIENT_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            KEEPER_CONF.RPC_FROM_CLIENT_CONF.PROTO_CONF = "ofi+sockets";
            KEEPER_CONF.RPC_FROM_CLIENT_CONF.IP = "127.0.0.1";
            KEEPER_CONF.RPC_FROM_CLIENT_CONF.BASE_PORT = 9999;
            KEEPER_CONF.RPC_FROM_CLIENT_CONF.SERVICE_PROVIDER_ID = 99;
            KEEPER_CONF.RPC_FROM_CLIENT_CONF.PORTS = 1;
            KEEPER_CONF.RPC_FROM_CLIENT_CONF.SERVICE_THREADS = 1;

            KEEPER_CONF.RPC_TO_VISOR_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            KEEPER_CONF.RPC_TO_VISOR_CONF.PROTO_CONF = "ofi+sockets";
            KEEPER_CONF.RPC_TO_VISOR_CONF.PORT = 3333;
            KEEPER_CONF.RPC_TO_VISOR_CONF.SERVICE_THREADS = 1;

            /* Client-related configurations */
            CLIENT_CONF.RPC_TO_VISOR_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            CLIENT_CONF.RPC_TO_VISOR_CONF.PROTO_CONF = "ofi+sockets";
            CLIENT_CONF.RPC_TO_VISOR_CONF.PORT = 4444;
            CLIENT_CONF.RPC_TO_VISOR_CONF.SERVICE_THREADS = 1;

            CLIENT_CONF.RPC_TO_KEEPER_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            CLIENT_CONF.RPC_TO_KEEPER_CONF.PROTO_CONF = "ofi+sockets";
            CLIENT_CONF.RPC_TO_KEEPER_CONF.PORT = 8888;
            CLIENT_CONF.RPC_TO_KEEPER_CONF.SERVICE_THREADS = 1;

            /* Authentication-related configurations */
            AUTH_CONF.AUTH_TYPE = "RBAC";
            AUTH_CONF.MODULE_PATH = "";

            /* Visor-local configurations */

            /* Client-local configurations */

            /* Keeper-local configurations */

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
            json_object *root = json_object_from_file(conf_file_path.c_str());
            if (root == nullptr)
            {
                LOGE("Unable to open file %s, exiting ...", conf_file_path.c_str());
                exit(CL_ERR_NOT_EXIST);
            }

            json_object_object_foreach(root, key, val)
            {
                if (strcmp(key, "clock") == 0)
                {
                    json_object *clock_conf = json_object_object_get(root, "clock");
                    if (clock_conf == nullptr || !json_object_is_type(clock_conf, json_type_object))
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "clock configuration is not an object");
                        exit(CL_ERR_INVALID_CONF);
                    }
                    json_object_object_foreach(clock_conf, key, val)
                    {
                        if (strcmp(key, "clocksource_type") == 0)
                        {
                            if (json_object_is_type(val, json_type_string))
                            {
                                const char *clocksource_type = json_object_get_string(val);
                                if (strcmp(clocksource_type, "C_STYLE") == 0)
                                    CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;
                                else if (strcmp(clocksource_type, "CPP_STYLE") == 0)
                                    CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::CPP_STYLE;
                                else if (strcmp(clocksource_type, "TSC") == 0)
                                    CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::TSC;
                                else
                                    LOGI("Unknown clocksource type: %s", clocksource_type);
                            }
                            else
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(), "clocksource_type is not a string");
                                exit(CL_ERR_INVALID_CONF);
                            }
                        }
                        else if (strcmp(key, "drift_cal_sleep_sec") == 0)
                        {
                            if (json_object_is_type(val, json_type_int))
                            {
                                CLOCK_CONF.DRIFT_CAL_SLEEP_SEC = json_object_get_int(val);
                            }
                            else
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(), "drift_cal_sleep_sec is not an integer");
                                exit(CL_ERR_INVALID_CONF);
                            }
                        }
                        else if (strcmp(key, "drift_cal_sleep_nsec") == 0)
                        {
                            if (json_object_is_type(val, json_type_int))
                            {
                                CLOCK_CONF.DRIFT_CAL_SLEEP_NSEC = json_object_get_int(val);
                            }
                            else
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(), "drift_cal_sleep_nsec is not an integer");
                                exit(CL_ERR_INVALID_CONF);
                            }
                        }
                    }
                }
                else if (strcmp(key, "authentication") == 0)
                {
                    json_object *auth_conf = json_object_object_get(root, "authentication");
                    if (auth_conf == nullptr || !json_object_is_type(auth_conf, json_type_object))
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "authentication configuration is not an "
                                                                    "object");
                        exit(CL_ERR_INVALID_CONF);
                    }
                    json_object_object_foreach(auth_conf, key, val)
                    {
                        if (strcmp(key, "auth_type") == 0)
                        {
                            if (json_object_is_type(val, json_type_string))
                            {
                                AUTH_CONF.AUTH_TYPE = json_object_get_string(val);
                            }
                            else
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(), "auth_type is not a string");
                                exit(CL_ERR_INVALID_CONF);
                            }
                        }
                        else if (strcmp(key, "module_location") == 0)
                        {
                            if (json_object_is_type(val, json_type_string))
                            {
                                AUTH_CONF.MODULE_PATH = json_object_get_string(val);
                            }
                            else
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(), "module_location is not a string");
                                exit(CL_ERR_INVALID_CONF);
                            }
                        }
                    }
                }
                else if (strcmp(key, "chrono_visor") == 0)
                {
                    json_object *chrono_visor_conf = json_object_object_get(root, "chrono_visor");
                    if (chrono_visor_conf == nullptr || !json_object_is_type(chrono_visor_conf, json_type_object))
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "chrono_visor configuration is not an object");
                        exit(CL_ERR_INVALID_CONF);
                    }
                    json_object_object_foreach(chrono_visor_conf, key, val)
                    {
                        if (strcmp(key, "rpc_from_client") == 0)
                        {
                            json_object *rpc_from_client_conf = json_object_object_get(chrono_visor_conf, "rpc_from_client");
                            if (rpc_from_client_conf == nullptr ||
                                !json_object_is_type(rpc_from_client_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_from_client configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            parseProviderEndConf(rpc_from_client_conf, VISOR_CONF.RPC_FROM_CLIENT_CONF);
                        }
                        else if (strcmp(key, "rpc_from_keeper") == 0)
                        {
                            json_object *rpc_from_keeper_conf = json_object_object_get(chrono_visor_conf,
                                                                                    "rpc_from_keeper");
                            if (rpc_from_keeper_conf == nullptr ||
                                !json_object_is_type(rpc_from_keeper_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_from_keeper configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            parseProviderEndConf(rpc_from_keeper_conf, VISOR_CONF.RPC_FROM_KEEPER_CONF);
                        }
                        else if (strcmp(key, "rpc_to_keeper") == 0)
                        {
                            json_object *rpc_to_keeper_conf = json_object_object_get(chrono_visor_conf,
                                                                                     "rpc_to_keeper");
                            if (rpc_to_keeper_conf == nullptr ||
                                !json_object_is_type(rpc_to_keeper_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_to_keeper configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            parseConsumerEndConf(rpc_to_keeper_conf, VISOR_CONF.RPC_TO_KEEPER_CONF);
                        }
                    }
                }
                else if (strcmp(key, "chrono_keeper") == 0)
                {
                    json_object *chrono_keeper_conf = json_object_object_get(root, "chrono_keeper");
                    if (chrono_keeper_conf == nullptr || !json_object_is_type(chrono_keeper_conf, json_type_object))
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "chrono_keeper configuration is not an object");
                        exit(CL_ERR_INVALID_CONF);
                    }
                    json_object_object_foreach(chrono_keeper_conf, key, val)
                    {
                        if (strcmp(key, "rpc_from_visor") == 0)
                        {
                            json_object *rpc_from_visor_conf = json_object_object_get(chrono_keeper_conf,
                                                                                      "rpc_from_visor");
                            if (rpc_from_visor_conf == nullptr ||
                                !json_object_is_type(rpc_from_visor_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_from_visor configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            parseProviderEndConf(rpc_from_visor_conf, KEEPER_CONF.RPC_FROM_VISOR_CONF);
                        }
                        else if (strcmp(key, "rpc_from_client") == 0)
                        {
                            json_object *rpc_from_client_conf = json_object_object_get(chrono_keeper_conf, "rpc_from_client");
                            if (rpc_from_client_conf == nullptr ||
                                !json_object_is_type(rpc_from_client_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_from_client configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            parseProviderEndConf(rpc_from_client_conf, KEEPER_CONF.RPC_FROM_CLIENT_CONF);
                        }
                        else if (strcmp(key, "rpc_to_visor") == 0)
                        {
                            json_object *rpc_to_visor_conf = json_object_object_get(chrono_keeper_conf,
                                                                                     "rpc_to_visor");
                            if (rpc_to_visor_conf == nullptr ||
                                !json_object_is_type(rpc_to_visor_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_to_visor configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            parseConsumerEndConf(rpc_to_visor_conf, KEEPER_CONF.RPC_TO_VISOR_CONF);
                        }
                    }
                }
                else if (strcmp(key, "chrono_client") == 0)
                {
                    json_object *chrono_client_conf = json_object_object_get(root, "chrono_client");
                    if (chrono_client_conf == nullptr || !json_object_is_type(chrono_client_conf, json_type_object))
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "chrono_client configuration is not an object");
                        exit(CL_ERR_INVALID_CONF);
                    }
                    json_object_object_foreach(chrono_client_conf, key, val)
                    {
                        if (strcmp(key, "rpc_to_visor") == 0)
                        {
                            json_object *rpc_to_visor_conf = json_object_object_get(chrono_client_conf,
                                                                                    "rpc_to_visor");
                            if (rpc_to_visor_conf == nullptr ||
                                !json_object_is_type(rpc_to_visor_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_to_visor configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            parseConsumerEndConf(rpc_to_visor_conf, CLIENT_CONF.RPC_TO_VISOR_CONF);
                        }
                        else if (strcmp(key, "rpc_to_keeper") == 0)
                        {
                            json_object *rpc_to_keeper_conf = json_object_object_get(chrono_client_conf,
                                                                                    "rpc_to_keeper");
                            if (rpc_to_keeper_conf == nullptr ||
                                !json_object_is_type(rpc_to_keeper_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_to_keeper configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            parseConsumerEndConf(rpc_to_keeper_conf, CLIENT_CONF.RPC_TO_KEEPER_CONF);
                        }
                    }
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
        void parseRPCImplConf(json_object *json_conf, ChronoLogRPCImplementation &rpc_impl)
        {
            if (json_object_is_type(json_conf, json_type_string))
            {
                const char *conf_str = json_object_get_string(json_conf);
                if (strcmp(conf_str, "Thallium_sockets") == 0)
                {
                    rpc_impl = CHRONOLOG_THALLIUM_SOCKETS;
                }
                else if (strcmp(conf_str, "Thallium_tcp") == 0)
                {
                    rpc_impl = CHRONOLOG_THALLIUM_TCP;
                }
                else if (strcmp(conf_str, "Thallium_roce") == 0)
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

        void parseConsumerEndConf(json_object *json_conf, RPCConsumerEndConf &rpc_consumer_conf)
        {
            assert(json_object_is_type(json_conf, json_type_object));
            json_object_object_foreach(json_conf, key, val)
            {
                if (strcmp(key, "rpc_implementation") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    parseRPCImplConf(val, rpc_consumer_conf.RPC_IMPLEMENTATION);
                }
                else if (strcmp(key, "protocol_conf") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    rpc_consumer_conf.PROTO_CONF = json_object_get_string(val);
                }
                else if (strcmp(key, "port") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_consumer_conf.PORT = json_object_get_int(val);
                }
                else if (strcmp(key, "service_threads") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_consumer_conf.SERVICE_THREADS = json_object_get_int(val);
                }
                else
                {
                    LOGE("Unknown client end configuration: %s", key);
                }
            }
        }

        void parseProviderEndConf(json_object *json_conf, RPCProviderEndConf &rpc_provider_conf)
        {
            assert(json_object_is_type(json_conf, json_type_object));
            json_object_object_foreach(json_conf, key, val)
            {
                if (strcmp(key, "rpc_implementation") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    parseRPCImplConf(val, rpc_provider_conf.RPC_IMPLEMENTATION);
                }
                else if (strcmp(key, "protocol_conf") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    rpc_provider_conf.PROTO_CONF = json_object_get_string(val);
                }
                else if (strcmp(key, "ip") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    rpc_provider_conf.IP = json_object_get_string(val);
                }
                else if (strcmp(key, "base_port") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_provider_conf.BASE_PORT = json_object_get_int(val);
                }
                else if (strcmp(key, "service_provider_id") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_provider_conf.SERVICE_PROVIDER_ID = json_object_get_int(val);
                }
                else if (strcmp(key, "ports") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_provider_conf.PORTS = json_object_get_int(val);
                }
                else if (strcmp(key, "service_threads") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_provider_conf.SERVICE_THREADS = json_object_get_int(val);
                }
                else
                {
                    LOGE("Unknown visor end configuration: %s", key);
                }
            }
        }

        void parseVisorLocalConf(json_object *json_conf)
        {
            assert(json_object_is_type(json_conf, json_type_object));
            json_object_object_foreach(json_conf, key, val)
            {
                if (strcmp(key, "visor_local_ip") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    const char *visor_local_ip = json_object_get_string(val);
                    LOGI("Visor local IP: %s", visor_local_ip);
                }
                else if (strcmp(key, "visor_local_port") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    int visor_local_port = json_object_get_int(val);
                    LOGI("Visor local port: %d", visor_local_port);
                }
                else
                {
                    LOGE("Unknown visor local configuration: %s", key);
                }
            }
        }

        void parseClientLocalConf(json_object *json_conf)
        {
            assert(json_object_is_type(json_conf, json_type_object));
        }

        void parseKeeperLocalConf(json_object *json_conf)
        {
            assert(json_object_is_type(json_conf, json_type_object));
        }
    };
}

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
