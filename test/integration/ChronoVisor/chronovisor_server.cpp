//
// Created by kfeng on 10/17/22.
//

#include "ChronoVisorServer2.h"

int main(int argc, char *argv[]) {
    std::string conf_file_path = "./default_conf.json";
    if (argc > 1) {
        conf_file_path = argv[1];
    }
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    ChronoVisor::ChronoVisorServer2 visor(confManager);
    visor.start();

    return 0;
}