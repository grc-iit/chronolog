//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <random>
#include <thallium.hpp>
#include <array>
#include <thallium/serialization/stl/vector.hpp>
#include <mpi.h>

#include <common.h>
#include <chrono_monitor.h>

#define WARM_UP_REPS 3

namespace tl = thallium;
using namespace std::chrono;
using my_clock = steady_clock;

void report_results(const std::string& mode,
                    long msg_size,
                    long repetition,
                    int nprocs,
                    double duration_ave,
                    double duration_min,
                    double duration_max,
                    double duration_wall,
                    double duration_init_ave,
                    double duration_comm_ave)
{
    long effective_reps = repetition - WARM_UP_REPS;
    if(effective_reps < 1) effective_reps = 1;
    double avg_latency_us = duration_comm_ave / effective_reps;
    double bandwidth_MBps = ((double)msg_size * effective_reps) / (duration_comm_ave / 1e6) / (1024.0 * 1024.0);
    double agg_bandwidth_MBps = bandwidth_MBps * nprocs;

    std::cout << std::fixed
              << "mode:          " << mode << std::endl
              << "msg_size:      " << msg_size << std::endl
              << "repetitions:   " << effective_reps << std::endl
              << "nprocs:        " << nprocs << std::endl
              << "total_comm:    " << std::setprecision(3) << duration_comm_ave << " us" << std::endl
              << "avg_latency:   " << std::setprecision(3) << avg_latency_us << " us" << std::endl
              << "bw_per_client: " << std::setprecision(3) << bandwidth_MBps << " MB/s" << std::endl
              << "agg_bandwidth: " << std::setprecision(3) << agg_bandwidth_MBps << " MB/s" << std::endl;
}


void calculate_time(time_point<my_clock, nanoseconds>& t_bigbang,
                    time_point<my_clock, nanoseconds>& t_local_init,
                    time_point<my_clock, nanoseconds>& t_local_finish,
                    time_point<my_clock, nanoseconds>& t_global_finish,
                    int nprocs,
                    double& duration_ave,
                    double& duration_min,
                    double& duration_max,
                    double& duration_wall,
                    double& duration_init_ave,
                    double& duration_comm_ave,
                    long repetition)
{
    double duration_e2e = duration_cast<nanoseconds>(t_local_finish - t_bigbang).count() / 1000.0;
    double duration_init = duration_cast<nanoseconds>(t_local_init - t_bigbang).count() / 1000.0;
    double duration_comm = duration_cast<nanoseconds>(t_local_finish - t_local_init).count() / 1000.0;
    duration_wall = duration_cast<nanoseconds>(t_global_finish - t_bigbang).count() / 1000.0;
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
    duration_ave /= (nprocs);
    duration_init_ave /= (nprocs);
    duration_comm_ave /= (nprocs);
}

std::string get_server_address(const std::string& base_address, long num_servers, int rank)
{
    std::string host_ip = base_address.substr(0, base_address.rfind(':'));
    std::string base_port = base_address.substr(base_address.rfind(':') + 1);
    // select target server port round-robin by rank
    auto new_port = strtol(base_port.c_str(), nullptr, 10) + rank % num_servers;
    LOG_DEBUG("[ThalliumClientMPI] Selected server port based on rank {}: {}", rank, new_port);
    std::string new_addr_str = host_ip + ":" + std::to_string(new_port);
    LOG_INFO("[ThalliumClientMPI] Generated server address for engine: {}", new_addr_str);
    return new_addr_str;
}

int main(int argc, char** argv)
{
    if(argc < 5)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <address> <#servers> <sendrecv|recv|rdma> <msg_size> [repetition]" << std::endl;
        std::cerr << "  sendrecv  Round-trip echo via send/recv" << std::endl;
        std::cerr << "  recv      One-way send/recv (server responds with size only)" << std::endl;
        std::cerr << "  rdma      One-way RDMA pull (server pulls bulk, responds with size)" << std::endl;
        exit(0);
    }

    int nprocs, my_rank;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);

    int result = chronolog::chrono_monitor::initialize("console",
                                                       "thallium_client_mpi.log",
                                                       chronolog::LogLevel::info,
                                                       "thallium_client_mpi",
                                                       1048576,
                                                       5,
                                                       chronolog::LogLevel::warn);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    std::string base_address = argv[1];
    long num_servers = strtol(argv[2], nullptr, 10);
    std::string mode = argv[3];
    long msg_size = strtol(argv[4], nullptr, 10);
    long repetition = 1;
    if(argc > 5)
        repetition = strtol(argv[5], nullptr, 10);
    std::string server_address;

    // Ensure enough reps for warm-up
    if(repetition <= WARM_UP_REPS)
        repetition = WARM_UP_REPS + 1;

    std::string proto = base_address.substr(0, base_address.find_first_of(':'));
    std::vector<char> send_vec;
    send_vec.reserve(msg_size);
    static const char alphanum[] = "0123456789"
                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz";
    for(long i = 0; i < msg_size; ++i) { send_vec.push_back(alphanum[dist(mt) % (sizeof(alphanum) - 1)]); }
    double duration_min, duration_max, duration_ave, duration_wall;
    double duration_init_ave, duration_comm_ave;
    time_point<my_clock, nanoseconds> t_bigbang, t_local_init, t_local_finish, t_global_finish;

    if(mode == "sendrecv")
    {
        // Round-trip echo: client sends data, server copies and echoes it back
        MPI_Barrier(MPI_COMM_WORLD);
        t_bigbang = my_clock::now();
        tl::engine myEngine(proto, THALLIUM_CLIENT_MODE);
        server_address = get_server_address(base_address, num_servers, my_rank);
        std::string rpc_name = "repeater";
        LOG_DEBUG("[ThalliumClientMPI] Establishing sendrecv RPC with server at: {}", server_address);
        tl::remote_procedure repeater = myEngine.define(rpc_name);
        tl::endpoint server = myEngine.lookup(server_address);
        std::vector<char> ret_vec;
        for(int i = 0; i < WARM_UP_REPS; i++)
            ret_vec = repeater.on(server)(send_vec).as<std::vector<char>>();
        t_local_init = my_clock::now();
        for(long i = WARM_UP_REPS; i < repetition; i++)
        {
            ret_vec = repeater.on(server)(send_vec).as<std::vector<char>>();
        }
        t_local_finish = my_clock::now();
        MPI_Barrier(MPI_COMM_WORLD);
        t_global_finish = my_clock::now();
    }
    else if(mode == "recv")
    {
        // One-way send/recv: client sends data, server responds with size only
        MPI_Barrier(MPI_COMM_WORLD);
        t_bigbang = my_clock::now();
        tl::engine myEngine(proto, THALLIUM_CLIENT_MODE);
        server_address = get_server_address(base_address, num_servers, my_rank);
        std::string rpc_name = "receiver";
        LOG_DEBUG("[ThalliumClientMPI] Establishing recv RPC with server at: {}", server_address);
        tl::remote_procedure receiver = myEngine.define(rpc_name);
        tl::endpoint server = myEngine.lookup(server_address);
        for(int i = 0; i < WARM_UP_REPS; i++)
            receiver.on(server)(send_vec);
        t_local_init = my_clock::now();
        for(long i = WARM_UP_REPS; i < repetition; i++)
        {
            receiver.on(server)(send_vec);
        }
        t_local_finish = my_clock::now();
        MPI_Barrier(MPI_COMM_WORLD);
        t_global_finish = my_clock::now();
    }
    else if(mode == "rdma")
    {
        // RDMA: client exposes memory, server pulls it, responds with size
        MPI_Barrier(MPI_COMM_WORLD);
        t_bigbang = my_clock::now();
        tl::engine myEngine(proto, THALLIUM_CLIENT_MODE);
        server_address = get_server_address(base_address, num_servers, my_rank);
        std::string rpc_name = "rdma_put";
        LOG_DEBUG("[ThalliumClientMPI] Establishing RDMA RPC with server at: {}", server_address);
        tl::remote_procedure rdma_put = myEngine.define(rpc_name);
        tl::endpoint server = myEngine.lookup(server_address);
        std::vector<std::pair<void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&send_vec[0]);
        segments[0].second = send_vec.size();
        tl::bulk myBulk = myEngine.expose(segments, tl::bulk_mode::read_only);
        for(int i = 0; i < WARM_UP_REPS; i++) rdma_put.on(server)(myBulk);
        t_local_init = my_clock::now();
        for(long i = WARM_UP_REPS; i < repetition; i++) rdma_put.on(server)(myBulk);
        t_local_finish = my_clock::now();
        MPI_Barrier(MPI_COMM_WORLD);
        t_global_finish = my_clock::now();
    }
    else
    {
        LOG_ERROR("[ThalliumClientMPI] Invalid mode: {}. Use sendrecv, recv, or rdma.", mode);
        MPI_Finalize();
        return 1;
    }

    calculate_time(t_bigbang,
                   t_local_init,
                   t_local_finish,
                   t_global_finish,
                   nprocs,
                   duration_ave,
                   duration_min,
                   duration_max,
                   duration_wall,
                   duration_init_ave,
                   duration_comm_ave,
                   repetition);

    if(my_rank == 0)
    {
        report_results(mode, msg_size, repetition, nprocs,
                       duration_ave, duration_min, duration_max, duration_wall,
                       duration_init_ave, duration_comm_ave);
    }

    MPI_Finalize();

    return 0;
}
