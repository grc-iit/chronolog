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

#ifndef INCLUDE_HCL_MULTIMAP_MULTIMAP_CPP_
#define INCLUDE_HCL_MULTIMAP_MULTIMAP_CPP_

/* Constructor to deallocate the shared memory*/
template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
multimap<KeyType, MappedType, Compare, Allocator, SharedType>::~multimap() {
  this->container::~container();
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
multimap<KeyType, MappedType, Compare, Allocator, SharedType>::multimap(
    CharStruct name_, uint16_t port)
    : container(name_, port), mymap() {
  AutoTrace trace = AutoTrace("hcl::multimap");
  if (is_server) {
    construct_shared_memory();
    bind_functions();
  } else if (!is_server && server_on_node) {
    open_shared_memory();
  }
}

/**
 * Put the data into the local multimap.
 * @param key, the key for put
 * @param data, the value for put
 * @return bool, true if Put was successful else false.
 */
template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
bool multimap<KeyType, MappedType, Compare, Allocator, SharedType>::LocalPut(
    KeyType &key, MappedType &data) {
  AutoTrace trace = AutoTrace("hcl::multimap::Put(local)", key, data);
  boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
      lock(*mutex);
  typename MyMap::iterator iterator = mymap->find(key);
  if (iterator != mymap->end()) {
    mymap->erase(iterator);
  }
  auto value = GetData<Allocator, MappedType, SharedType>(data);
  mymap->insert(std::pair<KeyType, MappedType>(key, value));
  return true;
}

/**
 * Put the data into the multimap. Uses key to decide the server to hash it
 * to,
 * @param key, the key for put
 * @param data, the value for put
 * @return bool, true if Put was successful else false.
 */
template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
bool multimap<KeyType, MappedType, Compare, Allocator, SharedType>::Put(
    KeyType &key, MappedType &data) {
  size_t key_hash = keyHash(key);
  uint16_t key_int = static_cast<uint16_t>(key_hash % num_servers);
  if (is_local(key_int)) {
    return LocalPut(key, data);
  } else {
    AutoTrace trace = AutoTrace("hcl::multimap::Put(remote)", key, data);
    return RPC_CALL_WRAPPER("_Put", key_int, bool, key, data);
  }
}

/**
 * Get the data in the local multimap.
 * @param key, key to get
 * @return return a pair of bool and Value. If bool is true then data was
 * found and is present in value part else bool is set to false
 */
template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::pair<bool, MappedType> multimap<KeyType, MappedType, Compare, Allocator,
                                     SharedType>::LocalGet(KeyType &key) {
  AutoTrace trace = AutoTrace("hcl::multimap::Get(local)", key);
  boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
      lock(*mutex);
  typename MyMap::iterator iterator = mymap->find(key);
  if (iterator != mymap->end()) {
    return std::pair<bool, MappedType>(true, iterator->second);
  } else {
    return std::pair<bool, MappedType>(false, MappedType());
  }
}

/**
 * Get the data into the multimap. Uses key to decide the server to hash it
 * to,
 * @param key, key to get
 * @return return a pair of bool and Value. If bool is true then data was
 * found and is present in value part else bool is set to false
 */
template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::pair<bool, MappedType> multimap<KeyType, MappedType, Compare, Allocator,
                                     SharedType>::Get(KeyType &key) {
  size_t key_hash = keyHash(key);
  uint16_t key_int = key_hash % num_servers;
  if (is_local(key_int)) {
    return LocalGet(key);
  } else {
    AutoTrace trace = AutoTrace("hcl::multimap::Get(remote)", key);
    typedef std::pair<bool, MappedType> ret_type;
    return RPC_CALL_WRAPPER("_Get", key_int, ret_type, key);
  }
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::pair<bool, MappedType> multimap<KeyType, MappedType, Compare, Allocator,
                                     SharedType>::LocalErase(KeyType &key) {
  AutoTrace trace = AutoTrace("hcl::multimap::Erase(local)", key);
  boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
      lock(*mutex);
  size_t s = mymap->erase(key);
  return std::pair<bool, MappedType>(s > 0, MappedType());
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::pair<bool, MappedType> multimap<KeyType, MappedType, Compare, Allocator,
                                     SharedType>::Erase(KeyType &key) {
  size_t key_hash = keyHash(key);
  uint16_t key_int = key_hash % num_servers;
  if (is_local(key_int)) {
    return LocalErase(key);
  } else {
    AutoTrace trace = AutoTrace("hcl::multimap::Erase(remote)", key);
    typedef std::pair<bool, MappedType> ret_type;
    return RPC_CALL_WRAPPER("_Erase", key_int, ret_type, key);
  }
}

/**
 * Get the data in the multimap. Uses key to decide the server to hash it
 * to,
 * @param key, key to get
 * @return return a pair of bool and Value. If bool is true then data was
 * found and is present in value part else bool is set to false
 */
template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::vector<std::pair<KeyType, MappedType>>
multimap<KeyType, MappedType, Compare, Allocator, SharedType>::Contains(
    KeyType &key) {
  AutoTrace trace = AutoTrace("hcl::multimap::Contains", key);
  std::vector<std::pair<KeyType, MappedType>> final_values =
      std::vector<std::pair<KeyType, MappedType>>();
  auto current_server = ContainsInServer(key);
  final_values.insert(final_values.end(), current_server.begin(),
                      current_server.end());
  for (int i = 0; i < num_servers; ++i) {
    if (i != my_server) {
      typedef std::vector<std::pair<KeyType, MappedType>> ret_type;
      auto server = RPC_CALL_WRAPPER("_Contains", i, ret_type, key);
      final_values.insert(final_values.end(), server.begin(), server.end());
    }
  }
  return final_values;
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::vector<std::pair<KeyType, MappedType>>
multimap<KeyType, MappedType, Compare, Allocator, SharedType>::GetAllData() {
  AutoTrace trace = AutoTrace("hcl::multimap::GetAllData");
  std::vector<std::pair<KeyType, MappedType>> final_values =
      std::vector<std::pair<KeyType, MappedType>>();
  auto current_server = GetAllDataInServer();
  final_values.insert(final_values.end(), current_server.begin(),
                      current_server.end());
  for (int i = 0; i < num_servers; ++i) {
    if (i != my_server) {
      typedef std::vector<std::pair<KeyType, MappedType>> ret_type;
      auto server = RPC_CALL_WRAPPER1("_GetAllData", i, ret_type);
      final_values.insert(final_values.end(), server.begin(), server.end());
    }
  }
  return final_values;
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::vector<std::pair<KeyType, MappedType>>
multimap<KeyType, MappedType, Compare, Allocator,
         SharedType>::LocalContainsInServer(KeyType &key) {
  AutoTrace trace = AutoTrace("hcl::multimap::ContainsInServer", key);
  std::vector<std::pair<KeyType, MappedType>> final_values =
      std::vector<std::pair<KeyType, MappedType>>();
  {
    boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
        lock(*mutex);
    typename MyMap::iterator lower_bound;
    size_t size = mymap->size();
    if (size == 0) {
    } else if (size == 1) {
      lower_bound = mymap->begin();
      final_values.insert(final_values.end(),
                          std::pair<KeyType, MappedType>(lower_bound->first,
                                                         lower_bound->second));
    } else {
      lower_bound = mymap->lower_bound(key);
      if (lower_bound == mymap->end()) return final_values;
      if (lower_bound != mymap->begin()) {
        --lower_bound;
        if (!key.Contains(lower_bound->first)) lower_bound++;
      }
      while (lower_bound != mymap->end()) {
        if (!(key.Contains(lower_bound->first) ||
              lower_bound->first.Contains(key)))
          break;
        final_values.insert(final_values.end(),
                            std::pair<KeyType, MappedType>(
                                lower_bound->first, lower_bound->second));
        lower_bound++;
      }
    }
  }
  return final_values;
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::vector<std::pair<KeyType, MappedType>>
multimap<KeyType, MappedType, Compare, Allocator, SharedType>::ContainsInServer(
    KeyType &key) {
  if (is_local()) {
    return LocalContainsInServer(key);
  } else {
    typedef std::vector<std::pair<KeyType, MappedType>> ret_type;
    auto my_server_i = my_server;
    return RPC_CALL_WRAPPER("_Contains", my_server_i, ret_type, key);
  }
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::vector<std::pair<KeyType, MappedType>>
multimap<KeyType, MappedType, Compare, Allocator,
         SharedType>::LocalGetAllDataInServer() {
  AutoTrace trace = AutoTrace("hcl::multimap::GetAllDataInServer");
  std::vector<std::pair<KeyType, MappedType>> final_values =
      std::vector<std::pair<KeyType, MappedType>>();
  {
    boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
        lock(*mutex);
    typename MyMap::iterator lower_bound;
    lower_bound = mymap->begin();
    while (lower_bound != mymap->end()) {
      final_values.insert(final_values.end(),
                          std::pair<KeyType, MappedType>(lower_bound->first,
                                                         lower_bound->second));
      lower_bound++;
    }
  }
  return final_values;
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
std::vector<std::pair<KeyType, MappedType>> multimap<
    KeyType, MappedType, Compare, Allocator, SharedType>::GetAllDataInServer() {
  if (is_local()) {
    return LocalGetAllDataInServer();
  } else {
    typedef std::vector<std::pair<KeyType, MappedType>> ret_type;
    auto my_server_i = my_server;
    return RPC_CALL_WRAPPER1("_GetAllData", my_server_i, ret_type);
  }
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
void multimap<KeyType, MappedType, Compare, Allocator,
              SharedType>::construct_shared_memory() {
  ShmemAllocator alloc_inst(segment.get_segment_manager());
  /* Construct Multimap in the shared memory space. */
  mymap = segment.construct<MyMap>(name.c_str())(Compare(), alloc_inst);
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
void multimap<KeyType, MappedType, Compare, Allocator,
              SharedType>::open_shared_memory() {
  std::pair<MyMap *, boost::interprocess::managed_mapped_file::size_type> res;
  res = segment.find<MyMap>(name.c_str());
  mymap = res.first;
}

template <typename KeyType, typename MappedType, typename Compare,
          typename Allocator, typename SharedType>
void multimap<KeyType, MappedType, Compare, Allocator,
              SharedType>::bind_functions() {
  /* Create a RPC server and map the methods to it. */
  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      std::function<bool(KeyType &, MappedType &)> putFunc(
          std::bind(&multimap<KeyType, MappedType, Compare, Allocator,
                              SharedType>::LocalPut,
                    this, std::placeholders::_1, std::placeholders::_2));
      std::function<std::pair<bool, MappedType>(KeyType &)> getFunc(
          std::bind(&multimap<KeyType, MappedType, Compare, Allocator,
                              SharedType>::LocalGet,
                    this, std::placeholders::_1));
      std::function<std::pair<bool, MappedType>(KeyType &)> eraseFunc(
          std::bind(&multimap<KeyType, MappedType, Compare, Allocator,
                              SharedType>::LocalErase,
                    this, std::placeholders::_1));
      std::function<std::vector<std::pair<KeyType, MappedType>>(void)>
          getAllDataInServerFunc(
              std::bind(&multimap<KeyType, MappedType, Compare, Allocator,
                                  SharedType>::LocalGetAllDataInServer,
                        this));
      std::function<std::vector<std::pair<KeyType, MappedType>>(KeyType &)>
          containsInServerFunc(std::bind(
              &multimap<KeyType, MappedType, Compare>::LocalContainsInServer,
              this, std::placeholders::_1));

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

      std::function<void(const tl::request &, KeyType &, MappedType &)> putFunc(
          std::bind(&multimap<KeyType, MappedType, Compare, Allocator,
                              SharedType>::ThalliumLocalPut,
                    this, std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3));
      std::function<void(const tl::request &, KeyType &)> getFunc(
          std::bind(&multimap<KeyType, MappedType, Compare, Allocator,
                              SharedType>::ThalliumLocalGet,
                    this, std::placeholders::_1, std::placeholders::_2));
      std::function<void(const tl::request &, KeyType &)> eraseFunc(
          std::bind(&multimap<KeyType, MappedType, Compare, Allocator,
                              SharedType>::ThalliumLocalErase,
                    this, std::placeholders::_1, std::placeholders::_2));
      std::function<void(const tl::request &)> getAllDataInServerFunc(
          std::bind(&multimap<KeyType, MappedType, Compare, Allocator,
                              SharedType>::ThalliumLocalGetAllDataInServer,
                    this, std::placeholders::_1));
      std::function<void(const tl::request &, KeyType &)> containsInServerFunc(
          std::bind(&multimap<KeyType, MappedType,
                              Compare>::ThalliumLocalContainsInServer,
                    this, std::placeholders::_1, std::placeholders::_2));

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

#endif  // INCLUDE_HCL_MULTIMAP_MULTIMAP_CPP_
