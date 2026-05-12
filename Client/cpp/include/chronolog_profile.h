#ifndef CHRONOLOG_PROFILE_H
#define CHRONOLOG_PROFILE_H

#define CL_PROFILE_CONCAT_INNER(lhs, rhs) lhs##rhs
#define CL_PROFILE_CONCAT(lhs, rhs) CL_PROFILE_CONCAT_INNER(lhs, rhs)

#if defined(CHRONOLOG_PROFILE_TAU)

#include <TAU.h>
#include <chrono>
#include <mutex>
#include <string>

namespace chronolog
{
namespace profiling
{

inline void initializeTau()
{
    static std::once_flag init_once;
    std::call_once(init_once, []() { TAU_PROFILE_SET_NODE(0); });
}

class TauRegion
{
  public:
    explicit TauRegion(const char* name) : name_(name), start_(std::chrono::steady_clock::now())
    {
        initializeTau();
        void* event = Tau_get_userevent(name_.c_str());
        Tau_userevent(event, 1.0);
    }

    TauRegion(const TauRegion&) = delete;
    TauRegion& operator=(const TauRegion&) = delete;

    ~TauRegion()
    {
        auto end = std::chrono::steady_clock::now();
        double duration_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count());
        std::string duration_name = name_ + "_duration_us";
        void* event = Tau_get_userevent(duration_name.c_str());
        Tau_userevent(event, duration_us);
    }

  private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};

inline void tauCounter(const char* name, double value)
{
    initializeTau();
    void* event = Tau_get_userevent(name);
    Tau_userevent(event, value);
}

} // namespace profiling
} // namespace chronolog

#define CL_PROFILE_REGION(name) ::chronolog::profiling::TauRegion CL_PROFILE_CONCAT(cl_profile_region_, __LINE__)(name)
#define CL_PROFILE_COUNTER(name, value) ::chronolog::profiling::tauCounter((name), static_cast<double>(value))

#else

#define CL_PROFILE_REGION(name) ((void)0)
#define CL_PROFILE_COUNTER(name, value) ((void)0)

#endif

#endif // CHRONOLOG_PROFILE_H
