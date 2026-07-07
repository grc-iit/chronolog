#ifndef CHRONOLOG_CLIENT_IMPL_H
#define CHRONOLOG_CLIENT_IMPL_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <thallium.hpp>

#include <client_errcode.h>
#include <ClientConfiguration.h>
#include <chronolog_types.h>
#include <chronolog_client.h>

#include "rpcVisorClient.h"
#include "StorytellerClient.h"
#include "ClientQueryService.h"

namespace chronolog
{

enum ClientMode
{
    WRITER_MODE = 0,
    READER_MODE = 1,
};

enum ChronologClientState
{
    UNKNOWN = 0,
    CONNECTED = 1,
    READING = 2,
    WRITING = 3,
    SHUTTING_DOWN = 4
};

class ChronologClientImpl
{
public:
    // static mutex ensures that there'd be the single instance
    // of ChronologClientImpl ever created regardless of the
    // thread(s) GetClientImplInstance() is called from
    static std::mutex chronologClientMutex;
    static ChronologClientImpl* chronologClientImplInstance;
    // Number of live Client objects sharing chronologClientImplInstance. The impl
    // (and its Thallium engine) is a process-wide singleton, so it must be deleted
    // exactly once, when the LAST Client is destroyed. Guarded by chronologClientMutex.
    static int chronologClientRefCount;

    static ChronologClientImpl* GetClientImplInstance(chronolog::ClientPortalServiceConf const&,
                                                      ClientMode const&,
                                                      chronolog::ClientQueryServiceConf const&);

    // Drop one Client's reference to the shared impl; deletes it once the count
    // reaches zero. Call this from ~Client() instead of `delete` — the singleton
    // is shared, so a plain delete double-frees it (and leaves a dangling
    // chronologClientImplInstance), which is the crash in issue #692.
    static void ReleaseClientImplInstance();

    // the classs is non-copyable
    ChronologClientImpl(ChronologClientImpl const&) = delete;

    ChronologClientImpl& operator=(ChronologClientImpl const&) = delete;

    ~ChronologClientImpl();

    int Connect();

    int Disconnect();

    ClientId client_id() const { return clientId; }

    int CreateChronicle(std::string const& chronicle_name);

    int DestroyChronicle(std::string const& chronicle_name);

    std::pair<int, StoryHandle*> AcquireStory(std::string const& chronicle_name, std::string const& story_name);

    int ReleaseStory(std::string const& chronicle_name, std::string const& story_name);
    int DestroyStory(std::string const& chronicle_name, std::string const& story_name);

    std::pair<int, std::vector<std::string>> ShowChronicles();
    std::pair<int, std::vector<std::string>> ShowStories(const std::string& chronicle_name);

    int
    replay_story(ChronicleName const&, StoryName const&, uint64_t start, uint64_t end, std::vector<Event>& eventSeries);

    int
    replay_story(ChronicleName const&, StoryName const&, uint64_t start, uint64_t end, Client::EventCallback callback);

private:
    ClientMode clientMode;
    ChronologClientState clientState;
    std::string clientLogin;
    uint32_t euid;
    ClientIdentity clientIdentity;
    ClientId clientId;
    ChronologTimer clockProxy;
    thallium::engine* tlEngine;
    RpcVisorClient* rpcVisorClient;
    StorytellerClient* storyteller;
    ClientQueryService* storyReaderService;

    ChronologClientImpl(ClientPortalServiceConf const&, ClientMode const&, chronolog::ClientQueryServiceConf const&);

    void defineClientIdentity(uint16_t query_service_port);
};
} //namespace chronolog

#endif
