//
// Created by kfeng on 10/22/21.
//

#include <iostream>
#include <ctime>
#include <ratio>
#include <chrono>
#include <unistd.h>
#include <log.h>
#include <TimeRecord.h>
#include "ClocksourceManager.h"

#define SLEEP_INTERVAL_IN_SEC 3

int main()
{
    ClocksourceManager*manager = ClocksourceManager::getInstance();
    manager->setClocksourceType(ClocksourceType::C_STYLE);
    Clocksource*clocksource_ = manager->getClocksource();

    TimeRecord r1;
    r1.setClocksource(clocksource_);
    r1.updateTimestamp();

    uint64_t t1 = r1.getTimestamp();

    std::cout << "sleeping for " << SLEEP_INTERVAL_IN_SEC << " seconds ...\n";
    usleep(SLEEP_INTERVAL_IN_SEC * 1000000);

    TimeRecord r2;
    r2.setClocksource(clocksource_);
    r2.updateTimestamp();
    uint64_t t2 = r2.getTimestamp();

    std::cout << "It took me " << t2 - t1 << " nanoseconds." << std::endl;
    std::cout << "Drift rate: " << std::scientific << (double)(t2 - t1) / SLEEP_INTERVAL_IN_SEC - 1 << std::endl;

    return 0;
}