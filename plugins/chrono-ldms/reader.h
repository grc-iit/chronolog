#pragma once
#include <chrono>
#include <thread>
#include <vector>
#include "chronolog/client/client.h"

namespace chronolog::ldms
{
struct StoryEvents
{
    Story story;
    std::vector<Event> events;
    bool complete{};
};

// Reads everything stored so far; the end bound is the current wall time, so a complete result means every
// Keeper sealed past it. Retries while the read reports LAGGING_WRITERS, bounded to about ten seconds.
inline absl::StatusOr<std::vector<StoryEvents>> readChronicle(client::Client& client, const std::string& chronicle)
{
    auto stories = client.listStories(chronicle);
    if(!stories.ok())
        return stories.status();
    std::vector<StoryEvents> out;
    for(const auto& story: *stories)
    {
        StoryEvents result{story, {}, false};
        for(int attempt = 0; attempt < 100 && !result.complete; ++attempt)
        {
            const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
            auto read = client.read(story.id, {Hlc{0, 0}, Hlc{now, 0}});
            if(!read.ok())
                return read.status();
            result.events.clear();
            for(;;)
            {
                auto item = read->next(std::chrono::system_clock::now() + std::chrono::seconds(10));
                if(!item.ok())
                {
                    if(absl::IsFailedPrecondition(item.status()) || absl::IsUnavailable(item.status()))
                        break;
                    return item.status();
                }
                if(!*item)
                    break;
                if((**item).completion)
                    result.complete = (**item).completion->complete;
                for(auto& event: (**item).events) result.events.push_back(std::move(event));
            }
            if(!result.complete)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        out.push_back(std::move(result));
    }
    return out;
}
} // namespace chronolog::ldms
