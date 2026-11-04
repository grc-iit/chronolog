#ifndef CHRONOLOG_RPC_CALL_STATE_H
#define CHRONOLOG_RPC_CALL_STATE_H
#include <thallium.hpp>
#include <optional>
#include <stdexcept>
namespace chronolog::rpc
{
// Per charged work, never a shared route descriptor or an engine owner. The
// containing work retains its actual engine owner and arguments until cleanup.
class RpcCallState
{
public:
    using Call = decltype(std::declval<thallium::remote_procedure>().on(std::declval<thallium::provider_handle>()));
    RpcCallState() = default;
    RpcCallState(RpcCallState const&) = delete;
    RpcCallState& operator=(RpcCallState const&) = delete;
    void bind(thallium::engine& engine,
              thallium::remote_procedure const& remote,
              std::string const& address,
              uint16_t provider)
    {
        checkEmpty();
        endpoint_.emplace(engine.lookup(address));
        bindPeer(remote, provider);
    }
    void bind(thallium::remote_procedure const& remote, thallium::endpoint const& endpoint, uint16_t provider)
    {
        checkEmpty();
        endpoint_.emplace(endpoint);
        bindPeer(remote, provider);
    }
    Call& callable() { return call_.value(); }
    // Only after a fully decoded known reply, on the charged worker. Unknown
    // work retains every native field; its destructor is not a settlement fence.
    void clearKnown()
    {
        call_.reset();
        peer_.reset();
        endpoint_.reset();
    }

private:
    void checkEmpty() const
    {
        if(call_)
            throw std::logic_error("Cannot replace an unsettled RPC callable");
    }
    void bindPeer(thallium::remote_procedure const& remote, uint16_t provider)
    {
        peer_.emplace(*endpoint_, provider);
        call_.emplace(remote.on(*peer_));
    }
    std::optional<thallium::endpoint> endpoint_;
    std::optional<thallium::provider_handle> peer_;
    std::optional<Call> call_;
};
} // namespace chronolog::rpc
#endif
