//
// Created by kfeng on 3/30/23.
//

#include <string>
#include <iostream>
#include <thread>
#include <sched.h>
#include <unistd.h>
#include <common.h>

#define NUM_TIMESTAMPS (1000 * 1000)

typedef struct thrd_args_
{
    int thread_id;
    int sleep_ns;
} thrd_args;

void rdtscp_thread(void*args)
{
    int cpu = sched_getcpu();
    thrd_args*arg = (thrd_args*)args;
    int thread_id = arg->thread_id;
    int sleep_time = arg->sleep_ns;
    LOG_INFO("[TimestampCollection] Thread ID: {} is executing function {} on CPU Core: {} with a sleep interval of {} ns."
         , thread_id, __FUNCTION__, cpu, sleep_time);
    unsigned int proc_id;
    unsigned long long*clock_list = collect_w_rdtscp(NUM_TIMESTAMPS, proc_id, sleep_time);
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    std::string fname = __FUNCTION__ + std::to_string(thread_id) + hostname;
    writeListToFile(clock_list, NUM_TIMESTAMPS, fname);
}

void clock_gettime_thread(void*args)
{
    int cpu = sched_getcpu();
    thrd_args*arg = (thrd_args*)args;
    int thread_id = arg->thread_id;
    int sleep_time = arg->sleep_ns;
    LOG_INFO("[TimestampCollection] Thread ID: {} is executing function {} on CPU Core: {} with a sleep interval of {} ns."
         , thread_id, __FUNCTION__, cpu, sleep_time);
    struct timespec*clock_list = collect_w_clock_gettime_tai(NUM_TIMESTAMPS, sleep_time);
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    std::string fname = __FUNCTION__ + std::to_string(thread_id) + hostname;
    std::vector <unsigned long long> clock_list_ull;
    for(int i = 0; i < NUM_TIMESTAMPS; i++)
        clock_list_ull.emplace_back(clock_list[i].tv_sec * 1e9 + clock_list[i].tv_nsec);
    writeVecToFile(clock_list_ull, NUM_TIMESTAMPS, fname);
    LOG_INFO("[TimestampCollection] Thread ID: {} completed execution. Timestamps saved to file: {}", thread_id, fname);
}

int main(int argc, char*argv[])
{
    long n_threads = 24;
    int sleep_ns = 0;
    if(argc > 1) n_threads = std::strtol(argv[1], nullptr, 10);
    if(argc > 2) sleep_ns = std::strtol(argv[2], nullptr, 10);

    std::vector <std::thread> threads;

    thrd_args*args = (thrd_args*)malloc(sizeof(thrd_args) * n_threads);
    for(int i = 0; i < n_threads; i++)
    {
        args[i].thread_id = i;
        args[i].sleep_ns = sleep_ns;
//        threads.emplace_back(rdtscp_thread, i);
        threads.emplace_back(clock_gettime_thread, &args[i]);
    }

    for(auto &thread: threads)
    {
        thread.join();
    }

    free(args);

    return 0;
}
