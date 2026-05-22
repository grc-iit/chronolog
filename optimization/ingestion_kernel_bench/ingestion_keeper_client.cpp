/**
 * ingestion_keeper_client.cpp
 *
 * Thallium client for the network-aware ingestion kernel benchmark.
 * N producer threads push LogEvents to ingestion_keeper_server via either:
 *
 *   sendrecv  One RPC per event (record_event), mirroring production
 *             KeeperRecordingService::record_event calls.
 *
 *   rdma      One RDMA bulk transfer per thread per rep (record_events_bulk).
 *             The thread's entire event batch is packed into a contiguous
 *             buffer and transferred in a single RDMA pull operation.
 *
 * Usage:
 *   ingestion_keeper_client <server_addr>
 *       [--rpc   sendrecv|rdma]   RPC transport (default: sendrecv)
 *       [--queue mutex|lockfree]  queue label written into CSV — must match server (default: mutex)
 *       [--stories N]             story handle count — must match server (default: 4)
 *       [--count N]               total events to send per repetition (default: 1000000)
 *       [--size N]                payload bytes per event (default: 256)
 *       [--reps N]                repetitions to average (default: 3)
 *       [--header]                print CSV header and exit
 *
 * CSV output (one row, average of --reps repetitions):
 *   rpc,queue,threads,stories,total_events,ingest_ms,merge_ms,pipeline_ms,
 *   ingest_Mevs,pipeline_Mevs
 *
 * Where ingest_ms and merge_ms are server-side measurements from get_stats().
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include "concurrentqueue.h"  // for EventPOD definition shared with server

namespace tl = thallium;

// ---------------------------------------------------------------------------
// LogEvent — same self-contained replica used by the server.
// The serialize() template must match the server's definition exactly.
// ---------------------------------------------------------------------------
namespace chronolog
{
typedef uint64_t StoryId;
typedef uint64_t ClientId;

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

    template <typename A>
    void serialize(A& ar) { ar(storyId, eventTime, clientId, eventIndex, logRecord); }
};
} // namespace chronolog

using LogEvent = chronolog::LogEvent;

// ---------------------------------------------------------------------------
// RDMA wire layout — must match ingestion_keeper_server.cpp exactly.
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
// CLI options
// ---------------------------------------------------------------------------
enum class RpcMode { SENDRECV, ONEWAY, RDMA, RDMA_POOL, BATCH };

struct Options
{
    std::string server_addr;
    RpcMode     rpc_mode    = RpcMode::SENDRECV;
    std::string queue_label = "mutex"; // label only — must match server's --queue
    size_t      stories     = 4;
    size_t      count       = 1'000'000;
    size_t      size        = 256;
    size_t      reps        = 3;
    bool        header      = false;
    size_t      batch_size  = 1000;   // events per RPC in batch mode
    // Multi-client coordination modes (used by sweep.sh for parallel client runs)
    bool        reset_only    = false; // call reset_stats and exit
    bool        send_only     = false; // skip reset + finalize; just send events
    bool        finalize_only = false; // call do_merge + get_stats, print timing, exit
    size_t      n_clients     = 1;     // number of concurrent client nodes (CSV column)
};

static Options parse_args(int argc, char** argv)
{
    Options opts;
    if(argc < 2)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <server_addr> [--rpc sendrecv|rdma] [--queue mutex|lockfree]"
                     " [--stories N] [--count N] [--size N] [--reps N] [--header]\n";
        std::exit(1);
    }
    opts.server_addr = argv[1];
    for(int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if(a == "--header") { opts.header = true; continue; }
        auto need = [&]() -> std::string
        {
            if(++i >= argc) { std::cerr << a << " requires a value\n"; std::exit(1); }
            return argv[i];
        };
        if     (a == "--stories") opts.stories     = std::stoull(need());
        else if(a == "--count")   opts.count       = std::stoull(need());
        else if(a == "--size")    opts.size        = std::stoull(need());
        else if(a == "--reps")    opts.reps        = std::stoull(need());
        else if(a == "--queue")
        {
            std::string v = need();
            if(v == "mutex" || v == "lockfree") opts.queue_label = v;
            else { std::cerr << "Unknown queue mode: " << v << " (expected mutex|lockfree)\n"; std::exit(1); }
        }
        else if(a == "--rpc")
        {
            std::string v = need();
            if     (v == "sendrecv")  opts.rpc_mode = RpcMode::SENDRECV;
            else if(v == "oneway")    opts.rpc_mode = RpcMode::ONEWAY;
            else if(v == "rdma")      opts.rpc_mode = RpcMode::RDMA;
            else if(v == "rdma-pool") opts.rpc_mode = RpcMode::RDMA_POOL;
            else if(v == "batch")     opts.rpc_mode = RpcMode::BATCH;
            else { std::cerr << "Unknown RPC mode: " << v << "\n"; std::exit(1); }
        }
        else if(a == "--batch-size") opts.batch_size = std::stoull(need());
        else if(a == "--reset-only")    opts.reset_only    = true;
        else if(a == "--send-only")     opts.send_only     = true;
        else if(a == "--finalize-only") opts.finalize_only = true;
        else if(a == "--n-clients")     opts.n_clients     = std::stoull(need());
        else { std::cerr << "Unknown option: " << a << "\n"; std::exit(1); }
    }
    if(opts.stories == 0) opts.stories = 1;
    return opts;
}

// ---------------------------------------------------------------------------
// Pre-generate events for all threads.
// Thread t owns events with storyId assigned round-robin across stories.
// ---------------------------------------------------------------------------
static std::vector<std::vector<LogEvent>>
generate_events(size_t total, size_t payload_size, size_t n_threads, size_t n_stories)
{
    std::vector<std::vector<LogEvent>> per_thread(n_threads);
    size_t per = total / n_threads;
    std::string payload(payload_size, 'x');

    uint64_t base_time = 1700000000000000000ULL;
    for(size_t t = 0; t < n_threads; ++t)
    {
        per_thread[t].reserve(per);
        for(size_t i = 0; i < per; ++i)
        {
            uint64_t sid  = (t * per + i) % n_stories;
            uint64_t when = base_time + (t * per + i) * 1000 + (i % 97);
            uint32_t idx  = static_cast<uint32_t>(t * per + i);
            per_thread[t].emplace_back(sid, when, t, idx, payload);
        }
    }
    return per_thread;
}

// ---------------------------------------------------------------------------
// Pack thread events into an RDMA bulk buffer.
// Layout: [EventPOD (32B)][payload bytes] per event, dense, no padding.
// ---------------------------------------------------------------------------
static std::vector<char>
pack_rdma_buffer(std::vector<LogEvent> const& events, size_t payload_size)
{
    size_t stride = sizeof(EventPOD) + payload_size;
    std::vector<char> buf(events.size() * stride, '\0');
    for(size_t i = 0; i < events.size(); ++i)
    {
        auto& ev  = events[i];
        auto* hdr = reinterpret_cast<EventPOD*>(buf.data() + i * stride);
        hdr->storyId    = ev.storyId;
        hdr->eventTime  = ev.eventTime;
        hdr->clientId   = ev.clientId;
        hdr->eventIndex = ev.eventIndex;
        hdr->payloadLen = static_cast<uint32_t>(
            std::min(ev.logRecord.size(), payload_size));
        std::memcpy(buf.data() + i * stride + sizeof(EventPOD),
                    ev.logRecord.data(), hdr->payloadLen);
    }
    return buf;
}

// ---------------------------------------------------------------------------
// One benchmark rep: call reset_stats, run producers, call do_merge + get_stats.
// Returns (ingest_ms, merge_ms) from the server.
// ---------------------------------------------------------------------------
struct RepResult
{
    double ingest_ms      = 0.0;
    double merge_ms       = 0.0;
    size_t threads        = 0;
    double avg_rtt_us     = 0.0;   // client-measured per-event RTT
    double avg_handler_us = 0.0;   // server: total handler time
    double avg_enqueue_us = 0.0;   // server: queue enqueue (lock overhead)
    double avg_rdma_us    = 0.0;   // server: RDMA pull (RDMA path only)
    double avg_deser_us   = 0.0;   // server: POD→LogEvent deser (RDMA path only)
};

static RepResult run_rep(tl::engine&                           engine,
                         tl::endpoint const&                   server,
                         tl::remote_procedure&                 rpc_reset,
                         tl::remote_procedure&                 rpc_record,
                         tl::remote_procedure&                 rpc_oneway,
                         tl::remote_procedure&                 rpc_wait_for,
                         tl::remote_procedure&                 rpc_rdma_event,
                         tl::remote_procedure&                 rpc_rdma_pool,
                         tl::remote_procedure&                 rpc_bulk,
                         tl::remote_procedure&                 rpc_batch,
                         tl::remote_procedure&                 rpc_merge,
                         tl::remote_procedure&                 rpc_stats,
                         std::vector<std::vector<LogEvent>>&   per_thread,
                         Options const&                        opts,
                         bool                                  do_reset    = true,
                         bool                                  do_finalize = true)
{
    if(do_reset) rpc_reset.on(server)();

    size_t n_threads    = per_thread.size();
    size_t payload_size = opts.size;
    RpcMode rpc_mode    = opts.rpc_mode;

    RepResult res{};
    std::atomic<int64_t> total_rtt_ns{0};
    std::atomic<size_t>  rtt_samples{0};

    if(rpc_mode == RpcMode::SENDRECV)
    {
        // N pthreads each call record_event() once per event (blocking RPC)
        std::vector<std::thread> producers;
        producers.reserve(n_threads);
        for(size_t t = 0; t < n_threads; ++t)
        {
            producers.emplace_back([&, t]()
            {
                for(auto const& ev : per_thread[t])
                {
                    auto t0 = std::chrono::steady_clock::now();
                    rpc_record.on(server)(ev);
                    auto t1 = std::chrono::steady_clock::now();
                    total_rtt_ns.fetch_add((t1 - t0).count(), std::memory_order_relaxed);
                    rtt_samples.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for(auto& th : producers) th.join();
    }
    else if(rpc_mode == RpcMode::ONEWAY)
    {
        // Fire-and-forget: record_event_oneway has disable_response().
        // Each thread submits events without blocking per-event.
        // avg_rtt_us is repurposed as avg submission latency (time-to-submit / n_events).
        std::vector<std::thread> producers;
        producers.reserve(n_threads);
        auto submit_t0 = std::chrono::steady_clock::now();
        for(size_t t = 0; t < n_threads; ++t)
        {
            producers.emplace_back([&, t]()
            {
                for(auto const& ev : per_thread[t])
                    rpc_oneway.on(server)(ev);
            });
        }
        for(auto& th : producers) th.join();
        auto submit_t1 = std::chrono::steady_clock::now();

        size_t total_sent = 0;
        for(auto const& v : per_thread) total_sent += v.size();
        res.avg_rtt_us = total_sent > 0
            ? static_cast<double>((submit_t1 - submit_t0).count())
              / static_cast<double>(total_sent) / 1e3
            : 0.0;

        // Wait for server to receive all events before do_merge.
        if(do_finalize)
            rpc_wait_for.on(server)(static_cast<uint64_t>(total_sent));
    }
    else if(rpc_mode == RpcMode::BATCH)
    {
        // N pthreads each send their events in batches of opts.batch_size per RPC.
        // Reduces per-event RPC overhead by batch_size× while staying on the proven
        // ofi+verbs;ofi_rxm messaging path (no RDMA memory registration).
        size_t bsz = opts.batch_size;
        std::vector<std::thread> producers;
        producers.reserve(n_threads);
        for(size_t t = 0; t < n_threads; ++t)
        {
            producers.emplace_back([&, t, bsz]()
            {
                auto& events = per_thread[t];
                size_t total  = events.size();
                for(size_t off = 0; off < total; off += bsz)
                {
                    size_t end = std::min(off + bsz, total);
                    std::vector<LogEvent> batch(events.begin() + off,
                                                events.begin() + end);
                    rpc_batch.on(server)(batch);
                }
            });
        }
        for(auto& th : producers) th.join();
    }
    else if(rpc_mode == RpcMode::RDMA)
    {
        // Naive per-event RDMA: server allocates + ibv_reg_mr per call (baseline).
        // Client pre-pins a buffer once; per-event sub-bulks hit the client MR cache.
        size_t stride = sizeof(EventPOD) + payload_size;
        std::vector<std::thread> producers;
        producers.reserve(n_threads);
        for(size_t t = 0; t < n_threads; ++t)
        {
            producers.emplace_back([&, t, stride]()
            {
                auto& events = per_thread[t];
                if(events.empty()) return;

                std::vector<char> rdma_buf = pack_rdma_buffer(events, payload_size);

                std::vector<std::pair<void*, size_t>> all_segs =
                    {{rdma_buf.data(), rdma_buf.size()}};
                tl::bulk bulk_all = engine.expose(all_segs, tl::bulk_mode::read_only);

                for(size_t i = 0; i < events.size(); ++i)
                {
                    char* ev_ptr = rdma_buf.data() + i * stride;
                    std::vector<std::pair<void*, size_t>> ev_segs = {{ev_ptr, stride}};
                    auto t0 = std::chrono::steady_clock::now();
                    tl::bulk ev_bulk = engine.expose(ev_segs, tl::bulk_mode::read_only);
                    rpc_rdma_event.on(server)(ev_bulk, static_cast<uint64_t>(payload_size));
                    auto t1 = std::chrono::steady_clock::now();
                    total_rtt_ns.fetch_add((t1 - t0).count(), std::memory_order_relaxed);
                    rtt_samples.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for(auto& th : producers) th.join();
    }
    else if(rpc_mode == RpcMode::RDMA_POOL)
    {
        // Optimized per-event RDMA: server uses a pre-pinned buffer pool (no ibv_reg_mr
        // per call).  Client side is identical to naive RDMA — sub-bulk per event.
        size_t stride = sizeof(EventPOD) + payload_size;
        std::vector<std::thread> producers;
        producers.reserve(n_threads);
        for(size_t t = 0; t < n_threads; ++t)
        {
            producers.emplace_back([&, t, stride]()
            {
                auto& events = per_thread[t];
                if(events.empty()) return;

                std::vector<char> rdma_buf = pack_rdma_buffer(events, payload_size);

                std::vector<std::pair<void*, size_t>> all_segs =
                    {{rdma_buf.data(), rdma_buf.size()}};
                tl::bulk bulk_all = engine.expose(all_segs, tl::bulk_mode::read_only);

                for(size_t i = 0; i < events.size(); ++i)
                {
                    char* ev_ptr = rdma_buf.data() + i * stride;
                    std::vector<std::pair<void*, size_t>> ev_segs = {{ev_ptr, stride}};
                    auto t0 = std::chrono::steady_clock::now();
                    tl::bulk ev_bulk = engine.expose(ev_segs, tl::bulk_mode::read_only);
                    rpc_rdma_pool.on(server)(ev_bulk, static_cast<uint64_t>(payload_size));
                    auto t1 = std::chrono::steady_clock::now();
                    total_rtt_ns.fetch_add((t1 - t0).count(), std::memory_order_relaxed);
                    rtt_samples.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for(auto& th : producers) th.join();
    }

    // For modes other than ONEWAY, compute avg_rtt_us from the RTT atomics.
    // ONEWAY already set res.avg_rtt_us to avg submission latency above.
    if(rpc_mode != RpcMode::ONEWAY)
    {
        size_t samples = rtt_samples.load(std::memory_order_relaxed);
        res.avg_rtt_us = samples > 0
            ? static_cast<double>(total_rtt_ns.load(std::memory_order_relaxed))
              / static_cast<double>(samples) / 1e3
            : 0.0;
    }

    if(do_finalize)
    {
        // Trigger server-side merge and retrieve stats
        rpc_merge.on(server)();
        std::string stats_csv = rpc_stats.on(server)();

        // Parse "received,ingest_ms,merged,merge_ms,n_threads,
        //         avg_handler_us,avg_enqueue_us,avg_rdma_us,avg_deser_us"
        std::istringstream ss(stats_csv);
        std::string tok;
        std::getline(ss, tok, ','); // received (ignored; client knows count)
        std::getline(ss, tok, ','); res.ingest_ms = std::stod(tok);
        std::getline(ss, tok, ','); // merged_events (ignored)
        std::getline(ss, tok, ','); res.merge_ms  = std::stod(tok);
        std::getline(ss, tok, ','); res.threads   = std::stoull(tok);
        if(std::getline(ss, tok, ',')) res.avg_handler_us = std::stod(tok);
        if(std::getline(ss, tok, ',')) res.avg_enqueue_us = std::stod(tok);
        if(std::getline(ss, tok, ',')) res.avg_rdma_us    = std::stod(tok);
        if(std::getline(ss, tok, ',')) res.avg_deser_us   = std::stod(tok);
    }
    return res;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    Options opts = parse_args(argc, argv);

    if(opts.header)
    {
        std::cout << "rpc,queue,threads,n_clients,stories,total_events,"
                     "ingest_ms,merge_ms,pipeline_ms,ingest_Mevs,pipeline_Mevs\n";
        return 0;
    }

    // Derive protocol from address string prefix (e.g. "ofi+verbs;ofi_rxm" from "ofi+verbs;ofi_rxm://…")
    std::string protocol = opts.server_addr.substr(0, opts.server_addr.find(':'));
    // use_progress_thread=true: dedicated thread drives HG_Progress so blocking
    // RPC calls from the main ULT can complete without deadlocking.
    tl::engine engine(protocol, THALLIUM_CLIENT_MODE, /*use_progress_thread=*/true, /*rpc_threads=*/0);

    // tl::endpoint and tl::remote_procedure hold Mercury resources that must be
    // freed while the engine is still alive.  Wrapping them in a lambda ensures
    // they are destroyed before engine.finalize() is called below.
    std::string output_row = [&]() -> std::string
    {
        tl::endpoint server = engine.lookup(opts.server_addr);

        tl::remote_procedure rpc_reset      = engine.define("reset_stats");
        tl::remote_procedure rpc_record     = engine.define("record_event");
        tl::remote_procedure rpc_oneway     = engine.define("record_event_oneway").disable_response();
        tl::remote_procedure rpc_wait_for   = engine.define("wait_for");
        tl::remote_procedure rpc_rdma_event = engine.define("record_event_rdma");
        tl::remote_procedure rpc_rdma_pool  = engine.define("record_event_rdma_pool");
        tl::remote_procedure rpc_bulk       = engine.define("record_events_bulk");
        tl::remote_procedure rpc_batch      = engine.define("record_events_batch");
        tl::remote_procedure rpc_merge      = engine.define("do_merge");
        tl::remote_procedure rpc_stats      = engine.define("get_stats");

        // ---- Multi-client coordination modes ----
        // Used by sweep.sh to implement reset → parallel-send → finalize protocol.

        if(opts.reset_only)
        {
            rpc_reset.on(server)();
            return "";  // endpoint freed at lambda exit, before engine.finalize()
        }

        if(opts.finalize_only)
        {
            // For oneway RPCs the server may still be processing events when the
            // send-only workers have already exited.  Wait until all events arrive
            // before draining the queues.
            if(opts.rpc_mode == RpcMode::ONEWAY)
            {
                uint64_t target = static_cast<uint64_t>(opts.n_clients)
                                * static_cast<uint64_t>(opts.count);
                rpc_wait_for.on(server)(target);
            }

            // Drain ingestion queues and retrieve timing.
            // Output "ingest_ms,merge_ms,n_threads" for the sweep script to parse (stdout).
            // Print server-side timing breakdown to stderr.
            rpc_merge.on(server)();
            std::string stats_csv = rpc_stats.on(server)();
            std::istringstream ss(stats_csv);
            std::string tok;
            std::getline(ss, tok, ',');                                // received (skip)
            std::getline(ss, tok, ','); double fin_ingest_ms  = std::stod(tok);
            std::getline(ss, tok, ',');                                // merged (skip)
            std::getline(ss, tok, ','); double fin_merge_ms   = std::stod(tok);
            std::getline(ss, tok, ','); size_t fin_threads    = std::stoull(tok);
            double fin_handler_us = 0.0, fin_enqueue_us = 0.0;
            double fin_rdma_us = 0.0, fin_deser_us = 0.0;
            if(std::getline(ss, tok, ',')) fin_handler_us = std::stod(tok);
            if(std::getline(ss, tok, ',')) fin_enqueue_us = std::stod(tok);
            if(std::getline(ss, tok, ',')) fin_rdma_us    = std::stod(tok);
            if(std::getline(ss, tok, ',')) fin_deser_us   = std::stod(tok);

            const char* rl = (opts.rpc_mode == RpcMode::ONEWAY)    ? "oneway"
                           : (opts.rpc_mode == RpcMode::RDMA)      ? "rdma"
                           : (opts.rpc_mode == RpcMode::RDMA_POOL) ? "rdma-pool"
                           : (opts.rpc_mode == RpcMode::BATCH)     ? "batch"
                           :                                          "sendrecv";
            std::cerr << "\n=== Server Timing Breakdown (" << rl << ", multi-client) ===\n"
                      << std::fixed << std::setprecision(2)
                      << "  Server handler (avg/event): " << fin_handler_us << " us\n"
                      << "    enqueue (lock/unlock):    " << fin_enqueue_us << " us\n";
            if(opts.rpc_mode == RpcMode::RDMA || opts.rpc_mode == RpcMode::RDMA_POOL)
                std::cerr << "    RDMA pull:                " << fin_rdma_us  << " us\n"
                          << "    POD->LogEvent deser:      " << fin_deser_us << " us\n";

            std::ostringstream out;
            out << std::fixed << std::setprecision(3)
                << fin_ingest_ms << "," << fin_merge_ms << "," << fin_threads << "\n";
            return out.str();
        }

        // ---- Normal send mode (single client or --send-only worker) ----
        size_t actual_total = opts.count;
        auto per_thread = generate_events(opts.count, opts.size, /*n_threads=*/1, opts.stories);

        double sum_ingest_ms  = 0.0, sum_merge_ms   = 0.0;
        double sum_rtt_us     = 0.0, sum_handler_us = 0.0;
        double sum_enqueue_us = 0.0, sum_rdma_us    = 0.0, sum_deser_us = 0.0;
        size_t server_threads = 0;
        for(size_t r = 0; r < opts.reps; ++r)
        {
            RepResult res = run_rep(engine, server,
                                    rpc_reset, rpc_record, rpc_oneway, rpc_wait_for,
                                    rpc_rdma_event, rpc_rdma_pool,
                                    rpc_bulk, rpc_batch, rpc_merge, rpc_stats,
                                    per_thread, opts,
                                    /*do_reset=*/   !opts.send_only,
                                    /*do_finalize=*/!opts.send_only);
            if(!opts.send_only)
            {
                sum_ingest_ms  += res.ingest_ms;
                sum_merge_ms   += res.merge_ms;
                server_threads  = res.threads;
                sum_rtt_us     += res.avg_rtt_us;
                sum_handler_us += res.avg_handler_us;
                sum_enqueue_us += res.avg_enqueue_us;
                sum_rdma_us    += res.avg_rdma_us;
                sum_deser_us   += res.avg_deser_us;
            }
        }

        if(opts.send_only) return "";  // worker node: no CSV output

        double reps_d      = static_cast<double>(opts.reps);
        double ingest_ms   = sum_ingest_ms / reps_d;
        double merge_ms    = sum_merge_ms  / reps_d;
        double pipeline_ms = ingest_ms + merge_ms;
        double n           = static_cast<double>(actual_total);
        double ingest_mevs   = (ingest_ms   > 0.0) ? n / ingest_ms   / 1000.0 : 0.0;
        double pipeline_mevs = (pipeline_ms > 0.0) ? n / pipeline_ms / 1000.0 : 0.0;
        const char* rpc_label = (opts.rpc_mode == RpcMode::ONEWAY)    ? "oneway"
                              : (opts.rpc_mode == RpcMode::RDMA)      ? "rdma"
                              : (opts.rpc_mode == RpcMode::RDMA_POOL) ? "rdma-pool"
                              : (opts.rpc_mode == RpcMode::BATCH)     ? "batch"
                              :                                          "sendrecv";

        double avg_rtt_us     = sum_rtt_us     / reps_d;
        double avg_handler_us = sum_handler_us / reps_d;
        double avg_enqueue_us = sum_enqueue_us / reps_d;
        double avg_rdma_us    = sum_rdma_us    / reps_d;
        double avg_deser_us   = sum_deser_us   / reps_d;

        std::cerr << "\n=== Timing Breakdown (" << rpc_label << ", " << opts.size
                  << "B payload, " << actual_total << " events) ===\n"
                  << std::fixed << std::setprecision(2);
        if(opts.rpc_mode == RpcMode::ONEWAY)
        {
            std::cerr << "  Client submit (avg/event):  " << avg_rtt_us     << " us\n"
                      << "  Server handler (avg/event): " << avg_handler_us << " us\n"
                      << "    enqueue (no respond):     " << avg_enqueue_us << " us\n";
        }
        else
        {
            double avg_net_us = avg_rtt_us - avg_handler_us;
            std::cerr << "  Client RTT (avg/event):     " << avg_rtt_us     << " us\n"
                      << "  Server handler (avg/event): " << avg_handler_us << " us\n"
                      << "    enqueue (lock/unlock):    " << avg_enqueue_us << " us\n";
            if(opts.rpc_mode == RpcMode::RDMA || opts.rpc_mode == RpcMode::RDMA_POOL)
            {
                std::cerr << "    RDMA pull:                " << avg_rdma_us   << " us\n"
                          << "    POD->LogEvent deser:      " << avg_deser_us  << " us\n";
                double rest = avg_handler_us - avg_rdma_us - avg_deser_us - avg_enqueue_us;
                if(opts.rpc_mode == RpcMode::RDMA)
                    std::cerr << "    buf alloc + MR expose:    " << rest << " us\n";
                else
                    std::cerr << "    pool acquire + respond:   " << rest << " us\n";
            }
            std::cerr << "  Network overhead (derived): " << avg_net_us << " us\n"
                      << "    (RTT - handler; ~= request + response transit)\n";
        }

        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << rpc_label        << "," << opts.queue_label << "," << server_threads << ","
            << opts.n_clients   << "," << opts.stories     << "," << actual_total   << ","
            << ingest_ms        << "," << merge_ms         << "," << pipeline_ms    << ","
            << std::setprecision(3)
            << ingest_mevs      << "," << pipeline_mevs    << "\n";
        return out.str();

    }();  // server + RPCs destroyed here, before engine.finalize()

    engine.finalize();
    std::cout << output_row;
    return 0;
}
