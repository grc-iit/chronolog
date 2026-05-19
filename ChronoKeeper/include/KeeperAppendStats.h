#ifndef KEEPER_APPEND_STATS_H
#define KEEPER_APPEND_STATS_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <chrono_monitor.h>

namespace chronolog
{

class KeeperAppendStats
{
public:
    enum class JournalOwnerFlushReason
    {
        Unknown,
        EventLimit,
        ByteLimit,
        QueueBoundary,
        StopSync
    };

    static KeeperAppendStats& instance()
    {
        static KeeperAppendStats stats;
        return stats;
    }

    static uint64_t nowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
    }

    void recordRpc(uint64_t duration_ns, uint64_t payload_bytes)
    {
        uint64_t record_count = recordEventCount.fetch_add(1, std::memory_order_relaxed) + 1;
        recordEventPayloadBytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        recordEventTotalNs.fetch_add(duration_ns, std::memory_order_relaxed);
        updateMax(recordEventMaxNs, duration_ns);
        recordLatencyBucket(recordEventLatencyBuckets, duration_ns);
        uint64_t const interval = statsIntervalEvents();
        if(interval > 0 && record_count % interval == 0)
        {
            logSummary("record_event_interval");
        }
    }

    void recordRpcBatch(uint64_t duration_ns, uint64_t payload_bytes, uint64_t event_count)
    {
        if(event_count == 0)
        {
            return;
        }
        uint64_t record_count = recordEventCount.fetch_add(event_count, std::memory_order_relaxed) + event_count;
        recordEventPayloadBytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        recordEventTotalNs.fetch_add(duration_ns * event_count, std::memory_order_relaxed);
        updateMax(recordEventMaxNs, duration_ns);
        recordLatencyBucket(recordEventLatencyBuckets, duration_ns, event_count);
        uint64_t const interval = statsIntervalEvents();
        if(interval > 0 && record_count % interval < event_count)
        {
            logSummary("record_event_interval");
        }
    }

    void recordRpcBatchPhases(uint64_t submit_ns, uint64_t completion_wait_ns)
    {
        rpcBatchPhaseCount.fetch_add(1, std::memory_order_relaxed);
        rpcBatchSubmitNs.fetch_add(submit_ns, std::memory_order_relaxed);
        rpcBatchCompletionWaitNs.fetch_add(completion_wait_ns, std::memory_order_relaxed);
        updateMax(rpcBatchSubmitMaxNs, submit_ns);
        updateMax(rpcBatchCompletionWaitMaxNs, completion_wait_ns);
    }

    void recordIngestionQueue(uint64_t duration_ns, uint64_t lookup_ns, bool orphan)
    {
        ingestionQueueCount.fetch_add(1, std::memory_order_relaxed);
        ingestionQueueTotalNs.fetch_add(duration_ns, std::memory_order_relaxed);
        ingestionQueueLookupNs.fetch_add(lookup_ns, std::memory_order_relaxed);
        updateMax(ingestionQueueMaxNs, duration_ns);
        if(orphan)
        {
            orphanEventCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void recordIngestionQueueBatch(uint64_t duration_ns, uint64_t lookup_ns, uint64_t event_count, bool orphan)
    {
        if(event_count == 0)
        {
            return;
        }
        ingestionQueueCount.fetch_add(event_count, std::memory_order_relaxed);
        ingestionQueueTotalNs.fetch_add(duration_ns, std::memory_order_relaxed);
        ingestionQueueLookupNs.fetch_add(lookup_ns, std::memory_order_relaxed);
        updateMax(ingestionQueueMaxNs, duration_ns);
        if(orphan)
        {
            orphanEventCount.fetch_add(event_count, std::memory_order_relaxed);
        }
    }

    void recordHandleIngest(uint64_t lock_wait_ns, uint64_t lock_hold_ns, uint64_t push_ns, uint64_t queue_depth)
    {
        handleIngestCount.fetch_add(1, std::memory_order_relaxed);
        handleLockWaitNs.fetch_add(lock_wait_ns, std::memory_order_relaxed);
        handleLockHoldNs.fetch_add(lock_hold_ns, std::memory_order_relaxed);
        handlePushNs.fetch_add(push_ns, std::memory_order_relaxed);
        updateMax(handleLockWaitMaxNs, lock_wait_ns);
        updateMax(handleLockHoldMaxNs, lock_hold_ns);
        updateMax(handlePushMaxNs, push_ns);
        updateMax(handleQueueDepthMax, queue_depth);
    }

    void recordHandleIngestBatch(uint64_t lock_wait_ns,
                                 uint64_t lock_hold_ns,
                                 uint64_t push_ns,
                                 uint64_t queue_depth,
                                 uint64_t event_count)
    {
        if(event_count == 0)
        {
            return;
        }
        handleIngestCount.fetch_add(event_count, std::memory_order_relaxed);
        handleLockWaitNs.fetch_add(lock_wait_ns, std::memory_order_relaxed);
        handleLockHoldNs.fetch_add(lock_hold_ns, std::memory_order_relaxed);
        handlePushNs.fetch_add(push_ns, std::memory_order_relaxed);
        updateMax(handleLockWaitMaxNs, lock_wait_ns);
        updateMax(handleLockHoldMaxNs, lock_hold_ns);
        updateMax(handlePushMaxNs, push_ns);
        updateMax(handleQueueDepthMax, queue_depth);
    }

    void recordHandleCollectLock(uint64_t collection_wait_ns,
                                 uint64_t collection_hold_ns,
                                 uint64_t lane_registry_wait_ns,
                                 uint64_t lane_registry_hold_ns,
                                 uint64_t event_count)
    {
        handleCollectCount.fetch_add(1, std::memory_order_relaxed);
        handleCollectEvents.fetch_add(event_count, std::memory_order_relaxed);
        handleCollectLockWaitNs.fetch_add(collection_wait_ns, std::memory_order_relaxed);
        handleCollectLockHoldNs.fetch_add(collection_hold_ns, std::memory_order_relaxed);
        handleLaneRegistryLockWaitNs.fetch_add(lane_registry_wait_ns, std::memory_order_relaxed);
        handleLaneRegistryLockHoldNs.fetch_add(lane_registry_hold_ns, std::memory_order_relaxed);
        updateMax(handleCollectLockWaitMaxNs, collection_wait_ns);
        updateMax(handleCollectLockHoldMaxNs, collection_hold_ns);
        updateMax(handleLaneRegistryLockWaitMaxNs, lane_registry_wait_ns);
        updateMax(handleLaneRegistryLockHoldMaxNs, lane_registry_hold_ns);
    }

    void recordJournalAppend(uint64_t lock_wait_ns,
                             uint64_t write_ns,
                             uint64_t fdatasync_ns,
                             uint64_t publish_ns,
                             uint64_t payload_bytes)
    {
        journalAppendCount.fetch_add(1, std::memory_order_relaxed);
        journalAppendPayloadBytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        journalLockWaitNs.fetch_add(lock_wait_ns, std::memory_order_relaxed);
        journalWriteNs.fetch_add(write_ns, std::memory_order_relaxed);
        journalFdSyncNs.fetch_add(fdatasync_ns, std::memory_order_relaxed);
        journalPublishNs.fetch_add(publish_ns, std::memory_order_relaxed);
        recordLatencyBucket(journalFdSyncBuckets, fdatasync_ns);
        updateMax(journalLockWaitMaxNs, lock_wait_ns);
        updateMax(journalWriteMaxNs, write_ns);
        updateMax(journalFdSyncMaxNs, fdatasync_ns);
        updateMax(journalPublishMaxNs, publish_ns);
    }

    void recordJournalAdmission(uint64_t event_count,
                                uint64_t payload_bytes,
                                uint64_t shard_group_count,
                                uint64_t work_build_ns,
                                uint64_t enqueue_ns)
    {
        if(event_count == 0)
        {
            return;
        }
        journalAdmissionBatchCount.fetch_add(1, std::memory_order_relaxed);
        journalAdmissionEventCount.fetch_add(event_count, std::memory_order_relaxed);
        journalAdmissionPayloadBytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        journalAdmissionShardGroupCount.fetch_add(shard_group_count, std::memory_order_relaxed);
        journalAdmissionWorkBuildNs.fetch_add(work_build_ns, std::memory_order_relaxed);
        journalAdmissionEnqueueNs.fetch_add(enqueue_ns, std::memory_order_relaxed);
        updateMax(journalAdmissionMaxEvents, event_count);
        updateMax(journalAdmissionMaxShardGroups, shard_group_count);
        updateMax(journalAdmissionWorkBuildMaxNs, work_build_ns);
        updateMax(journalAdmissionEnqueueMaxNs, enqueue_ns);
    }

    void recordJournalOwner(uint64_t queue_wait_ns, uint64_t service_ns, uint64_t publish_lock_wait_ns)
    {
        journalOwnerCount.fetch_add(1, std::memory_order_relaxed);
        journalOwnerQueueWaitNs.fetch_add(queue_wait_ns, std::memory_order_relaxed);
        journalOwnerServiceNs.fetch_add(service_ns, std::memory_order_relaxed);
        journalOwnerPublishLockWaitNs.fetch_add(publish_lock_wait_ns, std::memory_order_relaxed);
        updateMax(journalOwnerQueueWaitMaxNs, queue_wait_ns);
        updateMax(journalOwnerServiceMaxNs, service_ns);
        updateMax(journalOwnerPublishLockWaitMaxNs, publish_lock_wait_ns);
        recordLatencyBucket(journalOwnerQueueWaitBuckets, queue_wait_ns);
        recordLatencyBucket(journalOwnerServiceBuckets, service_ns);
    }

    void recordJournalOwnerPendingFlush(uint64_t pending_ns)
    {
        journalOwnerPendingFlushCount.fetch_add(1, std::memory_order_relaxed);
        journalOwnerPendingFlushNs.fetch_add(pending_ns, std::memory_order_relaxed);
        updateMax(journalOwnerPendingFlushMaxNs, pending_ns);
    }

    void recordJournalOwnerFlush(uint64_t record_count,
                                 uint64_t wall_ns,
                                 uint64_t fdatasync_ns,
                                 uint64_t publish_ns,
                                 uint64_t early_completion_ns,
                                 JournalOwnerFlushReason reason = JournalOwnerFlushReason::Unknown,
                                 uint64_t active_admission_count = 0)
    {
        if(record_count == 0)
        {
            return;
        }
        journalOwnerFlushCount.fetch_add(1, std::memory_order_relaxed);
        journalOwnerFlushRecords.fetch_add(record_count, std::memory_order_relaxed);
        journalOwnerFlushWallNs.fetch_add(wall_ns, std::memory_order_relaxed);
        journalOwnerFlushFdSyncNs.fetch_add(fdatasync_ns, std::memory_order_relaxed);
        journalOwnerFlushPublishNs.fetch_add(publish_ns, std::memory_order_relaxed);
        journalOwnerFlushEarlyCompletionNs.fetch_add(early_completion_ns, std::memory_order_relaxed);
        updateMax(journalOwnerFlushMaxRecords, record_count);
        updateMax(journalOwnerFlushWallMaxNs, wall_ns);
        updateMax(journalOwnerFlushFdSyncMaxNs, fdatasync_ns);
        updateMax(journalOwnerFlushPublishMaxNs, publish_ns);
        updateMax(journalOwnerFlushEarlyCompletionMaxNs, early_completion_ns);
        if(active_admission_count > 0)
        {
            journalOwnerFlushActiveAdmissionCount.fetch_add(1, std::memory_order_relaxed);
            journalOwnerFlushActiveAdmissionRecords.fetch_add(record_count, std::memory_order_relaxed);
            updateMax(journalOwnerFlushActiveAdmissionMax, active_admission_count);
        }
        switch(reason)
        {
            case JournalOwnerFlushReason::EventLimit:
                journalOwnerFlushEventLimitCount.fetch_add(1, std::memory_order_relaxed);
                break;
            case JournalOwnerFlushReason::ByteLimit:
                journalOwnerFlushByteLimitCount.fetch_add(1, std::memory_order_relaxed);
                break;
            case JournalOwnerFlushReason::QueueBoundary:
                journalOwnerFlushQueueBoundaryCount.fetch_add(1, std::memory_order_relaxed);
                if(active_admission_count > 0)
                {
                    journalOwnerFlushQueueBoundaryActiveAdmissionCount.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            case JournalOwnerFlushReason::StopSync:
                journalOwnerFlushStopSyncCount.fetch_add(1, std::memory_order_relaxed);
                break;
            case JournalOwnerFlushReason::Unknown:
                journalOwnerFlushUnknownCount.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }

    void recordJournalOwnerQueueBoundaryWait(uint64_t wait_ns, bool woke_for_work)
    {
        journalOwnerQueueBoundaryWaitCount.fetch_add(1, std::memory_order_relaxed);
        journalOwnerQueueBoundaryWaitNs.fetch_add(wait_ns, std::memory_order_relaxed);
        updateMax(journalOwnerQueueBoundaryWaitMaxNs, wait_ns);
        if(woke_for_work)
        {
            journalOwnerQueueBoundaryWaitWokeForWorkCount.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            journalOwnerQueueBoundaryWaitTimeoutCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void recordJournalOwnerQueueBoundaryAdmissionWait(uint64_t wait_ns, bool woke_for_work)
    {
        journalOwnerQueueBoundaryAdmissionWaitCount.fetch_add(1, std::memory_order_relaxed);
        journalOwnerQueueBoundaryAdmissionWaitNs.fetch_add(wait_ns, std::memory_order_relaxed);
        updateMax(journalOwnerQueueBoundaryAdmissionWaitMaxNs, wait_ns);
        if(woke_for_work)
        {
            journalOwnerQueueBoundaryAdmissionWaitWokeForWorkCount.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            journalOwnerQueueBoundaryAdmissionWaitDrainedCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void recordJournalCompletionDispatch(uint64_t record_count, uint64_t dispatch_ns)
    {
        if(record_count == 0)
        {
            return;
        }
        journalCompletionDispatchCount.fetch_add(1, std::memory_order_relaxed);
        journalCompletionDispatchRecords.fetch_add(record_count, std::memory_order_relaxed);
        journalCompletionDispatchNs.fetch_add(dispatch_ns, std::memory_order_relaxed);
        updateMax(journalCompletionDispatchMaxRecords, record_count);
        updateMax(journalCompletionDispatchMaxNs, dispatch_ns);
    }

    void recordJournalBatchCompletion(uint64_t record_count, uint64_t remaining_before, bool final_callback)
    {
        if(record_count == 0)
        {
            return;
        }
        journalBatchCompletionCount.fetch_add(1, std::memory_order_relaxed);
        journalBatchCompletionRecords.fetch_add(record_count, std::memory_order_relaxed);
        journalBatchCompletionRemainingBefore.fetch_add(remaining_before, std::memory_order_relaxed);
        updateMax(journalBatchCompletionMaxRecords, record_count);
        updateMax(journalBatchCompletionMaxRemainingBefore, remaining_before);
        recordBatchSizeBucket(journalBatchCompletionSizeBuckets, record_count);
        if(final_callback)
        {
            journalBatchCompletionFinalCallbackCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void recordJournalOwnerBatch(uint64_t record_count, bool group_commit)
    {
        if(record_count == 0)
        {
            return;
        }
        journalOwnerBatchCount.fetch_add(1, std::memory_order_relaxed);
        journalOwnerBatchRecords.fetch_add(record_count, std::memory_order_relaxed);
        updateMax(journalOwnerBatchMaxRecords, record_count);
        recordBatchSizeBucket(journalOwnerBatchSizeBuckets, record_count);
        if(group_commit)
        {
            journalOwnerGroupCommitCount.fetch_add(1, std::memory_order_relaxed);
            journalOwnerGroupCommitRecords.fetch_add(record_count, std::memory_order_relaxed);
            updateMax(journalOwnerGroupCommitMaxRecords, record_count);
            recordBatchSizeBucket(journalOwnerGroupCommitSizeBuckets, record_count);
        }
    }

    void recordJournalOwnerQueueEnqueue(uint64_t lock_wait_ns,
                                        uint64_t lock_hold_ns,
                                        uint64_t enqueued_count,
                                        uint64_t queue_depth)
    {
        journalOwnerQueueEnqueueCount.fetch_add(1, std::memory_order_relaxed);
        journalOwnerQueueEnqueueRecords.fetch_add(enqueued_count, std::memory_order_relaxed);
        journalOwnerQueueEnqueueLockWaitNs.fetch_add(lock_wait_ns, std::memory_order_relaxed);
        journalOwnerQueueEnqueueLockHoldNs.fetch_add(lock_hold_ns, std::memory_order_relaxed);
        updateMax(journalOwnerQueueEnqueueLockWaitMaxNs, lock_wait_ns);
        updateMax(journalOwnerQueueEnqueueLockHoldMaxNs, lock_hold_ns);
        updateMax(journalOwnerQueueDepthMax, queue_depth);
    }

    void recordJournalOwnerQueueDrain(uint64_t lock_wait_ns,
                                      uint64_t lock_hold_ns,
                                      uint64_t drained_count,
                                      uint64_t queue_depth)
    {
        journalOwnerQueueDrainCount.fetch_add(1, std::memory_order_relaxed);
        journalOwnerQueueDrainRecords.fetch_add(drained_count, std::memory_order_relaxed);
        journalOwnerQueueDrainLockWaitNs.fetch_add(lock_wait_ns, std::memory_order_relaxed);
        journalOwnerQueueDrainLockHoldNs.fetch_add(lock_hold_ns, std::memory_order_relaxed);
        updateMax(journalOwnerQueueDrainLockWaitMaxNs, lock_wait_ns);
        updateMax(journalOwnerQueueDrainLockHoldMaxNs, lock_hold_ns);
        updateMax(journalOwnerQueueDepthMax, queue_depth);
    }

    void recordJournalGroupSync(uint64_t fdatasync_ns, uint64_t record_count)
    {
        if(record_count == 0)
        {
            return;
        }
        journalGroupSyncCount.fetch_add(1, std::memory_order_relaxed);
        journalGroupSyncRecords.fetch_add(record_count, std::memory_order_relaxed);
        journalGroupSyncNs.fetch_add(fdatasync_ns, std::memory_order_relaxed);
        updateMax(journalGroupSyncMaxNs, fdatasync_ns);
        updateMax(journalGroupSyncMaxRecords, record_count);
        recordLatencyBucket(journalGroupSyncBuckets, fdatasync_ns);
        recordBatchSizeBucket(journalGroupSyncSizeBuckets, record_count);
    }

    void recordJournalDurablePublish(uint64_t record_count)
    {
        if(record_count == 0)
        {
            return;
        }
        journalDurablePublishCount.fetch_add(record_count, std::memory_order_relaxed);
        journalDurablePublishBatchCount.fetch_add(1, std::memory_order_relaxed);
        journalDurablePublishBatchRecords.fetch_add(record_count, std::memory_order_relaxed);
        updateMax(journalDurablePublishBatchMaxRecords, record_count);
        recordBatchSizeBucket(journalDurablePublishBatchSizeBuckets, record_count);
    }

    void recordJournalTailSeal(uint64_t record_count, uint64_t sealed_page_count)
    {
        if(record_count == 0)
        {
            return;
        }
        journalTailSealCount.fetch_add(1, std::memory_order_relaxed);
        journalTailSealRecords.fetch_add(record_count, std::memory_order_relaxed);
        updateMax(journalTailSealMaxRecords, record_count);
        updateMax(journalTailSealedPageMax, sealed_page_count);
    }

    void recordJournalCallbackDispatchEnqueue(uint64_t lock_wait_ns, uint64_t queue_depth)
    {
        journalCallbackDispatchEnqueueCount.fetch_add(1, std::memory_order_relaxed);
        journalCallbackDispatchEnqueueLockWaitNs.fetch_add(lock_wait_ns, std::memory_order_relaxed);
        updateMax(journalCallbackDispatchEnqueueLockWaitMaxNs, lock_wait_ns);
        updateMax(journalCallbackDispatchQueueDepthMax, queue_depth);
    }

    void recordJournalCallbackDispatch(uint64_t queue_wait_ns, uint64_t service_ns)
    {
        journalCallbackDispatchCount.fetch_add(1, std::memory_order_relaxed);
        journalCallbackDispatchQueueWaitNs.fetch_add(queue_wait_ns, std::memory_order_relaxed);
        journalCallbackDispatchServiceNs.fetch_add(service_ns, std::memory_order_relaxed);
        updateMax(journalCallbackDispatchQueueWaitMaxNs, queue_wait_ns);
        updateMax(journalCallbackDispatchServiceMaxNs, service_ns);
        recordLatencyBucket(journalCallbackDispatchQueueWaitBuckets, queue_wait_ns);
    }

    uint64_t currentRecordEventCount() const { return recordEventCount.load(std::memory_order_relaxed); }

    uint64_t currentJournalAppendCount() const { return journalAppendCount.load(std::memory_order_relaxed); }

    void recordJournalTail(uint64_t snapshot_ns,
                           uint64_t snapshot_lock_wait_ns,
                           uint64_t sort_ns,
                           uint64_t payload_read_ns,
                           uint64_t payload_alloc_ns,
                           uint64_t payload_syscall_ns,
                           uint64_t payload_memcpy_ns,
                           uint64_t event_build_ns,
                           uint64_t matched_events,
                           uint64_t read_group_count,
                           uint64_t physical_read_bytes,
                           uint64_t payload_copy_bytes,
                           bool direct_read,
                           bool vectored_read)
    {
        journalTailReadCount.fetch_add(1, std::memory_order_relaxed);
        journalTailMatchedEvents.fetch_add(matched_events, std::memory_order_relaxed);
        journalTailSnapshotNs.fetch_add(snapshot_ns, std::memory_order_relaxed);
        journalTailSnapshotLockWaitNs.fetch_add(snapshot_lock_wait_ns, std::memory_order_relaxed);
        journalTailSortNs.fetch_add(sort_ns, std::memory_order_relaxed);
        journalTailPayloadReadNs.fetch_add(payload_read_ns, std::memory_order_relaxed);
        journalTailPayloadAllocNs.fetch_add(payload_alloc_ns, std::memory_order_relaxed);
        journalTailPayloadSyscallNs.fetch_add(payload_syscall_ns, std::memory_order_relaxed);
        journalTailPayloadMemcpyNs.fetch_add(payload_memcpy_ns, std::memory_order_relaxed);
        journalTailEventBuildNs.fetch_add(event_build_ns, std::memory_order_relaxed);
        recordLatencyBucket(journalTailPayloadReadBuckets, payload_read_ns);
        journalTailReadGroupCount.fetch_add(read_group_count, std::memory_order_relaxed);
        journalTailPhysicalReadBytes.fetch_add(physical_read_bytes, std::memory_order_relaxed);
        journalTailPayloadCopyBytes.fetch_add(payload_copy_bytes, std::memory_order_relaxed);
        if(direct_read)
        {
            journalTailDirectReadCount.fetch_add(1, std::memory_order_relaxed);
        }
        else if(vectored_read)
        {
            journalTailVectoredReadCount.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            journalTailCoalescedReadCount.fetch_add(1, std::memory_order_relaxed);
        }
        updateMax(journalTailSnapshotMaxNs, snapshot_ns);
        updateMax(journalTailSnapshotLockWaitMaxNs, snapshot_lock_wait_ns);
        updateMax(journalTailSortMaxNs, sort_ns);
        updateMax(journalTailPayloadReadMaxNs, payload_read_ns);
        updateMax(journalTailPayloadAllocMaxNs, payload_alloc_ns);
        updateMax(journalTailPayloadSyscallMaxNs, payload_syscall_ns);
        updateMax(journalTailPayloadMemcpyMaxNs, payload_memcpy_ns);
        updateMax(journalTailEventBuildMaxNs, event_build_ns);
        updateMax(journalTailReadGroupMax, read_group_count);
    }

    void recordPostAckIngest(uint64_t duration_ns)
    {
        postAckIngestCount.fetch_add(1, std::memory_order_relaxed);
        postAckIngestNs.fetch_add(duration_ns, std::memory_order_relaxed);
        updateMax(postAckIngestMaxNs, duration_ns);
    }

    void recordPostAckIngestBatch(uint64_t duration_ns, uint64_t event_count)
    {
        if(event_count == 0)
        {
            return;
        }
        postAckIngestCount.fetch_add(event_count, std::memory_order_relaxed);
        postAckIngestNs.fetch_add(duration_ns, std::memory_order_relaxed);
        updateMax(postAckIngestMaxNs, duration_ns);
    }

    void recordAsyncDrainEnqueue(uint64_t queue_depth)
    {
        asyncDrainEnqueueCount.fetch_add(1, std::memory_order_relaxed);
        updateMax(asyncDrainQueueDepthMax, queue_depth);
    }

    void recordAsyncDrainComplete(uint64_t duration_ns)
    {
        asyncDrainCompleteCount.fetch_add(1, std::memory_order_relaxed);
        asyncDrainNs.fetch_add(duration_ns, std::memory_order_relaxed);
        updateMax(asyncDrainMaxNs, duration_ns);
    }

    void recordAsyncDrainCompleteBatch(uint64_t duration_ns, uint64_t event_count)
    {
        if(event_count == 0)
        {
            return;
        }
        asyncDrainCompleteCount.fetch_add(event_count, std::memory_order_relaxed);
        asyncDrainNs.fetch_add(duration_ns, std::memory_order_relaxed);
        updateMax(asyncDrainMaxNs, duration_ns);
    }

    void recordWalDrainRead(uint64_t duration_ns, uint64_t payload_bytes)
    {
        walDrainReadCount.fetch_add(1, std::memory_order_relaxed);
        walDrainReadNs.fetch_add(duration_ns, std::memory_order_relaxed);
        walDrainReadPayloadBytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        updateMax(walDrainReadMaxNs, duration_ns);
        walDrainReadBatchCount.fetch_add(1, std::memory_order_relaxed);
        walDrainReadPhysicalBytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        walDrainReadSyscallCount.fetch_add(payload_bytes > 0 ? 1 : 0, std::memory_order_relaxed);
        updateMax(walDrainReadBatchMaxRecords, 1);
    }

    void recordWalDrainReadBatch(uint64_t duration_ns,
                                 uint64_t payload_bytes,
                                 uint64_t record_count,
                                 uint64_t batch_count = 1,
                                 uint64_t physical_read_bytes = 0,
                                 uint64_t read_syscall_count = 0,
                                 uint64_t max_batch_records = 1)
    {
        if(record_count == 0)
        {
            return;
        }
        walDrainReadCount.fetch_add(record_count, std::memory_order_relaxed);
        walDrainReadNs.fetch_add(duration_ns, std::memory_order_relaxed);
        walDrainReadPayloadBytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        updateMax(walDrainReadMaxNs, duration_ns);
        walDrainReadBatchCount.fetch_add(batch_count == 0 ? 1 : batch_count, std::memory_order_relaxed);
        walDrainReadPhysicalBytes.fetch_add(physical_read_bytes == 0 ? payload_bytes : physical_read_bytes,
                                            std::memory_order_relaxed);
        walDrainReadSyscallCount.fetch_add(read_syscall_count == 0 ? batch_count : read_syscall_count,
                                           std::memory_order_relaxed);
        updateMax(walDrainReadBatchMaxRecords, max_batch_records);
    }

    void recordWalDrainBatchWait(uint64_t duration_ns)
    {
        walDrainBatchWaitCount.fetch_add(1, std::memory_order_relaxed);
        walDrainBatchWaitNs.fetch_add(duration_ns, std::memory_order_relaxed);
        updateMax(walDrainBatchWaitMaxNs, duration_ns);
    }

    void recordTimelineMerge(uint64_t lock_wait_ns,
                             uint64_t lock_hold_ns,
                             uint64_t insert_ns,
                             uint64_t event_count,
                             uint64_t inserted_count)
    {
        if(event_count == 0)
        {
            return;
        }
        timelineMergeBatchCount.fetch_add(1, std::memory_order_relaxed);
        timelineMergeEventCount.fetch_add(event_count, std::memory_order_relaxed);
        timelineMergeInsertedCount.fetch_add(inserted_count, std::memory_order_relaxed);
        timelineMergeLockWaitNs.fetch_add(lock_wait_ns, std::memory_order_relaxed);
        timelineMergeLockHoldNs.fetch_add(lock_hold_ns, std::memory_order_relaxed);
        timelineMergeInsertNs.fetch_add(insert_ns, std::memory_order_relaxed);
        updateMax(timelineMergeBatchMaxEvents, event_count);
        updateMax(timelineMergeLockWaitMaxNs, lock_wait_ns);
        updateMax(timelineMergeLockHoldMaxNs, lock_hold_ns);
        updateMax(timelineMergeInsertMaxNs, insert_ns);
    }

    void logSummary(char const* context) const
    {
        uint64_t rpc_count = recordEventCount.load(std::memory_order_relaxed);
        uint64_t queue_count = ingestionQueueCount.load(std::memory_order_relaxed);
        uint64_t handle_count = handleIngestCount.load(std::memory_order_relaxed);
        uint64_t handle_collect_count = handleCollectCount.load(std::memory_order_relaxed);
        uint64_t journal_count = journalAppendCount.load(std::memory_order_relaxed);
        uint64_t journal_admission_batch_count =
                journalAdmissionBatchCount.load(std::memory_order_relaxed);
        uint64_t journal_owner_count = journalOwnerCount.load(std::memory_order_relaxed);
        uint64_t journal_owner_pending_flush_count =
                journalOwnerPendingFlushCount.load(std::memory_order_relaxed);
        uint64_t journal_owner_flush_count = journalOwnerFlushCount.load(std::memory_order_relaxed);
        uint64_t journal_completion_dispatch_count =
                journalCompletionDispatchCount.load(std::memory_order_relaxed);
        uint64_t journal_batch_completion_count =
                journalBatchCompletionCount.load(std::memory_order_relaxed);
        uint64_t journal_owner_batch_count = journalOwnerBatchCount.load(std::memory_order_relaxed);
        uint64_t journal_owner_group_commit_count = journalOwnerGroupCommitCount.load(std::memory_order_relaxed);
        uint64_t journal_owner_queue_enqueue_count =
                journalOwnerQueueEnqueueCount.load(std::memory_order_relaxed);
        uint64_t journal_owner_queue_drain_count =
                journalOwnerQueueDrainCount.load(std::memory_order_relaxed);
        uint64_t journal_group_sync_count = journalGroupSyncCount.load(std::memory_order_relaxed);
        uint64_t journal_durable_publish_batch_count =
                journalDurablePublishBatchCount.load(std::memory_order_relaxed);
        uint64_t callback_dispatch_count = journalCallbackDispatchCount.load(std::memory_order_relaxed);
        uint64_t journal_tail_count = journalTailReadCount.load(std::memory_order_relaxed);
        uint64_t post_ack_ingest_count = postAckIngestCount.load(std::memory_order_relaxed);
        uint64_t async_drain_complete_count = asyncDrainCompleteCount.load(std::memory_order_relaxed);
        uint64_t wal_drain_read_count = walDrainReadCount.load(std::memory_order_relaxed);
        uint64_t wal_drain_batch_count = walDrainReadBatchCount.load(std::memory_order_relaxed);
        uint64_t wal_drain_syscall_count = walDrainReadSyscallCount.load(std::memory_order_relaxed);
        uint64_t timeline_merge_event_count = timelineMergeEventCount.load(std::memory_order_relaxed);
        uint64_t timeline_merge_batch_count = timelineMergeBatchCount.load(std::memory_order_relaxed);
        uint64_t rpc_batch_phase_count = rpcBatchPhaseCount.load(std::memory_order_relaxed);
        LOG_INFO("[KeeperAppendStats] context={} record_event_count={} payload_bytes={} "
                 "record_event_avg_us={} record_event_max_us={} "
                 "record_event_le_100us_count={} record_event_le_250us_count={} "
                 "record_event_le_500us_count={} record_event_le_1000us_count={} "
                 "record_event_le_2500us_count={} record_event_le_5000us_count={} "
                 "record_event_le_10000us_count={} record_event_gt_10000us_count={} "
                 "rpc_batch_phase_count={} rpc_batch_submit_avg_us={} rpc_batch_submit_max_us={} "
                 "rpc_batch_completion_wait_avg_us={} rpc_batch_completion_wait_max_us={} "
                 "ingestion_queue_count={} "
                 "ingestion_queue_avg_us={} ingestion_lookup_avg_us={} ingestion_queue_max_us={} orphan_events={} "
                 "handle_ingest_count={} handle_lock_wait_avg_us={} handle_lock_wait_max_us={} "
                 "handle_lock_hold_avg_us={} handle_lock_hold_max_us={} handle_push_avg_us={} "
                 "handle_push_max_us={} handle_queue_depth_max={} "
                 "handle_collect_count={} handle_collect_events={} "
                 "handle_collect_lock_wait_avg_us={} handle_collect_lock_wait_max_us={} "
                 "handle_collect_lock_hold_avg_us={} handle_collect_lock_hold_max_us={} "
                 "handle_lane_registry_lock_wait_avg_us={} handle_lane_registry_lock_wait_max_us={} "
                 "handle_lane_registry_lock_hold_avg_us={} handle_lane_registry_lock_hold_max_us={} "
                 "journal_append_count={} "
                 "journal_payload_bytes={} journal_lock_wait_avg_us={} journal_lock_wait_max_us={} "
                 "journal_write_avg_us={} journal_write_max_us={} journal_fdatasync_avg_us={} "
                 "journal_fdatasync_max_us={} "
                 "journal_fdatasync_le_100us_count={} journal_fdatasync_le_250us_count={} "
                 "journal_fdatasync_le_500us_count={} journal_fdatasync_le_1000us_count={} "
                 "journal_fdatasync_le_2500us_count={} journal_fdatasync_le_5000us_count={} "
                 "journal_fdatasync_le_10000us_count={} journal_fdatasync_gt_10000us_count={} "
                 "journal_publish_avg_us={} journal_publish_max_us={} "
                 "journal_admission_batch_count={} journal_admission_event_count={} "
                 "journal_admission_payload_bytes={} journal_admission_avg_events={} "
                 "journal_admission_max_events={} journal_admission_avg_shard_groups={} "
                 "journal_admission_max_shard_groups={} journal_admission_work_build_avg_us={} "
                 "journal_admission_work_build_max_us={} journal_admission_enqueue_avg_us={} "
                 "journal_admission_enqueue_max_us={} "
                 "journal_owner_count={} journal_owner_queue_wait_avg_us={} "
                 "journal_owner_queue_wait_max_us={} journal_owner_service_avg_us={} "
                 "journal_owner_service_max_us={} journal_owner_pending_flush_count={} "
                 "journal_owner_pending_flush_wait_avg_us={} journal_owner_pending_flush_wait_max_us={} "
                 "journal_owner_flush_count={} journal_owner_flush_records={} "
                 "journal_owner_flush_avg_records={} journal_owner_flush_max_records={} "
                 "journal_owner_flush_wall_avg_us={} journal_owner_flush_wall_max_us={} "
                 "journal_owner_flush_fdatasync_avg_us={} journal_owner_flush_fdatasync_max_us={} "
                 "journal_owner_flush_publish_avg_us={} journal_owner_flush_publish_max_us={} "
                 "journal_owner_flush_early_completion_avg_us={} "
                 "journal_owner_flush_early_completion_max_us={} "
                 "journal_owner_flush_event_limit_count={} journal_owner_flush_byte_limit_count={} "
                 "journal_owner_flush_queue_boundary_count={} journal_owner_flush_stop_sync_count={} "
                 "journal_owner_flush_unknown_count={} "
                 "journal_owner_flush_active_admission_count={} "
                 "journal_owner_flush_active_admission_records={} "
                 "journal_owner_flush_active_admission_max={} "
                 "journal_owner_flush_queue_boundary_active_admission_count={} "
                 "journal_owner_queue_boundary_wait_count={} "
                 "journal_owner_queue_boundary_wait_avg_us={} "
                 "journal_owner_queue_boundary_wait_max_us={} "
                 "journal_owner_queue_boundary_wait_woke_for_work_count={} "
                 "journal_owner_queue_boundary_wait_timeout_count={} "
                 "journal_owner_queue_boundary_admission_wait_count={} "
                 "journal_owner_queue_boundary_admission_wait_avg_us={} "
                 "journal_owner_queue_boundary_admission_wait_max_us={} "
                 "journal_owner_queue_boundary_admission_wait_woke_for_work_count={} "
                 "journal_owner_queue_boundary_admission_wait_drained_count={} "
                 "journal_completion_dispatch_count={} journal_completion_dispatch_records={} "
                 "journal_completion_dispatch_avg_records={} journal_completion_dispatch_max_records={} "
                 "journal_completion_dispatch_avg_us={} journal_completion_dispatch_max_us={} "
                 "journal_batch_completion_count={} journal_batch_completion_records={} "
                 "journal_batch_completion_avg_records={} journal_batch_completion_max_records={} "
                 "journal_batch_completion_final_callback_count={} "
                 "journal_batch_completion_avg_remaining_before={} "
                 "journal_batch_completion_max_remaining_before={} "
                 "journal_batch_completion_eq_1_count={} journal_batch_completion_le_2_count={} "
                 "journal_batch_completion_le_4_count={} journal_batch_completion_le_8_count={} "
                 "journal_batch_completion_le_16_count={} journal_batch_completion_le_32_count={} "
                 "journal_batch_completion_le_64_count={} journal_batch_completion_gt_64_count={} "
                 "journal_owner_service_le_100us_count={} journal_owner_service_le_250us_count={} "
                 "journal_owner_service_le_500us_count={} journal_owner_service_le_1000us_count={} "
                 "journal_owner_service_le_2500us_count={} journal_owner_service_le_5000us_count={} "
                 "journal_owner_service_le_10000us_count={} journal_owner_service_gt_10000us_count={} "
                 "journal_owner_publish_lock_wait_avg_us={} "
                 "journal_owner_publish_lock_wait_max_us={} journal_owner_batch_count={} "
                 "journal_owner_queue_wait_le_100us_count={} journal_owner_queue_wait_le_250us_count={} "
                 "journal_owner_queue_wait_le_500us_count={} journal_owner_queue_wait_le_1000us_count={} "
                 "journal_owner_queue_wait_le_2500us_count={} journal_owner_queue_wait_le_5000us_count={} "
                 "journal_owner_queue_wait_le_10000us_count={} journal_owner_queue_wait_gt_10000us_count={} "
                 "journal_owner_batch_avg_records={} journal_owner_batch_max_records={} "
                 "journal_owner_batch_eq_1_count={} journal_owner_batch_le_2_count={} "
                 "journal_owner_batch_le_4_count={} journal_owner_batch_le_8_count={} "
                 "journal_owner_batch_le_16_count={} journal_owner_batch_le_32_count={} "
                 "journal_owner_batch_le_64_count={} journal_owner_batch_gt_64_count={} "
                 "journal_owner_group_commit_count={} journal_owner_group_commit_avg_records={} "
                 "journal_owner_group_commit_max_records={} "
                 "journal_owner_group_commit_eq_1_count={} journal_owner_group_commit_le_2_count={} "
                 "journal_owner_group_commit_le_4_count={} journal_owner_group_commit_le_8_count={} "
                 "journal_owner_group_commit_le_16_count={} journal_owner_group_commit_le_32_count={} "
                 "journal_owner_group_commit_le_64_count={} journal_owner_group_commit_gt_64_count={} "
                 "journal_owner_queue_enqueue_count={} journal_owner_queue_enqueue_avg_records={} "
                 "journal_owner_queue_enqueue_lock_wait_avg_us={} "
                 "journal_owner_queue_enqueue_lock_wait_max_us={} "
                 "journal_owner_queue_enqueue_lock_hold_avg_us={} "
                 "journal_owner_queue_enqueue_lock_hold_max_us={} "
                 "journal_owner_queue_drain_count={} journal_owner_queue_drain_avg_records={} "
                 "journal_owner_queue_drain_lock_wait_avg_us={} "
                 "journal_owner_queue_drain_lock_wait_max_us={} "
                 "journal_owner_queue_drain_lock_hold_avg_us={} "
                 "journal_owner_queue_drain_lock_hold_max_us={} "
                 "journal_owner_queue_depth_max={} "
                 "journal_group_sync_count={} journal_group_sync_records={} "
                 "journal_group_sync_avg_us={} journal_group_sync_max_us={} "
                 "journal_group_sync_avg_records={} journal_group_sync_max_records={} "
                 "journal_group_sync_le_100us_count={} journal_group_sync_le_250us_count={} "
                 "journal_group_sync_le_500us_count={} journal_group_sync_le_1000us_count={} "
                 "journal_group_sync_le_2500us_count={} journal_group_sync_le_5000us_count={} "
                 "journal_group_sync_le_10000us_count={} journal_group_sync_gt_10000us_count={} "
                 "journal_group_sync_eq_1_count={} journal_group_sync_le_2_count={} "
                 "journal_group_sync_le_4_count={} journal_group_sync_le_8_count={} "
                 "journal_group_sync_le_16_count={} journal_group_sync_le_32_count={} "
                 "journal_group_sync_le_64_count={} journal_group_sync_gt_64_count={} "
                 "journal_durable_publish_count={} journal_durable_publish_batch_count={} "
                 "journal_durable_publish_batch_avg_records={} journal_durable_publish_batch_max_records={} "
                 "journal_durable_publish_batch_eq_1_count={} journal_durable_publish_batch_le_2_count={} "
                 "journal_durable_publish_batch_le_4_count={} journal_durable_publish_batch_le_8_count={} "
                 "journal_durable_publish_batch_le_16_count={} journal_durable_publish_batch_le_32_count={} "
                 "journal_durable_publish_batch_le_64_count={} journal_durable_publish_batch_gt_64_count={} "
                 "journal_tail_seal_count={} journal_tail_seal_records={} "
                 "journal_tail_seal_avg_records={} journal_tail_seal_max_records={} "
                 "journal_tail_sealed_page_max={} "
                 "journal_callback_dispatch_enqueue_count={} "
                 "journal_callback_dispatch_enqueue_lock_wait_avg_us={} "
                 "journal_callback_dispatch_enqueue_lock_wait_max_us={} "
                 "journal_callback_dispatch_queue_depth_max={} "
                 "journal_callback_dispatch_count={} journal_callback_dispatch_queue_wait_avg_us={} "
                 "journal_callback_dispatch_queue_wait_max_us={} journal_callback_dispatch_service_avg_us={} "
                 "journal_callback_dispatch_service_max_us={} "
                 "journal_callback_dispatch_queue_wait_le_100us_count={} "
                 "journal_callback_dispatch_queue_wait_le_250us_count={} "
                 "journal_callback_dispatch_queue_wait_le_500us_count={} "
                 "journal_callback_dispatch_queue_wait_le_1000us_count={} "
                 "journal_callback_dispatch_queue_wait_le_2500us_count={} "
                 "journal_callback_dispatch_queue_wait_le_5000us_count={} "
                 "journal_callback_dispatch_queue_wait_le_10000us_count={} "
                 "journal_callback_dispatch_queue_wait_gt_10000us_count={} "
                 "journal_tail_read_count={} journal_tail_matched_events={} journal_tail_snapshot_avg_us={} "
                 "journal_tail_snapshot_max_us={} journal_tail_snapshot_lock_wait_avg_us={} "
                 "journal_tail_snapshot_lock_wait_max_us={} journal_tail_payload_read_avg_us={} "
                 "journal_tail_payload_read_max_us={} journal_tail_sort_avg_us={} journal_tail_sort_max_us={} "
                 "journal_tail_payload_alloc_avg_us={} journal_tail_payload_alloc_max_us={} "
                 "journal_tail_payload_syscall_avg_us={} journal_tail_payload_syscall_max_us={} "
                 "journal_tail_payload_memcpy_avg_us={} journal_tail_payload_memcpy_max_us={} "
                 "journal_tail_event_build_avg_us={} journal_tail_event_build_max_us={} "
                 "journal_tail_payload_read_le_100us_count={} journal_tail_payload_read_le_250us_count={} "
                 "journal_tail_payload_read_le_500us_count={} journal_tail_payload_read_le_1000us_count={} "
                 "journal_tail_payload_read_le_2500us_count={} journal_tail_payload_read_le_5000us_count={} "
                 "journal_tail_payload_read_le_10000us_count={} journal_tail_payload_read_gt_10000us_count={} "
                 "journal_tail_read_group_count={} "
                 "journal_tail_read_group_avg={} journal_tail_read_group_max={} "
                 "journal_tail_physical_read_bytes={} journal_tail_payload_copy_bytes={} "
                 "journal_tail_overread_bytes={} journal_tail_direct_read_count={} "
                 "journal_tail_coalesced_read_count={} journal_tail_vectored_read_count={} post_ack_ingest_count={} "
                 "post_ack_ingest_avg_us={} post_ack_ingest_max_us={} async_drain_enqueue_count={} "
                 "async_drain_complete_count={} async_drain_avg_us={} async_drain_max_us={} "
                 "async_drain_queue_depth_max={} wal_drain_read_count={} wal_drain_read_payload_bytes={} "
                 "wal_drain_read_avg_us={} wal_drain_read_max_us={} wal_drain_read_batch_count={} "
                 "wal_drain_read_batch_avg_records={} wal_drain_read_batch_max_records={} "
                 "wal_drain_read_physical_bytes={} wal_drain_read_gap_bytes={} "
                 "wal_drain_read_syscall_count={} wal_drain_read_syscall_avg_records={} "
                 "wal_drain_batch_wait_count={} wal_drain_batch_wait_avg_us={} "
                 "wal_drain_batch_wait_max_us={} timeline_merge_batch_count={} "
                 "timeline_merge_event_count={} timeline_merge_inserted_count={} "
                 "timeline_merge_batch_avg_events={} timeline_merge_batch_max_events={} "
                 "timeline_merge_lock_wait_avg_us={} timeline_merge_lock_wait_max_us={} "
                 "timeline_merge_lock_hold_avg_us={} timeline_merge_lock_hold_max_us={} "
                 "timeline_merge_insert_avg_us={} timeline_merge_insert_max_us={}",
                 context,
                 rpc_count,
                 recordEventPayloadBytes.load(std::memory_order_relaxed),
                 avgUs(recordEventTotalNs.load(std::memory_order_relaxed), rpc_count),
                 nsToUs(recordEventMaxNs.load(std::memory_order_relaxed)),
                 recordEventLatencyBuckets[0].load(std::memory_order_relaxed),
                 recordEventLatencyBuckets[1].load(std::memory_order_relaxed),
                 recordEventLatencyBuckets[2].load(std::memory_order_relaxed),
                 recordEventLatencyBuckets[3].load(std::memory_order_relaxed),
                 recordEventLatencyBuckets[4].load(std::memory_order_relaxed),
                 recordEventLatencyBuckets[5].load(std::memory_order_relaxed),
                 recordEventLatencyBuckets[6].load(std::memory_order_relaxed),
                 recordEventLatencyBuckets[7].load(std::memory_order_relaxed),
                 rpc_batch_phase_count,
                 avgUs(rpcBatchSubmitNs.load(std::memory_order_relaxed), rpc_batch_phase_count),
                 nsToUs(rpcBatchSubmitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(rpcBatchCompletionWaitNs.load(std::memory_order_relaxed), rpc_batch_phase_count),
                 nsToUs(rpcBatchCompletionWaitMaxNs.load(std::memory_order_relaxed)),
                 queue_count,
                 avgUs(ingestionQueueTotalNs.load(std::memory_order_relaxed), queue_count),
                 avgUs(ingestionQueueLookupNs.load(std::memory_order_relaxed), queue_count),
                 nsToUs(ingestionQueueMaxNs.load(std::memory_order_relaxed)),
                 orphanEventCount.load(std::memory_order_relaxed),
                 handle_count,
                 avgUs(handleLockWaitNs.load(std::memory_order_relaxed), handle_count),
                 nsToUs(handleLockWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(handleLockHoldNs.load(std::memory_order_relaxed), handle_count),
                 nsToUs(handleLockHoldMaxNs.load(std::memory_order_relaxed)),
                 avgUs(handlePushNs.load(std::memory_order_relaxed), handle_count),
                 nsToUs(handlePushMaxNs.load(std::memory_order_relaxed)),
                 handleQueueDepthMax.load(std::memory_order_relaxed),
                 handle_collect_count,
                 handleCollectEvents.load(std::memory_order_relaxed),
                 avgUs(handleCollectLockWaitNs.load(std::memory_order_relaxed), handle_collect_count),
                 nsToUs(handleCollectLockWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(handleCollectLockHoldNs.load(std::memory_order_relaxed), handle_collect_count),
                 nsToUs(handleCollectLockHoldMaxNs.load(std::memory_order_relaxed)),
                 avgUs(handleLaneRegistryLockWaitNs.load(std::memory_order_relaxed), handle_collect_count),
                 nsToUs(handleLaneRegistryLockWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(handleLaneRegistryLockHoldNs.load(std::memory_order_relaxed), handle_collect_count),
                 nsToUs(handleLaneRegistryLockHoldMaxNs.load(std::memory_order_relaxed)),
                 journal_count,
                 journalAppendPayloadBytes.load(std::memory_order_relaxed),
                 avgUs(journalLockWaitNs.load(std::memory_order_relaxed), journal_count),
                 nsToUs(journalLockWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalWriteNs.load(std::memory_order_relaxed), journal_count),
                 nsToUs(journalWriteMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalFdSyncNs.load(std::memory_order_relaxed), journal_count),
                 nsToUs(journalFdSyncMaxNs.load(std::memory_order_relaxed)),
                 journalFdSyncBuckets[0].load(std::memory_order_relaxed),
                 journalFdSyncBuckets[1].load(std::memory_order_relaxed),
                 journalFdSyncBuckets[2].load(std::memory_order_relaxed),
                 journalFdSyncBuckets[3].load(std::memory_order_relaxed),
                 journalFdSyncBuckets[4].load(std::memory_order_relaxed),
                 journalFdSyncBuckets[5].load(std::memory_order_relaxed),
                 journalFdSyncBuckets[6].load(std::memory_order_relaxed),
                 journalFdSyncBuckets[7].load(std::memory_order_relaxed),
                 avgUs(journalPublishNs.load(std::memory_order_relaxed), journal_count),
                 nsToUs(journalPublishMaxNs.load(std::memory_order_relaxed)),
                 journal_admission_batch_count,
                 journalAdmissionEventCount.load(std::memory_order_relaxed),
                 journalAdmissionPayloadBytes.load(std::memory_order_relaxed),
                 avgDouble(journalAdmissionEventCount.load(std::memory_order_relaxed),
                           journal_admission_batch_count),
                 journalAdmissionMaxEvents.load(std::memory_order_relaxed),
                 avgDouble(journalAdmissionShardGroupCount.load(std::memory_order_relaxed),
                           journal_admission_batch_count),
                 journalAdmissionMaxShardGroups.load(std::memory_order_relaxed),
                 avgUs(journalAdmissionWorkBuildNs.load(std::memory_order_relaxed),
                       journal_admission_batch_count),
                 nsToUs(journalAdmissionWorkBuildMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalAdmissionEnqueueNs.load(std::memory_order_relaxed),
                       journal_admission_batch_count),
                 nsToUs(journalAdmissionEnqueueMaxNs.load(std::memory_order_relaxed)),
                 journal_owner_count,
                 avgUs(journalOwnerQueueWaitNs.load(std::memory_order_relaxed), journal_owner_count),
                 nsToUs(journalOwnerQueueWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalOwnerServiceNs.load(std::memory_order_relaxed), journal_owner_count),
                 nsToUs(journalOwnerServiceMaxNs.load(std::memory_order_relaxed)),
                 journal_owner_pending_flush_count,
                 avgUs(journalOwnerPendingFlushNs.load(std::memory_order_relaxed),
                       journal_owner_pending_flush_count),
                 nsToUs(journalOwnerPendingFlushMaxNs.load(std::memory_order_relaxed)),
                 journal_owner_flush_count,
                 journalOwnerFlushRecords.load(std::memory_order_relaxed),
                 avgDouble(journalOwnerFlushRecords.load(std::memory_order_relaxed), journal_owner_flush_count),
                 journalOwnerFlushMaxRecords.load(std::memory_order_relaxed),
                 avgUs(journalOwnerFlushWallNs.load(std::memory_order_relaxed), journal_owner_flush_count),
                 nsToUs(journalOwnerFlushWallMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalOwnerFlushFdSyncNs.load(std::memory_order_relaxed), journal_owner_flush_count),
                 nsToUs(journalOwnerFlushFdSyncMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalOwnerFlushPublishNs.load(std::memory_order_relaxed), journal_owner_flush_count),
                 nsToUs(journalOwnerFlushPublishMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalOwnerFlushEarlyCompletionNs.load(std::memory_order_relaxed),
                       journal_owner_flush_count),
                 nsToUs(journalOwnerFlushEarlyCompletionMaxNs.load(std::memory_order_relaxed)),
                 journalOwnerFlushEventLimitCount.load(std::memory_order_relaxed),
                 journalOwnerFlushByteLimitCount.load(std::memory_order_relaxed),
                 journalOwnerFlushQueueBoundaryCount.load(std::memory_order_relaxed),
                 journalOwnerFlushStopSyncCount.load(std::memory_order_relaxed),
                 journalOwnerFlushUnknownCount.load(std::memory_order_relaxed),
                 journalOwnerFlushActiveAdmissionCount.load(std::memory_order_relaxed),
                 journalOwnerFlushActiveAdmissionRecords.load(std::memory_order_relaxed),
                 journalOwnerFlushActiveAdmissionMax.load(std::memory_order_relaxed),
                 journalOwnerFlushQueueBoundaryActiveAdmissionCount.load(std::memory_order_relaxed),
                 journalOwnerQueueBoundaryWaitCount.load(std::memory_order_relaxed),
                 avgUs(journalOwnerQueueBoundaryWaitNs.load(std::memory_order_relaxed),
                       journalOwnerQueueBoundaryWaitCount.load(std::memory_order_relaxed)),
                 nsToUs(journalOwnerQueueBoundaryWaitMaxNs.load(std::memory_order_relaxed)),
                 journalOwnerQueueBoundaryWaitWokeForWorkCount.load(std::memory_order_relaxed),
                 journalOwnerQueueBoundaryWaitTimeoutCount.load(std::memory_order_relaxed),
                 journalOwnerQueueBoundaryAdmissionWaitCount.load(std::memory_order_relaxed),
                 avgUs(journalOwnerQueueBoundaryAdmissionWaitNs.load(std::memory_order_relaxed),
                       journalOwnerQueueBoundaryAdmissionWaitCount.load(std::memory_order_relaxed)),
                 nsToUs(journalOwnerQueueBoundaryAdmissionWaitMaxNs.load(std::memory_order_relaxed)),
                 journalOwnerQueueBoundaryAdmissionWaitWokeForWorkCount.load(std::memory_order_relaxed),
                 journalOwnerQueueBoundaryAdmissionWaitDrainedCount.load(std::memory_order_relaxed),
                 journal_completion_dispatch_count,
                 journalCompletionDispatchRecords.load(std::memory_order_relaxed),
                 avgDouble(journalCompletionDispatchRecords.load(std::memory_order_relaxed),
                           journal_completion_dispatch_count),
                 journalCompletionDispatchMaxRecords.load(std::memory_order_relaxed),
                 avgUs(journalCompletionDispatchNs.load(std::memory_order_relaxed),
                       journal_completion_dispatch_count),
                 nsToUs(journalCompletionDispatchMaxNs.load(std::memory_order_relaxed)),
                 journal_batch_completion_count,
                 journalBatchCompletionRecords.load(std::memory_order_relaxed),
                 avgDouble(journalBatchCompletionRecords.load(std::memory_order_relaxed),
                           journal_batch_completion_count),
                 journalBatchCompletionMaxRecords.load(std::memory_order_relaxed),
                 journalBatchCompletionFinalCallbackCount.load(std::memory_order_relaxed),
                 avgDouble(journalBatchCompletionRemainingBefore.load(std::memory_order_relaxed),
                           journal_batch_completion_count),
                 journalBatchCompletionMaxRemainingBefore.load(std::memory_order_relaxed),
                 journalBatchCompletionSizeBuckets[0].load(std::memory_order_relaxed),
                 journalBatchCompletionSizeBuckets[1].load(std::memory_order_relaxed),
                 journalBatchCompletionSizeBuckets[2].load(std::memory_order_relaxed),
                 journalBatchCompletionSizeBuckets[3].load(std::memory_order_relaxed),
                 journalBatchCompletionSizeBuckets[4].load(std::memory_order_relaxed),
                 journalBatchCompletionSizeBuckets[5].load(std::memory_order_relaxed),
                 journalBatchCompletionSizeBuckets[6].load(std::memory_order_relaxed),
                 journalBatchCompletionSizeBuckets[7].load(std::memory_order_relaxed),
                 journalOwnerServiceBuckets[0].load(std::memory_order_relaxed),
                 journalOwnerServiceBuckets[1].load(std::memory_order_relaxed),
                 journalOwnerServiceBuckets[2].load(std::memory_order_relaxed),
                 journalOwnerServiceBuckets[3].load(std::memory_order_relaxed),
                 journalOwnerServiceBuckets[4].load(std::memory_order_relaxed),
                 journalOwnerServiceBuckets[5].load(std::memory_order_relaxed),
                 journalOwnerServiceBuckets[6].load(std::memory_order_relaxed),
                 journalOwnerServiceBuckets[7].load(std::memory_order_relaxed),
                 avgUs(journalOwnerPublishLockWaitNs.load(std::memory_order_relaxed), journal_owner_count),
                 nsToUs(journalOwnerPublishLockWaitMaxNs.load(std::memory_order_relaxed)),
                 journal_owner_batch_count,
                 journalOwnerQueueWaitBuckets[0].load(std::memory_order_relaxed),
                 journalOwnerQueueWaitBuckets[1].load(std::memory_order_relaxed),
                 journalOwnerQueueWaitBuckets[2].load(std::memory_order_relaxed),
                 journalOwnerQueueWaitBuckets[3].load(std::memory_order_relaxed),
                 journalOwnerQueueWaitBuckets[4].load(std::memory_order_relaxed),
                 journalOwnerQueueWaitBuckets[5].load(std::memory_order_relaxed),
                 journalOwnerQueueWaitBuckets[6].load(std::memory_order_relaxed),
                 journalOwnerQueueWaitBuckets[7].load(std::memory_order_relaxed),
                 avgDouble(journalOwnerBatchRecords.load(std::memory_order_relaxed), journal_owner_batch_count),
                 journalOwnerBatchMaxRecords.load(std::memory_order_relaxed),
                 journalOwnerBatchSizeBuckets[0].load(std::memory_order_relaxed),
                 journalOwnerBatchSizeBuckets[1].load(std::memory_order_relaxed),
                 journalOwnerBatchSizeBuckets[2].load(std::memory_order_relaxed),
                 journalOwnerBatchSizeBuckets[3].load(std::memory_order_relaxed),
                 journalOwnerBatchSizeBuckets[4].load(std::memory_order_relaxed),
                 journalOwnerBatchSizeBuckets[5].load(std::memory_order_relaxed),
                 journalOwnerBatchSizeBuckets[6].load(std::memory_order_relaxed),
                 journalOwnerBatchSizeBuckets[7].load(std::memory_order_relaxed),
                 journal_owner_group_commit_count,
                 avgDouble(journalOwnerGroupCommitRecords.load(std::memory_order_relaxed),
                           journal_owner_group_commit_count),
                 journalOwnerGroupCommitMaxRecords.load(std::memory_order_relaxed),
                 journalOwnerGroupCommitSizeBuckets[0].load(std::memory_order_relaxed),
                 journalOwnerGroupCommitSizeBuckets[1].load(std::memory_order_relaxed),
                 journalOwnerGroupCommitSizeBuckets[2].load(std::memory_order_relaxed),
                 journalOwnerGroupCommitSizeBuckets[3].load(std::memory_order_relaxed),
                 journalOwnerGroupCommitSizeBuckets[4].load(std::memory_order_relaxed),
                 journalOwnerGroupCommitSizeBuckets[5].load(std::memory_order_relaxed),
                 journalOwnerGroupCommitSizeBuckets[6].load(std::memory_order_relaxed),
                 journalOwnerGroupCommitSizeBuckets[7].load(std::memory_order_relaxed),
                 journal_owner_queue_enqueue_count,
                 avgDouble(journalOwnerQueueEnqueueRecords.load(std::memory_order_relaxed),
                           journal_owner_queue_enqueue_count),
                 avgUs(journalOwnerQueueEnqueueLockWaitNs.load(std::memory_order_relaxed),
                       journal_owner_queue_enqueue_count),
                 nsToUs(journalOwnerQueueEnqueueLockWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalOwnerQueueEnqueueLockHoldNs.load(std::memory_order_relaxed),
                       journal_owner_queue_enqueue_count),
                 nsToUs(journalOwnerQueueEnqueueLockHoldMaxNs.load(std::memory_order_relaxed)),
                 journal_owner_queue_drain_count,
                 avgDouble(journalOwnerQueueDrainRecords.load(std::memory_order_relaxed),
                           journal_owner_queue_drain_count),
                 avgUs(journalOwnerQueueDrainLockWaitNs.load(std::memory_order_relaxed),
                       journal_owner_queue_drain_count),
                 nsToUs(journalOwnerQueueDrainLockWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalOwnerQueueDrainLockHoldNs.load(std::memory_order_relaxed),
                       journal_owner_queue_drain_count),
                 nsToUs(journalOwnerQueueDrainLockHoldMaxNs.load(std::memory_order_relaxed)),
                 journalOwnerQueueDepthMax.load(std::memory_order_relaxed),
                 journal_group_sync_count,
                 journalGroupSyncRecords.load(std::memory_order_relaxed),
                 avgUs(journalGroupSyncNs.load(std::memory_order_relaxed), journal_group_sync_count),
                 nsToUs(journalGroupSyncMaxNs.load(std::memory_order_relaxed)),
                 avgDouble(journalGroupSyncRecords.load(std::memory_order_relaxed), journal_group_sync_count),
                 journalGroupSyncMaxRecords.load(std::memory_order_relaxed),
                 journalGroupSyncBuckets[0].load(std::memory_order_relaxed),
                 journalGroupSyncBuckets[1].load(std::memory_order_relaxed),
                 journalGroupSyncBuckets[2].load(std::memory_order_relaxed),
                 journalGroupSyncBuckets[3].load(std::memory_order_relaxed),
                 journalGroupSyncBuckets[4].load(std::memory_order_relaxed),
                 journalGroupSyncBuckets[5].load(std::memory_order_relaxed),
                 journalGroupSyncBuckets[6].load(std::memory_order_relaxed),
                 journalGroupSyncBuckets[7].load(std::memory_order_relaxed),
                 journalGroupSyncSizeBuckets[0].load(std::memory_order_relaxed),
                 journalGroupSyncSizeBuckets[1].load(std::memory_order_relaxed),
                 journalGroupSyncSizeBuckets[2].load(std::memory_order_relaxed),
                 journalGroupSyncSizeBuckets[3].load(std::memory_order_relaxed),
                 journalGroupSyncSizeBuckets[4].load(std::memory_order_relaxed),
                 journalGroupSyncSizeBuckets[5].load(std::memory_order_relaxed),
                 journalGroupSyncSizeBuckets[6].load(std::memory_order_relaxed),
                 journalGroupSyncSizeBuckets[7].load(std::memory_order_relaxed),
                 journalDurablePublishCount.load(std::memory_order_relaxed),
                 journal_durable_publish_batch_count,
                 avgDouble(journalDurablePublishBatchRecords.load(std::memory_order_relaxed),
                           journal_durable_publish_batch_count),
                 journalDurablePublishBatchMaxRecords.load(std::memory_order_relaxed),
                 journalDurablePublishBatchSizeBuckets[0].load(std::memory_order_relaxed),
                 journalDurablePublishBatchSizeBuckets[1].load(std::memory_order_relaxed),
                 journalDurablePublishBatchSizeBuckets[2].load(std::memory_order_relaxed),
                 journalDurablePublishBatchSizeBuckets[3].load(std::memory_order_relaxed),
                 journalDurablePublishBatchSizeBuckets[4].load(std::memory_order_relaxed),
                 journalDurablePublishBatchSizeBuckets[5].load(std::memory_order_relaxed),
                 journalDurablePublishBatchSizeBuckets[6].load(std::memory_order_relaxed),
                 journalDurablePublishBatchSizeBuckets[7].load(std::memory_order_relaxed),
                 journalTailSealCount.load(std::memory_order_relaxed),
                 journalTailSealRecords.load(std::memory_order_relaxed),
                 avgDouble(journalTailSealRecords.load(std::memory_order_relaxed),
                           journalTailSealCount.load(std::memory_order_relaxed)),
                 journalTailSealMaxRecords.load(std::memory_order_relaxed),
                 journalTailSealedPageMax.load(std::memory_order_relaxed),
                 journalCallbackDispatchEnqueueCount.load(std::memory_order_relaxed),
                 avgUs(journalCallbackDispatchEnqueueLockWaitNs.load(std::memory_order_relaxed),
                       journalCallbackDispatchEnqueueCount.load(std::memory_order_relaxed)),
                 nsToUs(journalCallbackDispatchEnqueueLockWaitMaxNs.load(std::memory_order_relaxed)),
                 journalCallbackDispatchQueueDepthMax.load(std::memory_order_relaxed),
                 callback_dispatch_count,
                 avgUs(journalCallbackDispatchQueueWaitNs.load(std::memory_order_relaxed), callback_dispatch_count),
                 nsToUs(journalCallbackDispatchQueueWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalCallbackDispatchServiceNs.load(std::memory_order_relaxed), callback_dispatch_count),
                 nsToUs(journalCallbackDispatchServiceMaxNs.load(std::memory_order_relaxed)),
                 journalCallbackDispatchQueueWaitBuckets[0].load(std::memory_order_relaxed),
                 journalCallbackDispatchQueueWaitBuckets[1].load(std::memory_order_relaxed),
                 journalCallbackDispatchQueueWaitBuckets[2].load(std::memory_order_relaxed),
                 journalCallbackDispatchQueueWaitBuckets[3].load(std::memory_order_relaxed),
                 journalCallbackDispatchQueueWaitBuckets[4].load(std::memory_order_relaxed),
                 journalCallbackDispatchQueueWaitBuckets[5].load(std::memory_order_relaxed),
                 journalCallbackDispatchQueueWaitBuckets[6].load(std::memory_order_relaxed),
                 journalCallbackDispatchQueueWaitBuckets[7].load(std::memory_order_relaxed),
                 journal_tail_count,
                 journalTailMatchedEvents.load(std::memory_order_relaxed),
                 avgUs(journalTailSnapshotNs.load(std::memory_order_relaxed), journal_tail_count),
                 nsToUs(journalTailSnapshotMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalTailSnapshotLockWaitNs.load(std::memory_order_relaxed), journal_tail_count),
                 nsToUs(journalTailSnapshotLockWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalTailPayloadReadNs.load(std::memory_order_relaxed), journal_tail_count),
                 nsToUs(journalTailPayloadReadMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalTailSortNs.load(std::memory_order_relaxed), journal_tail_count),
                 nsToUs(journalTailSortMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalTailPayloadAllocNs.load(std::memory_order_relaxed), journal_tail_count),
                 nsToUs(journalTailPayloadAllocMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalTailPayloadSyscallNs.load(std::memory_order_relaxed), journal_tail_count),
                 nsToUs(journalTailPayloadSyscallMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalTailPayloadMemcpyNs.load(std::memory_order_relaxed), journal_tail_count),
                 nsToUs(journalTailPayloadMemcpyMaxNs.load(std::memory_order_relaxed)),
                 avgUs(journalTailEventBuildNs.load(std::memory_order_relaxed), journal_tail_count),
                 nsToUs(journalTailEventBuildMaxNs.load(std::memory_order_relaxed)),
                 journalTailPayloadReadBuckets[0].load(std::memory_order_relaxed),
                 journalTailPayloadReadBuckets[1].load(std::memory_order_relaxed),
                 journalTailPayloadReadBuckets[2].load(std::memory_order_relaxed),
                 journalTailPayloadReadBuckets[3].load(std::memory_order_relaxed),
                 journalTailPayloadReadBuckets[4].load(std::memory_order_relaxed),
                 journalTailPayloadReadBuckets[5].load(std::memory_order_relaxed),
                 journalTailPayloadReadBuckets[6].load(std::memory_order_relaxed),
                 journalTailPayloadReadBuckets[7].load(std::memory_order_relaxed),
                 journalTailReadGroupCount.load(std::memory_order_relaxed),
                 avgDouble(journalTailReadGroupCount.load(std::memory_order_relaxed), journal_tail_count),
                 journalTailReadGroupMax.load(std::memory_order_relaxed),
                 journalTailPhysicalReadBytes.load(std::memory_order_relaxed),
                 journalTailPayloadCopyBytes.load(std::memory_order_relaxed),
                 overreadBytes(),
                 journalTailDirectReadCount.load(std::memory_order_relaxed),
                 journalTailCoalescedReadCount.load(std::memory_order_relaxed),
                 journalTailVectoredReadCount.load(std::memory_order_relaxed),
                 post_ack_ingest_count,
                 avgUs(postAckIngestNs.load(std::memory_order_relaxed), post_ack_ingest_count),
                 nsToUs(postAckIngestMaxNs.load(std::memory_order_relaxed)),
                 asyncDrainEnqueueCount.load(std::memory_order_relaxed),
                 async_drain_complete_count,
                 avgUs(asyncDrainNs.load(std::memory_order_relaxed), async_drain_complete_count),
                 nsToUs(asyncDrainMaxNs.load(std::memory_order_relaxed)),
                 asyncDrainQueueDepthMax.load(std::memory_order_relaxed),
                 wal_drain_read_count,
                 walDrainReadPayloadBytes.load(std::memory_order_relaxed),
                 avgUs(walDrainReadNs.load(std::memory_order_relaxed), wal_drain_read_count),
                 nsToUs(walDrainReadMaxNs.load(std::memory_order_relaxed)),
                 wal_drain_batch_count,
                 avgDouble(wal_drain_read_count, wal_drain_batch_count),
                 walDrainReadBatchMaxRecords.load(std::memory_order_relaxed),
                 walDrainReadPhysicalBytes.load(std::memory_order_relaxed),
                 walDrainGapBytes(),
                 wal_drain_syscall_count,
                 avgDouble(wal_drain_read_count, wal_drain_syscall_count),
                 walDrainBatchWaitCount.load(std::memory_order_relaxed),
                 avgUs(walDrainBatchWaitNs.load(std::memory_order_relaxed),
                       walDrainBatchWaitCount.load(std::memory_order_relaxed)),
                 nsToUs(walDrainBatchWaitMaxNs.load(std::memory_order_relaxed)),
                 timeline_merge_batch_count,
                 timeline_merge_event_count,
                 timelineMergeInsertedCount.load(std::memory_order_relaxed),
                 avgDouble(timeline_merge_event_count, timeline_merge_batch_count),
                 timelineMergeBatchMaxEvents.load(std::memory_order_relaxed),
                 avgUs(timelineMergeLockWaitNs.load(std::memory_order_relaxed), timeline_merge_batch_count),
                 nsToUs(timelineMergeLockWaitMaxNs.load(std::memory_order_relaxed)),
                 avgUs(timelineMergeLockHoldNs.load(std::memory_order_relaxed), timeline_merge_batch_count),
                 nsToUs(timelineMergeLockHoldMaxNs.load(std::memory_order_relaxed)),
                 avgUs(timelineMergeInsertNs.load(std::memory_order_relaxed), timeline_merge_event_count),
                 nsToUs(timelineMergeInsertMaxNs.load(std::memory_order_relaxed)));
        chrono_monitor::flush();
    }

private:
    KeeperAppendStats() = default;

    static double nsToUs(uint64_t ns) { return static_cast<double>(ns) / 1000.0; }

    static double avgUs(uint64_t total_ns, uint64_t count)
    {
        return count == 0 ? 0.0 : nsToUs(total_ns) / static_cast<double>(count);
    }

    static double avgDouble(uint64_t total, uint64_t count)
    {
        return count == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(count);
    }

    static uint64_t statsIntervalEvents()
    {
        static uint64_t interval = []() {
            char const* value = std::getenv("CHRONOLOG_KEEPER_APPEND_STATS_INTERVAL_EVENTS");
            if(value == nullptr || *value == '\0')
            {
                return uint64_t{10000};
            }
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(value, &end, 10);
            if(end == value)
            {
                return uint64_t{10000};
            }
            return static_cast<uint64_t>(parsed);
        }();
        return interval;
    }

    uint64_t overreadBytes() const
    {
        uint64_t const physical = journalTailPhysicalReadBytes.load(std::memory_order_relaxed);
        uint64_t const payload = journalTailPayloadCopyBytes.load(std::memory_order_relaxed);
        return physical > payload ? physical - payload : 0;
    }

    uint64_t walDrainGapBytes() const
    {
        uint64_t const physical = walDrainReadPhysicalBytes.load(std::memory_order_relaxed);
        uint64_t const payload = walDrainReadPayloadBytes.load(std::memory_order_relaxed);
        return physical > payload ? physical - payload : 0;
    }

    static void updateMax(std::atomic<uint64_t>& target, uint64_t value)
    {
        uint64_t current = target.load(std::memory_order_relaxed);
        while(current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
        {}
    }

    static std::size_t latencyBucketIndex(uint64_t ns)
    {
        uint64_t const us = ns / 1000;
        if(us <= 100)
        {
            return 0;
        }
        if(us <= 250)
        {
            return 1;
        }
        if(us <= 500)
        {
            return 2;
        }
        if(us <= 1000)
        {
            return 3;
        }
        if(us <= 2500)
        {
            return 4;
        }
        if(us <= 5000)
        {
            return 5;
        }
        if(us <= 10000)
        {
            return 6;
        }
        return 7;
    }

    static void recordLatencyBucket(std::array<std::atomic<uint64_t>, 8>& buckets,
                                    uint64_t ns,
                                    uint64_t count = 1)
    {
        buckets[latencyBucketIndex(ns)].fetch_add(count, std::memory_order_relaxed);
    }

    static std::size_t batchSizeBucketIndex(uint64_t count)
    {
        if(count <= 1)
        {
            return 0;
        }
        if(count <= 2)
        {
            return 1;
        }
        if(count <= 4)
        {
            return 2;
        }
        if(count <= 8)
        {
            return 3;
        }
        if(count <= 16)
        {
            return 4;
        }
        if(count <= 32)
        {
            return 5;
        }
        if(count <= 64)
        {
            return 6;
        }
        return 7;
    }

    static void recordBatchSizeBucket(std::array<std::atomic<uint64_t>, 8>& buckets, uint64_t count)
    {
        buckets[batchSizeBucketIndex(count)].fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<uint64_t> recordEventCount{0};
    std::atomic<uint64_t> recordEventPayloadBytes{0};
    std::atomic<uint64_t> recordEventTotalNs{0};
    std::atomic<uint64_t> recordEventMaxNs{0};
    std::array<std::atomic<uint64_t>, 8> recordEventLatencyBuckets{};
    std::atomic<uint64_t> rpcBatchPhaseCount{0};
    std::atomic<uint64_t> rpcBatchSubmitNs{0};
    std::atomic<uint64_t> rpcBatchSubmitMaxNs{0};
    std::atomic<uint64_t> rpcBatchCompletionWaitNs{0};
    std::atomic<uint64_t> rpcBatchCompletionWaitMaxNs{0};

    std::atomic<uint64_t> ingestionQueueCount{0};
    std::atomic<uint64_t> ingestionQueueTotalNs{0};
    std::atomic<uint64_t> ingestionQueueLookupNs{0};
    std::atomic<uint64_t> ingestionQueueMaxNs{0};
    std::atomic<uint64_t> orphanEventCount{0};

    std::atomic<uint64_t> handleIngestCount{0};
    std::atomic<uint64_t> handleLockWaitNs{0};
    std::atomic<uint64_t> handleLockWaitMaxNs{0};
    std::atomic<uint64_t> handleLockHoldNs{0};
    std::atomic<uint64_t> handleLockHoldMaxNs{0};
    std::atomic<uint64_t> handlePushNs{0};
    std::atomic<uint64_t> handlePushMaxNs{0};
    std::atomic<uint64_t> handleQueueDepthMax{0};
    std::atomic<uint64_t> handleCollectCount{0};
    std::atomic<uint64_t> handleCollectEvents{0};
    std::atomic<uint64_t> handleCollectLockWaitNs{0};
    std::atomic<uint64_t> handleCollectLockWaitMaxNs{0};
    std::atomic<uint64_t> handleCollectLockHoldNs{0};
    std::atomic<uint64_t> handleCollectLockHoldMaxNs{0};
    std::atomic<uint64_t> handleLaneRegistryLockWaitNs{0};
    std::atomic<uint64_t> handleLaneRegistryLockWaitMaxNs{0};
    std::atomic<uint64_t> handleLaneRegistryLockHoldNs{0};
    std::atomic<uint64_t> handleLaneRegistryLockHoldMaxNs{0};

    std::atomic<uint64_t> journalAppendCount{0};
    std::atomic<uint64_t> journalAppendPayloadBytes{0};
    std::atomic<uint64_t> journalLockWaitNs{0};
    std::atomic<uint64_t> journalLockWaitMaxNs{0};
    std::atomic<uint64_t> journalWriteNs{0};
    std::atomic<uint64_t> journalWriteMaxNs{0};
    std::atomic<uint64_t> journalFdSyncNs{0};
    std::atomic<uint64_t> journalFdSyncMaxNs{0};
    std::array<std::atomic<uint64_t>, 8> journalFdSyncBuckets{};
    std::atomic<uint64_t> journalPublishNs{0};
    std::atomic<uint64_t> journalPublishMaxNs{0};
    std::atomic<uint64_t> journalAdmissionBatchCount{0};
    std::atomic<uint64_t> journalAdmissionEventCount{0};
    std::atomic<uint64_t> journalAdmissionPayloadBytes{0};
    std::atomic<uint64_t> journalAdmissionShardGroupCount{0};
    std::atomic<uint64_t> journalAdmissionMaxEvents{0};
    std::atomic<uint64_t> journalAdmissionMaxShardGroups{0};
    std::atomic<uint64_t> journalAdmissionWorkBuildNs{0};
    std::atomic<uint64_t> journalAdmissionWorkBuildMaxNs{0};
    std::atomic<uint64_t> journalAdmissionEnqueueNs{0};
    std::atomic<uint64_t> journalAdmissionEnqueueMaxNs{0};

    std::atomic<uint64_t> journalOwnerCount{0};
    std::atomic<uint64_t> journalOwnerQueueWaitNs{0};
    std::atomic<uint64_t> journalOwnerQueueWaitMaxNs{0};
    std::atomic<uint64_t> journalOwnerServiceNs{0};
    std::atomic<uint64_t> journalOwnerServiceMaxNs{0};
    std::array<std::atomic<uint64_t>, 8> journalOwnerServiceBuckets{};
    std::atomic<uint64_t> journalOwnerPendingFlushCount{0};
    std::atomic<uint64_t> journalOwnerPendingFlushNs{0};
    std::atomic<uint64_t> journalOwnerPendingFlushMaxNs{0};
    std::atomic<uint64_t> journalOwnerFlushCount{0};
    std::atomic<uint64_t> journalOwnerFlushRecords{0};
    std::atomic<uint64_t> journalOwnerFlushMaxRecords{0};
    std::atomic<uint64_t> journalOwnerFlushWallNs{0};
    std::atomic<uint64_t> journalOwnerFlushWallMaxNs{0};
    std::atomic<uint64_t> journalOwnerFlushFdSyncNs{0};
    std::atomic<uint64_t> journalOwnerFlushFdSyncMaxNs{0};
    std::atomic<uint64_t> journalOwnerFlushPublishNs{0};
    std::atomic<uint64_t> journalOwnerFlushPublishMaxNs{0};
    std::atomic<uint64_t> journalOwnerFlushEarlyCompletionNs{0};
    std::atomic<uint64_t> journalOwnerFlushEarlyCompletionMaxNs{0};
    std::atomic<uint64_t> journalOwnerFlushEventLimitCount{0};
    std::atomic<uint64_t> journalOwnerFlushByteLimitCount{0};
    std::atomic<uint64_t> journalOwnerFlushQueueBoundaryCount{0};
    std::atomic<uint64_t> journalOwnerFlushStopSyncCount{0};
    std::atomic<uint64_t> journalOwnerFlushUnknownCount{0};
    std::atomic<uint64_t> journalOwnerFlushActiveAdmissionCount{0};
    std::atomic<uint64_t> journalOwnerFlushActiveAdmissionRecords{0};
    std::atomic<uint64_t> journalOwnerFlushActiveAdmissionMax{0};
    std::atomic<uint64_t> journalOwnerFlushQueueBoundaryActiveAdmissionCount{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryWaitCount{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryWaitNs{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryWaitMaxNs{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryWaitWokeForWorkCount{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryWaitTimeoutCount{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryAdmissionWaitCount{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryAdmissionWaitNs{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryAdmissionWaitMaxNs{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryAdmissionWaitWokeForWorkCount{0};
    std::atomic<uint64_t> journalOwnerQueueBoundaryAdmissionWaitDrainedCount{0};
    std::atomic<uint64_t> journalCompletionDispatchCount{0};
    std::atomic<uint64_t> journalCompletionDispatchRecords{0};
    std::atomic<uint64_t> journalCompletionDispatchMaxRecords{0};
    std::atomic<uint64_t> journalCompletionDispatchNs{0};
    std::atomic<uint64_t> journalCompletionDispatchMaxNs{0};
    std::atomic<uint64_t> journalBatchCompletionCount{0};
    std::atomic<uint64_t> journalBatchCompletionRecords{0};
    std::atomic<uint64_t> journalBatchCompletionMaxRecords{0};
    std::atomic<uint64_t> journalBatchCompletionFinalCallbackCount{0};
    std::atomic<uint64_t> journalBatchCompletionRemainingBefore{0};
    std::atomic<uint64_t> journalBatchCompletionMaxRemainingBefore{0};
    std::array<std::atomic<uint64_t>, 8> journalBatchCompletionSizeBuckets{};
    std::atomic<uint64_t> journalOwnerPublishLockWaitNs{0};
    std::atomic<uint64_t> journalOwnerPublishLockWaitMaxNs{0};
    std::array<std::atomic<uint64_t>, 8> journalOwnerQueueWaitBuckets{};
    std::atomic<uint64_t> journalOwnerBatchCount{0};
    std::atomic<uint64_t> journalOwnerBatchRecords{0};
    std::atomic<uint64_t> journalOwnerBatchMaxRecords{0};
    std::array<std::atomic<uint64_t>, 8> journalOwnerBatchSizeBuckets{};
    std::atomic<uint64_t> journalOwnerGroupCommitCount{0};
    std::atomic<uint64_t> journalOwnerGroupCommitRecords{0};
    std::atomic<uint64_t> journalOwnerGroupCommitMaxRecords{0};
    std::array<std::atomic<uint64_t>, 8> journalOwnerGroupCommitSizeBuckets{};
    std::atomic<uint64_t> journalOwnerQueueEnqueueCount{0};
    std::atomic<uint64_t> journalOwnerQueueEnqueueRecords{0};
    std::atomic<uint64_t> journalOwnerQueueEnqueueLockWaitNs{0};
    std::atomic<uint64_t> journalOwnerQueueEnqueueLockWaitMaxNs{0};
    std::atomic<uint64_t> journalOwnerQueueEnqueueLockHoldNs{0};
    std::atomic<uint64_t> journalOwnerQueueEnqueueLockHoldMaxNs{0};
    std::atomic<uint64_t> journalOwnerQueueDrainCount{0};
    std::atomic<uint64_t> journalOwnerQueueDrainRecords{0};
    std::atomic<uint64_t> journalOwnerQueueDrainLockWaitNs{0};
    std::atomic<uint64_t> journalOwnerQueueDrainLockWaitMaxNs{0};
    std::atomic<uint64_t> journalOwnerQueueDrainLockHoldNs{0};
    std::atomic<uint64_t> journalOwnerQueueDrainLockHoldMaxNs{0};
    std::atomic<uint64_t> journalOwnerQueueDepthMax{0};
    std::atomic<uint64_t> journalGroupSyncCount{0};
    std::atomic<uint64_t> journalGroupSyncRecords{0};
    std::atomic<uint64_t> journalGroupSyncNs{0};
    std::atomic<uint64_t> journalGroupSyncMaxNs{0};
    std::atomic<uint64_t> journalGroupSyncMaxRecords{0};
    std::array<std::atomic<uint64_t>, 8> journalGroupSyncBuckets{};
    std::array<std::atomic<uint64_t>, 8> journalGroupSyncSizeBuckets{};
    std::atomic<uint64_t> journalDurablePublishCount{0};
    std::atomic<uint64_t> journalDurablePublishBatchCount{0};
    std::atomic<uint64_t> journalDurablePublishBatchRecords{0};
    std::atomic<uint64_t> journalDurablePublishBatchMaxRecords{0};
    std::array<std::atomic<uint64_t>, 8> journalDurablePublishBatchSizeBuckets{};
    std::atomic<uint64_t> journalTailSealCount{0};
    std::atomic<uint64_t> journalTailSealRecords{0};
    std::atomic<uint64_t> journalTailSealMaxRecords{0};
    std::atomic<uint64_t> journalTailSealedPageMax{0};

    std::atomic<uint64_t> journalCallbackDispatchEnqueueCount{0};
    std::atomic<uint64_t> journalCallbackDispatchEnqueueLockWaitNs{0};
    std::atomic<uint64_t> journalCallbackDispatchEnqueueLockWaitMaxNs{0};
    std::atomic<uint64_t> journalCallbackDispatchQueueDepthMax{0};
    std::atomic<uint64_t> journalCallbackDispatchCount{0};
    std::atomic<uint64_t> journalCallbackDispatchQueueWaitNs{0};
    std::atomic<uint64_t> journalCallbackDispatchQueueWaitMaxNs{0};
    std::atomic<uint64_t> journalCallbackDispatchServiceNs{0};
    std::atomic<uint64_t> journalCallbackDispatchServiceMaxNs{0};
    std::array<std::atomic<uint64_t>, 8> journalCallbackDispatchQueueWaitBuckets{};

    std::atomic<uint64_t> journalTailReadCount{0};
    std::atomic<uint64_t> journalTailMatchedEvents{0};
    std::atomic<uint64_t> journalTailSnapshotNs{0};
    std::atomic<uint64_t> journalTailSnapshotMaxNs{0};
    std::atomic<uint64_t> journalTailSnapshotLockWaitNs{0};
    std::atomic<uint64_t> journalTailSnapshotLockWaitMaxNs{0};
    std::atomic<uint64_t> journalTailSortNs{0};
    std::atomic<uint64_t> journalTailSortMaxNs{0};
    std::atomic<uint64_t> journalTailPayloadReadNs{0};
    std::atomic<uint64_t> journalTailPayloadReadMaxNs{0};
    std::atomic<uint64_t> journalTailPayloadAllocNs{0};
    std::atomic<uint64_t> journalTailPayloadAllocMaxNs{0};
    std::atomic<uint64_t> journalTailPayloadSyscallNs{0};
    std::atomic<uint64_t> journalTailPayloadSyscallMaxNs{0};
    std::atomic<uint64_t> journalTailPayloadMemcpyNs{0};
    std::atomic<uint64_t> journalTailPayloadMemcpyMaxNs{0};
    std::atomic<uint64_t> journalTailEventBuildNs{0};
    std::atomic<uint64_t> journalTailEventBuildMaxNs{0};
    std::array<std::atomic<uint64_t>, 8> journalTailPayloadReadBuckets{};
    std::atomic<uint64_t> journalTailReadGroupCount{0};
    std::atomic<uint64_t> journalTailReadGroupMax{0};
    std::atomic<uint64_t> journalTailPhysicalReadBytes{0};
    std::atomic<uint64_t> journalTailPayloadCopyBytes{0};
    std::atomic<uint64_t> journalTailDirectReadCount{0};
    std::atomic<uint64_t> journalTailCoalescedReadCount{0};
    std::atomic<uint64_t> journalTailVectoredReadCount{0};

    std::atomic<uint64_t> postAckIngestCount{0};
    std::atomic<uint64_t> postAckIngestNs{0};
    std::atomic<uint64_t> postAckIngestMaxNs{0};

    std::atomic<uint64_t> asyncDrainEnqueueCount{0};
    std::atomic<uint64_t> asyncDrainCompleteCount{0};
    std::atomic<uint64_t> asyncDrainNs{0};
    std::atomic<uint64_t> asyncDrainMaxNs{0};
    std::atomic<uint64_t> asyncDrainQueueDepthMax{0};

    std::atomic<uint64_t> walDrainReadCount{0};
    std::atomic<uint64_t> walDrainReadPayloadBytes{0};
    std::atomic<uint64_t> walDrainReadNs{0};
    std::atomic<uint64_t> walDrainReadMaxNs{0};
    std::atomic<uint64_t> walDrainReadBatchCount{0};
    std::atomic<uint64_t> walDrainReadBatchMaxRecords{0};
    std::atomic<uint64_t> walDrainReadPhysicalBytes{0};
    std::atomic<uint64_t> walDrainReadSyscallCount{0};
    std::atomic<uint64_t> walDrainBatchWaitCount{0};
    std::atomic<uint64_t> walDrainBatchWaitNs{0};
    std::atomic<uint64_t> walDrainBatchWaitMaxNs{0};

    std::atomic<uint64_t> timelineMergeBatchCount{0};
    std::atomic<uint64_t> timelineMergeEventCount{0};
    std::atomic<uint64_t> timelineMergeInsertedCount{0};
    std::atomic<uint64_t> timelineMergeBatchMaxEvents{0};
    std::atomic<uint64_t> timelineMergeLockWaitNs{0};
    std::atomic<uint64_t> timelineMergeLockWaitMaxNs{0};
    std::atomic<uint64_t> timelineMergeLockHoldNs{0};
    std::atomic<uint64_t> timelineMergeLockHoldMaxNs{0};
    std::atomic<uint64_t> timelineMergeInsertNs{0};
    std::atomic<uint64_t> timelineMergeInsertMaxNs{0};
};

} // namespace chronolog

#endif
