#include <chronolog_profile.h>

int side_effect()
{
    return 7;
}

int main()
{
    CL_PROFILE_REGION("probe_region");
    CL_PROFILE_COUNTER("probe_counter", side_effect());
    return 0;
}
