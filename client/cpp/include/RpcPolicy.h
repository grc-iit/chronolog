#ifndef CHRONOLOG_RPC_POLICY_H
#define CHRONOLOG_RPC_POLICY_H

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include "client_errcode.h"

namespace chronolog
{
// Total operation budgets and the cap on a constituent RPC attempt, in milliseconds.
// The runtime caps each attempt by the operation's remaining time.
struct RpcTimeoutPolicy
{
    uint64_t append_ms = 5000;
    uint64_t control_ms = 5000;
    uint64_t tail_ms = 5000;
    uint64_t replay_ms = 180000;
    uint64_t attempt_ms = 5000;
    uint64_t startup_ms = 30000;
    uint64_t shutdown_ms = 30000;
    uint64_t transfer_ms = 30000;
};

inline int validateRpcTimeoutPolicy(RpcTimeoutPolicy const& policy) noexcept
{
    constexpr auto max_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()).count();
    for(uint64_t value: {policy.append_ms,
                         policy.control_ms,
                         policy.tail_ms,
                         policy.replay_ms,
                         policy.attempt_ms,
                         policy.startup_ms,
                         policy.shutdown_ms,
                         policy.transfer_ms})
    {
        if(value == 0 || value > static_cast<uint64_t>(max_ms))
            return CL_ERR_INVALID_ARG;
    }
    return CL_SUCCESS;
}
} // namespace chronolog

#endif
