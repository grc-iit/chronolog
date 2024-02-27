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

#ifndef INCLUDE_HCL_UNORDERED_MAP_UNORDERED_MAP_H_
#define INCLUDE_HCL_UNORDERED_MAP_UNORDERED_MAP_H_

/**
 * Include Headers
 */

/** Standard C++ Headers**/
#include <hcl/common/singleton.h>
#include <hcl/common/typedefs.h>
#include <hcl/communication/rpc_factory.h>
#include <hcl/communication/rpc_lib.h>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
#include <hcl/common/container.h>

#include <boost/algorithm/string.hpp>
#include <boost/functional/hash.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/unordered/unordered_map.hpp>

/** Namespaces Uses **/

/** Global Typedefs **/

namespace hcl {
/**
 * This is a Distributed HashMap Class. It uses shared memory + RPC + MPI to
 * achieve the data structure.
 *
 * @tparam MappedType, the value of the HashMap
 */
template <typename KeyType, typename MappedType,
          typename Hash = std::hash<KeyType>, class Allocator = nullptr_t,
          class SharedType = nullptr_t>
class unordered_map : public container {
 private:
  /** Class Typedefs for ease of use **/
  typedef std::pair<const KeyType, MappedType> ValueType;
  typedef boost::interprocess::allocator<
      ValueType, boost::interprocess::managed_mapped_file::segment_manager>
      ShmemAllocator;
  typedef boost::interprocess::managed_mapped_file managed_segment;
  typedef boost::unordered::unordered_map<
      KeyType, MappedType, Hash, std::equal_to<KeyType>, ShmemAllocator>
      MyHashMap;
  /** Class attributes**/
  Hash keyHash;
  MyHashMap *myHashMap;

 public:
  really_long size_occupied;
  ~unordered_map();

  explicit unordered_map(CharStruct name_ = std::string("TEST_UNORDERED_MAP"),
                         uint16_t port = HCL_CONF->RPC_PORT);
  MyHashMap *data() {
    if (server_on_node || is_server)
      return myHashMap;
    else
      nullptr;
  }

  void construct_shared_memory() override {
    /* Construct unordered_map in the shared memory space. */
    myHashMap = segment.construct<MyHashMap>(name.c_str())(
        128, Hash(), std::equal_to<KeyType>(),
        segment.get_allocator<ValueType>());
  }

  void open_shared_memory() override;

  void bind_functions() override;

  bool LocalPut(KeyType &key, MappedType &data);
  std::pair<bool, MappedType> LocalGet(KeyType &key);
  std::pair<bool, MappedType> LocalErase(KeyType &key);
  std::vector<std::pair<KeyType, MappedType>> LocalGetAllDataInServer();

#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
  THALLIUM_DEFINE(LocalPut, (key, data), KeyType &key, MappedType &data)

  // void ThalliumLocalPut(const tl::request &thallium_req, tl::bulk
  // &bulk_handle, KeyType key) {
  //     MappedType data =
  //     rpc->prep_rdma_server<MappedType>(thallium_req.get_endpoint(),
  //     bulk_handle); thallium_req.respond(LocalPut(key, data));
  // }

  // void ThalliumLocalGet(const tl::request &thallium_req, KeyType key) {
  //     auto retpair = LocalGet(key);
  //     if (!retpair.first) {
  //         printf("error\n");
  //     }
  //     MappedType data = retpair.second;
  //     tl::bulk bulk_handle = rpc->prep_rdma_client<MappedType>(data);
  //     thallium_req.respond(bulk_handle);
  // }

  THALLIUM_DEFINE(LocalGet, (key), KeyType &key)
  THALLIUM_DEFINE(LocalErase, (key), KeyType &key)
  THALLIUM_DEFINE1(LocalGetAllDataInServer)
#endif

  bool Put(KeyType key, MappedType data);
  std::pair<bool, MappedType> Get(KeyType &key);
  std::pair<bool, MappedType> Erase(KeyType &key);
  std::vector<std::pair<KeyType, MappedType>> GetAllData();
  std::vector<std::pair<KeyType, MappedType>> GetAllDataInServer();
};

#include "unordered_map.cpp"

}  // namespace hcl

#endif  // INCLUDE_HCL_UNORDERED_MAP_UNORDERED_MAP_H_
