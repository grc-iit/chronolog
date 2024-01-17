#include <iostream>

#include <thallium.hpp>

#include "KeeperRegistry.h"
#include "KeeperRegistryService.h"
#include "DataStoreAdminClient.h"
#include "log.h"
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
            LOGC("[KeeperRegistry] Failed to initialize margo_instance");
            return -1;
        }
        LOGI("[KeeperRegistry] margo_instance initialized with NA_STRING {}", KEEPER_REGISTRY_SERVICE_NA_STRING);

        registryEngine = new tl::engine(margo_id);

        std::stringstream ss;
        ss << registryEngine->self();
        LOGI("[KeeperRegistry] Starting at address {} with provider id: {}", ss.str(), provider_id);

        keeperRegistryService = KeeperRegistryService::CreateKeeperRegistryService(*registryEngine, provider_id, *this);

        registryState = INITIALIZED;
        status = chronolog::CL_SUCCESS;
    }
    catch(tl::exception const &ex)
    {
        LOGE("[KeeperRegistry] Failed to start");
    }

    return status;

}
/////////////////

int KeeperRegistry::ShutdownRegistryService()
{


    std::lock_guard <std::mutex> lock(registryLock);

    if(is_shutting_down())
    {
        LOGI("[KeeperRegistry] Shutdown");
        return chronolog::CL_SUCCESS;
    }

    registryState = SHUTTING_DOWN;
    LOGI("[KeeperRegistry] Shutting down...");

    // send out shutdown instructions to 
    // all the registered keeper processes
    // then drain the registry
    if(!keeperProcessRegistry.empty())
    {
        for(auto process: keeperProcessRegistry)
        {
            if(process.second.keeperAdminClient != nullptr)
            {
                std::stringstream ss;
                ss << process.second.idCard;
                LOGI("[KeeperRegistry] Sending shutdown to keeper {}", ss.str());
                process.second.keeperAdminClient->shutdown_collection();
                delete process.second.keeperAdminClient;
            }
        }
        keeperProcessRegistry.clear();
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
    // unlikely but possible that the Registry still retains the record of the previous re-incarnation of hte Keeper process
    // running on the same host... check for this case and clean up the leftover record...
    auto keeper_process_iter = keeperProcessRegistry.find(
            std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()));
    if(keeper_process_iter != keeperProcessRegistry.end())
    {
        // delete keeperAdminClient before erasing keeper_process entry
        if((*keeper_process_iter).second.keeperAdminClient != nullptr)
        { delete (*keeper_process_iter).second.keeperAdminClient; }
        keeperProcessRegistry.erase(keeper_process_iter);

    }

    //create a client of Keeper's DataStoreAdminService listenning at adminServiceId
    std::string service_na_string("ofi+sockets://");
    service_na_string =
            admin_service_id.getIPasDottedString(service_na_string) + ":" + std::to_string(admin_service_id.port);

    DataStoreAdminClient*collectionClient = DataStoreAdminClient::CreateDataStoreAdminClient(*registryEngine
                                                                                             , service_na_string
                                                                                             , admin_service_id.provider_id);
    if(nullptr == collectionClient)
    {
        std::stringstream ss;
        ss << keeper_id_card;
        LOGE("[KeeperRegistry] Register Keeper: KeeperIdCard: {} failed to create DataStoreAdminClient for {}: provider_id={}"
             , ss.str(), service_na_string, admin_service_id.provider_id);
        return chronolog::CL_ERR_UNKNOWN;
    }

    //now create a new KeeperRecord with the new DataAdminclient
    auto insert_return = keeperProcessRegistry.insert(std::pair <std::pair <uint32_t, uint16_t>, KeeperProcessEntry>(
            std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()), KeeperProcessEntry(
                    keeper_id_card, admin_service_id)));
    if(false == insert_return.second)
    {
        std::stringstream ss;
        ss << keeper_id_card;
        LOGE("[KeeperRegistry] Register Keeper {} failed to registration", ss.str());
        delete collectionClient;
        return chronolog::CL_ERR_UNKNOWN;
    }

    (*insert_return.first).second.keeperAdminClient = collectionClient;

    std::stringstream s1;
    s1 << keeper_id_card;
    LOGD("[KeeperRegistry] Register Keeper: KeeperIdCard: {} failed to create DataStoreAdminClient for {}: provider_id={}"
         , s1.str(), service_na_string, admin_service_id.provider_id);

    // now that communnication with the Keeper is established and we still holding registryLock
    // update registryState in case this is the first KeeperProcess registration
    if(keeperProcessRegistry.size() > 0)
    { registryState = RUNNING; }

    LOGI("[KeeperRegistry] RUNNING with {} KeeperProcesses", keeperProcessRegistry.size());

    return chronolog::CL_SUCCESS;
}
/////////////////

int KeeperRegistry::unregisterKeeperProcess(KeeperIdCard const &keeper_id_card)
{
    if(is_shutting_down())
    { return chronolog::CL_ERR_UNKNOWN; }

    std::lock_guard <std::mutex> lock(registryLock);
    //check again after the lock is aquired
    if(is_shutting_down())
    { return chronolog::CL_ERR_UNKNOWN; }

    // stop & delete keeperAdminClient before erasing keeper_process entry
    auto keeper_process_iter = keeperProcessRegistry.find(
            std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()));
    if(keeper_process_iter != keeperProcessRegistry.end())
    {
        // delete keeperAdminClient before erasing keeper_process entry
        if((*keeper_process_iter).second.keeperAdminClient != nullptr)
        { delete (*keeper_process_iter).second.keeperAdminClient; }
        keeperProcessRegistry.erase(keeper_process_iter);
    }
    // now that we are still holding registryLock
    // update registryState if needed
    if(!is_shutting_down() && (0 == keeperProcessRegistry.size()))
    {
        registryState = INITIALIZED;
        LOGI("[KeeperRegistry] INITIALIZED with {} KeeperProcesses", keeperProcessRegistry.size());
    }

    return chronolog::CL_SUCCESS;
}
/////////////////

void KeeperRegistry::updateKeeperProcessStats(KeeperStatsMsg const &keeperStatsMsg)
{
    if(is_shutting_down())
    { return; }

    std::lock_guard <std::mutex> lock(registryLock);
    if(is_shutting_down())
    { return; }

    KeeperIdCard keeper_id_card = keeperStatsMsg.getKeeperIdCard();
    auto keeper_process_iter = keeperProcessRegistry.find(
            std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()));
    if(keeper_process_iter == keeperProcessRegistry.end())
    {    // however unlikely it is that the stats msg would be delivered for the keeper that's already unregistered
        // we should probably log a warning here...
        return;
    }
    KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
    // keeper_process.lastStatsTime = std::chrono::steady_clock::now();
    keeper_process.activeStoryCount = keeperStatsMsg.getActiveStoryCount();

}
/////////////////

std::vector <KeeperIdCard> &KeeperRegistry::getActiveKeepers(std::vector <KeeperIdCard> &keeper_id_cards)
{  //the process of keeper selection will probably get more nuanced; 
    //for now just return all the keepers registered
    if(is_shutting_down())
    { return keeper_id_cards; }

    std::lock_guard <std::mutex> lock(registryLock);
    if(is_shutting_down())
    { return keeper_id_cards; }

    keeper_id_cards.clear();
    for(auto keeperProcess: keeperProcessRegistry)
        keeper_id_cards.push_back(keeperProcess.second.idCard);

    return keeper_id_cards;
}
/////////////////

int KeeperRegistry::notifyKeepersOfStoryRecordingStart(std::vector <KeeperIdCard> const &vectorOfKeepers
                                                       , ChronicleName const &chronicle, StoryName const &story
                                                       , StoryId const &storyId)
{
    if(!is_running())
    {
        LOGW("[KeeperRegistry] Registry has no Keeper processes to start story recording");
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    std::chrono::time_point <std::chrono::system_clock> time_now = std::chrono::system_clock::now();

    uint64_t story_start_time = time_now.time_since_epoch().count();

    std::lock_guard <std::mutex> lock(registryLock);
    if(!is_running())
    { return chronolog::CL_ERR_NO_KEEPERS; }

    size_t keepers_left_to_notify = vectorOfKeepers.size();
    for(KeeperIdCard keeper_id_card: vectorOfKeepers)
    {
        auto keeper_process_iter = keeperProcessRegistry.find(
                std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()));
        if(keeper_process_iter == keeperProcessRegistry.end())
        {
            std::stringstream s0;
            s0 << keeper_id_card;
            LOGW("[KeeperRegistry] Registry failed to find Keeper with {}", s0.str());
            continue;
        }
        KeeperProcessEntry keeper_process = (*keeper_process_iter).second;

        std::stringstream ss, s1;
        ss << keeper_process.idCard;
        s1 << keeper_process.adminServiceId;
        LOGD("[KeeperRegistry] Found Keeper Process: KeeperIdCard: {}, AdminServiceId: {}, LastStatsTime: {}, ActiveStoryCount: {}"
             , ss.str(), s1.str(), keeper_process.lastStatsTime, keeper_process.activeStoryCount);
        if(nullptr == keeper_process.keeperAdminClient)
        {
            std::stringstream s2;
            s2 << keeper_id_card;
            LOGW("[KeeperRegistry] Registry record for {} is missing keeperAdminClient", ss.str());
            continue;
        }
        try
        {
            int rpc_return = keeper_process.keeperAdminClient->send_start_story_recording(chronicle, story, storyId
                                                                                          , story_start_time);
            if(rpc_return != chronolog::CL_SUCCESS)
            {
                std::stringstream s3;
                s3 << keeper_id_card;
                LOGW("[KeeperRegistry] Registry failed notification RPC to keeper {}", s3.str());
                continue;
            }
            std::stringstream s4;
            s4 << keeper_id_card;
            LOGI("[KeeperRegistry] Registry notified keeper {} to start recording StoryID={} at StartTime={}", s4.str()
                 , storyId, story_start_time);
            keepers_left_to_notify--;
        }
        catch(thallium::exception const &ex)
        {
            std::stringstream s5;
            s5 << keeper_id_card;
            LOGW("[KeeperRegistry] Registry failed notification RPC to keeper {} details:", s5.str());
            continue;
        }
    }

    if(keepers_left_to_notify == vectorOfKeepers.size())
    {
        LOGE("[KeeperRegistry] Registry failed to notify the keepers to start recording story {}", storyId);
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    return chronolog::CL_SUCCESS;
}
/////////////////

int KeeperRegistry::notifyKeepersOfStoryRecordingStop(std::vector <KeeperIdCard> const &vectorOfKeepers
                                                      , StoryId const &storyId)
{
    if(!is_running())
    {
        LOGD("[KeeperRegistry] Registry has no Keeper processes to notify of story release");
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    std::lock_guard <std::mutex> lock(registryLock);
    if(!is_running())
    { return chronolog::CL_ERR_NO_KEEPERS; }

    size_t keepers_left_to_notify = vectorOfKeepers.size();
    for(KeeperIdCard keeper_id_card: vectorOfKeepers)
    {
        auto keeper_process_iter = keeperProcessRegistry.find(
                std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()));
        if(keeper_process_iter == keeperProcessRegistry.end())
        {
            std::stringstream ss;
            ss << keeper_id_card;
            LOGW("[KeeperRegistry] Registry failed to find Keeper with {}", ss.str());
            continue;
        }
        KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
        if(nullptr == keeper_process.keeperAdminClient)
        {
            std::stringstream s1;
            s1 << keeper_id_card;
            LOGW("[KeeperRegistry] Registry record for {} is missing keeperAdminClient", s1.str());
            continue;
        }
        try
        {
            int rpc_return = keeper_process.keeperAdminClient->send_stop_story_recording(storyId);
            if(rpc_return < 0)
            {
                std::stringstream s2;
                s2 << keeper_id_card;
                LOGW("[KeeperRegistry] Registry failed notification RPC to keeper {}", s2.str());
                continue;
            }
            std::stringstream s3;
            s3 << keeper_id_card;
            LOGI("[KeeperRegistry] Registry notified keeper {} to stop recording story {}", s3.str(), storyId);
            keepers_left_to_notify--;
        }
        catch(thallium::exception const &ex)
        {
            std::stringstream s4;
            s4 << keeper_id_card;
            LOGW("[KeeperRegistry] Registry failed notification RPC to keeper {} details.", s4.str());
            continue;
        }
    }

    if(keepers_left_to_notify == vectorOfKeepers.size())
    {
        LOGE("[KeeperRegistry] Registry failed to notify the keepers to stop recording story {}", storyId);
        return chronolog::CL_ERR_NO_KEEPERS;
    }
    return chronolog::CL_SUCCESS;
}


}//namespace chronolog


