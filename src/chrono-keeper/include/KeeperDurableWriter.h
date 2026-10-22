#ifndef KEEPER_DURABLE_WRITER_H
#define KEEPER_DURABLE_WRITER_H

#include <cstdint>

#include <chronolog_types.h>

#include "IngestionQueue.h"
#include "KeeperChunkRetentionStore.h"
#include "KeeperDataStore.h"
#include "KeeperWal.h"

namespace chronolog
{

// The keeper side of a durable write (record_event_durable). The event is
// ingested only after the WAL synced it, and a refusal ingests nothing, so a
// durable request is never downgraded to an accepted one.
//
// The checks run in this order:
//   1. no WAL (log is nullptr): CL_ERR_DURABILITY_UNAVAILABLE;
//   2. the event's identity is looked up in the WAL: a durable and pending
//      event gets CL_SUCCESS without a write or an ingest, and an attempt in
//      flight gives its own result, so a retry is never refused by a later
//      check while its first attempt is on disk or on its way there;
//   3. an empty record: CL_ERR_INVALID_ARG (the pipeline would drop it after a
//      durable answer);
//   4. the WAL failed or stopped: CL_ERR_DURABILITY_UNAVAILABLE; its file
//      system below the reserve: CL_ERR_DURABILITY_FULL;
//   5. the retention store holds more than durable_cap_bytes:
//      CL_ERR_DURABILITY_FULL;
//   6. the story gets a pipeline if it has none (ensureStoryPipeline, which
//      keeps the store's drop mark);
//   7. the WAL appends the event and waits for its group's sync; only on
//      CL_SUCCESS is the event ingested.
//
// write may block the calling ULT for a group commit, on Argobots primitives
// only, so an RPC handler yields its stream instead of blocking it.
class KeeperDurableWriter
{
public:
    KeeperDurableWriter(DurableLog* log /* nullptr: WAL off */,
                        IngestionQueue& ingestion_queue,
                        KeeperChunkRetentionStore& tail_store,
                        KeeperDataStore& data_store,
                        uint64_t durable_cap_bytes);

    int write(ChronicleName const&, StoryName const&, LogEvent const&);

private:
    KeeperDurableWriter(KeeperDurableWriter const&) = delete;
    KeeperDurableWriter& operator=(KeeperDurableWriter const&) = delete;

    DurableLog* theLog;
    IngestionQueue& theIngestionQueue;
    KeeperChunkRetentionStore& theTailStore;
    KeeperDataStore& theDataStore;
    uint64_t durableCapBytes;
};

} // namespace chronolog

#endif
