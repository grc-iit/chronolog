//
// Created by eneko on 2/20/24.
//

#ifndef CHRONOKVS_CHRONOKVS_UTILS_H
#define CHRONOKVS_CHRONOKVS_UTILS_H

#include <string>
#include <utility> // For std::pair

// Serializes the given key and value into a single string.
std::string serialize(const std::string &key, const std::string &value);

// Deserializes the given string into a key-value pair.
std::pair <std::string, std::string> deserialize(const std::string &serialized);

#endif //CHRONOKVS_CHRONOKVS_UTILS_H
