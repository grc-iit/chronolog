#ifndef KEEPER_REGISTRY_H
#define KEEPER_REGISTRY_H

#include <chrono>
#include <cstdint>
#include <ctime>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
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
    ~KeeperProcessEntry() = default; // Registry is reponsible for creating & deleting keeperAdminClient

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
    {
        idCardString += idCard;
    }

    GrapherProcessEntry(GrapherProcessEntry const& other) = default;
    ~GrapherProcessEntry() = default; // Registry is reponsible for creating & deleting keeperAdminClient

    GrapherIdCard idCard;
    ServiceId adminServiceId;
    std::string idCardString;
    DataStoreAdminClient* adminClient;
    bool active;
    uint64_t lastStatsTime;
    uint32_t activeStoryCount;
    std::list<std::pair<std::time_t, DataStoreAdminClient*>> delayedExitGrapherClients;
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
    ~PlayerProcessEntry() = default; // Registry is reponsible for creating & deleting AdminClient

    PlayerIdCard idCard;
    ServiceId adminServiceId;
    std::string idCardString;
    DataStoreAdminClient* adminClient;
    bool active;
    uint64_t lastStatsTime;
    uint32_t activeStoryCount;
    std::list<std::pair<std::time_t, DataStoreAdminClient*>> delayedExitPlayerClients;
};


class RecordingGroup
{
public:
    RecordingGroup(RecordingGroupId group_id,
                   GrapherProcessEntry* grapher_ptr = nullptr,
                   PlayerProcessEntry* player_ptr = nullptr)
        : groupId(group_id)
        , activeKeeperCount(0)
        , grapherProcess(grapher_ptr)
        , playerProcess(player_ptr)
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
    GrapherProcessEntry* grapherProcess;
    PlayerProcessEntry* playerProcess;
    std::map<std::pair<uint32_t, uint16_t>, KeeperProcessEntry> keeperProcesses;
};

// The stories a keeper of `group` must start when it registers: those of
// `active` recorded by that group, with the names held for them in `names`.
// A keeper registering again after a restart has no pipeline for the stories
// clients still hold, and the visor sends start_story_recording only when a
// story is first acquired. A story with no names or no group is left out.
std::vector<std::tuple<ChronicleName, StoryName, StoryId>>
storiesToStartOnRegistration(std::map<StoryId, RecordingGroup*> const& active,
                             std::map<StoryId, std::pair<ChronicleName, StoryName>> const& names,
                             RecordingGroupId group);

class KeeperRegistry
{
    enum RegistryState
    {
        UNKNOWN = 0,
        INITIALIZED = 1,  // RegistryService is initialized, no active keepers
        RUNNING = 2,      // RegistryService and active Keepers
        SHUTTING_DOWN = 3 // Shutting down services
    };

public:
    KeeperRegistry();

    ~KeeperRegistry();

    bool is_initialized() const { return (INITIALIZED == registryState); }

    bool is_running() const { return (RUNNING == registryState); }

    bool is_shutting_down() const { return (SHUTTING_DOWN == registryState); }

    int InitializeRegistryService(RPCProviderConf const&, size_t delayed_exit_secs);

    int ShutdownRegistryService();

    int registerKeeperProcess(KeeperRegistrationMsg const& keeper_reg_msg);
    int unregisterKeeperProcess(KeeperIdCard const& keeper_id_card);
    void updateKeeperProcessStats(KeeperStatsMsg const& keeperStatsMsg);

    int notifyRecordingGroupOfStoryRecordingStart(ChronicleName const&,
                                                  StoryName const&,
                                                  StoryId const&,
                                                  std::vector<KeeperIdCard>&,
                                                  ServiceId&);
    int notifyRecordingGroupOfStoryRecordingStop(StoryId const&);

    // Broadcast destroy_story to every active Grapher in the registry.
    // A story may have been recorded by several recording groups over its
    // lifetime: an acquire picks the group from the story id and the active
    // groups, so the story moves when a group joins or leaves, and archives
    // written before #706 came from a group drawn at random at every acquire.
    // HDF5 files for the same story can therefore live on Graphers other than
    // the one currently recording it. Each Grapher filters by filename and
    // no-ops on stories it never saw.
    int notifyAllGraphersOfStoryDestruction(ChronicleName const&, StoryName const&, StoryId const&);

    // Broadcast destroy_chronicle to every active Grapher in the registry.
    int notifyAllGraphersOfChronicleDestruction(ChronicleName const&);

    int registerGrapherProcess(GrapherRegistrationMsg const& reg_msg);
    int unregisterGrapherProcess(GrapherIdCard const& id_card);
    void updateGrapherProcessStats(GrapherStatsMsg const&);

    int registerPlayerProcess(PlayerRegistrationMsg const& reg_msg);
    int unregisterPlayerProcess(PlayerIdCard const& id_card);
    void updatePlayerProcessStats(PlayerStatsMsg const&);

private:
    KeeperRegistry(KeeperRegistry const&) = delete; //disable copying
    KeeperRegistry& operator=(KeeperRegistry const&) = delete;

    int notifyGrapherOfStoryRecordingStart(RecordingGroup&,
                                           ChronicleName const&,
                                           StoryName const&,
                                           StoryId const&,
                                           uint64_t);
    int notifyGrapherOfStoryRecordingStop(RecordingGroup&, StoryId const&);
    int notifyPlayerOfStoryRecordingStart(RecordingGroup&,
                                          ChronicleName const&,
                                          StoryName const&,
                                          StoryId const&,
                                          uint64_t,
                                          std::vector<KeeperIdCard> const& story_keepers);
    int notifyPlayerOfStoryRecordingStop(RecordingGroup&, StoryId const&);
    int notifyKeepersOfStoryRecordingStart(RecordingGroup&,
                                           std::vector<KeeperIdCard>&,
                                           ChronicleName const&,
                                           StoryName const&,
                                           StoryId const&,
                                           uint64_t);
    int notifyKeepersOfStoryRecordingStop(RecordingGroup&, std::vector<KeeperIdCard> const&, StoryId const&);
    // Called without registryLock once registerKeeperProcess has added the keeper.
    void startHeldStoriesOnKeeper(KeeperIdCard const&,
                                  DataStoreAdminClient*,
                                  std::vector<std::tuple<ChronicleName, StoryName, StoryId>> const&);

    RegistryState registryState;
    std::mutex registryLock;
    thallium::engine* registryEngine;
    KeeperRegistryService* keeperRegistryService;
    std::string dataStoreAdminServiceProtocol;
    size_t delayedDataAdminExitSeconds;

    std::map<RecordingGroupId, RecordingGroup> recordingGroups;
    std::vector<RecordingGroup*> activeGroups;
    std::map<StoryId, RecordingGroup*> activeStories;
    // the chronicle and story names of each entry in activeStories, set and
    // erased with it, so a registering keeper can be told to start them
    std::map<StoryId, std::pair<ChronicleName, StoryName>> activeStoryNames;
};
} // namespace chronolog

#endif
