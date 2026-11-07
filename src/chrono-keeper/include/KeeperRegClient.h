#ifndef CHRONOLOG_KEEPER_REG_CLIENT_H
#define CHRONOLOG_KEEPER_REG_CLIENT_H
#include <chrono_monitor.h>
#include <KeeperRegistrationMsg.h>
#include <KeeperStatsMsg.h>
#include <private/RegistrySession.h>
namespace chronolog
{
class KeeperRegistryClient
{
public:
    static KeeperRegistryClient*
    CreateKeeperRegistryClient(thallium::engine& engine, std::string const& address, uint16_t provider)
    {
        auto owner = rpc::RpcBulkOwner::Find(engine.get_margo_instance());
        if(!owner)
            return nullptr;
        return CreateKeeperRegistryClient(owner,
                                          address,
                                          provider,
                                          rpc::RpcDeadline::after(std::chrono::milliseconds(owner->policy.startup_ms)));
    }
    static KeeperRegistryClient* CreateKeeperRegistryClient(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                                                            std::string const& address,
                                                            uint16_t provider,
                                                            rpc::RpcDeadline const& deadline)
    {
        auto session = rpc::RegistrySession::Create(owner, "keeper", address, provider, deadline);
        return session ? new KeeperRegistryClient(std::move(session), owner->policy) : nullptr;
    }
    int send_register_msg(KeeperRegistrationMsg const& message)
    {
        return send_register_msg(message, rpc::RpcDeadline::after(std::chrono::milliseconds(policy_.attempt_ms)));
    }
    int send_register_msg(KeeperRegistrationMsg const& message,
                          rpc::RpcDeadline const& deadline,
                          volatile sig_atomic_t const* interrupt = nullptr)
    {
        return session_->send(rpc::RegistrySession::Register, message, deadline, true, interrupt);
    }
    int send_unregister_msg(KeeperIdCard const& id)
    {
        return send_unregister_msg(id, rpc::RpcDeadline::after(std::chrono::milliseconds(policy_.attempt_ms)));
    }
    int send_unregister_msg(KeeperIdCard const& id, rpc::RpcDeadline const& deadline)
    {
        return session_->send(rpc::RegistrySession::Unregister, id, deadline);
    }
    void send_stats_msg(KeeperStatsMsg const& message)
    {
        // Coalesce one active send; main never waits for a stats RPC before TERM.
        session_->send(rpc::RegistrySession::Stats,
                       message,
                       rpc::RpcDeadline::after(std::chrono::milliseconds(policy_.attempt_ms)),
                       false);
    }
    ~KeeperRegistryClient() = default;

private:
    KeeperRegistryClient(std::shared_ptr<rpc::RegistrySession> session, RpcTimeoutPolicy policy)
        : session_(std::move(session))
        , policy_(policy)
    {}
    std::shared_ptr<rpc::RegistrySession> session_;
    RpcTimeoutPolicy const policy_;
};
} // namespace chronolog
#endif
