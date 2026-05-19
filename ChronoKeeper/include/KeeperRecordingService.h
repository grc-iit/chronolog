#ifndef KEEPER_RECORDING_SERVICE_H
#define KEEPER_RECORDING_SERVICE_H

#include <iostream>
#include <string>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <chronolog_errcode.h>
#include <KeeperIdCard.h>
#include <chronolog_types.h>
#include <chronolog_profile.h>

#include "IngestionQueue.h"
#include "KeeperDataStore.h"
#include "KeeperAppendStats.h"
#include "KeeperLocalJournal.h"

namespace tl = thallium;

namespace chronolog
{
class KeeperRecordingService: public tl::provider<KeeperRecordingService>
{
public:
    // KeeperRecordingService should be created on the heap not the stack thus the constructor is private...
    static KeeperRecordingService*
    CreateKeeperRecordingService(tl::engine& tl_engine,
                                 uint16_t service_provider_id,
                                 IngestionQueue& ingestion_queue,
                                 KeeperDataStore& data_store)
    {
        return new KeeperRecordingService(tl_engine, service_provider_id, ingestion_queue, data_store);
    }

    ~KeeperRecordingService()
    {
        stopAsyncDrain();
        KeeperRecordingService* expected = this;
        activeService().compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        KeeperAppendStats::instance().logSummary("recording_service_shutdown");
        LOG_DEBUG("[KeeperRecordingService] Destructor called. Cleaning up...");
        get_engine().pop_finalize_callback(this);
    }

    static bool WaitForAsyncDrainIdle(uint64_t timeout_ms)
    {
        KeeperRecordingService* service = activeService().load(std::memory_order_acquire);
        if(service == nullptr)
        {
            return true;
        }
        return service->waitForAsyncDrainIdle(timeout_ms);
    }

    void record_event(tl::request const& request, LogEvent const& log_event)
    {
        CL_PROFILE_REGION("keeper_record_event_rpc");
        CL_PROFILE_COUNTER("keeper_record_event_payload_bytes", log_event.logRecord.size());
        const uint64_t stats_start_ns = KeeperAppendStats::nowNs();
        //  ClientId teller_id,  StoryId story_id,
        //  ChronoTick const& chrono_tick, std::string const& record)
        LOG_DEBUG("[KeeperRecordingService] Recording event story_id={} event_time={} client_id={} event_index={} "
                  "payload_bytes={}",
                  log_event.storyId,
                  log_event.eventTime,
                  log_event.clientId,
                  log_event.eventIndex,
                  log_event.logRecord.size());
        bool const ack_before_ingest = ackBeforeIngest();
        bool const async_drain = ack_before_ingest && asyncDrainEnabled();
        bool const wal_drain = async_drain && asyncDrainFromWal();
        bool const skip_ingest = ack_before_ingest && skipIngestAfterAck();
        bool const deferred_response = ack_before_ingest && skip_ingest && deferRpcResponse();
        if(deferred_response)
        {
            tl::request deferred_request(request);
            std::size_t const payload_size = log_event.logRecord.size();
            auto& journal = KeeperLocalJournal::instance();
            journal.beginActiveAdmission();
            bool const submitted = KeeperLocalJournal::instance().appendAsync(
                    log_event,
                    [deferred_request = std::move(deferred_request), stats_start_ns, payload_size](
                            bool ok, KeeperLocalJournal::WalRecordCursor const&) mutable {
                        KeeperAppendStats::instance().recordRpc(KeeperAppendStats::nowNs() - stats_start_ns,
                                                                payload_size);
                        deferred_request.respond(ok ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN);
                    });
            journal.endActiveAdmission();
            if(!submitted)
            {
                request.respond(chronolog::CL_ERR_UNKNOWN);
            }
            return;
        }
        KeeperLocalJournal::WalRecordCursor wal_cursor;
        if(!KeeperLocalJournal::instance().append(log_event, wal_drain ? &wal_cursor : nullptr))
        {
            request.respond(chronolog::CL_ERR_UNKNOWN);
            return;
        }
        if(ack_before_ingest)
        {
            KeeperAppendStats::instance().recordRpc(KeeperAppendStats::nowNs() - stats_start_ns,
                                                    log_event.logRecord.size());
            request.respond(chronolog::CL_SUCCESS);
            if(skip_ingest)
            {
                return;
            }
            if(async_drain)
            {
                enqueueAsyncDrain(log_event, wal_drain ? &wal_cursor : nullptr);
                return;
            }
            uint64_t const post_ack_ingest_start_ns = KeeperAppendStats::nowNs();
            theIngestionQueue.ingestLogEvent(log_event);
            KeeperAppendStats::instance().recordPostAckIngest(KeeperAppendStats::nowNs() -
                                                              post_ack_ingest_start_ns);
            return;
        }
        theIngestionQueue.ingestLogEvent(log_event);
        KeeperAppendStats::instance().recordRpc(KeeperAppendStats::nowNs() - stats_start_ns, log_event.logRecord.size());
        request.respond(chronolog::CL_SUCCESS);
    }

    void record_events(tl::request const& request, std::vector<LogEvent> log_events)
    {
        CL_PROFILE_REGION("keeper_record_events_rpc");
        if(log_events.empty())
        {
            request.respond(chronolog::CL_SUCCESS);
            return;
        }

        uint64_t payload_bytes = 0;
        for(auto const& log_event: log_events)
        {
            payload_bytes += log_event.logRecord.size();
        }
        CL_PROFILE_COUNTER("keeper_record_event_payload_bytes", payload_bytes);
        CL_PROFILE_COUNTER("keeper_record_event_batch_records", log_events.size());

        bool const ack_before_ingest = ackBeforeIngest();
        bool const async_drain = ack_before_ingest && asyncDrainEnabled();
        bool const wal_drain = async_drain && asyncDrainFromWal();
        bool const skip_ingest = ack_before_ingest && skipIngestAfterAck();
        bool const deferred_response = ack_before_ingest && skip_ingest && deferRpcResponse();
        uint64_t const stats_start_ns = KeeperAppendStats::nowNs();

        if(deferred_response)
        {
            uint64_t const submit_start_ns = KeeperAppendStats::nowNs();
            auto submit_complete_ns = std::make_shared<std::atomic<uint64_t>>(0);
            tl::request deferred_request(request);
            auto completion = [deferred_request = std::move(deferred_request),
                               stats_start_ns,
                               submit_start_ns,
                               submit_complete_ns,
                               payload_bytes,
                               event_count = log_events.size()](bool ok) mutable {
                uint64_t const completion_ns = KeeperAppendStats::nowNs();
                uint64_t const submit_done_ns = submit_complete_ns->load(std::memory_order_acquire);
                uint64_t const submit_ns = submit_done_ns > submit_start_ns
                                                   ? submit_done_ns - submit_start_ns
                                                   : completion_ns - submit_start_ns;
                uint64_t const completion_wait_ns =
                        submit_done_ns > 0 && completion_ns > submit_done_ns ? completion_ns - submit_done_ns : 0;
                KeeperAppendStats::instance().recordRpcBatchPhases(submit_ns, completion_wait_ns);
                KeeperAppendStats::instance().recordRpcBatch(KeeperAppendStats::nowNs() - stats_start_ns,
                                                             payload_bytes,
                                                             event_count);
                deferred_request.respond(ok ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN);
            };
            auto& journal = KeeperLocalJournal::instance();
            journal.beginActiveAdmission();
            bool const submitted =
                    moveBatchPayloads()
                            ? journal.appendAsyncBatch(std::move(log_events), std::move(completion))
                            : journal.appendAsyncBatch(log_events, std::move(completion));
            journal.endActiveAdmission();
            submit_complete_ns->store(KeeperAppendStats::nowNs(), std::memory_order_release);
            if(!submitted)
            {
                KeeperAppendStats::instance().recordRpcBatchPhases(KeeperAppendStats::nowNs() - submit_start_ns, 0);
                request.respond(chronolog::CL_ERR_UNKNOWN);
            }
            return;
        }

        std::vector<std::pair<LogEvent, KeeperLocalJournal::WalRecordCursor>> drain_events;
        if(async_drain && !skip_ingest)
        {
            drain_events.reserve(log_events.size());
        }

        for(auto const& log_event: log_events)
        {
            KeeperLocalJournal::WalRecordCursor wal_cursor;
            if(!KeeperLocalJournal::instance().append(log_event, wal_drain ? &wal_cursor : nullptr))
            {
                request.respond(chronolog::CL_ERR_UNKNOWN);
                return;
            }
            if(ack_before_ingest)
            {
                KeeperAppendStats::instance().recordRpc(KeeperAppendStats::nowNs() - stats_start_ns,
                                                        log_event.logRecord.size());
                if(skip_ingest)
                {
                    continue;
                }
                if(async_drain)
                {
                    drain_events.emplace_back(log_event, wal_cursor);
                    continue;
                }
                uint64_t const post_ack_ingest_start_ns = KeeperAppendStats::nowNs();
                theIngestionQueue.ingestLogEvent(log_event);
                KeeperAppendStats::instance().recordPostAckIngest(KeeperAppendStats::nowNs() -
                                                                  post_ack_ingest_start_ns);
                continue;
            }
            theIngestionQueue.ingestLogEvent(log_event);
            KeeperAppendStats::instance().recordRpc(KeeperAppendStats::nowNs() - stats_start_ns,
                                                    log_event.logRecord.size());
        }

        request.respond(chronolog::CL_SUCCESS);
        for(auto& item: drain_events)
        {
            enqueueAsyncDrain(item.first, wal_drain ? &item.second : nullptr);
        }
    }

    std::vector<LogEvent> tail_events(StoryId const& story_id, chrono_time const& start_time, chrono_time const& end_time)
    {
        CL_PROFILE_REGION("keeper_tail_read");
        std::vector<LogEvent> events;
        char const* tail_source = std::getenv("CHRONOLOG_KEEPER_TAIL_SOURCE");
        bool const journal_tail = tail_source != nullptr && std::strcmp(tail_source, "journal") == 0;
        bool const journal_then_timeline =
                tail_source != nullptr && std::strcmp(tail_source, "journal_then_timeline") == 0;
        if(journal_tail || journal_then_timeline)
        {
            CL_PROFILE_REGION("keeper_journal_tail_read");
            bool const journal_ok = KeeperLocalJournal::instance().collectEvents(story_id, start_time, end_time, events);
            if(journal_tail || (journal_ok && !events.empty()))
            {
                CL_PROFILE_COUNTER("keeper_journal_tail_events", events.size());
                return events;
            }
        }
        theDataStore.collectTailEvents(story_id, start_time, end_time, events);
        return events;
    }

    KeeperTailBatch tail_events_since(StoryId const& story_id, KeeperTailCursorToken const& cursor)
    {
        CL_PROFILE_REGION("keeper_tail_read");
        CL_PROFILE_REGION("keeper_journal_tail_cursor_read");
        KeeperTailBatch batch;
        if(KeeperLocalJournal::instance().collectEventsAfter(story_id, cursor, batch))
        {
            CL_PROFILE_COUNTER("keeper_journal_tail_cursor_events", batch.events.size());
            return batch;
        }
        batch.ok = false;
        return batch;
    }

    KeeperTailPackedBatch tail_events_since_packed(StoryId const& story_id, KeeperTailCursorToken const& cursor)
    {
        CL_PROFILE_REGION("keeper_tail_read");
        CL_PROFILE_REGION("keeper_journal_tail_cursor_read");
        CL_PROFILE_REGION("keeper_journal_tail_cursor_pack");
        KeeperTailPackedBatch packed;
        if(!KeeperLocalJournal::instance().collectEventsAfterPacked(story_id, cursor, packed))
        {
            packed.ok = false;
            return packed;
        }
        CL_PROFILE_COUNTER("keeper_journal_tail_cursor_events", packed.eventTimes.size());
        CL_PROFILE_COUNTER("keeper_journal_tail_cursor_payload_bytes", packed.payloadBlob.size());
        return packed;
    }

    void tail_events_since_packed_bulk(tl::request const& request,
                                       StoryId const& story_id,
                                       KeeperTailCursorToken const& cursor,
                                       tl::bulk& remote_payload)
    {
        CL_PROFILE_REGION("keeper_tail_read");
        CL_PROFILE_REGION("keeper_journal_tail_cursor_read");
        CL_PROFILE_REGION("keeper_journal_tail_cursor_pack_bulk");

        uint64_t const total_start_ns = KeeperAppendStats::nowNs();
        KeeperTailPackedBatch packed;
        std::unique_ptr<char[]> payload_storage;
        char const* uninitialized_bulk_buffer_env =
                std::getenv("CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER");
        bool const use_uninitialized_bulk_buffer =
                uninitialized_bulk_buffer_env != nullptr &&
                (std::strcmp(uninitialized_bulk_buffer_env, "1") == 0 ||
                 std::strcmp(uninitialized_bulk_buffer_env, "true") == 0 ||
                 std::strcmp(uninitialized_bulk_buffer_env, "yes") == 0 ||
                 std::strcmp(uninitialized_bulk_buffer_env, "on") == 0);
        char const* parallel_bulk_transfer_env =
                std::getenv("CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER");
        bool const use_parallel_bulk_transfer =
                parallel_bulk_transfer_env != nullptr &&
                (std::strcmp(parallel_bulk_transfer_env, "1") == 0 ||
                 std::strcmp(parallel_bulk_transfer_env, "true") == 0 ||
                 std::strcmp(parallel_bulk_transfer_env, "yes") == 0 ||
                 std::strcmp(parallel_bulk_transfer_env, "on") == 0);
        std::size_t payload_bytes = 0;
        uint64_t const collect_start_ns = KeeperAppendStats::nowNs();
        bool const collected =
                use_uninitialized_bulk_buffer
                        ? KeeperLocalJournal::instance().collectEventsAfterPackedBulk(
                                  story_id, cursor, packed, payload_storage, payload_bytes)
                        : KeeperLocalJournal::instance().collectEventsAfterPacked(story_id, cursor, packed);
        uint64_t const collect_ns = KeeperAppendStats::nowNs() - collect_start_ns;
        std::size_t const bulk_chunk_bytes = []() {
            char const* chunk_env = std::getenv("CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES");
            if(chunk_env == nullptr || chunk_env[0] == '\0')
            {
                return std::size_t{0};
            }
            char* end = nullptr;
            unsigned long long const parsed = std::strtoull(chunk_env, &end, 10);
            if(end == chunk_env || (end != nullptr && *end != '\0'))
            {
                return std::size_t{0};
            }
            return static_cast<std::size_t>(parsed);
        }();
        uint64_t bulk_expose_ns = 0;
        uint64_t bulk_transfer_ns = 0;
        uint64_t response_ns = 0;
        std::size_t bulk_transfer_count = 0;
        auto log_tail_bulk_stats = [&](bool ok) {
            LOG_INFO("[KeeperTailBulkStats] ok={} event_count={} payload_bytes={} collect_us={} "
                     "bulk_expose_us={} bulk_transfer_us={} response_us={} total_us={} "
                     "uninitialized_buffer={} remote_capacity_bytes={} chunk_bytes={} transfer_count={} "
                     "parallel_transfer={}",
                     ok ? 1 : 0,
                     ok ? packed.eventTimes.size() : 0,
                     ok ? payload_bytes : 0,
                     collect_ns / 1000.0,
                     bulk_expose_ns / 1000.0,
                     bulk_transfer_ns / 1000.0,
                     response_ns / 1000.0,
                     (KeeperAppendStats::nowNs() - total_start_ns) / 1000.0,
                     use_uninitialized_bulk_buffer ? 1 : 0,
                     remote_payload.size(),
                     bulk_chunk_bytes,
                     bulk_transfer_count,
                     use_parallel_bulk_transfer ? 1 : 0);
        };
        if(collected)
        {
            if(!use_uninitialized_bulk_buffer)
            {
                payload_bytes = packed.payloadBlob.size();
            }
            if(payload_bytes <= remote_payload.size())
            {
                try
                {
                    if(payload_bytes > 0)
                    {
                        tl::endpoint ep = request.get_endpoint();
                        std::vector<std::pair<void*, std::size_t>> segments(1);
                        segments[0].first = use_uninitialized_bulk_buffer
                                                    ? static_cast<void*>(payload_storage.get())
                                                    : static_cast<void*>(packed.payloadBlob.data());
                        segments[0].second = payload_bytes;
                        uint64_t const bulk_expose_start_ns = KeeperAppendStats::nowNs();
                        tl::bulk local_payload = get_engine().expose(segments, tl::bulk_mode::read_only);
                        bulk_expose_ns += KeeperAppendStats::nowNs() - bulk_expose_start_ns;
                        {
                            CL_PROFILE_REGION("rpc_bulk_transfer");
                            uint64_t const bulk_transfer_start_ns = KeeperAppendStats::nowNs();
                            if(use_parallel_bulk_transfer && bulk_chunk_bytes > 0 && bulk_chunk_bytes < payload_bytes)
                            {
                                margo_instance_id const mid = get_engine().get_margo_instance();
                                hg_addr_t const remote_addr = ep.get_addr();
                                hg_bulk_t const remote_bulk = remote_payload.get_bulk();
                                hg_bulk_t const local_bulk = local_payload.get_bulk();
                                std::vector<margo_request> requests;
                                requests.reserve((payload_bytes + bulk_chunk_bytes - 1) / bulk_chunk_bytes);
                                std::size_t offset = 0;
                                while(offset < payload_bytes)
                                {
                                    std::size_t const chunk_size =
                                            std::min(bulk_chunk_bytes, payload_bytes - offset);
                                    margo_request transfer_request = MARGO_REQUEST_NULL;
                                    hg_return_t const submit_ret =
                                            margo_bulk_itransfer(mid,
                                                                 HG_BULK_PUSH,
                                                                 remote_addr,
                                                                 remote_bulk,
                                                                 offset,
                                                                 local_bulk,
                                                                 offset,
                                                                 chunk_size,
                                                                 &transfer_request);
                                    if(submit_ret != HG_SUCCESS)
                                    {
                                        for(auto& pending_request: requests)
                                        {
                                            if(pending_request != MARGO_REQUEST_NULL)
                                            {
                                                (void)margo_wait(pending_request);
                                            }
                                        }
                                        throw thallium::exception("margo_bulk_itransfer submit failed");
                                    }
                                    requests.push_back(transfer_request);
                                    ++bulk_transfer_count;
                                    offset += chunk_size;
                                }
                                for(auto& transfer_request: requests)
                                {
                                    hg_return_t const wait_ret = margo_wait(transfer_request);
                                    if(wait_ret != HG_SUCCESS)
                                    {
                                        throw thallium::exception("margo_bulk_itransfer wait failed");
                                    }
                                }
                            }
                            else if(bulk_chunk_bytes > 0 && bulk_chunk_bytes < payload_bytes)
                            {
                                auto remote = remote_payload.on(ep);
                                std::size_t offset = 0;
                                while(offset < payload_bytes)
                                {
                                    std::size_t const chunk_size =
                                            std::min(bulk_chunk_bytes, payload_bytes - offset);
                                    remote.select(offset, chunk_size) << local_payload.select(offset, chunk_size);
                                    ++bulk_transfer_count;
                                    offset += chunk_size;
                                }
                            }
                            else
                            {
                                remote_payload.on(ep) << local_payload;
                                bulk_transfer_count = payload_bytes > 0 ? 1 : 0;
                            }
                            bulk_transfer_ns += KeeperAppendStats::nowNs() - bulk_transfer_start_ns;
                        }
                    }
                    CL_PROFILE_COUNTER("keeper_journal_tail_cursor_events", packed.eventTimes.size());
                    CL_PROFILE_COUNTER("keeper_journal_tail_cursor_payload_bytes", payload_bytes);
                    packed.payloadBlob.clear();
                    uint64_t const response_start_ns = KeeperAppendStats::nowNs();
                    request.respond(packed);
                    response_ns += KeeperAppendStats::nowNs() - response_start_ns;
                    log_tail_bulk_stats(true);
                    return;
                }
                catch(thallium::exception const& ex)
                {
                    LOG_ERROR("[KeeperRecordingService] packed tail bulk transfer failed: {}", ex.what());
                }
            }
            else
            {
                LOG_ERROR("[KeeperRecordingService] packed tail bulk buffer too small: remote={} payload={}",
                          remote_payload.size(),
                          payload_bytes);
            }
        }
        packed.ok = false;
        packed.payloadBlob.clear();
        uint64_t const response_start_ns = KeeperAppendStats::nowNs();
        request.respond(packed);
        response_ns += KeeperAppendStats::nowNs() - response_start_ns;
        log_tail_bulk_stats(false);
    }

    void tail_events_since_packed_bulk_stream(tl::request const& request,
                                              StoryId const& story_id,
                                              KeeperTailCursorToken const& cursor,
                                              tl::bulk& remote_payload,
                                              uint64_t max_batches)
    {
        CL_PROFILE_REGION("keeper_tail_read");
        CL_PROFILE_REGION("keeper_journal_tail_cursor_read");
        CL_PROFILE_REGION("keeper_journal_tail_cursor_pack_bulk_stream");

        uint64_t const total_start_ns = KeeperAppendStats::nowNs();
        uint64_t collect_ns = 0;
        uint64_t bulk_expose_ns = 0;
        uint64_t bulk_transfer_ns = 0;
        uint64_t response_ns = 0;
        std::size_t payload_bytes = 0;
        std::size_t batch_count = 0;
        KeeperTailCursorToken stream_cursor = cursor;
        KeeperTailPackedBatch response;
        response.nextCursor = cursor;

        tl::endpoint ep = request.get_endpoint();
        auto remote = remote_payload.on(ep);
        std::size_t const batch_limit = max_batches == 0 ? std::numeric_limits<std::size_t>::max()
                                                         : static_cast<std::size_t>(max_batches);

        try
        {
            for(std::size_t batch_index = 0; batch_index < batch_limit; ++batch_index)
            {
                KeeperTailPackedBatch batch;
                std::unique_ptr<char[]> batch_payload;
                std::size_t batch_payload_bytes = 0;
                uint64_t const collect_start_ns = KeeperAppendStats::nowNs();
                bool const collected = KeeperLocalJournal::instance().collectEventsAfterPackedBulk(story_id,
                                                                                                   stream_cursor,
                                                                                                   batch,
                                                                                                   batch_payload,
                                                                                                   batch_payload_bytes);
                collect_ns += KeeperAppendStats::nowNs() - collect_start_ns;
                if(!collected || !batch.ok)
                {
                    response.ok = false;
                    break;
                }

                if(batch_payload_bytes > remote_payload.size() || payload_bytes > remote_payload.size() - batch_payload_bytes)
                {
                    if(batch_count > 0)
                    {
                        response.ok = true;
                    }
                    else
                    {
                        LOG_ERROR("[KeeperRecordingService] packed stream tail bulk buffer too small: remote={} payload={}",
                                  remote_payload.size(),
                                  batch_payload_bytes);
                        response.ok = false;
                    }
                    break;
                }

                std::size_t const payload_base = payload_bytes;
                std::size_t const event_count = batch.eventTimes.size();
                response.eventTimes.insert(response.eventTimes.end(), batch.eventTimes.begin(), batch.eventTimes.end());
                response.clientIds.insert(response.clientIds.end(), batch.clientIds.begin(), batch.clientIds.end());
                response.eventIndexes.insert(response.eventIndexes.end(),
                                             batch.eventIndexes.begin(),
                                             batch.eventIndexes.end());
                response.payloadOffsets.reserve(response.payloadOffsets.size() + batch.payloadOffsets.size());
                for(uint64_t offset: batch.payloadOffsets)
                {
                    response.payloadOffsets.push_back(static_cast<uint64_t>(payload_base) + offset);
                }
                response.payloadSizes.insert(response.payloadSizes.end(),
                                             batch.payloadSizes.begin(),
                                             batch.payloadSizes.end());
                if(batch_payload_bytes > 0)
                {
                    std::vector<std::pair<void*, std::size_t>> segments(1);
                    segments[0].first = static_cast<void*>(batch_payload.get());
                    segments[0].second = batch_payload_bytes;
                    uint64_t const bulk_expose_start_ns = KeeperAppendStats::nowNs();
                    tl::bulk local_payload = get_engine().expose(segments, tl::bulk_mode::read_only);
                    bulk_expose_ns += KeeperAppendStats::nowNs() - bulk_expose_start_ns;
                    {
                        CL_PROFILE_REGION("rpc_bulk_transfer");
                        uint64_t const bulk_transfer_start_ns = KeeperAppendStats::nowNs();
                        remote.select(payload_base, batch_payload_bytes) << local_payload.select(0, batch_payload_bytes);
                        bulk_transfer_ns += KeeperAppendStats::nowNs() - bulk_transfer_start_ns;
                    }
                }
                payload_bytes += batch_payload_bytes;
                stream_cursor = batch.nextCursor;
                response.nextCursor = batch.nextCursor;
                response.ok = true;
                ++batch_count;

                if(event_count == 0)
                {
                    break;
                }
            }
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingService] packed stream tail bulk transfer failed: {}", ex.what());
            response.ok = false;
        }

        response.sourceBatchCount = batch_count;
        if(response.ok)
        {
            CL_PROFILE_COUNTER("keeper_journal_tail_cursor_events", response.eventTimes.size());
            CL_PROFILE_COUNTER("keeper_journal_tail_cursor_payload_bytes", payload_bytes);
        }

        response.payloadBlob.clear();
        uint64_t const response_start_ns = KeeperAppendStats::nowNs();
        request.respond(response);
        response_ns += KeeperAppendStats::nowNs() - response_start_ns;
        LOG_INFO("[KeeperTailBulkStreamStats] ok={} event_count={} payload_bytes={} batch_count={} "
                 "collect_us={} bulk_expose_us={} bulk_transfer_us={} response_us={} total_us={} "
                 "remote_capacity_bytes={}",
                 response.ok ? 1 : 0,
                 response.ok ? response.eventTimes.size() : 0,
                 response.ok ? payload_bytes : 0,
                 batch_count,
                 collect_ns / 1000.0,
                 bulk_expose_ns / 1000.0,
                 bulk_transfer_ns / 1000.0,
                 response_ns / 1000.0,
                 (KeeperAppendStats::nowNs() - total_start_ns) / 1000.0,
                 remote_payload.size());
    }

private:
    KeeperRecordingService(tl::engine& tl_engine,
                           uint16_t service_provider_id,
                           IngestionQueue& ingestion_queue,
                           KeeperDataStore& data_store)
        : tl::provider<KeeperRecordingService>(tl_engine, service_provider_id)
        , theIngestionQueue(ingestion_queue)
        , theDataStore(data_store)
    {
        activeService().store(this, std::memory_order_release);
        if(asyncDrainEnabled())
        {
            std::size_t const drain_threads = asyncDrainThreadCount();
            asyncDrainWorkers.reserve(drain_threads);
            for(std::size_t worker_index = 0; worker_index < drain_threads; ++worker_index)
            {
                auto worker = std::make_unique<AsyncDrainWorker>();
                worker->thread = std::thread([this, worker_ptr = worker.get()]() { asyncDrainLoop(*worker_ptr); });
                asyncDrainWorkers.push_back(std::move(worker));
            }
            LOG_INFO("[KeeperRecordingService] started {} async journal drain worker(s)", drain_threads);
        }
        define("record_event", &KeeperRecordingService::record_event, tl::ignore_return_value());
        define("record_events", &KeeperRecordingService::record_events, tl::ignore_return_value());
        define("tail_events", &KeeperRecordingService::tail_events);
        define("tail_events_since", &KeeperRecordingService::tail_events_since);
        define("tail_events_since_packed", &KeeperRecordingService::tail_events_since_packed);
        define("tail_events_since_packed_bulk", &KeeperRecordingService::tail_events_since_packed_bulk);
        define("tail_events_since_packed_bulk_stream", &KeeperRecordingService::tail_events_since_packed_bulk_stream);
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]() { delete p; });
    }

    KeeperRecordingService(KeeperRecordingService const&) = delete;

    KeeperRecordingService& operator=(KeeperRecordingService const&) = delete;

    static std::atomic<KeeperRecordingService*>& activeService()
    {
        static std::atomic<KeeperRecordingService*> service{nullptr};
        return service;
    }

    static bool envEnabled(char const* name)
    {
        char const* value = std::getenv(name);
        return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
                                    std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0);
    }

    static bool ackBeforeIngest()
    {
        return KeeperLocalJournal::instance().enabled() && envEnabled("CHRONOLOG_KEEPER_JOURNAL_ACK_BEFORE_INGEST");
    }

    static bool asyncDrainEnabled()
    {
        return KeeperLocalJournal::instance().enabled() && envEnabled("CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN");
    }

    static bool skipIngestAfterAck()
    {
        return KeeperLocalJournal::instance().enabled() && envEnabled("CHRONOLOG_KEEPER_JOURNAL_SKIP_INGEST");
    }

    static bool deferRpcResponse()
    {
        return KeeperLocalJournal::instance().enabled() && envEnabled("CHRONOLOG_KEEPER_JOURNAL_DEFER_RPC_RESPONSE");
    }

    static bool moveBatchPayloads()
    {
        return KeeperLocalJournal::instance().enabled() &&
               envEnabled("CHRONOLOG_KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS");
    }

    static bool asyncDrainFromWal()
    {
        char const* value = std::getenv("CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE");
        return value != nullptr && (std::strcmp(value, "wal") == 0 || std::strcmp(value, "journal") == 0);
    }

    static std::size_t walDrainBatchEvents()
    {
        static std::size_t batch_events = []() {
            char const* value = std::getenv("CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_EVENTS");
            if(value == nullptr || *value == '\0')
            {
                return std::size_t{1};
            }
            char* end = nullptr;
            unsigned long parsed = std::strtoul(value, &end, 10);
            if(end == value || parsed == 0)
            {
                return std::size_t{1};
            }
            return static_cast<std::size_t>(std::min<unsigned long>(parsed, 4096UL));
        }();
        return batch_events;
    }

    static uint64_t walDrainBatchWaitUs()
    {
        static uint64_t wait_us = []() {
            char const* value = std::getenv("CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_WAIT_US");
            if(value == nullptr || *value == '\0')
            {
                return uint64_t{0};
            }
            char* end = nullptr;
            unsigned long parsed = std::strtoul(value, &end, 10);
            if(end == value)
            {
                return uint64_t{0};
            }
            return static_cast<uint64_t>(std::min<unsigned long>(parsed, 100000UL));
        }();
        return wait_us;
    }

    static std::size_t asyncDrainThreadCount()
    {
        static std::size_t thread_count = []() {
            char const* value = std::getenv("CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_THREADS");
            if(value == nullptr || *value == '\0')
            {
                return std::size_t{1};
            }
            char* end = nullptr;
            unsigned long parsed = std::strtoul(value, &end, 10);
            if(end == value || parsed == 0)
            {
                return std::size_t{1};
            }
            return static_cast<std::size_t>(std::min<unsigned long>(parsed, 64UL));
        }();
        return thread_count;
    }

    struct AsyncDrainItem
    {
        bool readFromWal{false};
        KeeperLocalJournal::WalRecordCursor cursor;
        LogEvent event;
    };

    struct AsyncDrainWorker
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<AsyncDrainItem> queue;
        bool stop{false};
        std::thread thread;
    };

    std::size_t asyncDrainWorkerIndex(LogEvent const& event,
                                      KeeperLocalJournal::WalRecordCursor const* cursor) const
    {
        if(asyncDrainWorkers.empty())
        {
            return 0;
        }
        uint64_t story_id = event.storyId;
        uint64_t client_id = event.clientId;
        uint32_t event_index = event.eventIndex;
        if(cursor != nullptr && cursor->valid)
        {
            story_id = cursor->story_id;
            client_id = cursor->client_id;
            event_index = cursor->event_index;
        }
        std::size_t const hash = std::hash<uint64_t>{}(story_id) ^
                                 (std::hash<uint64_t>{}(client_id) << 1U) ^
                                 (std::hash<uint32_t>{}(event_index) << 2U);
        return hash % asyncDrainWorkers.size();
    }

    void enqueueAsyncDrain(LogEvent const& event, KeeperLocalJournal::WalRecordCursor const* cursor)
    {
        if(asyncDrainWorkers.empty())
        {
            LOG_ERROR("[KeeperRecordingService] async drain requested but no drain workers are available");
            return;
        }
        std::size_t const worker_index = asyncDrainWorkerIndex(event, cursor);
        AsyncDrainWorker& worker = *asyncDrainWorkers[worker_index];
        std::size_t depth = 0;
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            if(cursor != nullptr)
            {
                worker.queue.push_back(AsyncDrainItem{true, *cursor, LogEvent{}});
            }
            else
            {
                worker.queue.push_back(AsyncDrainItem{false, KeeperLocalJournal::WalRecordCursor{}, event});
            }
            depth = worker.queue.size();
        }
        KeeperAppendStats::instance().recordAsyncDrainEnqueue(depth);
        worker.condition.notify_one();
    }

    void asyncDrainLoop(AsyncDrainWorker& worker)
    {
        for(;;)
        {
            AsyncDrainItem item;
            {
                std::unique_lock<std::mutex> lock(worker.mutex);
                worker.condition.wait(lock, [&worker]() { return worker.stop || !worker.queue.empty(); });
                if(worker.stop && worker.queue.empty())
                {
                    return;
                }
                item = std::move(worker.queue.front());
                worker.queue.pop_front();
            }
            activeAsyncDrainCount.fetch_add(1, std::memory_order_acq_rel);
            if(item.readFromWal)
            {
                drainWalBatch(worker, std::move(item));
                markAsyncDrainInactive();
                continue;
            }
            uint64_t const drain_start_ns = KeeperAppendStats::nowNs();
            LogEvent event = std::move(item.event);
            theIngestionQueue.ingestLogEvent(std::move(event));
            KeeperAppendStats::instance().recordPostAckIngest(KeeperAppendStats::nowNs() - drain_start_ns);
            KeeperAppendStats::instance().recordAsyncDrainComplete(KeeperAppendStats::nowNs() - drain_start_ns);
            markAsyncDrainInactive();
        }
    }

    void drainWalBatch(AsyncDrainWorker& worker, AsyncDrainItem first_item)
    {
        std::size_t const batch_limit = walDrainBatchEvents();
        std::vector<KeeperLocalJournal::WalRecordCursor> cursors;
        cursors.reserve(batch_limit);
        cursors.push_back(first_item.cursor);
        {
            std::unique_lock<std::mutex> lock(worker.mutex);
            waitForWalDrainBatch(worker, lock, cursors.size(), batch_limit);
            while(cursors.size() < batch_limit && !worker.queue.empty() &&
                  worker.queue.front().readFromWal)
            {
                cursors.push_back(worker.queue.front().cursor);
                worker.queue.pop_front();
            }
        }

        uint64_t const drain_start_ns = KeeperAppendStats::nowNs();
        uint64_t const wal_read_start_ns = KeeperAppendStats::nowNs();
        std::vector<LogEvent> events;
        KeeperLocalJournal::WalDrainReadStats wal_read_stats;
        bool read_ok = cursors.size() == 1
                               ? readSingleWalRecord(cursors.front(), events, wal_read_stats)
                               : KeeperLocalJournal::instance().readRecordsCoalesced(cursors, events, &wal_read_stats);
        if(!read_ok)
        {
            for(auto const& cursor: cursors)
            {
                LOG_ERROR("[KeeperRecordingService] failed to read acknowledged WAL record for async drain story_id={} "
                          "event_time={} client_id={} event_index={}",
                          cursor.story_id,
                          cursor.event_time,
                          cursor.client_id,
                          cursor.event_index);
            }
            return;
        }

        uint64_t payload_bytes = 0;
        for(auto const& event: events)
        {
            payload_bytes += event.logRecord.size();
        }
        KeeperAppendStats::instance().recordWalDrainReadBatch(KeeperAppendStats::nowNs() - wal_read_start_ns,
                                                              payload_bytes,
                                                              events.size(),
                                                              wal_read_stats.batch_count,
                                                              wal_read_stats.physical_read_bytes,
                                                              wal_read_stats.read_syscall_count,
                                                              wal_read_stats.max_batch_records);

        std::size_t const event_count = events.size();
        theIngestionQueue.ingestLogEvents(std::move(events));
        uint64_t const elapsed_ns = KeeperAppendStats::nowNs() - drain_start_ns;
        KeeperAppendStats::instance().recordPostAckIngestBatch(elapsed_ns, event_count);
        KeeperAppendStats::instance().recordAsyncDrainCompleteBatch(elapsed_ns, event_count);
    }

    bool readSingleWalRecord(KeeperLocalJournal::WalRecordCursor const& cursor,
                             std::vector<LogEvent>& events,
                             KeeperLocalJournal::WalDrainReadStats& stats)
    {
        LogEvent event;
        if(!KeeperLocalJournal::instance().readRecord(cursor, event))
        {
            return false;
        }
        stats.batch_count = 1;
        stats.physical_read_bytes = cursor.payload_size;
        stats.read_syscall_count = cursor.payload_size > 0 ? 1 : 0;
        stats.max_batch_records = 1;
        events.push_back(std::move(event));
        return true;
    }

    static std::size_t availableWalDrainItemsLocked(AsyncDrainWorker const& worker, std::size_t limit)
    {
        std::size_t count = 0;
        for(auto const& item: worker.queue)
        {
            if(!item.readFromWal || count >= limit)
            {
                break;
            }
            ++count;
        }
        return count;
    }

    void waitForWalDrainBatch(AsyncDrainWorker& worker,
                              std::unique_lock<std::mutex>& lock,
                              std::size_t current_batch_size,
                              std::size_t batch_limit)
    {
        uint64_t const wait_us = walDrainBatchWaitUs();
        if(wait_us == 0 || current_batch_size >= batch_limit || worker.stop)
        {
            return;
        }

        uint64_t const wait_start_ns = KeeperAppendStats::nowNs();
        worker.condition.wait_for(lock,
                                  std::chrono::microseconds(wait_us),
                                  [&worker, current_batch_size, batch_limit]() {
                                      return worker.stop ||
                                             current_batch_size +
                                                             availableWalDrainItemsLocked(worker,
                                                                                         batch_limit -
                                                                                                 current_batch_size) >=
                                                     batch_limit;
                                  });
        KeeperAppendStats::instance().recordWalDrainBatchWait(KeeperAppendStats::nowNs() - wait_start_ns);
    }

    void stopAsyncDrain()
    {
        if(asyncDrainWorkers.empty())
        {
            return;
        }
        for(auto& worker: asyncDrainWorkers)
        {
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                worker->stop = true;
            }
            worker->condition.notify_all();
        }
        for(auto& worker: asyncDrainWorkers)
        {
            if(worker->thread.joinable())
            {
                worker->thread.join();
            }
        }
        asyncDrainWorkers.clear();
    }

    void markAsyncDrainInactive()
    {
        activeAsyncDrainCount.fetch_sub(1, std::memory_order_acq_rel);
        asyncDrainIdleCondition.notify_all();
    }

    bool asyncDrainQueuesEmpty() const
    {
        for(auto const& worker: asyncDrainWorkers)
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            if(!worker->queue.empty())
            {
                return false;
            }
        }
        return true;
    }

    bool waitForAsyncDrainIdle(uint64_t timeout_ms)
    {
        if(asyncDrainWorkers.empty())
        {
            return true;
        }
        auto idle = [this]() {
            return activeAsyncDrainCount.load(std::memory_order_acquire) == 0 && asyncDrainQueuesEmpty();
        };
        if(idle())
        {
            return true;
        }
        if(timeout_ms == 0)
        {
            return false;
        }
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(asyncDrainIdleMutex);
        while(!idle())
        {
            if(asyncDrainIdleCondition.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                return idle();
            }
        }
        return true;
    }

    IngestionQueue& theIngestionQueue;
    KeeperDataStore& theDataStore;
    std::vector<std::unique_ptr<AsyncDrainWorker>> asyncDrainWorkers;
    std::atomic<uint64_t> activeAsyncDrainCount{0};
    std::mutex asyncDrainIdleMutex;
    std::condition_variable asyncDrainIdleCondition;
};

} // namespace chronolog

#endif
