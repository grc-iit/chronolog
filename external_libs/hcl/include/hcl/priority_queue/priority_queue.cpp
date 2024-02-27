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

#ifndef INCLUDE_HCL_PRIORITY_QUEUE_PRIORITY_QUEUE_CPP_
#define INCLUDE_HCL_PRIORITY_QUEUE_PRIORITY_QUEUE_CPP_

/* Constructor to deallocate the shared memory*/
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
priority_queue<MappedType, Compare, Allocator, SharedType>::~priority_queue() {
  this->container::~container();
}

template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
priority_queue<MappedType, Compare, Allocator, SharedType>::priority_queue(
    CharStruct name_, uint16_t port)
    : container(name_, port), queue() {
  AutoTrace trace = AutoTrace("hcl::priority_queue");
  if (is_server) {
    construct_shared_memory();
    bind_functions();
  } else if (!is_server && server_on_node) {
    open_shared_memory();
  }
}

/**
 * Push the data into the local priority queue.
 * @param key, the key for put
 * @param data, the value for put
 * @return bool, true if Put was successful else false.
 */
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
bool priority_queue<MappedType, Compare, Allocator, SharedType>::LocalPush(
    MappedType &data) {
  AutoTrace trace = AutoTrace("hcl::priority_queue::Push(local)", data);
  bip::scoped_lock<bip::interprocess_mutex> lock(*mutex);
  auto value = GetData<Allocator, MappedType, SharedType>(data);
  queue->push(value);
  return true;
}

/**
 * Push the data into the priority queue. Uses key to decide the
 * server to hash it to,
 * @param key, the key for put
 * @param data, the value for put
 * @return bool, true if Put was successful else false.
 */
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
bool priority_queue<MappedType, Compare, Allocator, SharedType>::Push(
    MappedType &data, uint16_t &key_int) {
  if (is_local(key_int)) {
    return LocalPush(data);
  } else {
    AutoTrace trace =
        AutoTrace("hcl::priority_queue::Push(remote)", data, key_int);
    return RPC_CALL_WRAPPER("_Push", key_int, bool, data);
  }
}

/**
 * Get the data from the local priority queue.
 * @param key_int, key_int to know which server
 * @return return a pair of bool and Value. If bool is true then data was
 * found and is present in value part else bool is set to false
 */
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, MappedType>
priority_queue<MappedType, Compare, Allocator, SharedType>::LocalPop() {
  AutoTrace trace = AutoTrace("hcl::priority_queue::Pop(local)");
  bip::scoped_lock<bip::interprocess_mutex> lock(*mutex);
  if (queue->size() > 0) {
    MappedType value = queue->top();
    queue->pop();
    return std::pair<bool, MappedType>(true, value);
  }
  return std::pair<bool, MappedType>(false, MappedType());
}

/**
 * Get the data from the priority queue. Uses key_int to decide the
 * server to hash it to,
 * @param key_int, key_int to know which server
 * @return return a pair of bool and Value. If bool is true then data was
 * found and is present in value part else bool is set to false
 */
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, MappedType> priority_queue<MappedType, Compare, Allocator,
                                           SharedType>::Pop(uint16_t &key_int) {
  if (is_local(key_int)) {
    return LocalPop();
  } else {
    AutoTrace trace = AutoTrace("hcl::priority_queue::Pop(remote)", key_int);
    typedef std::pair<bool, MappedType> ret_type;
    return RPC_CALL_WRAPPER1("_Pop", key_int, ret_type);
  }
}

/**
 * Get the data from the local priority queue.
 * @param key_int, key_int to know which server
 * @return return a pair of bool and Value. If bool is true then data was
 * found and is present in value part else bool is set to false
 */
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, MappedType>
priority_queue<MappedType, Compare, Allocator, SharedType>::LocalTop() {
  AutoTrace trace = AutoTrace("hcl::priority_queue::Top(local)");
  bip::scoped_lock<bip::interprocess_mutex> lock(*mutex);
  if (queue->size() > 0) {
    MappedType value = queue->top();
    return std::pair<bool, MappedType>(true, value);
  }
  return std::pair<bool, MappedType>(false, MappedType());
  ;
}

/**
 * Get the data from the priority queue. Uses key_int to decide the
 * server to hash it to,
 * @param key_int, key_int to know which server
 * @return return a pair of bool and Value. If bool is true then data was
 * found and is present in value part else bool is set to false
 */
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
std::pair<bool, MappedType> priority_queue<MappedType, Compare, Allocator,
                                           SharedType>::Top(uint16_t &key_int) {
  if (is_local(key_int)) {
    return LocalTop();
  } else {
    AutoTrace trace = AutoTrace("hcl::priority_queue::Top(remote)", key_int);
    typedef std::pair<bool, MappedType> ret_type;
    return RPC_CALL_WRAPPER1("_Top", key_int, ret_type);
  }
}

/**
 * Get the size of the local priority queue.
 * @param key_int, key_int to know which server
 * @return return a size of the priority queue
 */
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
size_t priority_queue<MappedType, Compare, Allocator, SharedType>::LocalSize() {
  AutoTrace trace = AutoTrace("hcl::priority_queue::Size(local)");
  bip::scoped_lock<bip::interprocess_mutex> lock(*mutex);
  size_t value = queue->size();
  return value;
}

/**
 * Get the size of the priority queue. Uses key_int to decide the
 * server to hash it to,
 * @param key_int, key_int to know which server
 * @return return a size of the priority queue
 */
template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
size_t priority_queue<MappedType, Compare, Allocator, SharedType>::Size(
    uint16_t &key_int) {
  if (is_local(key_int)) {
    return LocalSize();
  } else {
    AutoTrace trace = AutoTrace("hcl::priority_queue::Top(remote)", key_int);
    return RPC_CALL_WRAPPER1("_Size", key_int, size_t);
  }
}

template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
void priority_queue<MappedType, Compare, Allocator,
                    SharedType>::construct_shared_memory() {
  ShmemAllocator alloc_inst(segment.get_segment_manager());
  /* Construct priority queue in the shared memory space. */
  queue = segment.construct<Queue>("Queue")(Compare(), alloc_inst);
}

template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
void priority_queue<MappedType, Compare, Allocator,
                    SharedType>::open_shared_memory() {
  std::pair<Queue *, bip::managed_mapped_file::size_type> res;
  res = segment.find<Queue>("Queue");
  queue = res.first;
}

template <typename MappedType, typename Compare, typename Allocator,
          typename SharedType>
void priority_queue<MappedType, Compare, Allocator,
                    SharedType>::bind_functions() {
  /* Create a RPC server and map the methods to it. */
  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      std::function<bool(MappedType &)> pushFunc(
          std::bind(&hcl::priority_queue<MappedType, Compare>::LocalPush, this,
                    std::placeholders::_1));
      std::function<std::pair<bool, MappedType>(void)> popFunc(
          std::bind(&hcl::priority_queue<MappedType, Compare>::LocalPop, this));
      std::function<size_t(void)> sizeFunc(std::bind(
          &hcl::priority_queue<MappedType, Compare>::LocalSize, this));
      std::function<std::pair<bool, MappedType>(void)> topFunc(
          std::bind(&hcl::priority_queue<MappedType, Compare>::LocalTop, this));
      rpc->bind(func_prefix + "_Push", pushFunc);
      rpc->bind(func_prefix + "_Pop", popFunc);
      rpc->bind(func_prefix + "_Top", topFunc);
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
      std::function<void(const tl::request &, MappedType &)> pushFunc(std::bind(
          &hcl::priority_queue<MappedType, Compare>::ThalliumLocalPush, this,
          std::placeholders::_1, std::placeholders::_2));
      std::function<void(const tl::request &)> popFunc(
          std::bind(&hcl::priority_queue<MappedType, Compare>::ThalliumLocalPop,
                    this, std::placeholders::_1));
      std::function<void(const tl::request &)> sizeFunc(std::bind(
          &hcl::priority_queue<MappedType, Compare>::ThalliumLocalSize, this,
          std::placeholders::_1));
      std::function<void(const tl::request &)> topFunc(
          std::bind(&hcl::priority_queue<MappedType, Compare>::ThalliumLocalTop,
                    this, std::placeholders::_1));
      rpc->bind(func_prefix + "_Push", pushFunc);
      rpc->bind(func_prefix + "_Pop", popFunc);
      rpc->bind(func_prefix + "_Top", topFunc);
      rpc->bind(func_prefix + "_Size", sizeFunc);
      break;
    }
#endif
  }
}

#endif  // INCLUDE_HCL_PRIORITY_QUEUE_PRIORITY_QUEUE_CPP_
