#include "adapter/ArchiveService.h"

#include <deque>

#include "adapter/Convert.h"

namespace chronolog::keeper
{

namespace iv1 = chronolog::internal::v1;

namespace
{

class FetchHotReactor final: public grpc::ServerWriteReactor<iv1::FetchHotResponse>
{
public:
    // Called once, from a worker thread or inline on failure.
    void begin(std::deque<iv1::FetchHotResponse> messages, grpc::Status status)
    {
        messages_ = std::move(messages);
        status_ = std::move(status);
        next();
    }

    void OnWriteDone(bool ok) override
    {
        if(!ok)
        {
            Finish(grpc::Status::CANCELLED);
            return;
        }
        messages_.pop_front();
        next();
    }

    void OnDone() override { delete this; }

private:
    void next()
    {
        if(messages_.empty() || !status_.ok())
            Finish(status_);
        else
            StartWrite(&messages_.front());
    }

    std::deque<iv1::FetchHotResponse> messages_;
    grpc::Status status_{grpc::Status::OK};
};

} // namespace

ArchiveService::ArchiveService(RamJournal& journal, const Membership& membership, WorkerPool& pool, Options options)
    : journal_(journal)
    , membership_(membership)
    , pool_(pool)
    , options_(options)
{}

grpc::ServerWriteReactor<iv1::FetchHotResponse>* ArchiveService::FetchHot(grpc::CallbackServerContext*,
                                                                          const iv1::FetchHotRequest* request)
{
    auto* reactor = new FetchHotReactor();
    bool queued = pool_.submit(
            [this, reactor, req = *request]
            {
                CHRONOLOG_ASSERT_WORKER_THREAD();
                auto fail = [&](const absl::Status& status) { reactor->begin({}, convert::toGrpc(status)); };

                if(req.story_id() == 0)
                    return fail(absl::InvalidArgumentError("story_id is required"));
                if(journal_.dropped(req.story_id()))
                    return fail(absl::FailedPreconditionError("story was destroyed"));
                if(auto resolved = journal_.resolveRoute(req.story_id()); !resolved.ok())
                    return fail(absl::UnavailableError(std::string(resolved.message())));
                auto route = membership_.route(req.story_id());
                if(!route.ok())
                    return fail(route.status());
                std::optional<Predecessor> owner;
                for(const auto& p: journal_.predecessorOwners(req.story_id()))
                    if(p.epoch == req.expect_epoch())
                        owner = p;
                const auto instance = journal_.instance();
                Epoch epoch = route->epoch;
                if(owner)
                {
                    if(req.expect_instance() != instance)
                        return fail(absl::FailedPreconditionError("predecessor instance mismatch"));
                    epoch = owner->epoch;
                }
                else
                {
                    if(!req.expect_instance().empty() && req.expect_instance() != instance)
                        return fail(absl::FailedPreconditionError("instance mismatch"));
                    if((req.expect_epoch() != 0 && req.expect_epoch() != route->epoch) ||
                       journal_.retiredOwner(req.story_id()))
                        return fail(absl::FailedPreconditionError("owned epoch mismatch"));
                }
                Range range;
                switch(req.range_case())
                {
                    case iv1::FetchHotRequest::kHlc:
                        range.axis = Range::Axis::Hlc;
                        range.start = convert::fromProto(req.hlc().start());
                        range.end = convert::fromProto(req.hlc().end());
                        break;
                    case iv1::FetchHotRequest::kPhysical:
                        range.axis = Range::Axis::Physical;
                        range.start = Hlc{req.physical().start_ns(), 0};
                        range.end = Hlc{req.physical().end_ns(), 0};
                        break;
                    default:
                        return fail(absl::InvalidArgumentError("exactly one range is required"));
                }
                if(range.start > range.end)
                    return fail(absl::InvalidArgumentError("range start is after end"));
                const uint64_t limit = req.max_events() ? req.max_events() : options_.default_max_events;

                auto physical = journal_.physicalFrontier(req.story_id());
                if(!physical.ok())
                    return fail(physical.status());
                std::optional<Range> filter;
                if(range.axis == Range::Axis::Physical)
                    filter = range;
                if(req.has_physical_filter())
                    filter = Range{Range::Axis::Physical,
                                   {req.physical_filter().start_ns(), 0},
                                   {req.physical_filter().end_ns(), 0}};
                if(filter && filter->start >= filter->end)
                    return fail(absl::InvalidArgumentError("invalid physical range"));
                auto snapshot = journal_.sealedRead(req.story_id(), range, std::nullopt, filter);
                if(!snapshot.ok())
                    return fail(snapshot.status());
                std::deque<iv1::FetchHotResponse> out;
                iv1::FetchHotResponse* current = nullptr;
                size_t current_bytes = 0;
                size_t total_bytes = 0;
                uint64_t sent = 0;
                bool truncated = false;
                for(const auto& event: snapshot->events)
                {
                    const size_t bytes = event.envelope.payload.size();
                    if(sent >= limit || (sent > 0 && total_bytes + bytes > options_.max_bytes))
                    {
                        truncated = true;
                        break;
                    }
                    if(!current || current->batch().events_size() >= static_cast<int>(options_.batch_events) ||
                       (current_bytes > 0 && current_bytes + bytes > options_.batch_bytes))
                    {
                        current = &out.emplace_back();
                        current->mutable_batch();
                        current_bytes = 0;
                    }
                    *current->mutable_batch()->add_events() = convert::toProto(event);
                    current_bytes += bytes;
                    total_bytes += bytes;
                    ++sent;
                }
                if(!owner)
                {
                    auto applied = membership_.route(req.story_id());
                    if(!applied.ok())
                        return fail(applied.status());
                    epoch = applied->epoch;
                }
                auto* trailer = out.emplace_back().mutable_trailer();
                trailer->set_epoch(epoch);
                trailer->set_instance(instance);
                const auto window = PhysicalPolicy{}.acceptance_window_ns;
                const int64_t physical_cap = owner ? (owner->own_physical_ceiling_ns < INT64_MIN + window
                                                              ? INT64_MIN
                                                              : owner->own_physical_ceiling_ns - window)
                                                   : INT64_MAX;
                trailer->set_physical_frontier_ns(std::min(*physical, physical_cap));
                *trailer->mutable_sealed_frontier() = convert::toProto(
                        owner ? std::min(snapshot->view.sealed, owner->own_cut) : snapshot->view.sealed);
                for(const auto& f: snapshot->view.frontiers) *trailer->add_frontiers() = convert::toProto(f);
                *trailer->mutable_evicted_below() = convert::toProto(snapshot->evicted_below);
                trailer->set_truncated(truncated);
                reactor->begin(std::move(out), grpc::Status::OK);
            });
    if(!queued)
        reactor->begin({}, grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "keeper is saturated"));
    return reactor;
}

} // namespace chronolog::keeper
