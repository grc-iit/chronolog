#ifndef CHRONO_CHUNK_LOGGING_EXTRACTOR_H
#define CHRONO_CHUNK_LOGGING_EXTRACTOR_H

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>


namespace chronolog
{

class LoggingExtractor
{
public:
    int process_chunk(StoryChunk* story_chunk)
    {
        LOG_INFO("[LoggingExtractor] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",
                 thallium::thread::self_id(),
                 story_chunk->getStoryId(),
                 story_chunk->getChronicleName(),
                 story_chunk->getStoryName(),
                 story_chunk->getStartTime(),
                 story_chunk->getEndTime(),
                 story_chunk->getEventCount());

        return chronolog::CL_SUCCESS;
    }
};

} // namespace chronolog
#endif
