#ifndef KEEPER_LOCAL_JOURNAL_H
#define KEEPER_LOCAL_JOURNAL_H

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <chrono_monitor.h>
#include <chronolog_types.h>

#include "KeeperAppendStats.h"

namespace chronolog
{

class KeeperLocalJournal
{
public:
    struct WalRecordCursor
    {
        bool valid{false};
        uint64_t story_id{0};
        uint64_t event_time{0};
        uint64_t client_id{0};
        uint32_t event_index{0};
        std::size_t segment_index{0};
        uint64_t payload_offset{0};
        uint64_t payload_size{0};
    };

    struct WalDrainReadStats
    {
        uint64_t batch_count{0};
        uint64_t physical_read_bytes{0};
        uint64_t gap_bytes{0};
        uint64_t read_syscall_count{0};
        uint64_t max_batch_records{0};
    };

    using AppendCallback = std::function<void(bool, WalRecordCursor const&)>;
    using AppendBatchCallback = std::function<void(bool)>;

    struct AppendBatchCompletion
    {
        AppendBatchCompletion(std::size_t count, AppendBatchCallback callback)
            : remaining(count)
            , callback(std::move(callback))
        {}

        void complete(std::size_t count, bool completed_ok)
        {
            if(count == 0)
            {
                return;
            }
            if(!completed_ok)
            {
                ok.store(false, std::memory_order_relaxed);
            }
            std::size_t const previous = remaining.fetch_sub(count, std::memory_order_acq_rel);
            KeeperAppendStats::instance().recordJournalBatchCompletion(count, previous, previous <= count);
            if(previous <= count && callback)
            {
                callback(ok.load(std::memory_order_relaxed));
            }
        }

        std::atomic<std::size_t> remaining;
        std::atomic<bool> ok{true};
        AppendBatchCallback callback;
    };

    static KeeperLocalJournal& instance()
    {
        static KeeperLocalJournal journal;
        return journal;
    }

    bool enabled() const { return journalMode != Mode::Disabled; }

    void beginActiveAdmission()
    {
        if(!groupCommitWaitForActiveAdmissions)
        {
            return;
        }
        activeAdmissions.fetch_add(1, std::memory_order_relaxed);
    }

    void endActiveAdmission()
    {
        if(!groupCommitWaitForActiveAdmissions)
        {
            return;
        }
        if(activeAdmissions.fetch_sub(1, std::memory_order_relaxed) == 1)
        {
            notifyAdmissionBoundary();
        }
    }

    char const* modeName() const
    {
        switch(journalMode)
        {
            case Mode::Buffered:
                return "buffered";
            case Mode::FdSync:
                return "fdatasync";
            case Mode::FdSyncBatch:
                return "fdatasync_batch";
            case Mode::Disabled:
            default:
                return "disabled";
        }
    }

    bool append(LogEvent const& event, WalRecordCursor* cursor = nullptr)
    {
        if(!enabled())
        {
            if(cursor != nullptr)
            {
                *cursor = WalRecordCursor{};
            }
            return true;
        }

        if(!ensureOpen())
        {
            return false;
        }

        RecordHeader header;
        header.story_id = event.storyId;
        header.event_time = event.eventTime;
        header.client_id = event.clientId;
        header.event_index = event.eventIndex;
        header.payload_size = static_cast<uint64_t>(event.logRecord.size());

        JournalShard& shard = selectShard(event);
        if(singleWriterShards)
        {
            auto work = std::make_shared<JournalWork>();
            work->type = JournalWork::Type::Append;
            work->header = header;
            work->payload = &event.logRecord;
            work->enqueue_time_ns = KeeperAppendStats::nowNs();
            auto completion = work->completion.get_future();
            if(enqueueWork(shard, work))
            {
                shard.workAvailable.notify_one();
            }
            AppendCompletion const completed = completion.get();
            if(cursor != nullptr)
            {
                *cursor = toWalRecordCursor(completed.entry, completed.ok);
            }
            return completed.ok;
        }

        uint64_t lock_wait_ns = 0;
        uint64_t record_offset = 0;
        if(atomicOffsets)
        {
            record_offset = shard.nextWriteOffset.fetch_add(sizeof(header) + header.payload_size,
                                                            std::memory_order_relaxed);
        }
        else
        {
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            shard.mutex.lock();
            lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;

            off_t const offset = ::lseek(shard.fd, 0, SEEK_END);
            if(offset < 0)
            {
                LOG_ERROR("[KeeperLocalJournal] lseek failed for {}: {}", shard.path, std::strerror(errno));
                shard.mutex.unlock();
                return false;
            }
            record_offset = static_cast<uint64_t>(offset);
        }

        uint64_t const write_start_ns = KeeperAppendStats::nowNs();
        bool write_ok = false;
        if(atomicOffsets)
        {
            write_ok = pwriteFull(shard.fd, shard.path, &header, sizeof(header), record_offset) &&
                       (header.payload_size == 0 ||
                        pwriteFull(shard.fd,
                                   shard.path,
                                   event.logRecord.data(),
                                   event.logRecord.size(),
                                   record_offset + sizeof(header)));
        }
        else
        {
            write_ok = writeFull(shard.fd, shard.path, &header, sizeof(header)) &&
                       (header.payload_size == 0 ||
                        writeFull(shard.fd, shard.path, event.logRecord.data(), event.logRecord.size()));
        }
        if(!write_ok)
        {
            if(!atomicOffsets)
            {
                shard.mutex.unlock();
            }
            return false;
        }
        uint64_t const write_ns = KeeperAppendStats::nowNs() - write_start_ns;

        std::unique_lock<std::mutex> lock(shard.mutex, std::defer_lock);
        if(atomicOffsets)
        {
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            lock.lock();
            lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;
        }
        else
        {
            lock = std::unique_lock<std::mutex>(shard.mutex, std::adopt_lock);
        }

        uint64_t fdatasync_ns = 0;
        if(journalMode == Mode::FdSync || journalMode == Mode::FdSyncBatch)
        {
            ++shard.recordsSinceSync;
            bool const should_sync = journalMode == Mode::FdSync || shard.recordsSinceSync >= fdatasyncBatchEvents;
            if(should_sync)
            {
                uint64_t const fdatasync_start_ns = KeeperAppendStats::nowNs();
                if(::fdatasync(shard.fd) != 0)
                {
                    LOG_ERROR("[KeeperLocalJournal] fdatasync failed for {}: {}", shard.path, std::strerror(errno));
                    return false;
                }
                fdatasync_ns = KeeperAppendStats::nowNs() - fdatasync_start_ns;
                shard.recordsSinceSync = 0;
            }
        }

        uint64_t const publish_start_ns = KeeperAppendStats::nowNs();
        TailIndexEntry entry{header.story_id,
                             header.event_time,
                             header.client_id,
                             header.event_index,
                             shard.segmentIndex,
                             static_cast<uint64_t>(record_offset + sizeof(header)),
                             header.payload_size};
        if(cursor != nullptr)
        {
            *cursor = toWalRecordCursor(entry, true);
        }
        appendTailIndexEntry(shard, std::move(entry));
        if(journalMode == Mode::FdSync)
        {
            KeeperAppendStats::instance().recordJournalDurablePublish(1);
        }
        KeeperAppendStats::instance().recordJournalAppend(lock_wait_ns,
                                                          write_ns,
                                                          fdatasync_ns,
                                                          KeeperAppendStats::nowNs() - publish_start_ns,
                                                          header.payload_size);
        return true;
    }

    bool appendAsync(LogEvent const& event, AppendCallback callback)
    {
        if(!enabled())
        {
            if(callback)
            {
                callback(true, WalRecordCursor{});
            }
            return true;
        }
        if(!singleWriterShards || !ensureOpen())
        {
            return false;
        }

        AdmissionScope admission_scope(*this);
        RecordHeader header;
        header.story_id = event.storyId;
        header.event_time = event.eventTime;
        header.client_id = event.clientId;
        header.event_index = event.eventIndex;
        header.payload_size = static_cast<uint64_t>(event.logRecord.size());

        JournalShard& shard = selectShard(event);
        auto work = std::make_shared<JournalWork>();
        work->type = JournalWork::Type::Append;
        work->header = header;
        work->ownedPayload = event.logRecord;
        work->payload = &work->ownedPayload;
        work->enqueue_time_ns = KeeperAppendStats::nowNs();
        work->callback = std::move(callback);
        if(enqueueWork(shard, work))
        {
            shard.workAvailable.notify_one();
        }
        return true;
    }

    bool appendAsyncBatch(std::vector<LogEvent> const& events, std::vector<AppendCallback> callbacks)
    {
        if(events.size() != callbacks.size())
        {
            return false;
        }
        if(!enabled())
        {
            for(auto& callback: callbacks)
            {
                if(callback)
                {
                    callback(true, WalRecordCursor{});
                }
            }
            return true;
        }
        if(!singleWriterShards || !ensureOpen())
        {
            return false;
        }

        AdmissionScope admission_scope(*this);
        uint64_t const admission_start_ns = KeeperAppendStats::nowNs();
        uint64_t admission_payload_bytes = 0;
        std::unordered_map<JournalShard*, std::vector<std::shared_ptr<JournalWork>>> work_by_shard;
        work_by_shard.reserve(events.size());
        for(std::size_t index = 0; index < events.size(); ++index)
        {
            auto const& event = events[index];
            RecordHeader header;
            header.story_id = event.storyId;
            header.event_time = event.eventTime;
            header.client_id = event.clientId;
            header.event_index = event.eventIndex;
            header.payload_size = static_cast<uint64_t>(event.logRecord.size());
            admission_payload_bytes += header.payload_size;

            auto work = std::make_shared<JournalWork>();
            work->type = JournalWork::Type::Append;
            work->header = header;
            work->ownedPayload = event.logRecord;
            work->payload = &work->ownedPayload;
            work->enqueue_time_ns = KeeperAppendStats::nowNs();
            work->callback = std::move(callbacks[index]);
            work_by_shard[&selectShard(event)].push_back(std::move(work));
        }

        uint64_t const admission_enqueue_start_ns = KeeperAppendStats::nowNs();
        for(auto& shard_work: work_by_shard)
        {
            JournalShard& shard = *shard_work.first;
            if(enqueueWorkBatch(shard, shard_work.second))
            {
                shard.workAvailable.notify_one();
            }
        }
        KeeperAppendStats::instance().recordJournalAdmission(events.size(),
                                                             admission_payload_bytes,
                                                             work_by_shard.size(),
                                                             admission_enqueue_start_ns - admission_start_ns,
                                                             KeeperAppendStats::nowNs() -
                                                                     admission_enqueue_start_ns);
        return true;
    }

    bool appendAsyncBatch(std::vector<LogEvent> const& events, AppendBatchCallback callback)
    {
        if(events.empty())
        {
            if(callback)
            {
                callback(true);
            }
            return true;
        }
        if(!enabled())
        {
            if(callback)
            {
                callback(true);
            }
            return true;
        }
        if(!singleWriterShards || !ensureOpen())
        {
            return false;
        }

        auto batch_completion = std::make_shared<AppendBatchCompletion>(events.size(), std::move(callback));
        AdmissionScope admission_scope(*this);
        uint64_t const admission_start_ns = KeeperAppendStats::nowNs();
        uint64_t admission_payload_bytes = 0;
        std::unordered_map<JournalShard*, std::vector<std::shared_ptr<JournalWork>>> work_by_shard;
        work_by_shard.reserve(events.size());
        for(auto const& event: events)
        {
            RecordHeader header;
            header.story_id = event.storyId;
            header.event_time = event.eventTime;
            header.client_id = event.clientId;
            header.event_index = event.eventIndex;
            header.payload_size = static_cast<uint64_t>(event.logRecord.size());
            admission_payload_bytes += header.payload_size;

            auto work = std::make_shared<JournalWork>();
            work->type = JournalWork::Type::Append;
            work->header = header;
            work->ownedPayload = event.logRecord;
            work->payload = &work->ownedPayload;
            work->enqueue_time_ns = KeeperAppendStats::nowNs();
            work->batchCompletion = batch_completion;
            work_by_shard[&selectShard(event)].push_back(std::move(work));
        }

        uint64_t const admission_enqueue_start_ns = KeeperAppendStats::nowNs();
        for(auto& shard_work: work_by_shard)
        {
            JournalShard& shard = *shard_work.first;
            if(enqueueWorkBatch(shard, shard_work.second))
            {
                shard.workAvailable.notify_one();
            }
        }
        KeeperAppendStats::instance().recordJournalAdmission(events.size(),
                                                             admission_payload_bytes,
                                                             work_by_shard.size(),
                                                             admission_enqueue_start_ns - admission_start_ns,
                                                             KeeperAppendStats::nowNs() -
                                                                     admission_enqueue_start_ns);
        return true;
    }

    bool appendAsyncBatch(std::vector<LogEvent>&& events, AppendBatchCallback callback)
    {
        if(events.empty())
        {
            if(callback)
            {
                callback(true);
            }
            return true;
        }
        if(!enabled())
        {
            if(callback)
            {
                callback(true);
            }
            return true;
        }
        if(!singleWriterShards || !ensureOpen())
        {
            return false;
        }

        auto batch_completion = std::make_shared<AppendBatchCompletion>(events.size(), std::move(callback));
        AdmissionScope admission_scope(*this);
        uint64_t const admission_start_ns = KeeperAppendStats::nowNs();
        uint64_t admission_payload_bytes = 0;
        std::unordered_map<JournalShard*, std::vector<std::shared_ptr<JournalWork>>> work_by_shard;
        work_by_shard.reserve(events.size());
        for(auto& event: events)
        {
            JournalShard* shard = &selectShard(event);
            RecordHeader header;
            header.story_id = event.storyId;
            header.event_time = event.eventTime;
            header.client_id = event.clientId;
            header.event_index = event.eventIndex;
            header.payload_size = static_cast<uint64_t>(event.logRecord.size());
            admission_payload_bytes += header.payload_size;

            auto work = std::make_shared<JournalWork>();
            work->type = JournalWork::Type::Append;
            work->header = header;
            work->ownedPayload = std::move(event.logRecord);
            work->payload = &work->ownedPayload;
            work->enqueue_time_ns = KeeperAppendStats::nowNs();
            work->batchCompletion = batch_completion;
            work_by_shard[shard].push_back(std::move(work));
        }

        uint64_t const admission_enqueue_start_ns = KeeperAppendStats::nowNs();
        for(auto& shard_work: work_by_shard)
        {
            JournalShard& shard = *shard_work.first;
            if(enqueueWorkBatch(shard, shard_work.second))
            {
                shard.workAvailable.notify_one();
            }
        }
        KeeperAppendStats::instance().recordJournalAdmission(events.size(),
                                                             admission_payload_bytes,
                                                             work_by_shard.size(),
                                                             admission_enqueue_start_ns - admission_start_ns,
                                                             KeeperAppendStats::nowNs() -
                                                                     admission_enqueue_start_ns);
        return true;
    }

    bool readRecord(WalRecordCursor const& cursor, LogEvent& event)
    {
        if(!cursor.valid)
        {
            return false;
        }
        if(!ensureOpen())
        {
            return false;
        }
        if(cursor.segment_index >= journalSegments.size())
        {
            LOG_ERROR("[KeeperLocalJournal] invalid WAL cursor segment {}", cursor.segment_index);
            return false;
        }

        auto const& segment = journalSegments[cursor.segment_index];
        std::string payload(cursor.payload_size, '\0');
        if(cursor.payload_size > 0 &&
           !preadFull(segment.fd, segment.path, payload.data(), payload.size(), cursor.payload_offset))
        {
            return false;
        }
        event = LogEvent(cursor.story_id,
                         cursor.event_time,
                         cursor.client_id,
                         cursor.event_index,
                         std::move(payload));
        return true;
    }

    bool readRecordsCoalesced(std::vector<WalRecordCursor> const& cursors,
                              std::vector<LogEvent>& events,
                              WalDrainReadStats* stats = nullptr)
    {
        events.clear();
        if(cursors.empty())
        {
            return true;
        }
        if(!ensureOpen())
        {
            return false;
        }

        std::vector<TailIndexEntry> entries;
        entries.reserve(cursors.size());
        for(auto const& cursor: cursors)
        {
            if(!cursor.valid)
            {
                return false;
            }
            if(cursor.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid WAL cursor segment {}", cursor.segment_index);
                return false;
            }
            entries.push_back(TailIndexEntry{cursor.story_id,
                                             cursor.event_time,
                                             cursor.client_id,
                                             cursor.event_index,
                                             cursor.segment_index,
                                             cursor.payload_offset,
                                             cursor.payload_size});
        }

        std::vector<std::string> payloads;
        if(!readPayloadsVectored(entries, payloads, stats, nullptr))
        {
            return false;
        }

        events.reserve(entries.size());
        for(std::size_t index = 0; index < entries.size(); ++index)
        {
            auto const& entry = entries[index];
            events.emplace_back(entry.story_id,
                                entry.event_time,
                                entry.client_id,
                                entry.event_index,
                                std::move(payloads[index]));
        }
        return true;
    }

    bool collectEvents(StoryId const& story_id,
                       chrono_time const& start_time,
                       chrono_time const& end_time,
                       std::vector<LogEvent>& events)
    {
        if(!enabled())
        {
            return false;
        }

        std::vector<TailIndexEntry> matches;
        uint64_t const snapshot_start_ns = KeeperAppendStats::nowNs();
        uint64_t snapshot_lock_wait_ns = 0;
        if(!ensureOpen())
        {
            return false;
        }
        {
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            std::lock_guard<std::mutex> lock(journalMutex);
            snapshot_lock_wait_ns += KeeperAppendStats::nowNs() - lock_wait_start_ns;
            auto recovered_iter = recoveredTailIndexByStory.find(story_id);
            if(recovered_iter != recoveredTailIndexByStory.end())
            {
                matches.reserve(recovered_iter->second.size());
                for(auto const& entry: recovered_iter->second)
                {
                    if(entry.event_time >= start_time && entry.event_time < end_time)
                    {
                        matches.push_back(entry);
                    }
                }
            }
        }

        for(auto const& shard_ptr: appendShards)
        {
            JournalShard& shard = *shard_ptr;
            std::vector<std::shared_ptr<const std::vector<TailIndexEntry>>> sealed_pages;
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                snapshot_lock_wait_ns += KeeperAppendStats::nowNs() - lock_wait_start_ns;
                if(shard.fd >= 0 && journalMode == Mode::Buffered)
                {
                    ::fsync(shard.fd);
                }
                auto tail_iter = shard.tailIndexByStory.find(story_id);
                if(tail_iter == shard.tailIndexByStory.end())
                {
                    continue;
                }
                sealed_pages = tail_iter->second.sealedPages;
                for(auto const& entry: tail_iter->second.activeEntries)
                {
                    if(entry.event_time >= start_time && entry.event_time < end_time)
                    {
                        matches.push_back(entry);
                    }
                }
            }

            for(auto const& page: sealed_pages)
            {
                if(!page)
                {
                    continue;
                }
                matches.reserve(matches.size() + page->size());
                for(auto const& entry: *page)
                {
                    if(entry.event_time >= start_time && entry.event_time < end_time)
                    {
                        matches.push_back(entry);
                    }
                }
            }
        }
        uint64_t const snapshot_ns = KeeperAppendStats::nowNs() - snapshot_start_ns;

        uint64_t const sort_start_ns = KeeperAppendStats::nowNs();
        std::sort(matches.begin(), matches.end(), [](TailIndexEntry const& lhs, TailIndexEntry const& rhs) {
            return std::tie(lhs.event_time, lhs.client_id, lhs.event_index) <
                   std::tie(rhs.event_time, rhs.client_id, rhs.event_index);
        });
        uint64_t const sort_ns = KeeperAppendStats::nowNs() - sort_start_ns;

        TailReadStats tail_read_stats;
        uint64_t const payload_read_start_ns = KeeperAppendStats::nowNs();
        if(tailDirectReadIntoEvents && directTailReadMode(matches))
        {
            tail_read_stats.direct_read = true;
            if(!readPayloadsDirectToEvents(matches, events, tail_read_stats))
            {
                return false;
            }
        }
        else
        {
            std::vector<std::string> payloads;
            if(!readPayloads(matches, payloads, tail_read_stats))
            {
                return false;
            }
            uint64_t const event_build_start_ns = KeeperAppendStats::nowNs();
            for(std::size_t index = 0; index < matches.size(); ++index)
            {
                auto const& entry = matches[index];
                events.emplace_back(entry.story_id,
                                    entry.event_time,
                                    entry.client_id,
                                    entry.event_index,
                                    std::move(payloads[index]));
            }
            tail_read_stats.event_build_ns += KeeperAppendStats::nowNs() - event_build_start_ns;
        }
        KeeperAppendStats::instance().recordJournalTail(snapshot_ns,
                                                        snapshot_lock_wait_ns,
                                                        sort_ns,
                                                        KeeperAppendStats::nowNs() - payload_read_start_ns,
                                                        tail_read_stats.payload_alloc_ns,
                                                        tail_read_stats.payload_syscall_ns,
                                                        tail_read_stats.payload_memcpy_ns,
                                                        tail_read_stats.event_build_ns,
                                                        matches.size(),
                                                        tail_read_stats.read_group_count,
                                                        tail_read_stats.physical_read_bytes,
                                                        tail_read_stats.payload_copy_bytes,
                                                        tail_read_stats.direct_read,
                                                        tail_read_stats.vectored_read);
        return true;
    }

    bool collectEventsAfter(StoryId const& story_id,
                            KeeperTailCursorToken const& cursor,
                            KeeperTailBatch& batch)
    {
        batch.events.clear();
        batch.nextCursor = cursor;
        batch.ok = false;

        if(!enabled())
        {
            return false;
        }

        std::vector<TailIndexEntry> matches;
        uint64_t const snapshot_start_ns = KeeperAppendStats::nowNs();
        uint64_t snapshot_lock_wait_ns = 0;
        if(!ensureOpen())
        {
            return false;
        }

        auto after_cursor = [&cursor](TailIndexEntry const& entry) {
            if(!cursor.initialized)
            {
                return true;
            }
            return std::tie(entry.segment_index, entry.payload_offset) >
                   std::tie(cursor.segmentIndex, cursor.payloadOffset);
        };

        {
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            std::lock_guard<std::mutex> lock(journalMutex);
            snapshot_lock_wait_ns += KeeperAppendStats::nowNs() - lock_wait_start_ns;
            auto recovered_iter = recoveredTailIndexByStory.find(story_id);
            if(recovered_iter != recoveredTailIndexByStory.end())
            {
                matches.reserve(recovered_iter->second.size());
                for(auto const& entry: recovered_iter->second)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }
        }

        for(auto const& shard_ptr: appendShards)
        {
            JournalShard& shard = *shard_ptr;
            std::vector<std::shared_ptr<const std::vector<TailIndexEntry>>> sealed_pages;
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                snapshot_lock_wait_ns += KeeperAppendStats::nowNs() - lock_wait_start_ns;
                if(shard.fd >= 0 && journalMode == Mode::Buffered)
                {
                    ::fsync(shard.fd);
                }
                auto tail_iter = shard.tailIndexByStory.find(story_id);
                if(tail_iter == shard.tailIndexByStory.end())
                {
                    continue;
                }
                sealed_pages = tail_iter->second.sealedPages;
                for(auto const& entry: tail_iter->second.activeEntries)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }

            for(auto const& page: sealed_pages)
            {
                if(!page)
                {
                    continue;
                }
                matches.reserve(matches.size() + page->size());
                for(auto const& entry: *page)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }
        }
        uint64_t const snapshot_ns = KeeperAppendStats::nowNs() - snapshot_start_ns;

        uint64_t const sort_start_ns = KeeperAppendStats::nowNs();
        std::sort(matches.begin(), matches.end(), [](TailIndexEntry const& lhs, TailIndexEntry const& rhs) {
            return std::tie(lhs.segment_index, lhs.payload_offset) < std::tie(rhs.segment_index, rhs.payload_offset);
        });
        uint64_t const sort_ns = KeeperAppendStats::nowNs() - sort_start_ns;
        applyTailBatchLimit(matches);

        TailReadStats tail_read_stats;
        uint64_t const payload_read_start_ns = KeeperAppendStats::nowNs();
        if(tailDirectReadIntoEvents && directTailReadMode(matches))
        {
            tail_read_stats.direct_read = true;
            if(!readPayloadsDirectToBatch(matches, batch, tail_read_stats))
            {
                return false;
            }
        }
        else
        {
            std::vector<std::string> payloads;
            if(!readPayloads(matches, payloads, tail_read_stats))
            {
                return false;
            }

            batch.events.reserve(matches.size());
            uint64_t const event_build_start_ns = KeeperAppendStats::nowNs();
            for(std::size_t index = 0; index < matches.size(); ++index)
            {
                auto const& entry = matches[index];
                batch.events.emplace_back(entry.story_id,
                                          entry.event_time,
                                          entry.client_id,
                                          entry.event_index,
                                          std::move(payloads[index]));
                batch.nextCursor.segmentIndex = static_cast<uint64_t>(entry.segment_index);
                batch.nextCursor.payloadOffset = entry.payload_offset;
                batch.nextCursor.initialized = true;
            }
            tail_read_stats.event_build_ns += KeeperAppendStats::nowNs() - event_build_start_ns;
        }
        batch.ok = true;

        KeeperAppendStats::instance().recordJournalTail(snapshot_ns,
                                                        snapshot_lock_wait_ns,
                                                        sort_ns,
                                                        KeeperAppendStats::nowNs() - payload_read_start_ns,
                                                        tail_read_stats.payload_alloc_ns,
                                                        tail_read_stats.payload_syscall_ns,
                                                        tail_read_stats.payload_memcpy_ns,
                                                        tail_read_stats.event_build_ns,
                                                        matches.size(),
                                                        tail_read_stats.read_group_count,
                                                        tail_read_stats.physical_read_bytes,
                                                        tail_read_stats.payload_copy_bytes,
                                                        tail_read_stats.direct_read,
                                                        tail_read_stats.vectored_read);
        return true;
    }

    bool collectEventsAfterPacked(StoryId const& story_id,
                                  KeeperTailCursorToken const& cursor,
                                  KeeperTailPackedBatch& batch)
    {
        batch.eventTimes.clear();
        batch.clientIds.clear();
        batch.eventIndexes.clear();
        batch.payloadOffsets.clear();
        batch.payloadSizes.clear();
        batch.payloadBlob.clear();
        batch.nextCursor = cursor;
        batch.ok = false;

        if(!enabled())
        {
            return false;
        }

        std::vector<TailIndexEntry> matches;
        uint64_t const snapshot_start_ns = KeeperAppendStats::nowNs();
        uint64_t snapshot_lock_wait_ns = 0;
        if(!ensureOpen())
        {
            return false;
        }

        auto after_cursor = [&cursor](TailIndexEntry const& entry) {
            if(!cursor.initialized)
            {
                return true;
            }
            return std::tie(entry.segment_index, entry.payload_offset) >
                   std::tie(cursor.segmentIndex, cursor.payloadOffset);
        };

        {
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            std::lock_guard<std::mutex> lock(journalMutex);
            snapshot_lock_wait_ns += KeeperAppendStats::nowNs() - lock_wait_start_ns;
            auto recovered_iter = recoveredTailIndexByStory.find(story_id);
            if(recovered_iter != recoveredTailIndexByStory.end())
            {
                matches.reserve(recovered_iter->second.size());
                for(auto const& entry: recovered_iter->second)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }
        }

        for(auto const& shard_ptr: appendShards)
        {
            JournalShard& shard = *shard_ptr;
            std::vector<std::shared_ptr<const std::vector<TailIndexEntry>>> sealed_pages;
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                snapshot_lock_wait_ns += KeeperAppendStats::nowNs() - lock_wait_start_ns;
                if(shard.fd >= 0 && journalMode == Mode::Buffered)
                {
                    ::fsync(shard.fd);
                }
                auto tail_iter = shard.tailIndexByStory.find(story_id);
                if(tail_iter == shard.tailIndexByStory.end())
                {
                    continue;
                }
                sealed_pages = tail_iter->second.sealedPages;
                for(auto const& entry: tail_iter->second.activeEntries)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }

            for(auto const& page: sealed_pages)
            {
                if(!page)
                {
                    continue;
                }
                matches.reserve(matches.size() + page->size());
                for(auto const& entry: *page)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }
        }
        uint64_t const snapshot_ns = KeeperAppendStats::nowNs() - snapshot_start_ns;

        uint64_t const sort_start_ns = KeeperAppendStats::nowNs();
        std::sort(matches.begin(), matches.end(), [](TailIndexEntry const& lhs, TailIndexEntry const& rhs) {
            return std::tie(lhs.segment_index, lhs.payload_offset) < std::tie(rhs.segment_index, rhs.payload_offset);
        });
        uint64_t const sort_ns = KeeperAppendStats::nowNs() - sort_start_ns;
        applyTailBatchLimit(matches);

        TailReadStats tail_read_stats;
        bool const use_vectored = vectoredTailReadMode(matches);
        tail_read_stats.direct_read = !use_vectored;
        tail_read_stats.vectored_read = use_vectored;
        uint64_t const payload_read_start_ns = KeeperAppendStats::nowNs();
        bool const read_ok = use_vectored ? readPayloadsVectoredToPackedBatch(matches, batch, tail_read_stats)
                                          : readPayloadsDirectToPackedBatch(matches, batch, tail_read_stats);
        if(!read_ok)
        {
            return false;
        }
        batch.sourceBatchCount = 1;
        batch.ok = true;

        KeeperAppendStats::instance().recordJournalTail(snapshot_ns,
                                                        snapshot_lock_wait_ns,
                                                        sort_ns,
                                                        KeeperAppendStats::nowNs() - payload_read_start_ns,
                                                        tail_read_stats.payload_alloc_ns,
                                                        tail_read_stats.payload_syscall_ns,
                                                        tail_read_stats.payload_memcpy_ns,
                                                        tail_read_stats.event_build_ns,
                                                        matches.size(),
                                                        tail_read_stats.read_group_count,
                                                        tail_read_stats.physical_read_bytes,
                                                        tail_read_stats.payload_copy_bytes,
                                                        tail_read_stats.direct_read,
                                                        tail_read_stats.vectored_read);
        return true;
    }

    bool collectEventsAfterPackedBulk(StoryId const& story_id,
                                      KeeperTailCursorToken const& cursor,
                                      KeeperTailPackedBatch& batch,
                                      std::unique_ptr<char[]>& payload_storage,
                                      std::size_t& payload_bytes)
    {
        batch.eventTimes.clear();
        batch.clientIds.clear();
        batch.eventIndexes.clear();
        batch.payloadOffsets.clear();
        batch.payloadSizes.clear();
        batch.payloadBlob.clear();
        batch.nextCursor = cursor;
        batch.ok = false;
        payload_storage.reset();
        payload_bytes = 0;

        if(!enabled())
        {
            return false;
        }

        std::vector<TailIndexEntry> matches;
        uint64_t const snapshot_start_ns = KeeperAppendStats::nowNs();
        uint64_t snapshot_lock_wait_ns = 0;
        if(!ensureOpen())
        {
            return false;
        }

        auto after_cursor = [&cursor](TailIndexEntry const& entry) {
            if(!cursor.initialized)
            {
                return true;
            }
            return std::tie(entry.segment_index, entry.payload_offset) >
                   std::tie(cursor.segmentIndex, cursor.payloadOffset);
        };

        {
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            std::lock_guard<std::mutex> lock(journalMutex);
            snapshot_lock_wait_ns += KeeperAppendStats::nowNs() - lock_wait_start_ns;
            auto recovered_iter = recoveredTailIndexByStory.find(story_id);
            if(recovered_iter != recoveredTailIndexByStory.end())
            {
                matches.reserve(recovered_iter->second.size());
                for(auto const& entry: recovered_iter->second)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }
        }

        for(auto const& shard_ptr: appendShards)
        {
            JournalShard& shard = *shard_ptr;
            std::vector<std::shared_ptr<const std::vector<TailIndexEntry>>> sealed_pages;
            uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                snapshot_lock_wait_ns += KeeperAppendStats::nowNs() - lock_wait_start_ns;
                if(shard.fd >= 0 && journalMode == Mode::Buffered)
                {
                    ::fsync(shard.fd);
                }
                auto tail_iter = shard.tailIndexByStory.find(story_id);
                if(tail_iter == shard.tailIndexByStory.end())
                {
                    continue;
                }
                sealed_pages = tail_iter->second.sealedPages;
                for(auto const& entry: tail_iter->second.activeEntries)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }

            for(auto const& page: sealed_pages)
            {
                if(!page)
                {
                    continue;
                }
                matches.reserve(matches.size() + page->size());
                for(auto const& entry: *page)
                {
                    if(after_cursor(entry))
                    {
                        matches.push_back(entry);
                    }
                }
            }
        }
        uint64_t const snapshot_ns = KeeperAppendStats::nowNs() - snapshot_start_ns;

        uint64_t const sort_start_ns = KeeperAppendStats::nowNs();
        std::sort(matches.begin(), matches.end(), [](TailIndexEntry const& lhs, TailIndexEntry const& rhs) {
            return std::tie(lhs.segment_index, lhs.payload_offset) < std::tie(rhs.segment_index, rhs.payload_offset);
        });
        uint64_t const sort_ns = KeeperAppendStats::nowNs() - sort_start_ns;
        applyTailBatchLimit(matches);

        TailReadStats tail_read_stats;
        bool const use_vectored = vectoredTailReadMode(matches);
        tail_read_stats.direct_read = !use_vectored;
        tail_read_stats.vectored_read = use_vectored;
        uint64_t const payload_read_start_ns = KeeperAppendStats::nowNs();
        bool const read_ok = use_vectored
                                      ? readPayloadsVectoredToPackedBulkBuffer(matches,
                                                                               batch,
                                                                               payload_storage,
                                                                               payload_bytes,
                                                                               tail_read_stats)
                                      : readPayloadsDirectToPackedBulkBuffer(matches,
                                                                             batch,
                                                                             payload_storage,
                                                                             payload_bytes,
                                                                             tail_read_stats);
        if(!read_ok)
        {
            return false;
        }
        batch.sourceBatchCount = 1;
        batch.ok = true;

        KeeperAppendStats::instance().recordJournalTail(snapshot_ns,
                                                        snapshot_lock_wait_ns,
                                                        sort_ns,
                                                        KeeperAppendStats::nowNs() - payload_read_start_ns,
                                                        tail_read_stats.payload_alloc_ns,
                                                        tail_read_stats.payload_syscall_ns,
                                                        tail_read_stats.payload_memcpy_ns,
                                                        tail_read_stats.event_build_ns,
                                                        matches.size(),
                                                        tail_read_stats.read_group_count,
                                                        tail_read_stats.physical_read_bytes,
                                                        tail_read_stats.payload_copy_bytes,
                                                        tail_read_stats.direct_read,
                                                        tail_read_stats.vectored_read);
        return true;
    }

    std::string const& path() const { return journalPath; }

    bool syncAll()
    {
        if(!enabled())
        {
            return true;
        }
        if(!ensureOpen())
        {
            return false;
        }
        bool ok = true;
        for(auto const& shard_ptr: appendShards)
        {
            JournalShard& shard = *shard_ptr;
            if(singleWriterShards)
            {
                if(!syncShardOwner(shard))
                {
                    ok = false;
                }
                continue;
            }
            std::lock_guard<std::mutex> lock(shard.mutex);
            if(shard.fd >= 0 && ::fdatasync(shard.fd) != 0)
            {
                LOG_ERROR("[KeeperLocalJournal] fdatasync failed while syncing {}: {}",
                          shard.path,
                          std::strerror(errno));
                ok = false;
            }
            else
            {
                shard.recordsSinceSync = 0;
            }
        }
        return ok;
    }

private:
    enum class Mode
    {
        Disabled,
        Buffered,
        FdSync,
        FdSyncBatch
    };

    enum class TailReadMode
    {
        Auto,
        Coalesced,
        Direct,
        Vectored
    };

    enum class ShardPolicy
    {
        Mixed,
        Client,
        StoryClient,
        Story
    };

    struct RecordHeader
    {
        static constexpr uint64_t kMagic = 0x43484c4b4a4e4c31ULL; // CHLKJNL1

        uint64_t magic{kMagic};
        uint32_t version{1};
        uint32_t header_size{sizeof(RecordHeader)};
        uint64_t story_id{0};
        uint64_t event_time{0};
        uint64_t client_id{0};
        uint32_t event_index{0};
        uint32_t reserved{0};
        uint64_t payload_size{0};
    };

    struct TailIndexEntry
    {
        uint64_t story_id{0};
        uint64_t event_time{0};
        uint64_t client_id{0};
        uint32_t event_index{0};
        std::size_t segment_index{0};
        uint64_t payload_offset{0};
        uint64_t payload_size{0};
    };

    struct TailReadPlanEntry
    {
        TailIndexEntry entry;
        std::size_t output_index{0};
    };

    struct TailReadStats
    {
        uint64_t read_group_count{0};
        uint64_t physical_read_bytes{0};
        uint64_t payload_copy_bytes{0};
        uint64_t payload_alloc_ns{0};
        uint64_t payload_syscall_ns{0};
        uint64_t payload_memcpy_ns{0};
        uint64_t event_build_ns{0};
        bool direct_read{false};
        bool vectored_read{false};
    };

    struct JournalSegment
    {
        std::string path;
        int fd{-1};
        bool writable{false};
    };

    struct AppendCompletion
    {
        bool ok{false};
        TailIndexEntry entry;
    };

    struct JournalWork;

    struct JournalShard
    {
        struct StoryTailIndex
        {
            std::vector<TailIndexEntry> activeEntries;
            std::vector<std::shared_ptr<const std::vector<TailIndexEntry>>> sealedPages;
        };

        std::string path;
        int fd{-1};
        std::size_t segmentIndex{0};
        std::size_t recordsSinceSync{0};
        std::atomic<uint64_t> nextWriteOffset{0};
        std::mutex mutex;
        std::unordered_map<StoryId, StoryTailIndex> tailIndexByStory;
        std::mutex queueMutex;
        std::condition_variable workAvailable;
        std::deque<std::shared_ptr<JournalWork>> workQueue;
        std::thread writerThread;
    };

    struct JournalWork
    {
        enum class Type
        {
            Append,
            Sync,
            Stop
        };

        Type type{Type::Append};
        RecordHeader header;
        std::string const* payload{nullptr};
        std::string ownedPayload;
        uint64_t enqueue_time_ns{0};
        TailIndexEntry completedEntry;
        std::promise<AppendCompletion> completion;
        AppendCallback callback;
        std::shared_ptr<AppendBatchCompletion> batchCompletion;
    };

    struct CallbackWork
    {
        AppendCallback callback;
        std::shared_ptr<AppendBatchCompletion> batchCompletion;
        AppendCompletion completion;
        std::size_t batchCompletionCount{0};
        bool batchCompletionOk{true};
        uint64_t enqueue_time_ns{0};
        uint64_t maxPayloadBytes{0};
    };

    class AdmissionScope
    {
    public:
        explicit AdmissionScope(KeeperLocalJournal& owner)
            : journal(owner)
        {
            journal.beginActiveAdmission();
        }

        ~AdmissionScope() { journal.endActiveAdmission(); }

        AdmissionScope(AdmissionScope const&) = delete;
        AdmissionScope& operator=(AdmissionScope const&) = delete;

    private:
        KeeperLocalJournal& journal;
    };

    KeeperLocalJournal()
        : journalMode(parseMode(std::getenv("CHRONOLOG_KEEPER_JOURNAL_MODE")))
        , tailReadMode(parseTailReadMode(std::getenv("CHRONOLOG_KEEPER_JOURNAL_TAIL_READ_MODE")))
        , tailDirectReadIntoEvents(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS")))
        , tailBatchMaxEvents(parseOptionalSizeLimit(std::getenv("CHRONOLOG_KEEPER_TAIL_BATCH_MAX_EVENTS")))
        , tailBatchMaxBytes(parseOptionalSizeLimit(std::getenv("CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES")))
        , requestedShardCount(parseShardCount(std::getenv("CHRONOLOG_KEEPER_JOURNAL_SHARDS")))
        , shardPolicy(parseShardPolicy(std::getenv("CHRONOLOG_KEEPER_JOURNAL_SHARD_POLICY")))
        , fdatasyncBatchEvents(parseBatchEvents(std::getenv("CHRONOLOG_KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS")))
        , vectoredRecordWrite(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_WRITEV")))
        , batchVectoredRecordWrite(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_BATCH_WRITEV")))
        , atomicOffsets(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_ATOMIC_OFFSETS")))
        , singleWriterShards(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER")))
        , singleWriterBatchEvents(parseBatchEvents(std::getenv("CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER_BATCH_EVENTS")))
        , groupCommitWait(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT")))
        , groupCommitFlushEvents(
              parseBatchEvents(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS")))
        , groupCommitFlushBytes(
              parseByteLimit(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES")))
        , groupCommitStrictFlushEventCap(
              parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP")))
        , groupCommitLargePayloadBytes(
              parseByteLimit(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES")))
        , groupCommitLargePayloadFlushEvents(
              parseBatchEvents(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS")))
        , groupCommitWaitWindowUs(
              parseWaitWindowUs(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US")))
        , groupCommitFlushWaitUs(
              parseWaitWindowUs(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US")))
        , groupCommitQueueBoundaryMinEvents(parseOptionalBatchLimit(
              std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS")))
        , groupCommitQueueBoundaryMinPayloadBytes(parseByteLimit(
              std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES")))
        , groupCommitQueueBoundaryWaitEveryN(parseSampleInterval(
              std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N")))
        , groupCommitWaitForActiveAdmissions(
              parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS")))
        , ownerDrainYields(parseYieldCount(std::getenv("CHRONOLOG_KEEPER_JOURNAL_OWNER_DRAIN_YIELDS")))
        , asyncCallbackDispatch(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH")))
        , asyncBatchCompletionDispatch(
              parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH"), true))
        , callbackDispatchThreadCount(
              parseThreadCount(std::getenv("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS")))
        , callbackBatchDrain(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN")))
        , callbackBatchDrainMax(
              parseOptionalBatchLimit(std::getenv("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX")))
        , callbackBatchDrainMinPayloadBytes(
              parseByteLimit(std::getenv("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES")))
        , durableCompleteBeforePublish(
              parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH")))
        , notifyOwnerOnlyOnEmpty(parseBool(std::getenv("CHRONOLOG_KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY")))
    {}

    ~KeeperLocalJournal()
    {
        stopShardOwners();
        stopCallbackDispatcher();
        if(journalMode == Mode::FdSync || journalMode == Mode::FdSyncBatch)
        {
            for(auto const& shard_ptr: appendShards)
            {
                if(shard_ptr->fd >= 0)
                {
                    ::fdatasync(shard_ptr->fd);
                }
            }
        }
        for(auto& segment: journalSegments)
        {
            if(segment.fd >= 0)
            {
                ::close(segment.fd);
            }
        }
    }

    KeeperLocalJournal(KeeperLocalJournal const&) = delete;
    KeeperLocalJournal& operator=(KeeperLocalJournal const&) = delete;

    static WalRecordCursor toWalRecordCursor(TailIndexEntry const& entry, bool valid)
    {
        return WalRecordCursor{valid,
                               entry.story_id,
                               entry.event_time,
                               entry.client_id,
                               entry.event_index,
                               entry.segment_index,
                               entry.payload_offset,
                               entry.payload_size};
    }

    static Mode parseMode(char const* mode)
    {
        if(mode == nullptr || *mode == '\0' || std::strcmp(mode, "disabled") == 0)
        {
            return Mode::Disabled;
        }
        if(std::strcmp(mode, "buffered") == 0)
        {
            return Mode::Buffered;
        }
        if(std::strcmp(mode, "fdatasync") == 0)
        {
            return Mode::FdSync;
        }
        if(std::strcmp(mode, "fdatasync_batch") == 0 || std::strcmp(mode, "group_fdatasync") == 0)
        {
            return Mode::FdSyncBatch;
        }
        return Mode::Disabled;
    }

    static TailReadMode parseTailReadMode(char const* mode)
    {
        if(mode != nullptr && std::strcmp(mode, "direct") == 0)
        {
            return TailReadMode::Direct;
        }
        if(mode != nullptr && std::strcmp(mode, "coalesced") == 0)
        {
            return TailReadMode::Coalesced;
        }
        if(mode != nullptr && std::strcmp(mode, "vectored") == 0)
        {
            return TailReadMode::Vectored;
        }
        if(mode != nullptr && std::strcmp(mode, "auto") == 0)
        {
            return TailReadMode::Auto;
        }
        return TailReadMode::Auto;
    }

    static bool parseBool(char const* value, bool default_value = false)
    {
        if(value == nullptr)
        {
            return default_value;
        }
        return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
               std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0;
    }

    static bool shouldUseDirectTailReads(std::vector<TailIndexEntry> const& matches)
    {
        if(matches.empty())
        {
            return false;
        }
        uint64_t total_payload_bytes = 0;
        for(auto const& entry: matches)
        {
            total_payload_bytes += entry.payload_size;
        }
        uint64_t const average_payload_bytes = total_payload_bytes / static_cast<uint64_t>(matches.size());
        return average_payload_bytes >= 16 * 1024;
    }

    static bool shouldUseVectoredTailReads(std::vector<TailIndexEntry> const& matches)
    {
        return matches.size() > 1 && shouldUseDirectTailReads(matches);
    }

    bool directTailReadMode(std::vector<TailIndexEntry> const& matches) const
    {
        if(tailReadMode == TailReadMode::Direct)
        {
            return true;
        }
        if(tailReadMode == TailReadMode::Auto)
        {
            return shouldUseDirectTailReads(matches) && !shouldUseVectoredTailReads(matches);
        }
        return false;
    }

    void applyTailBatchLimit(std::vector<TailIndexEntry>& matches) const
    {
        if(matches.empty() || (tailBatchMaxEvents == 0 && tailBatchMaxBytes == 0))
        {
            return;
        }

        uint64_t payload_bytes = 0;
        std::size_t keep_count = 0;
        for(auto const& entry: matches)
        {
            if(tailBatchMaxEvents > 0 && keep_count >= tailBatchMaxEvents)
            {
                break;
            }
            if(tailBatchMaxBytes > 0 && keep_count > 0 && payload_bytes + entry.payload_size > tailBatchMaxBytes)
            {
                break;
            }
            payload_bytes += entry.payload_size;
            ++keep_count;
            if(tailBatchMaxBytes > 0 && payload_bytes >= tailBatchMaxBytes)
            {
                break;
            }
        }
        if(keep_count > 0 && keep_count < matches.size())
        {
            matches.resize(keep_count);
        }
    }

    static std::size_t parseOptionalSizeLimit(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 0;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        if(end == value)
        {
            return 0;
        }
        return static_cast<std::size_t>(parsed);
    }

    bool vectoredTailReadMode(std::vector<TailIndexEntry> const& matches) const
    {
        if(tailReadMode == TailReadMode::Vectored)
        {
            return true;
        }
        if(tailReadMode == TailReadMode::Auto)
        {
            return shouldUseVectoredTailReads(matches);
        }
        return false;
    }

    static std::size_t parseShardCount(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 1;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        if(end == value || parsed == 0)
        {
            return 1;
        }
        if(parsed > 1024)
        {
            return 1024;
        }
        return static_cast<std::size_t>(parsed);
    }

    static std::size_t parseBatchEvents(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 64;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        if(end == value || parsed == 0)
        {
            return 64;
        }
        if(parsed > 1000000)
        {
            return 1000000;
        }
        return static_cast<std::size_t>(parsed);
    }

    static std::size_t parseOptionalBatchLimit(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 0;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        if(end == value)
        {
            return 0;
        }
        if(parsed > 1000000)
        {
            return 1000000;
        }
        return static_cast<std::size_t>(parsed);
    }

    static std::size_t parseSampleInterval(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 1;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        if(end == value || parsed == 0)
        {
            return 1;
        }
        if(parsed > 1000000)
        {
            return 1000000;
        }
        return static_cast<std::size_t>(parsed);
    }

    static uint64_t parseWaitWindowUs(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 0;
        }
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(value, &end, 10);
        if(end == value)
        {
            return 0;
        }
        if(parsed > 1000000ULL)
        {
            return 1000000ULL;
        }
        return static_cast<uint64_t>(parsed);
    }

    static uint64_t parseByteLimit(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 0;
        }
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(value, &end, 10);
        if(end == value)
        {
            return 0;
        }
        return static_cast<uint64_t>(parsed);
    }

    static std::size_t parseThreadCount(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 1;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        if(end == value || parsed == 0)
        {
            return 1;
        }
        if(parsed > 64)
        {
            return 64;
        }
        return static_cast<std::size_t>(parsed);
    }

    static std::size_t parseYieldCount(char const* value)
    {
        if(value == nullptr || *value == '\0')
        {
            return 0;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        if(end == value)
        {
            return 0;
        }
        if(parsed > 1000)
        {
            return 1000;
        }
        return static_cast<std::size_t>(parsed);
    }

    static ShardPolicy parseShardPolicy(char const* value)
    {
        if(value == nullptr || *value == '\0' || std::strcmp(value, "mixed") == 0)
        {
            return ShardPolicy::Mixed;
        }
        if(std::strcmp(value, "client") == 0)
        {
            return ShardPolicy::Client;
        }
        if(std::strcmp(value, "story_client") == 0 || std::strcmp(value, "story-client") == 0)
        {
            return ShardPolicy::StoryClient;
        }
        if(std::strcmp(value, "story") == 0)
        {
            return ShardPolicy::Story;
        }
        return ShardPolicy::Mixed;
    }

    char const* shardPolicyName() const
    {
        switch(shardPolicy)
        {
            case ShardPolicy::Client:
                return "client";
            case ShardPolicy::StoryClient:
                return "story_client";
            case ShardPolicy::Story:
                return "story";
            case ShardPolicy::Mixed:
            default:
                return "mixed";
        }
    }

    JournalShard& selectShard(LogEvent const& event)
    {
        std::size_t hash = 0;
        switch(shardPolicy)
        {
            case ShardPolicy::Client:
                hash = std::hash<uint64_t>{}(event.clientId);
                break;
            case ShardPolicy::StoryClient:
                hash = std::hash<uint64_t>{}(event.storyId) ^ (std::hash<uint64_t>{}(event.clientId) << 1U);
                break;
            case ShardPolicy::Story:
                hash = std::hash<uint64_t>{}(event.storyId);
                break;
            case ShardPolicy::Mixed:
            default:
                hash = std::hash<uint64_t>{}(event.clientId) ^
                       (std::hash<uint32_t>{}(event.eventIndex) << 1U) ^
                       std::hash<std::thread::id>{}(std::this_thread::get_id());
                break;
        }
        return *appendShards[hash % appendShards.size()];
    }

    static constexpr std::size_t TAIL_INDEX_SEAL_BATCH_SIZE = 1024;

    void appendTailIndexEntry(JournalShard& shard, TailIndexEntry&& entry)
    {
        auto& story_index = shard.tailIndexByStory[entry.story_id];
        story_index.activeEntries.push_back(std::move(entry));
        if(story_index.activeEntries.size() < TAIL_INDEX_SEAL_BATCH_SIZE)
        {
            return;
        }

        auto next_entries =
                std::make_shared<std::vector<TailIndexEntry>>(std::make_move_iterator(story_index.activeEntries.begin()),
                                                              std::make_move_iterator(story_index.activeEntries.end()));
        story_index.activeEntries.clear();
        story_index.activeEntries.reserve(TAIL_INDEX_SEAL_BATCH_SIZE);
        story_index.sealedPages.push_back(std::move(next_entries));
        KeeperAppendStats::instance().recordJournalTailSeal(TAIL_INDEX_SEAL_BATCH_SIZE,
                                                            story_index.sealedPages.size());
    }

    bool ensureOpen()
    {
        if(opened.load(std::memory_order_acquire))
        {
            return true;
        }

        std::lock_guard<std::mutex> lock(journalMutex);
        if(opened.load(std::memory_order_relaxed))
        {
            return true;
        }

        char const* journal_dir_env = std::getenv("CHRONOLOG_KEEPER_JOURNAL_DIR");
        if(journal_dir_env == nullptr || *journal_dir_env == '\0')
        {
            LOG_ERROR("[KeeperLocalJournal] CHRONOLOG_KEEPER_JOURNAL_DIR is required when journal mode is {}",
                      modeName());
            return false;
        }

        char hostname[256] = {};
        if(::gethostname(hostname, sizeof(hostname) - 1) != 0)
        {
            std::snprintf(hostname, sizeof(hostname), "unknown-host");
        }

        std::filesystem::path journal_dir(journal_dir_env);
        std::error_code ec;
        std::filesystem::create_directories(journal_dir, ec);
        if(ec)
        {
            LOG_ERROR("[KeeperLocalJournal] failed to create journal dir {}: {}", journal_dir.string(), ec.message());
            return false;
        }

        std::string const hostname_string(hostname);
        if(!recoverExistingSegments(journal_dir, hostname_string))
        {
            return false;
        }

        for(std::size_t shard_index = 0; shard_index < requestedShardCount; ++shard_index)
        {
            std::string filename = "keeper-" + hostname_string + "-" +
                                   std::to_string(static_cast<long long>(::getpid()));
            if(requestedShardCount > 1)
            {
                filename += "-shard-" + std::to_string(shard_index);
            }
            filename += ".journal";

            std::string const shard_path = (journal_dir / filename).string();
            int fd = ::open(shard_path.c_str(), O_CREAT | O_RDWR, 0644);
            if(fd < 0)
            {
                LOG_ERROR("[KeeperLocalJournal] failed to open {}: {}", shard_path, std::strerror(errno));
                return false;
            }
            off_t const end_offset = ::lseek(fd, 0, SEEK_END);
            if(end_offset < 0)
            {
                LOG_ERROR("[KeeperLocalJournal] failed to seek {}: {}", shard_path, std::strerror(errno));
                ::close(fd);
                return false;
            }

            std::size_t const segment_index = journalSegments.size();
            journalSegments.push_back(JournalSegment{shard_path, fd, true});
            auto shard = std::make_unique<JournalShard>();
            shard->path = shard_path;
            shard->fd = fd;
            shard->segmentIndex = segment_index;
            shard->nextWriteOffset.store(static_cast<uint64_t>(end_offset), std::memory_order_relaxed);
            appendShards.push_back(std::move(shard));
            if(journalPath.empty())
            {
                journalPath = shard_path;
            }
        }

        opened.store(true, std::memory_order_release);
        LOG_INFO("[KeeperLocalJournal] opened {} writable shard(s) mode={} shard_policy={} writev={} batch_writev={} atomic_offsets={} single_writer={} "
                 "single_writer_batch_events={} group_commit_flush_events={} group_commit_flush_bytes={} "
                 "group_commit_strict_flush_event_cap={} "
                 "group_commit_large_payload_bytes={} group_commit_large_payload_flush_events={} group_commit_flush_wait_us={} "
                 "group_commit_queue_boundary_min_events={} group_commit_queue_boundary_min_payload_bytes={} "
                 "group_commit_queue_boundary_wait_every_n={} "
                 "group_commit_wait_for_active_admissions={} "
                 "owner_drain_yields={} async_callback_dispatch={} async_batch_completion_dispatch={} "
                 "callback_dispatch_threads={} callback_batch_drain={} callback_batch_drain_max={} "
                 "callback_batch_drain_min_payload_bytes={} "
                 "durable_complete_before_publish={} notify_owner_only_on_empty={}",
                 appendShards.size(),
                 modeName(),
                 shardPolicyName(),
                 vectoredRecordWrite,
                 batchVectoredRecordWrite,
                 atomicOffsets,
                 singleWriterShards,
                 singleWriterBatchEvents,
                 groupCommitFlushEvents,
                 groupCommitFlushBytes,
                 groupCommitStrictFlushEventCap,
                 groupCommitLargePayloadBytes,
                 groupCommitLargePayloadFlushEvents,
                 groupCommitFlushWaitUs,
                 groupCommitQueueBoundaryMinEvents,
                 groupCommitQueueBoundaryMinPayloadBytes,
                 groupCommitQueueBoundaryWaitEveryN,
                 groupCommitWaitForActiveAdmissions,
                 ownerDrainYields,
                 asyncCallbackDispatch,
                 asyncBatchCompletionDispatch,
                 callbackDispatchThreadCount,
                 callbackBatchDrain,
                 callbackBatchDrainMax,
                 callbackBatchDrainMinPayloadBytes,
                 durableCompleteBeforePublish,
                 notifyOwnerOnlyOnEmpty);
        if(asyncCallbackDispatch || asyncBatchCompletionDispatch)
        {
            callbackThreads.reserve(callbackDispatchThreadCount);
            for(std::size_t thread_index = 0; thread_index < callbackDispatchThreadCount; ++thread_index)
            {
                callbackThreads.emplace_back([this]() { callbackDispatcherLoop(); });
            }
        }
        if(singleWriterShards)
        {
            for(auto const& shard_ptr: appendShards)
            {
                JournalShard& shard = *shard_ptr;
                shard.writerThread = std::thread([this, &shard]() { shardOwnerLoop(shard); });
            }
        }
        return true;
    }

    bool recoverExistingSegments(std::filesystem::path const& journal_dir, std::string const& hostname)
    {
        std::vector<std::filesystem::path> paths;
        std::string const keeper_prefix = "keeper-" + hostname + "-";
        std::error_code ec;
        for(auto const& entry: std::filesystem::directory_iterator(journal_dir, ec))
        {
            if(ec)
            {
                LOG_ERROR("[KeeperLocalJournal] failed to scan journal dir {}: {}", journal_dir.string(), ec.message());
                return false;
            }
            if(entry.is_regular_file() && entry.path().extension() == ".journal" &&
               entry.path().filename().string().rfind(keeper_prefix, 0) == 0)
            {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());

        std::size_t recovered_records = 0;
        for(auto const& path: paths)
        {
            int fd = ::open(path.c_str(), O_RDONLY);
            if(fd < 0)
            {
                LOG_ERROR("[KeeperLocalJournal] failed to open existing journal {}: {}",
                          path.string(),
                          std::strerror(errno));
                return false;
            }

            std::size_t const segment_index = journalSegments.size();
            journalSegments.push_back(JournalSegment{path.string(), fd, false});
            if(!recoverSegment(segment_index, recovered_records))
            {
                return false;
            }
        }
        if(recovered_records > 0)
        {
            LOG_INFO("[KeeperLocalJournal] recovered {} journal tail index records from {} existing segments",
                     recovered_records,
                     paths.size());
        }
        return true;
    }

    bool recoverSegment(std::size_t segment_index, std::size_t& recovered_records)
    {
        auto const& segment = journalSegments[segment_index];
        uint64_t offset = 0;
        for(;;)
        {
            RecordHeader header;
            ssize_t bytes_read = preadSome(segment.fd, &header, sizeof(header), offset);
            if(bytes_read == 0)
            {
                return true;
            }
            if(bytes_read != static_cast<ssize_t>(sizeof(header)))
            {
                LOG_WARNING("[KeeperLocalJournal] ignoring truncated header while recovering {}", segment.path);
                return true;
            }
            if(header.magic != RecordHeader::kMagic || header.version != 1 || header.header_size != sizeof(RecordHeader))
            {
                LOG_ERROR("[KeeperLocalJournal] invalid header while recovering {}", segment.path);
                return false;
            }

            uint64_t const payload_offset = offset + sizeof(header);
            uint64_t const next_offset = payload_offset + header.payload_size;
            off_t const payload_tail = ::lseek(segment.fd, 0, SEEK_END);
            if(payload_tail < 0)
            {
                LOG_ERROR("[KeeperLocalJournal] lseek failed while recovering {}: {}",
                          segment.path,
                          std::strerror(errno));
                return false;
            }
            if(next_offset > static_cast<uint64_t>(payload_tail))
            {
                LOG_WARNING("[KeeperLocalJournal] ignoring truncated payload while recovering {}", segment.path);
                return true;
            }

            recoveredTailIndexByStory[header.story_id].push_back(TailIndexEntry{header.story_id,
                                                                                 header.event_time,
                                                                                 header.client_id,
                                                                                 header.event_index,
                                                                                 segment_index,
                                                                                 payload_offset,
                                                                                 header.payload_size});
            ++recovered_records;
            offset = next_offset;
        }
    }

    bool writeFull(int fd, std::string const& path, void const* data, std::size_t size)
    {
        auto const* cursor = static_cast<char const*>(data);
        std::size_t remaining = size;
        while(remaining > 0)
        {
            ssize_t written = ::write(fd, cursor, remaining);
            if(written < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }
                LOG_ERROR("[KeeperLocalJournal] write failed for {}: {}", path, std::strerror(errno));
                return false;
            }
            cursor += written;
            remaining -= static_cast<std::size_t>(written);
        }
        return true;
    }

    bool pwriteFull(int fd, std::string const& path, void const* data, std::size_t size, uint64_t offset)
    {
        auto const* cursor = static_cast<char const*>(data);
        std::size_t written = 0;
        while(written < size)
        {
            ssize_t ret = ::pwrite(fd,
                                   cursor + written,
                                   size - written,
                                   static_cast<off_t>(offset + written));
            if(ret < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }
                LOG_ERROR("[KeeperLocalJournal] pwrite failed for {}: {}", path, std::strerror(errno));
                return false;
            }
            if(ret == 0)
            {
                LOG_ERROR("[KeeperLocalJournal] pwrite returned 0 for {}", path);
                return false;
            }
            written += static_cast<std::size_t>(ret);
        }
        return true;
    }

    bool pwritevFull(int fd,
                     std::string const& path,
                     void const* first_data,
                     std::size_t first_size,
                     void const* second_data,
                     std::size_t second_size,
                     uint64_t offset)
    {
        char const* first = static_cast<char const*>(first_data);
        char const* second = static_cast<char const*>(second_data);
        std::size_t first_remaining = first_size;
        std::size_t second_remaining = second_size;
        uint64_t current_offset = offset;

        while(first_remaining > 0 || second_remaining > 0)
        {
            iovec iovecs[2];
            int iovec_count = 0;
            if(first_remaining > 0)
            {
                iovecs[iovec_count++] = iovec{const_cast<char*>(first), first_remaining};
            }
            if(second_remaining > 0)
            {
                iovecs[iovec_count++] = iovec{const_cast<char*>(second), second_remaining};
            }

            ssize_t ret = ::pwritev(fd, iovecs, iovec_count, static_cast<off_t>(current_offset));
            if(ret < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }
                LOG_ERROR("[KeeperLocalJournal] pwritev failed for {}: {}", path, std::strerror(errno));
                return false;
            }
            if(ret == 0)
            {
                LOG_ERROR("[KeeperLocalJournal] pwritev returned 0 for {}", path);
                return false;
            }

            std::size_t consumed = static_cast<std::size_t>(ret);
            current_offset += consumed;
            std::size_t const first_consumed = std::min(consumed, first_remaining);
            first += first_consumed;
            first_remaining -= first_consumed;
            consumed -= first_consumed;
            if(consumed > 0)
            {
                std::size_t const second_consumed = std::min(consumed, second_remaining);
                second += second_consumed;
                second_remaining -= second_consumed;
            }
        }
        return true;
    }

    bool pwritevFull(int fd, std::string const& path, std::vector<iovec> const& input_iovecs, uint64_t offset)
    {
        if(input_iovecs.empty())
        {
            return true;
        }

        long const sys_iov_max = ::sysconf(_SC_IOV_MAX);
        std::size_t const iov_max = sys_iov_max > 0 ? static_cast<std::size_t>(sys_iov_max) : 1024;
        if(input_iovecs.size() > iov_max)
        {
            LOG_ERROR("[KeeperLocalJournal] batch pwritev iovec count {} exceeds system limit {} for {}",
                      input_iovecs.size(),
                      iov_max,
                      path);
            return false;
        }

        std::vector<iovec> remaining = input_iovecs;
        std::size_t first_iovec = 0;
        uint64_t current_offset = offset;
        while(first_iovec < remaining.size())
        {
            ssize_t ret = ::pwritev(fd,
                                    remaining.data() + first_iovec,
                                    static_cast<int>(remaining.size() - first_iovec),
                                    static_cast<off_t>(current_offset));
            if(ret < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }
                LOG_ERROR("[KeeperLocalJournal] batch pwritev failed for {}: {}", path, std::strerror(errno));
                return false;
            }
            if(ret == 0)
            {
                LOG_ERROR("[KeeperLocalJournal] batch pwritev returned 0 for {}", path);
                return false;
            }

            std::size_t consumed = static_cast<std::size_t>(ret);
            current_offset += consumed;
            while(consumed > 0 && first_iovec < remaining.size())
            {
                auto& iovec_entry = remaining[first_iovec];
                if(consumed >= iovec_entry.iov_len)
                {
                    consumed -= iovec_entry.iov_len;
                    ++first_iovec;
                    continue;
                }
                iovec_entry.iov_base = static_cast<char*>(iovec_entry.iov_base) + consumed;
                iovec_entry.iov_len -= consumed;
                consumed = 0;
            }
        }
        return true;
    }

    struct AppendWorkResult
    {
        std::shared_ptr<JournalWork> work;
        TailIndexEntry entry;
        uint64_t owner_start_ns{0};
        uint64_t queue_wait_ns{0};
        uint64_t write_ns{0};
        uint64_t write_done_ns{0};
        uint64_t fdatasync_ns{0};
        bool completion_dispatched{false};
    };

    bool writeAppendBatch(JournalShard& shard,
                          std::vector<std::shared_ptr<JournalWork>>& batch,
                          std::vector<AppendWorkResult>& results,
                          bool flush_each_batch)
    {
        uint64_t const service_start_ns = KeeperAppendStats::nowNs();
        bool const group_commit_wait = groupCommitWait && journalMode == Mode::FdSyncBatch;
        if(batchVectoredRecordWrite && group_commit_wait && batch.size() > 1)
        {
            uint64_t batch_size = 0;
            std::vector<iovec> iovecs;
            iovecs.reserve(batch.size() * 2);
            for(auto& work: batch)
            {
                batch_size += sizeof(work->header) + work->header.payload_size;
                iovecs.push_back(iovec{&work->header, sizeof(work->header)});
                if(work->header.payload_size > 0)
                {
                    iovecs.push_back(iovec{const_cast<char*>(work->payload->data()), work->payload->size()});
                }
            }

            uint64_t const record_offset_base = shard.nextWriteOffset.fetch_add(batch_size, std::memory_order_relaxed);
            uint64_t const write_start_ns = KeeperAppendStats::nowNs();
            if(!pwritevFull(shard.fd, shard.path, iovecs, record_offset_base))
            {
                return false;
            }
            uint64_t const write_done_ns = KeeperAppendStats::nowNs();
            uint64_t const per_record_write_ns = (write_done_ns - write_start_ns) / batch.size();
            uint64_t record_offset = record_offset_base;
            for(auto& work: batch)
            {
                uint64_t const queue_wait_ns = work->enqueue_time_ns == 0 ? 0 : service_start_ns - work->enqueue_time_ns;
                ++shard.recordsSinceSync;
                results.push_back(AppendWorkResult{work,
                                                   TailIndexEntry{
                                                           work->header.story_id,
                                                           work->header.event_time,
                                                           work->header.client_id,
                                                           work->header.event_index,
                                                           shard.segmentIndex,
                                                           static_cast<uint64_t>(record_offset + sizeof(work->header)),
                                                           work->header.payload_size},
                                                   service_start_ns,
                                                   queue_wait_ns,
                                                   per_record_write_ns,
                                                   write_done_ns,
                                                   0});
                record_offset += sizeof(work->header) + work->header.payload_size;
            }
            if(flush_each_batch && !flushAppendResults(shard, results))
            {
                return false;
            }
            return true;
        }

        for(auto& work: batch)
        {
            uint64_t const queue_wait_ns = work->enqueue_time_ns == 0 ? 0 : service_start_ns - work->enqueue_time_ns;
            uint64_t const record_size = sizeof(work->header) + work->header.payload_size;
            uint64_t const record_offset = shard.nextWriteOffset.fetch_add(record_size, std::memory_order_relaxed);
            uint64_t const write_start_ns = KeeperAppendStats::nowNs();
            bool const write_ok =
                    vectoredRecordWrite
                    ? pwritevFull(shard.fd,
                                  shard.path,
                                  &work->header,
                                  sizeof(work->header),
                                  work->payload->data(),
                                  work->payload->size(),
                                  record_offset)
                    : (pwriteFull(shard.fd, shard.path, &work->header, sizeof(work->header), record_offset) &&
                       (work->header.payload_size == 0 ||
                        pwriteFull(shard.fd,
                                   shard.path,
                                   work->payload->data(),
                                   work->payload->size(),
                                   record_offset + sizeof(work->header))));
            if(!write_ok)
            {
                return false;
            }
            uint64_t const write_done_ns = KeeperAppendStats::nowNs();
            uint64_t const write_ns = write_done_ns - write_start_ns;

            uint64_t fdatasync_ns = 0;
            if(journalMode == Mode::FdSync || journalMode == Mode::FdSyncBatch)
            {
                ++shard.recordsSinceSync;
                bool const should_sync = !group_commit_wait &&
                                         (journalMode == Mode::FdSync ||
                                          shard.recordsSinceSync >= fdatasyncBatchEvents);
                if(should_sync)
                {
                    uint64_t const fdatasync_start_ns = KeeperAppendStats::nowNs();
                    if(::fdatasync(shard.fd) != 0)
                    {
                        LOG_ERROR("[KeeperLocalJournal] fdatasync failed for {}: {}", shard.path, std::strerror(errno));
                        return false;
                    }
                    fdatasync_ns = KeeperAppendStats::nowNs() - fdatasync_start_ns;
                    shard.recordsSinceSync = 0;
                }
            }
            results.push_back(AppendWorkResult{work,
                                               TailIndexEntry{work->header.story_id,
                                                              work->header.event_time,
                                                              work->header.client_id,
                                                              work->header.event_index,
                                                              shard.segmentIndex,
                                                              static_cast<uint64_t>(record_offset + sizeof(work->header)),
                                                              work->header.payload_size},
                                               service_start_ns,
                                               queue_wait_ns,
                                               write_ns,
                                               write_done_ns,
                                               fdatasync_ns});
        }
        if(flush_each_batch && !flushAppendResults(shard, results))
        {
            return false;
        }
        return true;
    }

    bool flushAppendResults(JournalShard& shard,
                            std::vector<AppendWorkResult>& results,
                            KeeperAppendStats::JournalOwnerFlushReason flush_reason =
                                    KeeperAppendStats::JournalOwnerFlushReason::Unknown)
    {
        if(results.empty())
        {
            return true;
        }

        uint64_t const flush_start_ns = KeeperAppendStats::nowNs();
        for(auto const& result: results)
        {
            if(result.write_done_ns > 0 && flush_start_ns >= result.write_done_ns)
            {
                KeeperAppendStats::instance().recordJournalOwnerPendingFlush(flush_start_ns - result.write_done_ns);
            }
        }

        bool const group_commit_wait = groupCommitWait && journalMode == Mode::FdSyncBatch;
        uint64_t group_fdatasync_ns = 0;
        uint64_t early_completion_ns = 0;
        if(group_commit_wait)
        {
            uint64_t const fdatasync_start_ns = KeeperAppendStats::nowNs();
            if(::fdatasync(shard.fd) != 0)
            {
                LOG_ERROR("[KeeperLocalJournal] group-commit fdatasync failed for {}: {}",
                          shard.path,
                          std::strerror(errno));
                return false;
            }
            group_fdatasync_ns = KeeperAppendStats::nowNs() - fdatasync_start_ns;
            KeeperAppendStats::instance().recordJournalGroupSync(group_fdatasync_ns, results.size());
            uint64_t const per_record_fdatasync_ns = group_fdatasync_ns / results.size();
            for(auto& result: results)
            {
                result.fdatasync_ns = per_record_fdatasync_ns;
            }
            shard.recordsSinceSync = 0;
            if(durableCompleteBeforePublish)
            {
                for(auto& result: results)
                {
                    result.work->completedEntry = result.entry;
                }
                uint64_t const early_completion_start_ns = KeeperAppendStats::nowNs();
                completeAppendResults(results, true);
                early_completion_ns = KeeperAppendStats::nowNs() - early_completion_start_ns;
                for(auto& result: results)
                {
                    result.completion_dispatched = true;
                }
            }
        }

        uint64_t const publish_start_ns = KeeperAppendStats::nowNs();
        uint64_t const publish_lock_start_ns = KeeperAppendStats::nowNs();
        uint64_t publish_lock_wait_ns = 0;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            publish_lock_wait_ns = KeeperAppendStats::nowNs() - publish_lock_start_ns;
            for(auto& result: results)
            {
                result.work->completedEntry = result.entry;
                appendTailIndexEntry(shard, std::move(result.entry));
            }
        }
        if(journalMode == Mode::FdSync || group_commit_wait)
        {
            KeeperAppendStats::instance().recordJournalDurablePublish(results.size());
        }
        uint64_t const completion_ns = KeeperAppendStats::nowNs();
        uint64_t const publish_ns = KeeperAppendStats::nowNs() - publish_start_ns;
        uint64_t const flush_wall_ns = completion_ns - flush_start_ns;
        uint64_t const per_record_publish_lock_wait_ns = publish_lock_wait_ns / results.size();
        uint64_t const per_record_publish_ns = publish_ns / results.size();
        KeeperAppendStats::instance().recordJournalOwnerFlush(results.size(),
                                                              flush_wall_ns,
                                                              group_fdatasync_ns,
                                                              publish_ns,
                                                              early_completion_ns,
                                                              flush_reason,
                                                              activeAdmissions.load(std::memory_order_relaxed));
        KeeperAppendStats::instance().recordJournalOwnerBatch(results.size(), group_commit_wait);
        bool same_owner_start = true;
        uint64_t const first_owner_start_ns = results.front().owner_start_ns;
        for(auto const& result: results)
        {
            if(result.owner_start_ns != first_owner_start_ns)
            {
                same_owner_start = false;
                break;
            }
        }
        uint64_t const shared_per_record_service_ns =
                first_owner_start_ns == 0 ? 0 : (completion_ns - first_owner_start_ns) / results.size();
        for(auto& result: results)
        {
            uint64_t const service_ns =
                    same_owner_start ? shared_per_record_service_ns
                                     : (result.owner_start_ns == 0 ? 0 : completion_ns - result.owner_start_ns);
            KeeperAppendStats::instance().recordJournalAppend(per_record_publish_lock_wait_ns,
                                                              result.write_ns,
                                                              result.fdatasync_ns,
                                                              per_record_publish_ns,
                                                              result.work->header.payload_size);
            KeeperAppendStats::instance().recordJournalOwner(result.queue_wait_ns,
                                                             service_ns,
                                                             per_record_publish_lock_wait_ns);
        }
        return true;
    }

    bool processAppendBatch(JournalShard& shard, std::vector<std::shared_ptr<JournalWork>>& batch)
    {
        std::vector<AppendWorkResult> results;
        results.reserve(batch.size());
        return writeAppendBatch(shard, batch, results, true);
    }

    static uint64_t appendResultsBytes(std::vector<AppendWorkResult> const& results)
    {
        uint64_t total_bytes = 0;
        for(auto const& result: results)
        {
            if(result.work)
            {
                total_bytes += sizeof(RecordHeader) + result.work->header.payload_size;
            }
        }
        return total_bytes;
    }

    static uint64_t appendResultsMaxPayloadBytes(std::vector<AppendWorkResult> const& results)
    {
        uint64_t max_payload_bytes = 0;
        for(auto const& result: results)
        {
            if(result.work)
            {
                max_payload_bytes = std::max<uint64_t>(max_payload_bytes, result.work->header.payload_size);
            }
        }
        return max_payload_bytes;
    }

    bool enqueueWork(JournalShard& shard, std::shared_ptr<JournalWork> work)
    {
        uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
        std::unique_lock<std::mutex> lock(shard.queueMutex);
        uint64_t const lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;
        uint64_t const lock_hold_start_ns = KeeperAppendStats::nowNs();
        bool const notify_owner = !notifyOwnerOnlyOnEmpty || shard.workQueue.empty();
        shard.workQueue.push_back(std::move(work));
        uint64_t const queue_depth = shard.workQueue.size();
        uint64_t const lock_hold_ns = KeeperAppendStats::nowNs() - lock_hold_start_ns;
        lock.unlock();
        KeeperAppendStats::instance().recordJournalOwnerQueueEnqueue(lock_wait_ns, lock_hold_ns, 1, queue_depth);
        return notify_owner;
    }

    bool enqueueWorkBatch(JournalShard& shard, std::vector<std::shared_ptr<JournalWork>>& works)
    {
        if(works.empty())
        {
            return false;
        }
        uint64_t const enqueued_count = works.size();
        uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
        std::unique_lock<std::mutex> lock(shard.queueMutex);
        uint64_t const lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;
        uint64_t const lock_hold_start_ns = KeeperAppendStats::nowNs();
        bool const notify_owner = !notifyOwnerOnlyOnEmpty || shard.workQueue.empty();
        for(auto& work: works)
        {
            shard.workQueue.push_back(std::move(work));
        }
        uint64_t const queue_depth = shard.workQueue.size();
        uint64_t const lock_hold_ns = KeeperAppendStats::nowNs() - lock_hold_start_ns;
        lock.unlock();
        KeeperAppendStats::instance().recordJournalOwnerQueueEnqueue(lock_wait_ns,
                                                                     lock_hold_ns,
                                                                     enqueued_count,
                                                                     queue_depth);
        return notify_owner;
    }

    void notifyAdmissionBoundary()
    {
        if(!groupCommitWaitForActiveAdmissions)
        {
            return;
        }
        for(auto& shard_ptr: appendShards)
        {
            if(shard_ptr)
            {
                shard_ptr->workAvailable.notify_all();
            }
        }
    }

    void completeAppendResults(std::vector<AppendWorkResult>& results, bool ok)
    {
        uint64_t const dispatch_start_ns = KeeperAppendStats::nowNs();
        struct BatchCompletionDispatch
        {
            std::shared_ptr<AppendBatchCompletion> completion;
            std::size_t count{0};
            uint64_t maxPayloadBytes{0};
        };
        std::unordered_map<AppendBatchCompletion*, BatchCompletionDispatch> batch_completions;
        std::size_t dispatched_records = 0;
        for(auto& result: results)
        {
            if(result.completion_dispatched)
            {
                continue;
            }
            AppendCompletion const completed{ok, result.work->completedEntry};
            if(result.work->batchCompletion)
            {
                auto& state = batch_completions[result.work->batchCompletion.get()];
                if(!state.completion)
                {
                    state.completion = result.work->batchCompletion;
                }
                ++state.count;
                ++dispatched_records;
                state.maxPayloadBytes = std::max<uint64_t>(state.maxPayloadBytes,
                                                           result.work->header.payload_size);
            }
            else if(result.work->callback)
            {
                dispatchCallback(std::move(result.work->callback), completed);
                ++dispatched_records;
            }
            else
            {
                result.work->completion.set_value(completed);
                ++dispatched_records;
            }
        }
        for(auto& entry: batch_completions)
        {
            dispatchBatchCompletion(entry.second.completion,
                                    entry.second.count,
                                    ok,
                                    entry.second.maxPayloadBytes);
        }
        if(dispatched_records > 0)
        {
            KeeperAppendStats::instance().recordJournalCompletionDispatch(
                    dispatched_records, KeeperAppendStats::nowNs() - dispatch_start_ns);
        }
    }

    void shardOwnerLoop(JournalShard& shard)
    {
        std::vector<AppendWorkResult> pending_flush_results;
        uint64_t queue_boundary_wait_eligible_count = 0;
        for(;;)
        {
            std::shared_ptr<JournalWork> work;
            {
                uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
                std::unique_lock<std::mutex> lock(shard.queueMutex);
                uint64_t const lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;
                shard.workAvailable.wait(lock, [&shard]() { return !shard.workQueue.empty(); });
                uint64_t const lock_hold_start_ns = KeeperAppendStats::nowNs();
                work = std::move(shard.workQueue.front());
                shard.workQueue.pop_front();
                uint64_t const queue_depth = shard.workQueue.size();
                uint64_t const lock_hold_ns = KeeperAppendStats::nowNs() - lock_hold_start_ns;
                KeeperAppendStats::instance().recordJournalOwnerQueueDrain(lock_wait_ns, lock_hold_ns, 1, queue_depth);
            }

            if(work->type == JournalWork::Type::Stop)
            {
                bool const pending_ok =
                        flushAppendResults(shard,
                                           pending_flush_results,
                                           KeeperAppendStats::JournalOwnerFlushReason::StopSync);
                completeAppendResults(pending_flush_results, pending_ok);
                pending_flush_results.clear();
                work->completion.set_value(AppendCompletion{true, TailIndexEntry{}});
                return;
            }
            if(work->type == JournalWork::Type::Sync)
            {
                bool ok = flushAppendResults(shard,
                                             pending_flush_results,
                                             KeeperAppendStats::JournalOwnerFlushReason::StopSync);
                completeAppendResults(pending_flush_results, ok);
                pending_flush_results.clear();
                if(shard.fd >= 0 && ::fdatasync(shard.fd) != 0)
                {
                    LOG_ERROR("[KeeperLocalJournal] fdatasync failed while syncing {}: {}",
                              shard.path,
                              std::strerror(errno));
                    ok = false;
                }
                else
                {
                    shard.recordsSinceSync = 0;
                }
                work->completion.set_value(AppendCompletion{ok, TailIndexEntry{}});
                continue;
            }

            std::size_t owner_batch_limit = singleWriterBatchEvents;
            auto apply_pending_flush_event_cap = [&](std::size_t flush_event_limit) {
                if(flush_event_limit == 0)
                {
                    return;
                }
                if(pending_flush_results.size() < flush_event_limit)
                {
                    std::size_t const remaining_capacity = flush_event_limit - pending_flush_results.size();
                    owner_batch_limit = std::min(owner_batch_limit, std::max<std::size_t>(remaining_capacity, 1));
                }
                else
                {
                    owner_batch_limit = 1;
                }
            };
            if(groupCommitWait && journalMode == Mode::FdSyncBatch && groupCommitFlushEvents > 1 &&
               groupCommitStrictFlushEventCap)
            {
                apply_pending_flush_event_cap(groupCommitFlushEvents);
            }
            if(groupCommitWait && journalMode == Mode::FdSyncBatch && groupCommitFlushEvents > 1 &&
               groupCommitLargePayloadBytes > 0 && groupCommitLargePayloadFlushEvents > 0 &&
               (work->header.payload_size >= groupCommitLargePayloadBytes ||
                appendResultsMaxPayloadBytes(pending_flush_results) >= groupCommitLargePayloadBytes))
            {
                apply_pending_flush_event_cap(groupCommitLargePayloadFlushEvents);
            }

            std::vector<std::shared_ptr<JournalWork>> batch;
            batch.push_back(work);
            {
                uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
                std::unique_lock<std::mutex> lock(shard.queueMutex);
                uint64_t const lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;
                uint64_t drained_count = 0;
                uint64_t lock_hold_ns = 0;
                auto drain_append_work = [&]() {
                    uint64_t const lock_hold_start_ns = KeeperAppendStats::nowNs();
                    while(batch.size() < owner_batch_limit && !shard.workQueue.empty() &&
                          shard.workQueue.front()->type == JournalWork::Type::Append)
                    {
                        batch.push_back(std::move(shard.workQueue.front()));
                        shard.workQueue.pop_front();
                        ++drained_count;
                    }
                    lock_hold_ns += KeeperAppendStats::nowNs() - lock_hold_start_ns;
                };
                drain_append_work();

                if(groupCommitWait && journalMode == Mode::FdSyncBatch && groupCommitWaitWindowUs > 0 &&
                   batch.size() < owner_batch_limit)
                {
                    auto const deadline = std::chrono::steady_clock::now() +
                                          std::chrono::microseconds(groupCommitWaitWindowUs);
                    while(batch.size() < owner_batch_limit)
                    {
                        if(!shard.workQueue.empty() && shard.workQueue.front()->type != JournalWork::Type::Append)
                        {
                            break;
                        }
                        if(shard.workQueue.empty() &&
                           shard.workAvailable.wait_until(lock, deadline) == std::cv_status::timeout)
                        {
                            break;
                        }
                        drain_append_work();
                        if(std::chrono::steady_clock::now() >= deadline)
                        {
                            break;
                        }
                    }
                }
                else if(groupCommitWait && journalMode == Mode::FdSyncBatch && ownerDrainYields > 0 &&
                        batch.size() < owner_batch_limit)
                {
                    for(std::size_t yield_count = 0; yield_count < ownerDrainYields &&
                                                   batch.size() < owner_batch_limit;
                        ++yield_count)
                    {
                        if(!shard.workQueue.empty() && shard.workQueue.front()->type != JournalWork::Type::Append)
                        {
                            break;
                        }
                        lock.unlock();
                        std::this_thread::yield();
                        lock.lock();
                        drain_append_work();
                        if(!shard.workQueue.empty() && shard.workQueue.front()->type != JournalWork::Type::Append)
                        {
                            break;
                        }
                    }
                }
                uint64_t const queue_depth = shard.workQueue.size();
                KeeperAppendStats::instance().recordJournalOwnerQueueDrain(lock_wait_ns,
                                                                           lock_hold_ns,
                                                                           drained_count,
                                                                           queue_depth);
            }
            bool const defer_flush = groupCommitWait && journalMode == Mode::FdSyncBatch && groupCommitFlushEvents > 1;
            if(defer_flush)
            {
                std::vector<AppendWorkResult> written_results;
                written_results.reserve(batch.size());
                bool const write_ok = writeAppendBatch(shard, batch, written_results, false);
                if(!write_ok)
                {
                    std::vector<AppendWorkResult> failed_results;
                    failed_results.reserve(batch.size());
                    for(auto& append_work: batch)
                    {
                        failed_results.push_back(AppendWorkResult{append_work, TailIndexEntry{}});
                    }
                    completeAppendResults(failed_results, false);
                    continue;
                }
                pending_flush_results.insert(pending_flush_results.end(),
                                             std::make_move_iterator(written_results.begin()),
                                             std::make_move_iterator(written_results.end()));

                std::size_t flush_event_limit = groupCommitFlushEvents;
                if(groupCommitLargePayloadBytes > 0 && groupCommitLargePayloadFlushEvents > 0 &&
                   groupCommitLargePayloadFlushEvents < flush_event_limit &&
                   appendResultsMaxPayloadBytes(pending_flush_results) >= groupCommitLargePayloadBytes)
                {
                    flush_event_limit = groupCommitLargePayloadFlushEvents;
                }
                bool const event_limit_reached = pending_flush_results.size() >= flush_event_limit;
                bool const byte_limit_reached =
                        groupCommitFlushBytes > 0 &&
                        appendResultsBytes(pending_flush_results) >= groupCommitFlushBytes;
                bool should_flush = event_limit_reached || byte_limit_reached;
                auto flush_reason =
                        event_limit_reached ? KeeperAppendStats::JournalOwnerFlushReason::EventLimit
                                            : (byte_limit_reached
                                                       ? KeeperAppendStats::JournalOwnerFlushReason::ByteLimit
                                                       : KeeperAppendStats::JournalOwnerFlushReason::Unknown);
                {
                    uint64_t const lock_wait_start_ns = KeeperAppendStats::nowNs();
                    std::unique_lock<std::mutex> lock(shard.queueMutex);
                    uint64_t const lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;
                    bool const underfilled_boundary =
                            groupCommitQueueBoundaryMinEvents > 0 &&
                            pending_flush_results.size() < groupCommitQueueBoundaryMinEvents;
                    bool const payload_boundary_enabled =
                            groupCommitQueueBoundaryMinPayloadBytes == 0 ||
                            appendResultsMaxPayloadBytes(pending_flush_results) >=
                                    groupCommitQueueBoundaryMinPayloadBytes;
                    if(!should_flush && shard.workQueue.empty() && groupCommitFlushWaitUs > 0 &&
                       payload_boundary_enabled && (groupCommitQueueBoundaryMinEvents == 0 || underfilled_boundary))
                    {
                        ++queue_boundary_wait_eligible_count;
                        if((queue_boundary_wait_eligible_count % groupCommitQueueBoundaryWaitEveryN) == 0)
                        {
                            auto const deadline = std::chrono::steady_clock::now() +
                                                  std::chrono::microseconds(groupCommitFlushWaitUs);
                            uint64_t const wait_start_ns = KeeperAppendStats::nowNs();
                            bool const woke_for_work =
                                    shard.workAvailable.wait_until(lock,
                                                                   deadline,
                                                                   [&shard]() { return !shard.workQueue.empty(); });
                            KeeperAppendStats::instance().recordJournalOwnerQueueBoundaryWait(
                                    KeeperAppendStats::nowNs() - wait_start_ns, woke_for_work);
                        }
                    }
                    if(!should_flush && shard.workQueue.empty() && groupCommitWaitForActiveAdmissions &&
                       activeAdmissions.load(std::memory_order_relaxed) > 0 && payload_boundary_enabled &&
                       (groupCommitQueueBoundaryMinEvents == 0 || underfilled_boundary))
                    {
                        uint64_t const wait_start_ns = KeeperAppendStats::nowNs();
                        shard.workAvailable.wait(lock, [&]() {
                            return !shard.workQueue.empty() ||
                                   activeAdmissions.load(std::memory_order_relaxed) == 0;
                        });
                        KeeperAppendStats::instance().recordJournalOwnerQueueBoundaryAdmissionWait(
                                KeeperAppendStats::nowNs() - wait_start_ns, !shard.workQueue.empty());
                    }
                    uint64_t const lock_hold_start_ns = KeeperAppendStats::nowNs();
                    if(shard.workQueue.empty() || shard.workQueue.front()->type != JournalWork::Type::Append)
                    {
                        if(!should_flush)
                        {
                            flush_reason = KeeperAppendStats::JournalOwnerFlushReason::QueueBoundary;
                            should_flush = true;
                        }
                    }
                    uint64_t const queue_depth = shard.workQueue.size();
                    uint64_t const lock_hold_ns = KeeperAppendStats::nowNs() - lock_hold_start_ns;
                    KeeperAppendStats::instance().recordJournalOwnerQueueDrain(lock_wait_ns,
                                                                               lock_hold_ns,
                                                                               0,
                                                                               queue_depth);
                }
                if(should_flush)
                {
                    bool const flush_ok = flushAppendResults(shard, pending_flush_results, flush_reason);
                    completeAppendResults(pending_flush_results, flush_ok);
                    pending_flush_results.clear();
                }
                continue;
            }

            bool const ok = processAppendBatch(shard, batch);
            std::vector<AppendWorkResult> completed_results;
            completed_results.reserve(batch.size());
            for(auto& append_work: batch)
            {
                completed_results.push_back(AppendWorkResult{append_work, append_work->completedEntry});
            }
            completeAppendResults(completed_results, ok);
        }
    }

    void dispatchCallback(AppendCallback callback, AppendCompletion const& completed)
    {
        if(!asyncCallbackDispatch)
        {
            callback(completed.ok, toWalRecordCursor(completed.entry, completed.ok));
            return;
        }
        uint64_t const enqueue_time_ns = KeeperAppendStats::nowNs();
        uint64_t const lock_wait_start_ns = enqueue_time_ns;
        uint64_t lock_wait_ns = 0;
        uint64_t queue_depth = 0;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;
            CallbackWork work;
            work.callback = std::move(callback);
            work.completion = completed;
            work.enqueue_time_ns = enqueue_time_ns;
            work.maxPayloadBytes = completed.entry.payload_size;
            callbackQueue.push_back(std::move(work));
            queue_depth = callbackQueue.size();
        }
        KeeperAppendStats::instance().recordJournalCallbackDispatchEnqueue(lock_wait_ns, queue_depth);
        callbackAvailable.notify_one();
    }

    void dispatchBatchCompletion(std::shared_ptr<AppendBatchCompletion> completion,
                                 std::size_t count,
                                 bool completed_ok,
                                 uint64_t max_payload_bytes)
    {
        if(!completion)
        {
            return;
        }
        if(!asyncBatchCompletionDispatch)
        {
            completion->complete(count, completed_ok);
            return;
        }
        uint64_t const enqueue_time_ns = KeeperAppendStats::nowNs();
        uint64_t const lock_wait_start_ns = enqueue_time_ns;
        uint64_t lock_wait_ns = 0;
        uint64_t queue_depth = 0;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            lock_wait_ns = KeeperAppendStats::nowNs() - lock_wait_start_ns;
            CallbackWork work;
            work.batchCompletion = std::move(completion);
            work.batchCompletionCount = count;
            work.batchCompletionOk = completed_ok;
            work.enqueue_time_ns = enqueue_time_ns;
            work.maxPayloadBytes = max_payload_bytes;
            callbackQueue.push_back(std::move(work));
            queue_depth = callbackQueue.size();
        }
        KeeperAppendStats::instance().recordJournalCallbackDispatchEnqueue(lock_wait_ns, queue_depth);
        callbackAvailable.notify_one();
    }

    void callbackDispatcherLoop()
    {
        std::vector<CallbackWork> local_batch;
        for(;;)
        {
            {
                std::unique_lock<std::mutex> lock(callbackMutex);
                callbackAvailable.wait(lock, [this]() { return callbackStop || !callbackQueue.empty(); });
                if(callbackQueue.empty())
                {
                    if(callbackStop)
                    {
                        return;
                    }
                    continue;
                }
                bool const drain_callback_batch =
                        callbackBatchDrain &&
                        (callbackBatchDrainMinPayloadBytes == 0 ||
                         callbackQueue.front().maxPayloadBytes >= callbackBatchDrainMinPayloadBytes);
                if(!drain_callback_batch)
                {
                    local_batch.clear();
                    local_batch.push_back(std::move(callbackQueue.front()));
                    callbackQueue.pop_front();
                }
                else
                {
                    local_batch.clear();
                    std::size_t const drain_limit =
                            callbackBatchDrainMax == 0 ? callbackQueue.size()
                                                       : std::min(callbackBatchDrainMax, callbackQueue.size());
                    local_batch.reserve(drain_limit);
                    while(local_batch.size() < drain_limit && !callbackQueue.empty())
                    {
                        local_batch.push_back(std::move(callbackQueue.front()));
                        callbackQueue.pop_front();
                    }
                }
            }
            for(auto& work: local_batch)
            {
                if(work.callback)
                {
                    uint64_t const callback_start_ns = KeeperAppendStats::nowNs();
                    uint64_t const queue_wait_ns =
                        work.enqueue_time_ns == 0 ? 0 : callback_start_ns - work.enqueue_time_ns;
                    work.callback(work.completion.ok, toWalRecordCursor(work.completion.entry, work.completion.ok));
                    KeeperAppendStats::instance().recordJournalCallbackDispatch(
                            queue_wait_ns, KeeperAppendStats::nowNs() - callback_start_ns);
                }
                else if(work.batchCompletion)
                {
                    uint64_t const callback_start_ns = KeeperAppendStats::nowNs();
                    uint64_t const queue_wait_ns =
                        work.enqueue_time_ns == 0 ? 0 : callback_start_ns - work.enqueue_time_ns;
                    work.batchCompletion->complete(work.batchCompletionCount, work.batchCompletionOk);
                    KeeperAppendStats::instance().recordJournalCallbackDispatch(
                            queue_wait_ns, KeeperAppendStats::nowNs() - callback_start_ns);
                }
            }
        }
    }

    void stopCallbackDispatcher()
    {
        if(callbackThreads.empty())
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callbackStop = true;
        }
        callbackAvailable.notify_all();
        for(auto& callback_thread: callbackThreads)
        {
            if(callback_thread.joinable())
            {
                callback_thread.join();
            }
        }
        callbackThreads.clear();
    }

    bool syncShardOwner(JournalShard& shard)
    {
        auto work = std::make_shared<JournalWork>();
        work->type = JournalWork::Type::Sync;
        auto completion = work->completion.get_future();
        if(enqueueWork(shard, work))
        {
            shard.workAvailable.notify_one();
        }
        return completion.get().ok;
    }

    void stopShardOwners()
    {
        if(!singleWriterShards)
        {
            return;
        }
        for(auto const& shard_ptr: appendShards)
        {
            JournalShard& shard = *shard_ptr;
            if(!shard.writerThread.joinable())
            {
                continue;
            }
            auto work = std::make_shared<JournalWork>();
            work->type = JournalWork::Type::Stop;
            auto completion = work->completion.get_future();
            if(enqueueWork(shard, work))
            {
                shard.workAvailable.notify_one();
            }
            completion.get();
            shard.writerThread.join();
        }
    }

    bool readPayloads(std::vector<TailIndexEntry> const& matches,
                      std::vector<std::string>& payloads,
                      TailReadStats& stats)
    {
        bool const use_direct = directTailReadMode(matches);
        stats.direct_read = use_direct;
        if(use_direct)
        {
            return readPayloadsDirect(matches, payloads, stats);
        }
        bool const use_vectored = vectoredTailReadMode(matches);
        stats.vectored_read = use_vectored;
        if(use_vectored)
        {
            return readPayloadsVectored(matches, payloads, nullptr, &stats);
        }
        return readPayloadsCoalesced(matches, payloads, stats);
    }

    bool readPayloadsDirect(std::vector<TailIndexEntry> const& matches,
                            std::vector<std::string>& payloads,
                            TailReadStats& stats)
    {
        payloads.assign(matches.size(), std::string());
        for(std::size_t index = 0; index < matches.size(); ++index)
        {
            TailIndexEntry const& entry = matches[index];
            if(entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid tail index segment {}", entry.segment_index);
                return false;
            }
            auto const& segment = journalSegments[entry.segment_index];
            uint64_t const alloc_start_ns = KeeperAppendStats::nowNs();
            std::string payload(entry.payload_size, '\0');
            stats.payload_alloc_ns += KeeperAppendStats::nowNs() - alloc_start_ns;
            ++stats.read_group_count;
            stats.physical_read_bytes += entry.payload_size;
            stats.payload_copy_bytes += entry.payload_size;
            if(entry.payload_size > 0)
            {
                uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
                bool const read_ok = preadFull(segment.fd, segment.path, payload.data(), payload.size(), entry.payload_offset);
                stats.payload_syscall_ns += KeeperAppendStats::nowNs() - syscall_start_ns;
                if(!read_ok)
                {
                    return false;
                }
            }
            payloads[index] = std::move(payload);
        }
        return true;
    }

    bool readPayloadsDirectToEvents(std::vector<TailIndexEntry> const& matches,
                                    std::vector<LogEvent>& events,
                                    TailReadStats& stats)
    {
        events.reserve(events.size() + matches.size());
        for(auto const& entry: matches)
        {
            if(entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid tail index segment {}", entry.segment_index);
                return false;
            }
            auto const& segment = journalSegments[entry.segment_index];
            uint64_t const event_build_start_ns = KeeperAppendStats::nowNs();
            events.emplace_back(entry.story_id, entry.event_time, entry.client_id, entry.event_index, std::string());
            LogEvent& event = events.back();
            uint64_t const alloc_start_ns = KeeperAppendStats::nowNs();
            event.logRecord.resize(entry.payload_size);
            stats.payload_alloc_ns += KeeperAppendStats::nowNs() - alloc_start_ns;
            stats.event_build_ns += KeeperAppendStats::nowNs() - event_build_start_ns;
            ++stats.read_group_count;
            stats.physical_read_bytes += entry.payload_size;
            stats.payload_copy_bytes += entry.payload_size;
            if(entry.payload_size > 0)
            {
                uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
                bool const read_ok =
                        preadFull(segment.fd, segment.path, event.logRecord.data(), event.logRecord.size(), entry.payload_offset);
                stats.payload_syscall_ns += KeeperAppendStats::nowNs() - syscall_start_ns;
                if(!read_ok)
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool readPayloadsDirectToBatch(std::vector<TailIndexEntry> const& matches,
                                   KeeperTailBatch& batch,
                                   TailReadStats& stats)
    {
        batch.events.reserve(matches.size());
        for(auto const& entry: matches)
        {
            if(entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid tail index segment {}", entry.segment_index);
                return false;
            }
            auto const& segment = journalSegments[entry.segment_index];
            uint64_t const event_build_start_ns = KeeperAppendStats::nowNs();
            batch.events.emplace_back(entry.story_id, entry.event_time, entry.client_id, entry.event_index, std::string());
            LogEvent& event = batch.events.back();
            uint64_t const alloc_start_ns = KeeperAppendStats::nowNs();
            event.logRecord.resize(entry.payload_size);
            stats.payload_alloc_ns += KeeperAppendStats::nowNs() - alloc_start_ns;
            batch.nextCursor.segmentIndex = static_cast<uint64_t>(entry.segment_index);
            batch.nextCursor.payloadOffset = entry.payload_offset;
            batch.nextCursor.initialized = true;
            stats.event_build_ns += KeeperAppendStats::nowNs() - event_build_start_ns;
            ++stats.read_group_count;
            stats.physical_read_bytes += entry.payload_size;
            stats.payload_copy_bytes += entry.payload_size;
            if(entry.payload_size > 0)
            {
                uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
                bool const read_ok =
                        preadFull(segment.fd, segment.path, event.logRecord.data(), event.logRecord.size(), entry.payload_offset);
                stats.payload_syscall_ns += KeeperAppendStats::nowNs() - syscall_start_ns;
                if(!read_ok)
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool readPayloadsDirectToPackedBatch(std::vector<TailIndexEntry> const& matches,
                                         KeeperTailPackedBatch& batch,
                                         TailReadStats& stats)
    {
        std::size_t payload_bytes = 0;
        for(auto const& entry: matches)
        {
            payload_bytes += static_cast<std::size_t>(entry.payload_size);
        }

        uint64_t const alloc_start_ns = KeeperAppendStats::nowNs();
        batch.payloadBlob.resize(payload_bytes);
        batch.eventTimes.reserve(matches.size());
        batch.clientIds.reserve(matches.size());
        batch.eventIndexes.reserve(matches.size());
        batch.payloadOffsets.reserve(matches.size());
        batch.payloadSizes.reserve(matches.size());
        stats.payload_alloc_ns += KeeperAppendStats::nowNs() - alloc_start_ns;

        std::size_t blob_offset = 0;
        for(auto const& entry: matches)
        {
            if(entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid tail index segment {}", entry.segment_index);
                return false;
            }
            auto const& segment = journalSegments[entry.segment_index];
            uint64_t const event_build_start_ns = KeeperAppendStats::nowNs();
            batch.eventTimes.push_back(entry.event_time);
            batch.clientIds.push_back(entry.client_id);
            batch.eventIndexes.push_back(entry.event_index);
            batch.payloadOffsets.push_back(static_cast<uint64_t>(blob_offset));
            batch.payloadSizes.push_back(entry.payload_size);
            batch.nextCursor.segmentIndex = static_cast<uint64_t>(entry.segment_index);
            batch.nextCursor.payloadOffset = entry.payload_offset;
            batch.nextCursor.initialized = true;
            stats.event_build_ns += KeeperAppendStats::nowNs() - event_build_start_ns;
            ++stats.read_group_count;
            stats.physical_read_bytes += entry.payload_size;
            stats.payload_copy_bytes += entry.payload_size;
            if(entry.payload_size > 0)
            {
                uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
                bool const read_ok = preadFull(segment.fd,
                                               segment.path,
                                               batch.payloadBlob.data() + blob_offset,
                                               static_cast<std::size_t>(entry.payload_size),
                                               entry.payload_offset);
                stats.payload_syscall_ns += KeeperAppendStats::nowNs() - syscall_start_ns;
                if(!read_ok)
                {
                    return false;
                }
                blob_offset += static_cast<std::size_t>(entry.payload_size);
            }
        }
        return true;
    }

    bool readPayloadsDirectToPackedBulkBuffer(std::vector<TailIndexEntry> const& matches,
                                              KeeperTailPackedBatch& batch,
                                              std::unique_ptr<char[]>& payload_storage,
                                              std::size_t& payload_bytes,
                                              TailReadStats& stats)
    {
        payload_bytes = 0;
        for(auto const& entry: matches)
        {
            payload_bytes += static_cast<std::size_t>(entry.payload_size);
        }

        uint64_t const alloc_start_ns = KeeperAppendStats::nowNs();
        if(payload_bytes > 0)
        {
            try
            {
                payload_storage.reset(new char[payload_bytes]);
            }
            catch(std::bad_alloc const&)
            {
                LOG_ERROR("[KeeperLocalJournal] failed to allocate {} bytes for packed bulk tail read", payload_bytes);
                return false;
            }
        }
        batch.eventTimes.reserve(matches.size());
        batch.clientIds.reserve(matches.size());
        batch.eventIndexes.reserve(matches.size());
        batch.payloadOffsets.reserve(matches.size());
        batch.payloadSizes.reserve(matches.size());
        stats.payload_alloc_ns += KeeperAppendStats::nowNs() - alloc_start_ns;

        std::size_t blob_offset = 0;
        for(auto const& entry: matches)
        {
            if(entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid tail index segment {}", entry.segment_index);
                return false;
            }
            auto const& segment = journalSegments[entry.segment_index];
            uint64_t const event_build_start_ns = KeeperAppendStats::nowNs();
            batch.eventTimes.push_back(entry.event_time);
            batch.clientIds.push_back(entry.client_id);
            batch.eventIndexes.push_back(entry.event_index);
            batch.payloadOffsets.push_back(static_cast<uint64_t>(blob_offset));
            batch.payloadSizes.push_back(entry.payload_size);
            batch.nextCursor.segmentIndex = static_cast<uint64_t>(entry.segment_index);
            batch.nextCursor.payloadOffset = entry.payload_offset;
            batch.nextCursor.initialized = true;
            stats.event_build_ns += KeeperAppendStats::nowNs() - event_build_start_ns;
            ++stats.read_group_count;
            stats.physical_read_bytes += entry.payload_size;
            stats.payload_copy_bytes += entry.payload_size;
            if(entry.payload_size > 0)
            {
                uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
                bool const read_ok = preadFull(segment.fd,
                                               segment.path,
                                               payload_storage.get() + blob_offset,
                                               static_cast<std::size_t>(entry.payload_size),
                                               entry.payload_offset);
                stats.payload_syscall_ns += KeeperAppendStats::nowNs() - syscall_start_ns;
                if(!read_ok)
                {
                    return false;
                }
                blob_offset += static_cast<std::size_t>(entry.payload_size);
            }
        }
        return true;
    }

    bool readPayloadsVectoredToPackedBulkBuffer(std::vector<TailIndexEntry> const& matches,
                                                KeeperTailPackedBatch& batch,
                                                std::unique_ptr<char[]>& payload_storage,
                                                std::size_t& payload_bytes,
                                                TailReadStats& stats)
    {
        payload_bytes = 0;
        for(auto const& entry: matches)
        {
            payload_bytes += static_cast<std::size_t>(entry.payload_size);
        }

        uint64_t const alloc_start_ns = KeeperAppendStats::nowNs();
        if(payload_bytes > 0)
        {
            try
            {
                payload_storage.reset(new char[payload_bytes]);
            }
            catch(std::bad_alloc const&)
            {
                LOG_ERROR("[KeeperLocalJournal] failed to allocate {} bytes for packed bulk tail read", payload_bytes);
                return false;
            }
        }
        batch.eventTimes.reserve(matches.size());
        batch.clientIds.reserve(matches.size());
        batch.eventIndexes.reserve(matches.size());
        batch.payloadOffsets.reserve(matches.size());
        batch.payloadSizes.reserve(matches.size());
        stats.payload_alloc_ns += KeeperAppendStats::nowNs() - alloc_start_ns;

        std::vector<TailReadPlanEntry> read_plan;
        read_plan.reserve(matches.size());
        std::size_t blob_offset = 0;
        for(std::size_t index = 0; index < matches.size(); ++index)
        {
            auto const& entry = matches[index];
            uint64_t const event_build_start_ns = KeeperAppendStats::nowNs();
            batch.eventTimes.push_back(entry.event_time);
            batch.clientIds.push_back(entry.client_id);
            batch.eventIndexes.push_back(entry.event_index);
            batch.payloadOffsets.push_back(static_cast<uint64_t>(blob_offset));
            batch.payloadSizes.push_back(entry.payload_size);
            batch.nextCursor.segmentIndex = static_cast<uint64_t>(entry.segment_index);
            batch.nextCursor.payloadOffset = entry.payload_offset;
            batch.nextCursor.initialized = true;
            stats.event_build_ns += KeeperAppendStats::nowNs() - event_build_start_ns;
            read_plan.push_back(TailReadPlanEntry{entry, index});
            blob_offset += static_cast<std::size_t>(entry.payload_size);
        }
        static constexpr uint64_t max_vectored_read_bytes = 4 * 1024 * 1024;
        static constexpr std::size_t max_iovecs = 1024;
        for(std::size_t group_begin = 0; group_begin < read_plan.size();)
        {
            TailReadPlanEntry const& first = read_plan[group_begin];
            if(first.entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid tail index segment {}", first.entry.segment_index);
                return false;
            }

            uint64_t group_start = first.entry.payload_offset;
            uint64_t group_end = first.entry.payload_offset + first.entry.payload_size;
            std::size_t group_end_index = group_begin + 1;
            std::size_t iovec_count = first.entry.payload_size > 0 ? 1 : 0;
            while(group_end_index < read_plan.size())
            {
                TailReadPlanEntry const& next = read_plan[group_end_index];
                if(next.entry.segment_index != first.entry.segment_index || next.entry.payload_offset < group_end)
                {
                    break;
                }
                uint64_t const next_end = next.entry.payload_offset + next.entry.payload_size;
                if(next_end - group_start > max_vectored_read_bytes)
                {
                    break;
                }
                std::size_t const additional_iovecs =
                        (next.entry.payload_offset > group_end ? 1 : 0) + (next.entry.payload_size > 0 ? 1 : 0);
                if(iovec_count + additional_iovecs > max_iovecs)
                {
                    break;
                }
                iovec_count += additional_iovecs;
                group_end = next_end;
                ++group_end_index;
            }

            uint64_t gap_bytes = 0;
            uint64_t next_offset = group_start;
            for(std::size_t index = group_begin; index < group_end_index; ++index)
            {
                TailReadPlanEntry const& planned = read_plan[index];
                if(planned.entry.payload_offset > next_offset)
                {
                    gap_bytes += planned.entry.payload_offset - next_offset;
                    next_offset = planned.entry.payload_offset;
                }
                next_offset += planned.entry.payload_size;
            }

            uint64_t const gap_alloc_start_ns = KeeperAppendStats::nowNs();
            std::vector<char> gap_buffer;
            if(gap_bytes > 0)
            {
                gap_buffer.resize(static_cast<std::size_t>(gap_bytes));
            }
            stats.payload_alloc_ns += KeeperAppendStats::nowNs() - gap_alloc_start_ns;

            auto const& segment = journalSegments[first.entry.segment_index];
            std::vector<iovec> iovecs;
            iovecs.reserve(iovec_count);
            uint64_t payload_read_bytes = 0;
            uint64_t gap_offset = 0;
            next_offset = group_start;
            for(std::size_t index = group_begin; index < group_end_index; ++index)
            {
                TailReadPlanEntry const& planned = read_plan[index];
                if(planned.entry.payload_offset > next_offset)
                {
                    uint64_t const current_gap = planned.entry.payload_offset - next_offset;
                    iovecs.push_back(iovec{gap_buffer.data() + static_cast<std::size_t>(gap_offset),
                                           static_cast<std::size_t>(current_gap)});
                    gap_offset += current_gap;
                    next_offset = planned.entry.payload_offset;
                }
                if(planned.entry.payload_size > 0)
                {
                    std::size_t const output_offset =
                            static_cast<std::size_t>(batch.payloadOffsets[planned.output_index]);
                    iovecs.push_back(iovec{payload_storage.get() + output_offset,
                                           static_cast<std::size_t>(planned.entry.payload_size)});
                    payload_read_bytes += planned.entry.payload_size;
                    next_offset += planned.entry.payload_size;
                }
            }

            uint64_t read_syscalls = 0;
            uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
            bool const read_ok = iovecs.empty() || preadvFull(segment.fd, segment.path, iovecs, group_start, &read_syscalls);
            stats.payload_syscall_ns += KeeperAppendStats::nowNs() - syscall_start_ns;
            if(!read_ok)
            {
                return false;
            }

            ++stats.read_group_count;
            stats.physical_read_bytes += group_end - group_start;
            stats.payload_copy_bytes += payload_read_bytes;
            group_begin = group_end_index;
        }
        return true;
    }

    bool readPayloadsVectoredToPackedBatch(std::vector<TailIndexEntry> const& matches,
                                           KeeperTailPackedBatch& batch,
                                           TailReadStats& stats)
    {
        std::size_t payload_bytes = 0;
        for(auto const& entry: matches)
        {
            payload_bytes += static_cast<std::size_t>(entry.payload_size);
        }

        uint64_t const alloc_start_ns = KeeperAppendStats::nowNs();
        batch.payloadBlob.resize(payload_bytes);
        batch.eventTimes.reserve(matches.size());
        batch.clientIds.reserve(matches.size());
        batch.eventIndexes.reserve(matches.size());
        batch.payloadOffsets.reserve(matches.size());
        batch.payloadSizes.reserve(matches.size());
        stats.payload_alloc_ns += KeeperAppendStats::nowNs() - alloc_start_ns;

        std::vector<TailReadPlanEntry> read_plan;
        read_plan.reserve(matches.size());
        std::size_t blob_offset = 0;
        for(std::size_t index = 0; index < matches.size(); ++index)
        {
            auto const& entry = matches[index];
            uint64_t const event_build_start_ns = KeeperAppendStats::nowNs();
            batch.eventTimes.push_back(entry.event_time);
            batch.clientIds.push_back(entry.client_id);
            batch.eventIndexes.push_back(entry.event_index);
            batch.payloadOffsets.push_back(static_cast<uint64_t>(blob_offset));
            batch.payloadSizes.push_back(entry.payload_size);
            batch.nextCursor.segmentIndex = static_cast<uint64_t>(entry.segment_index);
            batch.nextCursor.payloadOffset = entry.payload_offset;
            batch.nextCursor.initialized = true;
            stats.event_build_ns += KeeperAppendStats::nowNs() - event_build_start_ns;
            read_plan.push_back(TailReadPlanEntry{entry, index});
            blob_offset += static_cast<std::size_t>(entry.payload_size);
        }
        static constexpr uint64_t max_vectored_read_bytes = 4 * 1024 * 1024;
        static constexpr std::size_t max_iovecs = 1024;
        for(std::size_t group_begin = 0; group_begin < read_plan.size();)
        {
            TailReadPlanEntry const& first = read_plan[group_begin];
            if(first.entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid tail index segment {}", first.entry.segment_index);
                return false;
            }

            uint64_t group_start = first.entry.payload_offset;
            uint64_t group_end = first.entry.payload_offset + first.entry.payload_size;
            std::size_t group_end_index = group_begin + 1;
            std::size_t iovec_count = first.entry.payload_size > 0 ? 1 : 0;
            while(group_end_index < read_plan.size())
            {
                TailReadPlanEntry const& next = read_plan[group_end_index];
                if(next.entry.segment_index != first.entry.segment_index || next.entry.payload_offset < group_end)
                {
                    break;
                }
                uint64_t const next_end = next.entry.payload_offset + next.entry.payload_size;
                if(next_end - group_start > max_vectored_read_bytes)
                {
                    break;
                }
                std::size_t const additional_iovecs =
                        (next.entry.payload_offset > group_end ? 1 : 0) + (next.entry.payload_size > 0 ? 1 : 0);
                if(iovec_count + additional_iovecs > max_iovecs)
                {
                    break;
                }
                iovec_count += additional_iovecs;
                group_end = next_end;
                ++group_end_index;
            }

            auto const& segment = journalSegments[first.entry.segment_index];
            std::vector<iovec> iovecs;
            iovecs.reserve(iovec_count);
            std::deque<std::string> gap_buffers;
            uint64_t payload_read_bytes = 0;
            uint64_t next_offset = group_start;
            for(std::size_t index = group_begin; index < group_end_index; ++index)
            {
                TailReadPlanEntry const& planned = read_plan[index];
                if(planned.entry.payload_offset > next_offset)
                {
                    uint64_t const current_gap = planned.entry.payload_offset - next_offset;
                    uint64_t const gap_alloc_start_ns = KeeperAppendStats::nowNs();
                    gap_buffers.emplace_back(current_gap, '\0');
                    stats.payload_alloc_ns += KeeperAppendStats::nowNs() - gap_alloc_start_ns;
                    iovecs.push_back(iovec{gap_buffers.back().data(), gap_buffers.back().size()});
                    next_offset = planned.entry.payload_offset;
                }
                if(planned.entry.payload_size > 0)
                {
                    std::size_t const output_offset =
                            static_cast<std::size_t>(batch.payloadOffsets[planned.output_index]);
                    iovecs.push_back(iovec{batch.payloadBlob.data() + output_offset,
                                           static_cast<std::size_t>(planned.entry.payload_size)});
                    payload_read_bytes += planned.entry.payload_size;
                    next_offset += planned.entry.payload_size;
                }
            }

            uint64_t read_syscalls = 0;
            uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
            bool const read_ok = iovecs.empty() || preadvFull(segment.fd, segment.path, iovecs, group_start, &read_syscalls);
            stats.payload_syscall_ns += KeeperAppendStats::nowNs() - syscall_start_ns;
            if(!read_ok)
            {
                return false;
            }

            ++stats.read_group_count;
            stats.physical_read_bytes += group_end - group_start;
            stats.payload_copy_bytes += payload_read_bytes;
            group_begin = group_end_index;
        }
        return true;
    }

    bool readPayloadsCoalesced(std::vector<TailIndexEntry> const& matches,
                               std::vector<std::string>& payloads,
                               TailReadStats& stats)
    {
        payloads.assign(matches.size(), std::string());
        std::vector<TailReadPlanEntry> read_plan;
        read_plan.reserve(matches.size());
        for(std::size_t index = 0; index < matches.size(); ++index)
        {
            read_plan.push_back(TailReadPlanEntry{matches[index], index});
        }
        std::sort(read_plan.begin(), read_plan.end(), [](TailReadPlanEntry const& lhs, TailReadPlanEntry const& rhs) {
            return std::tie(lhs.entry.segment_index, lhs.entry.payload_offset, lhs.entry.event_time) <
                   std::tie(rhs.entry.segment_index, rhs.entry.payload_offset, rhs.entry.event_time);
        });

        static constexpr uint64_t max_coalesced_read_bytes = 4 * 1024 * 1024;
        for(std::size_t group_begin = 0; group_begin < read_plan.size();)
        {
            TailReadPlanEntry const& first = read_plan[group_begin];
            if(first.entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid tail index segment {}", first.entry.segment_index);
                return false;
            }

            uint64_t group_start = first.entry.payload_offset;
            uint64_t group_end = first.entry.payload_offset + first.entry.payload_size;
            std::size_t group_end_index = group_begin + 1;
            while(group_end_index < read_plan.size())
            {
                TailReadPlanEntry const& next = read_plan[group_end_index];
                if(next.entry.segment_index != first.entry.segment_index || next.entry.payload_offset < group_end)
                {
                    break;
                }
                uint64_t const next_end = next.entry.payload_offset + next.entry.payload_size;
                if(next_end - group_start > max_coalesced_read_bytes)
                {
                    break;
                }
                group_end = next_end;
                ++group_end_index;
            }

            auto const& segment = journalSegments[first.entry.segment_index];
            uint64_t const group_alloc_start_ns = KeeperAppendStats::nowNs();
            std::string group_buffer(group_end - group_start, '\0');
            stats.payload_alloc_ns += KeeperAppendStats::nowNs() - group_alloc_start_ns;
            ++stats.read_group_count;
            stats.physical_read_bytes += static_cast<uint64_t>(group_buffer.size());
            uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
            bool const read_ok = preadFull(segment.fd, segment.path, group_buffer.data(), group_buffer.size(), group_start);
            stats.payload_syscall_ns += KeeperAppendStats::nowNs() - syscall_start_ns;
            if(!read_ok)
            {
                return false;
            }

            for(std::size_t index = group_begin; index < group_end_index; ++index)
            {
                TailReadPlanEntry const& planned = read_plan[index];
                uint64_t const payload_alloc_start_ns = KeeperAppendStats::nowNs();
                std::string payload(planned.entry.payload_size, '\0');
                stats.payload_alloc_ns += KeeperAppendStats::nowNs() - payload_alloc_start_ns;
                stats.payload_copy_bytes += planned.entry.payload_size;
                if(planned.entry.payload_size > 0)
                {
                    uint64_t const memcpy_start_ns = KeeperAppendStats::nowNs();
                    std::memcpy(payload.data(),
                                group_buffer.data() + (planned.entry.payload_offset - group_start),
                                static_cast<std::size_t>(planned.entry.payload_size));
                    stats.payload_memcpy_ns += KeeperAppendStats::nowNs() - memcpy_start_ns;
                }
                payloads[planned.output_index] = std::move(payload);
            }
            group_begin = group_end_index;
        }
        return true;
    }

    bool readPayloadsVectored(std::vector<TailIndexEntry> const& matches,
                              std::vector<std::string>& payloads,
                              WalDrainReadStats* stats,
                              TailReadStats* tail_stats)
    {
        payloads.assign(matches.size(), std::string());
        std::vector<TailReadPlanEntry> read_plan;
        read_plan.reserve(matches.size());
        for(std::size_t index = 0; index < matches.size(); ++index)
        {
            uint64_t const alloc_start_ns = KeeperAppendStats::nowNs();
            payloads[index].resize(matches[index].payload_size);
            if(tail_stats != nullptr)
            {
                tail_stats->payload_alloc_ns += KeeperAppendStats::nowNs() - alloc_start_ns;
            }
            read_plan.push_back(TailReadPlanEntry{matches[index], index});
        }
        std::sort(read_plan.begin(), read_plan.end(), [](TailReadPlanEntry const& lhs, TailReadPlanEntry const& rhs) {
            return std::tie(lhs.entry.segment_index, lhs.entry.payload_offset, lhs.entry.event_time) <
                   std::tie(rhs.entry.segment_index, rhs.entry.payload_offset, rhs.entry.event_time);
        });

        static constexpr uint64_t max_vectored_read_bytes = 4 * 1024 * 1024;
        static constexpr std::size_t max_iovecs = 1024;
        for(std::size_t group_begin = 0; group_begin < read_plan.size();)
        {
            TailReadPlanEntry const& first = read_plan[group_begin];
            if(first.entry.segment_index >= journalSegments.size())
            {
                LOG_ERROR("[KeeperLocalJournal] invalid WAL batch segment {}", first.entry.segment_index);
                return false;
            }

            uint64_t group_start = first.entry.payload_offset;
            uint64_t group_end = first.entry.payload_offset + first.entry.payload_size;
            std::size_t group_end_index = group_begin + 1;
            std::size_t iovec_count = first.entry.payload_size > 0 ? 1 : 0;
            while(group_end_index < read_plan.size())
            {
                TailReadPlanEntry const& next = read_plan[group_end_index];
                if(next.entry.segment_index != first.entry.segment_index || next.entry.payload_offset < group_end)
                {
                    break;
                }
                uint64_t const next_end = next.entry.payload_offset + next.entry.payload_size;
                if(next_end - group_start > max_vectored_read_bytes)
                {
                    break;
                }
                std::size_t const additional_iovecs =
                        (next.entry.payload_offset > group_end ? 1 : 0) + (next.entry.payload_size > 0 ? 1 : 0);
                if(iovec_count + additional_iovecs > max_iovecs)
                {
                    break;
                }
                iovec_count += additional_iovecs;
                group_end = next_end;
                ++group_end_index;
            }

            auto const& segment = journalSegments[first.entry.segment_index];
            std::vector<iovec> iovecs;
            iovecs.reserve(iovec_count);
            std::deque<std::string> gap_buffers;
            uint64_t gap_bytes = 0;
            uint64_t payload_bytes = 0;
            uint64_t next_offset = group_start;
            for(std::size_t index = group_begin; index < group_end_index; ++index)
            {
                TailReadPlanEntry const& planned = read_plan[index];
                if(planned.entry.payload_offset > next_offset)
                {
                    uint64_t const current_gap = planned.entry.payload_offset - next_offset;
                    gap_bytes += current_gap;
                    uint64_t const gap_alloc_start_ns = KeeperAppendStats::nowNs();
                    gap_buffers.emplace_back(current_gap, '\0');
                    if(tail_stats != nullptr)
                    {
                        tail_stats->payload_alloc_ns += KeeperAppendStats::nowNs() - gap_alloc_start_ns;
                    }
                    iovecs.push_back(iovec{gap_buffers.back().data(), gap_buffers.back().size()});
                    next_offset = planned.entry.payload_offset;
                }
                if(planned.entry.payload_size > 0)
                {
                    std::string& payload = payloads[planned.output_index];
                    iovecs.push_back(iovec{payload.data(), payload.size()});
                    payload_bytes += planned.entry.payload_size;
                    next_offset += planned.entry.payload_size;
                }
            }
            uint64_t read_syscalls = 0;
            uint64_t const syscall_start_ns = KeeperAppendStats::nowNs();
            bool const read_ok = iovecs.empty() || preadvFull(segment.fd, segment.path, iovecs, group_start, &read_syscalls);
            uint64_t const syscall_ns = KeeperAppendStats::nowNs() - syscall_start_ns;
            if(!read_ok)
            {
                return false;
            }
            if(stats != nullptr)
            {
                ++stats->batch_count;
                stats->physical_read_bytes += group_end - group_start;
                stats->gap_bytes += gap_bytes;
                stats->read_syscall_count += read_syscalls;
                uint64_t const batch_records = static_cast<uint64_t>(group_end_index - group_begin);
                stats->max_batch_records =
                        stats->max_batch_records > batch_records ? stats->max_batch_records : batch_records;
            }
            if(tail_stats != nullptr)
            {
                ++tail_stats->read_group_count;
                tail_stats->physical_read_bytes += group_end - group_start;
                tail_stats->payload_copy_bytes += payload_bytes;
                tail_stats->payload_syscall_ns += syscall_ns;
            }
            group_begin = group_end_index;
        }
        return true;
    }

    ssize_t preadSome(int fd, void* data, std::size_t size, uint64_t offset)
    {
        for(;;)
        {
            ssize_t bytes_read = ::pread(fd, data, size, static_cast<off_t>(offset));
            if(bytes_read < 0 && errno == EINTR)
            {
                continue;
            }
            return bytes_read;
        }
    }

    bool preadFull(int fd, std::string const& path, void* data, std::size_t size, uint64_t offset)
    {
        auto* cursor = static_cast<char*>(data);
        std::size_t remaining = size;
        off_t current_offset = static_cast<off_t>(offset);
        while(remaining > 0)
        {
            ssize_t bytes_read = ::pread(fd, cursor, remaining, current_offset);
            if(bytes_read < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }
                LOG_ERROR("[KeeperLocalJournal] pread failed for {}: {}", path, std::strerror(errno));
                return false;
            }
            if(bytes_read == 0)
            {
                LOG_ERROR("[KeeperLocalJournal] truncated indexed payload while reading {}", path);
                return false;
            }
            cursor += bytes_read;
            remaining -= static_cast<std::size_t>(bytes_read);
            current_offset += bytes_read;
        }
        return true;
    }

    bool preadvFull(int fd,
                    std::string const& path,
                    std::vector<iovec> iovecs,
                    uint64_t offset,
                    uint64_t* syscall_count = nullptr)
    {
        std::size_t iov_index = 0;
        std::size_t remaining = 0;
        for(auto const& iov: iovecs)
        {
            remaining += iov.iov_len;
        }
        off_t current_offset = static_cast<off_t>(offset);
        while(remaining > 0)
        {
            ssize_t bytes_read = ::preadv(fd,
                                          iovecs.data() + iov_index,
                                          static_cast<int>(iovecs.size() - iov_index),
                                          current_offset);
            if(bytes_read >= 0 && syscall_count != nullptr)
            {
                ++(*syscall_count);
            }
            if(bytes_read < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }
                LOG_ERROR("[KeeperLocalJournal] preadv failed for {}: {}", path, std::strerror(errno));
                return false;
            }
            if(bytes_read == 0)
            {
                LOG_ERROR("[KeeperLocalJournal] truncated indexed payload while reading {}", path);
                return false;
            }

            std::size_t consumed = static_cast<std::size_t>(bytes_read);
            remaining -= consumed;
            current_offset += bytes_read;
            while(consumed > 0 && iov_index < iovecs.size())
            {
                if(consumed >= iovecs[iov_index].iov_len)
                {
                    consumed -= iovecs[iov_index].iov_len;
                    ++iov_index;
                }
                else
                {
                    iovecs[iov_index].iov_base = static_cast<char*>(iovecs[iov_index].iov_base) + consumed;
                    iovecs[iov_index].iov_len -= consumed;
                    consumed = 0;
                }
            }
        }
        return true;
    }

    Mode journalMode{Mode::Disabled};
    TailReadMode tailReadMode{TailReadMode::Auto};
    bool tailDirectReadIntoEvents{false};
    std::size_t tailBatchMaxEvents{0};
    std::size_t tailBatchMaxBytes{0};
    std::atomic_bool opened{false};
    std::size_t requestedShardCount{1};
    ShardPolicy shardPolicy{ShardPolicy::Mixed};
    std::size_t fdatasyncBatchEvents{64};
    bool vectoredRecordWrite{false};
    bool batchVectoredRecordWrite{false};
    bool atomicOffsets{false};
    bool singleWriterShards{false};
    std::size_t singleWriterBatchEvents{64};
    bool groupCommitWait{false};
    std::size_t groupCommitFlushEvents{1};
    uint64_t groupCommitFlushBytes{0};
    bool groupCommitStrictFlushEventCap{false};
    uint64_t groupCommitLargePayloadBytes{0};
    std::size_t groupCommitLargePayloadFlushEvents{64};
    uint64_t groupCommitWaitWindowUs{0};
    uint64_t groupCommitFlushWaitUs{0};
    std::size_t groupCommitQueueBoundaryMinEvents{0};
    uint64_t groupCommitQueueBoundaryMinPayloadBytes{0};
    std::size_t groupCommitQueueBoundaryWaitEveryN{1};
    bool groupCommitWaitForActiveAdmissions{false};
    std::size_t ownerDrainYields{0};
    bool asyncCallbackDispatch{false};
    bool asyncBatchCompletionDispatch{true};
    std::size_t callbackDispatchThreadCount{1};
    bool callbackBatchDrain{false};
    std::size_t callbackBatchDrainMax{0};
    uint64_t callbackBatchDrainMinPayloadBytes{0};
    bool durableCompleteBeforePublish{false};
    bool notifyOwnerOnlyOnEmpty{false};
    std::atomic<uint64_t> activeAdmissions{0};
    std::string journalPath;
    std::mutex journalMutex;
    std::unordered_map<StoryId, std::vector<TailIndexEntry>> recoveredTailIndexByStory;
    std::vector<JournalSegment> journalSegments;
    std::vector<std::unique_ptr<JournalShard>> appendShards;
    std::mutex callbackMutex;
    std::condition_variable callbackAvailable;
    std::deque<CallbackWork> callbackQueue;
    std::vector<std::thread> callbackThreads;
    bool callbackStop{false};
};

} // namespace chronolog

#endif
