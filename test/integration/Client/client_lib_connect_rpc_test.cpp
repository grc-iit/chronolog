#include <chrono>
#include <unordered_map>
#include <cassert>

#include "chronolog_client.h"
#include "ClientConfiguration.h"
#include "client_cmd_arg_parse.h"
#include "chrono_monitor.h"
#include "common.h"

#define NUM_CONNECTION (1)

int main(int argc, char** argv)
{
    // Load configuration
    std::string conf_file_path = chronolog::parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if (!conf_file_path.empty()) {
        if (!confManager.load_from_file(conf_file_path)) {
            std::cerr << "[ClientLibConnectRPCTest] Failed to load configuration file '" << conf_file_path << "'. Using default values instead." << std::endl;
        } else {
            std::cout << "[ClientLibConnectRPCTest] Configuration file loaded successfully from '" << conf_file_path << "'." << std::endl;
        }
    } else {
        std::cout << "[ClientLibConnectRPCTest] No configuration file provided. Using default values." << std::endl;
    }

    // Initialize logging
    int result = chronolog::chrono_monitor::initialize(confManager.LOG_CONF.LOGTYPE,
                                                       confManager.LOG_CONF.LOGFILE,
                                                       confManager.LOG_CONF.LOGLEVEL,
                                                       confManager.LOG_CONF.LOGNAME,
                                                       confManager.LOG_CONF.LOGFILESIZE,
                                                       confManager.LOG_CONF.LOGFILENUM,
                                                       confManager.LOG_CONF.FLUSHLEVEL);
    if (result == 1) {
        return EXIT_FAILURE;
    }

    // Build portal config
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    LOG_INFO("[ClientLibConnectRPCTest] Running test.");

    std::string server_ip = portalConf.IP;
    int base_port = portalConf.PORT;

    chronolog::Client client(portalConf);

    std::string server_uri;
    std::vector<std::string> client_ids;
    int flags = 0;
    bool ret = false;
    uint64_t offset;

    std::chrono::steady_clock::time_point t1, t2;
    std::chrono::duration<double, std::nano> duration_connect{}, duration_disconnect{};

    client_ids.reserve(NUM_CONNECTION);
    for (int i = 0; i < NUM_CONNECTION; i++) {
        client_ids.emplace_back(gen_random(8));
    }

    for (int i = 0; i < NUM_CONNECTION; i++) {
        server_uri = portalConf.PROTO_CONF + "://" + server_ip + ":" + std::to_string(base_port + i);
        t1 = std::chrono::steady_clock::now();
        ret = client.Connect();//server_uri, client_ids[i], flags); //, offset);
        assert(ret == chronolog::CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_connect += (t2 - t1);
    }

    for(int i = 0; i < NUM_CONNECTION; i++)
    {
        t1 = std::chrono::steady_clock::now();
        ret = client.Disconnect(); //client_ids[i], flags);
        assert(ret == chronolog::CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_disconnect += (t2 - t1);
    };

    LOG_INFO("[ClientLibConnectRPCTest] Average connection time: {} ns", duration_connect.count() / NUM_CONNECTION);
    LOG_INFO("[ClientLibConnectRPCTest] Average disconnection time: {} ns",
            duration_disconnect.count() / NUM_CONNECTION);

    return 0;
}