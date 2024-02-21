//
// Created by eneko on 2/20/24.
//

#ifndef CHRONOKVS_CHRONOKVS_MEMORYMANAGER_H
#define CHRONOKVS_CHRONOKVS_MEMORYMANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <utility> // for std::pair

class MemoryManager
{
public:
    void store(const std::string &key, std::uint64_t timestamp);

    std::vector <std::uint64_t> retrieveByKey(const std::string &key);
};

#endif //CHRONOKVS_CHRONOKVS_MEMORYMANAGER_H
