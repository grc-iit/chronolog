
#include <arpa/inet.h>

#include "chrono_common/KeeperIdCard.h"
#include "chrono_common/KeeperStatsMsg.h"
#include "KeeperRecordingService.h"
#include "KeeperRegClient.h"
#include "IngestionQueue.h"
#include "KeeperDataStore.h"
#include "DataStoreAdminService.h"
#include "ConfigurationManager.h"

#define KEEPER_GROUP_ID 7

// we will be using a combination of the uint32_t representation of the service IP address
// and uint16_t representation of the port number
int service_endpoint_from_dotted_string(std::string const &ip_string, int port, std::pair<uint32_t, uint16_t> &endpoint)
{
    // we will be using a combination of the uint32_t representation of the service IP address 
    // and uint16_t representation of the port number 
    // NOTE: both IP and port values in the KeeperCard are in the host byte order, not the network order)
    // to identfy the ChronoKeeper process

    struct sockaddr_in sa;
    // translate the recording service dotted IP string into 32bit network byte order representation
    int inet_pton_return = inet_pton(AF_INET, ip_string.c_str(), &sa.sin_addr.s_addr); //returns 1 on success
    if (1 != inet_pton_return)
    {
        std::cout << "invalid ip address" << std::endl;
        return (-1);
    }

    // translate 32bit ip from network into the host byte order
    uint32_t ntoh_ip_addr = ntohl(sa.sin_addr.s_addr);
    uint16_t ntoh_port = port;
    endpoint = std::pair<uint32_t, uint16_t>(ntoh_ip_addr, ntoh_port);

    return 1;
}

///////////////////////////////////////////////

int main(int argc, char **argv)
{

    int exit_code = 0;

    //INNA: TODO: pass the config file path on the command line & load the parameters inot a ConfigurationObject
    // for now all the arguments are hardcoded ...

    ChronoLog::ConfigurationManager confManager("./default_conf.json");
    // Instantiate ChronoKeeper MemoryDataStore
    //
    chronolog::IngestionQueue ingestionQueue;
    chronolog::KeeperDataStore theDataStore(ingestionQueue);

    // instantiate DataStoreAdminService
    uint64_t keeper_group_id = KEEPER_GROUP_ID;

    std::string datastore_service_ip = confManager.RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.KEEPER_IP;
    int datastore_service_port = confManager.RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.KEEPER_PORT;
    std::string KEEPER_DATASTORE_SERVICE_NA_STRING =
            confManager.RPC_CONF.VISOR_KEEPER_CONF.PROTO_CONF
            + "://" + datastore_service_ip
            + ":" + std::to_string(datastore_service_port);

    uint16_t datastore_service_provider_id =
            confManager.RPC_CONF.VISOR_KEEPER_CONF.KEEPER_END_CONF.SERVICE_PROVIDER_ID;

    chronolog::service_endpoint datastore_endpoint;
    // validate ip address, instantiate DataAdminService and create ServiceId to be included in KeeperRegistrationMsg

    if (-1 == service_endpoint_from_dotted_string(datastore_service_ip, datastore_service_port, datastore_endpoint))
    {
        std::cout << "invalid DataStoreAdmin service address" << std::endl;
        return (-1);
    }

    chronolog::ServiceId collectionServiceId(datastore_endpoint.first, datastore_endpoint.second,
                                             datastore_service_provider_id);


    margo_instance_id collection_margo_id = margo_init(KEEPER_DATASTORE_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1,
                                                       1);

    if (MARGO_INSTANCE_NULL == collection_margo_id)
    {
        std::cout << "FAiled to initialise collection_margo_instance" << std::endl;
        return 1;
    }
    std::cout << "collection_margo_instance initialized" << std::endl;

    tl::engine collectionEngine(collection_margo_id);

    std::cout << "ChronoKeeperInstance group_id {" << keeper_group_id << "} starting DataStoreAdminService at address {"
              << collectionEngine.self()
              << "} with provider_id {" << datastore_service_provider_id << "}" << std::endl;

    chronolog::DataStoreAdminService *keeperDataAdminService =
            chronolog::DataStoreAdminService::CreateDataStoreAdminService(collectionEngine,
                                                                          datastore_service_provider_id, theDataStore);


    // Instantiate KeeperRecordingService 

    std::string KEEPER_RECORDING_SERVICE_PROTOCOL = confManager.RPC_CONF.CLIENT_KEEPER_CONF.PROTO_CONF;
    std::string KEEPER_RECORDING_SERVICE_IP = confManager.RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.KEEPER_IP;
    uint16_t KEEPER_RECORDING_SERVICE_PORT = confManager.RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.KEEPER_PORT;
    uint16_t recording_service_provider_id = confManager.RPC_CONF.CLIENT_KEEPER_CONF.KEEPER_END_CONF.SERVICE_PROVIDER_ID;

    std::string KEEPER_RECORDING_SERVICE_NA_STRING = std::string(KEEPER_RECORDING_SERVICE_PROTOCOL)
                                                     + "://" + std::string(KEEPER_RECORDING_SERVICE_IP)
                                                     + ":" + std::to_string(KEEPER_RECORDING_SERVICE_PORT);

    // validate ip address, instantiate Recording Service and create KeeperIdCard

    chronolog::service_endpoint recording_endpoint;
    if (-1 == service_endpoint_from_dotted_string(KEEPER_RECORDING_SERVICE_IP, KEEPER_RECORDING_SERVICE_PORT,
                                                  recording_endpoint))
    {
        std::cout << "invalid KeeperRecordingService  address" << std::endl;
        return (-1);
    }

    margo_instance_id margo_id = margo_init(KEEPER_RECORDING_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);

    if (MARGO_INSTANCE_NULL == margo_id)
    {
        std::cout << "FAiled to initialise margo_instance" << std::endl;
        return 1;
    }
    std::cout << "margo_instance initialized" << std::endl;

    tl::engine recordingEngine(margo_id);

    std::cout << "ChronoKeeperInstance group_id {" << keeper_group_id
              << "} starting KeeperRecordingService at address {" << recordingEngine.self()
              << "} with provider_id {" << recording_service_provider_id << "}" << std::endl;


    // Instantiate KeeperRecordingService 

    chronolog::KeeperRecordingService *keeperRecordingService =
            chronolog::KeeperRecordingService::CreateKeeperRecordingService(recordingEngine,
                                                                            recording_service_provider_id,
                                                                            ingestionQueue);

    // create KeeperIdCard to identify this Keeper process in ChronoVisor's KeeperRegistry
    chronolog::KeeperIdCard keeperIdCard(keeper_group_id, recording_endpoint.first, recording_endpoint.second,
                                         recording_service_provider_id);
    std::cout << keeperIdCard << std::endl;

    // create KeeperRegistryClient and register the new KeeperRecording service with the KeeperRegistry 
    std::string KEEPER_REGISTRY_SERVICE_NA_STRING =
            confManager.RPC_CONF.VISOR_KEEPER_CONF.PROTO_CONF
            + "://" + confManager.RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_IP
            + ":" + std::to_string(confManager.RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.VISOR_BASE_PORT);

    uint16_t KEEPER_REGISTRY_SERVICE_PROVIDER_ID = confManager.RPC_CONF.VISOR_KEEPER_CONF.VISOR_END_CONF.SERVICE_PROVIDER_ID;

    chronolog::KeeperRegistryClient *keeperRegistryClient = chronolog::KeeperRegistryClient::CreateKeeperRegistryClient(
            collectionEngine, KEEPER_REGISTRY_SERVICE_NA_STRING, KEEPER_REGISTRY_SERVICE_PROVIDER_ID);

    keeperRegistryClient->send_register_msg(chronolog::KeeperRegistrationMsg(keeperIdCard, collectionServiceId));

    // now we are ready to ingest records coming from the storyteller clients ....

    // for now BOTH KeeperRecordingService and KeeperRegistryClient will be running until they are explicitly killed  
    // INNA: TODO: add a graceful shutdown mechanism with finalized callbacks and all
    //

    chronolog::KeeperStatsMsg keeperStatsMsg(keeperIdCard);
    while (!theDataStore.is_shutting_down())
    {
        keeperRegistryClient->send_stats_msg(keeperStatsMsg);
        theDataStore.collectIngestedEvents();
        sleep(10);
    }

    keeperRegistryClient->send_unregister_msg(keeperIdCard);
    delete keeperRegistryClient;
    collectionEngine.wait_for_finalize();
    recordingEngine.wait_for_finalize();
    delete keeperRecordingService;
    delete keeperDataAdminService;


    return exit_code;
}
