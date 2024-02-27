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

#ifndef INCLUDE_HCL_SET_SET_H_
#define INCLUDE_HCL_SET_SET_H_

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
#include <boost/interprocess/containers/set.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
/** Standard C++ Headers**/
#include <hcl/common/container.h>

#include <boost/interprocess/managed_mapped_file.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hcl {
/**
 * This is a Distributed Set Class. It uses shared memory + RPC + MPI to
 * achieve the data structure.
 *
 * @tparam MappedType, the value of the Set
 */

template <typename KeyType, typename Hash = std::hash<KeyType>,
          typename Compare = std::less<KeyType>, class Allocator = nullptr_t,
          class SharedType = nullptr_t>
class set : public container {
 private:
  /** Class Typedefs for ease of use **/
  typedef boost::interprocess::allocator<
      KeyType, boost::interprocess::managed_mapped_file::segment_manager>
      ShmemAllocator;
  typedef boost::interprocess::set<KeyType, Compare, ShmemAllocator> MySet;
  /** Class attributes**/
  Hash keyHash;
  MySet *myset;

 public:
  ~set();

  void construct_shared_memory() override;

  void open_shared_memory() override;

  void bind_functions() override;

  MySet *data() {
    if (server_on_node || is_server)
      return myset;
    else
      nullptr;
  }
  explicit set(CharStruct name_ = "TEST_SET",
               uint16_t port = HCL_CONF->RPC_PORT);

  bool LocalPut(KeyType &key);
  bool LocalGet(KeyType &key);
  bool LocalErase(KeyType &key);
  std::vector<KeyType> LocalGetAllDataInServer();
  std::vector<KeyType> LocalContainsInServer(KeyType &key_start,
                                             KeyType &key_end);
  std::pair<bool, KeyType> LocalSeekFirst();
  std::pair<bool, KeyType> LocalPopFirst();
  size_t LocalSize();
  std::pair<bool, std::vector<KeyType>> LocalSeekFirstN(uint32_t n);

#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
  THALLIUM_DEFINE(LocalPut, (key), KeyType &key)
  THALLIUM_DEFINE(LocalGet, (key), KeyType &key)
  THALLIUM_DEFINE(LocalErase, (key), KeyType &key)
  THALLIUM_DEFINE(LocalContainsInServer, (key_start, key_end),
                  KeyType &key_start, KeyType &key_end)
  THALLIUM_DEFINE(LocalSeekFirstN, (n), uint32_t n)

  THALLIUM_DEFINE1(LocalSize)
  THALLIUM_DEFINE1(LocalSeekFirst)
  THALLIUM_DEFINE1(LocalPopFirst)
  THALLIUM_DEFINE1(LocalGetAllDataInServer)
#endif

  bool Put(KeyType &key);
  bool Get(KeyType &key);

  bool Erase(KeyType &key);
  std::vector<KeyType> Contains(KeyType &key_start, KeyType &key_end);

  std::vector<KeyType> GetAllData();

  std::vector<KeyType> ContainsInServer(KeyType &key_start, KeyType &key_end);
  std::vector<KeyType> GetAllDataInServer();
  std::pair<bool, KeyType> SeekFirst(uint16_t &key_int);
  std::pair<bool, KeyType> PopFirst(uint16_t &key_int);
  std::pair<bool, std::vector<KeyType>> SeekFirstN(uint16_t &key_int,
                                                   uint32_t n);
  size_t Size(uint16_t &key_int);
};

#include "set.cpp"

}  // namespace hcl

#endif  // INCLUDE_HCL_SET_SET_H_
