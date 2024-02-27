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

#ifndef HCL_RPC_FACTORY_H
#define HCL_RPC_FACTORY_H

#include <memory>
#include <unordered_map>

#include "rpc_lib.h"

class RPCFactory {
 private:
  std::unordered_map<uint16_t, std::shared_ptr<RPC>> rpcs;

 public:
  RPCFactory() : rpcs() {}
  std::shared_ptr<RPC> GetRPC(uint16_t server_port) {
    auto iter = rpcs.find(server_port);
    if (iter != rpcs.end()) return iter->second;
    auto temp = HCL_CONF->RPC_PORT;
    HCL_CONF->RPC_PORT = server_port;
    auto rpc = std::make_shared<RPC>();
    rpcs.emplace(server_port, rpc);
    HCL_CONF->RPC_PORT = temp;
    return rpc;
  }
};
#endif  // HCL_RPC_FACTORY_H
