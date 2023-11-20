//
// Created by kfeng on 1/20/22.
//

#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <numeric>
#include <time.h>

#ifdef    NO_LFENCE
#define   lfence()
#else

#include <emmintrin.h>

#define   lfence()  _mm_lfence()
#endif

#ifdef    NO_MFENCE
#define   mfence()
#else

#include <emmintrin.h>

#define   mfence()  _mm_mfence()
#endif

enum test_mode
{
    NO_SYNC = 0, MUTEX = 1, ATOMIC = 2, TSC = 3, CLOCK_GETTIME = 4
};

#define TEST_DURATION 10

using namespace std;

int test_mode;
uint64_t event_counter = 0;
atomic_uint64_t atomic_event_counter(0);

mutex counter_mutex;
mutex print_mutex;
__volatile__ bool stop = false;
vector <double> access_time_list;
mutex access_time_list_mutex;

uint64_t read_tsc()
{
    unsigned int proc_id;
    uint64_t timestamp;
    timestamp = __builtin_ia32_rdtscp(&proc_id);
    lfence();
    return timestamp;
}

void thread_func()
{
    uint64_t access_count = 0;
    __volatile__ uint64_t timestamp;
    struct timespec timestamp_ts;

    while(!stop)
    {
        if(test_mode == MUTEX)
        {
            lock_guard <mutex> guard(counter_mutex);
            event_counter++;
        }
        else if(test_mode == ATOMIC)
        {
            atomic_event_counter++;
        }
        else if(test_mode == TSC)
        {
            timestamp = read_tsc();
        }
        else if(test_mode == CLOCK_GETTIME)
        {
            clock_gettime(CLOCK_MONOTONIC, &timestamp_ts);
        }
        access_count++;
    }

    this_thread::sleep_for(chrono::milliseconds(100));
    lock_guard <mutex> print_guard(print_mutex);
    double access_time = TEST_DURATION * 1000000000.0 / access_count;
    lock_guard <mutex> time_guard(access_time_list_mutex);
    access_time_list.emplace_back(access_time);
}

int main(int argc, char**argv)
{
    if(argc < 3)
    {
        cout << "Usage: ./sync_overhead_test mode #threads" << endl << "mode = 0: no sync" << "mode = 1: use std::mutex"
             << endl << "mode = 2: use std::atomic" << endl << "mode = 3: use TSC" << endl
             << "mode = 4: use clock_gettime" << endl;
        exit(-1);
    }

    test_mode = atoi(argv[1]);
    int num_thrds = atoi(argv[2]);
    thread*thrds = new thread[num_thrds];
    for(int i = 0; i < num_thrds; i++)
    {
        thrds[i] = thread(thread_func);
    }

    this_thread::sleep_for(std::chrono::seconds(TEST_DURATION));

    stop = true;
    for(int i = 0; i < num_thrds; i++)
    {
        thrds[i].join();
    }

    double sum = accumulate(access_time_list.begin(), access_time_list.end(), 0.0);
    double ave_access_time = sum / access_time_list.size();
    cout << "Average access time across " << num_thrds << " thread(s): " << ave_access_time << " ns" << endl;

    delete[]thrds;
}