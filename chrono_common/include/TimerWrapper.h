//
// Created by kfeng on 10/7/24.
//

#ifndef CHRONOLOG_TIMERWRAPPER_H
#define CHRONOLOG_TIMERWRAPPER_H

#include <iostream>
#include <chrono>
#include <functional>
#include <mpi.h>

class TimerWrapper
{
public:
    TimerWrapper(bool enable_timing, const std::string &name): name(name), enabled(enable_timing)
    {
        int rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        if(rank == 0)
        {
//            std::cout << "Resolution of timer " << name << ": " << MPI_Wtick() << " second.\n";
            int global_time_flag;
            int flag;
            MPI_Attr_get(MPI_COMM_SELF, MPI_WTIME_IS_GLOBAL, &global_time_flag, &flag);
            if(flag)
            {
                // If the attribute exists, print its value
                if(!global_time_flag)
                {
                    std::cerr << "MPI_Wtime clocks are NOT synchronized across all processes for timer " << name <<
                    std::endl;
                }
            }
            else
            {
                std::cerr << "MPI_WTIME_IS_GLOBAL is not available in this MPI implementation for timer " << name <<
                std::endl;
            }
        }
    }

    // Timing wrapper for functions that return non-void
    template <typename Func, typename... Args, typename RetType = decltype(std::invoke(std::declval <Func>()
                                                                                       , std::declval <Args>()...)), typename std::enable_if <!std::is_void <RetType>::value, int>::type = 0>
    auto timeBlock(Func &&func, Args &&... args) -> RetType
    {
        if(enabled)
        {
            int rank, size;
            MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            MPI_Comm_size(MPI_COMM_WORLD, &size);
            double local_start_time = MPI_Wtime();
            auto result = std::invoke(std::forward <Func>(func), std::forward <Args>(
                    args)...);  // Call function or block
            double local_end_time = MPI_Wtime();
            double local_elapsed = local_end_time - local_start_time;
            double global_start_time, global_end_time;
            MPI_Reduce(&local_start_time, &global_start_time, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
            MPI_Reduce(&local_end_time, &global_end_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
            double elapsed_max, elapsed_min, elapsed_ave;
            MPI_Reduce(&local_elapsed, &elapsed_max, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
            MPI_Reduce(&local_elapsed, &elapsed_min, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
            MPI_Reduce(&local_elapsed, &elapsed_ave, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
            double global_elapsed = global_end_time - global_start_time;
            if(rank == 0)
            {
                e2e_duration += global_elapsed;
                duration_min += elapsed_min;
                duration_ave += elapsed_ave;
                duration_max += elapsed_max;
            }
            return result;  // Return the result of the function or block
        }
        else
        {
            return std::invoke(std::forward <Func>(func), std::forward <Args>(args)...);  // Call without timing
        }
    }

    // Timing wrapper for functions that return void
    template <typename Func, typename... Args, typename RetType = decltype(std::invoke(std::declval <Func>()
                                                                                       , std::declval <Args>()...)), typename std::enable_if <std::is_void <RetType>::value, int>::type = 0>
    void timeBlock(Func &&func, Args &&... args)
    {
        if(enabled)
        {
            int rank, size;
            MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            MPI_Comm_size(MPI_COMM_WORLD, &size);
            double local_start_time = MPI_Wtime();
            std::invoke(std::forward <Func>(func), std::forward <Args>(args)...);  // Call function or block
            double local_end_time = MPI_Wtime();
            double local_elapsed = local_end_time - local_start_time;
            double global_start_time, global_end_time;
            MPI_Reduce(&local_start_time, &global_start_time, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
            MPI_Reduce(&local_end_time, &global_end_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
            double elapsed_max, elapsed_min, elapsed_ave;
            MPI_Reduce(&local_elapsed, &elapsed_max, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
            MPI_Reduce(&local_elapsed, &elapsed_min, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
            MPI_Reduce(&local_elapsed, &elapsed_ave, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
            double global_elapsed = global_end_time - global_start_time;
            if(rank == 0)
            {
                e2e_duration += global_elapsed;
                duration_max += elapsed_max;
                duration_min += elapsed_min;
                duration_ave += elapsed_ave;
            }
        }
        else
        {
            std::invoke(std::forward <Func>(func), std::forward <Args>(args)...);  // Call without timing
        }
    }

    // Pause timer
    void pauseTimer()
    {
        pause_time = MPI_Wtime();
    }

    // Resume timer
    void resumeTimer()
    {
        double pause_end_time = MPI_Wtime();
        double pause_duration = pause_end_time - pause_time;
        e2e_duration -= pause_duration;
    }

    // Reset timer
    void resetTimer()
    {
        e2e_duration = 0;
        duration_max = 0;
        duration_min = 0;
        duration_ave = 0;
    }

    // End timer
    void report()
    {
        int rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        if(rank == 0)
        {
            std::cout << "Wall time in " << name << ": " << e2e_duration << " s.\n";
            std::cout << "Min of local time in " << name << ": " << duration_min << " s.\n";
            std::cout << "Ave of local time in " << name << ": " << duration_ave << " s.\n";
            std::cout << "Max of local time in " << name << ": " << duration_max << " s.\n";
        }
    }

    // Get end-to-end duration of last call
    [[nodiscard]] double getDuration() const
    {
        return e2e_duration;
    }

    // Get max duration of last call
    [[nodiscard]] double getDurationMax() const
    {
        return duration_max;
    }

    // Get min duration of last call
    [[nodiscard]] double getDurationMin() const
    {
        return duration_min;
    }

    // Get ave duration of last call
    [[nodiscard]] double getDurationAve() const
    {
        return duration_ave;
    }

private:
    std::string name;  // Name of the timer
    bool enabled;  // Flag to enable or disable timing
    double e2e_duration;  // End-to-end duration of last call
    double duration_min;  // Min duration of last call
    double duration_ave;  // Ave duration of last call
    double duration_max;  // Max duration of last call
    double pause_time;  // Time of pause
};

#endif //CHRONOLOG_TIMERWRAPPER_H
