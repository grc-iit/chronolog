// Diagnostic: write N events, optionally HOLD the story acquired, then replay.
// Usage: chrono_rw_test <conf> <chronicle> <story> <n> <hold_secs>
#include <iostream>
#include <vector>
#include <string>
#include <unistd.h>
#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>
#include "bench_common.h"

int main(int argc, char** argv)
{
    if (argc < 6) { std::cerr << "args: conf chronicle story n hold_secs\n"; return 2; }
    std::string conf = argv[1], chr = argv[2], st = argv[3];
    uint64_t n = strtoull(argv[4], 0, 10);
    int hold = atoi(argv[5]);

    chronolog::ClientConfiguration cm; cm.load_from_file(conf);
    chronolog::chrono_monitor::initialize(cm.LOG_CONF.LOGTYPE,"rw.log",cm.LOG_CONF.LOGLEVEL,
        cm.LOG_CONF.LOGNAME,cm.LOG_CONF.LOGFILESIZE,cm.LOG_CONF.LOGFILENUM,cm.LOG_CONF.FLUSHLEVEL);
    chronolog::ClientPortalServiceConf p; p.PROTO_CONF=cm.PORTAL_CONF.PROTO_CONF;p.IP=cm.PORTAL_CONF.IP;p.PORT=cm.PORTAL_CONF.PORT;p.PROVIDER_ID=cm.PORTAL_CONF.PROVIDER_ID;
    chronolog::ClientQueryServiceConf q; q.PROTO_CONF=cm.QUERY_CONF.PROTO_CONF;q.IP=cm.QUERY_CONF.IP;q.PORT=cm.QUERY_CONF.PORT;q.PROVIDER_ID=cm.QUERY_CONF.PROVIDER_ID;
    chronolog::Client client(p, q);
    if (client.Connect()!=chronolog::CL_SUCCESS){std::cerr<<"connect failed\n";return 1;}
    client.CreateChronicle(chr);
    auto acq = client.AcquireStory(chr, st);
    if (acq.first!=chronolog::CL_SUCCESS){std::cerr<<"acquire failed "<<acq.first<<"\n";return 1;}
    std::string payload = make_payload(1024);
    double w0 = now_sec();
    for (uint64_t i=0;i<n;i++) acq.second->log_event(payload);
    std::cout << "wrote "<<n<<" events in "<<(now_sec()-w0)<<"s\n";

    if (hold>0){ std::cout<<"holding story acquired for "<<hold<<"s...\n"; sleep(hold); }

    // archive replay: full range
    std::vector<chronolog::Event> ev;
    double r0=now_sec();
    int rc = client.ReplayStory(chr, st, 1, 4000000000000000000ULL, ev);
    std::cout << "ReplayStory(full) rc="<<rc<<" events="<<ev.size()<<" in "<<(now_sec()-r0)<<"s\n";
    if(!ev.empty()) std::cout << "  first: " << ev.front().log_record().substr(0,80) << "\n";

    client.ReleaseStory(chr, st);
    client.Disconnect();
    return ev.empty()?3:0;
}
