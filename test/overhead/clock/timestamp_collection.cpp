//
// Created by kfeng on 3/30/23.
//

#include <string>
#include <iostream>
#include <thread>
#include <sched.h>
#include "common.h"

#define NUM_TIMESTAMPS (1000 * 1000 * 10)

void rdtscp_thread(int thread_id) {
    int cpu = sched_getcpu();
    int sleep_time = 1000000;
    std::cout << __FUNCTION__  << thread_id << " is running on CPU core " << cpu <<
              " w/ " << sleep_time << " ns sleep to emulate event interval" << std::endl;
    unsigned int proc_id;
    unsigned long long *clock_list = collect_w_rdtscp(NUM_TIMESTAMPS, proc_id, sleep_time);
    std::string fname = __FUNCTION__ + std::to_string(thread_id);
    writeListToFile(clock_list, NUM_TIMESTAMPS, fname);
}

void clock_gettime_thread(int thread_id) {
    int cpu = sched_getcpu();
    int sleep_time = 1000000;
    std::cout << __FUNCTION__  << thread_id << " is running on CPU core " << cpu <<
              " w/ " << sleep_time << " ns sleep to emulate event interval" << std::endl;
    struct timespec *clock_list = collect_w_clock_gettime_mono(NUM_TIMESTAMPS, sleep_time);
    std::string fname = __FUNCTION__ + std::to_string(thread_id);
    std::vector<unsigned long long> clock_list_ull;
    for (int i = 0; i < NUM_TIMESTAMPS; i++)
        clock_list_ull.emplace_back(clock_list[i].tv_sec * 1e9 + clock_list[i].tv_nsec);
    writeVecToFile(clock_list_ull, NUM_TIMESTAMPS, fname);
}

int main(int argc, char *argv[]) {
    long n_threads = 24;
    if (argc > 1) n_threads = std::strtol(argv[1], nullptr, 10);

    std::vector<std::thread> threads;

    for (int i = 0; i < n_threads; i++) {
//        threads.emplace_back(rdtscp_thread, i);
        threads.emplace_back(clock_gettime_thread, i);
    }

    for (auto &thread: threads) {
        thread.join();
    }

    return 0;
}