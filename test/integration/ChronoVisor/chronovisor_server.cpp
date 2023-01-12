//
// Created by kfeng on 10/17/22.
//

#include "ChronoVisorServer2.h"
#include "global_var_visor.h"

int main() {
//    CHRONOLOG_CONF->RPC_SERVER_IP = "127.0.0.1";
//    CHRONOLOG_CONF->RPC_BASE_SERVER_PORT = 6789;
//    CHRONOLOG_CONF->RPC_IMPLEMENTATION = CHRONOLOG_THALLIUM_SOCKETS;
//    CHRONOLOG_CONF->RPC_NUM_SERVER_PORTS = 1;
//    CHRONOLOG_CONF->RPC_NUM_SERVICE_THREADS = 8;
    CHRONOLOG_CONF->LoadConfFromJSONFile("./default_conf.json");
    ChronoVisor::ChronoVisorServer2 visor;
    visor.start();

    return 0;
}