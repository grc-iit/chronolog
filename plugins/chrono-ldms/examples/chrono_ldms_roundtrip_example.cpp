// chrono_ldms_roundtrip_example.cpp
//
// Self-contained, self-verifying round trip for the chrono-ldms /
// store_chronolog integration:
//
//   1. RECORD  -- write a handful of LDMS-shaped JSON metric samples, exactly
//                 as the store_chronolog ldmsd plugin emits them when an LDMS
//                 aggregator (e.g. an L2) routes metric sets to it.
//                 store_chronolog maps container -> Chronicle and one sample ->
//                 one Event; its story name is "<schema>_<producer>", one per
//                 producer feeding the storage policy. This example has no
//                 producers to fan out over, so it uses the schema alone as the
//                 story name -- the event payloads are identical, only the story
//                 name differs from a live plugin deployment.
//
//   2. TAIL READ  -- read the samples straight from the keepers' in-memory tail
//                    via StoryHandle::playback(), and VERIFY the payloads read
//                    back match exactly what was written. This is the round trip
//                    the example asserts; the process exit code reflects it.
//                    It needs only the keeper/portal (no player).
//
//   3. ARCHIVE READ -- best-effort probe of Client::ReplayStory() over the
//                      player / query service. A sealed chunk stays in the keeper
//                      tail until it leaves it: after tail_retention_secs
//                      (default 60) beyond the chunk's end time, or earlier under
//                      tail_capacity pressure. A small run hits the former, so
//                      its samples are readable via the tail well before they
//                      reach the archive. This leg is informational only and
//                      never affects the exit code.
//
// Each sample carries a per-run tag so the verification is robust to data left
// in the story by previous runs (playback returns the most-recent `count`
// events, which are this run's). Exit code: 0 if the tail round trip verified,
// 1 otherwise.
//
// Usage:
//   chrono-ldms-example-roundtrip [-c <client_conf>] [container] [schema] [count] [max_settle_seconds]
// Defaults: container=ldms, schema=meminfo, count=10, max_settle_seconds=90.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h> // getpid

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>
#include <cmd_arg_parse.h> // pulls in <getopt.h> (optind) used by parse_conf_path_arg

namespace
{

// Build one LDMS-style JSON metric record, matching what store_chronolog emits
// (timestamp/producer/instance/schema envelope + a metrics object), plus a
// per-run tag so repeated runs against the same story stay distinguishable.
std::string make_ldms_sample(std::string const& schema, uint64_t sample_sec, long mem_total, std::string const& run_tag)
{
    std::ostringstream o;
    o << "{\"timestamp\":" << sample_sec << ".000000" << ",\"producer\":\"node1\"" << ",\"instance\":\"node1/" << schema
      << "\"" << ",\"schema\":\"" << schema << "\"" << ",\"run\":\"" << run_tag << "\""
      << ",\"metrics\":{\"MemTotal\":" << mem_total << "}}";
    return o.str();
}

void print_events(char const* label, std::vector<chronolog::Event> const& events)
{
    std::cout << "  " << label << ": " << events.size() << " sample(s)\n";
    for(size_t i = 0; i < events.size(); ++i)
    {
        std::cout << "    [" << i << "] t=" << events[i].time() << "  " << events[i].log_record() << "\n";
    }
}

// The round trip is verified iff the tail returned exactly the payloads we wrote,
// in order.
bool tail_matches(std::vector<std::string> const& expected, std::vector<chronolog::Event> const& got)
{
    if(got.size() != expected.size())
        return false;
    for(size_t i = 0; i < expected.size(); ++i)
    {
        if(got[i].log_record() != expected[i])
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string conf_file_path = parse_conf_path_arg(argc, argv);

    // parse_conf_path_arg() runs getopt_long, which permutes argv and leaves
    // `optind` at the first non-option argument -- so read the positionals from
    // argv[optind..] rather than re-scanning argv by hand (which would mishandle
    // the -cPATH / --config=PATH attached forms getopt accepts).
    std::string container = "ldms";
    std::string schema = "meminfo";
    size_t count = 10;
    int max_settle_seconds = 90;
    {
        std::vector<std::string> pos(argv + (optind < argc ? optind : argc), argv + argc);
        try
        {
            if(pos.size() >= 1)
                container = pos[0];
            if(pos.size() >= 2)
                schema = pos[1];
            if(pos.size() >= 3)
                count = static_cast<size_t>(std::stoul(pos[2]));
            if(pos.size() >= 4)
                max_settle_seconds = std::stoi(pos[3]);
        }
        catch(std::exception const&)
        {
            std::cerr << "usage: chrono-ldms-example-roundtrip [-c conf] [container] [schema] "
                         "[count] [max_settle_seconds]\n";
            return 1;
        }
    }
    if(count == 0)
        count = 1;
    if(max_settle_seconds < 0)
        max_settle_seconds = 0;

    // Load config; build BOTH the portal config (write + tail read) and the
    // query-service config (best-effort archive read via the player).
    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty() && !confManager.load_from_file(conf_file_path))
    {
        std::cerr << "[chrono-ldms roundtrip] failed to load '" << conf_file_path << "', using client defaults\n";
    }

    chronolog::chrono_monitor::initialize(confManager.LOG_CONF.LOGTYPE,
                                          confManager.LOG_CONF.LOGFILE,
                                          confManager.LOG_CONF.LOGLEVEL,
                                          confManager.LOG_CONF.LOGNAME,
                                          confManager.LOG_CONF.LOGFILESIZE,
                                          confManager.LOG_CONF.LOGFILENUM,
                                          confManager.LOG_CONF.FLUSHLEVEL);

    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    chronolog::ClientQueryServiceConf queryConf;
    queryConf.PROTO_CONF = confManager.QUERY_CONF.PROTO_CONF;
    queryConf.IP = confManager.QUERY_CONF.IP;
    queryConf.PORT = confManager.QUERY_CONF.PORT;
    queryConf.PROVIDER_ID = confManager.QUERY_CONF.PROVIDER_ID;

    // writer/reader mode. Connect() targets the ChronoVisor (not the player), so
    // the write + tail round trip below works even if no player is running; the
    // player is only contacted by the best-effort ReplayStory() at the end.
    chronolog::Client client(portalConf, queryConf);
    if(client.Connect() != chronolog::CL_SUCCESS)
    {
        std::cerr << "[chrono-ldms roundtrip] Connect failed\n";
        return 1;
    }

    client.CreateChronicle(container); // idempotent
    auto acq = client.AcquireStory(container, schema);
    if(acq.first != chronolog::CL_SUCCESS)
    {
        std::cerr << "[chrono-ldms roundtrip] AcquireStory(" << container << "/" << schema
                  << ") failed rc=" << acq.first << "\n";
        client.Disconnect();
        return 1;
    }
    auto story_handle = acq.second;

    // ---- 1. RECORD: emit LDMS-shaped samples like store_chronolog does -------
    std::string run_tag =
            std::to_string(static_cast<long>(getpid())) + "." +
            std::to_string(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         std::chrono::system_clock::now().time_since_epoch())
                                                         .count()));
    std::cout << "[chrono-ldms roundtrip] recording " << count << " LDMS sample(s) into " << container << "/" << schema
              << " (run=" << run_tag << ", as an L2 aggregator -> store_chronolog would)\n";
    uint64_t base_sec = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
    std::vector<std::string> expected;
    expected.reserve(count);
    for(size_t i = 0; i < count; ++i)
    {
        std::string sample = make_ldms_sample(schema, base_sec + i, 65467136 - static_cast<long>(i), run_tag);
        story_handle->log_event(sample);
        expected.push_back(sample);
    }

    // ---- 2. TAIL READ: poll playback() until this run's samples are visible --
    // Polling (rather than a fixed sleep) makes the example robust to the
    // deployment's seal timing (chunk_duration + acceptance_window): it returns
    // as soon as the data is sealed into the tail, up to max_settle_seconds.
    std::cout << "[chrono-ldms roundtrip] TAIL read (playback): waiting up to " << max_settle_seconds
              << "s for the samples to seal...\n";
    const int interval = 3;
    int waited = 0;
    bool tail_ok = false;
    std::vector<chronolog::Event> tail_events;
    while(true)
    {
        tail_events.clear();
        int rc = story_handle->playback(count, tail_events);
        if(rc == chronolog::CL_SUCCESS && tail_matches(expected, tail_events))
        {
            tail_ok = true;
            break;
        }
        if(waited >= max_settle_seconds)
            break;
        std::this_thread::sleep_for(std::chrono::seconds(std::min(interval, max_settle_seconds - waited)));
        waited += interval;
    }
    print_events("tail", tail_events);
    std::cout << "[chrono-ldms roundtrip] TAIL round trip: " << (tail_ok ? "PASS" : "FAIL") << " (wrote " << count
              << ", read back " << tail_events.size()
              << (tail_ok ? " matching" : " ; payloads did not match within the settle window") << ")\n";

    // ---- 3. ARCHIVE READ: best-effort ReplayStory() over the player ----------
    // Informational only -- never affects the exit code. start=1, end=very-large
    // reads the whole story from the archive.
    std::cout << "[chrono-ldms roundtrip] ARCHIVE read (ReplayStory, best-effort):\n";
    std::vector<chronolog::Event> archive_events;
    int arc = client.ReplayStory(container, schema, 1, 2000000000000000000ULL, archive_events);
    if(arc != chronolog::CL_SUCCESS)
        std::cout << "  ReplayStory rc=" << chronolog::to_string_client(arc)
                  << " (player/query service may be unavailable)\n";
    print_events("archive", archive_events);
    if(arc == chronolog::CL_SUCCESS && archive_events.empty())
    {
        std::cout << "  (archive empty is expected this soon after writing: a sealed chunk stays\n"
                     "   resident in the keeper tail -- served by playback() above -- and reaches the\n"
                     "   grapher/player archive only once it leaves the tail, after tail_retention_secs\n"
                     "   (default 60) beyond the chunk's end time or earlier under tail_capacity\n"
                     "   pressure. Re-run the archive read after that window to see these samples.)\n";
    }

    client.ReleaseStory(container, schema);
    client.Disconnect();
    return tail_ok ? 0 : 1;
}
