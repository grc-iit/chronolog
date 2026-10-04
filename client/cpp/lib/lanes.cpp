#include "handles.h"

namespace chronolog::client
{
LaneWriter::LaneWriter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}
LaneWriter::~LaneWriter() = default;
LaneWriter::LaneWriter(LaneWriter&&) noexcept = default;
LaneWriter& LaneWriter::operator=(LaneWriter&&) noexcept = default;
size_t LaneWriter::lanes() const { return impl_->writers.size(); }
absl::StatusOr<AppendResult> LaneWriter::append(const AppendSpec& spec, Deadline deadline)
{
    auto result = impl_->append(std::span(&spec, 1), deadline, false);
    if(!result.ok())
        return result.status();
    return std::move(result->front());
}
absl::StatusOr<BatchResult> LaneWriter::appendBatch(std::span<const AppendSpec> specs, Deadline deadline)
{
    return impl_->append(specs, deadline, true);
}
absl::StatusOr<bool> LaneWriter::release(Deadline deadline)
{
    if(impl_->state->forked())
        return detail::forkedError();
    const auto end = impl_->state->deadline(deadline);
    std::unique_lock lock(impl_->mutex, std::defer_lock);
    if(!lock.try_lock_until(end))
        return absl::DeadlineExceededError("writer busy");
    absl::Status first;
    bool all = true;
    for(auto& writer: impl_->writers)
    {
        auto released = writer.release(end);
        if(!released.ok())
            first.Update(released.status());
        else
            all = all && *released;
    }
    if(!first.ok())
        return first;
    return all;
}
absl::StatusOr<BatchResult>
LaneWriter::Impl::append(std::span<const AppendSpec> specs, Deadline deadline, bool streaming)
{
    if(state->forked())
        return detail::forkedError();
    const auto end = state->deadline(deadline);
    std::unique_lock lock(mutex, std::defer_lock);
    if(!lock.try_lock_until(end))
        return absl::DeadlineExceededError("writer busy");
    if(specs.empty())
        return BatchResult{};
    if(pending)
    {
        if(pending->original.size() != specs.size() ||
           !std::equal(specs.begin(), specs.end(), pending->original.begin(), detail::same))
            return absl::FailedPreconditionError("retry uncertain append with identical specifications first");
    }
    else
    {
        Pending p;
        p.original.assign(specs.begin(), specs.end());
        p.stamped = p.original;
        for(auto& spec: p.stamped)
        {
            if(spec.physical)
                continue;
            auto reading = state->clock.now();
            if(!reading.ok())
                return reading.status();
            spec.physical = *reading;
        }
        p.done.resize(specs.size());
        pending = std::move(p);
    }
    const auto k = static_cast<int64_t>(writers.size());
    std::vector<std::vector<size_t>> lanes(writers.size());
    for(size_t i = 0; i < specs.size(); ++i)
    {
        if(pending->done[i])
            continue;
        const auto slice = std::max<int64_t>(pending->stamped[i].physical->physical_ns, 0) / slice_ns;
        lanes[static_cast<size_t>(slice % k)].push_back(i);
    }
    for(size_t lane = 0; lane < lanes.size(); ++lane)
    {
        const auto& indices = lanes[lane];
        if(indices.empty())
            continue;
        // I7.3: a sequential writer keeps its issue order across lanes.
        if(last_lane && *last_lane != lane)
            state->observe(last_hlc);
        std::vector<AppendSpec> batch;
        batch.reserve(indices.size());
        for(const auto i: indices) batch.push_back(pending->stamped[i]);
        auto sent = writers[lane].impl_->append(batch, Deadline(end), streaming);
        if(!sent.ok())
            return sent.status();
        last_lane = lane;
        for(size_t j = 0; j < indices.size(); ++j)
        {
            if((*sent)[j].ok())
                last_hlc = std::max(last_hlc, (*sent)[j]->hlc);
            pending->done[indices[j]] = std::move((*sent)[j]);
        }
    }
    BatchResult out;
    out.reserve(specs.size());
    for(auto& outcome: pending->done) out.push_back(std::move(*outcome));
    pending.reset();
    return out;
}
} // namespace chronolog::client
