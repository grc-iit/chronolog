//
// Created by kfeng on 2/14/22.
//

#ifndef CHRONOLOG_COMMON_H
#define CHRONOLOG_COMMON_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <random>
#include "singleton.h"
#include "../../../ChronoVisor/include/TimeRecord.h"
#include "../../../ChronoVisor/include/ClocksourceManager.h"

ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;
std::queue<TimeRecord> timeDBWriteQueue_;
int timeDBWriteQueueSize_{};
std::mutex timeDBWriteQueueMutex_;
std::condition_variable timeDBWriteQueueCV_;

std::random_device rd;
std::seed_seq ssq{rd()};
std::default_random_engine mt{rd()};
std::uniform_int_distribution<int> dist(0, INT32_MAX);

std::string gen_random(const int len) {
    static const char alphanum[] =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz";
    std::string tmp_s;
    tmp_s.reserve(len);

    for (int i = 0; i < len; ++i) {
        tmp_s += alphanum[dist(mt) % (sizeof(alphanum) - 1)];
    }

    return tmp_s;
}

#endif //CHRONOLOG_COMMON_H
