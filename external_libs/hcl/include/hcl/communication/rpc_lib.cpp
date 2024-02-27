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

#ifndef INCLUDE_HCL_COMMUNICATION_RPC_LIB_CPP_
#define INCLUDE_HCL_COMMUNICATION_RPC_LIB_CPP_

template <typename F>
void RPC::bind(CharStruct str, F func) {
  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      rpclib_server->bind(str.c_str(), func);
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
      thallium_server->define(str.string(), func);
      break;
    }
#endif
  }
}
template <typename Response, typename... Args>
Response RPC::callWithTimeout(uint16_t server_index, int timeout_ms,
                              CharStruct const &func_name, Args... args) {
  AutoTrace trace = AutoTrace("RPC::call", server_index, func_name);
  int16_t port = server_port + server_index;

  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      auto *client = rpclib_clients[server_index].get();
      if (client->get_connection_state() !=
          rpc::client::connection_state::connected) {
        rpclib_clients[server_index] = std::make_unique<rpc::client>(
            server_list[server_index].c_str(), server_port + server_index);
        client = rpclib_clients[server_index].get();
      }
      client->set_timeout(timeout_ms);
      Response response =
          client->call(func_name.c_str(), std::forward<Args>(args)...);
      client->clear_timeout();
      return response;
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_TCP
    case THALLIUM_TCP: {
      tl::remote_procedure remote_procedure =
          thallium_client->define(func_name.c_str());
      // Setup args for RDMA bulk transfer
      // std::vector<std::pair<void*,std::size_t>> segments(num_args);

      return remote_procedure.on(thallium_endpoints[server_index])(
          std::forward<Args>(args)...);
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_ROCE
    case THALLIUM_ROCE: {
      tl::remote_procedure remote_procedure =
          thallium_client->define(func_name.c_str());
      return remote_procedure.on(thallium_endpoints[server_index])(
          std::forward<Args>(args)...);
      break;
    }
#endif
  }
}
template <typename Response, typename... Args>
Response RPC::call(uint16_t server_index, CharStruct const &func_name,
                   Args... args) {
  AutoTrace trace = AutoTrace("RPC::call", server_index, func_name);
  int16_t port = server_port + server_index;

  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      auto *client = rpclib_clients[server_index].get();
      if (client->get_connection_state() !=
          rpc::client::connection_state::connected) {
        rpclib_clients[server_index] = std::make_unique<rpc::client>(
            server_list[server_index].c_str(), server_port + server_index);
        client = rpclib_clients[server_index].get();
      }
      /*client.set_timeout(5000);*/
      return client->call(func_name.c_str(), std::forward<Args>(args)...);
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_TCP
    case THALLIUM_TCP: {
      tl::remote_procedure remote_procedure =
          thallium_client->define(func_name.c_str());
      return remote_procedure.on(thallium_endpoints[server_index])(
          std::forward<Args>(args)...);
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_ROCE
    case THALLIUM_ROCE: {
      tl::remote_procedure remote_procedure =
          thallium_client->define(func_name.c_str());
      return remote_procedure.on(thallium_endpoints[server_index])(
          std::forward<Args>(args)...);
      break;
    }
#endif
  }
}

template <typename Response, typename... Args>
Response RPC::call(CharStruct &server, uint16_t &port,
                   CharStruct const &func_name, Args... args) {
  AutoTrace trace = AutoTrace("RPC::call", server, port, func_name);
  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      auto client = rpc::client(server.c_str(), port);
      /*client.set_timeout(5000);*/
      return client.call(func_name.c_str(), std::forward<Args>(args)...);
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_TCP
    case THALLIUM_TCP: {
      tl::remote_procedure remote_procedure =
          thallium_client->define(func_name.c_str());
      auto end_point = get_endpoint(HCL_CONF->TCP_CONF, server, port);
      return remote_procedure.on(end_point)(std::forward<Args>(args)...);
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_ROCE
    case THALLIUM_ROCE: {
      tl::remote_procedure remote_procedure =
          thallium_client->define(func_name.c_str());
      auto end_point = get_endpoint(HCL_CONF->TCP_CONF, server, port);
      return remote_procedure.on(end_point)(std::forward<Args>(args)...);
      break;
    }
#endif
  }
}

template <typename Response, typename... Args>
std::future<Response> RPC::async_call(uint16_t server_index,
                                      CharStruct const &func_name,
                                      Args... args) {
  AutoTrace trace = AutoTrace("RPC::call", server_index, func_name);
  int16_t port = server_port + server_index;

  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      auto *client = rpclib_clients[server_index].get();
      if (client->get_connection_state() !=
          rpc::client::connection_state::connected) {
        rpclib_clients[server_index] = std::make_unique<rpc::client>(
            server_list[server_index].c_str(), server_port + server_index);
        client = rpclib_clients[server_index].get();
      }
      // client.set_timeout(5000);
      return client->async_call(func_name.c_str(), std::forward<Args>(args)...);
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_TCP
    case THALLIUM_TCP: {
      // TODO:NotImplemented error
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_ROCE
    case THALLIUM_ROCE: {
      // TODO:NotImplemented error
      break;
    }
#endif
  }
}

template <typename Response, typename... Args>
std::future<Response> RPC::async_call(CharStruct &server, uint16_t &port,
                                      CharStruct const &func_name,
                                      Args... args) {
  AutoTrace trace = AutoTrace("RPC::async_call", server, port, func_name);

  switch (HCL_CONF->RPC_IMPLEMENTATION) {
#ifdef HCL_ENABLE_RPCLIB
    case RPCLIB: {
      auto client = rpc::client(server.c_str(), port);
      // client.set_timeout(5000);
      return client.async_call(func_name.c_str(), std::forward<Args>(args)...);
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_TCP
    case THALLIUM_TCP: {
      // TODO:NotImplemented error
      break;
    }
#endif
#ifdef HCL_ENABLE_THALLIUM_ROCE
    case THALLIUM_ROCE: {
      // TODO:NotImplemented error
      break;
    }
#endif
  }
}

#ifdef HCL_ENABLE_THALLIUM_ROCE
// These are still experimental for using RDMA bulk transfers
template <typename MappedType>
MappedType RPC::prep_rdma_server(tl::endpoint endpoint, tl::bulk &bulk_handle) {
  // MappedType buffer;
  std::string buffer;
  buffer.resize(1000000, 'a');
  std::vector<std::pair<void *, size_t>> segments(1);
  segments[0].first = (void *)(&buffer[0]);
  segments[0].second = 1000000 + 1;
  tl::bulk local = thallium_server->expose(segments, tl::bulk_mode::write_only);
  bulk_handle.on(endpoint) >> local;
  return buffer;
}

template <typename MappedType>
tl::bulk RPC::prep_rdma_client(MappedType &data) {
  MappedType my_buffer = data;
  std::vector<std::pair<void *, std::size_t>> segments(1);
  segments[0].first = (void *)&my_buffer[0];
  segments[0].second = 1000000 + 1;
  return thallium_client->expose(segments, tl::bulk_mode::read_only);
}
#endif

#endif  // INCLUDE_HCL_COMMUNICATION_RPC_LIB_CPP_
