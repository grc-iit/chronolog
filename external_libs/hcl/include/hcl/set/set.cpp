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

#ifndef INCLUDE_HCL_SET_SET_CPP_
#define INCLUDE_HCL_SET_SET_CPP_

/* Constructor to deallocate the shared memory*/
template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
set<KeyType, Hash, Compare, Allocator, SharedType>::~set() {
  this->container::~container();
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
set<KeyType, Hash, Compare, Allocator, SharedType>::set(CharStruct name_,
                                                        uint16_t port)
    : container(name_, port), myset() {
  AutoTrace trace = AutoTrace("hcl::set");
  if (is_server) {
    construct_shared_memory();
    bind_functions();
  } else if (!is_server && server_on_node) {
    open_shared_memory();
  }
}

/**
 * Put the data into the local set.
 * @param key, the key for put
 * @param data, the value for put
 * @return bool, true if Put was successful else false.
 */
template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
bool set<KeyType, Hash, Compare, Allocator, SharedType>::LocalPut(
    KeyType &key) {
  AutoTrace trace = AutoTrace("hcl::set::Put(local)", key);
  boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
      lock(*mutex);
  auto value = GetData<Allocator, KeyType, SharedType>(key);
  myset->insert(value);

  return true;
}

/**
 * Put the data into the set. Uses key to decide the server to hash it to,
 * @param key, the key for put
 * @param data, the value for put
 * @return bool, true if Put was successful else false.
 */
template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
bool set<KeyType, Hash, Compare, Allocator, SharedType>::Put(KeyType &key) {
  size_t key_hash = keyHash(key);
  uint16_t key_int = static_cast<uint16_t>(key_hash % num_servers);
  if (is_local(key_int)) {
    return LocalPut(key);
  } else {
    AutoTrace trace = AutoTrace("hcl::set::Put(remote)", key);
    return RPC_CALL_WRAPPER("_Put", key_int, bool, key);
  }
}

/**
 * Get the data in the local set.
 * @param key, key to get
 * @return return a pair of bool and Value. If bool is true then
 * data was found and is present in value part else bool is set to false
 */
template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
bool set<KeyType, Hash, Compare, Allocator, SharedType>::LocalGet(
    KeyType &key) {
  AutoTrace trace = AutoTrace("hcl::set::Get(local)", key);
  boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
      lock(*mutex);
  typename MySet::iterator iterator = myset->find(key);
  if (iterator != myset->end()) {
    return true;
  } else {
    return false;
  }
}

/**
 * Get the data in the set. Uses key to decide the server to hash it to,
 * @param key, key to get
 * @return return a pair of bool and Value. If bool is true then
 * data was found and is present in value part else bool is set to false
 */
template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
bool set<KeyType, Hash, Compare, Allocator, SharedType>::Get(KeyType &key) {
  size_t key_hash = keyHash(key);
  uint16_t key_int = key_hash % num_servers;
  if (is_local(key_int)) {
    return LocalGet(key);
  } else {
    AutoTrace trace = AutoTrace("hcl::set::Get(remote)", key);
    typedef bool ret_type;
    return RPC_CALL_WRAPPER("_Get", key_int, ret_type, key);
  }
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
bool set<KeyType, Hash, Compare, Allocator, SharedType>::LocalErase(
    KeyType &key) {
  AutoTrace trace = AutoTrace("hcl::set::Erase(local)", key);
  boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
      lock(*mutex);
  size_t s = myset->erase(key);

  return s > 0;
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
bool set<KeyType, Hash, Compare, Allocator, SharedType>::Erase(KeyType &key) {
  size_t key_hash = keyHash(key);
  uint16_t key_int = key_hash % num_servers;
  if (is_local(key_int)) {
    return LocalErase(key);
  } else {
    AutoTrace trace = AutoTrace("hcl::set::Erase(remote)", key);
    typedef bool ret_type;
    return RPC_CALL_WRAPPER("_Erase", key_int, ret_type, key);
  }
}

/**
 * Get the data into the set. Uses key to decide the server to hash it to,
 * @param key, key to get
 * @return return a pair of bool and Value. If bool is true then data was
 * found and is present in value part else bool is set to false
 */
template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::vector<KeyType>
set<KeyType, Hash, Compare, Allocator, SharedType>::Contains(KeyType &key_start,
                                                             KeyType &key_end) {
  AutoTrace trace = AutoTrace("hcl::set::Contains", key_start, key_end);
  std::vector<KeyType> final_values = std::vector<KeyType>();
  auto current_server = ContainsInServer(key_start, key_end);
  final_values.insert(final_values.end(), current_server.begin(),
                      current_server.end());
  for (int i = 0; i < num_servers; ++i) {
    if (i != my_server) {
      typedef std::vector<KeyType> ret_type;
      auto server =
          RPC_CALL_WRAPPER("_Contains", i, ret_type, key_start, key_end);
      final_values.insert(final_values.end(), server.begin(), server.end());
    }
  }
  return final_values;
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::vector<KeyType>
set<KeyType, Hash, Compare, Allocator, SharedType>::GetAllData() {
  AutoTrace trace = AutoTrace("hcl::set::GetAllData");
  std::vector<KeyType> final_values = std::vector<KeyType>();
  auto current_server = GetAllDataInServer();
  final_values.insert(final_values.end(), current_server.begin(),
                      current_server.end());
  for (int i = 0; i < num_servers; ++i) {
    if (i != my_server) {
      typedef std::vector<KeyType> ret_type;
      auto server = RPC_CALL_WRAPPER1("_GetAllData", i, ret_type);
      final_values.insert(final_values.end(), server.begin(), server.end());
    }
  }
  return final_values;
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::vector<KeyType> set<KeyType, Hash, Compare, Allocator,
                         SharedType>::LocalContainsInServer(KeyType &key_start,
                                                            KeyType &key_end) {
  AutoTrace trace = AutoTrace("hcl::set::ContainsInServer", key_start, key_end);
  std::vector<KeyType> final_values = std::vector<KeyType>();
  {
    boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
        lock(*mutex);
    typename MySet::iterator lower_bound;
    size_t size = myset->size();
    if (size == 0) {
    } else if (size == 1) {
      lower_bound = myset->begin();
      if (*lower_bound >= key_start)
        final_values.insert(final_values.end(), *lower_bound);
    } else {
      lower_bound = myset->lower_bound(key_start);
      /*KeyType k=*lower_bound;*/
      if (lower_bound == myset->end()) return final_values;
      if (lower_bound != myset->begin()) {
        --lower_bound;
        /*k=*lower_bound;*/
        if (key_start > *lower_bound) lower_bound++;
      }
      /*k=*lower_bound;*/
      while (lower_bound != myset->end()) {
        if (*lower_bound > key_end) break;
        final_values.insert(final_values.end(), *lower_bound);
        lower_bound++;
      }
    }
  }
  return final_values;
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::vector<KeyType> set<KeyType, Hash, Compare, Allocator,
                         SharedType>::ContainsInServer(KeyType &key_start,
                                                       KeyType &key_end) {
  if (is_local()) {
    return LocalContainsInServer(key_start, key_end);
  } else {
    typedef std::vector<KeyType> ret_type;
    auto my_server_i = my_server;
    return RPC_CALL_WRAPPER("_Contains", my_server_i, ret_type, key_start,
                            key_end);
  }
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::vector<KeyType>
set<KeyType, Hash, Compare, Allocator, SharedType>::LocalGetAllDataInServer() {
  AutoTrace trace = AutoTrace("hcl::set::GetAllDataInServer", NULL);
  std::vector<KeyType> final_values = std::vector<KeyType>();
  {
    boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
        lock(*mutex);
    typename MySet::iterator lower_bound;
    lower_bound = myset->begin();
    while (lower_bound != myset->end()) {
      final_values.insert(final_values.end(), *lower_bound);
      lower_bound++;
    }
  }
  return final_values;
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::vector<KeyType>
set<KeyType, Hash, Compare, Allocator, SharedType>::GetAllDataInServer() {
  if (is_local()) {
    return LocalGetAllDataInServer();
  } else {
    typedef std::vector<KeyType> ret_type;
    auto my_server_i = my_server;
    return RPC_CALL_WRAPPER1("_GetAllData", my_server_i, ret_type);
  }
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, KeyType>
set<KeyType, Hash, Compare, Allocator, SharedType>::LocalSeekFirst() {
  AutoTrace trace = AutoTrace("hcl::set::SeekFirst(local)");
  bip::scoped_lock<bip::interprocess_mutex> lock(*mutex);
  if (myset->size() > 0) {
    auto iterator = myset->begin();  // We want First (smallest) value in set
    KeyType value = *iterator;
    return std::pair<bool, KeyType>(true, value);
  }
  return std::pair<bool, KeyType>(false, KeyType());
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, KeyType> set<KeyType, Hash, Compare, Allocator,
                             SharedType>::SeekFirst(uint16_t &key_int) {
  if (is_local(key_int)) {
    return LocalSeekFirst();
  } else {
    AutoTrace trace = AutoTrace("hcl::set::SeekFirst(remote)", key_int);
    typedef std::pair<bool, KeyType> ret_type;
    return RPC_CALL_WRAPPER1("_SeekFirst", key_int, ret_type);
  }
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, std::vector<KeyType>>
set<KeyType, Hash, Compare, Allocator, SharedType>::LocalSeekFirstN(
    uint32_t n) {
  AutoTrace trace = AutoTrace("hcl::set::LocalSeekFirstN(local)");
  bip::scoped_lock<bip::interprocess_mutex> lock(*mutex);
  auto keys = std::vector<KeyType>();
  auto iterator = myset->begin();
  int i = 0;
  while (iterator != myset->end() && i < n) {
    keys.push_back(*iterator);
    i++;
    iterator++;
  }
  return std::pair<bool, std::vector<KeyType>>(i > 0, keys);
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, std::vector<KeyType>>
set<KeyType, Hash, Compare, Allocator, SharedType>::SeekFirstN(
    uint16_t &key_int, uint32_t n) {
  if (is_local(key_int)) {
    return LocalSeekFirstN(n);
  } else {
    AutoTrace trace = AutoTrace("hcl::set::SeekFirstN(remote)", key_int, n);
    typedef std::pair<bool, KeyType> ret_type;
    return RPC_CALL_WRAPPER("_SeekFirstN", key_int, ret_type, n);
  }
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, KeyType>
set<KeyType, Hash, Compare, Allocator, SharedType>::LocalPopFirst() {
  AutoTrace trace = AutoTrace("hcl::set::PopFirst(local)");
  bip::scoped_lock<bip::interprocess_mutex> lock(*mutex);
  if (myset->size() > 0) {
    auto iterator = myset->begin();  // We want First (smallest) value in set
    KeyType value = *iterator;
    myset->erase(iterator);
    return std::pair<bool, KeyType>(true, value);
  }
  return std::pair<bool, KeyType>(false, KeyType());
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, KeyType> set<KeyType, Hash, Compare, Allocator,
                             SharedType>::PopFirst(uint16_t &key_int) {
  if (is_local(key_int)) {
    return LocalPopFirst();
  } else {
    AutoTrace trace = AutoTrace("hcl::set::PopFirst(remote)", key_int);
    typedef std::pair<bool, KeyType> ret_type;
    return RPC_CALL_WRAPPER1("_PopFirst", key_int, ret_type);
  }
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
size_t set<KeyType, Hash, Compare, Allocator, SharedType>::LocalSize() {
  AutoTrace trace = AutoTrace("hcl::set::Size(local)");
  return myset->size();
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
size_t set<KeyType, Hash, Compare, Allocator, SharedType>::Size(
    uint16_t &key_int) {
  if (is_local(key_int)) {
    return LocalSize();
  } else {
    AutoTrace trace = AutoTrace("hcl::set::Size(remote)", key_int);
    typedef size_t ret_type;
    return RPC_CALL_WRAPPER1("_Size", key_int, ret_type);
  }
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
void set<KeyType, Hash, Compare, Allocator,
         SharedType>::construct_shared_memory() {
  ShmemAllocator alloc_inst(segment.get_segment_manager());
  /* Construct set in the shared memory space. */
  myset = segment.construct<MySet>(name.c_str())(Compare(), alloc_inst);
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
void set<KeyType, Hash, Compare, Allocator, SharedType>::open_shared_memory() {
  std::pair<MySet *, boost::interprocess::managed_mapped_file::size_type> res;
  res = segment.find<MySet>(name.c_str());
  myset = res.first;
}

template <typename KeyType, typename Hash, typename Compare, typename Allocator,
          typename SharedType>
void set<KeyType, Hash, Compare, Allocator, SharedType>::bind_functions() {
  /* Create a RPC server and map the methods to it. */
  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      std::function<bool(KeyType &)> putFunc(std::bind(
          &set<KeyType, Hash, Compare, Allocator, SharedType>::LocalPut, this,
          std::placeholders::_1));
      std::function<bool(KeyType &)> getFunc(std::bind(
          &set<KeyType, Hash, Compare, Allocator, SharedType>::LocalGet, this,
          std::placeholders::_1));
      std::function<bool(KeyType &)> eraseFunc(std::bind(
          &set<KeyType, Hash, Compare, Allocator, SharedType>::LocalErase, this,
          std::placeholders::_1));
      std::function<std::vector<KeyType>(void)> getAllDataInServerFunc(
          std::bind(&set<KeyType, Hash, Compare, Allocator,
                         SharedType>::LocalGetAllDataInServer,
                    this));
      std::function<std::vector<KeyType>(KeyType &, KeyType &)>
          containsInServerFunc(
              std::bind(&set<KeyType, Hash, Compare, Allocator,
                             SharedType>::LocalContainsInServer,
                        this, std::placeholders::_1, std::placeholders::_2));
      std::function<std::pair<bool, KeyType>(void)> seekFirstFunc(std::bind(
          &set<KeyType, Hash, Compare, Allocator, SharedType>::LocalSeekFirst,
          this));
      std::function<std::pair<bool, KeyType>(void)> popFirstFunc(std::bind(
          &set<KeyType, Hash, Compare, Allocator, SharedType>::LocalPopFirst,
          this));
      std::function<size_t(void)> sizeFunc(std::bind(
          &set<KeyType, Hash, Compare, Allocator, SharedType>::LocalSize,
          this));
      std::function<std::pair<bool, std::vector<KeyType>>(uint32_t)>
          localSeekFirstNFunc(std::bind(&set<KeyType, Hash, Compare, Allocator,
                                             SharedType>::LocalSeekFirstN,
                                        this, std::placeholders::_1));
      rpc->bind(func_prefix + "_Put", putFunc);
      rpc->bind(func_prefix + "_Get", getFunc);
      rpc->bind(func_prefix + "_Erase", eraseFunc);
      rpc->bind(func_prefix + "_GetAllData", getAllDataInServerFunc);
      rpc->bind(func_prefix + "_Contains", containsInServerFunc);

      rpc->bind(func_prefix + "_SeekFirst", seekFirstFunc);
      rpc->bind(func_prefix + "_PopFirst", popFirstFunc);
      rpc->bind(func_prefix + "_SeekFirstN", localSeekFirstNFunc);
      rpc->bind(func_prefix + "_Size", sizeFunc);
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

      std::function<void(const tl::request &, KeyType &)> putFunc(std::bind(
          &set<KeyType, Hash, Compare, Allocator, SharedType>::ThalliumLocalPut,
          this, std::placeholders::_1, std::placeholders::_2));
      std::function<void(const tl::request &, KeyType &)> getFunc(std::bind(
          &set<KeyType, Hash, Compare, Allocator, SharedType>::ThalliumLocalGet,
          this, std::placeholders::_1, std::placeholders::_2));
      std::function<void(const tl::request &, KeyType &)> eraseFunc(
          std::bind(&set<KeyType, Hash, Compare, Allocator,
                         SharedType>::ThalliumLocalErase,
                    this, std::placeholders::_1, std::placeholders::_2));
      std::function<void(const tl::request &)> getAllDataInServerFunc(
          std::bind(&set<KeyType, Hash, Compare, Allocator,
                         SharedType>::ThalliumLocalGetAllDataInServer,
                    this, std::placeholders::_1));
      std::function<void(const tl::request &, KeyType &, KeyType &)>
          containsInServerFunc(
              std::bind(&set<KeyType, Hash, Compare, Allocator,
                             SharedType>::ThalliumLocalContainsInServer,
                        this, std::placeholders::_1, std::placeholders::_2,
                        std::placeholders::_3));
      std::function<void(const tl::request &)> seekFirstFunc(
          std::bind(&set<KeyType, Hash, Compare, Allocator,
                         SharedType>::ThalliumLocalSeekFirst,
                    this, std::placeholders::_1));
      std::function<void(const tl::request &)> popFirstFunc(
          std::bind(&set<KeyType, Hash, Compare, Allocator,
                         SharedType>::ThalliumLocalPopFirst,
                    this, std::placeholders::_1));
      std::function<void(const tl::request &)> sizeFunc(
          std::bind(&set<KeyType, Hash, Compare, Allocator,
                         SharedType>::ThalliumLocalSize,
                    this, std::placeholders::_1));
      std::function<void(const tl::request &, uint32_t)> localSeekFirstNFunc(
          std::bind(&set<KeyType, Hash, Compare, Allocator,
                         SharedType>::ThalliumLocalSeekFirstN,
                    this, std::placeholders::_1, std::placeholders::_2));
      rpc->bind(func_prefix + "_Put", putFunc);
      rpc->bind(func_prefix + "_Get", getFunc);
      rpc->bind(func_prefix + "_Erase", eraseFunc);
      rpc->bind(func_prefix + "_GetAllData", getAllDataInServerFunc);
      rpc->bind(func_prefix + "_Contains", containsInServerFunc);

      rpc->bind(func_prefix + "_SeekFirst", seekFirstFunc);
      rpc->bind(func_prefix + "_PopFirst", popFirstFunc);
      // rpc->bind(func_prefix+"_SeekFirstN", localSeekFirstNFunc);
      rpc->bind(func_prefix + "_Size", sizeFunc);
      break;
    }
#endif
  }
}

#endif  // INCLUDE_HCL_SET_SET_CPP_
