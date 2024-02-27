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

#ifndef HCL_CONTAINER_H
#define HCL_CONTAINER_H

#include <hcl/communication/rpc_factory.h>
#include <hcl/communication/rpc_lib.h>

#include <cstdint>
#include <memory>

#include "typedefs.h"

namespace hcl {
class container {
 protected:
  int comm_size, my_rank, num_servers;
  uint16_t my_server;
  std::shared_ptr<RPC> rpc;
  really_long memory_allocated;
  bool is_server;
  boost::interprocess::managed_mapped_file segment;
  CharStruct name, func_prefix;
  boost::interprocess::interprocess_mutex *mutex;
  CharStruct backed_file;

 public:
  bool server_on_node;
  virtual void construct_shared_memory() = 0;
  virtual void open_shared_memory() = 0;
  virtual void bind_functions() = 0;

  inline bool is_local(uint16_t &key_int) {
    return key_int == my_server && server_on_node;
  }
  inline bool is_local() { return server_on_node; }

  template <typename Allocator, typename MappedType, typename SharedType>
  typename std::enable_if_t<std::is_same<Allocator, nullptr_t>::value,
                            MappedType>
  GetData(MappedType &data) {
    return std::move(data);
  }

  template <typename Allocator, typename MappedType, typename SharedType>
  typename std::enable_if_t<!std::is_same<Allocator, nullptr_t>::value,
                            SharedType>
  GetData(MappedType &data) {
    Allocator allocator(segment.get_segment_manager());
    SharedType value(allocator);
    value.assign(data);
    return std::move(value);
  }

  ~container() {
    if (is_server)
      boost::interprocess::file_mapping::remove(backed_file.c_str());
  }
  container(CharStruct name_, uint16_t port)
      : is_server(HCL_CONF->IS_SERVER),
        my_server(HCL_CONF->MY_SERVER),
        num_servers(HCL_CONF->NUM_SERVERS),
        comm_size(1),
        my_rank(0),
        memory_allocated(HCL_CONF->MEMORY_ALLOCATED),
        name(name_),
        segment(),
        func_prefix(name_),
        backed_file(HCL_CONF->BACKED_FILE_DIR + PATH_SEPARATOR + name_ + "_" +
                    std::to_string(my_server)),
        server_on_node(HCL_CONF->SERVER_ON_NODE) {
    AutoTrace trace = AutoTrace("hcl::container");
    /* Initialize MPI rank and size of world */
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    /* create per server name for shared memory. Needed if multiple servers are
       spawned on one node*/
    this->name += "_" + std::to_string(my_server);
    /* if current rank is a server */
    rpc = hcl::Singleton<RPCFactory>::GetInstance()->GetRPC(port);
    if (is_server) {
      /* Delete existing instance of shared memory space*/
      boost::interprocess::file_mapping::remove(backed_file.c_str());
      /* allocate new shared memory space */
      segment = boost::interprocess::managed_mapped_file(
          boost::interprocess::create_only, backed_file.c_str(),
          memory_allocated);
      mutex =
          segment.construct<boost::interprocess::interprocess_mutex>("mtx")();
    } else if (!is_server && server_on_node) {
      /* Map the clients to their respective memory pools */
      segment = boost::interprocess::managed_mapped_file(
          boost::interprocess::open_only, backed_file.c_str());
      std::pair<boost::interprocess::interprocess_mutex *,
                boost::interprocess::managed_mapped_file::size_type>
          res2;
      res2 = segment.find<boost::interprocess::interprocess_mutex>("mtx");
      mutex = res2.first;
    }
  }
  void lock() {
    if (server_on_node || is_server) mutex->lock();
  }

  void unlock() {
    if (server_on_node || is_server) mutex->unlock();
  }
};
}  // namespace hcl

#endif  // HCL_CONTAINER_H
