#ifndef CHRONOLOG_RECEIPT_TRACKER_H
#define CHRONOLOG_RECEIPT_TRACKER_H

#include <cstdint>

#include <chronolog_types.h>

namespace chronolog
{

// What a StoryPipeline tells about the receipts of the chunks it merges (see
// ChunkReceipt.h): which chunks hold a receipt's events, and whether all of
// them found a place. The grapher's StoryWatermarkRegistry implements it; the
// extractor that writes a holding chunk releases the receipt.
class ReceiptTracker
{
public:
    virtual ~ReceiptTracker() = default;

    // A timeline window or salvage chunk now holds events of the receipt.
    virtual void holdReceipt(StoryId const& story_id, uint64_t receipt) = 0;

    // One chunk holding events of the receipt was written.
    virtual void releaseReceipt(StoryId const& story_id, uint64_t receipt) = 0;

    // The receipt's chunk is merged and none of its events was discarded.
    virtual void receiptMerged(StoryId const& story_id, uint64_t receipt) = 0;
};

} // namespace chronolog

#endif // CHRONOLOG_RECEIPT_TRACKER_H
