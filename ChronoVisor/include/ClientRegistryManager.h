//
// Created by kfeng on 7/11/22.
//

#ifndef CHRONOLOG_CLIENTREGISTRYMANAGER_H
#define CHRONOLOG_CLIENTREGISTRYMANAGER_H

#include <unordered_map>
#include <memory>
#include <ClientRegistryRecord.h>

class ClientRegistryManager {
public:
    ClientRegistryManager();
    ~ClientRegistryManager();

    bool connect(const std::string &client_id, const ClientRegistryRecord &record);
    bool disconnect(const std::string& client_id);
private:
    std::shared_ptr<std::unordered_map<std::string, ClientRegistryRecord>> clientRegistry_;
};

#endif //CHRONOLOG_CLIENTREGISTRYMANAGER_H
