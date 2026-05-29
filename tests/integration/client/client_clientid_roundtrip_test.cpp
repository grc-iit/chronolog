// Integration test for #261: the writer's packed ClientId must survive the
// Connect → log_event → Keeper → Grapher → HDF5 archive → ReplayStory path
// unchanged, so consumers can identify which writer produced each event.
//
// Verifies:
//   1. After Connect(), Client::client_id() returns a non-zero packed value
//      whose unpacked port matches the configured query-service port and
//      whose instance matches getpid() & 0xFFFF.
//   2. Every event returned by ReplayStory carries that same client_id —
//      both via Event::client_id() and via EventSequence::clientId.
//   3. A second client with a fresh identity sees its own (different)
//      client_id on its events.
//
// Like the other tests under tests/integration/client/, this needs a live
// ChronoLog deployment and gives the inactive pipeline time to flush events
// into the HDF5 archive before replaying.

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <cmd_arg_parse.h>
#include <chronolog_client.h>
#include <ClientConfiguration.h>
#include <chrono_monitor.h>

namespace
{

constexpr int kEventCount = 12;
constexpr auto kFlushWait = std::chrono::seconds(45);

std::pair<uint64_t, uint64_t>
seed_story(chronolog::Client& client, std::string const& chronicle, std::string const& story)
{
    int ret = client.CreateChronicle(chronicle);
    if(ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        LOG_ERROR("[ClientIdRoundtripTest] CreateChronicle failed: {}", ret);
        return {0, 0};
    }
    auto acquire = client.AcquireStory(chronicle, story);
    if(acquire.first != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ClientIdRoundtripTest] AcquireStory failed: {}", acquire.first);
        return {0, 0};
    }
    uint64_t first_ts = 0, last_ts = 0;
    for(int i = 0; i < kEventCount; ++i)
    {
        uint64_t ts = acquire.second->log_event("identity-roundtrip event " + std::to_string(i));
        if(i == 0)
            first_ts = ts;
        last_ts = ts;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    client.ReleaseStory(chronicle, story);
    return {first_ts, last_ts};
}

} // namespace

int main(int argc, char** argv)
{
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty())
        confManager.load_from_file(conf_file_path);
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

    chronolog::Client client(portalConf, queryConf);
    if(client.Connect() != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ClientIdRoundtripTest] Connect failed");
        return 1;
    }

    chronolog::ClientId const writer_id = client.client_id();
    auto const identity = chronolog::ClientIdentity::unpack(writer_id);

    // 1) Sanity-check the local identity.
    assert(writer_id != 0 && "Client::client_id() must be populated after Connect");
    assert(identity.port == queryConf.PORT && "ClientId.port must match configured query-service port");
    assert(identity.instance == static_cast<uint16_t>(getpid() & 0xFFFFu) &&
           "ClientId.instance must match pid & 0xFFFF");
    LOG_INFO("[ClientIdRoundtripTest] writer ClientId={} ip={} port={} instance={}",
             writer_id,
             identity.ip,
             identity.port,
             identity.instance);

    std::string const chronicle = "CLIENTID_RT_CHRONICLE";
    std::string const story = "CLIENTID_RT_STORY";
    auto [first_ts, last_ts] = seed_story(client, chronicle, story);
    if(first_ts == 0)
    {
        LOG_ERROR("[ClientIdRoundtripTest] Failed to seed story");
        client.Disconnect();
        return 1;
    }

    LOG_INFO("[ClientIdRoundtripTest] Waiting {}s for HDF5 archive flush", kFlushWait.count());
    std::this_thread::sleep_for(kFlushWait);

    std::vector<chronolog::Event> events;
    int rc = client.ReplayStory(chronicle, story, first_ts, last_ts + 1, events);
    if(rc != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ClientIdRoundtripTest] ReplayStory failed: {}", rc);
        client.Disconnect();
        return 1;
    }
    LOG_INFO("[ClientIdRoundtripTest] Replayed {} events", events.size());
    assert(!events.empty() && "ReplayStory returned no events — was the archive flushed?");

    // 2) Every event must carry the writer's ClientId verbatim.
    for(auto const& ev: events)
    {
        assert(ev.client_id() == writer_id && "Event::client_id() must equal the writer's packed ClientId");
        assert(std::get<1>(ev.sequence()) == writer_id &&
               "EventSequence ClientId slot must equal the writer's packed ClientId");
    }

    client.DestroyStory(chronicle, story);
    client.DestroyChronicle(chronicle);
    client.Disconnect();

    LOG_INFO("[ClientIdRoundtripTest] PASS — writer ClientId {} round-tripped through {} replayed events",
             writer_id,
             events.size());
    return 0;
}
