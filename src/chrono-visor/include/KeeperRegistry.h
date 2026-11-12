#ifndef KEEPER_REGISTRY_H
#define KEEPER_REGISTRY_H

#include <chrono>
#include <cstdint>
#include <ctime>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <thallium.hpp>

#include "chronolog_types.h"
#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"
#include "KeeperRegistrationMsg.h"
#include "GrapherIdCard.h"
#include "GrapherRegistrationMsg.h"
#include "GrapherStatsMsg.h"
#include "PlayerIdCard.h"
#include "PlayerRegistrationMsg.h"
#include "PlayerStatsMsg.h"
#include "ConfigurationBlocks.h"
#include <RpcPolicy.h>
#include <VisorAdminRuntime.h>
#include <memory>
#include <atomic>

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
    {
        idCardString += idCard;
    }

    KeeperProcessEntry(KeeperProcessEntry const& other) = default;
    ~KeeperProcessEntry() = default; // Shared descriptors retain in-flight admin work.

    KeeperIdCard idCard;
    ServiceId adminServiceId;
    std::string idCardString;
    std::shared_ptr<DataStoreAdminClient> keeperAdminClient;
    bool active;
    uint64_t lastStatsTime;
    uint32_t activeStoryCount;
    std::list<std::pair<std::time_t, std::shared_ptr<DataStoreAdminClient>>> delayedExitClients;
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
    {
        idCardString += idCard;
    }

    GrapherProcessEntry(GrapherProcessEntry const& other) = default;
    ~GrapherProcessEntry() = default; // Shared descriptors retain in-flight admin work.

    GrapherIdCard idCard;
    ServiceId adminServiceId;
    std::string idCardString;
    std::shared_ptr<DataStoreAdminClient> adminClient;
    bool active;
    uint64_t lastStatsTime;
    uint32_t activeStoryCount;
    std::list<std::pair<std::time_t, std::shared_ptr<DataStoreAdminClient>>> delayedExitGrapherClients;
};


class PlayerProcessEntry
{
public:
    PlayerProcessEntry(PlayerIdCard const& id_card, ServiceId const& admin_service_id)
        : idCard(id_card)
        , adminServiceId(admin_service_id)
        , adminClient(nullptr)
        , active(false)
        , lastStatsTime(0)
    {
        idCardString += idCard;
    }

    PlayerProcessEntry(PlayerProcessEntry const& other) = default;
    ~PlayerProcessEntry() = default; // Shared descriptors retain in-flight admin work.

    PlayerIdCard idCard;
    ServiceId adminServiceId;
    std::string idCardString;
    std::shared_ptr<DataStoreAdminClient> adminClient;
    bool active;
    uint64_t lastStatsTime;
    uint32_t activeStoryCount;
    std::list<std::pair<std::time_t, std::shared_ptr<DataStoreAdminClient>>> delayedExitPlayerClients;
};


class RecordingGroup
{
public:
    RecordingGroup(RecordingGroupId group_id,
                   std::shared_ptr<GrapherProcessEntry> grapher_ptr = nullptr,
                   std::shared_ptr<PlayerProcessEntry> player_ptr = nullptr)
        : groupId(group_id)
        , activeKeeperCount(0)
        , grapherProcess(std::move(grapher_ptr))
        , playerProcess(std::move(player_ptr))
    {}

    RecordingGroup(RecordingGroup const& other) = default;
    ~RecordingGroup() = default;

    bool isActive() const;
    void startDelayedGrapherExit(GrapherProcessEntry&, std::time_t);
    void clearDelayedExitGrapher(GrapherProcessEntry&, std::time_t);
    void startDelayedKeeperExit(KeeperProcessEntry&, std::time_t);
    void clearDelayedExitKeeper(KeeperProcessEntry&, std::time_t);
    void startDelayedPlayerExit(PlayerProcessEntry&, std::time_t);
    void clearDelayedExitPlayer(PlayerProcessEntry&, std::time_t);

    std::vector<KeeperIdCard>& getActiveKeepers(std::vector<KeeperIdCard>& keeper_id_cards);

    RecordingGroupId groupId;
    size_t activeKeeperCount;
    std::shared_ptr<GrapherProcessEntry> grapherProcess;
    std::shared_ptr<PlayerProcessEntry> playerProcess;
    std::map<std::pair<uint32_t, uint16_t>, KeeperProcessEntry> keeperProcesses;
};

class KeeperRegistry: public std::enable_shared_from_this<KeeperRegistry>
{
    enum RegistryState
    {
        UNKNOWN = 0,
        INITIALIZED = 1,  // RegistryService is initialized, no active keepers
        RUNNING = 2,      // RegistryService and active Keepers
        SHUTTING_DOWN = 3 // Shutting down services
    };

public:
    static std::shared_ptr<KeeperRegistry> Create(RpcTimeoutPolicy const& policy = {},
                                                  std::size_t control_capacity = 32);

    static std::shared_ptr<KeeperRegistry> CreateOwned(RpcTimeoutPolicy const&,
                                                       std::shared_ptr<rpc::RpcRuntime> const& actualC32);
    int InitializeOwnedRegistryService(std::shared_ptr<thallium::engine> const&, uint16_t provider, size_t delayedExit);
    void releaseOwnedRegistryService(); // Charged worker, after native handler fence.
    void quiescePublic() noexcept
    {
        publicClosed.store(true, std::memory_order_release);
        registryState.store(SHUTTING_DOWN, std::memory_order_release);
    }
    bool quiescePublic(rpc::RpcDeadline const&);
    bool noRecordedDependencies(rpc::RpcDeadline const&);

    ~KeeperRegistry();

    std::shared_ptr<VisorAdminRuntime> adminOwner() const { return adminRuntime; }

    bool is_initialized() const { return (INITIALIZED == registryState); }

    bool is_running() const { return !publicClosed.load(std::memory_order_acquire) && RUNNING == registryState; }

    bool is_shutting_down() const
    {
        return publicClosed.load(std::memory_order_acquire) || SHUTTING_DOWN == registryState;
    }

    int InitializeRegistryService(RPCProviderConf const&, size_t delayed_exit_secs);

    int ShutdownRegistryService();
    int ShutdownRegistryService(rpc::RpcDeadline const& deadline);
    bool dependenciesUnregistered(rpc::RpcDeadline const& deadline);

    int registerKeeperProcess(KeeperRegistrationMsg const& keeper_reg_msg);
    int unregisterKeeperProcess(KeeperIdCard const& keeper_id_card);
    void updateKeeperProcessStats(KeeperStatsMsg const& keeperStatsMsg);

    int notifyRecordingGroupOfStoryRecordingStart(ChronicleName const&,
                                                  StoryName const&,
                                                  StoryId const&,
                                                  std::vector<KeeperIdCard>&,
                                                  ServiceId&,
                                                  rpc::RpcDeadline const* = nullptr,
                                                  bool* locally_rejected_before_setup = nullptr);
    int notifyRecordingGroupOfStoryRecordingStop(StoryId const&, rpc::RpcDeadline const* = nullptr);
    // Sends the stop of a Closing recording to its stopTargets and, once each
    // confirms, removes it from activeStories. CL_SUCCESS or the first failure.
    int resendStops(std::shared_ptr<StoryRecording> const&, rpc::RpcDeadline const&);

    // Broadcast destroy_story to every active Grapher in the registry.
    // A story may have been recorded by several recording groups over its
    // lifetime: an acquire picks the group from the story id and the active
    // groups, so the story moves when a group joins or leaves, and archives
    // written before #706 came from a group drawn at random at every acquire.
    // HDF5 files for the same story can therefore live on Graphers other than
    // the one currently recording it. Each Grapher filters by filename and
    // no-ops on stories it never saw.
    int notifyAllGraphersOfStoryDestruction(ChronicleName const&,
                                            StoryName const&,
                                            StoryId const&,
                                            rpc::RpcDeadline const* = nullptr);

    // Broadcast destroy_chronicle to every active Grapher in the registry.
    int notifyAllGraphersOfChronicleDestruction(ChronicleName const&, rpc::RpcDeadline const* = nullptr);

    int registerGrapherProcess(GrapherRegistrationMsg const& reg_msg);
    int unregisterGrapherProcess(GrapherIdCard const& id_card);
    void updateGrapherProcessStats(GrapherStatsMsg const&);

    int registerPlayerProcess(PlayerRegistrationMsg const& reg_msg);
    int unregisterPlayerProcess(PlayerIdCard const& id_card);
    void updatePlayerProcessStats(PlayerStatsMsg const&);

private:
    friend struct KeeperRegistryTestAccess;
    explicit KeeperRegistry(RpcTimeoutPolicy const&,
                            std::size_t control_capacity,
                            std::shared_ptr<AdminLookup> lookup = {},
                            std::shared_ptr<rpc::RpcRuntime> actual = {});
    KeeperRegistry(KeeperRegistry const&) = delete; //disable copying
    KeeperRegistry& operator=(KeeperRegistry const&) = delete;

    std::atomic<RegistryState> registryState;
    std::atomic<bool> publicClosed{false}; // Irreversible despite an older in-flight registration commit.
    int shutdownStatus = CL_SUCCESS;
    bool shutdownPrepared = false;
    std::vector<std::shared_ptr<DataStoreAdminClient>> shutdownKeepers;
    std::vector<std::shared_ptr<DataStoreAdminClient>> shutdownDependencies;
    std::shared_ptr<VisorAdminRuntime> adminRuntime;
    uint64_t nextGeneration = 0;
    std::map<std::string, bool> chronicleDestructions;
    std::mutex registryLock;
    std::shared_ptr<thallium::engine> registryEngine;
    KeeperRegistryService* keeperRegistryService;
    std::shared_ptr<KeeperRegistryService> ownedProvider;
    bool nativeOwned = false;
    std::string dataStoreAdminServiceProtocol;
    size_t delayedDataAdminExitSeconds;

    std::map<RecordingGroupId, RecordingGroup> recordingGroups;
    std::vector<RecordingGroup*> activeGroups;
    std::map<StoryId, std::shared_ptr<StoryRecording>> activeStories;
    // Sends start_story_recording for each recording to a keeper that just
    // registered. Called without registryLock; the starts are already in the
    // recordings, so a later stop waits for them and reaches the keeper.
    struct HeldStart
    {
        std::shared_ptr<StoryRecording> recording;
        std::shared_ptr<DataStoreAdminClient> client;
        std::shared_ptr<AdminCompletion> completion;
        rpc::RpcReservation reservation;
    };
    void startHeldStoriesOnKeeper(KeeperIdCard const&, std::vector<HeldStart>);
};
} // namespace chronolog

#endif
