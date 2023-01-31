//
// Created by kfeng on 10/17/22.
//

#include "ChronoVisorServer2.h"
#include "global_var_visor.h"

int main() {
    ClocksourceManager::getInstance()->setClocksourceType(ClocksourceType::C_STYLE);
    ChronoVisor::ChronoVisorServer2 visor;
    visor.start();

    return 0;
}