//
// Created by kfeng on 10/17/22.
//

#include <cassert>
#include <ChronoVisorServer2.h>
#include <global_var_visor.h>
#include <log.h>

int main() {
    ChronoVisor::ChronoVisorServer2 visor;
    visor.start();

    return 0;
}