#include <iostream>
#include <thread>
#include <algorithm>
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
    return std::shared_ptr<KeeperRegistry>(new KeeperRegistry(policy, 32, {}, actual));
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
    // stories clients still hold. Each start joins its story's recording here,
    // under the lock, so a stop that follows waits for it and also reaches
    // this keeper. A story whose start cannot be admitted is logged and left
    // out; the registration still succeeds, and a durable write for the story
    // starts its pipeline instead.
    std::vector<HeldStart> held;
    auto deadline = rpc::RpcDeadline::after(std::chrono::milliseconds(adminRuntime->policy.control_ms));
    for(auto const& recording: storiesToStartOnRegistration(activeStories, recording_group.groupId))
    {
        auto token = adminRuntime->runtime.reserve(deadline);
        if(!token)
        {
            LOG_WARNING("[ChronoProcessRegistry] No admin slot to start StoryId {} on registering keeper {}: {}",
                        recording->id,
                        to_string(keeper_id_card),
                        token.status());
            continue;
        }
        auto completion = std::make_shared<AdminCompletion>();
        recording->clients.push_back(collectionClient);
        recording->starts.push_back(completion);
        held.push_back({recording, collectionClient, completion, std::move(token)});
    }
    lock.unlock();
    if(!held.empty())
        startHeldStoriesOnKeeper(keeper_id_card, std::move(held));
    return chronolog::CL_SUCCESS;
}

void KeeperRegistry::startHeldStoriesOnKeeper(KeeperIdCard const& keeper_id_card, std::vector<HeldStart> held)
{
    LOG_INFO("[ChronoProcessRegistry] Starting {} held stories on keeper {}", held.size(), to_string(keeper_id_card));
    uint64_t start = std::chrono::system_clock::now().time_since_epoch().count();
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
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return rejectBeforeSetup(CL_ERR_DEADLINE_EXCEEDED);
        if(chronicleDestructions.count(chronicle))
            return CL_ERR_OUTCOME_UNCONFIRMED;
        auto existing = activeStories.find(id);
        if(existing != activeStories.end())
        {
            if(existing->second->phase != StoryRecording::Active)
                return CL_ERR_OUTCOME_UNCONFIRMED;
            output = existing->second->confirmedKeepers;
            player = existing->second->player;
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
        for(std::size_t i = 0; i < recording->clients.size(); ++i)
        {
            auto token = adminRuntime->runtime.reserve(deadline);
            if(!token)
                return rejectBeforeSetup(token.status());
            reservations.push_back(std::move(token));
            recording->starts.push_back(std::make_shared<AdminCompletion>());
        }
        recording->originalStarts = recording->starts.size();
        activeStories.emplace(id, recording);
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
        if(success(recording->starts[0]) && !confirmed.empty() && !playerSubmitted)
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
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_OUTCOME_UNCONFIRMED;
        // No completion callback erases or remaps a generation. Failed or
        // ambiguous setup remains a barrier; timeout is not a stop instruction.
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
    std::vector<rpc::RpcReservation> reservations;
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
        if(recording->phase != StoryRecording::Active)
            return CL_ERR_OUTCOME_UNCONFIRMED;
        // A late start must settle before the stop, so the stop reaches each
        // keeper after its start, and no new generation can start until every
        // stop confirms. An unknown start from the first acquire stays
        // quarantined.
        auto stop = storyStopTargets(*recording);
        if(stop.state == StoryStopTargets::Unconfirmed)
            return CL_ERR_OUTCOME_UNCONFIRMED;
        if(stop.state == StoryStopTargets::Pending)
        {
            lock.unlock();
            if(deadline.expired())
                return CL_ERR_OUTCOME_UNCONFIRMED;
            pause(deadline);
            continue;
        }
        targets = std::move(stop.targets);
        for(std::size_t i = 0; i < targets.size(); ++i)
        {
            auto token = adminRuntime->runtime.reserve(deadline);
            if(!token)
                return token.status();
            reservations.push_back(std::move(token));
        }
        recording->phase = StoryRecording::Closing;
        break;
    }
    std::vector<rpc::RpcPending<int>> pending;
    for(std::size_t i = 0; i < targets.size(); ++i)
    {
        auto c = std::make_shared<AdminCompletion>();
        pending.push_back(adminRuntime->runtime.submit<int>(
                std::move(reservations[i]),
                std::make_shared<AdminWork>(recording->clients[targets[i]],
                                            AdminArguments{AdminAction::Stop, "", "", id, 0, {}},
                                            c,
                                            shared_from_this())));
    }
    int status = CL_SUCCESS;
    for(auto& p: pending)
    {
        auto r = p.wait();
        if(!r.reply || *r.reply != CL_SUCCESS)
            status = r.reply ? *r.reply : r.status;
    }
    if(status == CL_SUCCESS)
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_OUTCOME_UNCONFIRMED;
        auto it = activeStories.find(id);
        if(it != activeStories.end() && it->second == recording)
            activeStories.erase(it);
    }
    return status;
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
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_DEADLINE_EXCEEDED;
        if(activeStories.count(id))
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
        auto barrier = std::make_shared<StoryRecording>();
        barrier->id = id;
        barrier->chronicle = chronicle;
        barrier->story = story;
        barrier->phase = StoryRecording::Closing;
        barrier->generation = ++nextGeneration;
        activeStories.emplace(id, std::move(barrier));
    }
    std::vector<rpc::RpcPending<int>> pending;
    for(std::size_t i = 0; i < clients.size(); ++i)
        pending.push_back(adminRuntime->runtime.submit<int>(
                std::move(reservations[i]),
                std::make_shared<AdminWork>(clients[i],
                                            AdminArguments{AdminAction::DestroyStory, chronicle, story, id, 0, {}},
                                            std::make_shared<AdminCompletion>(),
                                            shared_from_this())));
    int status = CL_SUCCESS;
    for(auto& p: pending)
    {
        auto r = p.wait();
        if(!r.reply || *r.reply != CL_SUCCESS)
            status = r.reply ? *r.reply : r.status;
    }
    if(status == CL_SUCCESS)
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_OUTCOME_UNCONFIRMED;
        activeStories.erase(id);
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
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_DEADLINE_EXCEEDED;
        if(chronicleDestructions.count(chronicle))
            return CL_ERR_OUTCOME_UNCONFIRMED;
        for(auto const& item: activeStories)
            if(item.second->chronicle == chronicle)
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
        chronicleDestructions.emplace(chronicle, true);
    }
    std::vector<rpc::RpcPending<int>> pending;
    for(std::size_t i = 0; i < clients.size(); ++i)
        pending.push_back(adminRuntime->runtime.submit<int>(
                std::move(reservations[i]),
                std::make_shared<AdminWork>(clients[i],
                                            AdminArguments{AdminAction::DestroyChronicle, chronicle, "", 0, 0, {}},
                                            std::make_shared<AdminCompletion>(),
                                            shared_from_this())));
    int status = CL_SUCCESS;
    for(auto& p: pending)
    {
        auto r = p.wait();
        if(!r.reply || *r.reply != CL_SUCCESS)
            status = r.reply ? *r.reply : r.status;
    }
    if(status == CL_SUCCESS)
    {
        auto lock = boundedRegistryLock(registryLock, deadline);
        if(!lock.owns_lock())
            return CL_ERR_OUTCOME_UNCONFIRMED;
        chronicleDestructions.erase(chronicle);
    }
    return status;
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
