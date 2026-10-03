#include "internal.h"
#include "catalog_target.h"
#include "handles.h"
#include "lease.h"
#include "chronolog/acquire_refusal.h"

namespace chronolog::client
{
std::optional<AcquireRefusal> acquireRefusalOf(const absl::Status& status) { return getAcquireRefusal(status); }
struct Client::Impl
{
    std::shared_ptr<detail::State> state;
    std::unique_ptr<detail::Scheduler> scheduler;
};
Client::Client(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}
Client::~Client()
{
    // A forked child must not join the parent's scheduler or tear down inherited gRPC state.
    if(impl_ && impl_->state->forked())
        (void)impl_.release();
}
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&& other) noexcept
{
    if(this != &other)
    {
        if(impl_ && impl_->state->forked())
            (void)impl_.release();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
void Client::observeFloor(Hlc floor) { impl_->state->observe(floor); }
Hlc Client::causalFloor() const { return impl_->state->causalFloor(); }
absl::StatusOr<Client> Client::Connect(ClientOptions options, Deadline deadline)
{
    if(options.catalog_endpoint.empty() || options.rpc_timeout.count() <= 0 || options.retry.backoff.count() < 0 ||
       !options.max_in_flight || options.max_in_flight > 64 || !options.batch_size || !options.max_batch_items ||
       options.batch_size > options.max_batch_items || !options.max_batch_bytes ||
       !(options.lease.lead_fraction >= 0 && options.lease.lead_fraction < 1) || options.lease.margin.count() < 0 ||
       options.lease.poll.count() <= 0 || !options.lease.max_batch || options.lease.max_batch > 10000)
        return absl::InvalidArgumentError("invalid client options");
    auto target = detail::catalogTarget(options.catalog_endpoint);
    if(!target.ok())
        return target.status();
    options.catalog_endpoint = std::move(*target);
    auto state = std::make_shared<detail::State>(std::move(options));
    auto channel = state->channel(state->options.catalog_endpoint);
    if(!channel->WaitForConnected(state->deadline(deadline)))
        return absl::DeadlineExceededError("catalog connection deadline");
    state->catalog = v1::Catalog::NewStub(channel);
    state->lifecycle = std::make_shared<detail::Lifecycle>();
    auto scheduler = std::make_unique<detail::Scheduler>(state);
    return Client(std::make_unique<Impl>(Impl{std::move(state), std::move(scheduler)}));
}
#define CATALOG_CALL(Method)                                                                                           \
    grpc::ClientContext context;                                                                                       \
    detail::withDeadline(context, impl_->state->deadline(deadline));                                                   \
    v1::Method##Response response;                                                                                     \
    auto transport = impl_->state->catalog->Method(&context, request, &response);                                      \
    if(!transport.ok())                                                                                                \
        return detail::status(transport);                                                                              \
    if(auto status = detail::status(response.status()); !status.ok())                                                  \
    return status
absl::StatusOr<Chronicle> Client::createChronicle(const std::string& name, Deadline deadline)
{
    v1::CreateChronicleRequest request;
    request.set_name(name);
    CATALOG_CALL(CreateChronicle);
    return detail::decode(response.chronicle());
}
absl::StatusOr<Chronicle> Client::getChronicle(const std::string& name, Deadline deadline)
{
    v1::GetChronicleRequest request;
    request.set_name(name);
    CATALOG_CALL(GetChronicle);
    return detail::decode(response.chronicle());
}
absl::StatusOr<std::vector<Chronicle>> Client::listChronicles(Deadline deadline)
{
    v1::ListChroniclesRequest request;
    CATALOG_CALL(ListChronicles);
    std::vector<Chronicle> out;
    for(const auto& c: response.chronicles()) out.push_back(detail::decode(c));
    return out;
}
absl::Status Client::destroyChronicle(const std::string& name, Deadline deadline)
{
    v1::DestroyChronicleRequest request;
    request.set_name(name);
    CATALOG_CALL(DestroyChronicle);
    return absl::OkStatus();
}
absl::StatusOr<Story> Client::createStory(const std::string& chronicle, const std::string& name, Deadline deadline)
{
    v1::CreateStoryRequest request;
    request.set_chronicle(chronicle);
    request.set_name(name);
    CATALOG_CALL(CreateStory);
    return detail::decode(response.story());
}
absl::StatusOr<Story> Client::getStory(StoryId id, Deadline deadline)
{
    v1::GetStoryRequest request;
    request.set_story_id(id);
    CATALOG_CALL(GetStory);
    return detail::decode(response.story());
}
absl::StatusOr<Route> Client::route(StoryId id, Deadline deadline)
{
    if(!id)
        return absl::InvalidArgumentError("invalid route story");
    v1::GetStoryRequest request;
    request.set_story_id(id);
    CATALOG_CALL(GetStory);
    const auto& story = response.story();
    if(story.tombstoned())
        return absl::FailedPreconditionError("story is tombstoned");
    if(!story.has_route() || !story.route().epoch() || story.route().epoch() != story.epoch() ||
       story.route().keepers().empty() || story.route().player().empty())
        return absl::DataLossError("story has no current route");
    auto current = detail::decode(story.route());
    impl_->state->route(id, current);
    return current;
}
absl::StatusOr<std::vector<Story>> Client::listStories(const std::string& chronicle, Deadline deadline)
{
    v1::ListStoriesRequest request;
    request.set_chronicle(chronicle);
    CATALOG_CALL(ListStories);
    std::vector<Story> out;
    for(const auto& s: response.stories()) out.push_back(detail::decode(s));
    return out;
}
absl::Status Client::destroyStory(StoryId id, Deadline deadline)
{
    v1::DestroyStoryRequest request;
    request.set_story_id(id);
    CATALOG_CALL(DestroyStory);
    return absl::OkStatus();
}
namespace
{
using Phase = detail::Lifecycle::Phase;
// A no-grant answer after which the Catalog may still have committed the grant.
bool uncertain(const absl::Status& s)
{
    switch(s.code())
    {
        case absl::StatusCode::kInvalidArgument:
        case absl::StatusCode::kNotFound:
        case absl::StatusCode::kFailedPrecondition:
        case absl::StatusCode::kAlreadyExists:
        case absl::StatusCode::kPermissionDenied:
        case absl::StatusCode::kUnauthenticated:
        case absl::StatusCode::kOutOfRange:
        case absl::StatusCode::kUnimplemented:
            return false;
        default:
            return true;
    }
}
absl::Status refusal(const v1::AcquireResponse& r)
{
    absl::Status result = detail::status(r.status());
    if(!result.ok() && (r.refusal_reason() || r.has_termination_cause() || r.incarnation()))
    {
        AcquireRefusal detail;
        const auto reason = static_cast<uint32_t>(r.refusal_reason());
        const auto cause = static_cast<uint32_t>(r.termination_cause());
        detail.refusal_reason =
                reason <= 2 ? static_cast<AcquireRefusalReason>(reason) : AcquireRefusalReason::Unspecified;
        detail.remaining_ns = std::max<int64_t>(r.remaining_ns(), 0);
        if(r.has_current_incarnation() && r.current_incarnation())
            detail.current_incarnation = r.current_incarnation();
        if(r.incarnation())
            detail.matched_incarnation = r.incarnation();
        if(r.has_termination_cause())
            detail.termination_cause = cause <= 4 ? static_cast<AcquisitionTerminationCause>(cause)
                                                  : AcquisitionTerminationCause::Unspecified;
        setAcquireRefusal(result, detail);
    }
    return result;
}
// RFC-G section 7: recover this Client's own prior incarnation for the same story and identity. A failed Release is
// retried; a prior that is still live, or whose Release stays unconfirmed, is taken over only by a compare-and-swap
// on that exact incarnation; a confirmed EXPIRED prior permits a conditional plain acquire. A newer foreign holder
// answers PRIOR_MISMATCH or HELD and is never taken over implicitly.
absl::StatusOr<AcquireOptions> recoverOwnPrior(detail::State& state,
                                               StoryId id,
                                               const std::string& identity,
                                               AcquireOptions options,
                                               detail::TimePoint end)
{
    if(options.takeover || options.expected_prior_incarnation)
        return options;
    auto& life = *state.lifecycle;
    detail::Lifecycle::Prior prior;
    {
        std::lock_guard lock(life.mutex);
        auto found = life.priors.find({id, identity});
        if(found == life.priors.end())
            return options;
        prior = found->second;
    }
    if(prior.cause)
    {
        // RELEASED reopens plainly; SUPERSEDED and OWNER_REMOVED need the caller's explicit takeover or remap.
        if(*prior.cause == AcquisitionTerminationCause::Expired)
            options.expected_prior_incarnation = prior.incarnation;
        return options;
    }
    bool attempted = prior.release_attempted;
    if(auto live = prior.writer.lock())
    {
        std::lock_guard lease_lock(live->mutex);
        if(!live->closing)
            return options;
        attempted = true;
    }
    if(attempted)
    {
        auto released = detail::release(state, {id, prior.writer_id, prior.incarnation}, end);
        if(released.ok())
        {
            std::lock_guard lock(life.mutex);
            life.released(id, identity, prior.incarnation);
            return options;
        }
        if(released.status().code() == absl::StatusCode::kFailedPrecondition ||
           released.status().code() == absl::StatusCode::kNotFound)
        {
            options.expected_prior_incarnation = prior.incarnation;
            return options;
        }
        if(!uncertain(released.status()))
            return released.status();
    }
    options.takeover = true;
    options.expected_prior_incarnation = prior.incarnation;
    return options;
}
} // namespace
absl::StatusOr<std::string> Client::newAcquireRequestId()
{
    if(impl_->state->forked())
        return detail::forkedError();
    auto& life = *impl_->state->lifecycle;
    std::lock_guard lock(life.mutex);
    return life.mint();
}
absl::StatusOr<Writer> Client::acquire(StoryId id, const std::string& identity, Deadline deadline)
{
    return acquire(id, identity, AcquireOptions{}, deadline);
}
absl::StatusOr<Writer>
Client::acquire(StoryId id, const std::string& identity, AcquireOptions options, Deadline deadline)
{
    auto& state = impl_->state;
    if(state->forked())
        return detail::forkedError();
    const auto end = state->deadline(deadline);
    auto& life = *state->lifecycle;
    const auto key = detail::keyOf(id, identity, options);
    std::string request_id = options.acquire_request_id;
    std::optional<AcquireOptions> dispatched;
    {
        std::unique_lock lock(life.mutex);
        if(request_id.empty())
        {
            for(const auto& [candidate, entry]: life.ids)
                if(entry.phase == Phase::Unresolved && entry.key == key)
                    request_id = candidate;
            if(request_id.empty())
                request_id = life.mint();
        }
        // C8: one dispatch per id at a time; a concurrent call waits for it.
        if(!life.cv.wait_until(lock,
                               end,
                               [&]
                               {
                                   auto found = life.ids.find(request_id);
                                   return found == life.ids.end() || found->second.phase != Phase::InFlight;
                               }))
            return absl::DeadlineExceededError("acquire request id is in flight in another call");
        auto found = life.ids.find(request_id);
        if(found == life.ids.end())
            return absl::InvalidArgumentError("acquire request id was not issued by this Client in this process");
        auto& entry = found->second;
        if(entry.phase == Phase::Delivered)
            return absl::FailedPreconditionError("acquire request id already delivered its grant to a Writer");
        if(entry.key && *entry.key != key)
            return absl::InvalidArgumentError("acquire request id reused with a different story, identity or options");
        entry.key = key;
        entry.phase = Phase::InFlight;
        entry.touched = ++life.clock;
        dispatched = entry.dispatched;
    }
    auto settle = [&](Phase phase)
    {
        {
            std::lock_guard lock(life.mutex);
            if(auto found = life.ids.find(request_id); found != life.ids.end())
            {
                found->second.phase = phase;
                if(dispatched && phase != Phase::Issued)
                    found->second.dispatched = dispatched;
            }
        }
        life.cv.notify_all();
    };
    if(!dispatched)
    {
        auto derived = recoverOwnPrior(*state, id, identity, options, end);
        if(!derived.ok())
        {
            settle(Phase::Issued);
            return derived.status();
        }
        dispatched = std::move(*derived);
    }
    v1::AcquireRequest request;
    request.set_story_id(id);
    request.set_writer_identity(identity);
    if(dispatched->lease_duration_ns)
        request.set_lease_duration_ns(*dispatched->lease_duration_ns);
    if(dispatched->preferred_keeper_process_id)
        request.set_preferred_keeper_process_id(*dispatched->preferred_keeper_process_id);
    request.set_takeover(dispatched->takeover);
    if(dispatched->expected_prior_incarnation)
        request.set_expected_prior_incarnation(*dispatched->expected_prior_incarnation);
    request.set_acquire_request_id(request_id);
    grpc::ClientContext context;
    detail::withDeadline(context, end);
    v1::AcquireResponse response;
    const auto sent = state->boottime();
    auto transport = state->catalog->Acquire(&context, request, &response);
    absl::Status failure = transport.ok() ? refusal(response) : detail::status(transport);
    Acquisition acquired{response.story_id(),
                         response.writer_id(),
                         response.incarnation(),
                         detail::decode(response.route()),
                         {response.assigned_keeper().process_id(), response.assigned_keeper().endpoint()},
                         {response.lease().duration_ns(), response.lease().remaining_ns()},
                         {}};
    if(response.has_keeper_preference())
        acquired.keeper_preference = static_cast<KeeperPreferenceResult>(response.keeper_preference());
    if(failure.ok() &&
       (!acquired.story_id || !acquired.writer_id || !acquired.incarnation || !acquired.route.epoch ||
        acquired.assigned_keeper.endpoint.empty() || acquired.lease.duration_ns < 0 || acquired.lease.remaining_ns < 0))
        failure = absl::DataLossError("incomplete acquisition response");
    if(!failure.ok())
    {
        // A same-id terminal retry names its matched incarnation; that id can never produce a grant again.
        const auto detail = getAcquireRefusal(failure);
        settle(uncertain(failure) && !(detail && detail->matched_incarnation) ? Phase::Unresolved : Phase::Refused);
        return failure;
    }
    auto lease = std::make_shared<detail::Lease>(
            RenewAcquisition{acquired.story_id, acquired.writer_id, acquired.incarnation});
    if(acquired.lease.duration_ns > 0)
    {
        std::lock_guard lease_lock(lease->mutex);
        lease->confirm(acquired.lease, sent, state->options.lease);
    }
    {
        std::lock_guard lock(life.mutex);
        auto& prior = life.priors[{id, identity}];
        const bool duplicate = prior.writer_id == acquired.writer_id && prior.incarnation == acquired.incarnation &&
                               !prior.writer.expired();
        if(auto found = life.ids.find(request_id); found != life.ids.end())
        {
            found->second.phase = Phase::Delivered;
            found->second.dispatched = dispatched;
        }
        if(duplicate)
        {
            life.cv.notify_all();
            return absl::FailedPreconditionError("this Client already holds a Writer for that incarnation");
        }
        prior = {acquired.writer_id, acquired.incarnation, false, {}, lease, ++life.clock};
        life.bound();
    }
    life.cv.notify_all();
    state->route(id, acquired.route);
    if(acquired.lease.duration_ns > 0)
        impl_->scheduler->add(lease);
    auto impl = std::make_unique<Writer::Impl>(state, std::move(acquired));
    impl->identity = identity;
    impl->lease = std::move(lease);
    return Writer(std::move(impl));
}
#undef CATALOG_CALL
absl::StatusOr<ReadStream> Client::read(StoryId id, HlcRange range, Deadline deadline)
{
    return read(id, range, ReadOptions{}, deadline);
}
absl::StatusOr<ReadStream> Client::read(StoryId id, HlcRange range, ReadOptions options, Deadline deadline)
{
    if(!id || range.end < range.start)
        return absl::InvalidArgumentError("invalid HLC read range");
    auto end = impl_->state->deadline(deadline);
    auto endpoint = impl_->state->playerEndpoint(id, end);
    if(!endpoint.ok())
        return endpoint.status();
    return ReadStream(std::make_unique<ReadStream::Impl>(impl_->state, *endpoint, id, range, options, deadline));
}
absl::StatusOr<ReadStream> Client::readPhysical(StoryId id, PhysicalRange range, Deadline deadline)
{
    if(!id || range.end_ns <= range.start_ns)
        return absl::InvalidArgumentError("invalid physical read range");
    auto endpoint = impl_->state->playerEndpoint(id, impl_->state->deadline(deadline));
    if(!endpoint.ok())
        return endpoint.status();
    return ReadStream(std::make_unique<ReadStream::Impl>(impl_->state, *endpoint, id, range, deadline));
}
absl::StatusOr<TailStream> Client::tail(StoryId id, std::optional<Position> after, Deadline deadline)
{
    if(!id || (after && after->id.story_id != id))
        return absl::InvalidArgumentError("invalid tail story or position");
    auto end = impl_->state->deadline(deadline);
    auto endpoint = impl_->state->playerEndpoint(id, end);
    if(!endpoint.ok())
        return endpoint.status();
    return TailStream(std::make_unique<TailStream::Impl>(impl_->state, *endpoint, id, after, deadline));
}
} // namespace chronolog::client
