#include "chrono-player/replay/KeeperHotSource.h"
#include <future>
#include <thread>
#include <iostream>
#include <syncstream>
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

std::shared_ptr<internal::v1::Archive::Stub> KeeperHotSource::stubFor(const std::string& address) const
{
    std::lock_guard lk(mu_);
    auto& stub = stubs_[address];
    if(!stub)
    {
        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS, 1000);
        args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
        args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 1000);
        args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 1000);
        args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 1);
        args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
        args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 1000);
        args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
        stub = internal::v1::Archive::NewStub(
                grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args));
    }
    return stub;
}

KeeperFetch KeeperHotSource::fetchOne(const KeeperRef& keeper,
                                      StoryId story,
                                      const Range& range,
                                      std::atomic<size_t>& retained) const
{
    KeeperFetch out;
    out.frontier.process_id = keeper.process_id;
    out.frontier.answered = false;

    const auto deadline = std::chrono::system_clock::now() + options_.deadline;
    const auto request = convert::fetchHotRequest(story, range, options_.max_events);
    const auto address = internal_address_(keeper);
    do {
        grpc::ClientContext context;
        context.set_deadline(deadline);
        context.set_wait_for_ready(true);
        auto stub = stubFor(address);
        auto reader = stub->FetchHot(&context, request);
        internal::v1::FetchHotResponse response;
        bool received = false;
        bool trailer = false;
        bool limited = false;
        while(reader->Read(&response))
        {
            received = true;
            if(response.has_batch())
            {
                for(const auto& event: response.batch().events())
                {
                    size_t count = retained.load();
                    while(count < options_.read_max_events && !retained.compare_exchange_weak(count, count + 1)) {}
                    if(count < options_.read_max_events)
                        out.events.push_back(convert::fromProto(event));
                    else
                        limited = true;
                }
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
        const auto status = reader->Finish();
        out.frontier.truncated |= limited;
        out.frontier.answered = status.ok() && trailer;
        if(!out.frontier.answered)
            std::osyncstream(std::clog) << "fetch_hot_failed keeper=" << keeper.process_id
                                        << " status=" << status.error_code() << " trailer=" << trailer
                                        << " message=" << status.error_message() << std::endl;
        // Retry readiness failures without replaying a partially received stream or extending the deadline.
        if(received || status.error_code() != grpc::StatusCode::UNAVAILABLE)
            break;
        std::this_thread::sleep_until(
                std::min(deadline, std::chrono::system_clock::now() + std::chrono::milliseconds(50)));
    } while(std::chrono::system_clock::now() < deadline);
    return out;
}

absl::StatusOr<HotFetch> KeeperHotSource::fetch(StoryId story, const Range& range) const
{
    auto route = routes_->route(story);
    if(!route.ok())
        return route.status();
    HotFetch out;
    out.route_epoch = route->epoch;
    std::atomic<size_t> retained{0};
    std::vector<std::future<KeeperFetch>> pending;
    pending.reserve(route->keepers.size());
    for(const auto& keeper: route->keepers)
        pending.push_back(
                std::async(std::launch::async, [&, keeper] { return fetchOne(keeper, story, range, retained); }));
    for(auto& f: pending) out.keepers.push_back(f.get());
    if(writers_)
        out.writers = writers_->writers(story);
    return out;
}

} // namespace chronolog::player
