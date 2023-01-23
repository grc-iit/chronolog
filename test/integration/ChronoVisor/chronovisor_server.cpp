//
// Created by kfeng on 10/17/22.
//

#include "ChronoVisorServer2.h"
#include "global_var_visor.h"

int main() {
    ChronoLog::ConfigurationManager confManager("./default_conf.json");
    ChronoVisor::ChronoVisorServer2 visor(confManager);
    visor.start();

    return 0;
}