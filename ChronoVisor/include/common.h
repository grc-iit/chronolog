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

ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;
std::queue<TimeRecord> timeDBWriteQueue_;
int timeDBWriteQueueSize_{};
std::mutex timeDBWriteQueueMutex_;
std::condition_variable timeDBWriteQueueCV_;

#endif //CHRONOLOG_COMMON_H
