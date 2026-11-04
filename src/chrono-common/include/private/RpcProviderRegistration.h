#ifndef CHRONOLOG_RPC_PROVIDER_REGISTRATION_H
#define CHRONOLOG_RPC_PROVIDER_REGISTRATION_H
#include <RpcDeadline.h>
#include <client_errcode.h>
#include <thallium.hpp>
#include <mutex>
namespace chronolog::rpc
{
// Non-owning adapter state embedded once in the actual engine/procedure owner.
// Margo 0.13.1 lazily clones base RPC registrations for each provider ID without
// synchronizing concurrent first forwards. Provision those IDs before forwarding.
class RpcProviderRegistration
{
public:
    explicit RpcProviderRegistration(thallium::engine& engine)
        : mid_(engine.get_margo_instance())
    {}
    int provision(char const* name, uint16_t provider, RpcDeadline const& deadline)
    {
        std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
        if(!lock.try_lock_until(deadline.expiresAt()) || deadline.expired())
            return CL_ERR_DEADLINE_EXCEEDED;
        hg_id_t id = 0;
        hg_bool_t registered = HG_FALSE;
        if(margo_provider_registered_name(mid_, name, provider, &id, &registered) != HG_SUCCESS)
            return CL_ERR_UNKNOWN;
        if(registered)
            return CL_SUCCESS;
        // These are the same public callbacks as Thallium's engine.define.
        // Mercury's registered callbacks are Margo wrappers, not these inputs.
        id = margo_provider_register_name(mid_,
                                          name,
                                          thallium::hg_proc_meta_serialization,
                                          thallium::hg_proc_meta_serialization,
                                          nullptr,
                                          provider,
                                          ABT_POOL_NULL);
        return id ? CL_SUCCESS : CL_ERR_UNKNOWN;
    }
    // No lookup, serialization, forward or per-call deregistration under this
    // mutex. Registrations remain native-owned through actual engine cleanup.
private:
    margo_instance_id const mid_;
    std::timed_mutex mutex_;
};
} // namespace chronolog::rpc
#endif
