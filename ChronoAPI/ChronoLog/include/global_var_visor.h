//
// Created by kfeng on 10/24/22.
//

#ifndef CHRONOLOG_GLOBAL_VAR_VISOR_H
#define CHRONOLOG_GLOBAL_VAR_VISOR_H

#include "ClocksourceManager.h"
#include <ConfigurationManager.h>
#include <Chronicle.h>
#include <ChronicleMetaDirectory.h>
#include <rpc.h>
#include <RPCFactory.h>
#include <RPCVisor.h>

/**
 * Clock management-related global variables
 */
ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;

/**
 * ClientRegistry-related global variables
 */
std::shared_ptr<ClientRegistryManager> g_clientRegistryManager =
        ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
std::mutex g_clientRegistryMutex_;

/**
 * ChronicleMetaDirectory-related global variables
 */
std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory =
        ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance();

std::mutex g_chronicleMetaDirectoryMutex_;
std::mutex g_acquiredChronicleMapMutex_;
std::mutex g_acquiredStoryMapMutex_;

#endif //CHRONOLOG_GLOBAL_VAR_VISOR_H
