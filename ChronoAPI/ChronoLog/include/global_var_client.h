//
// Created by kfeng on 10/24/22.
//

#ifndef CHRONOLOG_GLOBAL_VAR_CLIENT_H
#define CHRONOLOG_GLOBAL_VAR_CLIENT_H

#include <ClocksourceManager.h>

ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;
std::shared_ptr<ChronoLog::ConfigurationManager> g_confManager =
        ChronoLog::Singleton<ChronoLog::ConfigurationManager>::GetInstance();
std::shared_ptr<ChronoLogRPC> g_RPC = nullptr;

#endif //CHRONOLOG_GLOBAL_VAR_CLIENT_H
