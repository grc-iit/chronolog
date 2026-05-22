/**
 * ingestion_keeper_server.cpp
 *
 * Thallium server for the network-aware ingestion kernel benchmark.
 * Receives LogEvents over the network and routes them through a switchable
 * IngestionQueue, mirroring ChronoKeeper's ingestion pipeline.
 *
 * Usage:
 *   ingestion_keeper_server <listen_addr>
 *       [--queue mutex|lockfree]   queue implementation (default: mutex)
 *       [--threads N]              ingestion handler ULT count (default: 4)
 *       [--stories N]              story handle count (default: 4)
 *
 *   <listen_addr>  Thallium address, e.g.:
 *                    ofi+sockets://0.0.0.0:5600
 *                    ofi+verbs://0.0.0.0:5600
 *
 * RPCs:
 *   reset_stats()                                     → int (0)
 *   record_event(LogEvent)                            → uint64_t events_received
 *   record_event_oneway(LogEvent)                     → (no response; fire-and-forget)
 *   record_events_bulk(bulk&, n_events, payload_size) → uint64_t events_received
 *   wait_for(uint64_t target)                         → uint64_t events_received
 *   do_merge()                                        → uint64_t merged_events
 *   get_stats()  → string "received,ingest_ms,merged,merge_ms,..."
 *
 * The server is killed externally (SIGKILL) after each benchmark point.
 * Timing model:
 *   ingest_ms  = wall time from first to last received event (server-side)
 *   merge_ms   = time to drain IngestionQueue and sort into per-story maps
 *   pipeline   = ingest_ms + merge_ms
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <margo.h>

#include "concurrentqueue.h"

namespace tl = thallium;

// ---------------------------------------------------------------------------
// LogEvent — self-contained replica with thallium serialization.
// Fields and semantics match chronolog::LogEvent in the production codebase.
// ---------------------------------------------------------------------------
namespace chronolog
{
typedef uint64_t chrono_time;
typedef uint64_t ClientId;
typedef uint32_t chrono_index;
typedef uint64_t StoryId;
typedef std::tuple<chrono_time, ClientId, chrono_index> EventSequence;

struct LogEvent
{
    StoryId     storyId    = 0;
    uint64_t    eventTime  = 0;
    ClientId    clientId   = 0;
    uint32_t    eventIndex = 0;
    std::string logRecord;

    LogEvent() = default;
    LogEvent(StoryId sid, uint64_t t, ClientId cid, uint32_t idx, std::string rec)
        : storyId(sid), eventTime(t), clientId(cid), eventIndex(idx)
        , logRecord(std::move(rec))
    {}

    uint64_t time()  const { return eventTime;  }
    uint32_t index() const { return eventIndex; }

    template <typename A>
    void serialize(A& ar) { ar(storyId, eventTime, clientId, eventIndex, logRecord); }
};

typedef std::deque<LogEvent> EventDeque;
} // namespace chronolog

using EventSequence = chronolog::EventSequence;
using LogEvent      = chronolog::LogEvent;
using EventDeque    = chronolog::EventDeque;

// ---------------------------------------------------------------------------
// Per-story ingestion handles — same designs as ingestion_kernel_bench.cpp
// ---------------------------------------------------------------------------
class StoryIngestionHandleBase
{
public:
    virtual ~StoryIngestionHandleBase() = default;
    virtual void ingestEvent(LogEvent const& ev) = 0;
    virtual void drainInto(std::vector<LogEvent>& out) = 0;
};

// std::deque + std::mutex — mirrors production StoryIngestionHandle.
class MutexIngestionHandle : public StoryIngestionHandleBase
{
public:
    MutexIngestionHandle() : activeDeque(&deque1), passiveDeque(&deque2) {}

    void ingestEvent(LogEvent const& ev) override
    {
        std::lock_guard<std::mutex> lk(mtx);
        activeDeque->push_back(ev);
    }

    void drainInto(std::vector<LogEvent>& out) override
    {
        { std::lock_guard<std::mutex> lk(mtx); std::swap(activeDeque, passiveDeque); }
        while(!passiveDeque->empty())
        {
            out.push_back(std::move(passiveDeque->front()));
            passiveDeque->pop_front();
        }
    }

private:
    std::mutex  mtx;
    EventDeque  deque1, deque2;
    EventDeque* activeDeque;
    EventDeque* passiveDeque;
};

// moodycamel::ConcurrentQueue — lock-free MPMC.
class LockFreeIngestionHandle : public StoryIngestionHandleBase
{
public:
    explicit LockFreeIngestionHandle(size_t cap = 4096) : queue(cap) {}

    void ingestEvent(LogEvent const& ev) override { queue.enqueue(ev); }

    void drainInto(std::vector<LogEvent>& out) override
    {
        static constexpr size_t BULK = 256;
        LogEvent batch[BULK];
        size_t got;
        while((got = queue.try_dequeue_bulk(batch, BULK)) > 0)
            for(size_t i = 0; i < got; ++i)
                out.push_back(std::move(batch[i]));
    }

private:
    moodycamel::ConcurrentQueue<LogEvent> queue;
};

// Routes events to per-story handles by storyId.
class IngestionQueue
{
public:
    void registerHandle(uint64_t sid, StoryIngestionHandleBase* h) { handles[sid] = h; }

    void ingestEvent(LogEvent const& ev)
    {
        auto it = handles.find(ev.storyId);
        if(it != handles.end()) it->second->ingestEvent(ev);
    }

private:
    std::unordered_map<uint64_t, StoryIngestionHandleBase*> handles;
};

// ---------------------------------------------------------------------------
// Wire layout for RDMA bulk transfer (fixed-size event header + payload).
// Client packs N events as:  [EventPOD][payload bytes] × N
// stride = sizeof(EventPOD) + payload_size  (passed alongside the bulk handle)
// ---------------------------------------------------------------------------
struct alignas(8) EventPOD
{
    uint64_t storyId;
    uint64_t eventTime;
    uint64_t clientId;
    uint32_t eventIndex;
    uint32_t payloadLen;
};
static_assert(sizeof(EventPOD) == 32, "EventPOD layout mismatch");

// ---------------------------------------------------------------------------
// Pre-pinned server-side RDMA receive buffer pool.
//
// Each slot is allocated and registered with ibv_reg_mr exactly once at
// startup via engine.expose().  Per-event handlers acquire a slot by
// round-robin atomic counter — no allocation, no registration per call.
//
// Safety: pool_size is set to max(n_threads*4, 32).  At most n_threads
// handler ULTs are in-flight simultaneously (Argobots cooperative scheduler),
// so slot reuse only happens after pool_size events, well after the original
// user is done.
// ---------------------------------------------------------------------------
struct BulkSlot
{
    std::vector<char>                     buf;
    std::vector<std::pair<void*, size_t>> segs;
    tl::bulk                              bulk;  // pre-pinned write_only
};

struct BulkPool
{
    std::vector<BulkSlot> slots;
    std::atomic<uint64_t> next{0};

    void init(tl::engine& eng, size_t n_slots, size_t slot_size)
    {
        slots.resize(n_slots);
        for(size_t i = 0; i < n_slots; ++i)
        {
            slots[i].buf.assign(slot_size, '\0');
            slots[i].segs = {{slots[i].buf.data(), slot_size}};
            slots[i].bulk = eng.expose(slots[i].segs, tl::bulk_mode::write_only);
        }
    }

    BulkSlot& acquire()
    {
        size_t idx = static_cast<size_t>(
            next.fetch_add(1, std::memory_order_relaxed) % slots.size());
        return slots[idx];
    }
};

// ---------------------------------------------------------------------------
// Server state (shared across all RPC handler ULTs)
// ---------------------------------------------------------------------------
enum class QueueMode { MUTEX, LOCKFREE };

struct ServerState
{
    size_t    n_stories;
    size_t    n_threads;
    QueueMode mode;

    std::vector<std::unique_ptr<StoryIngestionHandleBase>> handles;
    IngestionQueue                                          queue;
    std::vector<std::map<EventSequence, LogEvent>>         chunks; // per-story sorted maps

    std::atomic<uint64_t> events_received{0};
    // Timing: nanoseconds from steady_clock. Atomics for concurrent updates.
    std::atomic<int64_t>  first_ns{std::numeric_limits<int64_t>::max()};
    std::atomic<int64_t>  last_ns {std::numeric_limits<int64_t>::min()};

    // Per-event timing accumulators (nanoseconds, reset each rep)
    std::atomic<int64_t> total_handler_ns{0};
    std::atomic<int64_t> total_enqueue_ns{0};
    std::atomic<int64_t> total_rdma_ns{0};
    std::atomic<int64_t> total_deser_ns{0};

    double   merge_ms      = 0.0;
    uint64_t merged_events = 0;

    explicit ServerState(size_t stories, size_t threads, QueueMode m)
        : n_stories(stories), n_threads(threads), mode(m), chunks(stories)
    {
        handles.reserve(stories);
        for(size_t s = 0; s < stories; ++s)
        {
            if(mode == QueueMode::LOCKFREE)
                handles.push_back(std::make_unique<LockFreeIngestionHandle>());
            else
                handles.push_back(std::make_unique<MutexIngestionHandle>());
            queue.registerHandle(s, handles[s].get());
        }
    }

    void reset()
    {
        events_received.store(0,  std::memory_order_relaxed);
        first_ns.store(std::numeric_limits<int64_t>::max(), std::memory_order_relaxed);
        last_ns.store (std::numeric_limits<int64_t>::min(), std::memory_order_relaxed);
        total_handler_ns.store(0, std::memory_order_relaxed);
        total_enqueue_ns.store(0, std::memory_order_relaxed);
        total_rdma_ns.store(0, std::memory_order_relaxed);
        total_deser_ns.store(0, std::memory_order_relaxed);
        merge_ms      = 0.0;
        merged_events = 0;
        for(auto& m : chunks) m.clear();
    }

    // Update arrival timestamps and event counter atomically.
    void record_arrival(uint64_t n = 1)
    {
        int64_t now = static_cast<int64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());

        // Atomic min for first_ns
        int64_t prev = first_ns.load(std::memory_order_relaxed);
        while(prev > now)
            if(first_ns.compare_exchange_weak(prev, now, std::memory_order_relaxed)) break;

        // Atomic max for last_ns
        prev = last_ns.load(std::memory_order_relaxed);
        while(prev < now)
            if(last_ns.compare_exchange_weak(prev, now, std::memory_order_relaxed)) break;

        events_received.fetch_add(n, std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// CLI options
// ---------------------------------------------------------------------------
struct Options
{
    std::string addr;
    QueueMode   mode             = QueueMode::MUTEX;
    size_t      threads          = 4;
    size_t      progress_threads = 1;
    size_t      stories          = 4;
    size_t      payload_size     = 4096;  // expected payload bytes; sizes the RDMA receive pool
};

static Options parse_args(int argc, char** argv)
{
    Options opts;
    if(argc < 2)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <listen_addr> [--queue mutex|lockfree]"
                     " [--threads N] [--progress-threads N] [--stories N] [--size N]\n";
        std::exit(1);
    }
    opts.addr = argv[1];
    for(int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        auto need = [&]() -> std::string
        {
            if(++i >= argc) { std::cerr << a << " requires a value\n"; std::exit(1); }
            return argv[i];
        };
        if     (a == "--stories")           opts.stories          = std::stoull(need());
        else if(a == "--threads")           opts.threads          = std::stoull(need());
        else if(a == "--progress-threads")  opts.progress_threads = std::stoull(need());
        else if(a == "--size")              opts.payload_size     = std::stoull(need());
        else if(a == "--queue")
        {
            std::string v = need();
            if     (v == "mutex"    || v == "stl")        opts.mode = QueueMode::MUTEX;
            else if(v == "lockfree" || v == "moodycamel") opts.mode = QueueMode::LOCKFREE;
            else { std::cerr << "Unknown queue mode: " << v << "\n"; std::exit(1); }
        }
        else { std::cerr << "Unknown option: " << a << "\n"; std::exit(1); }
    }
    if(opts.threads          == 0) opts.threads          = 1;
    if(opts.progress_threads == 0) opts.progress_threads = 1;
    return opts;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    Options opts = parse_args(argc, argv);
    ServerState state(opts.stories, opts.threads, opts.mode);

    // Initialize Margo/Thallium with 1 dedicated progress thread + handler pool.
    // Multiple progress ESs require a thread-safe transport (ofi+tcp);
    // ofi+verbs;ofi_rxm is not safe for concurrent HG_Progress calls.
    tl::engine engine(opts.addr, THALLIUM_SERVER_MODE,
                      /*use_progress_thread=*/true,
                      static_cast<std::int32_t>(opts.threads));

    // If more than 1 progress thread is requested, retrieve the existing progress
    // pool (already created by Margo) and spin up additional ESs on it.
    // Margo's HG_Progress loop is entered by whichever ES picks up the ULT, so
    // N ESs on the same pool yields N concurrent progress threads.
    std::vector<tl::managed<tl::xstream>> extra_progress_xstreams;
    if(opts.progress_threads > 1)
    {
        ABT_pool prog_pool = ABT_POOL_NULL;
        margo_get_progress_pool(engine.get_margo_instance(), &prog_pool);
        tl::pool tl_prog_pool(prog_pool);
        for(size_t i = 1; i < opts.progress_threads; ++i)
            extra_progress_xstreams.push_back(
                tl::xstream::create(tl::scheduler::predef::basic_wait, tl_prog_pool));
    }

    // Pre-pin receive buffer pool for the rdma-pool path.
    // pool_size >> n_threads ensures round-robin reuse never hits an in-use slot.
    size_t pool_n    = std::max(opts.threads * 4, size_t{32});
    size_t pool_slot = sizeof(EventPOD) + opts.payload_size;
    BulkPool rdma_pool;
    rdma_pool.init(engine, pool_n, pool_slot);

    std::cerr << "[keeper_server] address=" << engine.self()
              << "  queue=" << (opts.mode == QueueMode::LOCKFREE ? "lockfree" : "mutex")
              << "  threads=" << opts.threads
              << "  progress_threads=" << opts.progress_threads
              << "  stories=" << opts.stories
              << "  rdma_pool=" << pool_n << "x" << pool_slot << "B\n";

    // ---- reset_stats: clear counters and sorted maps for the next rep ----
    engine.define("reset_stats",
        [&state](tl::request const& req)
        {
            state.reset();
            req.respond(0);
        });

    // ---- record_event: send/recv path — one LogEvent per RPC call ----
    engine.define("record_event",
        [&state](tl::request const& req, LogEvent const& ev)
        {
            auto h0 = std::chrono::steady_clock::now();
            auto e0 = h0;
            state.queue.ingestEvent(ev);
            auto e1 = std::chrono::steady_clock::now();
            state.record_arrival();
            req.respond(state.events_received.load(std::memory_order_relaxed));
            auto h1 = std::chrono::steady_clock::now();
            state.total_enqueue_ns.fetch_add((e1 - e0).count(), std::memory_order_relaxed);
            state.total_handler_ns.fetch_add((h1 - h0).count(), std::memory_order_relaxed);
        });

    // ---- record_event_oneway: fire-and-forget — no respond(), eliminates ~38 µs respond cost ----
    // Client returns as soon as the request is queued in Mercury's send buffer.
    // Server handler measures only enqueue time; avg_handler_us ≈ avg_enqueue_us.
    // Use wait_for() to synchronize before do_merge().
    engine.define("record_event_oneway",
        [&state](tl::request const& /*req*/, LogEvent const& ev)
        {
            // disable_response() is set — req.respond() must NOT be called.
            auto h0 = std::chrono::steady_clock::now();
            auto e0 = h0;
            state.queue.ingestEvent(ev);
            auto e1 = std::chrono::steady_clock::now();
            state.record_arrival();
            state.total_enqueue_ns.fetch_add((e1 - e0).count(), std::memory_order_relaxed);
            state.total_handler_ns.fetch_add((e1 - h0).count(), std::memory_order_relaxed);
        }
    ).disable_response();

    // ---- record_events_batch: batched send/recv — N LogEvents per RPC, no RDMA ----
    // Reduces per-event RPC overhead by factor of batch_size while keeping the
    // proven ofi+verbs;ofi_rxm messaging path.  Queue throughput becomes the
    // bottleneck, making mutex vs lockfree differences visible.
    engine.define("record_events_batch",
        [&state](tl::request const& req, std::vector<LogEvent> const& batch)
        {
            for(auto const& ev : batch)
                state.queue.ingestEvent(ev);
            state.record_arrival(batch.size());
            req.respond(state.events_received.load(std::memory_order_relaxed));
        });

    // ---- record_event_rdma: per-event RDMA pull — same call frequency as record_event ----
    // Client pre-pins all events in one bulk descriptor; for each event it creates a
    // per-event sub-bulk (MR-cache hit) and calls this RPC.  Server RDMA-pulls one
    // EventPOD + payload (~stride bytes) per call, bypassing message-body serialization.
    engine.define("record_event_rdma",
        [&state, eng = engine](tl::request const& req,
                               tl::bulk& client_bulk,
                               uint64_t  payload_size) mutable
        {
            auto h0 = std::chrono::steady_clock::now();

            size_t stride = sizeof(EventPOD) + static_cast<size_t>(payload_size);
            std::vector<char> buf(stride);
            std::vector<std::pair<void*, size_t>> segs = {{buf.data(), stride}};
            tl::bulk server_buf = eng.expose(segs, tl::bulk_mode::write_only);

            auto r0 = std::chrono::steady_clock::now();
            client_bulk.on(req.get_endpoint()) >> server_buf;
            auto r1 = std::chrono::steady_clock::now();

            auto d0 = r1;
            const auto* h = reinterpret_cast<const EventPOD*>(buf.data());
            LogEvent ev;
            ev.storyId    = h->storyId;
            ev.eventTime  = h->eventTime;
            ev.clientId   = h->clientId;
            ev.eventIndex = h->eventIndex;
            ev.logRecord.assign(buf.data() + sizeof(EventPOD),
                                static_cast<size_t>(h->payloadLen));
            auto d1 = std::chrono::steady_clock::now();

            auto e0 = d1;
            state.queue.ingestEvent(ev);
            auto e1 = std::chrono::steady_clock::now();

            state.record_arrival();
            req.respond(state.events_received.load(std::memory_order_relaxed));
            auto h1 = std::chrono::steady_clock::now();

            state.total_rdma_ns.fetch_add   ((r1 - r0).count(), std::memory_order_relaxed);
            state.total_deser_ns.fetch_add  ((d1 - d0).count(), std::memory_order_relaxed);
            state.total_enqueue_ns.fetch_add((e1 - e0).count(), std::memory_order_relaxed);
            state.total_handler_ns.fetch_add((h1 - h0).count(), std::memory_order_relaxed);
        });

    // ---- record_event_rdma_pool: per-event RDMA using a pre-pinned receive buffer pool ----
    // Same call frequency as record_event_rdma but eliminates the per-event ibv_reg_mr cost:
    // the server acquire()s a slot from rdma_pool (pre-registered at startup) instead of
    // allocating a fresh buffer and calling engine.expose() on every handler invocation.
    engine.define("record_event_rdma_pool",
        [&state, &rdma_pool](tl::request const& req,
                             tl::bulk& client_bulk,
                             uint64_t  /*payload_size*/) mutable
        {
            auto h0 = std::chrono::steady_clock::now();

            BulkSlot& slot = rdma_pool.acquire();  // O(1), no allocation, no ibv_reg_mr

            auto r0 = std::chrono::steady_clock::now();
            client_bulk.on(req.get_endpoint()) >> slot.bulk;
            auto r1 = std::chrono::steady_clock::now();

            auto d0 = r1;
            const auto* h = reinterpret_cast<const EventPOD*>(slot.buf.data());
            LogEvent ev;
            ev.storyId    = h->storyId;
            ev.eventTime  = h->eventTime;
            ev.clientId   = h->clientId;
            ev.eventIndex = h->eventIndex;
            ev.logRecord.assign(slot.buf.data() + sizeof(EventPOD),
                                static_cast<size_t>(h->payloadLen));
            auto d1 = std::chrono::steady_clock::now();

            auto e0 = d1;
            state.queue.ingestEvent(ev);
            auto e1 = std::chrono::steady_clock::now();

            state.record_arrival();
            req.respond(state.events_received.load(std::memory_order_relaxed));
            auto h1 = std::chrono::steady_clock::now();

            state.total_rdma_ns.fetch_add   ((r1 - r0).count(), std::memory_order_relaxed);
            state.total_deser_ns.fetch_add  ((d1 - d0).count(), std::memory_order_relaxed);
            state.total_enqueue_ns.fetch_add((e1 - e0).count(), std::memory_order_relaxed);
            state.total_handler_ns.fetch_add((h1 - h0).count(), std::memory_order_relaxed);
        });

    // ---- record_events_bulk: legacy one-bulk-per-rep RDMA path (kept for reference) ----
    // Client exposes read_only bulk; server allocates a write_only buffer and pulls.
    // Buffer layout: [EventPOD | payload_bytes] × n_events, stride = 32 + payload_size
    engine.define("record_events_bulk",
        [&state, eng = engine](tl::request const& req,
                               tl::bulk&  client_bulk,
                               uint64_t   n_events,
                               uint64_t   payload_size) mutable
        {
            size_t stride = sizeof(EventPOD) + static_cast<size_t>(payload_size);
            size_t buf_sz = n_events * stride;
            std::vector<char> buf(buf_sz);
            std::vector<std::pair<void*, size_t>> segs = {{buf.data(), buf_sz}};
            tl::bulk server_buf = eng.expose(segs, tl::bulk_mode::write_only);
            // RDMA pull: server pulls data from client's exposed memory
            client_bulk.on(req.get_endpoint()) >> server_buf;

            // Deserialize events and route to IngestionQueue
            for(uint64_t i = 0; i < n_events; ++i)
            {
                const auto* h = reinterpret_cast<const EventPOD*>(buf.data() + i * stride);
                const char* payload_ptr = buf.data() + i * stride + sizeof(EventPOD);
                LogEvent ev;
                ev.storyId    = h->storyId;
                ev.eventTime  = h->eventTime;
                ev.clientId   = h->clientId;
                ev.eventIndex = h->eventIndex;
                ev.logRecord.assign(payload_ptr, h->payloadLen);
                state.queue.ingestEvent(ev);
            }
            state.record_arrival(n_events);
            req.respond(state.events_received.load(std::memory_order_relaxed));
        });

    // ---- wait_for: block until events_received >= target, then respond ----
    // Used by the client after a fire-and-forget send to synchronize before do_merge.
    // Sleeps the handler ULT (yields the ES) rather than busy-spinning.
    engine.define("wait_for",
        [&state, &engine](tl::request const& req, uint64_t target)
        {
            while(state.events_received.load(std::memory_order_relaxed) < target)
                tl::thread::sleep(engine, 1);  // suspend ULT for 1 ms, yield ES
            req.respond(state.events_received.load(std::memory_order_relaxed));
        });

    // ---- do_merge: drain IngestionQueue → per-story sorted maps ----
    // Called by the client after all producer threads have finished.
    engine.define("do_merge",
        [&state](tl::request const& req)
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            uint64_t total = 0;
            for(size_t s = 0; s < state.n_stories; ++s)
            {
                std::vector<LogEvent> drained;
                state.handles[s]->drainInto(drained);
                auto& chunk = state.chunks[s];
                for(auto& ev : drained)
                {
                    EventSequence key{ev.time(), ev.clientId, ev.index()};
                    chunk.emplace(std::move(key), std::move(ev));
                }
                total += static_cast<uint64_t>(chunk.size());
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            state.merge_ms      = std::chrono::duration<double, std::milli>(t1 - t0).count();
            state.merged_events = total;
            req.respond(total);
        });

    // ---- get_stats: return timing CSV for this rep ----
    // Format: received,ingest_ms,merged,merge_ms,n_threads,
    //         avg_handler_us,avg_enqueue_us,avg_rdma_us,avg_deser_us
    engine.define("get_stats",
        [&state](tl::request const& req)
        {
            uint64_t received = state.events_received.load(std::memory_order_relaxed);
            int64_t  fn = state.first_ns.load(std::memory_order_relaxed);
            int64_t  ln = state.last_ns.load(std::memory_order_relaxed);
            double ingest_ms = (fn < ln) ? static_cast<double>(ln - fn) / 1e6 : 0.0;

            double count = received > 0 ? static_cast<double>(received) : 1.0;
            double avg_handler_us = state.total_handler_ns.load(std::memory_order_relaxed) / count / 1e3;
            double avg_enqueue_us = state.total_enqueue_ns.load(std::memory_order_relaxed) / count / 1e3;
            double avg_rdma_us    = state.total_rdma_ns.load(std::memory_order_relaxed)    / count / 1e3;
            double avg_deser_us   = state.total_deser_ns.load(std::memory_order_relaxed)   / count / 1e3;

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(3)
                << received            << ","
                << ingest_ms           << ","
                << state.merged_events << ","
                << state.merge_ms      << ","
                << state.n_threads     << ","
                << avg_handler_us      << ","
                << avg_enqueue_us      << ","
                << avg_rdma_us         << ","
                << avg_deser_us;
            req.respond(oss.str());
        });

    engine.wait_for_finalize();
    return 0;
}
