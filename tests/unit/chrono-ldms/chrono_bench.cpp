/*
 * chrono_bench.cpp -- ChronoLog producer benchmark using the ChronoLog C++
 * client (the same client the LDMS store_chronolog plugin uses).
 *
 * T threads each write to their own Story (mirroring ChronoLog's intended
 * many-writers model). StoryHandle::log_event() is a synchronous keeper RPC,
 * so the measured loop time already reflects keeper-acknowledged events --
 * directly comparable to Kafka's flush-acked throughput.
 *
 * Usage: chrono_bench <client_conf> <chronicle> <payload_size> <total_events> <threads>
 */
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>

#include "bench_common.h"

int main(int argc, char** argv)
{
    if(argc < 6)
    {
        fprintf(stderr,
                "usage: %s <client_conf> <chronicle> "
                "<payload_size> <total_events> <threads>\n",
                argv[0]);
        return 2;
    }
    std::string conf_path = argv[1];
    std::string chronicle = argv[2];
    size_t payload_sz = strtoul(argv[3], nullptr, 10);
    uint64_t total = strtoull(argv[4], nullptr, 10);
    int threads = atoi(argv[5]);

    std::string payload = make_payload(payload_sz);
    size_t plen = payload.size();

    chronolog::ClientConfiguration cm;
    cm.load_from_file(conf_path);
    chronolog::chrono_monitor::initialize(cm.LOG_CONF.LOGTYPE,
                                          "chrono_bench.log",
                                          cm.LOG_CONF.LOGLEVEL,
                                          cm.LOG_CONF.LOGNAME,
                                          cm.LOG_CONF.LOGFILESIZE,
                                          cm.LOG_CONF.LOGFILENUM,
                                          cm.LOG_CONF.FLUSHLEVEL);

    chronolog::ClientPortalServiceConf portal;
    portal.PROTO_CONF = cm.PORTAL_CONF.PROTO_CONF;
    portal.IP = cm.PORTAL_CONF.IP;
    portal.PORT = cm.PORTAL_CONF.PORT;
    portal.PROVIDER_ID = cm.PORTAL_CONF.PROVIDER_ID;

    chronolog::Client client(portal);
    if(client.Connect() != chronolog::CL_SUCCESS)
    {
        fprintf(stderr, "Connect failed\n");
        return 1;
    }
    client.CreateChronicle(chronicle);

    // Pre-acquire one story per thread (setup, not measured).
    std::vector<chronolog::StoryHandle*> handles(threads, nullptr);
    std::vector<std::string> story_names(threads);
    for(int t = 0; t < threads; t++)
    {
        story_names[t] = "s" + std::to_string(t) + "_" + std::to_string(plen) + "_" + std::to_string(getpid());
        auto acq = client.AcquireStory(chronicle, story_names[t]);
        if(acq.first != chronolog::CL_SUCCESS || !acq.second)
        {
            fprintf(stderr, "AcquireStory %s failed rc=%d\n", story_names[t].c_str(), acq.first);
            return 1;
        }
        handles[t] = acq.second;
    }

    uint64_t per = total / threads;
    std::atomic<uint64_t> errors{0};

    double t0 = now_sec();
    std::vector<std::thread> pool;
    for(int t = 0; t < threads; t++)
    {
        pool.emplace_back(
                [&, t]()
                {
                    chronolog::StoryHandle* h = handles[t];
                    uint64_t local_err = 0;
                    for(uint64_t i = 0; i < per; i++)
                    {
                        uint64_t rc = h->log_event(payload);
                        if(rc == 0)
                            local_err++;
                    }
                    if(local_err)
                        errors.fetch_add(local_err, std::memory_order_relaxed);
                });
    }
    for(auto& th: pool) th.join();
    double t1 = now_sec();

    uint64_t n = per * threads;
    double acked_eps = n / (t1 - t0);
    double mbps = acked_eps * plen / (1024 * 1024);

    // CSV mirrors kafka_bench columns; for synchronous ChronoLog the enqueue
    // and acked rates are identical (each log_event waits for keeper ack).
    printf("chronolog,sync,%zu,%llu,%d,%.0f,%.2f,%.0f,%.2f,%llu,%llu,%.6f,%.6f\n",
           plen,
           (unsigned long long)n,
           threads,
           acked_eps,
           mbps,
           acked_eps,
           mbps,
           (unsigned long long)(n - errors.load()),
           (unsigned long long)errors.load(),
           t0,
           t1);
    fprintf(stderr,
            "[chronolog] size=%zu thr=%d | acked %.0f ev/s (%.1f MB/s) | "
            "errors=%llu | %.1f MB in %.2fs\n",
            plen,
            threads,
            acked_eps,
            mbps,
            (unsigned long long)errors.load(),
            (double)n * plen / (1024 * 1024),
            t1 - t0);

    for(int t = 0; t < threads; t++) client.ReleaseStory(chronicle, story_names[t]);
    client.Disconnect();
    return 0;
}
