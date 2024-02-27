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

#ifndef INCLUDE_HCL_QUEUE_QUEUE_H_
#define INCLUDE_HCL_QUEUE_QUEUE_H_

/**
 * Include Headers
 */
#include <hcl/common/debug.h>
#include <hcl/common/singleton.h>
#include <hcl/communication/rpc_factory.h>
#include <hcl/communication/rpc_lib.h>
/** MPI Headers**/
#include <mpi.h>
/** RPC Lib Headers**/
#ifdef HCL_ENABLE_RPCLIB
#include <rpc/client.h>
#include <rpc/rpc_error.h>
#include <rpc/server.h>
#endif
/** Thallium Headers **/
#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
#include <thallium.hpp>
#endif

/** Boost Headers **/
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/deque.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
/** Standard C++ Headers**/
#include <hcl/common/container.h>

#include <boost/interprocess/managed_mapped_file.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

/** Namespaces Uses **/
namespace bip = boost::interprocess;

namespace hcl {
/**
 * This is a Distributed Queue Class. It uses shared memory +
 * RPC + MPI to achieve the data structure.
 *
 * @tparam MappedType, the value of the Queue
 */
template <typename MappedType, class Allocator = nullptr_t,
          class SharedType = nullptr_t>
class queue : public container {
 private:
  /** Class Typedefs for ease of use **/
  typedef bip::allocator<MappedType, bip::managed_mapped_file::segment_manager>
      ShmemAllocator;
  typedef boost::interprocess::deque<MappedType, ShmemAllocator> Queue;

  /** Class attributes**/
  Queue *my_queue;

 public:
  ~queue();

  void construct_shared_memory() override;

  void open_shared_memory() override;

  void bind_functions() override;

  explicit queue(CharStruct name_ = "TEST_QUEUE",
                 uint16_t port = HCL_CONF->RPC_PORT);
  Queue *data() {
    if (server_on_node || is_server)
      return my_queue;
    else
      nullptr;
  }
  bool LocalPush(MappedType &data);
  std::pair<bool, MappedType> LocalPop();
  bool LocalWaitForElement();
  size_t LocalSize();

#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
  THALLIUM_DEFINE(LocalPush, (data), MappedType &data)
  THALLIUM_DEFINE1(LocalPop)
  THALLIUM_DEFINE1(LocalWaitForElement)
  THALLIUM_DEFINE1(LocalSize)
#endif

  bool Push(MappedType &data, uint16_t &key_int);
  std::pair<bool, MappedType> Pop(uint16_t &key_int);
  bool WaitForElement(uint16_t &key_int);
  size_t Size(uint16_t &key_int);
};

#include "queue.cpp"

}  // namespace hcl

#endif  // INCLUDE_HCL_QUEUE_QUEUE_H_
