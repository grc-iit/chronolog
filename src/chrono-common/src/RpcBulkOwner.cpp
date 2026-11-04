#include <private/RpcBulkOwner.h>
#include <map>
#include <mutex>
namespace chronolog::rpc
{
namespace
{
std::mutex ownersMutex;
std::map<margo_instance_id, std::weak_ptr<RpcBulkOwner>> owners;
} // namespace
std::shared_ptr<RpcBulkOwner> RpcBulkOwner::Find(margo_instance_id mid)
{
    std::lock_guard<std::mutex> lock(ownersMutex);
    auto found = owners.find(mid);
    return found == owners.end() ? nullptr : found->second.lock();
}
std::shared_ptr<RpcBulkOwner> RpcBulkOwner::Create(std::shared_ptr<thallium::engine> native,
                                                   std::shared_ptr<RpcRuntime> const& ownerRuntime,
                                                   RpcTimeoutPolicy const& policy,
                                                   bool outbound)
{
    auto owner = std::shared_ptr<RpcBulkOwner>(new RpcBulkOwner(std::move(native), policy));
    {
        std::lock_guard<std::mutex> lock(ownersMutex);
        auto& previous = owners[owner->engine->get_margo_instance()];
        if(!previous.expired())
            throw std::logic_error("Bulk owner already exists for this MID");
        previous = owner;
    }
    owner->runtime = ownerRuntime;
    ownerRuntime->retainOwner(owner,
                              [owner]
                              {
                                  // B12 must prove incoming/native settlement before explicit native
                                  // destruction. The main error/stop gate keeps referenced stores alive.
                                  throw std::runtime_error("Bulk native retirement is not proven");
                              });
    if(outbound)
    {
        owner->receiveChunk = owner->engine->define("receive_story_chunk");
        owner->available = owner->engine->define("receiver_available");
    }
    return owner;
}
} // namespace chronolog::rpc
