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

/*-------------------------------------------------------------------------
 *
 * Created: debug.h
 * June 5 2018
 * Hariharan Devarajan <hdevarajan@hdfgroup.org>
 *
 * Purpose: Defines debug macros for HCL.
 *
 *-------------------------------------------------------------------------
 */

#ifndef INCLUDE_HCL_COMMON_DEBUG_H_
#define INCLUDE_HCL_COMMON_DEBUG_H_

#include <execinfo.h>
#include <unistd.h>

#include <csignal>
#include <iostream>
#include <tuple>

/**
 * Handles signals and prints stack trace.
 *
 * @param sig
 */
inline void handler(int sig) {
  void *array[10];
  size_t size;
  // get void*'s for all entries on the stack
  size = backtrace(array, 300);
  int rank, comm_size;
  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(0);
}
/**
 * various macros to print variables and messages.
 */

#ifdef HCL_DEBUG
#define DBGVAR(var)                                                   \
  std::cout << "DBG: " << __FILE__ << "(" << __LINE__ << ") " << #var \
            << " = [" << (var) << "]" << std::endl

#define DBGVAR2(var1, var2)                                                \
  std::cout << "DBG: " << __FILE__ << "(" << __LINE__ << ") " << #var1     \
            << " = [" << (var1) << "]" << #var2 << " = [" << (var2) << "]" \
            << std::endl
#define DBGVAR3(var1, var2, var3)                                          \
  std::cout << "DBG: " << __FILE__ << "(" << __LINE__ << ") " << #var1     \
            << " = [" << (var1) << "]" << #var2 << " = [" << (var2) << "]" \
            << #var3 << " = [" << (var3) << "]" << std::endl

#define DBGMSG(msg)                                                  \
  std::cout << "DBG: " << __FILE__ << "(" << __LINE__ << ") " << msg \
            << std::endl
#else
#define DBGVAR(var)
#define DBGVAR2(var1, var2)
#define DBGVAR3(var1, var2, var3)
#define DBGMSG(msg)
#endif
#endif  // INCLUDE_HCL_COMMON_DEBUG_H_

#ifndef INCLUDE_HCL_COMMON_DEBUG_TIMER_H_
#define INCLUDE_HCL_COMMON_DEBUG_TIMER_H_
/**
 * Time all functions and instrument it
 */

#include <mpi.h>
#include <stdarg.h>

#include <chrono>
#include <sstream>
#include <stack>
#include <string>

class Timer {
 public:
  Timer() : elapsed_time(0) {}
  void resumeTime() { t1 = std::chrono::high_resolution_clock::now(); }
  double pauseTime() {
    auto t2 = std::chrono::high_resolution_clock::now();
    elapsed_time +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count() /
        1000000.0;
    return elapsed_time;
  }
  double getElapsedTime() { return elapsed_time; }

 private:
  std::chrono::high_resolution_clock::time_point t1;
  double elapsed_time;
};

#endif  // INCLUDE_HCL_COMMON_DEBUG_TIMER_H_
#ifndef INCLUDE_HCL_COMMON_DEBUG_AUTOTRACE_H_
#define INCLUDE_HCL_COMMON_DEBUG_AUTOTRACE_H_
/**
 * Implement Auto tracing Mechanism.
 */
using std::cout;
using std::endl;
using std::string;

using namespace std;

class AutoTrace {
#if defined(HCL_TIMER)
  Timer timer;
#endif
#if defined(HCL_TRACE) || defined(HCL_TIMER)
  static int rank, item;
  string m_line;
#endif
 public:
  template <typename... Args>
  AutoTrace(
#if defined(HCL_TRACE) || defined(HCL_TIMER)
      std::string string,
#endif
      Args... args)
#if defined(HCL_TRACE) || defined(HCL_TIMER)
      : m_line(string)
#endif
  {
#if defined(HCL_TRACE) || defined(HCL_TIMER)
    char thread_name[256];
    pthread_getname_np(pthread_self(), thread_name, 256);
    std::stringstream stream;

    if (rank == -1) MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    stream << "\033[31m";
    stream << ++item << ";" << thread_name << ";" << rank << ";" << m_line
           << ";";
#endif
#if defined(HCL_TIMER)
    stream << ";;";
#endif
#ifdef HCL_TRACE
    auto args_obj = std::make_tuple(args...);
    const ulong args_size = std::tuple_size<decltype(args_obj)>::value;
    stream << "args(";

    if (args_size == 0)
      stream << "Void";
    else {
      std::apply([&stream](auto &&...args) { ((stream << args << ", "), ...); },
                 args_obj);
    }
    stream << ");";
#endif
#if defined(HCL_TRACE) || defined(HCL_TIMER)
    stream << "start" << endl;
    stream << "\033[00m";
    cout << stream.str();
#endif
#ifdef HCL_TIMER
    timer.resumeTime();
#endif
  }

  ~AutoTrace() {
#if defined(HCL_TRACE) || defined(HCL_TIMER)
    std::stringstream stream;
    char thread_name[256];
    pthread_getname_np(pthread_self(), thread_name, 256);
    stream << "\033[31m";
    stream << item-- << ";" << std::string(thread_name) << ";" << rank << ";"
           << m_line << ";";
#endif
#if defined(HCL_TRACE)
    stream << ";";
#endif
#ifdef HCL_TIMER
    double end_time = timer.pauseTime();
    stream << end_time << ";msecs;";
#endif
#if defined(HCL_TRACE) || defined(HCL_TIMER)
    stream << "finish" << endl;
    stream << "\033[00m";
    cout << stream.str();
#endif
  }
};

#if defined(HCL_TRACE) || defined(HCL_TIMER)
int AutoTrace::rank = -1;
int AutoTrace::item = 0;
#endif
#endif  // INCLUDE_HCL_COMMON_DEBUG_AUTOTRACE_H_
