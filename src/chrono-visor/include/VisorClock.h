#ifndef CHRONOLOG_VISOR_CLOCK_H
#define CHRONOLOG_VISOR_CLOCK_H

#include <chrono>
#include <cstdint>

namespace chronolog
{

// VisorClock is the cluster time authority. It is simply the Visor process's
// monotonic steady_clock: both Visor engines (client portal + keeper registry)
// run in the same process and read the same physical clock, so a static reading
// is globally consistent for the cluster. Clients and daemons map their local
// clocks onto this timeline via a round-trip clock exchange (see ChronoClock).
class VisorClock
{
public:
    // Authoritative Visor tick in ns (monotonic non-decreasing).
    static uint64_t now() { return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()); }
};

} // namespace chronolog

#endif
