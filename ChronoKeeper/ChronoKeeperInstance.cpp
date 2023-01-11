
#include <arpa/inet.h>

//#include "Storyteller.h"


#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"
#include "KeeperRecordingService.h"
#include "KeeperRegClient.h"
//#include "KeeperProcess.h"



int main(int argc, char** argv) {

  int exit_code = 0;

    uint16_t provider_id = 22;

    margo_instance_id margo_id=margo_init("ofi+sockets://127.0.0.1:5555",MARGO_SERVER_MODE, 1, 0);

    if(MARGO_INSTANCE_NULL == margo_id)
    {
      std::cout<<"FAiled to initialise margo_instance"<<std::endl;
      return 1;
    }
    std::cout<<"margo_instance initialized"<<std::endl;

   tl::engine keeperEngine(margo_id);
 
    std::cout << "Starting KeeperRecordingService  at address " << keeperEngine.self()
        << " with provider id " << provider_id << std::endl;

   chronolog::KeeperRecordingService keeperRecordingService(keeperEngine, provider_id);


    chronolog::KeeperRegistryClient * keeperRegistryClient = chronolog::KeeperRegistryClient::CreateKeeperRegistryClient(
		     keeperEngine,"ofi+sockets://127.0.0.1:1234", 25); 
    chronolog::KeeperIdCard keeperIdCard(123, 5555, 22);
    keeperRegistryClient->send_register_msg(keeperIdCard);
    keeperRegistryClient->send_unregister_msg(keeperIdCard);

    delete keeperRegistryClient;

return exit_code;
}
