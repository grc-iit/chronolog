#ifndef CHRONOLOG_CHUNK_SENDER_ANSWER_H
#define CHRONOLOG_CHUNK_SENDER_ANSWER_H

#include <thallium.hpp>

#include <ChunkReceipt.h>
#include <chrono_monitor.h>

namespace chronolog
{

// Answers the keeper that sent a StoryChunk, for the grapher's and the
// player's receive_story_chunk handlers. A thallium error from respond (the
// keeper died, or the network timed the handle out) is logged and swallowed:
// thrown out of the handler it would end the process. Returns whether the
// answer was sent. A template over the request so a test can pass one whose
// respond throws; the handlers pass a tl::request.
template <typename Request>
bool answerChunkSender(Request const& request, ChunkReceipt const& receipt, char const* service)
{
    try
    {
        request.respond(receipt);
        return true;
    }
    catch(thallium::exception const& ex)
    {
        LOG_WARNING("[{}] Could not answer the StoryChunk sender: {}", service, ex.what());
        return false;
    }
}

} // namespace chronolog

#endif
