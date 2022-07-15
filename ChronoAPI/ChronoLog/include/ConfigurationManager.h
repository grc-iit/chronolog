//
// Created by kfeng on 3/30/22.
//

#ifndef CHRONOLOG_CONFIGURATIONMANAGER_H
#define CHRONOLOG_CONFIGURATIONMANAGER_H

#include <cstdint>
#include <fstream>
#include <iostream>
#include "data_structures.h"
#include "enum.h"

namespace ChronoLog {
    class ConfigurationManager {
    public:
        CharStruct RPC_SERVER_HOSTNAME;
        uint16_t RPC_PORT;
        uint16_t RPC_THREADS;
        RPCImplementation RPC_IMPLEMENTATION;
        CharStruct SOCKETS_CONF;
        CharStruct VERBS_CONF;
        CharStruct VERBS_DOMAIN;
        uint64_t MEMORY_ALLOCATED;

        bool IS_SERVER;
        int MY_SERVER_ID;
        CharStruct SERVER_LIST_FILE_PATH;
        std::vector<CharStruct> SERVER_LIST;
        CharStruct BACKED_FILE_DIR;

        ConfigurationManager() :
                RPC_SERVER_HOSTNAME(""),
                RPC_PORT(5555),
                RPC_THREADS(1),
                RPC_IMPLEMENTATION(THALLIUM_SOCKETS),
                SOCKETS_CONF("ofi+tcp"), VERBS_CONF("verbs"), VERBS_DOMAIN("mlx5_0"),
                MEMORY_ALLOCATED(1024ULL * 1024ULL * 128ULL),
                IS_SERVER(false), MY_SERVER_ID(0), SERVER_LIST_FILE_PATH("./server_list"),
                SERVER_LIST(), BACKED_FILE_DIR("/dev/shm") {}

        std::vector<CharStruct> LoadServers() {
            SERVER_LIST = std::vector<CharStruct>();
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
                 * the server_list file follows the format of a hostfile for MPI
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
                printf("Error: Can't open server list file %s\n", SERVER_LIST_FILE_PATH.c_str());
                exit(EXIT_FAILURE);
            }
            file.close();
            return SERVER_LIST;
        }

        void ConfigureDefaultClient(const std::string &server_list_path = "") {
            if (!server_list_path.empty()) SERVER_LIST_FILE_PATH = server_list_path;
            LoadServers();
            IS_SERVER = false;
        }

        void ConfigureDefaultServer(const std::string &server_list_path = "") {
            if (!server_list_path.empty()) SERVER_LIST_FILE_PATH = server_list_path;
            LoadServers();
            IS_SERVER = true;
        }
    };

}

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
