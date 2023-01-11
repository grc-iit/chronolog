
#include <iostream>

#include <margo.h>

#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"
#include "KeeperRegistryService.h"
#include "KeeperRegistry.h"


// this file allows testing of the KeeperRegistryService and KeeperRegistry classes 
// as the standalone process without the rest of the ChronoVizor modules 
//

#define KEEPER_REGISTRY_SERVICE_NA_STRING  "ofi+sockets://127.0.0.1:1234"
#define KEEPER_REGISTRY_SERVICE_PROVIDER_ID	25

int main(int argc, char** argv) {

    uint16_t provider_id = KEEPER_REGISTRY_SERVICE_PROVIDER_ID;

    margo_instance_id margo_id=margo_init(KEEPER_REGISTRY_SERVICE_NA_STRING, MARGO_SERVER_MODE, 1, 0);

    if(MARGO_INSTANCE_NULL == margo_id)
    {
      std::cout<<"FAiled to initialise margo_instance"<<std::endl;
      return 1;
    }
    std::cout<<"KeeperRegistryService:margo_instance initialized"<<std::endl;

   tl::engine keeper_reg_engine(margo_id);
 
    std::cout << "Starting KeeperRegService  at address " << keeper_reg_engine.self()
        << " with provider id " << provider_id << std::endl;

    chronolog::KeeperRegistry keeperRegistry;

    chronolog::KeeperRegistryService keeperRegistryService(keeper_reg_engine,provider_id, keeperRegistry);
    
    return 0;
}
