#include "handles.h"
#include <limits>
#include <numeric>

namespace chronolog::client
{
namespace
{
bool same(const AppendSpec& a, const AppendSpec& b)
{
    const bool physicalSame = a.physical.has_value() == b.physical.has_value() &&
                              (!a.physical || (a.physical->physical_ns == b.physical->physical_ns &&
                                               a.physical->uncertainty_ns == b.physical->uncertainty_ns &&
                                               a.physical->status == b.physical->status));
    return physicalSame && a.durability == b.durability && a.envelope.content_type == b.envelope.content_type &&
           a.envelope.payload == b.envelope.payload && a.envelope.trace_id == b.envelope.trace_id &&
           a.envelope.span_id == b.envelope.span_id && a.envelope.attributes == b.envelope.attributes;
}
struct Request
{
    v1::AppendStreamRequest wire;
    std::vector<size_t> indices;
};
} // namespace
Writer::Writer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}
Writer::~Writer() = default;
Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;
Acquisition Writer::acquisition() const
{
    std::lock_guard lock(impl_->acquisition_mutex);
    return impl_->acquired;
}
absl::StatusOr<bool> Writer::release(Deadline deadline)
{
    auto end = impl_->state->deadline(deadline);
    std::unique_lock lock(impl_->mutex, std::defer_lock);
    if(!lock.try_lock_until(end))
        return absl::DeadlineExceededError("writer busy");
    grpc::ClientContext context;
    context.set_deadline(end);
    v1::ReleaseRequest request;
    request.set_story_id(impl_->acquired.story_id);
    request.set_writer_id(impl_->acquired.writer_id);
    request.set_incarnation(impl_->acquired.incarnation);
    v1::ReleaseResponse response;
    auto transport = impl_->state->catalog->Release(&context, request, &response);
    if(!transport.ok())
        return detail::status(transport);
    auto status = detail::status(response.status());
    if(!status.ok())
        return status;
    return response.fenced();
}
absl::StatusOr<AppendResult> Writer::append(const AppendSpec& spec, Deadline deadline)
{
    auto result = impl_->append(std::span(&spec, 1), deadline, false);
    if(!result.ok())
        return result.status();
    return std::move(result->front());
}
absl::StatusOr<BatchResult> Writer::appendBatch(std::span<const AppendSpec> specs, Deadline deadline)
{
    return impl_->append(specs, deadline, true);
}
absl::StatusOr<BatchResult> Writer::Impl::append(std::span<const AppendSpec> specs, Deadline deadline, bool streaming)
{
    const auto end = state->deadline(deadline);
    std::unique_lock lock(mutex, std::defer_lock);
    if(!lock.try_lock_until(end))
        return absl::DeadlineExceededError("writer busy");
    if(requires_reacquisition)
        return absl::FailedPreconditionError("writer keeper removed; re-acquire before appending");
    if(specs.empty())
        return BatchResult{};
    if(specs.size() > state->options.max_batch_items || specs.size() > std::numeric_limits<uint64_t>::max() - sequence)
        return absl::ResourceExhaustedError("append batch item limit");
    if(pending)
    {
        if(pending->specs.size() != specs.size() ||
           !std::equal(specs.begin(), specs.end(), pending->specs.begin(), same))
            return absl::FailedPreconditionError("retry uncertain append with identical specifications first");
    }
    else
    {
        Pending p;
        p.specs.assign(specs.begin(), specs.end());
        size_t bytes = 0;
        for(size_t i = 0; i < specs.size(); ++i)
        {
            if(specs[i].durability != Durability::Durable && specs[i].durability != Durability::Accepted)
                return absl::InvalidArgumentError("invalid requested durability");
            v1::AppendItem item;
            item.set_writer_id(acquired.writer_id);
            item.set_incarnation(acquired.incarnation);
            item.set_sequence(sequence + i);
            auto reading = specs[i].physical ? absl::StatusOr<TimeReading>(*specs[i].physical) : state->clock.now();
            if(!reading.ok())
                return reading.status();
            item.mutable_physical()->set_physical_ns(reading->physical_ns);
            item.mutable_physical()->set_status(reading->status == ClockStatus::Synced ? v1::CLOCK_STATUS_SYNCED
                                                : reading->status == ClockStatus::Unsynced
                                                        ? v1::CLOCK_STATUS_UNSYNCED
                                                        : v1::CLOCK_STATUS_UNAVAILABLE);
            if(reading->uncertainty_ns)
                item.mutable_physical()->set_uncertainty_ns(*reading->uncertainty_ns);
            *item.mutable_envelope() = detail::encode(specs[i].envelope);
            if(item.ByteSizeLong() > state->options.max_batch_bytes - bytes)
                return absl::ResourceExhaustedError("append batch byte limit");
            bytes += item.ByteSizeLong();
            p.items.push_back(std::move(item));
        }
        p.outcomes.resize(specs.size());
        pending = std::move(p);
    }
    auto refusal = absl::FailedPreconditionError("stale epoch retry budget exhausted");
    for(size_t attempt = 0; attempt <= state->options.retry.max_retries; ++attempt)
    {
        if(std::chrono::system_clock::now() >= end)
            return absl::DeadlineExceededError("append deadline");
        std::vector<Request> requests;
        for(size_t i = 0; i < specs.size(); ++i)
        {
            if(pending->outcomes[i])
                continue;
            auto durability = static_cast<v1::Durability>(specs[i].durability);
            if(requests.empty() || requests.back().indices.size() >= state->options.batch_size ||
               requests.back().wire.durability() != durability || requests.back().wire.ByteSizeLong() >= (256u << 10))
            {
                Request r;
                r.wire.set_story_id(acquired.story_id);
                r.wire.set_epoch(acquired.route.epoch);
                r.wire.set_durability(durability);
                r.wire.set_batch_id(++batch_id);
                requests.push_back(std::move(r));
            }
            auto& request = requests.back();
            request.indices.push_back(i);
            *request.wire.add_items() = pending->items[i];
        }
        if(requests.empty())
            break;
        std::optional<Route> redirect;
        auto consume = [&](const auto& response, const Request& request) -> absl::Status
        {
            if(response.batch_id() != request.wire.batch_id() ||
               response.results_size() != static_cast<int>(request.indices.size()))
                return absl::DataLossError("append correlation or result count mismatch");
            for(size_t j = 0; j < request.indices.size(); ++j)
            {
                const auto i = request.indices[j];
                const auto& result = response.results(static_cast<int>(j));
                if(result.has_assigned_hlc())
                    state->observe(detail::decode(result.assigned_hlc()));
                auto s = detail::status(result.status());
                if(s.code() == absl::StatusCode::kFailedPrecondition &&
                   (result.has_current_route() || response.has_current_route()))
                {
                    auto route = detail::decode(result.has_current_route() ? result.current_route()
                                                                           : response.current_route());
                    if(route.keepers.empty())
                        return absl::DataLossError("invalid epoch redirect");
                    refusal = s;
                    if(route.epoch <= request.wire.epoch())
                        continue;
                    if(!redirect || route.epoch > redirect->epoch)
                        redirect = std::move(route);
                    continue;
                }
                if(!s.ok())
                {
                    pending->outcomes[i] = s;
                    continue;
                }
                EventId expected{acquired.story_id,
                                 acquired.writer_id,
                                 acquired.incarnation,
                                 pending->items[i].sequence()};
                auto achieved = static_cast<Durability>(result.achieved_durability());
                if(detail::decode(result.id()) != expected || !result.has_assigned_hlc() ||
                   (achieved != Durability::Accepted && achieved != Durability::Durable) ||
                   (specs[i].durability == Durability::Durable && achieved != Durability::Durable))
                    return absl::DataLossError("invalid append identity or achieved durability");
                pending->outcomes[i] = AppendResult{expected, detail::decode(result.assigned_hlc()), achieved};
            }
            return absl::OkStatus();
        };
        auto journal = v1::Journal::NewStub(state->channel(acquired.assigned_keeper.endpoint));
        grpc::ClientContext context;
        context.set_deadline(state->attemptDeadline(end));
        absl::Status transport;
        if(!streaming)
        {
            auto& request = requests.front();
            detail::encode(state->causalFloor(), request.wire.mutable_items(0)->mutable_causal_floor());
            v1::AppendRequest unary;
            unary.set_story_id(request.wire.story_id());
            unary.set_epoch(request.wire.epoch());
            unary.set_batch_id(request.wire.batch_id());
            unary.set_durability(request.wire.durability());
            *unary.mutable_items() = request.wire.items();
            v1::AppendResponse response;
            transport = detail::status(journal->Append(&context, unary, &response));
            if(transport.ok())
                transport = consume(response, request);
        }
        else
        {
            auto stream = journal->AppendStream(&context);
            std::mutex window_mutex;
            std::condition_variable cv;
            size_t in_flight = 0;
            bool stopped = false;
            std::vector<bool> sent(requests.size()), received(requests.size());
            std::map<uint64_t, size_t> correlation;
            for(size_t i = 0; i < requests.size(); ++i) correlation[requests[i].wire.batch_id()] = i;
            std::thread sender(
                    [&]
                    {
                        for(size_t i = 0; i < requests.size(); ++i)
                        {
                            {
                                std::unique_lock window_lock(window_mutex);
                                if(!cv.wait_until(window_lock,
                                                  end,
                                                  [&]
                                                  { return stopped || in_flight < state->options.max_in_flight; }) ||
                                   stopped)
                                    break;
                                sent[i] = true;
                                ++in_flight;
                            }
                            auto wire = requests[i].wire;
                            for(auto& item: *wire.mutable_items())
                                detail::encode(state->causalFloor(), item.mutable_causal_floor());
                            if(!stream->Write(wire))
                                break;
                        }
                        stream->WritesDone();
                    });
            v1::AppendStreamResponse response;
            while(stream->Read(&response))
            {
                auto found = correlation.find(response.batch_id());
                {
                    std::lock_guard window_lock(window_mutex);
                    if(found == correlation.end() || !sent[found->second] || received[found->second])
                        transport = absl::DataLossError("unknown or duplicate append batch_id");
                    else
                    {
                        received[found->second] = true;
                        --in_flight;
                    }
                }
                cv.notify_all();
                if(!transport.ok())
                    break;
                transport = consume(response, requests[found->second]);
                if(!transport.ok())
                    break;
            }
            {
                std::lock_guard window_lock(window_mutex);
                stopped = true;
            }
            cv.notify_all();
            if(!transport.ok())
                context.TryCancel();
            sender.join();
            auto finished = detail::status(stream->Finish());
            if(transport.ok())
                transport = finished;
            if(transport.ok() && std::find(received.begin(), received.end(), false) != received.end())
                transport = absl::DataLossError("append stream ended without every batch response");
        }
        if(redirect)
        {
            std::lock_guard acquisition_lock(acquisition_mutex);
            acquired.route = *redirect;
            state->route(acquired.story_id, *redirect);
            auto survivor = std::find_if(redirect->keepers.begin(),
                                         redirect->keepers.end(),
                                         [&](const auto& keeper)
                                         { return keeper.process_id == acquired.assigned_keeper.process_id; });
            if(survivor == redirect->keepers.end())
            {
                requires_reacquisition = true;
                for(size_t i = 0; i < pending->outcomes.size(); ++i)
                    if(!pending->outcomes[i])
                        pending->outcomes[i] =
                                absl::UnknownError("append outcome unknown; writer keeper removed; re-acquire");
                break;
            }
            acquired.assigned_keeper = *survivor;
        }
        const bool complete = std::all_of(pending->outcomes.begin(),
                                          pending->outcomes.end(),
                                          [](const auto& r) { return r.has_value(); });
        if(complete && transport.ok())
            break;
        if(!transport.ok() && !detail::retryable(transport))
            return transport;
        if(attempt == state->options.retry.max_retries)
            return transport.ok() ? refusal : transport;
        std::this_thread::sleep_until(std::min(end, std::chrono::system_clock::now() + state->options.retry.backoff));
    }
    BatchResult out;
    out.reserve(specs.size());
    for(size_t i = 0; i < specs.size(); ++i)
    {
        if(!pending->outcomes[i])
            return absl::InternalError("missing append outcome");
        if(pending->outcomes[i]->ok() || pending->outcomes[i]->status().code() == absl::StatusCode::kOutOfRange)
            sequence = std::max(sequence, pending->items[i].sequence() + 1);
        out.push_back(std::move(*pending->outcomes[i]));
    }
    pending.reset();
    return out;
}
} // namespace chronolog::client
