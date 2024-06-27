//
// Created by kfeng on 3/30/23.
//

#ifndef CHRONOLOG_COMMON_H
#define CHRONOLOG_COMMON_H

#include <regex>
#include <fstream>
#include <numeric>
#include <complex>
#include "chrono_monitor.h"

//#define NO_LFENCE
//#define NO_MFENCE

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

#define TEST_REPS (10000 * 1000)

double cpu_base_frequency()
{
    std::regex re(
            "model name\\s*:[^@]+@\\s*([0-9.]+)\\s*GHz"); // seems only work for Intel CPUs, AMD CPUs do not show frequency in cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::smatch m;
    for(std::string line; getline(cpuinfo, line);)
    {
        regex_match(line, m, re);
        if(m.size() == 2)
        {
            LOG_INFO("[Common] CPU freq obtained from /proc/cpuinfo: {} GHz", std::stod(m[1]));
            return std::stod(m[1]);
        }
    }
    double freq = 3.8; // base clock of Ryzen 7 5700G is 3.8GHz
    LOG_ERROR("[Common] Fail to get CPU freq from /proc/cpuinfo, returning hard-coded freq {} GHz", freq);
    return freq;
}

double const CPU_GHZ_INV = 1 / cpu_base_frequency();

static long diff_in_ns(struct timespec t1, struct timespec t2)
{
    struct timespec diff;
    if(t2.tv_nsec - t1.tv_nsec < 0)
    {
        diff.tv_sec = t2.tv_sec - t1.tv_sec - 1;
        diff.tv_nsec = t2.tv_nsec - t1.tv_nsec + 1000000000;
    }
    else
    {
        diff.tv_sec = t2.tv_sec - t1.tv_sec;
        diff.tv_nsec = t2.tv_nsec - t1.tv_nsec;
    }
    return (diff.tv_sec * 1000000000.0 + diff.tv_nsec);
}

template <class T>
void printVectorStats(std::vector <T> v)
{
    T min = *std::min_element(v.begin(), v.end());
    T max = *std::max_element(v.begin(), v.end());
    size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + n, v.end());
    double median = v[n];
    double sum = std::accumulate(v.begin(), v.end(), 0.0);
    double mean = sum / v.size();

    std::vector <double> diff(v.size());
    std::transform(v.begin(), v.end(), diff.begin(), [mean](double x)
    { return x - mean; });
    double sq_sum = std::inner_product(diff.begin(), diff.end(), diff.begin(), 0.0);
    double stdev = std::sqrt(sq_sum / v.size());
    LOG_INFO("[Common] Min/Max/Mean/Median/Stddev (ns)\n{}\t{}\t{}\t{}\t{}", min, max, mean, median, stdev);
}

template <class T>
void writeVectorStatsToFile(std::vector <T> v)
{
    sort(v.begin(), v.end());
    std::ofstream fout("data.csv");
    //    fout << "clocksource\t0.001%\t0.1%\t1%\t50%\t99%\t99.9%\t99.99%" << std::endl;
    fout << "clocksource\t0.001%\t0.1%\t50%\t99.9%\t99.99%" << std::endl;
    T min = v.at(0);
    T whislo = v.at(TEST_REPS * 0.0001);
    T q1 = v.at(TEST_REPS * 0.001);
    //    T q2 = v.at(TEST_REPS * 0.01);
    T med = v.at(TEST_REPS * 0.5);
    T q3 = v.at(TEST_REPS * 0.99);
    //    T q4 = v.at(TEST_REPS * 0.999);
    T whishi = v.at(TEST_REPS * 0.9999 - 1);
    T max = v.at(TEST_REPS - 2);
    fout << min << "\t" << whislo << "\t" << q1 << "\t"
         //         << q2 << "\t"
         << med << "\t" << q3 << "\t"
         //         << q4 << "\t"
         << whishi << "\t" << max << std::endl;
}

template <class T>
void writeListToFile(T*clock_list, int len, std::string &fname)
{
    std::ofstream outFile(fname, std::ios::trunc);
    if(!outFile.is_open())
    {
        LOG_ERROR("[Common] Failed to open output file.");
        exit(1);
    }

    for(int i = 0; i < len; i++)
    {
        outFile << *(clock_list + i) << std::endl;
    }

    outFile.close();
}

template <class T>
void writeVecToFile(std::vector <T> &clock_list, int len, std::string &fname)
{
    std::ofstream outFile(fname, std::ios::trunc);
    if(!outFile.is_open())
    {
        LOG_ERROR("[Common] Failed to open output file.");
        exit(1);
    }

    for(int i = 0; i < len; i++)
    {
        outFile << clock_list[i] << std::endl;
    }

    outFile.close();
}

using namespace std::chrono;
using my_clock = steady_clock;

inline void emulate_event_interval(int sleep_time)
{
    if(sleep_time != 0)
    {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = sleep_time;
        nanosleep(&ts, nullptr);
    }
}

unsigned long long*collect_w_rdtsc(int test_reps, int sleep_time = 0)
{
    unsigned long long*clock_list = (unsigned long long*)malloc((test_reps + 1) * sizeof(unsigned long long));
    for(int i = 0; i < test_reps + 1; i++)
    {
        mfence();
        lfence();
        clock_list[i] = __builtin_ia32_rdtsc();
        emulate_event_interval(sleep_time);
    }

    return clock_list;
}

unsigned long long*collect_w_rdtscp(int test_reps, unsigned int proc_id, int sleep_time = 0)
{
    unsigned long long*clock_list = (unsigned long long*)malloc((test_reps + 1) * sizeof(unsigned long long));
    for(int i = 0; i < test_reps + 1; i++)
    {
        clock_list[i] = __builtin_ia32_rdtscp(&proc_id);
        lfence();
        emulate_event_interval(sleep_time);
    }

    return clock_list;
}

struct timespec*collect_w_clock_gettime_tai(int test_reps, int sleep_time = 0)
{
    struct timespec*clock_list = (struct timespec*)malloc((test_reps + 1) * sizeof(struct timespec));
    for(int i = 0; i < test_reps + 1; i++)
    {
        mfence();
        lfence();
        clock_gettime(CLOCK_TAI, &clock_list[i]);
        emulate_event_interval(sleep_time);
    }

    return clock_list;
}

struct timespec*collect_w_clock_gettime_mono(int test_reps, int sleep_time = 0)
{
    struct timespec*clock_list = (struct timespec*)malloc((test_reps + 1) * sizeof(struct timespec));
    for(int i = 0; i < test_reps + 1; i++)
    {
        mfence();
        lfence();
        clock_gettime(CLOCK_MONOTONIC, &clock_list[i]);
        emulate_event_interval(sleep_time);
    }

    return clock_list;
}

struct timespec*collect_w_clock_gettime_mono_raw(int test_reps, int sleep_time = 0)
{
    struct timespec*clock_list = (struct timespec*)malloc((test_reps + 1) * sizeof(struct timespec));
    for(int i = 0; i < test_reps + 1; i++)
    {
        mfence();
        lfence();
        clock_gettime(CLOCK_MONOTONIC_RAW, &clock_list[i]);
        emulate_event_interval(sleep_time);
    }

    return clock_list;
}

my_clock::time_point*collect_w_std_high_res_clock_alap(int test_reps, int sleep_time = 0)
{
    my_clock::time_point*clock_list = (my_clock::time_point*)malloc((test_reps + 1) * sizeof(my_clock::time_point));
    for(int i = 0; i < test_reps; i++)
    {
        mfence();
        lfence();
        clock_list[i] = my_clock::now();
        emulate_event_interval(sleep_time);
    }

    return clock_list;
}

unsigned long long*collect_w_std_high_res_clock_imme(int test_reps, int sleep_time = 0)
{
    unsigned long long*clock_list = (unsigned long long*)malloc((test_reps + 1) * sizeof(unsigned long long));
    for(int i = 0; i < test_reps; i++)
    {
        mfence();
        lfence();
        clock_list[i] = my_clock::now().time_since_epoch().count();
        emulate_event_interval(sleep_time);
    }

    return clock_list;
}

void convert_ull_to_double_vec(unsigned long long*clock_list, int len, std::vector <double> &duration_list)
{
    duration_list.reserve(len);
    for(int i = 0; i < len; i++)
        duration_list.push_back((clock_list[i + 1] - clock_list[i]) * CPU_GHZ_INV);
}

void convert_timespec_to_ull_vec(struct timespec*clock_list, int len, std::vector <unsigned long long> &duration_list)
{
    duration_list.reserve(len);
    for(int i = 0; i < len; i++)
    {
        mfence();
        lfence();
        duration_list.push_back(diff_in_ns(clock_list[i], clock_list[i + 1]));
    }
}

void convert_time_point_to_double_vec(my_clock::time_point*clock_list, int len, std::vector <double> &duration_list)
{
    std::vector <duration <double, std::nano>> duration_list_nanosecond;
    duration_list_nanosecond.reserve(len);
    duration_list.reserve(len);
    for(int i = 0; i < len; i++)
    {
        mfence();
        lfence();
        duration_list_nanosecond.emplace_back(clock_list[i + 1] - clock_list[i]);
    }
    for(int i = 0; i < len; i++)
    {
        mfence();
        lfence();
        duration_list.push_back(duration_list_nanosecond[i].count());
    }
}

template <class T>
void printStatLine(std::ofstream &fout, std::vector <T> &duration_list)
{
    int len = duration_list.size();
    fout << duration_list.at(0) << "\t" << duration_list.at(len * 0.001) << "\t" << duration_list.at(len * 0.01) << "\t"
         << duration_list.at(len * 0.5) << "\t" << duration_list.at(len * 0.99) << "\t" << duration_list.at(len * 0.999)
         << "\t" << duration_list.at(len - 1) << std::endl;
}

#endif //CHRONOLOG_COMMON_H
