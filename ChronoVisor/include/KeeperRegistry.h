#ifndef KEEPER_REGISTRY_H
#define KEEPER_REGISTRY_H

#include <mutex>
#include <vector>
#include <map>
#include <chrono>
#include <thallium.hpp>

#include "chronolog_types.h"
#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"
#include "KeeperRegistrationMsg.h"
#include "GrapherIdCard.h"
#include "GrapherRegistrationMsg.h"
#include "ConfigurationManager.h"

namespace chronolog
{

struct RegistryConfiguration
{
    std::string REGISTRY_SERVICE_PROTOCOL;
    std::string REGISTRY_SERVICE_IP;
    std::string REGISTRY_SERVICE_PORT;
    uint16_t REGISTRY_SERVICE_PROVIDER_ID;
    uint16_t SERVICE_THREAD_COUNT;
};

class DataStoreAdminClient;

class KeeperRegistryService;

class KeeperProcessEntry
{
public:
    KeeperProcessEntry(KeeperIdCard const& keeper_id_card, ServiceId const& admin_service_id)
        : idCard(keeper_id_card)
        , adminServiceId(admin_service_id)
        , keeperAdminClient(nullptr)
        , active(false)
        , lastStatsTime(0)
        , activeStoryCount(0)
    {}

    KeeperProcessEntry(KeeperProcessEntry const& other) = default;

    void reset()
    {
        keeperAdminClient = nullptr;
        active = false;
        lastStatsTime = 0;
        activeStoryCount = 0;
    }

    ~KeeperProcessEntry() = default;// Registry is reponsible for creating & deleting keeperAdminClient

    KeeperIdCard idCard;
    ServiceId adminServiceId;
    std::string idCardString;
    DataStoreAdminClient* keeperAdminClient;
    bool active;
    uint64_t lastStatsTime;
    uint32_t activeStoryCount;
    std::list<std::pair<std::time_t, DataStoreAdminClient*>> delayedExitClients;
};

class GrapherProcessEntry
{
public:
    GrapherProcessEntry(GrapherIdCard const& id_card, ServiceId const& admin_service_id)
        : idCard(id_card)
        , adminServiceId(admin_service_id)
        , adminClient(nullptr)
        , active(false)
        , lastStatsTime(0)
        , activeStoryCount(0)
    {}

    GrapherProcessEntry(GrapherProcessEntry const& other) = default;
    ~GrapherProcessEntry() = default;// Registry is reponsible for creating & deleting keeperAdminClient

    GrapherIdCard idCard;
    ServiceId adminServiceId;
    std::string idCardString;
    DataStoreAdminClient* adminClient;
    bool active;
    uint64_t lastStatsTime;
    uint32_t activeStoryCount;
    std::list<std::pair<std::time_t, DataStoreAdminClient*>> delayedExitGrapherClients;
    };


    class RecordingGroup
    {
    public:
        RecordingGroup(RecordingGroupId group_id, GrapherProcessEntry* grapher_ptr = nullptr)
            : groupId(group_id)
            , grapherProcess(grapher_ptr)
        {}

        RecordingGroup(RecordingGroup const& other) = default;
        ~RecordingGroup() = default;

        void startDelayedGrapherExit(GrapherProcessEntry&, std::time_t);
        void clearDelayedExitGrapher(GrapherProcessEntry&, std::time_t);
        void startDelayedKeeperExit(KeeperProcessEntry&, std::time_t);
        void clearDelayedExitKeeper(KeeperProcessEntry&, std::time_t);

        RecordingGroupId groupId;
        GrapherProcessEntry* grapherProcess;
        std::map<std::pair<uint32_t, uint16_t>, KeeperProcessEntry> keeperProcesses;
    };

    class KeeperRegistry
    {
        enum RegistryState
        {
            UNKNOWN = 0,
            INITIALIZED = 1, // RegistryService is initialized, no active keepers
            RUNNING = 2,     // RegistryService and active Keepers
            SHUTTING_DOWN = 3// Shutting down services
        };

    public:
        KeeperRegistry()
            : registryState(UNKNOWN)
            , registryEngine(nullptr)
            , keeperRegistryService(nullptr)
        {}

        ~KeeperRegistry();

        bool is_initialized() const { return (INITIALIZED == registryState); }

        bool is_running() const { return (RUNNING == registryState); }

        bool is_shutting_down() const { return (SHUTTING_DOWN == registryState); }

        int InitializeRegistryService(ChronoLog::ConfigurationManager const&);

        int ShutdownRegistryService();

        int registerKeeperProcess(KeeperRegistrationMsg const& keeper_reg_msg);

        int unregisterKeeperProcess(KeeperIdCard const& keeper_id_card);

        void updateKeeperProcessStats(KeeperStatsMsg const& keeperStatsMsg);

        std::vector<KeeperIdCard>& getActiveKeepers(std::vector<KeeperIdCard>& keeper_id_cards);

        int notifyKeepersOfStoryRecordingStart(std::vector<KeeperIdCard>&, ChronicleName const&, StoryName const&,
                                               StoryId const&);

        int notifyKeepersOfStoryRecordingStop(std::vector<KeeperIdCard> const&, StoryId const&);

        int registerGrapherProcess(GrapherRegistrationMsg const& reg_msg);
        int unregisterGrapherProcess(GrapherIdCard const& id_card);

    private:
        KeeperRegistry(KeeperRegistry const&) = delete;//disable copying
        KeeperRegistry& operator=(KeeperRegistry const&) = delete;

        RegistryState registryState;
        std::mutex registryLock;
        std::map<RecordingGroupId, RecordingGroup> recordingGroups;
        thallium::engine* registryEngine;
        KeeperRegistryService* keeperRegistryService;
        size_t delayedDataAdminExitSeconds;
    };
}

#endif
