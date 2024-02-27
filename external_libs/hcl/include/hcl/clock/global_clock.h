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

#ifndef INCLUDE_HCL_CLOCK_GLOBAL_CLOCK_H_
#define INCLUDE_HCL_CLOCK_GLOBAL_CLOCK_H_

#include <hcl/common/data_structures.h>
#include <hcl/common/debug.h>
#include <hcl/common/singleton.h>
#include <hcl/common/typedefs.h>
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
class global_clock {
 private:
  typedef std::chrono::high_resolution_clock::time_point chrono_time;
  chrono_time *start;
  bool is_server;
  bip::interprocess_mutex *mutex;
  really_long memory_allocated;
  int my_rank, comm_size, num_servers;
  uint16_t my_server;
  bip::managed_mapped_file segment;
  std::string name, func_prefix;
  std::shared_ptr<RPC> rpc;
  bool server_on_node;
  CharStruct backed_file;

 public:
  /*
   * Destructor removes shared memory from the server
   */
  ~global_clock() {
    AutoTrace trace = AutoTrace("hcl::~global_clock", NULL);
    if (is_server) bip::file_mapping::remove(backed_file.c_str());
  }

  global_clock(std::string name_ = "TEST_GLOBAL_CLOCK",
               uint16_t port = HCL_CONF->RPC_PORT)
      : is_server(HCL_CONF->IS_SERVER),
        my_server(HCL_CONF->MY_SERVER),
        num_servers(HCL_CONF->NUM_SERVERS),
        comm_size(1),
        my_rank(0),
        memory_allocated(1024ULL * 1024ULL * 128ULL),
        name(name_),
        segment(),
        func_prefix(name_),
        backed_file(HCL_CONF->BACKED_FILE_DIR + PATH_SEPARATOR + name_),
        server_on_node(HCL_CONF->SERVER_ON_NODE) {
    AutoTrace trace = AutoTrace("hcl::global_clock");
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    name = name + "_" + std::to_string(my_server);
    rpc = Singleton<RPCFactory>::GetInstance()->GetRPC(port);
    if (is_server) {
      switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
        case RPCLIB: {
          std::function<HTime(void)> getTimeFunction(
              std::bind(&global_clock::LocalGetTime, this));
          rpc->bind(func_prefix + "_GetTime", getTimeFunction);
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
          std::function<void(const tl::request &)> getTimeFunction(
              std::bind(&global_clock::ThalliumLocalGetTime, this,
                        std::placeholders::_1));
          rpc->bind(func_prefix + "_GetTime", getTimeFunction);
          break;
        }
#endif
      }

      bip::file_mapping::remove(backed_file.c_str());
      segment = bip::managed_mapped_file(bip::create_only, backed_file.c_str(),
                                         65536);
      start = segment.construct<chrono_time>("Time")(
          std::chrono::high_resolution_clock::now());
      mutex =
          segment.construct<boost::interprocess::interprocess_mutex>("mtx")();
    } else if (!is_server && server_on_node) {
      segment = bip::managed_mapped_file(bip::open_only, backed_file.c_str());
      std::pair<chrono_time *, bip::managed_mapped_file::size_type> res;
      res = segment.find<chrono_time>("Time");
      start = res.first;
      std::pair<bip::interprocess_mutex *, bip::managed_mapped_file::size_type>
          res2;
      res2 = segment.find<bip::interprocess_mutex>("mtx");
      mutex = res2.first;
    }
  }
  chrono_time *data() {
    if (server_on_node || is_server)
      return start;
    else
      nullptr;
  }
  void lock() {
    if (server_on_node || is_server) mutex->lock();
  }

  void unlock() {
    if (server_on_node || is_server) mutex->unlock();
  }
  /*
   * GetTime() returns the time on the server
   */
  HTime GetTime() {
    if (server_on_node) {
      return LocalGetTime();
    } else {
      auto my_server_i = my_server;
      return RPC_CALL_WRAPPER1("_GetTime", my_server_i, HTime);
    }
  }

  /*
   * GetTimeServer() returns the time on the requested server using RPC calls,
   * or the local time if the server requested is the current client server
   */
  HTime GetTimeServer(uint16_t &server) {
    AutoTrace trace = AutoTrace("hcl::global_clock::GetTimeServer", server);
    if (my_server == server && server_on_node) {
      return LocalGetTime();
    } else {
      return RPC_CALL_WRAPPER1("_GetTime", server, HTime);
    }
  }

  /*
   * GetTime() returns the time locally within a node using chrono
   * high_resolution_clock
   */
  HTime LocalGetTime() {
    AutoTrace trace = AutoTrace("hcl::global_clock::GetTime", NULL);
    boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
        lock(*mutex);
    auto t2 = std::chrono::high_resolution_clock::now();
    auto t = std::chrono::duration_cast<std::chrono::microseconds>(t2 - *start)
                 .count();
    return t;
  }

#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
  THALLIUM_DEFINE1(LocalGetTime)
#endif
};

}  // namespace hcl

#endif  // INCLUDE_HCL_CLOCK_GLOBAL_CLOCK_H_
