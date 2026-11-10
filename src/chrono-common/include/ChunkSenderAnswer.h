#ifndef CHRONOLOG_CHUNK_SENDER_ANSWER_H
#define CHRONOLOG_CHUNK_SENDER_ANSWER_H

#include <ChunkReceipt.h>
#include <RpcAnswer.h>

namespace chronolog
{

// Answers the keeper that sent a StoryChunk, for the grapher's and the
// player's receive_story_chunk handlers: answerRpcCaller with the receipt.
template <typename Request>
bool answerChunkSender(Request const& request, ChunkReceipt const& receipt, char const* service)
{
    return answerRpcCaller(request, receipt, service);
}

} // namespace chronolog

#endif
