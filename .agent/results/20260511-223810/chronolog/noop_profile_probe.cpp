#include <chronolog_profile.h>

int side_effects = 0;

int side_effect()
{
    ++side_effects;
    return side_effects;
}

int main()
{
    CL_PROFILE_REGION(side_effect() ? "bad" : "bad");
    CL_PROFILE_COUNTER("noop_counter", side_effect());
    return side_effects == 0 ? 0 : 1;
}
