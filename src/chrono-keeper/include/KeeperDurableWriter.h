#ifndef KEEPER_DURABLE_WRITER_H
#define KEEPER_DURABLE_WRITER_H

#include <cstdint>

#include <chronolog_types.h>

namespace chronolog
{

class DurableLog;
class IngestionQueue;
class KeeperChunkRetentionStore;
class KeeperDataStore;

// The keeper side of a durable write (record_event_durable). The event is
// ingested only after the WAL synced it, and a refusal ingests nothing, so a
// durable request is never downgraded to an accepted one.
//
// The checks run in this order:
//   1. no WAL (log is nullptr): CL_ERR_DURABILITY_UNAVAILABLE;
//   2. the event's story id is not storyIdOf(chronicle, story):
//      CL_ERR_INVALID_ARG. The WAL keeps the names and recovery recomputes
//      the id from them, so the event would be recovered under another id;
//   3. the event's identity is looked up in the WAL: a durable and pending
//      event gets CL_SUCCESS without a write or an ingest, and an attempt in
//      flight gives its own result, so a retry is never refused by a later
//      check while its first attempt is on disk or on its way there;
//   4. an empty record: CL_ERR_INVALID_ARG (the pipeline would drop it after a
//      durable answer);
//   5. the WAL failed or stopped: CL_ERR_DURABILITY_UNAVAILABLE; its file
//      system below the reserve: CL_ERR_DURABILITY_FULL;
//   6. the retention store holds more than durable_cap_bytes:
//      CL_ERR_DURABILITY_FULL;
//   7. a story with no ingestion handle gets a pipeline (ensureStoryPipeline,
//      which keeps the store's drop mark); when none can be started:
//      CL_ERR_DURABILITY_UNAVAILABLE;
//   8. the WAL appends the event and waits for its group's sync; only on
//      CL_SUCCESS is the event ingested.
//
// Waiting for the WAL (the lookup's wait for an attempt in flight, and the
// group commit) is on a tl::eventual, so the RPC handler's ULT yields its
// stream. The other steps take std::mutex locks and block the stream's OS
// thread while they wait: the WAL's and the retention store's locks are held
// only for short updates, and the ingestion queue's handle map is shared.
// Only a story with no ingestion handle takes the data store's mutex, which the
// collector holds for a whole tick; that happens once per story after a keeper
// restart, until a visor start or the first durable write creates the
// pipeline.
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
