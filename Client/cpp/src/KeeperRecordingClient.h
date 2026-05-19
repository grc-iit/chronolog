#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <iostream>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <memory>
#include <string>
#include <vector>

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <chronolog_types.h>
#include <chronolog_client.h>
#include <ServiceId.h>
#include <client_errcode.h>
#include <chrono_monitor.h>
#include <chronolog_profile.h>

namespace tl = thallium;


namespace chronolog
{


class KeeperRecordingClient
{
    class AsyncRecordState;

public:
    static KeeperRecordingClient* CreateKeeperRecordingClient(tl::engine& tl_engine, ServiceId const& keeper_service_id)
    {
        try
        {
            return new KeeperRecordingClient(tl_engine, keeper_service_id);
        }
        catch(tl::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to create KeeperRecordingClient exception {}", ex.what());
        }
        return nullptr;
    }

    int send_event_msg(LogEvent const& eventMsg)
    {
        CL_PROFILE_REGION("rpc_send");
        CL_PROFILE_COUNTER("append_bytes", eventMsg.logRecord.size());

        try
        {
            //std::stringstream ss;
            //ss << eventMsg;
            //LOG_TRACE("[KeeperRecordingClient] Sending event message: {}", ss.str());
            int return_code = record_event.on(service_ph)(eventMsg);
            //LOG_TRACE("[KeeperRecordingClient] Sent event message: {} with return code: {}", ss.str(), return_code);
            return return_code;
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to send event message to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    LogEventFuture send_event_msg_async(LogEvent const& eventMsg)
    {
        CL_PROFILE_REGION("rpc_send_async");
        CL_PROFILE_COUNTER("append_bytes", eventMsg.logRecord.size());

        try
        {
            auto response = record_event.on(service_ph).async(eventMsg);
            return LogEventFuture(std::make_shared<AsyncRecordState>(
                    std::move(response), eventMsg.eventTime, recordingServiceId));
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to asynchronously send event message to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        return LogEventFuture();
    }

    LogEventFuture send_event_batch_msg_async(std::vector<LogEvent> const& eventMsgs)
    {
        CL_PROFILE_REGION("rpc_send_batch_async");
        uint64_t payload_bytes = 0;
        for(auto const& eventMsg: eventMsgs)
        {
            payload_bytes += eventMsg.logRecord.size();
        }
        CL_PROFILE_COUNTER("append_bytes", payload_bytes);
        CL_PROFILE_COUNTER("append_batch_records", eventMsgs.size());

        if(eventMsgs.empty())
        {
            return LogEventFuture();
        }

        try
        {
            auto response = record_events.on(service_ph).async(eventMsgs);
            return LogEventFuture(std::make_shared<AsyncRecordState>(
                    std::move(response), eventMsgs.back().eventTime, recordingServiceId));
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to asynchronously send event batch of {} records to {} "
                      "exception: {}",
                      eventMsgs.size(),
                      to_string(recordingServiceId),
                      ex.what());
        }
        return LogEventFuture();
    }

    LogEventFuture send_event_batch_msg_async(std::vector<LogEvent>&& eventMsgs)
    {
        CL_PROFILE_REGION("rpc_send_batch_async");
        uint64_t payload_bytes = 0;
        for(auto const& eventMsg: eventMsgs)
        {
            payload_bytes += eventMsg.logRecord.size();
        }
        CL_PROFILE_COUNTER("append_bytes", payload_bytes);
        CL_PROFILE_COUNTER("append_batch_records", eventMsgs.size());

        if(eventMsgs.empty())
        {
            return LogEventFuture();
        }

        try
        {
            auto keep_alive = std::make_shared<std::vector<LogEvent>>(std::move(eventMsgs));
            auto response = record_events.on(service_ph).async(*keep_alive);
            return LogEventFuture(std::make_shared<AsyncRecordState>(
                    std::move(response), keep_alive->back().eventTime, recordingServiceId, std::move(keep_alive)));
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to asynchronously send event batch of {} records to {} "
                      "exception: {}",
                      eventMsgs.size(),
                      to_string(recordingServiceId),
                      ex.what());
        }
        return LogEventFuture();
    }

    int send_tail_request(StoryId const& story_id,
                          chrono_time const& start_time,
                          chrono_time const& end_time,
                          std::vector<LogEvent>& events)
    {
        CL_PROFILE_REGION("client_query");
        CL_PROFILE_REGION("client_keeper_tail_rpc");

        try
        {
            auto const rpc_start = std::chrono::steady_clock::now();
            events = tail_events.on(service_ph)(story_id, start_time, end_time).as<std::vector<LogEvent>>();
            recordTailRpcStats("full", chronolog::CL_SUCCESS, true, events, elapsedMicros(rpc_start));
            return chronolog::CL_SUCCESS;
        }
        catch(thallium::exception const& ex)
        {
            LOG_DEBUG("[KeeperRecordingClient] Failed tail request to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        recordTailRpcStats("full", chronolog::CL_ERR_UNKNOWN, false, events, 0);
        return chronolog::CL_ERR_UNKNOWN;
    }

    int send_tail_since_request(StoryId const& story_id, KeeperTailCursorToken const& cursor, KeeperTailBatch& batch)
    {
        CL_PROFILE_REGION("client_query");
        CL_PROFILE_REGION("client_keeper_tail_cursor_rpc");

        try
        {
            auto const rpc_start = std::chrono::steady_clock::now();
            batch = tail_events_since.on(service_ph)(story_id, cursor).as<KeeperTailBatch>();
            int const status = batch.ok ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN;
            recordTailRpcStats("incremental", status, batch.ok, batch.events, elapsedMicros(rpc_start));
            return status;
        }
        catch(thallium::exception const& ex)
        {
            LOG_DEBUG("[KeeperRecordingClient] Failed tail cursor request to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        recordTailRpcStats("incremental", chronolog::CL_ERR_UNKNOWN, false, batch.events, 0);
        return chronolog::CL_ERR_UNKNOWN;
    }

    int send_tail_since_packed_request(StoryId const& story_id,
                                       KeeperTailCursorToken const& cursor,
                                       KeeperTailBatch& batch)
    {
        CL_PROFILE_REGION("client_query");
        CL_PROFILE_REGION("client_keeper_tail_cursor_rpc");
        CL_PROFILE_REGION("client_keeper_tail_cursor_unpack");

        try
        {
            auto const rpc_start = std::chrono::steady_clock::now();
            KeeperTailPackedBatch packed = tail_events_since_packed.on(service_ph)(story_id, cursor).as<KeeperTailPackedBatch>();
            batch.ok = packed.ok;
            batch.nextCursor = packed.nextCursor;
            if(packed.ok)
            {
                unpackTailBatch(story_id, std::move(packed), batch);
            }
            int const status = batch.ok ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN;
            recordTailRpcStats("incremental_packed", status, batch.ok, batch.events, elapsedMicros(rpc_start));
            return status;
        }
        catch(thallium::exception const& ex)
        {
            LOG_DEBUG("[KeeperRecordingClient] Failed packed tail cursor request to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        recordTailRpcStats("incremental_packed", chronolog::CL_ERR_UNKNOWN, false, batch.events, 0);
        return chronolog::CL_ERR_UNKNOWN;
    }

    int send_tail_since_packed_raw_request(StoryId const& story_id,
                                           KeeperTailCursorToken const& cursor,
                                           KeeperTailPackedBatch& packed)
    {
        CL_PROFILE_REGION("client_query");
        CL_PROFILE_REGION("client_keeper_tail_cursor_rpc");
        CL_PROFILE_REGION("client_keeper_tail_cursor_packed_raw");

        try
        {
            auto const rpc_start = std::chrono::steady_clock::now();
            packed = tail_events_since_packed.on(service_ph)(story_id, cursor).as<KeeperTailPackedBatch>();
            int const status = packed.ok ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN;
            recordTailPackedRpcStats("incremental_packed_raw", status, packed, elapsedMicros(rpc_start));
            return status;
        }
        catch(thallium::exception const& ex)
        {
            LOG_DEBUG("[KeeperRecordingClient] Failed raw packed tail cursor request to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        recordTailPackedRpcStats("incremental_packed_raw", chronolog::CL_ERR_UNKNOWN, packed, 0);
        return chronolog::CL_ERR_UNKNOWN;
    }

    int send_tail_since_packed_bulk_request(StoryId const& story_id,
                                            KeeperTailCursorToken const& cursor,
                                            std::size_t payload_buffer_bytes,
                                            KeeperTailPackedBatch& packed)
    {
        return send_tail_since_packed_bulk_request(story_id, cursor, payload_buffer_bytes, std::string(), packed);
    }

    int send_tail_since_packed_bulk_stream_request(StoryId const& story_id,
                                                   KeeperTailCursorToken const& cursor,
                                                   std::size_t payload_buffer_bytes,
                                                   std::size_t max_batches,
                                                   std::string payload_buffer,
                                                   KeeperTailPackedBatch& packed)
    {
        CL_PROFILE_REGION("client_query");
        CL_PROFILE_REGION("client_keeper_tail_cursor_rpc");
        CL_PROFILE_REGION("client_keeper_tail_cursor_packed_bulk_stream");

        try
        {
            auto const total_start = std::chrono::steady_clock::now();
            auto const buffer_start = std::chrono::steady_clock::now();
            payload_buffer.resize(payload_buffer_bytes);
            uint64_t const buffer_alloc_us = elapsedMicros(buffer_start);

            auto const expose_start = std::chrono::steady_clock::now();
            std::vector<std::pair<void*, std::size_t>> segments(1);
            segments[0].first = static_cast<void*>(payload_buffer.data());
            segments[0].second = payload_buffer.size();
            tl::bulk remote_payload = localEngine->expose(segments, tl::bulk_mode::write_only);
            uint64_t const bulk_expose_us = elapsedMicros(expose_start);

            auto const rpc_start = std::chrono::steady_clock::now();
            packed = tail_events_since_packed_bulk_stream.on(service_ph)(story_id,
                                                                         cursor,
                                                                         remote_payload,
                                                                         static_cast<uint64_t>(max_batches))
                             .as<KeeperTailPackedBatch>();
            uint64_t const rpc_us = elapsedMicros(rpc_start);

            auto const move_start = std::chrono::steady_clock::now();
            std::size_t const payload_bytes = packedPayloadBytes(packed);
            if(packed.ok && payload_bytes <= payload_buffer.size())
            {
                payload_buffer.resize(payload_bytes);
                packed.payloadBlob = std::move(payload_buffer);
            }
            else if(packed.ok)
            {
                packed.ok = false;
            }
            uint64_t const payload_move_us = elapsedMicros(move_start);
            uint64_t const total_us = elapsedMicros(total_start);
            int const status = packed.ok ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN;
            recordTailPackedRpcStats("incremental_packed_bulk_stream",
                                     status,
                                     packed,
                                     rpc_us,
                                     buffer_alloc_us,
                                     bulk_expose_us,
                                     payload_move_us,
                                     total_us);
            return status;
        }
        catch(thallium::exception const& ex)
        {
            LOG_DEBUG("[KeeperRecordingClient] Failed stream bulk packed tail cursor request to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        recordTailPackedRpcStats("incremental_packed_bulk_stream", chronolog::CL_ERR_UNKNOWN, packed, 0);
        return chronolog::CL_ERR_UNKNOWN;
    }

    int send_tail_since_packed_bulk_request(StoryId const& story_id,
                                            KeeperTailCursorToken const& cursor,
                                            std::size_t payload_buffer_bytes,
                                            std::string payload_buffer,
                                            KeeperTailPackedBatch& packed)
    {
        CL_PROFILE_REGION("client_query");
        CL_PROFILE_REGION("client_keeper_tail_cursor_rpc");
        CL_PROFILE_REGION("client_keeper_tail_cursor_packed_bulk");

        try
        {
            auto const total_start = std::chrono::steady_clock::now();
            auto const buffer_start = std::chrono::steady_clock::now();
            payload_buffer.resize(payload_buffer_bytes);
            uint64_t const buffer_alloc_us = elapsedMicros(buffer_start);

            auto const expose_start = std::chrono::steady_clock::now();
            std::vector<std::pair<void*, std::size_t>> segments(1);
            segments[0].first = static_cast<void*>(payload_buffer.data());
            segments[0].second = payload_buffer.size();
            tl::bulk remote_payload = localEngine->expose(segments, tl::bulk_mode::write_only);
            uint64_t const bulk_expose_us = elapsedMicros(expose_start);

            auto const rpc_start = std::chrono::steady_clock::now();
            packed = tail_events_since_packed_bulk.on(service_ph)(story_id, cursor, remote_payload)
                             .as<KeeperTailPackedBatch>();
            uint64_t const rpc_us = elapsedMicros(rpc_start);

            auto const move_start = std::chrono::steady_clock::now();
            std::size_t const payload_bytes = packedPayloadBytes(packed);
            if(packed.ok && payload_bytes <= payload_buffer.size())
            {
                payload_buffer.resize(payload_bytes);
                packed.payloadBlob = std::move(payload_buffer);
            }
            else if(packed.ok)
            {
                packed.ok = false;
            }
            uint64_t const payload_move_us = elapsedMicros(move_start);
            uint64_t const total_us = elapsedMicros(total_start);
            int const status = packed.ok ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN;
            recordTailPackedRpcStats("incremental_packed_bulk",
                                     status,
                                     packed,
                                     rpc_us,
                                     buffer_alloc_us,
                                     bulk_expose_us,
                                     payload_move_us,
                                     total_us);
            return status;
        }
        catch(thallium::exception const& ex)
        {
            LOG_DEBUG("[KeeperRecordingClient] Failed bulk packed tail cursor request to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        recordTailPackedRpcStats("incremental_packed_bulk", chronolog::CL_ERR_UNKNOWN, packed, 0);
        return chronolog::CL_ERR_UNKNOWN;
    }

    ServiceId const& getRecordingServiceId() const { return recordingServiceId; }

    ~KeeperRecordingClient()
    {
        record_event.deregister();
        record_events.deregister();
        tail_events.deregister();
        tail_events_since.deregister();
        LOG_DEBUG("[KeeperRecordingClient] Destructor called {}", to_string(recordingServiceId));
    }

private:
    static bool tailRpcStatsEnabled()
    {
        char const* value = std::getenv("CHRONOLOG_CLIENT_TAIL_RPC_STATS");
        return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
                                    std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0);
    }

    static uint64_t elapsedMicros(std::chrono::steady_clock::time_point const& start)
    {
        return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                        .count());
    }

    void recordTailRpcStats(char const* mode,
                            int status,
                            bool ok,
                            std::vector<LogEvent> const& events,
                            uint64_t rpc_us) const
    {
        if(!tailRpcStatsEnabled())
        {
            return;
        }
        uint64_t payload_bytes = 0;
        for(auto const& event: events)
        {
            payload_bytes += event.logRecord.size();
        }
        LOG_INFO("[ClientTailRpcStats] mode={} service={} status={} ok={} event_count={} payload_bytes={} rpc_us={}",
                 mode,
                 to_string(recordingServiceId),
                 status,
                 ok ? 1 : 0,
                 events.size(),
                 payload_bytes,
                 rpc_us);
        std::cerr << "[ClientTailRpcStats] mode=" << mode << " service=" << to_string(recordingServiceId)
                  << " status=" << status << " ok=" << (ok ? 1 : 0) << " event_count=" << events.size()
                  << " payload_bytes=" << payload_bytes << " rpc_us=" << rpc_us << std::endl;
    }

    void recordTailPackedRpcStats(char const* mode,
                                  int status,
                                  KeeperTailPackedBatch const& packed,
                                  uint64_t rpc_us,
                                  uint64_t buffer_alloc_us = 0,
                                  uint64_t bulk_expose_us = 0,
                                  uint64_t payload_move_us = 0,
                                  uint64_t total_us = 0) const
    {
        if(!tailRpcStatsEnabled())
        {
            return;
        }
        std::size_t const count = packed.eventTimes.size();
        bool const valid = packed.clientIds.size() == count && packed.eventIndexes.size() == count &&
                           packed.payloadOffsets.size() == count && packed.payloadSizes.size() == count;
        LOG_INFO("[ClientTailRpcStats] mode={} service={} status={} ok={} event_count={} payload_bytes={} rpc_us={} "
                 "buffer_alloc_us={} bulk_expose_us={} payload_move_us={} total_us={}",
                 mode,
                 to_string(recordingServiceId),
                 status,
                 packed.ok && valid ? 1 : 0,
                 valid ? count : 0,
                 valid ? packed.payloadBlob.size() : 0,
                 rpc_us,
                 buffer_alloc_us,
                 bulk_expose_us,
                 payload_move_us,
                 total_us);
        std::cerr << "[ClientTailRpcStats] mode=" << mode << " service=" << to_string(recordingServiceId)
                  << " status=" << status << " ok=" << (packed.ok && valid ? 1 : 0)
                  << " event_count=" << (valid ? count : 0)
                  << " payload_bytes=" << (valid ? packed.payloadBlob.size() : 0)
                  << " rpc_us=" << rpc_us
                  << " buffer_alloc_us=" << buffer_alloc_us
                  << " bulk_expose_us=" << bulk_expose_us
                  << " payload_move_us=" << payload_move_us
                  << " total_us=" << total_us << std::endl;
    }

    class AsyncRecordState: public LogEventFuture::State
    {
    public:
        AsyncRecordState(tl::async_response response,
                         uint64_t event_time,
                         ServiceId service_id,
                         std::shared_ptr<void> rpc_payload_keep_alive = nullptr)
            : rpcPayloadKeepAlive(std::move(rpc_payload_keep_alive))
            , response(std::move(response))
            , eventTime(event_time)
            , recordingServiceId(std::move(service_id))
        {}

        uint64_t wait() override
        {
            try
            {
                int const return_code = response.wait().as<int>();
                return return_code == chronolog::CL_SUCCESS ? eventTime : 0;
            }
            catch(thallium::exception const& ex)
            {
                LOG_ERROR("[KeeperRecordingClient] Failed waiting for async event response from {} exception: {}",
                          to_string(recordingServiceId),
                          ex.what());
            }
            return 0;
        }

    private:
        std::shared_ptr<void> rpcPayloadKeepAlive;
        tl::async_response response;
        uint64_t eventTime;
        ServiceId recordingServiceId;
    };

    ServiceId recordingServiceId;
    tl::provider_handle service_ph; //provider_handle for remote registry service
    tl::remote_procedure record_event;
    tl::remote_procedure record_events;
    tl::remote_procedure tail_events;
    tl::remote_procedure tail_events_since;
    tl::remote_procedure tail_events_since_packed;
    tl::remote_procedure tail_events_since_packed_bulk;
    tl::remote_procedure tail_events_since_packed_bulk_stream;
    tl::engine* localEngine{nullptr};

    static std::size_t packedPayloadBytes(KeeperTailPackedBatch const& packed)
    {
        return std::accumulate(packed.payloadSizes.begin(),
                               packed.payloadSizes.end(),
                               std::size_t{0},
                               [](std::size_t total, uint64_t size) {
                                   return total + static_cast<std::size_t>(size);
                               });
    }

    static void unpackTailBatch(StoryId const& story_id, KeeperTailPackedBatch&& packed, KeeperTailBatch& batch)
    {
        std::size_t const count = packed.eventTimes.size();
        if(packed.clientIds.size() != count || packed.eventIndexes.size() != count ||
           packed.payloadOffsets.size() != count || packed.payloadSizes.size() != count)
        {
            batch.ok = false;
            return;
        }

        batch.events.clear();
        batch.events.reserve(count);
        for(std::size_t index = 0; index < count; ++index)
        {
            uint64_t const offset = packed.payloadOffsets[index];
            uint64_t const size = packed.payloadSizes[index];
            if(offset > packed.payloadBlob.size() || size > packed.payloadBlob.size() - offset)
            {
                batch.ok = false;
                batch.events.clear();
                return;
            }
            batch.events.emplace_back(story_id,
                                      packed.eventTimes[index],
                                      packed.clientIds[index],
                                      packed.eventIndexes[index],
                                      packed.payloadBlob.substr(static_cast<std::size_t>(offset),
                                                                static_cast<std::size_t>(size)));
        }
    }

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient(tl::engine& tl_engine, ServiceId const& keeper_service_id)
        : recordingServiceId(keeper_service_id)
        , localEngine(&tl_engine)
    {
        LOG_DEBUG("[KeeperRecordingClient] KeeperRecordingiClient Constructor for {}", to_string(keeper_service_id));
        std::string service_addr_string;
        recordingServiceId.get_service_as_string(service_addr_string);

        service_ph = tl::provider_handle(tl_engine.lookup(service_addr_string), recordingServiceId.getProviderId());

        record_event = tl_engine.define("record_event");
        record_events = tl_engine.define("record_events");
        tail_events = tl_engine.define("tail_events");
        tail_events_since = tl_engine.define("tail_events_since");
        tail_events_since_packed = tl_engine.define("tail_events_since_packed");
        tail_events_since_packed_bulk = tl_engine.define("tail_events_since_packed_bulk");
        tail_events_since_packed_bulk_stream = tl_engine.define("tail_events_since_packed_bulk_stream");
    }


    KeeperRecordingClient() = delete;
    KeeperRecordingClient(KeeperRecordingClient const&) = delete;
    KeeperRecordingClient& operator=(KeeperRecordingClient const&) = delete;
};
} // namespace chronolog

#endif
