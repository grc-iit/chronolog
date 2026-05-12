#include <chronolog_profile.h>

int side_effect()
{
    return 11;
}

int main()
{
    CL_PROFILE_REGION("tau_probe_region");
    CL_PROFILE_COUNTER("tau_probe_counter", side_effect());
    return 0;
}
