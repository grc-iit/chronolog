#ifndef ARCHIVE_READING_REQUEST_QUEUE_H
#define ARCHIVE_READING_REQUEST_QUEUE_H

#include <string>
#include <cstdint>
#include <mutex>
#include <deque>

#include <chronolog_types.h>

namespace chronolog
{

class QueryResponseAgent;

class ArchiveReadingRequest
{
public:
    QueryResponseAgent* queryResponseAgent;
    uint32_t queryId;
    ChronicleName chronicleName;
    StoryName storyName;
    chrono_time startTime;
    chrono_time endTime;

    ArchiveReadingRequest(QueryResponseAgent* query_response_agent,
                          uint32_t query_id,
                          ChronicleName const& chronicle,
                          StoryName const& story,
                          chrono_time const& start,
                          chrono_time const& end)
        : queryResponseAgent(query_response_agent)
        , queryId(query_id)
        , chronicleName(chronicle)
        , storyName(story)
        , startTime(start)
        , endTime(end)
    {}

    bool is_valid() const
    {
        // NB: startTime == 0 is a legitimate "from the beginning of time" lower bound
        // (a ReplayStory(chronicle, story, 0, end, ...) — the common "read everything"
        // query). Requiring startTime > 0 here silently dropped those archive requests,
        // so their responses were never marked ready and the client timed out (-12).
        // A valid range only needs startTime < endTime.
        return (queryResponseAgent != nullptr) && (queryId > 0) && !chronicleName.empty() && !storyName.empty() &&
               (startTime < endTime);
    }
};

class ArchiveReadingRequestQueue
{
public:
    ArchiveReadingRequestQueue() {}

    ~ArchiveReadingRequestQueue() {}

    bool empty() const { return readingRequestQueue.empty(); }

    void pushReadingRequest(ArchiveReadingRequest* a_request)
    {
        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        readingRequestQueue.push_back(a_request);
    }

    ArchiveReadingRequest* popReadingRequest()
    {
        ArchiveReadingRequest* a_request = nullptr;

        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        if(!readingRequestQueue.empty())
        {

            a_request = readingRequestQueue.front();
            readingRequestQueue.pop_front();
        }

        return a_request;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        if(!readingRequestQueue.empty())
        {
            for(auto& element: readingRequestQueue) { delete element; }
            readingRequestQueue.clear();
        }
    }


private:
    ArchiveReadingRequestQueue(ArchiveReadingRequestQueue const&) = delete;

    ArchiveReadingRequestQueue& operator=(ArchiveReadingRequestQueue const&) = delete;

    std::mutex readingRequestQueueMutex;
    std::deque<ArchiveReadingRequest*> readingRequestQueue;
};

} // namespace chronolog

#endif
