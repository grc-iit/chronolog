//
// Created by kfeng on 2/14/22.
//

#ifndef CHRONOLOG_COMMON_H
#define CHRONOLOG_COMMON_H

#include <queue>
#include <mutex>
#include <TimeRecord.h>
#include <ClocksourceManager.h>
#include <condition_variable>

#define RPC_THREADS 4

#define THALLIUM_SOCKETS 0
#define THALLIUM_TCP 1
#define THALLIUM_UDP 2
#define THALLIUM_ROCE 3
#define CONF_COMM_LIB THALLIUM_SOCKETS

ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;
std::queue<TimeRecord> timeDBWriteQueue_;
int timeDBWriteQueueSize_{};
std::mutex timeDBWriteQueueMutex_;
std::condition_variable timeDBWriteQueueCV_;

#endif //CHRONOLOG_COMMON_H
