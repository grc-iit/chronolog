//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <chrono>
#include <thallium.hpp>
#include <array>
#include <thallium/serialization/stl/vector.hpp>
#include "../../ChronoAPI/ChronoLog/include/common.h"
#include <mpi.h>
#include "chrono_monitor.h"

#define WARM_UP_REPS 3
#define MSG_SIZE (1 * 1024 * 1024)
//#define MSG_SIZE (106930)

namespace tl = thallium;
using namespace std::chrono;
using my_clock = steady_clock;

void report_results(double duration_ave, double duration_min, double duration_max, double duration_wall
                    , double duration_init_ave, double duration_comm_ave)
{
    // Logging the results with descriptive headers for better context.
    LOG_INFO("[Performance Metrics Report (msg_size={}MB)(us)]", MSG_SIZE / (1024.0 * 1024.0));
    LOG_INFO("-------------------------------------------------------------------------------------------------------");
    LOG_INFO("{:>16} {:>16} {:>16} {:>16} {:>16} {:>16}", "Ave Time", "Min Time", "Max Time", "Wall Time"
             , "Ave Init Time", "Ave Comm Time");
    LOG_INFO("-------------------------------------------------------------------------------------------------------");
    LOG_INFO("{:>16.4f} {:>16.4f} {:>16.4f} {:>16.4f} {:>16.4f} {:>16.4f}", duration_ave, duration_min, duration_max
             , duration_wall, duration_init_ave, duration_comm_ave);
}


void calculate_time(time_point <my_clock, nanoseconds> &t_bigbang, time_point <my_clock, nanoseconds> &t_local_init
                    , time_point <my_clock, nanoseconds> &t_local_finish
                    , time_point <my_clock, nanoseconds> &t_global_finish, int nprocs, double &duration_ave
                    , double &duration_min, double &duration_max, double &duration_wall, double &duration_init_ave
                    , double &duration_comm_ave, long repetition)
{
    double duration_e2e = duration_cast <nanoseconds>(t_local_finish - t_bigbang).count() / 1000.0;
    double duration_init = duration_cast <nanoseconds>(t_local_init - t_bigbang).count() / 1000.0;
    double duration_comm = duration_cast <nanoseconds>(t_local_finish - t_local_init).count() / 1000.0;
    duration_wall = duration_cast <nanoseconds>(t_global_finish - t_bigbang).count() / 1000.0;
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
    duration_comm_ave /= (double)(repetition - WARM_UP_REPS);
}

std::string get_server_address(const std::string &base_address, long num_servers, int rank)
{
    std::string host_ip = base_address.substr(0, base_address.rfind(':'));
    std::string base_port = base_address.substr(base_address.rfind(':') + 1);
    // randomly select target server port
    auto new_port = strtol(base_port.c_str(), nullptr, 10) + rank % num_servers;
    LOG_DEBUG("[ThalliumClientMPI] Selected server port based on rank {}: {}", rank, new_port);
    std::string new_addr_str = host_ip + ":" + std::to_string(new_port);
    LOG_INFO("[ThalliumClientMPI] Generated server address for engine: {}", new_addr_str);
    return new_addr_str;
}

int main(int argc, char**argv)
{
    if(argc < 4)
    {
        std::cerr << "Usage: " << argv[0] << " <address> <#servers> <sendrecv|rdma> [repetition]" << std::endl;
        exit(0);
    }

    int nprocs, my_rank;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);

    int result = chronolog::chrono_monitor::initialize("console", "thallium_client_mpi.log", spdlog::level::debug
                                                       , "thallium_client_mpi", 1048576, 5, spdlog::level::debug);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    std::string base_address = argv[1];
    long num_servers = strtol(argv[2], nullptr, 10);
    std::string mode = argv[3];
    long repetition = 1;
    if(argc > 4)
        repetition = strtol(argv[4], nullptr, 10);
    std::string server_address;

    std::string proto = base_address.substr(0, base_address.find_first_of(':'));
    std::vector <char> send_vec, ret_vec;
    send_vec.reserve(MSG_SIZE);
    static const char alphanum[] = "0123456789"
                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz";
    for(int i = 0; i < MSG_SIZE; ++i)
    {
        send_vec.push_back(alphanum[dist(mt) % (sizeof(alphanum) - 1)]);
    }
    double duration_min, duration_max, duration_ave, duration_wall;
    double duration_init_ave, duration_comm_ave;
    time_point <my_clock, nanoseconds> t_bigbang, t_local_init, t_local_finish, t_global_finish;

    if(!strcmp(mode.c_str(), "sendrecv"))
    {
        /* send/recv version */
        MPI_Barrier(MPI_COMM_WORLD);
        t_bigbang = my_clock::now();
        tl::engine myEngine(proto, THALLIUM_CLIENT_MODE);
        server_address = get_server_address(base_address, num_servers, my_rank);
        std::string rpc_name = "repeater";
        LOG_DEBUG("[ThalliumClientMPI] Attempting to establish RPC connection for sendrecv mode with server at: {}"
                  , server_address);
        tl::remote_procedure repeater = myEngine.define(rpc_name);
        tl::endpoint server = myEngine.lookup(server_address);
        tl::provider_handle ph(server);
        for(int i = 0; i < WARM_UP_REPS; i++)
            ret_vec = repeater.on(server)(send_vec).as <std::vector <char>>(); // warm up
        t_local_init = my_clock::now();
        for(int i = WARM_UP_REPS; i < repetition; i++)
        {
            ret_vec = repeater.on(server)(send_vec).as <std::vector <char>>();
//            if(ret_vec != send_vec)
//            {
//                LOG_ERROR("[ThalliumClientMPI] Mismatch detected: The server's response does not match the sent data.");
//            }
        }
        t_local_finish = my_clock::now();
        MPI_Barrier(MPI_COMM_WORLD);
        t_global_finish = my_clock::now();
    }
    else if(!strcmp(mode.c_str(), "rdma"))
    {
        /* RDMA version */
        MPI_Barrier(MPI_COMM_WORLD);
        t_bigbang = my_clock::now();
        tl::engine myEngine(proto, THALLIUM_CLIENT_MODE);
        server_address = get_server_address(base_address, num_servers, my_rank);
        std::string rpc_name = "rdma_put";
        LOG_DEBUG("[ThalliumClientMPI] Attempting to establish RPC connection for RDMA mode with server at: {}"
                  , server_address);
        tl::remote_procedure rdma_put = myEngine.define(rpc_name); //.disable_response();
        tl::endpoint server = myEngine.lookup(server_address);
        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&send_vec[0]);
        segments[0].second = send_vec.size() + 1;
        LOG_DEBUG("[ThalliumClientMPI] Exposing memory segments of size {} for RDMA mode.", send_vec.size() + 1);
        tl::bulk myBulk = myEngine.expose(segments, tl::bulk_mode::read_only);
        for(int i = 0; i < WARM_UP_REPS; i++)
            rdma_put.on(server)(myBulk);
        t_local_init = my_clock::now();
        for(int i = WARM_UP_REPS; i < repetition; i++)
            rdma_put.on(server)(myBulk);
        t_local_finish = my_clock::now();
        MPI_Barrier(MPI_COMM_WORLD);
        t_global_finish = my_clock::now();
    }
    else
    {
        LOG_ERROR("[ThalliumClientMPI] Invalid mode selected: {}", mode);
        exit(0);
    }

    calculate_time(t_bigbang, t_local_init, t_local_finish, t_global_finish, nprocs, duration_ave, duration_min
                   , duration_max, duration_wall, duration_init_ave, duration_comm_ave, repetition);

    if(my_rank == 0)
    {
        report_results(duration_ave, duration_min, duration_max, duration_wall, duration_init_ave, duration_comm_ave);
    }

    MPI_Finalize();

    return 0;
}
