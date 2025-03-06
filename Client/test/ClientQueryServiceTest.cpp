
#include <iostream>
#include <signal.h>

#include "chrono_monitor.h"
#include "ServiceId.h"
#include "ClientQueryService.h"

namespace tl = thallium;
namespace chl = chronolog;

bool keep_running = true;
void sigterm_handler(int)
{
    std::cout<< "Received SIGTERM signal. Initiating shutdown procedure."<<std::endl;
    keep_running = false;
    return;
}

int main()
{

    signal(SIGTERM, sigterm_handler);

    int result = chronolog::chrono_monitor::initialize( "file" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
                                                    ,"/tmp/client.log"  //, confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
                                                    , spdlog::level::debug// confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
                                                    , "CronoClient"//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
                                                    , 10000//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
                                                    , 2//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
                                                    , spdlog::level::debug//confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
                                                    );


    chl::ServiceId queryServiceId("ofi+sockets", "127.0.0.1",5557,57);

    std::string SERVICE_NA_STRING;
    queryServiceId.get_service_as_string(SERVICE_NA_STRING);

    // validate ip address, instantiate Recording Service and create IdCard

    chronolog::service_endpoint service_endpoint;
    if( !queryServiceId.is_valid())
    {
        std::cout<<"Failed to start Service. Invalid endpoint provided."<< chl::to_string(queryServiceId)<<std::endl;
        return (-1);
    }


// Instantiate RecordingService
    tl::engine * queryEngine = nullptr;
    chronolog::ClientQueryService * queryService = nullptr;

    try
    {
        margo_instance_id margo_id = margo_init(SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);
        queryEngine = new tl::engine(margo_id);

        std::cout <<"starting ClientQueryService at "<< queryEngine->self() <<" with provider_id"<< queryServiceId.getProviderId()<<std::endl;
        queryService = chronolog::ClientQueryService::CreateClientQueryService(*queryEngine
                                                                                             , queryServiceId);
                                                                                            
    }
    catch(tl::exception const &)
    {
        queryService = nullptr;
    }

    if(nullptr == queryService)
    {
        std::cout << "Failed to create QueryService at "<<chl::to_string(queryServiceId)<<std::endl;
        return (-1);
    }
        
    std::cout << "Created QueryService at "<<chl::to_string(queryServiceId)<<std::endl;

    queryService->addPlaybackQueryClient(chl::ServiceId{"ofi+sockets","127.0.0.1", 5555,55});

    while( true ==  keep_running)
    {
        sleep(3);
    }

    std::cout << "Shutting down  QueryService at "<<chl::to_string(queryServiceId)<<std::endl;
    delete queryService;

    delete queryEngine;
return 1;
}

