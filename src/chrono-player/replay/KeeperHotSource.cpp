#include "chrono-player/replay/KeeperHotSource.h"
#include <future>
#include <grpcpp/grpcpp.h>
#include "chrono-player/adapter/Convert.h"

namespace chronolog::player
{

KeeperHotSource::KeeperHotSource(std::shared_ptr<const RouteSource> routes,
                                 std::shared_ptr<const WriterSource> writers,
                                 std::function<std::string(const KeeperRef&)> internal_address,
                                 KeeperHotSourceOptions options)
    : routes_(std::move(routes))
    , writers_(std::move(writers))
    , internal_address_(std::move(internal_address))
    , options_(options)
{}

internal::v1::Archive::Stub& KeeperHotSource::stubFor(const std::string& address) const
{
    std::lock_guard lk(mu_);
    auto& stub = stubs_[address];
    if(!stub)
        stub = internal::v1::Archive::NewStub(grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
    return *stub;
}

KeeperFetch KeeperHotSource::fetchOne(const KeeperRef& keeper, StoryId story, const Range& range) const
{
    KeeperFetch out;
    out.frontier.process_id = keeper.process_id;
    out.frontier.answered = false;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + options_.deadline);
    auto request = convert::fetchHotRequest(story, range, options_.max_events);
    auto reader = stubFor(internal_address_(keeper)).FetchHot(&context, request);

    internal::v1::FetchHotResponse response;
    bool trailer = false;
    while(reader->Read(&response))
    {
        if(response.has_batch())
        {
            for(const auto& event: response.batch().events()) out.events.push_back(convert::fromProto(event));
        }
        else if(response.has_trailer())
        {
            trailer = true;
            out.frontier.epoch = response.trailer().epoch();
            out.frontier.sealed = convert::fromProto(response.trailer().sealed_frontier());
            out.frontier.evicted_below = convert::fromProto(response.trailer().evicted_below());
            out.frontier.truncated = response.trailer().truncated();
        }
    }
    // A stream that ends without its trailer carries no seal, so it cannot count as an answer.
    out.frontier.answered = reader->Finish().ok() && trailer;
    return out;
}

absl::StatusOr<HotFetch> KeeperHotSource::fetch(StoryId story, const Range& range) const
{
    auto route = routes_->route(story);
    if(!route.ok())
        return route.status();
    HotFetch out;
    out.route_epoch = route->epoch;
    std::vector<std::future<KeeperFetch>> pending;
    pending.reserve(route->keepers.size());
    for(const auto& keeper: route->keepers)
        pending.push_back(std::async(std::launch::async, [&, keeper] { return fetchOne(keeper, story, range); }));
    for(auto& f: pending) out.keepers.push_back(f.get());
    if(writers_)
        out.writers = writers_->writers(story);
    return out;
}

} // namespace chronolog::player
