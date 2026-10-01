#include "membership/RouteWatcher.h"

#include "adapter/Convert.h"

namespace chronolog::keeper
{

namespace iv1 = chronolog::internal::v1;

RouteWatcher::RouteWatcher(ConfigMembership& membership,
                           std::shared_ptr<grpc::Channel> channel,
                           std::string process_id,
                           std::string instance,
                           RamJournal* journal)
    : journal_(journal)
    , membership_(membership)
    , stub_(iv1::Cluster::NewStub(std::move(channel)))
    , process_id_(std::move(process_id))
    , instance_(std::move(instance))
{
    watcher_ = std::make_unique<Watcher>([this](std::stop_token stop) { return session(stop); });
}

bool RouteWatcher::session(std::stop_token stop)
{
    grpc::ClientContext context;
    std::stop_callback cancel(stop, [&context] { context.TryCancel(); });
    iv1::WatchRoutesRequest request;
    request.set_process_id(process_id_);
    request.set_instance(instance_);
    auto reader = stub_->WatchRoutes(&context, request);
    iv1::WatchRoutesResponse message;
    bool progressed = false;
    while(reader->Read(&message))
    {
        auto current = membership_.route(message.story_id());
        if(current.ok() && message.route().epoch() < current->epoch)
            continue;
        auto& prior = applied_[message.story_id()];
        if(message.revision() < applied_revision_ || message.revision() < prior.first ||
           message.route().epoch() < prior.second)
            continue;
        applied_revision_ = message.revision();
        prior = {message.revision(), message.route().epoch()};
        progressed = true;
        auto state = convert::routeState(message);
        auto install = [&] { membership_.setRouteState(message.story_id(), state); };
        if(journal_)
            journal_->applyRoute(message.story_id(),
                                 state,
                                 std::find(message.observe_floor().begin(),
                                           message.observe_floor().end(),
                                           process_id_) != message.observe_floor().end(),
                                 message.revision(),
                                 install);
        else
            install();
    }
    reader->Finish();
    return progressed;
}

} // namespace chronolog::keeper
