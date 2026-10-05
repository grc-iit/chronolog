#pragma once

#include <algorithm>
#include <atomic>
#include <optional>
#include <vector>
#include "player/adapter/EventConvert.h"
#include "chronolog/message_limits.h"
#include "chronolog/replay.h"

namespace chronolog::player
{

// A finished result served in batches: the events are already merged, deduplicated and in their final order.
class MergedStream final: public ReplayStream
{
public:
    MergedStream(std::vector<Event> events, Completion completion, size_t batch_size)
        : events_(std::move(events))
        , completion_(std::move(completion))
        , batch_size_(std::max<size_t>(batch_size, 1))
    {}

    absl::StatusOr<std::optional<ReplayBatch>> next() override
    {
        if(cancelled_.load())
            return absl::CancelledError("replay stream cancelled");
        ReplayBatch batch;
        size_t bytes = 0;
        while(pos_ < events_.size() && batch.events.size() < batch_size_)
        {
            const size_t event_bytes = convert::encodedSize(events_[pos_]);
            if(!batch.events.empty() && bytes + event_bytes > kEventBatchBytes)
                break;
            bytes += event_bytes;
            batch.events.push_back(std::move(events_[pos_++]));
        }
        if(!batch.events.empty())
            return std::optional<ReplayBatch>(std::move(batch));
        if(!completion_sent_)
        {
            completion_sent_ = true;
            batch.completion = completion_;
            return std::optional<ReplayBatch>(std::move(batch));
        }
        return std::optional<ReplayBatch>();
    }
    void cancel() override { cancelled_.store(true); }

private:
    std::vector<Event> events_;
    Completion completion_;
    size_t batch_size_;
    size_t pos_{};
    bool completion_sent_{};
    std::atomic<bool> cancelled_{false};
};

} // namespace chronolog::player
