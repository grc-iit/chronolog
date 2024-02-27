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

#ifndef INCLUDE_HCL_MULTIMAP_MULTIMAP_H_
#define INCLUDE_HCL_MULTIMAP_MULTIMAP_H_

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
#include <boost/interprocess/containers/map.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
/** Standard C++ Headers**/
#include <hcl/common/container.h>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hcl {
/**
 * This is a Distributed MultiMap Class. It uses shared memory + RPC + MPI to
 * achieve the data structure.
 *
 * @tparam MappedType, the value of the MultiMap
 */
template <typename KeyType, typename MappedType,
          typename Compare = std::less<KeyType>, class Allocator = nullptr_t,
          class SharedType = nullptr_t>
class multimap : public container {
 private:
  /** Class Typedefs for ease of use **/
  typedef std::pair<const KeyType, MappedType> ValueType;
  typedef boost::interprocess::allocator<
      ValueType, boost::interprocess::managed_mapped_file::segment_manager>
      ShmemAllocator;
  typedef boost::interprocess::multimap<KeyType, MappedType, Compare,
                                        ShmemAllocator>
      MyMap;
  /** Class attributes**/
  std::hash<KeyType> keyHash;
  MyMap *mymap;

 public:
  /* Constructor to deallocate the shared memory*/
  ~multimap();

  void construct_shared_memory() override;

  void open_shared_memory() override;

  void bind_functions() override;

  MyMap *data() {
    if (server_on_node || is_server)
      return mymap;
    else
      nullptr;
  }
  explicit multimap(CharStruct name_ = "TEST_MULTIMAP",
                    uint16_t port = HCL_CONF->RPC_PORT);

  bool LocalPut(KeyType &key, MappedType &data);
  std::pair<bool, MappedType> LocalGet(KeyType &key);
  std::pair<bool, MappedType> LocalErase(KeyType &key);
  std::vector<std::pair<KeyType, MappedType>> LocalContainsInServer(
      KeyType &key);
  std::vector<std::pair<KeyType, MappedType>> LocalGetAllDataInServer();

#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
  THALLIUM_DEFINE(LocalPut, (key, data), KeyType &key, MappedType &data)
  THALLIUM_DEFINE(LocalGet, (key), KeyType &key)
  THALLIUM_DEFINE(LocalErase, (key), KeyType &key)
  THALLIUM_DEFINE(LocalContainsInServer, (key), KeyType &key)
  THALLIUM_DEFINE1(LocalGetAllDataInServer)
#endif

  bool Put(KeyType &key, MappedType &data);
  std::pair<bool, MappedType> Get(KeyType &key);

  std::pair<bool, MappedType> Erase(KeyType &key);
  std::vector<std::pair<KeyType, MappedType>> Contains(KeyType &key);

  std::vector<std::pair<KeyType, MappedType>> GetAllData();

  std::vector<std::pair<KeyType, MappedType>> ContainsInServer(KeyType &key);
  std::vector<std::pair<KeyType, MappedType>> GetAllDataInServer();
};

#include "multimap.cpp"

}  // namespace hcl

#endif  // INCLUDE_HCL_MULTIMAP_MULTIMAP_H_
