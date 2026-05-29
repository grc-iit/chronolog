// Integration test for the callback overload of Client::ReplayStory (#249).
//
// Verifies:
//   1. Count parity — the callback variant fires once per event, matching the
//      vector-overload's result size for the same [start, end) range.
//   2. Event-content parity — events delivered to the callback compare equal
//      to those returned by the vector overload, in order.
//   3. Exception safety — a user callback that throws does not hang the client;
//      ReplayStory still returns to the caller and subsequent queries still work.
//
// Like the other tests under tests/integration/client/, this test needs a live
// ChronoLog deployment and a story whose chunks have already been archived to
// HDF5 by the Grapher. The seed-write phase logs events first, then the test
// sleeps long enough for the inactive-pipeline flush to land in archive before
// replaying.

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <cmd_arg_parse.h>
#include <chronolog_client.h>
#include <ClientConfiguration.h>
#include <chrono_monitor.h>

namespace
{

constexpr int kEventCount = 16;
constexpr auto kFlushWait = std::chrono::seconds(45);

bool events_equal(chronolog::Event const& a, chronolog::Event const& b)
{
    return a.time() == b.time() && a.client_id() == b.client_id() && a.index() == b.index() &&
           a.log_record() == b.log_record();
}

// Seed events into a fresh story and return [first_ts, last_ts] of what we wrote.
// Returns {0, 0} on any failure.
std::pair<uint64_t, uint64_t>
seed_story(chronolog::Client& client, std::string const& chronicle, std::string const& story)
{
    int ret = client.CreateChronicle(chronicle);
    if(ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        LOG_ERROR("[ReplayCallbackTest] CreateChronicle failed: {}", ret);
        return {0, 0};
    }

    auto acquire = client.AcquireStory(chronicle, story);
    if(acquire.first != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ReplayCallbackTest] AcquireStory failed: {}", acquire.first);
        return {0, 0};
    }

    uint64_t first_ts = 0, last_ts = 0;
    for(int i = 0; i < kEventCount; ++i)
    {
        uint64_t ts = acquire.second->log_event("callback-test event " + std::to_string(i));
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
    {
        confManager.load_from_file(conf_file_path);
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

    chronolog::Client client(portalConf, queryConf);
    if(client.Connect() != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ReplayCallbackTest] Connect failed");
        return 1;
    }

    std::string const chronicle = "CALLBACK_TEST_CHRONICLE";
    std::string const story = "CALLBACK_TEST_STORY";

    auto [first_ts, last_ts] = seed_story(client, chronicle, story);
    if(first_ts == 0)
    {
        LOG_ERROR("[ReplayCallbackTest] Failed to seed story; aborting");
        client.Disconnect();
        return 1;
    }

    // Give the inactive pipeline time to flush into the HDF5 archive so the
    // Player has something to replay.
    LOG_INFO("[ReplayCallbackTest] Waiting {}s for archive flush", kFlushWait.count());
    std::this_thread::sleep_for(kFlushWait);

    uint64_t const start = first_ts;
    uint64_t const end = last_ts + 1; // ReplayStory range is half-open

    // 1) Vector baseline
    std::vector<chronolog::Event> baseline;
    int rc_vec = client.ReplayStory(chronicle, story, start, end, baseline);
    if(rc_vec != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ReplayCallbackTest] Vector ReplayStory failed: {}", rc_vec);
        client.Disconnect();
        return 1;
    }
    LOG_INFO("[ReplayCallbackTest] Vector overload returned {} events", baseline.size());

    // 2) Callback parity
    std::vector<chronolog::Event> collected;
    collected.reserve(baseline.size());
    int rc_cb = client.ReplayStory(
            chronicle,
            story,
            start,
            end,
            chronolog::Client::EventCallback{[&](chronolog::Event const& e) { collected.push_back(e); }});
    if(rc_cb != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ReplayCallbackTest] Callback ReplayStory failed: {}", rc_cb);
        client.Disconnect();
        return 1;
    }
    LOG_INFO("[ReplayCallbackTest] Callback overload delivered {} events", collected.size());

    assert(collected.size() == baseline.size() && "callback invocation count must equal vector size");
    for(size_t i = 0; i < baseline.size(); ++i)
    {
        assert(events_equal(collected[i], baseline[i]) && "callback events must match vector events in order");
    }

    // 3) Exception safety — a throwing callback must not hang the client.
    //    After ReplayStory returns, a subsequent query should still succeed.
    std::atomic<int> seen_before_throw{0};
    int rc_throw = client.ReplayStory(chronicle,
                                      story,
                                      start,
                                      end,
                                      chronolog::Client::EventCallback{[&](chronolog::Event const&)
                                                                       {
                                                                           if(seen_before_throw.fetch_add(1) == 0)
                                                                           {
                                                                               throw std::runtime_error(
                                                                                       "intentional test failure");
                                                                           }
                                                                       }});
    if(rc_throw != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ReplayCallbackTest] Throwing-callback ReplayStory returned {} (expected CL_SUCCESS — "
                  "the callback exception should be caught and logged, not surfaced)",
                  rc_throw);
        client.Disconnect();
        return 1;
    }
    assert(seen_before_throw.load() > 0 && "throwing callback was never invoked");

    // 4) Client is still usable after the throwing callback.
    std::vector<chronolog::Event> after;
    int rc_after = client.ReplayStory(chronicle, story, start, end, after);
    if(rc_after != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ReplayCallbackTest] Post-throw ReplayStory failed: {}", rc_after);
        client.Disconnect();
        return 1;
    }
    assert(after.size() == baseline.size() && "client state must survive a throwing user callback");

    LOG_INFO("[ReplayCallbackTest] All assertions passed ({} events round-tripped through both overloads)",
             baseline.size());

    client.DestroyStory(chronicle, story);
    client.DestroyChronicle(chronicle);
    client.Disconnect();
    return 0;
}
