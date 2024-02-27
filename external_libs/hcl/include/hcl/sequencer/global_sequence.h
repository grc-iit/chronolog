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

#ifndef INCLUDE_HCL_SEQUENCER_GLOBAL_SEQUENCE_H_
#define INCLUDE_HCL_SEQUENCER_GLOBAL_SEQUENCE_H_

#include <hcl/common/container.h>
#include <hcl/common/singleton.h>
#include <hcl/communication/rpc_factory.h>
#include <hcl/communication/rpc_lib.h>
#include <mpi.h>
#include <stdint-gcc.h>

#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <memory>
#include <string>
#include <utility>

namespace bip = boost::interprocess;

namespace hcl {
class global_sequence : public container {
 private:
  uint64_t *value;

 public:
  ~global_sequence() { this->container::~container(); }

  void construct_shared_memory() override {
    value = segment.construct<uint64_t>(name.c_str())(0);
  }

  void open_shared_memory() override {
    std::pair<uint64_t *, bip::managed_mapped_file::size_type> res;
    res = segment.find<uint64_t>(name.c_str());
    value = res.first;
  }

  void bind_functions() override {
    switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
      case RPCLIB: {
        std::function<uint64_t(void)> getNextSequence(
            std::bind(&hcl::global_sequence::LocalGetNextSequence, this));
        rpc->bind(func_prefix + "_GetNextSequence", getNextSequence);
        break;
      }
#endif
#ifdef HCL_ENABLE_THALLIUM_TCP
      case THALLIUM_TCP:
#endif
#ifdef HCL_ENABLE_THALLIUM_ROCE
      case THALLIUM_ROCE:
#endif
#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
      {
        std::function<void(const tl::request &)> getNextSequence(
            std::bind(&hcl::global_sequence::ThalliumLocalGetNextSequence, this,
                      std::placeholders::_1));
        rpc->bind(func_prefix + "_GetNextSequence", getNextSequence);
        break;
      }
#endif
    }
  }

  global_sequence(CharStruct name_ = "TEST_GLOBAL_SEQUENCE",
                  uint16_t port = HCL_CONF->RPC_PORT)
      : container(name_, port) {
    AutoTrace trace = AutoTrace("hcl::global_sequence");
    if (is_server) {
      construct_shared_memory();
      bind_functions();
    } else if (!is_server && server_on_node) {
      open_shared_memory();
    }
  }
  uint64_t *data() {
    if (server_on_node || is_server)
      return value;
    else
      nullptr;
  }
  uint64_t GetNextSequence() {
    if (is_local()) {
      return LocalGetNextSequence();
    } else {
      auto my_server_i = my_server;
      return RPC_CALL_WRAPPER1("_GetNextSequence", my_server_i, uint64_t);
    }
  }
  uint64_t GetNextSequenceServer(uint16_t &server) {
    if (is_local(server)) {
      return LocalGetNextSequence();
    } else {
      return RPC_CALL_WRAPPER1("_GetNextSequence", server, uint64_t);
    }
  }

  uint64_t LocalGetNextSequence() {
    boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
        lock(*mutex);
    return ++*value;
  }

#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
  THALLIUM_DEFINE1(LocalGetNextSequence)
#endif
};

}  // namespace hcl

#endif  // INCLUDE_HCL_SEQUENCER_GLOBAL_SEQUENCE_H_
