#ifndef ARCHIVE_READING_REQUEST_QUEUE_H
#define ARCHIVE_READING_REQUEST_QUEUE_H

#include <string>
#include <cstdint>
#include <mutex>
#include <deque>

#include <chronolog_types.h>

namespace chronolog
{

class StoryChunkTransferAgent;

struct ArchiveReadingRequest
{
    StoryChunkTransferAgent * queryResponseAgent;
    uint32_t queryId;
    ChronicleName chronicleName;
    StoryName storyName;
    chrono_time startTime;
    chrono_time endTime;

    ArchiveReadingRequest(StoryChunkTransferAgent * queryResponseAgent = nullptr,
			  uint32_t query_id = 0,
                          ChronicleName const& chronicle = std::string(),
                          StoryName const& story = std::string(),
                          chrono_time const& start = 0,
                          chrono_time const& end = 0)
        : queryResponseAgent(queryResponseAgent)
	, queryId (query_id)
        , chronicleName(chronicle)
        , storyName(story)
        , startTime(start)
        , endTime(end)
    {}

    bool is_valid() const
    { return (queryResponseAgent != nullptr) && (queryId >0); }
};

class ArchiveReadingRequestQueue
{
public:
    ArchiveReadingRequestQueue() {}

    ~ArchiveReadingRequestQueue() {}

    bool empty() const { return readingRequestQueue.empty(); }

    void pushReadingRequest(ArchiveReadingRequest const& a_request)
    {
        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        readingRequestQueue.push_back(a_request);
    }

    ArchiveReadingRequest& popReadingRequest(ArchiveReadingRequest& a_request)
    {
        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        if(!readingRequestQueue.empty())
        {
            a_request = readingRequestQueue.front();
            readingRequestQueue.pop_front();
        }
        else
        {
            a_request = ArchiveReadingRequest{};
        }

        return a_request;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        readingRequestQueue.clear();
    }


private:
    ArchiveReadingRequestQueue(ArchiveReadingRequestQueue const&) = delete;

    ArchiveReadingRequestQueue& operator=(ArchiveReadingRequestQueue const&) = delete;

    std::mutex readingRequestQueueMutex;
    std::deque<ArchiveReadingRequest> readingRequestQueue;
};

} // namespace chronolog

#endif
