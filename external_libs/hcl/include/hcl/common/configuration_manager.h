/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Distributed under BSD 3-Clause license.                                   *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Illinois Institute of Technology.                        *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of Hermes. The full Hermes copyright notice, including  *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the top directory. If you do not  *
 * have access to the file, you may request a copy from help@hdfgroup.org.   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef INCLUDE_HCL_COMMON_CONFIGURATION_MANAGER_H
#define INCLUDE_HCL_COMMON_CONFIGURATION_MANAGER_H

#include <hcl/common/data_structures.h>
#include <hcl/common/debug.h>
#include <hcl/common/enumerations.h>
#include <hcl/common/singleton.h>

#include <boost/thread/mutex.hpp>
#include <fstream>
#include <vector>

#include "typedefs.h"

namespace hcl {

class ConfigurationManager {
 private:
  boost::mutex file_load;

 public:
  uint16_t RPC_PORT;
  uint16_t RPC_THREADS;
  RPCImplementation RPC_IMPLEMENTATION;
  CharStruct TCP_CONF;
  CharStruct VERBS_CONF;
  CharStruct VERBS_DOMAIN;
  really_long MEMORY_ALLOCATED;

  bool IS_SERVER;
  uint16_t MY_SERVER;
  int NUM_SERVERS;
  bool SERVER_ON_NODE;
  CharStruct SERVER_LIST_PATH;
  std::vector<CharStruct> SERVER_LIST;
  CharStruct BACKED_FILE_DIR;

  bool DYN_CONFIG;  // Does not do anything (yet)

  ConfigurationManager()
      : SERVER_LIST(),
        BACKED_FILE_DIR("/dev/shm"),
        MEMORY_ALLOCATED(1024ULL * 1024ULL * 128ULL),
        RPC_PORT(9000),
        RPC_THREADS(1),
#if defined(HCL_ENABLE_RPCLIB)
        RPC_IMPLEMENTATION(RPCLIB),
#elif defined(HCL_ENABLE_THALLIUM_TCP)
        RPC_IMPLEMENTATION(THALLIUM_TCP),
#elif defined(HCL_ENABLE_THALLIUM_ROCE)
        RPC_IMPLEMENTATION(THALLIUM_ROCE),
#endif
        TCP_CONF("ofi+sockets"),
        VERBS_CONF("ofi-verbs"),
        VERBS_DOMAIN("mlx5_0"),
        IS_SERVER(false),
        MY_SERVER(0),
        NUM_SERVERS(1),
        SERVER_ON_NODE(true),
        SERVER_LIST_PATH(""),
        DYN_CONFIG(false) {
    AutoTrace trace = AutoTrace("ConfigurationManager");
  }

  std::vector<CharStruct> LoadServers() {
    file_load.lock();
    SERVER_LIST = std::vector<CharStruct>();
    fstream file;
    file.open(SERVER_LIST_PATH.c_str(), ios::in);
    if (file.is_open()) {
      std::string file_line;

      int count;
      while (getline(file, file_line)) {
        CharStruct server_node_name;
        if (!file_line.empty()) {
          int split_loc = file_line.find(":");
          int split_loc2 = file_line.find(':');  // split to node and net
          // int split_loc = file_line.find(" slots=");
          // int split_loc2 = file_line.find('='); // split to node and net
          if (split_loc != std::string::npos) {
            server_node_name = file_line.substr(0, split_loc);
            count = atoi(
                file_line.substr(split_loc2 + 1, std::string::npos).c_str());
          } else {
            // no special network
            server_node_name = file_line;
            count = 1;
          }
          // server list is list of network interfaces
          for (int i = 0; i < count; ++i) {
            SERVER_LIST.emplace_back(server_node_name);
          }
        }
      }
    } else {
      printf("Error: Can't open server list file %s\n",
             SERVER_LIST_PATH.c_str());
    }
    NUM_SERVERS = SERVER_LIST.size();
    file.close();
    file_load.unlock();
    return SERVER_LIST;
  }
  void ConfigureDefaultClient(std::string server_list_path = "") {
    if (server_list_path != "") SERVER_LIST_PATH = server_list_path;
    LoadServers();
    IS_SERVER = false;
    MY_SERVER = false;
    SERVER_ON_NODE = false;
  }

  void ConfigureDefaultServer(std::string server_list_path = "") {
    if (server_list_path != "") SERVER_LIST_PATH = server_list_path;
    LoadServers();
    IS_SERVER = true;
    MY_SERVER = false;
    SERVER_ON_NODE = true;
  }
};

}  // namespace hcl

#endif  // INCLUDE_HCL_COMMON_CONFIGURATION_MANAGER_H
