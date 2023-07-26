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

    typedef struct RPCClientEndConf_
    {
        uint16_t CLIENT_PORT;
        uint8_t CLIENT_SERVICE_THREADS;

        [[nodiscard]] std::string to_String() const
        {
            return "[CLIENT_PORT: " + std::to_string(CLIENT_PORT)
                   + ", CLIENT_SERVICE_THREADS: " + std::to_string(CLIENT_SERVICE_THREADS) + "]";
        }
    } RPCClientEndConf;

    typedef struct RPCVisorEndConf_
    {
        std::string VISOR_IP;
        uint16_t VISOR_BASE_PORT;
        uint16_t SERVICE_PROVIDER_ID;
        uint8_t VISOR_PORTS;
        uint8_t VISOR_SERVICE_THREADS;

        [[nodiscard]] std::string to_String() const
        {
            return "[VISOR_IP: " + VISOR_IP
                   + ", VISOR_BASE_PORT: " + std::to_string(VISOR_BASE_PORT)
                   + ", SERVICE_PROVIDER_ID: " + std::to_string(SERVICE_PROVIDER_ID)
                   + ", VISOR_PORTS: " + std::to_string(VISOR_PORTS)
                   + ", VISOR_SERVICE_THREADS: " + std::to_string(VISOR_SERVICE_THREADS) + "]";
        }
    } RPCVisorEndConf;

    typedef struct RPCKeeperEndConf_
    {
        std::string KEEPER_IP;
        uint16_t KEEPER_PORT;
        uint16_t SERVICE_PROVIDER_ID;
        uint8_t KEEPER_SERVICE_THREADS;

        [[nodiscard]] std::string to_String() const
        {
            return "[KEEPER_IP: " + KEEPER_IP
                   + ", KEEPER_PORT: " + std::to_string(KEEPER_PORT)
                   + ", SERVICE_PROVIDER_ID: " + std::to_string(SERVICE_PROVIDER_ID)
                   + ", KEEPER_SERVICE_THREADS: " + std::to_string(KEEPER_SERVICE_THREADS) + "]";
        }
    } RPCKeeperEndConf;

    typedef struct RPCClientVisorConf_
    {
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        std::string PROTO_CONF;
        RPCClientEndConf CLIENT_END_CONF;
        RPCVisorEndConf VISOR_END_CONF;

        [[nodiscard]] std::string to_String() const
        {
            return "{RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION))
                   + ", PROTO_CONF: " + PROTO_CONF
                   + ", CLIENT_END_CONF: " + CLIENT_END_CONF.to_String()
                   + ", VISOR_END_CONF: " + VISOR_END_CONF.to_String() + "}";
        }
    } RPCClientVisorConf;

    typedef struct RPCVisorKeeperConf_
    {
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        std::string PROTO_CONF;
        RPCVisorEndConf VISOR_END_CONF;
        RPCKeeperEndConf KEEPER_END_CONF;

        [[nodiscard]] std::string to_String() const
        {
            return "{RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION))
                   + ", PROTO_CONF: " + PROTO_CONF
                   + ", VISOR_END_CONF: " + VISOR_END_CONF.to_String()
                   + ", KEEPER_END_CONF: " + KEEPER_END_CONF.to_String() + "}";
        }
    } RPCVisorKeeperConf;

    typedef struct RPCClientKeeperConf_
    {
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        std::string PROTO_CONF;
        RPCClientEndConf CLIENT_END_CONF;
        RPCKeeperEndConf KEEPER_END_CONF;

        [[nodiscard]] std::string to_String() const
        {
            return "{RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION))
                   + ", PROTO_CONF: " + PROTO_CONF
                   + ", CLIENT_END_CONF: " + CLIENT_END_CONF.to_String()
                   + ", KEEPER_END_CONF: " + KEEPER_END_CONF.to_String() + "}";
        }
    } RPCClientKeeperConf;

    typedef struct RPCConf_
    {
        std::unordered_map<std::string, std::string> AVAIL_PROTO_CONF{};
        RPCClientVisorConf CLIENT_VISOR_CONF;
        RPCVisorKeeperConf VISOR_KEEPER_CONF;
        RPCClientKeeperConf CLIENT_KEEPER_CONF;

        [[nodiscard]] std::string to_String() const
        {
            std::string str = "AVAIL_PROTO_CONF: ";
            for (auto &proto_conf: AVAIL_PROTO_CONF)
            {
                str += proto_conf.first + "=" + proto_conf.second;
                str += ", ";
            }
            return str
                   + "CLIENT_VISOR_CONF: " + CLIENT_VISOR_CONF.to_String()
                   + ", VISOR_KEEPER_CONF: " + VISOR_KEEPER_CONF.to_String()
                   + ", CLIENT_KEEPER_CONF: " + CLIENT_KEEPER_CONF.to_String();
        }
    } RPCConf;

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

    typedef struct VisorConf_
    {
        [[nodiscard]] std::string to_String() const
        {
            return "";
        }
    } VisorConf;

    typedef struct ClientConf_
    {
        [[nodiscard]] std::string to_String() const
        {
            return "";
        }
    } ClientConf;

    typedef struct KeeperConf_
    {
        [[nodiscard]] std::string to_String() const
        {
            return "";
        }
    } KeeperConf;

    class ConfigurationManager
    {
    public:
        ChronoLogServiceRole ROLE{};
        ClockConf CLOCK_CONF{};
        ClocksourceType CLOCKSOURCE_TYPE{};
        RPCConf RPC_CONF{};
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

            /* RPC-related configurations */
            RPC_CONF.AVAIL_PROTO_CONF = {{"sockets_conf", "ofi+sockets"},
                                         {"tcp_conf",     "ofi+tcp"},
                                         {"shm_conf",     "ofi+shm"},
                                         {"verbs_conf",   "ofi+verbs"},
                                         {"verbs_domain", "mlx5_0"}};
            RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF = "ofi+sockets";
            RPC_CONF.CLIENT_VISOR_CONF.CLIENT_END_CONF.CLIENT_PORT = 4444;
            RPC_CONF.CLIENT_VISOR_CONF.CLIENT_END_CONF.CLIENT_SERVICE_THREADS = 1;
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP = "127.0.0.1";
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT = 5555;
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.SERVICE_PROVIDER_ID = 55;
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS = 1;
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_SERVICE_THREADS = 1;
            RPC_CONF.VISOR_KEEPER_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            RPC_CONF.VISOR_KEEPER_CONF.PROTO_CONF = "ofi+sockets";
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_IP = "127.0.0.1";
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_BASE_PORT = 6666;
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.SERVICE_PROVIDER_ID = 66;
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_PORTS = 1;
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_SERVICE_THREADS = 1;
            RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.KEEPER_IP = "127.0.0.1";
            RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.KEEPER_PORT = 7777;
            RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.SERVICE_PROVIDER_ID = 77;
            RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.KEEPER_SERVICE_THREADS = 1;
            RPC_CONF.CLIENT_KEEPER_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            RPC_CONF.CLIENT_KEEPER_CONF.PROTO_CONF = "ofi+sockets";
            RPC_CONF.CLIENT_KEEPER_CONF.CLIENT_END_CONF.CLIENT_PORT = 8888;
            RPC_CONF.CLIENT_KEEPER_CONF.CLIENT_END_CONF.CLIENT_SERVICE_THREADS = 1;
            RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.KEEPER_IP = "127.0.0.1";
            RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.KEEPER_PORT = 9999;
            RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.SERVICE_PROVIDER_ID = 99;
            RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.KEEPER_SERVICE_THREADS = 1;

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
            LOGI("RPC_CONF: %s", RPC_CONF.to_String().c_str());
            LOGI("AUTH_CONF: %s", AUTH_CONF.to_String().c_str());
            LOGI("VISOR_CONF: %s", VISOR_CONF.to_String().c_str());
            LOGI("CLIENT_CONF: %s", CLIENT_CONF.to_String().c_str());
            LOGI("KEEPER_CONF: %s", KEEPER_CONF.to_String().c_str());
            LOGI("******** End of configuration output ********");
        }

        void LoadConfFromJSONFile(const std::string &conf_file_path)
        {
            json_object *root = json_object_from_file(conf_file_path.c_str());
            if (root == NULL)
            {
                LOGE("Unable to open file %s, exiting ...", conf_file_path.c_str());
                exit(CL_ERR_NOT_EXIST);
            }

            json_object_object_foreach(root, key, val)
            {
                if (strcmp(key, "clock") == 0)
                {
                    json_object *clock_conf = json_object_object_get(root, "clock");
                    if (clock_conf == NULL || !json_object_is_type(clock_conf, json_type_object))
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
                else if (strcmp(key, "rpc") == 0)
                {
                    json_object *rpc_conf = json_object_object_get(root, "rpc");
                    if (rpc_conf == NULL || !json_object_is_type(rpc_conf, json_type_object))
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "rpc configuration is not an object");
                        exit(CL_ERR_INVALID_CONF);
                    }
                    json_object_object_foreach(rpc_conf, key, val)
                    {
                        if (strcmp(key, "available_protocol") == 0)
                        {
                            json_object *rpc_avail_protocol_conf = json_object_object_get(rpc_conf,
                                                                                          "available_protocol");
                            if (rpc_avail_protocol_conf == NULL ||
                                !json_object_is_type(rpc_avail_protocol_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "available_protocol configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            json_object_object_foreach(rpc_avail_protocol_conf, key, val)
                            {
                                if (strcmp(key, "sockets_conf") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        RPC_CONF.AVAIL_PROTO_CONF.emplace("sockets_conf", json_object_get_string(val));
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "sockets_conf is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "tcp_conf") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        RPC_CONF.AVAIL_PROTO_CONF.emplace("tcp_conf", json_object_get_string(val));
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "tcp_conf is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "shm_conf") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        RPC_CONF.AVAIL_PROTO_CONF.emplace("shm_conf", json_object_get_string(val));
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "shm_conf is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "verbs_conf") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        RPC_CONF.AVAIL_PROTO_CONF.emplace("verbs_conf", json_object_get_string(val));
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "verbs_conf is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "verbs_domain") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        RPC_CONF.AVAIL_PROTO_CONF.emplace("verbs_domain", json_object_get_string(val));
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "verbs_domain is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else
                                {
                                    LOGE("Unknown rpc protocol: %s", key);
                                }
                            }
                        }
                        else if (strcmp(key, "rpc_client_visor") == 0)
                        {
                            json_object *rpc_client_visor_conf = json_object_object_get(rpc_conf, "rpc_client_visor");
                            if (rpc_client_visor_conf == NULL ||
                                !json_object_is_type(rpc_client_visor_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_client_visor configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            json_object_object_foreach(rpc_client_visor_conf, key, val)
                            {
                                if (strcmp(key, "rpc_implementation") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        parseRPCImplConf(val, RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_implementation is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "protocol_conf") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF = json_object_get_string(val);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "protocol_conf is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "rpc_client_end") == 0)
                                {
                                    if (json_object_is_type(val, json_type_object))
                                    {
                                        parseClientEndConf(val, RPC_CONF.CLIENT_VISOR_CONF.CLIENT_END_CONF);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_client_end is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "rpc_visor_end") == 0)
                                {
                                    if (json_object_is_type(val, json_type_object))
                                    {
                                        parseVisorEndConf(val, RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_visor_end is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                            }
                        }
                        else if (strcmp(key, "rpc_visor_keeper") == 0)
                        {
                            json_object *rpc_visor_keeper_conf = json_object_object_get(rpc_conf, "rpc_visor_keeper");
                            if (rpc_visor_keeper_conf == NULL ||
                                !json_object_is_type(rpc_visor_keeper_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_visor_keeper configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            json_object_object_foreach(rpc_visor_keeper_conf, key, val)
                            {
                                if (strcmp(key, "rpc_implementation") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        parseRPCImplConf(val, RPC_CONF.VISOR_KEEPER_CONF.RPC_IMPLEMENTATION);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_implementation is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "protocol_conf") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        RPC_CONF.VISOR_KEEPER_CONF.PROTO_CONF = json_object_get_string(val);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "protocol_conf is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "rpc_visor_end") == 0)
                                {
                                    if (json_object_is_type(val, json_type_object))
                                    {
                                        parseVisorEndConf(val, RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_visor_end is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "rpc_keeper_end") == 0)
                                {
                                    if (json_object_is_type(val, json_type_object))
                                    {
                                        parseKeeperEndConf(val, RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_keeper_end is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                            }
                        }
                        else if (strcmp(key, "rpc_client_keeper") == 0)
                        {
                            json_object *rpc_client_keeper_conf = json_object_object_get(rpc_conf, "rpc_client_keeper");
                            if (rpc_client_keeper_conf == NULL ||
                                !json_object_is_type(rpc_client_keeper_conf, json_type_object))
                            {
                                LOGE("Error during parsing configuration file %s\n"
                                     "Error: %s\n", conf_file_path.c_str(),
                                     "rpc_client_keeper configuration is not an object");
                                exit(CL_ERR_INVALID_CONF);
                            }
                            json_object_object_foreach(rpc_client_keeper_conf, key, val)
                            {
                                if (strcmp(key, "rpc_implementation") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        parseRPCImplConf(val, RPC_CONF.CLIENT_KEEPER_CONF.RPC_IMPLEMENTATION);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_implementation is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "protocol_conf") == 0)
                                {
                                    if (json_object_is_type(val, json_type_string))
                                    {
                                        RPC_CONF.CLIENT_KEEPER_CONF.PROTO_CONF = json_object_get_string(val);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "protocol_conf is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "rpc_client_end") == 0)
                                {
                                    if (json_object_is_type(val, json_type_object))
                                    {
                                        parseClientEndConf(val, RPC_CONF.CLIENT_KEEPER_CONF.CLIENT_END_CONF);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_client_end is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                                else if (strcmp(key, "rpc_keeper_end") == 0)
                                {
                                    if (json_object_is_type(val, json_type_object))
                                    {
                                        parseKeeperEndConf(val, RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF);
                                    }
                                    else
                                    {
                                        LOGE("Error during parsing configuration file %s\n"
                                             "Error: %s\n", conf_file_path.c_str(), "rpc_keeper_end is not a string");
                                        exit(CL_ERR_INVALID_CONF);
                                    }
                                }
                            }
                        }
                    }
                }
                else if (strcmp(key, "authentication") == 0)
                {
                    json_object *auth_conf = json_object_object_get(root, "authentication");
                    if (auth_conf == NULL || !json_object_is_type(auth_conf, json_type_object))
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
                    if (json_object_is_type(val, json_type_object))
                    {
                        parseVisorLocalConf(val);
                    }
                    else
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "chrono_visor is not an object");
                        exit(CL_ERR_INVALID_CONF);
                    }
                }
                else if (strcmp(key, "chrono_client") == 0)
                {
                    if (json_object_is_type(val, json_type_object))
                    {
                        parseClientLocalConf(val);
                    }
                    else
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "chrono_client is not an object");
                        exit(CL_ERR_INVALID_CONF);
                    }
                }
                else if (strcmp(key, "chrono_keeper") == 0)
                {
                    if (json_object_is_type(val, json_type_object))
                    {
                        parseKeeperLocalConf(val);
                    }
                    else
                    {
                        LOGE("Error during parsing configuration file %s\n"
                             "Error: %s\n", conf_file_path.c_str(), "chrono_keeper is not an object");
                        exit(CL_ERR_INVALID_CONF);
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

        void parseClientEndConf(json_object *json_conf, RPCClientEndConf &rpc_conf)
        {
            assert(json_object_is_type(json_conf, json_type_object));
            json_object_object_foreach(json_conf, key, val)
            {
                if (strcmp(key, "client_port") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.CLIENT_PORT = json_object_get_int(val);
                }
                else if (strcmp(key, "client_service_threads") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.CLIENT_SERVICE_THREADS = json_object_get_int(val);
                }
                else
                {
                    LOGE("Unknown client end configuration: %s", key);
                }
            }
        }

        void parseVisorEndConf(json_object *json_conf, RPCVisorEndConf &rpc_conf)
        {
            assert(json_object_is_type(json_conf, json_type_object));
            json_object_object_foreach(json_conf, key, val)
            {
                if (strcmp(key, "visor_ip") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    rpc_conf.VISOR_IP = json_object_get_string(val);
                }
                else if (strcmp(key, "visor_base_port") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.VISOR_BASE_PORT = json_object_get_int(val);
                }
                else if (strcmp(key, "service_provider_id") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.SERVICE_PROVIDER_ID = json_object_get_int(val);
                }
                else if (strcmp(key, "visor_ports") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.VISOR_PORTS = json_object_get_int(val);
                }
                else if (strcmp(key, "visor_service_threads") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.VISOR_SERVICE_THREADS = json_object_get_int(val);
                }
                else
                {
                    LOGE("Unknown visor end configuration: %s", key);
                }
            }
        }

        void parseKeeperEndConf(json_object *json_conf, RPCKeeperEndConf &rpc_conf)
        {
            assert(json_object_is_type(json_conf, json_type_object));
            json_object_object_foreach(json_conf, key, val)
            {
                if (strcmp(key, "keeper_ip") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    rpc_conf.KEEPER_IP = json_object_get_string(val);
                }
                else if (strcmp(key, "keeper_port") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.KEEPER_PORT = json_object_get_int(val);
                }
                else if (strcmp(key, "service_provider_id") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.SERVICE_PROVIDER_ID = json_object_get_int(val);
                }
                else if (strcmp(key, "keeper_service_threads") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    rpc_conf.KEEPER_SERVICE_THREADS = json_object_get_int(val);
                }
                else
                {
                    LOGE("Unknown keeper end configuration: %s", key);
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
