#ifndef ARCHIVE_READING_REQUEST_H
#define ARCHIVE_READING_REQUEST_H

#include <mutex>
#include <deque>

#include "chronolog_types.h"

namespace chronolog
{

struct ArchiveReadingRequest
{
    ChronicleName chronicleName;
    StoryName     storyName;
    chrono_time      startTime;
    chrono_time      endTime;    

};

class ArchiveReadingRequestQueue
{
public:
    ArchiveReadingRequestQueue()
    {}
    
    ~ArchiveReadingRequestQueue()
    {}

    bool empty() const
    {   return readingRequestQueue.empty(); }

    void pushReadingRequest(ArchiveReadingRequest const& a_request)
    {
        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        readingRequestQueue.push_back(a_request);
    }
          
    ArchiveReadingRequest & popReadingRequest( ArchiveReadingRequest & a_request)
    {
        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        if( !readingRequestQueue.empty())
        {
            a_request = readingRequestQueue.front();
            readingRequestQueue.pop_front();
        }
        else 
        {
            a_request = ArchiveReadingRequest{"","",0,0};
        }

        return a_request;    
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(readingRequestQueueMutex);
        readingRequestQueue.clear();
    }

            
private:
    ArchiveReadingRequestQueue(ArchiveReadingRequestQueue const &) = delete;

    ArchiveReadingRequestQueue &operator=( ArchiveReadingRequestQueue const &) = delete;

    std::mutex  readingRequestQueueMutex;
    std::deque<ArchiveReadingRequest> readingRequestQueue;

};

}

#endif
