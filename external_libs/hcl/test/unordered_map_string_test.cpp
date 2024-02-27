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

#include <execinfo.h>
#include <hcl/common/data_structures.h>
#include <hcl/unordered_map/unordered_map.h>
#include <mpi.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <utility>

struct KeyType {
  size_t a;
  KeyType() : a(0) {}
  KeyType(size_t a_) : a(a_) {}
#ifdef HCL_ENABLE_RPCLIB
  MSGPACK_DEFINE(a);
#endif
  /* equal operator for comparing two Matrix. */
  bool operator==(const KeyType &o) const { return a == o.a; }
  KeyType &operator=(const KeyType &other) {
    a = other.a;
    return *this;
  }
  bool operator<(const KeyType &o) const { return a < o.a; }
  bool operator>(const KeyType &o) const { return a > o.a; }
  bool Contains(const KeyType &o) const { return a == o.a; }
};
#if defined(HCL_ENABLE_THALLIUM_TCP) || defined(HCL_ENABLE_THALLIUM_ROCE)
template <typename A>
void serialize(A &ar, KeyType &a) {
  ar &a.a;
}
#endif
namespace std {
template <>
struct hash<KeyType> {
  size_t operator()(const KeyType &k) const { return k.a; }
};
}  // namespace std

int main(int argc, char *argv[]) {
  int provided;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
  if (provided < MPI_THREAD_MULTIPLE) {
    printf("Didn't receive appropriate MPI threading specification\n");
    exit(EXIT_FAILURE);
  }
  int comm_size, my_rank;
  MPI_Comm_size(MPI_COMM_WORLD, &comm_size);
  MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
  int ranks_per_server = comm_size, num_request = 100;
  long size_of_request = 1024 * 16;
  bool debug = false;
  bool server_on_node = true;
  if (argc > 1) ranks_per_server = atoi(argv[1]);
  if (argc > 2) num_request = atoi(argv[2]);
  if (argc > 3) size_of_request = (long)atol(argv[3]);
  if (argc > 4) server_on_node = (bool)atoi(argv[4]);
  if (argc > 5) debug = (bool)atoi(argv[5]);

  /* if(comm_size/ranks_per_server < 2){
       perror("comm_size/ranks_per_server should be atleast 2 for this test\n");
       exit(-1);
   }*/
  int len;
  char processor_name[MPI_MAX_PROCESSOR_NAME];
  MPI_Get_processor_name(processor_name, &len);
  if (debug) {
    printf("%s/%d: %d\n", processor_name, my_rank, getpid());
  }

  char *server_list_path = std::getenv("SERVER_LIST_PATH");
  if (debug && my_rank == 0) {
    printf("%d ready for attach\n", comm_size);
    fflush(stdout);
    getchar();
  }
  MPI_Barrier(MPI_COMM_WORLD);
  bool is_server = (my_rank + 1) % ranks_per_server == 0;
  int my_server = my_rank / ranks_per_server;
  int num_servers = comm_size / ranks_per_server;

  // The following is used to switch to 40g network on Ares.
  // This is necessary when we use RoCE on Ares.
  std::string proc_name = std::string(processor_name);
  /*int split_loc = proc_name.find('.');
  std::string node_name = proc_name.substr(0, split_loc);
  std::string extra_info = proc_name.substr(split_loc+1, string::npos);
  proc_name = node_name + "-40g." + extra_info;*/

  std::string my_vals(size_of_request, 'x');

  HCL_CONF->IS_SERVER = is_server;
  HCL_CONF->MY_SERVER = my_server;
  HCL_CONF->NUM_SERVERS = num_servers;
  HCL_CONF->SERVER_ON_NODE = server_on_node || is_server;
  HCL_CONF->SERVER_LIST_PATH = std::string(server_list_path) + "server_list";

  typedef boost::interprocess::allocator<
      char, boost::interprocess::managed_mapped_file::segment_manager>
      CharAllocator;
  typedef bip::basic_string<char, std::char_traits<char>, CharAllocator>
      MappedUnitString;

  hcl::unordered_map<KeyType, std::string, std::hash<KeyType>, CharAllocator,
                     MappedUnitString> *map;

  if (is_server) {
    map = new hcl::unordered_map<KeyType, std::string, std::hash<KeyType>,
                                 CharAllocator, MappedUnitString>();
  }
  MPI_Barrier(MPI_COMM_WORLD);
  if (!is_server) {
    map = new hcl::unordered_map<KeyType, std::string, std::hash<KeyType>,
                                 CharAllocator, MappedUnitString>();
  }

  std::unordered_map<KeyType, std::string> lmap =
      std::unordered_map<KeyType, std::string>();

  MPI_Comm client_comm;
  MPI_Comm_split(MPI_COMM_WORLD, !is_server, my_rank, &client_comm);
  int client_comm_size;
  MPI_Comm_size(client_comm, &client_comm_size);
  MPI_Barrier(MPI_COMM_WORLD);
  if (!is_server) {
    Timer llocal_map_timer = Timer();
    std::hash<KeyType> keyHash;
    /*Local std::map test*/
    for (int i = 0; i < num_request; i++) {
      size_t val = my_server;
      llocal_map_timer.resumeTime();
      size_t key_hash = keyHash(KeyType(val)) % num_servers;
      if (key_hash == my_server && is_server) {
      }
      lmap.insert_or_assign(KeyType(val), my_vals);
      llocal_map_timer.pauseTime();
    }

    double llocal_map_throughput =
        num_request / llocal_map_timer.getElapsedTime() * 1000 *
        size_of_request * my_vals.size() / 1024 / 1024;

    Timer llocal_get_map_timer = Timer();
    for (int i = 0; i < num_request; i++) {
      size_t val = my_server;
      llocal_get_map_timer.resumeTime();
      size_t key_hash = keyHash(KeyType(val)) % num_servers;
      if (key_hash == my_server && is_server) {
      }
      auto iterator = lmap.find(KeyType(val));
      auto result = iterator->second;
      llocal_get_map_timer.pauseTime();
    }
    double llocal_get_map_throughput =
        num_request / llocal_get_map_timer.getElapsedTime() * 1000 *
        size_of_request * my_vals.size() / 1024 / 1024;

    if (my_rank == 0) {
      printf("llocal_map_throughput put: %f\n", llocal_map_throughput);
      printf("llocal_map_throughput get: %f\n", llocal_get_map_throughput);
    }
    MPI_Barrier(client_comm);

    if (HCL_CONF->SERVER_ON_NODE) {
      Timer local_map_timer = Timer();
      /*Local map test*/
      for (int i = 0; i < num_request; i++) {
        size_t val = my_server;
        auto key = KeyType(val);
        local_map_timer.resumeTime();
        auto result = map->Put(key, my_vals);
        local_map_timer.pauseTime();
        assert(result);
      }
      double local_map_throughput =
          num_request / local_map_timer.getElapsedTime() * 1000 *
          size_of_request * my_vals.size() / 1024 / 1024;

      Timer local_get_map_timer = Timer();
      /*Local map test*/
      for (int i = 0; i < num_request; i++) {
        size_t val = my_server;
        auto key = KeyType(val);
        local_get_map_timer.resumeTime();
        auto result = map->Get(key);
        local_get_map_timer.pauseTime();
        assert(result.first);
        assert(result.second.size() == size_of_request);
      }

      double local_get_map_throughput =
          num_request / local_get_map_timer.getElapsedTime() * 1000 *
          size_of_request * my_vals.size() / 1024 / 1024;

      double local_put_tp_result, local_get_tp_result;
      if (client_comm_size > 1) {
        MPI_Reduce(&local_map_throughput, &local_put_tp_result, 1, MPI_DOUBLE,
                   MPI_SUM, 0, client_comm);
        MPI_Reduce(&local_get_map_throughput, &local_get_tp_result, 1,
                   MPI_DOUBLE, MPI_SUM, 0, client_comm);
        local_put_tp_result /= client_comm_size;
        local_get_tp_result /= client_comm_size;
      } else {
        local_put_tp_result = local_map_throughput;
        local_get_tp_result = local_get_map_throughput;
      }

      if (my_rank == 0) {
        printf("local_map_throughput put: %f\n", local_put_tp_result);
        printf("local_map_throughput get: %f\n", local_get_tp_result);
      }
    }
    MPI_Barrier(client_comm);
    if (!HCL_CONF->SERVER_ON_NODE) {
      Timer remote_map_timer = Timer();
      /*Remote map test*/
      for (int i = 0; i < num_request; i++) {
        size_t val = my_server + 1;
        auto key = KeyType(val);
        remote_map_timer.resumeTime();
        auto result = map->Put(key, my_vals);
        remote_map_timer.pauseTime();
        assert(result);
      }
      double remote_map_throughput =
          num_request / remote_map_timer.getElapsedTime() * 1000 *
          size_of_request * my_vals.size() / 1024 / 1024;

      MPI_Barrier(client_comm);
      Timer remote_get_map_timer = Timer();
      /*Remote map test*/
      for (int i = 0; i < num_request; i++) {
        size_t val = my_server + 1;
        auto key = KeyType(val);
        remote_get_map_timer.resumeTime();
        auto result = map->Get(key);
        remote_get_map_timer.pauseTime();
        assert(result.first);
        assert(result.second.size() == size_of_request);
      }
      double remote_get_map_throughput =
          num_request / remote_get_map_timer.getElapsedTime() * 1000 *
          size_of_request * my_vals.size() / 1024 / 1024;

      double remote_put_tp_result, remote_get_tp_result;
      if (client_comm_size > 1) {
        MPI_Reduce(&remote_map_throughput, &remote_put_tp_result, 1, MPI_DOUBLE,
                   MPI_SUM, 0, client_comm);
        remote_put_tp_result /= client_comm_size;
        MPI_Reduce(&remote_get_map_throughput, &remote_get_tp_result, 1,
                   MPI_DOUBLE, MPI_SUM, 0, client_comm);
        remote_get_tp_result /= client_comm_size;
      } else {
        remote_put_tp_result = remote_map_throughput;
        remote_get_tp_result = remote_get_map_throughput;
      }

      if (my_rank == 0) {
        printf("remote map throughput (put): %f\n", remote_put_tp_result);
        printf("remote map throughput (get): %f\n", remote_get_tp_result);
      }
    }
  }
  MPI_Barrier(MPI_COMM_WORLD);
  delete (map);
  MPI_Finalize();
  exit(EXIT_SUCCESS);
}
