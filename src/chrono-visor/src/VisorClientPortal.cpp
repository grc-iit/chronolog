#include <VisorClientPortal.h>
#include <chrono_monitor.h>
#include <KeeperRegistry.h>
#include <ClientPortalService.h>
#include <private/RpcReply.h>
#include <ConnectResponseMsg.h>
#include <chronolog_client.h>
#include <thallium/serialization/stl/pair.hpp>
#include <thread>
#include <optional>
namespace chronolog
{
namespace
{
using namespace std::chrono_literals;
struct PortalWork final: rpc::OwnedRpcWork<std::shared_ptr<VisorControlReply>>
{
    std::shared_ptr<VisorClientPortal::Core> core;
    VisorControlAction action;
    ClientId client;
    std::string chronicle, story;
    uint32_t account, protocol;
    std::optional<thallium::request> request;
    PortalWork(std::shared_ptr<VisorClientPortal::Core> c,
               VisorControlAction a,
               ClientId id,
               std::string const& cn,
               std::string const& sn,
               uint32_t account,
               uint32_t protocol,
               thallium::request const* req)
        : core(std::move(c))
        , action(a)
        , client(id)
        , chronicle(cn)
        , story(sn)
        , account(account)
        , protocol(protocol)
    {
        if(req)
        {
            request.emplace(*req);
            ++core->incomingRequests;
        } // margo_ref_incr; only after admission.
    }
    ~PortalWork() override
    {
        if(request)
        {
            request.reset();
            --core->incomingRequests;
        }
    }
    rpc::RpcResult<std::shared_ptr<VisorControlReply>> run(rpc::OperationContext& context) override
    {
        if(!context.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
        auto reply = core->Execute(action, client, chronicle, story, account, protocol, context.deadline());
        if(request)
        {
            // request carries a native handle ref; Core retains engine/provider.
            // Native response, serialization and handle destruction are charged
            // to this same worker slot, including a response that settles late.
            switch(action)
            {
                case VisorControlAction::Connect:
                    rpc::respond(*request,
                                 ConnectResponseMsg(reply->status, reply->status == CL_SUCCESS ? client : ClientId{0}));
                    break;
                case VisorControlAction::Acquire:
                    rpc::respond(*request, reply->acquisition);
                    break;
                case VisorControlAction::ShowChronicles:
                case VisorControlAction::ShowStories:
                    rpc::respond(*request, std::make_pair(reply->status, reply->names));
                    break;
                default:
                    rpc::respond(*request, reply->status);
                    break;
            }
        }
        return {reply->status, true, std::move(reply)};
    }
};
std::shared_ptr<VisorControlReply> failure(int status)
{
    auto r = std::make_shared<VisorControlReply>();
    r->status = status;
    r->acquisition = AcquireStoryResponseMsg(status, 0, {});
    return r;
}
} // namespace
VisorClientPortal::Core::Core()
{
    clients.setChronicleMetaDirectory(&directory);
    directory.set_client_registry_manager(&clients);
}
VisorClientPortal::VisorClientPortal()
    : core_(std::make_shared<Core>())
{}
VisorClientPortal::~VisorClientPortal() { ShutdownServices(); }
void VisorClientPortal::ShutdownServices() { core_->shuttingDown.store(true, std::memory_order_release); }
int VisorClientPortal::StartServices(RPCProviderConf const& conf, std::shared_ptr<KeeperRegistry> registry)
{
    return core_->StartServices(conf, std::move(registry));
}
int VisorClientPortal::Core::StartServices(RPCProviderConf const& conf, std::shared_ptr<KeeperRegistry> r)
{
    if(engine)
        return CL_SUCCESS;
    registry = std::move(r);
    auto address = conf.PROTO_CONF + "://" + conf.IP + ":" + std::to_string(conf.BASE_PORT);
    auto mid = margo_init(address.c_str(), MARGO_SERVER_MODE, 1, 2);
    if(mid == MARGO_INSTANCE_NULL)
        return CL_ERR_UNKNOWN;
    engine = std::make_shared<thallium::engine>(mid);
    provider = ClientPortalService::CreateClientPortalService(*engine, conf.SERVICE_PROVIDER_ID, shared_from_this());
    // Provider roots Core through daemon lifetime. B12 must prove incoming and
    // outgoing settlement before breaking this lease and finalizing engines.
    return CL_SUCCESS;
}
int VisorClientPortal::Core::StartOwnedServices(RPCProviderConf const& conf,
                                                std::shared_ptr<KeeperRegistry> r,
                                                std::shared_ptr<thallium::engine> const& native)
{
    if(engine || !native)
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    registry = std::move(r);
    engine = native;
    provider = ClientPortalService::CreateOwned(*native, conf.SERVICE_PROVIDER_ID, shared_from_this(), ownedProvider);
    return CL_SUCCESS;
}
void VisorClientPortal::Core::releaseOwnedServices()
{
    ownedProvider.reset();
    provider = nullptr;
    engine.reset();
    registry.reset();
}
bool VisorClientPortal::Core::quiesce(rpc::RpcDeadline const& deadline)
{
    shuttingDown.store(true, std::memory_order_release);
    std::unique_lock<std::timed_mutex> admission(admissionMutex, std::defer_lock);
    // Joining this short admission gate observes every older reserve/copy/submit.
    // Original admitted work still settles on the same C32 before proof.
    return admission.try_lock_until(deadline.expiresAt()) && !deadline.expired();
}
std::shared_ptr<VisorControlReply> VisorClientPortal::Core::Invoke(VisorControlAction action,
                                                                   ClientId client,
                                                                   std::string const& chronicle,
                                                                   std::string const& story,
                                                                   uint32_t account)
{
    auto started = rpc::RpcDeadline::Clock::now();
    if(!registry || shuttingDown.load(std::memory_order_acquire))
        return failure(CL_ERR_SHUTDOWN_INCOMPLETE);
    auto owner = registry->adminOwner();
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(owner->policy.control_ms));
    std::unique_lock<std::timed_mutex> admission(admissionMutex, std::defer_lock);
    if(!admission.try_lock_until(deadline.expiresAt()) || deadline.expired())
        return failure(CL_ERR_DEADLINE_EXCEEDED);
    if(shuttingDown.load(std::memory_order_acquire))
        return failure(CL_ERR_SHUTDOWN_INCOMPLETE);
    auto token = owner->runtime.reserve(deadline);
    if(!token)
        return failure(token.status());
    auto work = std::make_shared<PortalWork>(shared_from_this(),
                                             action,
                                             client,
                                             chronicle,
                                             story,
                                             account,
                                             CLIENT_PROTOCOL_VERSION,
                                             nullptr);
    auto pending = owner->runtime.submit<std::shared_ptr<VisorControlReply>>(std::move(token), std::move(work));
    admission.unlock();
    auto result = pending.wait();
    return result.reply ? std::move(*result.reply) : failure(result.status);
}
void VisorClientPortal::Core::Dispatch(thallium::request const& request,
                                       VisorControlAction action,
                                       ClientId client,
                                       std::string const& chronicle,
                                       std::string const& story,
                                       uint32_t account,
                                       uint32_t protocol)
{
    auto started = rpc::RpcDeadline::Clock::now();
    if(!registry || shuttingDown.load(std::memory_order_acquire))
        return;
    auto owner = registry->adminOwner();
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(owner->policy.control_ms));
    std::unique_lock<std::timed_mutex> admission(admissionMutex, std::defer_lock);
    if(!admission.try_lock_until(deadline.expiresAt()) || deadline.expired() ||
       shuttingDown.load(std::memory_order_acquire))
        return;
    auto token = owner->runtime.reserve(deadline);
    // At saturation no request copy, argument ownership, response context or
    // mutation is created. The unchanged-wire client must bound its own wait.
    if(!token)
    {
        static RefusalLog refusals;
        if(auto refused = refusals.note())
            LOG_WARNING("[VisorClientPortal] Dropped {} client request(s) without a reply since the last such "
                        "warning, the latest with status {}: no free slot among the visor's {} (rpc_slots). Those "
                        "clients get CL_ERR_OUTCOME_UNCONFIRMED when their control_ms runs out",
                        refused,
                        token.status(),
                        owner->runtime.capacity());
        return;
    }
    auto work = std::make_shared<PortalWork>(shared_from_this(),
                                             action,
                                             client,
                                             chronicle,
                                             story,
                                             account,
                                             protocol,
                                             &request);
    auto pending = owner->runtime.submit<std::shared_ptr<VisorControlReply>>(std::move(token), std::move(work));
    admission.unlock();
    pending.wait();
}
std::shared_ptr<VisorControlReply> VisorClientPortal::Core::Execute(VisorControlAction action,
                                                                    ClientId client,
                                                                    std::string const& chronicle,
                                                                    std::string const& story,
                                                                    uint32_t account,
                                                                    uint32_t protocol,
                                                                    rpc::RpcDeadline const& deadline)
{
    std::unique_lock<std::mutex> lock(operationMutex, std::defer_lock);
    while(!deadline.expired() && !lock.try_lock())
        std::this_thread::sleep_for(std::min(deadline.remaining(), rpc::RpcDeadline::Clock::duration(1ms)));
    if(!lock.owns_lock() || deadline.expired())
        return failure(CL_ERR_DEADLINE_EXCEEDED);
    if(shuttingDown.load(std::memory_order_acquire))
        return failure(CL_ERR_SHUTDOWN_INCOMPLETE);
    auto r = failure(CL_ERR_UNKNOWN);
    StoryId id = 0;
    bool last = false;
    // A destroy of the chronicle, or of the story, that a grapher did not
    // confirm is sent again before a create or acquire of the name, as an
    // unconfirmed stop is; once every grapher confirms it, the name is removed
    // here too, and the create or acquire goes on as for a new name.
    auto finishDestruction = [&](std::string const& c, std::string const& s) -> int
    {
        bool chronicleDestroyed = false, storyDestroyed = false;
        int status = registry->finishUnconfirmedDestruction(c, s, deadline, chronicleDestroyed, storyDestroyed);
        if(status != CL_SUCCESS)
            return status;
        int removed = CL_SUCCESS;
        if(chronicleDestroyed)
            removed = directory.destroy_chronicle(c);
        else if(storyDestroyed)
            removed = directory.destroy_story(c, s);
        return removed == CL_SUCCESS || removed == CL_ERR_NOT_EXIST ? CL_SUCCESS : CL_ERR_OUTCOME_UNCONFIRMED;
    };
    switch(action)
    {
        case VisorControlAction::Connect:
        {
            if(protocol != CLIENT_PROTOCOL_VERSION)
            {
                r->status = CL_ERR_PROTOCOL_VERSION_MISMATCH;
                break;
            }
            ClientInfo info;
            r->status = clients.add_client_record(client, info);
            break;
        }
        case VisorControlAction::CreateChronicle:
            if(chronicle.empty())
            {
                r->status = CL_ERR_INVALID_ARG;
                break;
            }
            r->status = finishDestruction(chronicle, "");
            if(r->status == CL_SUCCESS)
                r->status = directory.create_chronicle(chronicle);
            break;
        case VisorControlAction::Acquire:
        {
            if(chronicle.empty() || story.empty())
            {
                r->status = CL_ERR_INVALID_ARG;
                break;
            }
            if(!registry->is_running())
            {
                r->status = CL_ERR_NO_KEEPERS;
                break;
            }
            // Before the acquisition is added, so a destroy finished here never
            // deletes it. A chronicle destroyed here leaves CL_ERR_NOT_EXIST.
            r->status = finishDestruction(chronicle, story);
            if(r->status != CL_SUCCESS)
                break;
            r->status = directory.acquire_story(client, chronicle, story, id);
            // The client already holds the story: an earlier acquire whose reply
            // it did not get. Answer with the story's current recording, as a
            // second client would get, so the client can settle that outcome.
            // The acquisition is the earlier call's, so this call never rolls it
            // back.
            bool repeated = r->status == CL_ERR_ACQUIRED;
            if(r->status != CL_SUCCESS && !repeated)
                break;
            std::vector<KeeperIdCard> keepers;
            ServiceId player;
            bool locallyRejectedBeforeSetup = false;
            r->status = registry->notifyRecordingGroupOfStoryRecordingStart(chronicle,
                                                                            story,
                                                                            id,
                                                                            keepers,
                                                                            player,
                                                                            &deadline,
                                                                            &locallyRejectedBeforeSetup);
            // This local proof is set only before roster admission/insertion and
            // child submission, never from a remote error code. Core exclusion
            // preserves this operation's acquisition until the checked rollback.
            if(r->status != CL_SUCCESS && locallyRejectedBeforeSetup && !repeated)
            {
                StoryId released = 0;
                bool wasLast = false;
                if(directory.release_story(client, chronicle, story, released, wasLast, id) != CL_SUCCESS)
                    r->status = CL_ERR_OUTCOME_UNCONFIRMED;
            }
            else if(r->status != CL_SUCCESS && r->status != CL_ERR_DEADLINE_EXCEEDED &&
                    r->status != CL_ERR_RPC_CAPACITY)
                // The client holds the story and some start may have run, so no
                // answer may read as a definite failure: the client would forget
                // the acquisition this registry keeps. Its next acquire or release
                // of the story settles it. The client reads -22 and -24 as
                // unknown already.
                r->status = CL_ERR_OUTCOME_UNCONFIRMED;
            // Dispatched/possibly dispatched setup retains acquisition. After a
            // lost reply or a remote -22/-24, a repeated acquire by the same
            // client settles the recording and answers with it.
            if(r->status == CL_SUCCESS)
            {
                std::vector<ServiceId> services;
                for(auto const& k: keepers) services.push_back(k.getRecordingServiceId());
                r->acquisition = AcquireStoryResponseMsg(CL_SUCCESS, id, services, player);
            }
            break;
        }
        case VisorControlAction::Release:
            r->status = directory.release_story(client, chronicle, story, id, last);
            if(r->status == CL_SUCCESS && last)
                r->status = registry->notifyRecordingGroupOfStoryRecordingStop(id, &deadline);
            break;
        case VisorControlAction::DestroyStory:
        {
            bool held = false;
            r->status = directory.evaluate_story_destroy(chronicle, story, client, id, held);
            if(r->status != CL_SUCCESS)
                break;
            if(held)
            {
                r->status = directory.release_story(client, chronicle, story, id, last);
                if(r->status != CL_SUCCESS)
                    break;
                if(last)
                {
                    r->status = registry->notifyRecordingGroupOfStoryRecordingStop(id, &deadline);
                    if(r->status != CL_SUCCESS)
                        break;
                }
            }
            r->status = registry->notifyAllGraphersOfStoryDestruction(chronicle, story, id, &deadline);
            if(r->status == CL_SUCCESS)
                r->status = directory.destroy_story(chronicle, story);
            break;
        }
        case VisorControlAction::DestroyChronicle:
        {
            std::vector<std::pair<StoryId, std::string>> held;
            r->status = directory.evaluate_chronicle_destroy(chronicle, client, held);
            if(r->status != CL_SUCCESS)
                break;
            for(auto const& item: held)
            {
                r->status = directory.release_story(client, chronicle, item.second, id, last);
                if(r->status != CL_SUCCESS)
                    break;
                if(last)
                {
                    r->status = registry->notifyRecordingGroupOfStoryRecordingStop(id, &deadline);
                    if(r->status != CL_SUCCESS)
                        break;
                }
            }
            if(r->status != CL_SUCCESS)
                break;
            r->status = registry->notifyAllGraphersOfChronicleDestruction(chronicle, &deadline);
            if(r->status == CL_SUCCESS)
                r->status = directory.destroy_chronicle(chronicle);
            break;
        }
        case VisorControlAction::Disconnect:
        {
            std::vector<StoryId> released;
            r->status = directory.release_all_acquired_stories(client, released);
            if(r->status != CL_SUCCESS)
                break;
            for(auto sid: released)
            {
                auto status = registry->notifyRecordingGroupOfStoryRecordingStop(sid, &deadline);
                if(status != CL_SUCCESS)
                    r->status = status;
            }
            auto status = clients.remove_client_record(client);
            if(r->status == CL_SUCCESS)
                r->status = status;
            break;
        }
        case VisorControlAction::ShowChronicles:
            r->status = directory.show_chronicles(r->names);
            break;
        case VisorControlAction::ShowStories:
            r->status = directory.show_stories(chronicle, r->names);
            break;
    }
    if(action == VisorControlAction::Acquire && r->status != CL_SUCCESS)
        r->acquisition = AcquireStoryResponseMsg(r->status, id, {});
    return r;
}
int VisorClientPortal::ClientConnect(uint32_t account, ClientId& client, uint64_t& offset)
{
    offset = 0;
    return core_->Invoke(VisorControlAction::Connect, client, "", "", account)->status;
}
int VisorClientPortal::ClientDisconnect(ClientId const& c)
{
    return core_->Invoke(VisorControlAction::Disconnect, c)->status;
}
int VisorClientPortal::CreateChronicle(ClientId const& c, ChronicleName const& n)
{
    return core_->Invoke(VisorControlAction::CreateChronicle, c, n)->status;
}
int VisorClientPortal::DestroyChronicle(ClientId const& c, ChronicleName const& n)
{
    return core_->Invoke(VisorControlAction::DestroyChronicle, c, n)->status;
}
int VisorClientPortal::DestroyStory(ClientId const& c, std::string const& n, std::string const& s)
{
    return core_->Invoke(VisorControlAction::DestroyStory, c, n, s)->status;
}
AcquireStoryResponseMsg VisorClientPortal::AcquireStory(ClientId const& c, std::string const& n, std::string const& s)
{
    return core_->Invoke(VisorControlAction::Acquire, c, n, s)->acquisition;
}
int VisorClientPortal::ReleaseStory(ClientId const& c, std::string const& n, std::string const& s)
{
    return core_->Invoke(VisorControlAction::Release, c, n, s)->status;
}
int VisorClientPortal::ShowChronicles(ClientId const& c, std::vector<std::string>& n)
{
    auto r = core_->Invoke(VisorControlAction::ShowChronicles, c);
    if(r->status == CL_SUCCESS)
        n = std::move(r->names);
    return r->status;
}
int VisorClientPortal::ShowStories(ClientId const& c, std::string const& n, std::vector<std::string>& s)
{
    auto r = core_->Invoke(VisorControlAction::ShowStories, c, n);
    if(r->status == CL_SUCCESS)
        s = std::move(r->names);
    return r->status;
}
} // namespace chronolog
