//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thallium.hpp>
#include <array>
#include <thallium/serialization/stl/vector.hpp>
#include "../../ChronoAPI/ChronoLog/include/common.h"
#include <mpi.h>
#include "log.h"
#include "unistd.h"

#define MSG_SIZE 100

namespace tl = thallium;
using namespace std::chrono;
using my_clock = steady_clock;

void report_results(double duration_ave, double duration_min, double duration_max, double duration_wall
                    , double duration_init_ave, double duration_comm_ave)
{
    // Logging the results with descriptive headers for better context.
    Logger::getLogger()->info("[Performance Metrics Report]");
    Logger::getLogger()->info("---------------------------------------------------------------");
    Logger::getLogger()->info("{:>20} {:>20} {:>20} {:>20} {:>20} {:>20}", "Average Duration", "Minimum Duration"
                              , "Maximum Duration", "Wall Duration", "Average Initialization Time"
                              , "Average Communication Time");
    Logger::getLogger()->info("---------------------------------------------------------------");
    Logger::getLogger()->info("{:>20.2f} {:>20.2f} {:>20.2f} {:>20.2f} {:>20.2f} {:>20.2f}", duration_ave, duration_min
                              , duration_max, duration_wall, duration_init_ave, duration_comm_ave);
}


void calculate_time(time_point <my_clock, nanoseconds> &t_bigbang, time_point <my_clock, nanoseconds> &t_local_init
                    , time_point <my_clock, nanoseconds> &t_local_finish
                    , time_point <my_clock, nanoseconds> &t_global_finish, int nprocs, double &duration_ave
                    , double &duration_min, double &duration_max, double &duration_wall, double &duration_init_ave
                    , double &duration_comm_ave, long repetition)
{
    double duration_e2e = std::chrono::duration <double>(t_local_finish - t_bigbang).count();
    double duration_init = std::chrono::duration <double>(t_local_init - t_bigbang).count();
    double duration_comm = std::chrono::duration <double>(t_local_finish - t_local_init).count();
    duration_wall = std::chrono::duration <double>(t_global_finish - t_bigbang).count();
    duration_min = 0;
    duration_max = 0;
    duration_ave = 0;
    duration_init_ave = 0;
    duration_comm_ave = 0;
    MPI_Allreduce(&duration_e2e, &duration_min, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
    MPI_Allreduce(&duration_e2e, &duration_max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&duration_e2e, &duration_ave, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&duration_init, &duration_init_ave, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&duration_comm, &duration_comm_ave, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    duration_ave /= nprocs;
    duration_init_ave /= nprocs;
    duration_comm_ave /= nprocs;
    duration_comm_ave /= repetition;
}

std::string get_server_address(const std::string &base_address, long num_servers, int rank)
{
    std::string host_ip = base_address.substr(0, base_address.rfind(':'));
    std::string base_port = base_address.substr(base_address.rfind(':') + 1);
    // randomly select target server port
    long max_port = stoi(base_port) + num_servers - 1;
    //  std::uniform_int_distribution<long> distr(stoi(base_port), max_port);
    //  long new_port = distr(mt);
    // round-robin select target server port
    int new_port = atoi(base_port.c_str()) + rank % num_servers;
    Logger::getLogger()->debug("[ThalliumClientMPI] Selected server port based on rank {}: {}", rank, new_port);
    std::string new_addr_str = host_ip + ":" + std::to_string(new_port);
    Logger::getLogger()->info("[ThalliumClientMPI] Generated server address for engine: {}", new_addr_str);
    return new_addr_str;
}

int main(int argc, char**argv)
{
    if(argc < 4)
    {
        Logger::getLogger()->error("[ThalliumClientMPI] Usage: {} <address> <#servers> <sendrecv|rdma> [repetition]"
                                   , argv[0]);
        exit(0);
    }

    int nprocs, my_rank;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);

    std::string base_address = argv[1];
    long num_servers = strtol(argv[2], nullptr, 10);
    std::string mode = argv[3];
    long repetition = 1;
    if(argc > 4)
        repetition = strtol(argv[4], nullptr, 10);
    std::string server_address;

    std::vector <char> send_vec, ret_vec;
    send_vec.reserve(MSG_SIZE);
    static const char alphanum[] = "0123456789"
                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz";
    for(int i = 0; i < MSG_SIZE; ++i)
    {
        send_vec[i] = alphanum[dist(mt) % (sizeof(alphanum) - 1)];
    }
    double duration_min, duration_max, duration_ave, duration_wall;
    double duration_init_ave, duration_comm_ave;
    time_point <my_clock, nanoseconds> t_bigbang, t_local_init, t_local_finish, t_global_finish;

    if(!strcmp(mode.c_str(), "sendrecv"))
    {
        /* send/recv version */
        MPI_Barrier(MPI_COMM_WORLD);
        t_bigbang = my_clock::now();
        tl::engine myEngine("ofi+sockets", THALLIUM_CLIENT_MODE);
        server_address = get_server_address(base_address, num_servers, my_rank);
        std::string rpc_name = "repeater";
        Logger::getLogger()->debug(
                "[ThalliumClientMPI] Attempting to establish RPC connection for sendrecv mode with server at: {}"
                , server_address);
        tl::remote_procedure repeater = myEngine.define(rpc_name);
        tl::endpoint server = myEngine.lookup(server_address);
        tl::provider_handle ph(server);
        t_local_init = my_clock::now();
        for(int i = 0; i < repetition; i++)
            repeater.on(server)(send_vec);
        t_local_finish = my_clock::now();
        MPI_Barrier(MPI_COMM_WORLD);
        t_global_finish = my_clock::now();
    }
    else if(!strcmp(mode.c_str(), "rdma"))
    {
        /* RDMA version */
        MPI_Barrier(MPI_COMM_WORLD);
        t_bigbang = my_clock::now();
        tl::engine myEngine("ofi+sockets", THALLIUM_CLIENT_MODE);
        server_address = get_server_address(base_address, num_servers, my_rank);
        std::string rpc_name = "rdma_put";
        Logger::getLogger()->debug(
                "[ThalliumClientMPI] Attempting to establish RPC connection for RDMA mode with server at: {}"
                , server_address);
        tl::remote_procedure rdma_put = myEngine.define(rpc_name).disable_response();
        tl::endpoint server = myEngine.lookup(server_address);
        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&send_vec[0]);
        segments[0].second = send_vec.size() + 1;
        tl::bulk myBulk = myEngine.expose(segments, tl::bulk_mode::read_only);
        t_local_init = my_clock::now();
        for(int i = 0; i < repetition; i++)
            rdma_put.on(server)(myBulk);
        t_local_finish = my_clock::now();
        MPI_Barrier(MPI_COMM_WORLD);
        t_global_finish = my_clock::now();
    }
    else
    {
        Logger::getLogger()->error("[ThalliumClientMPI] Invalid mode selected: {}", mode);
        exit(0);
    }

    calculate_time(t_bigbang, t_local_init, t_local_finish, t_global_finish, nprocs, duration_ave, duration_min
                   , duration_max, duration_wall, duration_init_ave, duration_comm_ave, repetition);

    if(my_rank == 0)
    {
        if(ret_vec != send_vec)
        {
            Logger::getLogger()->error(
                    "[ThalliumClientMPI] Mismatch detected: The server's response does not match the sent data.");
        }
        report_results(duration_ave, duration_min, duration_max, duration_wall, duration_init_ave, duration_comm_ave);
    }

    MPI_Finalize();

    return 0;
}