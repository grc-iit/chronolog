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

#ifndef INCLUDE_HCL_MAP_MAP_H_
#define INCLUDE_HCL_MAP_MAP_H_

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
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hcl {

/**
 * This is a Distributed Map Class. It uses shared memory + RPC + MPI to
 * achieve the data structure.
 *
 * @tparam MappedType, the value of the Map
 */
template <typename KeyType, typename MappedType,
          typename Compare = std::less<KeyType>, class Allocator = nullptr_t,
          class SharedType = nullptr_t>
class map : public container {
 private:
  /** Class Typedefs for ease of use **/
  typedef std::pair<const KeyType, MappedType> ValueType;
  typedef boost::interprocess::allocator<
      ValueType, boost::interprocess::managed_mapped_file::segment_manager>
      ShmemAllocator;
  typedef boost::interprocess::map<KeyType, MappedType, Compare, ShmemAllocator>
      MyMap;
  /** Class attributes**/
  MyMap *mymap;
  std::hash<KeyType> keyHash;

 public:
  ~map() { this->container::~container(); }

  void construct_shared_memory() override {
    ShmemAllocator alloc_inst(segment.get_segment_manager());
    /* Construct map in the shared memory space. */
    mymap = segment.construct<MyMap>(name.c_str())(Compare(), alloc_inst);
  }
  void open_shared_memory() override {
    std::pair<MyMap *, boost::interprocess::managed_mapped_file::size_type> res;
    res = segment.find<MyMap>(name.c_str());
    mymap = res.first;
  }
  void bind_functions() override {
    /* Create a RPC server and map the methods to it. */
    switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
      case RPCLIB: {
        std::function<bool(KeyType &, MappedType &)> putFunc(
            std::bind(&map<KeyType, MappedType, Compare>::LocalPut, this,
                      std::placeholders::_1, std::placeholders::_2));
        std::function<std::pair<bool, MappedType>(KeyType &)> getFunc(
            std::bind(&map<KeyType, MappedType, Compare>::LocalGet, this,
                      std::placeholders::_1));
        std::function<std::pair<bool, MappedType>(KeyType &)> eraseFunc(
            std::bind(&map<KeyType, MappedType, Compare>::LocalErase, this,
                      std::placeholders::_1));
        std::function<std::vector<std::pair<KeyType, MappedType>>(void)>
            getAllDataInServerFunc(std::bind(
                &map<KeyType, MappedType, Compare>::LocalGetAllDataInServer,
                this));
        std::function<std::vector<std::pair<KeyType, MappedType>>(KeyType &,
                                                                  KeyType &)>
            containsInServerFunc(std::bind(
                &map<KeyType, MappedType, Compare>::LocalContainsInServer, this,
                std::placeholders::_1, std::placeholders::_2));

        rpc->bind(func_prefix + "_Put", putFunc);
        rpc->bind(func_prefix + "_Get", getFunc);
        rpc->bind(func_prefix + "_Erase", eraseFunc);
        rpc->bind(func_prefix + "_GetAllData", getAllDataInServerFunc);
        rpc->bind(func_prefix + "_Contains", containsInServerFunc);
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

        std::function<void(const tl::request &, KeyType &, MappedType &)>
            putFunc(
                std::bind(&map<KeyType, MappedType, Compare>::ThalliumLocalPut,
                          this, std::placeholders::_1, std::placeholders::_2,
                          std::placeholders::_3));
        std::function<void(const tl::request &, KeyType &)> getFunc(
            std::bind(&map<KeyType, MappedType, Compare>::ThalliumLocalGet,
                      this, std::placeholders::_1, std::placeholders::_2));
        std::function<void(const tl::request &, KeyType &)> eraseFunc(
            std::bind(&map<KeyType, MappedType, Compare>::ThalliumLocalErase,
                      this, std::placeholders::_1, std::placeholders::_2));
        std::function<void(const tl::request &)> getAllDataInServerFunc(
            std::bind(&map<KeyType, MappedType,
                           Compare>::ThalliumLocalGetAllDataInServer,
                      this, std::placeholders::_1));
        std::function<void(const tl::request &, KeyType &, KeyType &)>
            containsInServerFunc(
                std::bind(&map<KeyType, MappedType,
                               Compare>::ThalliumLocalContainsInServer,
                          this, std::placeholders::_1, std::placeholders::_2,
                          std::placeholders::_3));

        rpc->bind(func_prefix + "_Put", putFunc);
        rpc->bind(func_prefix + "_Get", getFunc);
        rpc->bind(func_prefix + "_Erase", eraseFunc);
        rpc->bind(func_prefix + "_GetAllData", getAllDataInServerFunc);
        rpc->bind(func_prefix + "_Contains", containsInServerFunc);
        break;
      }
#endif
    }
  }

  explicit map(CharStruct name_ = "TEST_MAP",
               uint16_t port = HCL_CONF->RPC_PORT)
      : container(name_, port), mymap() {
    AutoTrace trace = AutoTrace("hcl::map");
    if (is_server) {
      construct_shared_memory();
      bind_functions();
    } else if (!is_server && server_on_node) {
      open_shared_memory();
    }
  }

  MyMap *data() {
    if (server_on_node || is_server)
      return mymap;
    else
      nullptr;
  }

  bool LocalPut(KeyType &key, MappedType &data);

  std::pair<bool, MappedType> LocalGet(KeyType &key);

  std::pair<bool, MappedType> LocalErase(KeyType &key);

  std::vector<std::pair<KeyType, MappedType>> LocalGetAllDataInServer();

  std::vector<std::pair<KeyType, MappedType>> LocalContainsInServer(
      KeyType &key_start, KeyType &key_end);

#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
  THALLIUM_DEFINE(LocalPut, (key, data), KeyType &key, MappedType &data)
  THALLIUM_DEFINE(LocalGet, (key), KeyType &key)
  THALLIUM_DEFINE(LocalErase, (key), KeyType &key)
  THALLIUM_DEFINE(LocalContainsInServer, (key_start, key_end),
                  KeyType &key_start, KeyType &key_end)
  THALLIUM_DEFINE1(LocalGetAllDataInServer)
#endif

  bool Put(KeyType &key, MappedType &data);

  std::pair<bool, MappedType> Get(KeyType &key);

  std::pair<bool, MappedType> Erase(KeyType &key);

  std::vector<std::pair<KeyType, MappedType>> Contains(KeyType &key_start,
                                                       KeyType &key_end);

  std::vector<std::pair<KeyType, MappedType>> GetAllData();

  std::vector<std::pair<KeyType, MappedType>> ContainsInServer(
      KeyType &key_start, KeyType &key_end);

  std::vector<std::pair<KeyType, MappedType>> GetAllDataInServer();
};

#include "map.cpp"

}  // namespace hcl

#endif  // INCLUDE_HCL_MAP_MAP_H_
