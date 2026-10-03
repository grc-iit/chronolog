#include "internal.h"
#include "catalog_target.h"
#include "handles.h"

namespace chronolog::client
{
struct Client::Impl
{
    std::shared_ptr<detail::State> state;
};
Client::Client(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}
Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;
absl::StatusOr<Client> Client::Connect(ClientOptions options, Deadline deadline)
{
    if(options.catalog_endpoint.empty() || options.rpc_timeout.count() <= 0 || options.retry.backoff.count() < 0 ||
       !options.max_in_flight || options.max_in_flight > 64 || !options.batch_size || !options.max_batch_items ||
       options.batch_size > options.max_batch_items || !options.max_batch_bytes)
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
    return Client(std::make_unique<Impl>(Impl{std::move(state)}));
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
absl::StatusOr<Writer> Client::acquire(StoryId id, const std::string& identity, Deadline deadline)
{
    v1::AcquireRequest request;
    request.set_story_id(id);
    request.set_writer_identity(identity);
    CATALOG_CALL(Acquire);
    Acquisition acquired{response.story_id(),
                         response.writer_id(),
                         response.incarnation(),
                         detail::decode(response.route()),
                         {response.assigned_keeper().process_id(), response.assigned_keeper().endpoint()}};
    if(!acquired.story_id || !acquired.writer_id || !acquired.incarnation || !acquired.route.epoch ||
       acquired.assigned_keeper.endpoint.empty())
        return absl::DataLossError("incomplete acquisition response");
    impl_->state->route(id, acquired.route);
    return Writer(std::make_unique<Writer::Impl>(impl_->state, std::move(acquired)));
}
#undef CATALOG_CALL
absl::StatusOr<ReadStream> Client::read(StoryId id, HlcRange range, Deadline deadline)
{
    if(!id || range.end < range.start)
        return absl::InvalidArgumentError("invalid HLC read range");
    auto end = impl_->state->deadline(deadline);
    auto endpoint = impl_->state->playerEndpoint(id, end);
    if(!endpoint.ok())
        return endpoint.status();
    return ReadStream(std::make_unique<ReadStream::Impl>(impl_->state, *endpoint, id, range, deadline));
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
