#include <string>
#include <iostream>
#include <chrono>
#include <ctime>
#include <unistd.h>
#include "common.h"

int main(int argc, char *argv[]) {
    sleep(10);
    int test_reps = TEST_REPS;

    std::ofstream fout("data.csv");
    fout << "clocksource\tmin\t0.01%\t0.1%\t50%\t99%\t99.9%\tmax" << std::endl;

    /**
     * using rdtsc instruction from CPU
     */
    {
        unsigned long long *clock_list = collect_w_rdtsc(test_reps);
        std::vector<double> duration_list;
        convert_ull_to_double_vec(clock_list, test_reps, duration_list);
        std::cout << "Test with __builtin_ia32_rdtsc() calls: " << std::endl;
        std::cout << "CPU_GHZ_INV: " << CPU_GHZ_INV << std::endl;
        printVectorStats(duration_list);
        std::cout << "Resolution: " << std::endl << "N/A" << std::endl;
        sort(duration_list.begin(), duration_list.end());
        fout << "RDTSC\t";
        printStatLine(fout, duration_list);
        free(clock_list);
    }

    /**
     * using rdtscp instruction from CPU
     */
    {
        unsigned int proc_id;
        unsigned long long *clock_list = collect_w_rdtscp(test_reps, proc_id);
        std::vector<double> duration_list;
        convert_ull_to_double_vec(clock_list, test_reps, duration_list);
        std::cout << "\nTest with __builtin_ia32_rdtscp() calls on processor " << proc_id << ": " << std::endl;
        std::cout << "CPU_GHZ_INV: " << CPU_GHZ_INV << std::endl;
        printVectorStats(duration_list);
//        writeVectorStatsToFile(duration_list);
        sort(duration_list.begin(), duration_list.end());
        fout << "RDTSCP\t";
        printStatLine(fout, duration_list);
        std::cout << "Resolution: " << std::endl << "N/A" << std::endl;
        free(clock_list);
    }

    /**
     * using clock_gettime from C
     */
    {
        // use clock_gettime(CLOCK_MONOTONIC)
        struct timespec *clock_list = collect_w_clock_gettime_mono(test_reps);
        std::vector<unsigned long long> duration_list;
        convert_timespec_to_ull_vec(clock_list, test_reps, duration_list);
        struct timespec res;
        std::cout << "\nTest with clock_gettime(CLOCK_MONOTONIC) calls: " << std::endl;
        printVectorStats(duration_list);
//        writeVectorStatsToFile(duration_list);
        sort(duration_list.begin(), duration_list.end());
        fout << "CLOCK_MONOTONIC\t";
        printStatLine(fout, duration_list);
        clock_getres(CLOCK_MONOTONIC, &res);
        std::cout << "Resolution (ns): " << std::endl
                  << res.tv_sec * 1e9 + res.tv_nsec << std::endl;
        free(clock_list);
    }

    {
        // use clock_gettime(CLOCK_MONOTONIC_RAW)
        struct timespec *clock_list = collect_w_clock_gettime_mono_raw(test_reps);
        std::vector<unsigned long long> duration_list;
        convert_timespec_to_ull_vec(clock_list, test_reps, duration_list);
        struct timespec res;
        std::cout << "\nTest with clock_gettime(CLOCK_MONOTONIC_RAW) calls: " << std::endl;
        printVectorStats(duration_list);
        sort(duration_list.begin(), duration_list.end());
        fout << "CLOCK_MONOTONIC_RAW\t";
        printStatLine(fout, duration_list);
        clock_getres(CLOCK_MONOTONIC_RAW, &res);
        std::cout << "Resolution (ns): " << std::endl
                  << res.tv_sec * 1e9 + res.tv_nsec << std::endl;
        free(clock_list);
    }

    /**
     * using std::chrono from C++
     */
    {
        // get time_point first, calculate timestamp as late as possible
        my_clock::time_point *clock_list = collect_w_std_high_res_clock_alap(test_reps);
        std::vector<double> duration_list;
        convert_time_point_to_double_vec(clock_list, test_reps, duration_list);
        std::cout << "\nTest with std::chrono::high_resolution_clock::now() calls (count as late as possible): "
                  << std::endl;
        printVectorStats(duration_list);
        sort(duration_list.begin(), duration_list.end());
        fout << "C++11(alap)\t";
        printStatLine(fout, duration_list);
        std::cout << "Resolution (ns)" << std::endl
                  << (double) std::chrono::high_resolution_clock::period::num /
                     std::chrono::high_resolution_clock::period::den * 1e9 << std::endl;
        free(clock_list);
    }

    {
        // calculate timestamp immediately after getting time_point
        unsigned long long *clock_list = collect_w_std_high_res_clock_imme(test_reps);
        std::vector<double> duration_list;
        convert_ull_to_double_vec(clock_list, test_reps, duration_list);
        std::cout << "\nTest with std::chrono::high_resolution_clock::now() calls (count immediately): " << std::endl;
        printVectorStats(duration_list);
        sort(duration_list.begin(), duration_list.end());
        fout << "C++11(imme)\t";
        printStatLine(fout, duration_list);
        std::cout << "Resolution (ns):" << std::endl
                  << (double)std::chrono::high_resolution_clock::period::num /
                     std::chrono::high_resolution_clock::period::den * 1e9 << std::endl;
        free(clock_list);
    }
}
