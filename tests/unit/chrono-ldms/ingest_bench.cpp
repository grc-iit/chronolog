/*
 * ingest_bench.cpp -- microbenchmark for the "sorted-at-ingest" tail-read design
 * question: how much slower is keeper event ingestion if the per-story pending
 * buffer maintains sorted order (to serve live tail reads) instead of the
 * current unsorted deque?
 *
 * Replicates the keeper ingestion shapes:
 *   current:   mutex + std::deque::push_back            (unsorted, O(1))
 *   sorted:    mutex + std::multimap::insert            (naive sorted)
 *   sortedH:   mutex + std::multimap::insert w/ hint    (near-sorted optimized)
 *   livetail:  sortedH + bounded capacity (erase oldest past 65536)
 *
 * And the downstream merge (background tick) both ways:
 *   deque -> std::map (current mergeEvents)  vs  sorted multimap -> map w/ hint
 *
 * Input patterns: sorted (1 client), near-sorted (8 clients, per-client
 * monotonic, round-robin interleave w/ jitter), random (adversarial).
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using Seq = std::tuple<uint64_t, uint64_t, uint32_t>; // (time, clientId, index)

struct Ev
{
    uint64_t storyId, time, clientId;
    uint32_t index;
    std::string record;
    Seq seq() const { return Seq{time, clientId, index}; }
};

static double now_sec()
{
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// Build N events in the given arrival pattern.
static std::vector<Ev> make_events(size_t n, size_t payload, const std::string& pattern)
{
    std::vector<Ev> evs;
    evs.reserve(n);
    std::string rec(payload, 'x');
    std::mt19937_64 rng(42);
    if(pattern == "sorted")
    {
        for(size_t i = 0; i < n; i++) evs.push_back({7, 1000000000ull + i * 1000, 1, (uint32_t)i, rec});
    }
    else if(pattern == "nearsorted")
    {
        // 8 clients, per-client monotonic clocks with up to ±20us skew,
        // events interleaved round-robin (realistic keeper arrival).
        const int C = 8;
        uint64_t clk[C];
        std::uniform_int_distribution<int64_t> skew(-20000, 20000);
        for(int c = 0; c < C; c++) clk[c] = 1000000000ull + skew(rng);
        std::uniform_int_distribution<uint64_t> step(500, 1500);
        uint32_t idx[C] = {0};
        for(size_t i = 0; i < n; i++)
        {
            int c = i % C;
            clk[c] += step(rng);
            evs.push_back({7, clk[c], (uint64_t)(c + 1), idx[c]++, rec});
        }
    }
    else // random
    {
        for(size_t i = 0; i < n; i++) evs.push_back({7, 1000000000ull + i * 1000, 1, (uint32_t)i, rec});
        std::shuffle(evs.begin(), evs.end(), rng);
    }
    return evs;
}

int main(int argc, char** argv)
{
    size_t N = (argc > 1) ? strtoull(argv[1], 0, 10) : 200000;
    size_t PAYLOAD = (argc > 2) ? strtoull(argv[2], 0, 10) : 1024;
    const size_t TAIL_CAP = 65536;

    printf("events=%zu payload=%zuB tail_cap=%zu\n", N, PAYLOAD, TAIL_CAP);
    printf("%-12s %-10s %12s %12s %12s %14s\n",
           "pattern",
           "structure",
           "ingest ns/ev",
           "merge ns/ev",
           "total ns/ev",
           "vs deque");

    for(std::string pattern: {"sorted", "nearsorted", "random"})
    {
        auto evs = make_events(N, PAYLOAD, pattern);
        double base_total = 0;

        // ---- current: mutex + deque push, then merge deque -> map ----
        {
            std::mutex mx;
            std::deque<Ev> dq;
            double t0 = now_sec();
            for(auto const& e: evs)
            {
                std::lock_guard<std::mutex> g(mx);
                dq.push_back(e);
            }
            double t1 = now_sec();
            std::map<Seq, Ev> chunk;
            double t2 = now_sec();
            while(!dq.empty())
            {
                chunk.emplace(dq.front().seq(), dq.front());
                dq.pop_front();
            }
            double t3 = now_sec();
            double ing = (t1 - t0) / N * 1e9, mrg = (t3 - t2) / N * 1e9;
            base_total = ing + mrg;
            printf("%-12s %-10s %12.0f %12.0f %12.0f %14s\n",
                   pattern.c_str(),
                   "deque",
                   ing,
                   mrg,
                   ing + mrg,
                   "1.00x (base)");
            if(chunk.size() != N)
                printf("!! chunk size mismatch\n");
        }
        // ---- sorted-at-ingest: mutex + multimap insert (naive) ----
        {
            std::mutex mx;
            std::multimap<Seq, Ev> buf;
            double t0 = now_sec();
            for(auto const& e: evs)
            {
                std::lock_guard<std::mutex> g(mx);
                buf.emplace(e.seq(), e);
            }
            double t1 = now_sec();
            std::map<Seq, Ev> chunk;
            double t2 = now_sec();
            // already sorted: hint-insert at end is amortized O(1)
            for(auto& kv: buf) chunk.emplace_hint(chunk.end(), kv.first, std::move(kv.second));
            buf.clear();
            double t3 = now_sec();
            double ing = (t1 - t0) / N * 1e9, mrg = (t3 - t2) / N * 1e9;
            printf("%-12s %-10s %12.0f %12.0f %12.0f %13.2fx\n",
                   pattern.c_str(),
                   "mmap",
                   ing,
                   mrg,
                   ing + mrg,
                   (ing + mrg) / base_total);
        }
        // ---- sorted-at-ingest with end-hint (near-sorted optimization) ----
        {
            std::mutex mx;
            std::multimap<Seq, Ev> buf;
            double t0 = now_sec();
            for(auto const& e: evs)
            {
                std::lock_guard<std::mutex> g(mx);
                buf.emplace_hint(buf.end(), e.seq(), e); // hint: append-mostly
            }
            double t1 = now_sec();
            std::map<Seq, Ev> chunk;
            double t2 = now_sec();
            for(auto& kv: buf) chunk.emplace_hint(chunk.end(), kv.first, std::move(kv.second));
            buf.clear();
            double t3 = now_sec();
            double ing = (t1 - t0) / N * 1e9, mrg = (t3 - t2) / N * 1e9;
            printf("%-12s %-10s %12.0f %12.0f %12.0f %13.2fx\n",
                   pattern.c_str(),
                   "mmap-hint",
                   ing,
                   mrg,
                   ing + mrg,
                   (ing + mrg) / base_total);
        }
        // ---- bounded live-tail alongside deque (hybrid option C) ----
        // deque (authoritative, unchanged) + capped sorted index of last-N.
        {
            std::mutex mx;
            std::deque<Ev> dq;
            std::multimap<Seq, const Ev*> tail; // index only, payload stays in deque
            double t0 = now_sec();
            for(auto const& e: evs)
            {
                std::lock_guard<std::mutex> g(mx);
                dq.push_back(e);
                tail.emplace_hint(tail.end(), e.seq(), &dq.back());
                if(tail.size() > TAIL_CAP)
                    tail.erase(tail.begin());
            }
            double t1 = now_sec();
            std::map<Seq, Ev> chunk;
            double t2 = now_sec();
            while(!dq.empty())
            {
                chunk.emplace(dq.front().seq(), dq.front());
                dq.pop_front();
            }
            double t3 = now_sec();
            double ing = (t1 - t0) / N * 1e9, mrg = (t3 - t2) / N * 1e9;
            printf("%-12s %-10s %12.0f %12.0f %12.0f %13.2fx\n",
                   pattern.c_str(),
                   "dq+tail",
                   ing,
                   mrg,
                   ing + mrg,
                   (ing + mrg) / base_total);
        }
        printf("\n");
    }
    return 0;
}
