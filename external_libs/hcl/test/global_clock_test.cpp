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

#include <hcl/clock/global_clock.h>
#include <mpi.h>

#include <iostream>

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);
  int my_rank, comm_size;
  MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

  bool debug = false;
  int ranks_per_server = comm_size, num_request = 100;
  long size_of_request = 1024 * 16;
  bool server_on_node = true;
  if (argc > 1) ranks_per_server = atoi(argv[1]);
  if (argc > 2) num_request = atoi(argv[2]);
  if (argc > 3) size_of_request = (long)atol(argv[3]);
  if (argc > 4) server_on_node = (bool)atoi(argv[4]);
  if (argc > 5) debug = (bool)atoi(argv[5]);
  if (debug && my_rank == 0) {
    printf("%d ready for attach\n", comm_size);
    fflush(stdout);
    getchar();
  }
  MPI_Barrier(MPI_COMM_WORLD);

  char *server_list_path = std::getenv("SERVER_LIST_PATH");
  bool is_server = (my_rank + 1) % ranks_per_server == 0;
  int my_server = my_rank / ranks_per_server;
  int num_servers = comm_size / ranks_per_server;

  if (my_rank == 0) {
    std::cout << num_servers << " Server Test" << std::endl;
  }
  MPI_Barrier(MPI_COMM_WORLD);
  HCL_CONF->IS_SERVER = is_server;
  HCL_CONF->MY_SERVER = my_server;
  HCL_CONF->NUM_SERVERS = num_servers;
  HCL_CONF->SERVER_ON_NODE = server_on_node || is_server;
  HCL_CONF->SERVER_LIST_PATH = std::string(server_list_path) + "server_list";
  hcl::global_clock *clock = new hcl::global_clock();

  for (uint16_t i = 0; i < comm_size; i++) {
    if (i == my_rank) {
      std::cout << "Time rank " << my_rank << ": " << clock->GetTime()
                << std::endl;
      for (uint16_t j = 0; j < num_servers; j++) {
        std::cout << "Time server " << j << " from rank " << my_rank << ": "
                  << clock->GetTimeServer(j) << std::endl;
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);
  }

  delete clock;
  MPI_Finalize();
}
