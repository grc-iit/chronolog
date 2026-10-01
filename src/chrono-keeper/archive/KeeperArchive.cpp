#include "archive/KeeperArchive.h"

#include <algorithm>
#include <iostream>
#include <syncstream>

#include <absl/crc/crc32c.h>
#include "adapter/Convert.h"

namespace chronolog::keeper
{
namespace iv1 = internal::v1;
namespace
{
Range range(const Chunk& chunk) { return {Range::Axis::Hlc, chunk.start, chunk.end}; }

template <class T>
WatermarkReport report(const T& proto)
{
    return {proto.story_id(),
            convert::fromProto(proto.watermark()),
            proto.grapher_instance(),
            proto.highest_receipt(),
            {proto.pending_receipts().begin(), proto.pending_receipts().end()},
            proto.dropped()};
}
} // namespace

std::shared_ptr<grpc::Channel> KeeperArchive::archiveChannel(const std::string& endpoint)
{
    std::lock_guard lock(mu_);
    auto& channel = channels_[endpoint];
    if(channel)
        return channel;
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS, 1000);
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    channel = grpc::CreateCustomChannel(endpoint, grpc::InsecureChannelCredentials(), args);
    return channel;
}

KeeperArchive::KeeperArchive(WalJournal& journal,
                             const Membership& membership,
                             std::string process_id,
                             KeeperArchiveConfig config,
                             Now now)
    : journal_(journal)
    , membership_(membership)
    , process_id_(std::move(process_id))
    , config_(config)
    , now_(std::move(now))
{
    if(config_.story_chunk_duration_secs == 0 || config_.seal_interval_ms == 0 || config_.chunk_max_bytes == 0 ||
       config_.chunk_max_bytes > (64u << 20) || config_.chunk_max_events == 0 || config_.chunk_max_events > 65536 ||
       config_.frame_bytes == 0 || config_.frame_bytes > (4u << 20))
        throw std::invalid_argument("invalid Keeper archive configuration");
    for(const auto& seal: journal_.sealedChunks())
    {
        const auto& chunk = seal.chunk;
        auto& story = stories_[chunk.story_id];
        story.chain_end = std::max(story.chain_end, chunk.end);
        story.has_chain = true;
        auto events = journal_.read(chunk.story_id, range(chunk));
        size_t bytes = 0;
        if(events.ok())
            for(const auto& event: *events) bytes += convert::toProto(event).ByteSizeLong();
        if(seal.settled)
            continue;
        State state;
        state.chunk = chunk;
        state.bytes = bytes;
        state.settled = state.delivered = seal.settled;
        state.settled_at = state.activity = now_();
        chunks_[chunk.id] = std::move(state);
    }
}
KeeperArchive::~KeeperArchive() { stop(); }

Hlc KeeperArchive::align(Hlc hlc) const
{
    const int64_t duration = static_cast<int64_t>(config_.story_chunk_duration_secs) * 1'000'000'000;
    int64_t quotient = hlc.physical_ns / duration;
    if(hlc.physical_ns < 0 && hlc.physical_ns % duration != 0)
        --quotient;
    return {quotient * duration, 0};
}

absl::Status KeeperArchive::addChunk(Chunk chunk, size_t bytes)
{
    chunk.id = process_id_ + ":" + std::to_string(chunk.story_id) + ":" + std::to_string(chunk.start.physical_ns) +
               "." + std::to_string(chunk.start.logical);
    if(auto status = journal_.recordSeal(chunk); !status.ok())
        return status;
    std::lock_guard lock(mu_);
    auto& story = stories_[chunk.story_id];
    story.chain_end = chunk.end;
    story.has_chain = true;
    State state;
    state.chunk = std::move(chunk);
    state.bytes = bytes;
    state.activity = now_();
    if(story.dropped)
    {
        (void)journal_.recordSettled(state.chunk.id);
        journal_.eraseEvents(state.chunk.story_id, range(state.chunk));
    }
    else
        chunks_[state.chunk.id] = std::move(state);
    cv_.notify_all();
    return absl::OkStatus();
}

absl::Status KeeperArchive::seal(bool through_frontier)
{
    std::lock_guard seal_lock(seal_mu_);
    const Hlc tick = journal_.sealTick();
    for(StoryId story_id: journal_.storyIds())
    {
        auto route = membership_.route(story_id);
        if(!route.ok() || route->grapher.empty())
            continue;
        auto snapshot = journal_.sealedRead(story_id, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}}, tick);
        if(!snapshot.ok())
            return snapshot.status();
        const Hlc end = through_frontier ? snapshot->view.sealed : align(snapshot->view.sealed);
        Hlc start;
        {
            std::lock_guard lock(mu_);
            auto& story = stories_[story_id];
            if(story.dropped)
                continue;
            if(story.has_chain)
                start = story.chain_end;
            else
            {
                if(snapshot->events.empty())
                    continue;
                start = align(snapshot->events.front().hlc);
            }
        }
        if(start >= end)
            continue;
        size_t bytes = 0;
        uint32_t events = 0;
        for(const auto& event: snapshot->events)
        {
            if(event.hlc < start || event.hlc >= end)
                continue;
            const size_t event_bytes = convert::toProto(event).ByteSizeLong();
            size_t record_bytes = event_bytes + 2;
            for(size_t value = event_bytes; value >= 128; value >>= 7) ++record_bytes;
            if(events != 0 && (bytes + record_bytes > config_.chunk_max_bytes || events == config_.chunk_max_events))
            {
                Chunk chunk;
                chunk.story_id = story_id;
                chunk.start = start;
                chunk.end = event.hlc;
                if(auto status = addChunk(std::move(chunk), bytes); !status.ok())
                    return status;
                start = event.hlc;
                bytes = 0;
                events = 0;
            }
            if(record_bytes > config_.chunk_max_bytes)
                return absl::ResourceExhaustedError("one event exceeds chunk_max_bytes");
            bytes += record_bytes;
            ++events;
        }
        if(events != 0)
        {
            Chunk chunk;
            chunk.story_id = story_id;
            chunk.start = start;
            chunk.end = end;
            if(auto status = addChunk(std::move(chunk), bytes); !status.ok())
                return status;
        }
    }
    return absl::OkStatus();
}

void KeeperArchive::settleLocked(State& state)
{
    if(!state.delivered || state.settled || state.receipt == 0)
        return;
    auto& story = stories_.at(state.chunk.story_id);
    auto view = story.receipts.find(state.instance);
    if(view == story.receipts.end() || state.receipt > view->second.highest ||
       view->second.pending.contains(state.receipt))
        return;
    if(auto status = journal_.recordSettled(state.chunk.id); !status.ok())
        return;
    std::osyncstream(std::clog) << "archive_settled chunk=" << state.chunk.id << " story=" << state.chunk.story_id
                                << std::endl;
    state.settled = true;
    state.settled_at = now_();
}

bool KeeperArchive::safe(const State& state) const
{
    return state.delivered && state.settled && !state.inflight &&
           state.chunk.end <= stories_.at(state.chunk.story_id).watermark &&
           now_() - state.settled_at >= std::chrono::seconds(config_.archive_visibility_delay_secs);
}

void KeeperArchive::collectLocked()
{
    for(auto it = chunks_.begin(); it != chunks_.end();)
    {
        if(!it->second.tail && safe(it->second))
        {
            journal_.eraseEvents(it->second.chunk.story_id, range(it->second.chunk), true);
            it = chunks_.erase(it);
        }
        else
            ++it;
    }
}

void KeeperArchive::delivered(const std::string& id, const iv1::ChunkReceipt& receipt, size_t bytes)
{
    if(receipt.status().code() != 0 || receipt.bytes() != bytes || receipt.receipt() == 0 ||
       receipt.grapher_instance().empty() || receipt.chunk_id() != id)
        return sendFailed(id);
    std::lock_guard lock(mu_);
    auto it = chunks_.find(id);
    if(it == chunks_.end())
        return;
    auto& state = it->second;
    state.inflight = false;
    state.activity = now_();
    state.failures = 0;
    state.delivered = true;
    state.instance = receipt.grapher_instance();
    state.receipt = receipt.receipt();
    settleLocked(state);
    collectLocked();
    cv_.notify_all();
}

void KeeperArchive::sendFailed(const std::string& id)
{
    std::lock_guard lock(mu_);
    auto it = chunks_.find(id);
    if(it == chunks_.end())
        return;
    auto& state = it->second;
    state.inflight = false;
    state.failures = std::min(state.failures + 1, 5u);
    state.retry_at = now_() + std::chrono::milliseconds(100u << (state.failures - 1));
    cv_.notify_all();
}

void KeeperArchive::applyReport(const WatermarkReport& report)
{
    std::lock_guard lock(mu_);
    auto& story = stories_[report.story_id];
    if(report.dropped)
    {
        story.dropped = true;
        for(auto it = chunks_.begin(); it != chunks_.end();)
        {
            if(it->second.chunk.story_id != report.story_id)
            {
                ++it;
                continue;
            }
            (void)journal_.recordSettled(it->first);
            journal_.eraseEvents(report.story_id, range(it->second.chunk), true);
            it = chunks_.erase(it);
        }
        journal_.eraseEvents(report.story_id, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
        cv_.notify_all();
        return;
    }
    story.watermark = std::max(story.watermark, report.watermark);
    if(!report.grapher_instance.empty())
    {
        auto& view = story.receipts[report.grapher_instance];
        const std::set<uint64_t> pending(report.pending_receipts.begin(), report.pending_receipts.end());
        for(auto it = view.pending.begin(); it != view.pending.end();)
            if(*it <= report.highest_receipt && !pending.contains(*it))
                it = view.pending.erase(it);
            else
                ++it;
        for(auto receipt: pending)
            if(receipt > view.highest && receipt <= report.highest_receipt)
                view.pending.insert(receipt);
        view.highest = std::max(view.highest, report.highest_receipt);
    }
    for(auto& [id, state]: chunks_)
        if(state.chunk.story_id == report.story_id)
            settleLocked(state);
    collectLocked();
    cv_.notify_all();
}

void KeeperArchive::releaseTail(StoryId story)
{
    std::lock_guard lock(mu_);
    for(auto& [id, state]: chunks_)
        if(state.chunk.story_id == story)
            state.tail = false;
    collectLocked();
}
void KeeperArchive::sweep()
{
    std::lock_guard lock(mu_);
    uint64_t bytes = 0;
    for(const auto& [id, state]: chunks_) bytes += state.bytes;
    const uint64_t cap =
            config_.retention_cap_mb > UINT64_MAX / (1u << 20) ? UINT64_MAX : config_.retention_cap_mb * (1u << 20);
    if(config_.retention_cap_mb != 0 && bytes > cap)
    {
        std::vector<State*> tail_only;
        for(auto& [id, state]: chunks_)
            if(safe(state))
                tail_only.push_back(&state);
        std::sort(tail_only.begin(),
                  tail_only.end(),
                  [](const State* a, const State* b) { return a->chunk.start < b->chunk.start; });
        for(auto* state: tail_only)
        {
            state->tail = false;
            bytes -= state->bytes;
            if(bytes <= cap)
                break;
        }
        if(bytes > cap && !cap_warned_)
        {
            cap_warned_ = true;
            std::cerr << "chrono_keeper: retention cap exceeded by archive-protected chunks\n";
        }
    }
    for(auto& [id, state]: chunks_)
        if(safe(state))
            state.tail = false;
    collectLocked();
}

std::vector<Chunk> KeeperArchive::chunks() const
{
    std::lock_guard lock(mu_);
    std::vector<Chunk> out;
    for(const auto& [id, state]: chunks_) out.push_back(state.chunk);
    std::sort(out.begin(),
              out.end(),
              [](const Chunk& a, const Chunk& b)
              { return std::tie(a.story_id, a.start) < std::tie(b.story_id, b.start); });
    return out;
}
Hlc KeeperArchive::knownWatermark(StoryId story) const
{
    std::lock_guard lock(mu_);
    auto it = stories_.find(story);
    return it == stories_.end() ? Hlc{} : it->second.watermark;
}

bool KeeperArchive::shipOne(std::stop_token stop)
{
    Chunk chunk;
    {
        std::lock_guard lock(mu_);
        for(auto& [id, state]: chunks_)
        {
            if(state.inflight || state.settled || now_() < state.retry_at ||
               (state.delivered &&
                now_() - state.activity <
                        (draining_ ? std::chrono::milliseconds(1000)
                                   : std::chrono::milliseconds(1000ull * config_.watermark_resend_timeout_secs))))
                continue;
            auto route = membership_.route(state.chunk.story_id);
            if(!route.ok() || route->grapher.empty())
                continue;
            state.inflight = true;
            chunk = state.chunk;
            break;
        }
    }
    if(chunk.id.empty())
        return false;
    auto fail = [&]
    {
        sendFailed(chunk.id);
        return true;
    };
    auto route = membership_.route(chunk.story_id);
    if(!route.ok() || route->grapher.empty())
        return fail();
    auto events = journal_.read(chunk.story_id, range(chunk));
    if(!events.ok())
        return fail();
    iv1::ChunkPayload payload;
    for(const auto& event: *events) *payload.add_events() = convert::toProto(event);
    const auto bytes = payload.SerializeAsString();
    const auto crc = static_cast<uint32_t>(absl::ComputeCrc32c(bytes));
    std::string checksum;
    for(int shift = 24; shift >= 0; shift -= 8) checksum.push_back(static_cast<char>(crc >> shift));
    auto stub = iv1::Archive::NewStub(archiveChannel(route->grapher));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    std::stop_callback cancel(stop, [&] { context.TryCancel(); });
    iv1::TransferChunkResponse response;
    std::osyncstream(std::clog) << "archive_transfer_start chunk=" << chunk.id << " grapher=" << route->grapher
                                << std::endl;
    auto stream = stub->TransferChunk(&context, &response);
    auto finish = [&]
    {
        const auto status = stream->Finish();
        if(status.error_code() == grpc::StatusCode::NOT_FOUND ||
           response.status().code() == static_cast<int>(absl::StatusCode::kNotFound))
        {
            applyReport({chunk.story_id, {}, {}, 0, {}, true});
            return true;
        }
        if(!status.ok())
        {
            std::cerr << "chrono_keeper: archive transfer " << chunk.id << " failed: " << status.error_code() << " "
                      << status.error_message() << '\n';
            return fail();
        }
        iv1::ChunkReceipt receipt;
        *receipt.mutable_status() = response.status();
        receipt.set_chunk_id(response.chunk_id());
        receipt.set_bytes(response.bytes());
        receipt.set_grapher_instance(response.grapher_instance());
        receipt.set_receipt(response.receipt());
        delivered(chunk.id, receipt, bytes.size());
        return true;
    };
    size_t offset = 0;
    do {
        iv1::TransferChunkRequest frame;
        auto* identity = frame.mutable_identity();
        identity->set_chunk_id(chunk.id);
        identity->set_story_id(chunk.story_id);
        *identity->mutable_start() = convert::toProto(chunk.start);
        *identity->mutable_end() = convert::toProto(chunk.end);
        frame.set_offset(offset);
        frame.set_total_bytes(bytes.size());
        frame.set_checksum(checksum);
        frame.set_checksum_algorithm(iv1::CHECKSUM_ALGORITHM_CRC32C);
        const auto size = std::min({config_.frame_bytes, size_t{1u << 20}, bytes.size() - offset});
        frame.set_data(bytes.data() + offset, size);
        offset += size;
        frame.set_final(offset == bytes.size());
        if(!stream->Write(frame))
        {
            stream->WritesDone();
            return finish();
        }
    } while(offset < bytes.size());
    stream->WritesDone();
    return finish();
}

void KeeperArchive::refreshSubscriptions()
{
    std::map<std::string, std::set<StoryId>> desired;
    {
        std::lock_guard lock(mu_);
        for(const auto& [id, state]: chunks_)
        {
            auto route = membership_.route(state.chunk.story_id);
            if(route.ok() && !route->grapher.empty())
                desired[route->grapher].insert(state.chunk.story_id);
        }
    }
    for(const auto& [endpoint, stories]: desired)
    {
        auto& subscription = subscriptions_[endpoint];
        if(!subscription)
        {
            subscription = std::make_unique<Subscription>();
            subscription->stories = stories;
            auto* ptr = subscription.get();
            subscription->watcher = std::make_unique<Watcher>([this, endpoint, ptr](std::stop_token stop)
                                                              { return watch(endpoint, *ptr, stop); });
        }
        else
        {
            std::lock_guard lock(subscription->mu);
            if(subscription->stories != stories)
            {
                subscription->stories = stories;
                if(subscription->context)
                    subscription->context->TryCancel();
            }
        }
    }
    for(auto it = subscriptions_.begin(); it != subscriptions_.end();)
        if(!desired.contains(it->first))
            it = subscriptions_.erase(it);
        else
            ++it;
}

bool KeeperArchive::watch(const std::string& endpoint, Subscription& subscription, std::stop_token stop)
{
    auto stub = iv1::Archive::NewStub(archiveChannel(endpoint));
    auto context = std::make_shared<grpc::ClientContext>();
    context->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
    iv1::WatchWatermarksRequest request;
    request.set_keeper_id(process_id_);
    {
        std::lock_guard lock(subscription.mu);
        subscription.context = context;
        for(auto story: subscription.stories) request.add_story_ids(story);
    }
    std::stop_callback cancel(stop, [context] { context->TryCancel(); });
    auto stream = stub->WatchWatermarks(context.get(), request);
    iv1::WatchWatermarksResponse response;
    bool progress = false;
    while(stream->Read(&response))
    {
        applyReport(report(response));
        progress = true;
    }
    (void)stream->Finish();
    {
        std::lock_guard lock(subscription.mu);
        subscription.context.reset();
    }
    return progress;
}

void KeeperArchive::start()
{
    if(sealer_.joinable())
        return;
    sealer_ = std::jthread(
            [this](std::stop_token stop)
            {
                while(!stop.stop_requested())
                {
                    if(auto status = seal(); !status.ok())
                        std::cerr << "chrono_keeper: archive seal failed: " << status << '\n';
                    sweep();
                    refreshSubscriptions();
                    std::unique_lock lock(mu_);
                    cv_.wait_for(lock, stop, std::chrono::milliseconds(config_.seal_interval_ms), [] { return false; });
                }
            });
    shipper_ = std::jthread(
            [this](std::stop_token stop)
            {
                while(!stop.stop_requested())
                {
                    if(shipOne(stop))
                        continue;
                    std::unique_lock lock(mu_);
                    cv_.wait_for(lock, stop, std::chrono::milliseconds(100), [] { return false; });
                }
            });
}
bool KeeperArchive::shutdown()
{
    const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(config_.shutdown_confirm_timeout_secs);
    sealer_.request_stop();
    cv_.notify_all();
    if(sealer_.joinable())
        sealer_.join();
    auto status = journal_.flush();
    if(status.ok())
        status = seal(true);
    {
        std::lock_guard lock(mu_);
        draining_ = true;
        for(auto& [id, state]: chunks_)
        {
            state.activity = now_() - std::chrono::seconds(config_.watermark_resend_timeout_secs);
            state.retry_at = now_();
        }
    }
    refreshSubscriptions();
    if(!shipper_.joinable())
        shipper_ = std::jthread(
                [this](std::stop_token stop)
                {
                    while(!stop.stop_requested())
                    {
                        if(shipOne(stop))
                            continue;
                        std::unique_lock lock(mu_);
                        cv_.wait_for(lock, stop, std::chrono::milliseconds(20), [] { return false; });
                    }
                });
    cv_.notify_all();
    bool confirmed;
    {
        std::unique_lock lock(mu_);
        confirmed = cv_.wait_until(lock,
                                   deadline,
                                   [this] {
                                       return std::all_of(chunks_.begin(),
                                                          chunks_.end(),
                                                          [](const auto& entry) { return entry.second.settled; });
                                   });
    }
    stop();
    return status.ok() && confirmed;
}

void KeeperArchive::stop()
{
    sealer_.request_stop();
    shipper_.request_stop();
    cv_.notify_all();
    if(sealer_.joinable())
        sealer_.join();
    if(shipper_.joinable())
        shipper_.join();
    subscriptions_.clear();
}
} // namespace chronolog::keeper
