#ifndef CHRONOLOG_PROFILE_H
#define CHRONOLOG_PROFILE_H

#define CL_PROFILE_CONCAT_INNER(lhs, rhs) lhs##rhs
#define CL_PROFILE_CONCAT(lhs, rhs) CL_PROFILE_CONCAT_INNER(lhs, rhs)

#if defined(CHRONOLOG_PROFILE_TAU)

#include <TAU.h>

namespace chronolog
{
namespace profiling
{

class TauRegion
{
  public:
    explicit TauRegion(const char* name)
    {
        Tau_profile_c_timer(&timer_, name, "", TAU_USER, "TAU_USER");
        Tau_lite_start_timer(timer_, 0);
    }

    TauRegion(const TauRegion&) = delete;
    TauRegion& operator=(const TauRegion&) = delete;

    ~TauRegion()
    {
        Tau_lite_stop_timer(timer_);
    }

  private:
    void* timer_ = nullptr;
};

inline void tauCounter(const char* name, double value)
{
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
