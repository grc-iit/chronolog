#ifndef CHRONOLOG_CLOCK_STATE_H
#define CHRONOLOG_CLOCK_STATE_H

#include <cstdint>

namespace chronolog
{

// ClockState is the result of a clock exchange with the ChronoVisor time
// authority. It is carried on the wire (Connect response, SyncClock response,
// heartbeat response) and cached per node.
//
//   visor_time  : the Visor authority tick (ns) read when it served the sync
//   offset      : ns to add to a node's local steady clock to reach the Visor
//                 timeline ( visor_tick ~= local_steady_now + offset )
//   drift_rate  : first-order frequency correction (ns per ns); 0 => zero-order
//                 only (shipped first; carried for observability / future use)
//   uncertainty : half-round-trip bound on the offset error (± RTT/2 at sync)
struct ClockState
{
    uint64_t visor_time = 0;
    int64_t offset = 0;
    double drift_rate = 0.0;
    uint64_t uncertainty = 0;

    // serialization function used by thallium RPC providers
    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & visor_time;
        serT & offset;
        serT & drift_rate;
        serT & uncertainty;
    }
};

} // namespace chronolog

#endif
