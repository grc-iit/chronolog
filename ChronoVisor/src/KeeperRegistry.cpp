#include <iostream>

#include <thallium.hpp>

#include "KeeperRegistry.h"
#include "KeeperRegistryService.h"
#include "DataStoreAdminClient.h"
#include "chrono_monitor.h"
#include "ConfigurationManager.h"
/////////////////////////

namespace tl = thallium;
namespace chl = chronolog;

namespace chronolog
{

int KeeperRegistry::InitializeRegistryService(ChronoLog::ConfigurationManager const &confManager)
{
    int status = chronolog::CL_ERR_UNKNOWN;
    std::lock_guard <std::mutex> lock(registryLock);

    if(registryState != UNKNOWN)
    { return chronolog::CL_SUCCESS; }

    try
    {
        // initialise thalium engine for KeeperRegistryService
        std::string KEEPER_REGISTRY_SERVICE_NA_STRING =
                confManager.VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
                confManager.VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.IP + ":" +
                std::to_string(confManager.VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.BASE_PORT);

        uint16_t provider_id = confManager.VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;
        margo_instance_id margo_id = margo_init(KEEPER_REGISTRY_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 2);

        if(MARGO_INSTANCE_NULL == margo_id)
        {
            LOG_CRITICAL("[ChronoProcessRegistry] Failed to initialize margo_instance");
            return -1;
        }
        LOG_INFO("[ChronoProcessRegistry] margo_instance initialized with NA_STRING {}", KEEPER_REGISTRY_SERVICE_NA_STRING);

        registryEngine = new tl::engine(margo_id);

        std::stringstream ss;
        ss << registryEngine->self();
        LOG_INFO("[ChronoProcessRegistry] Starting RegistryService at {} provider id: {}", ss.str(), provider_id);

        keeperRegistryService = KeeperRegistryService::CreateKeeperRegistryService(*registryEngine, provider_id, *this);

        delayedDataAdminExitSeconds = confManager.VISOR_CONF.DELAYED_DATA_ADMIN_EXIT_IN_SECS;

        // Kun: This protocol will be used to create dataStoreAdminClient for both Keeper and Grapher.
        // Since they share the same Thallium engine, we have to use the same protocol for both.
        // This is a temporary solution until we have a better way to handle this.
        // Currently, it is protected by the deployment script that ensures the same protocol is used for both.
        // TODO (Kun): add DataStoreAdminService section to the configuration of ChronoVisor and make sure all three
        //  ends (Keeper, Grapher, and Visor) use the same protocol.
        dataStoreAdminServiceProtocol = confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.PROTO_CONF;

        registryState = INITIALIZED;
        status = chronolog::CL_SUCCESS;
    }
    catch(tl::exception const &ex)
    {
        LOG_ERROR("[ChronoProcessRegistry] Failed to start");
    }

    return status;

}

KeeperRegistry::KeeperRegistry()
    : registryState(UNKNOWN)
    , registryEngine(nullptr)
    , keeperRegistryService(nullptr)
    , delayedDataAdminExitSeconds(3)
{
    // INNA: I'm using current time for seeding Mersene Twister number generator
    // there are different opinions on the use of std::random_device for seeding of Mersene Twister..
    // TODO: reseach the seeding of Mersense Twister int number generator

    size_t new_seed = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
    mt_random.seed(new_seed);//initial seed for the 32 int Mersene Twister generator
}

/////////////////

int KeeperRegistry::ShutdownRegistryService()
{


    std::lock_guard <std::mutex> lock(registryLock);

    if(is_shutting_down())
    {
        LOG_INFO("[ChronoProcessRegistry] Shutdown");
        return chronolog::CL_SUCCESS;
    }

    registryState = SHUTTING_DOWN;
    LOG_INFO("[ChronoProcessRegistry] Shutting down...");

    activeGroups.clear();
    activeStories.clear();

    while(!recordingGroups.empty())
    {
        std::time_t current_time =
                std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
        std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));

        for(auto group_iter = recordingGroups.begin(); group_iter != recordingGroups.end();)
        {
            RecordingGroup& recording_group = ((*group_iter).second);

            if(recording_group.grapherProcess != nullptr)
            {
                std::stringstream id_string;
                id_string << recording_group.grapherProcess->idCard;

                chl::GrapherProcessEntry* grapher_process = recording_group.grapherProcess;
                if(grapher_process->active)
                {
                    LOG_INFO("[ChronoProcessRegistry] Sending shutdown to grapher {}", id_string.str());
                    if(grapher_process->adminClient != nullptr) { grapher_process->adminClient->shutdown_collection(); }

                    // start delayed destruction for the lingering Adminclient to be safe...
                    recording_group.startDelayedGrapherExit(*grapher_process, delayedExitTime);
                }
                else
                {
                    //check if any existing delayed exit grapher processes can be cleared...
                    recording_group.clearDelayedExitGrapher(*grapher_process, current_time);
                }

                if(grapher_process->delayedExitGrapherClients.empty())
                {
                    LOG_INFO("[ChronoProcessRegistry] shudown: destroyed old entry for grapher {}", id_string.str());
                    delete grapher_process;
                    recording_group.grapherProcess = nullptr;
                }
            }

            // send out shutdown instructions to all active keeper processes
            // then start delayedExit procedure for them
            for(auto process_iter = recording_group.keeperProcesses.begin();
                process_iter != recording_group.keeperProcesses.end();)
            {
                std::stringstream id_string;
                id_string << (*process_iter).second.idCard;

                if((*process_iter).second.active)
                {
                    LOG_INFO("[ChronoProcessRegistry] Sending shutdown to keeper {}", id_string.str());
                    (*process_iter).second.keeperAdminClient->shutdown_collection();

                    LOG_INFO("[ChronoProcessRegistry] shutdown: starting delayedExit for keeper {} delayedExitTime={}",
                             id_string.str(), std::ctime(&delayedExitTime));
                    recording_group.startDelayedKeeperExit((*process_iter).second, delayedExitTime);
                }
                else
                {
                    LOG_INFO("[ChronoProcessRegistry] shutdown: clear delayedAdminClient for keeper {}", id_string.str());
                    recording_group.clearDelayedExitKeeper((*process_iter).second, current_time);
                }

                if((*process_iter).second.delayedExitClients.empty())
                {
                    LOG_INFO("[ChronoProcessRegistry] shutdown : destroys old keeperProcessEntry for {}", id_string.str());
                    process_iter = recording_group.keeperProcesses.erase(process_iter);
                }
                else
                {
                    LOG_INFO("[ChronoProcessRegistry] shutdown: old dataAdminClient for {} can't yet be destroyed",
                             id_string.str());
                    ++process_iter;
                }
            }
            if(recording_group.grapherProcess == nullptr && recording_group.keeperProcesses.empty())
            {
                LOG_INFO("[ChronoProcessRegistry] shutdown: recordingGroup {} is destroyed", recording_group.groupId);
                group_iter = recordingGroups.erase(group_iter);
            }
            else
            {
                LOG_INFO("[ChronoProcessRegistry] shutdown: recordingGroup {} can't yet be destroyed",
                         recording_group.groupId);
                ++group_iter;
            }
        }

        if(!recordingGroups.empty()) { sleep(1); }
    }


    if(nullptr != keeperRegistryService)
    { delete keeperRegistryService; }
    return chronolog::CL_SUCCESS;
}

KeeperRegistry::~KeeperRegistry()
{
    ShutdownRegistryService();
    registryEngine->finalize();
    delete registryEngine;
}
/////////////////


int KeeperRegistry::registerKeeperProcess(KeeperRegistrationMsg const &keeper_reg_msg)
{

    if(is_shutting_down())
    { return chronolog::CL_ERR_UNKNOWN; }

    std::lock_guard <std::mutex> lock(registryLock);
    //re-check state after ther lock is aquired
    if(is_shutting_down())
    { return chronolog::CL_ERR_UNKNOWN; }

    KeeperIdCard keeper_id_card = keeper_reg_msg.getKeeperIdCard();
    ServiceId admin_service_id = keeper_reg_msg.getAdminServiceId();

    //find the group that keeper belongs to in the registry
    auto group_iter = recordingGroups.find(keeper_id_card.getGroupId());
    if(group_iter == recordingGroups.end())
    {
        auto insert_return = recordingGroups.insert(std::pair<RecordingGroupId, RecordingGroup>(
                keeper_id_card.getGroupId(), RecordingGroup(keeper_id_card.getGroupId())));
        if(false == insert_return.second)
        {
            LOG_ERROR("[ChronoProcessRegistry] keeper registration failed to find RecordingGroup {}",
                      keeper_id_card.getGroupId());
            return chronolog::CL_ERR_UNKNOWN;
        }
        else { group_iter = insert_return.first; }
    }

    RecordingGroup& recording_group = (*group_iter).second;

    // unlikely but possible that the Registry still retains the record of the previous re-incarnation of hte Keeper process
    // running on the same host... check for this case and clean up the leftover record...
    auto keeper_process_iter = recording_group.keeperProcesses.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());

    if(keeper_process_iter != recording_group.keeperProcesses.end())
    {
        // must be a case of the KeeperProcess exiting without unregistering or some unexpected break in communication...
        // start delayed destruction process for hte lingering keeperAdminclient to be safe...

        if((*keeper_process_iter).second.active)
        {
            std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                    std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));
            LOG_WARNING("[ChronoProcessRegistry] registerKeeperProcess: found old instance of dataAdminclient for {} "
                        "delayedExitTime={}", to_string(keeper_id_card), std::ctime(&delayedExitTime));
            ;

            recording_group.startDelayedKeeperExit((*keeper_process_iter).second, delayedExitTime);
        }
        else
        {
            std::time_t current_time =
                    std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
            LOG_INFO("[ChronoProcessRegistry] registerKeeperProcess tries to clear dataAdmindClient for {}", to_string(keeper_id_card));
            recording_group.clearDelayedExitKeeper((*keeper_process_iter).second, current_time);
        }

        if((*keeper_process_iter).second.delayedExitClients.empty())
        {
            LOG_INFO("[ChronoProcessRegistry] registerKeeperProcess has destroyed old entry for keeper {}", to_string(keeper_id_card));
            recording_group.keeperProcesses.erase(keeper_process_iter);
        }
        else
        {
            LOG_INFO("[ChronoProcessRegistry] registration for Keeper {} cant's proceed as previous AdminClient is not yet dismantled", to_string(keeper_id_card));
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    //create a client of Keeper's DataStoreAdminService listening at adminServiceId

    std::string service_na_string;
    admin_service_id.get_service_as_string(service_na_string);

    DataStoreAdminClient*collectionClient = DataStoreAdminClient::CreateDataStoreAdminClient(*registryEngine
                                                                                             , service_na_string
                                                                                             , admin_service_id.getProviderId());
    if(nullptr == collectionClient)
    {
        LOG_ERROR("[ChronoProcessRegistry] Register Keeper: {} failed to create DataStoreAdminClient for {}"
             , to_string(keeper_id_card), to_string(admin_service_id));
        return chronolog::CL_ERR_UNKNOWN;
    }

    //now create a new KeeperRecord with the new DataAdminclient
    auto insert_return =
            recording_group.keeperProcesses.insert(std::pair<std::pair<uint32_t, uint16_t>, KeeperProcessEntry>(
                    keeper_id_card.getRecordingServiceId().get_service_endpoint(),
                    KeeperProcessEntry(keeper_id_card, admin_service_id)));
    if(false == insert_return.second)
    {
        LOG_ERROR("[ChronoProcessRegistry] registration failed for Keeper {}",to_string(keeper_id_card));
        delete collectionClient;
        return chronolog::CL_ERR_UNKNOWN;
    }

    (*insert_return.first).second.keeperAdminClient = collectionClient;
    (*insert_return.first).second.active = true;
    recording_group.activeKeeperCount += 1;

    LOG_INFO("[ChronoProcessRegistry] Register Keeper: {} created DataStoreAdminClient for {}"
         , to_string(keeper_id_card), to_string(admin_service_id));

    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has {} keepers", recording_group.groupId,
             recording_group.keeperProcesses.size());

    // check if this is the first keeper for the recording group and the group is ready to be part of
    //  the activeGroups rotation
    
    if(recording_group.isActive() && recording_group.activeKeeperCount == 1)
    {
        activeGroups.push_back(&recording_group);
        size_t new_seed = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
        mt_random.seed(new_seed);//re-seed the mersene_twister_generator
        group_id_distribution =
                std::uniform_int_distribution<size_t>(0, activeGroups.size() - 1);//reset the distribution range
    }

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ", activeGroups.size(), recordingGroups.size());
    if(activeGroups.size() > 0) { registryState = RUNNING; }
    return chronolog::CL_SUCCESS;
}
/////////////////

int KeeperRegistry::unregisterKeeperProcess(KeeperIdCard const &keeper_id_card)
{
    if(is_shutting_down())
    { return chronolog::CL_ERR_UNKNOWN; }

    std::lock_guard<std::mutex> lock(registryLock);
    //check again after the lock is acquired
    if(is_shutting_down())
    { return chronolog::CL_ERR_UNKNOWN; }

    auto group_iter = recordingGroups.find(keeper_id_card.getGroupId());
    if(group_iter == recordingGroups.end()) { return chronolog::CL_SUCCESS; }

    RecordingGroup& recording_group = (*group_iter).second;

    auto keeper_process_iter = recording_group.keeperProcesses.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());
            
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
            activeGroups.erase(std::remove(activeGroups.begin(), activeGroups.end(), &recording_group));
            if(activeGroups.size() > 0)
            {//reset the group distribution
                size_t new_seed = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
                mt_random.seed(new_seed);//re-seed the mersene_twister_generator
                group_id_distribution =
                    std::uniform_int_distribution<size_t>(0, activeGroups.size() - 1);//reset the distribution range
            }
        }

        // we mark the keeperProcessEntry as inactive and set the time it would be safe to delete.
        // we delay the destruction of the keeperEntry & keeperAdminClient by 5 secs
        // to prevent the case of deleting the keeperAdminClient while it might be waiting for rpc response on the
        // other thread

        std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));
        recording_group.startDelayedKeeperExit((*keeper_process_iter).second, delayedExitTime);
    }

    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has {} keepers", recording_group.groupId,
             recording_group.keeperProcesses.size());

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ", activeGroups.size(), recordingGroups.size());
    
    // update registryState if needed
    if(!is_shutting_down() && activeGroups.size() == 0) { registryState = INITIALIZED; }

    return chronolog::CL_SUCCESS;
}
/////////////////

void KeeperRegistry::updateKeeperProcessStats(KeeperStatsMsg const &keeperStatsMsg)
{
    // NOTE: we don't lock registryLock while updating the keeperProcess stats
    // delayed destruction of keeperProcessEntry protects us from the case when
    // stats message is received from the KeeperProcess that has unregistered
    // on the other thread
    if(is_shutting_down())
    { return; }

    KeeperIdCard keeper_id_card = keeperStatsMsg.getKeeperIdCard();

    LOG_DEBUG("[ChronoProcessRegistry] Received KeeperStatsMsg from {}", chl::to_string(keeper_id_card));

    auto group_iter = recordingGroups.find(keeper_id_card.getGroupId());
    if(group_iter == recordingGroups.end()) { return; }

    auto keeper_process_iter = 
        (*group_iter).second.keeperProcesses.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());
    if(keeper_process_iter == (*group_iter).second.keeperProcesses.end() || !((*keeper_process_iter).second.active))
    {// however unlikely it is that the stats msg would be delivered for the keeper that's already unregistered
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

    // pick recording_group from uniform group id distribution using a random int value
    // generated by Mirsene Twister generator


    std::time_t current_time = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());

    for(auto iter = keeperProcesses.begin(); iter != keeperProcesses.end();)
    {
        // first check if there are any delayedExit DataStoreClients to be deleted
        while(!(*iter).second.delayedExitClients.empty() &&
              (current_time >= (*iter).second.delayedExitClients.front().first))
        {
            auto dataStoreClientPair = (*iter).second.delayedExitClients.front();
            LOG_INFO("[ChronoProcessRegistry] getActiveKeepers() destroys dataAdminClient for keeper {} current_time={} delayedExitTime={}", 
                    (*iter).second.idCardString, ctime(&current_time), ctime(&(*iter).second.delayedExitClients.front
                    ().first));
            if(dataStoreClientPair.second != nullptr) { delete dataStoreClientPair.second; }
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
            LOG_INFO("[ChronoProcessRegistry] getActiveKeepers() destroys keeperProcessEntry for keeper {} current_time={}",
                     (*iter).second.idCardString, std::ctime(&current_time));
            iter = keeperProcesses.erase(iter);
        }
        else
        {
            LOG_INFO("ChronoProcessRegistry] getActiveKeepers still keeps keeperProcessEntry for keeper {} current_time={}",
                     (*iter).second.idCardString, std::ctime(&current_time));
            ++iter;
        }
    }

    return keeper_id_cards;
}
/////////////////
int KeeperRegistry::notifyRecordingGroupOfStoryRecordingStart(ChronicleName const& chronicle, StoryName const &story
                                                              , StoryId const &story_id
                                                              , std::vector <KeeperIdCard> &vectorOfKeepers
                                , ServiceId & player_service_id)
{
    vectorOfKeepers.clear();

    RecordingGroup* recording_group = nullptr;

    {
        //lock KeeperRegistry and choose the recording group for this story
        //NOTE we only keep the lock within this paragraph...
        std::lock_guard<std::mutex> lock(registryLock);
        if(!is_running())
        {
            LOG_ERROR("[ChronoProcessRegistry] Registry has no active RecordingGroups to start recording story {}", story_id);
            return chronolog::CL_ERR_NO_KEEPERS;
        }


        //first check if we are already recording this story for another client and have a recording group assigned to it

        auto story_iter = activeStories.find(story_id);

        if(story_iter != activeStories.end() && (*story_iter).second != nullptr)
        {
            //INNA:TODO: we should probably check if the group's active status hasn't changed
            //and implement group re-assignment procedure when we have recording processes dynamically coming and going..

            recording_group = (*story_iter).second;
            recording_group->getActiveKeepers(vectorOfKeepers); 
    
            //no need for notification , group processes are already recording this story
            LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} is already recording story {}", recording_group->groupId,story_id);
            
            return chronolog::CL_SUCCESS;
        }

        // select recording_group from the group id distribution using a random int value
        // generated by Mirsene Twister generator
        // NOTE: using uniform_distribution for now, we might add discrete distribution with weights later...

        recording_group = activeGroups[group_id_distribution(mt_random)];
        activeStories[story_id] = recording_group;
    }

    LOG_INFO("[ChronoProcessRegistry] selected RecordingGroup {} for story {}", recording_group->groupId, story_id);
    
    uint64_t story_start_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    // the registryLock is released by this point..
    // notify Grapher and notifyKeepers functions use delayedExit logic to protect
    // the rpc code from DataAdminClients being destroyed while notification is in progress..
    int rpc_return = notifyGrapherOfStoryRecordingStart(*recording_group, chronicle, story, story_id, story_start_time);

    if(rpc_return == chronolog::CL_SUCCESS)
    {
        recording_group->getActiveKeepers(vectorOfKeepers);
        rpc_return = notifyKeepersOfStoryRecordingStart(*recording_group, vectorOfKeepers, chronicle, story, story_id,
                                                        story_start_time);
    }

    if(rpc_return == chronolog::CL_SUCCESS && recording_group->playerProcess != nullptr)
    {
        player_service_id = recording_group->playerProcess->playbackServiceId;
    }
    return rpc_return;
}

////////////////
int KeeperRegistry::notifyGrapherOfStoryRecordingStart(RecordingGroup &recordingGroup, ChronicleName const &chronicle
                                                       , StoryName const &story, StoryId const &storyId
                                                       , uint64_t story_start_time)
{
    int return_code = chronolog::CL_ERR_NO_KEEPERS;

    if(!is_running())
    {
        LOG_ERROR("[ChronoProcessRegistry] Registry has no active RecordingGroups to start recording story {}", storyId);
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    DataStoreAdminClient* dataAdminClient = nullptr;

    {
        // NOTE: we release the registryLock before sending rpc request so that we do not hold it for the duration of rpc communication.
        // We delay the destruction of unactive adminClients that might be triggered by the unregister call from a different thread
        //  to protect us from the unfortunate case of aminClient object being deleted
        // while this thread is waiting for rpc response
        std::lock_guard<std::mutex> lock(registryLock);

        if(recordingGroup.grapherProcess != nullptr && recordingGroup.grapherProcess->active &&
           recordingGroup.grapherProcess->adminClient != nullptr)
        {
            dataAdminClient = recordingGroup.grapherProcess->adminClient;
        }
        else
        {
            LOG_WARNING("[ChronoProcessRegistry] grapher for recordingGroup {} is not available for notification",
                        recordingGroup.groupId);
        }
    }

    if(dataAdminClient == nullptr) { return return_code; }

    try
    {
        return_code = dataAdminClient->send_start_story_recording(chronicle, story, storyId, story_start_time);
        if(return_code != chronolog::CL_SUCCESS)
        {
            LOG_WARNING("[ChronoProcessRegistry] Registry failed RPC notification to {}", recordingGroup.grapherProcess->idCardString);
        }
        else
        {
            LOG_INFO("[ChronoProcessRegistry] Registry notified {} to start recording StoryID={} with StartTime={}",
                     recordingGroup.grapherProcess->idCardString, storyId, story_start_time);
        }
    }
    catch(thallium::exception const& ex)
    {
        LOG_WARNING("[ChronoProcessRegistry] Registry failed RPC notification to grapher {}", recordingGroup.grapherProcess->idCardString);
    }

    return return_code;
}

///////////////
int KeeperRegistry::notifyGrapherOfStoryRecordingStop(RecordingGroup& recordingGroup, StoryId const& storyId)
{
    int return_code = chronolog::CL_ERR_NO_KEEPERS;

    if(!is_running())
    {
        LOG_ERROR("[ChronoProcessRegistry] Registry has no active RecordingGroups to start recording story {}", storyId);
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    DataStoreAdminClient* dataAdminClient = nullptr;

    std::stringstream id_string;

    {
        // NOTE: we release the registryLock before sending rpc request so that we do not hold it for the duration of rpc communication.
        // We delay the destruction of unactive adminClients that might be triggered by the unregister call from a different thread
        //  to protect us from the unfortunate case of aminClient object being deleted
        // while this thread is waiting for rpc response
        std::lock_guard<std::mutex> lock(registryLock);

        if(recordingGroup.grapherProcess != nullptr && recordingGroup.grapherProcess->active &&
           recordingGroup.grapherProcess->adminClient != nullptr)
        {
            id_string << recordingGroup.grapherProcess->idCard;
            dataAdminClient = recordingGroup.grapherProcess->adminClient;
        }
        else
        {
            LOG_WARNING("[ChronoProcessRegistry] grapher for recordingGroup {} is not available for notification",
                        recordingGroup.groupId);
        }
    }

    if(dataAdminClient == nullptr) { return return_code; }

    try
    {
        return_code = dataAdminClient->send_stop_story_recording(storyId);
        if(return_code != chronolog::CL_SUCCESS)
        {
            LOG_WARNING("[ChronoProcessRegistry] Registry failed RPC notification to grapher {}", id_string.str());
        }
        else
        {
            LOG_INFO("[ChronoProcessRegistry] Registry notified grapher {} to stop recording StoryID={} ", id_string.str(),
                     storyId);
        }
    }
    catch(thallium::exception const& ex)
    {
        LOG_WARNING("[ChronoProcessRegistry] Registry failed RPC notification to grapher {}", id_string.str());
    }

    return return_code;
}
/////////////////////

int KeeperRegistry::notifyKeepersOfStoryRecordingStart(RecordingGroup& recordingGroup
                                                       , std::vector <KeeperIdCard> &vectorOfKeepers
                                                       , ChronicleName const &chronicle, StoryName const &story
                                                       , StoryId const &storyId, uint64_t story_start_time)
{

    // if there are no activeGroups ready for recording
    // we are out of luck...
    if(!is_running())
    {
        LOG_ERROR("[ChronoProcessRegistry] Registry has no active RecordingGroups to start recording story {}", storyId);
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    std::vector<KeeperIdCard> vectorOfKeepersToNotify = vectorOfKeepers;
    vectorOfKeepers.clear();

    auto keeper_processes = recordingGroup.keeperProcesses;

    for(KeeperIdCard keeper_id_card: vectorOfKeepersToNotify)
    {
        DataStoreAdminClient* dataAdminClient = nullptr;
        {
            // NOTE: we release the registryLock before sending rpc request so that we do not hold it for the duration of rpc communication.
            // We delay the destruction of unactive keeperProcessEntries that might be triggered by the keeper unregister call from a different thread
            // (see unregisterKeeperProcess()) to protect us from the unfortunate case of keeperProcessEntry.dataAdminClient object being deleted
            // while this thread is waiting for rpc response
            std::lock_guard<std::mutex> lock(registryLock);
            auto keeper_process_iter = keeper_processes.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());
                    
            if((keeper_process_iter != keeper_processes.end() && (*keeper_process_iter).second.active) &&
               ((*keeper_process_iter).second.keeperAdminClient != nullptr))
            {
                dataAdminClient = (*keeper_process_iter).second.keeperAdminClient;
            }
            else
            {
                LOG_WARNING("[ChronoProcessRegistry] Keeper {} is not available for notification", to_string(keeper_id_card));
                continue;
            }
        }

        try
        {
            int rpc_return = dataAdminClient->send_start_story_recording(chronicle, story, storyId, story_start_time);
            if(rpc_return != chronolog::CL_SUCCESS)
            {
                LOG_WARNING("[ChronoProcessRegistry] Registry failed RPC notification to keeper {}", to_string(keeper_id_card));
            }
            else
            {
                LOG_INFO("[ChronoProcessRegistry] Registry notified {} to start recording StoryID={} with StartTime={}",
                         to_string(keeper_id_card), storyId, story_start_time);
                vectorOfKeepers.push_back(keeper_id_card);
            }
        }
        catch(thallium::exception const& ex)
        {
            LOG_WARNING("[ChronoProcessRegistry] Registry failed RPC notification to keeper {}", to_string(keeper_id_card));
        }
    }

    if(vectorOfKeepers.empty())
    {
        LOG_ERROR("[ChronoProcessRegistry] Registry failed to notify keepers to start recording story {}", storyId);
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    return chronolog::CL_SUCCESS;
}
/////////////////
int KeeperRegistry::notifyRecordingGroupOfStoryRecordingStop(StoryId const& story_id)
{
    RecordingGroup* recording_group = nullptr;

    std::vector<KeeperIdCard> vectorOfKeepers;

    {
        //lock KeeperRegistry and choose the recording group for this story
        //NOTE we only keep the lock within this paragraph...
        std::lock_guard<std::mutex> lock(registryLock);
        if(!is_running())
        {
            LOG_ERROR("[ChronoProcessRegistry] Registry has no active RecordingGroups to start recording story {}", story_id);
            return chronolog::CL_ERR_NO_KEEPERS;
        }


        auto story_iter = activeStories.find(story_id);

        if(story_iter == activeStories.end())
        {
            //we don't know of this story
            return chronolog::CL_SUCCESS;
        }

        recording_group = (*story_iter).second;
        if(recording_group != nullptr) { recording_group->getActiveKeepers(vectorOfKeepers); }

        activeStories.erase(story_iter);
    }

    if(recording_group != nullptr)
    {
        // the registryLock is released by this point..
        // notify Grapher and notifyKeepers functions use delayedExit logic to protect
        // the rpc code from DataAdminClients being destroyed while notification is in progress..

        notifyKeepersOfStoryRecordingStop(*recording_group, vectorOfKeepers, story_id);
     
        notifyGrapherOfStoryRecordingStop(*recording_group, story_id);
    }

    return chronolog::CL_SUCCESS;
}
//////////////
int KeeperRegistry::notifyKeepersOfStoryRecordingStop(RecordingGroup& recordingGroup,
                                                      std::vector<KeeperIdCard> const& vectorOfKeepers,
                                                      StoryId const& storyId)
{
    if(!is_running())
    {
        LOG_ERROR("[ChronoProcessRegistry] Registry has no keepers to notify of story release {}", storyId);
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    auto keeper_processes = recordingGroup.keeperProcesses;

    for(KeeperIdCard keeper_id_card: vectorOfKeepers)
    {
        DataStoreAdminClient* dataAdminClient = nullptr;
        {
            // NOTE: we release the registryLock before sending rpc request so that we do not hold it for the duration of rpc communication.
            // We delay the destruction of unactive keeperProcessEntries that might be triggered by the keeper unregister call from a different thread
            // (see unregisterKeeperProcess()) to protect us from the unfortunate case of keeperProcessEntry.dataAdminClient object being deleted
            // while this thread is waiting for rpc response
            std::lock_guard<std::mutex> lock(registryLock);
            auto keeper_process_iter = keeper_processes.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());
            if((keeper_process_iter != keeper_processes.end() && (*keeper_process_iter).second.active) &&
               ((*keeper_process_iter).second.keeperAdminClient != nullptr))
            {
                dataAdminClient = (*keeper_process_iter).second.keeperAdminClient;
            }
            else
            {
                LOG_WARNING("[ChronoProcessRegistry] Keeper {} is not available for notification", to_string(keeper_id_card));
                continue;
            }
        }
        try
        {
            int rpc_return = dataAdminClient->send_stop_story_recording(storyId);
            if(rpc_return != chronolog::CL_SUCCESS)
            {
                LOG_WARNING("[ChronoProcessRegistry] Registry failed RPC notification to keeper {}", to_string(keeper_id_card));
            }
            else
            {
                LOG_INFO("[ChronoProcessRegistry] Registry notified  {} to stop recording story {}", to_string(keeper_id_card), storyId);
            }
        }
        catch(thallium::exception const& ex)
        {
            LOG_WARNING("[ChronoProcessRegistry] Registry failed RPC notification to keeper {}", to_string(keeper_id_card));
        }
    }

    return chronolog::CL_SUCCESS;
}

////////////////////////////

int KeeperRegistry::registerGrapherProcess(GrapherRegistrationMsg const & reg_msg)
{
    if(is_shutting_down()) { return chronolog::CL_ERR_UNKNOWN; }

    GrapherIdCard grapher_id_card = reg_msg.getGrapherIdCard();
    RecordingGroupId group_id = grapher_id_card.getGroupId();
    ServiceId admin_service_id = reg_msg.getAdminServiceId();

    std::lock_guard<std::mutex> lock(registryLock);
    //re-check state after ther lock is aquired
    if(is_shutting_down()) { return chronolog::CL_ERR_UNKNOWN; }

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
        else { group_iter = insert_return.first; }
    }

    RecordingGroup& recording_group = ((*group_iter).second);

    // it is possible that the Registry still retains the record of the previous re-incarnation of the grapher process
    // check for this case and clean up the leftover record...

    std::stringstream id_string;
    id_string << grapher_id_card;
    if(recording_group.grapherProcess != nullptr)
    {
        // start delayed destruction for the lingering Adminclient to be safe...

        chl::GrapherProcessEntry* grapher_process = recording_group.grapherProcess;
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
            LOG_INFO("[ChronoProcessRegistry] registerGrapherProcess has destroyed old entry for grapher {}", chl::to_string(grapher_id_card));
            delete grapher_process;
            recording_group.grapherProcess = nullptr;
        }
        else
        {
            LOG_INFO("[ChronoProcessRegistry] registration for Grapher{} cant's proceed as previous grapherClient isn't yet "
                     "dismantled", chl::to_string(grapher_id_card));
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    //create a client of the new grapher's DataStoreAdminService listenning at adminServiceId

    std::string service_na_string;
    admin_service_id.get_service_as_string(service_na_string);

    DataStoreAdminClient* collectionClient = DataStoreAdminClient::CreateDataStoreAdminClient(
            *registryEngine, service_na_string, admin_service_id.getProviderId());
    if(nullptr == collectionClient)
    {
        LOG_ERROR("[ChronoProcessRegistry] Register Grapher {} failed to create DataStoreAdminClient for {}: provider_id={}",
                  chl::to_string(grapher_id_card), chl::to_string(admin_service_id));
        return chronolog::CL_ERR_UNKNOWN;
    }

    //now create a new GrapherProcessEntry with the new DataAdminclient
    recording_group.grapherProcess = new GrapherProcessEntry(grapher_id_card, admin_service_id);
    recording_group.grapherProcess->adminClient = collectionClient;
    recording_group.grapherProcess->active = true;

    LOG_INFO("[ChronoProcessRegistry] Register grapher {} created DataStoreAdminClient for {}",
                  chl::to_string(grapher_id_card), chl::to_string(admin_service_id));

    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has a grappher and  {} keepers", recording_group.groupId,
             recording_group.keeperProcesses.size());

    // now that communnication with the Grapher is established and we are still holding registryLock
    // check if the group is ready for active group rotation
    if(recording_group.isActive())
    {
        activeGroups.push_back(&recording_group);
        
        //reset the group distribution
        size_t new_seed = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
        mt_random.seed(new_seed);//re-seed the mersene_twister_generator
        group_id_distribution = std::uniform_int_distribution<size_t>(0, activeGroups.size() - 1);
    }

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ", activeGroups.size(), recordingGroups.size());
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
    if(is_shutting_down()) { return chronolog::CL_ERR_UNKNOWN; }

    auto group_iter = recordingGroups.find(grapher_id_card.getGroupId());
    if(group_iter == recordingGroups.end()) { return chronolog::CL_SUCCESS; }

    RecordingGroup& recording_group = ((*group_iter).second);

    // we are about to unregister the grapher so the group can't perform recording duties 
    // if it were an active recordingGroup before remove it from rotation
    if(recording_group.isActive())
    {
        auto active_group_iter = activeGroups.begin();
        while (active_group_iter != activeGroups.end())
        {
            if((*active_group_iter) != &recording_group) 
            { ++active_group_iter;}
            else
            { break; }
        }

        if(active_group_iter != activeGroups.end())
        {
            //INNA: what do we do with any active Stories that this group were recordng? force release them and notify clients?
            // wait for the new grapher?
            LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} is not active; activeGroups.size{}", recording_group.groupId,activeGroups.size());
            activeGroups.erase(active_group_iter);
            if(activeGroups.size() > 0)
            {
                //reset the group distribution
                size_t new_seed = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
                mt_random.seed(new_seed);//re-seed the mersene_twister_generator
                group_id_distribution =
                    std::uniform_int_distribution<size_t>(0, activeGroups.size() - 1);//reset the distribution range
            }
        }
    }

    if(recording_group.grapherProcess != nullptr && recording_group.grapherProcess->active)
    {
        // start delayed destruction for the lingering Adminclient to be safe...
        // to prevent the case of deleting the keeperAdminClient while it might be waiting for rpc response on the
        // other thread

        std::time_t delayedExitTime = std::chrono::high_resolution_clock::to_time_t(
                std::chrono::high_resolution_clock::now() + std::chrono::seconds(delayedDataAdminExitSeconds));
        LOG_INFO("[ChronoProcessRegistry] starting delayedExit for grapher {} delayedExitTime={}", recording_group.grapherProcess->idCardString,
                 std::ctime(&delayedExitTime));

        recording_group.startDelayedGrapherExit(*(recording_group.grapherProcess), delayedExitTime);
    }

    // now that we are still holding registryLock
    // update registryState if needed
    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has no grapher and  {} keepers", recording_group.groupId,
             recording_group.keeperProcesses.size());
        
    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ", activeGroups.size(), recordingGroups.size());
    
    // update registryState in case this was the last active recordingGroup
    if(activeGroups.size() == 0) { registryState = INITIALIZED; }

    return chronolog::CL_SUCCESS;
}

}//namespace chronolog

void chl::KeeperRegistry::updateGrapherProcessStats(chl::GrapherStatsMsg const &statsMsg)
{
    // NOTE: we don't lock registryLock while updating the keeperProcess stats
    // delayed destruction of keeperProcessEntry protects us from the case when
    // stats message is received from the KeeperProcess that has unregistered
    // on the other thread
    if(is_shutting_down())
    { return; }

#ifdef DEBUG
    std::string id_string;
    id_string += stats.getGrapherIdCard();
    LOG_DEBUG("[ChronoProcessRegistry] Received GrapherStatsMsg from {}", chl::to_string(stats.getGrapherIdCard()));
#endif

    auto group_iter = recordingGroups.find(statsMsg.getGrapherIdCard().getGroupId());
    if(group_iter == recordingGroups.end()) 
    { return; }

    RecordingGroup& recording_group = ((*group_iter).second);
    if(recording_group.grapherProcess != nullptr && recording_group.grapherProcess->active)
    {
        // there's no need to update stats of inactive process
        recording_group.grapherProcess->lastStatsTime = std::chrono::steady_clock::now().time_since_epoch().count();
        recording_group.grapherProcess->activeStoryCount = statsMsg.getActiveStoryCount();

    }
}

/////////////////

int chl::KeeperRegistry::registerPlayerProcess(chl::PlayerRegistrationMsg const & reg_msg)
{
    if(is_shutting_down()) { return chronolog::CL_ERR_UNKNOWN; }

    chl::PlayerIdCard id_card = reg_msg.getPlayerIdCard();
    chl::RecordingGroupId group_id = id_card.getGroupId();
    chl::ServiceId admin_service_id = reg_msg.getAdminServiceId();

    std::lock_guard<std::mutex> lock(registryLock);
    if(is_shutting_down()) 
    { return chronolog::CL_ERR_UNKNOWN; }

    //find the recording group that new player process belongs to or create one 
    auto group_iter = recordingGroups.find(group_id);
    if(group_iter == recordingGroups.end())
    {
        auto insert_return =
                recordingGroups.insert(std::pair<chl::RecordingGroupId, chl::RecordingGroup>(group_id, chl::RecordingGroup(group_id)));
        if(false == insert_return.second)
        {
            LOG_ERROR("[ChronoProcessRegistry] player registration failed to find RecordingGroup {}", group_id);
            return chronolog::CL_ERR_UNKNOWN;
        }
        else { group_iter = insert_return.first; }
    }

    chl::RecordingGroup& recording_group = ((*group_iter).second);

    // it is possible that the Registry still retains the record of the previous re-incarnation of the player process
    // check for this case and clean up the leftover record...

    if(recording_group.playerProcess != nullptr)
    {
        // start delayed destruction for the lingering Adminclient to be safe...

        chl::PlayerProcessEntry* player_process = recording_group.playerProcess;
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
            LOG_INFO("[ChronoProcessRegistry] registerplayerProcess has destroyed old entry for player {}", chl::to_string(id_card));
            delete player_process;
            recording_group.playerProcess = nullptr;
        }
        else
        {
            LOG_INFO("[ChronoProcessRegistry] registration for Player{} cant's proceed as previous playerClient isn't yet "
                     "dismantled", chl::to_string(id_card));
                     
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    //create a client of the new player's DataStoreAdminService listenning at adminServiceId
    std::string service_na_string;
    admin_service_id.get_service_as_string(service_na_string);

    LOG_DEBUG("[ChronoProcessRegistry] creating AdminClient for service_na_string {}",service_na_string);

    chl::DataStoreAdminClient* collectionClient = chl::DataStoreAdminClient::CreateDataStoreAdminClient(
            *registryEngine, service_na_string, admin_service_id.getProviderId());
    if(nullptr == collectionClient)
    {
        LOG_ERROR("[ChronoProcessRegistry] Register Player {} failed to create DataStoreAdminClient for {}",
                  chl::to_string(id_card), chl::to_string(admin_service_id));
        return chronolog::CL_ERR_UNKNOWN;
    }

    //now create a new GrapherProcessEntry with the new DataAdminclient
    recording_group.playerProcess = new chl::PlayerProcessEntry(id_card, admin_service_id);
    recording_group.playerProcess->adminClient = collectionClient;
    recording_group.playerProcess->active = true;

    LOG_INFO("[ChronoProcessRegistry] Register Player {} created DataStoreAdminClient for {}",
             chl::to_string(id_card), chl::to_string(admin_service_id));

    // now that communnication with the Player is established and we are still holding registryLock
    // check if the group is ready for active group rotation

    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ", activeGroups.size(), recordingGroups.size());
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
    if(is_shutting_down()) { return chronolog::CL_ERR_UNKNOWN; }

    auto group_iter = recordingGroups.find(id_card.getGroupId());
    if(group_iter == recordingGroups.end()) 
    { return chronolog::CL_SUCCESS; }

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
        LOG_INFO("[ChronoProcessRegistry] starting delayedExit for player {} delayedExitTime={}", recording_group.playerProcess->idCardString,
                 std::ctime(&delayedExitTime));

        recording_group.startDelayedPlayerExit(*(recording_group.playerProcess), delayedExitTime);
    }

    // now that we are still holding registryLock
    // update registryState if needed
    LOG_INFO("[ChronoProcessRegistry] RecordingGroup {} has no chrono_player ", recording_group.groupId);
        
    LOG_INFO("[ChronoProcessRegistry]  has {} activeGroups; {} RecordingGroups ", activeGroups.size(), recordingGroups.size());
    
    // update registryState in case this was the last active recordingGroup
    if(activeGroups.size() == 0)
    { registryState = INITIALIZED; }

    return chronolog::CL_SUCCESS;
}

void chl::KeeperRegistry::updatePlayerProcessStats(chl::PlayerStatsMsg const &statsMsg)
{
    // NOTE: we don't lock registryLock while updating the keeperProcess stats
    // delayed destruction of keeperProcessEntry protects us from the case when
    // stats message is received from the KeeperProcess that has unregistered
    // on the other thread
    if(is_shutting_down())
    { return; }

#ifdef DEBUG
    std::string id_string;
    id_string += stats.getPlayerIdCard;
    LOG_DEBUG("[ChronoProcessRegistry] Received PlayerStatsMsg from {}", id_string.str());
#endif

    auto group_iter = recordingGroups.find(statsMsg.getPlayerIdCard().getGroupId());
    if(group_iter == recordingGroups.end())
    { return; }

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

    if(grapherProcess != nullptr && grapherProcess->active && activeKeeperCount >0)
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

    LOG_INFO("[ChronoProcessRegistry] recording_group {} starts delayedExit for {}", groupId, grapher_process.idCardString);
    if(grapher_process.adminClient != nullptr)
    {
        grapher_process.delayedExitGrapherClients.push_back(
                std::pair<std::time_t, chl::DataStoreAdminClient*>(delayedExitTime, grapher_process.adminClient));
        grapher_process.adminClient = nullptr;
    }
}

void chl::RecordingGroup::clearDelayedExitGrapher(chl::GrapherProcessEntry& grapher_process, std::time_t current_time)
{
    while(!grapher_process.delayedExitGrapherClients.empty() &&
          (current_time >= grapher_process.delayedExitGrapherClients.front().first))
    {
        LOG_INFO("[ChronoProcessRegistry] recording_Group {}, destroys delayed dataAdmindClient for {}", groupId,
                  grapher_process.idCardString);
        auto dataStoreClientPair = grapher_process.delayedExitGrapherClients.front();
        if(dataStoreClientPair.second != nullptr) { delete dataStoreClientPair.second; }
        grapher_process.delayedExitGrapherClients.pop_front();
    }
}

void chl::RecordingGroup::startDelayedPlayerExit(chl::PlayerProcessEntry& player_process,
                                                  std::time_t delayedExitTime)
{
    player_process.active = false;

    LOG_INFO("[ChronoProcessRegistry] recording_group {} starts delayedExit for {}", groupId, player_process.idCardString);
    if(player_process.adminClient != nullptr)
    {
        player_process.delayedExitPlayerClients.push_back(
                std::pair<std::time_t, chl::DataStoreAdminClient*>(delayedExitTime, player_process.adminClient));
        player_process.adminClient = nullptr;
    }
}

void chl::RecordingGroup::clearDelayedExitPlayer(chl::PlayerProcessEntry& player_process, std::time_t current_time)
{
    while(!player_process.delayedExitPlayerClients.empty() &&
          (current_time >= player_process.delayedExitPlayerClients.front().first))
    {
        LOG_INFO("[ChronoProcessRegistry] recording_Group {}, destroys delayed dataAdmindClient for {}", groupId,
                  player_process.idCardString);
        auto dataStoreClientPair = player_process.delayedExitPlayerClients.front();
        if(dataStoreClientPair.second != nullptr) { delete dataStoreClientPair.second; }
        player_process.delayedExitPlayerClients.pop_front();
    }
}


void chl::RecordingGroup::startDelayedKeeperExit(chl::KeeperProcessEntry& keeper_process, std::time_t delayedExitTime)
{
    // we mark the keeperProcessEntry as inactive and set the time it would be safe to delete.
    // we delay the destruction of the keeperEntry & keeperAdminClient by 5 secs
    // to prevent the case of deleting the keeperAdminClient while it might be waiting for rpc response on the
    // other thread

    keeper_process.active = false;
    activeKeeperCount -= 1;

    LOG_INFO("[ChronoProcessRegistry] recording_group {} starts delayedExit for {}", groupId, keeper_process.idCardString);
    if(keeper_process.keeperAdminClient != nullptr)
    {
        keeper_process.delayedExitClients.push_back(
                std::pair<std::time_t, chl::DataStoreAdminClient*>(delayedExitTime, keeper_process.keeperAdminClient));
        keeper_process.keeperAdminClient = nullptr;
    }
}

void chl::RecordingGroup::clearDelayedExitKeeper(chl::KeeperProcessEntry& keeper_process, std::time_t current_time)
{
    while(!keeper_process.delayedExitClients.empty() &&
          (current_time >= keeper_process.delayedExitClients.front().first))
    {
        LOG_INFO("[ChronoProcessRegistry] recording_group {} destroys delayed dataAdminClient for {}", groupId, keeper_process.idCardString);
        auto dataStoreClientPair = keeper_process.delayedExitClients.front();
        if(dataStoreClientPair.second != nullptr) { delete dataStoreClientPair.second; }
        keeper_process.delayedExitClients.pop_front();
    }
}
