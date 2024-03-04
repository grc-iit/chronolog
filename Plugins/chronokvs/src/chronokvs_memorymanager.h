#ifndef CHRONOKVS_CHRONOKVS_MEMORYMANAGER_H
#define CHRONOKVS_CHRONOKVS_MEMORYMANAGER_H

#include <string>
#include <vector>
#include <utility>
#include <unordered_map>

class MemoryManager
{
public:
    void store(const std::string &key, std::uint64_t timestamp);

    std::vector <std::uint64_t> retrieveByKey(const std::string &key);
};

#endif //CHRONOKVS_CHRONOKVS_MEMORYMANAGER_H
