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
        hg_id_t base = 0;
        hg_bool_t baseRegistered = HG_FALSE;
        int disabled = 0;
        if(margo_provider_registered_name(mid_, name, MARGO_DEFAULT_PROVIDER_ID, &base, &baseRegistered) !=
                   HG_SUCCESS ||
           !baseRegistered || margo_registered_disabled_response(mid_, base, &disabled) != HG_SUCCESS)
            return CL_ERR_UNKNOWN;
        if(registered)
        {
            int existing = 0;
            if(margo_registered_disabled_response(mid_, id, &existing) != HG_SUCCESS)
                return CL_ERR_UNKNOWN;
            // Never toggle an exposed ID while an earlier call can use it.
            return existing == disabled ? CL_SUCCESS : CL_ERR_INVALID_ARG;
        }
        // These are the same public callbacks as Thallium's engine.define.
        // Mercury's registered callbacks are Margo wrappers, not these inputs.
        id = margo_provider_register_name(mid_,
                                          name,
                                          thallium::hg_proc_meta_serialization,
                                          thallium::hg_proc_meta_serialization,
                                          nullptr,
                                          provider,
                                          ABT_POOL_NULL);
        // Pre-registration skips Margo's lazy copy of the base response mode.
        // Set it once before publishing this binding; reuse only validates it.
        return id && margo_registered_disable_response(mid_, id, disabled) == HG_SUCCESS ? CL_SUCCESS : CL_ERR_UNKNOWN;
    }
    // No lookup, serialization, forward or per-call deregistration under this
    // mutex. Registrations remain native-owned through actual engine cleanup.
private:
    margo_instance_id const mid_;
    std::timed_mutex mutex_;
};
} // namespace chronolog::rpc
#endif
