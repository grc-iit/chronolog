/*
 * chrono_read_bench.cpp -- ChronoLog READ benchmark (ReplayStory).
 *
 * One process: writes N events to its own story, HOLDS the story acquired for
 * `settle` seconds (required -- ChronoLog drops data if the story is released
 * before its chunk decays into the keeper), then times two read patterns:
 *   - archive: ReplayStory over the full time range  (bulk historical scan)
 *   - tail:    ReplayStory over the most recent window (~tail_k events)
 * Prints one stdout line per read pattern for the runner to aggregate:
 *   <readtype> <events_read> <payload_bytes> <t_start_epoch> <t_end_epoch>
 *
 * Usage: chrono_read_bench <conf> <chronicle> <payload_size> <n> <settle> <tail_k>
 */
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
    if(argc < 7)
    {
        fprintf(stderr, "args: conf chronicle payload_size n settle tail_k\n");
        return 2;
    }
    std::string conf = argv[1], chr = argv[2];
    size_t plen_req = strtoul(argv[3], 0, 10);
    uint64_t n = strtoull(argv[4], 0, 10);
    int settle = atoi(argv[5]);
    uint64_t tail_k = strtoull(argv[6], 0, 10);

    std::string payload = make_payload(plen_req);
    size_t plen = payload.size();
    std::string story = "r" + std::to_string(plen) + "_" + std::to_string(getpid());

    chronolog::ClientConfiguration cm;
    cm.load_from_file(conf);
    chronolog::chrono_monitor::initialize(cm.LOG_CONF.LOGTYPE,
                                          "cread.log",
                                          cm.LOG_CONF.LOGLEVEL,
                                          cm.LOG_CONF.LOGNAME,
                                          cm.LOG_CONF.LOGFILESIZE,
                                          cm.LOG_CONF.LOGFILENUM,
                                          cm.LOG_CONF.FLUSHLEVEL);
    chronolog::ClientPortalServiceConf p;
    p.PROTO_CONF = cm.PORTAL_CONF.PROTO_CONF;
    p.IP = cm.PORTAL_CONF.IP;
    p.PORT = cm.PORTAL_CONF.PORT;
    p.PROVIDER_ID = cm.PORTAL_CONF.PROVIDER_ID;
    // WRITER-ONLY client: the tail read (playback) talks to the keeper recording
    // service, not the query service, so reader mode is unnecessary — and the
    // reader-mode query connection is what becomes unstable under concurrency.
    chronolog::Client client(p);
    if(client.Connect() != chronolog::CL_SUCCESS)
    {
        fprintf(stderr, "connect failed\n");
        return 1;
    }
    client.CreateChronicle(chr);
    auto acq = client.AcquireStory(chr, story);
    if(acq.first != chronolog::CL_SUCCESS)
    {
        fprintf(stderr, "acquire failed %d\n", acq.first);
        return 1;
    }

    for(uint64_t i = 0; i < n; i++) { acq.second->log_event(payload); }

    sleep(settle); // hold story acquired so data becomes queryable

    // TAIL read: most recent ~tail_k events via the keeper-tail playback() path.
    // (Archive/ReplayStory is intentionally not measured here: with the tail
    // feature recently-sealed data is retained in the keeper hot tier and is not
    // in the cold archive, and ReplayStory needs the reader-mode query service
    // which is the unstable-under-concurrency path. Kafka still measures both.)
    {
        std::vector<chronolog::Event> ev;
        double t0 = now_sec();
        acq.second->playback(tail_k, ev);
        double t1 = now_sec();
        printf("tail %zu %zu %.6f %.6f\n", ev.size(), plen, t0, t1);
        fflush(stdout);
    }

    client.ReleaseStory(chr, story);
    client.Disconnect();
    return 0;
}
