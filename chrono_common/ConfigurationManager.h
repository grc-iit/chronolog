#ifndef CHRONOLOG_CONFIGURATIONMANAGER_H
#define CHRONOLOG_CONFIGURATIONMANAGER_H

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <rapidjson/document.h>
#include <sstream>
#include "data_structures.h"
#include "enum.h"
#include "log.h"
#include "errcode.h"

namespace ChronoLog {
    typedef struct ClockConf_ {
        ClocksourceType CLOCKSOURCE_TYPE;
        uint64_t DRIFT_CAL_SLEEP_SEC;
        uint64_t DRIFT_CAL_SLEEP_NSEC;

        [[nodiscard]] std::string to_String() const {
            return "CLOCKSOURCE_TYPE: " + std::string(getClocksourceTypeString(CLOCKSOURCE_TYPE))
                    + ", DRIFT_CAL_SLEEP_SEC: " + std::to_string(DRIFT_CAL_SLEEP_SEC)
                    + ", DRIFT_CAL_SLEEP_NSEC: " + std::to_string(DRIFT_CAL_SLEEP_NSEC);
        }
    } ClockConf;

    typedef struct RPCClientEndConf_ {
        uint16_t CLIENT_PORT;
        uint8_t CLIENT_SERVICE_THREADS;

        [[nodiscard]] std::string to_String() const {
            return "[CLIENT_PORT: " + std::to_string(CLIENT_PORT)
                    + ", CLIENT_SERVICE_THREADS: " + std::to_string(CLIENT_SERVICE_THREADS) + "]";
        }
    } RPCClientEndConf;

    typedef struct RPCVisorEndConf_ {
        ChronoLogCharStruct VISOR_IP;
        uint16_t VISOR_BASE_PORT;
        uint8_t VISOR_PORTS;
        uint8_t VISOR_SERVICE_THREADS;

        [[nodiscard]] std::string to_String() const {
            return "[VISOR_IP: " + VISOR_IP.string()
                    + ", VISOR_BASE_PORT: " + std::to_string(VISOR_BASE_PORT)
                    + ", VISOR_PORTS: " + std::to_string(VISOR_PORTS)
                    + ", VISOR_SERVICE_THREADS: " + std::to_string(VISOR_SERVICE_THREADS) + "]";
        }
    } RPCVisorEndConf;

    typedef struct RPCKeeperEndConf_ {
        ChronoLogCharStruct KEEPER_IP;
        uint16_t KEEPER_PORT;
        uint8_t KEEPER_SERVICE_THREADS;

        [[nodiscard]] std::string to_String() const {
            return "[KEEPER_IP: " + KEEPER_IP.string()
                    + ", KEEPER_PORT: " + std::to_string(KEEPER_PORT)
                    + ", KEEPER_SERVICE_THREADS: " + std::to_string(KEEPER_SERVICE_THREADS) + "]";
        }
    } RPCKeeperEndConf;

    typedef struct RPCClientVisorConf_ {
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        ChronoLogCharStruct PROTO_CONF;
        RPCClientEndConf CLIENT_END_CONF;
        RPCVisorEndConf VISOR_END_CONF;

        [[nodiscard]] std::string to_String() const {
            return "{RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION))
                   + ", PROTO_CONF: " + PROTO_CONF.string()
                   + ", CLIENT_END_CONF: " + CLIENT_END_CONF.to_String()
                   + ", VISOR_END_CONF: " + VISOR_END_CONF.to_String() + "}";
        }
    } RPCClientVisorConf;

    typedef struct RPCVisorKeeperConf_ {
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        ChronoLogCharStruct PROTO_CONF;
        RPCVisorEndConf VISOR_END_CONF;
        RPCKeeperEndConf KEEPER_END_CONF;

        [[nodiscard]] std::string to_String() const {
            return "{RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION))
                    + ", PROTO_CONF: " + PROTO_CONF.string()
                    + ", VISOR_END_CONF: " + VISOR_END_CONF.to_String()
                    + ", KEEPER_END_CONF: " + KEEPER_END_CONF.to_String() + "}";
        }
    } RPCVisorKeeperConf;

    typedef struct RPCClientKeeperConf_ {
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        ChronoLogCharStruct PROTO_CONF;
        RPCClientEndConf CLIENT_END_CONF;
        RPCKeeperEndConf KEEPER_END_CONF;

        [[nodiscard]] std::string to_String() const {
            return "{RPC_IMPLEMENTATION: " + std::string(getRPCImplString(RPC_IMPLEMENTATION))
                   + ", PROTO_CONF: " + PROTO_CONF.string()
                   + ", CLIENT_END_CONF: " + CLIENT_END_CONF.to_String()
                   + ", KEEPER_END_CONF: " + KEEPER_END_CONF.to_String() + "}";
        }
    } RPCClientKeeperConf;

    typedef struct RPCConf_ {
        std::unordered_map<ChronoLogCharStruct, ChronoLogCharStruct> AVAIL_PROTO_CONF{};
        RPCClientVisorConf CLIENT_VISOR_CONF;
        RPCVisorKeeperConf VISOR_KEEPER_CONF;
        RPCClientKeeperConf CLIENT_KEEPER_CONF;

        [[nodiscard]] std::string to_String() const {
            std::string str = "AVAIL_PROTO_CONF: ";
            for (auto &proto_conf : AVAIL_PROTO_CONF) {
                str += proto_conf.first.string() + "=" + proto_conf.second.string();
                str += ", ";
            }
            return str
                   + "CLIENT_VISOR_CONF: " + CLIENT_VISOR_CONF.to_String()
                   + ", VISOR_KEEPER_CONF: " + VISOR_KEEPER_CONF.to_String()
                   + ", CLIENT_KEEPER_CONF: " + CLIENT_KEEPER_CONF.to_String();
        }
    } RPCConf;

    typedef struct AuthConf_ {
        ChronoLogCharStruct AUTH_TYPE;
        ChronoLogCharStruct MODULE_PATH;

        [[nodiscard]] std::string to_String() const {
            return "AUTH_TYPE: " + AUTH_TYPE.string()
                   + ", MODULE_PATH: " + MODULE_PATH.string();
        }
    } AuthConf;

    typedef struct VisorConf_ {
        [[nodiscard]] std::string to_String() const {
            return "";
        }
    } VisorConf;

    typedef struct ClientConf_ {
        [[nodiscard]] std::string to_String() const {
            return "";
        }
    } ClientConf;

    typedef struct KeeperConf_ {
        [[nodiscard]] std::string to_String() const {
            return "";
        }
    } KeeperConf;

    class ConfigurationManager {
    public:
        ChronoLogServiceRole ROLE{};
        ClockConf CLOCK_CONF{};
        ClocksourceType CLOCKSOURCE_TYPE{};
        RPCConf RPC_CONF{};
        AuthConf AUTH_CONF{};
        VisorConf VISOR_CONF{};
        ClientConf CLIENT_CONF{};
        KeeperConf KEEPER_CONF{};

        ConfigurationManager() {
            LOGI("constructing configuration with all default values: ");
            ROLE = CHRONOLOG_UNKNOWN;

            /* Clock-related configurations */
            CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;
            CLOCK_CONF.DRIFT_CAL_SLEEP_SEC = 10;
            CLOCK_CONF.DRIFT_CAL_SLEEP_NSEC = 0;
            CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;

            /* RPC-related configurations */
            RPC_CONF.AVAIL_PROTO_CONF = { {"sockets_conf", "ofi+sockets"},
                                          {"tcp_conf", "ofi+tcp"},
                                          {"shm_conf", "ofi+shm"},
                                          {"verbs_conf", "ofi+verbs"},
                                          {"verbs_domain", "mlx5_0"}};
            RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF = "ofi+sockets";
            RPC_CONF.CLIENT_VISOR_CONF.CLIENT_END_CONF.CLIENT_PORT = 4444;
            RPC_CONF.CLIENT_VISOR_CONF.CLIENT_END_CONF.CLIENT_SERVICE_THREADS = 1;
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP = "127.0.0.1";
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT = 5555;
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS = 1;
            RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_SERVICE_THREADS = 1;
            RPC_CONF.VISOR_KEEPER_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            RPC_CONF.VISOR_KEEPER_CONF.PROTO_CONF = "ofi+sockets";
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_IP = "127.0.0.1";
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_BASE_PORT = 6666;
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_PORTS = 1;
            RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_SERVICE_THREADS = 1;
            RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.KEEPER_IP = "127.0.0.1";
            RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.KEEPER_PORT = 7777;
            RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.KEEPER_SERVICE_THREADS = 1;
            RPC_CONF.CLIENT_KEEPER_CONF.RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
            RPC_CONF.CLIENT_KEEPER_CONF.PROTO_CONF = "ofi+sockets";
            RPC_CONF.CLIENT_KEEPER_CONF.CLIENT_END_CONF.CLIENT_PORT = 8888;
            RPC_CONF.CLIENT_KEEPER_CONF.CLIENT_END_CONF.CLIENT_SERVICE_THREADS = 1;
            RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.KEEPER_IP = "127.0.0.1";
            RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.KEEPER_PORT = 9999;
            RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.KEEPER_SERVICE_THREADS = 1;

            /* Authentication-related configurations */
            AUTH_CONF.AUTH_TYPE = "RBAC";
            AUTH_CONF.MODULE_PATH = "";

            /* Visor-local configurations */

            /* Client-local configurations */

            /* Keeper-local configurations */

            PrintConf();
        }

        explicit ConfigurationManager(const std::string &conf_file_path) {
            LOGI("constructing configuration from a configuration file: %s", conf_file_path.c_str());
            LoadConfFromJSONFile(conf_file_path);
        }

        void SetConfiguration(const ConfigurationManager &confManager) {
            LOGI("setting configuration with a pre-configured ConfigurationManager object");
            ROLE = confManager.ROLE;
            CLOCK_CONF = confManager.CLOCK_CONF;
            RPC_CONF = confManager.RPC_CONF;
            AUTH_CONF = confManager.AUTH_CONF;
            VISOR_CONF = confManager.VISOR_CONF;
            CLIENT_CONF = confManager.CLIENT_CONF;
            KEEPER_CONF = confManager.KEEPER_CONF;
            LOGI("updated configuration:");
            PrintConf();
        }

        void PrintConf() const {
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

        void LoadConfFromJSONFile(const std::string& conf_file_path) {
            std::ifstream conf_file_stream(conf_file_path);
            if (!conf_file_stream.is_open()) {
                LOGE("Unable to open file %s, exiting ...", conf_file_path.c_str());
                exit(CL_ERR_NOT_EXIST);
            }

            std::stringstream contents;
            contents << conf_file_stream.rdbuf();
            rapidjson::Document doc;
            doc.Parse(contents.str().c_str());
            if (doc.HasParseError()) {
                LOGE("Error during parsing configuration file %s\n"
                     "Error: %d\n"
                     "at: %lu", conf_file_path.c_str(), doc.GetParseError(), doc.GetErrorOffset());
                exit(CL_ERR_INVALID_CONF);
            }

            for (auto& item : doc.GetObject()) {
                auto item_name = item.name.GetString();
                if (strcmp(item_name, "clock") == 0) {
                    rapidjson::Value& clock_conf = doc["clock"];
                    assert(clock_conf.IsObject());
                    for (auto m = clock_conf.MemberBegin(); m != clock_conf.MemberEnd(); ++m) {
                        assert(m->name.IsString());
                        if (strcmp(m->name.GetString(), "clocksource_type") == 0) {
                            assert(m->value.IsString());
                            if (strcmp(m->value.GetString(), "C_STYLE") == 0)
                                CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;
                            else if (strcmp(m->value.GetString(), "CPP_STYLE") == 0)
                                CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::CPP_STYLE;
                            else if (strcmp(m->value.GetString(), "TSC") == 0)
                                CLOCK_CONF.CLOCKSOURCE_TYPE = ClocksourceType::TSC;
                            else
                                LOGI("Unknown clocksource type: %s", m->value.GetString());
                        } else if (strcmp(m->name.GetString(), "drift_cal_sleep_sec") == 0) {
                            assert(m->value.IsInt());
                            CLOCK_CONF.DRIFT_CAL_SLEEP_SEC = m->value.GetInt();
                        } else if (strcmp(m->name.GetString(), "drift_cal_sleep_nsec") == 0) {
                            assert(m->value.IsInt());
                            CLOCK_CONF.DRIFT_CAL_SLEEP_NSEC = m->value.GetInt();
                        }
                    }
                } else if (strcmp(item_name, "rpc") == 0) {
                    rapidjson::Value& rpc_conf = doc["rpc"];
                    assert(item.value.IsObject());
                    for (auto m = rpc_conf.MemberBegin(); m != rpc_conf.MemberEnd(); ++m) {
                        assert(m->name.IsString());
                        if (strcmp(m->name.GetString(), "available_protocol") == 0) {
                            rapidjson::Value& rpc_avail_protocol_conf = doc["rpc"]["available_protocol"];
                            assert(rpc_avail_protocol_conf.IsObject());
                            for (auto n = rpc_avail_protocol_conf.MemberBegin();
                                    n != rpc_avail_protocol_conf.MemberEnd();
                                    ++n) {
                                assert(n->name.IsString());
                                if (strcmp(n->name.GetString(), "sockets_conf") == 0) {
                                    assert(n->value.IsString());
                                    RPC_CONF.AVAIL_PROTO_CONF.emplace("sockets_conf", n->value.GetString());
                                } else if (strcmp(n->name.GetString(), "tcp_conf") == 0) {
                                    assert(n->value.IsString());
                                    RPC_CONF.AVAIL_PROTO_CONF.emplace("tcp_conf", n->value.GetString());
                                } else if (strcmp(n->name.GetString(), "shm_conf") == 0) {
                                    assert(n->value.IsString());
                                    RPC_CONF.AVAIL_PROTO_CONF.emplace("shm_conf", n->value.GetString());
                                } else if (strcmp(n->name.GetString(), "verbs_conf") == 0) {
                                    assert(n->value.IsString());
                                    RPC_CONF.AVAIL_PROTO_CONF.emplace("verbs_conf", n->value.GetString());
                                } else if (strcmp(n->name.GetString(), "verbs_domain") == 0) {
                                    assert(n->value.IsString());
                                    RPC_CONF.AVAIL_PROTO_CONF.emplace("verbs_domain", n->value.GetString());
                                } else
                                    LOGE("Unknown rpc protocol: %s", n->name.GetString());
                            }
                        } else if (strcmp(m->name.GetString(), "rpc_client_visor") == 0) {
                            rapidjson::Value& rpc_client_visor_conf = doc["rpc"]["rpc_client_visor"];
                            assert(rpc_client_visor_conf.IsObject());
                            for (auto n = rpc_client_visor_conf.MemberBegin();
                                    n != rpc_client_visor_conf.MemberEnd();
                                    ++n) {
                                assert(n->name.IsString());
                                if (strcmp(n->name.GetString(), "rpc_implementation") == 0) {
                                    parseRPCImplConf(n->value,
                                                     RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION);
                                } else if (strcmp(n->name.GetString(), "protocol_conf") == 0) {
                                    assert(n->value.IsString());
                                    RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF = n->value.GetString();
                                } else if (strcmp(n->name.GetString(), "rpc_client_end") == 0) {
                                    rapidjson::Value& rpc_client_end_conf =
                                            doc["rpc"]["rpc_client_visor"]["rpc_client_end"];
                                    parseClientEndConf(rpc_client_end_conf, RPC_CONF.CLIENT_VISOR_CONF.CLIENT_END_CONF);
                                } else if (strcmp(n->name.GetString(), "rpc_visor_end") == 0) {
                                    rapidjson::Value& rpc_visor_end_conf =
                                            doc["rpc"]["rpc_client_visor"]["rpc_visor_end"];
                                    parseVisorEndConf(rpc_visor_end_conf, RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF);
                                } else
                                    LOGE("Unknown configuration for client-visor rpc: %s", n->name.GetString());
                            }
                        } else if (strcmp(m->name.GetString(), "rpc_visor_keeper") == 0) {
                            rapidjson::Value& rpc_visor_keeper_conf = doc["rpc"]["rpc_visor_keeper"];
                            assert(rpc_visor_keeper_conf.IsObject());
                            for (auto n = rpc_visor_keeper_conf.MemberBegin();
                                 n != rpc_visor_keeper_conf.MemberEnd();
                                 ++n) {
                                assert(n->name.IsString());
                                if (strcmp(n->name.GetString(), "rpc_implementation") == 0) {
                                    parseRPCImplConf(n->value, RPC_CONF.VISOR_KEEPER_CONF.RPC_IMPLEMENTATION);
                                } else if (strcmp(n->name.GetString(), "protocol_conf") == 0) {
                                    assert(n->value.IsString());
                                    RPC_CONF.VISOR_KEEPER_CONF.PROTO_CONF = n->value.GetString();
                                } else if (strcmp(n->name.GetString(), "rpc_visor_end") == 0) {
                                    rapidjson::Value& rpc_visor_end_conf =
                                            doc["rpc"]["rpc_visor_keeper"]["rpc_visor_end"];
                                    parseVisorEndConf(rpc_visor_end_conf, RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF);
                                } else if (strcmp(n->name.GetString(), "rpc_keeper_end") == 0) {
                                    rapidjson::Value& rpc_keeper_end_conf =
                                            doc["rpc"]["rpc_visor_keeper"]["rpc_keeper_end"];
                                    parseKeeperEndConf(rpc_keeper_end_conf, RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF);
                                } else
                                    LOGE("Unknown configuration for visor-keeper rpc: %s", n->name.GetString());
                            }
                        } else if (strcmp(m->name.GetString(), "rpc_client_keeper") == 0) {
                            rapidjson::Value& rpc_client_keeper_conf = doc["rpc"]["rpc_client_keeper"];
                            assert(rpc_client_keeper_conf.IsObject());
                            for (auto n = rpc_client_keeper_conf.MemberBegin();
                                 n != rpc_client_keeper_conf.MemberEnd();
                                 ++n) {
                                assert(n->name.IsString());
                                if (strcmp(n->name.GetString(), "rpc_implementation") == 0) {
                                    parseRPCImplConf(n->value, RPC_CONF.CLIENT_KEEPER_CONF.RPC_IMPLEMENTATION);
                                } else if (strcmp(n->name.GetString(), "protocol_conf") == 0) {
                                    assert(n->value.IsString());
                                    RPC_CONF.CLIENT_KEEPER_CONF.PROTO_CONF = n->value.GetString();
                                } else if (strcmp(n->name.GetString(), "rpc_client_end") == 0) {
                                    rapidjson::Value& rpc_client_end_conf =
                                            doc["rpc"]["rpc_client_keeper"]["rpc_client_end"];
                                    parseClientEndConf(rpc_client_end_conf, RPC_CONF.CLIENT_KEEPER_CONF.CLIENT_END_CONF);
                                } else if (strcmp(n->name.GetString(), "rpc_keeper_end") == 0) {
                                    rapidjson::Value& rpc_keeper_end_conf =
                                            doc["rpc"]["rpc_client_keeper"]["rpc_keeper_end"];
                                    parseKeeperEndConf(rpc_keeper_end_conf, RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF);
                                } else
                                    LOGE("Unknown configuration for client-keeper rpc: %s", n->name.GetString());
                            }
                        } else
                            LOGE("Unknown rpc configuration: %s", m->value.GetString());
                    }
                } else if (strcmp(item_name, "authentication") == 0) {
                    rapidjson::Value& auth_conf = doc["authentication"];
                    assert(item.value.IsObject());
                    for (auto m = auth_conf.MemberBegin(); m != auth_conf.MemberEnd(); ++m) {
                        assert(m->name.IsString());
                        if (strcmp(m->name.GetString(), "auth_type") == 0) {
                            AUTH_CONF.AUTH_TYPE = m->value.GetString();
                        } else if (strcmp(m->name.GetString(), "module_location") == 0) {
                            AUTH_CONF.MODULE_PATH = m->value.GetString();
                        } else
                            LOGE("Unknown rpc configuration: %s", m->value.GetString());
                    }
                } else if (strcmp(item_name, "chrono_visor") == 0) {
                    rapidjson::Value& visor_local_conf = doc["chrono_visor"];
                    parseVisorLocalConf(visor_local_conf);
                    assert(item.value.IsObject());
                } else if (strcmp(item_name, "chrono_client") == 0) {
                    rapidjson::Value& client_local_conf = doc["chrono_client"];
                    parseClientLocalConf(client_local_conf);
                    assert(item.value.IsObject());
                } else if (strcmp(item_name, "chrono_keeper") == 0) {
                    rapidjson::Value& keeper_local_conf = doc["chrono_keeper"];
                    parseKeeperLocalConf(keeper_local_conf);
                    assert(item.value.IsObject());
                } else {
                    LOGE("Unknown configuration: %s", item_name);
                }
            }
            LOGI("Finish loading configurations from file %s, new configurations: ", conf_file_path.c_str());
            PrintConf();
        }

    private:
        void parseRPCImplConf(rapidjson::Value &json_conf, ChronoLogRPCImplementation &rpc_impl) {
            assert(json_conf.IsString());
            if (strcmp(json_conf.GetString(), "Thallium_sockets") == 0) {
                rpc_impl = CHRONOLOG_THALLIUM_SOCKETS;
            } else if (strcmp(json_conf.GetString(), "Thallium_tcp") == 0) {
                rpc_impl = CHRONOLOG_THALLIUM_TCP;
            } else if (strcmp(json_conf.GetString(), "Thallium_roce") == 0) {
                rpc_impl = CHRONOLOG_THALLIUM_ROCE;
            } else
                LOGE("Unknown rpc implementation: %s", json_conf.GetString());
        }

        void parseClientEndConf(rapidjson::Value &json_conf, RPCClientEndConf &rpc_conf) {
            assert(json_conf.IsObject());
            for (auto i = json_conf.MemberBegin(); i != json_conf.MemberEnd(); ++i) {
                assert(i->name.IsString());
                if (strcmp(i->name.GetString(), "client_port") == 0) {
                    assert(i->value.IsInt());
                    rpc_conf.CLIENT_PORT = i->value.GetInt();
                } else if (strcmp(i->name.GetString(), "client_service_threads") == 0) {
                    assert(i->value.IsInt());
                    rpc_conf.CLIENT_SERVICE_THREADS = i->value.GetInt();
                } else
                    LOGE("Unknown client end configuration: %s", i->name.GetString());
            }
        }

        void parseVisorEndConf(rapidjson::Value &json_conf, RPCVisorEndConf &rpc_conf) {
            assert(json_conf.IsObject());
            for (auto i = json_conf.MemberBegin(); i != json_conf.MemberEnd(); ++i) {
                assert(i->name.IsString());
                if (strcmp(i->name.GetString(), "visor_ip") == 0) {
                    assert(i->value.IsString());
                    rpc_conf.VISOR_IP = i->value.GetString();
                } else if (strcmp(i->name.GetString(), "visor_base_port") == 0) {
                    assert(i->value.IsInt());
                    rpc_conf.VISOR_BASE_PORT = i->value.GetInt();
                } else if (strcmp(i->name.GetString(), "visor_ports") == 0) {
                    assert(i->value.IsInt());
                    rpc_conf.VISOR_PORTS = i->value.GetInt();
                } else if (strcmp(i->name.GetString(), "visor_service_threads") == 0) {
                    assert(i->value.IsInt());
                    rpc_conf.VISOR_SERVICE_THREADS = i->value.GetInt();
                } else
                    LOGE("Unknown visor end configuration: %s", i->name.GetString());
            }
        }

        void parseKeeperEndConf(rapidjson::Value &json_conf, RPCKeeperEndConf &rpc_conf) {
            assert(json_conf.IsObject());
            for (auto i = json_conf.MemberBegin(); i != json_conf.MemberEnd(); ++i) {
                assert(i->name.IsString());
                if (strcmp(i->name.GetString(), "keeper_ip") == 0) {
                    assert(i->value.IsString());
                    rpc_conf.KEEPER_IP = i->value.GetString();
                } else if (strcmp(i->name.GetString(), "keeper_port") == 0) {
                    assert(i->value.IsInt());
                    rpc_conf.KEEPER_PORT = i->value.GetInt();
                } else if (strcmp(i->name.GetString(), "keeper_service_threads") == 0) {
                    assert(i->value.IsInt());
                    rpc_conf.KEEPER_SERVICE_THREADS = i->value.GetInt();
                } else
                    LOGE("Unknown keeper end configuration: %s", i->name.GetString());
            }
        }
        void parseVisorLocalConf(rapidjson::Value &conf) {}
        void parseClientLocalConf(rapidjson::Value &conf) {}
        void parseKeeperLocalConf(rapidjson::Value &conf) {}
    };
}

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
