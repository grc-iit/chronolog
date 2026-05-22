/**
 * ingestion_kernel_bench.cpp
 *
 * Mini benchmark that isolates ChronoKeeper's ingestion kernel:
 *   1. N producer threads push LogEvents into per-story handles via IngestionQueue
 *   2. One consumer thread drains every handle into std::map<EventSequence, LogEvent>
 *      (the StoryChunk merge step)
 *
 * Knobs:
 *   --threads N           ingestion (producer) thread count
 *   --queue mutex|stl|lockfree|moodycamel
 *                         queue implementation:
 *                           mutex/stl  → std::deque + std::mutex (production today)
 *                           lockfree/moodycamel → moodycamel::ConcurrentQueue
 *   --stories N           number of stories (per-handle fan-out)
 *   --count N             total events generated across all threads
 *   --size N              payload bytes per event
 *   --reps N              repetitions averaged per data point
 *   --header              print CSV header only (then exit)
 *
 * One CSV row per run is written to stdout:
 *   queue,threads,stories,total_events,ingest_ms,merge_ms,pipeline_ms,ingest_Mevs,pipeline_Mevs
 *
 * Merged from ChronoLog/optimization/ingestion_threading + lock-free_deque, but
 * dropped the Argobots/thallium dependency so the kernel can be compiled with
 * just g++ + pthreads.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "concurrentqueue.h"

// ---------------------------------------------------------------------------
// Production type replicas
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
    StoryId  storyId    = 0;
    uint64_t eventTime  = 0;
    ClientId clientId   = 0;
    uint32_t eventIndex = 0;
    std::string logRecord;

    LogEvent() = default;
    LogEvent(StoryId sid, uint64_t t, ClientId cid, uint32_t idx, std::string rec)
        : storyId(sid), eventTime(t), clientId(cid), eventIndex(idx), logRecord(std::move(rec))
    {}

    uint64_t time()  const { return eventTime;  }
    uint32_t index() const { return eventIndex; }
};

typedef std::deque<LogEvent> EventDeque;
} // namespace chronolog

using EventSequence = chronolog::EventSequence;
using LogEvent      = chronolog::LogEvent;
using EventDeque    = chronolog::EventDeque;

// ---------------------------------------------------------------------------
// Per-story ingestion handle interface
// ---------------------------------------------------------------------------
class StoryIngestionHandleBase
{
public:
    virtual ~StoryIngestionHandleBase() = default;
    virtual void ingestEvent(LogEvent const& ev) = 0;  // match production: copy, not move
    virtual void drainInto(std::vector<LogEvent>& out) = 0;
};

// std::deque + std::mutex — mirrors current production StoryIngestionHandle.
class MutexIngestionHandle : public StoryIngestionHandleBase
{
public:
    MutexIngestionHandle()
        : activeDeque(&deque1), passiveDeque(&deque2) {}

    void ingestEvent(LogEvent const& ev) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        activeDeque->push_back(ev);  // copy, matches production StoryIngestionHandle::ingestEvent
    }

    void drainInto(std::vector<LogEvent>& out) override
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            std::swap(activeDeque, passiveDeque);
        }
        while (!passiveDeque->empty())
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

// moodycamel::ConcurrentQueue — lock-free MPMC, drained in 256-event bulks.
class LockFreeIngestionHandle : public StoryIngestionHandleBase
{
public:
    explicit LockFreeIngestionHandle(std::size_t capacity_hint = 4096)
        : queue(capacity_hint) {}

    void ingestEvent(LogEvent const& ev) override
    {
        queue.enqueue(ev);  // copy for fair comparison with mutex path
    }

    void drainInto(std::vector<LogEvent>& out) override
    {
        static constexpr std::size_t BULK = 256;
        LogEvent batch[BULK];
        std::size_t got;
        while ((got = queue.try_dequeue_bulk(batch, BULK)) > 0)
        {
            for (std::size_t i = 0; i < got; ++i)
                out.push_back(std::move(batch[i]));
        }
    }

private:
    moodycamel::ConcurrentQueue<LogEvent> queue;
};

// ---------------------------------------------------------------------------
// Routes events to per-story handles by storyId — same logic as production
// IngestionQueue (handles registered up front, lookups are lock-free).
// ---------------------------------------------------------------------------
class IngestionQueue
{
public:
    void registerHandle(uint64_t storyId, StoryIngestionHandleBase* handle)
    {
        handles[storyId] = handle;
    }

    void ingestEvent(LogEvent const& ev)
    {
        auto it = handles.find(ev.storyId);
        if (it != handles.end())
            it->second->ingestEvent(ev);
    }

private:
    std::unordered_map<uint64_t, StoryIngestionHandleBase*> handles;
};

// ---------------------------------------------------------------------------
// CLI options
// ---------------------------------------------------------------------------
enum class QueueMode { MUTEX, LOCKFREE };

struct Options
{
    std::size_t threads = 4;
    std::size_t stories = 4;
    std::size_t count   = 1'000'000;
    std::size_t size    = 256;
    std::size_t reps    = 3;
    QueueMode   mode    = QueueMode::MUTEX;
    bool        header  = false;
};

static const char* mode_str(QueueMode m)
{
    return (m == QueueMode::LOCKFREE) ? "lockfree" : "mutex";
}

static Options parse_args(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            std::cerr << "Usage: " << argv[0]
                << " [--threads N] [--stories N] [--count N] [--size N] [--reps N]"
                << " [--queue mutex|stl|lockfree|moodycamel] [--header]\n";
            std::exit(0);
        }
        if (arg == "--header") { opts.header = true; continue; }

        auto need_next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: " << arg << " requires a value\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if      (arg == "--threads") opts.threads = std::stoull(need_next());
        else if (arg == "--stories") opts.stories = std::stoull(need_next());
        else if (arg == "--count")   opts.count   = std::stoull(need_next());
        else if (arg == "--size")    opts.size    = std::stoull(need_next());
        else if (arg == "--reps")    opts.reps    = std::stoull(need_next());
        else if (arg == "--queue")
        {
            std::string val = need_next();
            if      (val == "mutex" || val == "stl")
                opts.mode = QueueMode::MUTEX;
            else if (val == "lockfree" || val == "moodycamel")
                opts.mode = QueueMode::LOCKFREE;
            else {
                std::cerr << "ERROR: --queue must be mutex|stl|lockfree|moodycamel\n";
                std::exit(1);
            }
        }
        else
        {
            std::cerr << "ERROR: unknown option '" << arg << "'\n";
            std::exit(1);
        }
    }
    if (opts.threads == 0) opts.threads = 1;
    if (opts.stories == 0) opts.stories = 1;
    return opts;
}

// ---------------------------------------------------------------------------
// Generate per-thread event vectors. Each thread t writes round-robin across
// stories so contention is realistic (multiple producers per handle).
// ---------------------------------------------------------------------------
static std::vector<std::vector<LogEvent>>
generate_per_thread_events(std::size_t total_events, std::size_t payload_size,
                           std::size_t num_threads, std::size_t num_stories)
{
    std::vector<std::vector<LogEvent>> per_thread(num_threads);
    std::size_t per = total_events / num_threads;
    std::string payload(payload_size, 'x');

    uint64_t base_time = 1700000000000000000ULL;
    for (std::size_t t = 0; t < num_threads; ++t)
    {
        per_thread[t].reserve(per);
        for (std::size_t i = 0; i < per; ++i)
        {
            uint64_t sid  = (t * per + i) % num_stories;
            uint64_t when = base_time + (t * per + i) * 1000 + (i % 97);
            uint32_t idx  = static_cast<uint32_t>(t * per + i);
            per_thread[t].emplace_back(sid, when, t, idx, payload);
        }
    }
    return per_thread;
}

struct BenchResult
{
    double ingest_ms   = 0.0;
    double merge_ms    = 0.0;
    double pipeline_ms = 0.0;
};

static BenchResult run_once(std::vector<std::vector<LogEvent>>& per_thread,
                            std::size_t num_stories, QueueMode mode)
{
    std::vector<std::unique_ptr<StoryIngestionHandleBase>> handles;
    handles.reserve(num_stories);
    for (std::size_t s = 0; s < num_stories; ++s)
    {
        if (mode == QueueMode::LOCKFREE)
            handles.push_back(std::make_unique<LockFreeIngestionHandle>());
        else
            handles.push_back(std::make_unique<MutexIngestionHandle>());
    }

    IngestionQueue queue;
    for (std::size_t s = 0; s < num_stories; ++s)
        queue.registerHandle(s, handles[s].get());

    BenchResult result;
    auto pipeline_start = std::chrono::high_resolution_clock::now();

    // --- Ingest phase: N producer threads each drain their event vector ---
    {
        std::vector<std::thread> producers;
        producers.reserve(per_thread.size());
        for (std::size_t t = 0; t < per_thread.size(); ++t)
        {
            producers.emplace_back([&queue, &per_thread, t]() {
                for (auto const& ev : per_thread[t])
                    queue.ingestEvent(ev);
            });
        }
        for (auto& th : producers) th.join();
    }

    auto ingest_end = std::chrono::high_resolution_clock::now();
    result.ingest_ms = std::chrono::duration<double, std::milli>(
        ingest_end - pipeline_start).count();

    // --- Merge phase: drain every handle into a per-story std::map ---
    std::vector<std::map<EventSequence, LogEvent>> chunks(num_stories);
    for (std::size_t s = 0; s < num_stories; ++s)
    {
        std::vector<LogEvent> drained;
        handles[s]->drainInto(drained);
        auto& chunk = chunks[s];
        for (auto& ev : drained)
        {
            EventSequence key{ev.time(), ev.clientId, ev.index()};
            chunk.emplace(std::move(key), std::move(ev));
        }
    }

    auto merge_end = std::chrono::high_resolution_clock::now();
    result.merge_ms = std::chrono::duration<double, std::milli>(
        merge_end - ingest_end).count();
    result.pipeline_ms = std::chrono::duration<double, std::milli>(
        merge_end - pipeline_start).count();
    return result;
}

int main(int argc, char** argv)
{
    Options opts = parse_args(argc, argv);

    if (opts.header)
    {
        std::cout << "queue,threads,stories,total_events,"
                     "ingest_ms,merge_ms,pipeline_ms,"
                     "ingest_Mevs,pipeline_Mevs\n";
        return 0;
    }

    std::size_t per          = opts.count / opts.threads;
    std::size_t actual_total = per * opts.threads;

    BenchResult avg{};
    for (std::size_t r = 0; r < opts.reps; ++r)
    {
        auto per_thread = generate_per_thread_events(
            opts.count, opts.size, opts.threads, opts.stories);
        auto res = run_once(per_thread, opts.stories, opts.mode);
        avg.ingest_ms   += res.ingest_ms;
        avg.merge_ms    += res.merge_ms;
        avg.pipeline_ms += res.pipeline_ms;
    }
    double reps = static_cast<double>(opts.reps);
    avg.ingest_ms   /= reps;
    avg.merge_ms    /= reps;
    avg.pipeline_ms /= reps;

    double n = static_cast<double>(actual_total);
    double ingest_mevs   = n / avg.ingest_ms   / 1000.0;
    double pipeline_mevs = n / avg.pipeline_ms / 1000.0;

    std::cout << std::fixed << std::setprecision(2)
              << mode_str(opts.mode) << ","
              << opts.threads << ","
              << opts.stories << ","
              << actual_total << ","
              << avg.ingest_ms << ","
              << avg.merge_ms << ","
              << avg.pipeline_ms << ","
              << std::setprecision(3)
              << ingest_mevs << ","
              << pipeline_mevs << "\n";
    return 0;
}
