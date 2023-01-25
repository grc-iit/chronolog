//
// Created by kfeng on 3/30/22.
//

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
#include "ClocksourceManager.h"

namespace ChronoLog {
    class ConfigurationManager {
    public:
        ChronoLogCharStruct RPC_VISOR_IP;
        uint16_t RPC_BASE_VISOR_PORT{};
        uint16_t RPC_NUM_VISOR_PORTS{};
        uint16_t RPC_NUM_VISOR_SERVICE_THREADS{};
        uint16_t RPC_CLIENT_PORT{};
        uint16_t RPC_NUM_CLIENT_SERVICE_THREADS{};
        ChronoLogRPCImplementation RPC_IMPLEMENTATION;
        ChronoLogCharStruct SOCKETS_CONF;
        ChronoLogCharStruct VERBS_CONF;
        ChronoLogCharStruct VERBS_DOMAIN;
        uint64_t MEMORY_ALLOCATED{};
        ClocksourceType CLOCKSOURCE{};
        uint64_t DRIFT_CAL_SLEEP_SEC{};
        uint64_t DRIFT_CAL_SLEEP_NSEC{};

        bool IS_VISOR{};
        int MY_VISOR_ID{};
        ChronoLogCharStruct SERVER_LIST_FILE_PATH;
        std::vector<ChronoLogCharStruct> SERVER_LIST;
        ChronoLogCharStruct BACKED_FILE_DIR;

        ConfigurationManager() :
                RPC_VISOR_IP("127.0.0.1"),
                RPC_BASE_VISOR_PORT(5555),
                RPC_NUM_VISOR_PORTS(1),
                RPC_NUM_VISOR_SERVICE_THREADS(1),
                RPC_CLIENT_PORT(6666),
                RPC_NUM_CLIENT_SERVICE_THREADS(1),
                RPC_IMPLEMENTATION(CHRONOLOG_THALLIUM_SOCKETS),
                SOCKETS_CONF("ofi+sockets"), VERBS_CONF("verbs"), VERBS_DOMAIN("mlx5_0"),
                MEMORY_ALLOCATED(1024ULL * 1024ULL * 128ULL),
                CLOCKSOURCE(ClocksourceType::C_STYLE), DRIFT_CAL_SLEEP_SEC(10), DRIFT_CAL_SLEEP_NSEC(0),
                IS_VISOR(true), MY_VISOR_ID(0), SERVER_LIST_FILE_PATH(""),
                SERVER_LIST({"localhost"}), BACKED_FILE_DIR("/dev/shm") {
            LOGI("initializing configuration with all default values: ");
            ClocksourceManager::getInstance()->setClocksourceType(CLOCKSOURCE);
            PrintConf();
        }

        explicit ConfigurationManager(const std::string &conf_file_path) :
                RPC_IMPLEMENTATION(CHRONOLOG_THALLIUM_SOCKETS){
            LOGI("initializing configuration from a configuration file: %s", conf_file_path.c_str());
            LoadConfFromJSONFile(conf_file_path);
        }

        void SetConfiguration(const ConfigurationManager &confManager) {
            RPC_VISOR_IP = confManager.RPC_VISOR_IP;
            RPC_BASE_VISOR_PORT = confManager.RPC_BASE_VISOR_PORT;
            RPC_NUM_VISOR_PORTS = confManager.RPC_NUM_VISOR_PORTS;
            RPC_NUM_VISOR_SERVICE_THREADS = confManager.RPC_NUM_VISOR_SERVICE_THREADS;
            RPC_CLIENT_PORT = confManager.RPC_CLIENT_PORT;
            RPC_NUM_CLIENT_SERVICE_THREADS = confManager.RPC_NUM_CLIENT_SERVICE_THREADS;
            RPC_IMPLEMENTATION = confManager.RPC_IMPLEMENTATION;
            SOCKETS_CONF = confManager.SOCKETS_CONF;
            VERBS_CONF = confManager.VERBS_CONF;
            VERBS_DOMAIN = confManager.VERBS_DOMAIN;
            MEMORY_ALLOCATED = confManager.MEMORY_ALLOCATED;
            DRIFT_CAL_SLEEP_SEC = confManager.DRIFT_CAL_SLEEP_SEC;
            DRIFT_CAL_SLEEP_NSEC = confManager.DRIFT_CAL_SLEEP_NSEC;
            IS_VISOR = confManager.IS_VISOR;
            MY_VISOR_ID = confManager.MY_VISOR_ID;
            SERVER_LIST_FILE_PATH = confManager.SERVER_LIST_FILE_PATH;
            SERVER_LIST = confManager.SERVER_LIST;
            BACKED_FILE_DIR = confManager.BACKED_FILE_DIR;
            ClocksourceManager::getInstance()->setClocksourceType(CLOCKSOURCE);
            LOGI("set configuration with a pre-configured ConfigurationManager object");
            PrintConf();
        }

        void PrintConf() {
            LOGI("******** Start of configuration output ********");
            LOGI("RPC_VISOR_IP: %s", RPC_VISOR_IP.c_str());
            LOGI("RPC_BASE_PORT: %d", RPC_BASE_VISOR_PORT);
            LOGI("RPC_NUM_PORTS: %d", RPC_NUM_VISOR_PORTS);
            LOGI("RPC_NUM_VISOR_SERVICE_THREADS: %d", RPC_NUM_VISOR_SERVICE_THREADS);
            LOGI("RPC_CLIENT_PORT: %d", RPC_CLIENT_PORT);
            LOGI("RPC_NUM_CLIENT_SERVICE_THREADS: %d", RPC_NUM_CLIENT_SERVICE_THREADS);
            LOGI("RPC_IMPLEMENTATION (0 for sockets, 1 for TCP, 2 for verbs): %d", RPC_IMPLEMENTATION);
            LOGI("SOCKETS_CONF: %s", SOCKETS_CONF.c_str());
            LOGI("VERBS_CONF: %s", VERBS_CONF.c_str());
            LOGI("VERBS_DOMAIN: %s", VERBS_DOMAIN.c_str());
            LOGI("MEMORY_ALLOCATED: %lu", MEMORY_ALLOCATED);
            LOGI("CLOCKSOURCE: %d (0 for C, 1 for C++, 2 for TSC)", CLOCKSOURCE);
            LOGI("DRIFT_CAL_SLEEP_SEC: %ld", DRIFT_CAL_SLEEP_SEC);
            LOGI("DRIFT_CAL_SLEEP_NSEC: %ld", DRIFT_CAL_SLEEP_NSEC);
            LOGI("IS_VISOR: %d", IS_VISOR);
            LOGI("MY_VISOR_ID: %d", MY_VISOR_ID);
            LOGI("SERVER_LIST_FILE_PATH: %s", realpath(SERVER_LIST_FILE_PATH.c_str(), NULL));
            LOGI("SERVER_LIST (overwrites what's in SERVER_LIST_FILE_PATH): ");
            for (const auto& server : SERVER_LIST)
                LOGI("%s ", server.c_str());
            LOGI("BACKED_FILE_DIR: %s", BACKED_FILE_DIR.c_str());
            LOGI("******** End of configuration output ********");
        }

        std::vector<ChronoLogCharStruct> LoadServers() {
            SERVER_LIST = std::vector<ChronoLogCharStruct>();
            std::fstream file;
            file.open(SERVER_LIST_FILE_PATH.c_str(), std::ios::in);
            if (file.is_open()) {
                std::string file_line;
                std::string server_node_name;
                while (getline(file, file_line)) {
                    // TODO: check if hostname is valid
                    auto colon_pos = file_line.find(':');
                    if (colon_pos == std::string::npos) {
                        // no port specified
                        SERVER_LIST.emplace_back(file_line);
                    } else {
                        // it has something after :, ignore things after :, custom port is not supported
                        // TODO: support different ports for different servers
                        SERVER_LIST.emplace_back(file_line.substr(0, colon_pos));
                    }

                }
                /*
                 * configuration file reading from HCL which uses MPI
                 * the serverList_ file follows the format of a hostfile for MPI
                 * each line contains a hostname and a number of processes for each node, separated by ':'
                 * e.g. ares-comp-05:4, where ares-comp-05 is the hostname of the server
                 * 4 is the number of processes on that node
                 */
                /*
                int count;
                while (getline(file, file_line)) {
                    unsigned long split_loc = file_line.find(':');  // split to node and count
                    if (split_loc != std::string::npos) {
                        server_node_name = file_line.substr(0, split_loc);
                        count = atoi(file_line.substr(split_loc + 1, std::string::npos).c_str());
                    } else {
                        // no special count
                        server_node_name = file_line;
                        count = 1;
                    }
                    // server list is list of network interfaces
                    for (int i = 0; i < count; ++i) {
                        SERVER_LIST.emplace_back(server_node_name);
                    }
                }
                */
            } else {
                LOGE("Error: Can't open server list file %s\n", SERVER_LIST_FILE_PATH.c_str());
                exit(EXIT_FAILURE);
            }
            file.close();
            return SERVER_LIST;
        }

        void ConfigureDefaultClient() {
            IS_VISOR = false;
            LOGD("default configuration for client is loaded, changed configuration:");
            LOGD("IS_VISOR: %d", IS_VISOR);
        }

        void ConfigureDefaultServer() {
            IS_VISOR = true;
            LOGD("default configuration for server is loaded, changed configuration:");
            LOGD("IS_VISOR: %d", IS_VISOR);
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
                if (strcmp(item_name, "rpc_server_ip") == 0) {
                    assert(item.value.IsString());
                    RPC_VISOR_IP = item.value.GetString();
                }
                else if (strcmp(item_name, "rpc_base_server_port") == 0) {
                    assert(item.value.IsInt());
                    RPC_BASE_VISOR_PORT = item.value.GetInt();
                }
                else if (strcmp(item_name, "rpc_num_server_ports") == 0) {
                    assert(item.value.IsInt());
                    RPC_NUM_VISOR_PORTS = item.value.GetInt();
                }
                else if (strcmp(item_name, "rpc_num_visor_service_threads") == 0) {
                    assert(item.value.IsInt());
                    RPC_NUM_VISOR_SERVICE_THREADS = item.value.GetInt();
                }
                else if (strcmp(item_name, "rpc_client_port") == 0) {
                    assert(item.value.IsInt());
                    RPC_CLIENT_PORT = item.value.GetInt();
                }
                else if (strcmp(item_name, "rpc_num_client_service_threads") == 0) {
                    assert(item.value.IsInt());
                    RPC_NUM_CLIENT_SERVICE_THREADS = item.value.GetInt();
                }
                else if (strcmp(item_name, "rpc_implementation") == 0) {
                    assert(item.value.IsInt());
                    RPC_IMPLEMENTATION = static_cast<ChronoLogRPCImplementation>(item.value.GetInt());
                }
                else if (strcmp(item_name, "sockets_conf") == 0) {
                    assert(item.value.IsString());
                    SOCKETS_CONF = item.value.GetString();
                }
                else if (strcmp(item_name, "verbs_conf") == 0) {
                    assert(item.value.IsString());
                    VERBS_CONF = item.value.GetString();
                }
                else if (strcmp(item_name, "verbs_domain") == 0) {
                    assert(item.value.IsString());
                    VERBS_DOMAIN = item.value.GetString();
                }
                else if (strcmp(item_name, "memory_allocated") == 0) {
                    assert(item.value.IsUint64());
                    MEMORY_ALLOCATED = item.value.GetUint64();
                }
                else if (strcmp(item_name, "clocksource") == 0) {
                    assert(item.value.IsInt());
                    CLOCKSOURCE = static_cast<ClocksourceType>(item.value.GetInt());
                    ClocksourceManager::getInstance()->setClocksourceType(CLOCKSOURCE);
                }
                else if (strcmp(item_name, "drift_cal_sleep_sec") == 0) {
                    assert(item.value.IsUint64());
                    DRIFT_CAL_SLEEP_SEC = item.value.GetUint64();
                }
                else if (strcmp(item_name, "drift_cal_sleep_nsec") == 0) {
                    assert(item.value.IsUint64());
                    DRIFT_CAL_SLEEP_NSEC = item.value.GetUint64();
                }
                else if (strcmp(item_name, "is_server") == 0) {
                    assert(item.value.IsBool());
                    IS_VISOR = item.value.GetBool();
                }
                else if (strcmp(item_name, "my_server_id") == 0) {
                    assert(item.value.IsInt());
                    MY_VISOR_ID = item.value.GetInt();
                }
                else if (strcmp(item_name, "server_list_file_path") == 0) {
                    assert(item.value.IsString());
                    SERVER_LIST_FILE_PATH = item.value.GetString();
                }
                else if (strcmp(item_name, "server_list") == 0) {
                    assert(item.value.IsArray());
                    SERVER_LIST.clear();
                    auto server_array = item.value.GetArray();
                    for (auto &server: server_array)
                        SERVER_LIST.emplace_back(server.GetString());
                }
                else if (strcmp(item_name, "backed_file_dir") == 0) {
                    assert(item.value.IsString());
                    BACKED_FILE_DIR = item.value.GetString();
                } else {
                    LOGI("Unknown configuration: %s", item_name);
                }
            }
            LOGI("Finish loading configurations from file %s, new configurations: ", conf_file_path.c_str());
            PrintConf();
        }
    };

}

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
