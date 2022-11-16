//
// Created by kfeng on 10/24/22.
//

#ifndef CHRONOLOG_GLOBAL_VAR_VISOR_H
#define CHRONOLOG_GLOBAL_VAR_VISOR_H

#include <ClocksourceManager.h>

/**
 * Configuration-related global variables
 */
std::shared_ptr<ChronoLog::ConfigurationManager> g_confManager =
        ChronoLog::Singleton<ChronoLog::ConfigurationManager>::GetInstance();

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
std::shared_ptr<std::unordered_map<std::string, Chronicle *>> g_chronicleMap =
        ChronoLog::Singleton<std::unordered_map<std::string, Chronicle *>>::GetInstance();
std::shared_ptr<ChronicleMetaDirectory> g_chronicleMetaDirectory =
        ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance(); // ChronicleMetaDirectory has to been inited
                                                                     // after global ChronicleMap since it tried to
                                                                     // store a reference to it using Singleton
                                                                     // template class in its constructor
std::mutex g_chronicleMetaDirectoryMutex_;

/**
 * RPC-related global variables
 */
// Shared RPC instance
std::shared_ptr<RPC> g_RPC =
        ChronoLog::Singleton<RPCFactory>::GetInstance()->GetRPC(CHRONOLOG_CONF->RPC_BASE_SERVER_PORT);
// RPC proxy instance for admin APIs (e.g., connect/disconnect)
std::shared_ptr<ChronoLogAdminRPCProxy> g_adminRPCProxy =
        ChronoLog::Singleton<ChronoLogAdminRPCProxy>::GetInstance();
// RPC proxy instance for metadata APIs (e.g., Chronicle/Story management)
std::shared_ptr<ChronicleMetadataRPCProxy> g_chronicleMetadataRPCProxy =
        ChronoLog::Singleton<ChronicleMetadataRPCProxy>::GetInstance();

#endif //CHRONOLOG_GLOBAL_VAR_VISOR_H
