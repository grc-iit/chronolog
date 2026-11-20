#include <iostream>
#include <thread>
#include <algorithm>
#include <optional>
#include <ctime>
#include <cstdlib>
#include <unistd.h>
#include <thallium.hpp>

#include <KeeperRegistry.h>
#include <StoryGroupSelection.h>
#include <KeeperRegistryService.h>
#include <DataStoreAdminClient.h>
#include <chrono_monitor.h>
#include <ConfigurationBlocks.h>

/////////////////////////

namespace tl = thallium;
namespace chl = chronolog;

namespace chronolog
{

int KeeperRegistry::InitializeRegistryService(chl::RPCProviderConf const& VISOR_KEEPER_REGISTRY_SERVICE_CONF,
                                              size_t DELAYED_DATA_ADMIN_EXIT_IN_SECS)
{
    int status = chronolog::CL_ERR_UNKNOWN;
    std::lock_guard<std::mutex> lock(registryLock);

    if(registryState != UNKNOWN)
    {
        return chronolog::CL_SUCCESS;
    }

    try
    {
        // initialise thalium engine for KeeperRegistryService
        std::string KEEPER_REGISTRY_SERVICE_NA_STRING = VISOR_KEEPER_REGISTRY_SERVICE_CONF.PROTO_CONF + "://" +
                                                        VISOR_KEEPER_REGISTRY_SERVICE_CONF.IP + ":" +
                                                        std::to_string(VISOR_KEEPER_REGISTRY_SERVICE_CONF.BASE_PORT);

        uint16_t provider_id = VISOR_KEEPER_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID;
        margo_instance_id margo_id = margo_init(KEEPER_REGISTRY_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 2);

        if(MARGO_INSTANCE_NULL == margo_id)
        {
            LOG_CRITICAL("[ChronoProcessRegistry] Failed to initialize margo_instance");
            return -1;
        }
        LOG_INFO("[ChronoProcessRegistry] margo_instance initialized with NA_STRING {}",
                 KEEPER_REGISTRY_SERVICE_NA_STRING);

        registryEngine = std::make_shared<tl::engine>(margo_id);
        adminRuntime->engine = registryEngine;
        adminRuntime->initializeRpcDefinitions();

        std::stringstream ss;
        ss << registryEngine->self();
        LOG_INFO("[ChronoProcessRegistry] Starting RegistryService at {} provider id: {}", ss.str(), provider_id);

        keeperRegistryService = KeeperRegistryService::CreateKeeperRegistryService(*registryEngine, provider_id, *this);

        delayedDataAdminExitSeconds = DELAYED_DATA_ADMIN_EXIT_IN_SECS;

        // Kun: This protocol will be used to create dataStoreAdminClient for both Keeper and Grapher.
        // Since they share the same Thallium engine, we have to use the same protocol for both.
        // This is a temporary solution until we have a better way to handle this.
        // Currently, it is protected by the deployment script that ensures the same protocol is used for both.
        // TODO (Kun): add DataStoreAdminService section to the configuration of ChronoVisor and make sure all three
        //  ends (Keeper, Grapher, and Visor) use the same protocol.
        //dataStoreAdminServiceProtocol = KEEPER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF;

        registryState = INITIALIZED;
        status = chronolog::CL_SUCCESS;
    }
    catch(tl::exception const& ex)
    {
        LOG_ERROR("[ChronoProcessRegistry] Failed to start");
    }

    return status;
}

std::shared_ptr<KeeperRegistry> KeeperRegistry::Create(RpcTimeoutPolicy const& policy, std::size_t capacity)
{
    return std::shared_ptr<KeeperRegistry>(new KeeperRegistry(policy, capacity));
}

std::shared_ptr<KeeperRegistry> KeeperRegistry::CreateOwned(RpcTimeoutPolicy const& policy,
                                                            std::shared_ptr<rpc::RpcRuntime> const& actual)
{
    if(!actual)
        throw std::invalid_argument("Visor needs its actual shared C32 owner");
    return std::shared_ptr<KeeperRegistry>(new KeeperRegistry(policy, actual->capacity(), {}, actual));
}
int KeeperRegistry::InitializeOwnedRegistryService(std::shared_ptr<thallium::engine> const& engine,
                                                   uint16_t provider,
                                                   size_t delayedExit)
{
    if(registryEngine || !engine)
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    nativeOwned = true;
    registryEngine = engine;
    adminRuntime->engine = engine;
    adminRuntime->initializeRpcDefinitions();
    delayedDataAdminExitSeconds = delayedExit;
    keeperRegistryService = KeeperRegistryService::CreateOwned(*engine, provider, *this, ownedProvider);
    RegistryState expected = UNKNOWN;
    registryState.compare_exchange_strong(expected, INITIALIZED, std::memory_order_acq_rel);
    return CL_SUCCESS; // A concurrent quiesce is never reopened.
}
void KeeperRegistry::releaseOwnedRegistryService()
{
    ownedProvider.reset();
    keeperRegistryService = nullptr;
    adminRuntime->releaseNativeDescriptors();
    registryEngine.reset();
}
KeeperRegistry::KeeperRegistry(RpcTimeoutPolicy const& policy,
                               std::size_t capacity,
                               std::shared_ptr<AdminLookup> lookup,
                               std::shared_ptr<rpc::RpcRuntime> actual)
    : registryState(UNKNOWN)
    , adminRuntime(std::make_shared<VisorAdminRuntime>(policy, capacity, std::move(lookup), std::move(actual)))
    , registryEngine(nullptr)
    , keeperRegistryService(nullptr)
    , delayedDataAdminExitSeconds(3)
{}

/////////////////

int KeeperRegistry::ShutdownRegistryService()
{
    auto entered = rpc::RpcDeadline::Clock::now();
    return ShutdownRegistryService(
            rpc::RpcDeadline::fromStart(entered, std::chrono::milliseconds(adminRuntime->policy.shutdown_ms)));
}

int KeeperRegistry::ShutdownRegistryService(rpc::RpcDeadline const& deadline)
{
    auto registryLease = shared_from_this();
    auto takeLock = [&]()
    {
        std::unique_lock<std::mutex> lock(registryLock, std::defer_lock);
        while(!deadline.expired())
        {
            if(lock.try_lock())
                return lock;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return lock;
    };
    std::vector<std::shared_ptr<DataStoreAdminClient>> keepers, dependencies;
    {
        auto lock = takeLock();
        if(!lock.owns_lock() || deadline.expired())
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        registryState = SHUTTING_DOWN;
        if(!shutdownPrepared)
        {
            // Keep the exact descriptors and roster. Neither ACK1 nor elapsed
            // grace permits freeing a dependency still needed by a Keeper.
            for(auto const& item: recordingGroups)
            {
                auto const& group = item.second;
                for(auto const& entry: group.keeperProcesses)
                    if(entry.second.active)
                        shutdownKeepers.push_back(entry.second.keeperAdminClient);
                if(group.playerProcess && group.playerProcess->active)
                    shutdownDependencies.push_back(group.playerProcess->adminClient);
                if(group.grapherProcess && group.grapherProcess->active)
                    shutdownDependencies.push_back(group.grapherProcess->adminClient);
            }
            shutdownPrepared = true;
        }
        keepers = shutdownKeepers;
        dependencies = shutdownDependencies;
    }
    int aggregate = CL_SUCCESS;
    for(auto const& client: keepers)
        if(deadline.expired() || !client || client->shutdown_collection(registryLease, deadline) != CL_SUCCESS)
            aggregate = CL_ERR_SHUTDOWN_INCOMPLETE;
    {
        auto lock = takeLock();
        if(!lock.owns_lock() || deadline.expired())
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        // A collection ACK says only that its join finished. The Keeper must
        // retain and archive its data before it sends matching unregister.
        for(auto const& group: recordingGroups)
            for(auto const& entry: group.second.keeperProcesses)
                if(entry.second.active)
                    aggregate = CL_ERR_SHUTDOWN_INCOMPLETE;
    }
    if(aggregate == CL_SUCCESS)
        for(auto const& client: dependencies)
            if(deadline.expired() || !client || client->shutdown_collection(registryLease, deadline) != CL_SUCCESS)
                aggregate = CL_ERR_SHUTDOWN_INCOMPLETE;
    {
        auto lock = takeLock();
        if(!lock.owns_lock() || deadline.expired())
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        shutdownStatus = aggregate;
    }
    return aggregate; // Collection phase only, never a daemon-exit certificate.
}

bool KeeperRegistry::quiescePublic(rpc::RpcDeadline const& deadline)
{
    quiescePublic();
    std::unique_lock<std::mutex> lock(registryLock, std::defer_lock);
    while(!deadline.expired() && !lock.try_lock()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(!lock.owns_lock() || deadline.expired())
        return false;
    registryState = SHUTTING_DOWN; // Observe every older registration critical section.
    return true;
}
bool KeeperRegistry::noRecordedDependencies(rpc::RpcDeadline const& deadline)
{
    std::unique_lock<std::mutex> lock(registryLock, std::defer_lock);
    while(!deadline.expired() && !lock.try_lock()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return lock.owns_lock() && !deadline.expired() && recordingGroups.empty();
}
bool KeeperRegistry::dependenciesUnregistered(rpc::RpcDeadline const& deadline)
{
    std::unique_lock<std::mutex> lock(registryLock, std::defer_lock);
    while(!deadline.expired() && !lock.try_lock()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(!lock.owns_lock() || deadline.expired())
        return false;
    for(auto const& item: recordingGroups)
    {
        auto const& group = item.second;
        for(auto const& entry: group.keeperProcesses)
            if(entry.second.active)
                return false;
        if((group.grapherProcess && group.grapherProcess->active) ||
           (group.playerProcess && group.playerProcess->active))
            return false;
    }
    return true;
}

KeeperRegistry::~KeeperRegistry()
{
    // Destruction never starts remote work. Every admitted AdminWork owns this
    // registry, so this point cannot be reached by a Running/quarantined job.
    // Remote shutdown is explicit while a real shared owner is still alive.
    {
        std::lock_guard<std::mutex> lock(registryLock);
        registryState = SHUTTING_DOWN;
        activeGroups.clear();
        activeStories.clear();
        recordingGroups.clear();
    }
    if(registryEngine && !nativeOwned)
        registryEngine->finalize();
    keeperRegistryService = nullptr;
}
/////////////////


struct KeeperRegistry::HeldTally
{
    HeldTally(size_t count, std::string keeperName)
        : remaining(count)
        , total(count)
        , keeper(std::move(keeperName))
    {}
    std::atomic<size_t> remaining, started{0};
    size_t const total;
    std::string const keeper;
    // Logs, after the last start returns, how many the keeper confirmed, as
    // v3.4.0 did when it started them one after the other.
    void settle(size_t count, bool confirmed)
    {
        if(count == 0)
            return;
        if(confirmed)
            started.fetch_add(count, std::memory_order_relaxed);
        if(remaining.fetch_sub(count, std::memory_order_acq_rel) == count)
            LOG_INFO("[ChronoProcessRegistry] Started {} of {} held stories on keeper {}",
                     started.load(std::memory_order_relaxed),
                     total,
                     keeper);
    }
    void skip(size_t count) { settle(count, false); }
};

int KeeperRegistry::registerKeeperProcess(KeeperRegistrationMsg const& keeper_reg_msg)
{

    if(is_shutting_down())
    {
        return chronolog::CL_ERR_UNKNOWN;
    }

    std::unique_lock<std::mutex> lock(registryLock);
    //re-check state after ther lock is aquired
    if(is_shutting_down())
    {
        return chronolog::CL_ERR_UNKNOWN;
    }

    KeeperIdCard keeper_id_card = keeper_reg_msg.getKeeperIdCard();
    ServiceId admin_service_id = keeper_reg_msg.getAdminServiceId();

    //find the group that keeper belongs to in the registry
    auto group_iter = recordingGroups.find(keeper_id_card.getGroupId());
    if(group_iter == recordingGroups.end())
    {
        auto insert_return = recordingGroups.insert(
                std::pair<RecordingGroupId, RecordingGroup>(keeper_id_card.getGroupId(),
                                                            RecordingGroup(keeper_id_card.getGroupId())));
        if(false == insert_return.second)
        {
            LOG_ERROR("[ChronoProcessRegistry] keeper registration failed to find RecordingGroup {}",
                      keeper_id_card.getGroupId());
            return chronolog::CL_ERR_UNKNOWN;
        }
        else
        {
            group_iter = insert_return.first;
        }
    }

    RecordingGroup& recording_group = (*group_iter).second;

    // unlikely but possible that the Registry still retains the record of the previous re-incarnation of hte Keeper process
    // running on the same host... check for this case and clean up the leftover record...
    auto keeper_process_iter =
            recording_group.keeperProcesses.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());

    if(keeper_process_iter != recording_group.keeperProcesses.end())
    {
        // must be a case of the KeeperProcess exiting without unregistering or some unexpected break in communication...
        // start delayed destruction process for hte lingering keeperAdminclient to be safe...

        if((*keeper_process_iter).second.active)
        {
            std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                    std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));
            LOG_WARNING("[ChronoProcessRegistry] registerKeeperProcess: found old instance of dataAdminclient for {} "
                        "delayedExitTime={}",
                        to_string(keeper_id_card),
                        std::ctime(&delayedExitTime));
            ;

            recording_group.startDelayedKeeperExit((*keeper_process_iter).second, delayedExitTime);
        }
        else
        {
            std::time_t current_time =
                    std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
            LOG_INFO("[ChronoProcessRegistry] registerKeeperProcess tries to clear dataAdmindClient for {}",
                     to_string(keeper_id_card));
            recording_group.clearDelayedExitKeeper((*keeper_process_iter).second, current_time);
        }

        if((*keeper_process_iter).second.delayedExitClients.empty())
        {
            LOG_INFO("[ChronoProcessRegistry] registerKeeperProcess has destroyed old entry for keeper {}",
                     to_string(keeper_id_card));
            recording_group.keeperProcesses.erase(keeper_process_iter);
        }
        else
        {
            LOG_INFO("[ChronoProcessRegistry] registration for Keeper {} cant's proceed as previous AdminClient is not "
                     "yet dismantled",
                     to_string(keeper_id_card));
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    //create a client of Keeper's DataStoreAdminService listening at adminServiceId

    std::string service_na_string;
    admin_service_id.get_service_as_string(service_na_string);

    auto collectionClient = DataStoreAdminClient::CreateDataStoreAdminClient(adminRuntime,
                                                                             service_na_string,
                                                                             admin_service_id.getProviderId());
    if(nullptr == collectionClient)
    {
        LOG_ERROR("[ChronoProcessRegistry] Register Keeper: {} failed to create DataStoreAdminClient for {}",
                  to_string(keeper_id_card),
                  to_string(admin_service_id));
        return chronolog::CL_ERR_UNKNOWN;
    }

    //now create a new KeeperRecord with the new DataAdminclient
    auto insert_return =
            recording_group.keeperProcesses.insert(std::pair<std::pair<uint32_t, uint16_t>, KeeperProcessEntry>(
                    keeper_id_card.getRecordingServiceId().get_service_endpoint(),
                    KeeperProcessEntry(keeper_id_card, admin_service_id)));
    if(false == insert_return.second)
    {
        LOG_ERROR("[ChronoProcessRegistry] registration failed for Keeper {}", to_string(keeper_id_card));
        collectionClient.reset();
        return chronolog::CL_ERR_UNKNOWN;
    }

    (*insert_return.first).second.keeperAdminClient = collectionClient;
    (*insert_return.first).second.active = true;
    recording_group.activeKeeperCount += 1;

    LOG_INFO("[ChronoProcessRegistry] Register Keeper: {} created DataStoreAdminClient for {}",
             to_string(keeper_id_card),
             to_string(admin_service_id));

    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has {} keepers",
             recording_group.groupId,
             recording_group.keeperProcesses.size());
    if(storyStartSlots(recording_group.activeKeeperCount) > adminRuntime->runtime.capacity())
        LOG_WARNING("[ChronoProcessRegistry] RecordingGroup {} has {} active keepers: starting a story there needs "
                    "{} RPC slots at once, more than the visor's {} (rpc_slots), so every acquire of a story no "
                    "client holds in this group fails. Raise rpc_slots to at least {}",
                    recording_group.groupId,
                    recording_group.activeKeeperCount,
                    storyStartSlots(recording_group.activeKeeperCount),
                    adminRuntime->runtime.capacity(),
                    storyStartSlots(recording_group.activeKeeperCount));

    // check if this is the first keeper for the recording group and the group is ready to be part of
    //  the activeGroups rotation

    if(recording_group.isActive() && recording_group.activeKeeperCount == 1)
    {
        add_active_group(activeGroups, &recording_group);
    }

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ",
             activeGroups.size(),
             recordingGroups.size());
    if(activeGroups.size() > 0)
    {
        registryState = RUNNING;
    }

    // A keeper registering again after a restart has no pipeline for the
    // stories clients still hold. Each start joins its story's recording
    // under the lock, so a stop that follows waits for it and also reaches
    // this keeper. The admin runtime has a fixed number of slots, so the
    // starts go out in waves: as many as there are free slots, then more as
    // slots come free. Registration returns once every held story's start is
    // sent, or is given up because no slot came free for control_ms; a
    // durable write to a story given up starts its pipeline instead.
    auto waiting = storiesToStartOnRegistration(activeStories, recording_group.groupId);
    if(waiting.empty())
        return chronolog::CL_SUCCESS;
    LOG_INFO("[ChronoProcessRegistry] Starting {} held stories on keeper {}",
             waiting.size(),
             to_string(keeper_id_card));
    auto tally = std::make_shared<HeldTally>(waiting.size(), to_string(keeper_id_card));
    auto const group_id = recording_group.groupId;
    auto const endpoint = keeper_id_card.getRecordingServiceId().get_service_endpoint();
    auto const idleLimit = std::chrono::milliseconds(adminRuntime->policy.control_ms);
    auto lastStart = std::chrono::steady_clock::now();
    for(;;)
    {
        auto held = reserveHeldStarts(waiting, collectionClient);
        lock.unlock();
        if(!held.empty())
        {
            startHeldStoriesOnKeeper(std::move(held), tally);
            lastStart = std::chrono::steady_clock::now();
        }
        if(waiting.empty())
            break;
        if(std::chrono::steady_clock::now() - lastStart >= idleLimit)
        {
            LOG_WARNING("[ChronoProcessRegistry] No admin slot came free in {} ms to start {} more held stories on "
                        "keeper {}",
                        idleLimit.count(),
                        waiting.size(),
                        to_string(keeper_id_card));
            tally->skip(waiting.size());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
        // The keeper may have unregistered, or registered again with another
        // admin client, while the lock was released.
        bool registered = false;
        auto group = recordingGroups.find(group_id);
        if(group != recordingGroups.end())
        {
            auto process = group->second.keeperProcesses.find(endpoint);
            registered = process != group->second.keeperProcesses.end() && process->second.active &&
                         process->second.keeperAdminClient == collectionClient;
        }
        if(!registered)
        {
            lock.unlock();
            tally->skip(waiting.size());
            break;
        }
        // A story released or destroyed meanwhile is not started.
        auto before = waiting.size();
        waiting.erase(std::remove_if(waiting.begin(),
                                     waiting.end(),
                                     [this](std::shared_ptr<StoryRecording> const& recording)
                                     {
                                         auto found = activeStories.find(recording->id);
                                         return found == activeStories.end() || found->second != recording ||
                                                recording->phase != StoryRecording::Active;
                                     }),
                      waiting.end());
        tally->skip(before - waiting.size());
    }
    return chronolog::CL_SUCCESS;
}

std::vector<KeeperRegistry::HeldStart>
KeeperRegistry::reserveHeldStarts(std::vector<std::shared_ptr<StoryRecording>>& waiting,
                                  std::shared_ptr<DataStoreAdminClient> const& client)
{
    std::vector<HeldStart> held;
    auto deadline = rpc::RpcDeadline::after(std::chrono::milliseconds(adminRuntime->policy.control_ms));
    size_t taken = 0;
    for(; taken < waiting.size(); ++taken)
    {
        auto token = adminRuntime->runtime.reserve(deadline);
        if(!token)
            break;
        auto const& recording = waiting[taken];
        auto completion = std::make_shared<AdminCompletion>();
        recording->clients.push_back(client);
        recording->starts.push_back(completion);
        held.push_back({recording, client, completion, std::move(token)});
    }
    waiting.erase(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(taken));
    return held;
}

void KeeperRegistry::startHeldStoriesOnKeeper(std::vector<HeldStart> held, std::shared_ptr<HeldTally> const& tally)
{
    uint64_t start = std::chrono::system_clock::now().time_since_epoch().count();
    for(auto& item: held)
        item.completion->finished = [tally](AdminCompletion const& done)
        { tally->settle(1, done.confirmed && done.status == chronolog::CL_SUCCESS); };
    for(auto& item: held)
    {
        // The keeper's start_story_recording is idempotent, so a story also
        // started by a concurrent acquire is harmless. Nothing waits for the
        // start here: the runtime keeps it to the end, and its completion
        // records the outcome for the story's stop.
        AdminArguments args{AdminAction::Start,
                            item.recording->chronicle,
                            item.recording->story,
                            item.recording->id,
                            start,
                            {}};
        adminRuntime->runtime
                .submit<int>(std::move(item.reservation),
                             std::make_shared<AdminWork>(std::move(item.client),
                                                         std::move(args),
                                                         std::move(item.completion),
                                                         shared_from_this()))
                .releaseToRuntime();
    }
}
/////////////////

int KeeperRegistry::unregisterKeeperProcess(KeeperIdCard const& keeper_id_card)
{

    std::lock_guard<std::mutex> lock(registryLock);
    //check again after the lock is acquired

    auto group_iter = recordingGroups.find(keeper_id_card.getGroupId());
    if(group_iter == recordingGroups.end())
    {
        return chronolog::CL_SUCCESS;
    }

    RecordingGroup& recording_group = (*group_iter).second;

    auto keeper_process_iter =
            recording_group.keeperProcesses.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());

    if(keeper_process_iter == recording_group.keeperProcesses.end())
    {
        //we don't have a record of this keeper, we have nothing to do
        return chronolog::CL_SUCCESS;
    }
    else
    {
        // check if the group is active and the keeper we are about to unregister is the only one this group has
        // and the group needs to be removed from the active group rotation
        if(recording_group.isActive() && (*keeper_process_iter).second.active && recording_group.activeKeeperCount == 1)
        {
            remove_active_group(activeGroups, &recording_group);
        }

        // we mark the keeperProcessEntry as inactive and set the time it would be safe to delete.
        // we delay the destruction of the keeperEntry & keeperAdminClient by 5 secs
        // to prevent the case of deleting the keeperAdminClient while it might be waiting for rpc response on the
        // other thread

        std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));
        recording_group.startDelayedKeeperExit((*keeper_process_iter).second, delayedExitTime);
    }

    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has {} keepers",
             recording_group.groupId,
             recording_group.keeperProcesses.size());

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ",
             activeGroups.size(),
             recordingGroups.size());

    // update registryState if needed
    if(!is_shutting_down() && activeGroups.size() == 0)
    {
        registryState = INITIALIZED;
    }

    return chronolog::CL_SUCCESS;
}
/////////////////

void KeeperRegistry::updateKeeperProcessStats(KeeperStatsMsg const& keeperStatsMsg)
{
    // NOTE: we don't lock registryLock while updating the keeperProcess stats
    // delayed destruction of keeperProcessEntry protects us from the case when
    // stats message is received from the KeeperProcess that has unregistered
    // on the other thread
    if(is_shutting_down())
    {
        return;
    }

    KeeperIdCard keeper_id_card = keeperStatsMsg.getKeeperIdCard();

    LOG_DEBUG("[ChronoProcessRegistry] Received KeeperStatsMsg from {}", chl::to_string(keeper_id_card));

    auto group_iter = recordingGroups.find(keeper_id_card.getGroupId());
    if(group_iter == recordingGroups.end())
    {
        return;
    }

    auto keeper_process_iter =
            (*group_iter).second.keeperProcesses.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());
    if(keeper_process_iter == (*group_iter).second.keeperProcesses.end() || !((*keeper_process_iter).second.active))
    { // however unlikely it is that the stats msg would be delivered for the keeper that's already unregistered
        // we should probably log a warning here...
        return;
    }
    KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
    keeper_process.lastStatsTime = std::chrono::steady_clock::now().time_since_epoch().count();
    keeper_process.activeStoryCount = keeperStatsMsg.getActiveStoryCount();
}
/////////////////

// NOTE: RecordingGroup methods are not currently protected by lock
// the assumptions is that the caller would use RegistryLock before calling the RecordingGroup method
// we may decide to revisit this and introduce RecordingGroup level locks later on..

std::vector<KeeperIdCard>& RecordingGroup::getActiveKeepers(std::vector<KeeperIdCard>& keeper_id_cards)
{
    // NOTE: RecordingGroup methods are not currently protected by lock
    // the assumptions is that the caller would use RegistryLock before calling the RecordingGroup
    // method
    // we may decide to revisit this and introduce RecordingGroup level locks later on..

    keeper_id_cards.clear();

    std::time_t current_time = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());

    for(auto iter = keeperProcesses.begin(); iter != keeperProcesses.end();)
    {
        // first check if there are any delayedExit DataStoreClients to be deleted
        while(!(*iter).second.delayedExitClients.empty() &&
              (current_time >= (*iter).second.delayedExitClients.front().first))
        {
            auto dataStoreClientPair = (*iter).second.delayedExitClients.front();
            LOG_INFO("[ChronoProcessRegistry] getActiveKeepers() destroys dataAdminClient for keeper {} "
                     "current_time={} delayedExitTime={}",
                     (*iter).second.idCardString,
                     ctime(&current_time),
                     ctime(&(*iter).second.delayedExitClients.front().first));
            if(dataStoreClientPair.second != nullptr)
            {
                dataStoreClientPair.second.reset();
            }
            (*iter).second.delayedExitClients.pop_front();
        }

        if((*iter).second.active)
        {
            // active keeper process is ready for story recording
            keeper_id_cards.push_back((*iter).second.idCard);
            ++iter;
        }
        else if((*iter).second.delayedExitClients.empty())
        {
            // it's safe to erase the entry for unregistered keeper process
            LOG_INFO("[ChronoProcessRegistry] getActiveKeepers() destroys keeperProcessEntry for keeper {} "
                     "current_time={}",
                     (*iter).second.idCardString,
                     std::ctime(&current_time));
            iter = keeperProcesses.erase(iter);
        }
        else
        {
            LOG_INFO("ChronoProcessRegistry] getActiveKeepers still keeps keeperProcessEntry for keeper {} "
                     "current_time={}",
                     (*iter).second.idCardString,
                     std::ctime(&current_time));
            ++iter;
        }
    }

    return keeper_id_cards;
}
/////////////////

namespace
{
using namespace std::chrono_literals;
std::unique_lock<std::mutex> boundedRegistryLock(std::mutex& mutex, rpc::RpcDeadline const& deadline)
{
    std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
    while(!deadline.expired())
    {
        if(lock.try_lock())
            return lock;
        std::this_thread::sleep_for(std::min(deadline.remaining(), rpc::RpcDeadline::Clock::duration(1ms)));
    }
    return lock;
}
bool complete(std::shared_ptr<AdminCompletion> const& c) { return c->done.load(std::memory_order_acquire); }
bool success(std::shared_ptr<AdminCompletion> const& c)
{
    return complete(c) && c->confirmed && c->status == CL_SUCCESS;
}
void pause(rpc::RpcDeadline const& d)
{
    std::this_thread::sleep_for(std::min(d.remaining(), rpc::RpcDeadline::Clock::duration(1ms)));
}
// Settles a recording an earlier acquire left Unresolved (see
// settleUnresolvedStart). Returns Pending while a start's work is still out;
// otherwise the recording is Active, or Closing with the stops still to send,
// or Stop with no targets, which the caller erases. Called under the registry
// lock.
UnresolvedStart::State settleUnresolved(StoryRecording& recording)
{
    auto settled = settleUnresolvedStart(recording);
    if(settled.state == UnresolvedStart::Active)
    {
        recording.confirmedKeepers = std::move(settled.keepers);
        recording.phase = StoryRecording::Active;
        LOG_INFO("[ChronoProcessRegistry] StoryID={} left unresolved by an earlier acquire: every start was "
                 "confirmed late, the story is active",
                 recording.id);
    }
    else if(settled.state == UnresolvedStart::Stop && !settled.targets.empty())
    {
        recording.stopTargets = std::move(settled.targets);
        recording.phase = StoryRecording::Closing;
        LOG_INFO("[ChronoProcessRegistry] StoryID={} left unresolved by an earlier acquire: sending a stop to the {} "
                 "component(s) whose start may have run",
                 recording.id,
                 recording.stopTargets.size());
    }
    return settled.state;
}
// How long an acquire waits for the other keepers of the group once the
// grapher and a first keeper have confirmed the start. Keepers answer the
// same start within milliseconds of each other; without the wait the roster
// misses a keeper whose answer came a moment after the first one.
constexpr auto lateKeeperWait = std::chrono::milliseconds(200);
} // namespace

int KeeperRegistry::notifyRecordingGroupOfStoryRecordingStart(ChronicleName const& chronicle,
                                                              StoryName const& story,
                                                              StoryId const& id,
                                                              std::vector<KeeperIdCard>& output,
                                                              ServiceId& player,
                                                              rpc::RpcDeadline const* supplied,
                                                              bool* locally_rejected_before_setup)
{
    if(locally_rejected_before_setup)
        *locally_rejected_before_setup = false;
    auto rejectBeforeSetup = [&](int status)
    {
        if(locally_rejected_before_setup)
            *locally_rejected_before_setup = true;
        return status;
    };
    auto deadline =
            supplied ? *supplied : rpc::RpcDeadline::after(std::chrono::milliseconds(adminRuntime->policy.control_ms));
    output.clear();
    std::shared_ptr<StoryRecording> recording;
    std::vector<rpc::RpcReservation> reservations;
    bool resent = false;
    for(;;)
    {
        std::shared_ptr<StoryRecording> closing;
        {
            auto lock = boundedRegistryLock(registryLock, deadline);
            if(!lock.owns_lock())
                return rejectBeforeSetup(CL_ERR_DEADLINE_EXCEEDED);
            // A destroy of the chronicle or the story is in flight (the
            // portal finished any unconfirmed one first): nothing is sent, so
            // the acquisition is rolled back.
            if(chronicleDestructions.count(chronicle))
                return rejectBeforeSetup(CL_ERR_OUTCOME_UNCONFIRMED);
            auto existing = activeStories.find(id);
            if(existing == activeStories.end())
                break;
            if(existing->second->destroy)
                return rejectBeforeSetup(CL_ERR_OUTCOME_UNCONFIRMED);
            if(existing->second->phase == StoryRecording::Unresolved)
            {
                auto settled = settleUnresolved(*existing->second);
                if(settled == UnresolvedStart::Pending)
                    return CL_ERR_OUTCOME_UNCONFIRMED;
                if(existing->second->phase == StoryRecording::Unresolved)
                {
                    // Nothing may have run anywhere: start the story anew.
                    activeStories.erase(existing);
                    break;
                }
            }
            if(existing->second->phase == StoryRecording::Active)
            {
                output = existing->second->confirmedKeepers;
                player = existing->second->player;
                return CL_SUCCESS;
            }
            // A release whose stop one component did not confirm left the
            // recording Closing. Send that stop again; a fresh recording starts
            // once every component confirmed it.
            if(existing->second->phase != StoryRecording::Closing || existing->second->stopTargets.empty())
                return CL_ERR_OUTCOME_UNCONFIRMED;
            closing = existing->second;
        }
        if(resendStops(closing, deadline) != CL_SUCCESS)
            return CL_ERR_OUTCOME_UNCONFIRMED;
        resent = true;
    }
    for(;;)
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return rejectBeforeSetup(CL_ERR_DEADLINE_EXCEEDED);
        if(chronicleDestructions.count(chronicle))
            return rejectBeforeSetup(CL_ERR_OUTCOME_UNCONFIRMED);
        // Another acquire may have started the story since the check above.
        if(auto again = activeStories.find(id); again != activeStories.end())
        {
            if(again->second->destroy)
                return rejectBeforeSetup(CL_ERR_OUTCOME_UNCONFIRMED);
            if(again->second->phase != StoryRecording::Active)
                return CL_ERR_OUTCOME_UNCONFIRMED;
            output = again->second->confirmedKeepers;
            player = again->second->player;
            return CL_SUCCESS;
        }
        if(!is_running())
            return rejectBeforeSetup(CL_ERR_NO_KEEPERS);
        auto* group = select_active_recording_group(id,
                                                    recordingGroups,
                                                    [](RecordingGroup const& g) {
                                                        return g.grapherProcess && g.grapherProcess->active &&
                                                               g.activeKeeperCount;
                                                    });
        if(!group)
            return rejectBeforeSetup(CL_ERR_NO_KEEPERS);
        // Free the admin clients and entries of keepers that left once their
        // delayed exit has passed. A recording that still needs a client for
        // its stop holds its own reference.
        std::vector<KeeperIdCard> stillActive;
        group->getActiveKeepers(stillActive);
        recording = std::make_shared<StoryRecording>();
        recording->id = id;
        recording->chronicle = chronicle;
        recording->story = story;
        recording->generation = ++nextGeneration;
        recording->group = group->groupId;
        recording->clients.push_back(group->grapherProcess->adminClient);
        for(auto const& item: group->keeperProcesses)
            if(item.second.active && item.second.keeperAdminClient)
            {
                recording->keepers.push_back(item.second.idCard);
                recording->clients.push_back(item.second.keeperAdminClient);
            }
        if(group->playerProcess && group->playerProcess->active && group->playerProcess->adminClient)
        {
            recording->clients.push_back(group->playerProcess->adminClient);
            recording->player = group->playerProcess->idCard.getPlaybackServiceId();
        }
        // All children, including the dependent Player, are admitted before any
        // forward. A portal coordinator occupies its own separately counted slot.
        bool retry = false;
        for(std::size_t i = 0; i < recording->clients.size(); ++i)
        {
            auto token = adminRuntime->runtime.reserve(deadline);
            if(!token)
            {
                // The stops just sent again return their slots a moment
                // after their results; wait for them, within this acquire's
                // deadline, instead of refusing a story that is free now.
                if(!resent || token.status() != CL_ERR_RPC_CAPACITY || deadline.expired())
                {
                    static RefusalLog refusals;
                    if(token.status() == CL_ERR_RPC_CAPACITY)
                        if(auto refused = refusals.note())
                            LOG_WARNING("[ChronoProcessRegistry] Refused {} story start(s) since the last such "
                                        "warning, the latest StoryID={}: it needs {} slots at once (the client's "
                                        "request and {} starts) and had reserved {} of the starts when the "
                                        "visor's {} slots (rpc_slots) ran out",
                                        refused,
                                        id,
                                        recording->clients.size() + 1,
                                        recording->clients.size(),
                                        i,
                                        adminRuntime->runtime.capacity());
                    return rejectBeforeSetup(token.status());
                }
                retry = true;
                break;
            }
            reservations.push_back(std::move(token));
            recording->starts.push_back(std::make_shared<AdminCompletion>());
        }
        if(retry)
        {
            reservations.clear();
            recording.reset();
            lock.unlock();
            pause(deadline);
            continue;
        }
        recording->originalStarts = recording->starts.size();
        activeStories.emplace(id, recording);
        break;
    }
    std::vector<rpc::RpcPending<int>> pending;
    uint64_t start = std::chrono::system_clock::now().time_since_epoch().count();
    for(std::size_t i = 0; i <= recording->keepers.size(); ++i)
    {
        AdminArguments args{AdminAction::Start, chronicle, story, id, start, {}};
        pending.push_back(adminRuntime->runtime.submit<int>(std::move(reservations[i]),
                                                            std::make_shared<AdminWork>(recording->clients[i],
                                                                                        std::move(args),
                                                                                        recording->starts[i],
                                                                                        shared_from_this())));
    }
    // Completion flags expose only private owned scalar state. Do not wait on
    // silent A/B before noticing responsive C or starting the reserved Player.
    bool playerSubmitted = false;
    int status = CL_ERR_OUTCOME_UNCONFIRMED;
    // Set when the grapher and a first keeper have confirmed: the roster is
    // fixed when every keeper has answered or at this time, whichever comes
    // first. The wait takes at most half the time left, so the player start
    // still has the rest of the budget.
    std::optional<rpc::RpcDeadline::Clock::time_point> rosterCloses;
    while(!deadline.expired())
    {
        if(complete(recording->starts[0]) && !success(recording->starts[0]))
        {
            status = recording->starts[0]->status;
            break;
        }
        std::vector<KeeperIdCard> confirmed;
        bool keepersDone = true;
        for(std::size_t i = 0; i < recording->keepers.size(); ++i)
        {
            keepersDone &= complete(recording->starts[i + 1]);
            if(success(recording->starts[i + 1]))
                confirmed.push_back(recording->keepers[i]);
        }
        bool rosterClosed = false;
        if(success(recording->starts[0]) && !confirmed.empty() && !playerSubmitted)
        {
            auto now = rpc::RpcDeadline::Clock::now();
            if(!rosterCloses)
                rosterCloses =
                        now + std::min<rpc::RpcDeadline::Clock::duration>(lateKeeperWait, deadline.remaining() / 2);
            rosterClosed = keepersDone || now >= *rosterCloses;
        }
        if(rosterClosed)
        {
            recording->confirmedKeepers = std::move(confirmed);
            if(recording->clients.size() == recording->keepers.size() + 1)
            {
                status = CL_SUCCESS;
                break;
            }
            std::vector<ServiceId> services;
            for(auto const& k: recording->confirmedKeepers) services.push_back(k.getRecordingServiceId());
            auto i = recording->clients.size() - 1;
            AdminArguments args{AdminAction::StartPlayer, chronicle, story, id, start, std::move(services)};
            pending.push_back(adminRuntime->runtime.submit<int>(std::move(reservations[i]),
                                                                std::make_shared<AdminWork>(recording->clients[i],
                                                                                            std::move(args),
                                                                                            recording->starts[i],
                                                                                            shared_from_this())));
            playerSubmitted = true;
        }
        if(playerSubmitted && complete(recording->starts.back()))
        {
            status = recording->starts.back()->status;
            break;
        }
        if(!playerSubmitted && keepersDone && confirmed.empty())
        {
            status = CL_ERR_NO_KEEPERS;
            break;
        }
        pause(deadline);
    }
    // A player start that was never submitted is known not to have run; mark
    // it so a later acquire or release can settle the recording.
    if(!playerSubmitted && recording->clients.size() > recording->keepers.size() + 1)
    {
        auto& unsent = *recording->starts.back();
        unsent.status = CL_ERR_DEADLINE_EXCEEDED;
        unsent.done.store(true, std::memory_order_release);
    }
    {
        // Taken whatever the deadline: the wait above usually ends because
        // the deadline passed, and a recording left Starting would read as
        // an acquire still in progress to every later acquire and release.
        // The registry lock is only ever held briefly.
        std::lock_guard<std::mutex> lock(registryLock);
        // No completion callback erases or remaps a generation. Failed or
        // ambiguous setup stays Unresolved until a later acquire or release
        // settles it (settleUnresolved); timeout is not a stop instruction.
        recording->phase = status == CL_SUCCESS ? StoryRecording::Active : StoryRecording::Unresolved;
        if(status == CL_SUCCESS)
        {
            output = recording->confirmedKeepers;
            player = recording->player;
        }
    }
    return status;
}

int KeeperRegistry::notifyRecordingGroupOfStoryRecordingStop(StoryId const& id, rpc::RpcDeadline const* supplied)
{
    auto deadline =
            supplied ? *supplied : rpc::RpcDeadline::after(std::chrono::milliseconds(adminRuntime->policy.control_ms));
    std::shared_ptr<StoryRecording> recording;
    std::vector<std::size_t> targets;
    for(;;)
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_DEADLINE_EXCEEDED;
        auto it = activeStories.find(id);
        if(it == activeStories.end())
            return CL_SUCCESS;
        recording = it->second;
        if(recording->phase == StoryRecording::Unresolved)
        {
            if(settleUnresolved(*recording) == UnresolvedStart::Pending)
                return CL_ERR_OUTCOME_UNCONFIRMED;
            if(recording->phase == StoryRecording::Unresolved)
            {
                activeStories.erase(it);
                return CL_SUCCESS;
            }
        }
        if(recording->phase == StoryRecording::Closing && !recording->stopTargets.empty())
        {
            // An earlier stop was not confirmed by every component: send it
            // again rather than leave the story closed.
            lock.unlock();
            return resendStops(recording, deadline);
        }
        if(recording->phase != StoryRecording::Active)
            return CL_ERR_OUTCOME_UNCONFIRMED;
        // A late start must settle before the stop, so the stop reaches each
        // keeper after its start, and no new generation can start until every
        // stop confirms. A first-acquire start that got no answer gets a stop
        // too (see storyStopTargets).
        auto stop = storyStopTargets(*recording);
        if(stop.state == StoryStopTargets::Pending)
        {
            lock.unlock();
            if(deadline.expired())
                return CL_ERR_OUTCOME_UNCONFIRMED;
            pause(deadline);
            continue;
        }
        targets = std::move(stop.targets);
        recording->stopTargets = targets;
        recording->phase = StoryRecording::Closing;
        break;
    }
    return resendStops(recording, deadline);
}

int KeeperRegistry::resendStops(std::shared_ptr<StoryRecording> const& recording, rpc::RpcDeadline const& deadline)
{
    std::vector<rpc::RpcReservation> reservations;
    std::vector<std::shared_ptr<DataStoreAdminClient>> clients;
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_OUTCOME_UNCONFIRMED;
        for(auto target: recording->stopTargets)
        {
            auto token = adminRuntime->runtime.reserve(deadline);
            if(!token)
                return token.status();
            reservations.push_back(std::move(token));
            clients.push_back(recording->clients[target]);
        }
    }
    std::vector<rpc::RpcPending<int>> pending;
    for(std::size_t i = 0; i < clients.size(); ++i)
        pending.push_back(adminRuntime->runtime.submit<int>(
                std::move(reservations[i]),
                std::make_shared<AdminWork>(clients[i],
                                            AdminArguments{AdminAction::Stop, "", "", recording->id, 0, {}},
                                            std::make_shared<AdminCompletion>(),
                                            shared_from_this())));
    int status = CL_SUCCESS;
    for(auto& p: pending)
    {
        auto r = p.wait();
        if(!r.reply || *r.reply != CL_SUCCESS)
            status = r.reply ? *r.reply : r.status;
    }
    if(status != CL_SUCCESS)
    {
        LOG_WARNING("[ChronoProcessRegistry] Stop of StoryID={} not confirmed by every component ({}); it is sent "
                    "again on the story's next acquire or release",
                    recording->id,
                    status);
        return status;
    }
    auto lock = boundedRegistryLock(registryLock, deadline);
    if(!lock.owns_lock())
        return CL_ERR_OUTCOME_UNCONFIRMED;
    auto it = activeStories.find(recording->id);
    if(it != activeStories.end() && it->second == recording)
        activeStories.erase(it);
    return CL_SUCCESS;
}

int KeeperRegistry::notifyAllGraphersOfStoryDestruction(ChronicleName const& chronicle,
                                                        StoryName const& story,
                                                        StoryId const& id,
                                                        rpc::RpcDeadline const* supplied)
{
    auto deadline =
            supplied ? *supplied : rpc::RpcDeadline::after(std::chrono::milliseconds(adminRuntime->policy.control_ms));
    std::vector<std::shared_ptr<DataStoreAdminClient>> clients;
    std::vector<rpc::RpcReservation> reservations;
    std::shared_ptr<StoryRecording> barrier;
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_DEADLINE_EXCEEDED;
        // A recording, or a destroy in flight, refuses; a destroy that ended
        // unconfirmed is sent again with the same placeholder.
        auto existing = activeStories.find(id);
        if(existing != activeStories.end() && (!existing->second->destroy || existing->second->destroyInFlight))
            return CL_ERR_OUTCOME_UNCONFIRMED;
        for(auto const& group: recordingGroups)
            if(group.second.grapherProcess && group.second.grapherProcess->active)
                clients.push_back(group.second.grapherProcess->adminClient);
        for(std::size_t i = 0; i < clients.size(); ++i)
        {
            auto t = adminRuntime->runtime.reserve(deadline);
            if(!t)
                return t.status();
            reservations.push_back(std::move(t));
        }
        if(existing != activeStories.end())
            barrier = existing->second;
        else
        {
            barrier = std::make_shared<StoryRecording>();
            barrier->id = id;
            barrier->chronicle = chronicle;
            barrier->story = story;
            barrier->phase = StoryRecording::Closing;
            barrier->generation = ++nextGeneration;
            barrier->destroy = true;
            activeStories.emplace(id, barrier);
        }
        barrier->destroyInFlight = true;
    }
    std::vector<rpc::RpcPending<int>> pending;
    for(std::size_t i = 0; i < clients.size(); ++i)
        pending.push_back(adminRuntime->runtime.submit<int>(
                std::move(reservations[i]),
                std::make_shared<AdminWork>(clients[i],
                                            AdminArguments{AdminAction::DestroyStory, chronicle, story, id, 0, {}},
                                            std::make_shared<AdminCompletion>(),
                                            shared_from_this())));
    // Once sent, a destroy that any grapher did not confirm stays pending and
    // is sent again later, so the caller is never told it failed: a grapher
    // may have deleted its files, and the next create or acquire of the name
    // finishes it.
    int status = CL_SUCCESS;
    for(auto& p: pending)
    {
        auto r = p.wait();
        if(!r.reply || *r.reply != CL_SUCCESS)
            status = CL_ERR_OUTCOME_UNCONFIRMED;
    }
    {
        // Taken whatever the deadline, as in the start path: the placeholder
        // must not be left marked in flight. Held only briefly.
        std::lock_guard<std::mutex> lock(registryLock);
        barrier->destroyInFlight = false;
        auto it = activeStories.find(id);
        if(status == CL_SUCCESS && it != activeStories.end() && it->second == barrier)
            activeStories.erase(it);
    }
    return status;
}
int KeeperRegistry::notifyAllGraphersOfChronicleDestruction(ChronicleName const& chronicle,
                                                            rpc::RpcDeadline const* supplied)
{
    auto deadline =
            supplied ? *supplied : rpc::RpcDeadline::after(std::chrono::milliseconds(adminRuntime->policy.control_ms));
    std::vector<std::shared_ptr<DataStoreAdminClient>> clients;
    std::vector<rpc::RpcReservation> reservations;
    std::vector<std::shared_ptr<StoryRecording>> covered;
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_DEADLINE_EXCEEDED;
        // A destroy in flight refuses; one that ended unconfirmed is sent
        // again. It also covers the placeholders of unconfirmed story
        // destroys in the chronicle; a recording there refuses.
        if(auto marker = chronicleDestructions.find(chronicle); marker != chronicleDestructions.end() && marker->second)
            return CL_ERR_OUTCOME_UNCONFIRMED;
        for(auto const& item: activeStories)
            if(item.second->chronicle == chronicle)
            {
                if(!item.second->destroy || item.second->destroyInFlight)
                    return CL_ERR_OUTCOME_UNCONFIRMED;
                covered.push_back(item.second);
            }
        for(auto const& group: recordingGroups)
            if(group.second.grapherProcess && group.second.grapherProcess->active)
                clients.push_back(group.second.grapherProcess->adminClient);
        for(std::size_t i = 0; i < clients.size(); ++i)
        {
            auto t = adminRuntime->runtime.reserve(deadline);
            if(!t)
                return t.status();
            reservations.push_back(std::move(t));
        }
        chronicleDestructions[chronicle] = true;
        for(auto const& c: covered) c->destroyInFlight = true;
    }
    std::vector<rpc::RpcPending<int>> pending;
    for(std::size_t i = 0; i < clients.size(); ++i)
        pending.push_back(adminRuntime->runtime.submit<int>(
                std::move(reservations[i]),
                std::make_shared<AdminWork>(clients[i],
                                            AdminArguments{AdminAction::DestroyChronicle, chronicle, "", 0, 0, {}},
                                            std::make_shared<AdminCompletion>(),
                                            shared_from_this())));
    // Once sent, a destroy that any grapher did not confirm stays pending and
    // is sent again later, so the caller is never told it failed: a grapher
    // may have deleted its files, and the next create or acquire of the name
    // finishes it.
    int status = CL_SUCCESS;
    for(auto& p: pending)
    {
        auto r = p.wait();
        if(!r.reply || *r.reply != CL_SUCCESS)
            status = CL_ERR_OUTCOME_UNCONFIRMED;
    }
    {
        std::lock_guard<std::mutex> lock(registryLock);
        for(auto const& c: covered)
        {
            c->destroyInFlight = false;
            auto it = activeStories.find(c->id);
            if(status == CL_SUCCESS && it != activeStories.end() && it->second == c)
                activeStories.erase(it);
        }
        if(status == CL_SUCCESS)
            chronicleDestructions.erase(chronicle);
        else
            chronicleDestructions[chronicle] = false;
    }
    return status;
}

int KeeperRegistry::finishUnconfirmedDestruction(ChronicleName const& chronicle,
                                                 StoryName const& story,
                                                 rpc::RpcDeadline const& deadline,
                                                 bool& chronicleDestroyed,
                                                 bool& storyDestroyed)
{
    chronicleDestroyed = storyDestroyed = false;
    bool chroniclePending = false;
    std::optional<StoryId> storyPending;
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_DEADLINE_EXCEEDED;
        if(auto marker = chronicleDestructions.find(chronicle); marker != chronicleDestructions.end())
        {
            if(marker->second)
                return CL_ERR_OUTCOME_UNCONFIRMED; // in flight
            chroniclePending = true;
        }
        else if(!story.empty())
            for(auto const& item: activeStories)
                if(item.second->destroy && item.second->chronicle == chronicle && item.second->story == story)
                {
                    if(item.second->destroyInFlight)
                        return CL_ERR_OUTCOME_UNCONFIRMED;
                    storyPending = item.first;
                }
    }
    if(!chroniclePending && !storyPending)
        return CL_SUCCESS;
    LOG_INFO("[ChronoProcessRegistry] Sending again the destroy of {} {} that a grapher did not confirm",
             chroniclePending ? "Chronicle" : "Story",
             chroniclePending ? chronicle : chronicle + "/" + story);
    int status = chroniclePending ? notifyAllGraphersOfChronicleDestruction(chronicle, &deadline)
                                  : notifyAllGraphersOfStoryDestruction(chronicle, story, *storyPending, &deadline);
    if(status != CL_SUCCESS)
        return CL_ERR_OUTCOME_UNCONFIRMED;
    (chroniclePending ? chronicleDestroyed : storyDestroyed) = true;
    return CL_SUCCESS;
}

int KeeperRegistry::registerGrapherProcess(GrapherRegistrationMsg const& reg_msg)
{
    if(is_shutting_down())
    {
        return chronolog::CL_ERR_UNKNOWN;
    }

    GrapherIdCard grapher_id_card = reg_msg.getGrapherIdCard();
    RecordingGroupId group_id = grapher_id_card.getGroupId();
    ServiceId admin_service_id = reg_msg.getAdminServiceId();

    std::lock_guard<std::mutex> lock(registryLock);
    //re-check state after ther lock is aquired
    if(is_shutting_down())
    {
        return chronolog::CL_ERR_UNKNOWN;
    }

    //find the group that grapher belongs to in the registry
    auto group_iter = recordingGroups.find(group_id);
    if(group_iter == recordingGroups.end())
    {
        auto insert_return =
                recordingGroups.insert(std::pair<RecordingGroupId, RecordingGroup>(group_id, RecordingGroup(group_id)));
        if(false == insert_return.second)
        {
            LOG_ERROR("[ChronoProcessRegistry] keeper registration failed to find RecordingGroup {}", group_id);
            return chronolog::CL_ERR_UNKNOWN;
        }
        else
        {
            group_iter = insert_return.first;
        }
    }

    RecordingGroup& recording_group = ((*group_iter).second);

    // it is possible that the Registry still retains the record of the previous re-incarnation of the grapher process
    // check for this case and clean up the leftover record...

    std::stringstream id_string;
    id_string << grapher_id_card;
    if(recording_group.grapherProcess != nullptr)
    {
        // start delayed destruction for the lingering Adminclient to be safe...

        auto grapher_process = recording_group.grapherProcess;
        if(grapher_process->active)
        {
            // start delayed destruction for the lingering Adminclient to be safe...

            std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                    std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));

            recording_group.startDelayedGrapherExit(*grapher_process, delayedExitTime);
        }
        else
        {
            //check if any existing delayed exit grapher processes can be cleared...
            std::time_t current_time =
                    std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
            recording_group.clearDelayedExitGrapher(*grapher_process, current_time);
        }

        if(grapher_process->delayedExitGrapherClients.empty())
        {
            LOG_INFO("[ChronoProcessRegistry] registerGrapherProcess has destroyed old entry for grapher {}",
                     chl::to_string(grapher_id_card));
            grapher_process.reset();
            recording_group.grapherProcess = nullptr;
        }
        else
        {
            LOG_INFO("[ChronoProcessRegistry] registration for Grapher{} cant's proceed as previous grapherClient "
                     "isn't yet "
                     "dismantled",
                     chl::to_string(grapher_id_card));
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    //create a client of the new grapher's DataStoreAdminService listenning at adminServiceId

    std::string service_na_string;
    admin_service_id.get_service_as_string(service_na_string);

    auto collectionClient = DataStoreAdminClient::CreateDataStoreAdminClient(adminRuntime,
                                                                             service_na_string,
                                                                             admin_service_id.getProviderId());
    if(nullptr == collectionClient)
    {
        LOG_ERROR("[ChronoProcessRegistry] Register Grapher {} failed to create DataStoreAdminClient for {}: "
                  "provider_id={}",
                  chl::to_string(grapher_id_card),
                  chl::to_string(admin_service_id));
        return chronolog::CL_ERR_UNKNOWN;
    }

    //now create a new GrapherProcessEntry with the new DataAdminclient
    recording_group.grapherProcess = std::make_shared<GrapherProcessEntry>(grapher_id_card, admin_service_id);
    recording_group.grapherProcess->adminClient = collectionClient;
    recording_group.grapherProcess->active = true;

    LOG_INFO("[ChronoProcessRegistry] Register grapher {} created DataStoreAdminClient for {}",
             chl::to_string(grapher_id_card),
             chl::to_string(admin_service_id));

    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has a grappher and  {} keepers",
             recording_group.groupId,
             recording_group.keeperProcesses.size());

    // now that communnication with the Grapher is established and we are still holding registryLock
    // check if the group is ready for active group rotation
    if(recording_group.isActive())
    {
        add_active_group(activeGroups, &recording_group);
    }

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ",
             activeGroups.size(),
             recordingGroups.size());
    // we still holding registryLock
    // update registryState if needed
    if(activeGroups.size() > 0)
    {
        registryState = RUNNING;
    }
    return chronolog::CL_SUCCESS;
}
/////////////////

int KeeperRegistry::unregisterGrapherProcess(GrapherIdCard const& grapher_id_card)
{
    std::lock_guard<std::mutex> lock(registryLock);
    //check again after the lock is acquired

    auto group_iter = recordingGroups.find(grapher_id_card.getGroupId());
    if(group_iter == recordingGroups.end())
    {
        return chronolog::CL_SUCCESS;
    }

    RecordingGroup& recording_group = ((*group_iter).second);

    // we are about to unregister the grapher so the group can't perform recording duties
    // if it were an active recordingGroup before remove it from rotation
    if(recording_group.isActive())
    {
        //INNA: what do we do with any active Stories that this group were recordng? force release them and notify clients?
        // wait for the new grapher?
        remove_active_group(activeGroups, &recording_group);
        LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} is not active; activeGroups.size{}",
                 recording_group.groupId,
                 activeGroups.size());
    }

    if(recording_group.grapherProcess != nullptr && recording_group.grapherProcess->active)
    {
        // start delayed destruction for the lingering Adminclient to be safe...
        // to prevent the case of deleting the keeperAdminClient while it might be waiting for rpc response on the
        // other thread

        std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));
        LOG_INFO("[ChronoProcessRegistry] starting delayedExit for grapher {} delayedExitTime={}",
                 recording_group.grapherProcess->idCardString,
                 std::ctime(&delayedExitTime));

        recording_group.startDelayedGrapherExit(*(recording_group.grapherProcess), delayedExitTime);
    }

    // now that we are still holding registryLock
    // update registryState if needed
    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has no grapher and  {} keepers",
             recording_group.groupId,
             recording_group.keeperProcesses.size());

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ",
             activeGroups.size(),
             recordingGroups.size());

    // update registryState in case this was the last active recordingGroup
    if(!is_shutting_down() && activeGroups.size() == 0)
    {
        registryState = INITIALIZED;
    }

    return chronolog::CL_SUCCESS;
}

} //namespace chronolog

void chl::KeeperRegistry::updateGrapherProcessStats(chl::GrapherStatsMsg const& statsMsg)
{
    // NOTE: we don't lock registryLock while updating the keeperProcess stats
    // delayed destruction of keeperProcessEntry protects us from the case when
    // stats message is received from the KeeperProcess that has unregistered
    // on the other thread
    if(is_shutting_down())
    {
        return;
    }

#ifdef DEBUG
    std::string id_string;
    id_string += stats.getGrapherIdCard();
    LOG_DEBUG("[ChronoProcessRegistry] Received GrapherStatsMsg from {}", chl::to_string(stats.getGrapherIdCard()));
#endif

    auto group_iter = recordingGroups.find(statsMsg.getGrapherIdCard().getGroupId());
    if(group_iter == recordingGroups.end())
    {
        return;
    }

    RecordingGroup& recording_group = ((*group_iter).second);
    if(recording_group.grapherProcess != nullptr && recording_group.grapherProcess->active)
    {
        // there's no need to update stats of inactive process
        recording_group.grapherProcess->lastStatsTime = std::chrono::steady_clock::now().time_since_epoch().count();
        recording_group.grapherProcess->activeStoryCount = statsMsg.getActiveStoryCount();
    }
}

/////////////////

int chl::KeeperRegistry::registerPlayerProcess(chl::PlayerRegistrationMsg const& reg_msg)
{
    if(is_shutting_down())
    {
        return chronolog::CL_ERR_UNKNOWN;
    }

    chl::PlayerIdCard id_card = reg_msg.getPlayerIdCard();
    chl::RecordingGroupId group_id = id_card.getGroupId();
    chl::ServiceId admin_service_id = reg_msg.getAdminServiceId();

    std::lock_guard<std::mutex> lock(registryLock);
    if(is_shutting_down())
    {
        return chronolog::CL_ERR_UNKNOWN;
    }

    //find the recording group that new player process belongs to or create one
    auto group_iter = recordingGroups.find(group_id);
    if(group_iter == recordingGroups.end())
    {
        auto insert_return = recordingGroups.insert(
                std::pair<chl::RecordingGroupId, chl::RecordingGroup>(group_id, chl::RecordingGroup(group_id)));
        if(false == insert_return.second)
        {
            LOG_ERROR("[ChronoProcessRegistry] player registration failed to find RecordingGroup {}", group_id);
            return chronolog::CL_ERR_UNKNOWN;
        }
        else
        {
            group_iter = insert_return.first;
        }
    }

    chl::RecordingGroup& recording_group = ((*group_iter).second);

    // it is possible that the Registry still retains the record of the previous re-incarnation of the player process
    // check for this case and clean up the leftover record...

    if(recording_group.playerProcess != nullptr)
    {
        // start delayed destruction for the lingering Adminclient to be safe...

        auto player_process = recording_group.playerProcess;
        if(player_process->active)
        {
            // start delayed destruction for the lingering Adminclient to be safe...

            std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                    std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));

            recording_group.startDelayedPlayerExit(*player_process, delayedExitTime);
        }
        else
        {
            //check if any existing delayed exit player processes can be cleared...
            std::time_t current_time =
                    std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
            recording_group.clearDelayedExitPlayer(*player_process, current_time);
        }

        if(player_process->delayedExitPlayerClients.empty())
        {
            LOG_INFO("[ChronoProcessRegistry] registerplayerProcess has destroyed old entry for player {}",
                     chl::to_string(id_card));
            player_process.reset();
            recording_group.playerProcess = nullptr;
        }
        else
        {
            LOG_INFO("[ChronoProcessRegistry] registration for Player{} cant's proceed as previous playerClient isn't "
                     "yet "
                     "dismantled",
                     chl::to_string(id_card));

            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    //create a client of the new player's DataStoreAdminService listenning at adminServiceId
    std::string service_na_string;
    admin_service_id.get_service_as_string(service_na_string);

    LOG_DEBUG("[ChronoProcessRegistry] creating AdminClient for service_na_string {}", service_na_string);

    auto collectionClient = chl::DataStoreAdminClient::CreateDataStoreAdminClient(adminRuntime,
                                                                                  service_na_string,
                                                                                  admin_service_id.getProviderId());
    if(nullptr == collectionClient)
    {
        LOG_ERROR("[ChronoProcessRegistry] Register Player {} failed to create DataStoreAdminClient for {}",
                  chl::to_string(id_card),
                  chl::to_string(admin_service_id));
        return chronolog::CL_ERR_UNKNOWN;
    }

    //now create a new GrapherProcessEntry with the new DataAdminclient
    recording_group.playerProcess = std::make_shared<chl::PlayerProcessEntry>(id_card, admin_service_id);
    recording_group.playerProcess->adminClient = collectionClient;
    recording_group.playerProcess->active = true;

    LOG_INFO("[ChronoProcessRegistry] Register Player {} created DataStoreAdminClient for {}",
             chl::to_string(id_card),
             chl::to_string(admin_service_id));

    // now that communnication with the Player is established and we are still holding registryLock
    // check if the group is ready for active group rotation

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ",
             activeGroups.size(),
             recordingGroups.size());
    // we still holding registryLock
    // update registryState if needed
    if(activeGroups.size() > 0)
    {
        registryState = RUNNING;
    }
    return chronolog::CL_SUCCESS;
}
/////////////////

int chl::KeeperRegistry::unregisterPlayerProcess(chl::PlayerIdCard const& id_card)
{
    std::lock_guard<std::mutex> lock(registryLock);

    auto group_iter = recordingGroups.find(id_card.getGroupId());
    if(group_iter == recordingGroups.end())
    {
        return chronolog::CL_SUCCESS;
    }

    RecordingGroup& recording_group = ((*group_iter).second);

    // we are about to unregister the player so the group can't perform playback duties

    // TODO: handle the case by notifying the clients that playback service is not available...


    if(recording_group.playerProcess != nullptr && recording_group.playerProcess->active)
    {
        // start delayed destruction for the lingering Adminclient to be safe...
        // to prevent the case of deleting the AdminClient while it might be waiting for rpc response on the
        // other thread

        std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));
        LOG_INFO("[ChronoProcessRegistry] starting delayedExit for player {} delayedExitTime={}",
                 recording_group.playerProcess->idCardString,
                 std::ctime(&delayedExitTime));

        recording_group.startDelayedPlayerExit(*(recording_group.playerProcess), delayedExitTime);
    }

    // now that we are still holding registryLock
    // update registryState if needed
    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has no chrono_player ", recording_group.groupId);

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ",
             activeGroups.size(),
             recordingGroups.size());

    // update registryState in case this was the last active recordingGroup
    if(!is_shutting_down() && activeGroups.size() == 0)
    {
        registryState = INITIALIZED;
    }

    return chronolog::CL_SUCCESS;
}

void chl::KeeperRegistry::updatePlayerProcessStats(chl::PlayerStatsMsg const& statsMsg)
{
    // NOTE: we don't lock registryLock while updating the keeperProcess stats
    // delayed destruction of keeperProcessEntry protects us from the case when
    // stats message is received from the KeeperProcess that has unregistered
    // on the other thread
    if(is_shutting_down())
    {
        return;
    }

#ifdef DEBUG
    std::string id_string;
    id_string += stats.getPlayerIdCard;
    LOG_DEBUG("[ChronoProcessRegistry] Received PlayerStatsMsg from {}", id_string.str());
#endif

    auto group_iter = recordingGroups.find(statsMsg.getPlayerIdCard().getGroupId());
    if(group_iter == recordingGroups.end())
    {
        return;
    }

    RecordingGroup& recording_group = ((*group_iter).second);
    if(recording_group.playerProcess != nullptr && recording_group.playerProcess->active)
    {
        // there's no need to update stats of inactive process
        recording_group.playerProcess->lastStatsTime = std::chrono::steady_clock::now().time_since_epoch().count();
    }
}


///////////////

bool chl::RecordingGroup::isActive() const
{
    //TODO: we might add a check for time since the last stats message received from
    // the processes listed as active

    if(grapherProcess != nullptr && grapherProcess->active && activeKeeperCount > 0)
    {
        LOG_DEBUG("[ChronoProcessRegistry RecordingGroup {} is active", groupId);
        return true;
    }
    else
    {
        LOG_DEBUG("[ChronoProcessRegistry] RecordingGroup {} is not active", groupId);
        return false;
    }
}

void chl::RecordingGroup::startDelayedGrapherExit(chl::GrapherProcessEntry& grapher_process,
                                                  std::time_t delayedExitTime)
{
    grapher_process.active = false;

    LOG_INFO("[ChronoProcessRegistry] recording_group {} starts delayedExit for {}",
             groupId,
             grapher_process.idCardString);
    if(grapher_process.adminClient != nullptr)
    {
        grapher_process.delayedExitGrapherClients.push_back(
                std::pair<std::time_t, std::shared_ptr<chl::DataStoreAdminClient>>(delayedExitTime,
                                                                                   grapher_process.adminClient));
        grapher_process.adminClient = nullptr;
    }
}

void chl::RecordingGroup::clearDelayedExitGrapher(chl::GrapherProcessEntry& grapher_process, std::time_t current_time)
{
    while(!grapher_process.delayedExitGrapherClients.empty() &&
          (current_time >= grapher_process.delayedExitGrapherClients.front().first))
    {
        LOG_INFO("[ChronoProcessRegistry] recording_Group {}, destroys delayed dataAdmindClient for {}",
                 groupId,
                 grapher_process.idCardString);
        auto dataStoreClientPair = grapher_process.delayedExitGrapherClients.front();
        if(dataStoreClientPair.second != nullptr)
        {
            dataStoreClientPair.second.reset();
        }
        grapher_process.delayedExitGrapherClients.pop_front();
    }
}

void chl::RecordingGroup::startDelayedPlayerExit(chl::PlayerProcessEntry& player_process, std::time_t delayedExitTime)
{
    player_process.active = false;

    LOG_INFO("[ChronoProcessRegistry] recording_group {} starts delayedExit for {}",
             groupId,
             player_process.idCardString);
    if(player_process.adminClient != nullptr)
    {
        player_process.delayedExitPlayerClients.push_back(
                std::pair<std::time_t, std::shared_ptr<chl::DataStoreAdminClient>>(delayedExitTime,
                                                                                   player_process.adminClient));
        player_process.adminClient = nullptr;
    }
}

void chl::RecordingGroup::clearDelayedExitPlayer(chl::PlayerProcessEntry& player_process, std::time_t current_time)
{
    while(!player_process.delayedExitPlayerClients.empty() &&
          (current_time >= player_process.delayedExitPlayerClients.front().first))
    {
        LOG_INFO("[ChronoProcessRegistry] recording_Group {}, destroys delayed dataAdmindClient for {}",
                 groupId,
                 player_process.idCardString);
        auto dataStoreClientPair = player_process.delayedExitPlayerClients.front();
        if(dataStoreClientPair.second != nullptr)
        {
            dataStoreClientPair.second.reset();
        }
        player_process.delayedExitPlayerClients.pop_front();
    }
}


void chl::RecordingGroup::startDelayedKeeperExit(chl::KeeperProcessEntry& keeper_process, std::time_t delayedExitTime)
{
    // we mark the keeperProcessEntry as inactive and set the time it would be safe to delete.
    // we delay the destruction of the keeperEntry & keeperAdminClient by 5 secs
    // to prevent the case of deleting the keeperAdminClient while it might be waiting for rpc response on the
    // other thread

    if(keeper_process.active)
        activeKeeperCount -= 1;
    keeper_process.active = false;

    LOG_INFO("[ChronoProcessRegistry] recording_group {} starts delayedExit for {}",
             groupId,
             keeper_process.idCardString);
    if(keeper_process.keeperAdminClient != nullptr)
    {
        keeper_process.delayedExitClients.push_back(
                std::pair<std::time_t, std::shared_ptr<chl::DataStoreAdminClient>>(delayedExitTime,
                                                                                   keeper_process.keeperAdminClient));
        keeper_process.keeperAdminClient = nullptr;
    }
}

void chl::RecordingGroup::clearDelayedExitKeeper(chl::KeeperProcessEntry& keeper_process, std::time_t current_time)
{
    while(!keeper_process.delayedExitClients.empty() &&
          (current_time >= keeper_process.delayedExitClients.front().first))
    {
        LOG_INFO("[ChronoProcessRegistry] recording_group {} destroys delayed dataAdminClient for {}",
                 groupId,
                 keeper_process.idCardString);
        auto dataStoreClientPair = keeper_process.delayedExitClients.front();
        if(dataStoreClientPair.second != nullptr)
        {
            dataStoreClientPair.second.reset();
        }
        keeper_process.delayedExitClients.pop_front();
    }
}
