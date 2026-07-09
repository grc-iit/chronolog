// Test the new keeper-tail playback() path.
// Usage: tail_test <client_conf> <chronicle> <story> <n_write> <settle_secs> <K>
#include <iostream>
#include <vector>
#include <string>
#include <unistd.h>

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>

int main(int argc, char** argv)
{
    if(argc < 7) { std::cerr << "args: conf chronicle story n_write settle K\n"; return 2; }
    std::string conf = argv[1], chr = argv[2], st = argv[3];
    int n_write = atoi(argv[4]), settle = atoi(argv[5]), K = atoi(argv[6]);

    chronolog::ClientConfiguration cm; cm.load_from_file(conf);
    chronolog::chrono_monitor::initialize(cm.LOG_CONF.LOGTYPE, "tail_test.log", cm.LOG_CONF.LOGLEVEL,
        cm.LOG_CONF.LOGNAME, cm.LOG_CONF.LOGFILESIZE, cm.LOG_CONF.LOGFILENUM, cm.LOG_CONF.FLUSHLEVEL);
    chronolog::ClientPortalServiceConf p;
    p.PROTO_CONF = cm.PORTAL_CONF.PROTO_CONF; p.IP = cm.PORTAL_CONF.IP;
    p.PORT = cm.PORTAL_CONF.PORT; p.PROVIDER_ID = cm.PORTAL_CONF.PROVIDER_ID;

    chronolog::Client client(p);
    if(client.Connect() != chronolog::CL_SUCCESS) { std::cerr << "connect failed\n"; return 1; }
    client.CreateChronicle(chr);
    auto acq = client.AcquireStory(chr, st);
    if(acq.first != chronolog::CL_SUCCESS) { std::cerr << "acquire failed " << acq.first << "\n"; return 1; }
    auto* handle = acq.second;

    std::cout << "[*] writing " << n_write << " events...\n";
    for(int i = 0; i < n_write; i++)
        handle->log_event("evt#" + std::to_string(i) + " :: payload-padding-padding-padding-" + std::to_string(i));

    std::cout << "[*] holding " << settle << "s for chunks to seal into the tail...\n";
    sleep(settle);

    std::vector<chronolog::Event> events;
    int rc = handle->playback(K, events);
    std::cout << "[*] playback(K=" << K << ") rc=" << rc << " returned " << events.size() << " events\n";
    size_t show = events.size() < 6 ? events.size() : 6;
    for(size_t i = 0; i < show / 2; i++)
        std::cout << "    [" << i << "] t=" << events[i].time() << "  " << events[i].log_record() << "\n";
    if(events.size() > show) std::cout << "    ...\n";
    for(size_t i = (events.size() < 3 ? 0 : events.size() - 3); i < events.size(); i++)
        std::cout << "    [" << i << "] t=" << events[i].time() << "  " << events[i].log_record() << "\n";

    // sanity: ascending order?
    bool ordered = true;
    for(size_t i = 1; i < events.size(); i++) if(events[i].time() < events[i-1].time()) ordered = false;
    std::cout << "[*] ascending-order=" << (ordered ? "yes" : "NO") << "\n";

    client.ReleaseStory(chr, st);
    client.Disconnect();
    return events.empty() ? 3 : 0;
}
