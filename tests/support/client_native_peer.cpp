#include "ClientNativePeer.h"
#include <chronolog_client.h>
#include <ConnectResponseMsg.h>
#include <AcquireStoryResponseMsg.h>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/pair.hpp>
#include <array>
#include <sys/prctl.h>
#include <signal.h>
#include <memory>
using namespace chronolog;
int main(int argc, char** argv)
{
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if(getppid() == 1)
        return 2;
    if(argc != 3)
        return 2;
    auto* state = static_cast<test::PeerState*>(
            mmap(nullptr, sizeof(test::PeerState), PROT_READ | PROT_WRITE, MAP_SHARED, 198, 0));
    close(198);
    if(state == MAP_FAILED)
        return 2;
    std::string protocol = argv[1];
    bool clock = std::string(argv[2]) == "clock";
    auto mid = margo_init((protocol + "://127.0.0.1:0").c_str(), MARGO_SERVER_MODE, 1, 1);
    if(mid == MARGO_INSTANCE_NULL)
        return 3;
    thallium::engine engine(mid);
    auto address = static_cast<std::string>(engine.self());
    auto port = static_cast<uint16_t>(std::stoul(address.substr(address.rfind(':') + 1)));
    uint16_t provider = clock ? 51 : 73;
    std::vector<thallium::remote_procedure> calls;
    calls.push_back(engine.define(
            "Connect",
            [state, mid](thallium::request const& req, uint32_t, ClientId id, uint32_t)
            {
                ++state->connects;
                if(state->delayConnect.load())
                    margo_thread_sleep(mid, 150);
                req.respond(ConnectResponseMsg(CL_SUCCESS, id));
            },
            provider));
    calls.push_back(engine.define(
            "Disconnect",
            [state, mid](thallium::request const& req, ClientId)
            {
                ++state->disconnects;
                if(state->delayDisconnect.load())
                    margo_thread_sleep(mid, 150);
                req.respond(CL_SUCCESS);
            },
            provider));
    calls.push_back(engine.define(
            "ReleaseStory",
            [](thallium::request const& req, ClientId, std::string const&, std::string const&)
            { req.respond(CL_SUCCESS); },
            provider));
    calls.push_back(engine.define(
            "CreateChronicle",
            [state, mid](thallium::request const& req, ClientId, std::string const&)
            {
                if(state->delay.load())
                    margo_thread_sleep(mid, 150);
                req.respond(CL_SUCCESS);
            },
            provider));
    calls.push_back(engine.define(
            "DestroyStory",
            [state, mid](thallium::request const& req, ClientId, std::string const&, std::string const&)
            {
                ++state->destroys;
                state->destroyedGeneration = state->storyGeneration.load();
                if(state->delay.load())
                    margo_thread_sleep(mid, 150);
                req.respond(CL_SUCCESS);
            },
            provider));
    calls.push_back(engine.define(
            "ShowChronicles",
            [state, mid](thallium::request const& req, ClientId)
            {
                if(state->delay.load())
                    margo_thread_sleep(mid, 150);
                req.respond(std::make_pair(CL_SUCCESS, std::vector<std::string>{"late"}));
            },
            provider));
    std::array<margo_instance_id, 3> keepers{MARGO_INSTANCE_NULL, MARGO_INSTANCE_NULL, MARGO_INSTANCE_NULL};
    std::vector<std::unique_ptr<thallium::engine>> keeperWrappers;
    std::vector<ServiceId> roster;
    for(unsigned i = 0; i < (clock ? 3u : 1u); ++i)
    {
        thallium::engine* keeper = &engine;
        uint16_t keeperPort = port;
        if(clock)
        {
            keepers[i] = margo_init((protocol + "://127.0.0.1:0").c_str(), MARGO_SERVER_MODE, 1, 1);
            if(keepers[i] == MARGO_INSTANCE_NULL)
                return 3;
            keeperWrappers.push_back(std::make_unique<thallium::engine>(keepers[i]));
            keeper = keeperWrappers.back().get();
            auto address = static_cast<std::string>(keeper->self());
            keeperPort = static_cast<uint16_t>(std::stoul(address.substr(address.rfind(':') + 1)));
        }
        roster.emplace_back(protocol, "127.0.0.1", keeperPort, clock ? 52 : 74);
        calls.push_back(keeper->define(
                "record_event",
                [state, mid, i](thallium::request const& req, LogEvent const& event)
                {
                    ++state->records;
                    while(state->blockRecords.load()) margo_thread_sleep(mid, 1);
                    unsigned index = state->eventCount.load();
                    if(index < 16)
                    {
                        auto& saved = state->events[index];
                        saved.time = event.time();
                        saved.storyId = event.storyId;
                        saved.eventIndex = event.eventIndex;
                        saved.route = i;
                        std::strncpy(saved.record, event.logRecord.c_str(), sizeof(saved.record) - 1);
                        state->eventCount.store(index + 1, std::memory_order_release);
                    }
                    req.respond(CL_SUCCESS);
                },
                clock ? 52 : 74));
    }
    calls.push_back(engine.define(
            "AcquireStory",
            [state, mid, roster, clock](thallium::request const& req,
                                        ClientId,
                                        std::string const&,
                                        std::string const& story)
            {
                ++state->acquisitions;
                if(state->delay.load())
                    margo_thread_sleep(mid, 150);
                req.respond(AcquireStoryResponseMsg(state->acquireStatus.load(),
                                                    clock && story != "a" ? 2 : 1,
                                                    clock || state->roster.load() ? roster : std::vector<ServiceId>{}));
            },
            provider));
    if(write(199, &port, sizeof(port)) != sizeof(port))
        return 4;
    close(199);
    while(!state->stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    state->blockRecords = false;
    // Margo drains delayed handlers before freeing their registration data.
    // Deregistering now would invalidate serialization inside a late respond.
    for(auto keeper: keepers)
        if(keeper != MARGO_INSTANCE_NULL)
            margo_finalize_and_wait(keeper);
    keeperWrappers.clear();
    margo_finalize_and_wait(mid);
    calls.clear();
    munmap(state, sizeof(test::PeerState));
    return 0;
}
