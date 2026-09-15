#ifndef CHRONOLOG_CHUNK_RECEIPT_H
#define CHRONOLOG_CHUNK_RECEIPT_H

#include <cstdint>
#include <vector>
#include <thallium/serialization/stl/vector.hpp>

namespace chronolog
{

// A grapher acknowledges a keeper's chunk as soon as it arrives, but decides
// only later where the chunk's events go: into a timeline window that is still
// open, into a past window reopened for them, or into a watermark-exempt
// salvage file. Only the first moves the persisted watermark W, so W covering
// a chunk does not prove the chunk was written. The grapher therefore gives
// every chunk a receipt and reports which receipts are still unwritten.

// Answer to receive_story_chunk. bytes is the size the receiver read; the
// sender compares it with what it sent. A grapher also returns the receipt it
// assigned the chunk, numbered per story, and the id of its own process
// instance, which changes on restart and with it the numbering. A receiver
// that tracks no durability (the player) leaves both 0.
struct ChunkReceipt
{
    uint64_t bytes = 0;
    uint64_t grapher_instance = 0;
    uint64_t receipt = 0;

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & bytes;
        serT & grapher_instance;
        serT & receipt;
    }
};

// One story's entry in the grapher's watermark report. Every receipt from
// grapher_instance up to highest_receipt has been written, except those in
// pending_receipts (ascending).
struct StoryWatermarkReport
{
    uint64_t watermark = 0;
    uint64_t grapher_instance = 0;
    uint64_t highest_receipt = 0;
    std::vector<uint64_t> pending_receipts;

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & watermark;
        serT & grapher_instance;
        serT & highest_receipt;
        serT & pending_receipts;
    }
};

} // namespace chronolog

#endif // CHRONOLOG_CHUNK_RECEIPT_H
